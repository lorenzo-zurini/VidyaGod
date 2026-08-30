#!/usr/bin/env bash
# Reproduce a package OUTSIDE VidyaGod: a plain proton prefix with a SIMULATED install of the package's content.
#
# THE RULE (2026-08-30): before debugging any package failure, reproduce it here first. Otherwise you cannot tell
# a game bug from a VidyaGod bug — a wrong mount, a bad DllOverride or a missing layer looks exactly like the game
# misbehaving, and hours get spent on the wrong one.
#
#   tools/pkgdebug.sh setup <name> <zip> [zip…]   fresh prefix + extract zips IN ORDER (base first, patches after)
#   tools/pkgdebug.sh run   <name> <exe> [args…]  run an exe inside that prefix
#   tools/pkgdebug.sh shell <name>                print the env to run things by hand
#   tools/pkgdebug.sh rm    <name>                delete the prefix
#
# Env: VG_PROTON (default newest GE-Proton in compatibilitytools.d), WINEDEBUG (passed through).
set -u
ROOT="${VG_PKGDEBUG_ROOT:-$HOME/pkgdebug}"
PROTON="${VG_PROTON:-$(ls -d "$HOME/.local/share/Steam/compatibilitytools.d/"GE-Proton* 2>/dev/null | sort -V | tail -1)}"
[ -d "$PROTON" ] || { echo "no proton found; set VG_PROTON" >&2; exit 1; }

name="${2:-}"; [ -n "$name" ] || { sed -n '2,16p' "$0"; exit 1; }
PFX="$ROOT/$name"
export STEAM_COMPAT_DATA_PATH="$PFX"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$PFX"
export WINEPREFIX="$PFX/pfx"
# Name the log deterministically and keep it WITH the prefix: without SteamGameId proton has no log name, so
# WINEDEBUG output silently goes nowhere and the run looks like it produced no diagnostics at all.
export SteamGameId="$name"
export PROTON_LOG=1
export PROTON_LOG_DIR="$PFX"
# A package's LIBRARY PARENTS are part of the reproduction. Omitting them makes the simulated install unfaithful
# and produces false conclusions: Wipeout XL's config dialog renders BLACK without dgVoodoo and fine with it, so a
# prefix missing dgVoodoo "reproduces" a bug that VidyaGod does not actually have. Pass them via VG_DLLOVERRIDES
# (and drop any DLLs the library places next to the exe into drive_c/game yourself).
[ -n "${VG_DLLOVERRIDES:-}" ] && export WINEDLLOVERRIDES="$VG_DLLOVERRIDES"

# Kill EVERYTHING from a previous run: the game, proton's wrapper, the wine service processes, and any X windows
# they left behind. Leftovers are not cosmetic — a stale window keeps answering xdotool searches, so the next run
# locks onto a DEAD window and every click and keystroke goes nowhere while the log stays silent. (Also: a hung
# game attracts kwin_killer_helper, which then sits there too.) Never pkill -f a pattern that appears in this
# script's own command line; match by PID.
teardown() {
  local pids
  pids=$(ps -eo pid,args | grep -iE "pkgdebug\.sh run|steam\.exe|wineserver|winedevice|services\.exe|explorer\.exe|plugplay|rpcss|kwin_killer_helper|\.exe" \
         | grep -viE "grep|/usr/bin/bash -c" | awk '{print $1}')
  for p in $pids; do kill -9 "$p" 2>/dev/null; done
  sleep 2
  # Stale X windows outlive their process; kill them or the next run targets a corpse.
  if [ -n "${1:-}" ]; then
    for w in $(xdotool search --name "$1" 2>/dev/null); do xdotool windowkill "$w" 2>/dev/null; done
  fi
  sleep 1
}

case "${1:?usage: see header}" in
kill|teardown)
  teardown "${3:-Wipeout}"
  left=$(ps -eo args | grep -iE "net-woxl|wolobby|wineserver|winedevice" | grep -vcE "grep|pkgdebug" || true)
  echo "remaining: ${left:-0}"
  ;;
setup)
  shift 2
  rm -rf "$PFX"; mkdir -p "$PFX/pfx/drive_c/game"
  echo "prefix: $PFX   proton: $(basename "$PROTON")"
  # Generate the prefix first so drive_c exists the way proton expects.
  "$PROTON/proton" run cmd /c ver >/dev/null 2>&1 || true
  for z in "$@"; do
    [ -f "$z" ] || { echo "missing zip: $z" >&2; exit 1; }
    echo "  + $(basename "$z")"
    unzip -o -q "$z" -d "$PFX/pfx/drive_c/game" || exit 1
  done
  echo "installed into $PFX/pfx/drive_c/game"
  find "$PFX/pfx/drive_c/game" -maxdepth 2 -iname "*.exe" -printf "    %P\n"
  ;;
run)
  exe="${3:?exe (path relative to drive_c/game, or absolute)}"; shift 3
  cd "$PFX/pfx/drive_c/game" || exit 1
  # ABSOLUTE path: proton resolves a bare name against its own cwd, not yours, and fails with
  # "run_process Failed to create process L\"X.EXE\": 2" (file not found) — which looks exactly like the game
  # refusing to start. Cost a debugging round; always hand proton a full path.
  case "$exe" in /*) ;; *) exe="$PFX/pfx/drive_c/game/$exe" ;; esac
  [ -f "$exe" ] || { echo "no such exe: $exe" >&2; exit 1; }
  teardown "${VG_WINDOW_HINT:-}"     # never inherit a previous run's processes or its stale windows
  echo "running $exe in $PFX"
  "$PROTON/proton" run "$exe" "$@"
  ;;
shell)
  echo "export STEAM_COMPAT_DATA_PATH=$PFX STEAM_COMPAT_CLIENT_INSTALL_PATH=$PFX WINEPREFIX=$PFX/pfx"
  echo "$PROTON/proton run <exe>"
  ;;
rm) rm -rf "$PFX"; echo "removed $PFX" ;;
*) sed -n '2,16p' "$0"; exit 1 ;;
esac
