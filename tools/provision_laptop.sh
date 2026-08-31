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

# The GUI holds a single-instance lock on the IPFS node; the config edit below needs it free.
pkill -f 'build/VidyaGod' 2>/dev/null && sleep 2

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
# --upgrade-source merge-upgrades in place: already-hydrated content is KEPT and the old version is demoted
# rather than deleted, so this is not a re-download of the whole library.
timeout 900 ./build/VidyaGod --upgrade-source VidyaGod "$GAMES_CID" 2>&1 | grep -aiE "upgrade|source|error|SUC" | tail -12

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
exit $RC
