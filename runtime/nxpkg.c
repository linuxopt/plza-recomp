// Packages -> game files (runtime/nxpkg.h). What is read, in order:
//   .nsp = a PFS0 of NCAs (+ tickets .tik); .xci = a gamecard image whose HFS0 "secure" partition holds the NCAs.
//   NCA3: 0xC00 header bytes in AES-XTS (header_key, sectors of 0x200, the sector number as a big-endian tweak); the
//   content key is the key area's AES-CTR key (key_area_key_<kind>_<generation>) or, with a rights id, the title key
//   from the ticket / title.keys (titlekek_<generation>). Sections are AES-CTR: the counter is the section's upper IV
//   (FS header +0x140, big-endian) and the byte offset in the NCA / 16.
//   An update's RomFS section (BKTR): an indirect table maps the patched section's offsets to the base game's RomFS
//   section (storage 0) or to the update's own data (storage 1), whose AES-CTR counter takes a generation from the
//   AesCtrEx table per range. Both tables are bucket trees (16 KiB nodes) in plain AES-CTR at the section's end.
//   The RomFS image is the IVFC data level (the last of the hash tree) of that section.
// Layouts this doesn't read (sparse and compressed sections, NCZ) are refused with a message.
#include "nxpkg.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "platform.h"  // (Windows: POSIX mkdir)
#include "rt.h"

#ifdef _WIN32
#define xseek(f, o) _fseeki64(f, (long long)(o), SEEK_SET)
#define xsize(f) (_fseeki64(f, 0, SEEK_END), (uint64_t)_ftelli64(f))
#else
#define xseek(f, o) fseeko(f, (off_t)(o), SEEK_SET)
#define xsize(f) (fseeko(f, 0, SEEK_END), (uint64_t)ftello(f))
#endif
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#include <immintrin.h>
#define HAVE_AESNI 1
#endif

#define CHUNK (8u << 20)

// ------------------------------------------------------------------ AES-128 (ECB / CTR / XTS)
static uint8_t g_sbox[256], g_isbox[256];

static uint8_t rotl8(uint8_t x, int s) { return (uint8_t)(x << s | x >> (8 - s)); }
static void aes_tables(void) {
    if (g_sbox[0]) return;
    uint8_t p = 1, q = 1;
    do {  // (p walks the field by multiplying by 3, q by dividing by 3: q = 1/p)
        p = (uint8_t)(p ^ (p << 1) ^ (p & 0x80 ? 0x1B : 0));
        q ^= (uint8_t)(q << 1), q ^= (uint8_t)(q << 2), q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;
        g_sbox[p] = (uint8_t)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63);
    } while (p != 1);
    g_sbox[0] = 0x63;
    for (int i = 0; i < 256; i++) g_isbox[g_sbox[i]] = (uint8_t)i;
}

typedef struct { uint8_t rk[176]; } Aes;

static void aes_key(Aes *a, const uint8_t key[16]) {
    aes_tables();
    memcpy(a->rk, key, 16);
    uint8_t rc = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4];
        memcpy(t, a->rk + i - 4, 4);
        if (i % 16 == 0) {
            uint8_t t0 = t[0];
            t[0] = (uint8_t)(g_sbox[t[1]] ^ rc), t[1] = g_sbox[t[2]], t[2] = g_sbox[t[3]], t[3] = g_sbox[t0];
            rc = (uint8_t)(rc << 1 ^ (rc & 0x80 ? 0x1B : 0));
        }
        for (int j = 0; j < 4; j++) a->rk[i + j] = (uint8_t)(a->rk[i - 16 + j] ^ t[j]);
    }
}

static uint8_t xt(uint8_t x) { return (uint8_t)(x << 1 ^ (x & 0x80 ? 0x1B : 0)); }
static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    for (; b; b >>= 1, a = xt(a)) if (b & 1) r ^= a;
    return r;
}

static void aes_enc(const Aes *a, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ a->rk[i];
    for (int r = 1; r <= 10; r++) {
        for (int i = 0; i < 16; i++) t[i] = g_sbox[s[(i + 4 * (i % 4)) % 16]];  // SubBytes + ShiftRows
        if (r < 10)
            for (int c = 0; c < 16; c += 4) {  // MixColumns
                uint8_t a0 = t[c], a1 = t[c + 1], a2 = t[c + 2], a3 = t[c + 3], x = a0 ^ a1 ^ a2 ^ a3;
                t[c] ^= x ^ xt(a0 ^ a1), t[c + 1] ^= x ^ xt(a1 ^ a2), t[c + 2] ^= x ^ xt(a2 ^ a3), t[c + 3] ^= x ^ xt(a3 ^ a0);
            }
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ a->rk[16 * r + i];
    }
    memcpy(out, s, 16);
}

