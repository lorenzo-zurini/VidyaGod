#!/usr/bin/env bash
# Drive the VidyaGod GUI itself — the counterpart to gamedrive.sh (which drives real GAMES). Runs a THROWAWAY
# instance against a scratch --data-dir so nothing touches the live library, forces the XCB platform so xdotool can
# actually see the window, and gives you click/type/shot verbs.
#
# WHY THIS EXISTS: VidyaGod's Qt build runs NATIVELY ON WAYLAND on this desktop, so `xdotool search --name VidyaGod`
# finds NOTHING and every click silently goes nowhere. QT_QPA_PLATFORM=xcb is the whole trick — it puts the app on
# XWayland where the X automation tools work.
#
# Usage:
#   tools/guidrive.sh start [datadir]        # launch a scratch GUI (xcb), print its window geometry
#   tools/guidrive.sh geom                   # window position + size (clicks are WINDOW-RELATIVE)
#   tools/guidrive.sh click <x> <y>          # click at window-relative coords
#   tools/guidrive.sh type <text>            # type into the focused widget (slow + --clearmodifiers, see below)
#   tools/guidrive.sh key <keysym>           # single keypress (Return, Escape, Tab…)
#   tools/guidrive.sh shot <out.png>         # full-screen capture
#   tools/guidrive.sh dialog <name>          # window id of a dialog whose title contains <name> ("" if absent)
#   tools/guidrive.sh stop                   # kill the scratch instance only (never the live one)
#
# Env: VG_DISPLAY (default :1), VG_DATADIR (default a fresh dir under TMPDIR), VG_SETTLE (default 1.0).
#
# HARD-WON NOTES:
#   * PREFER tests/gui/ FOR WIRING. A Qt Test that does findChild<QPushButton*>("upgrade_Src") + QTest::mouseClick
#     is deterministic, needs no compositor, and runs in ctest. Only come here for things that need a REAL app:
#     MODAL dialogs, the IPFS node, drag/drop, or "does it actually look right".
#   * MODAL DIALOGS DEADLOCK THE HEADLESS TESTS. QInputDialog/QMessageBox exec() will not yield to a QTimer under
#     the offscreen platform, so answering a modal from the same thread hangs forever. That is exactly the case
#     this script exists for — here the dialog is a real window you can find and click.
#   * `xdotool type` MANGLES CASE at default speed: "QmU7oAdKw…" arrived as "qMu7OaDkW…", which then failed as an
#     invalid CID and looked like a product bug. Always --clearmodifiers with a delay >= 50ms (the `type` verb
#     below does), and SCREENSHOT THE FIELD to confirm before submitting.
#   * A dialog's window id goes STALE the moment it closes; `xdotool type --window <id>` then dies with BadWindow.
#     Type into the FOCUSED window instead (no --window) unless you just re-queried the id.
#   * CLICK WINDOW-RELATIVE (`xdotool mousemove --window`). Absolute coords drift with the compositor.
#   * Networking is OFF by default in a fresh config, so any fetch/upgrade path fails with "node not started".
#     `start` seeds Settings.IPFS.Enabled=true so those paths are actually reachable.
#   * The scratch instance needs --bypass-single-instance-lock to coexist with your live GUI.
set -u
DISP="${VG_DISPLAY:-:1}"
SETTLE="${VG_SETTLE:-1.0}"
export DISPLAY="$DISP"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STATE="${TMPDIR:-/tmp}/vg-guidrive.state"

win_id() { xdotool search --name "^Vidya God$" 2>/dev/null | tail -1; }

case "${1:?usage: see header}" in
start)
  DATADIR="${2:-${VG_DATADIR:-${TMPDIR:-/tmp}/vg-guidrive.$$}}"
  mkdir -p "$DATADIR"
  # Seed a config with networking ON — otherwise every fetch/upgrade path dead-ends at "node not started".
  [ -f "$DATADIR/GlobalConfig.JSON" ] || cat >"$DATADIR/GlobalConfig.JSON" <<JSON
{ "LIBRARY": [], "Settings": { "IPFS": { "Enabled": true }, "PackageSources": [] } }
JSON
  echo "$DATADIR" >"$STATE"
  QT_QPA_PLATFORM=xcb setsid nohup "$ROOT/build/VidyaGod" \
      --data-dir "$DATADIR" --bypass-single-instance-lock \
      >"$DATADIR/gui.log" 2>&1 </dev/null &
  for _ in $(seq 1 40); do [ -n "$(win_id)" ] && break; sleep 1; done
  W=$(win_id)
  [ -z "$W" ] && { echo "GUI window never appeared — see $DATADIR/gui.log" >&2; exit 1; }
  echo "datadir: $DATADIR"
  echo "log:     $DATADIR/gui.log"
  xdotool getwindowgeometry "$W"
  ;;
geom)
  W=$(win_id); [ -z "$W" ] && { echo "no GUI window" >&2; exit 1; }
  xdotool getwindowgeometry "$W"
  ;;
click)
  X="${2:?x}"; Y="${3:?y}"; W=$(win_id)
  [ -z "$W" ] && { echo "no GUI window" >&2; exit 1; }
  xdotool mousemove --window "$W" "$X" "$Y"; sleep "$SETTLE"; xdotool click 1
  ;;
type)
  # Slow + clearmodifiers: the default speed silently swaps case (see notes).
  xdotool type --clearmodifiers --delay 60 "${2:?text}"
  ;;
key)
  xdotool key --clearmodifiers "${2:?keysym}"
  ;;
shot)
  spectacle -m -b -n -o "${2:?out.png}" 2>/dev/null; sleep 1; ls -la "$2"
  ;;
dialog)
  xdotool search --name "${2:?title fragment}" 2>/dev/null | tail -1
  ;;
stop)
  # Kill ONLY the scratch instance (matched by its --data-dir), never the user's live GUI.
  [ -f "$STATE" ] || { echo "no scratch instance recorded"; exit 0; }
  D=$(cat "$STATE")
  pkill -f "VidyaGod --data-dir $D" 2>/dev/null
  sleep 2
  echo "stopped scratch instance ($D)"
  ;;
*) sed -n '2,40p' "$0"; exit 1 ;;
esac
