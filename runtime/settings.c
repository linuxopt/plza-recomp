#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "input_bind.h"
#include "paths.h"

#define SETTINGS_FILE settings_file()

// LARECOMP_SETTINGS=<path>: another settings file (test runs never touch the player's larecomp.ini)
static const char *settings_file(void) {
    const char *e = getenv("LARECOMP_SETTINGS");
    return e && *e ? e : "larecomp.ini";
}

// The defaults (also the launcher's Restore defaults: settings_defaults)
#define SETTINGS_DEFAULTS {                                                 \
    .fullscreen = false,                                                    \
    .fps = 30,                                                              \
    .volume = 1.0f,                                                         \
    .mouse_sens = 0.1f,                                                     \
    .mouse_camera = true,                                                   \
    .stick_deadzone = 0.15f,                                                \
    .reverb = true,                                                         \
    .shadow_smooth = true,                                                  \
    .movie_audio = true,                                                    \
    .show_fps = false,                                                      \
    .render_thread = true,                                                  \
    .vertex_read = VREAD_AUTO,                                              \
    .job_workers = -1,                                                      \
    .upscaler = UPSCALE_SHARP,                                              \
    .game_opts = -1,                                                        \
    .game_opts_next = -1,                                                   \
    .antialias = AA_SMAA,  /* (the game has no anti-aliasing of its own) */ \
}
Settings g_settings = SETTINGS_DEFAULTS;
void settings_defaults(Settings *s) { *s = (Settings)SETTINGS_DEFAULTS; }

// Unknown keys (an older larecomp.ini: Link's Awakening's options from LAS Recomp) are ignored, and dropped at the
// next save.
static bool g_experimental_seen;
static void parse_line(const char *line) {
    char key[64];
    char val[1024];  // (mod: several names)
    if (sscanf(line, " %63[^= ] = %1023[^\n]", key, val) < 1) return;
    if (!strcmp(key, "game_lang") || !strcmp(key, "mod") || !strcmp(key, "plugins_off") || !strcmp(key, "plugins_on")) {  // (may be empty)
        if (sscanf(line, " %63[^= ] = %1023[^\n]", key, val) != 2) val[0] = 0;
        if (key[0] == 'g') strlcpy(g_settings.game_lang, val, sizeof g_settings.game_lang);
        else if (!strcmp(key, "plugins_off")) strlcpy(g_settings.plugins_off, val, sizeof g_settings.plugins_off);
        else if (key[0] == 'p') strlcpy(g_settings.plugins_on, val, sizeof g_settings.plugins_on);
        else strlcpy(g_settings.mod, val, sizeof g_settings.mod);
        return;
    }
    if (!strcmp(key, "fullscreen")) g_settings.fullscreen = atoi(val) != 0;
    else if (!strcmp(key, "fps")) { int f = atoi(val); g_settings.fps = f == 60 || f == 72 || f == 120 || f == 144 || f == FPS_UNLIMITED ? f : 30; }
    else if (!strcmp(key, "experimental")) g_settings.experimental = atoi(val) != 0, g_experimental_seen = true;
    else if (!strcmp(key, "frame_timing")) g_settings.fixed_steps = !strcmp(val, "fixed");
    else if (!strcmp(key, "render_height")) g_settings.res_height = atoi(val) >= 360 && atoi(val) <= 4320 ? atoi(val) : 0;
    else if (!strcmp(key, "volume")) g_settings.volume = (float)atof(val);
    else if (!strcmp(key, "mouse_sens")) g_settings.mouse_sens = (float)atof(val);
    else if (!strcmp(key, "stick_deadzone")) g_settings.stick_deadzone = (float)atof(val);
    else if (!strcmp(key, "reverb")) g_settings.reverb = atoi(val) != 0;
    else if (!strcmp(key, "movie_audio")) g_settings.movie_audio = atoi(val) != 0;
    else if (!strcmp(key, "show_fps")) g_settings.show_fps = atoi(val) != 0;
    else if (!strcmp(key, "skip_start_screen")) g_settings.skip_start = atoi(val) != 0;
    else if (!strcmp(key, "mouse_camera")) g_settings.mouse_camera = atoi(val) != 0;
    else if (!strcmp(key, "mouse_invert_x")) g_settings.mouse_invert_x = atoi(val) != 0;
    else if (!strcmp(key, "mouse_invert_y")) g_settings.mouse_invert_y = atoi(val) != 0;
    else if (!strcmp(key, "detail_distance")) g_settings.detail = atoi(val) >= 0 && atoi(val) <= 3 ? atoi(val) : 0;
    else if (!strcmp(key, "shadow_smoothing")) g_settings.shadow_smooth = atoi(val) != 0;
    else if (!strcmp(key, "crowd")) g_settings.crowd = atoi(val) >= 0 && atoi(val) <= 3 ? atoi(val) : 0;
    else if (!strcmp(key, "shadows")) g_settings.shadows = atoi(val) >= 0 && atoi(val) <= 2 ? atoi(val) : 0;
    else if (!strcmp(key, "shader_loading")) g_settings.shader_nowait = !strcmp(val, "nowait") || !strcmp(val, "1");
    else if (!strcmp(key, "game_options")) g_settings.game_opts = (int)strtol(val, NULL, 0) & 0x1fffff;
    else if (!strcmp(key, "game_options_next")) g_settings.game_opts_next = (int)strtol(val, NULL, 0) & 0x1fffff;
    else if (!strcmp(key, "pkhex_link")) g_settings.pkhex_link = atoi(val) >= 0 && atoi(val) <= 2 ? atoi(val) : 0;
    else if (!strcmp(key, "background")) g_settings.background = atoi(val) >= BG_KEEP && atoi(val) <= BG_PAUSE ? atoi(val) : BG_KEEP;
    else if (!strcmp(key, "resolution")) {
        int n = atoi(val);
        g_settings.res_mode = !strcmp(val, "dynamic") ? RES_DYNAMIC : (!strcmp(val, "75%") || !strcmp(val, "67%")) ? RES_Q75 :
                              !strcmp(val, "50%") ? RES_Q50 : !strcmp(val, "25%") ? RES_Q25 :
                              (n >= 1 && n <= 4) ? n : n > 4 ? 4 : RES_MATCH;
    }
    else if (!strcmp(key, "upscaler")) g_settings.upscaler = !strcmp(val, "bilinear") ? UPSCALE_BILINEAR : UPSCALE_SHARP;
    // (an older "antialias = off" was only LAS Recomp's default being saved: a new key, so SMAA starts on)
    else if (!strcmp(key, "smaa")) g_settings.antialias = atoi(val) != 0 ? AA_SMAA : AA_OFF;
    else if (!strcmp(key, "render_thread")) g_settings.render_thread = atoi(val) != 0;
    // (an older "job_workers = 0" was only the default of its experimental days being saved: a new key, so auto starts on)
    else if (!strcmp(key, "extra_workers")) g_settings.job_workers = !strcmp(val, "auto") ? -1 : atoi(val);
    else if (!strcmp(key, "vertex_read")) g_settings.vertex_read = !strcmp(val, "direct") ? VREAD_DIRECT : !strcmp(val, "copy") ? VREAD_COPY : VREAD_AUTO;
    else if (!strcmp(key, "dpad_move")) g_settings.dpad_move = atoi(val) != 0;
    else if (!strcmp(key, "wild_battle")) g_settings.wild_battle = atoi(val) != 0;
    else if (!strcmp(key, "button_prompts"))
        g_settings.button_prompts = !strcmp(val, "switch") ? PROMPTS_SWITCH : !strcmp(val, "xbox") ? PROMPTS_XBOX :
                                    !strcmp(val, "playstation") ? PROMPTS_PLAYSTATION : !strcmp(val, "keyboard") ? PROMPTS_KEYBOARD :
                                    PROMPTS_AUTO;
    else bind_settings_load(key, val);  // bind_pad_* / bind_key_*: runtime/hle/bindings.c
}

