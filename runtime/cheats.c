// Cheats: Atmosphère's cheat VM (dmnt) on the game's memory (runtime/cheats.h).
//
// The opcodes are Atmosphère's (docs/features/cheats.md there): 0 store static, 1 memory conditional, 2 end / else,
// 3 loop, 4 load register, 5 load register from memory, 6 store static to register address, 7 legacy arithmetic,
// 8 keys held, 9 register arithmetic (with the float operations A-D newer cheats use), A store register, C0 register
// conditional, C1 / C2 save / restore registers, C3 static registers, C4 extended keypress; FF0 / FF1 / FFF (pause,
// resume, log) are read and skipped. Memory regions: 0 main (0x7000000000), 1 heap (HEAP_BASE), 2 alias / 3 ASLR
// (none here: 0). The VM runs every enabled cheat in file order, masters first, about 12 times a second (Atmosphère's
// rate; LARECOMP_CHEAT_HZ), registers cleared each run, static registers kept.
//
// Code cheats: a write into the main module's code (or the zero padding after it, where cheats put their code caves)
// lands in guest memory as on a Switch; the compiled game code reads the word at each site the cheat file names and
// runs the instruction it holds (tools/gen.py --cheat-sites). The words a cheat overwrote are remembered and put back
// when it is switched off, so the file's "Restore Code" entry isn't needed.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // (process_vm_readv)
#endif
#include "cheats.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/uio.h>
#include <unistd.h>
#endif

#include "gamelib.h"
#include "paths.h"
#include "platform.h"
#include "rt.h"

#define MAIN_BASE 0x7000000000ull
#ifdef _WIN32
#define DIRSEP "\\"
#else
#define DIRSEP "/"
#endif

typedef struct {
    char name[128], label[128];  // (label: as shown - "№" is "#", the launcher's fonts have no №)
    bool master, header, hidden, code, on;
    uint32_t *w;
    int n;
    char note[160];
} Cheat;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Cheat *g_c;
static int g_n;
static char g_bid[17], g_path[64];
static uint64_t g_text_end;  // main's text size (offset), the code region (plus the padding to its page end)

// ------------------------------------------------------------------ guest memory
static bool mem_read(uint64_t a, void *out, size_t n) {
    if (a < 0x1000) return false;
#ifdef _WIN32
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)a, out, n, &got) && got == n;
#else
    struct iovec l = {out, n}, r = {(void *)(uintptr_t)a, n};
    return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == (ssize_t)n;
#endif
}
static bool in_code(uint64_t a) { return a >= MAIN_BASE && a < MAIN_BASE + ((g_text_end + 0xFFF) & ~0xFFFull); }

// Originals of the code words cheats overwrote (address -> the game's word), to put back when switched off
typedef struct { uint64_t a; uint32_t orig; int owner; } Saved;
static Saved *g_saved;
static int g_nsaved, g_capsaved;
static int g_running_cheat = -1;  // (the cheat whose codes run now: owner of the words it writes)

static void code_writable(uint64_t a, size_t n) {
    static uint64_t done[64];
    static int ndone;
    for (uint64_t p = a & ~(uint64_t)(PLAT_PAGE - 1); p < a + n; p += PLAT_PAGE) {
        bool seen = false;
        for (int i = 0; i < ndone; i++) seen |= done[i] == p;
        if (seen) continue;
        plat_protect(p, PLAT_PAGE, PLAT_PROT_RW);
        if (ndone < 64) done[ndone++] = p;
    }
}

static bool mem_write(uint64_t a, const void *in, size_t n) {
    char probe[8];
    if (n > 8 || !mem_read(a, probe, n)) return false;  // (mapped: a bad pointer chain is skipped, not a crash)
    if (in_code(a)) {
        for (uint64_t w = a & ~3ull; w < a + n; w += 4) {  // (each word's original, once)
            bool have = false;
            for (int i = 0; i < g_nsaved && !have; i++) have = g_saved[i].a == w;
            if (have) continue;
            if (g_nsaved == g_capsaved) {
                g_capsaved = g_capsaved ? g_capsaved * 2 : 256;
                g_saved = realloc(g_saved, sizeof *g_saved * (size_t)g_capsaved);
            }
            uint32_t o = 0;
            mem_read(w, &o, 4);
            g_saved[g_nsaved++] = (Saved){w, o, g_running_cheat};
        }
        code_writable(a, n);
    }
    // (a plain store: the page watch of vertex data - runtime/pagewatch.c - sees it as the game's own write)
    switch (n) {
    case 1: *(volatile uint8_t *)(uintptr_t)a = *(const uint8_t *)in; break;
    case 2: { uint16_t v; memcpy(&v, in, 2); *(volatile uint16_t *)(uintptr_t)a = v; break; }
    case 4: { uint32_t v; memcpy(&v, in, 4); *(volatile uint32_t *)(uintptr_t)a = v; break; }
    default: { uint64_t v; memcpy(&v, in, 8); *(volatile uint64_t *)(uintptr_t)a = v; break; }
    }
    return true;
}

