// Z-A's Settings screen (main menu -> Help & Settings -> Settings) with the start screen's settings in it: rows added
// after the game's own three on each tab, drawn and driven by the game's code like its own.
//
// The screen (GameSettingPage, code 0x7002c7b000..0x7002c85000; layout ui/data/option/option_gamesetting_00.arc):
// three tabs, each a scrolling list (7 rows on screen, one row layout reused) of three rows. The page P keeps its state
// at [P+0x118] (tab +0xb8, cursor row +0xbc), its text view at [P+0x128]. The list asks for its row count
// (f_7002c7f778: 3 per tab), then binds each row on screen (f_7002c818c8) from its type and value (f_7002c80230:
// x0 = the type - 0 three choices, 1 two choices, 2 a bar: 5 steps 1..5 when the high word is 0, else 10, 0..10 - and
// x1 = the value) and its texts (f_7002c83760 -> per tab: the name f_7002c83d04, the choices f_7002c83da4 /
// f_7002c83ef0, a bar's number f_7002c840bc). Left / right compute the next value from the type and store it
// (f_7002c806dc -> per tab), then redraw the row (f_7002c80708); the cursor's row shows its description
// (f_7002c83a94). Past row 2 the per-tab functions stop the game (or show nothing): those six are hooked for the rows
// added here. Text: every one of those goes through the message lookup (0x7000410a30, (manager, file, label) -> text
// object: UTF-16 at +0x30, length +0x52), asked with one of the game's labels while ours waits in a queue; the text it
// returns is replaced by ours.
//
// Five choices or more don't fit the three a row shows: those rows show three at a time, moving along as the value
// does. Frame rate and resolution are read at start: a change is kept for the next start (Settings.next_*).
// Gameplay's Controls row opens the key bindings in the list's place (below: "the Controls list").
// LARECOMP_ZA_OPTIONS=0: the game's screen as it is.
//
// Research: LARECOMP_ZA_ARCDUMP=<name>: every SARC archive the game decompresses (OodleLZ_Decompress, 0x7001a9c9e0)
// whose file names include <name> is written to arcdump_<n>.sarc in the working directory.
// LARECOMP_ZA_OPTTRACE=<file of hex addresses>: while LARECOMP_LIVE's dir has a file "opttrace", each entry of those
// functions is logged ("[opttrace] <fn> from <lr> x0..x3"), at most 40 times per function per "opttrace" file.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../input_bind.h"
#include "../settings.h"

void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));
void rt_call_original(Cpu *c, uint64_t addr);
void guest_call(Cpu *c, uint64_t addr);
void menu_apply_fullscreen(void);  // runtime/ui/menu.mm


#define ZA_OODLE_DECOMPRESS 0x7001a9c9e0ull  // (comp, comp size, raw, raw size, ...) -> raw size
#define ZA_MSG_LOOKUP 0x7000410a30ull        // (manager, &file label, &label, language) -> [x8] text object
#define ZA_MSG_TEXT_VTABLE 0x7003bd7038ull
#define OPT_ROW_COUNT 0x7002c7f778ull   // (page state) -> rows of its tab
#define OPT_ROW_TYPE 0x7002c80230ull    // (page, row) -> x0 type, x1 value
#define OPT_ROW_SET 0x7002c806dcull     // (page, row, value)
#define OPT_ROW_SHOW 0x7002c80708ull    // (page, row, type, value, animate)
#define OPT_ROW_TEXTS 0x7002c83760ull   // (view, row item, tab, row, value)
#define OPT_ROW_INFO 0x7002c83a94ull    // (view, tab, row)
#define OPT_SET_NAME 0x7002c83d04ull    // (view, row item, &label)
#define OPT_SET_CHOICES2 0x7002c83da4ull  // (view, row item, &label, &label)
#define OPT_SET_CHOICES3 0x7002c83ef0ull  // (view, row item, &label, &label, &label)
#define OPT_SET_NUMBER 0x7002c840bcull  // (view, row item, number)
#define OPT_SET_INFO 0x7002c84210ull    // (view, &label)
#define OPT_TAB_REFRESH 0x7002c8059cull  // (page): the tab's rows again (count, cursor on the first)
#define UI_LIST_SELECT 0x70009eae60ull  // (list, row): the cursor there (as the tab's refresh does with 0)
#define UI_LIST_SHOW 0x70009e65d0ull    // (list, row, 0, 1): scrolled to it

// ------------------------------------------------------------------ the rows

enum { CHOICE, BAR5, BAR10 };
typedef struct {
    int tab;  // 0 Gameplay, 1 Graphics (the game's Camera tab, renamed: its three camera rows stay first), 2 Sound
    const char *name, *info;
    int kind, n;  // CHOICE: n choices
    const char *choice[6];
    int (*get)(void);
    void (*set)(int);
    void (*act)(int);  // (A on the row: runs it for the choice shown)
} Row;

static const int k_fps[] = {30, 60, 72, 120, 144, FPS_UNLIMITED},  // (72: half of 144 Hz, every frame 2 refreshes)
                  k_lines[] = {0, 540, 720, 1080, 1440, 2160};  // (540p: weak GPUs, ~half 1080p's GPU time)
static int find(const int *v, int n, int x) {
    for (int i = 0; i < n; i++) if (v[i] == x) return i;
    return 0;
}
static int get_fps(void) { return find(k_fps, 6, g_settings.next_fps ? g_settings.next_fps : g_settings.fps); }
// (120 / 144: the game's own logic was made for 30 and some of it counts frames; the row's description says so while
// one of them is chosen - shown again right after the change, settings_screen_buttons)
static const char *const k_fps_warning = "Scripted scenes play at 60 fps on their own, then this frame rate again.",
                         *const k_unl_warning = "Scripted scenes play at 60 fps and battles at up to 144 fps on their own.";
