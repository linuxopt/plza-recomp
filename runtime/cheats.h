// Cheats (runtime/cheats.c): Atmosphère-format cheat files (the dmnt cheat VM's codes, as EdiZon / Breeze / the
// emulators read them) run on the game's memory, ~12 times a second as on a Switch.
//
// <data root>/cheats/<BUILD ID>.txt holds them (Z-A 2.0.2: B1F12FD919EAE86A.txt), cheats/enabled.txt which are on,
// cheats/notes.txt (Breeze's notes, optional) what they do. Data cheats (memory, pointers) work at once. Most Z-A
// cheats rewrite the game's ARM64 instructions instead: in a recompilation those instructions are C compiled for this
// PC, so every place a cheat file writes code is compiled with each instruction the cheats can put there, picked by
// what memory holds now (tools/gen.py --cheat-sites, runtime/gamebuild.c). Those take effect live too - once the game
// code was compiled with that cheat file (cheats_need_rebuild: Game Files -> Recompile game code).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHEATS_DIR "cheats"
// the 2.0.2 cheat database's download (GBAtemp, "Pokemon Legends Z-A Cheat Database"): its newest zip attachment
#define CHEATS_DB_THREAD "https://gbatemp.net/threads/pokemon-legends-z-a-cheat-database.675579/"

void cheats_load(void);           // (re)reads the cheat file for the extracted game's build, and enabled.txt
const char *cheats_build_id(void);  // "B1F12FD919EAE86A" (the extracted main's), "" none
const char *cheats_file(void);    // its path (relative to the data root), "" none
int cheats_count(void);
const char *cheats_name(int i);  // as shown ("№" as "#"); enabled.txt keeps the file's own names
const char *cheats_note(int i);   // notes.txt's line for it, "" none
bool cheats_is_master(int i);     // {Master code}: always on
bool cheats_is_header(int i);     // a section title ([Item/Misc] 20000000): no codes of its own
bool cheats_is_hidden(int i);     // not shown: a folder's closing entry, "Restore Code" (switching off restores already)
bool cheats_is_code(int i);       // writes the game's code (needs the game code compiled with this file)
bool cheats_enabled(int i);
void cheats_set(int i, bool on);  // saved to enabled.txt; a code cheat switched off gets the game's own words back
void cheats_all_off(void);
void cheats_frame(void);          // each present: runs the VM at its rate

// The places the cheat file writes the game's code, for the generator: a JSON object {"<address>": ["<word>", ...]}
// (main module offsets, hex). hash: a short id of that set, kept beside the game code (cheats_need_rebuild).
bool cheats_write_sites(const char *json_path, char *hash, size_t n);
// The code-writing cheats of the current file need the game code compiled again (its sites differ from the build's).
bool cheats_need_rebuild(void);
void cheats_mark_built(const char *game_dir, const char *hash);  // (runtime/gamebuild.c: after a successful build)

// A cheat file (.txt) or a zip of them (the database's LEGENDSZA/<title>/cheats/<build>.txt + notes.txt) taken into
// cheats/: the one for this build. msg: what happened, for the user. true = a cheat file was installed.
bool cheats_import(const char *path, char *msg, size_t n);
// Downloads the database's newest zip (curl or wget) and imports it, on a thread of its own.
enum { CHEATS_DL_IDLE, CHEATS_DL_RUNNING, CHEATS_DL_DONE, CHEATS_DL_FAILED };
void cheats_download_start(void);
int cheats_download_state(void);
const char *cheats_download_msg(void);

#ifdef __cplusplus
}
#endif