static void aes_dec(const Aes *a, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ a->rk[160 + i];
    for (int r = 9; r >= 0; r--) {
        for (int i = 0; i < 16; i++) t[(i + 4 * (i % 4)) % 16] = g_isbox[s[i]];  // InvShiftRows + InvSubBytes
        for (int i = 0; i < 16; i++) t[i] ^= a->rk[16 * r + i];
        if (r > 0)
            for (int c = 0; c < 16; c += 4) {  // InvMixColumns
                uint8_t a0 = t[c], a1 = t[c + 1], a2 = t[c + 2], a3 = t[c + 3];
                t[c] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
                t[c + 1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
                t[c + 2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
                t[c + 3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
            }
        memcpy(s, t, 16);
    }
    memcpy(out, s, 16);
}

static void ecb_dec(const uint8_t key[16], const uint8_t *in, uint8_t *out, size_t n) {
    Aes a;
    aes_key(&a, key);
    for (size_t i = 0; i + 16 <= n; i += 16) aes_dec(&a, in + i, out + i);
}

#ifdef HAVE_AESNI
__attribute__((target("aes,sse2"))) static void ctr_ni(const Aes *a, uint64_t hi, uint64_t ctr, uint8_t *buf, size_t nb) {
    __m128i k[11];
    for (int i = 0; i < 11; i++) k[i] = _mm_loadu_si128((const __m128i *)(a->rk + 16 * i));
    size_t b = 0;
    for (; b + 8 <= nb; b += 8) {
        __m128i x[8];
        for (int j = 0; j < 8; j++) x[j] = _mm_xor_si128(_mm_set_epi64x((long long)__builtin_bswap64(ctr + j), (long long)hi), k[0]);
        for (int r = 1; r < 10; r++)
            for (int j = 0; j < 8; j++) x[j] = _mm_aesenc_si128(x[j], k[r]);
        for (int j = 0; j < 8; j++) {
            __m128i *p = (__m128i *)(buf + 16 * (b + j));
            _mm_storeu_si128(p, _mm_xor_si128(_mm_loadu_si128(p), _mm_aesenclast_si128(x[j], k[10])));
        }
        ctr += 8;
    }
    for (; b < nb; b++, ctr++) {
        __m128i x = _mm_xor_si128(_mm_set_epi64x((long long)__builtin_bswap64(ctr), (long long)hi), k[0]);
        for (int r = 1; r < 10; r++) x = _mm_aesenc_si128(x, k[r]);
        __m128i *p = (__m128i *)(buf + 16 * b);
        _mm_storeu_si128(p, _mm_xor_si128(_mm_loadu_si128(p), _mm_aesenclast_si128(x, k[10])));
    }
}
#endif

// AES-CTR over buf (whole blocks), the counter: upper (big-endian) | block index (big-endian).
static void ctr_xor(const Aes *a, uint64_t upper, uint64_t block, uint8_t *buf, size_t nb) {
    uint8_t iv[16];
    for (int i = 0; i < 8; i++) iv[i] = (uint8_t)(upper >> (56 - 8 * i));
#ifdef HAVE_AESNI
    static int ni = -1;
    if (ni < 0) {  // (CPUID 1, ECX bit 25: AES-NI - read directly: __builtin_cpu_supports needs a runtime symbol the
                   // Windows build (zig's lld-link) doesn't have)
        unsigned a, b, c, d;
        ni = __get_cpuid(1, &a, &b, &c, &d) && (c >> 25 & 1);
    }
    if (ni) {
        uint64_t hi;
        memcpy(&hi, iv, 8);
        ctr_ni(a, hi, block, buf, nb);
        return;
    }
#endif
    for (size_t b = 0; b < nb; b++, block++) {
        uint8_t ks[16];
        for (int i = 0; i < 8; i++) iv[8 + i] = (uint8_t)(block >> (56 - 8 * i));
        aes_enc(a, iv, ks);
        for (int i = 0; i < 16; i++) buf[16 * b + i] ^= ks[i];
    }
}

// The NCA header: AES-XTS, sectors of 0x200 from sector 0, the tweak = the sector number, big-endian.
static void xts_dec(const uint8_t key[32], uint8_t *buf, size_t n) {
    Aes k1, k2;
    aes_key(&k1, key), aes_key(&k2, key + 16);
    for (size_t sec = 0; sec * 0x200 < n; sec++) {
        uint8_t tw[16] = {0}, t[16];
        for (int i = 0; i < 8; i++) tw[15 - i] = (uint8_t)(sec >> (8 * i));
        aes_enc(&k2, tw, t);
        for (size_t o = sec * 0x200; o < (sec + 1) * 0x200 && o + 16 <= n; o += 16) {
            uint8_t x[16];
            for (int i = 0; i < 16; i++) x[i] = buf[o + i] ^ t[i];
            aes_dec(&k1, x, x);
            for (int i = 0; i < 16; i++) buf[o + i] = x[i] ^ t[i];
            int carry = t[15] >> 7;  // (the tweak times x in GF(2^128), little-endian)
            for (int i = 15; i > 0; i--) t[i] = (uint8_t)(t[i] << 1 | t[i - 1] >> 7);
            t[0] = (uint8_t)(t[0] << 1 ^ (carry ? 0x87 : 0));
        }
    }
}

// ------------------------------------------------------------------ keys
#define NGEN 0x20
typedef struct {
    uint8_t header_key[32];
    uint8_t kak[3][NGEN][16];  // key_area_key_{application,ocean,system}_XX
    uint8_t titlekek[NGEN][16], master[NGEN][16];
    uint32_t have_kak[3], have_titlekek, have_master;  // (bit per generation)
    bool have_header;
    // sources, to derive what a keys file leaves out
    uint8_t kak_src[3][16], titlekek_src[16], aes_kek_gen_src[16], aes_key_gen_src[16], header_kek_src[16], header_key_src[32];
    uint32_t have_src;  // bits: 0-2 kak, 3 titlekek, 4 aes_kek_gen, 5 aes_key_gen, 6 header_kek, 7 header_key
    char path[1024];
} Keys;

typedef struct { uint8_t rights[16], key[16]; } TitleKey;

static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static size_t parse_hex(const char *s, uint8_t *out, size_t max) {
    size_t n = 0;
    while (*s == ' ' || *s == '\t') s++;
    for (; n < max && hexval(s[0]) >= 0 && hexval(s[1]) >= 0; s += 2) out[n++] = (uint8_t)(hexval(s[0]) << 4 | hexval(s[1]));
    return hexval(*s) >= 0 ? 0 : n;  // (longer than expected: not this key)
}

static bool keys_file(Keys *k, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[512];
    static const char *const kind[3] = {"application", "ocean", "system"};
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char name[128];
        size_t n = 0;
        for (char *c = line; *c && n + 1 < sizeof name; c++)
            if (!isspace((unsigned char)*c)) name[n++] = (char)tolower((unsigned char)*c);
        name[n] = 0;
        const char *v = eq + 1;
        uint8_t b[32];
        unsigned gen = 0;
        char tail[8];
        if (!strcmp(name, "header_key") && parse_hex(v, b, 32) == 32) memcpy(k->header_key, b, 32), k->have_header = true;
        else if (sscanf(name, "titlekek_%2x%1s", &gen, tail) == 1 && gen < NGEN && parse_hex(v, b, 16) == 16)
            memcpy(k->titlekek[gen], b, 16), k->have_titlekek |= 1u << gen;
        else if (sscanf(name, "master_key_%2x%1s", &gen, tail) == 1 && gen < NGEN && parse_hex(v, b, 16) == 16)
            memcpy(k->master[gen], b, 16), k->have_master |= 1u << gen;
        else if (!strcmp(name, "titlekek_source") && parse_hex(v, b, 16) == 16) memcpy(k->titlekek_src, b, 16), k->have_src |= 8;
        else if (!strcmp(name, "aes_kek_generation_source") && parse_hex(v, b, 16) == 16) memcpy(k->aes_kek_gen_src, b, 16), k->have_src |= 16;
        else if (!strcmp(name, "aes_key_generation_source") && parse_hex(v, b, 16) == 16) memcpy(k->aes_key_gen_src, b, 16), k->have_src |= 32;
        else if (!strcmp(name, "header_kek_source") && parse_hex(v, b, 16) == 16) memcpy(k->header_kek_src, b, 16), k->have_src |= 64;
        else if (!strcmp(name, "header_key_source") && parse_hex(v, b, 32) == 32) memcpy(k->header_key_src, b, 32), k->have_src |= 128;
        else
            for (int i = 0; i < 3; i++) {
                char pre[64], src[64];
                snprintf(pre, sizeof pre, "key_area_key_%s_%%2x%%1s", kind[i]);
                snprintf(src, sizeof src, "key_area_key_%s_source", kind[i]);
                if (sscanf(name, pre, &gen, tail) == 1 && gen < NGEN && parse_hex(v, b, 16) == 16)
                    memcpy(k->kak[i][gen], b, 16), k->have_kak[i] |= 1u << gen;
                else if (!strcmp(name, src) && parse_hex(v, b, 16) == 16) memcpy(k->kak_src[i], b, 16), k->have_src |= 1u << i;
            }
    }
    fclose(f);
    return true;
}

// The keys a file leaves out, from the master keys and the sources (as the console derives them).
static void keys_derive(Keys *k) {
    for (int g = 0; g < NGEN; g++) {
        if (!(k->have_master >> g & 1)) continue;
        if (!(k->have_titlekek >> g & 1) && (k->have_src & 8))
            ecb_dec(k->master[g], k->titlekek_src, k->titlekek[g], 16), k->have_titlekek |= 1u << g;
        if ((k->have_src & 48) != 48) continue;
        uint8_t kek[16];
        ecb_dec(k->master[g], k->aes_kek_gen_src, kek, 16);
        for (int i = 0; i < 3; i++)
            if (!(k->have_kak[i] >> g & 1) && (k->have_src >> i & 1)) {
                uint8_t sk[16];
                ecb_dec(kek, k->kak_src[i], sk, 16);
                ecb_dec(sk, k->aes_key_gen_src, k->kak[i][g], 16);
                k->have_kak[i] |= 1u << g;
            }
        if (g == 0 && !k->have_header && (k->have_src & 0xF0) == 0xF0) {
            uint8_t sk[16], hk[16];
            ecb_dec(kek, k->header_kek_src, sk, 16);
            ecb_dec(sk, k->aes_key_gen_src, hk, 16);
            ecb_dec(hk, k->header_key_src, k->header_key, 32);
            k->have_header = true;
        }
    }
}

static bool exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

// prod.keys: next to the packages, then the usual places (~/.switch, the emulators' key folders).
static bool keys_find(Keys *k, const char *folder) {
    char dirs[24][1024];
    int n = 0;
    snprintf(dirs[n++], sizeof dirs[0], "%s", folder);
    snprintf(dirs[n++], sizeof dirs[0], ".");  // (the data root)
    snprintf(dirs[n++], sizeof dirs[0], "keys");
    const char *home = getenv("HOME");
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (!home) home = getenv("USERPROFILE");
#endif
    if (home) snprintf(dirs[n++], sizeof dirs[0], "%s/.switch", home);
    static const char *const emus[] = {"eden", "yuzu", "suyu", "sudachi", "citron", "torzu"};
    for (size_t i = 0; i < sizeof emus / sizeof *emus && home; i++) {
#ifdef _WIN32
        if (appdata) snprintf(dirs[n++], sizeof dirs[0], "%s/%s/keys", appdata, emus[i]);
#elif defined(__APPLE__)
        snprintf(dirs[n++], sizeof dirs[0], "%s/Library/Application Support/%s/keys", home, emus[i]);
#else
        const char *xdg = getenv("XDG_DATA_HOME");
        if (xdg && *xdg) snprintf(dirs[n++], sizeof dirs[0], "%s/%s/keys", xdg, emus[i]);
        else snprintf(dirs[n++], sizeof dirs[0], "%s/.local/share/%s/keys", home, emus[i]);
#endif
    }
#ifdef _WIN32
    if (appdata) snprintf(dirs[n++], sizeof dirs[0], "%s/Ryujinx/system", appdata);
#elif defined(__APPLE__)
    if (home) snprintf(dirs[n++], sizeof dirs[0], "%s/Library/Application Support/Ryujinx/system", home);
#else
    if (home) snprintf(dirs[n++], sizeof dirs[0], "%s/.config/Ryujinx/system", home);
#endif
    for (int i = 0; i < n; i++) {
        static const char *const names[] = {"prod.keys", "keys.txt", "keys.prod"};
        for (size_t j = 0; j < sizeof names / sizeof *names; j++) {
            char p[1100];
            snprintf(p, sizeof p, "%s/%s", dirs[i], names[j]);
            if (!exists(p)) continue;
            Keys t;
            memset(&t, 0, sizeof t);
            keys_file(&t, p);
            keys_derive(&t);
            if (!t.have_header) continue;
            *k = t;
            snprintf(k->path, sizeof k->path, "%s", p);
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------ packages
typedef struct { char path[1024]; FILE *f; } Pkg;

typedef struct {
    int pkg;
    uint64_t off, size;  // in the package file
    uint8_t h[0xC00];    // the decrypted header
    uint8_t type;        // 0 program, 1 meta, 2 control, 3 manual, 4 data, 5 public data
    uint64_t id;         // program id
    uint32_t version;    // (from the package's meta, when it has one)
    bool patch;          // an update's program (its meta says so; the program id is the base game's)
    uint8_t cid[16];     // content id (the file name: <32 hex digits>.nca), what a meta's content records name
    bool has_cid;
    Aes key;
    int key_state;  // 0 not looked up, 1 ok, -1 missing
} Nca;

struct NxPkg {
    Keys keys;
    Pkg *pkgs;
    int npkg;
    Nca *ncas;
    int nnca;
    TitleKey *tks;
    int ntk;
    uint64_t title;
    int base;       // nca index of the base program
    int upd[16];    // nca indexes of the updates, newest first
    int nupd;
    int dlc;        // nca index of the add-on content (-1 none)
    bool compressed;
    uint8_t *scratch, *buf;  // (CHUNK + 32 each: raw blocks being decrypted, data being written)
};

static bool rd(NxPkg *P, int pkg, uint64_t off, void *buf, size_t n) {
    FILE *f = P->pkgs[pkg].f;
    return !xseek(f, off) && fread(buf, 1, n, f) == n;
}

static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t u64(const uint8_t *p) { return u32(p) | (uint64_t)u32(p + 4) << 32; }

static bool ext_is(const char *name, const char *ext) {
    size_t a = strlen(name), b = strlen(ext);
    if (a < b) return false;
    for (size_t i = 0; i < b; i++)
        if (tolower((unsigned char)name[a - b + i]) != ext[i]) return false;
    return true;
}

static void add_ticket(NxPkg *P, const uint8_t *t, size_t n) {
    if (n < 0x2C0) return;
    uint32_t sig = (uint32_t)t[0] << 24 | (uint32_t)t[1] << 16 | (uint32_t)t[2] << 8 | t[3];
    size_t data = sig == 0x10000 || sig == 0x10003 ? 0x240 : sig == 0x10002 || sig == 0x10005 ? 0x80 : 0x140;
    if (data + 0x170 > n || t[data + 0x141] != 0) return;  // (personalized tickets: the title key is RSA-wrapped)
    P->tks = realloc(P->tks, (size_t)(P->ntk + 1) * sizeof *P->tks);
    memcpy(P->tks[P->ntk].rights, t + data + 0x160, 16);
    memcpy(P->tks[P->ntk].key, t + data + 0x40, 16);
    P->ntk++;
}

static void title_keys_file(NxPkg *P, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *s = line;
        while (isspace((unsigned char)*s)) s++;
        TitleKey tk;
        if (parse_hex(s, tk.rights, 16) != 16 || parse_hex(eq + 1, tk.key, 16) != 16) continue;
        P->tks = realloc(P->tks, (size_t)(P->ntk + 1) * sizeof *P->tks);
        P->tks[P->ntk++] = tk;
    }
    fclose(f);
}

static void add_nca(NxPkg *P, int pkg, const char *name, uint64_t off, uint64_t size) {
    if (size < 0xC00) return;
    Nca n;
    memset(&n, 0, sizeof n);
    n.pkg = pkg, n.off = off, n.size = size;
    n.has_cid = strlen(name) >= 36 && name[32] == '.' && parse_hex(name, n.cid, 16) == 16;
    if (!rd(P, pkg, off, n.h, sizeof n.h)) return;
    xts_dec(P->keys.header_key, n.h, sizeof n.h);
    if (memcmp(n.h + 0x200, "NCA3", 4)) return;  // (NCA2 / NCA0: titles from before 1.0, not these games)
    n.type = n.h[0x205];
    n.id = u64(n.h + 0x210);
    P->ncas = realloc(P->ncas, (size_t)(P->nnca + 1) * sizeof *P->ncas);
    P->ncas[P->nnca++] = n;
}

// PFS0 (.nsp) at `off` of the package
static void read_pfs0(NxPkg *P, int pkg, uint64_t off) {
    uint8_t h[16];
    if (!rd(P, pkg, off, h, 16) || memcmp(h, "PFS0", 4)) return;
    uint32_t cnt = u32(h + 4), strsz = u32(h + 8);
    if (cnt > 4096 || strsz > (1u << 20)) return;
    size_t tsz = 16 + (size_t)cnt * 0x18 + strsz;
    uint8_t *t = malloc(tsz);
    if (!t || !rd(P, pkg, off, t, tsz)) { free(t); return; }
    const char *str = (const char *)t + 16 + cnt * 0x18;
    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t *e = t + 16 + i * 0x18;
        uint64_t fo = off + tsz + u64(e), fs = u64(e + 8);
        uint32_t no = u32(e + 16);
        if (no >= strsz) continue;
        char name[256];
        snprintf(name, sizeof name, "%.*s", (int)(strsz - no), str + no);
        if (ext_is(name, ".nca")) add_nca(P, pkg, name, fo, fs);
        else if (ext_is(name, ".ncz")) P->compressed = true;
        else if (ext_is(name, ".tik") && fs <= 0x1000) {
            uint8_t tik[0x1000];
            if (rd(P, pkg, fo, tik, (size_t)fs)) add_ticket(P, tik, (size_t)fs);
        }
    }
    free(t);
}

// HFS0 (gamecard partitions): the root's "secure" partition holds the NCAs
static void read_hfs0(NxPkg *P, int pkg, uint64_t off, bool root) {
    uint8_t h[16];
    if (!rd(P, pkg, off, h, 16) || memcmp(h, "HFS0", 4)) return;
    uint32_t cnt = u32(h + 4), strsz = u32(h + 8);
    if (cnt > 4096 || strsz > (1u << 20)) return;
    size_t tsz = 16 + (size_t)cnt * 0x40 + strsz;
    uint8_t *t = malloc(tsz);
    if (!t || !rd(P, pkg, off, t, tsz)) { free(t); return; }
    const char *str = (const char *)t + 16 + cnt * 0x40;
    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t *e = t + 16 + i * 0x40;
        uint64_t fo = off + tsz + u64(e), fs = u64(e + 8);
        uint32_t no = u32(e + 16);
        if (no >= strsz) continue;
        char name[256];
        snprintf(name, sizeof name, "%.*s", (int)(strsz - no), str + no);
        if (root) { if (!strcmp(name, "secure")) read_hfs0(P, pkg, fo, false); }
        else if (ext_is(name, ".nca")) add_nca(P, pkg, name, fo, fs);
        else if (ext_is(name, ".ncz")) P->compressed = true;
    }
    free(t);
}

static void add_package(NxPkg *P, const char *path) {
    bool xci = ext_is(path, ".xci") || ext_is(path, ".xcz"), nsp = ext_is(path, ".nsp") || ext_is(path, ".nsz");
    if (!xci && !nsp) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    P->pkgs = realloc(P->pkgs, (size_t)(P->npkg + 1) * sizeof *P->pkgs);
    Pkg *k = &P->pkgs[P->npkg];
    snprintf(k->path, sizeof k->path, "%s", path);
    k->f = f;
    int i = P->npkg++;
    if (nsp) read_pfs0(P, i, 0);
    else {
        uint8_t h[0x200];
        for (uint64_t base = 0; base <= 0x1000; base += 0x1000)  // (dumps with the key area in front)
            if (rd(P, i, base, h, sizeof h) && !memcmp(h + 0x100, "HEAD", 4)) {
                read_hfs0(P, i, base + u64(h + 0x130), true);
                break;
            }
    }
}

static void scan(NxPkg *P, const char *path, int depth, bool count_only, bool *any) {
    struct stat st;
    if (stat(path, &st) != 0) return;
    if (S_ISREG(st.st_mode)) {
        if (count_only) *any |= ext_is(path, ".nsp") || ext_is(path, ".xci") || ext_is(path, ".nsz") || ext_is(path, ".xcz");
        else add_package(P, path);
        return;
    }
    if (!S_ISDIR(st.st_mode) || depth > 4) return;
    DIR *d = opendir(path);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        char p[1100];
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        scan(P, p, depth + 1, count_only, any);
    }
    closedir(d);
}

bool nxpkg_any(const char *path) {
    bool any = false;
    scan(NULL, path, 0, true, &any);
    return any;
}

// ------------------------------------------------------------------ NCA sections
static const uint8_t *fs_header(const Nca *n, int s) { return n->h + 0x400 + 0x200 * s; }
static bool sec_range(const Nca *n, int s, uint64_t *start, uint64_t *size) {
    const uint8_t *e = n->h + 0x240 + 0x10 * s;
    uint64_t a = (uint64_t)u32(e) * 0x200, b = (uint64_t)u32(e + 4) * 0x200;
    if (b <= a || b > n->size) return false;
    *start = a, *size = b - a;
    return true;
}
static uint64_t upper_iv(const Nca *n, int s) {
    const uint8_t *f = fs_header(n, s);
    return (uint64_t)u32(f + 0x144) << 32 | u32(f + 0x140);  // (secure value, generation)
}

static bool nca_key(NxPkg *P, Nca *n, char *why, size_t wn) {
    if (n->key_state) {
        if (n->key_state < 0) snprintf(why, wn, "no key for %016llx", (unsigned long long)n->id);
        return n->key_state > 0;
    }
    n->key_state = -1;
    int gen = n->h[0x206] > n->h[0x220] ? n->h[0x206] : n->h[0x220];
    int rev = gen ? gen - 1 : 0;
    static const uint8_t zero[16];
    uint8_t k[16];
    if (memcmp(n->h + 0x230, zero, 16)) {  // a rights id: the title key from a ticket
        int t = -1;
        for (int i = 0; i < P->ntk && t < 0; i++)
            if (!memcmp(P->tks[i].rights, n->h + 0x230, 16)) t = i;
        if (t < 0) { snprintf(why, wn, "no ticket / title key for %016llx", (unsigned long long)n->id); return false; }
        if (rev >= NGEN || !(P->keys.have_titlekek >> rev & 1)) {
            snprintf(why, wn, "keys too old: titlekek_%02x missing (%s)", rev, P->keys.path);
            return false;
        }
        ecb_dec(P->keys.titlekek[rev], P->tks[t].key, k, 16);
    } else {
        int kind = n->h[0x207];
        if (kind > 2 || rev >= NGEN || !(P->keys.have_kak[kind] >> rev & 1)) {
            snprintf(why, wn, "keys too old: key_area_key_%s_%02x missing (%s)", kind == 1 ? "ocean" : kind == 2 ? "system" : "application", rev,
                     P->keys.path);
            return false;
        }
        uint8_t area[64];
        ecb_dec(P->keys.kak[kind][rev], n->h + 0x300, area, 64);
        memcpy(k, area + 0x20, 16);  // (the third key: AES-CTR)
    }
    aes_key(&n->key, k);
    n->key_state = 1;
    return true;
}


// Bytes [abs, abs+len) of the NCA (abs: the offset in the NCA), AES-CTR with that upper IV (enc false: as stored).
static bool nca_read(NxPkg *P, Nca *n, uint64_t abs, uint8_t *buf, size_t len, bool enc, uint64_t upper, char *why,
                     size_t wn) {
    while (len) {
        uint64_t a0 = abs & ~15ull;
        size_t lead = (size_t)(abs - a0), take = len < CHUNK ? len : CHUNK, raw = (lead + take + 15) & ~(size_t)15;
        if (a0 + raw > n->size || !rd(P, n->pkg, n->off + a0, P->scratch, raw)) {
            snprintf(why, wn, "read error: %s", P->pkgs[n->pkg].path);
            return false;
        }
        if (enc) ctr_xor(&n->key, upper, a0 >> 4, P->scratch, raw / 16);
        memcpy(buf, P->scratch + lead, take);
        buf += take, abs += take, len -= take;
    }
    return true;
}

static bool sec_ok(const Nca *n, int s, char *why, size_t wn) {
    const uint8_t *f = fs_header(n, s);
    if (u64(f + 0x150) || u64(f + 0x180)) {  // (sparse / compressed storage tables)
        snprintf(why, wn, "a sparse or compressed section in %016llx: not supported", (unsigned long long)n->id);
        return false;
    }
    int enc = f[4];
    if (enc != 1 && enc != 3 && enc != 4) {
        snprintf(why, wn, "section encryption %d in %016llx: not supported", enc, (unsigned long long)n->id);
        return false;
    }
    return true;
}

// Section bytes [off, off+len), plain AES-CTR with the section's IV (or unencrypted).
static bool sec_read(NxPkg *P, Nca *n, int s, uint64_t off, uint8_t *buf, size_t len, char *why, size_t wn) {
    uint64_t start, size;
    if (!sec_range(n, s, &start, &size) || off + len > size) {
        snprintf(why, wn, "read past a section's end (%016llx)", (unsigned long long)n->id);
        return false;
    }
    bool enc = fs_header(n, s)[4] != 1;
    if (enc && !nca_key(P, n, why, wn)) return false;
    return nca_read(P, n, start + off, buf, len, enc, upper_iv(n, s), why, wn);
}

// The section holding the program's code (a PFS0 with main.npdm) / the RomFS (IVFC): its index, -1 none.
static int find_pfs0(NxPkg *P, Nca *n, uint64_t *pfs, uint8_t **tab, uint32_t *cnt, uint32_t *strsz, char *why, size_t wn) {
    for (int s = 0; s < 4; s++) {
        uint64_t start, size;
        const uint8_t *f = fs_header(n, s);
        if (!sec_range(n, s, &start, &size) || f[2] != 1 || f[3] != 2 || !sec_ok(n, s, why, wn)) continue;
        uint32_t layers = u32(f + 8 + 0x24);
        if (layers < 1 || layers > 5) continue;
        uint64_t off = u64(f + 8 + 0x28 + 16 * (layers - 1));
        uint8_t h[16];
        if (!sec_read(P, n, s, off, h, 16, why, wn) || memcmp(h, "PFS0", 4)) continue;
        uint32_t c = u32(h + 4), ss = u32(h + 8);
        if (c > 1024 || ss > (1u << 16)) continue;
        size_t tsz = 16 + (size_t)c * 0x18 + ss;
        uint8_t *t = malloc(tsz);
        if (!t || !sec_read(P, n, s, off, t, tsz, why, wn)) { free(t); continue; }
        *pfs = off, *tab = t, *cnt = c, *strsz = ss;
        return s;
    }
    return -1;
}
static int find_romfs(Nca *n, uint64_t *l6off, uint64_t *l6size) {
    for (int s = 0; s < 4; s++) {
        uint64_t start, size;
        const uint8_t *f = fs_header(n, s);
        if (!sec_range(n, s, &start, &size) || f[2] != 0 || f[3] != 3 || memcmp(f + 8, "IVFC", 4)) continue;
        uint32_t layers = u32(f + 8 + 0xC);
        if (layers < 2 || layers > 7) continue;
        const uint8_t *lv = f + 8 + 0x10 + 0x18 * (layers - 2);  // (the data level: the last)
        *l6off = u64(lv), *l6size = u64(lv + 8);
        return s;
    }
    return -1;
}

static bool pfs0_has(const uint8_t *t, uint32_t cnt, uint32_t strsz, const char *name) {
    const char *str = (const char *)t + 16 + cnt * 0x18;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t no = u32(t + 16 + i * 0x18 + 16);
        if (no < strsz && !strncmp(str + no, name, strsz - no)) return true;
    }
    return false;
}