// The words cheat k overwrote, back to the game's (unless another cheat that is on wrote them as well)
static void restore_owned(int k) {
    for (int i = 0; i < g_nsaved; i++) {
        if (g_saved[i].owner != k) continue;
        code_writable(g_saved[i].a, 4);
        *(volatile uint32_t *)(uintptr_t)g_saved[i].a = g_saved[i].orig;
        g_saved[i] = g_saved[--g_nsaved];
        i--;
    }
}

// ------------------------------------------------------------------ the VM
typedef struct {
    uint64_t r[16], saved[16], stat[128];
    uint64_t keys, keys_before;
} Vm;
static Vm g_vm;

static uint64_t region(int m) { return m == 0 ? MAIN_BASE : m == 1 ? HEAP_BASE : 0; }
static int nib(uint32_t w, int i) { return (int)(w >> (28 - 4 * i)) & 0xF; }

// Words of the instruction at w[0] (for skipping a block not taken)
static int op_len(const uint32_t *w, int left) {
    uint32_t a = w[0];
    int op = nib(a, 0), t = nib(a, 1), n = 1;
    switch (op) {
    case 0x0: case 0x1: n = t == 8 ? 4 : 3; break;
    case 0x2: n = 1; break;
    case 0x3: n = nib(a, 1) == 0 ? 2 : 1; break;
    case 0x4: n = 3; break;
    case 0x5: n = 2; break;
    case 0x6: n = 3; break;
    case 0x7: n = 2; break;
    case 0x8: n = 1; break;
    case 0x9: n = nib(a, 5) == 1 ? (t == 8 ? 3 : 2) : 1; break;
    case 0xA: { int o = nib(a, 5); n = o == 2 || o == 4 || o == 5 ? 2 : 1; break; }
    case 0xC: {
        int sub = nib(a, 1);
        if (sub == 0) { int x = nib(a, 5); n = x == 0 || x == 2 ? 2 : x == 4 ? (nib(a, 2) == 8 ? 3 : 2) : 1; }
        else if (sub == 4) n = 3;
        else n = 1;
        break;
    }
    case 0xF: n = nib(a, 1) == 0xF && nib(a, 2) == 0xF ? (nib(a, 5) == 0 || nib(a, 5) == 2 ? 2 : 1) : 1; break;
    default: n = 1;
    }
    return n > left ? left : n;
}
static bool begins_block(uint32_t a) { int op = nib(a, 0); return op == 1 || op == 8 || (op == 0xC && (nib(a, 1) == 0 || nib(a, 1) == 4)); }

// Skip a block not taken: past its end (or to its else, when stop_at_else), counting nested blocks
static int skip_block(const uint32_t *w, int n, int i, bool stop_at_else) {
    int depth = 0;
    while (i < n) {
        uint32_t a = w[i];
        if (begins_block(a)) depth++;
        else if (nib(a, 0) == 2) {
            bool is_else = nib(a, 1) == 1;
            if (depth == 0) return i + 1;  // (end or else of this block: go on after it)
            if (!is_else) depth--;
            else if (depth == 0 && stop_at_else) return i + 1;
        }
        i += op_len(w + i, n - i);
    }
    return n;
}

static uint64_t rd(uint64_t a, int t) {
    uint64_t v = 0;
    if (t != 1 && t != 2 && t != 4 && t != 8) return 0;
    if (!mem_read(a, &v, (size_t)t)) return 0;
    return v;
}
static void wr(uint64_t a, int t, uint64_t v) {
    if (t == 1 || t == 2 || t == 4 || t == 8) mem_write(a, &v, (size_t)t);
}
static uint64_t mask(int t) { return t >= 8 ? ~0ull : (1ull << (8 * t)) - 1; }
static bool cond(int c, uint64_t x, uint64_t y) {
    switch (c) { case 1: return x > y; case 2: return x >= y; case 3: return x < y; case 4: return x <= y; case 5: return x == y; case 6: return x != y; }
    return false;
}
static uint64_t arith(int op, int t, uint64_t a, uint64_t b) {
    if (op >= 0xA && op <= 0xD) {  // float / double
        if (t == 8) {
            double x, y, z = 0;
            memcpy(&x, &a, 8), memcpy(&y, &b, 8);
            z = op == 0xA ? x + y : op == 0xB ? x - y : op == 0xC ? x * y : y != 0 ? x / y : 0;
            uint64_t r;
            memcpy(&r, &z, 8);
            return r;
        }
        float x, y, z;
        uint32_t ua = (uint32_t)a, ub = (uint32_t)b, r;
        memcpy(&x, &ua, 4), memcpy(&y, &ub, 4);
        z = op == 0xA ? x + y : op == 0xB ? x - y : op == 0xC ? x * y : y != 0 ? x / y : 0;
        memcpy(&r, &z, 4);
        return r;
    }
    uint64_t r;
    switch (op) {
    case 0: r = a + b; break; case 1: r = a - b; break; case 2: r = a * b; break;
    case 3: r = a << (b & 63); break; case 4: r = a >> (b & 63); break; case 5: r = a & b; break;
    case 6: r = a | b; break; case 7: r = ~a; break; case 8: r = a ^ b; break; default: r = a; break;  // 9: move
    }
    return r & mask(t);
}

