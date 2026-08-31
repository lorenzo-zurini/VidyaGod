#!/usr/bin/env bash
# Build vglobby.exe — a headless DirectPlay lobby launcher — as a 32-bit Windows PE.
#
# 32-bit is not optional: dplayx is a 32-bit component and the games that use it are 32-bit.
#
# The source deliberately needs no DirectX import library: DirectPlayLobbyCreateA is resolved with
# GetProcAddress, and INITGUID makes the headers emit the GUIDs, so plain mingw-w64 is the whole toolchain.
#
# Requires: mingw-w64-gcc   (Arch: sudo pacman -S --needed mingw-w64-gcc)
#
# Usage: tools/vglobby/build.sh [outdir]      # default outdir: tools/vglobby/build
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/build}"
CC="${VG_MINGW_CC:-i686-w64-mingw32-gcc}"

command -v "$CC" >/dev/null || {
  echo "error: $CC not found. Install it with:  sudo pacman -S --needed mingw-w64-gcc" >&2
  exit 1
}

mkdir -p "$OUT"
# -mwindows (GUI subsystem), NOT console: a console-subsystem exe pops a visible terminal window in front of
# the game every launch. stdout still reaches the launcher's log, because the parent's handles are inherited —
# we lose the window, not the diagnostics.
# No -municode: the entry point is a plain main(). -static-libgcc so the exe drops into a game directory
# without needing a runtime DLL beside it.
"$CC" -O2 -Wall -Wextra -Wno-unused-parameter \
      -o "$OUT/vglobby.exe" "$HERE/vglobby.c" \
      -ladvapi32 -static-libgcc -mwindows
i686-w64-mingw32-strip "$OUT/vglobby.exe" 2>/dev/null || true
ls -la "$OUT/vglobby.exe"
file "$OUT/vglobby.exe" 2>/dev/null || true
