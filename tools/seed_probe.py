#!/usr/bin/env python3
"""seed_probe.py — seed ONE file from libvgipfs.so directly and serve it for a while.

Usage: seed_probe.py <libvgipfs.so> <repo-dir> <file> [serve-seconds]
Prints "CID=<cid>" once seeded, then keeps the node alive serving it. Used by the E2E battery (phase 5) to
prove the BUILD UNDER TEST can seed+serve fresh content (ambient seeders can't serve a file that never
existed before this run). Exit 2 on load/start/seed failure.
"""
import ctypes, os, sys, time

lib_path, repo, path = sys.argv[1], sys.argv[2], sys.argv[3]
serve = int(sys.argv[4]) if len(sys.argv) > 4 else 60
try:
    lib = ctypes.CDLL(os.path.expanduser(lib_path))
except OSError as e:
    print(f"probe-error: cannot load {lib_path}: {e}", file=sys.stderr)
    sys.exit(2)
err, cid = ctypes.c_char_p(), ctypes.c_char_p()
if lib.VgStart(repo.encode(), ctypes.byref(err)) != 0:
    print(f"probe-error: VgStart failed: {err.value}", file=sys.stderr)
    sys.exit(2)
time.sleep(8)  # let the network come up so the CID is announced + servable
if lib.VgAddNoCopy(os.path.abspath(path).encode(), ctypes.byref(cid), ctypes.byref(err)) != 0:
    print(f"probe-error: VgAddNoCopy failed: {err.value}", file=sys.stderr)
    lib.VgStop()
    sys.exit(2)
print(f"CID={cid.value.decode()}", flush=True)
time.sleep(serve)  # keep serving while the other machine fetches
lib.VgStop()
