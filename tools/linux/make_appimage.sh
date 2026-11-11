#!/bin/bash
# tools/linux/make_appimage.sh: the portable Linux release of PL:Z-A Recomp as one AppImage, runnable on other distros
# (glibc 2.31+, x86-64 with AVX2, a Vulkan 1.3 driver). Everything is built with zig for an old glibc, so nothing from
# the build machine's distro is needed at run time except the GPU driver's Vulkan loader.
#
# It holds the runtime (launcher + game), its libraries (SDL 3, FFmpeg LGPL, shaderc), the plugins (Steam relay and
# its helper, Legends Lua) with the Lua research scripts as examples, and what the launcher needs to build the game code
# on the player's PC from their own dump (runtime/gamebuild.c): a portable Python with the generator and zig.
# No game files, no keys, no game code: the script refuses to finish if any ended up in it.
#
# LZA_LINUXBUILD (one folder, like the Windows build's LZA_WINBUILD):
#   toolchain/zig/      zig 0.17.0 for Linux x86-64 (the compiler of the build and the one shipped to players)
#   prefix/             SDL 3, FFmpeg, shaderc built with zig for glibc 2.31 (lib/, include/ + the Vulkan headers),
#                       and lib/libvulkan.so: a link-only stub (the player's own loader is used at run time)
#   bin/zcc, bin/zcxx   zig cc / c++ for x86_64-linux-gnu.2.31 (-march=<cpu> as zig's -mcpu)
#   dl/python.tar.gz    python-build-standalone CPython 3.12, x86_64-unknown-linux-gnu, install_only_stripped
#   dl/wheels/          capstone 5.0.9, unicorn 2.1.4, lz4 4.4.5 manylinux wheels (cp312)
#   dl/appimagetool     appimagetool-x86_64.AppImage
# Run from the repository root:  LZA_LINUXBUILD=/path tools/linux/make_appimage.sh  ->  $LZA_LINUXBUILD/dist/
set -euo pipefail
L=${LZA_LINUXBUILD:?set LZA_LINUXBUILD (see the top of this script)}
R=$PWD
V=$(sed -n 's/^#define RT_APP_VERSION "\(.*\)".*/\1/p' runtime/paths.h)
OUT=$L/dist/ZARecomp-$V-x86_64.AppImage
A=$L/AppDir
O=$A/opt/zarecomp
BB=build/appimage
export PATH=$L/bin:$PATH
say() { printf '\033[1m==> %s\033[0m\n' "$*"; }

say "Runtime and plugins (zig, glibc 2.31)"
make -j"$(nproc)" all CC=zcc CXX=zcxx DEPS="$L/prefix" SHADERC=-lshaderc_shared B=$BB > "$L/make-runtime.log" 2>&1 ||
    { grep -E "error" "$L/make-runtime.log" | head -20; exit 1; }
make -j"$(nproc)" CC=zcc DEPS="$L/prefix" B=$BB steamrelay legendslua > "$L/make-plugins.log" 2>&1 ||
    { grep -E "error" "$L/make-plugins.log" | head -20; exit 1; }
[ ! -e $BB/lasgame.so ] || { echo "refusing: $BB/lasgame.so (game code) exists in the AppImage build folder"; exit 1; }

say "AppDir"
rm -rf "$A" && mkdir -p "$O/lib" "$O/plugins" "$O/licenses"
cp $BB/lasrecomp "$O/"
patchelf --set-rpath '$ORIGIN/lib' "$O/lasrecomp"
# the libraries it needs, theirs too (not the Vulkan loader: the GPU driver's own)
cp -P "$L"/prefix/lib/libSDL3.so* "$L"/prefix/lib/libav{codec,format,util}.so* "$L"/prefix/lib/libsw{scale,resample}.so* \
      "$L"/prefix/lib/libshaderc_shared.so* "$O/lib/"