static int g_info_pending = -1;  // (a row whose description is to be drawn again)
bool legends_set_fps(int fps);  // runtime/hle/fps60.c: at once
static void set_fps(int i) {
    if (legends_set_fps(k_fps[i])) g_settings.fps = k_fps[i], g_settings.next_fps = 0;
    else g_settings.next_fps = k_fps[i] == g_settings.fps ? 0 : k_fps[i];
}
// A row's choices as offered: the frame rate's above 60 only with the start screen's Experimental settings on
static int nchoice(const void *row);
static int get_steps(void) { return g_settings.fixed_steps ? 1 : 0; }
static void set_steps(int i) { g_settings.fixed_steps = i == 1; }
static int get_res(void) { return find(k_lines, 6, g_settings.next_res_height ? g_settings.next_res_height - 1 : g_settings.res_height); }
static void set_res(int i) { g_settings.next_res_height = k_lines[i] == g_settings.res_height ? 0 : k_lines[i] + 1; }
static int get_full(void) { return g_settings.fullscreen ? 1 : 0; }
static void set_full(int i) { g_settings.fullscreen = i == 1, menu_apply_fullscreen(); }
static int get_aa(void) { return g_settings.antialias == AA_SMAA ? 0 : 1; }
static void set_aa(int i) { g_settings.antialias = i == 0 ? AA_SMAA : AA_OFF; }
static int get_prompts(void) { return g_settings.button_prompts >= 0 && g_settings.button_prompts <= 4 ? g_settings.button_prompts : 0; }
static void set_prompts(int i) { g_settings.button_prompts = i; }
static int get_fpsc(void) { return g_settings.show_fps ? 0 : 1; }
static void set_fpsc(int i) { g_settings.show_fps = i == 0; }
int input_last_was_kbm(void);  // runtime/hle/input.c
static int ctl_col(void) { return input_last_was_kbm() ? 0 : 1; }  // (the Controls list: the column A changes)
static void ctl_open(int i);
static int get_skip(void) { return g_settings.skip_start ? 1 : 0; }
static void set_skip(int i) { g_settings.skip_start = i == 1; }
static int get_mcam(void) { return g_settings.mouse_camera ? 0 : 1; }
static void set_mcam(int i) { g_settings.mouse_camera = i == 0; }
// (bars: the nearest step to what the ini has; mouse 0.025 degrees per pixel a step, dead zone 4% of the travel a step)
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int get_msens(void) { return clampi((int)(g_settings.mouse_sens / 0.025f + 0.5f) - 1, 0, 10); }
static void set_msens(int v) { g_settings.mouse_sens = 0.025f * (float)(v + 1); }
static int get_dz(void) { return clampi((int)(g_settings.stick_deadzone / 0.04f + 0.5f), 0, 10); }
static void set_dz(int v) { g_settings.stick_deadzone = 0.04f * (float)v; }
static int get_minvy(void) { return g_settings.mouse_invert_y ? 1 : 0; }
static void set_minvy(int i) { g_settings.mouse_invert_y = i == 1; }
static int get_minvx(void) { return g_settings.mouse_invert_x ? 1 : 0; }
static void set_minvx(int i) { g_settings.mouse_invert_x = i == 1; }
static int get_shl(void) { return g_settings.shader_nowait ? 1 : 0; }
static void set_shl(int i) { g_settings.shader_nowait = i; }
static int get_bg(void) { return g_settings.background >= BG_KEEP && g_settings.background <= BG_PAUSE ? g_settings.background : BG_KEEP; }
static void set_bg(int i) { g_settings.background = i; }
static int get_crowd(void) { return g_settings.crowd >= 0 && g_settings.crowd <= 3 ? g_settings.crowd : 0; }
static void set_crowd(int i) { g_settings.crowd = i; }
static int get_smooth(void) { return g_settings.shadow_smooth ? 0 : 1; }
static void set_smooth(int i) { g_settings.shadow_smooth = i == 0; }
static int get_detail(void) { return g_settings.detail >= 0 && g_settings.detail <= 3 ? g_settings.detail : 0; }
void za_set_detail(void);  // hle/za_fixes.c: at once
static void set_detail(int i) { g_settings.detail = i, za_set_detail(); }
static int get_shadows(void) { return g_settings.shadows >= 0 && g_settings.shadows <= 2 ? g_settings.shadows : 0; }
static void set_shadows(int i) { g_settings.shadows = i; }
void pkm_link_apply(void);  // (hle/pkm_link.c)
int pkm_files_get(void);
void pkm_files_set(int v), pkm_files_run(int v);
static int get_pklink(void) { return g_settings.pkhex_link >= 0 && g_settings.pkhex_link <= 2 ? g_settings.pkhex_link : 0; }
static void set_pklink(int i) { g_settings.pkhex_link = i; pkm_link_apply(); }
static int get_vol(void) { return clampi((int)(g_settings.volume * 10 + 0.5f), 0, 10); }
static void set_vol(int v) { g_settings.volume = (float)v / 10; }

