#!/usr/bin/env python3
"""Remove the retired PUBLISH flag from every node file (sharing is per package now — the flag has no meaning).

  python3 tools/strip_publish.py <LIBRARY dir> [--dry-run]

Backs up the library to <root>.pre-strip-publish-backup-<ts>.tar before writing.
"""
import json, os, sys, glob, tarfile, time, collections

args = [a for a in sys.argv[1:] if not a.startswith("--")]
if not args: sys.exit(__doc__)
root = args[0]; dry = "--dry-run" in sys.argv
todo = []
for p in sorted(glob.glob(os.path.join(root, "**", "*.json"), recursive=True)):
    try: d = json.load(open(p), object_pairs_hook=collections.OrderedDict)
    except ValueError: continue
    nodes = d if isinstance(d, list) else [d]
    if any(isinstance(n, dict) and "PUBLISH" in n for n in nodes): todo.append((p, d))
print(f"{len(todo)} node file(s) carry PUBLISH")
if dry: sys.exit(0)
if todo:
    ts = time.strftime("%Y%m%d-%H%M%S")
    tar = os.path.normpath(root) + f".pre-strip-publish-backup-{ts}.tar"
    with tarfile.open(tar, "w") as T:   # node files only — content never changes here (and it is tens of GB)
        for p, _ in todo: T.add(p, arcname=os.path.relpath(p, os.path.dirname(os.path.normpath(root))))
    for p, d in todo:
        for n in (d if isinstance(d, list) else [d]):
            if isinstance(n, dict): n.pop("PUBLISH", None)
        with open(p, "w") as F: json.dump(d, F, indent=2, ensure_ascii=False); F.write("\n")
    print(f"rewrote {len(todo)} file(s); backup: {tar}")
