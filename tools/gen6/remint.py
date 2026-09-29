#!/usr/bin/env python3
"""Edit frozen nodes in a LIBRARY and re-mint: each edited node gets the CID of its canonical bytes, and every node
that references an old CID is rewritten (and re-minted) in turn, until nothing changes.

  remint.py <LIBRARY> <edits.py> [--apply]

edits.py defines EDITS = {old_cid: fn(node) -> None (mutates)} and optionally NEW = [(package_dir, node_dict)].
Without --apply: prints what would change (old -> new CIDs, referrers) and writes nothing.
"""
import json, os, sys, importlib.util
sys.path.insert(0, os.path.expanduser("~/Code/VidyaGod/tools/gen6"))
from resolve import canonical, cid_of

lib, edits_path = sys.argv[1], sys.argv[2]
apply = "--apply" in sys.argv
spec = importlib.util.spec_from_file_location("edits", edits_path)
E = importlib.util.module_from_spec(spec); spec.loader.exec_module(E)

nodes = {}   # cid -> [path, node]
for d, dirs, fs in os.walk(lib):
    dirs[:] = [x for x in dirs if not x.startswith("_friend_")]
    for f in fs:
        if f.endswith(".json") and f.startswith("baf"):
            p = os.path.join(d, f)
            b = open(p, "rb").read()
            n = json.loads(b)
            c = f[:-5]
            assert cid_of(b) == c, f"{p} does not hash to its name"
            if c in nodes: nodes[c][0].append(p)      # a dependency's node copied into several package folders
            else: nodes[c] = [[p], n]

new_nodes = []
for pkg, n in getattr(E, "NEW", []):
    c = cid_of(canonical(n))
    new_nodes.append((os.path.join(lib, pkg, c + ".json"), n, c))
    print(f"NEW  {c}  {n.get('LABEL')}  in {pkg}")

def remap_refs(o, m):
    """Replace every string that is an old CID (NODE / ANY / NOT refs) — nodes reference by CID string only."""
    if isinstance(o, dict):
        return {k: remap_refs(v, m) for k, v in o.items()}
    if isinstance(o, list):
        return [remap_refs(v, m) for v in o]
    if isinstance(o, str) and o in m:
        return m[o]
    return o

for c, fn in E.EDITS.items():
    assert c in nodes, f"edit target {c} not in library"
    fn(nodes[c][1])

sys.setrecursionlimit(100000)
final = {}   # original cid -> final cid (refs mapped recursively)
def refs_of(o, out):
    if isinstance(o, dict):
        for v in o.values(): refs_of(v, out)
    elif isinstance(o, list):
        for v in o: refs_of(v, out)
    elif isinstance(o, str) and o in nodes: out.add(o)
def fin(c):
    if c in final: return final[c]
    final[c] = c                      # cycle guard (frozen CIDs cannot cycle)
    rs = set(); refs_of(nodes[c][1], rs)
    m = {r: fin(r) for r in rs}
    nodes[c][1] = remap_refs(nodes[c][1], {k: v for k, v in m.items() if k != v})
    final[c] = cid_of(canonical(nodes[c][1]))
    return final[c]
for c in list(nodes): fin(c)

writes, removes = [], []
for c, (ps, n) in nodes.items():
    nc = final[c]
    if nc != c:
        for p in ps:
            writes.append((os.path.join(os.path.dirname(p), nc + ".json"), canonical(n))); removes.append(p)
            print(f"MINT {c[:24]} -> {nc[:24]}  {n.get('LABEL','')[:60]}  [{os.path.relpath(os.path.dirname(p), lib)}]")
for p, n, c in new_nodes:
    writes.append((p, canonical(n)))

# dangling check: every NODE ref resolves
allc = set(final.values()) | {c for _, _, c in new_nodes}
for c, (ps, n) in nodes.items():
    p = ps[0]
    for L in n.get("LAYERS", []):
        r = L.get("NODE") if isinstance(L, dict) else None
        if isinstance(r, str) and r.startswith("baf") and r not in allc:
            print(f"DANGLING {r} in {p}"); sys.exit(1)

if apply:
    for p, b in writes:
        os.makedirs(os.path.dirname(p), exist_ok=True)
        tmp = p + ".tmp"
        open(tmp, "wb").write(b); os.replace(tmp, p)
    for p in removes:
        os.remove(p)
    print(f"applied: {len(writes)} written, {len(removes)} removed")
else:
    print(f"dry run: {len(writes)} would be written, {len(removes)} removed")
