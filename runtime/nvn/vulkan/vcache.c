// Persistent VRAM copies of the game's vertex streams and index ranges. Read in place from the game's memory, every vertex fetch crosses
// PCIe (Legends: Arceus: ~7 ms of GPU a frame, 1.7 with the vertices in VRAM); copied per submission it is ~28 MB a frame
// of CPU copies. Here a stream range (the buffer range a draw binds) is copied to its own VRAM buffer once and used until
// the game writes it (runtime/pagewatch.c: its pages are read-only after the copy, the first store marks them dirty) and
// the next draw uploads it anew. Ranges written again and again (streamed geometry, CPU-built meshes) are volatile: they
// keep the old path for a while. Ranges the GPU may write (exec.c gpuw) never come here. Render thread only.
// LARECOMP_VCACHE=0: off.
#include <string.h>
#include <SDL3/SDL_timer.h>

#include "../../rt.h"
#include "vk.h"

typedef struct VEnt {
    uint64_t addr, len;
    VkBuffer buf;
    VmaAllocation mem;
    uint64_t used_frame;
    uint64_t used_seq;    // pending submission when last bound
    uint8_t *shadow;      // the range's bytes in its partial first and last pages (split)
    uint64_t win_frame;   // uploads: since this frame (a 120-frame window)
    uint32_t uploads;
    uint32_t stamp;       // the write clock when it was copied (runtime/pagewatch.c)
    uint64_t hash;        // (LARECOMP_VCACHE_VERIFY: the bytes copied)
    bool vol;             // volatile: not cached until vol_frame + 300
    uint64_t vol_frame;
    struct VEnt *next;
} VEnt;
#define NBUCKET 16384
static VEnt *g_tab[NBUCKET];
static uint64_t g_bytes;  // VRAM held
static uint64_t g_frame;  // vk_frame_counter at the last lookup
typedef struct { VkBuffer buf; VmaAllocation mem; uint64_t seq; } DeadBuf;
static DeadBuf *g_dead;  // released buffers, destroyed once the GPU is past their last submission
static int g_ndead, g_capdead;
uint64_t g_vcache_hits, g_vcache_uploads;
uint64_t prof_vc_new, prof_vc_create_ns, prof_vc_protect_ns, prof_vc_copy_ns, prof_vc_part_ns;  // (exec.c [spike]: new entries, their parts)
uint64_t g_vcache_why[8];  // (LARECOMP_VCACHE_LOG: misses too big / outside the heap, volatile, turned volatile; bytes copied in part, in full; entries)

static inline bool in_heap(uint64_t a) { return a >= HEAP_BASE && a < HEAP_BASE + HEAP_MAX; }

static void reap(bool all) {
    uint64_t done = vk_completed_seq();
    int k = 0;
    for (int i = 0; i < g_ndead; i++) {
        if (all || g_dead[i].seq <= done) vmaDestroyBuffer(g_vk.vma, g_dead[i].buf, g_dead[i].mem);
        else g_dead[k++] = g_dead[i];
    }
    g_ndead = k;
}

static void drop_buf(VEnt *e) {
    if (!e->buf) return;
    if (g_ndead == g_capdead) { g_capdead = g_capdead ? g_capdead * 2 : 256; g_dead = realloc(g_dead, g_capdead * sizeof *g_dead); }
    g_dead[g_ndead++] = (DeadBuf){e->buf, e->mem, vk_pending_seq()};
    g_bytes -= (e->len + 255) & ~255ull;
    e->buf = VK_NULL_HANDLE;
}

// Ranges no draw has asked for in a while. The game's streamed geometry comes and goes at new addresses, and every range
// drawn once kept its entry and edge shadow (up to 8 KB) until a reboot - drop_buf only gives back the VRAM - so host
// memory grew with the ground covered (Z-A, Lumiose: GBs over a long session, until the desktop was OOM-killed). Every
// 600 frames: copies not drawn for 3600 frames give back their VRAM (below the budget too: the game has likely reused
// that memory for something else), and entries without a copy not asked for in 600 frames go. Volatile ones wait for
// their window to end (the next draw would reset them then anyway).
static void sweep(uint64_t frame) {
    for (int b = 0; b < NBUCKET; b++)
        for (VEnt **p = &g_tab[b]; *p;) {
            VEnt *e = *p;
            if (e->buf && frame - e->used_frame > 3600) drop_buf(e);
            if (!e->buf && frame - e->used_frame > 600 && (!e->vol || frame - e->vol_frame >= 300)) {
                *p = e->next;
                free(e->shadow);
                free(e);
            } else {
                p = &e->next;
            }
        }
}

static bool enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = ENV("LARECOMP_VCACHE");
        on = (!v || atoi(v) != 0) && pagewatch_supported() && g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        if (on) LOG("[vk] vertex streams kept in VRAM, copied again when the game writes them\n");
    }
    return on;
}