for f in "$O"/lib/*.so*; do [ -L "$f" ] || { strip --strip-unneeded "$f"; patchelf --set-rpath '$ORIGIN' "$f"; }; done
cp $BB/plugins/liblegendslua.so $BB/plugins/libsteamrelay.so $BB/plugins/steamrelay-helper "$O/plugins/"
strip --strip-unneeded "$O"/plugins/*
mkdir -p "$O/lua-examples" && cp contrib/legendslua/research/*.lua contrib/legendslua/README.md "$O/lua-examples/"

# the game code generator: portable Python + capstone / unicorn / lz4 + tools/gen.py and its modules (runtime/gamebuild.c
# looks for python/python and gen/gen.py beside the program)
mkdir -p "$O/python" "$O/gen"
tar xf "$L/dl/python.tar.gz" -C "$O/python" --strip-components=1
PYV=$(ls "$O/python/lib" | grep -m1 -E '^python3\.[0-9]+$')
SP=$O/python/lib/$PYV/site-packages
for w in "$L"/dl/wheels/*.whl; do (cd "$SP" && python3 -m zipfile -e "$w" .); done
rm -rf "$O/python/include" "$O/python/share" "$O/python/lib/pkgconfig" "$O/python/lib/$PYV/test" \
       "$O/python/lib/$PYV/idlelib" "$O/python/lib/$PYV/tkinter" "$O/python/lib/$PYV/ensurepip" \
       "$O/python/lib/$PYV/lib2to3" "$O/python/lib/$PYV/turtledemo" "$O/python/lib/$PYV/site-packages/pip"* \
       "$SP/unicorn/include" "$SP/capstone/include" "$O/python/lib/libtcl"* "$O/python/lib/libtk"* "$O/python/lib/tcl"* \
       "$O/python/lib/tk"* "$O/python/lib/itcl"* "$O/python/lib/thread"*
find "$O/python" -name '*.a' -delete
find "$O/python" -name __pycache__ -prune -exec rm -rf {} +
ln -s "bin/$PYV" "$O/python/python"
cp tools/{gen,lifter,analyze,nso,passes,patches,simd_c}.py "$O/gen/"
"$O/python/python" -I -c 'import capstone, unicorn, lz4.block; print("generator python ok:", capstone.__version__, unicorn.__version__)'

# the compiler: zig, only what compiling C for x86-64 Linux (glibc) needs
cp -a "$L/toolchain/zig" "$O/zig"
Z=$O/zig/lib
for d in "$Z"/libc/include/*; do
    case "$(basename "$d")" in generic-glibc|any-linux-any|x86-linux-any|x86_64-linux-any|x86_64-linux-gnu) ;; *) rm -rf "$d" ;; esac
done
for d in "$Z"/libc/*; do case "$(basename "$d")" in include|glibc) ;; *) rm -rf "$d" ;; esac; done
rm -rf "$Z/libcxx" "$Z/libcxxabi" "$Z/libtsan" "$Z/libunwind" "$Z/docs" "$Z/build-web" "$Z/init" "$O/zig/doc" "$O/zig/README.md"

# what the game code is compiled with (runtime/gamebuild.c), the profile of zig's clang family (Makefile PGO)
mkdir -p "$O/gamelib-src"
cp runtime/{cpu.h,simd.h,simd_fp.h,gamelib.h} tools/gamelib_entry.c runtime/hle/hle_symbols.txt "$O/gamelib-src/"
zstd -q -d -f data/pgo/za.clang21.profdata.zst -o "$O/gamelib-src/pgo.profdata"
cp docs/icon/icon_1024.png "$O/icon.png"
cp third_party/fonts/Baloo2/Baloo2-SemiBold.ttf data/shader_manifest.txt "$O/"

# licenses: this project's, then every bundled component's
cp LICENSE "$O/LICENSE.txt"
cp LICENSE-LASRecomp "$O/licenses/LICENSE-LASRecomp.txt"
cp THIRD-PARTY-NOTICES.md "$O/licenses/THIRD-PARTY-NOTICES.txt"
cp third_party/licenses/* "$O/licenses/"
cp third_party/imgui/LICENSE.txt "$O/licenses/imgui-LICENSE.txt"
cp third_party/vma/LICENSE.txt "$O/licenses/VulkanMemoryAllocator-LICENSE.txt"
cp third_party/smaa/LICENSE.txt "$O/licenses/SMAA-LICENSE.txt"
cp third_party/fonts/Baloo2/OFL.txt "$O/licenses/Baloo2-OFL.txt"
cp "$O/zig/LICENSE" "$O/licenses/zig-LICENSE.txt"
cp "$O/python/lib/$PYV/LICENSE.txt" "$O/licenses/Python-LICENSE.txt" 2>/dev/null || true
cp "$SP"/capstone-*.dist-info/LICENSE* "$O/licenses/capstone-LICENSE.txt" 2>/dev/null || true
cp "$SP"/lz4-*.dist-info/licenses/LICENSE "$O/licenses/python-lz4-LICENSE.txt" 2>/dev/null || true
cp third_party/licenses/GPL-2.0.txt "$O/licenses/unicorn-COPYING-GPL-2.0.txt"
for f in "$L"/deps-src/SDL3-*/LICENSE.txt; do [ -f "$f" ] && cp "$f" "$O/licenses/SDL3-LICENSE.txt"; done
for f in "$L"/deps-src/FFmpeg-*/COPYING.LGPLv2.1; do [ -f "$f" ] && cp "$f" "$O/licenses/FFmpeg-COPYING.LGPLv2.1.txt"; done
for f in "$L"/deps-src/shaderc-*/LICENSE; do [ -f "$f" ] && cp "$f" "$O/licenses/shaderc-LICENSE.txt"; done
cp tools/linux/README-AppImage.md "$O/README.md"

# the AppImage's own files
install -m755 tools/linux/AppRun "$A/AppRun"
if command -v magick >/dev/null; then magick docs/icon/icon_1024.png -resize 256x256 "$A/zarecomp.png"; else cp docs/icon/icon_1024.png "$A/zarecomp.png"; fi
ln -sf zarecomp.png "$A/.DirIcon"
cat > "$A/zarecomp.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=PL:Z-A Recomp
GenericName=Pokémon Legends: Z-A (static recompilation)
Comment=Native PC port of Pokémon Legends: Z-A 2.0.2 - bring your own dump
Exec=AppRun %U
Icon=zarecomp
Terminal=false
Categories=Game;
StartupWMClass=lasrecomp
X-AppImage-Version=$V
EOF

# nothing from the game: no game code library, no game files, no keys
if find "$A" -iname 'lasgame*' -o -iname '*.nso' -o -iname '*.nca' -o -iname '*.nsp' -o -iname '*.xci' \
    -o -iname 'prod.keys' -o -iname 'title.keys' -o -name exefs -o -name romfs -o -name romfs.bin | grep .; then
    echo "refusing: game files in the AppDir (above)"; exit 1
fi

say "AppImage"
mkdir -p "$L/dist"
chmod +x "$L/dl/appimagetool"
ARCH=x86_64 "$L/dl/appimagetool" --appimage-extract-and-run -n --comp zstd "$A" "$OUT" > "$L/appimagetool.log" 2>&1 ||
    { tail -20 "$L/appimagetool.log"; exit 1; }
du -sh "$A" "$OUT"
echo "$OUT"
