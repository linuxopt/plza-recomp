// 60 fps (experimental): the world and the menus at 60, battles and captures as the game made them (30).
//
// The game keeps a table of frame durations G (main+0xf7eeb8: 16.67 ms, 33.33 ms, 50 ms, ...; read through the pointer
// at main+0x15fb438). The world's update step comes from G[1] through the accessor f_7000039600; menus, battles and
// captures count their own frames, so at 60 fps they run twice as fast (letsgo-recomp's docs/PESQUISA-60FPS.md). A community "60 FPS"
// patch changes G[1] for every reader; here:
//  - the animation systems take "elapsed time" as a fixed 33.33 ms per update and divide it by G[1] to get frames: the
//    time the accessor hands out is that elapsed time, so with 16.67 ms there they advance half a frame per update at
//    60 fps - right, by time (menus, the world's animations);
//  - battles and captures also count integer frames of their own: they keep the game's 33.33 ms and 30 fps (the entry
//    points of both scenes, mapped in letsgo-recomp's docs/MODO-BATALHA.md, switch to it until the world's update runs again).
// Scenes move from the 30 fps side to the 60 fps side one at a time, once their frame counters are fixed.
//
// LARECOMP_FPS60=1 turns it on (settings: fps = 60). LARECOMP_G1_READERS="<addr>[,<addr>...]" (research): G[1] is
// 16.67 ms only while those functions run, to find which readers a speed comes from.
#include <stdatomic.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"

void rt_register_fnhook(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));  // runtime/fnhook.c
void rt_call_original(Cpu *c, uint64_t addr);
uint64_t renderer_game_frame(void);

#define G1_ADDR 0x7000f7eec0ull  // G[1]: 33333334 ns
#define G1_ORIGINAL 33333334u
#define G1_60HZ 16666667u
#define FIELD_UPDATE 0x70002f8bb0ull  // the world's update, every frame while it runs
#define G_ACCESSOR 0x7000039600ull    // G[index]: the world's step comes through here

static bool g_on;
static bool g_world = true;             // 60 fps now (not inside a battle or a capture)
static bool g_all60;                    // LARECOMP_FPS60=2
static bool g_special;                  // a battle or a capture started and the world hasn't updated since
#define ENCOUNTER_SCENE 0x700028ef70ull  // creates the capture scene (x0 = the encounter object)
#define BATTLE_START 0x700028bb60ull     // copies the battle's configuration

static long g_scene_start = -1;  // research: the present a capture began in (LARECOMP_SHOT_REL)
long fps60_scene_start(void) { return g_scene_start; }
static int scene_start_hook(Cpu *c, uint64_t addr) {
    if (g_scene_start < 0) {
        g_scene_start = (long)renderer_game_frame();
        LOG("[fps60] scene start %ld (f_%llx)\n", g_scene_start, (unsigned long long)addr);
    }
    (void)c, (void)addr;
    return 0;
}

static bool g_scene;  // inside a battle or a capture (until the world updates again)

// The frame counters a scene's timeline runs on (tools/patches.py) step on every other update at 60 fps: 1 = step now.
// Research (LARECOMP_FPS60_STATS=1): once a second, what the game did: presents, world updates (FIELD_UPDATE), reads of G through
// the accessor, and how many times the scene counters (tools/patches.py) were offered a step / allowed to take it.
static bool g_stats;
static unsigned long g_n_world, g_n_acc, g_n_tick_ask, g_n_tick_do;
int rt_scene_tick(void) {
    int r = !(g_all60 && g_scene && (renderer_game_frame() & 1));
    g_n_tick_ask++, g_n_tick_do += r;
    return r;
}

// Every battle / capture and the world's return after it, in the log ("[scene]"; letsgo-recomp's tools/capture_loop.py reads them) and for
// the scripted input (LARECOMP_INPUT_LOOP holds still for a while after the world comes back: input.c)
static bool g_mark_scene;
static long g_world_back = -1;
long fps60_world_back(void) { return g_world_back; } bool fps60_in_scene(void) { return g_mark_scene; }
static int scene_hook(Cpu *c, uint64_t addr) {
    scene_start_hook(c, addr);  // (one hook per address: this one also marks the start for LARECOMP_SHOT_REL)
    if (!g_mark_scene) { g_mark_scene = true; LOG("[scene] start %llu (f_%llx)\n", (unsigned long long)renderer_game_frame(), (unsigned long long)addr); }
    g_scene = true;
    if (g_all60) return 0;  // LARECOMP_FPS60=2: battles and captures at 60 too (their counters halved)
    g_special = true;
    (void)c, (void)addr;
    return 0;
}

// A battle the game starts without BATTLE_START: the wild battles mode builds it through the static battles' process
// (runtime/hle/wildbattle.c), which never calls it. Without this it counted as a menu and got in-between images.
void fps60_scene_begin(void) {
    g_scene = true;
    if (!g_all60) g_special = true;
}

static bool g_field_ran;  // the world's update ran since the last present
static bool g_played;     // the world has updated at least once (before: the controller screen, the title, loading)
static int field_hook(Cpu *c, uint64_t addr) {
    g_n_world++;
    if (g_mark_scene) { g_mark_scene = false; g_world_back = (long)renderer_game_frame(); LOG("[scene] world again %ld\n", g_world_back); }
    g_field_ran = true;
    g_played = true;
    g_special = false;
    g_scene = false;
    (void)c, (void)addr;
    return 0;  // (the game's function runs as usual)
}

