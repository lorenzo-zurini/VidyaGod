#!/bin/bash
# A windows-native launch through VidyaGod on the Windows test VM, end to end, its Sandboxie box as its prefix: the probe
# package (probe.c, whose exit code is the bitmask of failed checks) places files in the game folder (a zip and an
# edit), under %AppData% (an edit) and in the system folder (a DLL, in its own zip), writes a registry value, and keeps
# a folder in the game folder, a folder under %AppData% and a registry key. It is built here, copied with VidyaGod
# (default: the cross-build at ~/Storage/tmp/vgwin/build, whose dist DLLs must already be in C:\vg) to the VM, and
# launched in the desktop session (vmrun.sh). Two leftovers a real machine can have are planted first: the
# windows-native runner as v0.0.6 published it (same label, so picking it makes the probe fail), and the game's mount
# folder as a dead WinFsp junction (a machine that died with the mount up). It launches twice in a row (the box is
# re-used); then a crashed session's box is planted (its AppData save, count 41), a launch that fails before its box is
# prepared (its zip hidden) must leave that box alone, and a third launch saves it first.
# Passes (exit 0) when the probe exits 0 each time, the kept AppData counter reads 42 (the crashed session's save was
# stored first, then counted on) and the registry one 3, the box is emptied after, and the box is the launch's (its
# root in the instance) with each grant listed once, backslashed: the runtime's drive letter (a box cannot load
# DLLs from a volume mounted on a folder) and the volume behind it.
# Usage: tools/winvm/probe_launch.sh [build dir]
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
B=${1:-$HOME/Storage/tmp/vgwin/build}
O=(-P 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)
SSH=(ssh -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR dev@localhost)
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
cp "$H/probe.c" "$H/deadjunction.c" "$W/"
echo '__declspec(dllexport) int vgsys_value(void) { return 4242; }' > "$W/vgsys.c"
docker run --rm -v "$W:/w" vgwin-img sh -c 'x86_64-w64-mingw32-gcc -O2 -static -o /w/probe.exe /w/probe.c &&
    x86_64-w64-mingw32-gcc -O2 -municode -static -o /w/deadjunction.exe /w/deadjunction.c &&
    x86_64-w64-mingw32-gcc -O2 -shared -static-libgcc -o /w/vgsys.dll /w/vgsys.c'
