// NVN objects, builders, and state objects (everything except command recording).
#include <stdatomic.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../kernel.h"
#include "nvn.h"

void renderer_sync(void);

static uint64_t next_debug_id = 1;

// ================================================================== device
// NVNdeviceInfo -> value
static uint32_t device_integer(uint32_t which) {
    static const uint32_t v[] = {
        [0] = 53, [1] = 207,           // API version
        [2] = 14, [3] = 65536, [4] = 256, [5] = 8, [6] = 16, [7] = 4, [8] = 16, [9] = 32,
        [10] = 16, [11] = 4, [12] = 4, [13] = 4, [14] = 16, [15] = 32, [16] = 32,
        [17] = 256, [18] = 256,        // reserved texture / sampler descriptors
        [19] = 4, [20] = 8, [21] = 0x10000, [22] = 0x1000,
        [23] = 256, [24] = 256, [25] = 4096, [26] = 0x20000, [27] = 16,
        [28] = 1536, [29] = 1024, [30] = 64, [31] = 1536,
        [32] = 0x7FFFFFFF, [33] = 65535, [34] = 65535,
        [35] = 8, [36] = 1048576, [37] = 4096, [38] = 16, [39] = 0x10000,
        [40] = 1, [41] = 1, [42] = 1,  // min/max filtering, stencil8, ASTC
        [43] = 0x40000, [44] = 15, [45] = 2048,
        [46] = 1, [47] = 5, [48] = 1, [49] = 15,
        [50] = 256, [51] = 1, [52] = 0, [53] = 0, [54] = 0, [55] = 256, [56] = 1, [57] = 0x1000,
        [58] = 1, [59] = 1, [60] = 1, [61] = 1, [62] = 1, [63] = 1, [64] = 1,
    };
    return which < sizeof v / sizeof *v ? v[which] : 0;
}

NVN(nvnDeviceBuilderSetDefaults) { G(uint32_t, A(0), 0) = 0; }
NVN(nvnDeviceBuilderSetFlags) { G(uint32_t, A(0), 0) = (uint32_t)A(1); }
NVN(nvnDeviceBuilderGetFlags) { RET(G(uint32_t, A(0), 0)); }
NVN(nvnDeviceInitialize) {
    static bool once;
    if (!once) { once = true; backend_init(); }
    LOG("[nvn] device initialized\n");
    RET(1);
}
NVN(nvnDeviceFinalize) { (void)c; }
NVN(nvnDeviceSetDebugLabel) { (void)c; }
NVN(nvnDeviceGetInteger) {
    uint32_t which = (uint32_t)A(1), v = device_integer(which);
    static uint64_t seen[4];
    if (which < 256 && !(seen[which >> 6] >> (which & 63) & 1)) {
        seen[which >> 6] |= 1ull << (which & 63);
        if (ENV("LARECOMP_LOG_NVN_INTS") || which > 64) LOG("[nvn] DeviceGetInteger %u -> %u%s\n", which, v, which > 64 ? " (unknown)" : "");
    }
    *(int32_t *)AP(2) = (int32_t)v;
}
NVN(nvnDeviceGetCurrentTimestampInNanoseconds) {
    RET(plat_now_ns());
}
NVN(nvnDeviceGetTimestampInNanoseconds) { RET(G(uint64_t, A(1), 8)); }  // counter.timestamp (ticks == ns here)
NVN(nvnDeviceSetIntermediateShaderCache) { (void)c; }
NVN(nvnDeviceGetTextureHandle) { RET((A(1) & 0xFFFFF) | ((A(2) & 0xFFF) << 20)); }
NVN(nvnDeviceGetTexelFetchHandle) { RET(A(1) & 0xFFFFF); }
NVN(nvnDeviceGetImageHandle) { RET(A(1) & 0xFFFFF); }
NVN(nvnDeviceGetSeparateTextureHandle) { RET(A(1) & 0xFFFFF); }
NVN(nvnDeviceGetSeparateSamplerHandle) { RET((A(1) & 0xFFF) << 20); }
NVN(nvnDeviceInstallDebugCallback) { (void)c; }
NVN(nvnDeviceGenerateDebugDomainId) { RET(1); }
static uint32_t window_origin, depth_mode;
RT_ON_RESET(nvn_device_reset) { window_origin = depth_mode = 0; }  // (the next boot sets its own)
NVN(nvnDeviceSetWindowOriginMode) { window_origin = (uint32_t)A(1); LOG("[nvn] window origin mode %u\n", window_origin); }
uint32_t nvn_window_origin(void) { return window_origin; }
uint32_t nvn_depth_mode(void) { return depth_mode; }
NVN(nvnDeviceGetWindowOriginMode) { RET(window_origin); }
NVN(nvnDeviceSetDepthMode) { depth_mode = (uint32_t)A(1); LOG("[nvn] depth mode %u\n", depth_mode); }
NVN(nvnDeviceGetDepthMode) { RET(depth_mode); }
NVN(nvnDeviceRegisterFastClearColor) { RET(1); }
NVN(nvnDeviceRegisterFastClearColori) { RET(1); }
NVN(nvnDeviceRegisterFastClearColorui) { RET(1); }
NVN(nvnDeviceRegisterFastClearDepth) { RET(1); }
NVN(nvnDeviceApplyDeferredFinalizes) { (void)c; }
NVN(nvnDeviceFinalizeCommandHandle) { (void)c; }
NVN(nvnDeviceIsExternalDebuggerAttached) { RET(0); }
NVN(nvnDeviceWalkDebugDatabase) { (void)c; }

// ================================================================== queue
// QueueBuilder layout: device@0 flags@8 cmd@0xC compute@0x10 control@0x14 mem@0x18 memsize@0x20
//                      flush@0x28 priority@0x2C
NVN(nvnQueueBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnQueueBuilderSetDefaults) {
    memset(AP(0), 0, 0x30);
    G(uint32_t, A(0), 0x0C) = 0x10000;
    G(uint32_t, A(0), 0x10) = 0x40000;
    G(uint32_t, A(0), 0x14) = 0x4000;
    G(uint32_t, A(0), 0x28) = 0x8000;
    G(uint32_t, A(0), 0x2C) = 1;
}
NVN(nvnQueueBuilderSetFlags) { G(uint32_t, A(0), 8) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetCommandMemorySize) { G(uint32_t, A(0), 0x0C) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetComputeMemorySize) { G(uint32_t, A(0), 0x10) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetControlMemorySize) { G(uint32_t, A(0), 0x14) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetQueueMemory) { G(uint64_t, A(0), 0x18) = A(1); G(uint64_t, A(0), 0x20) = A(2); }
NVN(nvnQueueBuilderSetCommandFlushThreshold) { G(uint32_t, A(0), 0x28) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetQueuePriority) { G(uint32_t, A(0), 0x2C) = (uint32_t)A(1); }
NVN(nvnQueueBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnQueueBuilderGetFlags) { RET(G(uint32_t, A(0), 8)); }
NVN(nvnQueueBuilderGetCommandMemorySize) { *(int32_t *)AP(1) = G(int32_t, A(0), 0x0C); RET(1); }
NVN(nvnQueueBuilderGetComputeMemorySize) { *(int32_t *)AP(1) = G(int32_t, A(0), 0x10); RET(1); }
NVN(nvnQueueBuilderGetControlMemorySize) { *(int32_t *)AP(1) = G(int32_t, A(0), 0x14); RET(1); }
NVN(nvnQueueBuilderGetCommandFlushThreshold) { *(int32_t *)AP(1) = G(int32_t, A(0), 0x28); RET(1); }
NVN(nvnQueueBuilderGetQueuePriority) { RET(G(uint32_t, A(0), 0x2C)); }
NVN(nvnQueueBuilderGetMemorySize) { RET(0x10000); }  // required queue memory
NVN(nvnQueueBuilderGetQueueMemorySize) { RET(G(uint64_t, A(0), 0x20)); }
NVN(nvnQueueBuilderGetMemory) { RET(G(uint64_t, A(0), 0x18)); }

NVN(nvnQueueInitialize) { RET(1); }
NVN(nvnQueueFinalize) { (void)c; }
NVN(nvnQueueSetDebugLabel) { (void)c; }
NVN(nvnQueueGetError) { RET(0); }
NVN(nvnQueueGetTotalCommandMemoryUsed) { RET(0); }
NVN(nvnQueueGetTotalControlMemoryUsed) { RET(0); }
NVN(nvnQueueGetTotalComputeMemoryUsed) { RET(0); }
NVN(nvnQueueResetMemoryUsageCounts) { (void)c; }
NVN(nvnQueueSubmitCommands) {
    int n = (int)A(1);
    const uint64_t *handles = AP(2);
    for (int i = 0; i < n; i++) rt_submit(handles[i]);
}
NVN(nvnQueueFlush) { (void)c; }
static uint64_t g_wait_finish_ns;
NVN(nvnQueueFinish) { (void)c; uint64_t t0_ = plat_now_ns(); rt_drain(); renderer_sync(); g_wait_finish_ns += plat_now_ns() - t0_; }

static NvnSync *sync_of(uint64_t guest) {
    NvnSync *s = HOST(NvnSync, guest);
    if (!s) { s = calloc(1, sizeof *s); SET_HOST(guest, s); }
    return s;
}
uint64_t renderer_fence_seq(void);
void renderer_wait_seq(uint64_t seq);
extern volatile int g_plugin_stereo;  // (runtime/plugin.c: plugin.h set_stereo)
uint64_t rt_next_present(void);
void rt_wait_present(uint64_t p, uint64_t ticket, uint64_t seq_of_list);
NVN(nvnQueueFenceSync) {
    NvnSync *s = sync_of(A(1));
    s->signaled = 1;
    if (rt_active()) { s->seq = 0; s->ticket = rt_fence_ticket(); }  // (the jobs queued so far; the GPU seq is theirs once they have run)
    else { s->ticket = 0; s->seq = renderer_fence_seq(); }
    s->pres = rt_active() && g_plugin_stereo ? rt_next_present() : 0;  // (stereo: the frame is drawn again at its present)
}
void nvn_sync_set_seq(uint64_t guest, uint64_t seq) {  // (a fence inside a list: runs on the render thread)
    NvnSync *s = sync_of(guest);
    s->signaled = 1; s->ticket = 0; s->seq = seq;
    s->pres = rt_active() && g_plugin_stereo ? rt_next_present() : 0;
}
NVN(nvnQueueWaitSync) { RET(1); }