static void run(const uint32_t *w, int n) {
    int loop_at[16];
    for (int i = 0; i < 16; i++) loop_at[i] = -1;
    int steps = 0;
    for (int i = 0; i < n && steps < 200000; steps++) {
        uint32_t a = w[i];
        int len = op_len(w + i, n - i), op = nib(a, 0), t = nib(a, 1);
        const uint32_t *x = w + i;
        int next = i + len;
        switch (op) {
        case 0x0: {  // 0TMR00AA AAAAAAAA V(V)
            uint64_t ad = region(nib(a, 2)) + g_vm.r[nib(a, 3)] + (((uint64_t)(a & 0xFF) << 32) | x[1]);
            uint64_t v = t == 8 && len == 4 ? ((uint64_t)x[2] << 32) | x[3] : x[2];
            wr(ad, t, v);
            break;
        }
        case 0x1: {  // 1TMC00AA AAAAAAAA V(V)
            uint64_t ad = region(nib(a, 2)) + (((uint64_t)(a & 0xFF) << 32) | x[1]);
            uint64_t v = t == 8 && len == 4 ? ((uint64_t)x[2] << 32) | x[3] : x[2];
            if (!cond(nib(a, 3), rd(ad, t), v & mask(t))) next = skip_block(w, n, next, true);
            break;
        }
        case 0x2:  // end, or else reached at the end of a taken block: skip what the else holds
            if (nib(a, 1) == 1) next = skip_block(w, n, next, false);
            break;
        case 0x3: {
            int r = nib(a, 3);
            if (nib(a, 1) == 0) { g_vm.r[r] = x[1]; loop_at[r] = next; }
            else if (loop_at[r] >= 0 && g_vm.r[r] > 1) { g_vm.r[r]--; next = loop_at[r]; }
            else if (g_vm.r[r]) g_vm.r[r]--;
            break;
        }
        case 0x4: g_vm.r[nib(a, 3)] = ((uint64_t)x[1] << 32) | x[2]; break;
        case 0x5: {  // 5TMRI0AA AAAAAAAA
            int r = nib(a, 3), mode = nib(a, 4);
            uint64_t rel = ((uint64_t)(a & 0xFF) << 32) | x[1];
            uint64_t ad = mode == 1 ? g_vm.r[r] + rel : mode == 2 ? region(nib(a, 2)) + g_vm.r[r] + rel : region(nib(a, 2)) + rel;
            g_vm.r[r] = rd(ad, t);
            break;
        }
        case 0x6: {  // 6T0RIor0 V V
            int r = nib(a, 3), inc = nib(a, 4), o = nib(a, 5), ro = nib(a, 6);
            uint64_t ad = g_vm.r[r] + (o ? g_vm.r[ro] : 0), v = ((uint64_t)x[1] << 32) | x[2];
            wr(ad, t, v);
            if (inc) g_vm.r[r] += (uint64_t)t;
            break;
        }
        case 0x7: { int r = nib(a, 3); g_vm.r[r] = arith(nib(a, 4), t, g_vm.r[r], x[1]); break; }
        case 0x8: {  // 8kkkkkkk: all of those keys held
            uint64_t k = a & 0x0FFFFFFF;
            if ((g_vm.keys & k) != k) next = skip_block(w, n, next, true);
            break;
        }
        case 0x9: {  // 9TCRS0s0 / 9TCRS100 V(V)
            int c = nib(a, 2), r = nib(a, 3), s = nib(a, 4);
            uint64_t b = nib(a, 5) == 1 ? (t == 8 && len == 3 ? ((uint64_t)x[1] << 32) | x[2] : x[1]) : g_vm.r[nib(a, 6)];
            g_vm.r[r] = arith(c, t, g_vm.r[s], b);
            break;
        }
        case 0xA: {  // ATSRIOxa (aaaaaaaa)
            int s = nib(a, 2), r = nib(a, 3), inc = nib(a, 4), o = nib(a, 5), xx = nib(a, 6);
            uint64_t imm = len == 2 ? (((uint64_t)nib(a, 7) << 32) | x[1]) : 0, ad;
            switch (o) {
            case 1: ad = g_vm.r[r] + g_vm.r[xx]; break;
            case 2: ad = g_vm.r[r] + imm; break;
            case 3: ad = region(xx) + g_vm.r[r]; break;
            case 4: ad = region(xx) + imm; break;
            case 5: ad = region(xx) + g_vm.r[r] + imm; break;
            default: ad = g_vm.r[r];
            }
            wr(ad, t, g_vm.r[s]);
            if (inc) g_vm.r[r] += (uint64_t)t;
            break;
        }
        case 0xC: {
            int sub = nib(a, 1);
            if (sub == 0) {  // C0TcSX..
                int tt = nib(a, 2), c = nib(a, 3), s = nib(a, 4), xx = nib(a, 5), n6 = nib(a, 6), n7 = nib(a, 7);
                uint64_t v = 0;
                switch (xx) {
                case 0: v = rd(region(n6) + (((uint64_t)n7 << 32) | x[1]), tt); break;
                case 1: v = rd(region(n6) + g_vm.r[n7], tt); break;
                case 2: v = rd(g_vm.r[n6] + (((uint64_t)n7 << 32) | x[1]), tt); break;
                case 3: v = rd(g_vm.r[n6] + g_vm.r[n7], tt); break;
                case 4: v = tt == 8 && len == 3 ? ((uint64_t)x[1] << 32) | x[2] : x[1]; break;
                case 5: v = g_vm.r[n6]; break;
                }
                if (!cond(c, g_vm.r[s] & mask(tt), v & mask(tt))) next = skip_block(w, n, next, true);
            } else if (sub == 1) {  // C10D0Sx0
                int d = nib(a, 3), s = nib(a, 5), xx = nib(a, 6);
                if (xx == 0) g_vm.r[d] = g_vm.saved[s];
                else if (xx == 1) g_vm.saved[d] = g_vm.r[s];
                else if (xx == 2) g_vm.saved[d] = 0;
                else g_vm.r[d] = 0;
            } else if (sub == 2) {  // C2x0XXXX
                int xx = nib(a, 2);
                for (int k = 0; k < 16; k++) {
                    if (!((a >> k) & 1)) continue;
                    if (xx == 0) g_vm.r[k] = g_vm.saved[k];
                    else if (xx == 1) g_vm.saved[k] = g_vm.r[k];
                    else if (xx == 2) g_vm.saved[k] = 0;
                    else g_vm.r[k] = 0;
                }
            } else if (sub == 3) {  // C3000XXx
                int idx = (int)((a >> 4) & 0xFF), r = (int)(a & 0xF);
                if (idx < 0x80) g_vm.r[r] = g_vm.stat[idx];
                else g_vm.stat[idx - 0x80] = g_vm.r[r];
            } else if (sub == 4) {  // C4r00000 KKKKKKKK KKKKKKKK
                uint64_t k = ((uint64_t)x[1] << 32) | x[2];
                bool held = (g_vm.keys & k) == k, repeat = nib(a, 2) != 0;
                bool take = held && (repeat || (g_vm.keys_before & k) != k);
                if (!take) next = skip_block(w, n, next, true);
            }
            break;
        }
        default: break;  // (FF0 / FF1 / FFF and unknown: nothing)
        }
        i = next;
    }
}

