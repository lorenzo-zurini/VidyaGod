#!/usr/bin/env bash
# store-zips.sh — re-pack every .zip under a library directory as STORE (zip -0) so vidyagodfs serves
# them zero-copy (direct pread, no RAM/scratch — required for very large packages). Already-stored
# archives are skipped. Each conversion is staged in a temp file and integrity-checked (entry count +
# total uncompressed bytes) before atomically replacing the original; on any mismatch the original is
# kept. Symlinks and timestamps are preserved.
#
# Usage: tools/store-zips.sh [LIBRARY_DIR]   (default: "$HOME/The Vidya")
set -uo pipefail
LIB="${1:-$HOME/The Vidya}"
[ -d "$LIB" ] || { echo "no such dir: $LIB" >&2; exit 1; }

# total uncompressed bytes of a zip (sum of the Length column), and entry count
zip_len()   { unzip -l "$1" 2>/dev/null | tail -1 | awk '{print $1}'; }
zip_count() { unzip -l "$1" 2>/dev/null | tail -1 | awk '{print $2}'; }
# true if EVERY entry is stored (no compression)
all_stored() {
  # true (0) if every entry is STORED. Pure awk (portable across grep variants, e.g. ugrep): emit a
  # marker the moment a compressed entry is seen; all-stored iff no marker was emitted.
  local bad
  bad=$(unzip -v "$1" 2>/dev/null | awk '$1 ~ /^[0-9]+$/ && $2 ~ /^[A-Za-z]/ && $2 != "Stored" {print "x"; exit}')
  [ -z "$bad" ]
}

conv=0 skip=0 fail=0
while IFS= read -r -d '' zip; do
  if all_stored "$zip"; then skip=$((skip+1)); continue; fi
  echo "converting: $zip"
  tmpd=$(mktemp -d -p "$(dirname "$zip")") || { fail=$((fail+1)); continue; }
  tmpz=$(mktemp -p "$(dirname "$zip")" --suffix=.zip) || { rm -rf "$tmpd"; fail=$((fail+1)); continue; }
  rm -f "$tmpz"   # zip wants to create it
  if unzip -qq "$zip" -d "$tmpd" && ( cd "$tmpd" && zip -q -0 -r -y -X "$tmpz" . ); then
    if [ "$(zip_count "$zip")" = "$(zip_count "$tmpz")" ] && [ "$(zip_len "$zip")" = "$(zip_len "$tmpz")" ]; then
      mv -f "$tmpz" "$zip" && conv=$((conv+1)) || { echo "  replace FAILED" >&2; rm -f "$tmpz"; fail=$((fail+1)); }
    else
      echo "  integrity mismatch (count/size), keeping original" >&2; rm -f "$tmpz"; fail=$((fail+1))
    fi
  else
    echo "  unzip/zip FAILED, keeping original" >&2; rm -f "$tmpz"; fail=$((fail+1))
  fi
  rm -rf "$tmpd"
done < <(find "$LIB" -type f -name '*.zip' -print0)
echo "done: converted=$conv  skipped(already stored)=$skip  failed=$fail"