static const Row k_rows[] = {  // (per tab in this order; Gameplay's first is the Controls list: settings_screen_buttons)
    {0, "Controls", "Press A to see and change the key and the controller button for each action.", CHOICE, 2,
     {"Keyboard", "Controller"}, ctl_col, ctl_open},
    {0, "Button Prompts", "Choose which button prompts show. Auto shows the ones you are playing with.", CHOICE, 5,
     {"Auto", "Switch", "Xbox", "PlayStation", "Keyboard"}, get_prompts, set_prompts},
    {0, "Stick Dead Zone", "Choose how far a controller's sticks must move before they count.", BAR10, 0, {0}, get_dz, set_dz},
    {0, "Mouse Camera", "Choose whether a click in the game lets the mouse turn the camera. Esc lets it go.",
     CHOICE, 2, {"On", "Off"}, get_mcam, set_mcam},
    {0, "Mouse Sensitivity", "Choose how far the camera turns when you move the mouse.", BAR10, 0, {0}, get_msens, set_msens},
    {0, "Mouse Vertical", "Choose which way the camera turns when you move the mouse up and down.", CHOICE, 2,
     {"Regular", "Inverted"}, get_minvy, set_minvy},
    {0, "Mouse Horizontal", "Choose which way the camera turns when you move the mouse left and right.", CHOICE, 2,
     {"Regular", "Inverted"}, get_minvx, set_minvx},
    {0, "In Background", "Choose what the game does while you are in another window.",
     CHOICE, 3, {"Keep Playing", "Mute", "Pause"}, get_bg, set_bg},
    {0, "Start Screen", "Choose whether the start screen shows. Holding Shift as the game starts shows it.",
     CHOICE, 2, {"Show", "Skip"}, get_skip, set_skip},
    {0, "PKHeX Link", "Let PKHeX's LiveHeX plugin edit your boxes as you play, from this PC or your network.",
     CHOICE, 3, {"Off", "This PC", "Network"}, get_pklink, set_pklink},
    {0, "Pokémon Files", "Press A to save your boxed Pokémon as PKHeX files, or add the ones in pokemon/import.",
     CHOICE, 2, {"Export", "Import"}, pkm_files_get, pkm_files_set, pkm_files_run},
    {1, "Frame Rate", "Choose how many frames are drawn a second.",
     CHOICE, 6, {"30 fps", "60 fps", "72 fps", "120 fps", "144 fps", "Unlimited"}, get_fps, set_fps},  // (above 60: Experimental settings, nchoice)
    {1, "Frame Timing", "Choose whether the game keeps full speed when the frame rate dips (Real Time) or slows down with it.",
     CHOICE, 2, {"Real Time", "Fixed"}, get_steps, set_steps},
    {1, "Resolution (Next Start)", "Choose the resolution the game is drawn at, from the next time you start it.",
     CHOICE, 6, {"Auto", "540p", "720p", "1080p", "1440p", "4K"}, get_res, set_res},
    {1, "Screen Mode", "Choose whether the game fills the screen or plays in a window.", CHOICE, 2, {"Window", "Fullscreen"},
     get_full, set_full},
    {1, "Anti-Aliasing", "Choose whether to smooth jagged edges (SMAA).", CHOICE, 2, {"On", "Off"}, get_aa, set_aa},
    {1, "Draw Distance (People)", "Choose how many people and Pokémon are drawn at once and how far away they appear. More costs performance.", CHOICE, 4, {"Switch", "2x", "4x", "Max"}, get_crowd, set_crowd},
    {1, "Draw Distance (Detail)", "Choose how far away trees, buildings and Pokémon keep their detail and small props and rooftop details appear. Farther costs performance.", CHOICE, 4, {"Switch", "2x", "4x", "Max"}, get_detail, set_detail},
    {1, "Shadows (Next Start)", "Choose how sharp shadows are. Higher shows the shapes of people and Pokémon.", CHOICE, 3, {"Switch", "High", "Ultra"}, get_shadows, set_shadows},
    {1, "Shadow Smoothing (Next Start)", "Choose whether shadow edges are smoothed or show the shadow map's steps.", CHOICE, 2, {"On", "Off"}, get_smooth, set_smooth},
    {1, "Shader Loading", "Wait for new things to load (may stutter), or show them a moment later.",
     CHOICE, 2, {"Wait", "Don't Wait"}, get_shl, set_shl},
    {1, "FPS Counter", "Choose whether to show the frame rate in the corner of the screen.", CHOICE, 2, {"Show", "Hide"},
     get_fpsc, set_fpsc},
    {2, "Master Volume", "Adjust the volume of all of the game's sound.", BAR10, 0, {0}, get_vol, set_vol},
};
#define NROWS (int)(sizeof k_rows / sizeof *k_rows)
static int nchoice(const void *row) {
    const Row *w = row;
    return w->n;  // (every frame rate: the guards in hle/za_fixes.c)
}
static int g_first[NROWS];  // CHOICE rows of more than 3: the first of the three shown

static int added(int tab) {
    int n = 0;
    for (int i = 0; i < NROWS; i++) n += k_rows[i].tab == tab;
    return n;
}
static int row_index(int tab, int row) {  // (row 3 on: ours) -> k_rows index, or -1
    if (row < 3) return -1;
    for (int i = 0, k = 3; i < NROWS; i++)
        if (k_rows[i].tab == tab && k++ == row) return i;
    return -1;
}
static int tab_of(uint64_t page) { return *(const int32_t *)(uintptr_t)(*(const uint64_t *)(uintptr_t)(page + 0x118) + 0xb8); }

static void keep_shown(int r, int v) {  // (the three choices shown include v)
    const Row *w = &k_rows[r];
    if (v < g_first[r]) g_first[r] = v;
    if (v > g_first[r] + 2) g_first[r] = v - 2;
    g_first[r] = clampi(g_first[r], 0, nchoice(w) - 3);
}
static uint64_t type_of(const Row *w) {
    if (w->kind == BAR5) return 2;
    if (w->kind == BAR10) return 0xa00000002ull;
    return nchoice(w) == 2 ? 0xa00000001ull : 0xa00000000ull;
}
static int shown_value(int r) {  // the value as the row shows it
    const Row *w = &k_rows[r];
    int v = w->get();
    if (w->kind != CHOICE || nchoice(w) <= 3) return v;
    keep_shown(r, v);
    return v - g_first[r];
}

// ------------------------------------------------------------------ the Controls list
// A, or a click, on the Controls row puts the bindings (runtime/hle/bindings.c) in the Gameplay tab's place: a row an
// action (the Switch input the game reads, named as its Game Controls screen does), the key on the left, the
// controller's button on the right (the game's own icon for that Switch input: it shows the controller button bound
// to it, runtime/nvn/glyphs.c). A waits for the new one from what pressed it - the keyboard (or a click) for the key,
// the controller for its button; a click on a cell changes that one (Esc cancels; one already in use swaps over). X
// puts the defaults back (the keyboard's or the controller's, by the same rule), B goes back to the Gameplay rows.

static const struct { int slot; const char *name; } k_binds[] = {
    {BIND_LS_UP, "Move Forward"}, {BIND_LS_DOWN, "Move Back"}, {BIND_LS_LEFT, "Move Left"}, {BIND_LS_RIGHT, "Move Right"},
    {BIND_WALK, "Walk (Hold)"}, {BIND_RS_UP, "Camera Up"}, {BIND_RS_DOWN, "Camera Down"}, {BIND_RS_LEFT, "Camera Left"},
    {BIND_RS_RIGHT, "Camera Right"}, {BIND_A, "Investigate / Talk"}, {BIND_B, "Dash"}, {BIND_Y, "Roll"},
    {BIND_X, "Main Menu"}, {BIND_ZL, "Focus on Target"}, {BIND_ZR, "Throw Ball / Aim"}, {BIND_L, "Reset Camera"},
    {BIND_R, "Rotate Camera (Focus)"}, {BIND_LSTICK, "Crouch / Rise"}, {BIND_RSTICK, "Mega Evolution"},
    {BIND_UP, "Send Out Pokémon"}, {BIND_DOWN, "Call Pokémon Back"}, {BIND_LEFT, "Previous Pokémon"},
    {BIND_RIGHT, "Next Pokémon"}, {BIND_PLUS, "Map"}, {BIND_MINUS, "Photo Mode"},
};
#define NBINDS (int)(sizeof k_binds / sizeof *k_binds)
static bool g_ctl;  // the Controls list is up (Gameplay tab)

