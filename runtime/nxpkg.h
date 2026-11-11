// Game files straight from the player's own dumped packages (runtime/nxpkg.c), the way the emulators read them: the
// .nsp / .xci of the base game, its update and its add-on content, opened with the player's keys (prod.keys, dumped
// from their own Switch). Only what the runtime uses is written out: the program's exefs files, the RomFS image (the
// update's patch applied over the base game's RomFS) and the add-on content's RomFS image.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NXPKG_OK,
    NXPKG_NO_KEYS,       // no prod.keys found (or no usable header key in it)
    NXPKG_KEY_MISSING,   // keys older than the game (its master key generation) or no title key for it
    NXPKG_NO_GAME,       // packages found, but not the base game of a wanted title
    NXPKG_ONLY_UPDATE,   // a wanted title's update (or add-on content) without its base game
    NXPKG_COMPRESSED,    // .nsz / .xcz (compressed NCAs): not read
    NXPKG_UNSUPPORTED,   // a layout this reader doesn't know (sparse / compressed sections...)
    NXPKG_READ_ERROR,    // unreadable / damaged package, or the output couldn't be written
};

typedef struct NxPkg NxPkg;

// Scans `path` (a package file, or a folder searched a few levels deep) for .nsp / .xci and finds the keys (the folder
// itself, ~/.switch, the emulators' key folders). titles: the wanted base program ids, best first, 0-terminated.
// NULL = failed: *err (NXPKG_*) and why (a short message for the log).
NxPkg *nxpkg_open(const char *path, const uint64_t *titles, int *err, char *why, size_t why_size);
// The same from packages picked one by one (the base game, the update, the add-on content: files or folders, any
// order; empty entries skipped) with the keys file picked (keys NULL or "": searched as nxpkg_open does, beside the
// first package first). title.keys beside the keys file is read too.
NxPkg *nxpkg_open_list(const char *const *paths, int npaths, const char *keys, const uint64_t *titles, int *err, char *why,
                       size_t why_size);
void nxpkg_close(NxPkg *p);
// Where prod.keys would be found for packages in / beside `near` (a file or folder; NULL: the usual places only): its
// path in out. False: none with a usable header key.
bool nxpkg_find_keys(const char *near, char *out, size_t n);
// True when `path` holds .nsp / .xci / .nsz / .xcz (a file, or a folder searched a few levels deep).
bool nxpkg_any(const char *path);
// The base game picked (its program id).
uint64_t nxpkg_title(const NxPkg *p);
// Updates found for it, newest first (index 1..count; 0 = the base game alone), and their versions (v65536 = 1.0.1...).
int nxpkg_update_count(const NxPkg *p);
uint32_t nxpkg_update_version(const NxPkg *p, int upd);
// Writes the program's exefs files (with update upd, 0 = none) into the folder dir (created).
bool nxpkg_write_exefs(NxPkg *p, int upd, const char *dir, char *why, size_t why_size);
// Writes the RomFS image (the base game's, with update upd's patch applied) to the file out. progress may be NULL.
bool nxpkg_write_romfs(NxPkg *p, int upd, const char *out, void (*progress)(uint64_t done, uint64_t total), char *why,
                       size_t why_size);
// Writes the add-on content's RomFS image (the first found for the title) to out; false = none found or failed.
bool nxpkg_write_dlc(NxPkg *p, const char *out, char *why, size_t why_size);

#ifdef __cplusplus
}
#endif
