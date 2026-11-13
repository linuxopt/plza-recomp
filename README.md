# PLZA-Recomp: Pokémon Legends: Z-A for Linux (AppImage)

A native Linux build of **Pokémon Legends: Z-A** (Nintendo Switch, **version 2.0.2**) by static recompilation, packed
as a single **AppImage** that runs on most distributions. It is based on
[LZA-Recomp](https://gitlab.com/mr.wifi1143/LZA-Recomp) (branch `LegendsZA`) by MrWifi, with Linux packaging and
extra features:

- one-file AppImage for any distro with glibc 2.31+ (nothing to install but a GPU driver)
- pick your **base game**, **2.0.2 update**, **Mega Dimension DLC** (`.nsp` / `.xci`) and **prod.keys** one by one in
  the launcher, then **Recompile** - or add the DLC on its own later
- **cheats**: Atmosphère cheat codes (EdiZon / Breeze format), the GBAtemp cheat database downloadable from the launcher,
  toggled live in game (**F2**)
- **field of view** slider, **AMD FSR 1** upscaling and **RCAS sharpening**
- every frame rate (30 / 60 / 72 / 120 / 144 / Unlimited) without an "experimental" switch: scripted scenes play at 60
  and battles at up to 144 by themselves
- fixes: a freeze right after the name entry on AMD GPUs (RADV), setup from flat extracted dumps

> **No game files, keys or game code are included, and none ever will be.** You need your own dump of the game and
> its 2.0.2 update, and your own `prod.keys`, from your own Switch. The game code is generated and compiled on your PC
> from your dump, once.

Pokémon Legends: Z-A is © Nintendo / Creatures Inc. / GAME FREAK inc. / The Pokémon Company. This project is not
affiliated with or endorsed by them.

---

## Contents

1. [Requirements](#requirements)
2. [Download and install](#download-and-install)
3. [Your game files: base game (XCI / NSP), update, DLC, keys](#your-game-files)
4. [Mega Dimension DLC](#mega-dimension-dlc)
5. [Cheat codes](#cheat-codes)
6. [Options worth knowing](#options-worth-knowing)
7. [Where your data is](#where-your-data-is)
8. [Troubleshooting](#troubleshooting)
9. [Building the AppImage yourself](#building-the-appimage-yourself)
10. [Credits and license](#credits-and-license)

---

## Requirements

- **CPU**: x86-64 with **AVX2** (Intel Haswell / 2013 or newer, AMD Zen or newer)
- **GPU**: a **Vulkan 1.3** driver - Mesa (RADV for AMD, ANV for Intel, NVK) or NVIDIA's - and the Vulkan loader
  (`libvulkan.so.1`: package `vulkan-icd-loader` on Arch, `libvulkan1` on Debian / Ubuntu, `vulkan-loader` on Fedora)
- **Linux**: glibc 2.31 or newer - Ubuntu 20.04+, Debian 11+, Fedora 32+, Arch / Manjaro / EndeavourOS / CachyOS,
  SteamOS 3, openSUSE Leap 15.3+ / Tumbleweed, Linux Mint 20+, Pop!_OS 20.04+
- **Disk**: about **13 GB** free during setup on the drive the AppImage is on (about 8 GB stays: the game data
  image and the compiled game code)
- **RAM**: 16 GB recommended
- `curl` or `wget` (only for downloading the cheat database)

Python, the compiler (zig), SDL 3, FFmpeg and shaderc are inside the AppImage.

## Download and install

1. Download `ZARecomp-<version>-x86_64.AppImage` from the
   [Releases](https://github.com/linuxopt/plza-recomp/releases) page.
2. Make it executable and start it:

   ```sh
   chmod +x ZARecomp-*-x86_64.AppImage
   ./ZARecomp-*-x86_64.AppImage
   ```

   Or in a file manager: right-click → Properties → Permissions → "Allow executing", then double-click it.
3. Optional: add it to your application menu (it copies itself to `~/Applications`):

   ```sh
   ./ZARecomp-*-x86_64.AppImage --install      # --uninstall removes the menu entry again
   ```

If it says **FUSE** is missing, install `fuse3` (or `fuse` / `libfuse2`), or start it with
`--appimage-extract-and-run`. On **Steam Deck / SteamOS** see [Steam Deck](#steam-deck) below.

**Put the AppImage in a folder of its own** (e.g. `~/Games/plza-recomp/`): the game files it sets up go into a
`ZARecomp-data` folder beside it (see [Where your data is](#where-your-data-is)). **Don't start it with `sudo`** - it
refuses to run as root, because the files it made would belong to root and your user couldn't update them.

### Steam Deck

1. In **Desktop Mode** make a folder for it, e.g. `/home/deck/Games/plza-recomp/` - or on the SD card,
   `/run/media/deck/<card>/plza-recomp/` (the card must be **ext4** - SteamOS formats it that way - or exFAT; a FAT32
   card can't hold the 6 GB game image).
2. Put the AppImage there, make it executable (right-click → Properties → Permissions → *Is executable*) and start it
   once by double-clicking: set up the game files (below). They go into `plza-recomp/ZARecomp-data/`, so it needs about
   13 GB free on that drive during setup (~8 GB stays).
3. Steam → *Add a Non-Steam Game* → *Browse* → the AppImage. It switches to an X11 window by itself so Steam Input
   works; play it from Game Mode from then on.

**Updating**: download the new AppImage and replace the old file. Your game files, saves and settings stay; if the new
version needs the game code built again, the launcher does it by itself the first time (a few minutes).

## Your game files

You need, dumped from **your own Switch**:

| What | File | Notes |
|---|---|---|
| Base game | `.xci` (game card) or `.nsp` | Pokémon Legends: Z-A 1.0.0, title id `0100F43008C44000` |
| Update | `.nsp` | **2.0.2** (v393216). Other updates (2.0.0, v262144) are not supported |
| Mega Dimension DLC | `.nsp` (or `.xci`) | optional |
| Keys | `prod.keys` | dumped with Lockpick_RCM; `title.keys` beside it is read too |

Dump the game and update with a homebrew dumper (e.g. nxdumptool / DBI) and the keys with Lockpick_RCM on the same
Switch (keys from a recent system version open the 2.0.2 update). Compressed dumps (`.nsz` / `.xcz`) must be
decompressed first: `nsz -D file.nsz`.

### Setting up (once)

1. Start the AppImage. In the launcher open **GAME FILES**.
2. **Base game** → *Browse…* → pick your `.xci` or `.nsp`.
3. **Game update** → *Browse…* → pick the 2.0.2 update `.nsp`.
4. **Mega Dimension DLC** (optional) → *Browse…* → pick the DLC `.nsp` / `.xci`.
5. **prod.keys** → *Browse…* → pick your `prod.keys`. If one is already in a usual place (beside the base game,
   `~/.switch/`, or the Eden / Yuzu / Ryujinx key folders) it is shown in yellow and used when you don't pick one.
6. Click **Recompile the game**. The launcher
   - reads your packages with your keys and applies the 2.0.2 update over the base game,
   - writes the game data image (~6 GB) and the DLC's data,
   - generates the game code and compiles it for your PC (about 5 minutes on a 12-core CPU, longer on smaller ones).
7. When it says **ready to play**, go back and press **PLAY**. The first start also prepares the shaders (a progress
   bar; short stutters the first time new things appear are normal).

Your picks are remembered. Your dump files are only read, never changed - you can delete or move them afterwards.

**Already extracted files** work too: under *Or: already extracted files* click **Select game folder…** and pick the
folder with `exefs` (`main`, `sdk`, `main.npdm`) and `romfs` (`arc/data.trpfs`) of 2.0.2 - or one flat folder with
`main`, `sdk`, `main.npdm` and `arc/` side by side, as some tools extract it.

**Recompile game code** (beside "ready to play") builds the game code again from the files already set up - needed
after you add code cheats (see below).

If something is wrong the launcher says what: wrong version ("needs version 2.0.2"), no keys, keys too old, only the
update found, compressed files. Details are in `lasrecomp.log` (see [Where your data is](#where-your-data-is)).

## Mega Dimension DLC

The DLC is optional; without it the game runs as on a Switch that doesn't own it.

- **While setting up from packages**: pick it in the *Mega Dimension DLC* row before *Recompile the game*.
- **Adding it later** (also when you set the game up from extracted files): pick its `.nsp` / `.xci` in the
  *Mega Dimension DLC* row, then click **Add the DLC picked above**. It is read with your `prod.keys`; nothing is
  recompiled. The line above the button then says *Mega Dimension DLC: set up*. It is active from the next start of the
  game - the title screen shows "Downloadable content ready".
- **Replacing it**: pick another file and click *Replace with the DLC picked above*.

## Cheat codes

The **CHEATS** page of the launcher, and the **Cheats panel in game** (**F2** on the keyboard, **Minus + R** on a
controller; Esc / B closes it), run Atmosphère-format cheat codes - the `.txt` files EdiZon, Breeze and the emulators
use - on the game's memory, about 12 times a second like on a Switch.

### Getting cheats

- **Download the GBAtemp cheat database**: fetches the newest file of the
  [Pokemon Legends Z-A Cheat Database](https://gbatemp.net/threads/pokemon-legends-z-a-cheat-database.675579/)
  thread (needs `curl` or `wget` and an internet connection) and installs the one for 2.0.2 with its descriptions.
  Click it again later to update.
- **Import a cheat file…**: any cheat `.txt` for build `B1F12FD919EAE86A` (Z-A 2.0.2), or a `.zip` of cheats (the
  database's own zip, a `LEGENDSZA/.../cheats/` folder layout) - the right file is picked out of it.
- **By hand**: put the file in the data folder as `cheats/B1F12FD919EAE86A.txt` (and Breeze's `notes.txt` beside it
  for the descriptions). **Open the cheats folder** opens it.

### Using them

1. After installing or changing a cheat file the page may say *Code cheats take effect once the game code is compiled
   with this cheat file* - click **Recompile game code** (a few minutes, once per cheat file). Why: most Z-A cheats
   change the game's own instructions, and in a recompilation those instructions are compiled code; the build
   compiles every instruction the cheat file can put at each place it patches, so they can be switched on and off
   later without rebuilding.
2. Tick the cheats you want - in the launcher before playing, or in game with **F2** / **Minus + R**. They apply at
   once and stay on for the next start (`cheats/enabled.txt`).
3. Untick a cheat to switch it off: the game's own code is put back by itself, so the file's *Restore Code* entry is
   not needed (it is hidden). **All off** switches everything off.
4. Cheats with a button in their description ("Hold ZL to run fast", "Hold B to float", "R3 + ↑ / ↓") react to those
   buttons on your controller, or the keys bound to them.

Cheats that only change data (money values, pointers like the Moon Jump) work without the recompile. A cheat for
another game version won't do anything useful - keep to files for build `B1F12FD919EAE86A`. Cheats can break saves:
**back up `save/`** before trying ones that change items, Pokémon or progress. As on a Switch, some cheats conflict with
each other (two that use the same code cave) - turn one off.

### Writing your own

Cheat files are plain text in Atmosphère's format:

```
[My cheat name]
04000000 00A71864 B9004009
```

`[Name]` starts a cheat, `{Name}` a master code (always on), the lines are 8-digit hex code words. All of Atmosphère's
code types work: static writes (0), memory / key / register conditionals (1, 8, C0, C4), loops (3), register loads
and arithmetic including float operations (4, 5, 7, 9), register stores (6, A), saved / static registers (C1-C3).
Memory region 0 is the game's main module, 1 the heap. After editing the file, recompile the game code if you added
writes to code.

## Options worth knowing

All in the launcher's **OPTIONS** and in the game's own Settings (Menu → Help & Settings → Settings):

- **Frame rate** 30 / 60 / 72 / 120 / 144 / Unlimited. Above 60, scripted scenes switch to 60 fps by themselves and
  back after; at Unlimited, battles are held at 144.
- **Field of view** (Graphics; in game: Camera tab): 70-170 % of the game's own camera angle, applies at once.
- **Resolution** (540p - 4K, or Automatic) with **Upscaler**: AMD FSR 1 or Bilinear - FSR upscales when the resolution
  is below your window's (e.g. 1080p on a 1440p screen); **Sharpening** (AMD RCAS): Off / Low / Medium / High.
- Shadows (up to 4096), draw distance for people and detail, SMAA, FPS counter, shader loading Wait / Don't wait.
- **Controls**: controller test and remapping, keyboard and mouse (WASD, mouse look - click to take the mouse, Esc
  lets it go), button prompts for Xbox / PlayStation / keyboard.
- **Mods**: translations and texture packs (`.zip` / `.rar` / `.7z`), plugins: *Steam relay* (Link Trades and Private
  Battles with Steam friends) and *Legends Lua* (Lua scripting; examples in the data folder's `lua-examples/`).
- **PKHeX**: saves are the Switch's own files (`save/save_0/main`); *PKHeX Link* (Gameplay tab) lets PKHeX's LiveHeX
  connect on port 6000; *Pokémon Files* exports / imports `.pa9`.

In game: **Esc** / **F1** asks to close the game, **F11** or Alt+Enter toggles fullscreen, **F12** takes a screenshot,
**F2** opens the Cheats panel. Hold **Shift** while starting to see the launcher again if you ticked *Skip the launcher*.

## Where your data is

Game files, saves, settings, cheats, mods, shader caches and the log `lasrecomp.log` live in the data folder:

- a **`ZARecomp-data` folder next to the AppImage** by default - made on the first start, so everything stays in the
  folder you put the AppImage in (another drive, a Steam Deck's SD card). The launcher shows it at the bottom of
  GAME FILES (*Open* opens it), and `./ZARecomp-*.AppImage --data-dir` prints it;
- `~/.local/share/zarecomp` when the AppImage's folder isn't writable (or is FAT32), and for setups made by earlier
  versions, which keep using it (to move one: move that folder next to the AppImage and rename it `ZARecomp-data`);
- or set `LARECOMP_DATA_ROOT=/some/folder`.

`--install` copies the AppImage to `~/Applications`; its menu entry keeps using the data folder you had.

| Path | What |
|---|---|
| `extracted/` | the set-up game files and the compiled game code (made from your dump: don't share it) |
| `save/save_0/main` | your save (PKHeX opens it) |
| `cheats/` | `B1F12FD919EAE86A.txt`, `notes.txt`, `enabled.txt` |
| `larecomp.ini` | settings |
| `lasrecomp.log` / `lasrecomp-prev.log` | the log of this / the previous run |
| `mods/`, `plugins/`, `lua/` | mods, plugins, Lua scripts |

To start over, delete `extracted/` (your saves stay).

## Troubleshooting

- **"This needs a CPU with AVX2"** - the game code is built for x86-64-v3; older CPUs can't run it.
- **"No Vulkan loader"** / a black window - install your distribution's Vulkan package and a current GPU driver.
  `vulkaninfo --summary` should list your GPU with API 1.3 or newer.
- **The launcher says "needs version 2.0.2"** - your update isn't 2.0.2 (2.0.0 is v262144). Update the game on your
  Switch and dump the update again.
- **"No keys found" / "Your keys can't open those game files"** - pick your `prod.keys` in the prod.keys row; dump it
  again from your Switch on its current system version if it is older than the update.
- **"Not enough free space for the game files"** - the launcher checks before it starts and shows how much is free
  in which folder. Free some space, or move the AppImage to a bigger drive (its `ZARecomp-data` folder moves with it).
- **"The data folder is on a FAT32 drive"** - FAT32 holds at most 4 GB a file; the game image is 6 GB. Use an ext4
  or exFAT drive (Steam Deck: *Settings → System → Format SD card* makes it ext4 - this erases the card).
- **"Can't write to the data folder"** / setup fails right away on a Steam Deck - it was probably started with `sudo`
  once, which made the folder root's. Give it back: `sudo chown -R $USER <the folder it names>`, then start the
  AppImage normally.
- **Setup fails while compiling** - check there is space in the data folder, then look at the end of `lasrecomp.log`.
- **Stutters the first time** - shaders are compiled as new things appear; they are cached for later. For a frame time
  overlay start it with MangoHud: `mangohud ./ZARecomp-*-x86_64.AppImage`.
- **Crashes or freezes at 1440p and above** - the game's memory now grows with the screen: above 1080p it gets
  4.5 GB plus room for the shadows / draw distance options (before, 1440p was left at the Switch's 2.75 GB). If it
  still crashes there, start it with more:
  `LARECOMP_ZA_HEAP_MB=6144 ./ZARecomp-*-x86_64.AppImage` (at most 6656).
- **Code cheats do nothing** - the CHEATS page says if the game code needs a *Recompile game code* for the current
  cheat file.
- **A freeze or crash** - attach `lasrecomp.log` (and `lasrecomp-prev.log`) when you report it in the Issues.

## Building the AppImage yourself

Everything that goes into the AppImage is built from source here, except the official prebuilt downloads it pins by
checksum (zig, a portable CPython, three Python wheels, appimagetool). On an x86-64 Linux machine:

**1. Build tools and headers**

```sh
# Arch / Manjaro / EndeavourOS / CachyOS
sudo pacman -S --needed base-devel git curl cmake ninja python patchelf zstd unzip wayland wayland-protocols libxkbcommon \
  libdecor libpipewire libpulse alsa-lib dbus systemd-libs libx11 libxext libxrandr libxcursor libxi libxfixes libxss \
  vulkan-headers vulkan-icd-loader
# Debian 12+ / Ubuntu 22.04+
sudo apt install build-essential git curl xz-utils cmake ninja-build pkg-config python3 patchelf zstd unzip \
  libwayland-dev wayland-protocols libxkbcommon-dev libdecor-0-dev libpipewire-0.3-dev libpulse-dev libasound2-dev \
  libdbus-1-dev libudev-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev \
  libvulkan-dev
# Fedora
sudo dnf install gcc gcc-c++ make git curl cmake ninja-build pkgconf python3 patchelf zstd unzip wayland-devel \
  wayland-protocols-devel libxkbcommon-devel libdecor-devel pipewire-devel pulseaudio-libs-devel alsa-lib-devel \
  dbus-devel systemd-devel libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXi-devel libXfixes-devel \
  libXScrnSaver-devel vulkan-headers vulkan-loader-devel
```

**2. Build**

```sh
git clone https://github.com/linuxopt/plza-recomp.git
cd plza-recomp
tools/linux/build_appimage.sh
```

The first run downloads ~200 MB and builds SDL 3, FFmpeg and shaderc (a few minutes); the result is
`~/.cache/plza-appimage-build/dist/ZARecomp-<version>-x86_64.AppImage`. Set `LZA_LINUXBUILD=/path` to use another
build folder, `JOBS=<n>` for the parallel jobs, `FORCE=1` to rebuild the libraries.

How it is made portable: every C / C++ part is compiled with zig for `x86_64-linux-gnu.2.31`, so the binaries need
nothing newer than glibc 2.31; the libraries are built with their Linux backends loaded at run time (X11 / Wayland /
PipeWire / PulseAudio / ALSA), the Vulkan loader is the player's own. `tools/linux/make_appimage.sh` assembles the
AppDir (runtime, libraries, plugins, the portable Python with the game code generator, a trimmed zig for compiling the
game code on the player's PC, the licenses) and refuses to finish if anything from a game ended up in it.

**Development build** (without the AppImage, using your system's clang): see
[docs/README-upstream.md](docs/README-upstream.md) ("Building from source").

Source layout: `runtime/` the runtime (kernel, services, NVN on Vulkan, launcher `runtime/ui/launcher_lg.mm`, cheats
`runtime/cheats.c`), `tools/gen.py` + `tools/lifter.py` the game code generator, `contrib/` the plugins,
`tools/linux/` the AppImage scripts.

## Credits and license

- [LZA-Recomp](https://gitlab.com/mr.wifi1143/LZA-Recomp) by MrWifi - the Z-A recompilation this is built from
- [letsgo-recomp](https://github.com/raulzrrs/letsgo-recomp) by raulzrrs and [LAS Recomp](https://github.com/peiikai/LASRecomp)
  by Peiikai - the projects it descends from
- Cheat database: the contributors of the GBAtemp "Pokemon Legends Z-A Cheat Database" thread (not included; downloaded
  by the player)

MIT licensed: [LICENSE](LICENSE) and [LICENSE-LASRecomp](LICENSE-LASRecomp). Third-party components keep their own
licenses: [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md); the AppImage has them all in `opt/zarecomp/licenses/`.
Unicorn (used only by the game code generator) is GPL 2.0; FFmpeg is LGPL 2.1, shipped as separate shared libraries.