// ================================================================== sync / event
NVN(nvnSyncInitialize) { SET_HOST(A(0), calloc(1, sizeof(NvnSync))); RET(1); }
NVN(nvnSyncFinalize) { free(HOST(NvnSync, A(0))); SET_HOST(A(0), 0); }
NVN(nvnSyncSetDebugLabel) { (void)c; }
void renderer_flush(void);
// (research: LARECOMP_LOG_WAITS=1 - where the game threads wait for the GPU / render thread, ms per presented frame)
static struct { uint64_t lr, ns, n; } g_wait[16];
static uint64_t g_wait_pace_ns, g_wait_frames;
uint64_t g_wait_pace_add;
static pthread_mutex_t g_wait_mu = PTHREAD_MUTEX_INITIALIZER;
static void wait_note(uint64_t lr, uint64_t ns) {
    pthread_mutex_lock(&g_wait_mu);
    int i = 0;
    while (i < 16 && g_wait[i].lr && g_wait[i].lr != lr) i++;
    if (i < 16) g_wait[i].lr = lr, g_wait[i].ns += ns, g_wait[i].n++;
    pthread_mutex_unlock(&g_wait_mu);
}
static void wait_report(void) {
    static int on = -1;
    if (on < 0) on = ENV("LARECOMP_LOG_WAITS") != NULL;
    if (!on || ++g_wait_frames < 288) return;
    pthread_mutex_lock(&g_wait_mu);
    char line[1024];
    extern _Atomic uint64_t g_core_wait_ns, g_core_waits;  // (runtime/kernel.c: threads waiting for an emulated core)
    uint64_t cw = g_core_wait_ns, cn = g_core_waits;
    g_core_wait_ns = 0, g_core_waits = 0;
    extern _Atomic uint64_t g_wake_lat_ns, g_wake_n;
    uint64_t wl = g_wake_lat_ns, wn = g_wake_n;
    g_wake_lat_ns = 0, g_wake_n = 0;
    LOG("[waits] wakeups %.1f/frame, OS latency %.1f us each\n", (double)wn / g_wait_frames, wn ? wl / 1e3 / wn : 0.0);
    {
        extern uint64_t g_core_busy_ns[8], g_thread_busy_ns[256], g_thread_wait_ns[256];
        extern uint32_t g_thread_busy_mask[256];
        char cl[640];
        int cn_ = snprintf(cl, sizeof cl, "[waits] cores busy ms/frame:");
        for (int k = 0; k < 8; k++) if (k < 4 || g_core_busy_ns[k]) cn_ += snprintf(cl + cn_, sizeof cl - (size_t)cn_, " %.2f", g_core_busy_ns[k] / 1e6 / g_wait_frames), g_core_busy_ns[k] = 0;
        cn_ += snprintf(cl + cn_, sizeof cl - (size_t)cn_, " | threads:");
        for (int k = 0; k < 256; k++)
            if (g_thread_busy_ns[k] / g_wait_frames > 300000 && cn_ < 440)
                cn_ += snprintf(cl + cn_, sizeof cl - (size_t)cn_, " %x(m%x):%.2f w%.2f", k, g_thread_busy_mask[k], g_thread_busy_ns[k] / 1e6 / g_wait_frames,
                                g_thread_wait_ns[k] / 1e6 / g_wait_frames);
        memset(g_thread_busy_ns, 0, sizeof g_thread_busy_ns);
        memset(g_thread_wait_ns, 0, sizeof g_thread_wait_ns);
        LOG("%s\n", cl);
    }
    int len = snprintf(line, sizeof line, "[waits] ms/frame: finish %.2f pace %.2f core %.2f (x%.1f) syncwait", g_wait_finish_ns / 1e6 / g_wait_frames,
                       g_wait_pace_ns / 1e6 / g_wait_frames, cw / 1e6 / g_wait_frames, (double)cn / g_wait_frames);
    for (int i = 0; i < 16 && g_wait[i].lr && len < 900; i++)
        len += snprintf(line + len, sizeof line - (size_t)len, " %llx:%.2f(x%.1f)", (unsigned long long)g_wait[i].lr, g_wait[i].ns / 1e6 / g_wait_frames, (double)g_wait[i].n / g_wait_frames);
    memset(g_wait, 0, sizeof g_wait);
    g_wait_finish_ns = g_wait_pace_ns = g_wait_frames = 0;
    pthread_mutex_unlock(&g_wait_mu);
    LOG("%s\n", line);
}
void rt_trace(char ev, uint64_t arg);
NVN(nvnSyncWait) {  // NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED
    uint64_t t0_ = plat_now_ns();
    rt_trace('W', c->x[30]);
    static int flush = -1;
    static uint64_t n;
    if (flush < 0) flush = ENV("LARECOMP_SYNC_FLUSH") ? 1 : 0;
    if (ENV("LARECOMP_LOG_SYNC") && n++ % 60 == 0) LOG("[nvn] SyncWait #%llu (lr %llx)\n", (unsigned long long)n, c->x[30]);
    static int whole = -1;  // LARECOMP_SYNC_ALL=1: old behaviour, wait for all submitted work
    if (whole < 0) whole = ENV("LARECOMP_SYNC_ALL") ? 1 : 0;
    static uint64_t skip_lr = 1;  // (research, LARECOMP_SYNC_SKIP_LR=<hex lr>: that call site doesn't wait - unsafe)
    if (skip_lr == 1) skip_lr = ENV("LARECOMP_SYNC_SKIP_LR") ? strtoull(ENV("LARECOMP_SYNC_SKIP_LR"), NULL, 16) : 0;
    if (skip_lr && c->x[30] == skip_lr) { wait_note(c->x[30], 0); RET(0); return; }
    if (flush) { rt_drain(); renderer_flush(); }
    else if (whole) { rt_drain(); renderer_sync(); }
    else {
        NvnSync *s = sync_of(A(0));
        if (s->pres) rt_wait_present(s->pres, s->ticket, s->seq);  // (stereo: and the frame's second drawing)
        else if (s->ticket) rt_wait_ticket(s->ticket);  // only the work up to this fence
        else { if (s->seq) rt_drain(); renderer_wait_seq(s->seq); }  // (a fence inside a list is set once the list has run)
    }
    wait_note(c->x[30], plat_now_ns() - t0_);
    rt_trace('w', 0);
    RET(0);
}
NVN(nvnSyncCreateGLSync) { RET(0); }
NVN(nvnSyncInitializeFromFencedGLSync) { RET(0); }

// EventBuilder: pool@0 offset@8 ; Event: storage address (u32 value lives in guest memory)
NVN(nvnEventBuilderSetDefaults) { G(uint64_t, A(0), 0) = 0; G(uint64_t, A(0), 8) = 0; }
NVN(nvnEventBuilderSetStorage) { G(uint64_t, A(0), 0) = A(1); G(uint64_t, A(0), 8) = A(2); }
NVN(nvnEventBuilderGetStorage) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnEventBuilderGetMemoryPool) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnEventBuilderGetMemoryOffset) { RET(G(uint64_t, A(0), 8)); }
static uint64_t event_addr(uint64_t ev) { return G(uint64_t, ev, 0); }
NVN(nvnEventInitialize) {
    uint64_t pool = G(uint64_t, A(1), 0), off = G(uint64_t, A(1), 8);
    G(uint64_t, A(0), 0) = HOST(NvnPool, pool)->storage + off;
    G(uint64_t, A(0), 8) = pool;
    G(uint64_t, A(0), 16) = off;
    RET(1);
}
NVN(nvnEventFinalize) { (void)c; }
NVN(nvnEventGetValue) { RET(*(uint32_t *)(uintptr_t)event_addr(A(0))); }
NVN(nvnEventSignal) { *(uint32_t *)(uintptr_t)event_addr(A(0)) = (uint32_t)A(2); }
NVN(nvnEventGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnEventGetMemoryOffset) { RET(G(uint64_t, A(0), 16)); }

// ================================================================== memory pools
// Builder: device@0 flags@8 storage@0x10 size@0x18
NVN(nvnMemoryPoolBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnMemoryPoolBuilderSetDefaults) { memset(AP(0), 0, 0x20); G(uint32_t, A(0), 8) = 0x9; }
NVN(nvnMemoryPoolBuilderSetFlags) { G(uint32_t, A(0), 8) = (uint32_t)A(1); }
NVN(nvnMemoryPoolBuilderSetStorage) { G(uint64_t, A(0), 0x10) = A(1); G(uint64_t, A(0), 0x18) = A(2); }
NVN(nvnMemoryPoolBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnMemoryPoolBuilderGetFlags) { RET(G(uint32_t, A(0), 8)); }
NVN(nvnMemoryPoolBuilderGetMemory) { RET(G(uint64_t, A(0), 0x10)); }
NVN(nvnMemoryPoolBuilderGetSize) { RET(G(uint64_t, A(0), 0x18)); }

NVN(nvnMemoryPoolInitialize) {
    NvnPool *p = calloc(1, sizeof *p);
    p->storage = G(uint64_t, A(1), 0x10);
    p->size = G(uint64_t, A(1), 0x18);
    p->flags = G(uint32_t, A(1), 8);
    if (ENV("LARECOMP_LOG_POOLS"))
        LOG("[nvn] pool %llx: storage %llx size %#llx flags %#x (lr %llx)\n", (unsigned long long)A(0),
            (unsigned long long)p->storage, (unsigned long long)p->size, p->flags, (unsigned long long)c->x[30]);
    SET_HOST(A(0), p);
    RET(1);
}
// Objects the game destroys (frame interpolation, exec.c: a recorded frame that may reference them is not replayed)
uint64_t g_nvn_destroyed;
NVN(nvnMemoryPoolFinalize) {
    if (ENV("LARECOMP_LOG_POOLS")) LOG("[nvn] pool %llx finalized (lr %llx)\n", (unsigned long long)A(0), (unsigned long long)c->x[30]);
    g_nvn_destroyed++; rt_drain();
    NvnPool *p = HOST(NvnPool, A(0));
    extern void cmdbuf_pool_gone(uint64_t mem, uint64_t size);
    if (p) cmdbuf_pool_gone(p->storage, p->size);  // (out-of-line command records in its chunks: cmdbuf.c)
    free(p); SET_HOST(A(0), 0);
}
NVN(nvnMemoryPoolSetDebugLabel) { (void)c; }
NVN(nvnMemoryPoolMap) { RET(HOST(NvnPool, A(0))->storage); }
NVN(nvnMemoryPoolGetBufferAddress) { RET(HOST(NvnPool, A(0))->storage); }  // GPU address == CPU address
NVN(nvnMemoryPoolGetSize) { RET(HOST(NvnPool, A(0))->size); }
NVN(nvnMemoryPoolGetFlags) { RET(HOST(NvnPool, A(0))->flags); }
NVN(nvnMemoryPoolFlushMappedRange) {
    NvnPool *p = HOST(NvnPool, A(0));
    backend_pool_written(p->storage + A(1), A(2));
}
NVN(nvnMemoryPoolInvalidateMappedRange) { (void)c; }
NVN(nvnMemoryPoolMapVirtual) { RET(1); }

