# PL:Z-A Recomp — Linux AppImage

A native Linux port of **Pokémon Legends: Z-A** (Nintendo Switch, **version 2.0.2**) by static recompilation, in one
file that runs on most distributions. **No game files, keys or game code are included**: you bring your own dump,
and the game code is generated and compiled on your PC once, inside the launcher.

## Requirements

- x86-64 CPU with AVX2 (Intel Haswell / 2013 or newer, AMD Zen or newer)
- A GPU with a Vulkan 1.3 driver (Mesa RADV / ANV / NVK or NVIDIA) and the Vulkan loader (`libvulkan.so.1`)
- glibc 2.31 or newer (Ubuntu 20.04+, Debian 11+, Fedora 32+, Arch, SteamOS 3, openSUSE Leap 15.3+...)
- About 25 GB free in the data folder during setup (the game's data image is ~6 GB afterwards)

Nothing else: Python, the compiler (zig), SDL 3, FFmpeg and shaderc are inside.

## Start

```sh
chmod +x ZARecomp-*-x86_64.AppImage
./ZARecomp-*-x86_64.AppImage            # the launcher
./ZARecomp-*-x86_64.AppImage --install  # optional: menu entry (copies it to ~/Applications)
```

If it says FUSE is missing: install `fuse3` (or `fuse`/`libfuse2`), or run it with `--appimage-extract-and-run`.

## Set up the game (once)

In the launcher, **Game Files**:

1. **Base game** → Browse… → your Pokémon Legends: Z-A `.nsp` / `.xci` (1.0.0)
2. **Game update** → Browse… → the **2.0.2** update `.nsp` (v393216)
3. **Mega Dimension DLC** (optional) → its `.nsp`
4. **prod.keys** → Browse… → your keys (dumped from your own Switch with Lockpick_RCM). If one is found in the usual
   places (beside the base game, `~/.switch`, Eden / Yuzu / Ryujinx key folders) it is shown and used.
5. **Recompile the game**: the packages are read with your keys (2.0.2 applied over the base game), the game data image
   is written, and the game code is generated and compiled for this PC (several minutes on a big CPU, longer on a
   small one). Then **Play**. The first start also prepares the shaders once.

Already extracted files work too: **Select game folder…** and pick the folder holding `exefs` (`main`, `sdk`,
`main.npdm`) and `romfs` (`arc/data.trpfs`) — or a flat folder with both, as hactool / an emulator dump writes them.

**Recompile game code** (beside "ready to play") builds the game code again from the files already set up. After an
update of the AppImage the launcher does this by itself when it is needed.

### Mega Dimension DLC

Pick the DLC's `.nsp` / `.xci` in the **Mega Dimension DLC** row. With base + update packages it is read along with them;
when the game is set up already (any way, extracted files too) **Add the DLC picked above** reads it with your prod.keys
and adds it (`dlc.bin` in the data folder) - no recompile; it is active from the next start of the game.

## Where things are

The data folder (game files, saves, settings, shader caches, mods, `lasrecomp.log`):

- `~/.local/share/zarecomp` by default (`--data-dir` prints it)
- portable: make a folder named `ZARecomp-data` next to the AppImage and everything goes there instead
- or set `LARECOMP_DATA_ROOT=/some/folder`

Saves are the Switch's own files (`save/save_0/main`): PKHeX opens them.

## Included features and mods

Everything from the `LegendsZA` branch of LZA-Recomp (1.1.0-rc1):

- Launcher: Play, Options (display / graphics / game), Controls (controller test and remapping, keyboard and mouse
  bindings), Game Files, Mods; recommended settings for your PC; "Skip the launcher" (hold Shift to see it again)
- Frame rates 30 / 60 / 72 / 120 / 144 / Unlimited, all offered (no "Experimental" switch any more): scripted scenes play
  at 60 fps on their own, and at Unlimited battles are held at 144 fps on their own, then the chosen rate comes back
- Resolution the game's own way (1440p, 4K…), SMAA, shadows up to 4096, draw distance for people and detail,
  smooth shadow edges, shader loading Wait / Don't wait, FPS counter
- The game's own Settings screen with the launcher's settings, key bindings with button icons
- Keyboard and mouse play, mouse in menus, Xbox / PlayStation / keyboard button prompts
- PKHeX Link (LiveHeX on port 6000) and Pokémon Files export / import (`.pa9`)
- Mods page: translations and texture packs (`.zip` / `.rar` / `.7z`, several at once), plugins:
  - **Steam relay**: Link Trades and Private Battles with Steam friends (off until switched on)
  - **Legends Lua**: Lua scripting (off until switched on; scripts in the data folder's `lua/`; the research scripts are
    in `lua-examples/` as examples)
- **Cheats** (launcher page *Cheats*, and in game with **F2** or **Minus + R**): Atmosphère-format cheat codes (as EdiZon /
  Breeze use them), run on the game's memory about 12 times a second like on a Switch. **Download the GBAtemp cheat
  database** fetches the newest zip of the "Pokemon Legends Z-A Cheat Database" thread (curl or wget needed) and installs
  the 2.0.2 file (`cheats/B1F12FD919EAE86A.txt` + `notes.txt`); **Import a cheat file…** takes any `.txt` or `.zip`.
  Most Z-A cheats rewrite the game's instructions: the game code is compiled with every instruction the cheat file can
  put there (the launcher offers **Recompile game code** once, a few minutes), and from then on they switch on and off
  live; switching one off puts the game's own code back (no "Restore Code" needed). Data cheats work at once.
- **FSR / sharpening**: Options → Graphics → *Upscaler* (AMD FSR 1: EASU upscale + RCAS, or Bilinear) applies when the
  Resolution is below the window's (e.g. 1080p on a 1440p screen); *Sharpening* (AMD RCAS: Off / Low / Medium / High)
  at the window's resolution. Both are in the game's own Settings too.
- Local wireless play between copies (Link Trades, Private Battles), the Mega Dimension DLC when its `.nsp` is picked

## Licenses

`licenses/` inside the AppImage (`--appimage-extract` to read them). The project is MIT; Unicorn (used only by the
game code generator) is GPL 2.0, FFmpeg is LGPL 2.1 (separate shared libraries).