// G[1] at 60 Hz for the duration of f: the world's step; any other scene keeps the game's value.
static int accessor_hook(Cpu *c, uint64_t addr) {
    volatile uint32_t *g1 = (volatile uint32_t *)(uintptr_t)G1_ADDR;
    g_n_acc++;
    if (!g_on || !g_world) return 0;
    *g1 = G1_60HZ;
    rt_call_original(c, addr);
    *g1 = G1_ORIGINAL;
    return 1;
}

static int g1_reader_hook(Cpu *c, uint64_t addr) {
    volatile uint32_t *g1 = (volatile uint32_t *)(uintptr_t)G1_ADDR;
    *g1 = G1_60HZ;
    rt_call_original(c, addr);
    *g1 = G1_ORIGINAL;
    return 1;
}

// Research (LARECOMP_FPS60_LOG=1): the frame-rate setting object the animation systems index G with (main+0x15fab08
// holds a pointer to a pointer to it; +0x88 is the index), once a second.
static void log_index(void) {
    static uint64_t last;
    uint64_t f = renderer_game_frame();
    if (f - last < 60) return;
    last = f;
    const uint64_t *slot = (const uint64_t *)(uintptr_t)(0x7000000000ull + 0x15fab08);
    const uint64_t *obj = (const uint64_t *)(uintptr_t)*slot;
    if (!obj || (uint64_t)(uintptr_t)obj < 0x7000000000ull) return;
    const uint64_t *real = (const uint64_t *)(uintptr_t)*obj;
    if (!real || (uint64_t)(uintptr_t)real < 0x7000000000ull) return;
    LOG("[fps60] present %llu: frame-rate index %d (G[idx] = %u)\n", (unsigned long long)f, *(const int32_t *)((const uint8_t *)real + 0x88),
        *(const uint32_t *)(uintptr_t)(0x7000f7eeb8ull + 8ull * (uint32_t)*(const int32_t *)((const uint8_t *)real + 0x88)));
}

// Research (LARECOMP_ELAPSED_LOG=1): the elapsed time the animation functions f_7000663660 / f_7000664cd0 divide by
// G[index]: the object their parameter holds, asked as they do (its first virtual method, argument 1), field +8.
// LARECOMP_ELAPSED_LOG="<addr>:<which>[,...]" names other readers and the argument they pass (f_7000029d30, the frame
// loop: 0; f_700067be50: 1); LARECOMP_TIME_PROBE_FRAMES limits it to some presents.
static uint64_t g_el_fn[16];
static int g_el_arg[16], g_nel;
static long g_tp_from, g_tp_to = 0x7fffffffL;
static int elapsed_hook(Cpu *c, uint64_t addr) {
    static int n;
    long f = (long)renderer_game_frame();
    if (f < g_tp_from || f > g_tp_to) return 0;
    int which = 1;
    for (int i = 0; i < g_nel; i++) if (g_el_fn[i] == addr) which = g_el_arg[i];
    uint64_t saved[32], pc_arg = c->x[1];
    memcpy(saved, c->x, sizeof saved);
    uint64_t obj = pc_arg ? *(uint64_t *)(uintptr_t)pc_arg : 0;
    if (obj >= 0x7000000000ull && n < 400) {
        uint64_t vt = *(uint64_t *)(uintptr_t)obj, fn = vt >= 0x7000000000ull ? *(uint64_t *)(uintptr_t)vt : 0;
        if (fn >= 0x7000000000ull) {
            c->x[0] = obj;
            c->x[1] = (uint64_t)which;
            guest_call(c, fn);
            uint64_t r = c->x[0];
            if (r >= 0x7000000000ull)
                LOG("[elapsed] f_%llx present %llu: delta %lld ns (timer %d: object %llx, getter f_%llx, step %llx)\n", (unsigned long long)addr,
                    (unsigned long long)renderer_game_frame(), (long long)*(const int64_t *)(uintptr_t)(r + 8), which,
                    (unsigned long long)obj, (unsigned long long)fn, (unsigned long long)r);
            n++;
        }
    }
    memcpy(c->x, saved, sizeof saved);
    return 0;
}

bool fps60_enabled(void) { return g_on; }
bool fps60_world(void) { return g_world; }
// A menu (the Pokédex, the bag...): the world stopped and no battle or capture runs. Logic at 30, shown at 60 with the
// in-between image (runtime/nvn/vulkan/exec.c); battles and captures get none (tried, left out: letsgo-recomp's docs/HISTORICO.md 46).
bool fps60_menu(void) { return g_on && g_played && !g_world && !g_special; }

// At each present (before its pacing): was this a world frame?
void fps60_probe_present(void);
int rt_present_interval(void);
void fps60_present(void) {
    fps60_probe_present();
    if (g_stats) {
        static uint64_t t0, p0;
        extern uint64_t plat_now_ns(void);
        uint64_t now = plat_now_ns(), p = renderer_game_frame();
        if (!t0) t0 = now, p0 = p;
        if (now - t0 >= 1000000000ull) {
            LOG("[stats] t+%.2fs presents %llu world %lu acc %lu tick_ask %lu tick_do %lu scene %d interval %d G1 %u\n", (now - t0) / 1e9,
                (unsigned long long)(p - p0), g_n_world, g_n_acc, g_n_tick_ask, g_n_tick_do, g_scene, rt_present_interval(), *(volatile uint32_t *)(uintptr_t)G1_ADDR);
            t0 = now, p0 = p, g_n_world = g_n_acc = g_n_tick_ask = g_n_tick_do = 0;
        }
    }
    if (getenv("LARECOMP_FPS60_LOG")) log_index();
    if (!g_on) return;
    // 60 only while the world itself updates: menus, the Pokédex and other screens that stop it count their own frames
    // and ran twice as fast (measured: the menu opened in 4 game frames instead of 8)
    static int menu_native = -1;  // (research, LARECOMP_FPS60_MENU_NATIVE=1: menus at 60 natively, as before HISTORICO 47)
    if (menu_native < 0) menu_native = getenv("LARECOMP_FPS60_MENU_NATIVE") != NULL;
    bool w = !g_special && (g_field_ran || menu_native);
    g_field_ran = false;
    g_world = w;
    static int last = -1;  // 0 world, 1 menu (in-between images), 2 battle / capture
    int state = w ? 0 : fps60_menu() ? 1 : 2;
    static const char *const name[] = {"world: 60 fps", "menu: 30 fps with in-between images", "battle / capture: 30 fps"};
    if (state != last) LOG("[fps60] %s from present %llu\n", name[state], (unsigned long long)renderer_game_frame());
    last = state;
}