// The range's pages fully inside it [*ilo, *ihi) are write-protected; the partial pages at its ends are shared with
// whatever lies next to it (Legends: Arceus writes there every frame), so the range's bytes in them are compared with a
// shadow copy instead (at most 2 x 4 KB, at each draw).
static void split(uint64_t addr, uint64_t len, uint64_t *ilo, uint64_t *ihi, uint64_t *hn, uint64_t *tn) {
    const uint64_t P = 4096, end = addr + len;
    uint64_t lo = (addr + P - 1) & ~(P - 1), hi = end & ~(P - 1);
    if (hi < lo) hi = lo;
    uint64_t he = lo < end ? lo : end, ts = hi > he ? hi : he;
    if (ts > end) ts = end;  // (inside one page: all head)
    *ilo = lo, *ihi = hi, *hn = he - addr, *tn = end - ts;
}

static bool edges_changed(VEnt *e, uint64_t hn, uint64_t tn) {
    const uint8_t *g = (const uint8_t *)(uintptr_t)e->addr;
    return (hn && memcmp(g, e->shadow, hn)) || (tn && memcmp(g + e->len - tn, e->shadow + hn, tn));
}

// [a, a + n) of the range to its buffer; the edge bytes in it to the shadow, from the same snapshot
static void copy_part(VkCommandBuffer cb, VEnt *e, uint64_t a, uint64_t n, uint64_t hn, uint64_t tn) {
    // The edges' shadow from the game's memory, before the upload copy: read back from the upload buffer (write-combined
    // memory, ~100 MB/s to read) it cost the render thread ~30 us a new stream - 4.4 ms of the frame where 150 came in
    // (Lumiose). A write in between leaves the shadow older than the copy: the next draw sees the edge changed, uploads again.
    const uint8_t *p = (const uint8_t *)(uintptr_t)a;
    uint64_t ts = e->addr + e->len - tn;
    if (hn && a == e->addr) memcpy(e->shadow, p, hn);
    if (tn && a <= ts && a + n >= ts + tn) memcpy(e->shadow + hn, p + (ts - a), tn);
    VkSpan s = vk_transient(n, 16);
    memcpy(s.ptr, p, n);
    VkBufferCopy rg = {s.off, a - e->addr, n};
    vkCmdCopyBuffer(cb, s.buf, e->buf, 1, &rg);  // (the upload buffer runs before the draws, then a barrier)
}

// The VRAM copy of a vertex stream or index range, uploaded now if new or written since (false: not cached - volatile, too big,
// outside the heap; the caller keeps its old path). A written range gets its written parts copied into its buffer - after
// a barrier against the earlier submissions still reading it - or, when a draw of the pending submission already uses it
// (the upload buffer runs before all of them), a new buffer with all of it.
// Research, LARECOMP_VCACHE_VERIFY=1: every 64th hit compares a hash of the game's bytes with the copy's ("[vcache] stale")
static int verify(void) {
    static int on = -1;
    if (on < 0) on = ENV("LARECOMP_VCACHE_VERIFY") != NULL;
    return on;
}
static uint64_t hash_range(uint64_t addr, uint64_t len) {
    const uint64_t *p = (const uint64_t *)(uintptr_t)addr;
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint64_t i = 0; i < len / 8; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    for (uint64_t i = len & ~7ull; i < len; i++) h = (h ^ ((const uint8_t *)(uintptr_t)addr)[i]) * 0x100000001b3ull;
    return h;
}
static void verify_hit(VEnt *e) {
    static uint64_t n, stale;
    if (!verify() || (++n & 63)) return;
    if (hash_range(e->addr, e->len) != e->hash) {
        stale++;
        LOG("[vcache] stale copy drawn: %llx +%llu (%llu of %llu checked)\n", (unsigned long long)e->addr, (unsigned long long)e->len,
            (unsigned long long)stale, (unsigned long long)(n >> 6));
        e->hash = hash_range(e->addr, e->len);  // (once per change)
    }
}

