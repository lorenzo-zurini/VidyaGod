#!/usr/bin/env python3
"""Keep Pinata pinned to exactly the published PACKAGE manifests.

A pinning service pins recursively and bills every pin its whole closure, so the pin set must be the set of
package manifests: each one reaches its package's node blocks and, through them, its content CIDs — the package
once, and only a changed package re-pins. There is no library-level block (a library is a name, never a CID).

  tools/pinata_manifest.py status         pins, billed size, and the diff against config["Libraries"]
  tools/pinata_manifest.py sync           pin every package manifest not yet pinned, unpin everything else
  tools/pinata_manifest.py jobs           list pending pin jobs

Credential: ~/.pinata_jwt (never printed). Data dir: --data-dir <dir> (default ~/.VidyaGodSeeder).
"""
import json, os, sys, urllib.request

API = "https://api.pinata.cloud"
JWT = open(os.path.expanduser("~/.pinata_jwt")).read().strip()


def call(path, body=None, method=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(API + path, data=data, method=method or ("POST" if data else "GET"))
    req.add_header("Authorization", "Bearer " + JWT)
    if data: req.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(req, timeout=60) as r:
        raw = r.read().decode()
    try: return json.loads(raw)
    except ValueError: return {"raw": raw}       # DELETE answers with plain text


def pins():
    out, off = [], 0
    while True:
        page = call(f"/data/pinList?status=pinned&pageLimit=1000&pageOffset={off}")
        rows = page.get("rows", [])
        out += rows
        if len(rows) < 1000: return out
        off += 1000


def jobs():
    return {j["ipfs_pin_hash"]: j["status"] for j in call("/pinning/pinJobs?limit=1000").get("rows", [])}


argv = sys.argv[1:]
dd = "~/.VidyaGodSeeder"
if "--data-dir" in argv:
    i = argv.index("--data-dir")
    if i + 1 >= len(argv): sys.exit("--data-dir needs a directory")
    dd = argv[i + 1]; del argv[i:i + 2]
args = [a for a in argv if not a.startswith("--data-dir=")]
dd = next((a.split("=", 1)[1] for a in argv if a.startswith("--data-dir=")), dd)
cfg = json.load(open(os.path.expanduser(dd + "/GlobalConfig.JSON")))
wanted = {}                                     # package manifest cid → "lib / pkg"
for lib, rows in (cfg.get("Libraries") or {}).items():
    for r in rows or []:
        if isinstance(r, dict) and r.get("cid"): wanted[r["cid"]] = f"{lib} / {r.get('pkg', '')}"
phase = args[0] if args else "status"
tot = call("/data/userPinnedDataTotal")
have = {r["ipfs_pin_hash"] for r in pins()}
pending = jobs()
missing = [c for c in wanted if c not in have]
extra = [c for c in have if c not in wanted]
print(f"pins {tot.get('pin_count')} | billed {tot.get('pin_size_total', 0) / 1e9:.1f} GB | published packages {len(wanted)} | "
      f"pinned {len(wanted) - len(missing)} | missing {len(missing)} ({sum(1 for c in missing if c in pending)} in a job) | extra {len(extra)}")
if phase == "status":
    for c in missing: print("  missing", c[:24], wanted[c], "|", pending.get(c, "no job"))
    for c in extra: print("  extra  ", c[:24])
elif phase == "jobs":
    for c, s in pending.items(): print(" ", s, c[:24], wanted.get(c, ""))
elif phase == "sync":
    if not wanted: sys.exit("config lists no published packages — publish first")
    for c in missing:
        if c in pending: continue
        call("/pinning/pinByHash", {"hashToPin": c, "pinataMetadata": {"name": "VidyaGod " + wanted[c]}})
        print("  pin   ", c[:24], wanted[c])
    for c in extra:
        try: call(f"/pinning/unpin/{c}", method="DELETE"); print("  unpin ", c[:24])
        except Exception as e: print("  unpin error", c[:24], str(e)[:100])
    print("pins now:", call("/data/userPinnedDataTotal").get("pin_count"))
else:
    sys.exit(__doc__)
