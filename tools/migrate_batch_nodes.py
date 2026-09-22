#!/usr/bin/env python3
"""Migrate a package (or a whole library) from one-item-per-node to BATCHED nodes + one-file-per-node.

Collapses same-TYPE batchable nodes that currently share a file into ONE batched node:
  CustomVar      -> {TYPE:CustomVar,   VARS:[{KEY,DEFAULT,COMMENT,UI,WHEN}, ...]}
  DeclarePersist -> {TYPE:DeclarePersist, PERSISTS:[{SCOPE,PATH,TARGET,CLOUD,WHEN}, ...]}
  Content        -> {TYPE:VFSLayer,    LAYERS:[{FORM,PATH,TARGET,SOURCE,WHEN,TOGGLE,SUBMOUNTS,BASE_TARGETS,COMMENT}, ...]}
  DllOverride    -> {TYPE:DllOverride, OVERRIDES:{...merged...}}   (schema already multi; just merges)
Then writes exactly ONE node per .json file (filename from LABEL, de-duped).

Wiring: the batched node REUSES its first member's CID as its handle; every other member's handle is
remapped to it. Refs (PARENTS/LIBRARYITEM) are rewritten with a GLOBAL refmap built across ALL packages
first — batchable nodes (runner/library vars, shared content) are referenced cross-package, so a per-package
map would dangle those. Edges internal to a collapsed group vanish (they are now one node). Launch follows
PARENTS handles + content SOURCE.CIDs (byte-hashes, unchanged), so the migrated tree launches as-is;
Verify&Publish re-mints the (now stale) node CIDs. Content SOURCE.CID bytes are NEVER touched.

Idempotency: a package already carrying VFSLayer/VARS/PERSISTS is skipped (refuse-guard).

Usage: migrate_batch_nodes.py <dir> [--dry-run]
  <dir> = a package bundle (dir with node .json files) OR a library root (recurses to package bundles).
"""
import json, os, sys, re

BATCH = {"CustomVar": "VARS", "DeclarePersist": "PERSISTS", "Content": "LAYERS", "DllOverride": "OVERRIDES"}
ITEM_FIELDS = {
    "CustomVar":      ["KEY", "DEFAULT", "COMMENT", "UI", "WHEN"],
    "DeclarePersist": ["SCOPE", "PATH", "TARGET", "CLOUD", "WHEN"],
    "Content":        ["FORM", "PATH", "TARGET", "SOURCE", "WHEN", "TOGGLE", "SUBMOUNTS", "BASE_TARGETS", "COMMENT"],
}


def is_node(n):  return isinstance(n, dict) and isinstance(n.get("TYPE"), str)


def load_nodes(path):
    d = json.load(open(path))
    return d if isinstance(d, list) else [d]


def sanitize(name, used):
    base = re.sub(r"[^A-Za-z0-9._-]", "_", name) or "node"
    fn, i = base, 2
    while fn in used:
        fn = f"{base}_{i}"; i += 1
    used.add(fn)
    return fn


# PASS 1: collapse a package's batchable groups PER FILE, PER TYPE. Returns (out_nodes, refmap_additions).
# out_nodes carry a "_raw_parents" marker on batched nodes (resolved against the GLOBAL refmap in pass 2).
def collapse_package(pkg):
    files = [os.path.join(pkg, x) for x in sorted(os.listdir(pkg)) if x.endswith(".json")]
    if not files:
        return None
    per_file = {f: load_nodes(f) for f in files}
    all_nodes = [n for ns in per_file.values() for n in ns if is_node(n)]
    if not all_nodes:
        return None
    if any(n.get("TYPE") == "VFSLayer" or "VARS" in n or "PERSISTS" in n or "LAYERS" in n for n in all_nodes):
        return "skip"

    refmap = {}
    out_nodes = []
    for f, ns in per_file.items():
        by_type = {}
        for n in ns:
            if is_node(n) and n.get("TYPE") in BATCH:
                by_type.setdefault(n["TYPE"], []).append(n)
        collapsed = set()
        for typ, members in by_type.items():
            first_cid = members[0].get("CID", "")
            for m in members:
                if m.get("CID"):
                    refmap[m["CID"]] = first_cid
                collapsed.add(id(m))
            batched = {"TYPE": "VFSLayer" if typ == "Content" else typ}
            if first_cid:
                batched["CID"] = first_cid
            batched["_raw_parents"] = [p for m in members for p in m.get("PARENTS", [])]
            batched["LABEL"] = members[0].get("LABEL") or typ.lower()
            for k in ("TOGGLE", "EXCLUDE"):
                for m in members:
                    if k in m:
                        batched[k] = m[k]; break
            if typ == "DllOverride":
                merged = {}
                for m in members:
                    merged.update(m.get("OVERRIDES", {}))
                batched["OVERRIDES"] = merged
            else:
                batched[BATCH[typ]] = [{k: m[k] for k in ITEM_FIELDS[typ] if k in m} for m in members]
            out_nodes.append(batched)
        for n in ns:
            if is_node(n) and id(n) not in collapsed:
                out_nodes.append(n)
    return (out_nodes, refmap)