// ================================================================== buffers
// Builder: device@0 pool@8 offset@0x10 size@0x18. Buffer: address@0 pool@8 offset@0x10 size@0x18 id@0x20
NVN(nvnBufferBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnBufferBuilderSetDefaults) { memset(AP(0) + 8, 0, 0x18); }
NVN(nvnBufferBuilderSetStorage) {
    G(uint64_t, A(0), 8) = A(1); G(uint64_t, A(0), 0x10) = A(2); G(uint64_t, A(0), 0x18) = A(3);
}
NVN(nvnBufferBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnBufferBuilderGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnBufferBuilderGetMemoryOffset) { RET(G(uint64_t, A(0), 0x10)); }
NVN(nvnBufferBuilderGetSize) { RET(G(uint64_t, A(0), 0x18)); }
NVN(nvnBufferInitialize) {
    uint64_t pool = G(uint64_t, A(1), 8), off = G(uint64_t, A(1), 0x10);
    G(uint64_t, A(0), 0) = HOST(NvnPool, pool)->storage + off;
    G(uint64_t, A(0), 8) = pool;
    G(uint64_t, A(0), 0x10) = off;
    G(uint64_t, A(0), 0x18) = G(uint64_t, A(1), 0x18);
    G(uint64_t, A(0), 0x20) = next_debug_id++;
    extern void gpuheap_touch(uint64_t addr, uint64_t n);
    gpuheap_touch(G(uint64_t, A(0), 0), G(uint64_t, A(0), 0x18));  // (the GPU may write it unseen: backed now, gpuheap.c)
    RET(1);
}
NVN(nvnBufferFinalize) { (void)c; }
NVN(nvnBufferSetDebugLabel) { (void)c; }
NVN(nvnBufferMap) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnBufferGetAddress) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnBufferGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnBufferGetMemoryOffset) { RET(G(uint64_t, A(0), 0x10)); }
NVN(nvnBufferGetSize) { RET(G(uint64_t, A(0), 0x18)); }
NVN(nvnBufferGetDebugID) { RET(G(uint64_t, A(0), 0x20)); }
NVN(nvnBufferFlushMappedRange) { backend_pool_written(G(uint64_t, A(0), 0) + A(1), A(2)); }
NVN(nvnBufferInvalidateMappedRange) { (void)c; }

// ================================================================== textures
// TextureBuilder: device@0 flags@8 target@0xC width@0x10 height@0x14 depth@0x18 levels@0x1C
//   format@0x20 samples@0x24 swizzle@0x28 dsmode@0x2C stride@0x30 pool@0x38 offset@0x40
//   packaged@0x48 layout@0x50 glname@0x58
#define TB(T, off) G(T, A(0), off)
NVN(nvnTextureBuilderSetDevice) { TB(uint64_t, 0) = A(1); }
NVN(nvnTextureBuilderSetDefaults) {
    memset(AP(0) + 8, 0, 0x58);
    TB(uint32_t, 0xC) = 1;  // 2D
    TB(uint32_t, 0x10) = TB(uint32_t, 0x14) = TB(uint32_t, 0x18) = 1;
    TB(uint32_t, 0x1C) = 1;
    TB(uint8_t, 0x28) = 2; TB(uint8_t, 0x29) = 3; TB(uint8_t, 0x2A) = 4; TB(uint8_t, 0x2B) = 5;
}
NVN(nvnTextureBuilderSetFlags) { TB(uint32_t, 8) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetTarget) { TB(uint32_t, 0xC) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetWidth) { TB(uint32_t, 0x10) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetHeight) { TB(uint32_t, 0x14) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetDepth) { TB(uint32_t, 0x18) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetSize1D) { TB(uint32_t, 0x10) = (uint32_t)A(1); TB(uint32_t, 0x14) = 1; TB(uint32_t, 0x18) = 1; }
// (research, LARECOMP_LOG_TEXSIZE=<w>x<h>: the guest call chain of textures made that size)
static void texsize_trace(Cpu *c, uint32_t w, uint32_t h) {
    static uint32_t ww = 1, wh;
    static int n;
    if (ww == 1) { const char *e = ENV("LARECOMP_LOG_TEXSIZE"); ww = e ? (uint32_t)atoi(e) : 0; wh = e && strchr(e, 'x') ? (uint32_t)atoi(strchr(e, 'x') + 1) : 0; }
    if (!ww || w != ww || h != wh || n >= 40) return;
    n++;
    char line[512];
    int len = snprintf(line, sizeof line, "[texsize] %ux%u from %llx:", w, h, (unsigned long long)c->x[30]);
    uint64_t fp = c->x[29];
    for (int i = 0; i < 12 && fp >= GUEST_LO && fp < GUEST_HI && !(fp & 7) && len < 480; i++) {
        uint64_t lr = ((uint64_t *)(uintptr_t)fp)[1], up = ((uint64_t *)(uintptr_t)fp)[0];
        len += snprintf(line + len, sizeof line - (size_t)len, " %llx", (unsigned long long)lr);
        if (up <= fp) break;
        fp = up;
    }
    LOG("%s\n", line);
}
NVN(nvnTextureBuilderSetSize2D) {
    texsize_trace(c, (uint32_t)A(1), (uint32_t)A(2));
    TB(uint32_t, 0x10) = (uint32_t)A(1); TB(uint32_t, 0x14) = (uint32_t)A(2); TB(uint32_t, 0x18) = 1;
}
NVN(nvnTextureBuilderSetSize3D) {
    TB(uint32_t, 0x10) = (uint32_t)A(1); TB(uint32_t, 0x14) = (uint32_t)A(2); TB(uint32_t, 0x18) = (uint32_t)A(3);
}
NVN(nvnTextureBuilderSetLevels) { TB(uint32_t, 0x1C) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetFormat) { TB(uint32_t, 0x20) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetSamples) { TB(uint32_t, 0x24) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetSwizzle) {
    TB(uint8_t, 0x28) = (uint8_t)A(1); TB(uint8_t, 0x29) = (uint8_t)A(2);
    TB(uint8_t, 0x2A) = (uint8_t)A(3); TB(uint8_t, 0x2B) = (uint8_t)A(4);
}
NVN(nvnTextureBuilderSetDepthStencilMode) { TB(uint32_t, 0x2C) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetStride) { TB(uint64_t, 0x30) = A(1); }
NVN(nvnTextureBuilderSetStorage) { TB(uint64_t, 0x38) = A(1); TB(uint64_t, 0x40) = A(2); }
NVN(nvnTextureBuilderSetPackagedTextureData) { TB(uint64_t, 0x48) = A(1); }
NVN(nvnTextureBuilderSetPackagedTextureLayout) { TB(uint64_t, 0x50) = A(1) ? *(uint64_t *)AP(1) : 0; }
NVN(nvnTextureBuilderSetGLTextureName) { TB(uint32_t, 0x58) = (uint32_t)A(1); }
NVN(nvnTextureBuilderGetDevice) { RET(TB(uint64_t, 0)); }
NVN(nvnTextureBuilderGetFlags) { RET(TB(uint32_t, 8)); }
NVN(nvnTextureBuilderGetTarget) { RET(TB(uint32_t, 0xC)); }
NVN(nvnTextureBuilderGetWidth) { RET(TB(uint32_t, 0x10)); }
NVN(nvnTextureBuilderGetHeight) { RET(TB(uint32_t, 0x14)); }
NVN(nvnTextureBuilderGetDepth) { RET(TB(uint32_t, 0x18)); }
NVN(nvnTextureBuilderGetLevels) { RET(TB(uint32_t, 0x1C)); }
NVN(nvnTextureBuilderGetFormat) { RET(TB(uint32_t, 0x20)); }
NVN(nvnTextureBuilderGetSamples) { RET(TB(uint32_t, 0x24)); }
NVN(nvnTextureBuilderGetSwizzle) {
    for (int i = 0; i < 4; i++) *(uint32_t *)AP(1 + i) = TB(uint8_t, 0x28 + i);
}
NVN(nvnTextureBuilderGetDepthStencilMode) { RET(TB(uint32_t, 0x2C)); }
NVN(nvnTextureBuilderGetStride) { RET(TB(uint64_t, 0x30)); }
NVN(nvnTextureBuilderGetMemoryPool) { RET(TB(uint64_t, 0x38)); }
NVN(nvnTextureBuilderGetMemoryOffset) { RET(TB(uint64_t, 0x40)); }
NVN(nvnTextureBuilderGetPackagedTextureData) { RET(TB(uint64_t, 0x48)); }
NVN(nvnTextureBuilderGetPackagedTextureLayout) { RET(0); }
NVN(nvnTextureBuilderGetGLTextureName) { RET(TB(uint32_t, 0x58)); }
NVN(nvnTextureBuilderGetSparseTileLayout) { RET(0); }
NVN(nvnTextureBuilderGetZCullStorageSize) { RET(0); }
NVN(nvnTextureBuilderGetStorageClass) { RET(0); }
NVN(nvnTextureBuilderGetRawStorageClass) { RET(0); }

static bool target_is_array(uint32_t t) { return t == 3 || t == 4 || t == 6 || t == 8 || t == 9; }

static void builder_layout(uint64_t b, TexLayout *t) {
    uint32_t target = G(uint32_t, b, 0xC);
    t->fmt = nvn_format_info(G(uint32_t, b, 0x20));
    t->width = G(uint32_t, b, 0x10);
    t->height = target == 0 || target == 3 ? 1 : G(uint32_t, b, 0x14);
    if (target == 3) {  // 1D array: height is the layer count
        t->layers = G(uint32_t, b, 0x14);
    }
    t->depth = target == 2 ? G(uint32_t, b, 0x18) : 1;
    if (target_is_array(target) && target != 3) t->layers = G(uint32_t, b, 0x18);
    if (target == 8) t->layers = 6;
    if (!t->layers) t->layers = 1;
    t->levels = G(uint32_t, b, 0x1C) ? G(uint32_t, b, 0x1C) : 1;
    tegra_layout_init(t);
    // Packaged textures (BNTX) were swizzled offline with their own block height, which can differ
    // from the driver default: NVNpackagedTextureLayout's low word is log2(GOBs per block).
    uint64_t pl = G(uint64_t, b, 0x50);
    if (pl && (uint32_t)pl <= 5) t->block_height_log2 = (uint32_t)pl;
    if (pl && ENV("LARECOMP_LOG_PACKAGED")) {
        static int n;
        if (n++ < 4000) LOG("[nvn] packaged layout %016llx fmt %#x %ux%u lv %u -> default bh %u\n", pl, G(uint32_t, b, 0x20), t->width, t->height, t->levels, t->block_height_log2);
    }
}

