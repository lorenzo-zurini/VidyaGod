#!/usr/bin/env python3
"""Merge single-purpose nodes into one pluripotent node — the hand tool for collapsing a package, game by game.

  python3 tools/merge_nodes.py <LIBRARY dir> --into <target label> <label> [<label> ...] [--label <new label>] [--dry-run]

The target is the TOP of the group (every other node is beneath it through bare refs); --label renames the result.

Every listed node folds INTO the target: its payload sections are prepended to the target's (it sits beneath, so
its items apply first within each kind; kinds apply in phases, so cross-kind order inside one node is free), the
target's OVER becomes the union of every merged node's refs that point OUTSIDE the merged set (in closure order,
deduplicated), the target inherits a merged node's TILE and ENTRYPOINTS when it has none, ENV nets out (a name the
target removes is dropped from a merged node's ENV), and every OTHER node in the library that referred to a merged
node is re-pointed at the target. Refuses when a merged node is a variant, a runner, carries TOGGLE/WHEN, lives
in another package, or is referred to by an any-of group or a NOT (those are requirements, not composition).
Labels must be unique inside the package. The library is backed up first (<root>.pre-merge-<ts>.tar).
"""
import json, os, sys, glob, tarfile, time, collections, copy

SECTIONS = ["LAYERS", "PATCHES", "FILEEDITS", "REGEDITS", "DLLOVERRIDES", "VARS", "PERSISTS", "ENV", "ENV_REMOVE"]


def load_all(root):
    nodes = {}
    for p in sorted(glob.glob(os.path.join(root, "**", "*.json"), recursive=True)):
        try: d = json.load(open(p), object_pairs_hook=collections.OrderedDict)
        except ValueError: continue
        if isinstance(d, dict) and "CID" in d: nodes[d["CID"]] = (d, p)
    return nodes


def refs_of(n):
    bare, other = [], []
    for x in n.get("OVER", []):
        if isinstance(x, str): bare.append(x)
        elif isinstance(x, list): other += [m for m in x if isinstance(m, str)]
        elif isinstance(x, dict) and isinstance(x.get("NOT"), str): other.append(x["NOT"])
    return bare, other