// Before each boot (runtime/main.c): the launcher's "60 fps (experimental)" (larecomp.ini fps = 60) turns the mode on
// when LARECOMP_FPS60 doesn't say otherwise.
void fps60_configure(void) {
    static bool hooked;
    extern void settings_load(void);
    settings_load();  // (read once; without the launcher it would come later, in boot)
    if (!getenv("LARECOMP_FPS60")) g_on = g_settings.fps == 60, g_all60 = false;
    extern uint64_t rt_addr(uint64_t eevee);  // (runtime/fnhook.c: 0 in a build it doesn't know)
    if (!rt_addr(FIELD_UPDATE)) g_on = g_all60 = false;
    if ((g_on || g_stats || getenv("LARECOMP_INPUT_LOOP")) && !hooked) {
        hooked = true;
        rt_register_fnhook(FIELD_UPDATE, field_hook);
        rt_register_fnhook(ENCOUNTER_SCENE, scene_hook);
        rt_register_fnhook(BATTLE_START, scene_hook);
        rt_register_fnhook(G_ACCESSOR, accessor_hook);
    }
    g_world = true, g_special = g_scene = g_played = false;
    if (g_on) LOG("[fps60] on: the world at 60, menus at 30 with in-between images, battles and captures at 30\n");
}

__attribute__((constructor)) static void fps60_init(void) {
    const char *e = getenv("LARECOMP_FPS60");
    g_on = e && atoi(e);
    g_all60 = e && atoi(e) == 2;
    g_stats = getenv("LARECOMP_FPS60_STATS") != NULL;  // (the hooks: fps60_configure)
    if (getenv("LARECOMP_ELAPSED_LOG")) {
        const char *fr = getenv("LARECOMP_TIME_PROBE_FRAMES");
        if (fr) { g_tp_from = atol(fr); g_tp_to = strchr(fr, '-') ? atol(strchr(fr, '-') + 1) : g_tp_from; }
        char lb[512];
        snprintf(lb, sizeof lb, "%s", getenv("LARECOMP_ELAPSED_LOG"));
        if (strchr(lb, ':'))
            for (char *t = strtok(lb, ","); t && g_nel < 16; t = strtok(NULL, ",")) {
                g_el_fn[g_nel] = strtoull(t, NULL, 16);
                g_el_arg[g_nel] = strchr(t, ':') ? atoi(strchr(t, ':') + 1) : 1;
                rt_register_fnhook(g_el_fn[g_nel++], elapsed_hook);
            }
        else {
            rt_register_fnhook(0x7000663660ull, elapsed_hook);
            rt_register_fnhook(0x7000664cd0ull, elapsed_hook);
        }
    }
    if (!g_on && (getenv("LARECOMP_SHOT_REL") || getenv("LARECOMP_WINDOW_SHOTS_REL"))) { rt_register_fnhook(ENCOUNTER_SCENE, scene_start_hook); rt_register_fnhook(BATTLE_START, scene_start_hook); }
    e = getenv("LARECOMP_G1_READERS");
    char buf[512];
    if (!e) return;
    snprintf(buf, sizeof buf, "%s", e);
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) rt_register_fnhook(strtoull(t, NULL, 16), g1_reader_hook);
}

// ---- research: LARECOMP_TIME_PROBE="<fn addr>[,...]" (+ LARECOMP_TIME_PROBE_FRAMES=<first>-<last>) ------------------
// The readers of the frame-duration table G take an elapsed-time object in x1 (its +8: nanoseconds): log, per call, the
// present, x1's elapsed time and what the function returned (x0, and d0 as a double), up to 600 lines.
static int time_probe_hook(Cpu *c, uint64_t addr) {
    static int lines;
    uint64_t x1 = c->x[1];
    long f = (long)renderer_game_frame();
    int64_t el = x1 >= 0x7000000000ull && x1 < 0x7800000000ull ? *(const int64_t *)(uintptr_t)(x1 + 8) : -1;
    rt_call_original(c, addr);
    if (f >= g_tp_from && f <= g_tp_to && lines < 600) {
        double d0;
        memcpy(&d0, &c->v[0], 8);
        LOG("[time] present %ld f_%llx elapsed %lld ns -> x0 %llx d0 %g\n", f, (unsigned long long)addr, (long long)el,
            (unsigned long long)c->x[0], d0);
        lines++;
    }
    return 1;
}
__attribute__((constructor)) static void time_probe_init(void) {
    const char *e = getenv("LARECOMP_TIME_PROBE");
    if (!e) return;
    const char *fr = getenv("LARECOMP_TIME_PROBE_FRAMES");
    if (fr) { g_tp_from = atol(fr); g_tp_to = strchr(fr, '-') ? atol(strchr(fr, '-') + 1) : g_tp_from; }
    char buf[512];
    snprintf(buf, sizeof buf, "%s", e);
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) rt_register_fnhook(strtoull(t, NULL, 16), time_probe_hook);
}

