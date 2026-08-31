#!/usr/bin/env bash
# Drive Wipeout XL's TCP/IP lobby up to the title screen and leave the game running, so a debugger can be
# attached to the interesting moment. Prints "AT TITLE <pid>" on success.
#
# Input goes through tools/realinput.py (kernel uinput); xdotool's XTEST events never reach the game on this
# Wayland session. See tools/gamedrive.sh's header.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SC="${VG_SCRATCH:-/tmp}"
export DISPLAY="${VG_DISPLAY:-:1}"
QD=$(command -v qdbus6 || command -v qdbus)

act() {  # activate by EXACT caption: "Wipeout XL" is a prefix of "Wipeout XL DirectPlay Launcher"
  printf 'for (const c of workspace.windowList()) { if (c.caption === "%s") workspace.activeWindow = c; }\n' "$1" > /tmp/woxl_a.js
  local n; n=$($QD org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript /tmp/woxl_a.js)
  $QD org.kde.KWin "/Scripting/Script$n" org.kde.kwin.Script.run >/dev/null 2>&1
  sleep 0.7
}
size() { xdotool getwindowgeometry "$1" 2>/dev/null | grep -oE '[0-9]+x[0-9]+' | tail -1; }

bash "$ROOT/tools/gamedrive.sh" teardown >/dev/null 2>&1
sleep 3
export PROTON_LOG=1 PROTON_LOG_DIR="$SC"
rm -f "$SC/steam-17260.log"
bash "$ROOT/tools/gamedrive.sh" launch wipeout_xl_lobby "$SC/woxl_title.log" >/dev/null
sleep 45

LW=$(xdotool search --name "DirectPlay Launcher" 2>/dev/null | tail -1)
[ -z "$LW" ] && { echo "LOBBY-DID-NOT-START"; exit 1; }
act "Wipeout XL DirectPlay Launcher"
eval "$(xdotool getwindowgeometry --shell "$LW")"
python3 "$ROOT/tools/realinput.py" seq \
  "click $((X+248)) $((Y+154)); type Lorenzo; click $((X+248)) $((Y+215)); type VidyaGod; click $((X+462)) $((Y+343))" >/dev/null

# Confirm the game's own config dialog inside the ~10s it waits before starting anyway.
for _ in $(seq 1 40); do
  CW=$(xdotool search --name "^Wipeout XL$" 2>/dev/null | tail -1)
  if [ -n "$CW" ] && [ "$(size "$CW")" = "494x378" ]; then
    act "Wipeout XL"
    eval "$(xdotool getwindowgeometry --shell "$CW")"
    python3 "$ROOT/tools/realinput.py" click $((X+300)) $((Y+352)) >/dev/null
    break
  fi
  sleep 0.4
done

sleep 80
PID=$(pgrep -x net-woxl.exe | head -1)
[ -z "$PID" ] && { echo "DIED-BEFORE-TITLE"; exit 1; }
echo "AT TITLE $PID"
