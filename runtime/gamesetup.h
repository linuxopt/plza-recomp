// Game file setup on the start screen (runtime/gamesetup.c): the player picks the folder with their own
// Pokémon Legends: Z-A 2.0.2 - the dumped .nsp / .xci of the base game and the update (read with their prod.keys,
// runtime/nxpkg.c), or extracted files (exefs/ and romfs/, or a romfs.bin image) -, it is checked, set up in
// <data root>/extracted (exefs copied, the RomFS image built) and the game code is recompiled there from it
// (runtime/gamebuild.c). Runs on a background thread; the launcher polls its state.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SETUP_IDLE,         // nothing running
    SETUP_LOOKING,      // looking for the game files in the picked folder
    SETUP_UNPACKING,    // reading them out of the .nsp / .xci (the RomFS image with the update applied)
    SETUP_COPYING,      // copying exefs / the RomFS image
    SETUP_IMAGE,        // building the RomFS image from romfs/
    SETUP_READING,      // generator: translating the game code
    SETUP_COMPILING,    // compiler
    SETUP_FINISHING,    // linking
    SETUP_DONE,         // ready to play
    // failures (setup_state)
    SETUP_FAIL_NOT_FOUND,   // no exefs/main + romfs in that folder
    SETUP_FAIL_PACKAGE,     // compressed packages (.nsz / .xcz)
    SETUP_FAIL_VERSION,     // not Let's Go, Eevee! / Pikachu! (main.npdm's program id)
    SETUP_FAIL_COPY,        // disk full / unreadable files
    SETUP_FAIL_BUILD,       // the game code didn't build
    SETUP_FAIL_TOOLS,       // the generator / compiler are missing from this copy
    SETUP_FAIL_ZA_VERSION,  // Legends Z-A, but not 2.0.2 (game_za_unsupported)
    SETUP_FAIL_KEYS,        // packages, but no prod.keys found
    SETUP_FAIL_KEYS_OLD,    // the keys can't open them (older than the game, or no ticket / title key)
    SETUP_FAIL_NO_BASE,     // the update's package without the base game's
    SETUP_FAIL_UNPACK,      // a package couldn't be read (damaged, or a layout runtime/nxpkg.c doesn't read)
};

// The game files in <data root>/extracted: ready to play (exefs, romfs.bin and game code built for this version).
bool setup_ready(void);
// The game in that exefs/ folder: "Eevee", "Pikachu" or NULL.
const char *game_name(const char *exefs);
// The build of that exefs/main: one with address tables (runtime/fnhook.c, tools/patches.py) - the 60 fps and wild
// battles options and the generator's patches - or GAME_BUILD_OTHER (an update, say: those stay off). Legends Z-A:
// 2.0.2 (main B1F12FD919EAE86A), the build the runtime's Z-A addresses are written for.
enum { GAME_BUILD_OTHER, GAME_BUILD_EEVEE_100, GAME_BUILD_PIKACHU_100, GAME_BUILD_ZA_202 };
int game_build(const char *exefs);
// Legends Z-A, but not 2.0.2 (2.0.0, say): not played - its addresses are all elsewhere (SETUP_FAIL_ZA_VERSION).
bool game_za_unsupported(const char *exefs);
// Its version ("1.0.0", "1.0.2"...: the builds known by their build id; "1.0.0" otherwise), told to the game (am).
const char *game_version(const char *exefs);
// Starts a setup from the picked folder (NULL: only rebuild the game code from files set up already).
void setup_start(const char *folder);
// Starts a setup from packages picked one by one: the base game, its update and (optional, NULL / "") the add-on
// content (.nsp / .xci files), read with the keys file picked (NULL / "": prod.keys searched for as usual).
void setup_start_files(const char *base, const char *update, const char *dlc, const char *keys);
int setup_state(void);         // SETUP_*
float setup_progress(void);    // 0..1 while running
bool setup_running(void);

#ifdef __cplusplus
}
#endif