// ---- research: LARECOMP_TIMER_USERS=<first>-<last> -------------------------------------------------------------
// Who reads the frame-step timers: calls to their getter f_70000395f0 (x1 = the timer, the caller's lr), counted over
// those presents and listed at the last one. Timer 1 is the step the accessor f_7000039600 rewrites every frame with
// G[1]; timer 0 keeps 33.3 ms.
#define TIMER_GETTER 0x70000395f0ull
static struct { uint64_t lr; int which; uint64_t n; } g_tu[256];
static int g_ntu;
static long g_tu_from, g_tu_to;
static int timer_users_hook(Cpu *c, uint64_t addr) {
    long f = (long)renderer_game_frame();
    if (f >= g_tu_from && f <= g_tu_to) {
        uint64_t lr = c->x[30];
        int which = (int)c->x[1], i;
        for (i = 0; i < g_ntu && !(g_tu[i].lr == lr && g_tu[i].which == which); i++) {}
        if (i == g_ntu && g_ntu < 256) g_tu[g_ntu++] = (__typeof__(g_tu[0])){lr, which, 0};
        if (i < 256) g_tu[i].n++;
    } else if (f > g_tu_to && g_ntu) {
        for (int i = 0; i < g_ntu; i++)
            LOG("[timers] timer %d read from lr %llx: %llu times in %ld presents\n", g_tu[i].which, (unsigned long long)g_tu[i].lr,
                (unsigned long long)g_tu[i].n, g_tu_to - g_tu_from + 1);
        g_ntu = 0;
        g_tu_to = -1;
    }
    (void)addr;
    return 0;
}
__attribute__((constructor)) static void timer_users_init(void) {
    const char *e = getenv("LARECOMP_TIMER_USERS");
    if (!e) return;
    g_tu_from = atol(e);
    g_tu_to = strchr(e, '-') ? atol(strchr(e, '-') + 1) : g_tu_from;
    rt_register_fnhook(TIMER_GETTER, timer_users_hook);
}

// ---- research: LARECOMP_CALLCOUNT="<fn>[,...]" (+ LARECOMP_TIME_PROBE_FRAMES): calls per present of each function
#define CC_MAX 8
static uint64_t g_cc_fn[CC_MAX];
static unsigned g_cc_n[CC_MAX];
static int g_ncc;
static long g_cc_frame = -1;
static int callcount_hook(Cpu *c, uint64_t addr) {
    long f = (long)renderer_game_frame();
    if (f != g_cc_frame) {
        if (g_cc_frame >= g_tp_from && g_cc_frame <= g_tp_to) {
            char line[256];
            int o = snprintf(line, sizeof line, "[calls] present %ld:", g_cc_frame);
            for (int i = 0; i < g_ncc; i++) o += snprintf(line + o, sizeof line - o, " f_%llx %u", (unsigned long long)g_cc_fn[i], g_cc_n[i]);
            LOG("%s\n", line);
        }
        memset(g_cc_n, 0, sizeof g_cc_n);
        g_cc_frame = f;
    }
    for (int i = 0; i < g_ncc; i++) if (g_cc_fn[i] == addr) g_cc_n[i]++;
    (void)c;
    return 0;
}
__attribute__((constructor)) static void callcount_init(void) {
    const char *e = getenv("LARECOMP_CALLCOUNT");
    if (!e) return;
    const char *fr = getenv("LARECOMP_TIME_PROBE_FRAMES");
    if (fr) { g_tp_from = atol(fr); g_tp_to = strchr(fr, '-') ? atol(strchr(fr, '-') + 1) : g_tp_from; }
    char buf[512];
    snprintf(buf, sizeof buf, "%s", e);
    for (char *t = strtok(buf, ","); t && g_ncc < CC_MAX; t = strtok(NULL, ",")) {
        g_cc_fn[g_ncc++] = strtoull(t, NULL, 16);
        rt_register_fnhook(g_cc_fn[g_ncc - 1], callcount_hook);
    }
}

