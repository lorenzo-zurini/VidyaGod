#!/usr/bin/env bash
# build-win32u.sh <workdir> — rebuild GE-Proton10-34's win32u.so (i386 + x86_64) with win32u-dpi-scaled-text.patch.
#
# The bug: GE-Proton 10's win32u gives the DC of a DPI-scaled window that has no window surface (a Direct3D/OpenGL
# window: GDI draws straight to the device) a dpi_from -> dpi_to factor, applied to every position in lp_to_dp but
# never to the font. Text on such a window — an edit box over a game running at 800x600 on a 1080p screen — is drawn
# at its logical size with stretched spacing ("small, widely spaced letters, the first one cut"). Upstream Wine later
# removed per-DC DPI scaling; GE-Proton 11 draws such windows into a surface instead (and the box then sits under the
# game's swapchain: empty). The patch draws with the font scaled by the same factor, in NtGdiExtTextOutW only, so
# the metrics a program reads back are unchanged.
#
# The result must match GE's own build exactly (win32u.so talks to the runner's ntdll.so and drivers), so it is
# built from GE-Proton10-34's pins: Valve wine 1729f00e + wine-staging 05bc4b82 + GE's prep (staging, wayland,
# hotfixes), then make_vulkan / make_specfiles as GE's Makefile does. Exports are identical to GE's shipped files;
# only libc symbol versions differ (built against the host glibc, which is what the runner runs on).
#
# Output: <workdir>/out/{i386,x86_64}-unix/win32u.so — the files the geproton_10_34 runner node overlays from
# LIBRARY/VidyaGodRunners/proton/patches/<arch>-unix/win32u.so.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)   # before moving into the work dir: $0 may be relative
W=${1:?workdir}; mkdir -p "$W"; cd "$W"; W=$(pwd)
[ -d ge ] || git clone -q --depth 1 --branch GE-Proton10-34 https://github.com/GloriousEggroll/proton-ge-custom.git ge
rmdir ge/wine ge/wine-staging 2>/dev/null || true
if [ ! -d ge/wine/.git ]; then
    git clone -q --depth 1 --branch proton_10.0 https://github.com/ValveSoftware/wine.git ge/wine
    git -C ge/wine fetch -q --depth 1 origin 1729f00e17e879f98f9df1f2bca86bc5d21a65df
    git -C ge/wine fetch -q --depth 2 origin e813ca5771658b00875924ab88d525322e50d39f   # GE's prep reverts it
    git -C ge/wine -c advice.detachedHead=false checkout -q 1729f00e17e879f98f9df1f2bca86bc5d21a65df
fi
if [ ! -d ge/wine-staging/.git ]; then
    git clone -q https://github.com/wine-staging/wine-staging.git ge/wine-staging
    git -C ge/wine-staging -c advice.detachedHead=false checkout -q 05bc4b822fdb1898777b08a8597639ad851f5601
fi
# GE's prep, its "pushd wine ... popd" section only (one configure.ac hunk is already applied upstream: expected).
python3 - "$W/ge/patches/protonprep-valve-staging.sh" > prep-wine.sh <<'PY'
import sys
s = open(sys.argv[1]).read()
a = s.index('    pushd wine\n    git reset --hard HEAD'); b = s.index('\n    popd', a) + len('\n    popd')
h = s[:s.index('apply_all_in_dir() {')]; e = s.index('}', s.index('apply_all_in_dir() {')) + 2
print('#!/bin/bash\nset -u\ncd "$(dirname "$0")/ge"\n' + h + s[s.index('apply_all_in_dir() {'):e] + '\n' + s[a:b])
PY
bash prep-wine.sh > prep-wine.log 2>&1 || true
(cd ge/wine && patch -p1 < "$HERE/win32u-dpi-scaled-text.patch")
(cd ge/wine && XDG_CACHE_HOME="$W/vkcache" python3 dlls/winevulkan/make_vulkan -x vk.xml -X video.xml && tools/make_specfiles)
OPTS="--disable-tests --without-gstreamer --without-cups --without-sane --without-krb5 --without-pcap --without-oss"
mkdir -p build32 build64 out/i386-unix out/x86_64-unix
(cd build32 && PKG_CONFIG_PATH=/usr/lib32/pkgconfig PKG_CONFIG_LIBDIR=/usr/lib32/pkgconfig CC="gcc -m32" CXX="g++ -m32" \
    ../ge/wine/configure $OPTS > configure.log && make -j"$(nproc)" dlls/win32u/win32u.so > make.log)
(cd build64 && ../ge/wine/configure --enable-win64 $OPTS > configure.log && make -j"$(nproc)" dlls/win32u/win32u.so > make.log)
strip -o out/i386-unix/win32u.so build32/dlls/win32u/win32u.so
strip -o out/x86_64-unix/win32u.so build64/dlls/win32u/win32u.so
ls -la out/*/win32u.so