static Nca *program(NxPkg *P, int upd) { return &P->ncas[upd > 0 && upd <= P->nupd ? P->upd[upd - 1] : P->base]; }

bool nxpkg_write_exefs(NxPkg *P, int upd, const char *dir, char *why, size_t wn) {
    Nca *n = program(P, upd);
    uint64_t pfs;
    uint8_t *t = NULL;
    uint32_t cnt, strsz;
    int s = find_pfs0(P, n, &pfs, &t, &cnt, &strsz, why, wn);
    if (s < 0 || !pfs0_has(t, cnt, strsz, "main.npdm")) {
        if (!why[0]) snprintf(why, wn, "no code (exefs) in %016llx", (unsigned long long)n->id);
        free(t);
        return false;
    }
    mkdir(dir, 0755);
    const char *str = (const char *)t + 16 + cnt * 0x18;
    uint64_t data = pfs + 16 + (uint64_t)cnt * 0x18 + strsz;
    bool ok = true;
    for (uint32_t i = 0; ok && i < cnt; i++) {
        const uint8_t *e = t + 16 + i * 0x18;
        uint32_t no = u32(e + 16);
        if (no >= strsz) continue;
        char name[256], path[1400];
        snprintf(name, sizeof name, "%.*s", (int)(strsz - no), str + no);
        if (!name[0] || strchr(name, '/') || strchr(name, '\\') || !strcmp(name, "..") || !strcmp(name, ".")) continue;
        snprintf(path, sizeof path, "%s/%s", dir, name);
        FILE *o = fopen(path, "wb");
        if (!o) { snprintf(why, wn, "can't write %s", path); ok = false; break; }
        for (uint64_t at = 0, sz = u64(e + 8); ok && at < sz;) {
            size_t k = sz - at < CHUNK ? (size_t)(sz - at) : CHUNK;
            ok = sec_read(P, n, s, data + u64(e) + at, P->buf, k, why, wn);
            if (ok && fwrite(P->buf, 1, k, o) != k) snprintf(why, wn, "can't write %s (disk full?)", path), ok = false;
            at += k;
        }
        if (fclose(o) && ok) snprintf(why, wn, "can't write %s", path), ok = false;
    }
    free(t);
    return ok;
}