// ---- research: LARECOMP_PROBE="<fn addr>[:<bytes>]" ----------------------------------------------------------------
// At each entry of the function, remember its x0 (object) and, after the original returns, x0 (result): for the next
// LARECOMP_PROBE_FRAMES (120) presents, compare `bytes` (0x400) of each of them with the previous present and log, at
// the end, every 32-bit word that changed: how many presents, and its usual step as an integer and as a float. A
// frame counter shows as an integer stepping by 1 (or a float by a constant) every present of the scene.
#define PROBE_MAX 4
static struct { uint64_t base; const char *what; uint32_t prev[0x400 / 4]; uint32_t changed[0x400 / 4]; int32_t step[0x400 / 4]; uint32_t step_n[0x400 / 4]; bool have; } g_probe[PROBE_MAX];
static int g_nprobe;
static uint32_t g_probe_bytes = 0x400;
static uint64_t g_probe_from, g_probe_to;
static int probe_hook(Cpu *c, uint64_t addr) {
    uint64_t obj = c->x[0];
    rt_call_original(c, addr);
    uint64_t ret = c->x[0];
    if (!g_probe_from) g_probe_from = renderer_game_frame() + 1, g_probe_to = g_probe_from + (getenv("LARECOMP_PROBE_FRAMES") ? atoi(getenv("LARECOMP_PROBE_FRAMES")) : 120);
    for (int pass = 0; pass < 2 && g_nprobe < PROBE_MAX; pass++) {
        uint64_t p = pass ? ret : obj;
        if (p < 0x7000000000ull || p > 0x7800000000ull) continue;
        g_probe[g_nprobe].base = p, g_probe[g_nprobe].what = pass ? "returned object" : "x0 object";
        LOG("[probe] f_%llx %s %llx\n", (unsigned long long)addr, g_probe[g_nprobe].what, (unsigned long long)p);
        g_nprobe++;
    }
    return 1;
}
void fps60_probe_present(void) {
    uint64_t f = renderer_game_frame();
    if (!g_probe_from || f < g_probe_from || f > g_probe_to) return;
    for (int i = 0; i < g_nprobe; i++) {
        const uint32_t *now = (const uint32_t *)(uintptr_t)g_probe[i].base;
        for (uint32_t w = 0; w < g_probe_bytes / 4; w++) {
            if (g_probe[i].have && now[w] != g_probe[i].prev[w]) {
                g_probe[i].changed[w]++;
                int32_t d = (int32_t)(now[w] - g_probe[i].prev[w]);
                if (d == g_probe[i].step[w]) g_probe[i].step_n[w]++;
                else if (g_probe[i].step_n[w] == 0) g_probe[i].step[w] = d;
            }
            g_probe[i].prev[w] = now[w];
        }
        g_probe[i].have = true;
    }
    if (f == g_probe_to) {
        for (int i = 0; i < g_nprobe; i++)
            for (uint32_t w = 0; w < g_probe_bytes / 4; w++) {
                if (g_probe[i].changed[w] < 5) continue;
                float fv;
                memcpy(&fv, &g_probe[i].prev[w], 4);
                LOG("[probe] %s +0x%03x: changed in %u of %llu presents; usual step %d (%u times) value %u / %g\n", g_probe[i].what, w * 4,
                    g_probe[i].changed[w], (unsigned long long)(g_probe_to - g_probe_from), g_probe[i].step[w], g_probe[i].step_n[w],
                    g_probe[i].prev[w], fv);
            }
    }
}
__attribute__((constructor)) static void probe_init(void) {
    const char *e = getenv("LARECOMP_PROBE");
    if (!e) return;
    char buf[128];
    snprintf(buf, sizeof buf, "%s", e);
    char *colon = strchr(buf, ':');
    if (colon) *colon = 0, g_probe_bytes = (uint32_t)strtoul(colon + 1, NULL, 16);
    if (g_probe_bytes > 0x400) g_probe_bytes = 0x400;
    rt_register_fnhook(strtoull(buf, NULL, 16), probe_hook);
}

// ---- The Legends games at 60 fps (experimental), the way the community "60 FPS" cheat does it for Legends: Arceus:
// both step their world by a frame time looked up by present interval in a table of u64 nanoseconds (16666667,
// 33333334, 50000000... for intervals 1, 2, 3...) and present every other vsync. With the interval-2 entry at 16666667
// and presents paced at 60 (nvn/objects.c present_pace) the world runs at its own speed - a 10 s walk from the save
// ends at the same spot at 30 and at 60 fps. The launcher's 60 fps option, or LARECOMP_PLA_60FPS=1 (0: off).
#include "../platform.h"
#include <pthread.h>
// The menus, text boxes and loading screens are nn::ui2d layouts, whose animations advance a fixed step per update
// (Animator::UpdateFrame(progress): frame += speed * progress) - 4.8 times too fast at 144 fps. Both UpdateFrame
// implementations (Animator, shared by GroupAnimator / GroupArrayAnimator, and AnimatorEx: their vtable slot 3) get
// progress scaled by frame time / 33.33 ms. LARECOMP_UI_SCALE=0: off.
static const struct { uint8_t id[8]; uint64_t entry; const char *name; uint64_t ui2d[2]; } k_legends60[] = {
    {{0xae, 0xe8, 0xf1, 0x50, 0xdd, 0xa1, 0xb5, 0xa8}, 0x7000000000ull + 0x397E8D8, "Legends: Arceus 1.1.1",
     {0x70001395a4ull, 0x700013a7d4ull}},
    {{0xb1, 0xf1, 0x2f, 0xd9, 0x19, 0xea, 0xe8, 0x6a}, 0x7000000000ull + 0x3393E28, "Legends Z-A 2.0.2",
     {0x7002638d2cull, 0x700263a0fcull}},
};
static uint64_t g_pla60_entry;  // the interval-2 frame time of the running build (0: off)
static uint64_t g_pla60_build;  // (that entry of the running build, also at 30)
static bool g_pla60_patched;
static uint64_t g_pla60_ns = 16666667;  // the frame time: 60 fps, 120, 144, FPS_UNLIMITED's, or LARECOMP_LEGENDS_FPS=<n>
bool pla60_enabled(void) { return g_pla60_entry != 0; }
uint64_t pla60_period_ns(void) { return g_pla60_ns; }

void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));
static float g_ui_scale = 1.0f;
static int ui2d_update_frame(Cpu *c, uint64_t addr) {  // (s0 = progress; the original runs with it scaled)
    (void)addr;
    if (!g_pla60_entry) return 0;
    float v;
    uint32_t w = (uint32_t)c->v[0];
    memcpy(&v, &w, 4);
    static float seen[4];
    static int nseen;
    if (nseen < 4 && v != seen[0] && v != seen[1] && v != seen[2]) seen[nseen++] = v, LOG("[fps60] ui2d progress %g -> %g\n", v, v * g_ui_scale);
    v *= g_ui_scale;
    memcpy(&w, &v, 4);
    c->v[0] = w;
    return 0;
}