// ------------------------------------------------------------------ files
static uint64_t text_size_of(const char *exefs_main) {
    FILE *f = fopen(exefs_main, "rb");
    if (!f) return 0;
    uint8_t h[0x40];
    uint64_t s = 0;
    if (fread(h, 1, sizeof h, f) == sizeof h && !memcmp(h, "NSO0", 4)) s = (uint64_t)h[0x18] | h[0x19] << 8 | h[0x1a] << 16 | (uint64_t)h[0x1b] << 24;
    fclose(f);
    return s;
}

static void free_all(void) {
    for (int i = 0; i < g_n; i++) free(g_c[i].w);
    free(g_c);
    g_c = NULL, g_n = 0;
}

static void read_enabled(void) {
    FILE *f = fopen(CHEATS_DIR "/enabled.txt", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        for (int i = 0; i < g_n; i++) if (!strcmp(g_c[i].name, line)) g_c[i].on = true;
    }
    fclose(f);
}
static void save_enabled(void) {
    mkdir(CHEATS_DIR, 0755);
    FILE *f = fopen(CHEATS_DIR "/enabled.txt", "w");
    if (!f) return;
    for (int i = 0; i < g_n; i++) if (g_c[i].on && !g_c[i].master) fprintf(f, "%s\n", g_c[i].name);
    fclose(f);
}

static void read_notes(void) {  // Breeze's notes.txt: [Name] then note=...
    FILE *f = fopen(CHEATS_DIR "/notes.txt", "r");
    if (!f) return;
    char line[512], cur[128] = "";
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '[') {
            char *e = strrchr(line, ']');
            if (e) *e = 0;
            snprintf(cur, sizeof cur, "%s", line + 1);
        } else if (!strncmp(line, "note=", 5))
            for (int i = 0; i < g_n; i++)
                if (!strcmp(g_c[i].name, cur)) snprintf(g_c[i].note, sizeof g_c[i].note, "%s", line + 5);
    }
    fclose(f);
}

static bool hexword(const char *s, uint32_t *out) {
    int k = 0;
    uint32_t v = 0;
    for (; isxdigit((unsigned char)s[k]); k++) v = v << 4 | (uint32_t)(isdigit((unsigned char)s[k]) ? s[k] - '0' : (tolower((unsigned char)s[k]) - 'a' + 10));
    if (k != 8) return false;
    *out = v;
    return true;
}

