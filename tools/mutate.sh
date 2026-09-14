#!/usr/bin/env bash
# Mutation-test one edit: apply it, build, run EVERY suite, restore FROM GIT, rebuild, re-verify.
#
#   tools/mutate.sh <file> <<'PY'
#   <python: read `s`, produce `out`>
#   PY
#
# WHY THIS EXISTS. The hand-rolled loop it replaces shipped a RED commit and reported it green, twice over:
#   * it restored from a `cp` snapshot taken at an arbitrary earlier point, so an edit made AFTER the snapshot
#     was silently reverted along with the mutation. Restore is `git checkout --` here: the file goes back to
#     what is in the index, never to whatever a stale copy happened to hold.
#   * it re-ran only the suite being looked at. A mutation in shared code fails a DIFFERENT suite, and an
#     accidental revert shows up in one you did not think to run. All three run, both times.
#
# Exit status: 0 = the mutation SURVIVED (a finding about the tests), 1 = caught, 2 = the mutation was not a
# valid experiment (did not apply, or did not compile — a mutation that does not build tells you nothing
# about the tests), 3 = the tree was not green after restoring (something else is wrong; stop).
#
# Refuses to run if the target file has UNSTAGED changes, with no override: `git checkout --` would discard
# them, and there is no situation in which that is what you wanted. Staged changes are never at risk.
set -u
FILE="${1:?file to mutate}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if ! git diff --quiet -- "$FILE"; then
  echo "REFUSING: $FILE has unstaged changes that 'git checkout --' would discard. Stage them first." >&2
  exit 2
fi

SCRIPT="$(mktemp)"; cat > "$SCRIPT"
trap 'rm -f "$SCRIPT"' EXIT

apply() {
  python3 - "$FILE" "$SCRIPT" <<'DRIVER'
import sys
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

# 0 = green, 1 = a suite failed, 2 = did not build.
suites() {
  local log; log="$(cmake --build build -j"$(nproc)" 2>&1)" || { echo "$log" | grep -E " error" | head -5; return 2; }
  local bad=0
  # ctest covers every suite; the two are named so their per-test detail is in the output.
  ./build/vg_tests 2>&1 | grep -E "^\[FAIL\]" && bad=1
  QT_QPA_PLATFORM=offscreen ./build/test_pkgcanvas 2>&1 | grep -E "^FAIL" && bad=1
  ctest --test-dir build 2>&1 | grep -E "\(Failed\)" && bad=1
  return $bad
}

echo "--- applying mutation to $FILE"
apply || { echo "RESULT: MUTATION DID NOT APPLY"; exit 2; }
echo "--- suites WITH the mutation"
suites; RC=$?
case $RC in
  0) echo "RESULT: SURVIVED - no suite caught it" ;;
  1) echo "RESULT: caught" ;;
  *) echo "RESULT: DID NOT BUILD - not a valid mutation" ;;
esac

echo "--- restoring $FILE from git and re-verifying"
git checkout -- "$FILE"
if suites; then echo "restored clean"; else echo "RESTORE IS NOT GREEN - the tree was already broken, or the restore did not take" >&2; exit 3; fi
case $RC in 0) exit 0 ;; 1) exit 1 ;; *) exit 2 ;; esac
