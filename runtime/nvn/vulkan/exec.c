#include <ctype.h>
// Executes recorded NVN command lists with Vulkan (port of renderer.m; same structure, same behaviour).
#include <SDL3/SDL_cpuinfo.h>
#if defined(__x86_64__)
#include <immintrin.h>
#endif
#include <SDL3/SDL_timer.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "../../kernel.h"
#include "shaders.h"
#include "../../plugin.h"
#include "vk.h"
#include "../../platform.h"
#include "../renderer.h"
#include "../../settings.h"

static uint64_t g_draws_total;  // (never reset: LARECOMP_COUNTER_LOG)
static uint64_t stat_draws, stat_skipped, stat_passes, stat_dispatches, stat_wait_ns, stat_flushes, stat_feedback_copies;
// Skipped draws by reason (logged with the stats): 0 no program bound, 1 program still compiling, 2 program failed,
// 3 no program object, 4 primitive mode, 5 pipeline still building, 6 pipeline failed.
static uint64_t stat_skip_why[8];
uint64_t g_skin_watch;  // (debug, LARECOMP_DUMP_SKIN: a vertex cbuf watched across frames)
// letsgo-recomp's docs/PLANO-3D.md experiments, logged with the stats: [0] palettes replaced by identity, [1] skinned-draw cbufs
// read in place (UBO_DIRECT), [2] skinned-draw cbufs UBO_DIRECT could not place (copied), [3] one-bone shaders.
static uint64_t g_exp_count[4];
#define SKIP(why) (stat_skipped++, stat_skip_why[(why) & 7]++)
static uint64_t stat_upload_bytes;
// LARECOMP_VK_PROF=1: render-thread time per category in the fps log (exec = whole command lists, draw = draw(),
// sets = descriptor sets incl. cbuf copies, vtx = index scan + vertex copies, submit = vkQueueSubmit2)
static int prof = -1;
static uint64_t prof_exec, prof_draw, prof_sets, prof_vtx, prof_submit, prof_pre, prof_key, prof_dyn, prof_tex, prof_up, prof_gpu;
static uint64_t prof_dalloc, prof_dupd, prof_dbind, prof_bindpipe;  // (research: the driver's descriptor calls)
static VkQueryPool prof_qp;  // (two timestamps per submission slot: the GPU time of each submission, gpu_busy_ns)
static _Atomic uint64_t gpu_busy_ns;  // GPU time of the finished submissions so far, start to end of each (idle between excluded)
static uint64_t cum_exec, cum_wait, cum_draws, cum_submits;  // (profiling) never reset: hitch log
static uint32_t prof_end_line[4096];  // passes ended, by the exec.c line that ended them (0: submit / present)
uint64_t prof_views_created;  // textures.c vk_image_view cache misses
uint64_t prof_up_ns = ~0ull;  // textures.c upload checks (~0: profiling off)
#define PROF_T0() uint64_t prof_t0_ = prof ? SDL_GetTicksNS() : 0
#define PROF_ADD(v) do { if (prof) v += SDL_GetTicksNS() - prof_t0_; } while (0)
static _Atomic int stat_compiles;
// (research: how many programs / pipelines / pipeline parts are kept, logged as "[cache]" when they change; they are
// never released - Z-A's Rogue Mega battles made ~40000 parts in 10 minutes, host memory 7 -> 12 GB)
static _Atomic uint64_t g_cache_progs, g_cache_pipes, g_cache_libs, g_cache_pipes_replaced;
static _Atomic uint64_t stat_translate_ns, stat_spirv_ns, stat_pipe_ns;  // CPU time translating / compiling programs
static uint64_t stat_shader_wait_ns;  // draws waiting for their program / pipeline (wait_built)
static _Atomic int new_pipes;  // pipelines created since the pipeline cache was last saved
static _Atomic uint64_t stat_lib_ns, stat_link_ns, stat_libs, stat_links, stat_opt_ns, stat_opts;  // (g_vk.gpl: pipeline parts built, pipelines linked)
static _Atomic uint64_t stat_ready_ns, stat_ready_max_ns, stat_ready_n;  // first draw asking for a pipeline -> it is ready
static uint64_t op_counts[CMD_COUNT];
static uint64_t stat_vtx_copied, stat_vtx_direct;  // vertex / index ranges of the draws: copied to VRAM / read in place
bool g_debug_frame;
volatile uint64_t g_exp_nocull_start;
volatile long g_probe_lo, g_probe_hi;
volatile long g_nocull_lo, g_nocull_hi;
volatile int g_live_nocull;
volatile uint64_t g_skip_cs[8];
bool vk_gpu_may_write(uint64_t a);
static uint64_t g_poly_src_list, g_poly_src_rec, g_cur_list[9];
static int g_poly_src_depth, g_cur_depth;
volatile uint64_t g_probe_frame;
uint32_t g_dyn_epoch = 1;  // (draw_'s dynamic state cache, dc: bumped by whatever else sets graphics state)
static char stats_text[1024];
void vk_collect_dead(void);

// ================================================================== submissions
// A submission = optional upload command buffer + the main one, signalling the timeline with its number.
#define SUBS 8
typedef struct {
    VkCommandBuffer main, upload;
    bool main_begun, upload_begun, ts;  // ts: GPU timestamps written (profiling)
    uint64_t seq;
    VkDescriptorPool pools[16];
    int npools, cur_pool;
} Sub;
static Sub subs[SUBS];
static int cur_sub;
static uint64_t submit_seq;  // last submitted
static pthread_mutex_t seq_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t seq_cond = PTHREAD_COND_INITIALIZER;  // (submit_seq advanced: renderer_wait_seq)
static int g_submit_req;  // a thread waits for work not submitted yet: the render thread submits after its job
static bool in_pass;

uint64_t vk_completed_seq(void) {
    uint64_t v = 0;
    vkGetSemaphoreCounterValue(g_vk.dev, g_vk.timeline, &v);
    return v;
}

void vk_wait_seq(uint64_t seq) {
    if (!seq) return;
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_vk.timeline,
                              .pValues = &seq};
    uint64_t t0 = SDL_GetTicksNS();
    extern void rt_trace(char ev, uint64_t arg);
    rt_trace('G', seq);
    VkResult r = vkWaitSemaphores(g_vk.dev, &wi, 10ull * 1000000000ull);
    rt_trace('g', 0);
    stat_wait_ns += SDL_GetTicksNS() - t0;
    cum_wait += SDL_GetTicksNS() - t0;
    if (r == VK_TIMEOUT) LOG("[vk] GPU did not finish submission %llu within 10 s (device lost?)\n", (unsigned long long)seq);
    else if (r != VK_SUCCESS) {
        extern void vk_devlost_dump(void);
        vk_devlost_dump();
        vk_device_fault_report();
        FATAL("vkWaitSemaphores: %d (device lost)\n", r);
    }
}

uint64_t vk_pending_seq(void) { return submit_seq + 1; }

static Sub *sub(void) {
    Sub *s = &subs[cur_sub];
    if (s->seq != submit_seq + 1) {  // slot reused: its previous submission must be done
        vk_wait_seq(s->seq);
        if (s->ts) {  // GPU time of that submission
            uint64_t t[2];
            if (vkGetQueryPoolResults(g_vk.dev, prof_qp, (uint32_t)cur_sub * 2, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0]) {
                uint64_t ns = (uint64_t)((double)(t[1] - t[0]) * g_vk.props.limits.timestampPeriod);
                prof_gpu += ns;
                atomic_fetch_add(&gpu_busy_ns, ns);
            }
            s->ts = false;
        }
        for (int i = 0; i < s->npools; i++) vkResetDescriptorPool(g_vk.dev, s->pools[i], 0);
        s->cur_pool = 0;
        pthread_mutex_lock(&seq_lock);
        s->main_begun = s->upload_begun = false;
        s->seq = submit_seq + 1;
        pthread_mutex_unlock(&seq_lock);
    }
    return s;
}

static void begin_cb(VkCommandBuffer *cb, bool *begun) {
    if (*begun) return;
    g_dyn_epoch++;  // (draw_'s state cache: dc)
    if (!*cb) {
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_vk.pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &ai, cb));
    }
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(*cb, &bi));
    pthread_mutex_lock(&seq_lock);
    *begun = true;
    pthread_mutex_unlock(&seq_lock);
}

static void gp_mark(VkCommandBuffer cb, const char *fmt, ...);
VkCommandBuffer vk_cmd(void) {
    Sub *s = sub();
    if (!s->main_begun) {
        begin_cb(&s->main, &s->main_begun);
        gp_mark(s->main, "(submission start; includes GPU idle before it)");
        if (prof_qp) {
            vkCmdResetQueryPool(s->main, prof_qp, (uint32_t)cur_sub * 2, 2);
            vkCmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_NONE, prof_qp, (uint32_t)cur_sub * 2);
            s->ts = true;
        }
    }
    return s->main;
}

VkCommandBuffer vk_upload_cmd(void) {
    Sub *s = sub();
    begin_cb(&s->upload, &s->upload_begun);
    return s->upload;
}

// ---- The barrier before a render pass, only where one is needed (begin_pass, draw_). Every image stays in GENERAL,
// so between passes only the order of memory accesses matters: a pass rendering to, or a draw sampling, an image a pass
// rendered to since the last barrier (read / write after write), or a pass rendering to one a draw sampled (write
// after read). Those are tracked per backing image; every other GPU write (copies, compute, storage images, stores to
// guest memory, transform feedback) is not, and makes the next pass wait for everything as before - each of those
// goes through vk_full_barrier, which marks it. A full barrier before every pass cost ~12% of Z-A's GPU time (4K,
// RTX 4090): it drains the GPU ~60 times a frame. LARECOMP_VK_PASS_BARRIER=1: before every pass, as before.
#define HZ_MAX 48
static TexBackend *hz_w[HZ_MAX], *hz_r[HZ_MAX];  // rendered to / sampled since the last barrier
static int hz_nw, hz_nr;
static bool hz_all = true;  // something untracked was written since: the next pass waits for everything
static bool hz_force;       // a draw needs what its pass's predecessors wrote: the next pass begins with the barrier
// Passes (by their attachments) a draw had to end for the barrier: next time (frames repeat) they begin with it - a
// pass ended early is begun again, which costs a tiled GPU a store and a load of its attachments
static uint64_t hz_learned[1024], hz_pass_key;
static uint64_t stat_pass_barriers, stat_pass_nobarrier;
static bool hz_has(TexBackend *const *set, int n, const TexBackend *b) {
    for (int i = 0; i < n; i++) if (set[i] == b) return true;
    return false;
}
static void hz_add(TexBackend **set, int *n, TexBackend *b) {
    if (!b || hz_has(set, *n, b)) return;
    if (*n == HZ_MAX) hz_all = true;
    else set[(*n)++] = b;
}

void vk_full_barrier(VkCommandBuffer cb) {
    hz_all = true;  // (the work after it: not tracked)
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

static void ring_retire(void);

// ---- LARECOMP_GPUPROF_AT=<frame>: GPU time per pass / dispatch / copy of that frame, logged as "[gpuprof]".
// A timestamp (ALL_COMMANDS: after everything before it) at each mark; a row = the work since the previous mark.
bool g_gpuprof_frame;
static VkQueryPool gp_qp;
static int gp_n;
#define GP_MAX 4096
static char gp_label[GP_MAX][112];
static uint32_t gp_draws[GP_MAX];
// LARECOMP_GPUPROF_STATS=1 (with LARECOMP_GPUPROF_DRAWS): the pipeline statistics of every draw of the profiled frames
// (input vertices, vertex shader invocations, clipped primitives, fragment shader invocations), printed on its row.
static VkQueryPool gps_qp;
static int gps_n;
static int gp_stat[GP_MAX];  // mark -> its index in gps_qp + 1 (0: none)
// LARECOMP_VK_DEVLOST_TRACE=1: the last 64 GPU operations, dumped on device loss (with LARECOMP_SYNC_SUBMIT=1 the
// dump ends with the command list that faulted).
static char dl_ring[64][128];
static unsigned dl_n;
static int dl_on = -1;
static void dl_note(const char *fmt, ...) {
    if (dl_on < 0) dl_on = ENV("LARECOMP_VK_DEVLOST_TRACE") ? atoi(ENV("LARECOMP_VK_DEVLOST_TRACE")) : 0;
    if (!dl_on) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dl_ring[dl_n++ & 63], sizeof dl_ring[0], fmt, ap);
    va_end(ap);
}
void vk_dl_note_upload(void *img, uint32_t w, uint32_t h, uint32_t lv) { dl_note("  upload %p %ux%u lv%u", img, w, h, lv); }
void vk_devlost_dump(void) {
    if (!dl_on) return;
    LOG("[vk] last GPU operations before the device loss (oldest first):\n");
    for (unsigned i = dl_n > 64 ? dl_n - 64 : 0; i < dl_n; i++) LOG("[vk]   %s\n", dl_ring[i & 63]);
}
uint64_t vk_frame_counter(void);
static uint64_t gp_frame;  // the frame counter when the profiled frame began (parity: LARECOMP_EXP_ALT)
static void gp_mark(VkCommandBuffer cb, const char *fmt, ...) {
    if (!g_gpuprof_frame || gp_n >= GP_MAX - 1) return;
    if (!gp_qp) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = GP_MAX};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &gp_qp) != VK_SUCCESS) { g_gpuprof_frame = false; return; }
    }
    if (!gp_n) {
        gp_frame = vk_frame_counter();
        vkCmdResetQueryPool(cb, gp_qp, 0, GP_MAX);
        if (ENV("LARECOMP_GPUPROF_STATS")) {
            if (!gps_qp) {
                VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS,
                                            .queryCount = GP_MAX,
                                            .pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
                                                                  VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
                                                                  VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
                                                                  VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT};
                if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &gps_qp) != VK_SUCCESS) gps_qp = VK_NULL_HANDLE;
            }
            if (gps_qp) vkCmdResetQueryPool(cb, gps_qp, 0, GP_MAX);
            gps_n = 0;
        }
    }
    gp_stat[gp_n] = 0;
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gp_qp, (uint32_t)gp_n);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(gp_label[gp_n], sizeof gp_label[0], fmt, ap);
    va_end(ap);
    gp_draws[gp_n] = 0;
    gp_n++;
}
void renderer_flush(void);
void vk_gpuprof_report(void) {
    if (!gp_n) return;
    renderer_flush();
    static uint64_t t[GP_MAX];
    if (vkGetQueryPoolResults(g_vk.dev, gp_qp, 0, (uint32_t)gp_n, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
        double per = g_vk.props.limits.timestampPeriod / 1e6, total = (t[gp_n - 1] - t[0]) * per;
        LOG("[gpuprof] %d marks, %.2f ms first to last (frame %llu; rows: the work after each mark, until the next)\n", gp_n, total,
            (unsigned long long)gp_frame);
        static uint64_t stats[GP_MAX * 4];
        bool have_stats = gps_qp && gps_n &&
            vkGetQueryPoolResults(g_vk.dev, gps_qp, 0, (uint32_t)gps_n, sizeof stats, stats, 32, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS;
        for (int i = 0; i + 1 < gp_n; i++) {
            double ms = (t[i + 1] - t[i]) * per;
            bool st = have_stats && gp_stat[i];
            LOG("[gpuprof] %6.3f ms %5.1f%%  %s%s", ms, 100 * ms / (total > 0 ? total : 1), gp_label[i], gp_draws[i] || st ? "" : "\n");
            if (gp_draws[i]) LOG(", %u draws\n", gp_draws[i]);
            if (st) {
                const uint64_t *q = &stats[(gp_stat[i] - 1) * 4];  // (the enabled bits, in order: IA vertices, VS invocations, clip prims, FS invocations)
                LOG(" | ia %llu vsinv %llu clipprim %llu fsinv %llu\n", (unsigned long long)q[0], (unsigned long long)q[1],
                    (unsigned long long)q[2], (unsigned long long)q[3]);
            }
        }
    }
    gp_n = 0;
}

static void ring_flush_gpu(void);
static void oq_flush(void);
static uint64_t submit_draws;  // g_draws_total at the last submission (backend_execute's batching)
// Submit the pending work (if any). wait/signal: binary semaphores for presentation (may be null).
void vk_submit_extra(VkSemaphore wait, VkSemaphore signal, VkCommandBuffer extra) {
    (void)extra;
    vk_end_pass();
    oq_flush();  // (occlusion query results not copied yet, and the barrier for the CPU: oq_copy_out)
    ring_flush_gpu();
    Sub *s = sub();
    if (!s->main_begun && !s->upload_begun && !wait && !signal) return;
    VkCommandBufferSubmitInfo cbs[2];
    int n = 0;
    if (s->upload_begun) {
        vk_full_barrier(s->upload);  // uploads complete before anything in the main buffer reads them
        VK_CHECK(vkEndCommandBuffer(s->upload));
        cbs[n++] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->upload};
    }
    vk_cmd();  // (an empty main buffer still carries the semaphores)
    if (s->ts) vkCmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, prof_qp, (uint32_t)cur_sub * 2 + 1);
    VK_CHECK(vkEndCommandBuffer(s->main));
    cbs[n++] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->main};
    VkSemaphoreSubmitInfo sig[2] = {{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_vk.timeline,
                                     .value = submit_seq + 1, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
                                    {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = signal,
                                     .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}};
    VkSemaphoreSubmitInfo wt = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = wait,
                                .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = (uint32_t)n, .pCommandBufferInfos = cbs,
                        .signalSemaphoreInfoCount = signal ? 2 : 1, .pSignalSemaphoreInfos = sig,
                        .waitSemaphoreInfoCount = wait ? 1 : 0, .pWaitSemaphoreInfos = &wt};
    {
        PROF_T0();
        dl_note("submit %llu", (unsigned long long)submit_seq + 1);
        extern void vk_queue_lock(void), vk_queue_unlock(void);
        vk_queue_lock();
#if defined(__x86_64__)
        _mm_sfence();  // (copy_stream's stores, before the GPU reads them)
#else
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
        VkResult sr = vkQueueSubmit2(g_vk.queue, 1, &si, VK_NULL_HANDLE);
        vk_queue_unlock();
        if (sr == VK_ERROR_DEVICE_LOST) vk_devlost_dump();
        VK_CHECK(sr);
        PROF_ADD(prof_submit);
    }
    pthread_mutex_lock(&seq_lock);
    submit_seq++;
    cur_sub = (cur_sub + 1) % SUBS;
    pthread_cond_broadcast(&seq_cond);
    pthread_mutex_unlock(&seq_lock);
    submit_draws = g_draws_total;
    stat_flushes++;
    cum_submits++;
    ring_retire();
    vk_collect_dead();
}

void renderer_submit(void) { vk_submit_extra(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE); }
void renderer_wait_seq(uint64_t seq);
uint64_t renderer_fence_seq(void);

void renderer_flush(void) {
    renderer_submit();
    vk_wait_seq(submit_seq);
}

void renderer_sync(void) { renderer_wait_seq(renderer_fence_seq()); }  // (the pending submission too: batched lists)

uint64_t renderer_fence_seq(void) {
    pthread_mutex_lock(&seq_lock);
    Sub *s = &subs[cur_sub];
    // A reused slot keeps its old begun flags until sub() prepares it: an empty queue's fence must name the last
    // submission, not a future one that submit's empty-work fast path never signals (LAS Recomp 37b7ff6).
    bool pending = s->seq == submit_seq + 1 && (s->main_begun || s->upload_begun);
    uint64_t q = submit_seq + (pending ? 1 : 0);
    pthread_mutex_unlock(&seq_lock);
    return q;
}

// The render thread batches command lists into one submission (backend_execute): a fence of the one being filled is
// waited for by asking it to submit (after its current job; before it sleeps it always does) and waiting for that.
void renderer_wait_seq(uint64_t seq) {
    if (!seq) return;
    pthread_mutex_lock(&seq_lock);
    if (seq > submit_seq && rt_active() && !rt_on_render_thread()) {
        __atomic_store_n(&g_submit_req, 1, __ATOMIC_RELEASE);
        extern void rt_wake(void);
        pthread_mutex_unlock(&seq_lock);
        rt_wake();
        pthread_mutex_lock(&seq_lock);
        while (seq > submit_seq && !g_rt_unwind) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 5000000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&seq_cond, &seq_lock, &ts);
        }
    }
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    vk_wait_seq(seq > s ? s : seq);
}

// Render thread, between jobs: submit now if a thread waits for the pending work (renderer_wait_seq) or the queue is empty
void renderer_submit_if_needed(bool idle) {
    if (__atomic_exchange_n(&g_submit_req, 0, __ATOMIC_ACQ_REL) || idle) renderer_submit();
}

VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout) {
    Sub *s = sub();
    for (;;) {
        if (s->cur_pool == s->npools) {
            if (s->npools == 16) FATAL("descriptor pools exhausted\n");
            VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32768}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},
                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768},
                                            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1024},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096}};
            VkDescriptorPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 8192,
                                             .poolSizeCount = 6, .pPoolSizes = sizes};
            VK_CHECK(vkCreateDescriptorPool(g_vk.dev, &ci, NULL, &s->pools[s->npools++]));
        }
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = s->pools[s->cur_pool], .descriptorSetCount = 1, .pSetLayouts = &layout};
        VkDescriptorSet set;
        VkResult r = vkAllocateDescriptorSets(g_vk.dev, &ai, &set);
        if (r == VK_SUCCESS) return set;
        s->cur_pool++;
    }
}

// ================================================================== transient memory ring
// The largest index of a draw (bounds the vertex data copied for it), ignoring the restart value: AVX2 where the CPU has it (the
// game code needs it too), a plain loop elsewhere. A draw scans a few thousand indices: ~1.4 us scalar, ~0.15 us here.
#if defined(__x86_64__)
__attribute__((target("avx2"))) static uint32_t scan_max16_avx2(const uint16_t *q, uint32_t n, uint16_t restart) {
    __m256i acc = _mm256_setzero_si256(), rs = _mm256_set1_epi16((short)restart);
    uint32_t i = 0;
    for (; i + 16 <= n; i += 16) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(q + i));
        acc = _mm256_max_epu16(acc, _mm256_andnot_si256(_mm256_cmpeq_epi16(v, rs), v));
    }
    uint16_t t[16];
    _mm256_storeu_si256((__m256i *)t, acc);
    uint32_t m = 0;
    for (int k = 0; k < 16; k++) if (t[k] > m) m = t[k];
    for (; i < n; i++) if (q[i] != restart && q[i] > m) m = q[i];
    return m;
}
__attribute__((target("avx2"))) static uint32_t scan_max32_avx2(const uint32_t *q, uint32_t n, uint32_t restart) {
    __m256i acc = _mm256_setzero_si256(), rs = _mm256_set1_epi32((int)restart);
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(q + i));
        acc = _mm256_max_epu32(acc, _mm256_andnot_si256(_mm256_cmpeq_epi32(v, rs), v));
    }
    uint32_t t[8];
    _mm256_storeu_si256((__m256i *)t, acc);
    uint32_t m = 0;
    for (int k = 0; k < 8; k++) if (t[k] > m) m = t[k];
    for (; i < n; i++) if (q[i] != restart && q[i] > m) m = q[i];
    return m;
}
#endif
static uint32_t scan_max_index(const void *src, uint32_t isz, uint32_t n, uint32_t restart) {
#if defined(__x86_64__)
    static int avx2 = -1;
    if (avx2 < 0) avx2 = plat_cpu_x86_v3();
#endif
    uint32_t m = 0;
    if (isz == 2) {
#if defined(__x86_64__)
        if (avx2) return scan_max16_avx2(src, n, (uint16_t)restart);
#endif
        const uint16_t *q = src;
        for (uint32_t i = 0; i < n; i++) if (q[i] > m && q[i] != (uint16_t)restart) m = q[i];
    } else {
#if defined(__x86_64__)
        if (avx2) return scan_max32_avx2(src, n, restart);
#endif
        const uint32_t *q = src;
        for (uint32_t i = 0; i < n; i++) if (q[i] > m && q[i] != restart) m = q[i];
    }
    return m;
}

// Two pools of 64 MB chunks, a chunk reused once every submission that allocated from it has completed.
// Pool 0: host-visible chunks (VRAM through the BAR when the GPU has one) the GPU reads in place: constants and the sources of
// texture uploads. Pool 1 (ring_gpu_enabled, discrete GPUs): the vertex and index data of the draws, in VRAM. The CPU writes a
// staging chunk in system memory and each submission starts with a copy of what was written since the last one (ring_flush_gpu),
// so the GPU fetches vertices from VRAM instead of reading the game's memory over PCIe once per vertex invocation (an
// instanced draw re-read its per-instance streams for every vertex: ~0.8 ms for 71 thousand invocations, letsgo-recomp's docs/PESQUISA-GPU.md).
#define CHUNK_SIZE (64ull << 20)
typedef struct { VkBuffer buf, dev; VmaAllocation mem, dmem; uint8_t *ptr; uint64_t size, used, seq, flushed; } Chunk;
typedef struct { Chunk *c; int n, active; bool gpu; } Pool;
static Pool pools[2] = {{.active = -1}, {.active = -1, .gpu = true}};

// LARECOMP_VERTEX_READ=direct|copy|auto (default auto: copies on a discrete GPU, in place on an integrated one, whose memory
// is the system's). The draws' own ranges the GPU writes stay in place either way (gpuw below).
static bool ring_gpu_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = ENV("LARECOMP_VERTEX_READ");
        if (!e) e = g_settings.vertex_read == VREAD_DIRECT ? "direct" : g_settings.vertex_read == VREAD_COPY ? "copy" : NULL;
        if (e && !strcmp(e, "direct")) on = 0;
        else if (e && (!strcmp(e, "copy") || !strcmp(e, "hybrid"))) on = 1;
        else on = g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        LOG("[vk] vertex and index data: %s\n", on ? "copied to VRAM (ranges the GPU writes are read in place)" : "read in place from the game's memory");
    }
    return on;
}

static int new_chunk(Pool *pl, uint64_t size) {
    pl->c = realloc(pl->c, (pl->n + 1) * sizeof *pl->c);
    Chunk *c = &pl->c[pl->n];
    memset(c, 0, sizeof *c);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = pl->gpu ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                              : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = pl->gpu ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO,
                                  .flags = (pl->gpu ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT) |
                                           VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &c->buf, &c->mem, &info));
    c->ptr = info.pMappedData;
    c->size = size;
    if (pl->gpu) {
        VkBufferCreateInfo di = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                                 .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo da = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &di, &da, &c->dev, &c->dmem, NULL));
    }
    {   // (where the GPU reads the copies from: VRAM through the BAR, or system memory over PCIe)
        VkMemoryPropertyFlags mp = 0;
        vmaGetAllocationMemoryProperties(g_vk.vma, pl->gpu ? c->dmem : c->mem, &mp);
        LOG("[vk] ring %d chunk %d: %llu MB, %s%s\n", pl->gpu ? 1 : 0, pl->n, (unsigned long long)(size >> 20),
            mp & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? "device-local" : "system memory",
            mp & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? " host-visible" : "");
    }
    return pl->n++;
}

static void ring_retire(void) {}  // chunks are recycled lazily in transient_from

static VkSpan transient_from(Pool *pl, uint64_t size, uint64_t align) {
    if (align < 16) align = 16;
    uint64_t pending = vk_pending_seq();
    if (pl->active >= 0) {
        Chunk *c = &pl->c[pl->active];
        uint64_t off = (c->used + align - 1) & ~(align - 1);
        if (off + size <= c->size) {
            c->used = off + size;
            c->seq = pending;
            stat_upload_bytes += size;
            return (VkSpan){pl->gpu ? c->dev : c->buf, off, c->ptr + off};
        }
    }
    uint64_t done = vk_completed_seq();
    int pick = -1;
    for (int i = 0; i < pl->n; i++)
        if (i != pl->active && pl->c[i].seq <= done && pl->c[i].size >= size) { pick = i; break; }
    if (pick < 0) pick = new_chunk(pl, size > CHUNK_SIZE ? size : CHUNK_SIZE);
    pl->active = pick;
    Chunk *c = &pl->c[pick];
    c->used = size;
    c->flushed = 0;
    c->seq = pending;
    stat_upload_bytes += size;
    return (VkSpan){pl->gpu ? c->dev : c->buf, 0, c->ptr};
}

VkSpan vk_transient(uint64_t size, uint64_t align) { return transient_from(&pools[0], size, align); }
// Vertex / index data of a draw: in VRAM (valid once the submission's first copy has run), or as vk_transient where that is off.
static VkSpan transient_gpu(uint64_t size, uint64_t align) { return transient_from(&pools[ring_gpu_enabled() ? 1 : 0], size, align); }

// The start of a submission: the staging bytes written since the last one go to VRAM (the upload buffer runs first, then a
// barrier, vk_submit_extra).
static void ring_flush_gpu(void) {
    Pool *pl = &pools[1];
    for (int i = 0; i < pl->n; i++) {
        Chunk *c = &pl->c[i];
        if (c->used <= c->flushed) continue;
        VkBufferCopy rg = {c->flushed, c->flushed, c->used - c->flushed};
        vkCmdCopyBuffer(vk_upload_cmd(), c->buf, c->dev, 1, &rg);
        c->flushed = c->used;
    }
}

// GPU -> CPU copies: the ring is write-combined (often device-local over the BAR), where CPU reads crawl (a
// 1280x720 screenshot took 7 s). Readbacks use one host-cached buffer instead. Every user waits for the GPU and
// reads the data before the next readback (render thread), so a single grow-only buffer suffices.
VkSpan vk_readback(uint64_t size) {
    static VkBuffer buf;
    static VmaAllocation mem;
    static uint8_t *ptr;
    static uint64_t cap;
    if (size > cap) {
        if (buf) { vk_wait_seq(submit_seq); vmaDestroyBuffer(g_vk.vma, buf, mem); }
        cap = (size + (16 << 20) - 1) & ~((16ull << 20) - 1);
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = cap, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &buf, &mem, &info));
        ptr = info.pMappedData;
    }
    return (VkSpan){buf, 0, ptr};
}

// Copies into the transient rings, with streaming stores: only the GPU reads them, and ~2 MB a frame written through
// the cache pushed the game's own data out of the shared L3 (in Lumiose its threads ran ~20% slower with the backend
// working than without: the same instructions, more cache misses). vk_submit_extra's sfence orders them before the GPU
// reads them. LARECOMP_NO_STREAM_COPY=1: plain memcpy.
static void copy_stream(void *dst, const void *src, uint64_t n) {
    static int off = -1;
    if (off < 0) off = ENV("LARECOMP_NO_STREAM_COPY") != NULL;
    if (n < 256 || ((uintptr_t)dst & 15) || off) { memcpy(dst, src, n); return; }
    uint8_t *d = dst;
    const uint8_t *q = src;
    uint64_t i = 0;
#if defined(__x86_64__)
    for (; i + 64 <= n; i += 64) {
        __m128i a = _mm_loadu_si128((const __m128i *)(q + i)), b = _mm_loadu_si128((const __m128i *)(q + i + 16)),
                c = _mm_loadu_si128((const __m128i *)(q + i + 32)), e = _mm_loadu_si128((const __m128i *)(q + i + 48));
        _mm_stream_si128((__m128i *)(d + i), a), _mm_stream_si128((__m128i *)(d + i + 16), b);
        _mm_stream_si128((__m128i *)(d + i + 32), c), _mm_stream_si128((__m128i *)(d + i + 48), e);
    }
#endif
    if (i < n) memcpy(d + i, q + i, n - i);
#if defined(__x86_64__)
    if (!rt_on_render_thread()) _mm_sfence();  // (another thread's copy: ordered here, before it hands the span over)
#endif
}

VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align) {
    VkSpan s = vk_transient(size ? size : 16, align);
    if (size) copy_stream(s.ptr, data, size);
    return s;
}

// Per-draw constants that usually repeat the previous draw's (scale table, tint, driver constants): reuse that copy
// while it's identical, within one submission. slot: one per kind of data.
enum { DEDUPE_RSCALE_V, DEDUPE_RSCALE_F, DEDUPE_TINT, DEDUPE_DRV_V, DEDUPE_DRV_F, DEDUPE_DRV_C, DEDUPE_SLOTS };
static VkSpan copy_dedupe(int slot, const void *data, uint64_t size, uint64_t align) {
    static struct { uint64_t seq, size; uint8_t data[0x800]; VkSpan span; } last[DEDUPE_SLOTS];
    uint64_t seq = vk_pending_seq();
    if (size > sizeof last[0].data) return vk_transient_copy(data, size, align);
    if (last[slot].seq == seq && last[slot].size == size && !memcmp(last[slot].data, data, size)) return last[slot].span;
    VkSpan s = vk_transient_copy(data, size, align);
    last[slot].seq = seq; last[slot].size = size; memcpy(last[slot].data, data, size); last[slot].span = s;
    return s;
}

// Guest memory copied once per submission: draws binding the same range share the copy.
#define GCOPY_TAB 4096
static struct { uint64_t addr, size, seq; VkSpan span; bool gpu; } gcopy[GCOPY_TAB];
static uint64_t gcopy_gen;  // bumped to drop every shared copy (guest memory written mid-list)
static void guest_copy_flush(void) { gcopy_gen++; }
static VkSpan transient_gpu_copy(const void *data, uint64_t size, uint64_t align) {
    VkSpan s = transient_gpu(size ? size : 16, align);
    if (size) copy_stream(s.ptr, data, size);
    return s;
}
static VkSpan guest_copy_pool(uint64_t addr, uint64_t size, bool gpu) {  // (gpu: vertex / index data, in VRAM when that ring is on)
    uint64_t seq = vk_pending_seq() + (gcopy_gen << 40), h = (addr * 0x9E3779B97F4A7C15ull) ^ (size * 0xC2B2AE3D27D4EB4Full);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (GCOPY_TAB - 1);
    if (gcopy[slot].seq == seq && gcopy[slot].addr == addr && gcopy[slot].size == size && gcopy[slot].gpu == gpu) return gcopy[slot].span;
    VkSpan s = gpu ? transient_gpu_copy((const void *)(uintptr_t)addr, size, 256) : vk_transient_copy((const void *)(uintptr_t)addr, size, 256);
    gcopy[slot].addr = addr; gcopy[slot].size = size; gcopy[slot].seq = seq; gcopy[slot].span = s; gcopy[slot].gpu = gpu;
    return s;
}
static VkSpan guest_copy(uint64_t addr, uint64_t size) { return guest_copy_pool(addr, size, false); }

// Vertex data copied to VRAM, once per start address and submission: every draw of a mesh starts at the same address and wants a
// prefix of it (up to its largest index), so one copy serves them all. A per-vertex stream is copied whole (its bound range, up to
// 8 MB) the first time, so that later draws of other parts of the buffer don't copy it again (37 MB a frame became 16).
#define VCOPY_TAB 1024
static struct { uint64_t addr, len, seq; VkSpan span; } vcopy[VCOPY_TAB];
static VkSpan vertex_copy(uint64_t addr, uint64_t need, uint64_t bound, bool per_vertex) {
    uint64_t seq = vk_pending_seq() + (gcopy_gen << 40);
    uint32_t h = (uint32_t)((addr >> 4) * 0x9E3779B1u) & (VCOPY_TAB - 1), at = h;
    for (int i = 0; i < 4; i++) {
        uint32_t k = (h + i) & (VCOPY_TAB - 1);
        if (vcopy[k].addr == addr && vcopy[k].seq == seq) {
            if (vcopy[k].len >= need) return vcopy[k].span;
            at = k;  // (a longer prefix is needed now: a new copy replaces it)
            goto copy;
        }
        if (vcopy[k].seq != seq) at = k;  // (a free or stale slot)
    }
copy:;
    uint64_t len = per_vertex && bound <= (8ull << 20) && bound > need ? bound : need;
    VkSpan sp = transient_gpu_copy((const void *)(uintptr_t)addr, len, 256);
    vcopy[at].addr = addr; vcopy[at].len = len; vcopy[at].seq = seq; vcopy[at].span = sp;
    return sp;
}

// ================================================================== worker threads (shader / pipeline builds)
// First-use compiles run off the render thread; draws needing them are skipped until ready (as on Metal).
// LARECOMP_SYNC_SHADERS=1 builds on first use, blocking (deterministic tests).
typedef struct Job { void (*fn)(void *); void *arg; struct Job *next; } Job;
static Job *jobs_head, *jobs_tail;
static pthread_mutex_t jobs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t jobs_cond = PTHREAD_COND_INITIALIZER;

static void *worker(void *a) {
    (void)a;
    { extern void cpupin_rest_push(void); cpupin_rest_push(); }  // (not on the render thread's CPU, which started it: cpupin.c)
    for (;;) {
        pthread_mutex_lock(&jobs_lock);
        while (!jobs_head) pthread_cond_wait(&jobs_cond, &jobs_lock);
        Job *j = jobs_head;
        jobs_head = j->next;
        if (!jobs_head) jobs_tail = NULL;
        pthread_mutex_unlock(&jobs_lock);
        j->fn(j->arg);
        free(j);
    }
    return NULL;
}

static bool sync_shaders(void) {
    static int v = -1;
    if (v < 0) v = ENV("LARECOMP_SYNC_SHADERS") != NULL;
    return v;
}

// Shader translation runs on the workers too, then compiles on the same worker (translate_program). It had one
// dedicated thread (the translator used a static string ring, now per thread): on a first visit, ~380 programs took
// ~240 ms one after another while the workers idled. Off the game thread either way (up to ~90 ms per frame there).
static void run_job(void (*fn)(void *), void *arg) {
    if (sync_shaders()) { fn(arg); return; }
    static bool started;
    if (!started) {
        started = true;
        // first entry into an area builds dozens of pipelines at once: use most of the CPU (draws wait for them)
        int n = SDL_GetNumLogicalCPUCores() - 4;  // (the machine's: plat_cpu_count is the caller's, the pinned render thread's 1)
        if (n < 4) n = 4;
        if (n > 16) n = 16;
        for (int i = 0; i < n; i++) { pthread_t t; pthread_create(&t, NULL, worker, NULL); }
    }
    Job *j = malloc(sizeof *j);
    j->fn = fn; j->arg = arg; j->next = NULL;
    pthread_mutex_lock(&jobs_lock);
    if (jobs_tail) jobs_tail->next = j; else jobs_head = j;
    jobs_tail = j;
    pthread_cond_signal(&jobs_cond);
    pthread_mutex_unlock(&jobs_lock);
}

// ================================================================== state
typedef struct {
    NvnTexture *color[8];
    NvnTextureView cview[8];
    uint8_t has_cview[8];
    int ncolor;
    NvnTexture *depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtState;

typedef struct { uint64_t addr, size; } Range;

static struct {
    RtState rt;
    bool rt_dirty;
    NvnProgram *program;
    uint8_t blend[8][8];
    uint32_t channel_mask;
    uint8_t color_state[4], ds[8], ms[0x18], poly[4];
    float poly_offset[3];
    uint32_t attrib[16];
    int nattrib;
    uint32_t stream[16][2];
    int nstream;
    Range vbuf[16], ubuf[6][18], sbuf[6][16];
    uint64_t tex[6][32], img[6][8];
    NvnDescPool *tpool, *spool;
    float viewport[16][4];
    int32_t scissor[16][4];
    float depth_range[16][2];
    float blend_color[4];
    uint32_t stencil_ref[2], stencil_mask[2], stencil_vmask[2];
    bool prim_restart;
    uint32_t restart_index;
    uint64_t copy_row_stride, copy_image_stride;
    uint64_t obj;       // frame interpolation: the game object the current draws belong to (CMD_OBJECT), 0 = none
    uint32_t obj_draw;  // draws of that object so far in this run of it
    bool xfb_active, raster_discard;  // transform feedback (CMD_XFB_*), SetRasterizerDiscard
    bool depth_clamp;                 // SetDepthClamp (a pipeline key bit)
    uint64_t xfb_addr[4], xfb_size[4];
} st;

// frame interpolation (prototype, LARECOMP_INTERP=1): the hooks below; the rest at the end of this file
static int ip_on = -1;
static bool ip_replaying;     // running a recorded frame again for the in-between image
static uint64_t ip_stats_tagged, ip_stats_untagged;  // draws with / without a game object (CMD_OBJECT)
static uint64_t ip_draw_sig;  // the current draw's identity (draw_)
static bool ip_draw_obj;      // ... and whether it belongs to a game object (CMD_OBJECT): only those are blended
static bool ip_draw_2d;       // ... a 2D draw (no depth test): interface, menus
static bool ip_enabled(void);
bool vk_interp_enabled(void) { return ip_enabled(); }  // (cmdbuf.c: hooks the game's object draw functions)
static bool ip_enabled(void) {
    // The 60 fps mode (runtime/hle/fps60.c): the world at 60 natively; menus and the Pokédex at the game's 30 with the
    // in-between image; battles and captures at 30 without it (tried, left out: letsgo-recomp's docs/HISTORICO.md 46).
    // LARECOMP_INTERP=1 (research): always on.
    if (ip_on == -1) ip_on = ENV("LARECOMP_INTERP") ? atoi(ENV("LARECOMP_INTERP")) : 0;
    extern bool fps60_menu(void);
    return ip_on > 0 || fps60_menu();
}
static const void *ip_cbuf(int stage, int k, uint64_t addr, uint32_t size);
static void ip_note_list(uint64_t handle);
// Stereo (plugin.h set_stereo: a VR mod's both eyes of one frame, vk_stereo_render): records like frame interpolation
static bool st_on(void) { return g_plugin_stereo && !ip_enabled(); }
static bool ip_rec(void) { return ip_enabled() || g_plugin_stereo; }
static uint32_t st_pres_w = 1920, st_pres_h = 1080;  // (stereo: the presented image's size, the game's units - the overlay's test)
extern volatile float g_plugin_render_shape;  // (runtime/plugin.c: a VR mod's eye shape)
static uint64_t ip_key(int stage, int k);
static const void *st_cbuf_replay(uint64_t key, int stage, uint64_t addr, uint32_t size);
static void ip_record(uint64_t key, const void *data, uint32_t size);
static bool ip_skip_op(uint32_t op) {  // what the replay must not do again: it writes guest memory or signals the game
    switch (op) {
    case CMD_DISPATCH: case CMD_DISPATCH_INDIRECT: case CMD_COPY_BUFFER_TO_BUFFER: case CMD_COPY_TEXTURE_TO_BUFFER:
    case CMD_UPDATE_UNIFORM_BUFFER: case CMD_FENCE_SYNC: case CMD_WAIT_SYNC: case CMD_SIGNAL_EVENT: case CMD_WAIT_EVENT:
    case CMD_REPORT_COUNTER: case CMD_RESET_COUNTER: case CMD_CLEAR_BUFFER:
    case CMD_COPY_BUFFER_TO_TEXTURE:  // (uploads: done when the frame ran; again, they overran scaled images)
        return true;
    default:
        return false;
    }
}

uint32_t nvn_window_origin(void);
uint32_t nvn_depth_mode(void);
bool renderer_origin_lower_left(void) { return nvn_window_origin() == 0; }

// (LARECOMP_VK_PROF) called once per presented frame: frames over 50 ms are logged with where their time went.
// present_ns: time in backend_present's acquire/blit/present.
void vk_prof_frame(uint64_t frame, uint64_t present_ns) {
    if (!prof) return;
    static uint64_t last_t, l_exec, l_wait, l_draws, l_subs;
    uint64_t now = SDL_GetTicksNS();
    if (last_t && now - last_t > 50000000) {
        uint64_t dt = now - last_t, ex = cum_exec - l_exec, wt = cum_wait - l_wait;
        LOG("[hitch] frame %llu: %.0f ms = backend exec %.1f (of which GPU waits %.1f) + present %.1f + other %.1f; %llu draws, "
            "%llu submits, compiles %d pending\n", (unsigned long long)frame, dt / 1e6, ex / 1e6, wt / 1e6, present_ns / 1e6,
            ((double)dt - ex - present_ns) / 1e6, (unsigned long long)(cum_draws - l_draws), (unsigned long long)(cum_submits - l_subs),
            atomic_load(&stat_compiles));
    }
    last_t = now; l_exec = cum_exec; l_wait = cum_wait; l_draws = cum_draws; l_subs = cum_submits;
    {   // (LARECOMP_VK_PROF_SPIKE_MS=<n>: a frame whose command lists took over n ms on the render thread, by category)
        static double spike = -1;
        if (spike < 0) spike = ENV("LARECOMP_VK_PROF_SPIKE_MS") ? atof(ENV("LARECOMP_VK_PROF_SPIKE_MS")) : 0;
        extern uint64_t prof_tex_full_bytes, prof_up_sample_ns, prof_up_hash_ns, prof_up_swz_ns, prof_up_astc_ns, prof_up_new, prof_up_trans_ns, prof_up_copy_ns, prof_up_cmd_ns, prof_up_glyph_ns, prof_up_expand_ns, prof_vc_new, prof_vc_create_ns, prof_vc_protect_ns, prof_vc_copy_ns, prof_tb_new, prof_tb_dead_ns, prof_tb_create_ns, prof_views_created, prof_tex_samples, prof_tex_checks;
        extern uint64_t prof_wprog_n, prof_wprog_ns, prof_wpipe_n, prof_wpipe_ns, prof_new_prog, prof_new_pipe;
        enum { NV = 38 };
        uint64_t v[NV] = {prof_exec, prof_draw, prof_pre, prof_key, prof_dyn, prof_sets, prof_tex, prof_up_ns, prof_vtx, prof_submit,
                          prof_tex_full_bytes, stat_upload_bytes, prof_up_sample_ns, prof_up_hash_ns, prof_up_swz_ns, prof_up_astc_ns, prof_up_new, prof_up_trans_ns, prof_up_copy_ns, prof_up_cmd_ns, prof_up_glyph_ns, prof_up_expand_ns, prof_vc_new, prof_vc_create_ns, prof_vc_protect_ns, prof_vc_copy_ns, prof_tb_new, prof_tb_dead_ns, prof_tb_create_ns, prof_views_created, prof_tex_samples, prof_tex_checks, prof_wprog_n, prof_wprog_ns, prof_wpipe_n, prof_wpipe_ns, prof_new_prog, prof_new_pipe};
        static uint64_t lv[NV];
        if (spike > 0 && v[0] >= lv[0] && (double)(v[0] - lv[0]) > spike * 1e6) {
            uint64_t d[NV];
            for (int i = 0; i < NV; i++) d[i] = v[i] >= lv[i] ? v[i] - lv[i] : v[i];
            LOG("[spike] frame %llu: exec %.2f ms = draw %.2f (pre %.2f key %.2f dyn %.2f sets %.2f tex %.2f (upload checks %.2f) vtx %.2f) submit %.2f;"
                " textures hashed %.1f MB, uploaded %.1f MB; upload parts: samples %.2f hash %.2f deswizzle %.2f astc %.2f staging %.2f copy %.2f cmd %.2f glyph %.2f expand %.2f ms, %llu new; vertex copies %llu new: create %.2f protect %.2f copy %.2f ms; textures %llu new: collect %.2f create %.2f ms, views %llu; texture checks %llu, sampled %llu; new progs %llu pipes %llu, waits prog %llu %.2f ms pipe %llu %.2f ms\n",
                (unsigned long long)frame, d[0] / 1e6, d[1] / 1e6, d[2] / 1e6, d[3] / 1e6,
                d[4] / 1e6, d[5] / 1e6, d[6] / 1e6, d[7] / 1e6, d[8] / 1e6, d[9] / 1e6, d[10] / 1048576.0, d[11] / 1048576.0,
                d[12] / 1e6, d[13] / 1e6, d[14] / 1e6, d[15] / 1e6, d[17] / 1e6, d[18] / 1e6, d[19] / 1e6, d[20] / 1e6, d[21] / 1e6, (unsigned long long)d[16], (unsigned long long)d[22], d[23] / 1e6, d[24] / 1e6, d[25] / 1e6, (unsigned long long)d[26], d[27] / 1e6, d[28] / 1e6, (unsigned long long)d[29], (unsigned long long)d[31], (unsigned long long)d[30], (unsigned long long)d[36], (unsigned long long)d[37], (unsigned long long)d[32], d[33] / 1e6, (unsigned long long)d[34], d[35] / 1e6);
        }
        memcpy(lv, v, sizeof v);
    }
}

static void save_pipeline_cache(void *a) {
    (void)a;
    uint64_t t0 = SDL_GetTicksNS();
    vk_pipeline_cache_save();
    LOG("[vk] pipeline cache saved (%.0f ms)\n", (SDL_GetTicksNS() - t0) / 1e6);
}

const char *renderer_stats(void) {
    // The pipeline cache was written only at a clean exit: a crash or a killed process lost every pipeline built in
    // the session, and the next one popped in again. Save it on a worker once a burst of new pipelines settles
    // (no new ones for ~6 s: entering an area builds them for a few seconds).
    static int quiet, last_new;
    int np = atomic_load(&new_pipes);
    if (np && np == last_new) {
        if (++quiet >= 3) { atomic_fetch_sub(&new_pipes, np); np = 0; quiet = 0; run_job(save_pipeline_cache, NULL); }
    } else {
        quiet = 0;
    }
    last_new = np;
    static int dumps;
    if (dumps++ < 3) {
        LOG("[vk] command counts:");
        for (int i = 0; i < CMD_COUNT; i++) if (op_counts[i]) LOG(" %d:%llu", i, (unsigned long long)op_counts[i]);
        LOG("\n");
    }
    {
        static uint64_t last_p, last_q, last_l;
        uint64_t p = atomic_load(&g_cache_progs), q = atomic_load(&g_cache_pipes), l = atomic_load(&g_cache_libs);
        if (p != last_p || q != last_q || l != last_l)
            LOG("[cache] programs %llu (+%llu), pipelines %llu (+%llu, %llu replaced in a full table), pipeline parts %llu (+%llu)\n",
                (unsigned long long)p, (unsigned long long)(p - last_p), (unsigned long long)q, (unsigned long long)(q - last_q),
                (unsigned long long)atomic_load(&g_cache_pipes_replaced), (unsigned long long)l, (unsigned long long)(l - last_l));
        last_p = p, last_q = q, last_l = l;
    }
    snprintf(stats_text, sizeof stats_text,
             "%llu draws, %llu skipped, %llu passes, %llu dispatches, %llu flushes, wait %.0fms, compile %d (translate %.0fms, spirv %.0fms, pipelines %.0fms, draws waited %.0fms), fbcopy %llu, upload %.1fMB",
             (unsigned long long)stat_draws, (unsigned long long)stat_skipped, (unsigned long long)stat_passes,
             (unsigned long long)stat_dispatches, (unsigned long long)stat_flushes, (double)stat_wait_ns / 1e6,
             atomic_exchange(&stat_compiles, 0), atomic_exchange(&stat_translate_ns, 0) / 1e6, atomic_exchange(&stat_spirv_ns, 0) / 1e6, atomic_exchange(&stat_pipe_ns, 0) / 1e6,
             stat_shader_wait_ns / 1e6,
             (unsigned long long)stat_feedback_copies, (double)stat_upload_bytes / 1048576.0);
    if (vk_gpu_ms() >= 0) {
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " [gpu %.2f ms/frame]", vk_gpu_ms());
    }
    if (stat_pass_barriers + stat_pass_nobarrier) {
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " [barriers before %llu of %llu passes]", (unsigned long long)stat_pass_barriers,
                 (unsigned long long)(stat_pass_barriers + stat_pass_nobarrier));
        stat_pass_barriers = stat_pass_nobarrier = 0;
    }
    if (atomic_load(&stat_ready_n)) {
        size_t n = strlen(stats_text);
        uint64_t rn = atomic_exchange(&stat_ready_n, 0);
        snprintf(stats_text + n, sizeof stats_text - n, " [new pipelines %llu: ready after %.0f ms avg, %.0f max]", (unsigned long long)rn,
                 atomic_exchange(&stat_ready_ns, 0) / 1e6 / rn, atomic_exchange(&stat_ready_max_ns, 0) / 1e6);
    }
    if (g_vk.gpl) {
        size_t n = strlen(stats_text);
        uint64_t nl = atomic_exchange(&stat_libs, 0), nk = atomic_exchange(&stat_links, 0);
        double tl = atomic_exchange(&stat_lib_ns, 0) / 1e6, tk = atomic_exchange(&stat_link_ns, 0) / 1e6;
        uint64_t no = atomic_exchange(&stat_opts, 0);
        double to = atomic_exchange(&stat_opt_ns, 0) / 1e6;
        if (nl || nk || no) snprintf(stats_text + n, sizeof stats_text - n, " [gpl: %llu parts %.0fms, %llu links %.0fms, %llu optimized %.0fms]",
                                     (unsigned long long)nl, tl, (unsigned long long)nk, tk, (unsigned long long)no, to);
    }
    {
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | vertex ranges %llu copied, %llu in place", (unsigned long long)stat_vtx_copied, (unsigned long long)stat_vtx_direct);
        if (g_vcache_hits || g_vcache_uploads)
            n = strlen(stats_text), snprintf(stats_text + n, sizeof stats_text - n, ", %llu in VRAM (%llu uploaded)", (unsigned long long)g_vcache_hits, (unsigned long long)g_vcache_uploads);
        stat_vtx_copied = stat_vtx_direct = 0;
        g_vcache_hits = g_vcache_uploads = 0;
        if (ENV("LARECOMP_VCACHE_LOG")) {
            extern uint64_t g_vcache_why[8], g_vcache_gpuw;
            LOG("[vcache] misses: big/outside %llu volatile %llu became volatile %llu gpuw %llu; entries %llu; bytes partial %llu full %llu\n", (unsigned long long)g_vcache_why[0],
                (unsigned long long)g_vcache_why[1], (unsigned long long)g_vcache_why[2], (unsigned long long)g_vcache_gpuw, (unsigned long long)g_vcache_why[5],
                (unsigned long long)g_vcache_why[3], (unsigned long long)g_vcache_why[4]);
            g_vcache_why[0] = g_vcache_why[1] = g_vcache_why[2] = g_vcache_why[3] = g_vcache_why[4] = g_vcache_gpuw = 0;
        }
    }
    stat_feedback_copies = stat_upload_bytes = stat_shader_wait_ns = 0;
    if (g_exp_count[0] | g_exp_count[1] | g_exp_count[2] | g_exp_count[3]) {
        LOG("[exp] skin identity %llu, ubo direct %llu, ubo copied %llu, one-bone shaders %llu\n",
            (unsigned long long)g_exp_count[0], (unsigned long long)g_exp_count[1], (unsigned long long)g_exp_count[2],
            (unsigned long long)g_exp_count[3]);
        g_exp_count[0] = g_exp_count[1] = g_exp_count[2] = 0;  // ([3]: a running total)
    }
    if (g_skin_watch) {
        const uint32_t *w = (const uint32_t *)(uintptr_t)g_skin_watch;
        LOG("[skin] cb3 %llx now: row8 %08x %08x row9 %08x %08x\n", (unsigned long long)g_skin_watch, w[32], w[33], w[36], w[37]);
    }
    if (stat_skipped) {
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " [skip: noprog %llu compiling %llu failed %llu noobj %llu mode %llu pipe-building %llu pipe-failed %llu]",
                 (unsigned long long)stat_skip_why[0], (unsigned long long)stat_skip_why[1],
                 (unsigned long long)stat_skip_why[2], (unsigned long long)stat_skip_why[3],
                 (unsigned long long)stat_skip_why[4], (unsigned long long)stat_skip_why[5],
                 (unsigned long long)stat_skip_why[6]);
        memset(stat_skip_why, 0, sizeof stat_skip_why);
    }
    if (prof) {
        prof_up = prof_up_ns;
        prof_up_ns = 0;
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | gpu %.0f cpu exec %.0f draw %.0f (pre %.0f key %.0f dyn %.0f sets %.0f tex %.0f (upload %.0f) vtx %.0f) submit %.0f ms",
                 prof_gpu / 1e6, prof_exec / 1e6, prof_draw / 1e6, prof_pre / 1e6, prof_key / 1e6, prof_dyn / 1e6, prof_sets / 1e6,
                 prof_tex / 1e6, prof_up / 1e6, prof_vtx / 1e6, prof_submit / 1e6);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | dsalloc %.0f dsupdate %.0f dsbind %.0f pipebind %.0f ms", prof_dalloc / 1e6,
                 prof_dupd / 1e6, prof_dbind / 1e6, prof_bindpipe / 1e6);
        prof_dalloc = prof_dupd = prof_dbind = prof_bindpipe = 0;
        extern uint64_t prof_tex_checks, prof_tex_samples, prof_tex_full, prof_tex_full_bytes, prof_tex_dyn;
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | tex checks %llu samples %llu full %llu (%.1f MB, dynamic %llu)",
                 (unsigned long long)prof_tex_checks, (unsigned long long)prof_tex_samples, (unsigned long long)prof_tex_full,
                 prof_tex_full_bytes / 1048576.0, (unsigned long long)prof_tex_dyn);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | pass ends:");
        for (int l = 0; l < 4096; l++)
            if (prof_end_line[l] > 20) { n = strlen(stats_text); snprintf(stats_text + n, sizeof stats_text - n, " %d:%u", l, prof_end_line[l]); }
        memset(prof_end_line, 0, sizeof prof_end_line);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | views created %llu", (unsigned long long)prof_views_created);
        prof_views_created = 0;
        prof_tex_checks = prof_tex_samples = prof_tex_full = prof_tex_full_bytes = prof_tex_dyn = 0;
        prof_exec = prof_draw = prof_sets = prof_vtx = prof_submit = prof_pre = prof_key = prof_dyn = prof_tex = prof_up = prof_gpu = 0;
    }
    stat_draws = stat_skipped = stat_passes = stat_dispatches = stat_flushes = stat_wait_ns = 0;
    return stats_text;
}

static void st_defaults(void) {
    memset(&st, 0, sizeof st);
    for (int i = 0; i < 16; i++) {
        st.viewport[i][2] = 1280; st.viewport[i][3] = 720;
        st.scissor[i][2] = 16384; st.scissor[i][3] = 16384;
        st.depth_range[i][1] = 1.0f;
    }
    st.channel_mask = 0xFFFFFFFF;
    st.stencil_mask[0] = st.stencil_mask[1] = 0xFF;
    st.stencil_vmask[0] = st.stencil_vmask[1] = 0xFF;
    st.ds[1] = 1; st.ds[2] = 2;
    st.poly[1] = 1; st.poly[2] = 2;
    for (int i = 0; i < 8; i++) { st.blend[i][1] = 2; st.blend[i][2] = 1; st.blend[i][3] = 2; st.blend[i][4] = 1; st.blend[i][5] = 1; st.blend[i][6] = 1; }
}

void vk_exec_init(void) {
    prof = ENV("LARECOMP_VK_PROF") != NULL;
    if (prof) prof_up_ns = 0;
    // GPU time per frame (the FPS counter, dynamic resolution, the stats line): always, unless LARECOMP_VK_NO_GPUTIME=1
    if ((prof || !ENV("LARECOMP_VK_NO_GPUTIME")) && g_vk.props.limits.timestampPeriod > 0 && g_vk.props.limits.timestampComputeAndGraphics) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = SUBS * 2};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &prof_qp) != VK_SUCCESS) prof_qp = VK_NULL_HANDLE;
    }
    st_defaults();
}

void renderer_texture_scale(NvnTexture *t, float *sx, float *sy);
bool renderer_texture_presented(NvnTexture *t);
bool renderer_texture_first_write(NvnTexture *t);
bool renderer_texture_level_fresh(NvnTexture *t);
bool renderer_texture_frame_fresh(NvnTexture *t);
void renderer_texture_mark_presented(NvnTexture *t);
uint64_t renderer_present_count(void);
void renderer_save_texture_tex(TexBackend *b, const char *path);
static float pass_sx = 1, pass_sy = 1;
static float vp_sx = 1, vp_sy = 1, vp_ox = 0, vp_oy = 0;
static float tex_sc[2][64][2];
static uint32_t tex_wh[2][64][2];

// ================================================================== occlusion queries
// The game's SAMPLES_PASSED counters (Z-A: ~45 a frame, each object's proxy box drawn between ResetCounter and
// ReportCounter) as Vulkan occlusion queries: the report's counter word gets the query's result, copied by the GPU in
// the frame's last submission (the one presenting: its occlusion passes are long done, so the copies' waits cost
// nothing - copied at every submission's end, right after the queries, they drained the GPU: ~20% fps). (Written from
// the CPU once the GPU was done, frames later, results landed on memory the game had reused: vertex explosions.)
// Until then, and wherever a query can't be made (no pass running, the pass ended before the report, no pool), the
// counter reads "visible". (It read 0: every proxy hidden, so objects popped in late
// - Lumiose's arrival cutscene.) LARECOMP_VK_SAMPLES=<n>: no queries, every report n (0: the old behaviour).
#define OQ_SLOTS 4096
#define OQ_VISIBLE 0x10000ull
static VkQueryPool oq_pool;
static int oq_mode = -1;  // 1: queries; 0: every report oq_fixed
static uint64_t oq_fixed;
static uint32_t oq_next;
static int oq_active = -1;  // the query running in this pass (its slot)
static bool oq_wanted;      // a reset came outside a pass: the query begins with the next one
static uint64_t oq_seq[OQ_SLOTS];  // submission each slot was last used in
static struct { uint32_t slot; uint64_t off; } oq_copy[OQ_SLOTS];  // reports waiting for their query's result
static int oq_ncopy;
static uint64_t oq_stats[3];  // queries copied, reports without a query, queries cut by a pass end

static void oq_init(void) {
    const char *e = ENV("LARECOMP_VK_SAMPLES");
    oq_mode = 0, oq_fixed = e ? strtoull(e, NULL, 0) : OQ_VISIBLE;
    if (e) { LOG("[vk] occlusion queries off: every report %llu\n", (unsigned long long)oq_fixed); return; }
    VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION,
                                .queryCount = OQ_SLOTS};
    if (!g_vk.oq_host_reset || vkCreateQueryPool(g_vk.dev, &qi, NULL, &oq_pool) != VK_SUCCESS) {
        LOG("[vk] occlusion queries unavailable (host query reset %d): every report says visible\n", g_vk.oq_host_reset);
        return;
    }
    vkResetQueryPool(g_vk.dev, oq_pool, 0, OQ_SLOTS);
    oq_mode = 1;
    LOG("[vk] occlusion queries on (%s sample counts)\n", g_vk.oq_precise ? "exact" : "non-zero");
}
static void oq_begin(void) {  // (in a pass)
    uint32_t slot = oq_next % OQ_SLOTS;
    if (oq_seq[slot] == vk_pending_seq() || oq_ncopy == OQ_SLOTS) return;  // (ring full: no query, "visible")
    if (oq_seq[slot] && oq_seq[slot] > vk_completed_seq()) return;           // (its last copy still running: same)
    for (int i = 0; i < oq_ncopy; i++) if (oq_copy[i].slot == slot) return;  // (not copied yet)
    oq_next++;
    if (oq_seq[slot]) vkResetQueryPool(g_vk.dev, oq_pool, slot, 1);
    oq_seq[slot] = vk_pending_seq();
    vkCmdBeginQuery(vk_cmd(), oq_pool, slot, g_vk.oq_precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    oq_active = (int)slot;
}
static void oq_pass_begun(void) {
    if (oq_wanted && oq_mode == 1) oq_begin();
    oq_wanted = false;
}
static void oq_pass_ending(void) {  // (before vkCmdEndRendering: a query can't outlive its pass)
    if (oq_active < 0) return;
    vkCmdEndQuery(vk_cmd(), oq_pool, (uint32_t)oq_active);
    oq_active = -1;
    oq_stats[2]++;
}
static VkBuffer gmem_buf;
static bool oq_copied;  // results were copied in the submission being recorded (it ends with a barrier for the CPU)
// The results into the reports' counter words, right after the pass their queries ran in (outside it), in the same
// submission as the draws they measured: written where the game's own fences cover them, as a Switch GPU writes a
// report in its command stream. (Copied once a frame at its very end, a result could land after the game, done
// waiting for that frame, had reused the word for something else - vertices: explosions.)
static void oq_copy_out(void) {
    if (!oq_ncopy) return;
    VkCommandBuffer cb = vk_cmd();
    for (int i = 0; i < oq_ncopy; i++) {
        vkCmdCopyQueryPoolResults(cb, oq_pool, oq_copy[i].slot, 1, gmem_buf, oq_copy[i].off, 8,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        oq_seq[oq_copy[i].slot] = vk_pending_seq();  // (reusable once this submission is done)
    }
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    oq_stats[0] += (uint64_t)oq_ncopy;
    oq_ncopy = 0;
    oq_copied = true;
}
static void oq_flush(void) {  // (each submission, outside any pass) what is left, and the barrier for the CPU's reads
    oq_copy_out();
    if (!oq_copied) return;
    oq_copied = false;
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(vk_cmd(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

// ================================================================== render passes
static VkFormat pass_color[8], pass_depth, pass_stencil;
static uint32_t pass_ncolor, pass_samples = 1, pass_w, pass_h;
static int debug_pass_no;

static void vk_end_pass_at(int line);
void vk_end_pass(void) { vk_end_pass_at(0); }
#define vk_end_pass() vk_end_pass_at(__LINE__)
static bool g_storage_written;  // a draw of this pass wrote storage images (SUST / SURED)
static NvnTexture *pass_rt[9];  // the running pass's color targets and depth (frame dumps save these at its end)
static void vk_end_pass_at(int line) {
    if (!in_pass) return;
    if (prof && line < 4096) prof_end_line[line]++;
    oq_pass_ending();
    vkCmdEndRendering(vk_cmd());
    in_pass = false;
    oq_copy_out();  // (the occlusion query results of this pass, into the game's report words)
    if (g_storage_written) {  // the next passes sample (or keep accumulating into) what the shaders stored
        g_storage_written = false;
        hz_all = true;
        VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
        vkCmdPipelineBarrier(vk_cmd(), VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
    static long pass_fmt = -2;  // LARECOMP_DEBUG_PASS_FMT=<nvn format>: every pass rendering to that format, all frames
    if (pass_fmt == -2) pass_fmt = ENV("LARECOMP_DEBUG_PASS_FMT") ? strtol(ENV("LARECOMP_DEBUG_PASS_FMT"), NULL, 0) : -1;
    if (pass_fmt >= 0 && st.rt.ncolor && st.rt.color[0] && st.rt.color[0]->backend && (long)st.rt.color[0]->format == pass_fmt && st.rt.color[0]->width < 1000) {
        static int n;
        if (n < 400) {
            char path[256];
            snprintf(path, sizeof path, "build/fmtpass_%05llu_%03d_%ux%u.png", (unsigned long long)renderer_present_count(), n++,
                     st.rt.color[0]->width, st.rt.color[0]->height);
            renderer_save_texture_tex(st.rt.color[0]->backend, path);
        }
    }
    if (g_debug_frame && pass_rt[0] && pass_rt[0]->backend) {
        char path[256];
        static uint64_t debug_pass_frame = ~0ull;
        static int multi = -1;
        if (multi < 0) multi = ENV("LARECOMP_DEBUG_FRAMES") != NULL;
        if (multi && renderer_present_count() != debug_pass_frame) { debug_pass_frame = renderer_present_count(); debug_pass_no = 0; }
        if (multi) snprintf(path, sizeof path, "build/pass_%llu_%03d.png", (unsigned long long)debug_pass_frame, debug_pass_no++);
        else snprintf(path, sizeof path, "build/pass_%03d.png", debug_pass_no++);
        renderer_save_texture_tex(pass_rt[0]->backend, path);
        for (int i = 1; i < 8 && !multi; i++)  // (the other color targets: build/pass_NNN_c<i>.png)
            if (pass_rt[i] && pass_rt[i]->backend) {
                snprintf(path, sizeof path, "build/pass_%03d_c%d.png", debug_pass_no - 1, i);
                renderer_save_texture_tex(pass_rt[i]->backend, path);
            }
    }
}

static uint32_t level_extent(uint32_t v, uint32_t lv) { v >>= lv; return v ? v : 1; }

static void begin_pass(void) {
    if (in_pass && !st.rt_dirty) return;
    vk_end_pass();
    if (dl_on == 2) renderer_flush();  // (LARECOMP_VK_DEVLOST_TRACE=2: each pass its own submission)
    st.rt_dirty = false;
    if (ENV("LARECOMP_LOG_SMALL_PASSES")) {  // (research: every distinct pass with a target under 1000 px wide)
        static uint64_t seen[256];
        static int nseen;
        for (int i = 0; i < st.rt.ncolor; i++) {
            NvnTexture *t = st.rt.color[i];
            if (!t || t->width >= 1000) continue;
            uint64_t key = (uint64_t)t->width << 40 | (uint64_t)t->height << 20 | t->format << 4 | (uint64_t)i;
            int k = 0;
            while (k < nseen && seen[k] != key) k++;
            if (k == nseen && nseen < 256) {
                seen[nseen++] = key;
                LOG("[pass] target %d of %d: %ux%u fmt %#x (frame %llu)\n", i, st.rt.ncolor, t->width, t->height, t->format,
                    (unsigned long long)renderer_present_count());
            }
        }
    }
    VkCommandBuffer cb = vk_cmd();
    {
        NvnTexture *t0n = st.rt.ncolor ? st.rt.color[0] : NULL;
        dl_note("pass %d color %ux%u fmt %#x depth %p", st.rt.ncolor, t0n ? t0n->width : 0, t0n ? t0n->height : 0, t0n ? t0n->format : 0,
                (void *)st.rt.depth);
    }
    if (g_gpuprof_frame) {
        NvnTexture *t0 = st.rt.ncolor ? st.rt.color[0] : NULL;
        gp_mark(cb, "pass: %d color %ux%u fmt %#x, depth %s", st.rt.ncolor, t0 ? t0->width : 0, t0 ? t0->height : 0,
                t0 ? t0->format : 0, st.rt.depth ? "yes" : "no");
    }
    VkRenderingAttachmentInfo ca[8], da = {0}, sa = {0};
    for (int i = 0; i < 8; i++) pass_rt[i] = i < st.rt.ncolor ? st.rt.color[i] : NULL;
    for (int i = 0; i < 8 && !pass_rt[0]; i++) if (pass_rt[i]) { pass_rt[0] = pass_rt[i]; pass_rt[i] = NULL; }  // (slot 0 empty)
    pass_rt[8] = st.rt.depth;
    uint32_t w = 0, minw = UINT32_MAX, minh = UINT32_MAX;  // w: first attachment seen
    uint32_t layers = 1;  // a 3D color target: all its slices (vk_view's 2D-array render view)
    pass_ncolor = (uint32_t)st.rt.ncolor;
    pass_samples = 1;
    for (int i = 0; i < st.rt.ncolor; i++) {
        NvnTexture *t = st.rt.color[i];
        ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        pass_color[i] = VK_FORMAT_UNDEFINED;
        if (!t) continue;
        renderer_mark_gpu_owned(t);
        VkImageView v = vk_view(t, &st.rt.cview[i], st.rt.has_cview[i], true, TEXTYPE_2D);
        TexBackend *b = t->backend;
        if (!v) continue;
        ca[i].imageView = v;
        ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        bool fresh = (int)renderer_texture_level_fresh(t) | (int)renderer_texture_frame_fresh(t);  // both: each marks its state
        if (renderer_texture_first_write(t) || fresh) {
            ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            ca[i].clearValue.color.float32[3] = fresh ? 0.0f : 1.0f;
        }
        pass_color[i] = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 4) && vk_format(t->format) == b->format
                      ? vk_format(st.rt.cview[i].format) : b->format;
        uint32_t lv = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 1) ? st.rt.cview[i].base_level : 0;
        uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
        if (b->base_type == VK_IMAGE_VIEW_TYPE_3D) {
            uint32_t z = level_extent(b->depth, lv);
            if (st.rt.has_cview[i] && (st.rt.cview[i].setmask & 2) && st.rt.cview[i].num_layers && st.rt.cview[i].num_layers < z)
                z = st.rt.cview[i].num_layers;
            if (i == 0 || z < layers) layers = z;  // (the pass's layer count: within every attachment)
        }
        if (!w) { renderer_texture_scale(t, &pass_sx, &pass_sy); w = ew; }
        if (ew < minw) minw = ew;
        if (eh < minh) minh = eh;
        pass_samples = b->samples;
    }
    // Bound color slots without a texture / view went in as VK_NULL_HANDLE attachments (legal), but Intel's Windows
    // driver for UHD 630 crashes in vkCmdBeginRendering on them (LAS Recomp f4cb039, its issues #8 / #46). Empty slots
    // at the end are left out (pipelines and clears follow pass_ncolor); one in the middle is logged once.
    while (pass_ncolor && !ca[pass_ncolor - 1].imageView) pass_ncolor--;
    static bool logged_hole;
    for (uint32_t i = 0; i < pass_ncolor && !logged_hole; i++)
        if (!ca[i].imageView) {
            logged_hole = true;
            LOG("[vk] pass with an empty color slot %u of %u (texture %s)\n", i, pass_ncolor, st.rt.color[i] ? "without a view" : "none");
        }
    pass_depth = pass_stencil = VK_FORMAT_UNDEFINED;
    bool has_d = false, has_s = false;
    if (st.rt.depth) {
        renderer_mark_gpu_owned(st.rt.depth);
        VkImageView v = vk_view(st.rt.depth, &st.rt.dview, st.rt.has_dview, true, TEXTYPE_2D);
        TexBackend *b = st.rt.depth->backend;
        if (v && renderer_format_is_depth(st.rt.depth->format)) {
            VkRenderingAttachmentInfo a = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v,
                                           .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                           .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
            if (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) { da = a; has_d = true; pass_depth = b->format; }
            if (b->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) { sa = a; has_s = true; pass_stencil = b->format; }
            uint32_t lv = st.rt.has_dview && (st.rt.dview.setmask & 1) ? st.rt.dview.base_level : 0;
            uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
            if (!w) { renderer_texture_scale(st.rt.depth, &pass_sx, &pass_sy); w = ew; }
            if (ew < minw) minw = ew;
            if (eh < minh) minh = eh;
            if (!pass_ncolor) pass_samples = b->samples;
        }
    }
    if (!w) { pass_sx = pass_sy = 1; minw = minh = 16; }
    pass_w = minw; pass_h = minh;
    if (g_debug_frame && st.rt.depth && st.rt.depth->backend)  // (research: which layer / level a depth pass targets)
        LOG("[pass] depth %p %ux%u layers %u view set %#x layer %u+%u level %u | layers %u, colors %u\n", (void *)st.rt.depth,
            st.rt.depth->width, st.rt.depth->height, ((TexBackend *)st.rt.depth->backend)->layers, st.rt.has_dview ? st.rt.dview.setmask : 0,
            st.rt.dview.min_layer, st.rt.dview.num_layers, st.rt.dview.base_level, layers, pass_ncolor);
    {   // passes, copies and dispatches see each other's results: the barrier, where this pass needs one (hz_w, above)
        static int always = -1;
        if (always < 0) always = ENV("LARECOMP_VK_PASS_BARRIER") && atoi(ENV("LARECOMP_VK_PASS_BARRIER"));
        TexBackend *att[9];
        int natt = 0;
        for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i] && st.rt.color[i]->backend) att[natt++] = st.rt.color[i]->backend;
        if (st.rt.depth && st.rt.depth->backend) att[natt++] = st.rt.depth->backend;
        uint64_t key = 0x9e3779b97f4a7c15ull;
        for (int i = 0; i < natt; i++) key = (key ^ (uint64_t)(uintptr_t)att[i]) * 0x100000001b3ull;
        hz_pass_key = key;
        if (hz_learned[key & 1023] == key && (hz_nw || hz_all)) hz_force = true;  // (its draws will sample what came before)
        bool need = always || hz_all || hz_force;
        for (int i = 0; i < natt && !need; i++) need = hz_has(hz_w, hz_nw, att[i]) || hz_has(hz_r, hz_nr, att[i]);
        if (need && !always && !hz_all) {
            // only passes since the last barrier: their attachment writes and shader reads, before this pass's
            // attachment accesses (write after write / read) and - a draw samples what they rendered - shader reads.
            // The GPU can start this pass's work up to that point (vertex shading of a pass whose draws only render to
            // what came before) instead of draining first: ~6% of Z-A's GPU time on the RTX 4090 (4K)
            VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                   .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                                   VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                                   VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                                   .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                                   VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                                                   (hz_force ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT : 0),
                                   .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                                    (hz_force ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT : 0)};
            VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
            vkCmdPipelineBarrier2(cb, &di);
            hz_nw = hz_nr = 0;
            hz_force = false;
            stat_pass_barriers++;
        } else if (need) {
            vk_full_barrier(cb);
            hz_nw = hz_nr = 0;
            hz_all = hz_force = false;
            stat_pass_barriers++;
        } else stat_pass_nobarrier++;
        for (int i = 0; i < natt; i++) hz_add(hz_w, &hz_nw, att[i]);
    }
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {minw, minh}}, .layerCount = layers,
                          .colorAttachmentCount = pass_ncolor, .pColorAttachments = ca,
                          .pDepthAttachment = has_d ? &da : NULL, .pStencilAttachment = has_s ? &sa : NULL};
    vkCmdBeginRendering(cb, &ri);
    in_pass = true;
    oq_pass_begun();
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    stat_passes++;
}

static void pass_size(int *w, int *h) {
    *w = *h = 0;
    for (int i = 0; i < st.rt.ncolor && !*w; i++)
        if (st.rt.color[i]) { *w = (int)st.rt.color[i]->width; *h = (int)st.rt.color[i]->height; }
    if (!*w && st.rt.depth) { *w = (int)st.rt.depth->width; *h = (int)st.rt.depth->height; }
    *w = (int)lroundf(*w * pass_sx);
    *h = (int)lroundf(*h * pass_sy);
}

static VkRect2D scissor_rect(void) {
    int w, h;
    pass_size(&w, &h);
    if (!w) return (VkRect2D){{0, 0}, {pass_w, pass_h}};
    int x = st.scissor[0][0], y = st.scissor[0][1], sw = st.scissor[0][2], sh = st.scissor[0][3];
    if (vp_sx != 1 || vp_sy != 1 || vp_ox != 0 || vp_oy != 0) {
        double x0 = floor(x * (double)vp_sx + vp_ox), y0 = floor(y * (double)vp_sy + vp_oy);
        double x1 = ceil(((double)x + sw) * vp_sx + vp_ox), y1 = ceil(((double)y + sh) * vp_sy + vp_oy);
        if (sw >= 16384) x1 = 1e9;
        if (sh >= 16384) y1 = 1e9;
        if (x1 > 1e9) x1 = 1e9;
        if (y1 > 1e9) y1 = 1e9;
        x = (int)x0; y = (int)y0; sw = (int)(x1 - x0); sh = (int)(y1 - y0);
    }
    if (x < 0) { sw += x; x = 0; }
    if (y < 0) { sh += y; y = 0; }
    if (w > (int)pass_w) w = (int)pass_w;
    if (h > (int)pass_h) h = (int)pass_h;
    if (x >= w || y >= h) { x = y = 0; sw = sh = 0; }
    if (x + sw > w) sw = w - x;
    if (y + sh > h) sh = h - y;
    if (sw < 0) sw = 0;
    if (sh < 0) sh = 0;
    return (VkRect2D){{x, y}, {(uint32_t)sw, (uint32_t)sh}};
}

// (the viewport and scissor draw_ set last: its state cache, dc - reset with it by dc_begin)
static VkViewport dc_vp;
static VkRect2D dc_sc;
static void apply_scissor(void) {
    VkRect2D r = scissor_rect();
    if (!memcmp(&r, &dc_sc, sizeof r)) return;
    dc_sc = r;
    vkCmdSetScissor(vk_cmd(), 0, 1, &r);
}

// Metal convention (y up in NDC, origin top-left) through a negative-height viewport.
static void set_viewport(float x, float y, float w, float h, float zn, float zf) {
    if (w <= 0) w = 1;
    if (h == 0) h = 1;
    VkViewport vp = {x, y + h, w, -h, fminf(fmaxf(zn, 0), 1), fminf(fmaxf(zf, 0), 1)};
    if (!memcmp(&vp, &dc_vp, sizeof vp)) return;  // (the same again: NVIDIA's driver spent as long on it as on a draw)
    dc_vp = vp;
    vkCmdSetViewport(vk_cmd(), 0, 1, &vp);
}

// ================================================================== clears
static VkPipelineLayout pc_layout(void) {  // push constants only (clears)
    static VkPipelineLayout l;
    if (!l) {
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    }
    return l;
}

static const VkDynamicState DYN[] = {
    VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
    VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE};
#define NDYN (sizeof DYN / sizeof DYN[0])

// Fixed-function state that isn't part of a pipeline is set explicitly on every bind (it persists across pipelines).
static void dyn_defaults(VkCommandBuffer cb) {
    g_dyn_epoch++;  // (draw_'s state cache: dc)
    vkCmdSetDepthBias(cb, 0, 0, 0);
    vkCmdSetDepthBiasEnable(cb, VK_FALSE);
    float bc[4] = {0, 0, 0, 0};
    vkCmdSetBlendConstants(cb, bc);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetCullMode(cb, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(cb, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetPrimitiveTopology(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetDepthTestEnable(cb, VK_FALSE);
    vkCmdSetDepthWriteEnable(cb, VK_FALSE);
    vkCmdSetDepthCompareOp(cb, VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilTestEnable(cb, VK_FALSE);
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                      VK_COMPARE_OP_ALWAYS);
    vkCmdSetPrimitiveRestartEnable(cb, VK_FALSE);
    vkCmdSetRasterizerDiscardEnable(cb, VK_FALSE);
}

typedef struct { uint64_t key; VkPipeline pipe; } PipeEnt;
#define CLEAR_TAB 256
static PipeEnt clear_tab[CLEAR_TAB];

static VkPipeline clear_pipeline(int index, int kind, uint32_t mask, bool depth, bool stencil) {
    uint64_t key = 0xcbf29ce484222325ull;
#define MIX(v) do { uint64_t v_ = (uint64_t)(v); for (int b_ = 0; b_ < 8; b_++) { key ^= (v_ >> (b_ * 8)) & 0xFF; key *= 0x100000001B3ull; } } while (0)
    MIX(index); MIX(kind); MIX(mask); MIX(depth); MIX(stencil); MIX(pass_ncolor); MIX(pass_depth); MIX(pass_stencil); MIX(pass_samples);
    for (uint32_t i = 0; i < pass_ncolor; i++) MIX(pass_color[i]);
    uint32_t slot = (uint32_t)key & (CLEAR_TAB - 1);
    for (int p = 0; p < CLEAR_TAB; p++, slot = (slot + 1) & (CLEAR_TAB - 1)) {
        if (clear_tab[slot].key == key) return clear_tab[slot].pipe;
        if (!clear_tab[slot].key) break;
    }
    char fsname[32];
    if (index >= 0) snprintf(fsname, sizeof fsname, "clear.frag.%c%d", kind == 1 ? 'i' : kind == 2 ? 'u' : 'f', index);
    else snprintf(fsname, sizeof fsname, "clear.frag.none");
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("clear.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fsname), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)pass_samples};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < pass_ncolor; i++)
        if ((int)i == index) cba[i].colorWriteMask = mask & 0xF;
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = pass_ncolor, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkPipelineRenderingCreateInfo rinfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = pass_ncolor,
                                           .pColorAttachmentFormats = pass_color, .depthAttachmentFormat = pass_depth,
                                           .stencilAttachmentFormat = pass_stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rinfo, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = pc_layout()};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p) != VK_SUCCESS) LOG("[vk] clear pipeline failed\n");
    clear_tab[slot].key = key;
    clear_tab[slot].pipe = p;
    return p;
}

static void clear(int index, int kind, const void *color, uint32_t mask, bool depth, float dval, bool stencil, uint32_t sval,
                  uint32_t smask) {
    g_dyn_epoch++;  // (draw_'s state cache: dc)
    begin_pass();
    VkCommandBuffer cb = vk_cmd();
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    VkRect2D rect = scissor_rect();
    if (!rect.extent.width || !rect.extent.height) return;
    if (index >= 0 && (index >= (int)pass_ncolor || pass_color[index] == VK_FORMAT_UNDEFINED)) return;
    bool simple = (index < 0 || (mask & 0xF) == 0xF) && (!stencil || (smask & 0xFF) == 0xFF);
    if (simple) {
        VkClearAttachment a[2];
        uint32_t n = 0;
        if (index >= 0) {
            a[n] = (VkClearAttachment){.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = (uint32_t)index};
            memcpy(a[n].clearValue.color.float32, color, 16);
            n++;
        }
        VkImageAspectFlags dsa = (depth && pass_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (stencil && pass_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if (dsa) a[n++] = (VkClearAttachment){.aspectMask = dsa, .clearValue.depthStencil = {dval, sval}};
        VkClearRect cr = {rect, 0, 1};
        if (n) vkCmdClearAttachments(cb, n, a, 1, &cr);
        return;
    }
    VkPipeline p = clear_pipeline(index, kind, mask, depth, stencil);
    if (!p) return;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    dyn_defaults(cb);
    if (depth) { vkCmdSetDepthTestEnable(cb, VK_TRUE); vkCmdSetDepthWriteEnable(cb, VK_TRUE); }
    if (stencil) {
        vkCmdSetStencilTestEnable(cb, VK_TRUE);
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE,
                          VK_COMPARE_OP_ALWAYS);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, smask);
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, sval);
    }
    int w, h;
    pass_size(&w, &h);
    VkViewport vp = {0, 0, (float)(w ? w : (int)pass_w), (float)(h ? h : (int)pass_h), 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &rect);
    float pc[8] = {0};
    if (color) memcpy(pc, color, 16);
    pc[4] = dval;
    vkCmdPushConstants(cb, pc_layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
}

// ================================================================== programs
#define MAX_BIND 96
typedef struct {
    ShaderInfo *vi, *fi;
    uint64_t code[2], cmax[2];  // translation inputs (copied: translated on the translator thread)
    uint64_t cst[2], ncst[2];   // (and the compiler constants: BRX jump tables)
    uint64_t fs_len;            // (the warm-up's copy of the fragment stage: its bytes; 0 = the game's memory)
    ShaderKey vkey, fkey;
    VkShaderModule vs, fs;
    VkBindingInfo binds[MAX_BIND];
    int nbinds;
    VkDescriptorSetLayout set_layout[2];
    VkPipelineLayout layout;
    bool vs_uses_vid;
    _Atomic int state;  // 0 building, 1 ready, 2 failed
    uint64_t fs_hash;   // vk_fs_hash of the fragment program (draw_: made on first use, fs_hash_ok)
    bool fs_hash_ok;
    bool warm;          // (vk_warm_program's: released right after, no pipeline parts)
    uint64_t dbg_id[2], dbg_spv[2];  // (LARECOMP_WARMUP_LOG: program hashes, SPIR-V hashes)
    uint64_t content[2];  // (LARECOMP_LOG_PIPEKEYS: the stages' content hashes)
} Prog;

static VkPipelineLayout make_layout(Prog *p, int nsets, VkShaderStageFlags s0, VkShaderStageFlags s1) {
    p->set_layout[0] = vk_set_layout(p->binds, p->nbinds, 0, s0);
    if (nsets > 1) p->set_layout[1] = vk_set_layout(p->binds, p->nbinds, 1, s1);
    VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = (uint32_t)nsets,
                                     .pSetLayouts = p->set_layout};
    VkPipelineLayout l;
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    return l;
}

static void save_glsl(const char *src, uint64_t addr, const char *suffix) {
    if (!ENV("LARECOMP_SAVE_MSL")) return;
    mkdir("build/glsl", 0755);
    char path[256];
    snprintf(path, sizeof path, "build/glsl/%llx_%s.glsl", (unsigned long long)addr, suffix);
    FILE *f = fopen(path, "w");
    if (f) { fputs(src, f); fclose(f); }
}

static void gpl_prebuild(void *arg);
// Legends Z-A's shadow mask takes 16 taps of its shadow maps (t5: depth, t6: an 8-bit id of what is drawn there) and
// counts a tap lit when the receiver is at least as near the light (r40 >= depth) or the tap is the receiver itself
// (|r5 - depth| <= r7 with the same id r4: the depth register is zeroed). Each tap reads one texel, so a shadow's edge
// steps along the map's texels - blocks at 1024, smaller steps at 2048 / 4096. Here each tap's 0 / 1 becomes the same
// test on the 4 texels around it, weighted by where the tap lies between them (bilinear PCF, what a hardware compare
// sampler does). The "itself" test gets twice the tolerance for those texels: a neighbour on a sloping surface is a
// little nearer or farther than the receiver, and at the game's tolerance some of them count as shadow (the ground
// went darker; a receiver-plane bias from screen-space derivatives instead left specks - derivatives in this
// shader's branches aren't reliable). The tap's position is saved before its read: the read often lands in the
// position's own register. Returns the new GLSL, or NULL if the code isn't laid out as expected (16 taps, 16 tests).
static char *za_soft_shadows(const char *src) {
    // (the translator reads a register as a float as uintBitsToFloat(r) - as_f(r) before 2026-10-28 - and the id map
    // is an integer texture or, before, a float one: both forms are matched)
    bool uid = strstr(src, "usampler2DArray t6;") != NULL;
    char helper[2048];
    snprintf(helper, sizeof helper,
        "float pcf_bilinear(vec3 tap, float ref, float tol, uint myid, int l6, float cmpref) {\n"
        "  vec2 s5 = vec2(textureSize(t5, 0).xy), s6 = floor(vec2(textureSize(t6, 0).xy) / rscale[7].xy + 0.5f);\n"
        "  vec2 p = tap.xy * s5 - 0.5f, b = floor(p), w = p - b;\n"
        "  float r = 0.0f;\n"
        "  for (int k = 0; k < 4; k++) {\n"
        "    ivec2 c = clamp(ivec2(b) + ivec2(k & 1, k >> 1), ivec2(0), ivec2(s5) - 1);\n"
        "    float d = texelFetch(t5, ivec3(c, int(tap.z)), 0).x;\n"
        "    uint id = %s(texelFetch(t6, ivec3(ivec2(rsc(uvec2((vec2(c) + 0.5f) / s5 * s6), rscale[7])), l6), 0).x);\n"
        "    float lit = ((abs(ref - d) <= 2.0f * tol && myid == id) || cmpref >= d) ? 1.0f : 0.0f;\n"
        "    r += lit * ((k & 1) != 0 ? w.x : 1.0f - w.x) * ((k >> 1) != 0 ? w.y : 1.0f - w.y);\n"
        "  }\n"
        "  return r;\n"
        "}\n", uid ? "" : "floatBitsToUint");
    static const char *const k_tap[2] = {
        "texture(t5, vec3(vec2(uintBitsToFloat(r%d), uintBitsToFloat(r%d)), float(min(uint(r%d), uint(tsize5(0).z) - 1u))), uintBitsToFloat(r%d));%n",
        "texture(t5, vec3(vec2(as_f(r%d), as_f(r%d)), float(min(uint(r%d), uint(tsize5(0).z) - 1u))), as_f(r%d));%n"};
    static const char *const k_lit[2] = {
        " r%d = ((((uintBitsToFloat(r40) >= uintBitsToFloat(r%d))) && true) ? 0x3f800000u : 0u);%n",
        " r%d = ((((as_f(r40) >= as_f(r%d))) && true) ? 0x3f800000u : 0u);%n"};
    static const char *const k_r40[2] = {"    r40 = floatBitsToUint((false ? fmin(as_f(0u), uintBitsToFloat(r5))",
                                         "    r40 = as_u((false ? fmin(as_f(0u), as_f(r5))"};
    size_t cap = strlen(src) * 2 + 8192, n = 0;
    char *out = malloc(cap);
    int tap_of[256], ntap = 0, nlit = 0;
    bool captured = false, mained = false;
    for (int i = 0; i < 256; i++) tap_of[i] = -1;
#define EMIT(str, len) do { size_t l_ = (len); if (n + l_ + 1 > cap) { cap = (n + l_ + 1) * 2; out = realloc(out, cap); } memcpy(out + n, (str), l_); n += l_; } while (0)
    for (const char *l = src; *l;) {
        const char *e = strchr(l, '\n');
        size_t len = e ? (size_t)(e - l) + 1 : strlen(l);
        char buf[512];
        int u, v, ly, bias, a, b, d, end = 0, end2 = 0, f = -1;
        char c;
        const char *t = len < 400 ? strstr(l, "texture(t5, vec3(vec2(") : NULL;
        if (t && t < l + len && ntap < 16 && e)
            for (int k = 0; k < 2 && f < 0; k++)
                if (end = 0, sscanf(t, k_tap[k], &u, &v, &ly, &bias, &end) == 4 && end) f = k;
        if (f >= 0 && sscanf(e + 1, " r%d = as_u(t[0]);%n", &d, &end2) == 1 && end2 && d >= 0 && d < 256) {
            const char *e2 = strchr(e + 1, '\n');
            size_t len2 = e2 ? (size_t)(e2 - e) : strlen(e + 1);
            int m = snprintf(buf, sizeof buf, "    pcf_tap[%d] = vec3(uintBitsToFloat(r%d), uintBitsToFloat(r%d), float(min(uint(r%d), uint(tsize5(0).z) - 1u)));\n",
                             ntap, u, v, ly);
            EMIT(buf, (size_t)m);
            EMIT(l, len);
            EMIT(e + 1, len2);  // (the read's result: the tap's depth register)
            tap_of[d] = ntap++, l = e + 1 + len2;
            continue;
        }
        f = -1;
        if (captured && len < 200)
            for (int k = 0; k < 2 && f < 0; k++)
                if (end = 0, sscanf(l, k_lit[k], &a, &b, &end) == 2 && end && a >= 0 && a < 256 && b >= 0 && b < 256 && tap_of[b] >= 0) f = k;
        if (f >= 0) {
            int m = snprintf(buf, sizeof buf, "    r%d = floatBitsToUint(pcf_bilinear(pcf_tap[%d], pcf_ref, pcf_tol, pcf_id, pcf_l6, uintBitsToFloat(r40)));\n",
                             a, tap_of[b]);
            EMIT(buf, (size_t)m);
            tap_of[a] = -1, nlit++, l += len;
            continue;
        }
        if (len < 400 && sscanf(l, " r%d = %c", &a, &c) == 2 && a >= 0 && a < 256 && strncmp(strchr(l, '=') + 2, "0u;", 3))
            tap_of[a] = -1;  // (a register written with anything but the "itself" zero no longer holds its tap's depth)
        if (!mained && !strncmp(l, "void main() {", 13)) {
            EMIT(helper, strlen(helper));
            EMIT(l, len);
            static const char decl[] = "  vec3 pcf_tap[16]; float pcf_ref = 0.0f, pcf_tol = 0.0f; uint pcf_id = 0u; int pcf_l6 = 0;\n";
            EMIT(decl, sizeof decl - 1);
            mained = true, l += len;
            continue;
        }
        EMIT(l, len);
        for (int k = 0; k < 2 && !captured; k++)
            if (!strncmp(l, k_r40[k], strlen(k_r40[k]))) {
                static const char cap_[] = "    pcf_ref = uintBitsToFloat(r5); pcf_tol = uintBitsToFloat(r7); pcf_id = r4; pcf_l6 = int(r6);\n";
                EMIT(cap_, sizeof cap_ - 1);
                captured = true;
            }
        l += len;
    }
#undef EMIT
    out[n] = 0;
    if (ntap == 16 && nlit == 16 && captured && mained) return out;
    LOG("[za] shadow mask: %d taps, %d tests, captured %d - not patched\n", ntap, nlit, captured);
    free(out);
    return NULL;
}

static void compile_program(void *arg) {
    Prog *p = arg;
    size_t nv = 0, nf = 0;
    uint64_t t0 = SDL_GetTicksNS();
    uint32_t *sv = vk_glsl_to_spirv(p->vi->msl, STAGE_VERTEX, "vs", &nv);
    uint32_t *sf = sv ? vk_glsl_to_spirv(p->fi->msl, STAGE_FRAGMENT, "fs", &nf) : NULL;
    if (sv && ENV("LARECOMP_RENDERDOC_AT")) {  // which guest shader each SPIR-V is: letsgo-recomp's tools/rdc_skin.py (FNV-1a 64)
        static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
        uint64_t h = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < nv * 4; i++) { h ^= ((const uint8_t *)sv)[i]; h *= 0x100000001B3ull; }
        pthread_mutex_lock(&lock);
        FILE *f = fopen("build/rdc-shaders.txt", "a");
        if (f) { fprintf(f, "%016llx vs %llx\n", (unsigned long long)h, (unsigned long long)p->code[0]); fclose(f); }
        pthread_mutex_unlock(&lock);
    }
    if (!sv || !sf) { free(sv); free(sf); atomic_store(&p->state, 2); return; }
    if (ENV("LARECOMP_WARMUP_LOG")) {
        p->dbg_spv[0] = 0xcbf29ce484222325ull, p->dbg_spv[1] = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < nv * 4; i++) p->dbg_spv[0] = (p->dbg_spv[0] ^ ((const uint8_t *)sv)[i]) * 0x100000001B3ull;
        for (size_t i = 0; i < nf * 4; i++) p->dbg_spv[1] = (p->dbg_spv[1] ^ ((const uint8_t *)sf)[i]) * 0x100000001B3ull;
    }
    p->nbinds = vk_reflect(sv, nv, p->binds, MAX_BIND);
    p->nbinds += vk_reflect(sf, nf, p->binds + p->nbinds, MAX_BIND - p->nbinds);
    p->vs = vk_module(sv, nv);
    p->fs = vk_module(sf, nf);
    free(sv);
    free(sf);
    p->layout = make_layout(p, 2, VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT);
    atomic_fetch_add(&stat_spirv_ns, SDL_GetTicksNS() - t0);
    atomic_fetch_add(&stat_compiles, 1);
    atomic_store(&p->state, p->vs && p->fs ? 1 : 2);
    if (g_vk.gpl && !p->warm && p->vs && p->fs) run_job(gpl_prebuild, p);  // (its shader parts, ahead of the draws)
}

// Transform feedback (NVN programs carry the capture layout in their control section, objects.c): the vertex shader gets
// float outputs decorated for capture, assigned from the outputs it wrote (components in attribute order fill the
// buffers' slots in turn, 4 bytes each) at the end of main.
static void xfb_rewrite(Prog *p) {
    const char *src = p->vi->msl;
    bool wrote[32][4] = {{false}};
    for (const char *q = src; (q = strchr(q, 'v')); q++) {  // "vN[c] = " component writes
        if (q > src && (isalnum((unsigned char)q[-1]) || q[-1] == '_')) continue;
        int n, cpos = 0;
        unsigned comp;
        if (sscanf(q, "v%d[%u] =%n", &n, &comp, &cpos) == 2 && cpos && n >= 0 && n < 32 && comp < 4) wrote[n][comp] = true;
    }
    int order[128][2], no = 0;
    for (int n = 0; n < 32; n++) for (int k = 0; k < 4; k++) if (wrote[n][k] && no < 128) { order[no][0] = n; order[no][1] = k; no++; }
    int need = 0;
    for (int b = 0; b < 4; b++) need += p->vkey.xfb_count[b];
    if (need > no || need > 12) {
        LOG("[xfb] vertex program %llx: %d captured components but %d written outputs: not captured\n", (unsigned long long)p->code[0], need, no);
        return;
    }
    char *decl = malloc(4096), *asg = malloc(4096);
    size_t dl = 0, al = 0;
    int k = 0;
    for (int b = 0; b < 4; b++) {
        if (!p->vkey.xfb_count[b]) continue;
        dl += (size_t)snprintf(decl + dl, 4096 - dl, "layout(xfb_buffer = %d, xfb_stride = %u) out;\n", b, p->vkey.xfb_stride[b]);
        for (int i = 0; i < p->vkey.xfb_count[b]; i++, k++) {
            dl += (size_t)snprintf(decl + dl, 4096 - dl, "layout(location = %d, xfb_buffer = %d, xfb_offset = %d) out float xfb%d;\n",
                                   20 + k, b, i * 4, k);
            al += (size_t)snprintf(asg + al, 4096 - al, "  xfb%d = v%d[%d];\n", k, order[k][0], order[k][1]);
        }
    }
    const char *m = strstr(src, "void main() {"), *e = strrchr(src, '}');
    if (!m || !e || e < m) { free(decl); free(asg); return; }
    size_t len = strlen(src);
    char *o = malloc(len + dl + al + 1), *w = o;
    memcpy(w, src, (size_t)(m - src)); w += m - src;
    memcpy(w, decl, dl); w += dl;
    memcpy(w, m, (size_t)(e - m)); w += e - m;
    memcpy(w, asg, al); w += al;
    memcpy(w, e, len - (size_t)(e - src) + 1);
    p->vi->msl = o;
    free(decl);
    free(asg);
    static int logged;
    if (logged++ < 4) LOG("[xfb] vertex program %llx captures %d components\n", (unsigned long long)p->code[0], need);
}

static void translate_program(void *arg) {
    Prog *p = arg;
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    uint64_t t0 = SDL_GetTicksNS();
    p->vi = maxwell_translate_consts((const uint8_t *)(uintptr_t)p->code[0], p->cmax[0], (const uint8_t *)(uintptr_t)p->cst[0], p->ncst[0],
                                     STAGE_VERTEX, &p->vkey);
    // Experiment (research): LARECOMP_EXP_VS_EARLY=<vertex program address, hex>: that program's vertex shader returns at
    // the start of main (every output at its default), to tell the cost of running the shader from the rest of its draw.
    // (LARECOMP_EXP_VS_EARLY=<address>:<pc, hex>: it returns just before the instruction at that pc instead)
    if (p->vi->msl && ENV("LARECOMP_EXP_VS_EARLY") && p->code[0] == strtoull(ENV("LARECOMP_EXP_VS_EARLY"), NULL, 16)) {
        const char *m = strstr(p->vi->msl, "void main() {"), *r = m ? strstr(m, "  uint r") : NULL;
        const char *colon = strchr(ENV("LARECOMP_EXP_VS_EARLY"), ':');
        if (m && colon) {
            char pat[32];
            snprintf(pat, sizeof pat, "// pc %s:", colon + 1);
            const char *at = strstr(m, pat);
            while (at && at > m && at[-1] != '\n') at--;  // (the start of that line)
            r = at ? at : NULL;
        }
        if (r) {
            size_t len = strlen(p->vi->msl), pr = (size_t)(r - p->vi->msl);
            char *s = malloc(len + 16);
            memcpy(s, p->vi->msl, pr);
            memcpy(s + pr, "  return;\n", 10);
            memcpy(s + pr + 10, p->vi->msl + pr, len - pr + 1);
            p->vi->msl = s;
            LOG("[exp] vertex program %llx returns at the start of main\n", (unsigned long long)p->code[0]);
        }
    }
    // Experiment (research): LARECOMP_GLSL_OVERRIDE_DIR=<dir>: <dir>/<code address, hex>_vs.glsl replaces the generated GLSL
    // of that vertex program (a hand-edited version of what LARECOMP_SAVE_MSL=1 wrote in build/glsl/).
    if (p->vi->msl && ENV("LARECOMP_GLSL_OVERRIDE_DIR")) {
        char path[512];
        snprintf(path, sizeof path, "%s/%llx_vs.glsl", ENV("LARECOMP_GLSL_OVERRIDE_DIR"), (unsigned long long)p->code[0]);
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            char *s = malloc((size_t)n + 1);
            if (s && fread(s, 1, (size_t)n, f) == (size_t)n) { s[n] = 0; p->vi->msl = s; LOG("[exp] GLSL override: %s\n", path); }
            fclose(f);
        }
    }
    // Experiment X4 (letsgo-recomp's docs/PLANO-3D.md), LARECOMP_VK_SKIN_ONEBONE=1: skinned vertex shaders see 100% weight on the
    // first bone (a12 = bone weights) - rigid, but whole, if the first bone's matrix reaches the GPU correctly.
    // Experiment X6, LARECOMP_VK_SKIN_IDX_X=1: every bone slot uses the first bone index (a11.x), weights unchanged.
    if (p->vi->msl && ENV("LARECOMP_VK_SKIN_IDX_X") && strstr(p->vi->msl, "in uvec4 a11;")) {
        const char *d = strstr(p->vi->msl, "in uvec4 a11;"), *m = strstr(p->vi->msl, "void main() {");
        if (d && m && d < m) {
            static const char def[] = "#define a11 uvec4(a11_in.x, a11_in.x, a11_in.x, a11_in.x)\n";
            size_t len = strlen(p->vi->msl), pd = (size_t)(d - p->vi->msl) + strlen("in uvec4 a11"), pm = (size_t)(m - p->vi->msl);
            char *s = malloc(len + 16 + sizeof def), *o = s;
            memcpy(o, p->vi->msl, pd); o += pd;
            memcpy(o, "_in", 3); o += 3;                       // in uvec4 a11_in;
            memcpy(o, p->vi->msl + pd, pm - pd); o += pm - pd;
            memcpy(o, def, sizeof def - 1); o += sizeof def - 1;
            memcpy(o, p->vi->msl + pm, len - pm + 1);
            p->vi->msl = s;
            g_exp_count[3]++;
        }
    }
    // Experiments X8+, LARECOMP_VK_SKIN_W="<GLSL vec4 expression of a12_in>": the bone weights a skinned vertex shader
    // sees, rewritten (a12_in = the weights as the GPU fetched them).
    if (p->vi->msl && ENV("LARECOMP_VK_SKIN_W") && strstr(p->vi->msl, "in vec4 a12;")) {
        const char *d = strstr(p->vi->msl, "in vec4 a12;"), *m = strstr(p->vi->msl, "void main() {");
        if (d && m && d < m) {
            char def[512];
            snprintf(def, sizeof def, "#define a12 (%s)\n", ENV("LARECOMP_VK_SKIN_W"));
            size_t dl = strlen(def), len = strlen(p->vi->msl), pd = (size_t)(d - p->vi->msl) + strlen("in vec4 a12"),
                   pm = (size_t)(m - p->vi->msl);
            char *s = malloc(len + 16 + dl), *o = s;
            memcpy(o, p->vi->msl, pd); o += pd;
            memcpy(o, "_in", 3); o += 3;                       // in vec4 a12_in;
            memcpy(o, p->vi->msl + pd, pm - pd); o += pm - pd;
            memcpy(o, def, dl); o += dl;
            memcpy(o, p->vi->msl + pm, len - pm + 1);
            p->vi->msl = s;
            g_exp_count[3]++;
        }
    }
    // (only skinned shaders - the ones with bone indices too: a12 is something else elsewhere.)
    if (p->vi->msl && ENV("LARECOMP_VK_SKIN_ONEBONE") && strstr(p->vi->msl, "in vec4 a12;") &&
        strstr(p->vi->msl, "in uvec4 a11;")) {
        const char *m = strstr(p->vi->msl, "void main() {");
        if (m) {
            static const char def[] = "#define a12 vec4(1.0, 0.0, 0.0, 0.0)\n";
            size_t pre = (size_t)(m - p->vi->msl), len = strlen(p->vi->msl);
            char *s = malloc(len + sizeof def);
            memcpy(s, p->vi->msl, pre);
            memcpy(s + pre, def, sizeof def - 1);
            memcpy(s + pre + sizeof def - 1, m, len - pre + 1);
            p->vi->msl = s;  // (the translator's string is kept: it lives as long as the program)
            g_exp_count[3]++;
        }
    }
    if (p->vi->msl && !p->code[1]) {  // vertex-only program (see get_program): an empty fragment shader
        p->fi = calloc(1, sizeof *p->fi);
        p->fi->stage = STAGE_FRAGMENT;
        p->fi->msl = strdup("#version 460\nvoid main() {}\n");
    } else if (p->vi->msl) {
        p->fkey.vs_out_mask = p->vi->out_mask;
        // (the shadow mask that za_soft_shadows patches below: in the spelling its patterns match)
        extern uint64_t vk_prog_fs_hash(uint64_t fs, size_t len);
        maxwell_plain_glsl = g_settings.shadow_smooth && vk_prog_fs_hash(p->code[1], p->fs_len) == 0xec1c842f7a98584dull;
        p->fi = maxwell_translate_consts((const uint8_t *)(uintptr_t)p->code[1], p->cmax[1], (const uint8_t *)(uintptr_t)p->cst[1],
                                         p->ncst[1], STAGE_FRAGMENT, &p->fkey);
        maxwell_plain_glsl = false;
    }
    atomic_fetch_add(&stat_translate_ns, SDL_GetTicksNS() - t0);
    if (!p->vi->msl) {
        LOG("[shader] VS translate failed: %s (code %llx)\n", p->vi->error, (unsigned long long)p->code[0]);
        atomic_store(&p->state, 2);
        return;
    }
    if (!p->fi->msl) {
        LOG("[shader] FS translate failed: %s\n", p->fi->error);
        atomic_store(&p->state, 2);
        return;
    }
    uint64_t vh = 0, fh = 0, t_xlat = SDL_GetTicksNS() - t0;
    static int wlog = -1;  // LARECOMP_WARMUP_LOG=1: programs translated / compiled at their first draw (not in the caches yet)
    if (wlog < 0) wlog = ENV("LARECOMP_WARMUP_LOG") != NULL;
    if (!p->warm) {  // (the shader manifest: what the next warm-up builds first)
        extern void warmup_note_program(uint64_t vh, uint64_t fh, const ShaderKey *vk, const ShaderKey *fk);
        vh = maxwell_program_hash((const uint8_t *)(uintptr_t)p->code[0], p->cmax[0], (const uint8_t *)(uintptr_t)p->cst[0],
                                           p->ncst[0], STAGE_VERTEX);
        fh = p->code[1] ? maxwell_program_hash((const uint8_t *)(uintptr_t)p->code[1], p->cmax[1],
                                                        (const uint8_t *)(uintptr_t)p->cst[1], p->ncst[1], STAGE_FRAGMENT) : 0;
        warmup_note_program(vh, fh, &p->vkey, &p->fkey);
    }
    if (wlog) {
        p->dbg_id[0] = vh ? vh : maxwell_program_hash((const uint8_t *)(uintptr_t)p->code[0], p->cmax[0], (const uint8_t *)(uintptr_t)p->cst[0], p->ncst[0], STAGE_VERTEX);
        p->dbg_id[1] = fh ? fh : p->code[1] ? maxwell_program_hash((const uint8_t *)(uintptr_t)p->code[1], p->cmax[1], (const uint8_t *)(uintptr_t)p->cst[1], p->ncst[1], STAGE_FRAGMENT) : 0;
    }
    if (ENV("LARECOMP_GLSL_OVERRIDE_DIR")) {  // <code address>_fs.glsl replaces a fragment program's GLSL too
        char path[512];
        FILE *f;
        {
            snprintf(path, sizeof path, "%s/%llx_fs.glsl", ENV("LARECOMP_GLSL_OVERRIDE_DIR"), (unsigned long long)p->code[1]);
            f = fopen(path, "rb");
            if (!f && p->code[1]) {  // or h<fs hash>_fs.glsl: the hash the draw logs print, stable between runs
                extern uint64_t vk_prog_fs_hash(uint64_t fs, size_t len);
                snprintf(path, sizeof path, "%s/h%016llx_fs.glsl", ENV("LARECOMP_GLSL_OVERRIDE_DIR"), (unsigned long long)vk_prog_fs_hash(p->code[1], p->fs_len));
                f = fopen(path, "rb");
            }
            if (f) {
                fseek(f, 0, SEEK_END);
                long n = ftell(f);
                fseek(f, 0, SEEK_SET);
                char *s = malloc((size_t)n + 1);
                if (s && fread(s, 1, (size_t)n, f) == (size_t)n) { s[n] = 0; p->fi->msl = s; LOG("[exp] GLSL override: %s\n", path); }
                fclose(f);
            }
        }
    }
    // Legends Z-A's half-resolution depth (fs 6023c3a85ef7d692: one nearest read of the full-resolution depth per
    // half-res pixel). Each pixel's coordinate lands exactly on the corner of four full-res texels, and which one a
    // nearest read takes then rests on the last bit of the interpolated coordinate: on this GPU it changes from column
    // to column (78% the top-left texel, 19% the top-right), the console (and Eden) take the top-left one every time.
    // The half-res depth came out jagged (a 20x rougher surface) for what reads it, the shadow mask's contact shadows
    // among them. The read is moved a quarter of a full-res texel up-left, inside the top-left texel - the
    // half-res depth then matches Eden's texel for texel. LARECOMP_ZA_DEPTH_DS_FIX=0: as translated.
    if (p->code[1] && !(ENV("LARECOMP_ZA_DEPTH_DS_FIX") && atoi(ENV("LARECOMP_ZA_DEPTH_DS_FIX")) == 0)) {
        extern uint64_t vk_prog_fs_hash(uint64_t fs, size_t len);
        static const char old[] = "texture(t0, vec2(", fix[] = "texture(t0, vec2(-0.25) / vec2(textureSize(t0, 0)) + vec2(";
        const char *at;
        if (vk_prog_fs_hash(p->code[1], p->fs_len) == 0x6023c3a85ef7d692ull && (at = strstr(p->fi->msl, old)) && !strstr(at + 1, old)) {
            size_t len = strlen(p->fi->msl), pre = (size_t)(at - p->fi->msl);
            char *s = malloc(len + sizeof fix);
            memcpy(s, p->fi->msl, pre);
            memcpy(s + pre, fix, sizeof fix - 1);
            memcpy(s + pre + sizeof fix - 1, at + sizeof old - 1, len - pre - (sizeof old - 1) + 1);
            p->fi->msl = s;
            static int logged;
            if (!logged++) LOG("[za] half-resolution depth: reads moved into the top-left texel (fs %llx)\n", (unsigned long long)p->code[1]);
        }
    }
    // Legends Z-A's shadow mask (fs ec1c842f7a98584d), Settings.shadow_smooth: its 16 shadow-map taps are bilinear
    // (za_soft_shadows) - the edges of the shadows no longer step along the shadow map's texels.
    if (p->code[1] && g_settings.shadow_smooth) {
        extern uint64_t vk_prog_fs_hash(uint64_t fs, size_t len);
        if (vk_prog_fs_hash(p->code[1], p->fs_len) == 0xec1c842f7a98584dull) {
            char *s = za_soft_shadows(p->fi->msl);
            static int logged;
            if (s) p->fi->msl = s;
            if (!logged++) LOG("[za] shadow mask: %s\n", s ? "bilinear taps (smooth edges)" : "not the expected code, left as it is");
        }
    }
    if (p->vkey.xfb_count[0]) xfb_rewrite(p);
    p->vs_uses_vid = strstr(p->vi->msl, "gl_VertexIndex") != NULL;
    save_glsl(p->vi->msl, p->code[0], "vs");
    save_glsl(p->fi->msl, p->code[1], "fs");
    if (ENV("LARECOMP_SAVE_MSL") && p->code[1]) {  // (and by the stable hash: build/glsl/h<hash>_fs.glsl, what overrides match)
        extern uint64_t vk_prog_fs_hash(uint64_t fs, size_t len);
        char path[96];
        snprintf(path, sizeof path, "build/glsl/h%016llx_fs.glsl", (unsigned long long)vk_prog_fs_hash(p->code[1], p->fs_len));
        FILE *f = fopen(path, "w");
        if (f) { fputs(p->fi->msl, f); fclose(f); }
    }
    compile_program(p);  // (this worker: queued again it would wait behind the other translations)
    if (wlog && !p->warm && SDL_GetTicksNS() - t0 > 4000000)
        LOG("[warmup] built at a draw: %016llx %016llx (translated %.1f ms, all %.1f ms)\n", (unsigned long long)vh, (unsigned long long)fh,
            (double)t_xlat / 1e6, (double)(SDL_GetTicksNS() - t0) / 1e6);
}

#define PROG_TAB 16384
static struct { uint64_t key; Prog *prog; } prog_tab[PROG_TAB];

// BindProgram(program, stages): the stages in the mask come from `program` (NULL: unbound), the others stay. A bind
// that leaves one program's stages and nothing else uses that program itself (as before); a mix of stages from
// several programs (Let's Go: vertex and fragment bound separately) uses a combined program. The shader caches are
// keyed by code address, so the combination needs no identity of its own.
static NvnProgram g_comb_prog;
static void bind_program_stages(NvnProgram *p, uint32_t mask) {
    if (!mask) mask = p ? p->stage_mask : 0x3F;  // (no mask recorded: the whole program)
    NvnProgram *cur = st.program;
    NvnProgram next = cur ? *cur : (NvnProgram){0};
    next.backend = NULL;
    for (int i = 0; i < 6; i++) {
        uint32_t bit = 1u << i;
        if (!(mask & bit)) continue;
        if (p && (p->stage_mask & bit)) next.stages[i] = p->stages[i], next.stage_mask |= bit;
        else memset(&next.stages[i], 0, sizeof next.stages[i]), next.stage_mask &= ~bit;
        if (i == 0) {  // (the transform feedback layout belongs to the vertex stage)
            bool vs = p && (p->stage_mask & 1);
            for (int b = 0; b < 4; b++) { next.xfb_count[b] = vs ? p->xfb_count[b] : 0; next.xfb_stride[b] = vs ? p->xfb_stride[b] : 0; }
        }
    }
    if (!next.stage_mask) { st.program = NULL; return; }
    if (p && next.stage_mask == p->stage_mask &&
        !memcmp(next.stages, p->stages, sizeof next.stages)) { st.program = p; return; }
    g_comb_prog = next;
    st.program = &g_comb_prog;
}

static uint64_t hash_words(uint64_t h, const void *p, size_t n);
static Prog *get_program(NvnProgram *np, ShaderKey *vkey, ShaderKey *fkey) {
    // A vertex shader without a fragment shader (Let's Go's depth / shadow passes: stages 0x1) gets an empty fragment
    // shader in translate_program - no color output, only depth, as the hardware does. They were all skipped.
    if (!(np->stage_mask & 1)) {
        static uint32_t seen;
        if (np->stage_mask < 32 && !(seen & (1u << np->stage_mask))) {
            seen |= 1u << np->stage_mask;
            LOG("[shader] draw with stages %#x (needs vertex + fragment): skipped\n", np->stage_mask);
        }
        return NULL;
    }
    // (word at a time: this runs for every draw)
    // Keyed by what is at the addresses (objects.c content_hash: the code and its constants), not where: Z-A loads the
    // same effect shaders again at other addresses (one fragment program at 176 in a Rogue Mega battle), and each load
    // was translated, compiled and given pipelines again - never freed. The address stays in the key where the hash
    // does not cover all the code (no constants: only its first 4 KB hashed) and for the address-named research overrides.
    static int by_addr = -1;
    if (by_addr < 0) by_addr = ENV("LARECOMP_GLSL_OVERRIDE_DIR") || ENV("LARECOMP_EXP_VS_EARLY") || ENV("LARECOMP_PROG_BY_ADDR");
    uint64_t in[6] = {by_addr || !np->stages[0].const_offset ? np->stages[0].code_addr : 0,
                      by_addr || !np->stages[1].const_offset ? np->stages[1].code_addr : 0,
                      np->stages[0].content_hash, np->stages[1].content_hash, 0, fkey->tex_stencil};
    memcpy(&in[4], fkey->rt_kind, 8);
    uint64_t h = hash_words(0xcbf29ce484222325ull, in, sizeof in);
    h = hash_words(h, fkey->img_kind, 8);
    h = hash_words(h, vkey, sizeof *vkey);
    if (!h) h = 1;
    uint32_t slot = (uint32_t)(h ^ (h >> 31)) & (PROG_TAB - 1);
    for (int probe = 0; prog_tab[slot].key && prog_tab[slot].key != h; probe++) {
        if (probe == 64) { prog_tab[slot].key = 0; break; }  // (full run of slots: the entry is replaced, its Prog kept)
        slot = (slot + 1) & (PROG_TAB - 1);
    }
    if (prog_tab[slot].key) return prog_tab[slot].prog;
    {   // an address seen before with other code: the old address-only key would have handed back that shader
        static struct { uint64_t addr, content; } seen[8192];
        static uint64_t nother;
        for (int i = 0; i < 2; i++) {
            uint64_t a = np->stages[i].code_addr, c = np->stages[i].content_hash;
            if (!a) continue;
            uint32_t s = (uint32_t)((a >> 8) * 0x9E3779B1u) & 8191;
            for (int k = 0; k < 16; k++, s = (s + 1) & 8191) {
                if (!seen[s].addr) { seen[s].addr = a; seen[s].content = c; break; }
                if (seen[s].addr != a) continue;
                if (seen[s].content != c && (nother++, !(nother & (nother - 1))))
                    LOG("[shader] %s shader address %llx now holds another shader %llu (f%llu)\n", i ? "fragment" : "vertex",
                        (unsigned long long)a, (unsigned long long)nother, (unsigned long long)renderer_game_frame());
                seen[s].content = c;
                break;
            }
        }
    }
    if (ENV("LARECOMP_LOG_PROGS"))  // (research: the shaders each new program uses, by content hash)
    {
        char ak[33], rk[9];
        for (int i = 0; i < 32; i++) ak[i] = (char)('0' + vkey->attr_kind[i]);
        for (int i = 0; i < 8; i++) rk[i] = (char)('0' + fkey->rt_kind[i]);
        ak[32] = rk[8] = 0;
        LOG("[prog] vs %016llx@%llx fs %016llx@%llx attr %s rt %s xfb %u img %llx/%llx stencil %x/%x\n", (unsigned long long)np->stages[0].content_hash,
            (unsigned long long)np->stages[0].code_addr, (unsigned long long)np->stages[1].content_hash,
            (unsigned long long)np->stages[1].code_addr, ak, rk, vkey->xfb_count[0], (unsigned long long)*(uint64_t *)vkey->img_kind,
            (unsigned long long)*(uint64_t *)fkey->img_kind, vkey->tex_stencil, fkey->tex_stencil);
    }
    Prog *p = calloc(1, sizeof *p);
    prog_tab[slot].key = h;
    prog_tab[slot].prog = p;
    atomic_fetch_add(&g_cache_progs, 1);
    p->content[0] = np->stages[0].content_hash, p->content[1] = np->stages[1].content_hash;
    for (int i = 0; i < 2; i++) {
        p->code[i] = np->stages[i].code_addr;
        p->cmax[i] = np->stages[i].const_offset ? np->stages[i].const_offset : 0x10000;
        if (np->stages[i].const_offset && np->stages[i].const_size <= 0x10000)
            p->cst[i] = np->stages[i].code_addr + np->stages[i].const_offset, p->ncst[i] = np->stages[i].const_size;
    }
    p->vkey = *vkey;
    p->fkey = *fkey;
    { extern uint64_t prof_new_prog; prof_new_prog++; }
    run_job(translate_program, p);
    return p;
}

// ================================================================== vertex formats / fixed function tables
static int attr_kind(uint32_t fmt) {
    switch (fmt) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23:
    case 0x27: case 0x2C: case 0x2F: case 0x3E:
        return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24:
    case 0x28: case 0x2D: case 0x30:
        return 1;
    default: return 0;
    }
}

static VkFormat vertex_format(uint32_t f) {
    switch (f) {
    case 0x01: return VK_FORMAT_R8_UNORM;
    case 0x02: return VK_FORMAT_R8_SNORM;
    case 0x03: return VK_FORMAT_R8_UINT;
    case 0x04: return VK_FORMAT_R8_SINT;
    case 0x05: return VK_FORMAT_R16_SFLOAT;
    case 0x06: return VK_FORMAT_R16_UNORM;
    case 0x07: return VK_FORMAT_R16_SNORM;
    case 0x08: return VK_FORMAT_R16_UINT;
    case 0x09: return VK_FORMAT_R16_SINT;
    case 0x0A: return VK_FORMAT_R32_SFLOAT;
    case 0x0B: return VK_FORMAT_R32_UINT;
    case 0x0C: return VK_FORMAT_R32_SINT;
    case 0x0D: return VK_FORMAT_R8G8_UNORM;
    case 0x0E: return VK_FORMAT_R8G8_SNORM;
    case 0x0F: return VK_FORMAT_R8G8_UINT;
    case 0x10: return VK_FORMAT_R8G8_SINT;
    case 0x11: return VK_FORMAT_R16G16_SFLOAT;
    case 0x12: return VK_FORMAT_R16G16_UNORM;
    case 0x13: return VK_FORMAT_R16G16_SNORM;
    case 0x14: return VK_FORMAT_R16G16_UINT;
    case 0x15: return VK_FORMAT_R16G16_SINT;
    case 0x16: return VK_FORMAT_R32G32_SFLOAT;
    case 0x17: return VK_FORMAT_R32G32_UINT;
    case 0x18: return VK_FORMAT_R32G32_SINT;
    case 0x19: return VK_FORMAT_R8G8B8_UNORM;
    case 0x1A: return VK_FORMAT_R8G8B8_SNORM;
    case 0x1B: return VK_FORMAT_R8G8B8_UINT;
    case 0x1C: return VK_FORMAT_R8G8B8_SINT;
    case 0x1D: return VK_FORMAT_R16G16B16_SFLOAT;
    case 0x1E: return VK_FORMAT_R16G16B16_UNORM;
    case 0x1F: return VK_FORMAT_R16G16B16_SNORM;
    case 0x20: return VK_FORMAT_R16G16B16_UINT;
    case 0x21: return VK_FORMAT_R16G16B16_SINT;
    case 0x22: return VK_FORMAT_R32G32B32_SFLOAT;
    case 0x23: return VK_FORMAT_R32G32B32_UINT;
    case 0x24: return VK_FORMAT_R32G32B32_SINT;
    case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x26: return VK_FORMAT_R8G8B8A8_SNORM;
    case 0x27: return VK_FORMAT_R8G8B8A8_UINT;
    case 0x28: return VK_FORMAT_R8G8B8A8_SINT;
    case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0x2A: return VK_FORMAT_R16G16B16A16_UNORM;
    case 0x2B: return VK_FORMAT_R16G16B16A16_SNORM;
    case 0x2C: return VK_FORMAT_R16G16B16A16_UINT;
    case 0x2D: return VK_FORMAT_R16G16B16A16_SINT;
    case 0x2E: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x2F: return VK_FORMAT_R32G32B32A32_UINT;
    case 0x30: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 0x51: return VK_FORMAT_R8_UINT;
    default: {
        static uint8_t told[256];
        if (f < 256 && !told[f]++) LOG("[vk] vertex format %#x unknown: read as RGBA32F\n", f);
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    }
}

// 3-component 8/16-bit vertex formats aren't universally supported: read 4 components instead.
static VkFormat vertex_format_supported(VkFormat f) {
    static uint8_t known[256];  // 1 ok, 2 not
    uint32_t i = f < 256 ? f : 0;
    if (!known[i]) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
        known[i] = (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? 1 : 2;
    }
    if (known[i] == 1) return f;
    { static uint8_t told[256]; if (!told[i]++) LOG("[vk] vertex format %d unsupported: read as 4 components\n", f); }
    switch (f) {
    case VK_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R16G16B16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VK_FORMAT_R16G16B16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
    default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

static VkBlendFactor blend_factor(uint8_t f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: {
        static uint8_t warned[256];
        if (!warned[f]++) LOG("[vk] unknown blend factor %#x\n", f);
        return VK_BLEND_FACTOR_ONE;
    }
    }
}

static VkBlendOp blend_op(uint8_t e) {
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static VkCompareOp compare_fn(uint8_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

static VkStencilOp stencil_op(uint8_t o) {
    switch (o) {
    case 2: return VK_STENCIL_OP_ZERO;
    case 3: return VK_STENCIL_OP_REPLACE;
    case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6: return VK_STENCIL_OP_INVERT;
    case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

static int rt_kind(uint32_t f) {
    switch (f) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x27: case 0x2C: case 0x2F: case 0x3E: return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x28: case 0x2D: case 0x30: return 1;
    default: return 0;
    }
}

// The format class (rt_kind) of each image bound to a stage: the shader's image types follow it (ShaderKey.img_kind)
static void image_kinds(int stage, uint8_t kind[8]) {
    for (int i = 0; i < 8; i++) {
        uint64_t h = st.img[stage][i];
        uint32_t id = h & 0xFFFFF;
        NvnTexture *t = h && st.tpool && id < (uint32_t)st.tpool->count ? ((NvnTexEntry *)st.tpool->entries)[id].tex : NULL;
        kind[i] = t ? (uint8_t)rt_kind(t->format) : 0;
    }
}

// The texture slots of a stage bound to a depth-stencil texture in stencil mode, or to an unsigned integer color
// format (ShaderKey.tex_stencil: usampler). A float sampler on a UINT view is undefined in Vulkan: Z-A's shadow
// filter reads its R8_UINT object-id map (which taps are the object's own, not self-shadowing) through one.
// LARECOMP_VK_UINT_TEX_FLOAT=1: the old float sampler for those.
static uint32_t stencil_slots(int stage) {
    static int uint_float = -1;
    if (uint_float < 0) uint_float = ENV("LARECOMP_VK_UINT_TEX_FLOAT") != NULL;
    uint32_t m = 0;
    for (int i = 0; i < 32; i++) {
        uint64_t h = st.tex[stage][i];
        uint32_t tic = h & 0xFFFFF;
        if (!h || !st.tpool || tic >= (uint32_t)st.tpool->count) continue;
        NvnTexEntry *e = &((NvnTexEntry *)st.tpool->entries)[tic];
        if (!e->tex) continue;
        uint32_t f = e->has_view && (e->view.setmask & 4) ? e->view.format : e->tex->format;
        if (!uint_float && !renderer_format_is_depth(e->tex->format) && rt_kind(f) == 2) { m |= 1u << i; continue; }
        if (!renderer_format_is_depth(e->tex->format)) continue;
        uint32_t dsm = e->has_view && (e->view.setmask & 16) ? e->view.ds_mode : e->tex->ds_mode;
        if (dsm == 1) m |= 1u << i;
    }
    return m;
}

// ================================================================== graphics pipelines
// Everything a pipeline bakes in, captured by value so it can be built on a worker thread.
#define ZERO_BINDING 16
typedef struct {
    Prog *prog;
    VkFormat color[8], depth, stencil;
    uint32_t ncolor, samples;
    uint32_t attr_fmt[32], attr_off[32], attr_bind[32];
    uint32_t attr_mask;
    uint32_t stride[17], divisor[17], used_streams;
    uint8_t blend[8][8];
    uint8_t blend_on, a2c, topo_class, poly_fill;
    uint32_t write_mask[8];
    uint8_t depth_clamp, pad_[3];
} PipeKey;

typedef struct {
    PipeKey k;
    uint64_t hash;
    VkPipeline pipe;
    _Atomic int state;  // 0 building, 1 ready, 2 failed
    uint64_t t_asked;
    VkPipeline parts[4];             // (g_vk.gpl: the libraries it was linked from)
    _Atomic(VkPipeline) optimized;   // (gpl_optimize: the same, link-time optimized - swapped in by the next draw)
} Pipe;
// draw_'s inputs to a PipeKey, and the pipeline of the last draw that had these (vk_exec_reset forgets it)
typedef struct {
    Prog *pr;
    VkFormat pcolor[8], pdepth, pstencil;
    uint32_t pncolor, psamples, nattrib, nstream, attrib[16], stream[16][2];
    NvnTexture *rtc[8];
    uint32_t rtf[8], chmask;
    uint8_t cstate, ms0c, cls, poly2, dclamp, pad_[3];
    uint8_t blend[8][8];
} PipeIn;
static Pipe *g_pipe_memo;
static PipeIn g_pipe_memo_in;
#define PIPE_TAB 65536  // (8192 filled up in a long Z-A session: the lookup below then never ended)
static struct { uint64_t key; Pipe *pipe; } pipe_tab[PIPE_TAB];

// 8 bytes a step (PipeKey is ~700 bytes, hashed per draw); in-memory tables only - hmix values are compared with
// stored constants (fragment-shader hashes) and must not change.
static uint64_t hash_words(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; memcpy(&w, b, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; n; n--, b++) { h = (h ^ *b) * 0x100000001B3ull; }
    return h ^ (h >> 32);
}

static uint64_t hmix(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}
uint64_t vk_fs_hash(uint64_t fs) { return hmix(1469598103934665603ull, (const void *)(uintptr_t)(fs + 0x30), 0x200); }
// The same for a program's fragment shader, 0 when its code is shorter than what the hash reads: the warm-up's programs
// (warmup.c) are copies of each stage, len bytes from its header (0: the game's memory): reading past one crashed at boot.
uint64_t vk_prog_fs_hash(uint64_t fs, size_t len) { return fs && (!len || len >= 0x230) ? vk_fs_hash(fs) : 0; }

static VkPrimitiveTopology topo_class_base(uint8_t c) {
    return c == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : c == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

// LARECOMP_PIPE_STATS=<vertex program address, hex>|all: after each pipeline is created, what the driver reports about its
// compiled shaders (VK_KHR_pipeline_executable_properties: one executable per stage; registers, spills, instructions...).
static void pipe_exec_report(VkPipeline pipe, uint64_t vs, uint64_t fs) {
    const char *want = ENV("LARECOMP_PIPE_STATS");
    if (!want || (strcmp(want, "all") && strtoull(want, NULL, 16) != vs)) return;
    static PFN_vkGetPipelineExecutablePropertiesKHR get_props;
    static PFN_vkGetPipelineExecutableStatisticsKHR get_stats;
    if (!get_props) {
        get_props = (PFN_vkGetPipelineExecutablePropertiesKHR)vkGetDeviceProcAddr(g_vk.dev, "vkGetPipelineExecutablePropertiesKHR");
        get_stats = (PFN_vkGetPipelineExecutableStatisticsKHR)vkGetDeviceProcAddr(g_vk.dev, "vkGetPipelineExecutableStatisticsKHR");
        if (!get_props || !get_stats) { LOG("[pipestats] the driver does not export the functions\n"); return; }
    }
    uint32_t n = 0;
    VkPipelineInfoKHR pinfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, .pipeline = pipe};
    if (get_props(g_vk.dev, &pinfo, &n, NULL) != VK_SUCCESS || !n) return;
    VkPipelineExecutablePropertiesKHR props[8] = {0};
    if (n > 8) n = 8;
    for (uint32_t i = 0; i < n; i++) props[i].sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR;
    if (get_props(g_vk.dev, &pinfo, &n, props) != VK_SUCCESS) return;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ns = 0;
        VkPipelineExecutableInfoKHR einfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR, .pipeline = pipe, .executableIndex = i};
        if (get_stats(g_vk.dev, &einfo, &ns, NULL) != VK_SUCCESS) continue;
        VkPipelineExecutableStatisticKHR stats[64] = {0};
        if (ns > 64) ns = 64;
        for (uint32_t s = 0; s < ns; s++) stats[s].sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR;
        if (get_stats(g_vk.dev, &einfo, &ns, stats) != VK_SUCCESS) continue;
        char line[1024];
        int l = snprintf(line, sizeof line, "[pipestats] vs %llx fs %llx  %s (stages %#x):", (unsigned long long)vs, (unsigned long long)fs,
                         props[i].name, props[i].stages);
        for (uint32_t s = 0; s < ns && l < (int)sizeof line - 80; s++) {
            const VkPipelineExecutableStatisticValueKHR *v = &stats[s].value;
            switch (stats[s].format) {
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: l += snprintf(line + l, sizeof line - l, " %s=%u;", stats[s].name, v->b32); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: l += snprintf(line + l, sizeof line - l, " %s=%lld;", stats[s].name, (long long)v->i64); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: l += snprintf(line + l, sizeof line - l, " %s=%llu;", stats[s].name, (unsigned long long)v->u64); break;
            default: l += snprintf(line + l, sizeof line - l, " %s=%g;", stats[s].name, v->f64); break;
            }
        }
        LOG("%s\n", line);
    }
}

static __thread bool tl_warm;  // (vk_warm_program: its pipelines stay out of the pipeline cache file)

// A PipeKey's create info, every part filled in (the structs point at each other: filled in place, never copied)
typedef struct {
    VkPipelineShaderStageCreateInfo stages[2];
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb[17];
    VkVertexInputBindingDivisorDescriptionEXT vd[17];
    VkPipelineVertexInputDivisorStateCreateInfoEXT dvi;
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vs;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendAttachmentState cba[8];
    VkPipelineColorBlendStateCreateInfo cb;
    VkPipelineDynamicStateCreateInfo dy;
    VkPipelineRenderingCreateInfo ri;
    VkGraphicsPipelineCreateInfo pi;
} PipeInfo;

static void pipe_info(PipeInfo *s, const PipeKey *k) {
    memset(s, 0, sizeof *s);
    s->stages[0] = (VkPipelineShaderStageCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                     .pNext = vk_sg32(VK_SHADER_STAGE_VERTEX_BIT), .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                                     .module = k->prog->vs, .pName = "main"};
    s->stages[1] = (VkPipelineShaderStageCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                     .pNext = vk_sg32(VK_SHADER_STAGE_FRAGMENT_BIT), .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                                     .module = k->prog->fs, .pName = "main"};
    uint32_t na = 0, nb = 0, nd = 0;
    for (int i = 0; i < 32; i++)
        if (k->attr_mask & (1u << i)) s->va[na++] = (VkVertexInputAttributeDescription){(uint32_t)i, k->attr_bind[i], k->attr_fmt[i], k->attr_off[i]};
    for (uint32_t b = 0; b < 17; b++) {
        if (!(k->used_streams & (1u << b))) continue;
        bool inst = k->divisor[b] != 0;
        s->vb[nb++] = (VkVertexInputBindingDescription){b, k->stride[b], inst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (inst && k->divisor[b] > 1 && g_vk.divisor) s->vd[nd++] = (VkVertexInputBindingDivisorDescriptionEXT){b, k->divisor[b]};
    }
    s->dvi = (VkPipelineVertexInputDivisorStateCreateInfoEXT){.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
                                                              .vertexBindingDivisorCount = nd, .pVertexBindingDivisors = s->vd};
    s->vi = (VkPipelineVertexInputStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nd ? &s->dvi : NULL,
                                                   .vertexBindingDescriptionCount = nb, .pVertexBindingDescriptions = s->vb,
                                                   .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = s->va};
    {   // (LARECOMP_PIPE_STATS: the vertex fetch layout of the program asked for)
        const char *w = ENV("LARECOMP_PIPE_STATS");
        if (w && (!strcmp(w, "all") || strtoull(w, NULL, 16) == k->prog->code[0])) {
            LOG("[pipestats] vs %llx inputs:", (unsigned long long)k->prog->code[0]);
            for (uint32_t i = 0; i < na; i++) LOG(" a%u(bind %u fmt %d off %u)", s->va[i].location, s->va[i].binding, (int)s->va[i].format, s->va[i].offset);
            LOG(" | bindings:");
            for (uint32_t i = 0; i < nb; i++)
                LOG(" %u(stride %u %s)", s->vb[i].binding, s->vb[i].stride, s->vb[i].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE ? "instance" : "vertex");
            for (uint32_t i = 0; i < nd; i++) LOG(" div%u=%u", s->vd[i].binding, s->vd[i].divisor);
            LOG("\n");
        }
    }
    s->ia = (VkPipelineInputAssemblyStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                     .topology = topo_class_base(k->topo_class)};
    s->vs = (VkPipelineViewportStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    s->rs = (VkPipelineRasterizationStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                     .polygonMode = k->poly_fill ? VK_POLYGON_MODE_FILL : VK_POLYGON_MODE_LINE, .lineWidth = 1,
                                                     .depthClampEnable = k->depth_clamp};
    s->ms = (VkPipelineMultisampleStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                   .rasterizationSamples = (VkSampleCountFlagBits)k->samples, .alphaToCoverageEnable = k->a2c};
    s->ds = (VkPipelineDepthStencilStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    for (uint32_t i = 0; i < k->ncolor; i++) {
        s->cba[i].colorWriteMask = k->write_mask[i];
        if ((k->blend_on >> i) & 1) {
            const uint8_t *b = k->blend[i];
            s->cba[i].blendEnable = VK_TRUE;
            s->cba[i].srcColorBlendFactor = blend_factor(b[1]);
            s->cba[i].dstColorBlendFactor = blend_factor(b[2]);
            s->cba[i].srcAlphaBlendFactor = blend_factor(b[3]);
            s->cba[i].dstAlphaBlendFactor = blend_factor(b[4]);
            s->cba[i].colorBlendOp = blend_op(b[5]);
            s->cba[i].alphaBlendOp = blend_op(b[6]);
        }
    }
    s->cb = (VkPipelineColorBlendStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                  .attachmentCount = k->ncolor, .pAttachments = s->cba};
    s->dy = (VkPipelineDynamicStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                               .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    s->ri = (VkPipelineRenderingCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->ncolor,
                                            .pColorAttachmentFormats = k->color, .depthAttachmentFormat = k->depth,
                                            .stencilAttachmentFormat = k->stencil};
    s->pi = (VkGraphicsPipelineCreateInfo){.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &s->ri, .stageCount = 2,
                                           .pStages = s->stages, .pVertexInputState = &s->vi, .pInputAssemblyState = &s->ia,
                                           .pViewportState = &s->vs, .pRasterizationState = &s->rs, .pMultisampleState = &s->ms,
                                           .pDepthStencilState = &s->ds, .pColorBlendState = &s->cb, .pDynamicState = &s->dy,
                                           .layout = k->prog->layout};
}

// ---- graphics pipeline libraries (VK_EXT_graphics_pipeline_library; g_vk.gpl)
// A pipeline is made of four parts built on their own and linked: the vertex input (attribute formats, strides,
// topology), the vertex shader (+ rasterizer state), the fragment shader (+ depth / stencil, all dynamic) and the
// fragment output (target formats, blending, write masks, multisampling). Shader parts depend on the program only, so
// a program seen with a new blend mode, target format or vertex layout costs a fast link (~0.1-1 ms) instead of
// compiling its shaders again (~50-100 ms cold on NVIDIA: the hitches when an effect or area shows up the first time).
// The shader parts are started as soon as a program is compiled (gpl_prebuild), before any draw asks for a pipeline.
typedef struct { uint64_t key; VkPipeline pipe; _Atomic int state; } Lib;  // state: 0 building, 1 ready, 2 failed
#define LIB_TAB 65536
static struct { uint64_t key; Lib *lib; } lib_tab[LIB_TAB];
static int lib_count;
static pthread_mutex_t lib_lock = PTHREAD_MUTEX_INITIALIZER;

// Fast-linked pipelines run as fast as whole ones on NVIDIA (each library is compiled fully); on AMD (RADV, the Steam
// Deck) and Intel the fast link skips the cross-stage optimizations and their shaders run slower. There each pipeline is
// also linked again with link-time optimization on a worker (gpl_optimize) and replaces the fast one when done, as DXVK
// does. LARECOMP_VK_GPL_LTO=0/1 forces it off / on.
static bool gpl_lto(void) {
    static int on = -1;
    if (on < 0) on = ENV("LARECOMP_VK_GPL_LTO") ? atoi(ENV("LARECOMP_VK_GPL_LTO")) != 0 : g_vk.props.vendorID != 0x10DE;
    return on;
}

static __thread bool tl_in_warm;  // (vk_warm_program_keyed / vk_warm_state on this thread: the warm-up's own parts)
static void lib_create(Lib *l, VkGraphicsPipelineCreateInfo *pi, VkGraphicsPipelineLibraryFlagsEXT part) {
    VkGraphicsPipelineLibraryCreateInfoEXT li = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
                                                 .pNext = (void *)pi->pNext, .flags = part};
    pi->pNext = &li;
    pi->flags |= VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
    if (gpl_lto()) pi->flags |= VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT;
    uint64_t t0 = SDL_GetTicksNS();
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, pi, NULL, &l->pipe);
    atomic_fetch_add(&stat_lib_ns, SDL_GetTicksNS() - t0);
    static int wlog = -1;  // (LARECOMP_WARMUP_LOG=1: a part made in play over 5 ms)
    if (wlog < 0) wlog = ENV("LARECOMP_WARMUP_LOG") != NULL;
    if (wlog && !tl_in_warm && SDL_GetTicksNS() - t0 > 5000000)
        LOG("[warmup] pipeline part %#x made at play: %.1f ms\n", (unsigned)part, (double)(SDL_GetTicksNS() - t0) / 1e6);
    atomic_fetch_add(&stat_libs, 1);
    if (r == VK_SUCCESS) atomic_fetch_add(&new_pipes, 1);
    else {
        static int n;
        if (n++ < 10) LOG("[vk] pipeline library (part %#x) failed: %d\n", (unsigned)part, r);
    }
    atomic_store(&l->state, r == VK_SUCCESS ? 1 : 2);
}

static void lib_build(Lib *l, int part, const PipeKey *k);
// The part for key: built here if no one has started it, else waited for (another worker is building it)
// The state parts' inputs (also what the shader manifest records of them: warmup.c, vk_warm_state)
typedef struct { uint32_t mask, fmt[32], off[32], bind[32], stride[17], divisor[17], streams; uint8_t topo; } ViState;
typedef struct { VkFormat color[8], depth, stencil; uint32_t ncolor, samples, write_mask[8]; uint8_t blend[8][8], blend_on, a2c; } FoState;
static void vi_state(const PipeKey *k, ViState *v) {
    memset(v, 0, sizeof *v);
    v->mask = k->attr_mask, v->streams = k->used_streams, v->topo = k->topo_class;
    for (int i = 0; i < 32; i++) if (k->attr_mask & (1u << i)) v->fmt[i] = k->attr_fmt[i], v->off[i] = k->attr_off[i], v->bind[i] = k->attr_bind[i];
    for (int i = 0; i < 17; i++) if (k->used_streams & (1u << i)) v->stride[i] = k->stride[i], v->divisor[i] = k->divisor[i];
}
static void fo_state(const PipeKey *k, FoState *v) {
    memset(v, 0, sizeof *v);
    v->ncolor = k->ncolor, v->depth = k->depth, v->stencil = k->stencil, v->samples = k->samples, v->a2c = k->a2c;
    v->blend_on = k->blend_on;
    for (uint32_t i = 0; i < k->ncolor && i < 8; i++) {
        v->color[i] = k->color[i], v->write_mask[i] = k->write_mask[i];
        if ((k->blend_on >> i) & 1) memcpy(v->blend[i], k->blend[i], 8);
    }
}
static __thread bool tl_warm_state;  // (vk_warm_state: not noted as played)
static bool vk_warm_state_build(int kind, const void *state, size_t n, Prog *none);
static VkPipeline lib_get(uint64_t key, int part, const PipeKey *k) {
    pthread_mutex_lock(&lib_lock);
    uint32_t sl = (uint32_t)(key ^ (key >> 29)) & (LIB_TAB - 1);
    while (lib_tab[sl].key && lib_tab[sl].key != key) sl = (sl + 1) & (LIB_TAB - 1);
    Lib *l = lib_tab[sl].lib;
    bool mine = false;
    if (!l && lib_count < LIB_TAB * 3 / 4) {
        l = calloc(1, sizeof *l);
        l->key = key;
        lib_tab[sl].key = key, lib_tab[sl].lib = l, lib_count++;
        atomic_fetch_add(&g_cache_libs, 1);
        mine = true;
    }
    pthread_mutex_unlock(&lib_lock);
    if (!l) return VK_NULL_HANDLE;  // (table full: the caller builds a whole pipeline)
    if (mine && !tl_warm && !tl_warm_state && (part == VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT ||
                             part == VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT)) {  // (the manifest's states)
        extern void warmup_note_state(int kind, const void *state, size_t n);
        if (part == VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT) { ViState v; vi_state(k, &v); warmup_note_state(1, &v, sizeof v); }
        else { FoState v; fo_state(k, &v); warmup_note_state(4, &v, sizeof v); }
    }
    if (mine) lib_build(l, part, k);
    else while (atomic_load(&l->state) == 0) SDL_DelayNS(50000);
    return atomic_load(&l->state) == 1 ? l->pipe : VK_NULL_HANDLE;
}

static void lib_build(Lib *l, int part, const PipeKey *k) {
    {
        PipeInfo s;
        pipe_info(&s, k);
        static const VkPipelineRenderingCreateInfo no_targets = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pDynamicState = &s.dy};
        switch (part) {
        case VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT:
            pi.pVertexInputState = &s.vi, pi.pInputAssemblyState = &s.ia;
            break;
        case VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT:
            pi.pNext = &no_targets, pi.stageCount = 1, pi.pStages = &s.stages[0], pi.pViewportState = &s.vs;
            pi.pRasterizationState = &s.rs, pi.layout = k->prog->layout;
            break;
        case VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT:  // (multisampling: left to the output part)
            pi.pNext = &no_targets, pi.stageCount = 1, pi.pStages = &s.stages[1], pi.pDepthStencilState = &s.ds;
            pi.layout = k->prog->layout;
            break;
        default:  // fragment output
            pi.pNext = &s.ri, pi.pColorBlendState = &s.cb, pi.pMultisampleState = &s.ms;
            break;
        }
        uint64_t t0 = SDL_GetTicksNS();
        lib_create(l, &pi, (VkGraphicsPipelineLibraryFlagsEXT)part);
        if ((part == VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT || part == VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT) &&
            ENV("LARECOMP_WARMUP_LOG"))
            LOG("[warmup] %s part %#x prog %016llx %016llx spv %016llx %016llx fill %d clamp %d binds %u: %.1f ms\n", tl_in_warm ? "warm" : "play",
                (unsigned)part, (unsigned long long)k->prog->dbg_id[0], (unsigned long long)k->prog->dbg_id[1],
                (unsigned long long)k->prog->dbg_spv[0], (unsigned long long)k->prog->dbg_spv[1], k->poly_fill, k->depth_clamp,
                k->prog->nbinds, (double)(SDL_GetTicksNS() - t0) / 1e6);
    }
}

static uint64_t lib_key(uint64_t kind, const void *p, size_t n) { return hash_words(0x6a09e667f3bcc909ull * kind, p, n) | 1; }

static uint64_t key_vertex_input(const PipeKey *k) {
    ViState v;
    vi_state(k, &v);
    return lib_key(1, &v, sizeof v);
}
static uint64_t key_vertex_shader(const PipeKey *k) {
    struct { const Prog *p; uint8_t fill, clamp; } v = {k->prog, k->poly_fill, k->depth_clamp};
    return lib_key(2, &v, sizeof v);
}
static uint64_t key_fragment_shader(const PipeKey *k) {
    struct { const Prog *p; } v = {k->prog};
    return lib_key(3, &v, sizeof v);
}
static uint64_t key_output(const PipeKey *k) {
    FoState v;
    fo_state(k, &v);
    return lib_key(4, &v, sizeof v);
}

// A state part (vertex input: kind 1, fragment output: kind 4) built ahead from a recorded state (warmup.c, the shader
// manifest): kept in lib_tab as play would have made it - a scene's new vertex layouts and target / blend combinations
// cost ~10-50 ms each on a fresh driver cache, built one after another as its draws first ask (the hitches left on a
// first play once its shaders were ready). They depend on no program: a zeroed one stands in.
bool vk_warm_state(int kind, const void *state, size_t n) {
    static Prog none;
    tl_warm_state = tl_in_warm = true;
    bool ok = vk_warm_state_build(kind, state, n, &none);
    tl_warm_state = tl_in_warm = false;
    return ok;
}
static bool vk_warm_state_build(int kind, const void *state, size_t n, Prog *none) {
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.prog = none;
    if (kind == 1 && n == sizeof(ViState)) {
        const ViState *v = state;
        k.attr_mask = v->mask, k.used_streams = v->streams, k.topo_class = v->topo;
        memcpy(k.attr_fmt, v->fmt, sizeof k.attr_fmt), memcpy(k.attr_off, v->off, sizeof k.attr_off), memcpy(k.attr_bind, v->bind, sizeof k.attr_bind);
        memcpy(k.stride, v->stride, sizeof k.stride), memcpy(k.divisor, v->divisor, sizeof k.divisor);
        return lib_get(key_vertex_input(&k), VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, &k) != VK_NULL_HANDLE;
    }
    if (kind == 4 && n == sizeof(FoState)) {
        const FoState *v = state;
        k.ncolor = v->ncolor, k.depth = v->depth, k.stencil = v->stencil, k.samples = v->samples, k.a2c = v->a2c, k.blend_on = v->blend_on;
        memcpy(k.color, v->color, sizeof k.color), memcpy(k.write_mask, v->write_mask, sizeof k.write_mask), memcpy(k.blend, v->blend, sizeof k.blend);
        return lib_get(key_output(&k), VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT, &k) != VK_NULL_HANDLE;
    }
    return false;
}

// The four parts linked (fast: no optimization across them). false: a part failed - the caller builds it whole.
static void gpl_optimize(void *arg);
static bool gpl_link(Pipe *pp) {
    const PipeKey *k = &pp->k;
    VkPipeline parts[4] = {
        lib_get(key_vertex_input(k), VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, k),
        lib_get(key_vertex_shader(k), VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, k),
        lib_get(key_fragment_shader(k), VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, k),
        lib_get(key_output(k), VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT, k)};
    for (int i = 0; i < 4; i++) if (!parts[i]) return false;
    memcpy(pp->parts, parts, sizeof parts);
    VkPipelineLibraryCreateInfoKHR li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, .libraryCount = 4, .pLibraries = parts};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &li, .layout = k->prog->layout};
    uint64_t t0 = SDL_GetTicksNS();
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, VK_NULL_HANDLE, 1, &pi, NULL, &pp->pipe);
    atomic_fetch_add(&stat_link_ns, SDL_GetTicksNS() - t0);
    if (ENV("LARECOMP_WARMUP_LOG") && SDL_GetTicksNS() - t0 > 5000000) LOG("[warmup] pipeline link: %.1f ms\n", (double)(SDL_GetTicksNS() - t0) / 1e6);
    atomic_fetch_add(&stat_links, 1);
    if (r != VK_SUCCESS) {
        static int n;
        if (n++ < 10) LOG("[vk] pipeline link failed: %d\n", r);
        pp->pipe = VK_NULL_HANDLE;
        return false;
    }
    if (gpl_lto()) run_job(gpl_optimize, pp);
    return true;
}

static void gpl_optimize(void *arg) {  // (gpl_lto: the pipeline linked again, optimized; draw_ swaps it in)
    Pipe *pp = arg;
    VkPipelineLibraryCreateInfoKHR li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, .libraryCount = 4, .pLibraries = pp->parts};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &li,
                                       .flags = VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT, .layout = pp->k.prog->layout};
    uint64_t t0 = SDL_GetTicksNS();
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p) == VK_SUCCESS) {
        atomic_fetch_add(&new_pipes, 1);
        atomic_store(&pp->optimized, p);
    }
    atomic_fetch_add(&stat_opt_ns, SDL_GetTicksNS() - t0);
    atomic_fetch_add(&stat_opts, 1);
}

// A compiled program's shader parts, started at once and side by side (one job each: a cold part is ~40-80 ms on
// NVIDIA, the two one after the other kept the program's first pipeline waiting twice as long). The usual rasterizer
// state: filled, no depth clamp.
static void gpl_prebuild_part(Prog *p, int part) {
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.prog = p, k.poly_fill = 1;
    lib_get(part == VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT ? key_fragment_shader(&k) : key_vertex_shader(&k), part, &k);
}
static void gpl_prebuild_vs(void *arg) { gpl_prebuild_part(arg, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT); }
static void gpl_prebuild(void *arg) {  // (the fragment part here, the vertex part on another worker)
    run_job(gpl_prebuild_vs, arg);
    gpl_prebuild_part(arg, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT);
}

static void pipe_ready_stat(Pipe *pp) {
    if (!pp->t_asked) return;
    uint64_t dt = SDL_GetTicksNS() - pp->t_asked, m = atomic_load(&stat_ready_max_ns);
    atomic_fetch_add(&stat_ready_ns, dt), atomic_fetch_add(&stat_ready_n, 1);
    while (dt > m && !atomic_compare_exchange_weak(&stat_ready_max_ns, &m, dt)) {}
}
static void build_pipeline(void *arg) {
    Pipe *pp = arg;
    PipeKey *k = &pp->k;
    if (g_vk.gpl && !tl_warm && gpl_link(pp)) {
        atomic_fetch_add(&stat_compiles, 1);
        pipe_ready_stat(pp);
        atomic_store(&pp->state, 1);
        return;
    }
    PipeInfo s;
    pipe_info(&s, k);
    uint64_t tp = SDL_GetTicksNS();
    extern bool g_pipe_exec_stats;
    if (g_pipe_exec_stats) s.pi.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, tl_warm ? VK_NULL_HANDLE : g_vk.pcache, 1, &s.pi, NULL, &pp->pipe);
    atomic_fetch_add(&stat_pipe_ns, SDL_GetTicksNS() - tp);
    if (r == VK_SUCCESS && g_pipe_exec_stats) pipe_exec_report(pp->pipe, k->prog->code[0], k->prog->code[1]);
    if (r != VK_SUCCESS) {
        static int n;
        if (n++ < 10) LOG("[vk] pipeline creation failed: %d\n", r);
    }
    atomic_fetch_add(&stat_compiles, 1);
    if (r == VK_SUCCESS) atomic_fetch_add(&new_pipes, 1);
    pipe_ready_stat(pp);
    atomic_store(&pp->state, r == VK_SUCCESS ? 1 : 2);
}

// Shader warm-up (warmup.c): a program from the game's files translated and compiled as get_program's would be with the
// default key (its SPIR-V and translation land in their disk caches); pipeline: also built into a pipeline of a typical
// state and released, for the driver's shader cache. That only partly helps the draws' own pipelines later (NVIDIA, fresh
// shader cache, PLA walk test: pipeline creation 10.3 -> 6.0 s) for a long wait (8427 programs: 98 s on 23 threads
// instead of 17), so the warm-up asks for it only with LARECOMP_WARMUP_PIPELINES=1. false: failed.
// keys: a shader manifest entry's (warmup.c) - translated with them, and its pipeline shader parts built into the driver's
// caches as gpl_prebuild makes them in play (where they are then read back, not compiled); NULL: the usual key, no parts
// unless LARECOMP_WARMUP_PARTS
bool vk_warm_program_keyed(const uint8_t *vs, size_t vmax, size_t vcn, const uint8_t *fs, size_t fmax, size_t fcn, size_t flen,
                           bool pipeline, const ShaderKey *vkey, const ShaderKey *fkey) {
    Prog *p = calloc(1, sizeof *p);
    tl_in_warm = true;
    p->fs_len = fs ? (flen ? flen : 1) : 0;
    p->code[0] = (uint64_t)(uintptr_t)vs, p->cmax[0] = vmax;
    p->code[1] = (uint64_t)(uintptr_t)fs, p->cmax[1] = fs ? fmax : 0;
    if (vcn) p->cst[0] = (uint64_t)(uintptr_t)(vs + vmax), p->ncst[0] = vcn;
    if (fs && fcn) p->cst[1] = (uint64_t)(uintptr_t)(fs + fmax), p->ncst[1] = fcn;
    p->vkey.glsl = p->fkey.glsl = 1;  // (flip_y / depth_minus_one 0: window origin and depth mode 1 in both Legends games)
    if (vkey) p->vkey = *vkey;
    if (fkey && fs) p->fkey = *fkey;
    p->warm = true;
    translate_program(p);  // (and compile_program)
    bool ok = atomic_load(&p->state) == 1;
    if (ok && g_vk.gpl && (vkey || ENV("LARECOMP_WARMUP_PARTS"))) {  // (all programs: opt-in, ~80 ms of CPU a part - minutes)
        // its two shader parts, as gpl_prebuild makes them in play, into the pipeline cache: in play they are then read
        // back instead of compiled (~50-100 ms each, cold, on NVIDIA). Built and released (not kept in lib_tab: this
        // program is freed below).
        PipeKey k;
        memset(&k, 0, sizeof k);
        k.prog = p, k.poly_fill = 1;
        Lib l[2];
        memset(l, 0, sizeof l);
        lib_build(&l[0], VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, &k);
        lib_build(&l[1], VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, &k);
        for (int i = 0; i < 2; i++) if (l[i].pipe) vkDestroyPipeline(g_vk.dev, l[i].pipe, NULL);
    }
    if (ok && pipeline) {
        Pipe pp = {0};
        PipeKey *k = &pp.k;
        k->prog = p;
        uint32_t rts = p->code[1] ? p->fi->out_mask & 0xFF : 0;
        while (rts >> k->ncolor) k->ncolor++;
        for (uint32_t i = 0; i < k->ncolor; i++) k->color[i] = VK_FORMAT_R8G8B8A8_UNORM, k->write_mask[i] = 0xF;
        k->depth = VK_FORMAT_D32_SFLOAT;
        k->samples = VK_SAMPLE_COUNT_1_BIT;
        k->attr_mask = p->vi->attr_mask;
        for (int i = 0; i < 32; i++) k->attr_fmt[i] = VK_FORMAT_R32G32B32A32_SFLOAT;
        k->stride[0] = 16, k->used_streams = 1;
        k->topo_class = 2, k->poly_fill = 1;
        tl_warm = true;
        build_pipeline(&pp);
        tl_warm = false;
        ok = atomic_load(&pp.state) == 1;
        if (pp.pipe) vkDestroyPipeline(g_vk.dev, pp.pipe, NULL);
    }
    if (p->vs) vkDestroyShaderModule(g_vk.dev, p->vs, NULL);
    if (p->fs) vkDestroyShaderModule(g_vk.dev, p->fs, NULL);
    if (p->layout) vkDestroyPipelineLayout(g_vk.dev, p->layout, NULL);
    for (int i = 0; i < 2; i++) if (p->set_layout[i]) vkDestroyDescriptorSetLayout(g_vk.dev, p->set_layout[i], NULL);
    if (p->vi) maxwell_free(p->vi);
    if (p->fi && p->code[1]) maxwell_free(p->fi);
    else if (p->fi) { free(p->fi->msl); free(p->fi); }
    free(p);
    tl_in_warm = false;
    return ok;
}

// GPU time per presented frame, averaged over the last half second (-1: not measured yet). Called once per present
// (device.c); the FPS counter and the stats line read the last value (vk_gpu_ms).
static _Atomic uint64_t gpu_ms_bits = 0xBFF0000000000000ull;  // (-1.0)
double vk_gpu_frame_update(void) {
    static uint64_t t0, busy0, n0;
    extern uint64_t renderer_present_count(void);
    uint64_t now = SDL_GetTicksNS(), f = renderer_present_count(), b = atomic_load(&gpu_busy_ns);
    if (!t0 || f < n0) t0 = now, busy0 = b, n0 = f;
    else if (now - t0 >= 500000000ull && f > n0) {
        double ms = (double)(b - busy0) / 1e6 / (double)(f - n0);
        uint64_t bits;
        memcpy(&bits, &ms, 8);
        atomic_store(&gpu_ms_bits, bits);
        t0 = now, busy0 = b, n0 = f;
    }
    return vk_gpu_ms();
}
double vk_gpu_ms(void) {
    uint64_t bits = atomic_load(&gpu_ms_bits);
    double ms;
    memcpy(&ms, &bits, 8);
    return ms;
}

// ================================================================== feedback snapshots (renderer.m resolve_feedback)
static NvnTexture *feedback_tex[4];
static VkImageView feedback_view[4];
static int nfeedback;
static uint64_t g_cur_fs_hash;
static NvnTexture *snap_tex[8];
static bool snap_ok[8];
static const char *snap_why[8];
static const char *snap_reason = "?";
static void snapshots_invalidate(NvnTexture *t) {
    for (int i = 0; i < 8; i++) if ((!t || snap_tex[i] == t) && snap_ok[i]) { snap_ok[i] = false; snap_why[i] = snap_reason; }
}

typedef struct { NvnTexture *key; TexBackend copy; } Snap;
static Snap snaps[16];
static int nsnaps;

static NvnTexture *shader_texture_object(int stage, ShaderTexture *st_) {
    int slot = ((int)st_->handle_offset - 0x20) / 8;
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    uint32_t tic = h & 0xFFFFF;
    if (!h || !st.tpool || tic >= (uint32_t)st.tpool->count) return NULL;
    return ((NvnTexEntry *)st.tpool->entries)[tic].tex;
}

static bool is_attachment(NvnTexture *t) {
    if (!t) return false;
    if (t == st.rt.depth) return true;
    for (int i = 0; i < st.rt.ncolor; i++) if (t == st.rt.color[i]) return true;
    return false;
}

// A draw's part in the pass barriers (begin_pass). Before its pass begins (!after): a texture it samples that a pass
// rendered to since the last barrier, or storage images it reads, need one - the running pass ends and the next begins
// with it (the pass's own attachments: feedback, resolve_feedback; their earlier writes made the pass begin with it).
// After: what it samples is recorded (write after read), and stores to guest memory or transform feedback make the
// next pass wait for everything.
static void hz_draw(Prog *pr, bool after) {
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    bool need = false;
    for (int sg = 0; sg < 2; sg++) {
        ShaderInfo *si = infos[sg];
        if (!si || (sg == 1 && !pr->code[1])) continue;
        if (after && si->gmem_stores) hz_all = true;
        if (si->nimg && !after) {  // (storage images: any image another pass rendered to since)
            for (int k = 0; k < hz_nw && !need; k++) {
                bool own = st.rt.depth && st.rt.depth->backend == hz_w[k];
                for (int c = 0; c < st.rt.ncolor; c++) own |= st.rt.color[c] && st.rt.color[c]->backend == hz_w[k];
                need = !own;
            }
        }
        for (int i = 0; i < si->ntex; i++) {
            NvnTexture *t = shader_texture_object(sg, &si->tex[i]);
            if (!t || !t->backend || is_attachment(t)) continue;
            if (after) hz_add(hz_r, &hz_nr, t->backend);
            else need |= hz_has(hz_w, hz_nw, t->backend);
        }
    }
    if (after) {
        if (st.xfb_active) hz_all = true;
        return;
    }
    if (need && !hz_force) {
        if (in_pass) hz_learned[hz_pass_key & 1023] = hz_pass_key;
        vk_end_pass();
        hz_force = true;
    }
}
// The mip level t is rendered to in this pass (an attachment's view picks one; else level 0)
static uint32_t attachment_level(NvnTexture *t) {
    if (t == st.rt.depth) return st.rt.has_dview && (st.rt.dview.setmask & 1) ? st.rt.dview.base_level : 0;
    for (int i = 0; i < st.rt.ncolor; i++)
        if (t == st.rt.color[i]) return st.rt.has_cview[i] && (st.rt.cview[i].setmask & 1) ? st.rt.cview[i].base_level : 0;
    return 0;
}
// Whether a shader's texture samples the very level its attachment is rendered to: a mip chain built pass by pass
// (Z-A's depth pyramid for its prop culling: each level drawn from the one above) reads another level of the same
// texture, no feedback - its own view binds as is (a snapshot holds only level 0: the passes read it for every
// level, and most of the pyramid came out empty - props tested as hidden: pop-in).
static bool samples_attached_level(int stage, ShaderTexture *st_, NvnTexture *t) {
    int slot = ((int)st_->handle_offset - 0x20) / 8;
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    uint32_t tic = h & 0xFFFFF;
    if (!h || !st.tpool || tic >= (uint32_t)st.tpool->count) return true;
    NvnTexEntry *e = &((NvnTexEntry *)st.tpool->entries)[tic];
    if (!e->has_view || !(e->view.setmask & 1)) return true;  // (all its levels)
    uint32_t lv = attachment_level(t), b = e->view.base_level, n = e->view.num_levels ? e->view.num_levels : 0xFFFF;
    return lv >= b && lv < b + n;
}

static TexBackend *snapshot_image(NvnTexture *t, TexBackend *src) {
    Snap *s = NULL;
    for (int i = 0; i < nsnaps; i++) if (snaps[i].key == t) s = &snaps[i];
    if (!s) {
        if (nsnaps == 16) { static int rr; s = &snaps[rr++ & 15]; } else s = &snaps[nsnaps++];
        s->key = t;
    }
    TexBackend *d = &s->copy;
    if (d->image && (d->width != src->width || d->height != src->height || d->format != src->format)) {
        vk_views_forget(d);  // (resolution change)
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = src->format,
                                .extent = {src->width, src->height, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = (VkSampleCountFlagBits)src->samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = src->format; d->width = src->width; d->height = src->height; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = src->samples; d->aspect = src->aspect; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    return d;
}

// (debug frames) a depth image's depth values as a greyscale PGM, stretched over its own min..max
static void debug_save_depth_layer(TexBackend *b, const char *path, uint32_t layer) {
    if (!b || !b->image || !(b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ||
        (b->format != VK_FORMAT_D32_SFLOAT_S8_UINT && b->format != VK_FORMAT_D32_SFLOAT) || layer >= b->layers) {
        if (b) LOG("[vk] depth dump %s skipped: fmt %d aspect %#x layers %u\n", path, b->format, b->aspect, b->layers);
        return;
    }
    uint64_t n = (uint64_t)b->width * b->height;
    VkSpan out = vk_readback(n * 4);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, layer, 1},
                           .imageExtent = {b->width, b->height, 1}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    const float *d = out.ptr;
    float lo = 1e30f, hi = -1e30f;
    for (uint64_t i = 0; i < n; i++) { if (d[i] < lo) lo = d[i]; if (d[i] > hi) hi = d[i]; }
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5 %u %u 255\n", b->width, b->height);
    for (uint64_t i = 0; i < n; i++) fputc(hi > lo ? (int)((d[i] - lo) / (hi - lo) * 255.0f) : 0, f);
    fclose(f);
    char raw[300];
    snprintf(raw, sizeof raw, "%s.f32", path);  // (the floats as they are)
    if ((f = fopen(raw, "wb"))) { fwrite(d, 4, n, f); fclose(f); }
    LOG("[vk] saved %s (%ux%u depth %.6f..%.6f)\n", path, b->width, b->height, lo, hi);
}
static void debug_save_depth(TexBackend *b, const char *path) { debug_save_depth_layer(b, path, 0); }
void vk_debug_save_r8_layers(TexBackend *b, const char *base) {  // (frame dumps: R8_UINT textures as sampled, raw .pgm)
    if (!b || !b->image || b->format != VK_FORMAT_R8_UINT) return;
    uint64_t n = (uint64_t)b->width * b->height;
    for (uint32_t l = 0; l < b->layers && l < 8; l++) {
        VkSpan out = vk_readback(n);
        VkCommandBuffer cb = vk_cmd();
        vk_full_barrier(cb);
        VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, l, 1},
                               .imageExtent = {b->width, b->height, 1}};
        vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
        renderer_flush();
        char path[256];
        snprintf(path, sizeof path, "%s_l%u.pgm", base, l);
        FILE *f = fopen(path, "wb");
        if (!f) continue;
        fprintf(f, "P5 %u %u 255\n", b->width, b->height);
        fwrite(out.ptr, 1, n, f);
        fclose(f);
        LOG("[vk] saved %s (R8UI)\n", path);
    }
}
void vk_debug_save_depth_mips(TexBackend *b, const char *base) {  // (frame dumps: a mipmapped depth texture's levels)
    if (!b || !b->image || !(b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ||
        (b->format != VK_FORMAT_D32_SFLOAT_S8_UINT && b->format != VK_FORMAT_D32_SFLOAT)) return;
    for (uint32_t l = 0; l < b->levels && l < 8; l++) {
        uint32_t w = b->width >> l ? b->width >> l : 1, h = b->height >> l ? b->height >> l : 1;
        uint64_t n = (uint64_t)w * h;
        VkSpan out = vk_readback(n * 4);
        VkCommandBuffer cb = vk_cmd();
        vk_full_barrier(cb);
        VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, l, 0, 1}, .imageExtent = {w, h, 1}};
        vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
        renderer_flush();
        const float *d = out.ptr;
        float lo = 1e30f, hi = -1e30f;
        for (uint64_t i = 0; i < n; i++) { if (d[i] < lo) lo = d[i]; if (d[i] > hi) hi = d[i]; }
        char path[256];
        snprintf(path, sizeof path, "%s_m%u.pgm", base, l);
        FILE *f = fopen(path, "wb");
        if (!f) continue;
        fprintf(f, "P5 %u %u 255\n", w, h);
        for (uint64_t i = 0; i < n; i++) fputc(hi > lo ? (int)((d[i] - lo) / (hi - lo) * 255.0f) : 0, f);
        fclose(f);
        LOG("[vk] saved %s_m%u (%ux%u depth %.6f..%.6f)\n", base, l, w, h, lo, hi);
    }
}
void vk_debug_save_r32f_mips(TexBackend *b, const char *base) {  // (frame dumps: an R32F texture's levels as sampled)
    if (!b || !b->image || b->format != VK_FORMAT_R32_SFLOAT) return;
    for (uint32_t l = 0; l < b->levels && l < 8; l++) {
        uint32_t w = b->width >> l ? b->width >> l : 1, h = b->height >> l ? b->height >> l : 1;
        uint64_t n = (uint64_t)w * h;
        VkSpan out = vk_readback(n * 4);
        VkCommandBuffer cb = vk_cmd();
        vk_full_barrier(cb);
        VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1}, .imageExtent = {w, h, 1}};
        vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
        renderer_flush();
        const float *d = out.ptr;
        float lo = 1e30f, hi = -1e30f;
        for (uint64_t i = 0; i < n; i++) { if (d[i] < lo) lo = d[i]; if (d[i] > hi) hi = d[i]; }
        char path[256];
        snprintf(path, sizeof path, "%s_m%u.pgm", base, l);
        FILE *f = fopen(path, "wb");
        if (!f) continue;
        fprintf(f, "P5 %u %u 255\n", w, h);
        for (uint64_t i = 0; i < n; i++) fputc(hi > lo ? (int)((d[i] - lo) / (hi - lo) * 255.0f) : 0, f);
        fclose(f);
        snprintf(path, sizeof path, "%s_m%u.f32", base, l);
        if ((f = fopen(path, "wb"))) { fwrite(d, 4, n, f); fclose(f); }
        LOG("[vk] saved %s_m%u (%ux%u, %.6f..%.6f)\n", base, l, w, h, lo, hi);
    }
}
void vk_debug_save_depth_layers(TexBackend *b, const char *base) {  // (frame dumps: every layer of a pass's depth target)
    char path[256];
    for (uint32_t l = 0; b && l < b->layers && l < 8; l++) {
        snprintf(path, sizeof path, "%s_l%u.pgm", base, l);
        debug_save_depth_layer(b, path, l);
    }
}

static void resolve_feedback(Prog *pr) {
    nfeedback = 0;
    static int off = -1;
    if (off < 0) off = ENV("LARECOMP_NO_FEEDBACK") != NULL;
    if (off) return;
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < infos[sg]->ntex; i++) {
            NvnTexture *t = shader_texture_object(sg, &infos[sg]->tex[i]);
            if (!is_attachment(t)) continue;
            static int nolevel = -1;  // (LARECOMP_FEEDBACK_ALL_LEVELS=1: every level counted as feedback, as before)
            if (nolevel < 0) nolevel = ENV("LARECOMP_FEEDBACK_ALL_LEVELS") != NULL;
            if (!nolevel && !samples_attached_level(sg, &infos[sg]->tex[i], t)) continue;
            {   // A mip chain built pass by pass: the attachment is a level N > 0 and the shader samples the texture
                // (an explicit lod, the level above): it gets the levels before N, in place - not a snapshot of level 0
                uint32_t lv = attachment_level(t);
                bool have = false;
                for (int k = 0; k < nfeedback; k++) have |= feedback_tex[k] == t;
                if (!nolevel && lv > 0 && !have && nfeedback < 4) {
                    TexBackend *src = vk_tex(t, true);
                    VkImageAspectFlags sa = src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : src->aspect;
                    static const VkComponentMapping id = {0};
                    VkImageView v = lv <= src->levels ? vk_image_view(src, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, lv, 0, 1, id, sa) : VK_NULL_HANDLE;
                    if (v) {
                        feedback_tex[nfeedback] = t;
                        feedback_view[nfeedback++] = v;
                        static int logged;
                        if (!logged++) LOG("[vk] mip chain built in place (first: level %u of a %ux%u %s texture reads levels 0..%u)\n", lv,
                                           src->width, src->height, sa & VK_IMAGE_ASPECT_DEPTH_BIT ? "depth" : "color", lv - 1);
                        continue;
                    }
                }
            }
            bool have = false;
            for (int k = 0; k < nfeedback; k++) have |= feedback_tex[k] == t;
            if (have || nfeedback == 4) continue;
            static int nosnap = -1;
            if (nosnap < 0) nosnap = ENV("LARECOMP_NO_SNAPSHOT_REUSE") != NULL;
            int si = -1;
            for (int k = 0; k < 8; k++) if (snap_tex[k] == t) si = k;
            bool self_write = t == st.rt.depth && st.ds[0] && st.ds[1];
            TexBackend *src = vk_tex(t, true);
            Snap *existing = NULL;
            for (int k = 0; k < nsnaps; k++) if (snaps[k].key == t && snaps[k].copy.image) existing = &snaps[k];
            VkImageAspectFlags sa = src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : src->aspect;
            static const VkComponentMapping id = {0};
            if (!nosnap && si >= 0 && (snap_ok[si] || self_write) && existing) {
                feedback_tex[nfeedback] = t;
                feedback_view[nfeedback++] = vk_image_view(&existing->copy, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, sa);
                continue;
            }
            stat_feedback_copies++;
            vk_end_pass();
            TexBackend *dst = snapshot_image(t, src);
            VkCommandBuffer cb = vk_cmd();
            vk_full_barrier(cb);
            gp_mark(cb, "feedback snapshot %ux%u fmt %d", src->width, src->height, src->format);
            VkImageCopy rg = {.srcSubresource = {src->aspect, 0, 0, 1}, .dstSubresource = {src->aspect, 0, 0, 1},
                              .extent = {src->width, src->height, 1}};
            vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
            if (g_debug_frame && (src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
                static int nd;
                char path[128];
                snprintf(path, sizeof path, "build/fbdepth_%03d_src.pgm", nd);
                debug_save_depth(src, path);
                snprintf(path, sizeof path, "build/fbdepth_%03d_snap.pgm", nd++);
                debug_save_depth(dst, path);
                cb = vk_cmd();
            }
            feedback_tex[nfeedback] = t;
            feedback_view[nfeedback++] = vk_image_view(dst, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, sa);
            if (si < 0) { static int rr; si = rr++ & 7; snap_tex[si] = t; }
            snap_ok[si] = true;
            st.rt_dirty = true;
        }
}

// ================================================================== resource binding
static VkDescriptorBufferInfo ubo_span(VkSpan s, uint64_t size) {
    uint64_t max = g_vk.props.limits.maxUniformBufferRange;
    return (VkDescriptorBufferInfo){s.buf, s.off, size > max ? max : size};
}

// LARECOMP_FIND_VIEW_AT (device.c): each distinct constant buffer range bound that frame, scanned for 3x4 row-major
// rigid transforms (three orthonormal rows: a view or model matrix) - where the camera is, for the VR mod.
uint64_t g_findview_frame;
static void findview_scan(int stage, int k, uint64_t addr, uint64_t size) {
    static uint64_t seen[512], seen_frame;
    static int nseen;
    if (seen_frame != g_findview_frame) { seen_frame = g_findview_frame; nseen = 0; }
    for (int i = 0; i < nseen; i++) if (seen[i] == addr) return;
    if (nseen < 512) seen[nseen++] = addr;
    const float *f = (const float *)(uintptr_t)addr;
    {   // and the raw contents of each, for offline analysis: build/findview/<frame>_s<stage>_k<k>_<addr>.bin
        static int made;
        if (!made) made = 1, mkdir("build/findview", 0755);
        char path[128];
        snprintf(path, sizeof path, "build/findview/%llu_s%d_k%d_%llx.bin", (unsigned long long)g_findview_frame, stage, k,
                 (unsigned long long)addr);
        FILE *fp = fopen(path, "wb");
        if (fp) fwrite(f, 1, size, fp), fclose(fp);
    }
    int found = 0;
    for (uint64_t o = 0; o + 48 <= size && found < 6; o += 16) {
        const float *m = f + o / 4;
        bool ok = true, ident = true;
        for (int r = 0; r < 3 && ok; r++) {
            float n = m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2];
            if (!(n > 0.998f && n < 1.002f)) ok = false;
            for (int c = r + 1; c < 3 && ok; c++) {
                float d = m[r * 4] * m[c * 4] + m[r * 4 + 1] * m[c * 4 + 1] + m[r * 4 + 2] * m[c * 4 + 2];
                if (!(d > -0.002f && d < 0.002f)) ok = false;
            }
            for (int c = 0; c < 3; c++) if (m[r * 4 + c] != (r == c ? 1.0f : 0.0f)) ident = false;
        }
        if (!ok || ident) continue;
        found++;
        LOG("[findview] frame %llu stage %d cbuf %d addr %llx+%llx (size %llx): %.4f %.4f %.4f %.2f | %.4f %.4f %.4f %.2f | %.4f %.4f %.4f %.2f\n",
            (unsigned long long)g_findview_frame, stage, k, (unsigned long long)addr, (unsigned long long)o, (unsigned long long)size,
            m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
    }
}

// LARECOMP_CAMSCAN=<cbuf addr>@<frame> (device.c): the camera's eye position from the view matrix at cbuf+0x40 (column-major,
// PLA's per-view constants), then every place in the guest's writable memory holding those three floats - the camera
// object, for the VR mod. Logged as "[camscan]" with the 0x40 bytes around each hit.
void camscan(uint64_t cbuf) {
    const float *v = (const float *)(uintptr_t)(cbuf + 0x40);
    float eye[3];
    for (int c = 0; c < 3; c++) eye[c] = -(v[c * 4 + 0] * v[12] + v[c * 4 + 1] * v[13] + v[c * 4 + 2] * v[14]);
    LOG("[camscan] eye %.4f %.4f %.4f forward %.4f %.4f %.4f\n", eye[0], eye[1], eye[2], -v[2], -v[6], -v[10]);
    FILE *m = fopen("/proc/self/maps", "r");
    char line[512];
    int hits = 0;
    while (m && fgets(line, sizeof line, m) && hits < 200) {
        unsigned long long lo, hi;
        char perm[8];
        if (sscanf(line, "%llx-%llx %7s", &lo, &hi, perm) != 3 || perm[0] != 'r' || perm[1] != 'w') continue;
        if (lo < 0x7000000000ull || hi > 0x8000000000ull) continue;
        for (uint64_t a = lo; a + 12 <= hi && hits < 200; a += 4) {
            const float *f = (const float *)(uintptr_t)a;
            float d0 = f[0] - eye[0];
            if (!(d0 < 0.02f && d0 > -0.02f)) continue;  // (written so that NaN fails)
            float d1 = f[1] - eye[1], d2 = f[2] - eye[2];
            if (!(d1 < 0.02f && d1 > -0.02f && d2 < 0.02f && d2 > -0.02f)) continue;
            hits++;
            if (f[-8] > 1.777f && f[-8] < 1.778f && f[-7] == -960.0f && ENV("LARECOMP_CAMSCAN_WATCH")) {
                // a render camera like Legends: Arceus' (aspect, the 1920x1080 extents, then eye and view): who writes it
                extern void hwwatch_arm(uint64_t addr, uint64_t len);
                static int armed;
                if (armed < 2) armed++, hwwatch_arm(a, 8), hwwatch_arm(a + 0x10, 8);
            }
            if (f[5] > 1.777f && f[5] < 1.778f && ENV("LARECOMP_CAMSCAN_WATCH_REC")) {  // the camera record (quaternion,
                extern void hwwatch_arm(uint64_t addr, uint64_t len);                  // eye, fov, aspect): who writes it
                static bool armed;
                if (!armed) {
                    armed = true;
                    hwwatch_arm(a - 0x10, 8), hwwatch_arm(a - 8, 8), hwwatch_arm(a, 8), hwwatch_arm(a + 0x10, 8);
                }
            }
            const float *w = (const float *)(uintptr_t)((a - 0x20) & ~0xfull);
            LOG("[camscan] hit %llx (region %llx-%llx)\n", (unsigned long long)a, lo, hi);
            for (int r = 0; r < 6; r++)
                LOG("[camscan]   %llx: %11.4f %11.4f %11.4f %11.4f\n", (unsigned long long)(uintptr_t)(w + r * 4), w[r * 4], w[r * 4 + 1],
                    w[r * 4 + 2], w[r * 4 + 3]);
        }
    }
    if (m) fclose(m);
    LOG("[camscan] %d hit(s)\n", hits);
}

static int g_ubo_direct = -1;  // constant buffers bound in place (1) or copied per draw (0); cbuf_info
// Ranges the game updates inline (UpdateUniformBuffer: written between draws of one list, which in place would all read
// the last value): constant buffers over them are copied per draw, the rest stay in place. Legends Z-A has a few.
#define NINLINE 256
static struct { uint64_t lo, hi; } g_inline[NINLINE];
static int g_ninline;
static bool inline_target(uint64_t lo, uint64_t hi) {
    for (int i = 0; i < g_ninline; i++) if (lo < g_inline[i].hi && g_inline[i].lo < hi) return true;
    return false;
}
static void inline_note(uint64_t lo, uint64_t hi) {
    for (int i = 0; i < g_ninline; i++)
        if (lo <= g_inline[i].hi && g_inline[i].lo <= hi) {  // (overlapping or touching: one range)
            if (lo < g_inline[i].lo) g_inline[i].lo = lo;
            if (hi > g_inline[i].hi) g_inline[i].hi = hi;
            return;
        }
    if (g_ninline < NINLINE) {
        g_inline[g_ninline].lo = lo, g_inline[g_ninline].hi = hi, g_ninline++;
        static int logged;
        if (logged++ < 8) LOG("[vk] inline uniform updates at %llx..%llx: copied per draw (%d ranges)\n", (unsigned long long)lo,
                              (unsigned long long)hi, g_ninline);
    } else if (g_ubo_direct == 1) {
        g_ubo_direct = 0;  // (too many to track: copies everywhere)
        LOG("[vk] inline uniform updates in over %d ranges: constant buffers are copied per draw from now on\n", NINLINE);
    }
}
static uint64_t g_dispatch_indirect;  // (dispatch_compute: nonzero while an indirect dispatch runs - its argument address)
static VkSpan g_dispatch_drv;         // (its compute driver constants: cbuf_info)
// Constant buffer k of a stage (renderer.m bind_cbufs), bound in place or copied into transient memory.
static VkDescriptorBufferInfo cbuf_info(int stage, ShaderInfo *si, int k, uint32_t gx, uint32_t gy, uint32_t gz) {
    static const uint8_t zeros[0x10000];
    if (k == 0) {
        if (stage != 5) {  // (graphics: made of the storage buffer bindings alone - compared by those, not as built per draw)
            static struct { uint64_t seq, size; Range sb[16]; VkSpan span; } m[2];
            int sg = stage == 1;
            uint64_t size = si->cbuf_size[0] > 0x800 ? 0x800 : (si->cbuf_size[0] ? si->cbuf_size[0] : 256), seq = vk_pending_seq();
            if (m[sg].seq == seq && m[sg].size == size && !memcmp(m[sg].sb, st.sbuf[stage], sizeof m[sg].sb)) return ubo_span(m[sg].span, size);
            uint32_t drv[0x800 / 4];
            memset(drv, 0, size);
            uint32_t base = 0x110 + 0x100 * (stage == 1 ? 4 : 0);
            for (int i = 0; i < 16; i++) {  // (past `size`: not copied)
                uint32_t *e = &drv[(base + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[stage][i].addr;
                e[1] = (uint32_t)(st.sbuf[stage][i].addr >> 32);
                e[2] = (uint32_t)st.sbuf[stage][i].size;
            }
            m[sg].span = vk_transient_copy(drv, size, 256);
            m[sg].seq = seq, m[sg].size = size;
            memcpy(m[sg].sb, st.sbuf[stage], sizeof m[sg].sb);
            return ubo_span(m[sg].span, size);
        }
        uint32_t drv[0x800 / 4] = {0};
        if (stage == 5) {
            drv[0x200 / 4] = gx; drv[0x204 / 4] = gy; drv[0x208 / 4] = gz;
            for (int i = 0; i < 16; i++) {
                uint32_t *u = &drv[(0x210 + 16 * i) / 4];
                u[0] = (uint32_t)st.ubuf[5][i].addr; u[1] = (uint32_t)(st.ubuf[5][i].addr >> 32); u[2] = (uint32_t)st.ubuf[5][i].size;
                uint32_t *e = &drv[(0x310 + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[5][i].addr; e[1] = (uint32_t)(st.sbuf[5][i].addr >> 32); e[2] = (uint32_t)st.sbuf[5][i].size;
            }
        } else {
            uint32_t base = 0x110 + 0x100 * (stage == 1 ? 4 : 0);
            for (int i = 0; i < 16; i++) {
                uint32_t *e = &drv[(base + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[stage][i].addr;
                e[1] = (uint32_t)(st.sbuf[stage][i].addr >> 32);
                e[2] = (uint32_t)st.sbuf[stage][i].size;
            }
        }
        uint64_t size = si->cbuf_size[0] > sizeof drv ? sizeof drv : (si->cbuf_size[0] ? si->cbuf_size[0] : 256);
        if (size < 0x500 && stage == 5) size = 0x500;
        if (stage == 5 && g_dispatch_indirect) {  // (its own copy: the group counts are copied into it on the GPU)
            g_dispatch_drv = vk_transient_copy(drv, size, 256);
            return ubo_span(g_dispatch_drv, size);
        }
        return ubo_span(copy_dedupe(stage == 5 ? DEDUPE_DRV_C : stage == 1 ? DEDUPE_DRV_F : DEDUPE_DRV_V, drv, size, 256), size);
    }
    if (k == 1) {  // compiler constants stored after the shader code
        NvnShaderStage *ss = &st.program->stages[stage];
        uint64_t size = ss->const_size ? ss->const_size : 16;
        if (size < si->cbuf_size[1]) size = si->cbuf_size[1];
        return ubo_span(guest_copy(ss->code_addr + ss->const_offset, size), size);  // (constant: shared per submission)
    }
    int idx = k - 3;  // cbuf_base
    Range r = (idx >= 0 && idx < 18) ? st.ubuf[stage][idx] : (Range){0, 0};
    uint64_t size = si->cbuf_size[k] ? si->cbuf_size[k] : 256;
    if (!r.addr) return ubo_span(vk_transient_copy(zeros, size, 256), size);
    if ((si->cbuf_dynamic & (1u << k)) && r.size) size = r.size > 0x10000 ? 0x10000 : r.size;
    if (size > r.size && r.size) size = r.size;
    if (size < 16) size = 16;
    bool skinned_vs = stage == 0 && ((si->attr_mask >> 11) & 1);  // (Let's Go: a11 = bone indices)
    // Experiment X2 (letsgo-recomp's docs/PLANO-3D.md), LARECOMP_VK_SKIN_IDENTITY=1: a skinned vertex shader's dynamically indexed
    // cbuf (its bone palette) reads identity matrices - models in their bind pose if the rest of the chain is right.
    static int skin_identity = -1;
    if (skin_identity < 0) skin_identity = ENV("LARECOMP_VK_SKIN_IDENTITY") != NULL;
    if (skin_identity && skinned_vs && (si->cbuf_dynamic & (1u << k))) {
        static float ident[0x10000 / 4];
        if (ident[0] != 1.0f)
            for (int m = 0; m < 0x10000 / 64; m++) ident[m * 16] = ident[m * 16 + 5] = ident[m * 16 + 10] = ident[m * 16 + 15] = 1.0f;
        g_exp_count[0]++;
        return ubo_span(vk_transient_copy(ident, size, 256), size);
    }
    if (g_plugins_cbuf && !ip_replaying) plugins_cbuf_bound(stage, r.addr, (uint32_t)size);  // (plugins see every bind, in place or copied)
    // Stereo, eye 1 (vk_stereo_render): what the plugin puts in a buffer's place, else the copy eye 0 bound (a range the
    // game updates inside its lists holds only the last value now), else the same as eye 0 - game memory in place, the
    // game's fences covering this second drawing (renderthread.c rt_wait_present)
    uint64_t st_key = 0;
    if (st_on() && stage <= 1) {
        st_key = ip_key(stage, k);
        const void *d = ip_replaying ? st_cbuf_replay(st_key, stage, r.addr, (uint32_t)size) : NULL;
        if (d) return ubo_span(vk_transient_copy(d, size, 256), size);
    }
    // Bound in place in the imported guest heap, read when the GPU runs - as on the console (the game's NVN fences wait
    // for the GPU: nvnSyncWait, rt_wait_ticket), and no per-draw copy (Legends: Arceus copied 6 MB of constants a
    // frame). Ranges the game updates inline are copied (g_inline). LARECOMP_VK_UBO_DIRECT=0: always copy.
    if (g_ubo_direct < 0) g_ubo_direct = ENV("LARECOMP_VK_UBO_DIRECT") ? atoi(ENV("LARECOMP_VK_UBO_DIRECT")) != 0 : 1;
    if (g_ubo_direct == 1 && !ip_enabled() && !inline_target(r.addr, r.addr + size)) {  // (frame interpolation records the copies)
        extern bool vk_guest_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off);
        VkBuffer gb;
        VkDeviceSize goff;
        uint64_t align = g_vk.props.limits.minUniformBufferOffsetAlignment;
        if (!(r.addr % (align ? align : 256)) && vk_guest_direct(r.addr, size, &gb, &goff)) {
            if (skinned_vs) g_exp_count[1]++;
            return ubo_span((VkSpan){.buf = gb, .off = goff}, size);
        }
        if (skinned_vs) g_exp_count[2]++;
    }
    if (ip_enabled()) {  // frame interpolation: recorded, or the blended copy in the replay
        const void *d = ip_cbuf(stage, k, r.addr, (uint32_t)size);
        if (d) return ubo_span(vk_transient_copy(d, size, 256), size);
    }
    if (st_key && !ip_replaying) ip_record(st_key, (const void *)(uintptr_t)r.addr, (uint32_t)size);  // (stereo: a copied bind, for eye 1)
    // snapshot: later CPU writes must not leak in. Shared by the draws of one submission binding the same range:
    // within a command list nothing writes guest memory (no inline UpdateUniformBuffer: that writes through below
    // and drops the shared copies).
    if (g_findview_frame) findview_scan(stage, k, r.addr, size);
    return ubo_span(guest_copy(r.addr, size), size);
}

// Global memory (shader LDG/STG, maxwell_gmem_base = HEAP_BASE): the guest heap imported as a GPU buffer
// (VK_EXT_external_memory_host), zero-copy like metal.m's g_guest_buffer, so compute results land in guest
// memory. The import pins its pages, so it covers only the heap the game set up (SetHeapSize), made on first use.
// LARECOMP_VK_NO_GMEM=1: no import, reads return zero.
static VkDeviceMemory gmem_mem;
static uint64_t gmem_size;
static VkDeviceAddress gmem_addr;  // (its GPU address: shaders reach the heap through it - gmem_ref_info)

static bool gmem_import(uint64_t size) {
#ifdef _WIN32
    plat_commit(HEAP_BASE, size);  // (Windows commits guest memory on first touch: the driver can only pin committed pages)
#endif
    PFN_vkGetMemoryHostPointerPropertiesEXT props =
        (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetMemoryHostPointerPropertiesEXT");
    VkMemoryHostPointerPropertiesEXT mp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult pr = props ? props(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void *)(uintptr_t)HEAP_BASE, &mp) : VK_ERROR_EXTENSION_NOT_PRESENT;
    if (pr != VK_SUCCESS || !mp.memoryTypeBits) {
        LOG("[vk] guest heap import: host pointer properties %d, memory types %#x\n", (int)pr, mp.memoryTypeBits);
        return false;
    }
    VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                            .pHostPointer = (void *)(uintptr_t)HEAP_BASE};
    VkMemoryAllocateFlagsInfo af = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .pNext = &imp,
                                    .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &af, .allocationSize = size,
                               .memoryTypeIndex = (uint32_t)__builtin_ctz(mp.memoryTypeBits)};
    VkDeviceMemory mem;
    VkResult ar = vkAllocateMemory(g_vk.dev, &ai, NULL, &mem);
    if (ar != VK_SUCCESS) { LOG("[vk] guest heap import: vkAllocateMemory %d (%llu MB)\n", (int)ar, (unsigned long long)(size >> 20)); return false; }
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = size,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |  // (uniforms read in place: cbuf_info)
                                      VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |  // (indirect draws: arguments the game's compute writes)
                                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |  // (shader global memory: gmem_ref_info)
                                      (g_vk.xfb ? VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT : 0)};  // (CMD_XFB_*)
    VkBuffer buf;
    if (vkCreateBuffer(g_vk.dev, &bi, NULL, &buf) != VK_SUCCESS) { vkFreeMemory(g_vk.dev, mem, NULL); return false; }
    vkBindBufferMemory(g_vk.dev, buf, mem, 0);
    if (gmem_buf) {  // the heap grew: the old import may still be in use by submitted work
        vk_wait_seq(submit_seq);
        vkDestroyBuffer(g_vk.dev, gmem_buf, NULL);
        vkFreeMemory(g_vk.dev, gmem_mem, NULL);
    }
    gmem_buf = buf;
    gmem_mem = mem;
    gmem_size = size;
    VkBufferDeviceAddressInfo dai = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buf};
    gmem_addr = vkGetBufferDeviceAddress(g_vk.dev, &dai);
    LOG("[vk] guest heap imported for shader global memory: %llu MB\n", (unsigned long long)(size >> 20));
    return true;
}

static uint64_t gmem_tried;  // heap size the import was last attempted for (gmem_info imports / re-imports)
static VkDescriptorBufferInfo gmem_info(void);

// Without the host import (gpuheap.c: the heap window is GPU memory, dma-buf chunks mapped at their guest addresses):
// one sparse buffer over the whole window, each chunk imported and bound into it as gpuheap backs it. Bound before the
// work that may read it is submitted (a fence: binds are few, at load).
#define GMEM_MAX_CHUNKS 256
static VkDeviceMemory gmem_chunk[GMEM_MAX_CHUNKS];
// Each chunk also as a plain buffer over the same memory, for what a driver can't do with a sparse one (Turnip's
// transform feedback binding reads the buffer's own allocation: null for a sparse buffer, a crash): gmem_plain
static VkBuffer gmem_chunk_buf[GMEM_MAX_CHUNKS];
static uint64_t gmem_chunk_size;
static bool gmem_sparse_bind(void) {
    extern int gpuheap_take(uint64_t *off, int *fds, int max);
    extern uint64_t gpuheap_end(void), gpuheap_chunk_size(void), g_gpuheap_end, g_gpuheap_gen;
    static uint64_t gen_bound = UINT64_MAX;  // (gpuheap backs chunks below its end lazily on RADV: g_gpuheap_gen counts them)
    uint64_t gen = __atomic_load_n(&g_gpuheap_gen, __ATOMIC_ACQUIRE);
    if (gmem_buf && __atomic_load_n(&g_gpuheap_end, __ATOMIC_ACQUIRE) == gmem_size && gen == gen_bound) return true;  // (nothing new)
    gen_bound = gen;
    if (!gmem_buf) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = HEAP_MAX,
                                 .flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                          VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                          (g_vk.xfb ? VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT : 0)};
        if (vkCreateBuffer(g_vk.dev, &bi, NULL, &gmem_buf) != VK_SUCCESS) { gmem_buf = VK_NULL_HANDLE; return false; }
        VkBufferDeviceAddressInfo dai = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = gmem_buf};
        gmem_addr = vkGetBufferDeviceAddress(g_vk.dev, &dai);
    }
    uint64_t off[GMEM_MAX_CHUNKS];
    int fds[GMEM_MAX_CHUNKS];
    int n = gpuheap_take(off, fds, GMEM_MAX_CHUNKS);
    if (n) {
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(g_vk.dev, gmem_buf, &req);
        PFN_vkGetMemoryFdPropertiesKHR fdprops = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(g_vk.dev, "vkGetMemoryFdPropertiesKHR");
        VkSparseMemoryBind binds[GMEM_MAX_CHUNKS];
        uint64_t chunk = gpuheap_chunk_size();
        for (int i = 0; i < n; i++) {
            VkMemoryFdPropertiesKHR fp = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
            if (fdprops) fdprops(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fds[i], &fp);
            uint32_t bits = fp.memoryTypeBits & req.memoryTypeBits;
            if (!bits) FATAL("guest heap: the GPU memory can't back the heap buffer (types %#x / %#x)\n", fp.memoryTypeBits, req.memoryTypeBits);
            VkImportMemoryFdInfoKHR imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                                           .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = fds[i]};
            VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = chunk,
                                       .memoryTypeIndex = (uint32_t)__builtin_ctz(bits)};
            unsigned k = (unsigned)(off[i] / chunk);
            VkResult r = k < GMEM_MAX_CHUNKS ? vkAllocateMemory(g_vk.dev, &ai, NULL, &gmem_chunk[k]) : VK_ERROR_OUT_OF_DEVICE_MEMORY;
            if (r != VK_SUCCESS) FATAL("guest heap: importing its GPU memory failed (%d)\n", (int)r);
            binds[i] = (VkSparseMemoryBind){.resourceOffset = off[i], .size = chunk, .memory = gmem_chunk[k]};
            VkBufferCreateInfo pb = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = chunk,
                                     .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                              VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                              (g_vk.xfb ? VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT : 0)};
            if (vkCreateBuffer(g_vk.dev, &pb, NULL, &gmem_chunk_buf[k]) == VK_SUCCESS)
                vkBindBufferMemory(g_vk.dev, gmem_chunk_buf[k], gmem_chunk[k], 0);
            gmem_chunk_size = chunk;
        }
        VkSparseBufferMemoryBindInfo bb = {.buffer = gmem_buf, .bindCount = (uint32_t)n, .pBinds = binds};
        VkBindSparseInfo si = {.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, .bufferBindCount = 1, .pBufferBinds = &bb};
        VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence;
        vkCreateFence(g_vk.dev, &fci, NULL, &fence);
        extern void vk_queue_lock(void), vk_queue_unlock(void);
        vk_queue_lock();
        VkResult r = vkQueueBindSparse(g_vk.queue, 1, &si, fence);
        vk_queue_unlock();
        if (r == VK_SUCCESS) r = vkWaitForFences(g_vk.dev, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(g_vk.dev, fence, NULL);
        if (r != VK_SUCCESS) FATAL("guest heap: binding its GPU memory failed (%d)\n", (int)r);
    }
    uint64_t end = gpuheap_end();
    if (end != gmem_size) LOG("[vk] guest heap bound for shader global memory: %llu MB (sparse)\n", (unsigned long long)(end >> 20));
    if (n) {
        extern unsigned gpuheap_backed_mb(void);
        static unsigned shown;
        unsigned mb = gpuheap_backed_mb();
        if (mb >= shown + 512 || ENV("LARECOMP_LOG_GPUHEAP")) LOG("[vk] guest heap: %u MB of GPU memory backed\n", mb), shown = mb;
    }
    gmem_size = end;
    return true;
}
// [a, a + n) of guest memory as a buffer and offset: the imported heap's; with the sparse heap, its chunk's plain
// buffer (false when the range crosses chunks)
static bool gmem_plain(uint64_t a, uint64_t n, VkBuffer *buf, VkDeviceSize *off) {
    if (!gmem_buf || a < HEAP_BASE || a + n > HEAP_BASE + gmem_size) return false;
    uint64_t o = a - HEAP_BASE;
    if (!g_vk.gpuheap) { *buf = gmem_buf, *off = o; return true; }
    unsigned k = gmem_chunk_size ? (unsigned)(o / gmem_chunk_size) : 0;
    if (!gmem_chunk_size || k >= GMEM_MAX_CHUNKS || !gmem_chunk_buf[k] || (o + n - 1) / gmem_chunk_size != k) return false;
    *buf = gmem_chunk_buf[k], *off = o - (uint64_t)k * gmem_chunk_size;
    return true;
}

// What shaders read and write the guest heap through (binding GMEM_BUFFER_INDEX): its GPU address and the size imported,
// a 16-byte uniform. The translator's GLSL turns them into a buffer reference (GL_EXT_buffer_reference): 64-bit
// addressing, no 4 GB limit - the heap was a storage buffer, and Vulkan storage buffers reach 4 GB at most
// (maxStorageBufferRange): past that every read returned 0 (Z-A's text was gone with a bigger game heap).
static VkDescriptorBufferInfo gmem_ref_info(void) {
    gmem_info();  // (imported, or grown, first)
    uint64_t v[2] = {gmem_buf ? (uint64_t)gmem_addr : 0, gmem_buf ? gmem_size : 0};
    VkSpan s = vk_transient_copy(v, sizeof v, g_vk.props.limits.minUniformBufferOffsetAlignment);
    return (VkDescriptorBufferInfo){s.buf, s.off, sizeof v};
}

static VkDescriptorBufferInfo gmem_info(void) {
    static int off = -1;
    if (off < 0) off = ENV("LARECOMP_VK_NO_GMEM") != NULL || (!g_vk.host_import && !g_vk.gpuheap);
    if (!off && g_vk.gpuheap) {  // (the heap window is GPU memory: gpuheap.c)
        if (!gmem_sparse_bind()) LOG("[vk] guest heap: no sparse buffer: shader global memory reads return zero\n"), off = 1;
        uint64_t max = g_vk.props.limits.maxStorageBufferRange;  // (a storage buffer binding: shaders use gmem_ref_info)
        if (gmem_buf) return (VkDescriptorBufferInfo){gmem_buf, 0, gmem_size < max ? (gmem_size ? gmem_size : 256) : max};
    }
    extern uint64_t g_stdheap_used;  // (runtime/hle/heaps.c: the StandardAllocator block above the heap)
    uint64_t heap = kernel_heap_size(), std_end = g_stdheap_used ? STDHEAP_BASE + g_stdheap_used - HEAP_BASE : 0;
    uint64_t want = ((heap > std_end ? heap : std_end) + 0xFFFF) & ~0xFFFFull;
    if (want > HEAP_MAX) want = HEAP_MAX;
    if (!off && want > gmem_size && !gmem_import(want)) {
        LOG("[vk] guest heap import failed (%llu MB): shader global memory reads return zero\n", (unsigned long long)(want >> 20));
        off = 1;
    }
    if (gmem_buf) return (VkDescriptorBufferInfo){gmem_buf, 0, gmem_size};
    static uint32_t zero[1024];
    VkSpan s = vk_transient_copy(zero, sizeof zero, 256);
    return (VkDescriptorBufferInfo){s.buf, s.off, sizeof zero};
}

// Guest memory ranges the GPU may write: the storage buffers a compute dispatch binds (the game's compute shaders produce vertex
// data for some draws: a 16-byte stream here, a few more there). Vertex or index data inside one of them is read in place when
// the GPU runs, as the game expects, and is never copied when the draw is recorded (the copy would hold what the GPU has not
// written yet). Sorted, merged intervals; the render thread is the only user.
// Grows as needed. It held 512 ranges: once full, a new range below the last one was not recorded at all (only one
// past the end widened the last), so the VRAM vertex cache (vcache.c) kept a copy of memory the GPU rewrites every
// frame - posed characters' vertices drawn from a stale copy, exploding. A long session gets there (each posed
// character, Pokemon, cloth... its own compute outputs, and the list never shrinks); a save just loaded doesn't.
// LARECOMP_GPUW_CAP=<n>: the old fixed size, to compare.
static struct GpuwRange { uint64_t a, e; } *gpuw;
static int ngpuw, capgpuw;
static void gpuw_note(uint64_t a, uint64_t n) {
    uint64_t e = a + (n ? n : 16);
    int i = 0, j, hi = ngpuw;  // (sorted, disjoint: the first range ending at or after a, by halving)
    while (i < hi) {
        int mid = (i + hi) / 2;
        if (gpuw[mid].e < a) i = mid + 1; else hi = mid;
    }
    for (j = i; j < ngpuw && gpuw[j].a <= e; j++) {
        if (gpuw[j].a < a) a = gpuw[j].a;
        if (gpuw[j].e > e) e = gpuw[j].e;
    }
    if (j > i) {  // merged with [i, j)
        if (i + 1 == j && gpuw[i].a == a && gpuw[i].e == e) return;  // (known)
        gpuw[i].a = a; gpuw[i].e = e;
        memmove(&gpuw[i + 1], &gpuw[j], (size_t)(ngpuw - j) * sizeof gpuw[0]);
        ngpuw -= j - i - 1;
    } else {
        static int cap_env = -1;
        if (cap_env < 0) cap_env = ENV("LARECOMP_GPUW_CAP") ? atoi(ENV("LARECOMP_GPUW_CAP")) : 0;
        if (cap_env > 0 && ngpuw >= cap_env) {  // (the old behaviour, for comparison)
            static int nlost;
            if (gpuw[ngpuw - 1].e < e) gpuw[ngpuw - 1].e = e;
            else if (nlost++ < 20) LOG("[gpuw] full (%d): %llx-%llx not recorded\n", ngpuw, (unsigned long long)a, (unsigned long long)e);
            return;
        }
        if (ngpuw == capgpuw) {
            capgpuw = capgpuw ? capgpuw * 2 : 1024;
            gpuw = realloc(gpuw, (size_t)capgpuw * sizeof *gpuw);
        }
        memmove(&gpuw[i + 1], &gpuw[i], (size_t)(ngpuw - i) * sizeof gpuw[0]);
        gpuw[i].a = a; gpuw[i].e = e;
        ngpuw++;
        if ((ngpuw & (ngpuw - 1)) == 0 && ngpuw >= 128)  // (128, 256, 512...: how many a session gathers)
            LOG("[gpuw] %d ranges the GPU writes (frame %llu)\n", ngpuw, (unsigned long long)vk_frame_counter());
    }
    if (ENV("LARECOMP_LOG_GPUWRITES")) LOG("[gpuw] the GPU may write %llx-%llx (%d ranges)\n", (unsigned long long)a, (unsigned long long)e, ngpuw);
    vcache_invalidate(a, e - a);  // (a copy made before this dispatch was seen would stay stale: the CPU never writes there)
}
bool vk_gpu_may_write(uint64_t a);
bool vk_gpu_may_write(uint64_t a) {  // (research, hle/pscan.c; render thread)
    for (int i = 0; i < ngpuw; i++) if (a >= gpuw[i].a && a < gpuw[i].e) return true;
    return false;
}
// A shader that stores to global memory with no storage buffer bound writes through pointers we cannot see: from then on every
// vertex / index range is read in place, as before the VRAM ring (slower, never stale).
static bool gpuw_unknown;
static void gpuw_stores(const ShaderInfo *si, const Range *sbufs, int n, const char *what, uint64_t code) {
    if (!si || !si->gmem_stores) return;
    bool any = false;
    for (int i = 0; i < n; i++) if (sbufs[i].addr) { gpuw_note(sbufs[i].addr, sbufs[i].size); any = true; }
    if (!any && !gpuw_unknown) {
        gpuw_unknown = true;
        LOG("[vk] %s %llx stores to global memory without a storage buffer: vertex and index data is read in place from now on\n", what, (unsigned long long)code);
    }
}
static bool gpuw_overlaps(uint64_t a, uint64_t n) {
    uint64_t e = a + n;
    if (gpuw_unknown) return true;
    int lo = 0, hi = ngpuw;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (gpuw[mid].e <= a) lo = mid + 1; else hi = mid;
    }
    return lo < ngpuw && gpuw[lo].a < e;
}

// Vertex / index data read in place from the imported guest heap (renderer.m bind_guest does the same with
// g_guest_buffer): no per-draw copy, no index scan for the copy size. The game fences its CPU writes as on the
// Switch. Falls back to copies outside the heap or without the import; LARECOMP_VK_COPY_VERTICES=1 forces copies.
static bool guest_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    static int copy = -1;
    if (copy < 0) copy = ENV("LARECOMP_VK_COPY_VERTICES") != NULL;
    if (copy || addr < HEAP_BASE) return false;
    if (addr + size > HEAP_BASE + gmem_size && kernel_heap_size() != gmem_tried) { gmem_tried = kernel_heap_size(); gmem_info(); }
    if (!gmem_buf || addr + size > HEAP_BASE + gmem_size) return false;
    *buf = gmem_buf;
    *off = addr - HEAP_BASE;
    return true;
}
// Vertex / index range of a draw: in place from the game's memory (true) or to be copied (false). With the VRAM ring on, copied
// unless a compute dispatch may write it, or it is large: Legends Z-A's draws name whole buffers (MBs each, ~6 GB a second
// copied in its opening scene, 8 fps; read in place: 30 fps).
#define VTX_COPY_MAX (256u << 10)
volatile int g_heal_tex, g_heal_vtx;
uint64_t g_vcache_gpuw;  // (vertex streams the GPU may write: never in the VRAM cache)  // (LARECOMP_HEAL_KEYS research keys, runtime/hle/input.c)
static bool vtx_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    if (g_heal_vtx) gpuw_unknown = true;
    static uint64_t copy_max;
    if (!copy_max) copy_max = ENV("LARECOMP_VK_VTX_COPY_MAX") ? strtoull(ENV("LARECOMP_VK_VTX_COPY_MAX"), NULL, 0) : VTX_COPY_MAX;
    if (ring_gpu_enabled() && !gpuw_overlaps(addr, size) && (size <= copy_max || !guest_direct(addr, size, buf, off))) {
        stat_vtx_copied++;
        return false;
    }
    bool d = guest_direct(addr, size, buf, off);
    if (d) stat_vtx_direct++;
    return d;
}
bool vk_guest_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {  // (cbuf_info, above)
    return guest_direct(addr, size, buf, off);
}

static int g_cs_n;  // (compute program table below)

// In-place reboot (device.c backend_reset; GPU idle): drop what is keyed by guest addresses - programs by code
// address (the old Prog / Pipe objects are leaked, so pipeline keys holding their pointers can't collide), compute
// programs, shared copies - and the heap import: its pages are replaced (re-imported on first use).
void vk_exec_reset(void) {
    vcache_reset();
    memset(prog_tab, 0, sizeof prog_tab);
    memset(pipe_tab, 0, sizeof pipe_tab);
    g_pipe_memo = NULL;
    g_cs_n = 0;
    ngpuw = 0;  // (guest memory is replaced)
    gcopy_gen++;
    if (gmem_buf) {
        vkDestroyBuffer(g_vk.dev, gmem_buf, NULL);
        if (gmem_mem) vkFreeMemory(g_vk.dev, gmem_mem, NULL);
        gmem_buf = VK_NULL_HANDLE;
        gmem_mem = VK_NULL_HANDLE;
    }
    for (int i = 0; i < GMEM_MAX_CHUNKS; i++) {  // (gpuheap's chunks: imported again after the guest reset)
        if (gmem_chunk_buf[i]) vkDestroyBuffer(g_vk.dev, gmem_chunk_buf[i], NULL), gmem_chunk_buf[i] = VK_NULL_HANDLE;
        if (gmem_chunk[i]) vkFreeMemory(g_vk.dev, gmem_chunk[i], NULL), gmem_chunk[i] = VK_NULL_HANDLE;
    }
    gmem_size = gmem_tried = 0;
    st_defaults();
}

static void texture_info(int stage, ShaderInfo *si, int i, VkDescriptorImageInfo *out, float sc[2], uint32_t wh[2]) {
    ShaderTexture *st_ = &si->tex[i];
    int slot = ((int)st_->handle_offset - 0x20) / 8;  // 64-bit handles from offset 0x20
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler smp = VK_NULL_HANDLE;
    uint32_t tic = h & 0xFFFFF, tsc = (uint32_t)(h >> 20) & 0xFFF;
    sc[0] = sc[1] = 1;
    wh[0] = wh[1] = 0;
    if (h && st.tpool && tic < (uint32_t)st.tpool->count) {
        NvnTexEntry *e = &((NvnTexEntry *)st.tpool->entries)[tic];
        if (e->tex) {
            // Per-frame memo: ~2400 lookups per overworld frame recomputed the view (format / range math, cache
            // probe). The first lookup of a frame still runs vk_view (upload check); a view that may have been
            // destroyed (eviction, image re-created) bumps vk_view_gen. LARECOMP_VK_NO_TEX_MEMO=1 disables.
            extern uint32_t vk_view_gen;
            extern uint64_t vk_frame_counter(void);
            static int nomemo = -1;
            if (nomemo < 0) nomemo = ENV("LARECOMP_VK_NO_TEX_MEMO") != NULL;
            static struct { NvnTexture *tex; uint64_t vh, frame; uint32_t gen; uint8_t type, has_view; VkImageView view; } memo[1024];
            uint64_t vh = e->has_view ? hash_words(0, &e->view, sizeof e->view) : 0, frame = vk_frame_counter();
            uint32_t ms = (uint32_t)((((uintptr_t)e->tex >> 4) ^ vh ^ (uint64_t)st_->type * 0x9E3779B97F4A7C15ull) & 1023);
            if (!nomemo && memo[ms].tex == e->tex && memo[ms].vh == vh && memo[ms].frame == frame && memo[ms].gen == vk_view_gen &&
                memo[ms].type == st_->type && memo[ms].has_view == e->has_view) {
                view = memo[ms].view;
            } else {
                view = vk_view(e->tex, &e->view, e->has_view, false, st_->type);
                memo[ms].tex = e->tex; memo[ms].vh = vh; memo[ms].frame = frame; memo[ms].gen = vk_view_gen;
                memo[ms].type = (uint8_t)st_->type; memo[ms].has_view = e->has_view; memo[ms].view = view;
            }
            renderer_texture_scale(e->tex, &sc[0], &sc[1]);
            wh[0] = e->tex->width; wh[1] = e->tex->height;
            for (int k = 0; k < nfeedback; k++)
                if (e->tex == feedback_tex[k] && (st_->type == TEXTYPE_2D || st_->type == TEXTYPE_2D_ARRAY)) {
                    view = feedback_view[k];
                    if (st_->type == TEXTYPE_2D_ARRAY) view = vk_dummy_view(TEXTYPE_2D_ARRAY);  // (2D snapshot as array: rare)
                }
        }
    }
    if (h && st.spool && tsc < (uint32_t)st.spool->count) smp = vk_sampler_for(&((NvnSamplerDesc *)st.spool->entries)[tsc], st_->shadow);
    static int cs_texlog = -1;  // (research, LARECOMP_CS_TEXLOG=1: compute shaders' mipmapped textures, as bound)
    if (cs_texlog < 0) cs_texlog = ENV("LARECOMP_CS_TEXLOG") != NULL;
    if (cs_texlog && stage == 5 && h && st.tpool && tic < (uint32_t)st.tpool->count) {
        NvnTexEntry *e = &((NvnTexEntry *)st.tpool->entries)[tic];
        static int nlog;
        if (e->tex && e->tex->levels > 1 && nlog < 60) {
            nlog++;
            NvnSamplerDesc *sd = st.spool && tsc < (uint32_t)st.spool->count ? &((NvnSamplerDesc *)st.spool->entries)[tsc] : NULL;
            LOG("[cstex] cs %llx tex %ux%u levels %u fmt %#x view %d (levels %u+%u) sampler min %u mag %u lod %.2f..%.2f bias %.2f reduction %u\n",
                (unsigned long long)(st.program ? st.program->stages[5].code_addr : 0), e->tex->width, e->tex->height, e->tex->levels, e->tex->format,
                e->has_view, e->has_view ? e->view.base_level : 0, e->has_view ? e->view.num_levels : 0, sd ? sd->min_filter : 99, sd ? sd->mag_filter : 99,
                sd ? (double)sd->lod_min : -1.0, sd ? (double)sd->lod_max : -1.0, sd ? (double)sd->lod_bias : 0.0, sd ? sd->reduction : 99);
        }
    }
    static int dump_sampled = -1;  // LARECOMP_DUMP_SAMPLED=1 (with the frame dump): depth / R8UI / G-buffer textures as sampled
    if (dump_sampled < 0) dump_sampled = ENV("LARECOMP_DUMP_SAMPLED") != NULL;  // (its readbacks split the frame's passes)
    if (dump_sampled && g_debug_frame && h && st.tpool && tic < (uint32_t)st.tpool->count) {
        NvnTexture *dt = ((NvnTexEntry *)st.tpool->entries)[tic].tex;
        static uint32_t done[64]; static int ndone;
        bool seen = false;
        for (int k = 0; k < ndone; k++) seen |= done[k] == tic;
        if (dt && dt->backend && (renderer_format_is_depth(dt->format) || dt->format == 0x3) && !seen && ndone < 64) {
            extern void vk_debug_save_depth_layers(TexBackend *b, const char *base);
            extern void vk_debug_save_r8_layers(TexBackend *b, const char *base);
            char base[64];
            done[ndone++] = tic;
            snprintf(base, sizeof base, "build/sampled_tex%u", tic);
            if (dt->format == 0x3) vk_debug_save_r8_layers(dt->backend, base);
            else if (((TexBackend *)dt->backend)->levels > 1) {
                extern void vk_debug_save_depth_mips(TexBackend *b, const char *base);
                vk_debug_save_depth_mips(dt->backend, base);
            } else vk_debug_save_depth_layers(dt->backend, base);
        } else if (dt && dt->backend && !seen && ndone < 64 && dt->levels > 1 && ((TexBackend *)dt->backend)->format == VK_FORMAT_R32_SFLOAT) {
            extern void vk_debug_save_r32f_mips(TexBackend *b, const char *base);
            char base[64];  // (a float pyramid: Z-A's depth pyramid for its prop culling)
            done[ndone++] = tic;
            snprintf(base, sizeof base, "build/sampled_tex%u_r32f", tic);
            vk_debug_save_r32f_mips(dt->backend, base);
        } else if (dt && dt->backend && !seen && ndone < 64 && dt->width >= 900 && (dt->format == 0x25 || dt->format == 0x38 || dt->format == 0x3f)) {
            char path[64];  // (the G-buffer as sampled)
            done[ndone++] = tic;
            snprintf(path, sizeof path, "build/sampled_tex%u_f%x.png", tic, dt->format);
            renderer_save_texture_tex(dt->backend, path);
        } else if (dt && dt->backend && !seen && ndone < 64 && ENV("LARECOMP_DUMP_SAMPLED_FMT") &&
                   dt->format == strtoul(ENV("LARECOMP_DUMP_SAMPLED_FMT"), NULL, 16)) {
            char path[64];  // (LARECOMP_DUMP_SAMPLED_FMT=<hex NVN format>: textures of that format as sampled, level 0)
            done[ndone++] = tic;
            snprintf(path, sizeof path, "build/sampled_tex%u_f%x.png", tic, dt->format);
            renderer_save_texture_tex(dt->backend, path);
            LOG("[vk] %s: %ux%u addr %llx\n", path, dt->width, dt->height, (unsigned long long)dt->addr);
        }
    }
    if (g_debug_frame && h && st.spool && tsc < (uint32_t)st.spool->count) {
        const NvnSamplerDesc *sd = &((NvnSamplerDesc *)st.spool->entries)[tsc];
        LOG("[draw]   sampler %u (tex %u): min %u mag %u wrap %u/%u lod %.2f..%.2f bias %.2f aniso %.1f\n", tsc, tic, sd->min_filter,
            sd->mag_filter, sd->wrap[0], sd->wrap[1], sd->lod_min, sd->lod_max, sd->lod_bias, sd->max_aniso);
    }
    if (!view) view = vk_dummy_view(st_->type);
    if (!smp) smp = vk_default_sampler(true);
    *out = (VkDescriptorImageInfo){smp, view, VK_IMAGE_LAYOUT_GENERAL};
}

// ---- nvnCommandBufferDrawTexture: the texture's src rectangle (texels) onto the dst rectangle (render target pixels)
// of colour target 0, with the bound blend state and channel mask; no game shaders, no depth test. Pipelines are kept
// per pass formats + blend state (like the clear pipelines).
static VkPipelineLayout drawtex_layout;
static VkDescriptorSetLayout drawtex_set_layout;
static PipeEnt drawtex_tab[256];

static VkPipeline drawtex_pipeline(uint32_t write_mask, bool blend, const uint8_t *bf) {
    uint64_t key = 0xcbf29ce484222325ull;
    MIX(write_mask); MIX(blend); MIX(pass_ncolor); MIX(pass_depth); MIX(pass_stencil); MIX(pass_samples);
    if (blend) for (int i = 1; i < 7; i++) MIX(bf[i]);
    for (uint32_t i = 0; i < pass_ncolor; i++) MIX(pass_color[i]);
    if (!key) key = 1;
    uint32_t slot = (uint32_t)key & 255;
    for (int p = 0; p < 256; p++, slot = (slot + 1) & 255) {
        if (drawtex_tab[slot].key == key) return drawtex_tab[slot].pipe;
        if (!drawtex_tab[slot].key) break;
    }
    if (!drawtex_layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &drawtex_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &drawtex_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &drawtex_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("drawtex.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader("drawtex.frag"), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)pass_samples};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    cba[0].colorWriteMask = write_mask & 0xF;
    if (blend) {
        cba[0].blendEnable = VK_TRUE;
        cba[0].srcColorBlendFactor = blend_factor(bf[1]);
        cba[0].dstColorBlendFactor = blend_factor(bf[2]);
        cba[0].srcAlphaBlendFactor = blend_factor(bf[3]);
        cba[0].dstAlphaBlendFactor = blend_factor(bf[4]);
        cba[0].colorBlendOp = blend_op(bf[5]);
        cba[0].alphaBlendOp = blend_op(bf[6]);
    }
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = pass_ncolor, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkPipelineRenderingCreateInfo rinfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = pass_ncolor,
                                           .pColorAttachmentFormats = pass_color, .depthAttachmentFormat = pass_depth,
                                           .stencilAttachmentFormat = pass_stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rinfo, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = drawtex_layout};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p) != VK_SUCCESS) LOG("[vk] draw texture pipeline failed\n");
    drawtex_tab[slot].key = key;
    drawtex_tab[slot].pipe = p;
    return p;
}

static void draw_texture(CmdRec *r) {
    g_dyn_epoch++;  // (draw_'s state cache: dc)
    const float *dst = (const float *)r->extra, *src = dst + 4;
    uint64_t h = r->x[0];
    uint32_t tic = h & 0xFFFFF, tsc = (uint32_t)(h >> 20) & 0xFFF;
    NvnTexture *rt = st.rt.ncolor ? st.rt.color[0] : NULL;
    if (!rt || !h || !st.tpool || tic >= (uint32_t)st.tpool->count) return;
    NvnTexEntry *e = &((NvnTexEntry *)st.tpool->entries)[tic];
    if (!e->tex || !e->tex->width || !e->tex->height) return;
    begin_pass();
    if (!pass_ncolor || pass_color[0] == VK_FORMAT_UNDEFINED || rt_kind(rt->format)) return;
    VkImageView view = vk_view(e->tex, &e->view, e->has_view, false, TEXTYPE_2D);
    if (!view) return;
    VkSampler smp = st.spool && tsc < (uint32_t)st.spool->count ? vk_sampler(&((NvnSamplerDesc *)st.spool->entries)[tsc])
                                                                : vk_default_sampler(true);
    bool blend = (st.color_state[0] & 1) != 0;
    VkPipeline p = drawtex_pipeline(st.channel_mask & 0xF, blend, st.blend[0]);
    if (!p) return;
    VkCommandBuffer cb = vk_cmd();
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    VkRect2D rect = scissor_rect();
    if (!rect.extent.width || !rect.extent.height) return;
    VkDescriptorSet ds = vk_alloc_set(drawtex_set_layout);
    VkDescriptorImageInfo ii = {smp, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, drawtex_layout, 0, 1, &ds, 0, NULL);
    dyn_defaults(cb);
    int pw, ph;
    pass_size(&pw, &ph);
    VkViewport vp = {0, 0, (float)(pw ? pw : (int)pass_w), (float)(ph ? ph : (int)pass_h), 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &rect);
    // dst: render target pixels of the guest's size (the pass may be scaled) -> NDC (Vulkan y down; NVN's lower-left
    // window origin counts y up); src: texels -> uv
    float gw = (float)rt->width, gh = (float)rt->height, tw = (float)e->tex->width, th = (float)e->tex->height;
    bool up = nvn_window_origin() == 0;
    float pc[8] = {dst[0] / gw * 2 - 1, up ? 1 - dst[1] / gh * 2 : dst[1] / gh * 2 - 1,
                   dst[2] / gw * 2 - 1, up ? 1 - dst[3] / gh * 2 : dst[3] / gh * 2 - 1,
                   src[0] / tw, src[1] / th, src[2] / tw, src[3] / th};
    vkCmdPushConstants(cb, drawtex_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pc);
    vkCmdDraw(cb, 6, 1, 0, 0);
    if (g_debug_frame)
        LOG("[drawtex] tex %ux%u f%#x (%.1f,%.1f)-(%.1f,%.1f) -> rt %ux%u (%.1f,%.1f)-(%.1f,%.1f) blend %d\n", e->tex->width,
            e->tex->height, e->tex->format, src[0], src[1], src[2], src[3], rt->width, rt->height, dst[0], dst[1], dst[2], dst[3], blend);
}

// Image i of a stage's shader: the NVN image bound at its slot (a texture pool entry registered with RegisterImage), as a
// storage view; a 1x1 dummy when nothing usable is bound (the format must suit the declaration: r32ui for atomics).
static void image_info(int stage, ShaderInfo *si, int i, VkDescriptorImageInfo *out) {
    const ShaderImage *im = &si->img[i];
    uint64_t h = stage < 6 && im->slot < 8 ? st.img[stage][im->slot] : 0;
    uint32_t id = h & 0xFFFFF;
    VkImageView view = VK_NULL_HANDLE;
    if (h && st.tpool && id < (uint32_t)st.tpool->count) {
        NvnTexEntry *e = &((NvnTexEntry *)st.tpool->entries)[id];
        if (e->tex) {
            view = vk_storage_view(e->tex, &e->view, e->has_view, im->dim);
            TexBackend *b = e->tex->backend;
            if (view && im->atomic && !(b && b->format == VK_FORMAT_R32_UINT)) view = VK_NULL_HANDLE;  // (r32ui declared)
            if (view && !im->atomic && rt_kind(e->tex->format) != im->kind) view = VK_NULL_HANDLE;    // (another type)
            static int nlog;
            if (!view && nlog++ < 20)
                LOG("[vk] image slot %d (stage %d): texture fmt %#x flags %#x not usable as a%s storage image\n", im->slot, stage,
                    e->tex->format, e->tex->flags, im->atomic ? "n r32ui" : "");
        }
    }
    if (!view) view = vk_dummy_storage_view(im->dim, im->atomic);
    else g_storage_written = true;
    *out = (VkDescriptorImageInfo){VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
}

// Writes the descriptor set of one stage. gx..gz: compute group counts (driver constants).
// pre_ii: texture descriptors already resolved by the caller (draw: fragment textures, needed earlier for the
// viewport decision), or NULL.
static VkDescriptorSet stage_set(Prog *p, VkDescriptorSetLayout layout, uint32_t set, int stage, ShaderInfo *si, const float *rscale_row0,
                                 const float tint[8], uint32_t gx, uint32_t gy, uint32_t gz, const VkDescriptorImageInfo *pre_ii) {
    VkDescriptorSet ds = VK_NULL_HANDLE;  // (allocated below, once the writes are known)
    VkWriteDescriptorSet w[MAX_BIND];
    VkDescriptorBufferInfo bi[MAX_BIND];
    VkDescriptorImageInfo ii[MAX_BIND];
    int n = 0;
    int sg = stage == 1 ? 1 : 0;
    for (int i = 0; i < p->nbinds; i++) {
        VkBindingInfo *b = &p->binds[i];
        if (b->set != set) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b->binding >= 32) {
            int ti = (int)b->binding - 32;
            float sc[2]; uint32_t wh[2];
            if (ti < si->ntex && pre_ii && ti < 64) { ii[n] = pre_ii[ti]; sc[0] = tex_sc[sg][ti][0]; sc[1] = tex_sc[sg][ti][1]; wh[0] = tex_wh[sg][ti][0]; wh[1] = tex_wh[sg][ti][1]; }
            else if (ti < si->ntex) texture_info(stage, si, ti, &ii[n], sc, wh);
            else { ii[n] = (VkDescriptorImageInfo){vk_default_sampler(true), vk_dummy_view(TEXTYPE_2D), VK_IMAGE_LAYOUT_GENERAL}; sc[0] = sc[1] = 1; wh[0] = wh[1] = 0; }
            if (ti < 64) {
                tex_sc[sg][ti][0] = sc[0]; tex_sc[sg][ti][1] = sc[1];
                tex_wh[sg][ti][0] = wh[0]; tex_wh[sg][ti][1] = wh[1];
            }
            w[n].pImageInfo = &ii[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && b->binding >= 64 && (int)b->binding - 64 < si->nimg) {
            image_info(stage, si, (int)b->binding - 64, &ii[n]);
            w[n].pImageInfo = &ii[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->binding < 17) {
            bi[n] = cbuf_info(stage, si, (int)b->binding, gx, gy, gz);
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->binding == 17) {  // (GMEM_BUFFER_INDEX: the heap's address)
            bi[n] = gmem_ref_info();
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            bi[n] = gmem_info();
            w[n].pBufferInfo = &bi[n];
        } else {
            continue;  // rscale / tint: written below, once the textures (and their scales) are known
        }
        n++;
    }
    for (int i = 0; i < p->nbinds; i++) {
        VkBindingInfo *b = &p->binds[i];
        if (b->set != set || b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || (b->binding != 18 && b->binding != 19)) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->binding == 18) {
            // (the block: row 0, then a texture scale per row - the same as the last one of this submission for most
            // draws: compared by those, not as the whole 1 KB block built and compared per draw)
            static struct { uint64_t seq; float row0[4], sc[64][2]; int nt; VkSpan span; } m[2];
            static const float one[4] = {1, 1, 0, 0};
            const float *row0 = rscale_row0 ? rscale_row0 : one;
            int nt = si->ntex < 64 ? si->ntex : 64;
            uint64_t seq = vk_pending_seq();
            if (!(m[sg].seq == seq && m[sg].nt == nt && !memcmp(m[sg].row0, row0, 16) && !memcmp(m[sg].sc, tex_sc[sg], (size_t)nt * 8))) {
                float rs[65][4];
                memset(rs, 0, sizeof rs);
                memcpy(rs[0], row0, 16);
                for (int t = 0; t < nt; t++) rs[1 + t][0] = tex_sc[sg][t][0], rs[1 + t][1] = tex_sc[sg][t][1];
                m[sg].span = vk_transient_copy(rs, sizeof rs, 256);
                m[sg].seq = seq, m[sg].nt = nt;
                memcpy(m[sg].row0, row0, 16), memcpy(m[sg].sc, tex_sc[sg], (size_t)nt * 8);
            }
            bi[n] = ubo_span(m[sg].span, sizeof(float[65][4]));
        } else {
            float zero[8] = {0};
            bi[n] = ubo_span(copy_dedupe(DEDUPE_TINT, tint ? tint : zero, 32, 256), 32);
        }
        w[n].pBufferInfo = &bi[n];
        n++;
    }
    uint64_t pa_ = prof ? SDL_GetTicksNS() : 0;
    ds = vk_alloc_set(layout);
    if (prof) prof_dalloc += SDL_GetTicksNS() - pa_;
    for (int i = 0; i < n; i++) w[i].dstSet = ds;
    uint64_t pu_ = prof ? SDL_GetTicksNS() : 0;
    if (n) vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    if (prof) prof_dupd += SDL_GetTicksNS() - pu_;
    return ds;
}

// ================================================================== compute
typedef struct { Prog p; ShaderInfo *si; VkPipeline pipe; uint32_t local[3]; uint64_t addr; } CsProg;

// Draw distance (Z-A): the game culls its GPU-instanced props (station arches and walls, trees, lamp posts, flower
// beds...) by their distance to the camera in three compute shaders, each testing max(0, distance - radius) against a
// level-of-detail band; past the last band a prop isn't drawn. LARECOMP_DRAW_DISTANCE=<factor> divides that distance
// in them (2: props show up and keep their detail twice as far). Found by their instructions (the pc and code words the
// translator writes beside each line), not by address: the shaders load wherever the game puts them.
static void draw_distance_patch(CsProg *cp) {
    static float factor = -1;
    if (factor < 0) {
        const char *e = ENV("LARECOMP_DRAW_DISTANCE");
        factor = e ? (float)atof(e) : 1.0f;
        if (!(factor >= 0.25f && factor <= 16.0f)) factor = 1.0f;
    }
    if (factor == 1.0f || !cp->si->msl || !cp->addr) return;
    // (pc, its instruction word in the shader's code; the GLSL line it became, and what must follow it)
    static const struct { uint32_t pc; uint64_t word; const char *line, *next, *repl; } k[] = {
        {0xa68, 0x5c6017800047ff05ull, "    r5 = as_u((false ? fmin(as_f(0u), as_f(r4)) : fmax(as_f(0u), as_f(r4))));\n",
         "bool cmp = (as_f(r5) >= as_f(r14));", "    r5 = as_u(fmax(0.0, as_f(r4)) * %.6f);  // (draw distance)\n"},
        {0x990, 0x5c6017800057ff05ull, "    r5 = as_u((false ? fmin(as_f(0u), as_f(r5)) : fmax(as_f(0u), as_f(r5))));\n",
         "bool cmp = (as_f(r5) >= as_f(r10));", "    r5 = as_u(fmax(0.0, as_f(r5)) * %.6f);  // (draw distance)\n"},
        {0x17b0, 0x5080000000870c0cull, "    r12 = as_u(sqrt(as_f(r12)));\n",
         "r30 = as_u(((-as_f(r30)) + as_f(r12)));", "    r12 = as_u(sqrt(as_f(r12)) * %.6f);  // (draw distance)\n"},
    };
    for (size_t i = 0; i < sizeof k / sizeof *k; i++) {
        uint64_t w;
        memcpy(&w, (const void *)(uintptr_t)(cp->addr + 0x100 + k[i].pc), 8);  // (pc: past the program's 0x100-byte header)
        if (w != k[i].word) continue;
        const char *src = cp->si->msl, *at = src;
        size_t ll = strlen(k[i].line);
        while ((at = strstr(at, k[i].line))) {  // (the occurrence followed, within a few lines, by its use)
            const char *nx = strstr(at + ll, k[i].next);
            if (nx && nx - (at + ll) < 160) break;
            at += ll;
        }
        if (!at) continue;
        char repl[160];
        snprintf(repl, sizeof repl, k[i].repl, (double)(1.0f / factor));
        size_t head = (size_t)(at - src), rl = strlen(repl), tail = strlen(at + ll);
        char *out = malloc(head + rl + tail + 1);
        if (!out) return;
        memcpy(out, src, head), memcpy(out + head, repl, rl), memcpy(out + head + rl, at + ll, tail + 1);
        cp->si->msl = out;
        LOG("[vk] draw distance x%.2f in the prop culling shader %llx\n", (double)factor, (unsigned long long)cp->addr);
        return;
    }
}

static void build_compute(void *arg) {
    CsProg *cp = arg;
    size_t n = 0;
    if (ENV("LARECOMP_GLSL_OVERRIDE_DIR")) {  // (research: <dir>/<code address>_cs.glsl replaces a compute program's GLSL)
        char path[512];
        snprintf(path, sizeof path, "%s/%llx_cs.glsl", ENV("LARECOMP_GLSL_OVERRIDE_DIR"), (unsigned long long)cp->addr);
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long fl = ftell(f);
            fseek(f, 0, SEEK_SET);
            char *src = malloc((size_t)fl + 1);
            if (src && fread(src, 1, (size_t)fl, f) == (size_t)fl) { src[fl] = 0; cp->si->msl = src; LOG("[exp] GLSL override: %s\n", path); }
            fclose(f);
        }
    }
    draw_distance_patch(cp);
    if (ENV("LARECOMP_ZA_NO_HIZ")) {  // (research: the culling's HiZ sample reads the far plane - no occlusion culling)
        uint64_t w;
        static const char line[] = "    { vec4 t = textureLod(t0, vec2(as_f(r4), as_f(r5)), as_f(r19));\n";
        memcpy(&w, (const void *)(uintptr_t)(cp->addr + 0x100 + 0xfb8), 8);
        char *at = w == 0xd86002aff1370404ull && cp->si->msl ? strstr(cp->si->msl, line) : NULL;
        if (at) {
            static const char repl[] = "    { vec4 t = vec4(0.0);  // (research: no HiZ)\n";
            size_t head = (size_t)(at - cp->si->msl), tail = strlen(at + sizeof line - 1);
            char *out = malloc(head + sizeof repl + tail);
            memcpy(out, cp->si->msl, head), memcpy(out + head, repl, sizeof repl - 1), memcpy(out + head + sizeof repl - 1, at + sizeof line - 1, tail + 1);
            cp->si->msl = out;
            LOG("[exp] no HiZ in the prop culling shader %llx\n", (unsigned long long)cp->addr);
        }
    }
    uint32_t *spv = vk_glsl_to_spirv(cp->si->msl, STAGE_COMPUTE, "cs", &n);
    if (!spv) { atomic_store(&cp->p.state, 2); return; }
    cp->p.nbinds = vk_reflect(spv, n, cp->p.binds, MAX_BIND);
    VkShaderModule m = vk_module(spv, n);
    free(spv);
    cp->p.layout = make_layout(&cp->p, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0);
    VkSpecializationMapEntry me[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
    VkSpecializationInfo spec = {3, me, 12, cp->local};
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .pNext = vk_sg32(VK_SHADER_STAGE_COMPUTE_BIT),
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main",
                                                .pSpecializationInfo = &spec},
                                      .layout = cp->p.layout};
    VkResult r = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &cp->pipe);
    atomic_fetch_add(&stat_compiles, 1);
    if (r == VK_SUCCESS) atomic_fetch_add(&new_pipes, 1);
    atomic_store(&cp->p.state, r == VK_SUCCESS ? 1 : 2);
    if (r != VK_SUCCESS) LOG("[compute] pipeline failed: %d\n", r);
}

#define CS_MAX 2048
static struct CsEnt { uint64_t addr, content; CsProg *cp; } g_cs_tab[CS_MAX];  // compute programs by code address and content

static CsProg *compute_program(NvnProgram *np) {
    NvnShaderStage *ss = &np->stages[5];
    if (!ss->code_addr) return NULL;
    struct CsEnt *tab = g_cs_tab;
    int n = g_cs_n;
    for (int i = 0; i < n; i++) if (tab[i].addr == ss->code_addr && tab[i].content == ss->content_hash) return tab[i].cp;
    CsProg *cp = calloc(1, sizeof *cp);
    cp->addr = ss->code_addr;
    if (n < CS_MAX) { tab[n].addr = ss->code_addr; tab[n].content = ss->content_hash; tab[n].cp = cp; g_cs_n = n + 1; }
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    ShaderKey k = {0};
    k.glsl = 1;
    bool kc = ss->const_offset && ss->const_size <= 0x10000;
    cp->si = maxwell_translate_consts((const uint8_t *)(uintptr_t)ss->code_addr, ss->const_offset ? ss->const_offset : 0x10000,
                                      kc ? (const uint8_t *)(uintptr_t)(ss->code_addr + ss->const_offset) : NULL, kc ? ss->const_size : 0,
                                      STAGE_COMPUTE, &k);
    cp->p.vi = cp->si;
    if (!cp->si->msl) {
        LOG("[compute] translate failed: %s (code %llx)\n", cp->si->error, (unsigned long long)ss->code_addr);
        atomic_store(&cp->p.state, 2);
        return cp;
    }
    for (int i = 0; i < 3; i++) {
        cp->local[i] = ss->control ? *(const uint32_t *)(ss->control + 0x748 + 4 * i) : 1;
        if (!cp->local[i]) cp->local[i] = 1;
    }
    save_glsl(cp->si->msl, ss->code_addr, "cs");
    // Built now, not on a worker: a dispatch skipped while its pipeline compiles loses work whose results persist
    // (counters, simulation state, buffers written once), unlike a draw missing from one frame. A first-use hitch instead.
    build_compute(cp);
    return cp;
}

static uint64_t cs_logn;  // (LARECOMP_LOG_CS: dispatches of that shader so far)
static void dispatch_compute(uint32_t gx, uint32_t gy, uint32_t gz) {
    g_dyn_epoch++;  // (draw_'s state cache: dc)
    snap_reason = "compute"; snapshots_invalidate(NULL);
    if (!st.program || !gx || !gy || !gz) return;
    CsProg *cp = compute_program(st.program);
    if (!cp || atomic_load(&cp->p.state) != 1) { stat_skipped++; return; }
    {   // (research: LARECOMP_LIVE <dir>/skipcs "<addr>[,<addr>...]": those compute shaders not dispatched; "0" ends it)
        extern volatile uint64_t g_skip_cs[8];
        for (int k = 0; k < 8 && g_skip_cs[k]; k++)
            if (g_skip_cs[k] == st.program->stages[5].code_addr) return;
    }
    if (g_debug_frame) {
        char line[600];
        int len = snprintf(line, sizeof line, "[dispatch] cs %llx groups %ux%ux%u local %ux%ux%u", (unsigned long long)st.program->stages[5].code_addr,
                           gx, gy, gz, cp->local[0], cp->local[1], cp->local[2]);
        for (int k = 0; k < 16 && len < 520; k++)
            if (st.sbuf[5][k].addr) len += snprintf(line + len, sizeof line - (size_t)len, " s%d=%llx+%llx", k, (unsigned long long)st.sbuf[5][k].addr,
                                                    (unsigned long long)st.sbuf[5][k].size);
        LOG("%s\n", line);
    }
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    gp_mark(cb, "dispatch %ux%ux%u (local %ux%ux%u) cs %llx sbuf0 %llx+%llx", gx, gy, gz, cp->local[0], cp->local[1], cp->local[2],
            (unsigned long long)st.program->stages[5].code_addr, (unsigned long long)st.sbuf[5][0].addr, (unsigned long long)st.sbuf[5][0].size);
    if (ENV("LARECOMP_LOG_CS") && strtoull(ENV("LARECOMP_LOG_CS"), NULL, 16) == st.program->stages[5].code_addr && (cs_logn++ % (ENV("LARECOMP_LOG_CS_EVERY") ? atoi(ENV("LARECOMP_LOG_CS_EVERY")) : 600)) == 0) {
        // (research: a compute shader's storage buffers, every LARECOMP_LOG_CS_EVERY (600) of its dispatches)
        const uint32_t *a = (const uint32_t *)(uintptr_t)st.sbuf[5][0].addr, *b = (const uint32_t *)(uintptr_t)st.sbuf[5][1].addr;
        LOG("[cs] frame %llu sbuf0 %llx+%llx: %u %u %u %u | sbuf1 %llx+%llx", (unsigned long long)vk_frame_counter(),
            (unsigned long long)st.sbuf[5][0].addr, (unsigned long long)st.sbuf[5][0].size, a ? a[0] : 0, a ? a[1] : 0, a ? a[2] : 0, a ? a[3] : 0,
            (unsigned long long)st.sbuf[5][1].addr, (unsigned long long)st.sbuf[5][1].size);
        for (uint32_t i = 0; a && b && i < 6 && i < a[0]; i++) {
            uint32_t idx = a[8 + i * 8];
            const uint32_t *r = b + idx * 32;
            LOG(" | e%u idx %u w1 %x n %u %u %u %u", i, idx, r[1], r[24], r[25], r[26], r[27]);
        }
        for (int k = 2; k < 6; k++)
            if (st.sbuf[5][k].addr) LOG(" | sbuf%d %llx+%llx: %u %u", k, (unsigned long long)st.sbuf[5][k].addr, (unsigned long long)st.sbuf[5][k].size,
                                        ((const uint32_t *)(uintptr_t)st.sbuf[5][k].addr)[0], ((const uint32_t *)(uintptr_t)st.sbuf[5][k].addr)[1]);
        LOG("\n");
    }
    dl_note("dispatch %ux%ux%u cs %llx gmem %d", gx, gy, gz, (unsigned long long)st.program->stages[5].code_addr, cp->si->uses_gmem);
    gpuw_stores(cp->si, st.sbuf[5], 16, "compute shader", st.program->stages[5].code_addr);
    for (int i = 0; i < 16; i++) if (st.sbuf[5][i].addr) gpuw_note(st.sbuf[5][i].addr, st.sbuf[5][i].size);  // (any storage buffer it has bound)
    nfeedback = 0;
    VkDescriptorSet ds = stage_set(&cp->p, cp->p.set_layout[0], 0, 5, cp->si, NULL, NULL, gx, gy, gz, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->p.layout, 0, 1, &ds, 0, NULL);
    if (g_dispatch_indirect) {
        // The group counts are the GPU's (written by an earlier pass): from the argument words to the dispatch, and into
        // the driver constants the shader reads them from (c0[0x200..0x208], cbuf_info)
        VkDescriptorBufferInfo gm = gmem_info();
        uint64_t off = g_dispatch_indirect - HEAP_BASE;
        if (!gm.buffer || gm.buffer != gmem_buf || g_dispatch_indirect < HEAP_BASE || off + 12 > gmem_size) {  // (gmem_size: the range may be capped)
            static int warned;
            if (!warned++) LOG("[vk] indirect dispatch: arguments at %llx outside the heap import: skipped\n", (unsigned long long)g_dispatch_indirect);
            return;
        }
        if (g_dispatch_drv.buf) {
            VkBufferCopy bc = {.srcOffset = off, .dstOffset = g_dispatch_drv.off + 0x200, .size = 12};
            vkCmdCopyBuffer(cb, gm.buffer, g_dispatch_drv.buf, 1, &bc);
            VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                  .dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT};
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        }
        vkCmdDispatchIndirect(cb, gm.buffer, off);
    } else {
        vkCmdDispatch(cb, gx, gy, gz);
    }
    stat_dispatches++;
}

// ================================================================== draws
static void debug_draw(Prog *pr, CmdRec *r, bool indexed);
static void meshlog(Prog *pr, CmdRec *r, bool indexed);
static void skinprobe(Prog *pr, CmdRec *r);

// Quads / fans / polygons as a 32-bit triangle index list.
static uint32_t *expand_indices(uint32_t mode, uint32_t count, const void *indices, int itype, uint32_t first, uint32_t *out_count) {
    uint32_t tris = mode == 7 ? count / 4 * 2 : count >= 3 ? count - 2 : 0;
    uint32_t *out = malloc(tris * 3 * 4 + 4);
    uint32_t n = 0;
#define IDX(i) (indices ? (itype == 0 ? ((const uint8_t *)indices)[i] : itype == 1 ? ((const uint16_t *)indices)[i] : ((const uint32_t *)indices)[i]) : first + (i))
    if (mode == 7) {
        for (uint32_t q = 0; q + 3 < count; q += 4) {
            out[n++] = IDX(q); out[n++] = IDX(q + 1); out[n++] = IDX(q + 2);
            out[n++] = IDX(q); out[n++] = IDX(q + 2); out[n++] = IDX(q + 3);
        }
    } else {
        for (uint32_t i = 1; i + 1 < count; i++) { out[n++] = IDX(0); out[n++] = IDX(i); out[n++] = IDX(i + 1); }
    }
#undef IDX
    *out_count = n;
    return out;
}

static bool topology(uint32_t mode, VkPrimitiveTopology *t, uint8_t *cls) {
    switch (mode) {
    case 0: *t = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; *cls = 0; return true;
    case 1: *t = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; *cls = 1; return true;
    case 3: *t = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; *cls = 1; return true;
    case 4: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return true;
    case 5: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; *cls = 2; return true;
    default: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return false;
    }
}

// Dynamic state draw_ set last, to skip the calls that would set it again (the driver's share of the render thread was
// a third, mostly these). Valid for one command buffer while g_dyn_epoch holds: everything else that sets graphics
// state (clears, blits, post passes, the present, a new command buffer) bumps it.
static struct {
    VkCommandBuffer cb;
    uint32_t epoch;
    VkPipeline pipe;
    int64_t rdisc, dtest, dwrite, dcomp, sten, cull, front, topo, prim_restart, dbias_on;
    float dbias[3], blend[4];
    int64_t sop[2][4], smask[2][3];
    VkBuffer vb[17], ib;  // (the vertex / index buffers bound)
    VkDeviceSize vboff[17], iboff;
    int64_t itype;
} dc;
#define DC_SET(field, val, call) do { int64_t v_ = (int64_t)(val); if (dc.field != v_) { dc.field = v_; call; } } while (0)
static void dc_bind_vb(VkCommandBuffer cb, uint32_t s, VkBuffer b, VkDeviceSize off) {
    if (dc.vb[s] == b && dc.vboff[s] == off) return;
    dc.vb[s] = b, dc.vboff[s] = off;
    vkCmdBindVertexBuffers(cb, s, 1, &b, &off);
}
static void dc_begin(VkCommandBuffer cb) {
    if (dc.cb == cb && dc.epoch == g_dyn_epoch && !ENV("LARECOMP_NO_DC")) return;  // (LARECOMP_NO_DC: research, no cache)
    memset(&dc, 0xff, sizeof dc);  // (all unknown: -1, NaN)
    memset(&dc_vp, 0xff, sizeof dc_vp), memset(&dc_sc, 0xff, sizeof dc_sc);
    dc.cb = cb, dc.epoch = g_dyn_epoch, dc.pipe = VK_NULL_HANDLE;
}
// Indirect draws (nvnCommandBufferDraw{Elements,Arrays}Indirect): the arguments are in guest memory, written by the
// game's GPU culling (Z-A's GpuInstancingCulling* passes: props, foliage, the station facade...) in the same submission,
// so they are read by the GPU (vkCmdDraw*Indirect from the heap import) and the index / vertex data is bound in place
// whole. NVN's argument layouts are Vulkan's (VkDrawIndexedIndirectCommand / VkDrawIndirectCommand).
static uint64_t g_draw_indirect;  // (draw_: nonzero while an indirect draw runs - its argument address)
static void draw_(CmdRec *r, bool indexed);
static void draw(CmdRec *r, bool indexed) {
    PROF_T0();
    draw_(r, indexed);
    PROF_ADD(prof_draw);
}
// A draw whose program or pipeline is still being built waits for it, up to a budget per frame, instead of being
// skipped (objects popping in on entering an area). At 30 fps a frame has ~27 ms to spare (uncapped, the overworld
// runs at ~160 fps), so 20 ms per frame hid the pop-in with a warm cache (~1 ms per program). Legends Z-A brings ~40 new
// programs at once when a scene starts (its pipelines ~6 ms each): at 20 ms its characters popped in piece by piece
// even warm; 100 ms (a hitch at the cut, where the game loads anyway) took the skipped draws from 267 to 41 on its
// opening without more frames over 50 ms. A cold cache still skips what is left. LARECOMP_SHADER_WAIT_MS=<ms> (0: never).
// Settings' Shader Loading "Don't Wait" (g_settings.shader_nowait) is 0: a first play of Z-A's intro on cold caches had
// 5 two-second spans with a frame over 100 ms instead of 19, and ~5x the skipped draws (things show a moment later).
static bool wait_budget(_Atomic int *state);
// (and the waits counted for [spike]: kind 0 a program, 1 a pipeline)
uint64_t prof_wprog_n, prof_wprog_ns, prof_wpipe_n, prof_wpipe_ns, prof_new_prog, prof_new_pipe;
static bool wait_built(_Atomic int *state, int kind) {
    if (atomic_load(state) != 0) return true;
    uint64_t t0 = SDL_GetTicksNS();
    bool r = wait_budget(state);
    uint64_t dt = SDL_GetTicksNS() - t0;
    if (kind) prof_wpipe_n++, prof_wpipe_ns += dt; else prof_wprog_n++, prof_wprog_ns += dt;
    static int wlog = -1;  // (LARECOMP_WARMUP_LOG=1: a draw's wait over 10 ms)
    if (wlog < 0) wlog = ENV("LARECOMP_WARMUP_LOG") != NULL;
    if (wlog && dt > 10000000) LOG("[warmup] a draw waited %.1f ms for its %s\n", (double)dt / 1e6, kind ? "pipeline" : "program");
    return r;
}
static bool wait_budget(_Atomic int *state) {
    if (atomic_load(state) != 0) return true;
    int64_t budget = (ENV("LARECOMP_SHADER_WAIT_MS") ? atoi(ENV("LARECOMP_SHADER_WAIT_MS")) : g_settings.shader_nowait ? 0 : 100) * 1000000ll;
    if (sync_shaders()) return true;  // (built already)
    extern uint64_t vk_frame_counter(void);
    static uint64_t frame;
    static int64_t spent;
    if (vk_frame_counter() != frame) { frame = vk_frame_counter(); spent = 0; }
    if (spent >= budget) return false;
    uint64_t t0 = SDL_GetTicksNS();
    while (atomic_load(state) == 0 && spent + (int64_t)(SDL_GetTicksNS() - t0) < budget) SDL_DelayNS(100000);
    uint64_t dt = SDL_GetTicksNS() - t0;
    spent += (int64_t)dt;
    stat_shader_wait_ns += dt;
    return atomic_load(state) != 0;
}

static uint64_t prof_tp_;
static void draw_(CmdRec *r, bool indexed) {
    if (prof) prof_tp_ = SDL_GetTicksNS();
    if (!st.program) { SKIP(0); return; }
    if (ip_rec()) {  // (frame interpolation, stereo: what identifies this draw from one frame to the next)
        // what the draw is (program, vertex buffer, parameters), and which game object it belongs to: the same mesh
        // drawn for two objects (bushes, Pidgeys) is told apart by the object, not by the order the game drew them in
        uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t)(uintptr_t)st.program ^ ((uint64_t)indexed << 63);
        h = (h ^ st.vbuf[0].addr) * 0x100000001B3ull;
        for (int i = 0; i < 8; i++) h = (h ^ r->x[i]) * 0x100000001B3ull;
        if (st.obj) h = (h ^ st.obj) * 0x100000001B3ull, ip_stats_tagged++;
        else ip_stats_untagged++;
        ip_draw_sig = h;
        // 3D draws (a depth target with the depth test on) are blended, with or without a game object: the ground and the
        // scenery have none, and they must take the same (blended) light matrices as the shadow map the objects are drawn
        // into, or shadows slide away from their casters. Interface and full-screen passes don't test depth: frame N.
        ip_draw_obj = st.rt.depth != NULL && st.ds[0];
        // In menus (the Pokédex, the bag...) the 2D draws are blended too: lists and panels slide there. A 2D draw takes the
        // strict jump test in ip_cbuf (a recycled row or glyph moves far: it keeps frame N).
        ip_draw_2d = !ip_draw_obj;
        extern bool fps60_menu(void);
        if (fps60_menu()) ip_draw_obj = true;
    }
    ShaderKey vkey = {0}, fkey = {0};
    vkey.glsl = fkey.glsl = 1;
    vkey.flip_y = fkey.flip_y = nvn_window_origin() == 0;
    if (st.xfb_active && ENV("LARECOMP_LOG_XFB")) {
        static int n;
        if (n++ < 5) LOG("[xfb] draw while active: program %p vs %llx layout %u/%u\n", (void *)st.program,
                         (unsigned long long)st.program->stages[0].code_addr, st.program->xfb_count[0], st.program->xfb_stride[0]);
    }
    if (st.xfb_active && g_vk.xfb)  // transform feedback: the vertex program's capture layout (translate_program: xfb outputs)
        for (int b = 0; b < 4; b++) { vkey.xfb_count[b] = st.program->xfb_count[b]; vkey.xfb_stride[b] = st.program->xfb_stride[b]; }
    vkey.depth_minus_one = nvn_depth_mode() == 0;
    for (int i = 0; i < st.nattrib; i++) vkey.attr_kind[i] = (uint8_t)attr_kind(st.attrib[i] & 0xFF);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) fkey.rt_kind[i] = (uint8_t)rt_kind(st.rt.color[i]->format);
    image_kinds(0, vkey.img_kind);
    image_kinds(1, fkey.img_kind);
    vkey.tex_stencil = stencil_slots(0);
    fkey.tex_stencil = stencil_slots(1);
    Prog *pr = get_program(st.program, &vkey, &fkey);
    if (pr) wait_built(&pr->state, 0);
    if (!pr || atomic_load(&pr->state) != 1) {
        if (pr && atomic_load(&pr->state) > 1 && ENV("LARECOMP_LOG_FAILED_DRAWS")) {  // what a failed program draws to
            static void *seen[64];
            static int nseen;
            int k = 0;
            while (k < nseen && seen[k] != pr) k++;
            if (k == nseen && nseen < 64) {
                seen[nseen++] = pr;
                NvnTexture *t0 = st.rt.ncolor ? st.rt.color[0] : NULL;
                LOG("[draw] failed program %p (vs %llx fs %llx) -> color %ux%u fmt %#x, %d targets\n", (void *)pr,
                    (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr,
                    t0 ? t0->width : 0, t0 ? t0->height : 0, t0 ? t0->format : 0, st.rt.ncolor);
            }
        }
        SKIP(pr ? 1 + (atomic_load(&pr->state) > 1) : 3);
        return;
    }
    {   // fragment-shader hash: interior backdrop detection (widescreen), skip lists, item twinkle (renderer.m)
        uint64_t fs = st.program->stages[1].code_addr;
        if (!pr->fs_hash_ok) pr->fs_hash = fs ? hmix(1469598103934665603ull, (const void *)(uintptr_t)(fs + 0x30), 0x200) : 0, pr->fs_hash_ok = true;
        uint64_t last_hash = pr->fs_hash;
        static uint64_t skip[16];
        static int nskip = -1;
        if (nskip < 0) {
            nskip = 0;
            for (const char *p = ENV("LARECOMP_SKIP_FSHASH"); p && *p && nskip < 16;) {
                skip[nskip++] = strtoull(p, (char **)&p, 16);
                while (*p == ',') p++;
            }
        }
        for (int i = 0; i < nskip; i++) if (last_hash == skip[i]) { stat_skipped++; return; }
        if (g_debug_frame && ENV("LARECOMP_LOG_FSHASH")) LOG("[fshash] fs=%llx hash %016llx\n", (unsigned long long)fs, (unsigned long long)last_hash);
        g_cur_fs_hash = last_hash;
    }
    {   // (research, LARECOMP_COUNT_TEX=<w>x<h>: per frame, the draws and instances sampling a fragment texture of that size)
        static int tw = -1, th;
        if (tw < 0) { const char *e = ENV("LARECOMP_COUNT_TEX"); tw = 0; if (e) sscanf(e, "%dx%d", &tw, &th); }
        if (tw > 0) {
            static uint64_t frame = ~0ull;
            static unsigned nd, ni;
                        if (renderer_present_count() != frame) {
                if (frame != ~0ull) LOG("[counttex] frame %llu: %u draws %u instances\n", (unsigned long long)frame, nd, ni);
                frame = renderer_present_count(), nd = ni = 0;
            }
            for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
                NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
                if (t && (int)t->width == tw && (int)t->height == th) { nd++; ni += (unsigned)(indexed ? r->x[6] : r->x[4]); break; }
            }
        }
    }
    if (st.program->stage_mask & 0x1C) {  // (geometry / tessellation stages: not translated - which draws have them)
        static uint64_t seen[64];
        static int nseen;
        uint64_t key = st.program->stages[0].code_addr ^ ((uint64_t)st.program->stage_mask << 56);
        int i = 0;
        while (i < nseen && seen[i] != key) i++;
        if (i == nseen && nseen < 64) {
            seen[nseen++] = key;
            LOG("[shader] draw with stages %#x (vs %llx gs %llx tcs %llx tes %llx fs %llx): only vertex + fragment run\n", st.program->stage_mask,
                (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[2].code_addr,
                (unsigned long long)st.program->stages[3].code_addr, (unsigned long long)st.program->stages[4].code_addr,
                (unsigned long long)st.program->stages[1].code_addr);
        }
    }
    if (ENV("LARECOMP_MESHLOG")) meshlog(pr, r, indexed);
    if (ENV("LARECOMP_SKINPROBE")) skinprobe(pr, r);
    if (g_debug_frame) debug_draw(pr, r, indexed);
    else if ((pr->vi->attr_mask >> 11) & 1) {  // skinned draws (a11: bone indices)
        static int n;
        static int64_t dump_frame = -2;  // LARECOMP_DUMP_SKIN_FRAME: dump the first one from that frame on
        if (dump_frame == -2) dump_frame = ENV("LARECOMP_DUMP_SKIN_FRAME") ? atoll(ENV("LARECOMP_DUMP_SKIN_FRAME")) : -1;
        if (dump_frame >= 0 && renderer_present_count() >= (uint64_t)dump_frame) {
            debug_draw(pr, r, indexed);  // (debug_draw writes build/skin/ once)
            dump_frame = -1;
        } else if (ENV("LARECOMP_LOG_SKIN") && n++ % 50 == 0 && n < 1000) {
            debug_draw(pr, r, indexed);
        }
    }
    resolve_feedback(pr);
    hz_draw(pr, false);  // (a texture rendered to since the last barrier: this pass ends, the next begins with one)
    snap_reason = "draw-depthwrite";
    if (st.rt.depth && st.ds[0] && st.ds[1]) snapshots_invalidate(st.rt.depth);
    snap_reason = "draw-colorwrite";
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i] && ((st.channel_mask >> (i * 4)) & 0xF)) snapshots_invalidate(st.rt.color[i]);
    begin_pass();
    hz_draw(pr, true);
    VkCommandBuffer cb = vk_cmd();

    uint32_t mode = (uint32_t)r->x[0];
    VkPrimitiveTopology topo;
    uint8_t cls;
    bool native = topology(mode, &topo, &cls);
    if (!native && mode != 6 && mode != 7 && mode != 9) { SKIP(4); return; }

    uint64_t prof_tk_ = prof ? SDL_GetTicksNS() : 0;
    if (prof) prof_pre += prof_tk_ - prof_tp_;
    // ---- pipeline
    // (what the key below is made of, as it was: the same as the previous draw's for most draws - then its pipeline,
    // without building and hashing the ~700-byte key: half a microsecond a draw)
    PipeIn in;
    memset(&in, 0, sizeof in);
    in.pr = pr;
    memcpy(in.pcolor, pass_color, sizeof in.pcolor);
    in.pdepth = pass_depth, in.pstencil = pass_stencil, in.pncolor = pass_ncolor, in.psamples = pass_samples;
    in.nattrib = st.nattrib, in.nstream = st.nstream;
    memcpy(in.attrib, st.attrib, sizeof in.attrib);
    memcpy(in.stream, st.stream, sizeof in.stream);
    for (int i = 0; i < 8; i++) in.rtc[i] = st.rt.color[i], in.rtf[i] = st.rt.color[i] ? st.rt.color[i]->format : 0;
    in.chmask = st.channel_mask, in.cstate = st.color_state[0], in.ms0c = st.ms[0xC], in.cls = cls, in.poly2 = st.poly[2];
    in.dclamp = st.depth_clamp;
    memcpy(in.blend, st.blend, sizeof in.blend);
    Pipe *pp;
    if (g_pipe_memo && !memcmp(&in, &g_pipe_memo_in, sizeof in)) {
        pp = g_pipe_memo;
    } else {
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.prog = pr;
    k.ncolor = pass_ncolor;
    // (only the pass's own slots: the ones past them keep what the last pass with more targets set, so a depth-only
    // pass's pipelines got several keys and were built again for each - LAS Recomp 3921201)
    for (uint32_t i = 0; i < pass_ncolor && i < 8; i++) k.color[i] = pass_color[i];
    k.depth = pass_depth;
    k.stencil = pass_stencil;
    k.samples = pass_samples;
    for (int i = 0; i < 32; i++) {
        if (!(pr->vi->attr_mask & (1u << i))) continue;
        k.attr_mask |= 1u << i;
        uint32_t a = i < st.nattrib ? st.attrib[i] : 0x2E;
        uint32_t fmt = a & 0xFF, stream = (a >> 8) & 0x1F, off = a >> 13;
        if (i >= st.nattrib || fmt == 0 || stream >= 16) {  // unbound attribute: (0, 0, 0, 1) from a constant stream
            k.attr_fmt[i] = VK_FORMAT_R32G32B32A32_SFLOAT;
            k.attr_bind[i] = ZERO_BINDING;
            k.used_streams |= 1u << ZERO_BINDING;
            continue;
        }
        k.attr_fmt[i] = vertex_format_supported(vertex_format(fmt));
        k.attr_off[i] = off;
        k.attr_bind[i] = stream;
        k.used_streams |= 1u << stream;
    }
    for (int s = 0; s < 16; s++) {
        if (!(k.used_streams & (1u << s))) continue;
        k.stride[s] = s < st.nstream ? st.stream[s][0] : 0;
        k.divisor[s] = s < st.nstream && k.stride[s] ? st.stream[s][1] : 0;
        if (k.divisor[s] > 1 && !g_vk.divisor) k.divisor[s] = 1;
    }
    for (uint32_t i = 0; i < k.ncolor && i < 8; i++) {
        if (!st.rt.color[i] || k.color[i] == VK_FORMAT_UNDEFINED) continue;
        // Targets the fragment shader doesn't output keep their contents on Metal, but are undefined in Vulkan
        // (NVIDIA writes black): mask them. Foliage depth pre-passes left black silhouettes (user 2026-10-08).
        k.write_mask[i] = ((pr->fi->out_mask >> (i * 4)) & 0xF) ? (st.channel_mask >> (i * 4)) & 0xF : 0;
        if (((st.color_state[0] >> i) & 1) && !rt_kind(st.rt.color[i]->format)) {
            k.blend_on |= (uint8_t)(1u << i);
            memcpy(k.blend[i], st.blend[i], 8);
        }
    }
    k.a2c = st.ms[0xC] != 0;
    k.topo_class = cls;
    k.poly_fill = st.poly[2] != 1;
    k.depth_clamp = st.depth_clamp && g_vk.depth_clamp;
    uint64_t ph = hash_words(0xcbf29ce484222325ull, &k, sizeof k);
    if (!ph) ph = 1;
    uint32_t pslot = (uint32_t)(ph ^ (ph >> 29)) & (PIPE_TAB - 1);
    for (int probe = 0; pipe_tab[pslot].key && pipe_tab[pslot].key != ph; probe++) {
        if (probe == 64) {  // (full run of slots: the entry is replaced, its Pipe kept - draws may still use it)
            pipe_tab[pslot].key = 0, pipe_tab[pslot].pipe = NULL;
            atomic_fetch_add(&g_cache_pipes_replaced, 1);
            break;
        }
        pslot = (pslot + 1) & (PIPE_TAB - 1);
    }
    pp = pipe_tab[pslot].pipe;
    if (!pp) {
        static int log_keys = -1;
        if (log_keys < 0) log_keys = ENV("LARECOMP_LOG_PIPEKEYS") != NULL;
        if (log_keys) {  // (research: which part of the key makes each new pipeline new - grouped by program offline)
            #define KH(...) (unsigned)(hash_words(0xcbf29ce484222325ull, __VA_ARGS__) >> 32)
            LOG("[pipekey] f%llu prog %p vs %016llx@%llx fs %016llx@%llx rt %08x va %08x st %08x bl %08x wm %08x topo %u fill %u dc %u\n",
                (unsigned long long)renderer_game_frame(), (void *)k.prog, (unsigned long long)k.prog->content[0],
                (unsigned long long)k.prog->code[0], (unsigned long long)k.prog->content[1], (unsigned long long)k.prog->code[1],
                KH(k.color, sizeof k.color + sizeof k.depth + sizeof k.stencil + sizeof k.ncolor + sizeof k.samples),
                KH(k.attr_fmt, sizeof k.attr_fmt + sizeof k.attr_off + sizeof k.attr_bind + sizeof k.attr_mask),
                KH(k.stride, sizeof k.stride + sizeof k.divisor + sizeof k.used_streams),
                KH(k.blend, sizeof k.blend + 2), KH(k.write_mask, sizeof k.write_mask), k.topo_class, k.poly_fill, k.depth_clamp);
            #undef KH
        }
        atomic_fetch_add(&g_cache_pipes, 1);
        pp = calloc(1, sizeof *pp);
        pp->k = k;
        pp->t_asked = SDL_GetTicksNS();
        pipe_tab[pslot].key = ph;
        pipe_tab[pslot].pipe = pp;
        prof_new_pipe++;
        run_job(build_pipeline, pp);
    }
    wait_built(&pp->state, 1);
    if (atomic_load(&pp->state) != 1) { SKIP(5 + (atomic_load(&pp->state) > 1)); return; }
    g_pipe_memo = pp, g_pipe_memo_in = in;
    }
    const PipeKey *kp = &pp->k;  // (the key it was built for: this draw's)
    dc_begin(cb);
    uint64_t pp_ = prof ? SDL_GetTicksNS() : 0;
    if (atomic_load_explicit(&pp->optimized, memory_order_relaxed)) {  // (gpl_optimize: the optimized link replaces the fast one)
        VkPipeline o = atomic_exchange(&pp->optimized, VK_NULL_HANDLE);
        extern void vk_defer_pipeline(VkPipeline p);  // (textures.c: destroyed once the GPU is past the work that used it)
        vk_defer_pipeline(pp->pipe);
        pp->pipe = o;
    }
    if (dc.pipe != pp->pipe) dc.pipe = pp->pipe, vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->pipe);
    if (prof) prof_bindpipe += SDL_GetTicksNS() - pp_;
    DC_SET(rdisc, st.raster_discard, vkCmdSetRasterizerDiscardEnable(cb, st.raster_discard));
    uint64_t prof_td_ = prof ? SDL_GetTicksNS() : 0;
    if (prof) prof_key += prof_td_ - prof_tk_;

    // ---- depth / stencil (dynamic)
    static int nodt = -1;
    if (nodt < 0) nodt = ENV("LARECOMP_NO_DEPTH_TEST") != NULL;
    bool dt = st.ds[0] && !nodt && pass_depth;
    DC_SET(dtest, dt, vkCmdSetDepthTestEnable(cb, dt));
    DC_SET(dwrite, dt && st.ds[1], vkCmdSetDepthWriteEnable(cb, dt && st.ds[1]));
    VkCompareOp dop = dt ? compare_fn(st.ds[2]) : VK_COMPARE_OP_ALWAYS;
    // EQUAL against a depth pre-pass written by a different vertex shader (foliage, trees) needs both shaders to land
    // on identical depths. They do since the translator stopped using unpackHalf2x16 (NVIDIA ran the half math after
    // it in fp16, differently per shader: black slashes in foliage). LARECOMP_VK_EQUAL_TOL=1 brings back the old
    // workaround: LESS_OR_EQUAL with a few ulps of bias towards the camera, which also let walls paint over the
    // plants stuck flat on them (beach) - so it is off.
    static int tol = -1;
    if (tol < 0) tol = ENV("LARECOMP_VK_EQUAL_TOL") != NULL;
    bool equal_tol = dop == VK_COMPARE_OP_EQUAL && tol;
    if (equal_tol) dop = VK_COMPARE_OP_LESS_OR_EQUAL;
    DC_SET(dcomp, dop, vkCmdSetDepthCompareOp(cb, dop));
    bool sten = st.ds[3] && pass_stencil;
    DC_SET(sten, sten, vkCmdSetStencilTestEnable(cb, sten));
    if (sten) {
        uint32_t ops = st.ds[5] | st.ds[6] << 8 | st.ds[7] << 16;
        for (int f = 0; f < 2; f++) {
            uint32_t o = f ? ops >> 12 : ops;
            VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
            int64_t sop = (int64_t)(o & 0xFFF) | (int64_t)(f ? st.ds[4] >> 4 : st.ds[4] & 0xF) << 12;
            DC_SET(sop[f][0], sop, vkCmdSetStencilOp(cb, face, stencil_op(o & 0xF), stencil_op((o >> 8) & 0xF), stencil_op((o >> 4) & 0xF),
                                                        compare_fn(f ? st.ds[4] >> 4 : st.ds[4] & 0xF)));
            DC_SET(smask[f][0], st.stencil_vmask[f] & 0xFF, vkCmdSetStencilCompareMask(cb, face, st.stencil_vmask[f] & 0xFF));
            DC_SET(smask[f][1], st.stencil_mask[f] & 0xFF, vkCmdSetStencilWriteMask(cb, face, st.stencil_mask[f] & 0xFF));
            DC_SET(smask[f][2], st.stencil_ref[f], vkCmdSetStencilReference(cb, face, st.stencil_ref[f]));
        }
    }
    if (memcmp(dc.blend, st.blend_color, sizeof dc.blend)) memcpy(dc.blend, st.blend_color, sizeof dc.blend), vkCmdSetBlendConstants(cb, st.blend_color);

    // ---- rasterizer
    uint8_t cull = st.poly[0];
    VkCullModeFlags cm = cull == 1 ? VK_CULL_MODE_FRONT_BIT : cull == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    extern volatile int g_live_nocull;  // (research: LARECOMP_LIVE <dir>/nocull "1" / "0"; hle/za_fixes.c sets it for Legends Z-A)
    if (ENV("LARECOMP_VK_NO_CULL") || g_live_nocull) cm = VK_CULL_MODE_NONE;  // (geometry missing from one side - culled?)
    extern volatile uint64_t g_exp_nocull_start;  // (LARECOMP_LIVE <dir>/nocullbits starts it on the next frame)
    if (ENV("LARECOMP_EXP_NOCULL_BITS") || g_exp_nocull_start) {  // (research: frame start + k culls nothing for the draws whose index in the frame has bit k)
        static uint64_t fr = ~0ull, base;
        extern uint64_t renderer_game_frame(void);
        uint64_t f = vk_frame_counter(), start = g_exp_nocull_start ? g_exp_nocull_start : strtoull(ENV("LARECOMP_EXP_NOCULL_BITS"), NULL, 10);
        if (f != fr) fr = f, base = g_draws_total;
        uint64_t idx = g_draws_total - base;
        if (f >= start && f < start + 12 && idx == 0) LOG("[exp] nocull bit %llu: vk frame %llu, present %llu\n", (unsigned long long)(f - start),
                                                       (unsigned long long)f, (unsigned long long)renderer_present_count());
        if (f >= start && f < start + 12 && ((idx >> (f - start)) & 1)) cm = VK_CULL_MODE_NONE;
        if (f == start + 12 && idx == 0) LOG("[exp] nocull bits done\n");
    }
    {   // (research: LARECOMP_LIVE <dir>/nocullone "<first> <last>": those draw indices never culled, until removed again)
        extern volatile long g_nocull_lo, g_nocull_hi;
        static uint64_t nfr = ~0ull, nbase;
        uint64_t f = vk_frame_counter();
        if (f != nfr) nfr = f, nbase = g_draws_total;
        long idx = (long)(g_draws_total - nbase);
        if (g_nocull_hi > 0 && idx >= g_nocull_lo && idx <= g_nocull_hi) cm = VK_CULL_MODE_NONE;
    }
    DC_SET(cull, cm, vkCmdSetCullMode(cb, cm));
    bool ccw = st.poly[1] == 1;
    static int flipw = -1;  // LARECOMP_VK_FLIP_WINDING: invert the front-face mapping (A/B against Metal)
    if (flipw < 0) flipw = ENV("LARECOMP_VK_FLIP_WINDING") != NULL;
    bool front_ccw = (ccw != vkey.flip_y) != (flipw != 0);
    DC_SET(front, front_ccw, vkCmdSetFrontFace(cb, front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE));
    {   // (research: LARECOMP_LIVE <dir>/drawprobe "<first> <last>": the draws with those indices in the next frame, in detail)
        extern volatile long g_probe_lo, g_probe_hi;
        extern volatile uint64_t g_probe_frame;
        static uint64_t pfr = ~0ull, pbase;
        uint64_t f = vk_frame_counter();
        if (f != pfr) pfr = f, pbase = g_draws_total;
        long idx = (long)(g_draws_total - pbase);
        if (g_probe_frame == f && idx >= g_probe_lo && idx <= g_probe_hi) {
            const float *m10 = st.ubuf[0][7].addr ? (const float *)(uintptr_t)st.ubuf[0][7].addr : NULL;
            const float *m5 = st.ubuf[0][2].addr ? (const float *)(uintptr_t)st.ubuf[0][2].addr : NULL;
            LOG("[probe] #%ld vs %llx fs %llx poly %02x%02x cull %d front_ccw %d flip_y %d vp %.0f %.0f %.0f %.0f inst %u\n", idx,
                (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr, st.poly[0], st.poly[1],
                (int)cm, front_ccw, (int)vkey.flip_y, st.viewport[0][0], st.viewport[0][1], st.viewport[0][2], st.viewport[0][3], (unsigned)st.ubuf[0][7].size);
            if (indexed && (st.attrib[0] & 0xFF) == 0x22 && (st.attrib[1] & 0xFF) == 0x29) {  // winding vs the stored normals
                int sidx = (st.attrib[0] >> 8) & 0x1F, agree = 0, tot = 0;
                uint32_t stride = st.stream[sidx][0], o0 = st.attrib[0] >> 13, o1 = st.attrib[1] >> 13;
                const uint8_t *vb = (const uint8_t *)(uintptr_t)st.vbuf[sidx].addr;
                const void *ib = (const void *)(uintptr_t)r->x[3];
                uint32_t isz = (uint32_t)r->x[1] == 1 ? 2 : 4;
                for (uint32_t t = 0; vb && ib && t + 2 < r->x[2] && tot < 4096; t += 3, tot++) {
                    float P[3][3], Nn[3] = {0, 0, 0};
                    for (int q = 0; q < 3; q++) {
                        uint32_t id = (isz == 2 ? ((const uint16_t *)ib)[t + q] : ((const uint32_t *)ib)[t + q]) + (int32_t)r->x[4];
                        memcpy(P[q], vb + (uint64_t)id * stride + o0, 12);
                        const uint16_t *n = (const uint16_t *)(vb + (uint64_t)id * stride + o1);  // (RGBA16F)
                        for (int j = 0; j < 3; j++) {
                            uint32_t h = n[j], e = (h >> 10) & 0x1F, m = h & 0x3FF;
                            float v = e ? ldexpf((float)(m | 0x400), (int)e - 25) : ldexpf((float)m, -24);
                            Nn[j] += (h & 0x8000) ? -v : v;
                        }
                    }
                    float e1[3], e2[3], g[3];
                    for (int j = 0; j < 3; j++) e1[j] = P[1][j] - P[0][j], e2[j] = P[2][j] - P[0][j];
                    g[0] = e1[1] * e2[2] - e1[2] * e2[1], g[1] = e1[2] * e2[0] - e1[0] * e2[2], g[2] = e1[0] * e2[1] - e1[1] * e2[0];
                    agree += g[0] * Nn[0] + g[1] * Nn[1] + g[2] * Nn[2] > 0;
                }
                const float *p0 = vb ? (const float *)(vb + o0) : NULL;
                LOG("[probe]   winding agrees with normals %d/%d; vbuf %llx (gpu writes %d) ibuf %llx (gpu writes %d) v0 (%g %g %g)\n", agree, tot,
                    (unsigned long long)st.vbuf[sidx].addr, (int)vk_gpu_may_write(st.vbuf[sidx].addr), (unsigned long long)r->x[3],
                    (int)vk_gpu_may_write(r->x[3]), p0 ? p0[0] : 0, p0 ? p0[1] : 0, p0 ? p0[2] : 0);
            }
            LOG("[probe]   in list %llx (depth %d, top %llx); polygon state from list %llx rec %llx depth %d\n", (unsigned long long)g_cur_list[g_cur_depth < 9 ? g_cur_depth : 8],
                g_cur_depth, (unsigned long long)g_cur_list[0], (unsigned long long)g_poly_src_list, (unsigned long long)g_poly_src_rec, g_poly_src_depth);
            LOG("[probe]   mode %llu args %llx %llx %llx %llx %llx %llx native %d\n", (unsigned long long)r->x[0], (unsigned long long)r->x[1],
                (unsigned long long)r->x[2], (unsigned long long)r->x[3], (unsigned long long)r->x[4], (unsigned long long)r->x[5],
                (unsigned long long)r->x[6], (int)native);
            {
                char line[512];
                int len = snprintf(line, sizeof line, "[probe]   attribs:");
                for (int i = 0; i < st.nattrib && len < 440; i++)
                    if ((pr->vi->attr_mask >> i) & 1)
                        len += snprintf(line + len, sizeof line - (size_t)len, " a%d=f%#x/s%u/+%u", i, st.attrib[i] & 0xFF, (st.attrib[i] >> 8) & 0x1F, st.attrib[i] >> 13);
                for (int k = 0; k < st.nstream && k < 6 && len < 480; k++)
                    len += snprintf(line + len, sizeof line - (size_t)len, " | s%d stride %u div %u", k, st.stream[k][0], st.stream[k][1]);
                LOG("%s\n", line);
            }
            if (m10) LOG("[probe]   cb10 %llx: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g || %g %g %g %g | %g %g %g %g\n", (unsigned long long)st.ubuf[0][7].addr,
                         m10[0], m10[1], m10[2], m10[3], m10[4], m10[5], m10[6], m10[7], m10[8], m10[9], m10[10], m10[11], m10[12], m10[13], m10[14], m10[15],
                         m10[16], m10[17], m10[18], m10[19], m10[20], m10[21], m10[22], m10[23]);
            if (m5) LOG("[probe]   cb5 %llx: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n", (unsigned long long)st.ubuf[0][2].addr, m5[0], m5[1], m5[2], m5[3],
                        m5[4], m5[5], m5[6], m5[7], m5[8], m5[9], m5[10], m5[11], m5[12], m5[13], m5[14], m5[15]);
        }
    }
    VkPrimitiveTopology vtopo = native ? topo : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    DC_SET(topo, vtopo, vkCmdSetPrimitiveTopology(cb, vtopo));
    bool restart = st.prim_restart && native && (mode == 3 || mode == 5);
    DC_SET(prim_restart, restart, vkCmdSetPrimitiveRestartEnable(cb, restart));
    {
        float f[3] = {0, 0, 0};
        static int off = -1;
        if (off < 0) off = ENV("LARECOMP_NO_DEPTH_BIAS") != NULL;
        bool on = (st.poly[3] & 4) && !off;
        if (on) memcpy(f, st.poly_offset, sizeof f);
        if (equal_tol) { f[1] -= 8; on = true; }  // (see the EQUAL comment above: a few depth ulps nearer)
        static float force[2] = {-1, 0};  // (research) LARECOMP_FORCE_DEPTH_BIAS=<const>,<slope>: on depth-only-ish D32F passes
        if (force[0] < 0) { const char *e = ENV("LARECOMP_FORCE_DEPTH_BIAS"); force[0] = 0; if (e) sscanf(e, "%f,%f", &force[0], &force[1]); }
        if ((force[0] || force[1]) && st.rt.depth && st.rt.depth->format == 0x34) { f[1] = force[0]; f[2] = 0; f[0] = force[1]; on = true; }
        DC_SET(dbias_on, on, vkCmdSetDepthBiasEnable(cb, on));
        if (memcmp(dc.dbias, f, sizeof f)) memcpy(dc.dbias, f, sizeof f), vkCmdSetDepthBias(cb, f[1], f[2], f[0]);
    }

    // ---- textures first (their scales feed the widescreen decision and the rscale table), then the viewport
    float *vp = st.viewport[0];
    float zn = st.depth_range[0][0], zf = st.depth_range[0][1];
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    float tint[8] = {0};  // (the per-draw tint the translator can apply to colour target 0: none)
    // fragment textures are bound before deciding the viewport (tex_sc / tex_wh), rscale depends on the decision:
    // build the vertex set now and the fragment set after the decision (stage_set fills tex_sc for stage 1).
    if (prof) prof_dyn += SDL_GetTicksNS() - prof_td_;
    PROF_T0();
    VkDescriptorSet vset = stage_set(pr, pr->set_layout[0], 0, 0, pr->vi, NULL, NULL, 0, 0, 0, NULL);
    PROF_ADD(prof_sets);
    VkDescriptorImageInfo fii[64];
    uint64_t prof_tx_ = prof ? SDL_GetTicksNS() : 0;
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
        float sc[2]; uint32_t wh[2];
        texture_info(1, pr->fi, i, &fii[i], sc, wh);
        tex_sc[1][i][0] = sc[0]; tex_sc[1][i][1] = sc[1]; tex_wh[1][i][0] = wh[0]; tex_wh[1][i][1] = wh[1];
    }
    if (prof) prof_tex += SDL_GetTicksNS() - prof_tx_;
    // (the scene's final target, the presented image's size, takes the box too for the interface drawn into it: Legends:
    // Arceus' sprites go there and a last pass copies it to the window - in a VR mod's eye shape they came out squeezed)
    bool final_rt = st.rt.ncolor && st.rt.color[0] && !renderer_texture_presented(st.rt.color[0]) &&
                    st.rt.color[0]->width == st_pres_w && st.rt.color[0]->height == st_pres_h && !(st.rt.depth && st.ds[0]) &&
                    (st.color_state[0] & 1) && g_plugin_render_shape > 0;
    if (fabsf(pass_sx - pass_sy) > 0.01f * pass_sy && st.rt.ncolor && (renderer_texture_presented(st.rt.color[0]) || final_rt)) {
        // Draws into the swapchain image (see renderer.m): composites fill the window, UI goes in a centred 16:9 box.
        bool reads_scene = false;
        for (int i = 0; i < pr->fi->ntex && i < 64; i++)
            if ((tex_sc[1][i][0] != 1 || tex_sc[1][i][1] != 1) && tex_wh[1][i][0] == st.rt.color[0]->width &&
                tex_wh[1][i][1] == st.rt.color[0]->height)
                reads_scene = true;
        bool fullscreen_quad = r->x[2] <= 6 && st.viewport[0][0] == 0 && st.viewport[0][1] == 0 &&
                               st.viewport[0][2] >= st.rt.color[0]->width && st.viewport[0][3] >= st.rt.color[0]->height;
        bool fullscreen_fill = pr->fi->ntex == 0 && fullscreen_quad;
        if (fullscreen_quad && !reads_scene && pr->fi->ntex == 1 && (tex_sc[1][0][0] != 1 || tex_sc[1][0][1] != 1)) reads_scene = true;
        static uint64_t wide_frame = ~0ull;
        if (reads_scene) wide_frame = renderer_present_count();
        bool fade_fill = false;
        if (fullscreen_fill && st.ubuf[1][0].addr && st.ubuf[1][0].size >= 12) {
            const float *c = (const float *)(uintptr_t)st.ubuf[1][0].addr;
            bool black = c[0] < 0.01f && c[1] < 0.01f && c[2] < 0.01f, white = c[0] > 0.99f && c[1] > 0.99f && c[2] > 0.99f;
            fade_fill = black || white;
        }
        bool composite = reads_scene || fade_fill || (fullscreen_fill && wide_frame == renderer_present_count());
        if (!composite) wide_frame = ~0ull;
        if (!composite) {
            int pw, ph;
            pass_size(&pw, &ph);
            float u = fminf(pass_sx, pass_sy);
            vp_sx = vp_sy = u;
            vp_ox = (pw - st.rt.color[0]->width * u) * 0.5f;
            vp_oy = (ph - st.rt.color[0]->height * u) * 0.5f;
        }
    }
    {   // (research, LARECOMP_COUNT_FS=<fs hash>: draws with that fragment shader per frame, logged when the count changes)
        static uint64_t want = 1, frame = ~0ull;
        static unsigned n, last = ~0u;
        if (want == 1) { const char *e = ENV("LARECOMP_COUNT_FS"); want = e ? strtoull(e, NULL, 16) : 0; }
        if (want) {
                        if (renderer_present_count() != frame) {
                if (frame != ~0ull && n != last) LOG("[countfs] frame %llu: %u\n", (unsigned long long)frame, n);
                if (frame != ~0ull) last = n;
                frame = renderer_present_count(), n = 0;
            }
            if (g_cur_fs_hash == want) n++;
        }
    }
    if (ip_replaying && st_on() && g_plugin_overlay_px != 0 && st.rt.ncolor && st.rt.color[0]) {
        // Stereo, eye 1: the 2D overlay drawn g_plugin_overlay_px to the left (its parallax: plugin.h set_overlay_parallax).
        // The overlay: blended, no depth test, reading no screen-sized image (that is a composite or a post pass), into a
        // target the size of the presented image - Legends: Arceus' interface sprites into its 1920x1080 scene target.
        NvnTexture *rt = st.rt.color[0];
        bool overlay = (st.color_state[0] & 1) && !(st.rt.depth && st.ds[0]) && rt->width * 2 >= st_pres_w && rt->height * 2 >= st_pres_h;
        for (int i = 0; overlay && i < pr->fi->ntex && i < 64; i++)
            if (tex_sc[1][i][0] != 1 || tex_sc[1][i][1] != 1 || (tex_wh[1][i][0] == rt->width && tex_wh[1][i][1] == rt->height)) overlay = false;
        if (overlay) vp_ox -= g_plugin_overlay_px;  // (pixels of the image as drawn, as vp_ox)
    }
    {
        float vx = vp[0] * vp_sx + vp_ox, vw = vp[2] * vp_sx;
        set_viewport(vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, zn, zf);
        if (g_debug_frame) {
            VkRect2D sr = scissor_rect();
            LOG("[draw]   vp %.2f %.2f %.2f %.2f (fs hash %016llx) scissor %d %d %u %u | guest sc %d %d %d %d | pass %ux%u s %.4f %.4f\n",
                vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, (unsigned long long)g_cur_fs_hash, sr.offset.x, sr.offset.y,
                sr.extent.width, sr.extent.height, st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3], pass_w, pass_h,
                vp_sx, vp_sy);
        }
    }
    apply_scissor();
    float rs0[4] = {vp_sx, vp_sy, vp_ox, vp_oy};
    uint64_t prof_t1_ = prof ? SDL_GetTicksNS() : 0;
    gpuw_stores(pr->vi, st.sbuf[0], 16, "vertex shader", st.program->stages[0].code_addr);
    gpuw_stores(pr->fi, st.sbuf[1], 16, "fragment shader", st.program->stages[1].code_addr);
    VkDescriptorSet fset = stage_set(pr, pr->set_layout[1], 1, 1, pr->fi, rs0, tint, 0, 0, 0, fii);
    if (prof) prof_sets += SDL_GetTicksNS() - prof_t1_;
    VkDescriptorSet sets[2] = {vset, fset};
    uint64_t pb_ = prof ? SDL_GetTicksNS() : 0;
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, 0, 2, sets, 0, NULL);
    if (prof) prof_dbind += SDL_GetTicksNS() - pb_;

    uint64_t prof_t2_ = prof ? SDL_GetTicksNS() : 0;
    // ---- indices (copied; the largest index bounds the vertex data copied below)
    uint32_t first = 0, count = (uint32_t)r->x[2], inst, base_inst, max_index = 0;
    int32_t base_vertex = 0;
    VkSpan ib = {0};
    VkIndexType itype_vk = VK_INDEX_TYPE_UINT32;
    uint32_t icount = 0;
    bool use_index = indexed || !native;
    const void *scan_src = NULL;  // deferred index scan (direct index buffer)
    uint32_t scan_isz = 0, scan_restart = 0;
    if (!indexed) {
        first = (uint32_t)r->x[1]; base_inst = (uint32_t)r->x[3]; inst = (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 1;
        max_index = g_draw_indirect ? 0x7FFFFFFF : first + count;
        if (!native) {
            uint32_t *e = expand_indices(mode, count, NULL, 0, first, &icount);
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
    } else {
        uint32_t itype = (uint32_t)r->x[1];
        uint64_t iaddr = r->x[3];
        base_vertex = (int32_t)r->x[4];
        base_inst = (uint32_t)r->x[5];
        inst = (uint32_t)r->x[6] ? (uint32_t)r->x[6] : 1;
        const void *src = (const void *)(uintptr_t)iaddr;
        uint32_t restart = st.prim_restart ? st.restart_index : 0xFFFFFFFFu;
        if (g_draw_indirect) {  // (the count is the GPU's: the index data from its address to the end of its mapping, in place)
            uint32_t isz = itype == 1 ? 2 : itype == 2 ? 4 : 1;
            itype_vk = itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
            uint64_t in = 4ull << 20;
            gmem_info();
            if (iaddr >= HEAP_BASE && iaddr < HEAP_BASE + gmem_size && iaddr + in > HEAP_BASE + gmem_size) in = HEAP_BASE + gmem_size - iaddr;
            if (itype == 0 || iaddr % isz || !guest_direct(iaddr, in, &ib.buf, &ib.off)) {
                static int warned;
                if (!warned++) LOG("[vk] indirect draw: index buffer %llx (type %u) not readable in place: skipped\n", (unsigned long long)iaddr, itype);
                SKIP(7);
                return;
            }
            max_index = 0x7FFFFFFF;
        } else if (native && itype != 0) {
            uint32_t isz = itype == 1 ? 2 : 4;
            itype_vk = itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
            icount = count;
            uint64_t in = (uint64_t)count * isz;
            if (iaddr % isz == 0 && ((!gpuw_overlaps(iaddr, in) && vcache_get(iaddr, in, &ib.buf, &ib.off)) || vtx_direct(iaddr, in, &ib.buf, &ib.off))) {
                scan_src = src; scan_isz = isz; scan_restart = restart;  // max_index only if a stream gets copied
            } else {
                ib = transient_gpu_copy(src, (uint64_t)count * isz + 4, 4);
                { uint32_t m = scan_max_index(src, isz, count, restart); if (m > max_index) max_index = m; }
            }
        } else if (native) {  // 8-bit indices: widen to 16
            ib = vk_transient((uint64_t)count * 2 + 4, 4);
            uint16_t *w = ib.ptr;
            const uint8_t *s = src;
            for (uint32_t i = 0; i < count; i++) { w[i] = s[i] == 0xFF && st.prim_restart ? 0xFFFF : s[i]; if (s[i] > max_index && w[i] != 0xFFFF) max_index = s[i]; }
            itype_vk = VK_INDEX_TYPE_UINT16;
            icount = count;
        } else {
            uint32_t *e = expand_indices(mode, count, src, (int)itype, 0, &icount);
            for (uint32_t i = 0; i < icount; i++) if (e[i] > max_index) max_index = e[i];
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
        if (!scan_src) {
            max_index += 1;
            if (base_vertex > 0) max_index += (uint32_t)base_vertex;
        }
    }
    if (use_index && (dc.ib != ib.buf || dc.iboff != ib.off || dc.itype != (int64_t)itype_vk)) {
        dc.ib = ib.buf, dc.iboff = ib.off, dc.itype = itype_vk;
        vkCmdBindIndexBuffer(cb, ib.buf, ib.off, itype_vk);
    }

    // ---- vertex streams: the range this draw can read, copied once per submission
    for (int s = 0; s < 17; s++) {
        if (!(kp->used_streams & (1u << s))) continue;
        VkSpan sp;
        if (s == ZERO_BINDING) {
            static const float zero[4] = {0, 0, 0, 1};
            sp = vk_transient_copy(zero, 16, 16);
        } else {
            Range v = st.vbuf[s];
            VkBuffer dbuf;
            VkDeviceSize doff;
            bool gw = v.addr && v.size && gpuw_overlaps(v.addr, v.size) && !ENV("LARECOMP_VCACHE_IGNORE_GPUW");
            g_vcache_gpuw += gw;
            if (ENV("LARECOMP_LOG_VSTREAM") && g_gpuprof_frame)  // (research: the stream ranges of a profiled frame)
                LOG("[vstream] %llx +%llu stride %u div %u gpuw %d max_index %u inst %u\n", (unsigned long long)v.addr, (unsigned long long)v.size,
                    (unsigned)kp->stride[s], (unsigned)kp->divisor[s], gw, max_index, inst);
            if (v.addr && v.size && ((!gw && vcache_get(v.addr, v.size, &dbuf, &doff)) ||
                                     vtx_direct(v.addr, v.size, &dbuf, &doff))) {
                dc_bind_vb(cb, (uint32_t)s, dbuf, doff);
                continue;
            }
            if (scan_src) {  // copying after all: the index scan bounds the copy
                { uint32_t m = scan_max_index(scan_src, scan_isz, icount, scan_restart); if (m > max_index) max_index = m; }
                max_index += 1;
                if (base_vertex > 0) max_index += (uint32_t)base_vertex;
                scan_src = NULL;
            }
            uint64_t stride = kp->stride[s], need;
            if (!stride) need = 256;
            else if (g_draw_indirect) need = v.size ? v.size : 256;
            else if (kp->divisor[s]) need = ((uint64_t)base_inst + inst + kp->divisor[s] - 1) / kp->divisor[s] * stride + 256;
            else need = (uint64_t)max_index * stride + 256;
            if (v.size && need > v.size) need = v.size;
            if (!v.addr) { static const uint8_t zero[256]; sp = vk_transient_copy(zero, 256, 16); }
            else {
                sp = vertex_copy(v.addr, need ? need : 16, v.size, kp->divisor[s] == 0);
                if (ENV("LARECOMP_LOG_VCOPY") && g_gpuprof_frame)  // (research: what a profiled frame copies, per stream)
                    LOG("[vcopy] vs %llx stream %d addr %llx need %llu of bound %llu stride %llu %s max_index %u base_vertex %d base_inst %u inst %u\n",
                        (unsigned long long)st.program->stages[0].code_addr, s, (unsigned long long)v.addr, (unsigned long long)need,
                        (unsigned long long)v.size, (unsigned long long)stride, kp->divisor[s] ? "instance" : "vertex", max_index, base_vertex, base_inst, inst);
            }
        }
        dc_bind_vb(cb, (uint32_t)s, sp.buf, sp.off);
    }

    if (prof) prof_vtx += SDL_GetTicksNS() - prof_t2_;
    // ---- draw
    // LARECOMP_GPUPROF_DRAWS=1 (with LARECOMP_GPUPROF_AT): a mark before every draw, so each row is one draw's GPU time
    static int gp_draw_marks = -1;
    if (gp_draw_marks < 0) gp_draw_marks = ENV("LARECOMP_GPUPROF_DRAWS") != NULL;
    if (g_gpuprof_frame && gp_draw_marks)
        gp_mark(cb, "draw vs %llx fs %016llx fsaddr %llx %s %u inst %u", (unsigned long long)st.program->stages[0].code_addr,
                (unsigned long long)g_cur_fs_hash, (unsigned long long)st.program->stages[1].code_addr, use_index ? "elements" : "arrays", use_index ? icount : count, inst);
    // Experiment (research): LARECOMP_EXP_VS=<vertex program address, hex> with LARECOMP_EXP_MODE=skip | scissor | half
    // changes only the draws of that program: no draw, a 1x1 scissor (no pixel work, vertex work kept) or half the instances.
    static uint64_t exp_vs;
    static int exp_mode = -1;
    if (exp_mode < 0) {
        const char *v = ENV("LARECOMP_EXP_VS"), *m = ENV("LARECOMP_EXP_MODE");
        exp_vs = v ? strtoull(v, NULL, 16) : 0;
        exp_mode = !exp_vs || !m ? 0 : !strcmp(m, "skip") ? 1 : !strcmp(m, "scissor") ? 2 : !strcmp(m, "half") ? 3 : 0;
    }
    // (LARECOMP_EXP_ALT=1: only on odd frames, so one run holds both versions of the same scene)
    static int exp_alt = -1;
    if (exp_alt < 0) exp_alt = ENV("LARECOMP_EXP_ALT") != NULL;
    bool exp_hit = exp_mode && st.program->stages[0].code_addr == exp_vs && (!exp_alt || (vk_frame_counter() & 1));
    uint32_t inst_eff = exp_hit && exp_mode == 3 && inst > 1 ? inst / 2 : inst;
    if (exp_hit && exp_mode == 2) { VkRect2D one = {{0, 0}, {1, 1}}; vkCmdSetScissor(cb, 0, 1, &one); memset(&dc_sc, 0xff, sizeof dc_sc); }
    bool gp_q = g_gpuprof_frame && gp_draw_marks && gps_qp && gp_n && gps_n < GP_MAX;
    if (gp_q) vkCmdBeginQuery(cb, gps_qp, (uint32_t)gps_n, 0);
    // Transform feedback: the program captures its outputs (xfb outputs of its GLSL) into the bound guest buffers, which
    // lie in the imported heap (gmem_buf); the draws that read them as vertex data later read them in place (gpuw).
    bool xfb_on = st.xfb_active && g_vk.xfb && pr->vkey.xfb_count[0];
    if (xfb_on) {
        VkDescriptorBufferInfo gi = gmem_info();
        VkBuffer bufs[4];
        VkDeviceSize offs[4], sizes[4];
        uint32_t nb = 0;
        for (int b = 0; b < 4 && pr->vkey.xfb_count[b]; b++) {
            uint64_t a = st.xfb_addr[b], n = st.xfb_size[b];
            if (!gmem_buf || gi.buffer != gmem_buf || (a & 3) || !gmem_plain(a, n, &bufs[nb], &offs[nb])) { xfb_on = false; break; }
            sizes[nb] = n; nb++;
        }
        static int logged;
        if (!xfb_on && logged++ < 10) LOG("[xfb] capture skipped: buffer %llx outside the imported heap\n", (unsigned long long)st.xfb_addr[0]);
        if (xfb_on) {
            g_vk.bind_xfb(cb, 0, nb, bufs, offs, sizes);
            g_vk.begin_xfb(cb, 0, 0, NULL, NULL);
        }
    }
    if (exp_hit && exp_mode == 1) { /* skipped */ }
    else if (g_draw_indirect) {
        VkDescriptorBufferInfo gm = gmem_info();
        uint64_t off = g_draw_indirect - HEAP_BASE;
        if (gm.buffer && gm.buffer == gmem_buf && g_draw_indirect >= HEAP_BASE && off + 20 <= gmem_size) {  // (gmem_size: the range may be capped)
            if (use_index) vkCmdDrawIndexedIndirect(cb, gm.buffer, off, 1, 20);
            else vkCmdDrawIndirect(cb, gm.buffer, off, 1, 16);
        } else {
            static int warned;
            if (!warned++) LOG("[vk] indirect draw: arguments at %llx outside the heap import: skipped\n", (unsigned long long)g_draw_indirect);
        }
    }
    else if (!use_index) vkCmdDraw(cb, count, inst_eff, first, base_inst);
    else vkCmdDrawIndexed(cb, icount, inst_eff, 0, base_vertex, base_inst);
    if (xfb_on) {
        g_vk.end_xfb(cb, 0, 0, NULL, NULL);
        for (int b = 0; b < 4 && pr->vkey.xfb_count[b]; b++) gpuw_note(st.xfb_addr[b], st.xfb_size[b]);
    }
    if (gp_q) { vkCmdEndQuery(cb, gps_qp, (uint32_t)gps_n); gp_stat[gp_n - 1] = gps_n + 1; gps_n++; }
    if (exp_hit && exp_mode == 2) apply_scissor();
    stat_draws++;
    g_draws_total++;
    cum_draws++;
    if (g_gpuprof_frame && gp_n && !gp_draw_marks) {
        if (!gp_draws[gp_n - 1]) {  // first draw of the row: its fragment shader hash
            size_t l = strlen(gp_label[gp_n - 1]);
            snprintf(gp_label[gp_n - 1] + l, sizeof gp_label[0] - l, ", first fs %016llx", (unsigned long long)g_cur_fs_hash);
        }
        gp_draws[gp_n - 1]++;
    }
}

// LARECOMP_MESHLOG=<file>: a census of what the game draws, for the mod editor (Legends Studio): each distinct mesh once
// ("M": its vertex layout - format / stream / offset per attribute, the streams' strides -, index count and a hash of
// its first vertices and indices, to find it among the game's .trmbf / .trmsh) and each distinct place it is drawn at
// ("P": the first 3x4 floats of vertex cbufs 5 and 10, where the object / instance matrices are, at most 256 per mesh),
// with the frame and the wall clock ("T" lines every 60 frames: to line the log up with a screen recording).
static uint64_t ml_fnv(const void *p, size_t n, uint64_t h) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}
static uint64_t ml_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
static void meshlog(Prog *pr, CmdRec *r, bool indexed) {
    static FILE *f;
    static int off;
    if (off) return;
    if (!f) {
        const char *path = ENV("LARECOMP_MESHLOG");
        if (!path || !(f = fopen(path, "w"))) { off = 1; return; }
        setvbuf(f, NULL, _IOFBF, 1 << 20);
        fprintf(f, "# LARECOMP_MESHLOG: M <frame> <ms> <mesh> ..., P <frame> <ms> <mesh> inst <n> cb5 <12 floats> cb10 <12 floats>\n");
    }
    static uint64_t *mesh_keys, *place_keys;
    static uint16_t *mesh_places;
    enum { MESH_BITS = 20, PLACE_BITS = 23 };
    if (!mesh_keys) {
        mesh_keys = calloc((size_t)1 << MESH_BITS, 8), mesh_places = calloc((size_t)1 << MESH_BITS, 2);
        place_keys = calloc((size_t)1 << PLACE_BITS, 8);
        if (!mesh_keys || !mesh_places || !place_keys) { off = 1; return; }
    }
    static uint64_t last_t;
    uint64_t frame = renderer_present_count();
    if (frame >= last_t + 60 || !last_t) { last_t = frame ? frame : 1; fprintf(f, "T %llu %llu\n", (unsigned long long)frame, (unsigned long long)ml_ms()); fflush(f); }
    if (!st.nattrib || !st.vbuf[0].addr || st.vbuf[0].size < 16) return;
    uint64_t key = 0xcbf29ce484222645ull;
    for (int i = 0; i < st.nattrib; i++) if ((pr->vi->attr_mask >> i) & 1) key = ml_fnv(&st.attrib[i], 4, key);
    for (int s = 0; s < st.nstream && s < 4; s++) key = ml_fnv(&st.stream[s][0], 4, key);
    uint32_t stride0 = st.stream[0][0] ? st.stream[0][0] : 16;
    uint64_t count = r->x[2];
    key = ml_fnv(&st.vbuf[0], sizeof st.vbuf[0], key);  // (where its vertices / indices are, not what they hold: geometry
    key = ml_fnv(&count, 8, key);                       // rewritten every frame would be new each time)
    if (indexed) key = ml_fnv(&r->x[3], 8, key);
    if (!key) key = 1;
    size_t mi = (size_t)(key >> (64 - MESH_BITS));
    static size_t nmesh, nplace;
    static uint64_t bytes;
    if (bytes > (3ull << 30)) { fprintf(f, "# 3 GB: stopped\n"); fclose(f); off = 1; return; }
    for (size_t n = 0; mesh_keys[mi] && mesh_keys[mi] != key; n++) {  // (bounded: a full table never loops for ever)
        if (n > 64) return;
        mi = (mi + 1) & (((size_t)1 << MESH_BITS) - 1);
    }
    uint64_t ms = ml_ms();
    if (!mesh_keys[mi]) {
        if (nmesh >= ((size_t)1 << MESH_BITS) / 2) return;  // (half full: no more new ones)
        nmesh++;
        mesh_keys[mi] = key;
        uint64_t vh = 0, ih = 0;
        uint32_t isz = 0;
        size_t vn = st.vbuf[0].size < (uint64_t)stride0 * 4 ? (size_t)st.vbuf[0].size : (size_t)stride0 * 4;
        vh = ml_fnv((const void *)(uintptr_t)st.vbuf[0].addr, vn, 0xcbf29ce484222645ull);
        if (indexed && r->x[3]) {
            isz = (uint32_t)r->x[1] == 1 ? 2 : (uint32_t)r->x[1] == 0 ? 1 : 4;
            ih = ml_fnv((const void *)(uintptr_t)r->x[3], (size_t)(count < 12 ? count : 12) * isz, 0xcbf29ce484222645ull);
        }
        bytes += 400;
        fprintf(f, "M %llu %llu %016llx vs %llx count %llu isz %u base %lld inst %llu vbsize %llu vbhash %016llx ibhash %016llx strides",
                (unsigned long long)frame, (unsigned long long)ms, (unsigned long long)key, (unsigned long long)st.program->stages[0].code_addr,
                (unsigned long long)count, isz, indexed ? (long long)(int32_t)r->x[4] : 0, (unsigned long long)(indexed ? r->x[6] : r->x[4]),
                (unsigned long long)st.vbuf[0].size, (unsigned long long)vh, (unsigned long long)ih);
        for (int s = 0; s < st.nstream && s < 4; s++) fprintf(f, " %u", st.stream[s][0]);
        fprintf(f, " attrs");
        for (int i = 0; i < st.nattrib; i++)
            if ((pr->vi->attr_mask >> i) & 1) fprintf(f, " a%d=f%#x/s%u/+%u", i, st.attrib[i] & 0xFF, (st.attrib[i] >> 8) & 0x1F, st.attrib[i] >> 13);
        const uint8_t *v = (const uint8_t *)(uintptr_t)st.vbuf[0].addr;  // (the first vertex's bytes)
        fprintf(f, " v0");
        for (uint32_t b = 0; b < stride0 && b < 64 && b < st.vbuf[0].size; b++) fprintf(f, "%s%02x", b ? "" : " ", v[b]);
        fprintf(f, "\n");
    }
    if (mesh_places[mi] >= 256) return;
    float m[2][12];
    int cbs[2] = {2, 7};  // (vertex cbufs 5 and 10: st.ubuf[0][c - 3])
    for (int k = 0; k < 2; k++) {
        Range u = st.ubuf[0][cbs[k]];
        if (((pr->vi->cbuf_mask >> (cbs[k] + 3)) & 1) && u.addr && u.size >= 48) memcpy(m[k], (const void *)(uintptr_t)u.addr, 48);
        else memset(m[k], 0, 48);
    }
    int32_t q[24];
    for (int k = 0; k < 24; k++) q[k] = (int32_t)lrintf(((float *)m)[k] * 100.0f);  // (1 cm / 0.01 steps: one record per place)
    uint64_t pk = ml_fnv(q, sizeof q, key);
    if (!pk) pk = 1;
    size_t pi = (size_t)(pk >> (64 - PLACE_BITS));
    for (size_t n = 0; place_keys[pi] && place_keys[pi] != pk; n++) {
        if (n > 64) return;
        pi = (pi + 1) & (((size_t)1 << PLACE_BITS) - 1);
    }
    if (place_keys[pi] || nplace >= ((size_t)1 << PLACE_BITS) / 2) return;
    nplace++;
    bytes += 250;
    place_keys[pi] = pk;
    mesh_places[mi]++;
    fprintf(f, "P %llu %llu %016llx inst %llu cb5", (unsigned long long)frame, (unsigned long long)ms, (unsigned long long)key,
            (unsigned long long)(indexed ? r->x[6] : r->x[4]));
    for (int k = 0; k < 12; k++) fprintf(f, " %.4g", m[0][k]);
    fprintf(f, " cb10");
    for (int k = 0; k < 12; k++) fprintf(f, " %.4g", m[1][k]);
    fprintf(f, "\n");
}

// LARECOMP_SKINPROBE=<file>: exploded characters, as they happen. Z-A skins its characters in compute: the draws read
// the posed vertices from their own streams (s0 float3 positions, s1 / s2 packed normals / tangents) - "pos" lines: a
// draw whose sampled positions (512 at most) reach more than 20 m from their middle. Draws of the unposed mesh with its
// bones (a12 u8x4 indices, a13 unorm16x4 weights) and a dynamically indexed palette - "bones" lines: a matrix not
// finite, scaled over 10x or 20 m from the palette's middle, or an index past its end. At most once a second per
// program and buffer, with the frame and the wall clock to find it in a recording. Palettes are read as 3x4 row-major
// matrices (translation in floats 3, 7, 11).
static int sp_cmp(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }
static void skinprobe(Prog *pr, CmdRec *r) {
    static FILE *f;
    static int off;
    if (off) return;
    if (!f) {
        if (!(f = fopen(ENV("LARECOMP_SKINPROBE"), "w"))) { off = 1; return; }
        fprintf(f, "# LARECOMP_SKINPROBE: pos <frame> <ms> ... / bones <frame> <ms> ...\n");
        fflush(f);
    }
    static struct { uint64_t key, frame; } seen[256];
    uint64_t frame = renderer_present_count();
    if (st.nattrib > 1 && (st.attrib[0] & 0xFF) == 0x22 && ((st.attrib[0] >> 8) & 0x1F) == 0 && st.stream[0][0] == 12 &&
        (st.attrib[1] & 0xFF) == 0x69 && ((st.attrib[1] >> 8) & 0x1F) == 1 && st.vbuf[0].addr && st.vbuf[0].size >= 36) {
        const float *p = (const float *)(uintptr_t)st.vbuf[0].addr;
        uint64_t nv = st.vbuf[0].size / 12, step = nv > 512 ? nv / 512 : 1;
        static float xs[512], ys[512], zs[512], ox[512], oy[512], oz[512];
        int n = 0;
        for (uint64_t v = 0; v < nv && n < 512; v += step, n++) xs[n] = ox[n] = p[v * 3], ys[n] = oy[n] = p[v * 3 + 1], zs[n] = oz[n] = p[v * 3 + 2];
        qsort(xs, (size_t)n, 4, sp_cmp), qsort(ys, (size_t)n, 4, sp_cmp), qsort(zs, (size_t)n, 4, sp_cmp);
        float mx = xs[n / 2], my = ys[n / 2], mz = zs[n / 2], far = 0;
        static float ds[512], dsort[512];
        for (int i = 0; i < n; i++) {
            float dx = ox[i] - mx, dy = oy[i] - my, dz = oz[i] - mz;
            ds[i] = dsort[i] = dx * dx + dy * dy + dz * dz;
        }
        qsort(dsort, (size_t)n, 4, sp_cmp);
        float d90 = dsort[n * 9 / 10], lim = d90 * 64.0f > 25.0f ? d90 * 64.0f : 25.0f;  // (squared: 8 x the 90% spread, 5 m at least)
        int out = 0, worst = -1;
        for (int i = 0; i < n; i++) {
            float d = ds[i];
            if (!(d == d) || d > lim) { out++; if (!(d <= far)) far = d, worst = i; }
        }
        if (out) {
            uint64_t key = st.program->stages[0].code_addr ^ st.vbuf[0].addr * 31;
            int slot = (int)(key % 256);
            if (!(seen[slot].key == key && frame < seen[slot].frame + 60)) {
                seen[slot].key = key, seen[slot].frame = frame;
                fprintf(f, "pos %llu %llu vs %llx vb %llx verts %llu sampled %d out %d spread90 %.2f m middle %.2f %.2f %.2f farthest %.1f m (%.2f %.2f %.2f) count %llu inst %llu\n",
                        (unsigned long long)frame, (unsigned long long)ml_ms(), (unsigned long long)st.program->stages[0].code_addr,
                        (unsigned long long)st.vbuf[0].addr, (unsigned long long)nv, n, out, sqrtf(d90), mx, my, mz, sqrtf(far),
                        worst >= 0 ? ox[worst] : 0, worst >= 0 ? oy[worst] : 0, worst >= 0 ? oz[worst] : 0,
                        (unsigned long long)r->x[2], (unsigned long long)r->x[6]);
                fflush(f);
            }
        }
        return;
    }
    if (!((pr->vi->attr_mask >> 12) & 1)) return;
    for (int c = 0; c < 18; c++) {
        if (!((pr->vi->cbuf_dynamic >> (c + 3)) & 1) || !st.ubuf[0][c].addr || st.ubuf[0][c].size < 48) continue;
        const float *u = (const float *)(uintptr_t)st.ubuf[0][c].addr;
        int n = (int)(st.ubuf[0][c].size / 48);
        if (n > 1024) n = 1024;
        static float tx[1024], ty[1024], tz[1024];
        for (int i = 0; i < n; i++) tx[i] = u[i * 12 + 3], ty[i] = u[i * 12 + 7], tz[i] = u[i * 12 + 11];
        float sx[1024], sy[1024], sz[1024];
        memcpy(sx, tx, (size_t)n * 4), memcpy(sy, ty, (size_t)n * 4), memcpy(sz, tz, (size_t)n * 4);
        qsort(sx, (size_t)n, 4, sp_cmp), qsort(sy, (size_t)n, 4, sp_cmp), qsort(sz, (size_t)n, 4, sp_cmp);
        float mx = sx[n / 2], my = sy[n / 2], mz = sz[n / 2];
        int bad = 0, first[8], nf = 0;
        for (int i = 0; i < n; i++) {
            const float *m = u + i * 12;
            bool b = false;
            for (int k = 0; k < 12; k++) if (!(m[k] == m[k]) || fabsf(m[k]) > 1e5f) b = true;
            for (int row = 0; row < 3 && !b; row++)
                if (m[row * 4] * m[row * 4] + m[row * 4 + 1] * m[row * 4 + 1] + m[row * 4 + 2] * m[row * 4 + 2] > 100.0f) b = true;
            float dx = m[3] - mx, dy = m[7] - my, dz = m[11] - mz;
            if (!b && dx * dx + dy * dy + dz * dz > 400.0f) b = true;
            if (b) { bad++; if (nf < 8) first[nf++] = i; }
        }
        // bone indices past the palette (a12 u8x4 / a 16-bit form: the first 256 vertices of its stream)
        int over = 0, maxidx = -1;
        uint32_t a = st.attrib[12];
        int sidx = (a >> 8) & 0x1F;
        uint32_t stride = st.stream[sidx][0], off11 = a >> 13, fmt = a & 0xFF;
        const uint8_t *vb = (const uint8_t *)(uintptr_t)st.vbuf[sidx].addr;
        int isz = fmt == 0x08 || fmt == 0x09 || fmt == 0x13 || fmt == 0x14 || fmt == 0x2b || fmt == 0x2c ? 2 : 1;
        if (vb && stride)
            for (uint64_t v = 0; v < 256 && (v + 1) * stride <= st.vbuf[sidx].size; v++)
                for (int k = 0; k < 4; k++) {
                    int idx = isz == 2 ? ((const uint16_t *)(vb + v * stride + off11))[k] : vb[v * stride + off11 + k];
                    if (idx > maxidx) maxidx = idx;
                    if (idx >= n) over++;
                }
        if (!bad && !over) continue;
        uint64_t key = st.program->stages[0].code_addr ^ st.ubuf[0][c].addr * 31;
        int slot = (int)(key % 256);
        if (seen[slot].key == key && frame < seen[slot].frame + 60) continue;
        seen[slot].key = key, seen[slot].frame = frame;
        fprintf(f, "bones %llu %llu vs %llx cb %d bones %d bad %d middle %.2f %.2f %.2f | a12 fmt %#x max index %d past-end %d | count %llu inst %llu |",
                (unsigned long long)frame, (unsigned long long)ml_ms(), (unsigned long long)st.program->stages[0].code_addr, c + 3, n, bad,
                mx, my, mz, fmt, maxidx, over, (unsigned long long)r->x[2], (unsigned long long)r->x[6]);
        for (int k = 0; k < nf; k++) {
            const float *m = u + first[k] * 12;
            fprintf(f, " #%d t %.2f %.2f %.2f rows %.3f %.3f %.3f", first[k], m[3], m[7], m[11], m[0], m[5], m[10]);
        }
        fprintf(f, "\n");
        fflush(f);
    }
}

static void debug_draw(Prog *pr, CmdRec *r, bool indexed) {
    if (ENV("LARECOMP_LOG_DET")) {  // (research: the determinant of the instance matrix rows in vertex cbuf 10, per draw)
        float d[2] = {0, 0};
        uint64_t ad[2] = {0, 0};
        for (int k = 0; k < 2; k++) {
            Range u = st.ubuf[0][k ? 7 : 2];
            if (!u.addr || u.size < 48) continue;
            const float *m = (const float *)(uintptr_t)u.addr;
            ad[k] = u.addr;
            d[k] = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
        }
        // winding against the stored normals (a0 = float3 position, a1 = snorm8 normal, same stream): CCW triangles whose
        // normal agrees, of the first 64
        int agree = -1, tot = 0;
        if (indexed && st.nattrib > 1 && (st.attrib[0] & 0xFF) == 0x22 && (st.attrib[1] & 0xFF) == 0x29 &&
            ((st.attrib[0] >> 8) & 0x1F) == ((st.attrib[1] >> 8) & 0x1F)) {
            int sidx = (st.attrib[0] >> 8) & 0x1F;
            uint32_t stride = st.stream[sidx][0], o0 = st.attrib[0] >> 13, o1 = st.attrib[1] >> 13;
            const uint8_t *vb = (const uint8_t *)(uintptr_t)st.vbuf[sidx].addr;
            const void *ib = (const void *)(uintptr_t)r->x[3];
            uint32_t isz = (uint32_t)r->x[1] == 1 ? 2 : 4;
            int32_t bv = (int32_t)r->x[4];
            agree = 0;
            for (uint32_t t = 0; vb && ib && t + 2 < r->x[2] && tot < 64; t += 3, tot++) {
                float P[3][3], N[3] = {0, 0, 0};
                for (int k = 0; k < 3; k++) {
                    uint32_t id = (isz == 2 ? ((const uint16_t *)ib)[t + k] : ((const uint32_t *)ib)[t + k]) + bv;
                    memcpy(P[k], vb + (uint64_t)id * stride + o0, 12);
                    const int8_t *n = (const int8_t *)(vb + (uint64_t)id * stride + o1);
                    for (int j = 0; j < 3; j++) N[j] += n[j];
                }
                float e1[3], e2[3], g[3];
                for (int j = 0; j < 3; j++) e1[j] = P[1][j] - P[0][j], e2[j] = P[2][j] - P[0][j];
                g[0] = e1[1] * e2[2] - e1[2] * e2[1], g[1] = e1[2] * e2[0] - e1[0] * e2[2], g[2] = e1[0] * e2[1] - e1[1] * e2[0];
                agree += g[0] * N[0] + g[1] * N[1] + g[2] * N[2] > 0;
            }
        }
        LOG("[det] draw %llu vs %llx count %llu agree %d/%d cb5 %llx det5 %.4f cb10 %llx det10 %.4f poly %02x%02x\n", (unsigned long long)g_draws_total,
            (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)r->x[2], agree, tot, (unsigned long long)ad[0], d[0],
            (unsigned long long)ad[1], d[1], st.poly[0], st.poly[1]);
    }
    LOG("[draw] %s inst %llu mode %llu count %llu prog vs=%llx fs=%llx | vs cb %#x fs cb %#x | rt %d", indexed ? "e" : "a",
        (unsigned long long)(indexed ? r->x[6] : r->x[4]), (unsigned long long)r->x[0], (unsigned long long)r->x[2],
        (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr,
        pr->vi->cbuf_mask, pr->fi->cbuf_mask, st.rt.ncolor);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) LOG(" %ux%u/f%#x", st.rt.color[i]->width, st.rt.color[i]->height, st.rt.color[i]->format);
    if (st.rt.depth) LOG(" d%#x %p", st.rt.depth->format, (void *)st.rt.depth);
    LOG(" | vp %.0f %.0f %.0f %.0f | attrs %#x blend %#x ds %02x%02x%02x poly %02x%02x off %x (%.3g %.3g %.3g) chmask %08x\n",
        st.viewport[0][0], st.viewport[0][1], st.viewport[0][2], st.viewport[0][3], pr->vi->attr_mask, st.color_state[0], st.ds[0],
        st.ds[1], st.ds[2], st.poly[0], st.poly[1], st.poly[3], st.poly_offset[0], st.poly_offset[1], st.poly_offset[2], st.channel_mask);
    if (ENV("LARECOMP_DEBUG_CBUF_DUMP")) {  // (debug frames: the fragment stage's cbufs 2..17 raw, build/cb_<draw>_c<k>.bin)
        static uint64_t last_frame = ~0ull;
        static int nd;
        if (renderer_present_count() != last_frame) last_frame = renderer_present_count(), nd = 0;
        for (int k = 3; k < 18; k++) {
            Range r = st.ubuf[1][k - 3];
            if (!r.addr || !((pr->fi->cbuf_mask >> k) & 1)) continue;
            uint64_t size = r.size ? r.size : 0x1000;
            if (size > 0x10000) size = 0x10000;
            char path[128];
            snprintf(path, sizeof path, "build/cb_%04d_c%d.bin", nd, k);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite((const void *)(uintptr_t)r.addr, 1, size, f); fclose(f); }
        }
        LOG("[draw]   cbufs dumped as build/cb_%04d_c*.bin\n", nd++);
    }
    LOG("[draw]   attribs:");  // format / stream / offset per attribute, and the streams' strides
    for (int i = 0; i < st.nattrib; i++)
        if ((pr->vi->attr_mask >> i) & 1)
            LOG(" a%d=f%#x/s%u/+%u", i, st.attrib[i] & 0xFF, (st.attrib[i] >> 8) & 0x1F, st.attrib[i] >> 13);
    for (int s = 0; s < st.nstream && s < 4; s++) LOG(" | stream%d stride %u", s, st.stream[s][0]);
    LOG("\n");
    if (ENV("LARECOMP_DUMP_SKIN") && ((pr->vi->attr_mask >> 11) & 1)) {  // one skinned draw's inputs -> build/skin/
        static int dumped;
        const char *at_frame = ENV("LARECOMP_DUMP_SKIN_FRAME");  // (the first one from that frame on)
        if (at_frame ? renderer_present_count() >= (uint64_t)atoll(at_frame) && !dumped++
                     : ++dumped == (atoi(ENV("LARECOMP_DUMP_SKIN")) > 1 ? atoi(ENV("LARECOMP_DUMP_SKIN")) : 1)) {  // (or the Nth skinned draw)
            mkdir("build/skin", 0755);
            char path[256];
            FILE *f;
            snprintf(path, sizeof path, "build/skin/info.txt");
            if ((f = fopen(path, "w"))) {
                fprintf(f, "vs %llx stride %u\n", (unsigned long long)st.program->stages[0].code_addr, st.stream[0][0]);
                for (int i = 0; i < st.nattrib; i++)
                    if ((pr->vi->attr_mask >> i) & 1)
                        fprintf(f, "attr %d fmt %#x off %u\n", i, st.attrib[i] & 0xFF, st.attrib[i] >> 13);
                for (int c = 0; c < 18; c++)
                    if ((pr->vi->cbuf_mask >> (c + 3)) & 1)
                        fprintf(f, "cb %d size %u\n", c + 3, (unsigned)st.ubuf[0][c].size);
                fclose(f);
            }
            uint64_t vsz = (uint64_t)st.stream[0][0] * 64;
            if (st.vbuf[0].addr && (f = fopen("build/skin/verts.bin", "wb"))) { fwrite((void *)(uintptr_t)st.vbuf[0].addr, 1, vsz < st.vbuf[0].size ? vsz : st.vbuf[0].size, f); fclose(f); }
            for (int c = 0; c < 18; c++) {
                if (!((pr->vi->cbuf_mask >> (c + 3)) & 1) || !st.ubuf[0][c].addr) continue;
                snprintf(path, sizeof path, "build/skin/cb%d.bin", c + 3);
                if ((f = fopen(path, "wb"))) { fwrite((void *)(uintptr_t)st.ubuf[0][c].addr, 1, st.ubuf[0][c].size, f); fclose(f); }
            }
            {   // compiler constants (cb1) live after the shader code
                NvnShaderStage *ss = &st.program->stages[0];
                if (ss->const_offset && (f = fopen("build/skin/cb1.bin", "wb"))) {
                    fwrite((void *)(uintptr_t)(ss->code_addr + ss->const_offset), 1, ss->const_size ? ss->const_size : 256, f);
                    fclose(f);
                }
            }
            LOG("[skin] dumped draw inputs of vs %llx to build/skin/\n", (unsigned long long)st.program->stages[0].code_addr);
            extern uint64_t g_skin_watch;  // (renderer_stats: is this cbuf filled later, by the GPU?)
            g_skin_watch = st.ubuf[0][0].addr;
            const uint32_t *w = (const uint32_t *)(uintptr_t)g_skin_watch;
            LOG("[skin] cb3 %llx at draw: row8 %08x %08x row9 %08x %08x\n", (unsigned long long)g_skin_watch, w[32], w[33], w[36], w[37]);
        }
    }
    if (ENV("LARECOMP_DEBUG_VSCB"))
        LOG("[draw]   vbuf0 %llx+%llu vbuf1 %llx+%llu\n", (unsigned long long)st.vbuf[0].addr,
            (unsigned long long)st.vbuf[0].size, (unsigned long long)st.vbuf[1].addr, (unsigned long long)st.vbuf[1].size);
    if (ENV("LARECOMP_DEBUG_VSCB") && st.vbuf[0].addr) {  // first vertices: bytes of attrs 11 / 12
        const uint8_t *v = (const uint8_t *)(uintptr_t)st.vbuf[0].addr;
        uint32_t stride = st.stream[0][0];
        for (int i = 0; i < 4 && stride; i++) {
            const uint8_t *p = v + (uint64_t)i * stride;
            LOG("[draw]   v%d pos %.3f %.3f %.3f", i, ((const float *)p)[0], ((const float *)p)[1], ((const float *)p)[2]);
            for (int a = 11; a <= 12; a++) {
                if (!((pr->vi->attr_mask >> a) & 1)) continue;
                const uint8_t *q = p + (st.attrib[a] >> 13);
                LOG(" a%d %u %u %u %u", a, q[0], q[1], q[2], q[3]);
            }
            LOG("\n");
        }
    }
    for (int c = 0; c < 18 && ENV("LARECOMP_DEBUG_VSCB"); c++) {  // dynamically indexed vertex cbufs (bone palettes)
        if (!((pr->vi->cbuf_dynamic >> (c + 3)) & 1) || !st.ubuf[0][c].addr) continue;
        const float *u = (const float *)(uintptr_t)st.ubuf[0][c].addr;
        LOG("[draw]   vcb%d %llx size %u:", c, (unsigned long long)st.ubuf[0][c].addr, (unsigned)st.ubuf[0][c].size);
        int n = (int)(st.ubuf[0][c].size / 4), bad = 0, ident = 0;
        float mx = 0;
        for (int k = 0; k < n; k++) {
            float f = u[k];
            if (f != f || f > 1e6f || f < -1e6f) bad++;
            else if (fabsf(f) > mx) mx = fabsf(f);
        }
        for (int m = 0; m + 16 <= n; m += 16) {
            bool id = true;
            for (int k = 0; k < 16; k++) if (u[m + k] != ((k % 5) == 0 ? 1.0f : 0.0f)) id = false;
            ident += id;
        }
        LOG(" floats %d, non-finite/huge %d, max |x| %.4g, identity 4x4 %d of %d | m1:", n, bad, mx, ident, n / 16);
        for (int k = 16; k < 32 && k < n; k++) LOG(" %.4g", u[k]);
        LOG(" | m5:");
        for (int k = 80; k < 96 && k < n; k++) LOG(" %.4g", u[k]);
        LOG("\n");
    }
    if (ENV("LARECOMP_DEBUG_DATA")) {  // vertex streams 0-1 and fragment cbufs: the first floats
        for (int s = 0; s < 2; s++) {
            if (!st.vbuf[s].addr) continue;
            const float *v = (const float *)(uintptr_t)st.vbuf[s].addr;
            LOG("[draw]   vbuf%d %llx size %llu:", s, (unsigned long long)st.vbuf[s].addr, (unsigned long long)st.vbuf[s].size);
            for (int k = 0; k < 16; k++) LOG(" %.4g", v[k]);
            LOG("\n");
        }
        for (int c = 3; c < 21; c++) {  // (guest cbuf c: slot c - 3, as bind_ubo does)
            if (!((pr->fi->cbuf_mask >> c) & 1) || !st.ubuf[1][c - 3].addr) continue;
            const float *u = (const float *)(uintptr_t)st.ubuf[1][c - 3].addr;
            LOG("[draw]   fcb%d %llx size %u:", c, (unsigned long long)st.ubuf[1][c - 3].addr, (unsigned)st.ubuf[1][c - 3].size);
            for (int k = 0; k < 96 && k * 4 < (int)st.ubuf[1][c - 3].size; k++) LOG(" %.6g", u[k]);
            LOG("\n");
        }
    }
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {  // fragment textures: guest size / format, allocation, scale
        NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
        const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
        if (!t) { LOG("[draw]   tex%d: none\n", i); continue; }
        LOG("[draw]   tex%d: %p %ux%u f%#x lv %u -> %ux%u scale %.4f %.4f%s | target %u flags %#x layers %u bh %u addr %llx "
            "stride %llu%s%s '%s'\n", i, (void *)t, t->width, t->height, t->format,
            b ? b->levels : 0, b ? b->width : 0, b ? b->height : 0, b ? b->sx : 0, b ? b->sy : 0, b && b->scalable ? " scalable" : "",
            t->target, t->flags, t->layout.layers, t->layout.block_height_log2, (unsigned long long)t->addr,
            (unsigned long long)t->stride, b && b->gpu_owned ? " gpu-owned" : "", b && b->dynamic ? " dynamic" : "", t->label);
    }
}

// ================================================================== texture copies
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } TexRefRec;

static void copy_region_dims(NvnTexture *t, const TexRefRec *ref, const FormatInfo **fi, int32_t rg[6], int *level,
                             uint64_t *row, uint64_t *img) {
    *fi = nvn_format_info(t->format);
    memcpy(rg, ref->region, sizeof ref->region);
    if (!rg[3]) rg[3] = (int32_t)t->width;
    if (!rg[4]) rg[4] = (int32_t)t->height;
    if (!rg[5]) rg[5] = 1;
    *level = ref->has_view && (ref->view.setmask & 1) ? ref->view.base_level : 0;
    uint32_t wb = ((uint32_t)rg[3] + (*fi)->bw - 1) / (*fi)->bw, hb = ((uint32_t)rg[4] + (*fi)->bh - 1) / (*fi)->bh;
    *row = st.copy_row_stride ? st.copy_row_stride : (uint64_t)wb * (*fi)->bpb;
    *img = st.copy_image_stride ? st.copy_image_stride : *row * hb;
}

static void copy_buffer_to_texture(uint64_t src, const TexRefRec *dst) {
    snap_reason = "buf2tex"; snapshots_invalidate(NULL);
    NvnTexture *t = (NvnTexture *)(uintptr_t)dst->tex;
    if (!t || !src) return;
    vk_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, dst, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb) return;
    if (ENV("LARECOMP_LOG_BUF2TEX") && (t->format == 0x1 || t->format == 0xd)) {  // (movie planes)
        static int n;
        if (n++ < 12) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)src;
            unsigned sum = 0;
            for (int i = 0; i < 4096; i++) sum += p[(uint64_t)i * 97 % (row * (uint64_t)rg[4] ? row * (uint64_t)rg[4] : 1)];
            LOG("[vk] buf2tex src %llx -> %ux%u fmt %#x region %d,%d %dx%d row %llu sample-sum %u\n",
                (unsigned long long)src, t->width, t->height, t->format, rg[0], rg[1], rg[3], rg[4],
                (unsigned long long)row, sum);
        }
    }
    extern void vk_tex_unscale(NvnTexture *t);
    vk_tex_unscale(t);  // the copied data is full size: no output-resolution scaling for this texture
    TexBackend *b = vk_tex(t, false);  // prior CPU contents uploaded first
    renderer_mark_gpu_owned(t);
    if (b->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) return;
    if (b->astc_bw) {  // (blocks would need the CPU decode: not seen in the game yet)
        static int nlog;
        if (nlog++ < 5) LOG("[vk] buffer -> ASTC texture copy skipped (%ux%u)\n", t->width, t->height);
        return;
    }
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t hb = ((uint32_t)rg[4] + fi->bh - 1) / fi->bh, wb = ((uint32_t)rg[3] + fi->bw - 1) / fi->bw;
    uint32_t out_bpb = fi->bpb == 3 ? 4 : fi->bpb;
    uint32_t nz = (uint32_t)rg[5];
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    uint64_t out_row = (uint64_t)wb * out_bpb;
    VkSpan s = vk_transient(out_row * hb * nz, 16);
    for (uint32_t z = 0; z < nz; z++)
        for (uint32_t y = 0; y < hb; y++) {
            const uint8_t *sr = (const uint8_t *)(uintptr_t)(src + (uint64_t)z * img + (uint64_t)y * row);
            uint8_t *dr = (uint8_t *)s.ptr + ((uint64_t)z * hb + y) * out_row;
            if (out_bpb == fi->bpb) memcpy(dr, sr, out_row);
            else for (uint32_t x = 0; x < wb; x++) { memcpy(dr + x * 4, sr + x * 3, 3); dr[x * 4 + 3] = 255; }
        }
    VkBufferImageCopy c = {.bufferOffset = s.off, .bufferRowLength = wb * fi->bw, .bufferImageHeight = hb * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}

static void copy_texture_to_buffer(const TexRefRec *src, uint64_t dst) {
    NvnTexture *t = (NvnTexture *)(uintptr_t)src->tex;
    if (!t || !dst || dst < HEAP_BASE) return;
    vk_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, src, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb || fi->bpb == 3 || row % fi->bpb) return;
    TexBackend *b = vk_tex(t, false);
    float tsx, tsy;
    renderer_texture_scale(t, &tsx, &tsy);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImage srcimg = b->image;
    VmaAllocation small_mem = NULL;
    if ((tsx != 1 || tsy != 1) && level == 0 && !renderer_format_is_depth(t->format)) {
        // the CPU expects guest-sized texels: filter the scaled allocation down first
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = b->format,
                                .extent = {t->width, t->height, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &srcimg, &small_mem, NULL));
        vk_image_init_layout(cb, srcimg, VK_IMAGE_ASPECT_COLOR_BIT);
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)b->width, (int32_t)b->height, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)t->width, (int32_t)t->height, 1}}};
        vkCmdBlitImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, srcimg, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
        vk_full_barrier(cb);
    }
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t nz = (uint32_t)rg[5];
    uint64_t total = img * nz;
    VkSpan out = vk_readback(total);
    VkBufferImageCopy c = {.bufferOffset = out.off, .bufferRowLength = (uint32_t)(row / fi->bpb) * fi->bw,
                           .bufferImageHeight = (uint32_t)(img / row) * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    vkCmdCopyImageToBuffer(cb, srcimg, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();  // the CPU may read the result right after the fence
    memcpy((void *)(uintptr_t)dst, out.ptr, total);
    if (small_mem) vmaDestroyImage(g_vk.vma, srcimg, small_mem);
}

static void copy_texture(const TexRefRec *src, const TexRefRec *dst, uint64_t flags) {
    snap_reason = "tex2tex"; snapshots_invalidate(NULL);
    NvnTexture *st_ = (NvnTexture *)(uintptr_t)src->tex, *dt = (NvnTexture *)(uintptr_t)dst->tex;
    if (!st_ || !dt) return;
    vk_end_pass();
    if (dl_on == 2) renderer_flush();  // (LARECOMP_VK_DEVLOST_TRACE=2: each copy its own submission, waited on)
    gp_mark(vk_cmd(), "copy texture %ux%u -> %ux%u", st_->width, st_->height, dt->width, dt->height);
    dl_note("copy texture %ux%u f%#x -> %ux%u f%#x (%p -> %p)", st_->width, st_->height, st_->format, dt->width, dt->height,
            dt->format, (void *)st_, (void *)dt);
    TexBackend *s = vk_tex(st_, false), *d = vk_tex(dt, true);
    renderer_mark_gpu_owned(dt);
    if (renderer_texture_presented(dt) && st_->width == dt->width && st_->height == dt->height) renderer_texture_mark_presented(st_);
    renderer_texture_first_write(dt);
    renderer_texture_level_fresh(dt);
    const int32_t *sr = src->region, *dr = dst->region;
    float ssx, ssy, dsx, dsy;
    renderer_texture_scale(st_, &ssx, &ssy);
    renderer_texture_scale(dt, &dsx, &dsy);
    int sx0 = (int)lroundf(sr[0] * ssx), sy0 = (int)lroundf(sr[1] * ssy);
    int dx0 = (int)lroundf(dr[0] * dsx), dy0 = (int)lroundf(dr[1] * dsy);
    int sw = sr[3] ? (int)lroundf(sr[3] * ssx) : (int)s->width, sh = sr[4] ? (int)lroundf(sr[4] * ssy) : (int)s->height;
    int dw = dr[3] ? (int)lroundf(dr[3] * dsx) : (int)d->width, dh = dr[4] ? (int)lroundf(dr[4] * dsy) : (int)d->height;
    uint32_t slv = src->has_view && (src->view.setmask & 1) ? src->view.base_level : 0;
    uint32_t dlv = dst->has_view && (dst->view.setmask & 1) ? dst->view.base_level : 0;
    uint32_t sly = src->has_view && (src->view.setmask & 2) ? src->view.min_layer : 0;
    uint32_t dly = dst->has_view && (dst->view.setmask & 2) ? dst->view.min_layer : 0;
    if (slv >= s->levels || dlv >= d->levels || sly >= s->layers || dly >= d->layers) return;
    {   // whole-image regions are the level's extent, and every region stays inside its level: scaled allocations
        // (internal resolution below 1x) round guest regions past the edge, and out-of-bounds copies / blits are
        // undefined in Vulkan (garbage that changed every frame on NVIDIA: flicker in the depth pyramid's users)
        int slw = (int)level_extent(s->width, slv), slh = (int)level_extent(s->height, slv);
        int dlw = (int)level_extent(d->width, dlv), dlh = (int)level_extent(d->height, dlv);
        if (!sr[3]) sw = slw - sx0;
        if (!sr[4]) sh = slh - sy0;
        if (!dr[3]) dw = dlw - dx0;
        if (!dr[4]) dh = dlh - dy0;
        if (sx0 + sw > slw) sw = slw - sx0;
        if (sy0 + sh > slh) sh = slh - sy0;
        if (dx0 + dw > dlw) dw = dlw - dx0;
        if (dy0 + dh > dlh) dh = dlh - dy0;
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    }
    if (g_debug_frame)
        LOG("[copytex] %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] -> %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] phys (%d,%d %dx%d) -> (%d,%d %dx%d) flags %llx\n",
            (void *)st_, st_->width, st_->height, st_->format, slv, sr[0], sr[1], sr[3], sr[4], s->width, s->height, (void *)dt, dt->width,
            dt->height, dt->format, dlv, dr[0], dr[1], dr[3], dr[4], d->width, d->height, sx0, sy0, sw, sh, dx0, dy0, dw, dh,
            (unsigned long long)flags);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImageAspectFlags sa = s->aspect, da = d->aspect;
    if (s->samples > 1 && d->samples == 1 && sw == dw && sh == dh && s->format == d->format && !(sa & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        VkImageResolve rv = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                             .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdResolveImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rv);
        return;
    }
    if (s->samples != d->samples) return;
    if (sw == dw && sh == dh && s->format == d->format) {
        VkImageCopy c = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                         .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        return;
    }
    // scaled / converting copy: the game builds its depth pyramid this way (nearest), colour copies filter if asked
    bool depth = (sa | da) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    if (depth && s->format != d->format) return;
    VkImageBlit bl = {.srcSubresource = {depth ? (sa & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : sa) : sa, slv, sly, 1},
                      .srcOffsets = {{sx0, sy0, 0}, {sx0 + sw, sy0 + sh, 1}},
                      .dstSubresource = {depth ? (da & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : da) : da, dlv, dly, 1},
                      .dstOffsets = {{dx0, dy0, 0}, {dx0 + dw, dy0 + dh, 1}}};
    bool linear = (flags & 1) && !depth && !rt_kind(st_->format);
    dl_note("  blit %p lv%u/%u %ux%u (%d,%d %dx%d) -> %p lv%u/%u %ux%u (%d,%d %dx%d) %s", (void *)s->image, slv, s->levels, s->width,
            s->height, sx0, sy0, sw, sh, (void *)d->image, dlv, d->levels, d->width, d->height, dx0, dy0, dw, dh, linear ? "linear" : "nearest");
    vkCmdBlitImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

// ================================================================== present helpers (device.c)
static VkPipelineLayout present_layout;
static VkDescriptorSetLayout present_set_layout;

static VkPipeline present_pipeline(VkFormat fmt) {
    static struct { VkFormat f; VkPipeline p; } cache[4];
    for (int i = 0; i < 4; i++) if (cache[i].p && cache[i].f == fmt) return cache[i].p;
    if (!present_layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &present_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &present_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &present_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("present.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader("present.frag"), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = present_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 4; i++) if (!cache[i].p) { cache[i].f = fmt; cache[i].p = p; break; }
    return p;
}

// Scanout shows the bytes the game wrote: sRGB images are read through a UNORM view (no decode), like metal.m.
static VkImageView raw_view(TexBackend *b) {
    VkFormat f = b->format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM
               : b->format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : b->format;
    static const VkComponentMapping id = {0};
    return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, f, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

// ---- post passes (Video tab "Anti-aliasing" / "Upscaler"): SMAA 1x at the game image's size, then AMD FSR 1 (EASU
// upscale to the image's size in the window, RCAS sharpen) when it is smaller than its place in the window, as
// metal.m does with MetalFX. Each pass: a fullscreen triangle into an RGBA8 image (gamma-encoded, like the input);
// up to three inputs (bindings 0-2), sampled linear / clamp.
static VkPipelineLayout post_layout;
static VkDescriptorSetLayout post_set_layout;

static VkPipeline post_pipeline(const char *fs) {
    static struct { const char *fs; VkPipeline p; } cache[8];
    for (int i = 0; i < 8; i++) if (cache[i].p && cache[i].fs == fs) return cache[i].p;
    if (!post_layout) {
        VkDescriptorSetLayoutBinding b[3];
        for (uint32_t i = 0; i < 3; i++)
            b[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &post_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &post_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &post_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("post.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fs), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = post_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 8; i++) if (!cache[i].p) { cache[i].fs = fs; cache[i].p = p; break; }
    return p;
}

// Post image `i` at w x h (RGBA8, kept until the size changes). 0, 1: SMAA edges / weights, 2: SMAA output,
// 3, 4: FSR EASU / RCAS, 5, 6: Sharpening's window-size image / RCAS.
static TexBackend *post_image(int i, uint32_t w, uint32_t h) {
    static TexBackend img[7];
    TexBackend *d = &img[i];
    if (d->image && (d->width != w || d->height != h)) {
        vk_views_forget(d);
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = VK_FORMAT_R8G8B8A8_UNORM; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    return d;
}

static VkImageView post_view(TexBackend *d) {
    static const VkComponentMapping id = {0};
    return vk_image_view(d, VK_IMAGE_VIEW_TYPE_2D, d->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

// SMAA's lookup textures (runtime/nvn/smaa_data.c): 0 = area (160 x 560 R8G8), 1 = search (64 x 16 R8), uploaded once.
static VkImageView smaa_lut(int i) {
    static TexBackend t[2];
    TexBackend *d = &t[i];
    if (!d->image) {
        extern const unsigned char *smaa_area_tex(void);
        extern const unsigned char *smaa_search_tex(void);
        uint32_t w = i ? 64 : 160, h = i ? 16 : 560, bpp = i ? 1 : 2;
        VkFormat f = i ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8_UNORM;
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                                .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = f; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        VkCommandBuffer cb = vk_cmd();
        vk_image_init_layout(cb, d->image, d->aspect);
        VkSpan s = vk_transient_copy(i ? smaa_search_tex() : smaa_area_tex(), (uint64_t)w * h * bpp, 16);
        VkBufferImageCopy c = {.bufferOffset = s.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
        vkCmdCopyBufferToImage(cb, s.buf, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    return post_view(d);
}

// One post pass (outside the window image's rendering): srcs (bindings 0..n-1) -> dst, fragment shader `fs`.
// clear: start from zero (SMAA's edge pass only writes edge pixels; its weights / output read them).
static void post_pass(VkCommandBuffer cb, const char *fs, const VkImageView *srcs, int n, TexBackend *dst, const float pc[8],
                      bool clear) {
    g_dyn_epoch++;  // (draw_'s state cache: dc)
    VkPipeline pipe = post_pipeline(fs);  // (before the rendering begins: created inside it, the NVIDIA driver crashed)
    vk_full_barrier(cb);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = post_view(dst),
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {dst->width, dst->height}},
                          .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkDescriptorSet ds = vk_alloc_set(post_set_layout);
    VkDescriptorImageInfo ii[3];
    VkWriteDescriptorSet w[3];
    for (int i = 0; i < n; i++) {
        ii[i] = (VkDescriptorImageInfo){vk_default_sampler(true), srcs[i], VK_IMAGE_LAYOUT_GENERAL};
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = (uint32_t)i,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      .pImageInfo = &ii[i]};
    }
    vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, post_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)dst->width, (float)dst->height, 0, 1};
    VkRect2D sc = {{0, 0}, {dst->width, dst->height}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdPushConstants(cb, post_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
}

// The view the present samples: the game image, or its anti-aliased / upscaled version. ow x oh: the whole image's
// size in the window (its crop is drawn into the viewport).
static VkImageView post_process(VkCommandBuffer cb, TexBackend *b, double ow_d, double oh_d) {
    VkImageView v = raw_view(b);
    bool smaa = g_settings.antialias == AA_SMAA;
    uint32_t ow = (uint32_t)lround(ow_d), oh = (uint32_t)lround(oh_d);
    if (ow > 16384) ow = 16384;
    if (oh > 16384) oh = 16384;
    bool fsr = g_settings.upscaler == UPSCALE_FSR1 && (ow > b->width + 1 || oh > b->height + 1);  // (an upscale only)
    // Sharpening (Settings.sharpen 1..3): RCAS at the window's size - after the render's reduction to it when it is
    // bigger (the area average, as present.frag), so the sharpening is per screen pixel. 1440p on a 1080p screen read
    // soft though the field really renders at 1440 (the game's low-resolution textures; the user, 2026-11-08).
    int sharpen = g_settings.sharpen >= 1 && g_settings.sharpen <= 3 ? g_settings.sharpen : 0;
    // (RCAS: 0 stops = the most. Full strength made faces look cut out - polygon edges dark lines, stubble blown up: the
    // user's Laventon close-up - so High is AMD's FSR default, 0.2 stops, and Low / Medium sit below it)
    static const float k_sharp_stops[4] = {0, 2.0f, 1.0f, 0.2f};
    if (fsr) sharpen = 0;  // (FSR's own RCAS sharpens an upscale)
    if (!smaa && !fsr && !sharpen) return v;
    extern void vk_frame_suspend(void);
    extern void vk_frame_resume(void);
    vk_frame_suspend();
    if (smaa) {  // SMAA 1x: luma edges -> blending weights -> neighbourhood blending
        float w = (float)b->width, h = (float)b->height;
        float m[8] = {1.0f / w, 1.0f / h, w, h};
        TexBackend *edges = post_image(0, b->width, b->height), *weights = post_image(1, b->width, b->height);
        TexBackend *out = post_image(2, b->width, b->height);
        VkImageView in[3] = {v};
        post_pass(cb, "smaa_edge.frag", in, 1, edges, m, true);
        VkImageView wi[3] = {post_view(edges), smaa_lut(0), smaa_lut(1)};
        post_pass(cb, "smaa_weight.frag", wi, 3, weights, m, true);
        VkImageView bi[3] = {v, post_view(weights)};
        post_pass(cb, "smaa_blend.frag", bi, 2, out, m, false);
        v = post_view(out);
    }
    if (fsr) {
        float iw = (float)b->width, ih = (float)b->height;
        TexBackend *e = post_image(3, ow, oh), *r = post_image(4, ow, oh);
        float pe[8] = {iw / ow, ih / oh, 0.5f * iw / ow - 0.5f, 0.5f * ih / oh - 0.5f, iw, ih};
        VkImageView ei[3] = {v};
        post_pass(cb, "easu.frag", ei, 1, e, pe, false);
        float pr[8] = {exp2f(-0.2f), 0, 0, 0, (float)ow, (float)oh};  // (sharpness 0.2 stops: AMD's default)
        VkImageView ri_[3] = {post_view(e)};
        post_pass(cb, "rcas.frag", ri_, 1, r, pr, false);
        v = post_view(r);
    }
    if (sharpen) {
        uint32_t sw = b->width, sh = b->height;
        if (ow + 1 < b->width || oh + 1 < b->height) {  // bigger than the window: reduced to it first
            TexBackend *a = post_image(5, ow, oh);
            float pa[8] = {(float)b->width / ow, (float)b->height / oh, 0, 0, (float)b->width, (float)b->height};
            VkImageView ai[3] = {v};
            post_pass(cb, "area.frag", ai, 1, a, pa, false);
            v = post_view(a);
            sw = ow, sh = oh;
        }
        TexBackend *r = post_image(6, sw, sh);
        float pr[8] = {exp2f(-k_sharp_stops[sharpen]), 0, 0, 0, (float)sw, (float)sh};
        VkImageView ri_[3] = {v};
        post_pass(cb, "rcas.frag", ri_, 1, r, pr, false);
        v = post_view(r);
    }
    vk_full_barrier(cb);
    vk_frame_resume();
    {
        static uint32_t piw, pih, pow_, poh;
        static int psm = -1, pfs = -1, psh = -1;
        if (b->width != piw || b->height != pih || ow != pow_ || oh != poh || smaa != psm || fsr != pfs || sharpen != psh)
            LOG("[post] %ux%u%s%s%s -> %ux%u\n", b->width, b->height, smaa ? " smaa" : "", fsr ? " fsr1" : "",
                sharpen ? " sharpen" : "", ow, oh);
        piw = b->width; pih = b->height; pow_ = ow; poh = oh; psm = smaa; pfs = fsr; psh = sharpen;
    }
    return v;
}

// Inside the window image's rendering (device.c vk_frame_begin): the game image into (vx, vy, vw, vh).
void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8]) {
    g_dyn_epoch++;  // (draw_'s state cache: dc)
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    VkImageView src = post_process(cb, b, pv[6] > 0 ? vw / pv[6] : vw, pv[7] > 0 ? vh / pv[7] : vh);
    VkPipeline p = present_pipeline(g_vk.sc_format);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), src, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {vx, vy, vw, vh, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pv);
    vkCmdDraw(cb, 3, 1, 0, 0);
    VkViewport full = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &full);
}

// Screenshot of a game texture (presented crop applied to build/frame_* shots), via the present shader into RGBA8.
void renderer_save_texture_tex(TexBackend *b, const char *path) {
    g_dyn_epoch++;  // (draw_'s state cache: dc)
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    extern const float *vk_shot_crop(void);
    float crop[4] = {0, 0, 1, 1};
    if (strstr(path, "frame_")) memcpy(crop, vk_shot_crop(), sizeof crop);
    uint32_t w = (uint32_t)(b->width * crop[2]), h = (uint32_t)(b->height * crop[3]);
    if (!w || !h) return;
    vk_end_pass();
    static int raw_rgba = -1;  // LARECOMP_DEBUG_RAW_RGBA=1: RGBA8 targets saved as stored (alpha too), not through the present shader
    if (raw_rgba < 0) raw_rgba = ENV("LARECOMP_DEBUG_RAW_RGBA") != NULL;
    if (raw_rgba && b->format == VK_FORMAT_R8G8B8A8_UNORM && b->layers <= 1) {
        VkCommandBuffer cb = vk_cmd();
        vk_full_barrier(cb);
        VkSpan out = vk_readback((uint64_t)b->width * b->height * 4);
        VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               .imageExtent = {b->width, b->height, 1}};
        vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
        renderer_flush();
        vk_save_png(path, out.ptr, b->width, b->height);
        return;
    }
    if (b->format == VK_FORMAT_R8_UINT) {  // (an id map: copied as it is, layer 0; R = the value, G / B spread it apart)
        VkCommandBuffer cb = vk_cmd();
        vk_full_barrier(cb);
        VkSpan out = vk_readback((uint64_t)b->width * b->height);
        VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               .imageExtent = {b->width, b->height, 1}};
        vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
        renderer_flush();
        uint8_t *rgba = malloc((size_t)b->width * b->height * 4);
        uint32_t hist[256] = {0};
        for (size_t i = 0; i < (size_t)b->width * b->height; i++) {
            uint8_t v = ((const uint8_t *)out.ptr)[i];
            hist[v]++;
            rgba[4 * i] = v, rgba[4 * i + 1] = (uint8_t)(v * 37), rgba[4 * i + 2] = (uint8_t)(v * 91), rgba[4 * i + 3] = 255;
        }
        vk_save_png(path, rgba, b->width, b->height);
        free(rgba);
        char line[1024];
        int n = snprintf(line, sizeof line, "[vk] %s: id values", path);
        for (int v = 0; v < 256 && n < (int)sizeof line - 24; v++)
            if (hist[v]) n += snprintf(line + n, sizeof line - n, " %d:%u", v, hist[v]);
        LOG("%s\n", line);
        return;
    }
    VkImage img;
    VmaAllocation mem;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &img, &mem, NULL));
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    vk_image_init_layout(cb, img, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView tv;
    VK_CHECK(vkCreateImageView(g_vk.dev, &vci, NULL, &tv));
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = tv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkPipeline p = present_pipeline(VK_FORMAT_R8G8B8A8_UNORM);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), raw_view(b), VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    float pv[8] = {renderer_origin_lower_left() && !strstr(path, "window") ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
    vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pv);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    vk_full_barrier(cb);
    VkSpan out = vk_readback((uint64_t)w * h * 4);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    vk_save_png(path, out.ptr, w, h);
    vkDestroyImageView(g_vk.dev, tv, NULL);
    vmaDestroyImage(g_vk.vma, img, mem);
}

// (research, LARECOMP_EXP_DRAW_NS=<n>: the render thread spends n ns more per draw - how much its speed is worth)
static void exp_draw_delay(void) {
    static int64_t ns = -1;
    if (ns < 0) ns = ENV("LARECOMP_EXP_DRAW_NS") ? atol(ENV("LARECOMP_EXP_DRAW_NS")) : 0;
    if (!ns) return;
    uint64_t t0 = SDL_GetTicksNS();
    while (SDL_GetTicksNS() - t0 < (uint64_t)ns) {}
}

// ================================================================== executor (renderer.m exec_list)
static void exec_list(uint64_t handle, int depth) {
    if (depth < 9) g_cur_list[depth] = handle;
    g_cur_depth = depth;
    uint64_t p = handle;
    if (depth == 0) st.obj = 0, st.obj_draw = 0;  // (a submitted list starts with no object; its called lists inherit)
    for (int guard = 0; guard < 10000000; guard++) {
        CmdRec *r = (CmdRec *)(uintptr_t)p;
        // (the records were written by a game thread on another core: fetch ahead, each one was a cache miss here)
        __builtin_prefetch((const void *)(uintptr_t)(p + 256));
        __builtin_prefetch((const void *)(uintptr_t)(p + 512));
        void *x = r->extra;
        if (x) __builtin_prefetch(x);
        if (r->op < CMD_COUNT) op_counts[r->op]++;
        if (ip_replaying && ip_skip_op(r->op)) { p += r->size; continue; }
        switch (r->op) {
        case CMD_END: return;
        case CMD_XFB_BEGIN: st.xfb_active = true; { static int n; if (ENV("LARECOMP_LOG_XFB") && n++ < 3) LOG("[xfb] begin executed\n"); } break;
        case CMD_XFB_END: st.xfb_active = false; break;
        case CMD_XFB_BIND: if (r->x[0] < 4) { st.xfb_addr[r->x[0]] = r->x[1]; st.xfb_size[r->x[0]] = r->x[2]; } break;
        case CMD_SET_RASTER_DISCARD: st.raster_discard = r->x[0] != 0; break;
        case CMD_SET_DEPTH_CLAMP: st.depth_clamp = r->x[0] != 0; break;
        case CMD_OBJECT: st.obj = r->x[0]; st.obj_draw = 0; { static int d; if (ENV("LARECOMP_OBJDBG") && vk_frame_counter() == 1450 && d++ < 80) LOG("[objdbg] depth %d object %llx\n", depth,(unsigned long long)r->x[0]); } break;
        case CMD_JUMP: p = r->x[0]; __builtin_prefetch((const void *)(uintptr_t)(p + 64)); __builtin_prefetch((const void *)(uintptr_t)(p + 128)); continue;
        case CMD_CALL: {
            const uint64_t *h = x;
            uint64_t obj = st.obj;  // (a called list names objects of its own: they end with it)
            uint32_t obj_draw = st.obj_draw;
            // (the called lists are small and each starts in memory not touched yet: fetch their first lines now - the
            // look-ahead above only covers what follows the record being run)
            for (uint64_t i = 0; i < r->x[0] && i < 16; i++) {
                __builtin_prefetch((const void *)(uintptr_t)h[i]);
                __builtin_prefetch((const void *)(uintptr_t)(h[i] + 64));
                __builtin_prefetch((const void *)(uintptr_t)(h[i] + 128));
            }
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) {
                if (i + 16 < r->x[0]) __builtin_prefetch((const void *)(uintptr_t)h[i + 16]);
                st.obj = obj, st.obj_draw = obj_draw;
                exec_list(h[i], depth + 1);
            }
            g_cur_depth = depth;
            st.obj = obj, st.obj_draw = obj_draw;
            break;
        }
        case CMD_BIND_PROGRAM: bind_program_stages((NvnProgram *)(uintptr_t)r->x[0], (uint32_t)r->x[1]); break;
        case CMD_BIND_BLEND: { uint8_t *b = x; memcpy(st.blend[b[0] & 7], b, 8); break; }
        case CMD_BIND_CHANNEL_MASK: memcpy(&st.channel_mask, x, 4); break;
        case CMD_BIND_COLOR: memcpy(st.color_state, x, 4); break;
        case CMD_BIND_DEPTH_STENCIL: memcpy(st.ds, x, 8); break;
        case CMD_BIND_MULTISAMPLE: memcpy(st.ms, x, 0x18); break;
        case CMD_BIND_POLYGON: memcpy(st.poly, x, 4); g_poly_src_list = handle, g_poly_src_rec = p, g_poly_src_depth = depth; break;
        case CMD_BIND_VERTEX_ATTRIB:
            st.nattrib = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.attrib, x, st.nattrib * 4);
            break;
        case CMD_BIND_VERTEX_STREAM:
            st.nstream = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.stream, x, st.nstream * 8);
            break;
        case CMD_BIND_VERTEX_BUFFERS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(&st.vbuf[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_UNIFORM_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 18 && r->x[0] < 6; i++)
                memcpy(&st.ubuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_STORAGE_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 16 && r->x[0] < 6; i++)
                memcpy(&st.sbuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_TEXTURES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 32 && r->x[0] < 6; i++)
                memcpy(&st.tex[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_BIND_IMAGES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 8 && r->x[0] < 6; i++)
                memcpy(&st.img[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_TEXTURE_POOL: st.tpool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_SAMPLER_POOL: st.spool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_RENDER_TARGETS: {
            const struct { uint64_t tex[8]; NvnTextureView views[8]; uint8_t has_view[8]; uint64_t depth;
                           NvnTextureView dview; uint8_t has_dview; } *rt = x;
            RtState prev = st.rt;  // the same targets again: keep the pass (a restart = full barrier + reload)
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = (int)r->x[0] > 8 ? 8 : (int)r->x[0];
            for (int i = 0; i < st.rt.ncolor; i++) {
                st.rt.color[i] = (NvnTexture *)(uintptr_t)rt->tex[i];
                st.rt.cview[i] = rt->views[i];
                st.rt.has_cview[i] = rt->has_view[i];
            }
            st.rt.depth = (NvnTexture *)(uintptr_t)rt->depth;
            st.rt.dview = rt->dview;
            st.rt.has_dview = rt->has_dview;
            static int always = -1;
            if (always < 0) always = ENV("LARECOMP_VK_PASS_RESTART") != NULL;
            if (always || memcmp(&prev, &st.rt, sizeof prev)) st.rt_dirty = true;
            break;
        }
        case CMD_SET_VIEWPORTS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.viewport[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_SCISSORS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.scissor[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_DEPTH_RANGES:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.depth_range[r->x[0] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_BLEND_COLOR: memcpy(st.blend_color, x, 16); break;
        case CMD_SET_STENCIL: {
            int face = (int)r->x[0];
            for (int f = 0; f < 2; f++) {
                if (!(face & (1 << f))) continue;
                if (r->x[6] == 0) st.stencil_ref[f] = (uint32_t)r->x[1];
                else if (r->x[6] == 1) st.stencil_mask[f] = (uint32_t)r->x[1];
                else st.stencil_vmask[f] = (uint32_t)r->x[1];
            }
            break;
        }
        case CMD_SET_POLYGON_OFFSET:
            memcpy(st.poly_offset, r->f, sizeof st.poly_offset);
            if (ENV("LARECOMP_LOG_POLY_OFFSET")) {
                static int n;
                if (n++ < 20) LOG("[poly] offset factor %g units %g clamp %g (x1 %#llx x2 %#llx)\n", r->f[0], r->f[1], r->f[2],
                                  (unsigned long long)r->x[0], (unsigned long long)r->x[1]);
            }
            break;
        case CMD_SET_PRIMITIVE_RESTART: st.prim_restart = r->x[0] != 0; st.restart_index = (uint32_t)r->x[1]; break;
        case CMD_SET_COPY_STRIDES: st.copy_row_stride = r->x[0]; st.copy_image_stride = r->x[1]; break;
        case CMD_CLEAR_COLOR: {
            snap_reason = "clear-color"; snapshots_invalidate(NULL);
            int index = (int)r->x[0];
            if (ENV("LARECOMP_LOG_CLEARS") && index < st.rt.ncolor && st.rt.color[index])
                LOG("[cclear] frame %llu idx %d %ux%u f%#x mask %#x color %.3f %.3f %.3f %.3f\n", (unsigned long long)renderer_present_count(),
                    index, st.rt.color[index]->width, st.rt.color[index]->height, st.rt.color[index]->format, (uint32_t)r->x[2],
                    ((const float *)x)[0], ((const float *)x)[1], ((const float *)x)[2], ((const float *)x)[3]);
            if (index < st.rt.ncolor && st.rt.color[index]) clear(index, (int)r->x[5], x, (uint32_t)r->x[2], false, 0, false, 0, 0);
            break;
        }
        case CMD_CLEAR_TEXTURE: {
            // ClearTexture(cmd, texture, view, region, color, mask): a render-target clear of the texture alone (renderer.m)
            const struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } *ct = x;
            NvnTexture *t = (NvnTexture *)(uintptr_t)ct->tex;
            if (!t) break;
            snap_reason = "clear-texture"; snapshots_invalidate(NULL);
            RtState saved = st.rt;
            int32_t sc[4];
            memcpy(sc, st.scissor[0], sizeof sc);
            vk_end_pass();
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = 1;
            st.rt.color[0] = t;
            if (ct->has_view) { st.rt.cview[0] = ct->view; st.rt.has_cview[0] = 1; }
            st.rt_dirty = true;
            const int32_t *rg = ct->region;
            if (rg[3] > 0 && rg[4] > 0) { st.scissor[0][0] = rg[0]; st.scissor[0][1] = rg[1]; st.scissor[0][2] = rg[3]; st.scissor[0][3] = rg[4]; }
            else { st.scissor[0][0] = st.scissor[0][1] = 0; st.scissor[0][2] = st.scissor[0][3] = 16384; }
            clear(0, (int)r->x[6], ct->color, (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 0xF, false, 0, false, 0, 0);
            vk_end_pass();
            st.rt = saved;
            memcpy(st.scissor[0], sc, sizeof sc);
            st.rt_dirty = true;
            break;
        }
        case CMD_CLEAR_DEPTH_STENCIL:
            snap_reason = "clear-depth"; snapshots_invalidate(NULL);
            if (st.rt.depth) {
                bool has_stencil = renderer_format_has_stencil(st.rt.depth->format);
                clear(-1, 0, NULL, 0, r->x[0] != 0, r->f[0], has_stencil && r->x[2] != 0, (uint32_t)r->x[1], (uint32_t)r->x[2]);
            }
            break;
        case CMD_DISPATCH: dispatch_compute((uint32_t)r->x[0], (uint32_t)r->x[1], (uint32_t)r->x[2]); break;
        case CMD_DRAW_ELEMENTS_INDIRECT: case CMD_DRAW_ARRAYS_INDIRECT: {
            // (cmd, mode, index type, index buffer, indirect buffer) / (cmd, mode, indirect buffer): as a direct draw record
            // whose counts come from the GPU (draw_ with g_draw_indirect)
            static int logged;
            if (!logged++) LOG("[vk] indirect draws executed (first: %s, arguments at %llx)\n", r->op == CMD_DRAW_ELEMENTS_INDIRECT ? "elements" : "arrays",
                               (unsigned long long)(r->op == CMD_DRAW_ELEMENTS_INDIRECT ? r->x[3] : r->x[1]));
            CmdRec d = *r;
            bool idx = r->op == CMD_DRAW_ELEMENTS_INDIRECT;
            memset(d.x, 0, sizeof d.x);
            d.x[0] = r->x[0];
            if (idx) d.x[1] = r->x[1], d.x[3] = r->x[2], d.x[6] = 1;
            else d.x[4] = 1;
            g_draw_indirect = idx ? r->x[3] : r->x[1];
            if (g_debug_frame) LOG("[indirect] %s args %llx index %llx vs %llx\n", idx ? "elements" : "arrays", (unsigned long long)g_draw_indirect,
                                   (unsigned long long)(idx ? r->x[2] : 0), (unsigned long long)(st.program ? st.program->stages[0].code_addr : 0));
            draw(&d, idx);
            g_draw_indirect = 0;
            break;
        }
        case CMD_DISPATCH_INDIRECT: {  // (cmd, indirect buffer: three u32 group counts the GPU wrote)
            static int logged;
            if (!logged++) LOG("[vk] indirect dispatches executed (first: arguments at %llx)\n", (unsigned long long)r->x[0]);
            g_dispatch_indirect = r->x[0];
            g_dispatch_drv = (VkSpan){0};
            dispatch_compute(1, 1, 1);  // (placeholder counts: the real ones are copied in on the GPU)
            g_dispatch_indirect = 0;
            break;
        }
        case CMD_DRAW_ARRAYS: draw(r, false); exp_draw_delay(); break;
        case CMD_DRAW_TEXTURE: draw_texture(r); break;
        case CMD_DRAW_ELEMENTS: draw(r, true); exp_draw_delay(); break;
        case CMD_UPDATE_UNIFORM_BUFFER:
            inline_note(r->x[0] + r->x[2], r->x[0] + r->x[2] + r->x[3]);  // (copied per draw from now on: g_inline)
            memcpy((void *)(uintptr_t)(r->x[0] + r->x[2]), x, r->x[3]);
            guest_copy_flush();  // (cbuf copies shared within the submission are stale now)
            break;
        case CMD_FENCE_SYNC: {
            static int fence_flush = -1;
            if (fence_flush < 0) fence_flush = ENV("LARECOMP_FENCE_FLUSH") ? 1 : 0;
            if (fence_flush) renderer_flush();
            extern void nvn_sync_set_seq(uint64_t guest, uint64_t seq);
            if (r->x[1]) nvn_sync_set_seq(r->x[1], renderer_fence_seq());
            break;
        }
        case CMD_RESET_COUNTER:
            if (r->x[0] != 1) break;  // (SAMPLES_PASSED only: an occlusion query starts here)
            if (oq_mode < 0) oq_init();
            if (oq_mode != 1) break;
            if (oq_active >= 0) { vkCmdEndQuery(vk_cmd(), oq_pool, (uint32_t)oq_active); oq_active = -1; }  // (a reset again)
            if (in_pass) oq_begin();
            else oq_wanted = true;
            break;
        case CMD_REPORT_COUNTER: {
            // The game times its GPU work with these (two per frame) and drops to 30 fps when that looks slow;
            // the value is our translation time, not GPU time. LARECOMP_VK_COUNTER=frozen: the same timestamp for
            // every counter within a presented frame (experiment).
            uint64_t *out = (uint64_t *)(uintptr_t)r->x[1];
            uint32_t type = (uint32_t)r->x[0];
            static uint32_t seen;
            if (type < 32 && !(seen & (1u << type))) { seen |= 1u << type; LOG("[nvn] ReportCounter type %u\n", type); }
            static int frozen = -1;  // 1: frozen; 2: ideal (LARECOMP_VK_COUNTER=ideal: a 33.3 ms frame, 1 ms per counter in it)
            if (frozen < 0) { const char *e = ENV("LARECOMP_VK_COUNTER"); frozen = !e ? 0 : !strcmp(e, "frozen") ? 1 : !strcmp(e, "ideal") ? 2 : 0; }
            static uint64_t frame_seen = ~0ull, frame_ts, in_frame;
            extern uint64_t vk_frame_counter(void);
            if (vk_frame_counter() != frame_seen) { frame_seen = vk_frame_counter(); frame_ts = SDL_GetTicksNS(); in_frame = 0; }
            uint64_t v = frozen == 1 ? frame_ts : frozen == 2 ? frame_seen * 33333333ull + in_frame * 1000000ull : SDL_GetTicksNS();
            in_frame++;
            if (out) { out[0] = 0; out[1] = v; }
            if (type == 1) {  // SAMPLES_PASSED: the occlusion query since the reset (above)
                if (oq_mode < 0) oq_init();
                if (out) out[0] = oq_mode == 1 ? OQ_VISIBLE : oq_fixed;  // (until the query's result is copied over it)
                uint64_t a = r->x[1];
                if (oq_active >= 0) {
                    vkCmdEndQuery(vk_cmd(), oq_pool, (uint32_t)oq_active);
                    if (in_pass && gmem_buf && a >= HEAP_BASE && a + 8 <= HEAP_BASE + gmem_size && oq_ncopy < OQ_SLOTS)
                        oq_copy[oq_ncopy].slot = (uint32_t)oq_active, oq_copy[oq_ncopy++].off = a - HEAP_BASE;
                    oq_active = -1;
                } else if (oq_mode == 1) {
                    oq_stats[1]++;
                }
                oq_wanted = false;
                static uint64_t logged;
                if (oq_mode == 1 && renderer_present_count() / 1200 != logged) {
                    logged = renderer_present_count() / 1200;
                    LOG("[vk] occlusion queries: %llu results copied, %llu reports without a query, %llu cut by a pass end\n",
                        (unsigned long long)oq_stats[0], (unsigned long long)oq_stats[1], (unsigned long long)oq_stats[2]);
                }
            }
            static int clog = -1;  // LARECOMP_COUNTER_LOG=1: each counter, with the draws recorded since the previous one
            if (clog < 0) clog = ENV("LARECOMP_COUNTER_LOG") != NULL;
            static uint64_t last_draws;
            if (clog) LOG("[counter] frame %llu #%llu type %u at %llx value %llu draws %llu\n", (unsigned long long)frame_seen,
                          (unsigned long long)in_frame, type, (unsigned long long)r->x[1], (unsigned long long)v,
                          (unsigned long long)(g_draws_total - last_draws));
            last_draws = g_draws_total;
            break;
        }
        case CMD_COPY_TEXTURE_TO_TEXTURE: {
            const TexRefRec *t = x;
            if (ENV("LARECOMP_LOG_SMALL_PASSES")) {
                NvnTexture *s_ = (NvnTexture *)(uintptr_t)t[0].tex, *d_ = (NvnTexture *)(uintptr_t)t[1].tex;
                static int n;
                if (d_ && d_->width < 1000 && n++ < 40)
                    LOG("[copy] tex %ux%u f%#x (%d,%d %dx%d) -> tex %ux%u f%#x (%d,%d %dx%d)\n", s_ ? s_->width : 0, s_ ? s_->height : 0,
                        s_ ? s_->format : 0, t[0].region[0], t[0].region[1], t[0].region[3], t[0].region[4], d_->width, d_->height,
                        d_->format, t[1].region[0], t[1].region[1], t[1].region[3], t[1].region[4]);
            }
            copy_texture(&t[0], &t[1], r->x[6]);
            break;
        }
        case CMD_DOWNSAMPLE: { const TexRefRec *t = x; copy_texture(&t[0], &t[1], 1); break; }
        case CMD_COPY_BUFFER_TO_TEXTURE: {
            if (ENV("LARECOMP_LOG_SMALL_PASSES")) {
                const TexRefRec *t = x;
                NvnTexture *d_ = (NvnTexture *)(uintptr_t)t->tex;
                static int n;
                if (d_ && d_->width < 1000 && n++ < 40)
                    LOG("[copy] buffer %llx -> tex %ux%u f%#x (%d,%d %dx%d)\n", (unsigned long long)r->x[0], d_->width, d_->height,
                        d_->format, t->region[0], t->region[1], t->region[3], t->region[4]);
            }
            copy_buffer_to_texture(r->x[0], x);
            break;
        }
        case CMD_COPY_TEXTURE_TO_BUFFER: copy_texture_to_buffer(x, r->x[3]); break;
        case CMD_COPY_BUFFER_TO_BUFFER:
            renderer_flush();
            memmove((void *)(uintptr_t)r->x[1], (void *)(uintptr_t)r->x[0], r->x[2]);
            break;
        default: break;
        }
        p += r->size;
    }
}

// One submission per command list cost Legends: Arceus ~50 vkQueueSubmits a frame (each with its upload buffer,
// barrier and timeline signal). With the render thread, lists queued back to back share one, up to ~2000 draws (the GPU
// starts on the first part while the rest is recorded); the render thread submits when its queue runs empty and when a
// thread waits for the work (renderer_wait_seq). LARECOMP_VK_BATCH=0: one per list.
static bool batch_lists(void) {
    static int on = -1;
    if (on < 0) on = ENV("LARECOMP_VK_BATCH") ? atoi(ENV("LARECOMP_VK_BATCH")) != 0 : 1;
    return on;
}

void backend_execute(uint64_t handle) {
    if (ENV("LARECOMP_EXP_NOEXEC")) return;  // (research: the game's chain without the backend's work; the picture stays blank)
    PROF_T0();
    if (ip_rec()) ip_note_list(handle);
    exec_list(handle, 0);
    PROF_ADD(prof_exec);
    if (prof) cum_exec += SDL_GetTicksNS() - prof_t0_;
    static int sync_submit = -1;
    if (sync_submit < 0) sync_submit = ENV("LARECOMP_SYNC_SUBMIT") ? 1 : 0;
    // A submission every ~100 draws: the GPU starts on a frame while the render thread still translates the rest of
    // it. The game keeps at most two frames in flight (Z-A: f_70017fb59c waits for the older one), so how soon a frame
    // is done on the GPU is what its frame rate waits on - 2000 draws per submission (about one a frame) left the GPU
    // idle until the frame was translated: Lumiose ran 112 fps at 144, 120 with this. LARECOMP_SUBMIT_DRAWS=<n> chooses.
    static uint64_t batch_draws;
    if (!batch_draws) batch_draws = ENV("LARECOMP_SUBMIT_DRAWS") ? strtoull(ENV("LARECOMP_SUBMIT_DRAWS"), NULL, 0) : 100;
    if (!batch_draws) batch_draws = 100;
    if (sync_submit) renderer_flush();
    else if (!batch_lists() || !rt_on_render_thread() || !rt_more_queued() || g_draws_total - submit_draws >= batch_draws) renderer_submit();
}

// ================================================================== frame interpolation (prototype)
// LARECOMP_INTERP=1: 60 fps from the game's 30 by drawing an in-between frame (letsgo-recomp's docs/PESQUISA-60FPS.md). The game's
// logic advances one step per frame, so the frame rate can't simply be raised; instead, like RT64 (Zelda64Recomp)
// and Ship of Harkinian, frame N's draws are run a second time with their constant buffers (camera, world, bone
// matrices) blended halfway between N-1 and N, and that image is shown half a frame before N.
//  - recording: the command lists submitted in a frame (and a hash of their records), the exec state at the frame's
//    start, and a copy of every vertex / fragment constant buffer a draw binds (cbuf 3 and up), keyed by stage,
//    binding, the program, vertex buffer 0, the draw's parameters and the occurrence of that key in the frame;
//  - replay (vk_interp_render, at the present): the same lists from the same start state, skipping what writes guest
//    memory (compute, copies to buffers, inline uniform updates, syncs, counters); each constant buffer is the blend of
//    its two copies, word by word, for normal floats close enough (other words, and draws without a match, keep N);
//  - a list whose records changed since it ran (the game reused its memory) cancels that frame's in-between image.
typedef struct { uint64_t key; uint32_t off, size; uint64_t obj; } IpEntry;  // (obj: the game object of the draw)
typedef struct {
    IpEntry *e;
    uint32_t n, cap;
    uint8_t *data;
    uint64_t used, dcap;
    uint32_t *tab;  // open addressing: key -> e index + 1
    uint64_t lists[256], hashes[256];
    int nlists;
    bool overflow, valid;
    uint64_t destroyed;  // g_nvn_destroyed when the frame started
    __typeof__(st) start;
} IpFrame;
extern uint64_t g_nvn_destroyed;  // (objects.c: textures, pools, programs the game finalized)
#define IP_TAB (1u << 16)
static IpFrame ipf[2];
static int ipc;  // ipf[ipc]: the frame being recorded; ipf[ipc ^ 1]: the one before
static float ip_t = 0.5f;
static uint32_t ip_occ[IP_TAB];  // occurrences of each base key in this frame / replay (by slot)
static uint64_t ip_occ_key[IP_TAB];
static uint64_t ip_stats[4];  // blended, kept (no match), words kept, frames replayed
static uint64_t st_stats[4];  // stereo eye 1: buffers as recorded, from the plugin, without a recording; frames run again
// LARECOMP_INTERP_DIAG=1: what the in-between image is made of (letsgo-recomp's docs/PESQUISA-60FPS.md, "Diagnóstico da interpolação")
typedef struct { int stage, k; uint32_t size; uint64_t draws, nomatch, tagged, words, changed, blended, jumps, nonfloat; } IpDiag;
static IpDiag ip_diag[96];
static int ip_ndiag;
static uint64_t ip_rej[8];  // frames without an in-between image: no previous frame, overflow, destroyed, format, list changed
static IpDiag *ip_diag_for(int stage, int k, uint32_t size) {
    for (int i = 0; i < ip_ndiag; i++)
        if (ip_diag[i].stage == stage && ip_diag[i].k == k && ip_diag[i].size == size) return &ip_diag[i];
    if (ip_ndiag >= 96) return &ip_diag[95];
    ip_diag[ip_ndiag] = (IpDiag){.stage = stage, .k = k, .size = size};
    return &ip_diag[ip_ndiag++];
}
static void ip_diag_dump(void) {
    LOG("[interp-diag] rejected frames: no-prev %llu overflow %llu destroyed %llu format %llu list-changed %llu; replayed %llu\n",
        (unsigned long long)ip_rej[0], (unsigned long long)ip_rej[1], (unsigned long long)ip_rej[2], (unsigned long long)ip_rej[3],
        (unsigned long long)ip_rej[4], (unsigned long long)ip_stats[3]);
    for (int i = 0; i < ip_ndiag; i++) {
        const IpDiag *d = &ip_diag[i];
        LOG("[interp-diag] stage %d cbuf %d size %u: binds %llu (tagged %llu, no match %llu) words %llu changed %llu blended %llu jumps %llu nonfloat %llu\n",
            d->stage, d->k, d->size, (unsigned long long)d->draws, (unsigned long long)d->tagged, (unsigned long long)d->nomatch,
            (unsigned long long)d->words, (unsigned long long)d->changed, (unsigned long long)d->blended, (unsigned long long)d->jumps,
            (unsigned long long)d->nonfloat);
    }
    memset(ip_diag, 0, sizeof ip_diag);
    ip_ndiag = 0;
    memset(ip_rej, 0, sizeof ip_rej);
}

static uint64_t ip_mix(uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); return h * 0x100000001B3ull; }

static void ip_occ_reset(void) { memset(ip_occ_key, 0, sizeof ip_occ_key); }

static uint64_t ip_key(int stage, int k) {
    uint64_t base = ip_mix(ip_mix(ip_draw_sig, (uint64_t)stage), (uint64_t)k);
    if (!base) base = 1;
    uint32_t s = (uint32_t)(base ^ (base >> 29)) & (IP_TAB - 1);
    while (ip_occ_key[s] && ip_occ_key[s] != base) s = (s + 1) & (IP_TAB - 1);
    if (!ip_occ_key[s]) { ip_occ_key[s] = base; ip_occ[s] = 0; }
    return ip_mix(base, ip_occ[s]++);
}

static const IpEntry *ip_find(const IpFrame *f, uint64_t key) {
    if (!f->tab) return NULL;
    uint32_t s = (uint32_t)(key ^ (key >> 31)) & (IP_TAB - 1);
    while (f->tab[s]) {
        const IpEntry *e = &f->e[f->tab[s] - 1];
        if (e->key == key) return e;
        s = (s + 1) & (IP_TAB - 1);
    }
    return NULL;
}

static void ip_frame_clear(IpFrame *f) {
    f->n = 0; f->used = 0; f->nlists = 0; f->overflow = false; f->valid = false;
    if (f->tab) memset(f->tab, 0, IP_TAB * sizeof *f->tab);
}

static void ip_record(uint64_t key, const void *data, uint32_t size) {
    IpFrame *f = &ipf[ipc];
    if (!f->tab) f->tab = calloc(IP_TAB, sizeof *f->tab);
    if (f->n >= IP_TAB / 2) { f->overflow = true; return; }
    if (f->n == f->cap) { f->cap = f->cap ? f->cap * 2 : 4096; f->e = realloc(f->e, f->cap * sizeof *f->e); }
    if (f->used + size > f->dcap) { f->dcap = (f->used + size) * 2 + (1 << 20); f->data = realloc(f->data, f->dcap); }
    memcpy(f->data + f->used, data, size);
    f->e[f->n] = (IpEntry){key, (uint32_t)f->used, size, st.obj};
    f->used += size;
    uint32_t s = (uint32_t)(key ^ (key >> 31)) & (IP_TAB - 1);
    while (f->tab[s]) s = (s + 1) & (IP_TAB - 1);
    f->tab[s] = ++f->n;
}

// cbuf_info, constant buffers 3+ of the vertex / fragment stage: recording copies them (returns NULL: bind as usual);
// the replay returns the blended data to bind.
// Objects whose buffers changed too much between N-1 and N to be blended (vk_interp_render's check, before the replay):
// a camera cut, a model swapped (another level of detail, another skeleton) or two draws paired wrongly. Blending them
// mixes unrelated poses - characters stretched across the screen, Pokémon torn apart. They keep frame N's pose.
#define IP_REJ 1024
static uint64_t ip_rej_obj[IP_REJ];
static bool ip_obj_rejected(uint64_t obj) {
    uint32_t s = (uint32_t)((obj >> 4) * 0x9E3779B1u) & (IP_REJ - 1);
    while (ip_rej_obj[s]) { if (ip_rej_obj[s] == obj) return true; s = (s + 1) & (IP_REJ - 1); }
    return false;
}
static const void *ip_cbuf(int stage, int k, uint64_t addr, uint32_t size) {
    if (stage > 1) return NULL;
    uint64_t key = ip_key(stage, k);
    if (!ip_replaying) { ip_record(key, (const void *)(uintptr_t)addr, size); return NULL; }
    const IpEntry *n = ip_find(&ipf[ipc], key), *o = ip_find(&ipf[ipc ^ 1], key);
    static int diag = -1;
    if (diag < 0) diag = ENV("LARECOMP_INTERP_DIAG") ? 1 : 0;
    IpDiag *dg = diag ? ip_diag_for(stage, k, size) : NULL;
    if (dg) { dg->draws++; if (ip_draw_obj) dg->tagged++; if (!o) dg->nomatch++; }
    if (!n) return NULL;
    const uint8_t *nd = ipf[ipc].data + n->off;
    if (!ip_draw_obj || ip_obj_rejected(st.obj ? st.obj : 1)) { ip_stats[1]++; return nd; }  // interface and other draws no object owns: their order and text change between frames, a blend smears them
    if (!o || o->size != n->size) { ip_stats[1]++; return nd; }
    static uint32_t buf[0x10000 / 4];
    const uint32_t *a = (const uint32_t *)(ipf[ipc ^ 1].data + o->off), *b = (const uint32_t *)nd;
    uint32_t nw = n->size / 4;
    // A jump anywhere in the buffer (a bone, a matrix row) keeps all of it at N: blending the rest would pull part of a
    // skeleton or of a matrix halfway and leave the other part, which stretches the model.
    for (uint32_t i = 0; i < nw; i++) {
        if (a[i] == b[i]) continue;
        uint32_t ea = (a[i] >> 23) & 0xFF, eb = (b[i] >> 23) & 0xFF;
        if (!ea || !eb || ea == 0xFF || eb == 0xFF) continue;
        float fa, fb;
        memcpy(&fa, &a[i], 4);
        memcpy(&fb, &b[i], 4);
        bool jump = ip_draw_2d ? fabsf(fb - fa) > 0.3f * fmaxf(fabsf(fa), fabsf(fb)) && fabsf(fb - fa) > 0.02f  // (2D: a big move is a recycled row or glyph, not motion)
                               : fabsf(fb - fa) > 0.5f * (fabsf(fa) + fabsf(fb)) + 4.0f;
        if (jump) {
            if (dg) dg->jumps += nw;
            ip_stats[1]++;
            return nd;
        }
    }
    // Blended a vec4 at a time, all or nothing: a matrix row with some components blended and others kept (one of them
    // crossing zero, or an integer in a float's place) is not a transformation any more - it shears and shrinks the model.
    // Words that are equal need no test; the others must be finite floats (0 counts) without a jump.
    for (uint32_t g = 0; g < nw; g += 4) {
        uint32_t m = nw - g < 4 ? nw - g : 4;
        int bad = 0, changed = 0, jump = 0;
        for (uint32_t i = g; i < g + m; i++) {
            if (a[i] == b[i]) continue;
            changed++;
            uint32_t ea = (a[i] >> 23) & 0xFF, eb = (b[i] >> 23) & 0xFF;
            bool fa_ok = ea != 0xFF && (ea || !(a[i] & 0x7FFFFF)), fb_ok = eb != 0xFF && (eb || !(b[i] & 0x7FFFFF));
            float fa, fb;
            memcpy(&fa, &a[i], 4);
            memcpy(&fb, &b[i], 4);
            if (!fa_ok || !fb_ok) bad++;
            else if (fabsf(fb - fa) > 0.5f * (fabsf(fa) + fabsf(fb)) + 4.0f) jump++;
        }
        if (dg) { dg->words += m; dg->changed += changed; }
        if (!bad && !jump) {
            for (uint32_t i = g; i < g + m; i++) {
                if (a[i] == b[i]) { buf[i] = b[i]; continue; }
                float fa, fb, v;
                memcpy(&fa, &a[i], 4);
                memcpy(&fb, &b[i], 4);
                v = fa + (fb - fa) * ip_t;
                memcpy(&buf[i], &v, 4);
            }
            if (dg) dg->blended += changed;
        } else {
            for (uint32_t i = g; i < g + m; i++) buf[i] = b[i];
            ip_stats[2] += changed;
            if (dg) { dg->jumps += jump ? changed : 0; dg->nonfloat += bad ? changed : 0; }
        }
    }
    if (n->size > nw * 4) memcpy((uint8_t *)buf + nw * 4, nd + nw * 4, n->size - nw * 4);
    ip_stats[0]++;
    return buf;
}

// hash of a command list's records (JUMPs and CALLs followed), to tell whether its memory still holds what ran
static uint64_t ip_list_hash(uint64_t handle, int depth) {
    uint64_t h = 0xcbf29ce484222325ull, p = handle;
    for (int guard = 0; guard < 10000000; guard++) {
        const CmdRec *r = (const CmdRec *)(uintptr_t)p;
        if (r->op >= CMD_COUNT || r->size < sizeof(CmdRec) || r->size > (1u << 20)) return 0;
        const uint64_t *w = (const uint64_t *)r;
        for (uint32_t i = 0; i < r->size / 8; i++) h = ip_mix(h, w[i]);
        if (r->op == CMD_END) return h;
        if (r->op == CMD_JUMP) { p = r->x[0]; continue; }
        if (r->op == CMD_CALL) {
            const uint64_t *hs = (const uint64_t *)r->extra;
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) h = ip_mix(h, ip_list_hash(hs[i], depth + 1));
        }
        p += r->size;
    }
    return 0;
}

static bool st_check(void) {  // (research, LARECOMP_STEREO_CHECK=1: stereo hashes the lists too, as interpolation does)
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_STEREO_CHECK") && atoi(getenv("LARECOMP_STEREO_CHECK")) != 0;
    return on;
}
static void ip_note_list(uint64_t handle) {
    IpFrame *f = &ipf[ipc];
    if (f->nlists >= 256) { f->overflow = true; return; }
    f->hashes[f->nlists] = st_on() && !st_check() ? 1 : ip_list_hash(handle, 0);  // (stereo: the fences keep the lists)
    f->lists[f->nlists++] = handle;
}

// The hold image: frame N's presented texture, kept while the replay overwrites it with the in-between image.
static TexBackend ip_hold;
static TexBackend *ip_hold_copy(TexBackend *src) {
    TexBackend *d = &ip_hold;
    if (d->image && (d->width != src->width || d->height != src->height || d->format != src->format)) {
        vk_views_forget(d);
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                                .imageType = VK_IMAGE_TYPE_2D, .format = src->format, .extent = {src->width, src->height, 1},
                                .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = src->format; d->width = src->width; d->height = src->height; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    d->sx = src->sx; d->sy = src->sy; d->scalable = src->scalable;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .extent = {src->width, src->height, 1}};
    vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    vk_full_barrier(cb);
    return d;
}

// Stereo, eye 1's run (cbuf_info): what the plugin puts in a buffer's place, else the copy eye 0 bound, else NULL (in place)
static const void *st_cbuf_replay(uint64_t key, int stage, uint64_t addr, uint32_t size) {
    const IpEntry *e = ip_find(&ipf[ipc], key);
    const void *in = e && e->size == size ? ipf[ipc].data + e->off : (const void *)(uintptr_t)addr;
    static uint8_t out[0x10000];
    if (size <= sizeof out && plugins_stereo_cbuf(stage, addr, size, in, out)) { st_stats[1]++; return out; }
    if (e && e->size == size) { st_stats[0]++; return in; }
    st_stats[2]++;
    return NULL;
}

// Stereo (plugin.h set_stereo), at the present of a frame whose image b holds eye 0 (the plugins have it already): its
// lists run again from the state it started with, the constant buffers as recorded (ip_cbuf, with the plugin's
// replacements), what writes game memory skipped (ip_skip_op) - b then holds eye 1. False: the frame can't be run again
// (not recorded from its start, its lists' memory reused, an object destroyed). Recording moves on to the next frame.
void vk_stereo_present_size(uint32_t w, uint32_t h) { st_pres_w = w, st_pres_h = h; }
bool vk_stereo_render(TexBackend *b) {
    IpFrame *cur = &ipf[ipc];
    bool ok = cur->valid && cur->nlists > 0 && !cur->overflow && b && b->image && b->aspect == VK_IMAGE_ASPECT_COLOR_BIT &&
              b->samples <= 1 && cur->destroyed == g_nvn_destroyed;
    int why = !cur->valid || !cur->nlists ? 0 : cur->overflow ? 1 : cur->destroyed != g_nvn_destroyed ? 2 : 3;
    extern volatile uint64_t g_st_released_early;
    extern uint64_t rt_running_present(void);
    extern bool rt_on_render_thread(void);
    if (ok && rt_on_render_thread() && __atomic_load_n(&g_st_released_early, __ATOMIC_ACQUIRE) == rt_running_present())
        ok = false, why = 5;  // (a fence released this frame's memory before its present: not drawn again)
    for (int i = 0; ok && st_check() && i < cur->nlists; i++)
        if (!cur->hashes[i] || ip_list_hash(cur->lists[i], 0) != cur->hashes[i]) ok = false, why = 4;
    if (ok) {
        vk_end_pass();
        __typeof__(st) end = st;
        st = cur->start;
        st.rt_dirty = true;
        ip_replaying = true;
        ip_occ_reset();
        uint64_t d0 = g_draws_total;
        for (int i = 0; i < cur->nlists; i++) {
            exec_list(cur->lists[i], 0);
            if (g_draws_total - d0 >= 2000) vk_end_pass(), renderer_submit(), d0 = g_draws_total;
        }
        vk_end_pass();
        ip_replaying = false;
        st = end;
        st.rt_dirty = true;
        st_stats[3]++;
    } else ip_rej[why]++;
    static uint64_t logged;
    if (st_stats[3] + ip_rej[0] + ip_rej[1] + ip_rej[2] + ip_rej[3] + ip_rej[4] + ip_rej[5] >= logged + 600) {
        logged = st_stats[3] + ip_rej[0] + ip_rej[1] + ip_rej[2] + ip_rej[3] + ip_rej[4] + ip_rej[5];
        LOG("[stereo] eye 1 drawn for %llu frames (not: unrecorded %llu, overflow %llu, destroyed %llu, list reused %llu, "
            "released early %llu); "
            "buffers: copies %llu, from the plugin %llu, in place %llu\n", (unsigned long long)st_stats[3],
            (unsigned long long)ip_rej[0], (unsigned long long)ip_rej[1], (unsigned long long)ip_rej[2], (unsigned long long)ip_rej[4],
            (unsigned long long)ip_rej[5],
            (unsigned long long)st_stats[0], (unsigned long long)st_stats[1], (unsigned long long)st_stats[2]);
    }
    ip_frame_clear(cur);
    cur->start = st;
    cur->destroyed = g_nvn_destroyed;
    cur->valid = true;
    ip_occ_reset();
    return ok;
}

// At the present of frame N (presented texture b): when frame N-1 was recorded too and N's lists are intact, renders
// the in-between image into the game's textures (b then holds it) and returns frame N's own image in *hold; false:
// nothing to show before N. Either way, recording moves on to the next frame.
bool vk_interp_render(TexBackend *b, TexBackend **hold) {
    if (!ip_enabled()) {
        // Off (the world, a battle): what was recorded the last time it was on is stale - a menu opened later blended
        // with that old scene (a flash when the menu opened while walking)
        ipf[0].valid = ipf[1].valid = false;
        return false;
    }
    IpFrame *cur = &ipf[ipc], *prev = &ipf[ipc ^ 1];
    bool ok = prev->valid && cur->valid && cur->nlists > 0 && !cur->overflow && !prev->overflow && b && b->image &&
              b->aspect == VK_IMAGE_ASPECT_COLOR_BIT && b->samples <= 1 && cur->destroyed == g_nvn_destroyed;
    bool structural = ok;
    for (int i = 0; ok && i < cur->nlists; i++)
        if (!cur->hashes[i] || ip_list_hash(cur->lists[i], 0) != cur->hashes[i]) ok = false;
    if (!ok) {
        if (!prev->valid || !cur->valid || cur->nlists == 0) ip_rej[0]++;
        else if (cur->overflow || prev->overflow) ip_rej[1]++;
        else if (cur->destroyed != g_nvn_destroyed) ip_rej[2]++;
        else if (!structural) ip_rej[3]++;
        else ip_rej[4]++;
    }
    if (ok) {
        // Per object, before the replay: the share of its changed float words that changed by a large step. Above
        // LARECOMP_INTERP_OBJ_LIMIT (0.2) the object is not blended (ip_obj_rejected); when the rejected objects hold
        // most of the frame's changes (a camera cut), there is no in-between image at all.
        typedef struct { uint64_t obj, changed, big; } ObjAcc;
        static ObjAcc acc[IP_REJ];
        memset(acc, 0, sizeof acc);
        memset(ip_rej_obj, 0, sizeof ip_rej_obj);
        uint64_t changed = 0, big = 0, rej_changed = 0;
        int nrej = 0, nobj = 0;
        for (uint32_t i = 0; i < cur->n; i++) {
            const IpEntry *n = &cur->e[i], *o = ip_find(prev, n->key);
            if (!o || o->size != n->size) continue;
            uint64_t gobj = n->obj ? n->obj : 1;  // (draws without an object: one group)
            const float *fa = (const float *)(prev->data + o->off), *fb = (const float *)(cur->data + n->off);
            uint64_t c = 0, bg = 0;
            for (uint32_t w = 0; w < n->size / 4; w++) {
                float a = fa[w], v = fb[w];
                if (a == v || !isfinite(a) || !isfinite(v)) continue;
                float m = fmaxf(fabsf(a), fabsf(v));
                if (m < 1e-3f || m > 1e7f) continue;
                c++;
                if (fabsf(v - a) > 0.3f * m && fabsf(v - a) > 0.1f) bg++;
            }
            if (!c) continue;
            uint32_t sl = (uint32_t)((gobj >> 4) * 0x9E3779B1u) & (IP_REJ - 1);
            while (acc[sl].obj && acc[sl].obj != gobj) sl = (sl + 1) & (IP_REJ - 1);
            if (!acc[sl].obj) { acc[sl].obj = gobj; nobj++; }
            acc[sl].changed += c, acc[sl].big += bg;
            changed += c, big += bg;
        }
        extern bool fps60_menu(void);
        bool menu = fps60_menu();  // (menus: the cut checks are for battles; a menu screen changes by large steps all the time)
        static double obj_limit = -1;
        if (obj_limit < 0) obj_limit = ENV("LARECOMP_INTERP_OBJ_LIMIT") ? atof(ENV("LARECOMP_INTERP_OBJ_LIMIT")) : 0.2;
        for (int i = 0; i < IP_REJ; i++)
            if (!menu && acc[i].obj && acc[i].changed >= 8 && (double)acc[i].big > obj_limit * (double)acc[i].changed) {
                uint32_t sl = (uint32_t)((acc[i].obj >> 4) * 0x9E3779B1u) & (IP_REJ - 1);
                while (ip_rej_obj[sl]) sl = (sl + 1) & (IP_REJ - 1);
                ip_rej_obj[sl] = acc[i].obj;
                nrej++;
                rej_changed += acc[i].changed;
            }
        if (ENV("LARECOMP_INTERP_DIAG"))
            LOG("[interp-cut] present %llu: objects %d rejected %d (%llu of %llu changed words; big %llu)\n",
                (unsigned long long)renderer_present_count(), nobj, nrej, (unsigned long long)rej_changed, (unsigned long long)changed,
                (unsigned long long)big);
        if (!menu && changed && rej_changed * 10 > changed * 6) { ok = false; ip_rej[5]++; }
    }
    if (ok) {
        vk_end_pass();
        *hold = ip_hold_copy(b);
        __typeof__(st) end = st;
        st = cur->start;
        st.rt_dirty = true;
        ip_replaying = true;
        ip_occ_reset();
        for (int i = 0; i < cur->nlists; i++) exec_list(cur->lists[i], 0);
        vk_end_pass();
        ip_replaying = false;
        st = end;
        st.rt_dirty = true;
        ip_stats[3]++;
        if (ip_stats[3] % 300 == 1)
            LOG("[interp] frame %llu: %llu blended, %llu without a match, %llu words kept (replays %llu)\n",
                (unsigned long long)vk_frame_counter(), (unsigned long long)ip_stats[0], (unsigned long long)ip_stats[1],
                (unsigned long long)ip_stats[2], (unsigned long long)ip_stats[3]);
        if (ip_stats[3] % 300 == 0 && ENV("LARECOMP_INTERP_DIAG")) ip_diag_dump();
        if (ip_stats[3] % 300 == 1)
            LOG("[interp] draws with an object %llu, without %llu\n", (unsigned long long)ip_stats_tagged, (unsigned long long)ip_stats_untagged);
    }
    // the next frame records into the other slot, from the state this one ended with
    ipc ^= 1;
    ip_frame_clear(&ipf[ipc]);
    ipf[ipc].start = st;
    ipf[ipc].destroyed = g_nvn_destroyed;
    ipf[ipc].valid = true;
    ip_occ_reset();
    return ok;
}
