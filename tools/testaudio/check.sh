#!/bin/sh
# ctest audio_is_private: in the test environment, no tool can reach the owner's session audio. wpctl/pactl on PATH are
# the stubs, and the real ones (by absolute path) cannot connect. Teeth: drop the PATH prefix, or a dead remote.
here="$(cd "$(dirname "$0")" && pwd)"; fail=0
for t in wpctl pactl; do
  got="$(command -v "$t")"
  [ "$got" = "$here/$t" ] || { echo "FAIL: $t resolves to '$got', not the stub"; fail=1; }
done
[ "$(wpctl get-volume @DEFAULT_AUDIO_SINK@)" = "Volume: 0.50" ] || { echo "FAIL: wpctl is not the stub"; fail=1; }
for real in /usr/bin/wpctl /bin/wpctl; do
  [ -x "$real" ] || continue
  if timeout 5 "$real" get-volume @DEFAULT_AUDIO_SINK@ >/dev/null 2>&1; then echo "FAIL: $real reached a PipeWire server"; fail=1; fi
done
for real in /usr/bin/pactl /bin/pactl; do
  [ -x "$real" ] || continue
  if timeout 5 "$real" info >/dev/null 2>&1; then echo "FAIL: $real reached a sound server"; fail=1; fi
done
[ $fail = 0 ] && echo "audio is private: stubs on PATH, real servers unreachable"
exit $fail
