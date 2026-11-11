// Game file setup (runtime/gamesetup.h). The picked folder is searched a few levels deep (dump tools nest the files):
// exefs/ is the folder with main and main.npdm, the RomFS either a romfs.bin image or the folder with bin/message (arc/data.trpfs in Z-A).
// exefs/ is copied into <data root>/extracted (hard links where the file system allows), the RomFS image is copied
// or built (runtime/romfs_build.c), then the game code is recompiled from exefs/ (runtime/gamebuild.c).
// No extracted files but .nsp / .xci: read out of those instead (runtime/nxpkg.c), the update picked that makes a
// supported build (2.0.2 for Z-A), the exefs checked before the old game files are replaced.
#include "gamesetup.h"

#include <SDL3/SDL.h>
#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gamebuild.h"
#include "gamelib.h"
#include "hle.h"
#include "nxpkg.h"
#include "paths.h"
#include "platform.h"
#include "romfs_build.h"

#define ROOT "extracted"  // (in the data root, the working directory)
// The two games (program ids, main.npdm's ACI0), any version. The 60 fps and wild battles options and the generator's
// instruction patches are addresses in the main: only the builds with address tables get them (game_build).
#define TITLE_EEVEE 0x0100187003A36000ull
#define TITLE_PIKACHU 0x010003F003A34000ull
#define TITLE_PLA 0x01001F5010DFA000ull  // Pokemon Legends: Arceus
#define TITLE_ZA 0x0100F43008C44000ull   // Pokemon Legends: Z-A
static const uint8_t EEVEE_100[8] = {0x0a, 0x3f, 0x0d, 0x21, 0xe6, 0x7d, 0xad, 0x80};    // Let's Go, Eevee! v1.0.0 main
static const uint8_t PIKACHU_100[8] = {0x1c, 0x37, 0x56, 0x92, 0xde, 0x4f, 0x4b, 0x87};  // Let's Go, Pikachu! v1.0.0
// Legends Z-A 2.0.2 (base game 1.0.0 + the 2.0.2 update, patch v6): the build the runtime's Z-A addresses are written
// for. Any other (2.0.0: main 8A1C86C437394B69, the base game alone...) isn't played: the start screen asks for 2.0.2
// (game_za_unsupported).
static const uint8_t ZA_202[8] = {0xb1, 0xf1, 0x2f, 0xd9, 0x19, 0xea, 0xe8, 0x6a};

static _Atomic int g_state;
static _Atomic float g_progress;
static _Atomic bool g_running;
static char g_folder[1024];
// Packages picked one by one on the Game Files page (setup_start_files): base game, update, add-on content, keys
static char g_files[3][1024], g_keys[1024];
static bool g_files_mode;

int setup_state(void) { return g_state; }
float setup_progress(void) { return g_progress; }
bool setup_running(void) { return g_running; }

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
static bool is_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static uint64_t program_id(const char *exefs) {
    char p[1100];
    snprintf(p, sizeof p, "%s/main.npdm", exefs);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    uint64_t id = 0;
    uint8_t meta[0x80], aci[0x18];  // ACI0 (offset at META+0x70): the program id at +0x10
    if (fread(meta, 1, sizeof meta, f) == sizeof meta && !memcmp(meta, "META", 4) &&
        !fseek(f, *(uint32_t *)(meta + 0x70), SEEK_SET) && fread(aci, 1, sizeof aci, f) == sizeof aci &&
        !memcmp(aci, "ACI0", 4))
        id = *(uint64_t *)(aci + 0x10);
    fclose(f);
    return id;
}

static bool game_ok(const char *exefs) {
    uint64_t id = program_id(exefs);
    return id == TITLE_EEVEE || id == TITLE_PIKACHU || id == TITLE_PLA || id == TITLE_ZA;
}

int game_build(const char *exefs) {
    char p[1100];
    snprintf(p, sizeof p, "%s/main", exefs);
    FILE *f = fopen(p, "rb");
    if (!f) return GAME_BUILD_OTHER;
    uint8_t id[8] = {0};
    fseek(f, 0x40, SEEK_SET);  // (NSO header: the module id)
    size_t n = fread(id, 1, 8, f);
    fclose(f);
    uint64_t pid = program_id(exefs);
    if (n == 8 && !memcmp(id, EEVEE_100, 8) && pid == TITLE_EEVEE) return GAME_BUILD_EEVEE_100;
    if (n == 8 && !memcmp(id, PIKACHU_100, 8) && pid == TITLE_PIKACHU) return GAME_BUILD_PIKACHU_100;
    if (n == 8 && !memcmp(id, ZA_202, 8) && pid == TITLE_ZA) return GAME_BUILD_ZA_202;
    return GAME_BUILD_OTHER;
}

