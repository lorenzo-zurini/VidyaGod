#!/usr/bin/env bash
# Drive Wipeout XL's TCP/IP multiplayer end to end and report whether it survives the title screen.
#
# The whole flow is: WOLOBBY (fill player + session, Launch) -> the game's config dialog (Play Game) ->
# intro FMV -> title screen -> Enter. Enter at the title is where net-woxl dies inside dgVoodoo's Blt, so
# "did the process survive Enter" is the pass/fail signal.
#
# ⚠ THIS HARNESS IS NOT YET A TRUSTWORTHY A/B TOOL — read before believing a result.
# The game alternates title screen <-> attract-mode demo on a timer. Enter only reaches the crashing code path
# while the TITLE is up; land it during the demo and the run reports SURVIVED-ENTER without having tested
# anything. Because the wait here is a fixed sleep, pass/fail tracks *when the sleep expired*, not the change
# under test. A sweep of dgVoodoo settings run this way produced results that did not replicate, and an
# apparent "2.78.2 is better than 2.81.3" signal (3/3 vs 0/5) collapsed as soon as a manual run at a different
# delay crashed on 2.78.2 too. Before using this to compare anything, make it detect the title screen from a
# screenshot (match the "PRESS ENTER" band) and press Enter only then.
#
# Input goes through tools/realinput.py (kernel uinput), NOT xdotool: on this Wayland session XTEST events
# reach Xwayland but the compositor never routes them to the game, so synthetic clicks silently do nothing and
# the game looks hung when it is really just waiting for input that never arrives.
#
# Usage: tools/woxl_mp_test.sh [label]
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LABEL="${1:-run}"
SC="${VG_SCRATCH:-/tmp}"
export DISPLAY="${VG_DISPLAY:-:1}"
QD=$(command -v qdbus6 || command -v qdbus)

kwin_activate() {  # $1 = caption, $2 = "exact" to require an exact match
  local j=/tmp/woxl_act.js n
  # Substring matching is wrong for the game itself: "Wipeout XL" is a prefix of "Wipeout XL DirectPlay
  # Launcher", so the lobby would win and the keystroke would land on the wrong window.
  if [ "${2:-}" = exact ]; then
    printf 'for (const c of workspace.windowList()) { if (c.caption === "%s") workspace.activeWindow = c; }\n' "$1" > "$j"
  else
    printf 'for (const c of workspace.windowList()) { if (c.caption && c.caption.indexOf("%s") !== -1) workspace.activeWindow = c; }\n' "$1" > "$j"
  fi
  n=$($QD org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript "$j")
  $QD org.kde.KWin "/Scripting/Script$n" org.kde.kwin.Script.run >/dev/null 2>&1
  sleep 0.6
}

win_size() { xdotool getwindowgeometry "$1" 2>/dev/null | grep -oE '[0-9]+x[0-9]+' | tail -1; }

bash "$ROOT/tools/gamedrive.sh" teardown >/dev/null 2>&1
sleep 3
export PROTON_LOG=1 PROTON_LOG_DIR="$SC"
rm -f "$SC/steam-17260.log"
bash "$ROOT/tools/gamedrive.sh" launch wipeout_xl_lobby "$SC/woxl_$LABEL.log" >/dev/null
sleep 45

LW=$(xdotool search --name "DirectPlay Launcher" 2>/dev/null | tail -1)
[ -z "$LW" ] && { echo "RESULT[$LABEL]: LOBBY-DID-NOT-START"; exit 1; }
kwin_activate "DirectPlay"
eval "$(xdotool getwindowgeometry --shell "$LW")"
python3 "$ROOT/tools/realinput.py" click $((X+248)) $((Y+154)) >/dev/null
python3 "$ROOT/tools/realinput.py" type "Lorenzo"                >/dev/null
python3 "$ROOT/tools/realinput.py" click $((X+248)) $((Y+215)) >/dev/null
python3 "$ROOT/tools/realinput.py" type "VidyaGod"               >/dev/null
python3 "$ROOT/tools/realinput.py" click $((X+462)) $((Y+343)) >/dev/null

# The config dialog is shown but the game stops waiting for it after ~10s, so confirm it inside that window.
for _ in $(seq 1 40); do
  CW=$(xdotool search --name "^Wipeout XL$" 2>/dev/null | tail -1)
  if [ -n "$CW" ] && [ "$(win_size "$CW")" = "494x378" ]; then
    kwin_activate "Wipeout XL"
    eval "$(xdotool getwindowgeometry --shell "$CW")"
    python3 "$ROOT/tools/realinput.py" click $((X+300)) $((Y+352)) >/dev/null
    break
  fi
  sleep 0.4
done

# Wait for the intro to finish and the title screen to come up (window goes fullscreen first).
for _ in $(seq 1 60); do
  GW=$(xdotool search --name "^Wipeout XL$" 2>/dev/null | tail -1)
  [ -n "$GW" ] && [ "$(win_size "$GW")" != "494x378" ] && break
  sleep 1
done
sleep 70

if ! pgrep -x net-woxl.exe >/dev/null; then
  echo "RESULT[$LABEL]: DIED-BEFORE-TITLE"
  grep -aE "Unhandled exception|page fault" "$SC/steam-17260.log" 2>/dev/null | tail -1
  exit 1
fi

# The game window MUST be the compositor's active window or the keystroke goes nowhere and the run reports a
# false pass ("survived" when the game simply never saw the Enter). Verify it took, and say so if it did not.
kwin_activate "Wipeout XL" exact
printf 'print("VGACTIVE: " + (workspace.activeWindow ? workspace.activeWindow.caption : "none"));\n' > /tmp/woxl_q.js
qn=$($QD org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript /tmp/woxl_q.js)
$QD org.kde.KWin "/Scripting/Script$qn" org.kde.kwin.Script.run >/dev/null 2>&1
sleep 0.5
ACT=$(journalctl --user -n 10 --no-pager -o cat 2>/dev/null | grep -o "VGACTIVE: .*" | tail -1)
if [ "$ACT" != "VGACTIVE: Wipeout XL" ]; then
  echo "RESULT[$LABEL]: INCONCLUSIVE-NOT-FOCUSED ($ACT)"
  exit 3
fi
python3 "$ROOT/tools/realinput.py" key Return 1 >/dev/null
sleep 8
if pgrep -x net-woxl.exe >/dev/null; then
  echo "RESULT[$LABEL]: SURVIVED-ENTER"
  exit 0
fi
echo "RESULT[$LABEL]: CRASHED-ON-ENTER"
grep -aE "page fault on read access" "$SC/steam-17260.log" 2>/dev/null | tail -1
exit 2