// ------------------------------------------------------------------ RomFS (+ the update's patch)
typedef struct { uint64_t virt, phys; int idx; } Ind;           // indirect: patched offset -> base (0) / patch (1)
typedef struct { uint64_t off; uint32_t gen; bool plain; } Ctx;  // AesCtrEx: the patch data's counter generation

// A bucket tree (16 KiB nodes: the index nodes, then the entry sets) in plain AES-CTR at [toff, toff+tsize) of the
// section; the entries copied into *out (esz bytes each), *end = the offset where the last entry ends.
static bool bucket_tree(NxPkg *P, Nca *n, int s, uint64_t toff, uint64_t tsize, const uint8_t *hdr, size_t esz,
                        uint8_t **out, int *count, uint64_t *end, char *why, size_t wn) {
    *out = NULL, *count = 0, *end = 0;
    int32_t cnt = (int32_t)u32(hdr + 8);
    if (memcmp(hdr, "BKTR", 4) || cnt < 0) { snprintf(why, wn, "bad patch table"); return false; }
    if (!cnt) return true;
    const int64_t node = 0x4000, per = (node - 16) / (int64_t)esz, offs = (node - 16) / 8;
    int64_t sets = (cnt + per - 1) / per, l2 = 0;
    if (sets > offs) {
        int64_t nl2 = (sets + offs - 1) / offs;
        l2 = (sets - (offs - (nl2 - 1)) + offs - 1) / offs;
    }
    uint64_t es = (uint64_t)(1 + l2) * node;
    if (es + (uint64_t)sets * node > tsize) { snprintf(why, wn, "bad patch table size"); return false; }
    uint8_t *t = malloc((size_t)sets * node), *o = malloc((size_t)cnt * esz);
    bool ok = t && o && sec_read(P, n, s, toff + es, t, (size_t)sets * node, why, wn);
    int got = 0;
    for (int64_t i = 0; ok && i < sets; i++) {
        const uint8_t *h = t + i * node;
        int32_t c = (int32_t)u32(h + 4);
        if (c < 0 || c > per || got + c > cnt) { snprintf(why, wn, "bad patch table entry set"); ok = false; break; }
        memcpy(o + (size_t)got * esz, h + 16, (size_t)c * esz);
        got += c;
        *end = u64(h + 8);
    }
    free(t);
    if (!ok || got != cnt) { free(o); if (ok) snprintf(why, wn, "patch table short"); return false; }
    *out = o, *count = cnt;
    return true;
}