mkdir -p "$W/pkg" "$W/sys" && cp "$W/probe.exe" "$W/pkg/" && echo "the game's own file" > "$W/pkg/data.txt"
cp "$W/vgsys.dll" "$W/sys/"
(cd "$W/pkg" && zip -q -0 ../probe.zip probe.exe data.txt)
(cd "$W/sys" && zip -q -0 ../sys.zip vgsys.dll)
# The node, named by its CID (MetaPackageFormat: canonical JSON, one raw block, CIDv1 sha2-256 base32).
CID=$(python3 -I - "$W" <<'PY'
import base64, hashlib, json, os, sys
w = sys.argv[1]
node = {"LABEL": "WinProbe", "VARIANT": "Probe", "LAYERS": [
    {"SIZE": os.path.getsize(os.path.join(w, "probe.zip")), "TARGET": "FILES/%GameDir%", "ZIP": "probe.zip"},
    {"EDIT": [{"MODE": "Overwrite", "VALUE": "%GameDir%"}], "TARGET": "FILES/%GameDir%/cfg.txt"},
    {"EDIT": [{"MODE": "Overwrite", "VALUE": "%GameDir%"}], "TARGET": "FILES/%AppData%/probe/cfg.txt"},
    {"SIZE": os.path.getsize(os.path.join(w, "sys.zip")), "TARGET": "FILES/%SysDir64%", "ZIP": "sys.zip"},
    {"REG": {"HKCU": {"Software": {"VidyaGodProbe": {"GameDir": "%GameDir%"}}}}},
    {"KEEP": {"FILES/%GameDir%/saves/": {"NAME": "probe saves"}, "FILES/%AppData%/probe/saves/": {"NAME": "probe appdata saves"},
              "REG/HKCU/Software/VidyaGodProbe/Kept": True, "REG/HKCU/Software/VidyaGodProbe/Kept2": True,
              "REG/HKCU/Software/VidyaGodProbe/Kept3": True}},
    {"EXEC": [{"ARGS": ["%GameDir%", "%AppData%"], "EXE": "%GameDir%/probe.exe", "HOST": "win64", "LABEL": "Play",
               "TILE": {"TITLE": "WinProbe", "UID": "990001"}}]}]}
b = json.dumps(node, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()
cid = "b" + base64.b32encode(bytes([1, 0x55, 0x12, 0x20]) + hashlib.sha256(b).digest()).decode().lower().rstrip("=")
open(os.path.join(w, cid + ".json"), "wb").write(b)
print(cid)
PY
)
OLD=bafkreiaj2f2uth2gehoqdg5czxpvgdibvqrmitkntcsyh4cx2ilprz72ta
cp "$H/windows-native-v0.0.6.json" "$W/$OLD.json"
"${SSH[@]}" 'taskkill /f /im VidyaGod.exe >nul 2>&1 & taskkill /f /im vidyagodfs.exe >nul 2>&1 & rmdir /s /q C:\vgprobe 2>nul & mkdir C:\vgprobe\LIBRARY\WinProbe C:\vgprobe\LIBRARY\VidyaGodRunners\windows-native C:\vg 2>nul & exit 0'
scp -q "${O[@]}" "$W/$CID.json" "$W/probe.zip" "$W/sys.zip" dev@localhost:C:/vgprobe/LIBRARY/WinProbe/
scp -q "${O[@]}" "$W/$OLD.json" dev@localhost:C:/vgprobe/LIBRARY/VidyaGodRunners/windows-native/
scp -q "${O[@]}" "$W/deadjunction.exe" dev@localhost:C:/vgprobe/
scp -q "${O[@]}" "$B/VidyaGod.exe" "$B/vidyagodfs.exe" dev@localhost:C:/vg/
cat > "$W/job.cmd" <<EOF
@echo off
set PATH=C:\\vg;C:\\Program Files (x86)\\WinFsp\\bin;%PATH%
set QT_QPA_PLATFORM=offscreen
set RT=%TEMP%\\VidyaGod\\990001\\DefaultInstance
set BOX=C:\\vgprobe\\USERDATA\\990001\\DefaultInstance\\.vidyagod-box
rem rmdir on a junction removes the junction only, never what it names
if exist %RT%\\RUNTIME rmdir %RT%\\RUNTIME 2>nul
rmdir /s /q %RT% 2>nul
mkdir %RT%
C:\\vgprobe\\deadjunction.exe %RT%\\RUNTIME || (echo FAIL could not plant the dead junction & exit /b 90)
C:\\vg\\VidyaGod.exe --offline --bypass-single-instance-lock --data-dir C:\\vgprobe --node $CID > C:\\vgprobe\\launch.log 2>&1
rem at once again: the box is re-used (its grants must be replaced, not added to, and a start while it winds down is killed)
C:\\vg\\VidyaGod.exe --offline --bypass-single-instance-lock --data-dir C:\\vgprobe --node $CID > C:\\vgprobe\\launch2.log 2>&1
rem a crash: the box of a session that never got its teardown still holds that session's save (count 41)
mkdir "%BOX%\\user\\current\\AppData\\Roaming\\probe\\saves"
> "%BOX%\\user\\current\\AppData\\Roaming\\probe\\saves\\count.txt" echo 41
> "%BOX%\\vidyagod-placed.txt" echo user/current/AppData/Roaming/probe/saves/count.txt
rem a launch that fails before its box is prepared (its zip is missing) must leave that box alone
ren C:\\vgprobe\\LIBRARY\\WinProbe\\probe.zip probe.zip.hidden
C:\\vg\\VidyaGod.exe --offline --bypass-single-instance-lock --data-dir C:\\vgprobe --node $CID > C:\\vgprobe\\launch2b.log 2>&1
ren C:\\vgprobe\\LIBRARY\\WinProbe\\probe.zip.hidden probe.zip
if exist "%BOX%\\user\\current\\AppData\\Roaming\\probe\\saves\\count.txt" if exist "%BOX%\\vidyagod-placed.txt" > C:\\vgprobe\\kept2b.txt echo kept
C:\\vg\\VidyaGod.exe --offline --bypass-single-instance-lock --data-dir C:\\vgprobe --node $CID > C:\\vgprobe\\launch3.log 2>&1
findstr /c:"Process exited" /c:"[ERR" /c:"did not finish" C:\\vgprobe\\launch.log C:\\vgprobe\\launch2.log C:\\vgprobe\\launch3.log
powershell -NoProfile -ExecutionPolicy Bypass -File C:\\vgprobe\\check.ps1
EOF
cat > "$W/check.ps1" <<'EOF'
foreach ($run in 'launch','launch2','launch3') {
    $m = Get-Content "C:\vgprobe\$run.log" | Select-String 'Process exited normally with code (\d+)' | Select-Object -Last 1
    if (-not $m) { "FAIL $run`: the probe did not run"; exit 1 }
    $code = [int]$m.Matches[0].Groups[1].Value
    if ($code -ne 0) { "FAIL $run`: the probe exited $code (1 game file, 2 GameDir edit, 4 AppData edit, 8 machine's C:, 32 AppData not real, 64 save, 128 working directory, 256 registry, 512 system DLL, 1024 AppData count, 2048 registry count)"; exit 1 }
    "PASS $run`: the probe exited 0"
}
if (Get-Content C:\vgprobe\launch2b.log | Select-String 'Process exited') { 'FAIL launch2b: the launch without its zip started the game'; exit 1 }
if (Get-Content C:\vgprobe\launch2b.log | Select-String 'BoxLaunch::') { 'FAIL launch2b: a launch that prepared no box touched the box (harvested or emptied it)'; exit 1 }
if (-not (Test-Path C:\vgprobe\kept2b.txt)) { "FAIL launch2b: a launch that failed before its box was prepared took or emptied the crashed session's box"; exit 1 }
"PASS launch2b: a launch that failed before its box was prepared left the crashed session's box alone"
$count = 'C:\vgprobe\USERDATA\990001\DefaultInstance\probe appdata saves\count.txt'
if (-not (Test-Path $count) -or (Get-Content $count) -ne '42') { "FAIL the kept AppData counter is not 42 (41, saved by the crashed session's box, + 1): $count = $(Get-Content $count)"; exit 1 }
"PASS the kept AppData counter reads 42: the crashed session's save was kept, then counted on"
if (Test-Path "C:\vgprobe\USERDATA\990001\DefaultInstance\.vidyagod-box\vidyagod-placed.txt") { 'FAIL the box was not emptied after the launch'; exit 1 }
'PASS the box was emptied after the launch'
$store = Get-Content 'C:\vgprobe\USERDATA\990001\DefaultInstance\REGKEYS\user.reg' -ErrorAction SilentlyContinue
foreach ($k in 'Kept','Kept2','Kept3') {   # each key's Runs, read from its own section of the store
    $section = $false; $runs = $null
    foreach ($l in $store) {
        if ($l -match '^\[') { $section = ($l -match ('^\[Software\\\\VidyaGodProbe\\\\' + $k + '\]')) ; continue }
        if ($section -and $l -match '^"Runs"=dword:([0-9a-f]+)$') { $runs = [Convert]::ToInt32($Matches[1], 16) }
    }
    if ($runs -ne 3) { "FAIL the kept registry counter $k is not 3 after three launches (it is $runs)"; exit 1 }
}
'PASS the three kept registry counters read 3 (three keys read out of the box one after another)'
$box = $null; $sections = [ordered]@{}
foreach ($l in (Get-Content C:\Windows\Sandboxie.ini)) {
    if ($l -match '^\[(.*)\]') { $box = $Matches[1]; $sections[$box] = @(); continue }
    if ($box -like 'VidyaGod_*') { $sections[$box] += $l }
}
$root = $null; $grants = @()
foreach ($k in $sections.Keys) {   # the probe's box: the (last) one whose root is in its instance
    if ($sections[$k] -match '^FileRootPath=C:\\vgprobe\\USERDATA\\990001\\DefaultInstance\\\.vidyagod-box$') {
        $root = $k
        $grants = @($sections[$k] | Where-Object { $_ -match '^OpenFilePath=' } | ForEach-Object { $_.Substring(13) })
    }
}
if (-not $root) { 'FAIL the box has not its root in the instance'; exit 1 }
if (-not ($grants -match '^[A-Z]:\\$')) { 'FAIL no grant for the launch mount (a drive letter) in Sandboxie.ini'; exit 1 }
if (-not ($grants -like '\Device\Volume{*}')) { 'FAIL no grant for the mount volume in Sandboxie.ini'; exit 1 }
if (($grants | Select-Object -Unique).Count -ne $grants.Count) { 'FAIL a grant is listed twice: ' + ($grants -join ' | '); exit 1 }
if ($grants -match '/') { 'FAIL a grant has a forward slash: ' + ($grants -join ' | '); exit 1 }
'PASS the box grants ' + ($grants -join ' | ')
exit 0
EOF
scp -q "${O[@]}" "$W/check.ps1" dev@localhost:C:/vgprobe/
exec "$H/vmrun.sh" "$W/job.cmd" 240
