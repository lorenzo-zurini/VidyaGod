#!/usr/bin/env python3
"""health_probe.py — drive libvgipfs.so directly (no app) and print the VgHealth service report.

Usage: health_probe.py <libvgipfs.so> <repo-dir> [warm-seconds]
Prints one line per service: "<status> <name> -- <detail>". Exit 0 always (callers assert on the rows);
exit 2 only if the library itself cannot be loaded/started. Used by the E2E battery (phase 6) and handy
standalone for API smoke tests without launching VidyaGod.
"""
import ctypes, json, os, sys, time

lib_path, repo = sys.argv[1], sys.argv[2]
warm = int(sys.argv[3]) if len(sys.argv) > 3 else 12
try:
    lib = ctypes.CDLL(os.path.expanduser(lib_path))
except OSError as e:
    print(f"probe-error: cannot load {lib_path}: {e}", file=sys.stderr)
    sys.exit(2)
lib.VgHealth.argtypes = [ctypes.POINTER(ctypes.c_char_p)]
err, out = ctypes.c_char_p(), ctypes.c_char_p()
if lib.VgStart(repo.encode(), ctypes.byref(err)) != 0:
    print(f"probe-error: VgStart failed: {err.value}", file=sys.stderr)
    sys.exit(2)
time.sleep(warm)  # let goOnline join the swarm so Network/DHT/Reachability rows are meaningful
lib.VgHealth(ctypes.byref(out))
for e in json.loads(out.value.decode() or "[]"):
    print(f"{e['status']:5} {e['name']} -- {e['detail']}")
lib.VgStop()