static bool is_linear(uint64_t b) { return (G(uint32_t, b, 8) & 0x10) || G(uint32_t, b, 0xC) == 10; }

static uint64_t builder_storage_size(uint64_t b) {
    if (G(uint32_t, b, 0xC) == 10) {  // buffer texture
        return (uint64_t)G(uint32_t, b, 0x10) * nvn_format_info(G(uint32_t, b, 0x20))->bpb;
    }
    if (is_linear(b)) return G(uint64_t, b, 0x30) * G(uint32_t, b, 0x14);
    TexLayout t = {0};
    builder_layout(b, &t);
    return tegra_total_size(&t);
}
NVN(nvnTextureBuilderGetStorageSize) { RET(builder_storage_size(A(0))); }
NVN(nvnTextureBuilderGetStorageAlignment) { RET(is_linear(A(0)) ? 32 : 512); }

static NvnTexture *texture_from_builder(uint64_t b) {
    NvnTexture *t = calloc(1, sizeof *t);
    t->target = G(uint32_t, b, 0xC);
    t->flags = G(uint32_t, b, 8);
    t->width = G(uint32_t, b, 0x10);
    t->height = G(uint32_t, b, 0x14);
    t->depth = G(uint32_t, b, 0x18);
    t->levels = G(uint32_t, b, 0x1C) ? G(uint32_t, b, 0x1C) : 1;
    t->format = G(uint32_t, b, 0x20);
    t->samples = G(uint32_t, b, 0x24);
    memcpy(t->swizzle, (void *)(uintptr_t)(b + 0x28), 4);
    t->ds_mode = G(uint32_t, b, 0x2C);
    t->stride = G(uint64_t, b, 0x30);
    uint64_t pool = G(uint64_t, b, 0x38);
    t->pool = pool ? HOST(NvnPool, pool) : NULL;
    t->pool_offset = G(uint64_t, b, 0x40);
    t->addr = t->pool ? t->pool->storage + t->pool_offset : 0;
    builder_layout(b, &t->layout);
    t->debug_id = next_debug_id++;
    if (ENV("LARECOMP_LOG_TEX"))
        LOG("[tex] #%u %llx %ux%ux%u lv%u fmt %#x target %u flags %#x swz %u%u%u%u stride %llu\n", t->debug_id,
            (unsigned long long)t->addr, t->width, t->height, t->depth, t->levels, t->format, t->target, t->flags,
            t->swizzle[0], t->swizzle[1], t->swizzle[2], t->swizzle[3], (unsigned long long)t->stride);
    return t;
}

// A texture-pool slot keeps the host object it was registered with. The game finalizes a texture and initializes it again
// in the same NVNtexture object (a bigger version streamed in) without registering the slot again: on the console the
// slot's descriptor still describes the texture's memory; here the slot kept the freed host object and, whenever malloc
// gave that memory to another texture, showed that one (the world after a capture: fences, signs and ferns with other
// textures until the area changed, letsgo-recomp's docs/HISTORICO.md 60). Finalize clears those slots and initializing the same object
// puts the new host object back. LARECOMP_POOL_FIX=0: as before, only counted (research).
extern NvnDescPool *g_texture_pools[64];
static uint64_t g_pool_dangling, g_pool_reattached, g_pool_mismatch;
static int pool_fix(void) {
    static int f = -1;
    if (f < 0) f = ENV("LARECOMP_POOL_FIX") ? atoi(ENV("LARECOMP_POOL_FIX")) : 1;
    return f;
}
static void pool_note(const char *what, uint64_t n) { if (!(n & (n - 1))) LOG("[nvn] texture pool: %s %llu\n", what, (unsigned long long)n); }

// The slots each NVNtexture object was registered in (register_texture): finalize / initialize look at those, not at
// every slot of every pool - that scan ran on each texture the game streams in and out, 1.75% of Z-A's CPU on the
// Steam Frame. A list can hold slots registered with another object since (checked: e->guest), dropped when seen.
typedef struct { NvnDescPool *d; int id; } SlotRef;
typedef struct ObjSlots { uint64_t obj; int n, cap; SlotRef *v; struct ObjSlots *next; } ObjSlots;
#define OS_BUCKETS 16384
static ObjSlots *g_os[OS_BUCKETS];
static pthread_mutex_t g_os_lock = PTHREAD_MUTEX_INITIALIZER;
static ObjSlots *obj_slots(uint64_t obj, bool make) {  // (g_os_lock held)
    unsigned h = (unsigned)((obj >> 4) ^ (obj >> 18)) & (OS_BUCKETS - 1);
    for (ObjSlots *s = g_os[h]; s; s = s->next) if (s->obj == obj) return s;
    if (!make) return NULL;
    ObjSlots *s = calloc(1, sizeof *s);
    s->obj = obj, s->next = g_os[h], g_os[h] = s;
    return s;
}
static void obj_slot_add(uint64_t obj, NvnDescPool *d, int id) {
    pthread_mutex_lock(&g_os_lock);
    ObjSlots *s = obj_slots(obj, true);
    bool have = false;
    for (int i = 0; i < s->n && !have; i++) have = s->v[i].d == d && s->v[i].id == id;
    if (!have) {
        if (s->n == s->cap) s->cap = s->cap ? s->cap * 2 : 2, s->v = realloc(s->v, (size_t)s->cap * sizeof *s->v);
        s->v[s->n++] = (SlotRef){d, id};
    }
    pthread_mutex_unlock(&g_os_lock);
}
// A slot registered with another object now: off obj's list, and the list freed once empty. Only dropping stale
// references as FOR_OBJ_SLOTS sees them left one list per texture object the game ever made (streamed textures come
// and go at new addresses, and a finalized object's list was never looked at again).
static void obj_slot_remove(uint64_t obj, NvnDescPool *d, int id) {
    pthread_mutex_lock(&g_os_lock);
    unsigned h = (unsigned)((obj >> 4) ^ (obj >> 18)) & (OS_BUCKETS - 1);
    for (ObjSlots **p = &g_os[h]; *p; p = &(*p)->next) {
        ObjSlots *s = *p;
        if (s->obj != obj) continue;
        for (int i = 0; i < s->n; i++)
            if (s->v[i].d == d && s->v[i].id == id) { s->v[i] = s->v[--s->n]; break; }
        if (!s->n) *p = s->next, free(s->v), free(s);
        break;
    }
    pthread_mutex_unlock(&g_os_lock);
}
static void obj_slots_reset(void) {
    pthread_mutex_lock(&g_os_lock);
    for (int h = 0; h < OS_BUCKETS; h++)
        for (ObjSlots *s = g_os[h], *n; s; s = n) n = s->next, free(s->v), free(s);
    memset(g_os, 0, sizeof g_os);
    pthread_mutex_unlock(&g_os_lock);
}
// each slot still registered with obj (stale references dropped): f(entry)
#define FOR_OBJ_SLOTS(obj, e, body)                                                       \
    do {                                                                                  \
        pthread_mutex_lock(&g_os_lock);                                                   \
        ObjSlots *s_ = obj_slots(obj, false);                                             \
        for (int i_ = 0; s_ && i_ < s_->n;) {                                             \
            NvnTexEntry *e = &((NvnTexEntry *)s_->v[i_].d->entries)[s_->v[i_].id];        \
            if (e->guest != (obj)) { s_->v[i_] = s_->v[--s_->n]; continue; }              \
            body;                                                                         \
            i_++;                                                                         \
        }                                                                                 \
        pthread_mutex_unlock(&g_os_lock);                                                 \
    } while (0)

static void pool_forget(NvnTexture *t) {
    FOR_OBJ_SLOTS(t->obj, e, {
        if (e->tex == t) {
            pool_note("slots left on a finalized texture", ++g_pool_dangling);
            if (pool_fix()) e->tex = NULL;
        }
    });
}
static void pool_reattach(uint64_t guest, NvnTexture *t) {
    FOR_OBJ_SLOTS(guest, e, {
        if (e->tex != t) {
            if (!e->tex) { e->tex = t; pool_note("slots given the texture initialized again", ++g_pool_reattached); }
            else if (!pool_fix()) pool_note("slots on another object than the texture initialized again", ++g_pool_mismatch);
        }
    });
}
NVN(nvnTextureInitialize) {
    NvnTexture *t = texture_from_builder(A(1));
    t->obj = A(0);
    if (t->flags & 2)
        LOG("[nvn] video texture %p: %ux%u fmt %#x flags %#x target %u addr %llx stride %llu\n", t, t->width, t->height,
            t->format, t->flags, t->target, t->addr, t->stride);
    SET_HOST(A(0), t);
    pool_reattach(A(0), t);
    RET(1);
}
static void texture_destroy_on_render_thread(void *t) { backend_texture_destroyed(t); }
NVN(nvnTextureFinalize) {
    g_nvn_destroyed++;
    NvnTexture *t = HOST(NvnTexture, A(0));
    if (t) { rt_call(texture_destroy_on_render_thread, t); pool_forget(t); free(t); }
    SET_HOST(A(0), 0);
}
NVN(nvnTextureSetDebugLabel) {
    NvnTexture *t = HOST(NvnTexture, A(0));
    if (t && A(1)) strncpy(t->label, (const char *)AP(1), sizeof t->label - 1);
}
#define TEX HOST(NvnTexture, A(0))
NVN(nvnTextureGetFlags) { RET(TEX->flags); }
NVN(nvnTextureGetTarget) { RET(TEX->target); }
NVN(nvnTextureGetWidth) { RET(TEX->width); }
NVN(nvnTextureGetHeight) { RET(TEX->height); }
NVN(nvnTextureGetDepth) { RET(TEX->depth); }
NVN(nvnTextureGetLevels) { RET(TEX->levels); }
NVN(nvnTextureGetFormat) { RET(TEX->format); }
NVN(nvnTextureGetSamples) { RET(TEX->samples); }
NVN(nvnTextureGetSwizzle) { for (int i = 0; i < 4; i++) *(uint32_t *)AP(1 + i) = TEX->swizzle[i]; }
NVN(nvnTextureGetDepthStencilMode) { RET(TEX->ds_mode); }
NVN(nvnTextureGetStride) { RET(TEX->stride); }
NVN(nvnTextureGetTextureAddress) { RET(TEX->addr); }
NVN(nvnTextureGetMemoryPool) { RET(0); }
NVN(nvnTextureGetMemoryOffset) { RET(TEX->pool_offset); }
NVN(nvnTextureGetStorageSize) { RET(tegra_total_size(&TEX->layout)); }
NVN(nvnTextureGetDebugID) { RET(TEX->debug_id); }
NVN(nvnTextureGetViewOffset) { RET(0); }
NVN(nvnTextureGetStorageClass) { RET(0); }
NVN(nvnTextureGetRawStorageClass) { RET(0); }
NVN(nvnTextureGetSparseTileLayout) { RET(0); }
NVN(nvnTextureGetZCullStorageSize) { RET(0); }
NVN(nvnTextureCompare) { RET(A(0) == A(1)); }
NVN(nvnTextureFlushTexels) { (void)c; }
NVN(nvnTextureInvalidateTexels) { (void)c; }