typedef struct {
    FILE *o;
    uint64_t done, total;
    void (*progress)(uint64_t, uint64_t);
} Out;

static bool out_write(Out *w, const uint8_t *b, size_t n, char *why, size_t wn) {
    if (fwrite(b, 1, n, w->o) != n) { snprintf(why, wn, "can't write the RomFS image (disk full?)"); return false; }
    w->done += n;
    if (w->progress) w->progress(w->done, w->total);
    return true;
}

// Section bytes [off, off+len) of a plain section into the output.
static bool copy_sec(NxPkg *P, Nca *n, int s, uint64_t off, uint64_t len, Out *w, char *why, size_t wn) {
    while (len) {
        size_t k = len < CHUNK ? (size_t)len : CHUNK;
        if (!sec_read(P, n, s, off, P->buf, k, why, wn) || !out_write(w, P->buf, k, why, wn)) return false;
        off += k, len -= k;
    }
    return true;
}

static FILE *open_out(const char *path, char *why, size_t wn) {
    FILE *o = fopen(path, "wb");
    if (!o) snprintf(why, wn, "can't create %s", path);
    else setvbuf(o, NULL, _IOFBF, 1 << 20);
    return o;
}
static bool close_out(FILE *o, bool ok, const char *path, char *why, size_t wn) {
    if (fclose(o) && ok) snprintf(why, wn, "can't write %s (disk full?)", path), ok = false;
    if (!ok) remove(path);
    return ok;
}

