#!/bin/bash
# tools/linux/build_appimage.sh: the PL:Z-A Recomp AppImage from this repository, in one command.
#
#   tools/linux/build_appimage.sh            ->  $LZA_LINUXBUILD/dist/ZARecomp-<version>-x86_64.AppImage
#
# Everything that isn't in this repository goes to one build folder, LZA_LINUXBUILD (default:
# ~/.cache/plza-appimage-build, ~3 GB): pinned downloads (checked against their SHA-256), zig 0.17.0 as the compiler
# (for x86_64-linux-gnu.2.31: the AppImage runs on glibc 2.31+ distros), SDL 3 / FFmpeg (LGPL) / shaderc built with it,
# a portable Python with the game code generator's modules, and appimagetool. Then tools/linux/make_appimage.sh packs
# the AppImage. Steps already done are skipped; FORCE=1 rebuilds the libraries. No game files are used or included.
#
# Needs on the build machine (the distribution's packages): bash, curl, tar, xz, zstd, unzip, make, cmake, ninja,
# pkg-config, python3, patchelf, binutils (nm, strip, objdump), wayland-scanner and the development headers SDL 3 builds
# its Linux backends against (X11 + Xext/Xrandr/Xcursor/Xi/Xfixes/Xss, Wayland, xkbcommon, libdecor, PipeWire,
# PulseAudio, ALSA, D-Bus, udev), the Vulkan headers and loader (libvulkan.so.1). README.md lists them per distro.
set -euo pipefail
cd "$(dirname "$0")/../.."
R=$PWD
L=${LZA_LINUXBUILD:-$HOME/.cache/plza-appimage-build}
export LZA_LINUXBUILD=$L
J=${JOBS:-$(nproc)}
say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { printf '\033[1;31mError: %s\033[0m\n' "$*" >&2; exit 1; }
mkdir -p "$L/dl/wheels" "$L/bin" "$L/toolchain" "$L/deps-src" "$L/prefix/lib" "$L/prefix/include"

[ "$(uname -m)" = x86_64 ] || die "builds on x86-64 Linux only"
for t in curl tar xz zstd unzip make cmake ninja pkg-config python3 patchelf nm strip wayland-scanner; do
    command -v $t >/dev/null || die "missing: $t (README.md: Building)"
done
VKLIB=$( (ldconfig -p 2>/dev/null || true) | awk '/libvulkan\.so\.1 .*x86-64/ && !v {v = $NF} END {print v}')
for c in /usr/lib/x86_64-linux-gnu/libvulkan.so.1 /usr/lib64/libvulkan.so.1 /usr/lib/libvulkan.so.1; do
    [ -n "$VKLIB" ] || { [ -e "$c" ] && VKLIB=$c; }
done
[ -n "$VKLIB" ] || die "missing: the Vulkan loader (libvulkan.so.1)"
[ -f /usr/include/vulkan/vulkan.h ] || die "missing: the Vulkan headers (/usr/include/vulkan/vulkan.h)"

# ---- downloads (name, url, sha256; "-" = not checked: appimagetool's continuous build)
say "Downloads ($L/dl)"
fetch() {
    local f=$L/dl/$1 url=$2 sum=$3
    if [ ! -s "$f" ]; then
        curl -fL --retry 5 -o "$f.part" "$url" || die "download failed: $url"
        mv "$f.part" "$f"
    fi
    if [ "$sum" != - ] && ! echo "$sum  $f" | sha256sum -c --quiet - 2>/dev/null; then
        rm -f "$f"; die "checksum mismatch: $1 (removed; run again)"
    fi
}
fetch zig.tar.xz https://ziglang.org/download/0.17.0/zig-x86_64-linux-0.17.0.tar.xz 1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026
fetch ffmpeg.tar.gz https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n9.0.2.tar.gz 6e374ed621e48faa40639307dff48ba6fe574a509977956d2cce9669b7cc27e9
fetch sdl3.tar.gz https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz 9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
fetch shaderc.tar.gz https://github.com/google/shaderc/archive/refs/tags/v2026.4.tar.gz f06ce5bcca94e5df7f34e115743597d0ad2e13c5fe9213c67dc8c76031241947
GLSLANG=e1b562a8bed273a02f30b59b66a5d499793cede5 SPVT=ef96ed763b43b59b33b31b362f09a02b729fa1c9 SPVH=04fd3caa1e8267e4d95c806cad901181728e1006
fetch glslang.tar.gz https://github.com/KhronosGroup/glslang/archive/$GLSLANG.tar.gz 907174a24713c6202c146f164bf81783f1fbc79c8cb821a30f18f159eb980312
fetch SPIRV-Tools.tar.gz https://github.com/KhronosGroup/SPIRV-Tools/archive/$SPVT.tar.gz 82c62146083fd558735a3171cf97cfc47903ca7d368482e87f94bd44883c0f00
fetch SPIRV-Headers.tar.gz https://github.com/KhronosGroup/SPIRV-Headers/archive/$SPVH.tar.gz 392f4801409aad9c4f1b77745f179952fd6264e4c8bd0fc1bc45dfa6807bf6d0
fetch python.tar.gz https://github.com/astral-sh/python-build-standalone/releases/download/20261110/cpython-3.12.16%2B20261110-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz 6a9e62dad033790aef722dbd370e0ce76ce17b73627ba3ae679f537049350b39
fetch wheels/capstone-5.0.9-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl https://files.pythonhosted.org/packages/py3/c/capstone/capstone-5.0.9-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl 273fd8d747d2e35c88f91450be51a603ecfaafb00d96d9f315dcb8689c86193e
fetch wheels/unicorn-2.1.4-cp37-abi3-manylinux_2_17_x86_64.manylinux2014_x86_64.whl https://files.pythonhosted.org/packages/cp37/u/unicorn/unicorn-2.1.4-cp37-abi3-manylinux_2_17_x86_64.manylinux2014_x86_64.whl 9d6e6dea140560de4ebd8446661f7ef84a357d428c14a3ef09dacd306ec8c239
fetch wheels/lz4-4.4.5-cp312-cp312-manylinux2014_x86_64.manylinux_2_17_x86_64.manylinux_2_28_x86_64.whl https://files.pythonhosted.org/packages/cp312/l/lz4/lz4-4.4.5-cp312-cp312-manylinux2014_x86_64.manylinux_2_17_x86_64.manylinux_2_28_x86_64.whl 24092635f47538b392c4eaeff14c7270d2c8e806bf4be2a6446a378591c5e69e
fetch appimagetool https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage -
chmod +x "$L/dl/appimagetool"