// (research) LARECOMP_DT_READERS="<fn>[,<fn>...]": the frame time entry reads 1/60 s only while those functions run
static int dt_reader(Cpu *c, uint64_t addr) {
    volatile uint64_t *ft = (volatile uint64_t *)(uintptr_t)g_pla60_entry;
    if (!g_pla60_patched) return 0;
    {   // (LARECOMP_DT_CHANNEL=<n>: only that w1; logged: the channels and their objects, once)
        static int ch = -2;
        if (ch == -2) ch = getenv("LARECOMP_DT_CHANNEL") ? (int)strtol(getenv("LARECOMP_DT_CHANNEL"), NULL, 0) : -1;  // (a mask of channels)
        static int seen[64];
        int w1 = (int)c->x[1];
        if (w1 >= 0 && w1 < 64 && !seen[w1]) seen[w1] = 1, LOG("[fps60] dt channel %d: obj %llx vtable %llx fn %llx\n", w1,
            (unsigned long long)c->x[0], (unsigned long long)*(uint64_t *)(uintptr_t)c->x[0],
            (unsigned long long)((uint64_t *)(uintptr_t)*(uint64_t *)(uintptr_t)c->x[0])[2]);
        if (ch >= 0 && !(w1 >= 0 && w1 < 31 && (ch >> w1 & 1))) return 0;
    }
    *ft = 16666667;
    rt_call_original(c, addr);
    *ft = g_pla60_ns;
    return 1;
}
void rt_call_original(Cpu *c, uint64_t addr);
// (research) LARECOMP_CH1_READERS="<fn>,...": channel 1's frame time (the timers object 0x70029f4214 fills) reads
// 1/60 s while those functions run
static volatile uint64_t g_timers;
static int timers_seen(Cpu *c, uint64_t addr) { g_timers = c->x[0]; return 0; }
// (research) LARECOMP_ZA_CHLOG=1: each distinct (caller, channel, table index) that fills a time channel, logged once
static int timers_log(Cpu *c, uint64_t addr) {
    (void)addr;
    static struct { uint64_t lr; uint32_t ch, idx; } seen[64];
    static int n;
    uint64_t lr = c->x[30];
    uint32_t ch = (uint32_t)c->x[1], idx = (uint32_t)c->x[2];
    for (int i = 0; i < n; i++)
        if (seen[i].lr == lr && seen[i].ch == ch && seen[i].idx == idx) return 0;
    if (n < 64) {
        seen[n].lr = lr, seen[n].ch = ch, seen[n].idx = idx, n++;
        uint64_t v = *(const uint64_t *)(uintptr_t)(0x7003393e20ull + 8ull * (int32_t)idx);
        LOG("[fps60] time channel %u <- table[%d] = %llu ns (caller %llx, timers %llx)\n", ch, (int)(int32_t)idx,
            (unsigned long long)v, (unsigned long long)lr, (unsigned long long)c->x[0]);
    }
    return 0;
}
static int ch1_reader(Cpu *c, uint64_t addr) {
    uint64_t t = g_timers;
    if (!t || !g_pla60_patched) return 0;
    volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)(t + 0x10 + 16);
    uint64_t v = *p;
    *p = 16666667;
    rt_call_original(c, addr);
    *p = v;
    return 1;
}

// (research) LARECOMP_ZA_BTLOG=1: the per-object updaters (behaviour trees: 0x7001e682b8 reads a rolling time window,
// index [ctx+0x10], of the time channel and calls 0x7001e66080(obj, seconds)): per window index, once a real second,
// the calls, distinct objects and the seconds handed over per object - vs the real time that passed
static __thread int g_bt_win = -1;
static struct { uint64_t obj; uint64_t calls; double secs; int win; } g_bto[2048];
static int g_nbto;
static int bt_window(Cpu *c, uint64_t addr) {
    (void)addr;
    g_bt_win = (int)*(const uint32_t *)(uintptr_t)(c->x[1] + 0x10);
    return 0;
}
static int bt_cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static int bt_update(Cpu *c, uint64_t addr) {
    (void)addr;
    int w = g_bt_win;
    g_bt_win = -1;
    if (w != 1) return 0;  // (window 1: the AI's per-update time; the others hand over ~0)
    float sec;
    uint32_t b = (uint32_t)c->v[0];
    memcpy(&sec, &b, 4);
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    int i = 0;
    while (i < g_nbto && g_bto[i].obj != c->x[0]) i++;
    if (i == g_nbto && i < 2048) g_bto[g_nbto].obj = c->x[0], g_bto[g_nbto].calls = 0, g_bto[g_nbto].secs = 0, g_nbto++;
    if (i < 2048) g_bto[i].calls++, g_bto[i].secs += sec;
    static uint64_t since, frames0;
    extern uint64_t renderer_present_count(void);
    uint64_t now = plat_now_ns();
    if (!since) since = now, frames0 = renderer_present_count();
    if (now - since >= 2000000000ull) {
        double real = (double)(now - since) / 1e9, r[2048], cpf[2048];
        uint64_t fr = renderer_present_count() - frames0;
        int n = 0;
        for (int k = 0; k < g_nbto; k++)
            if (g_bto[k].calls > 2) r[n] = g_bto[k].secs / real, cpf[n] = (double)g_bto[k].calls / (double)(fr ? fr : 1), n++;
        qsort(r, n, sizeof *r, bt_cmp), qsort(cpf, n, sizeof *cpf, bt_cmp);
        if (n) LOG("[bt] %d objects over %.2f s (%llu frames): game time per real time min %.2f p10 %.2f median %.2f max %.2f; "
                   "updates per frame p10 %.2f median %.2f max %.2f\n", n, real, (unsigned long long)fr, r[0], r[n / 10], r[n / 2],
                   r[n - 1], cpf[n / 10], cpf[n / 2], cpf[n - 1]);
        g_nbto = 0, since = now, frames0 = renderer_present_count();
    }
    pthread_mutex_unlock(&mu);
    return 0;
}

