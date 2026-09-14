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

# TRACKED, checked first: `git diff --quiet` passes for an UNTRACKED file, and `git checkout --` then fails
# with a pathspec error that this script used to ignore — so the "restore" run executed the still-mutated file
# and, if it happened to be green, printed "restored clean" and exited 0. A false SURVIVED on a permanently
# mutated tree is the worst output this tool can produce.
if ! git ls-files --error-unmatch -- "$FILE" >/dev/null 2>&1; then
  echo "REFUSING: $FILE is not tracked by git, so there is nothing to restore it from." >&2
  exit 2
fi
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
#
# A suite that CRASHES must count as caught. Grepping a suite's stdout for "FAIL" cannot see an abort — the
# process dies before it prints one — and ctest labels a crash "(Subprocess aborted)" or "(SEGFAULT)", never
# "(Failed)". An earlier version checked only those, so `std::abort()` in SafeId killed two suites outright and
# was reported "SURVIVED": blind to precisely the failure this codebase spent the changeset closing (an
# exception out of paintGL). EXIT STATUS is the thing that cannot be faked, so it is what decides; the greps
# only surface the useful detail.
suites() {
  local log; log="$(cmake --build build -j"$(nproc)" 2>&1)" || { echo "$log" | grep -E " error" | head -5; return 2; }
  local bad=0
  # ctest covers every suite; the two are named so their per-test detail is in the output.
  # TIMED OUT, because a mutation can make a loop infinite — SafeId's byte walk is the obvious candidate — and
  # a hung harness is not a result. ctest brings its own 1500s default; these two had none. `timeout` exits
  # 124, which is non-zero, so a hang counts as caught exactly like a crash does.
  timeout 600 ./build/vg_tests 2>&1 | grep -E "^\[FAIL\]"
  [ "${PIPESTATUS[0]}" -eq 0 ] || { bad=1; echo "  (vg_tests exited non-zero - failed, crashed or timed out)"; }
  QT_QPA_PLATFORM=offscreen timeout 600 ./build/test_pkgcanvas 2>&1 | grep -E "^FAIL"
  [ "${PIPESTATUS[0]}" -eq 0 ] || { bad=1; echo "  (test_pkgcanvas exited non-zero - failed, crashed or timed out)"; }
  ctest --test-dir build 2>&1 | grep -E "\(Failed\)|\(Subprocess aborted\)|\(SEGFAULT\)|\(Timeout\)"
  [ "${PIPESTATUS[0]}" -eq 0 ] || bad=1
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
git checkout -- "$FILE" || { echo "RESTORE FAILED - $FILE is still mutated. Fix the tree by hand." >&2; exit 3; }
if suites; then echo "restored clean"; else echo "RESTORE IS NOT GREEN - the tree was already broken, or the restore did not take" >&2; exit 3; fi
case $RC in 0) exit 0 ;; 1) exit 1 ;; *) exit 2 ;; esac