static bool ctl_on(int tab) { return g_ctl && tab == 0; }
static int g_ctl_pending = -1;  // (a click on the other cell / on the Controls row: done on the next read of the pad)
bool menu_mouse_value_press(void);  // runtime/hle/menu_mouse.c
static void ctl_open(int i) { if (menu_mouse_value_press()) g_ctl_pending = 100 + i; }  // (a click on its cells)
// a binding's cell: the key's name / the game's icon for the Switch input ("{...}": runtime text tag), or waiting
static const char *ctl_cell(int row, int col, char *buf, size_t n) {
    int slot = k_binds[row].slot, dev, cslot, ms;
    if (bind_capture_active(&dev, &cslot, &ms) && cslot == slot && dev == (col ? BIND_DEV_PAD : BIND_DEV_KEY))
        return col ? "Press a button..." : "Press a key...";
    if (!col) {
        int k = g_bind_key[slot];
        snprintf(buf, n, "%s", k ? bind_key_label(k) : "-");
        return buf;
    }
    unsigned g = 0;  // (a glyph of the game's or_font, U+E31D.., which its text boxes draw)
    const char *label = NULL;
    if (slot == BIND_WALK) label = "-";
    else if (slot >= BIND_PAD_COUNT) g = slot < BIND_RS_UP ? 0xe335 : 0xe336;  // (the stick)
    else {
        int host = g_bind_pad[slot];
        label = bind_pad_label(host, bind_label_family());
        switch (host) {
        case 0: case 1: case 2: case 3:  // (face buttons: the letter the controller has; PlayStation's shapes as words)
            if (label && label[0] >= 'A' && label[0] <= 'Y' && !label[1] && strchr("ABXY", label[0]))
                g = 0xe31d + (unsigned)(strchr("ABXY", label[0]) - "ABXY");
            break;
        case 4: g = 0xe32a; break;    // back / select / minus
        case 5: g = 0xe32b; break;    // guide / home
        case 6: g = 0xe329; break;    // start / plus
        case 7: g = 0xe338; break;    // left stick press
        case 8: g = 0xe339; break;    // right stick press
        case 9: g = 0xe321; break;    // left shoulder
        case 10: g = 0xe322; break;   // right shoulder
        case 11: case 12: case 13: case 14: g = 0xe32e + (unsigned)(host - 11); break;  // d-pad up, down, left, right
        case PADB_LTRIGGER: g = 0xe323; break;
        case PADB_RTRIGGER: g = 0xe324; break;
        default: break;
        }
    }
    if (g) snprintf(buf, n, "%c%c%c", 0xe0 | (g >> 12), 0x80 | ((g >> 6) & 0x3f), 0x80 | (g & 0x3f));
    else snprintf(buf, n, "%s", label ? label : "-");
    return buf;
}

// ------------------------------------------------------------------ text

typedef struct { uint64_t hash; const char *str; uint64_t len, zero; } Label;  // (the game's message label)
static Label make_label(const char *s) {
    uint64_t h = 0xcbf29ce484222645ull;  // (FNV-1a)
    for (const char *p = s; *p; p++) h = (h ^ (uint8_t)*p) * 0x100000001b3ull;
    return (Label){h, s, strlen(s), 0};
}
static Label g_label;  // (any label of the game's: its text is replaced)

static __thread const char *t_text[4];  // the texts the next lookups give, in order
static __thread int t_ntext, t_next;

static int msg_tags_log(Cpu *c, uint64_t addr) {  // (research, LARECOMP_ZA_MSGTAGS=1: texts with tags, as hex)
    uint64_t res = c->x[8], lab = c->x[2];
    rt_call_original(c, addr);
    const uint8_t *obj = (const uint8_t *)(uintptr_t)*(const uint64_t *)(uintptr_t)res;
    if (!obj || *(const uint64_t *)obj != ZA_MSG_TEXT_VTABLE) return 1;
    const uint16_t *t = *(uint16_t *const *)(obj + 0x30);
    int n = *(const uint16_t *)(obj + 0x52), tag = 0;
    for (int i = 0; i < n; i++) tag |= t[i] == 0x0e || t[i] == 0x10;
    if (!tag) return 1;
    char buf[2048];
    int k = 0;
    for (int i = 0; i < n && i < 300 && k < (int)sizeof buf - 8; i++)
        k += t[i] >= 0x20 && t[i] < 0x7f ? snprintf(buf + k, sizeof buf - k, "%c", t[i]) : snprintf(buf + k, sizeof buf - k, "<%x>", t[i]);
    const Label *l = (const Label *)(uintptr_t)lab;
    LOG("[msgtags] %.*s: %s\n", (int)(l->len < 80 ? l->len : 80), l->str, buf);
    return 1;
}

static void set_text(uint8_t *obj, const char *s);
static int msg_lookup_hook(Cpu *c, uint64_t addr) {
    static int tags = -1;
    if (tags < 0) tags = getenv("LARECOMP_ZA_MSGTAGS") != NULL;
    {   // the Settings screen's Camera tab is Graphics here (the frame rate, resolution... rows are on it)
        static const char tab[] = "msg_option_setting_tab_02";
        const Label *l = (const Label *)(uintptr_t)c->x[2];
        if (l && l->str && l->len == sizeof tab - 1 && !memcmp(l->str, tab, sizeof tab - 1)) {
            uint64_t res = c->x[8];
            rt_call_original(c, addr);
            uint8_t *obj = (uint8_t *)(uintptr_t)*(const uint64_t *)(uintptr_t)res;
            if (obj && *(const uint64_t *)obj == ZA_MSG_TEXT_VTABLE) set_text(obj, "Graphics");
            return 1;
        }
    }
    if (t_next >= t_ntext) return tags ? msg_tags_log(c, addr) : 0;
    uint64_t res = c->x[8];
    rt_call_original(c, addr);
    const char *s = t_text[t_next++];
    uint8_t *obj = (uint8_t *)(uintptr_t)*(const uint64_t *)(uintptr_t)res;
    if (!obj || *(const uint64_t *)obj != ZA_MSG_TEXT_VTABLE) return 1;
    set_text(obj, s);
    return 1;
}