void pla60_configure(const char *exefs) {
    char p[1100];
    snprintf(p, sizeof p, "%s/main", exefs);
    uint8_t id[8] = {0};
    FILE *f = fopen(p, "rb");
    if (f) {
        fseek(f, 0x40, SEEK_SET);
        if (fread(id, 1, 8, f) != 8) memset(id, 0, 8);
        fclose(f);
    }
    const char *e = getenv("LARECOMP_PLA_60FPS"), *n = getenv("LARECOMP_LEGENDS_FPS");
    bool on = e ? atoi(e) != 0 : g_settings.fps >= 60;
    g_pla60_ns = !e && g_settings.fps > 60 ? 1000000000ull / (uint64_t)g_settings.fps : 16666667;
    if (n && atoi(n) > 30) on = true, g_pla60_ns = 1000000000ull / (uint64_t)atoi(n);
    g_pla60_entry = g_pla60_build = 0;
    g_pla60_patched = false;
    for (size_t i = 0; i < sizeof k_legends60 / sizeof *k_legends60; i++)
        if (!memcmp(id, k_legends60[i].id, 8)) {
            g_pla60_build = k_legends60[i].entry;
            if (on) g_pla60_entry = g_pla60_build, LOG("[fps60] %s at %.0f fps\n", k_legends60[i].name, 1e9 / (double)g_pla60_ns);
            g_ui_scale = on ? (float)((double)g_pla60_ns / 33333334.0) : 1.0f;  // (the hooks also at 30: the rate may change)
            {
                static bool dth;
                const char *r = getenv("LARECOMP_DT_READERS");
                for (const char *q = r; !dth && q && *q;) {
                    uint64_t fa = strtoull(q, (char **)&q, 16);
                    if (fa) rt_register_fnhook_at(fa, dt_reader), LOG("[fps60] research: %llx sees 1/60 s\n", (unsigned long long)fa);
                    if (*q == ',') q++; else break;
                }
                const char *r1 = getenv("LARECOMP_CH1_READERS");
                if (!dth && r1) rt_register_fnhook_at(0x70029f4214ull, timers_seen);
                else if (!dth && getenv("LARECOMP_ZA_CHLOG")) rt_register_fnhook_at(0x70029f4214ull, timers_log);
                if (!dth && getenv("LARECOMP_ZA_BTLOG"))
                    rt_register_fnhook_at(0x7001e682b8ull, bt_window), rt_register_fnhook_at(0x7001e66080ull, bt_update);
                for (const char *q = r1; !dth && q && *q;) {
                    uint64_t fa = strtoull(q, (char **)&q, 16);
                    if (fa) rt_register_fnhook_at(fa, ch1_reader);
                    if (*q == ',') q++; else break;
                }
                dth = true;
            }
            static bool hooked;
            const char *u = getenv("LARECOMP_UI_SCALE");
            if (!hooked && !(u && atoi(u) == 0)) {
                hooked = true;
                for (int j = 0; j < 2; j++) rt_register_fnhook_at(k_legends60[i].ui2d[j], ui2d_update_frame);
            }
        }
}


// The frame rate changed while playing (the game's Settings screen, hle/*_options.c): 30 the game's own; above it the
// table entry and the present pacing (and the rest that follows pla60_period_ns) at once. False: not a known build.
bool legends_set_fps(int fps) {
    if (!g_pla60_build) return false;
    uint64_t ns = fps == 60 ? 16666667 : fps > 30 ? 1000000000ull / (uint64_t)fps : 33333334;
    volatile uint64_t *ft = (volatile uint64_t *)(uintptr_t)g_pla60_build;
    bool loaded = g_pla60_patched || *ft == 33333334u;  // (the game's data there: else pla60_present patches it later)
    if (loaded) {
        plat_protect(g_pla60_build & ~(PLAT_PAGE - 1), PLAT_PAGE, PLAT_PROT_RW);
        *ft = ns;
    }
    g_pla60_ns = fps > 30 ? ns : 16666667;
    g_ui_scale = fps > 30 ? (float)((double)ns / 33333334.0) : 1.0f;
    g_pla60_entry = fps > 30 ? g_pla60_build : 0;
    g_pla60_patched = fps > 30 && loaded;
    LOG("[fps60] %d fps from now on\n", fps);
    return true;
}

// A real-time step (pla60_present) from the time since the last present: at least half the frame rate's period (two
// presents close together), the time a short step took for granted taken off the next ones - the world's time adds up
// to the real time exactly (clamped to the period, the paced presents' jitter ran the game a few % fast) - and at most
// 33.3 ms, the game's own step at 30 fps. A longer frame's rest is made up over the next frames, each still at most
// 33.3 ms: the sound runs on the audio device's clock, and a cutscene's picture stayed behind its sound by every hitch
// it had (a first play's shader stall, a stream encoding beside the game: seconds by the end of the intro). Up to 1 s
// is made up (LARECOMP_CATCHUP_MS; 0: none, as before) - a longer stall is a load, and its next scene isn't sped up.
static uint64_t real_step(uint64_t real) {
    static uint64_t ahead, behind;  // (the world's time ahead of / behind the real time)
    static int64_t max_behind = -1;
    if (max_behind < 0) max_behind = (getenv("LARECOMP_CATCHUP_MS") ? atoll(getenv("LARECOMP_CATCHUP_MS")) : 1000) * 1000000ll;
    uint64_t lo = g_pla60_ns / 2, hi = 33333334u, dt = real > ahead ? real - ahead : 0;
    ahead = dt < lo ? lo - dt : 0;
    if (dt < lo) dt = lo;
    dt += behind;
    behind = dt > hi ? dt - hi : 0;
    if (behind > (uint64_t)max_behind) {
        static int logged;
        if (logged++ < 20) LOG("[fps60] a %.0f ms stall: %.0f ms of it not made up (a load)\n", (double)real / 1e6, (double)(behind - (uint64_t)max_behind) / 1e6);
        behind = (uint64_t)max_behind;
    }
    return dt > hi ? hi : dt;
}

