#!/usr/bin/env bash
# Mutation-test one edit: apply it, build, run EVERY suite, restore FROM GIT, rebuild, re-verify.
#
#   tools/mutate.sh <file> <<'PY'
#   <python: read `s`, produce `out`>
#   PY
#
# WHY THIS EXISTS. The hand-rolled version of this loop shipped a RED commit and reported it green. Two
# defects, both of which this script removes by construction:
#   * it restored from a `cp` snapshot taken at an arbitrary earlier point, so an edit made AFTER the snapshot
#     was silently reverted along with the mutation — and the commit that described that edit kept the test
#     golden it had moved. Restore is `git checkout --` here: the tree goes back to HEAD+staged, never to
#     whatever a stale copy happened to hold.
#   * it re-ran only the suite being looked at. A mutation in shared code fails a DIFFERENT suite, and an
#     accidental revert shows up in one you did not think to run. All three run, every time, both times.
#
# Exit status is the mutation's: 0 = the mutation SURVIVED (the tests did not catch it, which is a finding
# about the tests), non-zero = it was caught.
set -u
FILE="${1:?file to mutate}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if ! git diff --quiet -- "$FILE" && [ -z "${VG_MUTATE_ALLOW_DIRTY:-}" ]; then
  echo "REFUSING: $FILE has uncommitted changes; `git checkout --` would discard them." >&2
  echo "Stage or commit them first, or set VG_MUTATE_ALLOW_DIRTY=1 if they are staged." >&2
  exit 2
fi

SCRIPT="$(mktemp)"; cat > "$SCRIPT"
trap 'rm -f "$SCRIPT"' EXIT

apply() {
  python3 - "$FILE" "$SCRIPT" <<'DRIVER'
import sys, runpy
path, script = sys.argv[1], sys.argv[2]
s = open(path).read()
ns = {"s": s}
exec(open(script).read(), ns)
out = ns.get("out")
assert out is not None, "the mutation script must set `out`"
assert out != s, "MUTATION DID NOT APPLY (no change to the file)"
open(path, "w").write(out)
DRIVER
}

suites() {
  if ! cmake --build build -j"$(nproc)" 2>&1 | grep -E " error" | head -5; then :; fi
  if cmake --build build -j"$(nproc)" 2>&1 | grep -qE " error"; then echo "BUILD FAILED"; return 1; fi
  local bad=0
  #ctest runs every gui suite as well as these two; the two are named because their output is the useful
  #detail, and ctest is what makes sure nothing ELSE was broken by the edit.
  ./build/vg_tests 2>&1 | grep -E "^\[FAIL\]" && bad=1
  QT_QPA_PLATFORM=offscreen ./build/test_pkgcanvas 2>&1 | grep -E "^FAIL" && bad=1
  ctest --test-dir build 2>&1 | grep -E "\(Failed\)" && bad=1
  return $bad
}

echo "--- applying mutation to $FILE"
apply || { echo "MUTATION DID NOT APPLY"; exit 2; }
echo "--- suites WITH the mutation"
if suites; then CAUGHT=0; echo "RESULT: SURVIVED - no suite caught it"; else CAUGHT=1; echo "RESULT: caught"; fi

echo "--- restoring $FILE from git and re-verifying"
git checkout -- "$FILE"
if suites; then echo "restored clean"; else echo "RESTORE IS NOT GREEN - the tree was already broken, or the restore did not take" >&2; exit 3; fi
exit $((1 - CAUGHT))