// Parses an Atmosphère cheat file into a list (out, count)
static int parse(const char *path, Cheat **out) {
    FILE *f = fopen(path, "r");
    *out = NULL;
    if (!f) return 0;
    Cheat *c = NULL;
    int n = 0, cap = 0;
    char line[1024];
    Cheat *cur = NULL;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (isspace((unsigned char)*s)) s++;
        if (*s == '[' || *s == '{') {
            char close = *s == '[' ? ']' : '}';
            char *e = strrchr(s, close);
            if (!e) continue;
            *e = 0;
            if (n == cap) cap = cap ? cap * 2 : 64, c = realloc(c, sizeof *c * (size_t)cap);
            cur = &c[n++];
            memset(cur, 0, sizeof *cur);
            snprintf(cur->name, sizeof cur->name, "%s", s + 1);
            cur->master = close == '}';
            continue;
        }
        if (!cur) continue;
        for (char *t = strtok(s, " \t\r\n"); t; t = strtok(NULL, " \t\r\n")) {
            uint32_t v;
            if (!hexword(t, &v)) continue;
            cur->w = realloc(cur->w, sizeof *cur->w * (size_t)(cur->n + 1));
            cur->w[cur->n++] = v;
        }
    }
    fclose(f);
    // the title line ([Pokemon Legends Z-A v393216 TID=... BID=...]) and section headers (only 20000000 / 20000001):
    // headers, never run
    // (Breeze's folders: 20000000 opens one, 20000001 closes it - the closing entry isn't shown.) "Restore Code" entries
    // put the game's words back: done here when a cheat is switched off, and run beside others it would undo them.
    for (int i = 0; i < n; i++) {
        bool only_ends = true, closes = c[i].n > 0;
        for (int k = 0; k < c[i].n; k++) only_ends &= c[i].w[k] == 0x20000000 || c[i].w[k] == 0x20000001, closes &= c[i].w[k] == 0x20000001;
        c[i].header = !c[i].master && only_ends;
        c[i].hidden = (c[i].header && closes) || !strncasecmp(c[i].name, "Restore Code", 12);
        char *o = c[i].label;
        for (const char *q = c[i].name; *q && o < c[i].label + sizeof c[i].label - 2; q++) {
            if ((unsigned char)q[0] == 0xE2 && (unsigned char)q[1] == 0x84 && (unsigned char)q[2] == 0x96) *o++ = '#', q += 2;  // №
            else *o++ = *q;
        }
        *o = 0;
    }
    *out = c;
    return n;
}

static void classify_code(void) {  // the cheats that write main's code (op 0 / 1-free static stores into the code region)
    for (int i = 0; i < g_n; i++) {
        Cheat *ch = &g_c[i];
        ch->code = false;
        for (int k = 0; k < ch->n;) {
            uint32_t a = ch->w[k];
            int len = op_len(ch->w + k, ch->n - k);
            if (nib(a, 0) == 0 && nib(a, 2) == 0 && len >= 3) {
                uint64_t off = ((uint64_t)(a & 0xFF) << 32) | ch->w[k + 1];
                if (off < ((g_text_end + 0xFFF) & ~0xFFFull)) ch->code = true;
            }
            k += len;
        }
    }
}

void cheats_load(void) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_n; i++) if (g_c[i].on) restore_owned(i);
    free_all();
    g_bid[0] = g_path[0] = 0;
    FILE *f = fopen("extracted/exefs/main", "rb");
    if (f) {
        uint8_t id[8];
        if (fseek(f, 0x40, SEEK_SET) == 0 && fread(id, 1, 8, f) == 8)
            for (int i = 0; i < 8; i++) snprintf(g_bid + 2 * i, 3, "%02X", id[i]);
        fclose(f);
    }
    g_text_end = text_size_of("extracted/exefs/main");
    if (g_bid[0]) {
        snprintf(g_path, sizeof g_path, CHEATS_DIR "/%s.txt", g_bid);
        g_n = parse(g_path, &g_c);
        if (!g_n) {  // (a lower-case file name, as some packs ship)
            char lc[64];
            snprintf(lc, sizeof lc, "%s", g_path);
            for (char *p = lc + strlen(CHEATS_DIR) + 1; *p; p++) *p = (char)tolower((unsigned char)*p);
            g_n = parse(lc, &g_c);
            if (g_n) snprintf(g_path, sizeof g_path, "%s", lc);
        }
        for (int i = 0; i < g_n; i++) if (g_c[i].master) g_c[i].on = true;
        read_enabled();
        read_notes();
        classify_code();
    }
    if (g_n) LOG("[cheats] %s: %d entries\n", g_path, g_n);
    pthread_mutex_unlock(&g_lock);
}

const char *cheats_build_id(void) { return g_bid; }
const char *cheats_file(void) { return g_n ? g_path : ""; }
int cheats_count(void) { return g_n; }
const char *cheats_name(int i) { return i >= 0 && i < g_n ? g_c[i].label : ""; }
bool cheats_is_hidden(int i) { return i >= 0 && i < g_n && g_c[i].hidden; }
const char *cheats_note(int i) { return i >= 0 && i < g_n ? g_c[i].note : ""; }
bool cheats_is_master(int i) { return i >= 0 && i < g_n && g_c[i].master; }
bool cheats_is_header(int i) { return i >= 0 && i < g_n && g_c[i].header; }
bool cheats_is_code(int i) { return i >= 0 && i < g_n && g_c[i].code; }
bool cheats_enabled(int i) { return i >= 0 && i < g_n && g_c[i].on; }