def main():
    args = sys.argv[1:]
    dry = "--dry-run" in args; args = [a for a in args if a != "--dry-run"]
    new_label = None
    if "--label" in args:
        i = args.index("--label"); new_label = args[i + 1]; del args[i:i + 2]
    if len(args) < 4 or args[1] != "--into": sys.exit(__doc__)
    root, target_label, merge_labels = args[0], args[2], args[3:]
    nodes = load_all(root)
    by_label = collections.defaultdict(list)
    for c, (n, p) in nodes.items(): by_label[n.get("LABEL", "")].append(c)

    def one(label):
        cs = by_label.get(label, [])
        if len(cs) != 1: sys.exit(f"label '{label}': {len(cs)} node(s) carry it — must be exactly one")
        return cs[0]
    T = one(target_label); M = [one(l) for l in merge_labels]
    pkg = os.path.dirname(nodes[T][1])
    for c in M:
        n, p = nodes[c]
        if os.path.dirname(p) != pkg: sys.exit(f"'{n['LABEL']}' lives in another package")
        if n.get("VARIANT"): sys.exit(f"'{n['LABEL']}' is a variant — a choice, never merged")
        if any(isinstance(e, dict) and e.get("GUEST") for e in n.get("ENTRYPOINTS", [])): sys.exit(f"'{n['LABEL']}' is a runner")
        if "TOGGLE" in n or "WHEN" in n: sys.exit(f"'{n['LABEL']}' carries TOGGLE/WHEN — its semantics would change")
    merged = set(M) | {T}
    # who refers to the merged nodes, and how
    for c, (n, p) in nodes.items():
        bare, other = refs_of(n)
        for r in other:
            if r in M: sys.exit(f"'{n['LABEL']}' names '{nodes[r][0]['LABEL']}' in a group/NOT — a requirement, cannot merge it")
    # closure order among the merged set: a node before the nodes above it (DFS over bare refs inside the set)
    order = []
    def visit(c, seen):
        if c in seen: return
        seen.add(c)
        for r in refs_of(nodes[c][0])[0]:
            if r in merged: visit(r, seen)
        order.append(c)
    visit(T, set())
    missing = [nodes[c][0]["LABEL"] for c in M if c not in order]
    if missing: sys.exit(f"not beneath the target through bare refs: {missing}")
    # build the merged node
    new = copy.deepcopy(nodes[T][0])
    for s in SECTIONS: new.pop(s, None)
    ext_over, seen_over = [], set()
    tile, entries = None, None
    env, env_remove = collections.OrderedDict(), []
    for c in order:
        n = nodes[c][0]
        for x in n.get("OVER", []):
            key = json.dumps(x, sort_keys=True)
            if isinstance(x, str) and x in merged: continue
            if key not in seen_over: seen_over.add(key); ext_over.append(x)
        for s in ("LAYERS", "PATCHES", "FILEEDITS", "VARS", "PERSISTS"):
            if s in n: new[s] = new.get(s, []) + n[s]
        if "REGEDITS" in n: new["REGEDITS"] = new.get("REGEDITS", []) + n["REGEDITS"]
        if "DLLOVERRIDES" in n:
            d = new.get("DLLOVERRIDES", collections.OrderedDict()); d.update(n["DLLOVERRIDES"]); new["DLLOVERRIDES"] = d
        for k in n.get("ENV_REMOVE", []):
            env.pop(k, None)
            if k not in env_remove: env_remove.append(k)
        for k, v in n.get("ENV", {}).items():
            env[k] = v
            if k in env_remove: env_remove.remove(k)
        if "TILE" in n and tile is None: tile = n["TILE"]
        if n.get("ENTRYPOINTS"): entries = n["ENTRYPOINTS"]   # the node above replaces: the last in order wins
    if env: new["ENV"] = env
    if env_remove: new["ENV_REMOVE"] = env_remove
    if tile is not None: new["TILE"] = tile
    if entries: new["ENTRYPOINTS"] = entries
    new["OVER"] = ext_over
    if not ext_over: new.pop("OVER", None)
    if new_label: new["LABEL"] = new_label      # e.g. keep the pristine's name when its registry (above it) is the top
    new.pop("POS", None)
    # re-point every referrer of a merged node at the target
    rewired = []
    for c, (n, p) in nodes.items():
        if c in merged: continue          # the target's own refs are rebuilt above; a merged node's file is removed
        changed = False
        for i, x in enumerate(n.get("OVER", [])):
            if isinstance(x, str) and x in M: n["OVER"][i] = T; changed = True
        if changed:
            seen, dedup = set(), []
            for x in n["OVER"]:
                k = json.dumps(x, sort_keys=True)
                if k not in seen: seen.add(k); dedup.append(x)
            n["OVER"] = dedup; rewired.append(c)
    print(f"merge into '{target_label}': {[nodes[c][0]['LABEL'] for c in order if c != T]}")
    print(f"  sections: {[(s, len(new[s])) for s in SECTIONS if s in new]}  OVER -> {[(nodes[x][0]['LABEL'] if isinstance(x, str) and x in nodes else x) for x in ext_over]}")
    print(f"  tile: {'yes' if 'TILE' in new else 'no'}  entries: {len(new.get('ENTRYPOINTS', []))}  referrers re-pointed: {[nodes[c][0]['LABEL'] for c in rewired]}")
    if dry: print("[dry-run]"); return
    ts = time.strftime("%Y%m%d-%H%M%S")
    tar = os.path.normpath(root) + f".pre-merge-{ts}.tar"
    with tarfile.open(tar, "w") as Tf:   # node files only: content never changes in a merge (and it is tens of GB)
        for pth in glob.glob(os.path.join(root, "**", "*.json"), recursive=True):
            Tf.add(pth, arcname=os.path.relpath(pth, os.path.dirname(os.path.normpath(root))))
    for c in M: os.remove(nodes[c][1])
    with open(nodes[T][1], "w") as F: json.dump(new, F, indent=2, ensure_ascii=False); F.write("\n")
    for c in rewired:
        with open(nodes[c][1], "w") as F: json.dump(nodes[c][0], F, indent=2, ensure_ascii=False); F.write("\n")
    print(f"written; {len(M)} node file(s) removed; backup: {tar}")


if __name__ == "__main__":
    main()
