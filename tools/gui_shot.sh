#!/usr/bin/env bash
# Launch VidyaGod under a throwaway Xvfb, optionally drive it with xdotool, screenshot, and tear everything down —
# ALL in one process so it works inside a single tool call (backgrounded jobs don't survive across calls).
#
# Usage:
#   tools/gui_shot.sh <out.png> [settle_secs] [xdotool_actions]
# Env:
#   VIDYAGOD_START_TAB=IPFS|Library|Catalog|Settings   open directly on that tab (no clicking)
# Examples:
#   VIDYAGOD_START_TAB=IPFS tools/gui_shot.sh /tmp/ipfs.png 8
#   tools/gui_shot.sh /tmp/lib.png 3 'xdotool mousemove 250 32 click 1'
#
# Notes: uses a fixed display :97; cleans stale X locks; trap-cleans the app, wine, FUSE mounts, and Xvfb on exit.
set -u
OUT="${1:?usage: gui_shot.sh <out.png> [settle] [xdotool_actions]}"
SETTLE="${2:-4}"
ACTIONS="${3:-}"
DN=97
BIN="$(cd "$(dirname "$0")/.." && pwd)/build/VidyaGod"

# Fresh display: kill any prior Xvfb on :DN and clear its lock/socket.
for p in $(ps -C Xvfb -o pid=,args= 2>/dev/null | awk -v d=":$DN" '$0 ~ d {print $1}'); do kill -9 "$p" 2>/dev/null; done
rm -f "/tmp/.X${DN}-lock" "/tmp/.X11-unix/X${DN}" 2>/dev/null
sleep 0.5

Xvfb ":$DN" -screen 0 1680x1000x24 >/tmp/gui_xvfb.log 2>&1 &
XVFB_PID=$!
export DISPLAY=":$DN"

cleanup() {
  # the app + any wine it spawned
  [ -n "${APP_PID:-}" ] && kill -9 -"$APP_PID" 2>/dev/null
  pkill -9 -f "$BIN" 2>/dev/null
  for w in wineserver services.exe explorer.exe; do pkill -9 -x "$w" 2>/dev/null; done
  # any FUSE mounts the app left (never stat the mountpoint — read procfs, lazy-unmount)
  for mp in $(awk '$2 ~ /VidyaGod\/TEMP/{print $2}' /proc/self/mounts 2>/dev/null); do fusermount3 -uz "$mp" 2>/dev/null; done
  kill -9 "$XVFB_PID" 2>/dev/null
}
trap cleanup EXIT

# Wait for the X server.
for _ in $(seq 1 30); do xdpyinfo >/dev/null 2>&1 && break; sleep 0.3; done
xdpyinfo >/dev/null 2>&1 || { echo "ERROR: Xvfb :$DN did not come up"; cat /tmp/gui_xvfb.log; exit 1; }

# Launch the app in its own session/process-group so cleanup can kill the whole tree.
# Force software GL/raster so Qt renders into the Xvfb framebuffer (no GPU → a GL surface would grab black).
export LIBGL_ALWAYS_SOFTWARE=1 QT_OPENGL=software QT_XCB_GL_INTEGRATION=none GALLIUM_DRIVER=llvmpipe
setsid "$BIN" </dev/null >/tmp/gui_app.log 2>&1 &
APP_PID=$!

# Wait for the MAIN window — search returns several (tiny Qt helper windows too), so pick the largest by area.
pick_main_window() {
  local best="" area=0 w g ww hh a
  for w in $(xdotool search --name "Vidya God" 2>/dev/null); do
    g=$(xdotool getwindowgeometry "$w" 2>/dev/null | awk '/Geometry/{print $2}')   # WxH
    ww=${g%x*}; hh=${g#*x}
    [ -z "$ww" ] || [ -z "$hh" ] && continue
    a=$((ww * hh))
    if [ "$a" -gt "$area" ]; then area=$a; best=$w; fi
  done
  echo "$best"
}
WID=""
for _ in $(seq 1 50); do
  WID=$(pick_main_window)
  [ -n "$WID" ] && break
  kill -0 "$APP_PID" 2>/dev/null || { echo "ERROR: app exited early"; tail -5 /tmp/gui_app.log; exit 1; }
  sleep 0.4
done
[ -z "$WID" ] && { echo "ERROR: no window appeared"; tail -5 /tmp/gui_app.log; exit 1; }

# No window manager here, and the app may restore an off-screen saved geometry → move it on-screen before grabbing.
xdotool windowmove "$WID" 0 0 2>/dev/null
xdotool windowsize "$WID" 1680 1000 2>/dev/null
xdotool windowactivate --sync "$WID" 2>/dev/null
xdotool windowfocus "$WID" 2>/dev/null
xdotool windowmap "$WID" 2>/dev/null
sleep 1

# Optional interaction (a string of xdotool commands). $WID is exported so actions can target the window, e.g.
#   'xdotool mousemove --window "$WID" 200 600 click 1; xdotool key --window "$WID" Right'
export WID
[ -n "$ACTIONS" ] && eval "$ACTIONS"

sleep "$SETTLE"
import -window "$WID" "$OUT" 2>/dev/null || import -window root "$OUT" 2>/dev/null
if [ -f "$OUT" ]; then echo "OK: $OUT  $(identify -format '%wx%h' "$OUT" 2>/dev/null)"; else echo "ERROR: screenshot failed"; exit 1; fi