# PASS 2: rewrite refs with the GLOBAL refmap, then write one node per file.
def rewrite_and_write(pkg, out_nodes, refmap, dry):
    def dedup(refs, self_cid):
        seen, ps = set(), []
        for p in refs:
            r = refmap.get(p, p)
            if r and r != self_cid and r not in seen:
                seen.add(r); ps.append(r)
        return ps
    for n in out_nodes:
        self_cid = n.get("CID", "")
        if "_raw_parents" in n:
            n["PARENTS"] = dedup(n.pop("_raw_parents"), self_cid)
        elif isinstance(n.get("PARENTS"), list):
            n["PARENTS"] = dedup(n["PARENTS"], self_cid)
        if isinstance(n.get("LIBRARYITEM"), str):
            n["LIBRARYITEM"] = refmap.get(n["LIBRARYITEM"], n["LIBRARYITEM"])
    if dry:
        return
    for f in [os.path.join(pkg, x) for x in os.listdir(pkg) if x.endswith(".json")]:
        os.remove(f)
    used = set()
    for n in out_nodes:
        n.pop("POS", None)                                   # canvas recomputes layout at publish
        name = sanitize(n.get("LABEL") or n.get("TYPE", "node"), used) + ".json"
        json.dump(n, open(os.path.join(pkg, name), "w"), indent=1)


def is_package(d):
    return any(x.endswith(".json") for x in os.listdir(d))


def find_packages(root):
    out = []
    for dirpath, _, filenames in os.walk(root):
        for jf in (f for f in filenames if f.endswith(".json")):
            try:
                if any(is_node(n) for n in load_nodes(os.path.join(dirpath, jf))):
                    out.append(dirpath); break
            except Exception:
                continue
    return sorted(set(out))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    dry = "--dry-run" in sys.argv
    if not args:
        print(__doc__); sys.exit(2)
    root = os.path.abspath(args[0])
    single = os.path.isdir(root) and is_package(root) and not any(
        os.path.isdir(os.path.join(root, x)) and is_package(os.path.join(root, x)) for x in os.listdir(root))
    pkgs = [root] if single else find_packages(root)

    # PASS 1 across ALL packages: collapse + accumulate a GLOBAL refmap.
    plans = {}
    global_refmap = {}
    n_skip = 0
    for p in pkgs:
        r = collapse_package(p)
        if r is None:
            continue
        if r == "skip":
            n_skip += 1; print(f"  [skip] {os.path.basename(p)}: already batched"); continue
        out_nodes, refmap = r
        global_refmap.update(refmap)
        plans[p] = out_nodes
    # PASS 2: rewrite refs with the global map + write.
    for p, out_nodes in plans.items():
        before = sum(1 for _ in out_nodes)   # placeholder; count reported below differs (informational)
        rewrite_and_write(p, out_nodes, global_refmap, dry)
        print(f"  [{'dry' if dry else 'ok'}] {os.path.basename(p)}: {len(out_nodes)} node(s)")
    print(f"\n{len(plans)} package(s) migrated, {n_skip} skipped; global refmap {len(global_refmap)} handle(s) remapped.")


if __name__ == "__main__":
    main()
