#!/bin/bash
# VidyaGodFS on WinFsp + Sandboxie, checked on the Windows test VM: builds the probe program (probe.c) in the
# cross-build image, zips it STORE beside a data file, copies both and vidyagodfs.exe (default: the cross-build's)
# to the VM, and runs fs_check.cmd in the desktop session (vmrun.sh). Exit code = failed checks.
# Usage: tools/winvm/fs_check.sh [vidyagodfs.exe]
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
FS=${1:-$HOME/Storage/tmp/vgwin/build/vidyagodfs.exe}
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
cp "$H/probe.c" "$W/"
docker run --rm -v "$W:/w" vgwin-img x86_64-w64-mingw32-gcc -O2 -static -o /w/probe.exe /w/probe.c
mkdir -p "$W/pkg" && cp "$W/probe.exe" "$W/pkg/" && echo zipped > "$W/pkg/data.txt"
(cd "$W" && zip -q -0 -r pkg.zip pkg)
O=(-P 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)
ssh -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR dev@localhost \
    '"C:\Program Files\Sandboxie-Plus\Start.exe" /terminate_all & taskkill /f /im vidyagodfs.exe >nul 2>&1 & mkdir C:\vgcheck C:\vg 2>nul & exit 0'
scp -q "${O[@]}" "$W/probe.exe" "$W/pkg.zip" dev@localhost:C:/vgcheck/
scp -q "${O[@]}" "$FS" dev@localhost:C:/vg/vidyagodfs.exe
exec "$H/vmrun.sh" "$H/fs_check.cmd" 180
