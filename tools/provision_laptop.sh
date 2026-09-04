#!/usr/bin/env bash
# provision_laptop.sh — bring the laptop to the current commit and point it at the current games source.
#
# Two things are load-bearing for the Wipeout XL multiplayer test and both are easy to miss:
#   1. The SessionVars -> EXEARGS join path is NEW CODE. On an older binary %VIDYAGOD_JOIN_ADDRESS% never reaches
#      vglobby, so "Join" silently degrades into hosting a second empty session — it looks like a netcode bug.
#   2. The games source CID is CONFIG-ONLY (unlike libraries/runners, which are compiled into main.cpp), so it
#      does not arrive with the rebuild and has to be set explicitly.
#
# Never discards remote work: uncommitted changes are stashed with a named label, not reset away.
set -uo pipefail

HOST="${VG_LAPTOP_HOST:-lorenzo-zurini@10.10.0.5}"
REPO="${VG_LAPTOP_REPO:-~/Code/VidyaGod}"
COMMIT="${VG_COMMIT:?set VG_COMMIT to the commit to deploy}"
GAMES_CID="${VG_GAMES_CID:?set VG_GAMES_CID to the games collection CID}"

say() { printf '\n=== %s ===\n' "$*"; }

say "target: $HOST  commit: $COMMIT  games CID: $GAMES_CID"

ssh -o ConnectTimeout=10 -o BatchMode=yes "$HOST" bash -s -- "$REPO" "$COMMIT" "$GAMES_CID" <<'REMOTE'
set -uo pipefail
REPO="$1"; COMMIT="$2"; GAMES_CID="$3"
eval REPO="$REPO"
cd "$REPO" || { echo "!! no repo at $REPO"; exit 1; }

echo "== host: $(hostname)  cores: $(nproc)"

# NOTE: no pkill here. Earlier versions killed 'build/VidyaGod' — which missed a GUI launched via the ~/VidyaGod
# symlink (different cmdline) AND risked killing the user's live session or a running game sandbox (same binary).
# The build doesn't need the lock; only the upgrade step does, and it now tells you when the lock is the problem.

echo "== stashing any local work (never reset away)"
git stash push -u -m "provision_laptop $(date -Iseconds)" 2>&1 | tail -2

echo "== fetching + checking out $COMMIT"
git fetch --all --prune 2>&1 | tail -3
git checkout "$COMMIT" 2>&1 | tail -3 || { echo "!! checkout failed"; exit 1; }
git submodule update --init --recursive 2>&1 | tail -3
# A submodule that fails to reach its pinned commit leaves the OLD source in place and the build then succeeds
# against it -- a binary whose C++ and Go halves disagree. For the join flow that is invisible: the C++ sends a
# presence field the stale node never parses. Refuse rather than ship a half-updated build.
WANT="$(git ls-tree HEAD VidyaGodIPFS | awk '{print $3}')"
HAVE="$(git -C VidyaGodIPFS rev-parse HEAD 2>/dev/null)"
if [ -z "$WANT" ] || [ "$WANT" != "$HAVE" ]; then
    echo "!! SUBMODULE MISMATCH: VidyaGodIPFS is at ${HAVE:-<none>}, this commit pins $WANT"
    echo "!! The pinned commit is probably unpushed. Push VidyaGodIPFS, then re-run."
    echo "!! Refusing to build: the Go node would be older than the C++ that talks to it."
    exit 1
fi
echo "== now at: $(git log --oneline -1)  submodule: $(git -C VidyaGodIPFS log --oneline -1)"

echo "== building"
cmake -S . -B build >/dev/null 2>&1
if ! cmake --build build -j"$(nproc)" 2>&1 | tail -5; then
    echo "!! BUILD FAILED"; exit 1
fi
[ -x build/VidyaGod ] || { echo "!! no build/VidyaGod produced"; exit 1; }
echo "== build OK"

echo "== running the test suite"
ctest --test-dir build 2>&1 | tail -3

echo "== pointing the games source at $GAMES_CID"
# The source NAME is per-machine (this PC calls it "VidyaGod", the laptop "VidyaGod Games"), so discover it from
# the config rather than assuming — an assumed name just fails with "no package source named ...".
SRC_NAME="$(python3 - <<'PY'
import json, os
c = json.load(open(os.path.expanduser("~/.VidyaGod/GlobalConfig.JSON")))
names = [s.get("NAME","") for s in c.get("Settings", {}).get("PackageSources", [])]
games = [n for n in names if n not in ("VidyaGodLibraries", "VidyaGodRunners")]
print(games[0] if len(games) == 1 else "")
PY
)"
if [ -z "$SRC_NAME" ]; then
    echo "!! could not identify the games source by elimination — set it by hand"; exit 1
fi
echo "== games source is named '$SRC_NAME'"

CUR_CID="$(python3 - "$SRC_NAME" <<'PY2'
import json, os, sys
c = json.load(open(os.path.expanduser("~/.VidyaGod/GlobalConfig.JSON")))
for s in c.get("Settings", {}).get("PackageSources", []):
    if s.get("NAME") == sys.argv[1]: print(s.get("CID", "")); break
PY2
)"
if [ "$CUR_CID" = "$GAMES_CID" ]; then
    echo "== already at $GAMES_CID — skipping the upgrade (no lock needed, no network needed)"
else

# --upgrade-source merge-upgrades in place: already-hydrated content is KEPT and the old version is demoted
# rather than deleted, so this is not a re-download of the whole library.
#
# NOT piped into grep: a pipeline reports the EXIT STATUS OF THE LAST COMMAND, so `... | grep` returns grep's
# status and a failed upgrade reads as success. That is exactly how the first run of this script reported OK
# while the upgrade had actually failed on a wrong source name.
if ! timeout 1800 ./build/VidyaGod --upgrade-source "$SRC_NAME" "$GAMES_CID" > /tmp/vg_upgrade.log 2>&1; then
    echo "!! UPGRADE FAILED:"; grep -aiE "ERR |deadline" /tmp/vg_upgrade.log | tail -5
    echo "!! If it says 'Another instance': close VidyaGod on THIS machine first (single-instance lock)."
    echo "!! If this timed out fetching: the OTHER machine must be RUNNING VidyaGod to seed the CID."
    exit 1
fi
grep -aiE "Upgrade |content kept|content new|deprecated|SUC .*[Uu]pgrad" /tmp/vg_upgrade.log | tail -8
fi

echo "== resulting sources:"
python3 - <<'PY'
import json, os
c = json.load(open(os.path.expanduser("~/.VidyaGod/GlobalConfig.JSON")))
for s in c.get("Settings", {}).get("PackageSources", []):
    print(f'   {s.get("NAME"):20} {s.get("CID")}')
PY
REMOTE

RC=$?
say "provision exited $RC"
if [ "$RC" -ne 0 ]; then exit "$RC"; fi

# POST-BUILD REGRESSION: every laptop build immediately proves every user flow still works END TO END across
# both machines over the open internet (friending, virtual LAN + game traffic, link recovery, IPFS fetch +
# verify). A build that provisions but breaks a flow must fail HERE, loudly, not in the field.
# VG_SKIP_E2E=1 skips (e.g. when iterating on the provision script itself).
if [ "${VG_SKIP_E2E:-0}" != "1" ]; then
    say "post-build e2e regression (VG_SKIP_E2E=1 to skip)"
    VG_LAPTOP_HOST="$HOST" VG_GAMES_CID="$GAMES_CID" bash "$(dirname "$0")/e2e_regression.sh"
    RC=$?
    say "e2e regression exited $RC"
fi
exit $RC