// Texture views (our layout of the guest struct, nvn.h; only its first 0x18 bytes are ours to write)
#define TV(T, off) G(T, A(0), off)
NVN(nvnTextureViewSetDefaults) { memset(AP(0), 0, sizeof(NvnTextureView)); }
NVN(nvnTextureViewSetLevels) { TV(uint32_t, 0) |= 1; TV(uint16_t, 4) = (uint16_t)A(1); TV(uint16_t, 6) = (uint16_t)A(2); }
NVN(nvnTextureViewSetLayers) { TV(uint32_t, 0) |= 2; TV(uint16_t, 8) = (uint16_t)A(1); TV(uint16_t, 10) = (uint16_t)A(2); }
NVN(nvnTextureViewSetFormat) { TV(uint32_t, 0) |= 4; TV(uint32_t, 12) = (uint32_t)A(1); }
NVN(nvnTextureViewSetSwizzle) {
    NvnTextureView *v = AP(0);
    v->setmask |= 8;
    v->swizzle[0] = (uint8_t)A(1); v->swizzle[1] = (uint8_t)A(2); v->swizzle[2] = (uint8_t)A(3); v->swizzle[3] = (uint8_t)A(4);
}
NVN(nvnTextureViewSetDepthStencilMode) { NvnTextureView *v = AP(0); v->setmask |= 16; v->ds_mode = (uint8_t)A(1); }
NVN(nvnTextureViewSetTarget) { NvnTextureView *v = AP(0); v->setmask |= 32; v->target = (uint16_t)A(1); }
NVN(nvnTextureViewGetLevels) { *(int32_t *)AP(1) = TV(uint16_t, 4); *(int32_t *)AP(2) = TV(uint16_t, 6); RET(TV(uint32_t, 0) & 1); }
NVN(nvnTextureViewGetLayers) { *(int32_t *)AP(1) = TV(uint16_t, 8); *(int32_t *)AP(2) = TV(uint16_t, 10); RET(!!(TV(uint32_t, 0) & 2)); }
NVN(nvnTextureViewGetFormat) { *(uint32_t *)AP(1) = TV(uint32_t, 12); RET(!!(TV(uint32_t, 0) & 4)); }
NVN(nvnTextureViewGetSwizzle) { NvnTextureView *v = AP(0); for (int i = 0; i < 4; i++) *(uint32_t *)AP(1 + i) = v->swizzle[i]; RET(!!(v->setmask & 8)); }
NVN(nvnTextureViewGetDepthStencilMode) { NvnTextureView *v = AP(0); *(uint32_t *)AP(1) = v->ds_mode; RET(!!(v->setmask & 16)); }
NVN(nvnTextureViewGetTarget) { NvnTextureView *v = AP(0); *(uint32_t *)AP(1) = v->target; RET(!!(v->setmask & 32)); }
NVN(nvnTextureViewCompare) { RET(memcmp(AP(0), AP(1), sizeof(NvnTextureView)) == 0); }

NVN(nvnTextureWriteTexels) {
    // (texture, view, region, data) -- linear data into block-linear storage
    NvnTexture *t = HOST(NvnTexture, A(0));
    const int32_t *r = AP(2);  // xoffset, yoffset, zoffset, width, height, depth
    LevelLayout l = tegra_level(&t->layout, 0);
    if (r && r[0] == 0 && r[1] == 0 && (uint32_t)r[3] == t->width && (uint32_t)r[4] == t->height) {
        tegra_swizzle(AP(3), (uint8_t *)(uintptr_t)t->addr + l.offset, l.wb, l.hb, r[5] > 0 ? (uint32_t)r[5] : 1,
                      t->layout.fmt->bpb, l.block_height_log2, l.block_depth_log2);
        backend_pool_written(t->addr, tegra_total_size(&t->layout));
    } else {
        LOG("[nvn] partial WriteTexels not implemented\n");
    }
}
NVN(nvnTextureWriteTexelsStrided) { LOG("[nvn] WriteTexelsStrided not implemented\n"); (void)c; }
NVN(nvnTextureReadTexels) { LOG("[nvn] ReadTexels not implemented\n"); (void)c; }
NVN(nvnTextureReadTexelsStrided) { (void)c; }

// ================================================================== samplers
// SamplerBuilder (64 bytes in the guest's memory, often on its stack): device@0, then a packed copy of the state at 8.
// The full NvnSamplerDesc (72 bytes) wrote past the builder's end and over the caller's saved frame pointer and
// return address (Legends Z-A crashed right after creating its first sampler).
typedef struct {
    uint8_t min_filter, mag_filter, wrap[3], compare_mode, compare_func, reduction;
    float lod_min, lod_max, lod_bias, max_aniso;
    float border[4];
} NvnSamplerBuild;
_Static_assert(8 + sizeof(NvnSamplerBuild) <= 64, "NVNsamplerBuilder is 64 bytes");
#define SD ((NvnSamplerBuild *)(AP(0) + 8))
static void sampler_desc_from_build(NvnSamplerDesc *d, const NvnSamplerBuild *b) {
    memset(d, 0, sizeof *d);
    d->min_filter = b->min_filter; d->mag_filter = b->mag_filter;
    for (int i = 0; i < 3; i++) d->wrap[i] = b->wrap[i];
    d->compare_mode = b->compare_mode; d->compare_func = b->compare_func; d->reduction = b->reduction;
    d->lod_min = b->lod_min; d->lod_max = b->lod_max; d->lod_bias = b->lod_bias; d->max_aniso = b->max_aniso;
    memcpy(d->border, b->border, 16);
}
NVN(nvnSamplerBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnSamplerBuilderSetDefaults) {
    NvnSamplerBuild *d = SD;
    memset(d, 0, sizeof *d);
    d->min_filter = 0; d->mag_filter = 0;
    d->wrap[0] = d->wrap[1] = d->wrap[2] = 1;  // REPEAT
    d->lod_min = 0; d->lod_max = 1000.0f; d->max_aniso = 1.0f;
    d->compare_func = 4;
}
NVN(nvnSamplerBuilderSetMinMagFilter) { SD->min_filter = (uint32_t)A(1); SD->mag_filter = (uint32_t)A(2); }
NVN(nvnSamplerBuilderSetWrapMode) { SD->wrap[0] = (uint32_t)A(1); SD->wrap[1] = (uint32_t)A(2); SD->wrap[2] = (uint32_t)A(3); }
NVN(nvnSamplerBuilderSetLodClamp) { SD->lod_min = AF(0); SD->lod_max = AF(1); }
NVN(nvnSamplerBuilderSetLodBias) { SD->lod_bias = AF(0); }
NVN(nvnSamplerBuilderSetCompare) { SD->compare_mode = (uint32_t)A(1); SD->compare_func = (uint32_t)A(2); }
NVN(nvnSamplerBuilderSetBorderColor) { memcpy(SD->border, AP(1), 16); }
NVN(nvnSamplerBuilderSetBorderColori) { memcpy(SD->border, AP(1), 16); }
NVN(nvnSamplerBuilderSetBorderColorui) { memcpy(SD->border, AP(1), 16); }
NVN(nvnSamplerBuilderSetMaxAnisotropy) { SD->max_aniso = AF(0); }
NVN(nvnSamplerBuilderSetReductionFilter) { SD->reduction = (uint32_t)A(1); }
NVN(nvnSamplerBuilderSetLodSnap) { (void)c; }
NVN(nvnSamplerBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnSamplerBuilderGetMinMagFilter) { *(uint32_t *)AP(1) = SD->min_filter; *(uint32_t *)AP(2) = SD->mag_filter; }
NVN(nvnSamplerBuilderGetWrapMode) { for (int i = 0; i < 3; i++) *(uint32_t *)AP(1 + i) = SD->wrap[i]; }
NVN(nvnSamplerBuilderGetLodClamp) { *(float *)AP(1) = SD->lod_min; *(float *)AP(2) = SD->lod_max; }
NVN(nvnSamplerBuilderGetLodBias) { RETF(SD->lod_bias); }
NVN(nvnSamplerBuilderGetCompare) { *(uint32_t *)AP(1) = SD->compare_mode; *(uint32_t *)AP(2) = SD->compare_func; }
NVN(nvnSamplerBuilderGetBorderColor) { memcpy(AP(1), SD->border, 16); }
NVN(nvnSamplerBuilderGetBorderColori) { memcpy(AP(1), SD->border, 16); }
NVN(nvnSamplerBuilderGetBorderColorui) { memcpy(AP(1), SD->border, 16); }
NVN(nvnSamplerBuilderGetMaxAnisotropy) { RETF(SD->max_aniso); }
NVN(nvnSamplerBuilderGetReductionFilter) { RET(SD->reduction); }
NVN(nvnSamplerBuilderGetLodSnap) { RETF(0); }

NVN(nvnSamplerInitialize) {
    NvnSamplerDesc *d = malloc(sizeof *d);
    sampler_desc_from_build(d, (const NvnSamplerBuild *)(AP(1) + 8));
    SET_HOST(A(0), d);
    RET(1);
}
NVN(nvnSamplerFinalize) { free(HOST(NvnSamplerDesc, A(0))); SET_HOST(A(0), 0); }
NVN(nvnSamplerSetDebugLabel) { (void)c; }
#define SMP HOST(NvnSamplerDesc, A(0))
NVN(nvnSamplerGetMinMagFilter) { *(uint32_t *)AP(1) = SMP->min_filter; *(uint32_t *)AP(2) = SMP->mag_filter; }
NVN(nvnSamplerGetWrapMode) { for (int i = 0; i < 3; i++) *(uint32_t *)AP(1 + i) = SMP->wrap[i]; }
NVN(nvnSamplerGetLodClamp) { *(float *)AP(1) = SMP->lod_min; *(float *)AP(2) = SMP->lod_max; }
NVN(nvnSamplerGetLodBias) { RETF(SMP->lod_bias); }
NVN(nvnSamplerGetCompare) { *(uint32_t *)AP(1) = SMP->compare_mode; *(uint32_t *)AP(2) = SMP->compare_func; }
NVN(nvnSamplerGetBorderColor) { memcpy(AP(1), SMP->border, 16); }
NVN(nvnSamplerGetBorderColori) { memcpy(AP(1), SMP->border, 16); }
NVN(nvnSamplerGetBorderColorui) { memcpy(AP(1), SMP->border, 16); }
NVN(nvnSamplerGetMaxAnisotropy) { RETF(SMP->max_aniso); }
NVN(nvnSamplerGetReductionFilter) { RET(SMP->reduction); }
NVN(nvnSamplerCompare) { RET(memcmp(HOST(NvnSamplerDesc, A(0)), HOST(NvnSamplerDesc, A(1)), sizeof(NvnSamplerDesc)) == 0); }
NVN(nvnSamplerGetDebugID) { RET(A(0)); }

