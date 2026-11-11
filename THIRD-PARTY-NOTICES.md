# Third-party notices

LZ&A Recomp's own code is MIT licensed (`LICENSE`, `LICENSE-LASRecomp`). It uses, and the Windows release ships, the
components below. Each keeps its own license. The full license texts are in `third_party/` (source components) and,
in the Windows release, in the `licenses/` folder next to the program.

## Compiled into the program (source in `third_party/`)

| Component | Version | License | Copyright |
|---|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.91.9 | MIT | Omar Cornut |
| [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | 3.3.0 | MIT | Advanced Micro Devices, Inc. |
| [miniz](https://github.com/richgel999/miniz) (texture packs' PNG reading) | 11.0.2 | MIT | Rich Geldreich, RAD Game Tools, Valve Software |
| [SMAA](https://github.com/iryoku/smaa) | - | MIT | Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, Diego Gutierrez |
| [Baloo 2](https://github.com/EkType/Baloo2) font | - | SIL Open Font License 1.1 | The Baloo 2 Project Authors |
| [Lua](https://www.lua.org) (in the Lua scripting plugin, `contrib/legendslua`) | 5.3.6 | MIT | Lua.org, PUC-Rio |

## Libraries shipped with the Windows release (unmodified, dynamically linked)

| Component | Version | License | Source |
|---|---|---|---|
| [SDL 3](https://libsdl.org) (`SDL3.dll`) | 3.4.18 | zlib | https://github.com/libsdl-org/SDL |
| [FFmpeg](https://ffmpeg.org) (`avcodec`, `avformat`, `avutil`, `swresample`, `swscale` DLLs) | 9.0.2 | LGPL 2.1 or later | https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz |
| [shaderc](https://github.com/google/shaderc) (`libshaderc_shared.dll`, with glslang and SPIRV-Tools) | 2026.4 (glslang 16.6.0, SPIRV-Tools 1.4.363.0) | Apache 2.0; glslang: BSD-3-Clause and others (see its license) | https://github.com/google/shaderc, https://github.com/KhronosGroup/glslang, https://github.com/KhronosGroup/SPIRV-Tools |
| [GNU libiconv](https://www.gnu.org/software/libiconv/) (`libiconv-2.dll`) | 1.19 | LGPL 2.1 or later | https://ftp.gnu.org/pub/gnu/libiconv/libiconv-1.19.tar.gz |

FFmpeg was built from the unmodified 9.0.2 release as LGPL (no `--enable-gpl`, no `--enable-nonfree`), configured with:

```
--enable-cross-compile --target-os=mingw32 --arch=x86_64 --enable-shared --disable-static --disable-x86asm
--disable-autodetect --disable-programs --disable-doc --disable-network --disable-avdevice --disable-avfilter
--disable-everything --enable-protocol=file --enable-demuxer='matroska,mov'
--enable-decoder='vp9,vp8,vorbis,opus,h264,aac' --enable-parser='vp9,vp8,vorbis,opus,h264,aac'
```

SDL 3, shaderc and libiconv are the MSYS2 UCRT64 builds (`mingw-w64-ucrt-x86_64-*` packages; their sources are at
https://github.com/msys2/MINGW-packages). The LGPL libraries are separate DLLs: you may replace them with your own
builds of the same version.

## The game code builder shipped with the Windows release

The Windows release builds the game code on the player's PC from their own game files. For that it ships:

| Component | Version | License | Source |
|---|---|---|---|
| [Python](https://www.python.org) (embeddable package, `python/`) | 3.12.10 | PSF License 2.0 (and the licenses of its parts: OpenSSL Apache 2.0, libffi MIT, SQLite public domain...; `python/LICENSE.txt`) | https://www.python.org/downloads/release/python-31210/ |
| [Capstone](https://www.capstone-engine.org) (Python package) | 5.0.9 | BSD-3-Clause | https://github.com/capstone-engine/capstone |
| [Unicorn](https://www.unicorn-engine.org) (Python package) | 2.1.4 | GPL 2.0 | https://github.com/unicorn-engine/unicorn/releases/tag/2.1.4 |
| [python-lz4](https://github.com/python-lz4/python-lz4) (with LZ4) | 4.4.5 | BSD-3-Clause (LZ4: BSD-2-Clause) | https://github.com/python-lz4/python-lz4 |
| [Zig](https://ziglang.org) (`zig/`, used as a C compiler; includes LLVM / Clang / LLD and MinGW-w64) | 0.17.0 | MIT (LLVM: Apache 2.0 with LLVM exception; MinGW-w64: ZPL / public domain and others) | https://ziglang.org/download/ |

Unicorn is GPL 2.0 licensed. It is shipped unmodified (the official 2.1.4 wheel from PyPI), used only by the game
code generator (`gen/gen.py`) while the game code is built, never by the game itself. Its complete corresponding
source code is at the link above (tag 2.1.4); the GPL 2.0 text is in `licenses/`. The generator's own source is in
this repository.

## Not included

No part of any Nintendo / The Pokémon Company / Game Freak game is included: no game files, no keys, no game code
(recompiled or not). Players supply their own dump; the game code is generated from it on their own PC.

## The Linux AppImage (tools/linux/build_appimage.sh)

Built from the unmodified sources by tools/linux/build_appimage.sh (each download pinned by its SHA-256 there) and
shipped as shared libraries in `opt/zarecomp/lib/`: SDL 3.4.18 (zlib), FFmpeg 9.0.2 as LGPL with the configure line
above but for Linux (`--enable-shared --disable-static --disable-x86asm ...`), shaderc 2026.4 with glibc-free
glslang / SPIRV-Tools of the commits in that script (Apache 2.0 / BSD). The game code builder ships zig 0.17.0 for
Linux (trimmed to compiling C for x86-64 Linux), python-build-standalone CPython 3.12.16 (release 20261110, PSF license
and its parts' licenses) and the capstone 5.0.9, unicorn 2.1.4 (GPL 2.0) and lz4 4.4.5 manylinux wheels from PyPI.
The AppImage runtime is appimagetool's (MIT). Every license text is in `opt/zarecomp/licenses/`.