bool g_settings_fresh;

void settings_load(void) {
    static bool loaded;
    if (loaded) return;
    loaded = true;
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) { g_settings_fresh = true; return; }
    char line[1200];
    while (fgets(line, sizeof line, f)) {
        // (a file edited by hand may have CRLF: "resolution = dynamic\r" did not match and fell back silently)
        for (size_t n = strlen(line); n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' '); n--) line[n - 1] = 0;
        // (0.3.1 saved some settings without their line break - "fullscreen = 0fps = 60render_height = 1440": a key
        // glued to the previous value is split off again, so a file written then keeps its settings)
        const char *p = line;
        for (;;) {
            const char *eq = strstr(p, " = "), *next = eq ? strstr(eq + 3, " = ") : NULL, *k = next;
            while (k && k > eq + 3 && ((k[-1] >= 'a' && k[-1] <= 'z') || k[-1] == '_')) k--;
            if (!next || k == next || k <= eq + 3) { parse_line(p); break; }
            char part[1200];
            snprintf(part, sizeof part, "%.*s", (int)(k - p), p);
            parse_line(part);
            p = k;
        }
    }
    fclose(f);
    // Above 60 fps is experimental (the game's logic was made for 30; Unlimited: battles misbehave): offered only with
    // "Experimental settings" on. An ini from before that switch with such a rate in it: the switch turned on for it.
    // AppImage build: every frame rate is offered (scripted scenes drop to 60 and battles at Unlimited to 144 on their
    // own: hle/za_fixes.c), so the switch is always on; an older ini's "experimental = 0" no longer clamps.
    (void)g_experimental_seen;
    g_settings.experimental = true;
    if (g_settings.volume < 0) g_settings.volume = 0;
    if (g_settings.volume > 2) g_settings.volume = 2;
    LOG("[settings] loaded %s\n", SETTINGS_FILE);
}