// ================================================================== descriptor pools
NvnDescPool *g_texture_pools[64], *g_sampler_pools[64];
RT_ON_RESET(desc_pools_reset) {
    memset(g_texture_pools, 0, sizeof g_texture_pools); memset(g_sampler_pools, 0, sizeof g_sampler_pools);
    obj_slots_reset();
}

static NvnDescPool *desc_pool_init(uint64_t guest, uint64_t pool, uint64_t off, int count, size_t entry, NvnDescPool **reg) {
    NvnDescPool *d = calloc(1, sizeof *d);
    d->addr = HOST(NvnPool, pool)->storage + off;
    d->count = count;
    d->entries = calloc(count, entry);
    SET_HOST(guest, d);
    G(uint64_t, guest, 8) = pool;
    G(uint64_t, guest, 16) = off;
    G(uint32_t, guest, 24) = (uint32_t)count;
    for (int i = 0; i < 64; i++) if (!reg[i]) { reg[i] = d; break; }
    return d;
}

NVN(nvnTexturePoolInitialize) {
    desc_pool_init(A(0), A(1), A(2), (int)A(3), sizeof(NvnTexEntry), g_texture_pools);
    RET(1);
}
NVN(nvnTexturePoolFinalize) { (void)c; }
NVN(nvnTexturePoolSetDebugLabel) { (void)c; }
NVN(nvnTexturePoolGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnTexturePoolGetMemoryOffset) { RET(G(uint64_t, A(0), 16)); }
NVN(nvnTexturePoolGetSize) { RET(G(uint32_t, A(0), 24)); }
static void register_texture(Cpu *c) {
    NvnDescPool *d = HOST(NvnDescPool, A(0));
    int id = (int)A(1);
    if (id < 0 || id >= d->count) return;
    NvnTexEntry *e = &((NvnTexEntry *)d->entries)[id];
    if (e->guest && e->guest != A(2)) obj_slot_remove(e->guest, d, id);
    e->tex = HOST(NvnTexture, A(2));
    e->guest = A(2);
    obj_slot_add(A(2), d, id);  // (pool_forget / pool_reattach: this object's slots)
    e->has_view = A(3) != 0;
    if (e->has_view) memcpy(&e->view, AP(3), sizeof e->view);
}
NVN(nvnTexturePoolRegisterTexture) { register_texture(c); }
NVN(nvnTexturePoolRegisterImage) { register_texture(c); }

NVN(nvnSamplerPoolInitialize) {
    desc_pool_init(A(0), A(1), A(2), (int)A(3), sizeof(NvnSamplerDesc), g_sampler_pools);
    RET(1);
}
NVN(nvnSamplerPoolFinalize) { (void)c; }
NVN(nvnSamplerPoolSetDebugLabel) { (void)c; }
NVN(nvnSamplerPoolGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnSamplerPoolGetMemoryOffset) { RET(G(uint64_t, A(0), 16)); }
NVN(nvnSamplerPoolGetSize) { RET(G(uint32_t, A(0), 24)); }
NVN(nvnSamplerPoolRegisterSampler) {
    NvnDescPool *d = HOST(NvnDescPool, A(0));
    int id = (int)A(1);
    if (id >= 0 && id < d->count) ((NvnSamplerDesc *)d->entries)[id] = *HOST(NvnSamplerDesc, A(2));
}
NVN(nvnSamplerPoolRegisterSamplerBuilder) {
    NvnDescPool *d = HOST(NvnDescPool, A(0));
    int id = (int)A(1);
    if (id >= 0 && id < d->count) sampler_desc_from_build(&((NvnSamplerDesc *)d->entries)[id], (const NvnSamplerBuild *)(AP(2) + 8));
}

NvnDescPool *nvn_find_pool(NvnDescPool **reg, uint64_t guest_pool) {
    return guest_pool ? HOST(NvnDescPool, guest_pool) : NULL;
}

// ================================================================== programs
NVN(nvnProgramInitialize) { SET_HOST(A(0), calloc(1, sizeof(NvnProgram))); RET(1); }
NVN(nvnProgramFinalize) { g_nvn_destroyed++; rt_drain(); free(HOST(NvnProgram, A(0))); SET_HOST(A(0), 0); }
NVN(nvnProgramSetDebugLabel) { (void)c; }
NVN(nvnProgramSetSampleShading) { (void)c; }
NVN(nvnProgramSetSubroutineLinkage) { RET(1); }
// The code up to the compiler constants (its length; 4 KB when there are none) and the constants. The renderer keyed
// translated programs by code address only: after a capture Let's Go loads shaders again at addresses that held other
// shaders before ("[shader] ... now holds another shader" in the log), and the world drew with old translations (other
// texture layouts: ferns with the sign's texture, black trees, flat-coloured plants until the area changed;
// letsgo-recomp's docs/HISTORICO.md 66).
static uint64_t shader_content_hash(const NvnShaderStage *s) {
    uint64_t h = 0xcbf29ce484222325ull;
    uint32_t n = s->const_offset ? s->const_offset : 0x1000;
    if (n > 0x100000) n = 0x100000;
    const uint64_t *w = (const uint64_t *)(uintptr_t)s->code_addr;
    for (uint32_t i = 0; i < n / 8; i++) h = (h ^ w[i]) * 0x100000001b3ull;
    if (s->const_offset && s->const_size && s->const_size <= 0x10000) {
        const uint8_t *k = (const uint8_t *)(uintptr_t)(s->code_addr + s->const_offset);
        for (uint32_t i = 0; i < s->const_size; i++) h = (h ^ k[i]) * 0x100000001b3ull;
    }
    return h ? h : 1;
}

static void set_shaders(Cpu *c, int stride) {
    NvnProgram *p = HOST(NvnProgram, A(0));
    int n = (int)A(1);
    const uint8_t *data = AP(2);  // NVNshaderData[n] = {u64 data (GPU addr), const void* control}
    memset(p->stages, 0, sizeof p->stages);
    p->stage_mask = 0;
    p->backend = NULL;
    for (int i = 0; i < n; i++) {
        uint64_t code = *(uint64_t *)(data + i * stride);
        const uint8_t *control = (const uint8_t *)(uintptr_t) * (uint64_t *)(data + i * stride + 8);
        // stage from the Maxwell shader program header (after the 0x30-byte NVN header)
        uint32_t sph0 = *(uint32_t *)(uintptr_t)(code + 0x30);
        uint32_t stage;
        switch ((sph0 >> 10) & 0xF) {
        case 1: stage = 0; break;  // vertex
        case 5: stage = 1; break;  // fragment
        case 4: stage = 2; break;  // geometry
        case 2: stage = 3; break;  // tess control
        case 3: stage = 4; break;  // tess eval
        default: stage = 5; break; // compute (no SPH)
        }
        if (*(uint32_t *)(uintptr_t)code != 0x12345678) stage = 5;
        p->stages[stage].stage = stage;
        p->stages[stage].code_addr = code;
        p->stages[stage].control = control;
        p->stages[stage].const_offset = *(const uint32_t *)(control + 0x700);
        p->stages[stage].const_size = *(const uint32_t *)(control + 0x6FC);
        p->stages[stage].content_hash = shader_content_hash(&p->stages[stage]);
        if (stage <= 1) {  // (a container not seen yet: all its programs compiled ahead of their draws, runtime/nvn/vulkan/warmup.c)
            extern void warmup_capture(uint64_t control);
            warmup_capture((uint64_t)(uintptr_t)control);
        }
        p->stage_mask |= 1u << stage;
        if (stage == 0) {
            // Transform feedback layout (GLSLC control section, worked out on PLA's skinning shader: 0x20.. 0x22 at
            // 0xdc + i * 0x80 = the varyings, then per buffer the component count at 0x4dc + b * 0x80 and the strides
            // as 4 u16 at 0x6dc). Captured components follow the shader's written outputs in order (exec.c).
            for (int b = 0; b < 4; b++) {
                uint32_t n_ = *(const uint32_t *)(control + 0x4dc + b * 0x80);
                uint16_t s_ = *(const uint16_t *)(control + 0x6dc + b * 2);
                bool ok = n_ <= 32 && s_ <= 512 && !(s_ & 3) && n_ * 4 <= s_;
                p->xfb_count[b] = ok ? (uint8_t)n_ : 0;
                p->xfb_stride[b] = ok && n_ ? s_ : 0;
            }
            if (p->xfb_count[0] && ENV("LARECOMP_LOG_XFB"))
                LOG("[xfb] program %llx layout: %u/%u %u/%u %u/%u %u/%u\n", (unsigned long long)code, p->xfb_count[0], p->xfb_stride[0],
                    p->xfb_count[1], p->xfb_stride[1], p->xfb_count[2], p->xfb_stride[2], p->xfb_count[3], p->xfb_stride[3]);
        }
    }
    RET(1);
}

NVN(nvnProgramSetShaders) { set_shaders(c, 16); }    // NVNshaderData {data, control}
NVN(nvnProgramSetShadersExt) { set_shaders(c, 24); } // NVNshaderDataExt {data, control, debug hash}

// ================================================================== window
// WindowBuilder: device@0 native@8 num@0x10 textures@0x18 interval@0x20 active@0x24
NVN(nvnWindowBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnWindowBuilderSetDefaults) { memset(AP(0) + 8, 0, 0x20); G(uint32_t, A(0), 0x20) = 1; }
NVN(nvnWindowBuilderSetNativeWindow) { G(uint64_t, A(0), 8) = A(1); }
NVN(nvnWindowBuilderSetTextures) { G(uint32_t, A(0), 0x10) = (uint32_t)A(1); G(uint64_t, A(0), 0x18) = A(2); }
NVN(nvnWindowBuilderSetPresentInterval) { G(uint32_t, A(0), 0x20) = (uint32_t)A(1); }
NVN(nvnWindowBuilderSetNumActiveTextures) { G(uint32_t, A(0), 0x24) = (uint32_t)A(1); }
NVN(nvnWindowBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnWindowBuilderGetNativeWindow) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnWindowBuilderGetNumTextures) { RET(G(uint32_t, A(0), 0x10)); }
NVN(nvnWindowBuilderGetTexture) {
    int i = (int)A(1);
    RET(i < (int)G(uint32_t, A(0), 0x10) ? ((uint64_t *)(uintptr_t)G(uint64_t, A(0), 0x18))[i] : 0);
}
NVN(nvnWindowBuilderGetPresentInterval) { RET(G(uint32_t, A(0), 0x20)); }
NVN(nvnWindowBuilderGetNumActiveTextures) { RET(G(uint32_t, A(0), 0x24)); }