bool game_za_unsupported(const char *exefs) {
    return program_id(exefs) == TITLE_ZA && game_build(exefs) != GAME_BUILD_ZA_202;
}

const char *game_version(const char *exefs) {
    static const struct { uint8_t id[8]; const char *v; } k[] = {
        {{0x0a, 0x3f, 0x0d, 0x21, 0xe6, 0x7d, 0xad, 0x80}, "1.0.0"},  // Eevee
        {{0x1c, 0x37, 0x56, 0x92, 0xde, 0x4f, 0x4b, 0x87}, "1.0.0"},  // Pikachu
        {{0xc2, 0x08, 0xdb, 0x6a, 0x4e, 0xf4, 0x36, 0x1f}, "1.0.2"},  // Pikachu (issue #1's log)
    };
    char p[1100];
    snprintf(p, sizeof p, "%s/main", exefs);
    uint8_t id[8] = {0};
    FILE *f = fopen(p, "rb");
    if (f) {
        fseek(f, 0x40, SEEK_SET);
        if (fread(id, 1, 8, f) != 8) memset(id, 0, 8);
        fclose(f);
    }
    for (size_t i = 0; i < sizeof k / sizeof *k; i++)
        if (!memcmp(id, k[i].id, 8)) return k[i].v;
    return "1.0.0";  // (a build not listed here)
}

uint64_t game_title_id(void) { return program_id(ROOT "/exefs"); }  // (the extracted game's program id)

const char *game_name(const char *exefs) {
    uint64_t id = program_id(exefs);
    return id == TITLE_EEVEE ? "Eevee" : id == TITLE_PIKACHU ? "Pikachu" : id == TITLE_PLA ? "Legends: Arceus" : id == TITLE_ZA ? "Legends: Z-A" : NULL;
}

bool setup_ready(void) {
    return game_ok(ROOT "/exefs") && !game_za_unsupported(ROOT "/exefs") && is_file(ROOT "/romfs.bin") && gamelib_present(ROOT);
}

typedef struct { char exefs[1024], romfs[1024], romfs_bin[1024]; bool package; } Found;

static void find_game(const char *dir, int depth, Found *f) {
    if (depth > 5 || (f->exefs[0] && (f->romfs[0] || f->romfs_bin[0]))) return;
    char p[1100];
    snprintf(p, sizeof p, "%s/main.npdm", dir);
    if (!f->exefs[0] && is_file(p)) {
        snprintf(p, sizeof p, "%s/main", dir);
        if (is_file(p)) snprintf(f->exefs, sizeof f->exefs, "%s", dir);
    }
    snprintf(p, sizeof p, "%s/bin/message", dir);  // (Let's Go; the Legends games: bin/archive (Arceus), arc/data.trpfs (Z-A))
    if (!f->romfs[0] && is_dir(p)) snprintf(f->romfs, sizeof f->romfs, "%s", dir);
    snprintf(p, sizeof p, "%s/bin/archive", dir);
    if (!f->romfs[0] && is_dir(p)) snprintf(f->romfs, sizeof f->romfs, "%s", dir);
    snprintf(p, sizeof p, "%s/arc/data.trpfs", dir);
    if (!f->romfs[0] && is_file(p)) snprintf(f->romfs, sizeof f->romfs, "%s", dir);
    snprintf(p, sizeof p, "%s/romfs.bin", dir);
    if (!f->romfs_bin[0] && is_file(p)) snprintf(f->romfs_bin, sizeof f->romfs_bin, "%s", p);
    DIR *d = opendir(dir);
    if (!d) return;
    char sub[64][256];
    int nsub = 0;
    for (struct dirent *e; (e = readdir(d));) {
        const char *n = e->d_name;
        if (n[0] == '.') continue;
        size_t len = strlen(n);
        if (len > 4) {
            char ext[5];
            for (int i = 0; i < 4; i++) ext[i] = (char)tolower((unsigned char)n[len - 4 + i]);
            ext[4] = 0;
            if (!strcmp(ext, ".xci") || !strcmp(ext, ".nsp") || !strcmp(ext, ".nca") || !strcmp(ext, ".nsz") ||
                !strcmp(ext, ".xcz"))
                f->package = true;
        }
        snprintf(p, sizeof p, "%s/%s", dir, n);
        if (nsub < 64 && strcmp(n, "bin") && is_dir(p)) snprintf(sub[nsub++], sizeof sub[0], "%s", n);
    }
    closedir(d);
    for (int i = 0; i < nsub; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, sub[i]);
        find_game(p, depth + 1, f);
    }
}

