# Source this before running anything that starts VidyaGod (or any tool) on this machine for a test, a sweep, a perf or
# mutation run: the session's audio is the owner's, and nothing a test does may reach it. wpctl and pactl become the
# logging stubs beside this file, and PipeWire / PulseAudio clients (wpctl, pactl, libpulse, libpipewire) are pointed at
# sockets that do not exist, so even a tool run by absolute path cannot connect. ctest sets the same for every test.
# (A harness that RUNS GAMES needs a silent device instead: .scratch/hs.sh's private PipeWire. It may still source
# this with VG_AUDIO_KEEP_SERVERS=1 to take the stubs only.)
_vg_testaudio="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
export PATH="$_vg_testaudio:$PATH"
export VG_AUDIO_STUB_LOG="${VG_AUDIO_STUB_LOG:-${TMPDIR:-/tmp}/vg-audio-stub.log}"
if [ -z "${VG_AUDIO_KEEP_SERVERS:-}" ]; then
  export PIPEWIRE_REMOTE="${TMPDIR:-/tmp}/vg-no-pipewire-here"
  export PULSE_SERVER="unix:${TMPDIR:-/tmp}/vg-no-pulse-here"
fi
unset _vg_testaudio