bool nxpkg_write_romfs(NxPkg *P, int upd, const char *path, void (*progress)(uint64_t, uint64_t), char *why, size_t wn) {
    Nca *b = &P->ncas[P->base], *u = upd > 0 && upd <= P->nupd ? &P->ncas[P->upd[upd - 1]] : NULL;
    uint64_t boff, bsize, uoff = 0, usize = 0;
    int bs = find_romfs(b, &boff, &bsize), us = u ? find_romfs(u, &uoff, &usize) : -1;
    if (bs < 0 || !sec_ok(b, bs, why, wn)) { if (!why[0]) snprintf(why, wn, "no RomFS in the base game"); return false; }
    const uint8_t *uf = us >= 0 ? fs_header(u, us) : NULL;
    if (uf && uf[4] != 4 && !u64(uf + 0x108)) b = u, bs = us, boff = uoff, bsize = usize, uf = NULL;  // (a whole RomFS)
    FILE *o = open_out(path, why, wn);
    if (!o) return false;
    Out w = {o, 0, uf ? usize : bsize, progress};
    if (!uf) return close_out(o, copy_sec(P, b, bs, boff, bsize, &w, why, wn), path, why, wn);

    // the update's patch: the indirect table over (base section, patch data) and the patch data's counters
    uint8_t *ie = NULL, *ce = NULL;
    int ni = 0, nc = 0;
    uint64_t iend, cend, ustart, usz;
    bool ok = sec_range(u, us, &ustart, &usz) && nca_key(P, u, why, wn) &&
              bucket_tree(P, u, us, u64(uf + 0x100), u64(uf + 0x108), uf + 0x110, 0x14, &ie, &ni, &iend, why, wn) &&
              bucket_tree(P, u, us, u64(uf + 0x120), u64(uf + 0x128), uf + 0x130, 0x10, &ce, &nc, &cend, why, wn);
    Ind *ind = ok ? calloc((size_t)ni + 1, sizeof *ind) : NULL;
    Ctx *cx = ok ? calloc((size_t)nc + 1, sizeof *cx) : NULL;
    if (ok && (!ind || !cx)) snprintf(why, wn, "out of memory"), ok = false;
    for (int i = 0; ok && i < ni; i++) {
        const uint8_t *e = ie + 0x14 * i;
        ind[i] = (Ind){u64(e), u64(e + 8), (int)u32(e + 16)};
        if (ind[i].idx > 1 || (i && ind[i].virt <= ind[i - 1].virt)) snprintf(why, wn, "bad patch table"), ok = false;
    }
    for (int i = 0; ok && i < nc; i++) {
        const uint8_t *e = ce + 0x10 * i;
        cx[i] = (Ctx){u64(e), u32(e + 12), e[8] == 1};
    }
    if (ok && (!ni || ind[0].virt > uoff || iend < uoff + usize)) snprintf(why, wn, "the patch doesn't cover the RomFS"), ok = false;
    uint64_t sv = upper_iv(u, us) & ~0xFFFFFFFFull;
    int ci = 0;
    for (int i = 0; ok && i < ni; i++) {
        uint64_t v0 = ind[i].virt, v1 = i + 1 < ni ? ind[i + 1].virt : iend;
        uint64_t a = v0 > uoff ? v0 : uoff, z = v1 < uoff + usize ? v1 : uoff + usize;
        if (a >= z) continue;
        uint64_t p = ind[i].phys + (a - v0), len = z - a;
        if (ind[i].idx == 0) { ok = copy_sec(P, b, bs, p, len, &w, why, wn); continue; }
        while (ok && len) {  // patch data: split where the counter generation changes
            if (ci >= nc || cx[ci].off > p) ci = 0;
            while (ci + 1 < nc && cx[ci + 1].off <= p) ci++;
            if (!nc || cx[ci].off > p || p >= cend) { snprintf(why, wn, "patch data outside its counter table"); ok = false; break; }
            uint64_t lim = ci + 1 < nc ? cx[ci + 1].off : cend;
            size_t k = (size_t)(len < lim - p ? len : lim - p);
            if (k > CHUNK) k = CHUNK;
            ok = p + k <= usz && nca_read(P, u, ustart + p, P->buf, k, !cx[ci].plain, sv | cx[ci].gen, why, wn) &&
                 out_write(&w, P->buf, k, why, wn);
            p += k, len -= k;
        }
    }
    if (ok && w.done != usize) snprintf(why, wn, "patched RomFS size mismatch"), ok = false;
    free(ie), free(ce), free(ind), free(cx);
    return close_out(o, ok, path, why, wn);
}

