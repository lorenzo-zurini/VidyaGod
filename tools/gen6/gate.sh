#!/usr/bin/env bash
# The generation-6 equivalence gate, end to end: a scratch copy of the gen-5 library → migrate → self-tests →
# equivalence (960 launchables + option launches + 12 runner roots + the shelf) → the C++ resolver against the
# reference (fold_gate.py; VG_FOLD=path overrides build/vg_fold) → the mutation harness.
#   tools/gen6/gate.sh <gen5-work-dir> [--no-mutants]
# <gen5-work-dir> holds dumps/ and base/ (gen5_dumps.py, or the archived ground truth + --relink).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
g5="$(cd "$1" && pwd)"; shift
g6="$g5/../gen6-lib-$$"
trap 'rm -rf "$g6" "$g6".pre-gen6-*.tar' EXIT
python3 "$here/resolve.py" --self-test
python3 "$here/migrate.py" --self-test
cp -r "$g5/base/LIBRARY" "$g6"
python3 "$here/migrate.py" "$g6" --report "$g5/migrate-report.json"
python3 "$here/equivalence.py" "$g5" "$g6" --json "$g5/equivalence-report.json" --record "$g5/resolves.jsonl"
# the C++ resolver (build/vg_fold) against the reference, on exactly the resolves above + the rule fixtures
python3 "$here/fold_gate.py" "$g6" "$g5/resolves.jsonl" ${VG_FOLD:+--vg-fold "$VG_FOLD"}
[[ "${1:-}" == "--no-mutants" ]] || python3 "$here/mutate_gate.py" "$g5" "$g6"
