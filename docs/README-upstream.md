# PL:Z-A Recomp (LZ&A Recomp, branch `LegendsZA`)

A native PC port of **Pokémon Legends: Z-A** (Nintendo Switch, **version 2.0.2**) by **static recompilation**: the game's
ARM64 code is translated to C and compiled into a regular Windows or Linux program, with a runtime standing in for
the Switch's operating system (kernel, system services, NVN graphics on Vulkan, audio, controllers).

LZ&A Recomp has one branch per game:

- **`LegendsA`**: Pokémon Legends: Arceus (PL:A Recomp)
- **`LegendsZA`** (this branch): Pokémon Legends: Z-A (PL:Z-A Recomp)

> **No game files are included, and none ever will be.** You need your own dump of the game and its 2.0.2 update.
> Nothing derived from the game (game code, recompiled or not, assets, keys) is in this repository or its releases.

> **Version 2.0.2 only.** The recomp is made for Z-A 2.0.2 (base game 1.0.0 + the 2.0.2 update, main build
> `B1F12FD919EAE86A`). Other versions (2.0.0, or the base game without its update) are not supported: the start
> screen tells you so if you pick them. Update the game to 2.0.2 on your Switch and dump it again.

> This project was made largely with AI (Anthropic's Claude, through Claude Code), directed and tested by a human.

![The launcher](docs/launcher.png)

## Playing on Windows

1. Download `LZA-Recomp-ZA-windows-v<version>.zip` from the [releases](https://gitlab.com/mr.wifi1143/LZA-Recomp/-/releases)
   and unzip it somewhere you can write to, on a short path (e.g. `C:\Games\`; not Program Files, not a deeply nested folder).
2. Put your dumped game files in one folder: the base game and the 2.0.2 update (`.nsp` / `.xci`, below).
3. Run `ZARecomp.exe` (not code-signed: "More info" -> "Run anyway" if Windows asks), click **Set up game** ->
   **Select game folder...** and pick that folder.
4. Wait while the game files are read and the game code is generated and compiled for your PC. This happens once,
   and takes from several minutes on a big CPU to much longer on a small one.
5. Click **Play**. The first start also prepares the shaders once (a progress bar).

Requirements: Windows 10/11 64-bit, a CPU with AVX2 (Intel 2013+ / AMD Zen+), a GPU with Vulkan 1.3 and a current
driver, ~25 GB free disk space during setup.

## Playing on Linux

One line in a terminal (Arch / Manjaro / EndeavourOS, Debian 13+ / Ubuntu 25.04+ and their derivatives, Fedora): it
downloads this branch, installs what it needs (asks for your password only if something is missing), builds the
program and starts it.

```sh
git clone -b LegendsZA https://gitlab.com/mr.wifi1143/LZA-Recomp.git PLZA-Recomp && cd PLZA-Recomp && ./tools/linux/install.sh && ./play.sh
```

In the launcher click **Set up game** -> **Select game folder...** and pick the folder with your dumped `.nsp` / `.xci` files
(below); the game code is built for your PC once. Later, start it again with `./play.sh` in that folder. To update:
`git pull && ./tools/linux/install.sh`.


Requirements: x86-64 with AVX2, a GPU with a Vulkan 1.3 driver (Mesa or NVIDIA), ~25 GB free during setup.

## Your game files

What you need, all from your own Switch:

- the base game and the **2.0.2 update**, dumped as `.nsp` (or `.xci` for the game card)
- `prod.keys` (dumped with Lockpick_RCM)
- optional: the Mega Dimension DLC `.nsp`, if you own it

Put the `.nsp` / `.xci` files in one folder and pick it in the launcher (**Game Files**). The recomp reads them itself, like the
emulators do: it applies the 2.0.2 update over the base game, writes only what it needs (the game code, one RomFS
image, the DLC's data) and never changes your dumps. It looks for `prod.keys` in that folder first, then in `.switch`
in your home folder and in the emulators' key folders (Eden, Yuzu and its forks, Ryujinx). Title keys come from the
tickets inside the `.nsp` files, or from a `title.keys` beside `prod.keys`.

Compressed dumps (`.nsz` / `.xcz`) aren't read: decompress them first (`nsz -D`). If several updates are in the
folder, the newest one that is 2.0.2 is used; the base game alone or another update won't work (the launcher
says "This needs Pokémon Legends: Z-A version 2.0.2").

**Mega Dimension (the DLC, optional).** Not needed to play. If its `.nsp` is in the folder, the game sees the DLC as
on a Switch that owns it (its only file is an empty `keyfile.dat`; what it unlocks ships in the update). Without
it, the game runs as without the DLC.

**Already extracted files** work too: pick the folder with `exefs` (`main`, `sdk`, `main.npdm`) and `romfs` (with
`arc/data.trpfs`), extracted with the 2.0.2 update applied (e.g. `hac2l -k prod.keys --titlekeys=title.keys
--basenca=<base program>.nca --exefsdir=game/exefs --romfsdir=game/romfs <update program>.nca`), plus an optional
`dlc` folder beside them with the DLC's RomFS.

Which update is a dump? `lasrecomp.log` lists the packages found with their versions: update v393216 (patch v6) is
**2.0.2**; v262144 (v4) is 2.0.0. The update's `control.nacp`
also holds the display version ("2.0.2"). The extracted `exefs/main` of 2.0.2 has the build id `B1F12FD919EAE86A`
(bytes 0x40..0x47 of the file).

Set up with other files before (2.0.0, say): the launcher shows the version message; pick the folder with the
2.0.2 files with **Select game folder...** and the game code is built again for it. Saves stay where they are (2.0.2
loads saves made with 2.0.0).

## Status

Playable through the opening into Lumiose City: boot, character creation, the intro, the station, Lumiose free roam.
Tested on Linux with an RTX 4090; the Windows build is tested under Wine.

- Frame rates: 30 (the game's own), 60, 120 or 144 fps, with the game, its UI and its cutscenes at the right speed
  (cutscenes follow real time, so they stay with their audio).
- Graphics: GPU-culled and instanced props (station facade, iron work, trees, hedges, lamp posts), cast shadows,
  SMAA. Resolution the game's own way: the Internal resolution option sets the screen size Z-A renders for (1440p,
  4K...: its scene at 0.65 of it, upscaled by its own upscaler, every pass at the new size); 1080p is the Switch's.
- NPCs animate every frame (the game's own animation level of detail is bypassed).
- Launcher, like an older PC game's: **Play**, **Options** (display, graphics, game: resolution, fullscreen,
  anti-aliasing, frame rate, FPS counter, button prompts... with this PC's recommended settings), **Controls**
  (controller test and remapping, keyboard remapping), **Game Files**, **Mods**. "Skip the launcher" goes straight
  to the game from then on (hold Shift while starting to see it again, or set Start Screen in the game's Settings).
  The older one-window start screen is kept for console ports: `LARECOMP_START_SCREEN=classic`.
- The game's own Settings screen (Menu -> Help & Settings -> Settings) has the launcher's settings too, as rows
  like the game's: frame rate, resolution, screen mode, anti-aliasing, button prompts and FPS counter on Gameplay,
  mouse camera, mouse sensitivity and stick dead zone on Camera, master volume on Sound. They apply at once (frame
  rate and resolution: from the next start) and are saved with the launcher's. Its Controls row opens the key
  bindings there: each action with its key and its controller button (the controller's own button icon); select one
  and press the new key on the keyboard or the new button on the controller (or click the key / the button).
- Button prompts for what you play with: the bound keys and mouse buttons, Xbox letters or PlayStation shapes instead
  of the Switch's (Auto follows the last used keyboard / mouse or controller, even when it changes while playing;
  remapped buttons show their new button).
- Keyboard and mouse: WASD (Left Ctrl walks), mouse look (click to take the mouse, Esc lets it go: the camera turns by
  as much as the mouse moves - the sensitivity is degrees per pixel - while a controller's stick still turns it the
  game's way), mouse buttons and wheel bindable like keys. The game's menus work with the keyboard and the mouse:
  pointing at a button moves the game's cursor to it, a click presses it (on the Settings screen: the choice, the
  point on a bar or the tab clicked), the right button goes back, the wheel scrolls; where there's nothing to point
  at (a message, a tip) a click goes on.
- PKHeX: saves are the Switch's own files (`save/save_0/main`), so PKHeX opens them (close the game first). Settings
  -> Gameplay -> **PKHeX Link** lets PKHeX's LiveHeX plugin (PKHeX-Plugins) connect to the running game and read and
  write your boxes, as it does with sys-botbase on a Switch: in LiveHeX, IP `127.0.0.1` (Network: this PC's address),
  port 6000. **Pokémon Files** (A on it): Export saves every Pokémon in your boxes as a `.pa9` file in
  `pokemon/export/<date>/`; Import puts the `.pa9` files in `pokemon/import/` into the first empty box slots (placed
  files move to `pokemon/import/done/`). Save in the game afterwards to keep imported Pokémon.

Known issues:

- Lumiose free roam (1440p, Draw Distance Max, Ultra shadows; RTX 4090, i9-12900K): about 180 fps uncapped (1% low
  ~126). On CPUs with 6+ fast cores the game gets up to three job threads more than the Switch's three
  (`extra_workers = auto`, the default; `0` keeps the Switch's three: about 167 fps there).
- The Unlimited frame rate is hidden for now: battles were reported slow at it (enemies late to pick a move, a switched
  Pokémon not sent out) and are being retested; 30-144 fps are not affected.
- On a fresh install, shaders and pipelines compile the first time they are seen (short stutters).
- Expect other missing effects and bugs.

## Building from source (Linux, by hand)

What the script does, step by step. Requirements: clang + lld, SDL 3, FFmpeg, shaderc, the Vulkan loader, Python 3.

```sh
python3 -m venv .venv && .venv/bin/pip install lz4 capstone unicorn
make -j$(nproc) all SHADERC=-lshaderc_shared  # build/lasrecomp (Debian / Ubuntu: SHADERC=-lshaderc)
mkdir -p extracted && ./play.sh               # the launcher sets up your game files and builds the game code
```

For development, with the game code built by the Makefile instead: your own Z-A 2.0.2 files in extracted/exefs
(main, sdk, main.npdm) and extracted/romfs, then

```sh
make gen                                      # translate the game code into generated/
make -j$(nproc) all SHADERC=-lshaderc_shared  # build/lasrecomp + build/lasgame.so
cc -O2 -Iruntime tools/romfs/mkromfs.c runtime/romfs_build.c -o build/mkromfs
./build/mkromfs extracted/romfs extracted/romfs.bin
./play.sh                                     # launcher, then the game (--play skips it)
```

`SHADERC=-lshaderc_shared` is for distributions that ship only the shared shaderc library, such as Arch.

`make gen` stops with a message on a 2.0.0 `main`. The runtime's Z-A addresses (hooks, patched data, the comments
that cite them) are 2.0.2's. For a future update, `tools/port_addrs.py` finds each of them in the new `main` and can
rewrite the Z-A sources in place (its header says how, and which addresses needed a hand between 2.0.2 and 2.0.0).

The Windows release is cross-built from Linux with zig: see `tools/windows/README.md`.

ARM Linux (experimental; tested on a Steam Frame: SteamOS, Snapdragon, Adreno 750 with Mesa's Turnip) builds the same
way on the device: `make gen` there writes AArch64-flavoured game code (the game's FP / SIMD as inline assembly). SteamOS
has no SDL 3 package: build SDL 3 from source into a prefix and pass it as `DEPS=<prefix>` (and put `<prefix>/lib` in
`LD_LIBRARY_PATH` to run). Adreno needs no settings: its driver can't map the game's memory, so the game's heap is
made of GPU memory there, and its 64-lane subgroups run the game's 32-lane warp code in slices (see `CHANGELOG.md`).
Headsets and handhelds heat up fast: a first run compiles thousands of shaders on every core.

Never commit or share `extracted/`, `generated/`, `build/` (the built game library is recompiled game code), `save/`
or anything else that comes from the game. See `CONTRIBUTING.md`.

## Credits and license

Built on [letsgo-recomp](https://github.com/raulzrrs/letsgo-recomp) (Pokémon: Let's Go) by raulzrrs, itself a fork of
[LAS Recomp](https://github.com/peiikai/LASRecomp) (Link's Awakening) by Peiikai. MIT licensed: `LICENSE` and
`LICENSE-LASRecomp`. Third-party components keep their own licenses: `THIRD-PARTY-NOTICES.md`.

Pokémon Legends: Z-A is © Nintendo / Creatures Inc. / GAME FREAK inc. / The Pokémon Company. This project is not
affiliated with or endorsed by them.