void cheats_set(int i, bool on) {
    pthread_mutex_lock(&g_lock);
    if (i >= 0 && i < g_n && !g_c[i].master && !g_c[i].header && !g_c[i].hidden && g_c[i].on != on) {
        g_c[i].on = on;
        if (!on) restore_owned(i);
        save_enabled();
        LOG("[cheats] %s: %s\n", g_c[i].name, on ? "on" : "off");
    }
    pthread_mutex_unlock(&g_lock);
}
void cheats_all_off(void) {
    for (int i = 0; i < g_n; i++) cheats_set(i, false);
}

void cheats_frame(void) {
    static uint64_t last, period;
    if (!g_n) return;
    if (!period) {
        const char *e = getenv("LARECOMP_CHEAT_HZ");
        int hz = e && atoi(e) > 0 ? atoi(e) : 12;
        period = 1000000000ull / (uint64_t)hz;
    }
    uint64_t now = plat_now_ns();
    if (now - last < period) return;
    last = now;
    extern uint64_t input_buttons_now(void);  // (runtime/hle/input.c: the Switch's button bits, as Atmosphère's keys)
    pthread_mutex_lock(&g_lock);
    g_vm.keys_before = g_vm.keys;
    g_vm.keys = input_buttons_now();
    for (int pass = 0; pass < 2; pass++)  // masters first
        for (int i = 0; i < g_n; i++) {
            Cheat *c = &g_c[i];
            if (!c->on || c->header || c->hidden || c->master != (pass == 0)) continue;
            memset(g_vm.r, 0, sizeof g_vm.r);
            g_running_cheat = i;
            run(c->w, c->n);
        }
    g_running_cheat = -1;
    pthread_mutex_unlock(&g_lock);
}

// ------------------------------------------------------------------ code sites (the generator's)
typedef struct { uint64_t off; uint32_t w[32]; int n; } Site;
static int site_cmp(const void *a, const void *b) {
    uint64_t x = ((const Site *)a)->off, y = ((const Site *)b)->off;
    return x < y ? -1 : x > y;
}
static void site_add(Site **s, int *n, int *cap, uint64_t off, uint32_t w) {
    for (int i = 0; i < *n; i++)
        if ((*s)[i].off == off) {
            for (int k = 0; k < (*s)[i].n; k++) if ((*s)[i].w[k] == w) return;
            if ((*s)[i].n < 32) (*s)[i].w[(*s)[i].n++] = w;
            return;
        }
    if (*n == *cap) *cap = *cap ? *cap * 2 : 128, *s = realloc(*s, sizeof **s * (size_t)*cap);
    (*s)[*n].off = off, (*s)[*n].w[0] = w, (*s)[*n].n = 1;
    (*n)++;
}

// Every static store of the file into the code region (op 0, main, 4 or 8 bytes): the words it can hold
static int collect_sites(Site **out) {
    Site *s = NULL;
    int n = 0, cap = 0;
    uint64_t lim = (g_text_end + 0xFFF) & ~0xFFFull;
    for (int i = 0; i < g_n; i++) {
        Cheat *ch = &g_c[i];
        for (int k = 0; k < ch->n;) {
            uint32_t a = ch->w[k];
            int len = op_len(ch->w + k, ch->n - k), t = nib(a, 1);
            if (nib(a, 0) == 0 && nib(a, 2) == 0 && len >= 3) {
                uint64_t off = ((uint64_t)(a & 0xFF) << 32) | ch->w[k + 1];
                if (off < lim && !(off & 3)) {
                    if (t == 4) site_add(&s, &n, &cap, off, ch->w[k + 2]);
                    else if (t == 8 && len == 4) site_add(&s, &n, &cap, off, ch->w[k + 3]), site_add(&s, &n, &cap, off + 4, ch->w[k + 2]);
                }
            }
            k += len;
        }
    }
    qsort(s, (size_t)n, sizeof *s, site_cmp);
    *out = s;
    return n;
}

static void sites_hash(const Site *s, int n, char *out, size_t cap) {
    uint64_t h = 1469598103934665603ull;  // FNV-1a over the sites
    for (int i = 0; i < n; i++) {
        h = (h ^ s[i].off) * 1099511628211ull;
        for (int k = 0; k < s[i].n; k++) h = (h ^ s[i].w[k]) * 1099511628211ull;
    }
    snprintf(out, cap, "%016llx", n ? (unsigned long long)h : 0ull);
}

