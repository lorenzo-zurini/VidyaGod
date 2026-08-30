#!/usr/bin/env bash
# Capture + compare what is ON SCREEN, so driving a game does not mean re-deriving screenshotting every time.
# Used by gamedrive.sh (games) and guidrive.sh (the GUI); usable directly.
#
#   tools/shot.sh win     <caption> <out.png>            capture JUST that window, cropped, nothing else
#   tools/shot.sh screen  <out.png>                      whole screen (works on Wayland too)
#   tools/shot.sh stable  <caption> <out.png> [tmo] [n]  WAIT until the window stops changing, then capture
#   tools/shot.sh diff    <a.png> <b.png>                changed pixels + % (did the screen react?)
#   tools/shot.sh moving  <caption> [secs]               is the window animating? (video//loading vs idle menu)
#   tools/shot.sh dialogs                                list error/crash-looking windows currently up
#
# Env: VG_DISPLAY (default :1).
#
# WHY EACH VERB EXISTS (all three were re-invented per game before this):
#   * `win` USES `import -window <id>`, which grabs the WINDOW'S OWN PIXELS. Do NOT full-screen-capture and then
#     crop by xdotool geometry: under XWayland a window's X-side position does NOT match where the compositor
#     draws it, so the crop lands on the wrong pixels (the same mismatch that makes absolute clicks miss).
#     A window-only shot is also far easier to read back than a 1920x1080 desktop with panels and wallpaper.
#   * `stable` REPLACES `sleep 30`. Fixed sleeps are how you screenshot a black loading frame and conclude the
#     game is broken, or miss a splash entirely. This polls until N consecutive frames are identical, so it
#     returns the moment the game settles — and FAILS LOUDLY on timeout instead of handing back a lie.
#   * `moving` answers "is a video playing / is it still loading?" — the question behind "did the intro run?".
#     A menu is near-static; a playing movie is not. Cheap proxy when you cannot read the game's own logs.
#   * `dialogs` is crash detection. ⚠ Windows of a KILLED process can linger on screen; a stale "Error" window
#     made a crash look live for hours once. Always tear down (gamedrive.sh teardown kills orphaned X windows
#     FIRST) before trusting this.
set -u
export DISPLAY="${VG_DISPLAY:-:1}"

win_id() { xdotool search --name "$1" 2>/dev/null | tail -1; }

# Capture one window to $2. Returns NON-ZERO when the window does not exist.
#
# ⚠ NO SILENT FULL-SCREEN FALLBACK. The first version fell back to a whole-desktop grab whenever the caption was
# missing, which made `stable` declare "stable after 4s" while the game had not opened a window at all — it was
# measuring the motionless wallpaper. A capture that quietly photographs something else is worse than no capture.
# For a native-Wayland target (VidyaGod's own GUI has no X window), opt in with VG_ALLOW_SCREEN=1.
grab_win() {
  local cap="$1" out="$2" w
  w=$(win_id "$cap")
  if [ -n "$w" ] && import -silent -window "$w" "$out" 2>/dev/null; then return 0; fi
  if [ "${VG_ALLOW_SCREEN:-0}" = "1" ]; then
    grim "$out" 2>/dev/null || spectacle -m -b -n -o "$out" >/dev/null 2>&1
    return $?
  fi
  return 1
}

# Is this frame essentially featureless (a black/blank loading screen)? Uses pixel standard deviation.
#
# ⚠ A BLANK FRAME IS PERFECTLY STABLE. The first `stable` happily returned three identical BLACK frames four
# seconds after MechWarrior 3 opened its window but before it drew anything — "stable" in the letter, useless in
# the spirit. Stability alone is not evidence the game is up; the frame must also contain something.
is_blank() {
  local sd
  sd=$(identify -format "%[fx:standard_deviation]" "$1" 2>/dev/null || echo 1)
  awk -v v="$sd" 'BEGIN{ exit !(v < 0.02) }'
}

# Block until a window with that caption exists (the precondition every capture verb needs).
wait_win() {
  local cap="$1" tmo="${2:-60}"
  for ((i=0; i<tmo; i++)); do [ -n "$(win_id "$cap")" ] && return 0; sleep 1; done
  return 1
}

case "${1:?usage: see header}" in
win)
  CAP="${2:?caption}"; OUT="${3:?out.png}"
  grab_win "$CAP" "$OUT" || { echo "no window matching '$CAP' (set VG_ALLOW_SCREEN=1 for a Wayland target)" >&2; exit 1; }
  [ -s "$OUT" ] || { echo "capture failed: $CAP" >&2; exit 1; }
  identify -format "%wx%h %f\n" "$OUT" 2>/dev/null || ls -la "$OUT"
  ;;