// progress: copying + the image are the first 15%, the game code the rest
static float g_phase0, g_phase1;
static void copy_progress(uint64_t done, uint64_t total) {
    if (total) g_progress = g_phase0 + (g_phase1 - g_phase0) * (float)((double)done / (double)total);
}
static void build_progress(double f, const char *step) {
    g_progress = 0.15f + 0.85f * (float)f;
    g_state = !strncmp(step, "Reading", 7) ? SETUP_READING : !strncmp(step, "Finishing", 9) ? SETUP_FINISHING
                                                                                           : SETUP_COMPILING;
}

// From the player's packages: the exefs of each update found (newest first, then the base game alone) is unpacked
// into a scratch folder until one is a supported build; then the old game files go and that exefs, the RomFS image
// (base + that update) and the add-on content are written into extracted/.
#define UNPACK_TMP "extracted.unpack"
static bool from_packages(void) {
    static const uint64_t titles[] = {TITLE_ZA, TITLE_PLA, TITLE_EEVEE, TITLE_PIKACHU, 0};
    int err;
    char why[512] = "";
    g_state = SETUP_UNPACKING;
    NxPkg *p;
    if (g_files_mode) {
        const char *list[3] = {g_files[0], g_files[1], g_files[2]};
        LOG("[setup] base game %s, update %s, add-on content %s, keys %s\n", g_files[0][0] ? g_files[0] : "-",
            g_files[1][0] ? g_files[1] : "-", g_files[2][0] ? g_files[2] : "-", g_keys[0] ? g_keys : "(searched)");
        p = nxpkg_open_list(list, 3, g_keys, titles, &err, why, sizeof why);
    } else
        p = nxpkg_open(g_folder, titles, &err, why, sizeof why);
    if (!p) {
        LOG("[setup] packages in %s: %s\n", g_files_mode ? g_files[0] : g_folder, why);
        g_state = err == NXPKG_NO_KEYS ? SETUP_FAIL_KEYS : err == NXPKG_KEY_MISSING ? SETUP_FAIL_KEYS_OLD
                : err == NXPKG_ONLY_UPDATE ? SETUP_FAIL_NO_BASE : err == NXPKG_COMPRESSED ? SETUP_FAIL_PACKAGE
                : err == NXPKG_NO_GAME ? SETUP_FAIL_VERSION : SETUP_FAIL_UNPACK;
        return false;
    }
    int pick = -1, n = nxpkg_update_count(p), order[17], no = 0;
    bool za = nxpkg_title(p) == TITLE_ZA;
    for (int u = 1; u <= n && no < 16; u++) order[no++] = u;
    order[no++] = 0;
    for (int i = 0; pick < 0 && i < no; i++) {
        int u = order[i];
        plat_remove_tree(UNPACK_TMP);
        mkdir(UNPACK_TMP, 0755);
        if (!nxpkg_write_exefs(p, u, UNPACK_TMP "/exefs", why, sizeof why)) {
            LOG("[setup] exefs: %s\n", why);
            g_state = SETUP_FAIL_UNPACK;
            break;
        }
        bool good = game_ok(UNPACK_TMP "/exefs") && !game_za_unsupported(UNPACK_TMP "/exefs");
        LOG("[setup] %s v%u: %s\n", u ? "update" : "base game", nxpkg_update_version(p, u), good ? "supported" : "not this build");
        if (good) pick = u;
    }
    if (pick < 0) {
        if (g_state == SETUP_UNPACKING) g_state = za ? SETUP_FAIL_ZA_VERSION : SETUP_FAIL_VERSION;
        plat_remove_tree(UNPACK_TMP);
        nxpkg_close(p);
        return false;
    }
    rt_remove_game_files(ROOT);  // (an earlier or interrupted setup's)
    mkdir(ROOT, 0755);
    bool ok = rename(UNPACK_TMP "/exefs", ROOT "/exefs") == 0;
    plat_remove_tree(UNPACK_TMP);
    g_phase0 = 0, g_phase1 = 0.15f;
    why[0] = 0;
    bool read_err = false;
    if (ok && !nxpkg_write_romfs(p, pick, ROOT "/romfs.bin", copy_progress, why, sizeof why)) {
        LOG("[setup] romfs image: %s\n", why);
        ok = false, read_err = !strstr(why, "can't");  // (else: the image couldn't be written - disk space)
    }
    if (ok && nxpkg_write_dlc(p, ROOT "/dlc.bin", why, sizeof why)) LOG("[setup] add-on content written\n");
    nxpkg_close(p);
    if (!ok) g_state = read_err ? SETUP_FAIL_UNPACK : SETUP_FAIL_COPY;
    return ok;
}

