#!/usr/bin/env bash
# Drive a REAL game on the REAL desktop: launch it, click through its dialogs, reach the menu, screenshot it,
# and tear the whole wine stack down again. The counterpart to gui_shot.sh (which drives the VidyaGod GUI under a
# throwaway Xvfb) — a game needs the actual GPU and compositor, so everything here targets the live session.
#
# Usage (source it, or call subcommands directly):
#   tools/gamedrive.sh launch <nodeid> [logfile]     # detached launch that survives the calling shell
#   tools/gamedrive.sh wait <caption> [timeout=90]   # block until a window whose title contains <caption> exists
#   tools/gamedrive.sh raise <caption>               # focus it (KWin scripting — works on XWayland)
#   tools/gamedrive.sh click <caption> <x> <y>       # click at WINDOW-RELATIVE coords
#   tools/gamedrive.sh dismiss <caption> <x> <y>     # click only if that window is present (dialog handling)
#   tools/gamedrive.sh key <keysym> [count=1]        # paced keydown/keyup
#   tools/gamedrive.sh type <text>                   # per-character paced typing
#   tools/gamedrive.sh shot <out.png>                # full-screen capture
#   tools/gamedrive.sh diff <a.png> <b.png>          # changed-pixel count (did the screen react?)
#   tools/gamedrive.sh teardown                      # kill game + wine + stray VidyaGod launches
#
# Env: VG_DISPLAY (default :1), VG_SETTLE (default 1.5) — pointer settle before a click.
#
# HARD-WON NOTES (every one of these cost a debugging cycle):
#   * LAUNCH DETACHED. `setsid nohup … &` — a plain background job dies with the tool call that started it.
#   * NEVER bare `pkill -f <pattern>` when the pattern also appears in your own command line: the shell matches
#     ITSELF and dies mid-script. Use a bracket class ("MW4Merc[s]") or kill by PID from `ps -eo pid,comm`.
#   * CLICK WINDOW-RELATIVE. Under XWayland a game's X-side position (xdotool getwindowgeometry) does NOT match
#     where the compositor shows it — absolute warps land nowhere. `xdotool mousemove --window <id> x y` is right.
#   * WARP THEN WAIT THEN CLICK. Games poll input; a warp+click in the same instant clicks the OLD position.
#     VG_SETTLE covers the lag, and the cursor visibly lands before the button press.
#   * `xdotool type` OFTEN DOES NOT LAND in DirectInput games. Send explicit keydown/keyup with ~0.12s spacing.
#   * A game holding a POINTER GRAB swallows synthetic input entirely. Add a package RegEdit
#     HKCU\Software\Wine\X11 Driver  GrabPointer=N  for automation runs (MW4 needed it).
#   * Escape during a fullscreen movie can reach the MENU BEHIND IT and quit the game — check the exit code
#     (0 = the game quit cleanly, i.e. you navigated it into quitting; it did not crash).
set -u
DISP="${VG_DISPLAY:-:1}"
SETTLE="${VG_SETTLE:-1.5}"
export DISPLAY="$DISP"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="${TMPDIR:-/tmp}/vg-gamedrive.$$"

win_id() { xdotool search --name "$1" 2>/dev/null | tail -1; }   # newest match wins (dialogs stack)

case "${1:?usage: see header}" in
launch)
  NODE="${2:?node id}"; LOG="${3:-$TMP.launch.log}"
  setsid nohup "$ROOT/build/VidyaGod" --node "$NODE" >"$LOG" 2>&1 </dev/null &
  echo "launched $NODE -> $LOG"
  ;;
wait)
  CAP="${2:?caption}"; T="${3:-90}"
  for ((i=0; i<T; i++)); do
    [ -n "$(win_id "$CAP")" ] && { echo "up: $CAP"; exit 0; }
    sleep 1
  done
  echo "TIMEOUT waiting for: $CAP" >&2; exit 1
  ;;
raise)
  CAP="${2:?caption}"; J="$TMP.js"
  # KWin scripting is the only reliable activator for XWayland game windows; skip our own tooling windows.
  cat >"$J" <<JS
for (const c of workspace.windowList()) {
    if (!c.caption) continue;
    if (c.caption.indexOf("Dolphin") !== -1 || c.caption.indexOf("Konsole") !== -1) continue;
    if (c.caption.indexOf("$CAP") !== -1) { workspace.activeWindow = c; break; }
}
JS
  N=$(qdbus org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript "$J")
  qdbus org.kde.KWin "/Scripting/Script$N" org.kde.kwin.Script.run
  sleep 0.6
  qdbus org.kde.KWin "/Scripting/Script$N" org.kde.kwin.Script.stop 2>/dev/null
  qdbus org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "$J" >/dev/null 2>&1
  rm -f "$J"
  ;;
click|dismiss)
  CAP="${2:?caption}"; X="${3:?x}"; Y="${4:?y}"; W=$(win_id "$CAP")
  if [ -z "$W" ]; then
    [ "$1" = dismiss ] && exit 0                       # dismiss = best-effort: absent dialog is success
    echo "no window: $CAP" >&2; exit 1
  fi
  xdotool mousemove --window "$W" "$X" "$Y"; sleep "$SETTLE"
  xdotool mousedown 1; sleep 0.3; xdotool mouseup 1     # held press — instant clicks fall between input polls
  ;;
key)
  K="${2:?keysym}"; C="${3:-1}"
  for ((i=0; i<C; i++)); do xdotool keydown "$K"; sleep 0.12; xdotool keyup "$K"; sleep 0.4; done
  ;;
type)
  T="${2:?text}"
  for ((i=0; i<${#T}; i++)); do
    c="${T:$i:1}"; xdotool keydown "$c"; sleep 0.12; xdotool keyup "$c"; sleep 0.12
  done
  ;;
shot)
  spectacle -m -b -n -o "${2:?out.png}" 2>/dev/null; sleep 1; ls -la "$2"
  ;;
diff)
  python3 - "$2" "$3" <<'PY'
import sys, numpy as np
from PIL import Image
a, b = (np.asarray(Image.open(p).convert('L'), dtype=int) for p in sys.argv[1:3])
print("changed pixels:", int((np.abs(a - b) > 16).sum()) if a.shape == b.shape else "size mismatch")
PY
  ;;
teardown)
  for p in $(pgrep -f "VidyaGod --nod"); do kill -9 "$p" 2>/dev/null; done
  for p in $(ps -eo pid,comm | grep -iE "wineserver|winedevice|services\.exe|svchost|plugplay|explorer\.exe|rpcss|tabtip|xalia|steam\.exe|\.exe$" | awk '{print $1}'); do
    kill -9 "$p" 2>/dev/null
  done
  sleep 3
  echo "remaining: $(ps -eo comm | grep -icE 'wine|\.exe' || true)"
  ;;
*) sed -n '2,30p' "$0"; exit 1 ;;
esac
