#!/bin/bash
# ---------------------------------------------------------------------------
# windeploy.sh — assemble a portable Windows distribution of VidyaGod.
#
# Run inside an MSYS2 mingw64 shell AFTER building (cmake --build build). It bundles:
#   * VidyaGod.exe, vidyagodfs.exe, libvgipfs.dll   (our three artifacts)
#   * the Qt6 runtime + plugins (platforms/qwindows.dll, styles, imageformats, tls, …) via windeployqt6
#   * every non-system MinGW-w64 DLL the binaries transitively need (libstdc++, libgcc, winpthread,
#     libzip, zstd and their deps) — found by walking ldd, so nothing is missed
#
# The result (dist/) runs on a stock Windows machine with NO MSYS2 present. The ONE external
# requirement is WinFsp: it ships a kernel-mode driver that cannot live in a portable folder, so the
# real installer (NSIS/MSIX, a later step) must install the WinFsp MSI as a prerequisite. For a
# portable run, install WinFsp once on the machine.
#
#   usage: tools/windeploy.sh [build-dir] [dist-dir]   (defaults: build, dist)
# ---------------------------------------------------------------------------
set -euo pipefail
export PATH=/mingw64/bin:$PATH
BUILD="${1:-build}"
DIST="${2:-dist}"

command -v windeployqt6 >/dev/null || { echo "windeployqt6 not on PATH (need mingw-w64-x86_64-qt6-tools)"; exit 1; }
for f in VidyaGod.exe vidyagodfs.exe libvgipfs.dll; do
    [ -f "$BUILD/$f" ] || { echo "missing $BUILD/$f — build first"; exit 1; }
done

rm -rf "$DIST"; mkdir -p "$DIST"
cp "$BUILD/VidyaGod.exe" "$BUILD/vidyagodfs.exe" "$BUILD/libvgipfs.dll" "$DIST/"

echo "== windeployqt6 (Qt runtime + plugins) =="
windeployqt6 --release --no-translations --compiler-runtime "$DIST/VidyaGod.exe" >/dev/null

# Copy every mingw64 DLL a binary depends on (transitively caught by re-scanning what we copy).
copydeps() {
    ldd "$1" 2>/dev/null | awk '/mingw64\/bin/ && $3 {print $3}' | while read -r d; do
        [ -f "$d" ] && cp -n "$d" "$DIST/" || true
    done
}
echo "== bundling MinGW runtime + libzip/zstd deps (ldd walk) =="
copydeps "$DIST/VidyaGod.exe"
copydeps "$DIST/vidyagodfs.exe"
copydeps "$DIST/libvgipfs.dll"
# Qt plugin DLLs windeployqt dropped in subfolders can pull further mingw deps — scan them too.
while IFS= read -r p; do copydeps "$p"; done < <(find "$DIST" -name '*.dll')
# One more pass so deps-of-deps just copied are themselves satisfied.
while IFS= read -r p; do copydeps "$p"; done < <(find "$DIST" -maxdepth 1 -name '*.dll')

echo "== done =="
echo "dist: $(du -sh "$DIST" | cut -f1), $(find "$DIST" -name '*.dll' | wc -l) DLL(s), $(find "$DIST" -name '*.exe' | wc -l) exe(s)"
echo "NOTE: install WinFsp on the target (kernel driver — not bundlable in a portable folder)."