static bool build_code(void) {
    if (!gamebuild_available()) { g_state = SETUP_FAIL_TOOLS; return false; }
    int cores = SDL_GetNumLogicalCPUCores(), ram_gb = SDL_GetSystemRAM() / 1024;
    GameBuildOpts o = {build_progress, cores < ram_gb - 2 ? cores : ram_gb - 2 > 1 ? ram_gb - 2 : 1};
    char why[256] = "";
    g_state = SETUP_READING;
    if (!gamebuild_run(ROOT, &o, why, sizeof why)) {
        LOG("[setup] game code: %s\n", why);
        g_state = SETUP_FAIL_BUILD;
        return false;
    }
    return true;
}

// <base>/dlc (or <base>/dlc/romfs/: the add-on extracted as a whole title) when it is a folder - with the add-on's
// keyfile.dat in it when need_key (a folder found beside the game's own folder, not inside it: game/ and dlc/ side by
// side, the add-on dumped as a title of its own)
static bool dlc_dir(const char *base, bool need_key, char *out, size_t n) {
    char d[1200], k[1300];
    struct stat sb;
    snprintf(d, sizeof d, "%.1100s/dlc", base);
    snprintf(k, sizeof k, "%s/romfs", d);
    if (stat(k, &sb) == 0 && S_ISDIR(sb.st_mode)) snprintf(d, sizeof d, "%s", k);
    if (stat(d, &sb) != 0 || !S_ISDIR(sb.st_mode)) return false;
    snprintf(k, sizeof k, "%s/keyfile.dat", d);
    if (need_key && stat(k, &sb) != 0) return false;
    snprintf(out, n, "%s", d);
    return true;
}