bool cheats_write_sites(const char *json_path, char *hash, size_t hn) {
    cheats_load();
    pthread_mutex_lock(&g_lock);
    Site *s;
    int n = collect_sites(&s);
    sites_hash(s, n, hash, hn);
    FILE *f = fopen(json_path, "w");
    if (f) {
        fputs("{", f);
        for (int i = 0; i < n; i++) {
            fprintf(f, "%s\"0x%llx\": [", i ? ",\n" : "\n", (unsigned long long)s[i].off);
            for (int k = 0; k < s[i].n; k++) fprintf(f, "%s\"0x%08x\"", k ? ", " : "", s[i].w[k]);
            fputs("]", f);
        }
        fputs("\n}\n", f);
        fclose(f);
    }
    free(s);
    LOG("[cheats] %d code sites for the game code (%s)\n", n, hash);
    pthread_mutex_unlock(&g_lock);
    return f != NULL;
}

bool cheats_need_rebuild(void) {
    pthread_mutex_lock(&g_lock);
    Site *s;
    int n = collect_sites(&s);
    char want[32], have[32] = "";
    sites_hash(s, n, want, sizeof want);
    free(s);
    pthread_mutex_unlock(&g_lock);
    if (!n) return false;
    FILE *f = fopen("extracted/" GAMELIB_NAME ".cheats", "r");
    if (f) { if (!fgets(have, sizeof have, f)) have[0] = 0; fclose(f); }
    have[strcspn(have, "\r\n")] = 0;
    return strcmp(have, want) != 0;
}

void cheats_mark_built(const char *game_dir, const char *hash) {
    char p[1100];
    snprintf(p, sizeof p, "%s/" GAMELIB_NAME ".cheats", game_dir);
    FILE *f = fopen(p, "w");
    if (f) fprintf(f, "%s\n", hash), fclose(f);
}

// ------------------------------------------------------------------ import / download
static bool copy_file(const char *from, const char *to) {
    FILE *a = fopen(from, "rb");
    if (!a) return false;
    FILE *b = fopen(to, "wb");
    if (!b) { fclose(a); return false; }
    char buf[65536];
    size_t k;
    bool ok = true;
    while ((k = fread(buf, 1, sizeof buf, a)) > 0) ok &= fwrite(buf, 1, k, b) == k;
    fclose(a);
    ok &= fclose(b) == 0;
    return ok;
}

static bool ends_with(const char *s, const char *e) {
    size_t a = strlen(s), b = strlen(e);
    return a >= b && !strcasecmp(s + a - b, e);
}

static int run_wait(const char *const *argv, const char *log) {
    char exe[1100];  // (plat_spawn takes a path: a bare name is looked up in PATH here)
    const char *v[16];
    int k = 0;
    for (; argv[k] && k < 15; k++) v[k] = argv[k];
    v[k] = NULL;
    if (!strchr(argv[0], '/')) {
        const char *path = getenv("PATH");
        bool found = false;
        for (const char *p = path ? path : "/usr/bin:/bin"; p && *p && !found;) {
            const char *e = strchr(p, ':');
            int len = e ? (int)(e - p) : (int)strlen(p);
            snprintf(exe, sizeof exe, "%.*s/%s", len, p, argv[0]);
            found = access(exe, X_OK) == 0;
            p = e ? e + 1 : NULL;
        }
        if (!found) return -1;
        v[0] = exe;
    }
    PlatProc p = plat_spawn(v, log, false);
    if (!p) return -1;
    return plat_proc_wait(p, true);
}

// a folder searched for <bid>.txt (any case) and notes.txt
static bool find_in(const char *dir, const char *want, char *out, size_t n, int depth) {
    DIR *d = opendir(dir);
    if (!d) return false;
    bool ok = false;
    for (struct dirent *e; !ok && (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        char p[1100];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode) && depth < 6) ok = find_in(p, want, out, n, depth + 1);
        else if (S_ISREG(st.st_mode) && !strcasecmp(e->d_name, want)) snprintf(out, n, "%s", p), ok = true;
    }
    closedir(d);
    return ok;
}