// a text object's text replaced (UTF-8 in, the game's UTF-16 out; "{Name}": a button icon)
static void set_text(uint8_t *obj, const char *s) {
    uint16_t *buf = *(uint16_t **)(obj + 0x30);
    uint64_t cap = *(const uint64_t *)(obj + 0x40);
    size_t n = 0, col = 0;
    for (const uint8_t *p = (const uint8_t *)s; *p && n + 1 < cap;) {  // (UTF-8 -> UTF-16, up to 3-byte sequences)
        if (*p == '{' && strchr((const char *)p, '}') && n + 8 < cap) {  // ("{Decide}": the game's button icon by name -
            const uint8_t *e = (const uint8_t *)strchr((const char *)p, '}');  // tag 0x10 2 0xbd0a <length>, the name;
            // names seen: Decide (A), Cancel (B), L, R, ZL, ZR, Plus, Minus, Home, Capture)
            buf[n++] = 0x10, buf[n++] = 2, buf[n++] = 0xbd0a, buf[n++] = (uint16_t)(e - p - 1);
            for (p++; p < e && n + 1 < cap; p++) buf[n++] = *p;
            p = e + 1, col += 2;
            continue;
        }
        if (*p == ' ') {  // (the text box doesn't wrap - it squeezes a long line: lines of up to 44 characters)
            size_t w = 1;
            while (p[w] && p[w] != ' ') w++;
            if (col + w > 44) { buf[n++] = '\n', col = 0, p++; continue; }
        }
        uint32_t u = *p++;
        if (u >= 0xe0 && p[0] && p[1]) u = (u & 0xf) << 12 | (p[0] & 0x3fu) << 6 | (p[1] & 0x3fu), p += 2;
        else if (u >= 0xc0 && p[0]) u = (u & 0x1f) << 6 | (p[0] & 0x3fu), p++;
        buf[n++] = (uint16_t)u, col++;
    }
    buf[n] = 0;
    *(uint16_t *)(obj + 0x52) = (uint16_t)n;
}

// the game's function fn(x0..x4) with the texts its lookups give
static void call_with_texts(Cpu *c, uint64_t fn, const uint64_t *args, int nargs, const char *const *texts, int ntexts) {
    uint64_t save[5];
    memcpy(save, c->x, sizeof save);
    for (int i = 0; i < nargs; i++) c->x[i] = args[i];
    for (int i = 0; i < ntexts; i++) t_text[i] = texts[i];
    t_ntext = ntexts, t_next = 0;
    guest_call(c, fn);
    t_ntext = t_next = 0;
    memcpy(c->x, save, sizeof save);
}

// ------------------------------------------------------------------ hooks

static int row_count_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    int tab = *(const int32_t *)(uintptr_t)(c->x[0] + 0xb8);
    c->x[0] = ctl_on(tab) ? NBINDS : tab >= 0 && tab < 3 ? 3 + (uint64_t)added(tab) : 0;
    return 1;
}

static uint64_t g_page, g_page_vt;  // (the Settings screen last drawn, its class: settings_cursor_row)

static int row_type_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    g_page = c->x[0], g_page_vt = *(const uint64_t *)(uintptr_t)c->x[0];
    if (ctl_on(tab_of(c->x[0]))) {
        c->x[0] = 0xa00000001ull, c->x[1] = (uint64_t)ctl_col();
        return 1;
    }
    int r = row_index(tab_of(c->x[0]), (int)(uint32_t)c->x[1]);
    if (r < 0) return 0;
    c->x[0] = type_of(&k_rows[r]);
    c->x[1] = (uint64_t)shown_value(r);
    return 1;
}

static int row_set_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (ctl_on(tab_of(c->x[0]))) {  // (left / right: nothing; a click on the other cell: that one waits)
        if (menu_mouse_value_press()) g_ctl_pending = (int)(uint32_t)c->x[2] & 1;
        return 1;
    }
    int r = row_index(tab_of(c->x[0]), (int)(uint32_t)c->x[1]);
    if (r < 0) return 0;
    const Row *w = &k_rows[r];
    int v = (int)(uint32_t)c->x[2];
    if (w->kind == CHOICE && nchoice(w) > 3) {  // (shown 0..2, moving on past either end: +1 or -1 of the value)
        int cur = w->get(), was = cur - g_first[r], n = nchoice(w);
        v = v == (was + 1) % 3 ? (cur + 1) % n : (cur + n - 1) % n;
        keep_shown(r, v);
    }
    if (w->get == get_fps) {
        // Changing the frame rate changes the menu's own step: the frame it takes effect, the game handles the same
        // press a second time (~35 ms later at 30 -> 60) and set it straight back - 60 couldn't be chosen in here
        // (mouse, keyboard or controller alike). A change back within 250 ms is that echo, not a press.
        static uint64_t changed_ms;
        static int before = -1;
        uint64_t now = plat_now_ns() / 1000000;
        if (v == before && now - changed_ms < 250) return 1;
        before = w->get(), changed_ms = now;
    }
    w->set(v);
    settings_save();
    if (w->get == get_fps) g_info_pending = (int)(uint32_t)c->x[1];
    return 1;
}

static int row_show_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (ctl_on(tab_of(c->x[0]))) { c->x[3] = (uint64_t)ctl_col(); return 0; }
    int r = row_index(tab_of(c->x[0]), (int)(uint32_t)c->x[1]);
    if (r >= 0) c->x[3] = (uint64_t)shown_value(r);  // (moved along: not the value the game worked out)
    return 0;
}

