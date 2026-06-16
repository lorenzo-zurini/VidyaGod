#!/bin/bash
# Hang-proof headless launch for verification. Runs VidyaGod DETACHED in its own session (=process group),
# waits until the runner is invoked (engine resolved+mounted+spawned) or a timeout, then GUARANTEES teardown:
# kill the whole process group + lazy-unmount any leftover FUSE mount via /proc (never stat-ing the mountpoint,
# which would block in D-state on a dead mount). This is what prevents the stale-mount hangs.
#   usage: headless_run.sh <logfile> <maxsecs> <vidyagod args...>
BIN=/home/lorenzo-zurini/Code/VidyaGod/build/VidyaGod
LOG="$1"; MAX="$2"; shift 2
clean_mounts() { for mp in $(awk '$2 ~ /VidyaGod\/TEMP/{print $2}' /proc/self/mounts 2>/dev/null); do fusermount3 -uz "$mp" 2>/dev/null; done; }
# Kill wine background services that daemonize out of our process group and would otherwise linger (and, before
# the O_CLOEXEC lock fix, kept the single-instance lock held). Generic wine-only names — safe to SIGKILL.
kill_wine() { pkill -9 wineserver 2>/dev/null; for w in services.exe winedevice.exe plugplay.exe explorer.exe svchost.exe rpcss.exe conhost.exe start.exe wineboot.exe; do pkill -9 "$w" 2>/dev/null; done; }
# Pre-clean leftovers from any prior run.
pkill -9 -x VidyaGod 2>/dev/null; pkill -9 -x snes9x 2>/dev/null; kill_wine; pkill -9 -x vidyagodfs 2>/dev/null
clean_mounts
# Launch detached: setsid => new session, so $! is the process-group leader.
setsid "$BIN" "$@" >"$LOG" 2>&1 &
PID=$!
for ((i=0; i<MAX; i++)); do
  grep -q "ContainerWrapper::Execute Runner:" "$LOG" 2>/dev/null && { sleep 2; break; }
  grep -qE "aborting|No runner found|Could not resolve|Game did not" "$LOG" 2>/dev/null && break
  kill -0 "$PID" 2>/dev/null || break
  sleep 1
done
# Guaranteed teardown: kill the whole group + wine daemons, then unmount.
kill -9 -"$PID" 2>/dev/null
pkill -9 -x snes9x 2>/dev/null; pkill -9 -x wine 2>/dev/null; kill_wine; pkill -9 -x vidyagodfs 2>/dev/null
sleep 1; clean_mounts
echo "[headless_run] teardown complete (waited ${i}s)"
