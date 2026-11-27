#!/usr/bin/env python3
"""Edit frozen nodes in a LIBRARY and re-mint: each edited node gets the CID of its canonical bytes, and every node
that references an old CID is rewritten (and re-minted) in turn, until nothing changes.

  remint.py <LIBRARY> <edits.py> [--apply]

edits.py defines EDITS = {old_cid: fn(node) -> None (mutates)} and optionally NEW = [(package_dir, node_dict)].
Without --apply: prints what would change (old -> new CIDs, referrers) and writes nothing.

A node's CID is the CID of its canonical bytes, whatever its file is called (MetaPackageFormat chapter 02 §2.5: node
files are named by their labels). A re-minted node is rewritten where it is; a new one is named by its label
(bumped with its CID's tail when the name is taken). Publishing names every node file by the rule.
"""
import json, os, sys, importlib.util
sys.path.insert(0, os.path.expanduser("~/Code/VidyaGod/tools/gen6"))
from resolve import canonical, cid_of, node_refs, remap_node

lib, edits_path = sys.argv[1], sys.argv[2]
apply = "--apply" in sys.argv
spec = importlib.util.spec_from_file_location("edits", edits_path)
E = importlib.util.module_from_spec(spec); spec.loader.exec_module(E)

DEVICES = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"} | {f"COM{i}" for i in range(1, 10)} | {f"LPT{i}" for i in range(1, 10)}

def safe_name(s):
    """NodeFiles::SafeName: a label as one file name — the same bytes on every machine."""
    o = bytearray(c if (chr(c).isascii() and chr(c).isalnum()) or chr(c) in "_-. []()" else ord("_") for c in s.encode("utf-8"))[:120]
    o = o.decode("ascii")
    if o.strip(".") == "": o = "_" * max(len(o), 1)
    i = len(o)
    while i > 0 and o[i - 1] in ". ":
        i -= 1
    o = o[:i] + "_" * (len(o) - i)
    if o.split(".")[0].rstrip(" ").upper() in DEVICES: o = "_" + o
    return o

nodes = {}   # cid -> [path, node]
for d, dirs, fs in os.walk(lib):
    dirs[:] = [x for x in dirs if not x.startswith("_peer_")]
    for f in fs:
        if not f.endswith(".json"):
            continue
        p = os.path.join(d, f)
        b = open(p, "rb").read()
        try:
            n = json.loads(b)
        except ValueError:
            continue
        if not isinstance(n, dict) or not isinstance(n.get("LAYERS"), list):
            continue                                  # not a node (a content file that happens to be JSON)
        if "CID" in n:
            print(f"SKIP draft (a CID handle): {p}"); continue
        assert b == canonical(n), f"{p} is not its node's canonical form"
        c = cid_of(b)
        if c in nodes: nodes[c][0].append(p)          # the same node in several package folders
        else: nodes[c] = [[p], n]

planned = set()   # paths this run writes: two new nodes of one label never share a file

def new_path(pkg_dir, n, c):
    name = safe_name(n.get("LABEL") or "node") + ".json"
    p = os.path.join(pkg_dir, name)
    taken = p.lower() in planned or (os.path.exists(p) and cid_of(open(p, "rb").read()) != c)
    if taken:
        p = os.path.join(pkg_dir, name[:-5] + " [" + c[-8:] + "].json")   # publishing names every claimant by the rule
    assert p.lower() not in planned, f"{p} is planned twice"
    planned.add(p.lower())
    return p

new_nodes = []
for pkg, n in getattr(E, "NEW", []):
    c = cid_of(canonical(n))
    new_nodes.append((new_path(os.path.join(lib, pkg), n, c), n, c))
    print(f"NEW  {c}  {n.get('LABEL')}  in {pkg}")

def remap_refs(o, m):
    """Every reference renamed: NODE, INTO members, and node CIDs inside a WHEN (resolve.remap_node)."""
    return remap_node(o, m)

for c, fn in E.EDITS.items():
    assert c in nodes, f"edit target {c} not in library"
    fn(nodes[c][1])

sys.setrecursionlimit(100000)
final = {}   # original cid -> final cid (refs mapped recursively)
def refs_of(o, out):
    for _, r in node_refs(o):
        if r in nodes: out.add(r)
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
            writes.append((p, canonical(n)))          # rewritten where it is: its file is named by its label
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