screen)
  OUT="${2:?out.png}"
  grim "$OUT" 2>/dev/null || spectacle -m -b -n -o "$OUT" >/dev/null 2>&1
  ls -la "$OUT"
  ;;
stable)
  CAP="${2:?caption}"; OUT="${3:?out.png}"; TMO="${4:-60}"; NEED="${5:-3}"
  # TOLERANCE is a PERCENT of changed pixels, not an absolute count: on a 1920x1080 window the old 500-pixel rule
  # was 0.02%, so MechWarrior 3's pulsing menu orbs alone kept it "unstable" until timeout. A game menu is rarely
  # perfectly still (animated highlights, cursors, idle loops) — "settled" means small motion, not zero.
  TOL="${VG_STABLE_TOL:-1.5}"
  # The window must EXIST before "has it stopped changing?" means anything.
  wait_win "$CAP" "$TMO" || { echo "window '$CAP' never appeared within ${TMO}s" >&2; exit 1; }
  TMP="${TMPDIR:-/tmp}/vg-shot.$$"; SAME=0
  # DEADLINE in real seconds. The loop used to count ITERATIONS and call them seconds, so a capture+compare costing
  # ~2.7s turned "120s" into five and a half minutes of wall clock while reporting 120.
  START=$(date +%s); DEADLINE=$((START + TMO))
  while [ "$(date +%s)" -lt "$DEADLINE" ]; do
    grab_win "$CAP" "$TMP.b.png"
    if [ -f "$TMP.a.png" ] && [ -s "$TMP.b.png" ]; then
      PCT=$("$0" diff "$TMP.a.png" "$TMP.b.png" 2>/dev/null | grep -oE '\(([0-9.]+)%\)' | tr -d '()%' || echo 100)
      [ -z "$PCT" ] && PCT=100
      # Settled AND not blank: a black loading screen satisfies "unchanged" indefinitely.
      if awk -v p="$PCT" -v t="$TOL" 'BEGIN{exit !(p<t)}' && ! is_blank "$TMP.b.png"; then
        SAME=$((SAME+1))
      else
        SAME=0
      fi
      if [ "$SAME" -ge "$NEED" ]; then
        cp "$TMP.b.png" "$OUT"; rm -f "$TMP".*.png
        echo "stable after $(( $(date +%s) - START ))s (last delta ${PCT}%) → $OUT"; exit 0
      fi
    fi
    mv -f "$TMP.b.png" "$TMP.a.png" 2>/dev/null
  done
  cp -f "$TMP.a.png" "$OUT" 2>/dev/null; rm -f "$TMP".*.png
  echo "NEVER STABILIZED in ${TMO}s (captured last frame anyway) — treat $OUT with suspicion" >&2
  exit 1
  ;;
diff)
  python3 - "${2:?a.png}" "${3:?b.png}" <<'PY'
import sys
import numpy as np
from PIL import Image
a, b = (np.asarray(Image.open(p).convert('L'), dtype=int) for p in sys.argv[1:3])
if a.shape != b.shape:
    print("0 (size mismatch %s vs %s)" % (a.shape, b.shape)); raise SystemExit
n = int((np.abs(a - b) > 16).sum())
print("%d changed pixels (%.2f%%)" % (n, 100.0 * n / a.size))
PY
  ;;
moving)
  CAP="${2:?caption}"; SECS="${3:-4}"
  TMP="${TMPDIR:-/tmp}/vg-shot-mv.$$"
  grab_win "$CAP" "$TMP.a.png"; sleep "$SECS"; grab_win "$CAP" "$TMP.b.png"
  echo -n "over ${SECS}s: "; "$0" diff "$TMP.a.png" "$TMP.b.png"
  rm -f "$TMP".*.png
  ;;
dialogs)
  # Crash/error-looking top-levels. Deliberately broad — game crash dialogs are named inconsistently.
  for pat in Error error "encountered" "not respond" Watson Exception Fault crash; do
    for w in $(xdotool search --name "$pat" 2>/dev/null); do
      printf "%s\t%s\n" "$w" "$(xdotool getwindowname "$w" 2>/dev/null)"
    done
  done | sort -u
  ;;
*) sed -n '2,30p' "$0"; exit 1 ;;
esac