NVN(nvnWindowInitialize) {
    NvnWindow *w = calloc(1, sizeof *w);
    w->num = (int)G(uint32_t, A(1), 0x10);
    if (w->num > 8) w->num = 8;
    memcpy(w->textures, (void *)(uintptr_t)G(uint64_t, A(1), 0x18), w->num * 8);
    w->interval = (int)G(uint32_t, A(1), 0x20);
    w->active = G(uint32_t, A(1), 0x24) ? (int)G(uint32_t, A(1), 0x24) : w->num;
    SET_HOST(A(0), w);
    LOG("[nvn] window with %d textures\n", w->num);
    RET(1);
}
NVN(nvnWindowFinalize) { (void)c; }
NVN(nvnWindowSetDebugLabel) { (void)c; }
#define WIN HOST(NvnWindow, A(0))
NVN(nvnWindowGetNativeWindow) { RET(0); }
NVN(nvnWindowGetNumTextures) { RET(WIN->num); }
NVN(nvnWindowGetNumActiveTextures) { RET(WIN->active); }
NVN(nvnWindowSetNumActiveTextures) { WIN->active = (int)A(1); }
NVN(nvnWindowGetPresentInterval) { RET(WIN->interval); }
NVN(nvnWindowSetPresentInterval) { WIN->interval = (int)A(1); }
NVN(nvnWindowSetCrop) { int *cr = WIN->crop; cr[0] = (int)A(1); cr[1] = (int)A(2); cr[2] = (int)A(3); cr[3] = (int)A(4); }
NVN(nvnWindowGetCrop) { memcpy(AP(1), WIN->crop, 16); }
NVN(nvnWindowAcquireTexture) {
    NvnWindow *w = WIN;
    if (A(1)) sync_of(A(1))->signaled = 1;
    *(int32_t *)AP(2) = w->next;
    w->next = (w->next + 1) % (w->active ? w->active : 1);
    RET(0);  // NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS
}
NVN(nvnQueueAcquireTexture) {
    NvnWindow *w = HOST(NvnWindow, A(1));
    *(int32_t *)AP(2) = w->next;
    w->next = (w->next + 1) % (w->active ? w->active : 1);
    RET(0);
}
// Frame pacing: on the console a present waits for the window's present interval in vsyncs (Let's Go: 2, 30 fps).
// Without it the game ran at ~240 fps and its audio stuttered. LARECOMP_FPS=<n> paces at n fps instead (0: off).
static void present_pace(int interval) {
    static int fps = -1;
    static uint64_t next;
    {   // Settings.background = pause: the game waits here while its window isn't focused (runtime/hle/input.c)
        extern bool input_background_paused(void);
        if (input_background_paused()) {
            LOG("[nvn] paused (the window isn't focused)\n");
            kernel_block_begin();
            while (input_background_paused()) plat_sleep_until_ns(plat_now_ns() + 50000000ull);
            kernel_block_end();
            LOG("[nvn] resumed\n");
            next = 0;  // (pacing starts over)
        }
    }
    if (fps < 0) {
        const char *e = ENV("LARECOMP_FPS");
        fps = e ? atoi(e) : -2;  // -2: the game's own interval
    }
    uint64_t period;
    extern bool fps60_enabled(void), fps60_world(void), pla60_enabled(void);
    extern void pla60_present(void);
    pla60_present();
    extern uint64_t pla60_period_ns(void);
    if (pla60_enabled() && fps != 0) period = pla60_period_ns();  // (runtime/hle/fps60.c: the Legends at 60 fps or more)
    else if (fps60_enabled()) period = fps60_world() ? 16666667ull : 33333333ull;  // (runtime/hle/fps60.c)
    else if (fps == -2) period = (uint64_t)(interval > 0 ? interval : 1) * 16666667ull;
    else if (fps > 0) period = 1000000000ull / (uint64_t)fps;
    else return;
    static uint64_t logged;
    if (logged != period) { logged = period; LOG("[nvn] present pacing: %.1f fps (interval %d)\n", 1e9 / period, interval); }
    uint64_t now = plat_now_ns();
    static int old_pace = -1;  // (LARECOMP_PACE_OLD=1, research: the core taken back at the frame's time, as before)
    if (old_pace < 0) old_pace = ENV("LARECOMP_PACE_OLD") != NULL;
    if (!next || now > next + 2 * period) next = now;  // first frame / fell behind: start over, no burst
    else if (old_pace) {
        kernel_block_begin();
        plat_sleep_until_ns(next);
        kernel_block_end();
        extern uint64_t g_wait_pace_add;
        g_wait_pace_add += plat_now_ns() - now;
    } else {
        // The emulated core goes to its other threads while the game thread waits (runtime/kernel.c), and is taken back
        // `margin` before the frame's time: taking it back waits for the thread holding it, which on a busy CPU took
        // milliseconds - the frame then started late. A 4-core PC capped at 60 had 6% of its frames at 20-25 ms (2.5% of
        // the time lost) while the same walk uncapped had almost none over 16.7 ms; the 60 fps cap dipped into the 50s on
        // a fast PC too. The rest is waited out holding the core: a sleep, then a spin for the last 0.3 ms (a sleep wakes
        // late by up to ~0.1 ms, more on a busy CPU). margin follows the latest take-backs (the slowest, decaying).
        // LARECOMP_PACE_LOG=1: how late they were, every 10 s.
        static int64_t margin = 2000000;
        static int plog = -1;
        static uint64_t n_frames, n_late, sum_back, max_back;
        if (plog < 0) plog = ENV("LARECOMP_PACE_LOG") != NULL;
        // (scaled to the time left: near the frame rate's limit - 144 on a game that does ~170 - holding the core early
        // took ~0.5-0.8 ms a frame from the threads sharing it and more frames came late, 2.75% -> 4.4% over 110% of
        // the period; there the take-back has little to wait for anyway)
        uint64_t left = next > now ? next - now : 0, hold = (uint64_t)margin < left / 4 ? (uint64_t)margin : left / 4, spin = 300000 < left / 8 ? 300000 : left / 8;
        uint64_t until = left > hold ? next - hold : now;  // (a late frame: no wait, the take-back measured alone)
        kernel_block_begin();
        if (until > now) plat_sleep_until_ns(until);
        kernel_block_end();
        uint64_t got = plat_now_ns();
        int64_t back = (int64_t)(got - until);  // (the sleep's overshoot + the core's take-back)
        margin = margin * 31 / 32;  // (decays ~3% a frame) ...
        if (back + 300000 > margin) margin = back + 300000;  // ... to the slowest recent take-back, with room
        if (margin < 500000) margin = 500000;
        if (margin > (int64_t)period / 3) margin = (int64_t)period / 3;
        if (next > got + spin) plat_sleep_until_ns(next - spin);
        while (plat_now_ns() < next) {
#if defined(__x86_64__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            __asm__ volatile("yield");
#endif
        }
        uint64_t end = plat_now_ns();
        n_frames++, n_late += got > next, sum_back += (uint64_t)back, max_back = (uint64_t)back > max_back ? (uint64_t)back : max_back;
        if (plog && n_frames % 600 == 0) {
            LOG("[pace] %llu frames: core taken back %.2f ms avg, %.2f max; %llu after the frame's time; margin now %.2f ms\n",
                (unsigned long long)n_frames, (double)sum_back / 600 / 1e6, (double)max_back / 1e6, (unsigned long long)n_late,
                (double)margin / 1e6);
            sum_back = max_back = 0, n_late = 0;
        }
        extern uint64_t g_wait_pace_add;
        g_wait_pace_add += end - now;
    }
    next += period;
}

static int g_last_interval;
int rt_present_interval(void) { return g_last_interval; }
NVN(nvnQueuePresentTexture) {
    NvnWindow *w = HOST(NvnWindow, A(1));
    int i = (int)A(2);
    {   // LARECOMP_POKE32="<guest addr>=<value>[;...]": 32-bit values written to the game's memory before every present
        static const char *poke = (const char *)1;  // (experiments: a constant the game reads, without rebuilding anything)
        if (poke == (const char *)1) poke = ENV("LARECOMP_POKE32");
        for (const char *p = poke; p && *p;) {
            char *eq;
            uint64_t pa = strtoull(p, &eq, 0);
            if (*eq != '=') break;
            char *end;
            uint32_t v = (uint32_t)strtoull(eq + 1, &end, 0);
            if (pa >= 0x7000000000ull && pa < 0x8000000000ull) *(uint32_t *)(uintptr_t)pa = v;
            p = *end == ';' ? end + 1 : end;
        }
    }
    g_last_interval = w->interval;
    g_wait_pace_ns += g_wait_pace_add, g_wait_pace_add = 0;
    wait_report();
    { extern void fps60_present(void); fps60_present(); }
    present_pace(w->interval);
    if (i >= 0 && i < w->num) rt_present(HOST(NvnTexture, w->textures[i]));
}

// ================================================================== small state objects
// BlendState (8): target, srcC, dstC, srcA, dstA, eqC, eqA, advanced
NVN(nvnBlendStateSetDefaults) {
    uint8_t *b = AP(0);
    b[0] = 0; b[1] = 2; b[2] = 1; b[3] = 2; b[4] = 1; b[5] = 1; b[6] = 1; b[7] = 0;
}
NVN(nvnBlendStateSetBlendTarget) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnBlendStateSetBlendFunc) {
    uint8_t *b = AP(0);
    b[1] = (uint8_t)A(1); b[2] = (uint8_t)A(2); b[3] = (uint8_t)A(3); b[4] = (uint8_t)A(4);
}
NVN(nvnBlendStateSetBlendEquation) { G(uint8_t, A(0), 5) = (uint8_t)A(1); G(uint8_t, A(0), 6) = (uint8_t)A(2); }
NVN(nvnBlendStateSetAdvancedMode) { G(uint8_t, A(0), 7) = (uint8_t)A(1); }
NVN(nvnBlendStateSetAdvancedOverlap) { (void)c; }
NVN(nvnBlendStateSetAdvancedPremultipliedSrc) { (void)c; }
NVN(nvnBlendStateSetAdvancedNormalizedDst) { (void)c; }
NVN(nvnBlendStateGetBlendTarget) { RET(G(uint8_t, A(0), 0)); }
NVN(nvnBlendStateGetBlendFunc) {
    uint8_t *b = AP(0);
    for (int i = 0; i < 4; i++) *(uint32_t *)AP(1 + i) = b[1 + i];
}
NVN(nvnBlendStateGetBlendEquation) { *(uint32_t *)AP(1) = G(uint8_t, A(0), 5); *(uint32_t *)AP(2) = G(uint8_t, A(0), 6); }
NVN(nvnBlendStateGetAdvancedMode) { RET(G(uint8_t, A(0), 7)); }
NVN(nvnBlendStateGetAdvancedOverlap) { RET(0); }
NVN(nvnBlendStateGetAdvancedPremultipliedSrc) { RET(1); }
NVN(nvnBlendStateGetAdvancedNormalizedDst) { RET(1); }

