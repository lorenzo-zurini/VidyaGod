#!/bin/bash
# Runs a local .cmd file on the Windows test VM (dockurr "winvm", ssh -p 2222 dev@localhost) in the logged-on
# user's INTERACTIVE session, through a one-shot scheduled task, waits for it and prints its output.
# A Sandboxie box started straight from the SSH logon runs its program as ANONYMOUS LOGON, and it never starts,
# so a launch test over plain ssh hangs; real launches come from the desktop session, as these do.
# Every run has its own job id, so a job left running by an earlier timed-out run cannot pass for this one.
# The job runs in C:\vgjobs\<id>; cmd looks for a bare program name there before PATH, so scripts name full paths.
# Usage: tools/winvm/vmrun.sh script.cmd [timeout seconds]   Exit code = the script's (124 on timeout).
set -euo pipefail
F=$1; T=${2:-180}
ID=j$(date +%s)$RANDOM
O=(-P 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)
SSH=(ssh -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR dev@localhost)
J="C:\\vgjobs\\$ID"
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
printf '@echo off\r\ncd /d %s\r\ncall %s\\job.cmd > %s\\job.log 2>&1\r\n> %s\\done echo %%ERRORLEVEL%%\r\n' "$J" "$J" "$J" "$J" > "$W/wrap.cmd"
sed 's/\r\?$/\r/' "$F" > "$W/job.cmd"
"${SSH[@]}" "schtasks /end /tn vgjob >nul 2>&1 & mkdir $J"
scp -q "${O[@]}" "$W/job.cmd" "$W/wrap.cmd" "dev@localhost:C:/vgjobs/$ID/"
"${SSH[@]}" "schtasks /create /tn vgjob /tr $J\\wrap.cmd /sc once /st 00:00 /it /ru dev /f >nul 2>&1 && schtasks /run /tn vgjob >nul"
for ((i = 0; i < T; i += 2)); do
    "${SSH[@]}" "if exist $J\\done (exit 0) else (exit 1)" && break
    sleep 2
done
"${SSH[@]}" "type $J\\job.log 2>nul & exit 0"
RC=$("${SSH[@]}" "type $J\\done 2>nul" | tr -dc '0-9' || true)
[ -n "$RC" ] || { "${SSH[@]}" 'schtasks /end /tn vgjob >nul 2>&1' || true; echo "[vmrun] TIMEOUT after ${T}s (job $ID ended)"; exit 124; }
exit "$RC"
