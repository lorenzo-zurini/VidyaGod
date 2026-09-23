#!/usr/bin/env python3
"""Keep Pinata at exactly ONE pin: the published library manifest.

Pinata bills every pin its whole closure, so pinning each library root pinned the same content hundreds of
times over (928 root pins = 1.15 TB for 63 GB of content). The library manifest block reaches every root,
every node and every content CID, so it is the only pin needed.

  tools/pinata_manifest.py status                  pins, billed size, manifest CID + its job state
  tools/pinata_manifest.py pin-manifest            pin GlobalConfig.JSON's PublishedManifest (async on Pinata's side)
  tools/pinata_manifest.py wipe-rest               unpin everything that is NOT the current manifest
  tools/pinata_manifest.py jobs                    list pending pin jobs

Credential: ~/.pinata_jwt (never printed). Data dir: --data-dir (default ~/.VidyaGodSeeder).
"""
import json, os, sys, time, urllib.request, urllib.error

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


def unpin_all(rows, label):
    ok = err = 0
    for i, r in enumerate(rows, 1):
        try: call(f"/pinning/unpin/{r['ipfs_pin_hash']}", method="DELETE"); ok += 1
        except Exception as e: err += 1; print("  unpin error", r["ipfs_pin_hash"][:20], str(e)[:100])
        if i % 250 == 0: print(f"  {label}: {i}/{len(rows)}", flush=True)
    print(f"{label}: unpinned {ok}, errors {err}")


argv = sys.argv[1:]
dd = "~/.VidyaGodSeeder"
if "--data-dir" in argv:
    i = argv.index("--data-dir"); dd = argv[i + 1]; del argv[i:i + 2]
args = [a for a in argv if not a.startswith("--data-dir=")]
dd = next((a.split("=", 1)[1] for a in argv if a.startswith("--data-dir=")), dd)
cfg = json.load(open(os.path.expanduser(dd + "/GlobalConfig.JSON")))
manifest = cfg.get("PublishedManifest", "")
phase = args[0] if args else "status"
tot = call("/data/userPinnedDataTotal")
print(f"pins {tot.get('pin_count')} | billed {tot.get('pin_size_total', 0) / 1e9:.1f} GB | manifest {manifest[:24] or '(none yet)'}")
if phase == "status":
    if manifest: print("manifest job:", jobs().get(manifest, "no job"), "| pinned:", any(r["ipfs_pin_hash"] == manifest for r in pins()))
elif phase == "jobs":
    for c, s in jobs().items(): print(" ", s, c)
elif phase == "pin-manifest":
    if not manifest: sys.exit("no PublishedManifest in config — publish first")
    print(call("/pinning/pinByHash", {"hashToPin": manifest, "pinataMetadata": {"name": "VidyaGod library manifest"}}))
elif phase == "wipe-rest":
    if not manifest: sys.exit("no PublishedManifest in config — refusing to unpin everything; publish first")
    rest = [r for r in pins() if r["ipfs_pin_hash"] != manifest]
    print("pins to drop (everything but the manifest):", len(rest))
    unpin_all(rest, "rest")
    print("pins now:", call("/data/userPinnedDataTotal").get("pin_count"))
else:
    sys.exit(__doc__)