static int row_texts_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (ctl_on((int)(uint32_t)c->x[2])) {
        int b = (int)(uint32_t)c->x[3];
        if (b < 0 || b >= NBINDS) return 1;
        static char k[64], p[64];
        const char *cells[2] = {ctl_cell(b, 0, k, sizeof k), ctl_cell(b, 1, p, sizeof p)};
        uint64_t view = c->x[0], item = c->x[1], lab = (uint64_t)(uintptr_t)&g_label;
        call_with_texts(c, OPT_SET_NAME, (uint64_t[]){view, item, lab}, 3, &k_binds[b].name, 1);
        call_with_texts(c, OPT_SET_CHOICES2, (uint64_t[]){view, item, lab, lab}, 4, cells, 2);
        return 1;
    }
    int r = row_index((int)(uint32_t)c->x[2], (int)(uint32_t)c->x[3]);
    if (r < 0) return 0;
    const Row *w = &k_rows[r];
    uint64_t view = c->x[0], item = c->x[1], lab = (uint64_t)(uintptr_t)&g_label;
    call_with_texts(c, OPT_SET_NAME, (uint64_t[]){view, item, lab}, 3, &w->name, 1);
    if (w->kind != CHOICE) {
        call_with_texts(c, OPT_SET_NUMBER, (uint64_t[]){view, item, (uint64_t)w->get()}, 3, NULL, 0);
        return 1;
    }
    int first = nchoice(w) > 3 ? (shown_value(r), g_first[r]) : 0;
    if (nchoice(w) == 2) call_with_texts(c, OPT_SET_CHOICES2, (uint64_t[]){view, item, lab, lab}, 4, w->choice, 2);
    else call_with_texts(c, OPT_SET_CHOICES3, (uint64_t[]){view, item, lab, lab, lab}, 5, w->choice + first, 3);
    return 1;
}

static int row_info_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (ctl_on((int)(uint32_t)c->x[1])) {
        int dev, slot, ms;
        bool wait = bind_capture_active(&dev, &slot, &ms);
        const char *t = wait ? (dev == BIND_DEV_KEY ? "Press the new key, mouse button or wheel. Esc cancels; one already in use swaps over."
                                                    : "Press the new controller button. Esc cancels; one already in use swaps over.")
                             : "Select an action to change it: a key from the keyboard, a button from the controller.";
        call_with_texts(c, OPT_SET_INFO, (uint64_t[]){c->x[0], (uint64_t)(uintptr_t)&g_label}, 2, &t, 1);
        return 1;
    }
    int r = row_index((int)(uint32_t)c->x[1], (int)(uint32_t)c->x[2]);
    if (r < 0) return 0;
    const char *info = k_rows[r].get == get_fps && get_fps() >= 2 ? (get_fps() == 5 ? k_unl_warning : k_fps_warning) : k_rows[r].info;
    call_with_texts(c, OPT_SET_INFO, (uint64_t[]){c->x[0], (uint64_t)(uintptr_t)&g_label}, 2, &info, 1);
    return 1;
}

int settings_tab(void) {  // the Settings screen's tab (runtime/hle/menu_mouse.c), -1 when it isn't up
    if (!g_page || *(const uint64_t *)(uintptr_t)g_page != g_page_vt) return -1;
    return tab_of(g_page);
}

// The cursor's row on the Settings screen (runtime/hle/menu_mouse.c: a click on a choice or a bar): its type and value,
// as left / right see them. (Asked while its rows are on screen: the page is the one they were bound by.)
bool settings_cursor_row(Cpu *c, uint64_t *type, int *value) {
    if (!g_page || *(const uint64_t *)(uintptr_t)g_page != g_page_vt) return false;
    uint64_t save[31];
    memcpy(save, c->x, sizeof save);
    c->x[0] = g_page;
    c->x[1] = (uint64_t)*(const uint32_t *)(uintptr_t)(*(const uint64_t *)(uintptr_t)(g_page + 0x118) + 0xbc);
    guest_call(c, OPT_ROW_TYPE);
    *type = c->x[0], *value = (int)(uint32_t)c->x[1];
    memcpy(c->x, save, sizeof save);
    return true;
}

// The Controls list's keys, each read of the controller (runtime/hle/input.c, the game's thread): A on the Controls
// row opens it, and in it A / X / B / L / R do its things - those presses are kept from the game (until let go).
static void guest(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    uint64_t save[31];
    memcpy(save, c->x, sizeof save);
    c->x[0] = a0, c->x[1] = a1, c->x[2] = a2, c->x[3] = a3, c->x[4] = a4;
    guest_call(c, fn);
    memcpy(c->x, save, sizeof save);
}
static void ctl_redraw_row(Cpu *c, int row) {  // (the cursor's row and the description)
    guest(c, OPT_ROW_SHOW, g_page, (uint64_t)row, 0xa00000001ull, (uint64_t)ctl_col(), 0);
    guest(c, OPT_ROW_INFO, *(const uint64_t *)(uintptr_t)(g_page + 0x128), 0, (uint64_t)row, 0, 0);
}
static void ctl_set(Cpu *c, bool on) {
    g_ctl = on;
    guest(c, OPT_TAB_REFRESH, g_page, 0, 0, 0, 0);
}

// The title screen's banner (N_banner_00 under N_inout_00, in the layout with the logo's P_t_logo_01): "Downloadable
// Content Available" with MINUS, a link to the eShop (not emulated - the game stops). Hidden, and MINUS kept from the
// game while the logo is up (menu_mouse.c's pane hook). With the DLC, "Downloadable content ready" is another pane: left.
static _Atomic uint64_t g_title_frame;
static bool pane_named(uint64_t p, const char *name) {
    return p >= 0x7000000000ull && p < 0x8000000000ull && !strncmp((const char *)(uintptr_t)(p + 0xb0), name, 24);
}
static void title_pane(uint64_t p) {
    extern uint64_t renderer_game_frame(void);
    uint64_t f = renderer_game_frame();
    if (pane_named(p, "P_t_logo_01")) { atomic_store(&g_title_frame, f); return; }
    uint64_t tf = atomic_load(&g_title_frame);
    if (tf && f - tf < 3 && pane_named(p, "N_banner_00") && pane_named(*(uint64_t *)(uintptr_t)(p + 0x18), "N_inout_00"))
        *(uint8_t *)(uintptr_t)(p + 0x58) &= (uint8_t)~1;  // (flag bit 0: visible)
}

