#!/bin/bash
# Install UserPatch with one feature flag set, in a throwaway Wine prefix, and record what it changed.
#
#   UP_WORK=<dir> runr.sh <name> <flags>        (flags = SetupAoC.exe -f: digits; 0 off, 1 on, 2 installer default)
#
# UP_WORK holds: base/ (a TC 1.0e game tree: AoK + 2.0a + TC + 1.0c + 1.0e extracted in chain order),
# UserPatch/SetupAoC.exe (from UserPatch.v1.5.20190228-000000.zip), pfx0/ (a template prefix: `WINEPREFIX=pfx0 wineboot -i`).
# Writes out/<name>.exe (the installed age2_x1.exe), out/<name>.extra (files it added to age2_x1/), out/<name>.reg
# (registry lines it changed); KEEPW=1 also keeps the age2_x1 folder as out/<name>.dir.
set -u
W=${UP_WORK:?set UP_WORK}
P=$W/pfx_$1; rm -rf "$P"; cp -a "$W/pfx0" "$P"
export WINEPREFIX=$P WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
G=$W/w_$1; rm -rf "$G"; mkdir -p "$G" "$W/out"
cp -as "$W/base/." "$G/" 2>/dev/null; rm -f "$G/age2_x1/age2_x1.exe"; cp "$W/base/age2_x1/age2_x1.exe" "$G/age2_x1/"
cp "$W/UserPatch/SetupAoC.exe" "$G/"
( cd "$G" && timeout 120 wine SetupAoC.exe -i -b -l -f:"$2" > "$W/out/$1.log" 2>&1 ); wineserver -w
cp "$G/age2_x1/age2_x1.exe" "$W/out/$1.exe" 2>/dev/null
ls "$G/age2_x1" | grep -v -x -f <(ls "$W/base/age2_x1") > "$W/out/$1.extra"
for h in system user; do
    diff <(grep -a -v '^#time\|^\[.*\] [0-9]*$' "$W/pfx0/$h.reg") <(grep -a -v '^#time\|^\[.*\] [0-9]*$' "$P/$h.reg") | grep '^[<>]' | sed "s/^/$h /"
done > "$W/out/$1.reg"
[ -n "${KEEPW:-}" ] && cp -a "$G/age2_x1" "$W/out/$1.dir"
rm -rf "$G" "$P"
