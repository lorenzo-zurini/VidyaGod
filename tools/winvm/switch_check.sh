#!/bin/bash
# Switching a package's version on Windows while a dead machine's WinFsp junction is left under the session root (the
# folder WinFsp mounts a game on, naming a volume that is gone): the switch must go through, since no game is running.
# Runs the cross-built test_packagecatalog.exe (with every DLL it imports) on the Windows test VM, in the desktop
# session (vmrun.sh), after planting the junction with deadjunction.c. Exit code = the test's.
# Usage: tools/winvm/switch_check.sh [build dir]   (cross-build with TARGETS="… test_packagecatalog")
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
B=${1:-$HOME/Storage/tmp/vgwin/build}
O=(-P 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
mkdir -p "$W/d/platforms"
cp "$B/test_packagecatalog.exe" "$W/d/"
cp "$(find "$B" -maxdepth 3 -name libvgipfs.dll | head -1)" "$W/d/"   # built here, not in the sysroot
cp "$H/deadjunction.c" "$W/"
docker run --rm -v "$W:/w" vgwin-img sh -c '
    SYS=/usr/x86_64-w64-mingw32/sys-root/mingw
    x86_64-w64-mingw32-gcc -O2 -municode -static -o /w/d/deadjunction.exe /w/deadjunction.c
    cp $SYS/lib/qt6/plugins/platforms/qoffscreen.dll /w/d/platforms/
    while :; do added=0
        for f in $(find /w/d -name "*.exe" -o -name "*.dll"); do
            for dll in $(x86_64-w64-mingw32-objdump -p "$f" | awk "/DLL Name:/ {print \$3}"); do
                [ -e "/w/d/$dll" ] && continue
                src=$(find $SYS/bin -maxdepth 1 -iname "$dll" | head -1)
                [ -n "$src" ] && { cp "$src" /w/d/; added=1; }
            done
        done
        [ $added = 0 ] && break
    done
    chown -R '"$(id -u):$(id -g)"' /w'
ssh -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR dev@localhost \
    'rmdir /s /q C:\vgswitch 2>nul & mkdir C:\vgswitch & exit 0'
scp -q -r "${O[@]}" "$W/d/"* dev@localhost:C:/vgswitch/
cat > "$W/job.cmd" <<'EOF'
@echo off
set PATH=C:\vgswitch;%PATH%
set QT_QPA_PLATFORM=offscreen
set T=C:\vgswitch\test_packagecatalog.exe an_installed_package_switches_to_a_friends_version
set J=%TEMP%\VidyaGod\990999\DefaultInstance
if exist %J%\RUNTIME rmdir %J%\RUNTIME
%T% > C:\vgswitch\control.log 2>&1
findstr /c:"0 failed" C:\vgswitch\control.log >nul || (echo FAIL the test fails with no junction at all & type C:\vgswitch\control.log & exit /b 2)
mkdir %J% 2>nul
C:\vgswitch\deadjunction.exe %J%\RUNTIME || (echo FAIL could not plant the dead junction & exit /b 90)
%T% > C:\vgswitch\junction.log 2>&1
set RC=%ERRORLEVEL%
rmdir %J%\RUNTIME
if %RC%==0 (echo PASS the switch goes through beside a dead junction & exit /b 0)
echo FAIL the switch was refused beside a dead junction
findstr /c:"FAIL" C:\vgswitch\junction.log
exit /b 1
EOF
exec "$H/vmrun.sh" "$W/job.cmd" 180