void settings_save(void) {
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) { LOG("[settings] cannot write %s\n", SETTINGS_FILE); return; }
    fprintf(f, "# " RT_APP_NAME " settings (set on the start screen)\n");
    fprintf(f, "fullscreen = %d\n", g_settings.fullscreen);
    fprintf(f, "fps = %d\n", g_settings.next_fps ? g_settings.next_fps : g_settings.fps);
    fprintf(f, "frame_timing = %s\n", g_settings.fixed_steps ? "fixed" : "real_time");
    fprintf(f, "render_height = %d\n", g_settings.next_res_height ? g_settings.next_res_height - 1 : g_settings.res_height);
    fprintf(f, "smaa = %d\n", g_settings.antialias == AA_SMAA);
    fprintf(f, "mod = %s\n", g_settings.mod);
    fprintf(f, "plugins_off = %s\n", g_settings.plugins_off);
    fprintf(f, "plugins_on = %s\n", g_settings.plugins_on);
    fprintf(f, "mouse_sens = %.3f\n", g_settings.mouse_sens);
    fprintf(f, "stick_deadzone = %.2f\n", g_settings.stick_deadzone);
    fprintf(f, "volume = %.2f\n", g_settings.volume);
    fprintf(f, "reverb = %d\n", g_settings.reverb);
    fprintf(f, "movie_audio = %d\n", g_settings.movie_audio);
    fprintf(f, "show_fps = %d\n", g_settings.show_fps);
    fprintf(f, "skip_start_screen = %d\n", g_settings.skip_start);
    fprintf(f, "experimental = %d\n", g_settings.experimental);
    fprintf(f, "mouse_camera = %d\n", g_settings.mouse_camera);
    fprintf(f, "mouse_invert_x = %d\nmouse_invert_y = %d\n", g_settings.mouse_invert_x, g_settings.mouse_invert_y);
    fprintf(f, "background = %d\n", g_settings.background);
    fprintf(f, "crowd = %d\n", g_settings.crowd);
    fprintf(f, "shadow_smoothing = %d\n", g_settings.shadow_smooth);
    fprintf(f, "detail_distance = %d\n", g_settings.detail);
    fprintf(f, "shadows = %d\n", g_settings.shadows);
    fprintf(f, "shader_loading = %s\n", g_settings.shader_nowait ? "nowait" : "wait");
    fprintf(f, "pkhex_link = %d\n", g_settings.pkhex_link);
    if (g_settings.game_opts >= 0) fprintf(f, "game_options = 0x%06x\n", g_settings.game_opts);
    if (g_settings.game_opts_next >= 0) fprintf(f, "game_options_next = 0x%06x\n", g_settings.game_opts_next);
    fprintf(f, "upscaler = %s\n", g_settings.upscaler == UPSCALE_FSR1 ? "fsr1" : "bilinear");
    if (g_settings.res_mode == RES_DYNAMIC) fprintf(f, "resolution = dynamic\n");
    else if (g_settings.res_mode == RES_Q75) fprintf(f, "resolution = 75%%\n");
    else if (g_settings.res_mode == RES_Q25) fprintf(f, "resolution = 25%%\n");
    else if (g_settings.res_mode == RES_Q50) fprintf(f, "resolution = 50%%\n");
    else if (g_settings.res_mode >= 1 && g_settings.res_mode <= 8) fprintf(f, "resolution = %dx\n", g_settings.res_mode);
    else fprintf(f, "resolution = match\n");
    fprintf(f, "render_thread = %d\n", g_settings.render_thread);
    if (g_settings.job_workers < 0) fprintf(f, "extra_workers = auto\n");
    else fprintf(f, "extra_workers = %d\n", g_settings.job_workers);
    fprintf(f, "vertex_read = %s\n", g_settings.vertex_read == VREAD_DIRECT ? "direct" : g_settings.vertex_read == VREAD_COPY ? "copy" : "auto");
    fprintf(f, "dpad_move = %d\n", g_settings.dpad_move);
    fprintf(f, "wild_battle = %d\n", g_settings.wild_battle);
    static const char *const prompts[] = {"auto", "switch", "xbox", "playstation", "keyboard"};
    fprintf(f, "button_prompts = %s\n", prompts[g_settings.button_prompts >= 0 && g_settings.button_prompts <= 4 ? g_settings.button_prompts : 0]);
    fprintf(f, "game_lang = %s\n", g_settings.game_lang);
    bind_settings_save(f);
    fclose(f);
}
