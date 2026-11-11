// User settings (larecomp.ini in the data folder), set on the start screen (runtime/ui/launcher_lg.mm).
#pragma once
#include <stdbool.h>

enum { RES_MATCH = 0, /* 1..8 = 1x..8x */ RES_DYNAMIC = 9, RES_Q75 = 10, RES_Q25 = 11, RES_Q50 = 12 };
// (11 was 67% of the display: larecomp.ini "67%" now loads as RES_Q75)
enum { UPSCALE_BILINEAR = 0, UPSCALE_FSR1 = 2 /* AMD FSR 1 */ };
#define UPSCALE_SHARP UPSCALE_FSR1  // the non-bilinear upscaler (the default)
enum { AA_OFF = 0, AA_SMAA = 1 };
enum { VREAD_AUTO = 0, VREAD_COPY = 1, VREAD_DIRECT = 2 };
#define FPS_UNLIMITED 360  // Settings.fps "Unlimited": as fast as the PC goes, paced at 360 at most
enum { BG_KEEP = 0, BG_MUTE = 1, BG_PAUSE = 2 };  // (Settings.background)
enum { PROMPTS_AUTO, PROMPTS_SWITCH, PROMPTS_XBOX, PROMPTS_PLAYSTATION, PROMPTS_KEYBOARD };  // (auto: what was used last)

typedef struct {
    bool fullscreen;
    int fps;  // 30 (the game's own), 60 (Let's Go: interpolated, exec.c), or 60 / 72 / 120 / 144 / FPS_UNLIMITED (the Legends: hle/fps60.c)
    bool fixed_steps;    // the Legends above 30 fps: the world steps the frame rate's period each frame (a dip slows the
                         // game down), not the time that really passed (the default; FPS_UNLIMITED always: hle/fps60.c)
    int res_mode;        // RES_MATCH, 1..4 (N x the Switch's 1080p), RES_DYNAMIC, RES_Q75/50/25 (share of the display)
    int res_height;      // the launcher's render height in lines (720 / 1080 / 1440 / 2160); 0: res_mode decides
    int upscaler;        // UPSCALE_*: how a lower internal resolution reaches the window
    int antialias;       // AA_*: anti-aliasing of the game image before it is scaled to the window
    float volume;        // master volume, 1 = 100%
    float mouse_sens;    // mouse aim (the virtual gyro, runtime/hle/input.c): degrees per pixel; the mouse camera's speed
    bool mouse_camera;   // a click in the game window takes the mouse: it turns the camera (runtime/hle/input.c)
    bool mouse_invert_x, mouse_invert_y;  // the mouse camera turned the other way (runtime/hle/input.c)
    int background;      // BG_*: what the game does while its window isn't focused
    int crowd;           // Z-A: how many characters are drawn in full at once, 0 the Switch's .. 3 (hle/za_fixes.c)
    int detail;          // Z-A: how far away models keep their full detail and small props appear, 0 the Switch's .. 3 (x1 / x2 / x4 / x10; props at most x4: hle/za_fixes.c)
    int shadows;         // Z-A: the shadow maps' size, 0 the Switch's 1024, 1 2048, 2 4096 (hle/za_fixes.c; at the next start)
    int sharpen;         // sharpening at the window's size (AMD RCAS, nvn/vulkan/exec.c post_process): 0 off, 1 low, 2 medium, 3 high
    bool shadow_smooth;  // Z-A: the shadow mask's taps bilinear - smooth shadow edges (nvn/vulkan/exec.c za_soft_shadows; at the next start)
    int shader_nowait;   // 1: draws whose shaders are still being built are skipped, not waited for (nvn/vulkan/exec.c wait_built)
    int pkhex_link;      // PKHeX's LiveHeX on port 6000: 0 off, 1 this PC, 2 the local network too (hle/pkm_link.c)
    int game_opts;       // the game's own settings (text speed, camera, volumes...) as its save last had them, -1 not seen yet
    int game_opts_next;  // ... as the launcher set them: put in the save when the game loads it, -1 none (hle/za_options.c)
    float stick_deadzone;  // a controller's sticks: the share of their travel read as rest (runtime/hle/input.c)
    bool reverb;         // room reverb effect
    bool movie_audio;
    bool show_fps;
    bool skip_start;     // the start screen goes straight to the game once it's ready (Shift held at start: shown)
    bool experimental;   // "Experimental settings" (start screen): frame rates above 60 offered - 120, 144, Unlimited - in the
                         // start screen and the game's Settings; off: 60 at most
    bool dpad_move;      // the d-pad (and arrow keys) move the player like the stick
    bool render_thread;  // the backend runs on its own thread (runtime/nvn/vulkan/renderthread.c); default on
    int vertex_read;     // VREAD_*: where the GPU reads vertex and index data (runtime/nvn/vulkan/exec.c)
    int job_workers;     // Z-A: job workers added to the game's three (ini extra_workers), -1 = auto (runtime/hle/za_jobs.c)
    bool wild_battle;    // touching a wild Pokémon starts a battle, then the capture (runtime/hle/wildbattle.c)
    int button_prompts;  // PROMPTS_*: which controller's buttons the game's prompts show (runtime/nvn/glyphs.c)
    char game_lang[8];   // the game's language code ("en-US", "es"...: runtime/mods.c game_language)
    char mod[1024];      // the mods that are on (mods/<name>/), in load order, separated by commas; empty = none (runtime/mods.h)
    char plugins_off[256];  // plugins/ files left out, comma-separated (runtime/plugin.c)
    char plugins_on[256];   // ... and the opt-in ones switched on ("steamrelay.dll"; plugin_opt_in)
    int next_fps, next_res_height;  // set in game (hle/za_options.c; the height + 1), 0 = not: for the ini, from the next start
} Settings;

extern Settings g_settings;
extern bool g_settings_fresh;  // no larecomp.ini yet (a first start): the start screen applies the recommended settings

void settings_load(void);
void settings_save(void);
void settings_defaults(Settings *s);  // (what a first start has)