bool nxpkg_write_dlc(NxPkg *P, const char *path, char *why, size_t wn) {
    if (P->dlc < 0) { snprintf(why, wn, "no add-on content"); return false; }
    Nca *n = &P->ncas[P->dlc];
    uint64_t off, size;
    int s = find_romfs(n, &off, &size);
    if (s < 0 || !sec_ok(n, s, why, wn) || fs_header(n, s)[4] == 4) { if (!why[0]) snprintf(why, wn, "no RomFS in the add-on content"); return false; }
    FILE *o = open_out(path, why, wn);
    if (!o) return false;
    Out w = {o, 0, size, NULL};
    return close_out(o, copy_sec(P, n, s, off, size, &w, why, wn), path, why, wn);
}

// ------------------------------------------------------------------ opening
uint64_t nxpkg_title(const NxPkg *P) { return P->title; }
int nxpkg_update_count(const NxPkg *P) { return P->nupd; }
uint32_t nxpkg_update_version(const NxPkg *P, int upd) { return upd > 0 && upd <= P->nupd ? P->ncas[P->upd[upd - 1]].version : 0; }

void nxpkg_close(NxPkg *P) {
    if (!P) return;
    for (int i = 0; i < P->npkg; i++) fclose(P->pkgs[i].f);
    free(P->pkgs), free(P->ncas), free(P->tks), free(P->scratch), free(P->buf), free(P);
}

// The packages' meta NCAs (a .cnmt in a PFS0: title id, version, type - 0x81 = an update, its title id = the base
// game's | 0x800 - and content records naming the NCAs by content id): version and kind onto the programs they name
// (or, with renamed files, the package's one program of that id). A package without one: an update's RomFS has the
// patch tables.
static void read_versions(NxPkg *P) {
    for (int i = 0; i < P->nnca; i++) {
        Nca *n = &P->ncas[i];
        uint64_t off, size;
        int s = n->type == 0 ? find_romfs(n, &off, &size) : -1;
        n->patch = s >= 0 && u64(fs_header(n, s) + 0x108);
    }
    for (int i = 0; i < P->nnca; i++) {
        Nca *m = &P->ncas[i];
        char why[256] = "";
        uint64_t pfs;
        uint8_t *t = NULL;
        uint32_t cnt, strsz;
        if (m->type != 1) continue;
        int s = find_pfs0(P, m, &pfs, &t, &cnt, &strsz, why, sizeof why);
        if (s >= 0 && cnt) {
            uint8_t h[0x1000];
            const uint8_t *e = t + 16;
            size_t sz = u64(e + 8) < sizeof h ? (size_t)u64(e + 8) : sizeof h;
            if (sz >= 0x20 && sec_read(P, m, s, pfs + 16 + (uint64_t)cnt * 0x18 + strsz + u64(e), h, sz, why, sizeof why) &&
                (h[12] == 0x80 || h[12] == 0x81)) {
                size_t rec = 0x20 + (h[0xE] | h[0xF] << 8);
                int named = 0, nrec = h[0x10] | h[0x11] << 8;
                for (int r = 0; r < nrec && rec + 0x38 * (size_t)(r + 1) <= sz; r++) {
                    const uint8_t *c = h + rec + 0x38 * r;
                    if (c[0x36] != 1) continue;  // (program content)
                    for (int j = 0; j < P->nnca; j++) {
                        Nca *n = &P->ncas[j];
                        if (n->type == 0 && n->has_cid && !memcmp(n->cid, c + 0x20, 16))
                            n->version = u32(h + 8), n->patch = h[12] == 0x81, named++;
                    }
                }
                int same = 0, last = -1;
                for (int j = 0; !named && j < P->nnca; j++)
                    if (P->ncas[j].pkg == m->pkg && P->ncas[j].type == 0 && P->ncas[j].id == (u64(h) & ~0x800ull)) same++, last = j;
                if (same == 1) P->ncas[last].version = u32(h + 8), P->ncas[last].patch = h[12] == 0x81;
            }
        }
        free(t);
    }
}