# ---- zig and its wrappers: C / C++ for x86_64-linux-gnu.2.31 (clang's -march=<cpu> as zig's -mcpu=<cpu>)
if [ ! -x "$L/toolchain/zig/zig" ]; then
    say "zig 0.17.0"
    rm -rf "$L/toolchain/zig" && tar xf "$L/dl/zig.tar.xz" -C "$L/toolchain" && mv "$L"/toolchain/zig-x86_64-linux-0.17.0 "$L/toolchain/zig"
fi
for w in cc c++; do
    n=$([ $w = cc ] && echo zcc || echo zcxx)
    cat > "$L/bin/$n" <<EOF
#!/bin/bash
a=()
for x in "\$@"; do case \$x in -march=*) c=\${x#-march=}; a+=("-mcpu=\${c//-/_}") ;; -fuse-ld=lld) ;; *) a+=("\$x") ;; esac; done
exec "$L/toolchain/zig/zig" $w -target x86_64-linux-gnu.2.31 -Wno-error=date-time "\${a[@]}" \${ZCC_EXTRA:-}
EOF
done
printf '#!/bin/sh\nexec "%s/toolchain/zig/zig" ar "$@"\n' "$L" > "$L/bin/zar"
printf '#!/bin/sh\nexec "%s/toolchain/zig/zig" ranlib "$@"\n' "$L" > "$L/bin/zranlib"
chmod +x "$L"/bin/*
export PATH=$L/bin:$PATH

# ---- the libraries (into $L/prefix)
P=$L/prefix S=$L/deps-src
[ -n "${FORCE:-}" ] && rm -f "$P"/lib/libSDL3.so "$P"/lib/libavcodec.so "$P"/lib/libshaderc_shared.so
cd "$S"
if [ ! -f "$P/lib/libSDL3.so" ]; then
    say "SDL 3.4.18"
    rm -rf SDL3-3.4.18 build-sdl && tar xf "$L/dl/sdl3.tar.gz" && mkdir build-sdl && cd build-sdl
    export ZCC_EXTRA="-idirafter /usr/include -idirafter /usr/lib/dbus-1.0/include -idirafter /usr/lib64/dbus-1.0/include -idirafter /usr/lib/x86_64-linux-gnu/dbus-1.0/include"
    cmake ../SDL3-3.4.18 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$P" \
        -DCMAKE_C_COMPILER=zcc -DCMAKE_CXX_COMPILER=zcxx -DCMAKE_AR="$L/bin/zar" -DCMAKE_RANLIB="$L/bin/zranlib" \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_TESTS=OFF \
        -DSDL_X11=ON -DSDL_X11_SHARED=ON -DSDL_WAYLAND=ON -DSDL_WAYLAND_SHARED=ON -DSDL_WAYLAND_LIBDECOR=ON \
        -DSDL_WAYLAND_LIBDECOR_SHARED=ON -DSDL_PIPEWIRE=ON -DSDL_PIPEWIRE_SHARED=ON -DSDL_PULSEAUDIO=ON \
        -DSDL_PULSEAUDIO_SHARED=ON -DSDL_ALSA=ON -DSDL_ALSA_SHARED=ON -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_DBUS=ON \
        -DSDL_IBUS=ON -DSDL_LIBUDEV=ON -DSDL_VULKAN=ON -DSDL_KMSDRM=OFF -DSDL_HIDAPI=ON -DSDL_HIDAPI_LIBUSB=OFF \
        -DSDL_RPATH=OFF > "$S/sdl-config.log" 2>&1 || { tail -30 "$S/sdl-config.log"; die "SDL 3 configure failed"; }
    ninja -j"$J" > "$S/sdl-build.log" 2>&1 || { tail -30 "$S/sdl-build.log"; die "SDL 3 build failed"; }
    ninja install > /dev/null
    unset ZCC_EXTRA
    cd "$S"
fi
if [ ! -f "$P/lib/libavcodec.so" ]; then
    say "FFmpeg 9.0.2 (LGPL, the decoders the game's movies use)"
    rm -rf FFmpeg-n9.0.2 && tar xf "$L/dl/ffmpeg.tar.gz" && cd FFmpeg-n9.0.2
    ./configure --prefix="$P" --cc=zcc --cxx=zcxx --ar=zar --ranlib=zranlib --enable-shared --disable-static \
        --disable-x86asm --disable-autodetect --disable-programs --disable-doc --disable-network --disable-avdevice \
        --disable-avfilter --disable-everything --enable-protocol=file --enable-demuxer='matroska,mov' \
        --enable-decoder='vp9,vp8,vorbis,opus,h264,aac' --enable-parser='vp9,vp8,vorbis,opus,h264,aac' --enable-pic \
        > "$S/ffmpeg-config.log" 2>&1 || { tail -30 "$S/ffmpeg-config.log"; die "FFmpeg configure failed"; }
    sed -i 's/#define HAVE_SYSCTL 1/#define HAVE_SYSCTL 0/' config.h  # (glibc 2.31's headers in zig have no sys/sysctl.h)
    make -j"$J" > "$S/ffmpeg-build.log" 2>&1 || { tail -30 "$S/ffmpeg-build.log"; die "FFmpeg build failed"; }
    make install > /dev/null
    cd "$S"
fi
if [ ! -f "$P/lib/libshaderc_shared.so" ]; then
    say "shaderc 2026.4 (glslang, SPIRV-Tools)"
    rm -rf shaderc-2026.4 glslang-$GLSLANG SPIRV-Tools-$SPVT SPIRV-Headers-$SPVH build-shaderc
    for f in shaderc glslang SPIRV-Tools SPIRV-Headers; do tar xf "$L/dl/$f.tar.gz"; done
    ln -sfn ../../glslang-$GLSLANG shaderc-2026.4/third_party/glslang
    ln -sfn ../../SPIRV-Tools-$SPVT shaderc-2026.4/third_party/spirv-tools
    ln -sfn ../../SPIRV-Headers-$SPVH shaderc-2026.4/third_party/spirv-headers
    mkdir build-shaderc && cd build-shaderc
    cmake ../shaderc-2026.4 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$P" \
        -DCMAKE_C_COMPILER=zcc -DCMAKE_CXX_COMPILER=zcxx -DCMAKE_AR="$L/bin/zar" -DCMAKE_RANLIB="$L/bin/zranlib" \
        -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -DSPIRV_SKIP_TESTS=ON \
        -DSPIRV_SKIP_EXECUTABLES=ON -DENABLE_GLSLANG_BINARIES=OFF -DSPIRV_TOOLS_BUILD_STATIC=ON -DBUILD_SHARED_LIBS=OFF \
        -DSPIRV_WERROR=OFF -DENABLE_CTEST=OFF -DSKIP_GLSLANG_INSTALL=ON -DSKIP_SPIRV_TOOLS_INSTALL=ON \
        > "$S/shaderc-config.log" 2>&1 || { tail -30 "$S/shaderc-config.log"; die "shaderc configure failed"; }
    ninja -j"$J" shaderc_shared > "$S/shaderc-build.log" 2>&1 || { tail -30 "$S/shaderc-build.log"; die "shaderc build failed"; }
    mkdir -p "$P/include/shaderc" && cp ../shaderc-2026.4/libshaderc/include/shaderc/*.h "$P/include/shaderc/"
    for f in $(find . -name 'libshaderc_shared.so*' \( -type f -o -type l \)); do cp -L "$f" "$P/lib/"; done
    [ -f "$P/lib/libshaderc_shared.so" ] || cp -L "$P"/lib/libshaderc_shared.so.* "$P/lib/libshaderc_shared.so"
    cd "$S"
fi
# the Vulkan headers, and a link-only libvulkan.so built for glibc 2.31 (the player's own loader is used at run time)
mkdir -p "$P/include" && cp -r /usr/include/vulkan "$P/include/" && { [ -d /usr/include/vk_video ] && cp -r /usr/include/vk_video "$P/include/" || true; }
if [ ! -f "$P/lib/libvulkan.so" ]; then
    say "Vulkan link stub (from $VKLIB's exports)"
    nm -D --defined-only "$VKLIB" | awk '$2=="T"{print $3}' | sed 's/@.*//' | sort -u | awk '{print "void "$1"(void){}"}' > "$S/vkstub.c"
    zcc -shared -fPIC -Wl,-soname,libvulkan.so.1 -o "$P/lib/libvulkan.so" "$S/vkstub.c"
fi

# ---- the AppImage
cd "$R"
tools/linux/make_appimage.sh