uint64_t settings_screen_buttons(Cpu *c, uint64_t b) {
    enum { A = 1 << 0, B = 1 << 1, X = 1 << 2, L = 1 << 6, R = 1 << 7 };
    static uint64_t prev, kept;
    static int serial0 = -1;
    {
        extern uint64_t renderer_game_frame(void);
        uint64_t tf = atomic_load(&g_title_frame);
        if (tf && renderer_game_frame() - tf < 10) b &= ~(1ull << 11);  // (the title screen: no MINUS - the eShop banner's)
    }
    uint64_t edge = b & ~prev;
    prev = b;
    kept &= b;
    extern bool menu_settings_up(void);
    static int gone;  // (frames the screen hasn't been seen: gone after 30 - not a frame's gap in what's drawn)
    bool live = g_page && *(const uint64_t *)(uintptr_t)g_page == g_page_vt;
    gone = live && menu_settings_up() ? 0 : gone + 1;
    if (!live || gone > 30) {
        g_ctl = false;
        return b & ~kept;
    }
    if (gone) return b & ~kept;
    const uint8_t *d = (const uint8_t *)(uintptr_t)*(const uint64_t *)(uintptr_t)(g_page + 0x118);
    int tab = *(const int32_t *)(d + 0xb8), row = *(const int32_t *)(d + 0xbc);
    int dev, slot, ms, serial;
    bind_last_result(&dev, &slot, &ms, &serial);
    if (serial0 < 0) serial0 = serial;
    if (g_ctl && bind_capture_active(&dev, &slot, &ms)) { kept |= b; return 0; }  // (the next press is the binding's)
    if (serial != serial0) {  // (a binding set, swapped or cancelled: the row again; a swap's other one as it scrolls in)
        serial0 = serial;
        settings_save();
        if (g_ctl) ctl_redraw_row(c, row);
    }
    int pending = g_ctl_pending;
    g_ctl_pending = -1;
    if (g_info_pending >= 0 && !g_ctl) {  // (the frame rate changed: its description again, with or without the warning)
        guest(c, OPT_ROW_INFO, *(const uint64_t *)(uintptr_t)(g_page + 0x128), (uint64_t)tab, (uint64_t)g_info_pending, 0, 0);
        g_info_pending = -1;
    }
    if (!g_ctl) {
        if (tab == 0 && row_index(0, row) == 0 && ((edge & A) || pending >= 100)) kept |= edge & A, ctl_set(c, true);  // (Controls)
        else if ((edge & A) && row_index(tab, row) >= 0 && k_rows[row_index(tab, row)].act) {  // (Pokémon Files)
            const Row *r = &k_rows[row_index(tab, row)];
            kept |= A, r->act(r->get());
        }
    } else if (pending >= 0 && pending < 2 && row >= 0 && row < NBINDS) {  // (a click on a cell)
        if (!(pending && k_binds[row].slot >= BIND_PAD_COUNT)) {
            bind_capture_begin(pending ? BIND_DEV_PAD : BIND_DEV_KEY, k_binds[row].slot);
            ctl_redraw_row(c, row);
        }
    } else if (edge & B) {
        kept |= B, ctl_set(c, false);
    } else if (edge & (L | R)) {
        g_ctl = false;  // (another tab: the game's own refresh)
    } else if (edge & X) {
        kept |= X, bind_reset(ctl_col() ? BIND_DEV_PAD : BIND_DEV_KEY), ctl_set(c, true);
    } else if ((edge & A) && row >= 0 && row < NBINDS) {
        kept |= A;
        int col = ctl_col();
        if (!(col && k_binds[row].slot >= BIND_PAD_COUNT)) {  // (a controller's sticks stay its sticks)
            bind_capture_begin(col ? BIND_DEV_PAD : BIND_DEV_KEY, k_binds[row].slot);
            ctl_redraw_row(c, row);
        }
    }
    return b & ~kept;
}

// ------------------------------------------------------------------ research

static bool has(const uint8_t *p, size_t n, const char *s) {
    size_t k = strlen(s);
    for (size_t i = 0; k && i + k <= n; i++)
        if (p[i] == (uint8_t)s[0] && !memcmp(p + i, s, k)) return true;
    return false;
}

// The description line sits where the list's rows 5-7 are (it was below three): moved under the list (its bottom
// at -293, the bottom line at -451), in the layout as it is decompressed. (+ research: LARECOMP_ZA_ARCDUMP)
static void move_description(uint8_t *arc, size_t n) {
    static const char name[] = "T_explanation_00";
    for (size_t i = 0x0c; i + 0x34 <= n; i++)
        if (arc[i] == 'T' && !memcmp(arc + i, name, sizeof name) && !memcmp(arc + i - 0x0c, "txt1", 4)) {
            float *y = (float *)(arc + i - 0x0c + 0x30);
            if (*y == -224.0f) *y = -372.0f;
            return;
        }
}

static int decompress_hook(Cpu *c, uint64_t addr) {
    uint64_t raw = c->x[2];
    rt_call_original(c, addr);
    int64_t n = (int64_t)c->x[0];
    if (n < 0x20 || memcmp((const void *)(uintptr_t)raw, "SARC", 4)) return 1;
    uint8_t *arc = (uint8_t *)(uintptr_t)raw;
    size_t head = (size_t)(n < 0x10000 ? n : 0x10000);  // (the file names)
    if (has(arc, head, "blyt/option_gamesetting_00.bflyt")) move_description(arc, (size_t)n);
    const char *want = getenv("LARECOMP_ZA_ARCDUMP");
    if (!want || !has(arc, head, want)) return 1;
    static int k;
    char path[64];
    snprintf(path, sizeof path, "arcdump_%d.sarc", k++);
    FILE *f = fopen(path, "wb");
    if (f) fwrite(arc, 1, (size_t)n, f), fclose(f);
    LOG("[options] %s: %lld bytes\n", path, (long long)n);
    return 1;
}

static _Atomic int g_trace_on, g_trace_gen;
static struct { uint64_t addr; int gen, n; } g_traced[1024];
static int g_ntraced;