// each present: the table entry, once the game is loaded (read-only data: the page made writable first)
void pla60_present(void) {
    {   // (research: LARECOMP_LIVE=<dir> with <dir>/fps holding a number: that frame rate from now on, the file removed)
        static const char *live = (const char *)1;
        if (live == (const char *)1) live = getenv("LARECOMP_LIVE");
        static unsigned n;
        if (live && ++n % 30 == 0) {
            char p[1100];
            snprintf(p, sizeof p, "%s/fps", live);
            FILE *f = fopen(p, "r");
            int v = 0;
            if (f) { if (fscanf(f, "%d", &v) != 1) v = 0; fclose(f); remove(p); if (v >= 30) legends_set_fps(v); }
        }
    }
    { extern void fscan_present(uint64_t frame); extern uint64_t renderer_present_count(void); fscan_present(renderer_present_count()); }
    { extern void za_options_present(void); za_options_present(); }
    { extern void cheats_frame(void); cheats_frame(); }  // (runtime/cheats.c: the enabled cheats, ~12 times a second)
    { extern void menu_mouse_present(void); menu_mouse_present(); }
    if (g_pla60_entry && g_pla60_patched) {
        // Real-time steps (Settings Frame Timing, the default; Unlimited always): the world steps by the time that
        // really passed since the last present, not the frame rate's fixed period - a slow frame (a hitch, a dip below
        // the frame rate) no longer slows the game down (walking, NPCs, animations). real_step: never more than 33.3 ms,
        // the game's own step at 30 fps, the most it was made for. The menus' animations follow. LARECOMP_DYN_DT=0/1
        // overrides the setting.
        static int env = -2;
        if (env == -2) env = getenv("LARECOMP_DYN_DT") ? atoi(getenv("LARECOMP_DYN_DT")) != 0 : -1;
        bool dyn = env >= 0 ? env : !g_settings.fixed_steps || g_settings.fps >= FPS_UNLIMITED;
        static bool was_dyn;
        static uint64_t last;
        uint64_t now = plat_now_ns();
        volatile uint64_t *ft = (volatile uint64_t *)(uintptr_t)g_pla60_entry;
        if (dyn && last) {
            uint64_t dt = real_step(now - last);
            // Whole periods only, the rest carried (the world still keeps to the real time: a long frame's step is two
            // periods). Some of the game's timers count whole ticks and round up: a step a little over the period
            // (16.9 ms at 60) counted as two - the Z-A intro's cinematics ran ~1.8x fast (Prism Tower lit before the talk
            // ended; the train outran the field's tree streaming: bare fields out the window). LARECOMP_DT_QUANT=0: raw.
            static int quant = -1;
            if (quant < 0) quant = !(getenv("LARECOMP_DT_QUANT") && atoi(getenv("LARECOMP_DT_QUANT")) == 0);
            if (quant && g_pla60_ns) {
                static int64_t bal;
                bal += (int64_t)dt;
                int64_t k = (bal + (int64_t)g_pla60_ns / 2) / (int64_t)g_pla60_ns;
                if (k < 1) k = 1;
                if ((uint64_t)k * g_pla60_ns > 33333334u) k = 33333334u / g_pla60_ns ? 33333334u / g_pla60_ns : 1;
                dt = (uint64_t)k * g_pla60_ns;
                bal -= (int64_t)dt;
                if (bal < -(int64_t)g_pla60_ns) bal = -(int64_t)g_pla60_ns;
            }
            *ft = dt;
            g_ui_scale = (float)((double)dt / 33333334.0);
            static int dtlog = -1;  // (research: LARECOMP_DT_LOG=1 - each second: steps written, their sum against the real time)
            if (dtlog < 0) dtlog = getenv("LARECOMP_DT_LOG") && atoi(getenv("LARECOMP_DT_LOG")) > 0;
            if (dtlog) {
                static uint64_t t0, sum, n, mn = ~0ull, mx;
                extern uint64_t renderer_present_count(void);
                static uint64_t p0;
                if (!t0) t0 = now, p0 = renderer_present_count();
                sum += dt, n++, mn = dt < mn ? dt : mn, mx = dt > mx ? dt : mx;
                if (now - t0 >= 1000000000ull) {
                    LOG("[dt] %llu steps, %llu presents in %.3f s: world %.3f s (step %.1f..%.1f ms)\n", (unsigned long long)n,
                        (unsigned long long)(renderer_present_count() - p0), (double)(now - t0) / 1e9, (double)sum / 1e9, mn / 1e6, mx / 1e6);
                    t0 = now, p0 = renderer_present_count(), sum = n = mx = 0, mn = ~0ull;
                }
            }
        } else if (was_dyn) {  // (switched to Fixed in game: the period again)
            *ft = g_pla60_ns;
            g_ui_scale = (float)((double)g_pla60_ns / 33333334.0);
        }
        was_dyn = dyn;
        last = now;
        return;
    }
    if (!g_pla60_entry) return;
    volatile uint64_t *ft = (volatile uint64_t *)(uintptr_t)g_pla60_entry;
    if (*ft != 33333334u) return;
    plat_protect(g_pla60_entry & ~(PLAT_PAGE - 1), PLAT_PAGE, PLAT_PROT_RW);
    *ft = g_pla60_ns;
    if (getenv("LARECOMP_LEGENDS_DT_FPS")) *ft = 1000000000ull / (uint64_t)atoi(getenv("LARECOMP_LEGENDS_DT_FPS"));  // (research: the world's frame time apart from the pacing)
    g_pla60_patched = true;
}