bool vcache_get(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off) {
    if (!enabled()) return false;
    if (!len || len > (64ull << 20) || !in_heap(addr) || !in_heap(addr + len - 1)) { g_vcache_why[0]++; return false; }
    uint64_t frame = vk_frame_counter(), seq = vk_pending_seq();
    if (frame != g_frame) {
        g_frame = frame;
        reap(false);
        if (g_bytes > (1536ull << 20)) {  // over budget: drop what hasn't been drawn for a while
            for (int b = 0; b < NBUCKET; b++)
                for (VEnt *e = g_tab[b]; e; e = e->next)
                    if (e->buf && frame - e->used_frame > 120) drop_buf(e);
        }
        static uint64_t swept;  // (a frame with no draws here never comes through: not frame % 600)
        if (frame - swept >= 600) swept = frame, sweep(frame);
    }
    uint32_t h = (uint32_t)(((addr >> 4) ^ (len * 0x9E3779B1u)) * 0x9E3779B1u) & (NBUCKET - 1);
    VEnt *e = g_tab[h];
    while (e && (e->addr != addr || e->len != len)) e = e->next;
    if (!e) {
        e = calloc(1, sizeof *e);
        e->addr = addr, e->len = len, e->next = g_tab[h];
        g_vcache_why[5]++;
        g_tab[h] = e;
    }
    if (e->vol) {
        if (frame - e->vol_frame < 300) { g_vcache_why[1]++; return false; }
        e->vol = false, e->uploads = 0;
    }
    e->used_frame = frame;
    if (e->buf && e->used_seq == seq) {  // (checked for this submission already: an upload would run before all of its
        g_vcache_hits++;                 // draws, so checking again only cost a memcmp of its edges - a draw's worth)
        *buf = e->buf, *off = 0;
        return true;
    }
    uint64_t ilo, ihi, hn, tn;
    split(addr, len, &ilo, &ihi, &hn, &tn);
    bool idirty = ihi > ilo && pagewatch_written_since(ilo, ihi - ilo, e->stamp), edirty = e->buf && edges_changed(e, hn, tn);
    if (e->buf && !idirty && !edirty) {
        g_vcache_hits++;
        verify_hit(e);
        e->used_seq = seq;
        *buf = e->buf, *off = 0;
        return true;
    }
    if (frame - e->win_frame >= 120) e->win_frame = frame, e->uploads = 0;
    if (e->buf && ++e->uploads > 20) {  // written in most frames: not for the cache (the page faults would cost more)
        e->vol = true, e->vol_frame = frame;
        drop_buf(e);
        g_vcache_why[2]++;
        if (ENV("LARECOMP_VCACHE_LOG")) LOG("[vcache] volatile %llx +%llu (frame %llu)\n", (unsigned long long)addr, (unsigned long long)len, (unsigned long long)frame);
        return false;
    }
    VkCommandBuffer cb = vk_upload_cmd();
    if (e->buf && e->used_seq != seq) {  // the written parts only
        static uint64_t barrier_seq;
        if (barrier_seq != seq) {  // (write after the earlier submissions' vertex reads)
            barrier_seq = seq;
            VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT};
            VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
            vkCmdPipelineBarrier2(cb, &di);
        }
        uint32_t now = pagewatch_now(), was = e->stamp;
        for (uint64_t pa = ilo; idirty && pa < ihi;) {
            if (!pagewatch_written_since(pa, 1, was)) { pa += 4096; continue; }
            uint64_t pe = pa + 4096;
            while (pe < ihi && pagewatch_written_since(pe, 1, was)) pe += 4096;
            pagewatch_protect(pa, pe - pa);
            copy_part(cb, e, pa, pe - pa, hn, tn);
            g_vcache_why[3] += pe - pa;
            pa = pe;
        }
        if (idirty) e->stamp = now;
        if (edirty) {
            if (hn) copy_part(cb, e, addr, hn, hn, tn);
            if (tn) copy_part(cb, e, addr + len - tn, tn, hn, tn);
            g_vcache_why[3] += hn + tn;
        }
    } else {
        uint64_t t0 = SDL_GetTicksNS();
        drop_buf(e);
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (len + 255) & ~255ull,
                                 .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &e->buf, &e->mem, NULL) != VK_SUCCESS) {
            e->buf = VK_NULL_HANDLE;
            e->vol = true, e->vol_frame = frame;
            return false;
        }
        g_bytes += bi.size;
        uint64_t t1 = SDL_GetTicksNS();
        e->stamp = pagewatch_now();
        if (ihi > ilo) pagewatch_protect(ilo, ihi - ilo);
        uint64_t t2 = SDL_GetTicksNS();
        if (!e->shadow && hn + tn) e->shadow = malloc(hn + tn);
        copy_part(cb, e, addr, len, hn, tn);
        g_vcache_why[4] += len;
        prof_vc_new++, prof_vc_create_ns += t1 - t0, prof_vc_protect_ns += t2 - t1, prof_vc_copy_ns += SDL_GetTicksNS() - t2;
    }
    g_vcache_uploads++;
    if (verify()) e->hash = hash_range(addr, len);
    e->used_seq = seq;
    *buf = e->buf, *off = 0;
    return true;
}

// [addr, addr + len) turned out to be written by the GPU (exec.c gpuw_note: a compute dispatch's storage buffer, transform
// feedback): copies made of it before are dropped - the CPU never writes there, so they would never be refreshed (Z-A,
// entering Lumiose from the station: vertices drawn from a stale copy of compute-written data, exploding). The draws
// read such ranges in place from then on (exec.c gpuw_overlaps).
void vcache_invalidate(uint64_t addr, uint64_t len) {
    int n = 0;
    for (int b = 0; b < NBUCKET; b++)
        for (VEnt *e = g_tab[b]; e; e = e->next)
            if (e->buf && e->addr < addr + len && addr < e->addr + e->len) drop_buf(e), n++;
    static int nlog;
    if (n && nlog++ < 8) LOG("[vk] %d VRAM vertex / index copies dropped: the GPU writes %llx-%llx\n", n, (unsigned long long)addr,
                            (unsigned long long)(addr + len));
}

// In-place reboot (device.c backend_reset; GPU idle): no copies, no read-only pages
void vcache_reset(void) {
    for (int b = 0; b < NBUCKET; b++) {
        for (VEnt *e = g_tab[b], *n; e; e = n) {
            n = e->next;
            drop_buf(e);
            free(e->shadow);
            free(e);
        }
        g_tab[b] = NULL;
    }
    reap(true);
    pagewatch_reset();
}
