#!/bin/bash
# Capture normalized --resolve-only dumps of the canary games, for phase-invariance diffing.
# Usage: capture-goldens.sh <outdir>   (compare with `diff -r baseline current`)
set +e
BIN=~/Code/VidyaGod/build/VidyaGod
OUT="${1:-$HOME/scratch-nfs/goldens-current}"
mkdir -p "$OUT"
for G in nfsu2_base_game aoe2_tc sh2_base_game mc_1_20_1_game; do
  "$BIN" --resolve-only "$G" >/dev/null 2>&1
  F=~/.VidyaGod/vg_resolve_$G.json
  [ -f "$F" ] && sed -E "s|$HOME|\$HOME|g; s|/TEMP/[0-9]+/|/TEMP/\$UID/|g" "$F" > "$OUT/$G.golden"
done
echo "goldens in $OUT"