// ChannelMaskState (4): 4 bits per target
NVN(nvnChannelMaskStateSetDefaults) { G(uint32_t, A(0), 0) = 0xFFFFFFFF; }
NVN(nvnChannelMaskStateSetChannelMask) {
    int t = (int)A(1);
    uint32_t m = (A(2) ? 1 : 0) | (A(3) ? 2 : 0) | (A(4) ? 4 : 0) | (A(5) ? 8 : 0);
    G(uint32_t, A(0), 0) = (G(uint32_t, A(0), 0) & ~(0xFu << (t * 4))) | (m << (t * 4));
}
NVN(nvnChannelMaskStateGetChannelMask) {
    uint32_t m = G(uint32_t, A(0), 0) >> ((int)A(1) * 4);
    for (int i = 0; i < 4; i++) *(uint8_t *)AP(2 + i) = (m >> i) & 1;
}

// ColorState (4): blend enable mask, logic op, alpha test
NVN(nvnColorStateSetDefaults) { G(uint8_t, A(0), 0) = 0; G(uint8_t, A(0), 1) = 3; G(uint8_t, A(0), 2) = 8; }
NVN(nvnColorStateSetBlendEnable) {
    uint8_t m = G(uint8_t, A(0), 0), bit = (uint8_t)(1u << A(1));
    G(uint8_t, A(0), 0) = A(2) ? (m | bit) : (m & ~bit);
}
NVN(nvnColorStateSetLogicOp) { G(uint8_t, A(0), 1) = (uint8_t)A(1); }
NVN(nvnColorStateSetAlphaTest) { G(uint8_t, A(0), 2) = (uint8_t)A(1); }
NVN(nvnColorStateGetBlendEnable) { RET((G(uint8_t, A(0), 0) >> A(1)) & 1); }
NVN(nvnColorStateGetLogicOp) { RET(G(uint8_t, A(0), 1)); }
NVN(nvnColorStateGetAlphaTest) { RET(G(uint8_t, A(0), 2)); }

// DepthStencilState (8): [0]=depthTest [1]=depthWrite [2]=depthFunc [3]=stencilTest
//   [4]=front func|back func<<4 [5..7]=ops (front fail,zfail,zpass / back) in 4-bit fields
NVN(nvnDepthStencilStateSetDefaults) {
    uint8_t *d = AP(0);
    d[0] = 0; d[1] = 1; d[2] = 2; d[3] = 0; d[4] = 0x88;
    d[5] = 0x11; d[6] = 0x11; d[7] = 0x11;
}
NVN(nvnDepthStencilStateSetDepthTestEnable) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetDepthWriteEnable) { G(uint8_t, A(0), 1) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetDepthFunc) { G(uint8_t, A(0), 2) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetStencilTestEnable) { G(uint8_t, A(0), 3) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetStencilFunc) {
    uint8_t *d = AP(0);
    int face = (int)A(1);  // 1 front, 2 back, 3 both
    if (face & 1) d[4] = (d[4] & 0xF0) | (A(2) & 0xF);
    if (face & 2) d[4] = (d[4] & 0x0F) | (uint8_t)((A(2) & 0xF) << 4);
}
NVN(nvnDepthStencilStateSetStencilOp) {
    uint8_t *d = AP(0);
    int face = (int)A(1);
    uint32_t ops = (uint32_t)((A(2) & 0xF) | (A(3) & 0xF) << 4 | (A(4) & 0xF) << 8);
    uint32_t packed = d[5] | d[6] << 8 | d[7] << 16;
    if (face & 1) packed = (packed & ~0xFFFu) | ops;
    if (face & 2) packed = (packed & 0xFFF) | ops << 12;
    d[5] = (uint8_t)packed; d[6] = (uint8_t)(packed >> 8); d[7] = (uint8_t)(packed >> 16);
}
NVN(nvnDepthStencilStateGetDepthTestEnable) { RET(G(uint8_t, A(0), 0)); }
NVN(nvnDepthStencilStateGetDepthWriteEnable) { RET(G(uint8_t, A(0), 1)); }
NVN(nvnDepthStencilStateGetDepthFunc) { RET(G(uint8_t, A(0), 2)); }
NVN(nvnDepthStencilStateGetStencilTestEnable) { RET(G(uint8_t, A(0), 3)); }
NVN(nvnDepthStencilStateGetStencilFunc) { RET((A(1) & 2) ? G(uint8_t, A(0), 4) >> 4 : G(uint8_t, A(0), 4) & 0xF); }
NVN(nvnDepthStencilStateGetStencilOp) {
    uint8_t *d = AP(0);
    uint32_t packed = d[5] | d[6] << 8 | d[7] << 16;
    if (A(1) & 2) packed >>= 12;
    for (int i = 0; i < 3; i++) *(uint32_t *)AP(2 + i) = (packed >> (4 * i)) & 0xF;
}

// MultisampleState (0x18): enable@0 samples@4 raster@8 a2c@0xC a2cdither@0xD cov2color@0xE output@0xF
NVN(nvnMultisampleStateSetDefaults) { memset(AP(0), 0, 0x18); G(uint8_t, A(0), 0) = 1; G(uint8_t, A(0), 0xD) = 1; }
NVN(nvnMultisampleStateSetMultisampleEnable) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetSamples) { G(uint32_t, A(0), 4) = (uint32_t)A(1); }
NVN(nvnMultisampleStateSetRasterSamples) { G(uint32_t, A(0), 8) = (uint32_t)A(1); }
NVN(nvnMultisampleStateSetAlphaToCoverageEnable) { G(uint8_t, A(0), 0xC) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetAlphaToCoverageDither) { G(uint8_t, A(0), 0xD) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetCoverageToColorEnable) { G(uint8_t, A(0), 0xE) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetCoverageToColorOutput) { G(uint8_t, A(0), 0xF) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetCoverageModulationMode) { G(uint8_t, A(0), 0x10) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetSampleLocations) { (void)c; }
NVN(nvnMultisampleStateSetSampleLocationsEnable) { (void)c; }
NVN(nvnMultisampleStateSetSampleLocationsGridEnable) { (void)c; }
NVN(nvnMultisampleStateGetMultisampleEnable) { RET(G(uint8_t, A(0), 0)); }
NVN(nvnMultisampleStateGetSamples) { RET(G(uint32_t, A(0), 4)); }
NVN(nvnMultisampleStateGetRasterSamples) { RET(G(uint32_t, A(0), 8)); }
NVN(nvnMultisampleStateGetAlphaToCoverageEnable) { RET(G(uint8_t, A(0), 0xC)); }
NVN(nvnMultisampleStateGetAlphaToCoverageDither) { RET(G(uint8_t, A(0), 0xD)); }
NVN(nvnMultisampleStateGetCoverageToColorEnable) { RET(G(uint8_t, A(0), 0xE)); }
NVN(nvnMultisampleStateGetCoverageToColorOutput) { RET(G(uint8_t, A(0), 0xF)); }
NVN(nvnMultisampleStateGetCoverageModulationMode) { RET(G(uint8_t, A(0), 0x10)); }
NVN(nvnMultisampleStateGetSampleLocationsEnable) { RET(0); }
NVN(nvnMultisampleStateGetSampleLocationsGrid) { (void)c; }
NVN(nvnMultisampleStateGetSampleLocationsGridEnable) { RET(0); }

// PolygonState (4): cull, front face, polygon mode, offset enables
NVN(nvnPolygonStateSetDefaults) { uint8_t *p = AP(0); p[0] = 0; p[1] = 1; p[2] = 2; p[3] = 0; }
NVN(nvnPolygonStateSetCullFace) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnPolygonStateSetFrontFace) { G(uint8_t, A(0), 1) = (uint8_t)A(1); }
NVN(nvnPolygonStateSetPolygonMode) { G(uint8_t, A(0), 2) = (uint8_t)A(1); }
NVN(nvnPolygonStateSetPolygonOffsetEnables) { G(uint8_t, A(0), 3) = (uint8_t)A(1); }
NVN(nvnPolygonStateGetCullFace) { RET(G(uint8_t, A(0), 0)); }
NVN(nvnPolygonStateGetFrontFace) { RET(G(uint8_t, A(0), 1)); }
NVN(nvnPolygonStateGetPolygonMode) { RET(G(uint8_t, A(0), 2)); }
NVN(nvnPolygonStateGetPolygonOffsetEnables) { RET(G(uint8_t, A(0), 3)); }

// VertexAttribState (4): format bits 0-7, stream bits 8-12, relative offset bits 13-31
NVN(nvnVertexAttribStateSetDefaults) { G(uint32_t, A(0), 0) = 0; }
NVN(nvnVertexAttribStateSetFormat) {
    uint32_t v = G(uint32_t, A(0), 0);
    G(uint32_t, A(0), 0) = (v & 0x1F00) | (uint32_t)(A(1) & 0xFF) | (uint32_t)(A(2) << 13);
}
NVN(nvnVertexAttribStateSetStreamIndex) {
    G(uint32_t, A(0), 0) = (G(uint32_t, A(0), 0) & ~0x1F00u) | (uint32_t)((A(1) & 0x1F) << 8);
}
NVN(nvnVertexAttribStateGetFormat) {
    uint32_t v = G(uint32_t, A(0), 0);
    *(uint32_t *)AP(1) = v & 0xFF;
    *(int64_t *)AP(2) = v >> 13;
}
NVN(nvnVertexAttribStateGetStreamIndex) { RET((G(uint32_t, A(0), 0) >> 8) & 0x1F); }

// VertexStreamState (8): stride u32, divisor u32
NVN(nvnVertexStreamStateSetDefaults) { G(uint64_t, A(0), 0) = 0; }
NVN(nvnVertexStreamStateSetStride) { G(uint32_t, A(0), 0) = (uint32_t)A(1); }
NVN(nvnVertexStreamStateSetDivisor) { G(uint32_t, A(0), 4) = (uint32_t)A(1); }
NVN(nvnVertexStreamStateGetStride) { RET(G(uint32_t, A(0), 0)); }
NVN(nvnVertexStreamStateGetDivisor) { RET(G(uint32_t, A(0), 4)); }