static int trace_hook(Cpu *c, uint64_t addr) {
    if (!atomic_load(&g_trace_on)) return 0;
    for (int i = 0; i < g_ntraced; i++)
        if (g_traced[i].addr == addr) {
            int gen = atomic_load(&g_trace_gen);
            if (g_traced[i].gen != gen) g_traced[i].gen = gen, g_traced[i].n = 0;
            if (g_traced[i].n++ >= 40) return 0;
            break;
        }
    LOG("[opttrace] %llx from %llx x0 %llx x1 %llx x2 %llx x3 %llx\n", (unsigned long long)addr, (unsigned long long)c->x[30],
        (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3]);
    return 0;
}

// (each present: the live dir's "opttrace" file turns tracing on; a new one - removed and made again - restarts the counts)
void za_options_present(void) {
    { extern void za_fixes_present(void); za_fixes_present(); }
    static const char *dir;
    static bool init;
    if (!init) init = true, dir = getenv("LARECOMP_ZA_OPTTRACE") ? getenv("LARECOMP_LIVE") : NULL;
    if (!dir) return;
    char path[512];
    snprintf(path, sizeof path, "%s/opttrace", dir);
    FILE *f = fopen(path, "r");
    bool on = f != NULL;
    if (f) fclose(f);
    if (on && !atomic_load(&g_trace_on)) atomic_fetch_add(&g_trace_gen, 1);
    atomic_store(&g_trace_on, on);
}

// ------------------------------------------------------------------ the game's own settings, from the launcher
// The game's nine settings (Rumble, Text Speed, Minimap Rotation; camera Vertical / Horizontal invert and Sensitivity; Music,
// Sound Effects, Pokémon Cries) are one bit field at [cfg+0x40], bits 0..20, in the save ("main", key 0xac6dd22f): its
// config block loads it (ZA_CFG_LOAD), a new game sets the defaults (ZA_CFG_INIT, 0x1554f2) and saving writes it back
// (ZA_CFG_SAVE). ZA_CFG_APPLY pushes rumble and the volumes to the sound engine (camera and text read the field live).
// Launcher: the values the save had (Settings.game_opts, kept at each load and save) and new ones it chose
// (game_opts_next), written into the field each time the game loads the save - or starts a new one - until it saves them.
// Bits: 0 rumble off; 1-2 text speed 0..2; 3 minimap rotates; 4 / 5 vertical / horizontal camera NOT inverted;
// 6-8 camera sensitivity 1..5; 9-12 / 13-16 / 17-20 music / sound effects / cries 0..10.
#define ZA_CFG_LOAD 0x7000ba34e4ull   // (cfg): the field from the save, then applied
#define ZA_CFG_INIT 0x7000b20cd4ull   // (cfg): the defaults, applied
#define ZA_CFG_SAVE 0x7000b20f10ull   // (cfg): the field into the save
#define ZA_CFG_APPLY 0x7000b20d04ull  // (cfg): rumble and the volumes to the sound engine
#define ZA_CFG_BITS 0x1fffffull

static void cfg_seen(uint64_t cfg, bool saving) {  // (the save's values, for the launcher)
    int v = (int)(*(const uint64_t *)(uintptr_t)(cfg + 0x40) & ZA_CFG_BITS);
    bool done = saving && g_settings.game_opts_next >= 0 && v == g_settings.game_opts_next;  // (in the save now)
    if (v == g_settings.game_opts && !done) return;
    g_settings.game_opts = v;
    if (done) g_settings.game_opts_next = -1;
    settings_save();
}
static int cfg_loaded_hook(Cpu *c, uint64_t addr) {  // (ZA_CFG_LOAD and ZA_CFG_INIT, after the game's own)
    uint64_t cfg = c->x[0];
    rt_call_original(c, addr);
    uint64_t r0 = c->x[0], r1 = c->x[1];
    if (cfg && addr == ZA_CFG_LOAD) cfg_seen(cfg, false);
    int next = g_settings.game_opts_next;
    if (cfg && next >= 0) {
        uint64_t *f = (uint64_t *)(uintptr_t)(cfg + 0x40);
        LOG("[options] the game's settings from the launcher: 0x%06x (the save had 0x%06x)\n", next, (int)(*f & ZA_CFG_BITS));
        *f = (*f & ~ZA_CFG_BITS) | (uint64_t)next;
        c->x[0] = cfg;
        guest_call(c, ZA_CFG_APPLY);
    }
    c->x[0] = r0, c->x[1] = r1;
    return 1;
}
static int cfg_save_hook(Cpu *c, uint64_t addr) {
    if (c->x[0]) cfg_seen(c->x[0], true);
    (void)addr;
    return 0;  // (the game's own runs)
}

void za_options_configure(void) {
    { extern void (*g_menu_pane_cb)(uint64_t); g_menu_pane_cb = title_pane; }
    const char *e = getenv("LARECOMP_ZA_OPTIONS");
    if (!(e && atoi(e) == 0)) {
        g_label = make_label("msg_ui_option_01");
        rt_register_fnhook_at(ZA_MSG_LOOKUP, msg_lookup_hook);
        rt_register_fnhook_at(OPT_ROW_COUNT, row_count_hook);
        rt_register_fnhook_at(OPT_ROW_TYPE, row_type_hook);
        rt_register_fnhook_at(OPT_ROW_SET, row_set_hook);
        rt_register_fnhook_at(OPT_ROW_SHOW, row_show_hook);
        rt_register_fnhook_at(OPT_ROW_TEXTS, row_texts_hook);
        rt_register_fnhook_at(OPT_ROW_INFO, row_info_hook);
        rt_register_fnhook_at(ZA_OODLE_DECOMPRESS, decompress_hook);
    }
    rt_register_fnhook_at(ZA_CFG_LOAD, cfg_loaded_hook);
    rt_register_fnhook_at(ZA_CFG_INIT, cfg_loaded_hook);
    rt_register_fnhook_at(ZA_CFG_SAVE, cfg_save_hook);
    const char *tf = getenv("LARECOMP_ZA_OPTTRACE");
    FILE *f = tf ? fopen(tf, "r") : NULL;
    unsigned long long a;
    while (f && g_ntraced < 1024 && fscanf(f, "%llx", &a) == 1) g_traced[g_ntraced++].addr = a, rt_register_fnhook_at(a, trace_hook);
    if (f) fclose(f), LOG("[options] tracing %d functions\n", g_ntraced);
    if (getenv("LARECOMP_ZA_ARCDUMP")) rt_register_fnhook_at(ZA_OODLE_DECOMPRESS, decompress_hook);
}