bool nxpkg_find_keys(const char *near, char *out, size_t n) {
    char folder[1024] = ".";
    struct stat st;
    if (near && near[0]) {
        snprintf(folder, sizeof folder, "%s", near);
        if (stat(near, &st) == 0 && S_ISREG(st.st_mode)) {
            char *sl = strrchr(folder, '/');
            if (sl) *sl = 0; else snprintf(folder, sizeof folder, ".");
        }
    }
    Keys *k = calloc(1, sizeof *k);
    bool ok = k && keys_find(k, folder);
    if (ok) snprintf(out, n, "%s", k->path);
    free(k);
    return ok;
}

NxPkg *nxpkg_open(const char *path, const uint64_t *titles, int *err, char *why, size_t wn) {
    const char *paths[1] = {path};
    return nxpkg_open_list(paths, 1, NULL, titles, err, why, wn);
}

// A keys file picked by the player: read as is (no search). False: unreadable, or no usable header key in it.
static bool keys_load(Keys *k, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    Keys t;
    memset(&t, 0, sizeof t);
    keys_file(&t, path);
    keys_derive(&t);
    if (!t.have_header) return false;
    *k = t;
    snprintf(k->path, sizeof k->path, "%s", path);
    return true;
}

NxPkg *nxpkg_open_list(const char *const *paths, int npaths, const char *keys, const uint64_t *titles, int *err, char *why,
                       size_t wn) {
    why[0] = 0;
    NxPkg *P = calloc(1, sizeof *P);
    P->base = P->dlc = -1;
    P->scratch = malloc(CHUNK + 32), P->buf = malloc(CHUNK + 32);
    char folder[1024] = ".";
    struct stat st;
    if (npaths > 0 && paths[0] && paths[0][0]) {
        snprintf(folder, sizeof folder, "%s", paths[0]);
        if (stat(paths[0], &st) == 0 && S_ISREG(st.st_mode)) {
            char *sl = strrchr(folder, '/');
            if (sl) *sl = 0; else snprintf(folder, sizeof folder, ".");
        }
    }
    if (!P->scratch || !P->buf) { *err = NXPKG_READ_ERROR, snprintf(why, wn, "out of memory"); nxpkg_close(P); return NULL; }
    if (keys && keys[0] ? !keys_load(&P->keys, keys) : !keys_find(&P->keys, folder)) {
        *err = NXPKG_NO_KEYS;
        snprintf(why, wn, keys && keys[0] ? "the keys file picked has no usable header_key" : "no prod.keys (with header_key) found");
        nxpkg_close(P);
        return NULL;
    }
    LOG("[nxpkg] keys: %s\n", P->keys.path);
    char tk[1100];
    snprintf(tk, sizeof tk, "%s", P->keys.path);
    char *sl = strrchr(tk, '/');
    snprintf(sl ? sl + 1 : tk, sizeof tk - (size_t)(sl ? sl + 1 - tk : 0), "title.keys");
    title_keys_file(P, tk);
    for (int i = 0; i < npaths; i++) {  // (each package once: the same file picked twice is read once)
        if (!paths[i] || !paths[i][0]) continue;
        bool dup = false;
        for (int j = 0; j < i; j++) dup |= paths[j] && !strcmp(paths[j], paths[i]);
        if (!dup) scan(P, paths[i], 0, false, NULL);
    }
    read_versions(P);
    for (int i = 0; i < P->nnca; i++)
        if (P->ncas[i].type == 0 || P->ncas[i].type >= 4)
            LOG("[nxpkg] %s: %016llx %s v%u\n", P->pkgs[P->ncas[i].pkg].path, (unsigned long long)P->ncas[i].id,
                P->ncas[i].type ? "add-on content" : P->ncas[i].patch ? "update" : "base game", P->ncas[i].version);
    for (const uint64_t *t = titles; *t && P->base < 0; t++)
        for (int i = 0; i < P->nnca && P->base < 0; i++)
            if (P->ncas[i].type == 0 && !P->ncas[i].patch && P->ncas[i].id == *t) P->base = i, P->title = *t;
    if (P->base < 0) {
        bool upd_only = false;
        for (const uint64_t *t = titles; *t; t++)
            for (int i = 0; i < P->nnca; i++) upd_only |= P->ncas[i].type == 0 && P->ncas[i].id == *t;
        *err = P->compressed && !upd_only ? NXPKG_COMPRESSED : upd_only ? NXPKG_ONLY_UPDATE : NXPKG_NO_GAME;
        snprintf(why, wn, P->compressed ? "compressed packages (.nsz / .xcz) aren't read" : upd_only ? "only the update found, not the base game"
                          : P->npkg ? "the base game isn't in those packages" : "no packages found");
        nxpkg_close(P);
        return NULL;
    }
    for (int i = 0; i < P->nnca; i++) {
        Nca *n = &P->ncas[i];
        if (n->type == 0 && n->patch && n->id == P->title && P->nupd < 16) {
            int k = P->nupd++;  // (newest first)
            while (k > 0 && P->ncas[P->upd[k - 1]].version < n->version) P->upd[k] = P->upd[k - 1], k--;
            P->upd[k] = i;
        }
        if ((n->type == 4 || n->type == 5) && (n->id & ~0xFFFull) == (P->title & ~0xFFFull) + 0x1000 && P->dlc < 0) P->dlc = i;
    }
    for (int i = -1; i < P->nupd; i++)
        if (!nca_key(P, &P->ncas[i < 0 ? P->base : P->upd[i]], why, wn)) {
            *err = NXPKG_KEY_MISSING;
            nxpkg_close(P);
            return NULL;
        }
    if (P->dlc >= 0 && !nca_key(P, &P->ncas[P->dlc], why, wn)) LOG("[nxpkg] add-on content skipped: %s\n", why), P->dlc = -1, why[0] = 0;
    *err = NXPKG_OK;
    return P;
}