static void *setup_thread(void *arg) {
    (void)arg;
    g_progress = 0;
    if (g_files_mode) {  // (the packages picked one by one: straight to reading them)
        if (from_packages() && build_code()) g_state = setup_ready() ? SETUP_DONE : SETUP_FAIL_BUILD;
        LOG("[setup] %s\n", g_state == SETUP_DONE ? "ready" : "failed");
        g_running = false;
        return NULL;
    }
    if (!g_folder[0]) {  // only the game code (files set up already, e.g. a new version of the app)
        if (build_code()) g_state = setup_ready() ? SETUP_DONE : SETUP_FAIL_BUILD;
        g_running = false;
        return NULL;
    }
    g_state = SETUP_LOOKING;
    Found f = {0};
    find_game(g_folder, 0, &f);
    LOG("[setup] %s: exefs %s, romfs %s, romfs.bin %s\n", g_folder, f.exefs[0] ? f.exefs : "-", f.romfs[0] ? f.romfs : "-",
        f.romfs_bin[0] ? f.romfs_bin : "-");
    if (!f.exefs[0] || (!f.romfs[0] && !f.romfs_bin[0])) {
        if (f.package || nxpkg_any(g_folder)) {
            if (from_packages() && build_code()) g_state = setup_ready() ? SETUP_DONE : SETUP_FAIL_BUILD;
            LOG("[setup] %s\n", g_state == SETUP_DONE ? "ready" : "failed");
            g_running = false;
            return NULL;
        }
        g_state = SETUP_FAIL_NOT_FOUND;
    } else if (!game_ok(f.exefs)) g_state = SETUP_FAIL_VERSION;
    else if (game_za_unsupported(f.exefs)) g_state = SETUP_FAIL_ZA_VERSION;
    else {
        // (the picked folder can't be our own copy: setting up again deletes that first)
        char a[4096], b[4096];
        mkdir(ROOT, 0755);
        if (realpath(f.exefs, a) && realpath(ROOT, b) && !strncmp(a, b, strlen(b)))
            snprintf(f.exefs, sizeof f.exefs, "%s", ""), g_state = SETUP_FAIL_NOT_FOUND;
    }
    if (g_state >= SETUP_FAIL_NOT_FOUND) { g_running = false; return NULL; }

    rt_remove_game_files(ROOT);  // (an earlier or interrupted setup's)
    mkdir(ROOT, 0755);
    g_state = SETUP_COPYING;
    g_phase0 = 0, g_phase1 = 0.01f;
    bool ok = true;
    if (!strcmp(f.exefs, f.romfs)) {  // (a flat dump, main next to bin/: only the six code files, not the whole RomFS)
        static const char *const code[] = {"main", "main.npdm", "rtld", "sdk", "subsdk0", "subsdk1"};
        mkdir(ROOT "/exefs", 0755);
        for (size_t i = 0; ok && i < sizeof code / sizeof *code; i++) {
            char src[1100], dst[1100];
            snprintf(src, sizeof src, "%s/%s", f.exefs, code[i]);
            snprintf(dst, sizeof dst, ROOT "/exefs/%s", code[i]);
            if (!is_file(src)) {  // (rtld and the subsdks are optional: Z-A has no subsdk; main, main.npdm and sdk are not)
                ok = i == 2 || i >= 4;  // (rtld, subsdk0, subsdk1)
                continue;
            }
            ok = romfs_copy_file(src, dst, copy_progress);
        }
    } else
        ok = romfs_copy_tree(f.exefs, ROOT "/exefs", copy_progress);
    if (ok && f.romfs_bin[0]) {
        g_phase0 = 0.01f, g_phase1 = 0.15f;
        ok = romfs_copy_file(f.romfs_bin, ROOT "/romfs.bin", copy_progress);
    } else if (ok) {
        g_state = SETUP_IMAGE;
        g_phase0 = 0.01f, g_phase1 = 0.15f;
        char why[256] = "";
        ok = romfs_build(f.romfs, ROOT "/romfs.bin", copy_progress, why, sizeof why);
        if (!ok) LOG("[setup] romfs image: %s\n", why);
    }
    if (ok) {  // (optional: the add-on's RomFS files - Z-A's Mega Dimension, services/fs.c - made into dlc.bin)
        char game[1100], up[1100], dlc[1200];
        const char *sl = strrchr(f.exefs, '/');
        snprintf(game, sizeof game, "%.*s", sl ? (int)(sl - f.exefs) : 1, sl ? f.exefs : ".");
        sl = strrchr(game, '/');
        snprintf(up, sizeof up, "%.*s", sl ? (int)(sl - game) : 2, sl ? game : "..");
        if (dlc_dir(game, false, dlc, sizeof dlc) || dlc_dir(g_folder, false, dlc, sizeof dlc) || dlc_dir(up, true, dlc, sizeof dlc)) {
            char why[256] = "";
            if (romfs_build(dlc, ROOT "/dlc.bin", NULL, why, sizeof why)) LOG("[setup] add-on content: %s\n", dlc);
            else LOG("[setup] add-on content %s skipped: %s\n", dlc, why);
        } else LOG("[setup] add-on content: none (no dlc folder beside exefs, in %s or beside %s)\n", g_folder, game);
    }
    if (!ok) g_state = SETUP_FAIL_COPY;
    else if (build_code()) g_state = setup_ready() ? SETUP_DONE : SETUP_FAIL_BUILD;
    LOG("[setup] %s\n", g_state == SETUP_DONE ? "ready" : "failed");
    g_running = false;
    return NULL;
}

void setup_start(const char *folder) {
    if (g_running) return;
    g_files_mode = false;
    snprintf(g_folder, sizeof g_folder, "%s", folder ? folder : "");
    for (char *c = g_folder; *c; c++) if (*c == '\\') *c = '/';
    g_running = true;
    g_state = SETUP_LOOKING;
    pthread_t t;
    pthread_create(&t, NULL, setup_thread, NULL);
    pthread_detach(t);
}

void setup_start_files(const char *base, const char *update, const char *dlc, const char *keys) {
    if (g_running) return;
    const char *in[3] = {base, update, dlc};
    for (int i = 0; i < 3; i++) {
        snprintf(g_files[i], sizeof g_files[i], "%s", in[i] ? in[i] : "");
        for (char *c = g_files[i]; *c; c++) if (*c == '\\') *c = '/';
    }
    snprintf(g_keys, sizeof g_keys, "%s", keys ? keys : "");
    for (char *c = g_keys; *c; c++) if (*c == '\\') *c = '/';
    g_folder[0] = 0;
    g_files_mode = true;
    g_running = true;
    g_state = SETUP_LOOKING;
    pthread_t t;
    pthread_create(&t, NULL, setup_thread, NULL);
    pthread_detach(t);
}