bool cheats_import(const char *path, char *msg, size_t mn) {
    if (!g_bid[0]) cheats_load();
    if (!g_bid[0]) { snprintf(msg, mn, "Set up the game files first (the cheats are for its build)."); return false; }
    mkdir(CHEATS_DIR, 0755);
    char want[32], dst[64];
    snprintf(want, sizeof want, "%s.txt", g_bid);
    snprintf(dst, sizeof dst, CHEATS_DIR "/%s", want);
    bool ok = false;
    if (ends_with(path, ".zip")) {
        const char *tmp = CHEATS_DIR "/.import";
        plat_remove_tree(tmp);
        mkdir(tmp, 0755);
        char py[1100];
        int r = -1;
        if (rt_resource("python" DIRSEP "python" PLAT_EXE_SUFFIX, NULL, py, sizeof py)) {
            const char *argv[] = {py, "-I", "-m", "zipfile", "-e", path, tmp, NULL};
            r = run_wait(argv, CHEATS_DIR "/.import.log");
        }
        if (r != 0) { const char *argv[] = {"unzip", "-o", "-q", path, "-d", tmp, NULL}; r = run_wait(argv, CHEATS_DIR "/.import.log"); }
        if (r != 0) { const char *argv[] = {"bsdtar", "-xf", path, "-C", tmp, NULL}; r = run_wait(argv, CHEATS_DIR "/.import.log"); }
        char found[1100], notes[1100];
        if (r == 0 && find_in(tmp, want, found, sizeof found, 0)) {
            ok = copy_file(found, dst);
            char dir[1100];
            snprintf(dir, sizeof dir, "%s", found);
            char *sl = strrchr(dir, '/');
            if (sl) *sl = 0;
            if (find_in(dir, "notes.txt", notes, sizeof notes, 5)) copy_file(notes, CHEATS_DIR "/notes.txt");
            snprintf(msg, mn, ok ? "Cheats for build %s installed from the zip." : "The cheat file couldn't be copied.", g_bid);
        } else
            snprintf(msg, mn, r != 0 ? "The zip couldn't be opened." : "That zip has no cheats for this game's build (%s).", g_bid);
        plat_remove_tree(tmp);
    } else if (ends_with(path, ".txt")) {
        Cheat *c;
        int n = parse(path, &c);
        for (int i = 0; i < n; i++) free(c[i].w);
        free(c);
        if (!n) snprintf(msg, mn, "No cheats in that file.");
        else {
            const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
            if (strcasecmp(base, want) && strcasecmp(base, "notes.txt"))
                LOG("[cheats] %s imported as %s (its name isn't this build's id: check it is for 2.0.2)\n", base, want);
            ok = copy_file(path, dst);
            snprintf(msg, mn, ok ? "%d cheats installed." : "The cheat file couldn't be copied.", n);
        }
    } else
        snprintf(msg, mn, "Pick a cheat file (.txt) or a zip of them.");
    if (ok) cheats_load();
    return ok;
}

static _Atomic int g_dl;
static char g_dl_msg[300];
int cheats_download_state(void) { return g_dl; }
const char *cheats_download_msg(void) { return g_dl_msg; }

static int fetch(const char *url, const char *out) {  // curl, else wget: 0 = ok
    const char *ua = "Mozilla/5.0 (X11; Linux x86_64) Gecko/20100101 Firefox/140.0";
    const char *a1[] = {"curl", "-fsSL", "--retry", "3", "-A", ua, "-e", CHEATS_DB_THREAD, "-o", out, url, NULL};
    int r = run_wait(a1, CHEATS_DIR "/.download.log");
    if (r != 0) {
        const char *a2[] = {"wget", "-q", "--tries=3", "-U", ua, "--referer=" CHEATS_DB_THREAD, "-O", out, url, NULL};
        r = run_wait(a2, CHEATS_DIR "/.download.log");
    }
    return r;
}

static void *download_thread(void *arg) {
    (void)arg;
    mkdir(CHEATS_DIR, 0755);
    const char *page = CHEATS_DIR "/.thread.html", *zip = CHEATS_DIR "/.database.zip";
    if (fetch(CHEATS_DB_THREAD, page) != 0) {
        snprintf(g_dl_msg, sizeof g_dl_msg, "Couldn't reach GBAtemp (curl or wget needed, and a connection).");
        g_dl = CHEATS_DL_FAILED;
        return NULL;
    }
    // the first post's newest zip attachment: /attachments/<name>-zip.<id>/
    FILE *f = fopen(page, "rb");
    char best[300] = "";
    long best_id = -1;
    if (f) {
        static char buf[1 << 21];
        size_t k = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[k] = 0;
        for (char *p = strstr(buf, "/attachments/"); p; p = strstr(p + 1, "/attachments/")) {
            char *q = p + 13, *e = q;
            while (*e && *e != '/' && *e != '"' && (e - q) < 200) e++;
            if (*e != '/') continue;
            char nm[256];
            snprintf(nm, sizeof nm, "%.*s", (int)(e - q), q);
            char *dot = strrchr(nm, '.');
            if (!dot || !strstr(nm, "-zip.")) continue;
            long id = atol(dot + 1);
            if (id > best_id) best_id = id, snprintf(best, sizeof best, "https://gbatemp.net/attachments/%s/", nm);
        }
    }
    remove(page);
    if (best_id < 0) {
        snprintf(g_dl_msg, sizeof g_dl_msg, "No cheat zip found on the database's page.");
        g_dl = CHEATS_DL_FAILED;
        return NULL;
    }
    LOG("[cheats] downloading %s\n", best);
    if (fetch(best, zip) != 0) {
        snprintf(g_dl_msg, sizeof g_dl_msg, "The download failed.");
        g_dl = CHEATS_DL_FAILED;
        return NULL;
    }
    char m[256];
    bool ok = cheats_import(zip, m, sizeof m);
    remove(zip);
    snprintf(g_dl_msg, sizeof g_dl_msg, "%s", m);
    g_dl = ok ? CHEATS_DL_DONE : CHEATS_DL_FAILED;
    return NULL;
}

void cheats_download_start(void) {
    if (g_dl == CHEATS_DL_RUNNING) return;
    g_dl = CHEATS_DL_RUNNING;
    snprintf(g_dl_msg, sizeof g_dl_msg, "Downloading...");
    pthread_t t;
    pthread_create(&t, NULL, download_thread, NULL);
    pthread_detach(t);
}
