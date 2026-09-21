#!/usr/bin/env python3
"""Migrate the node identity key NODE_ID → LABEL across a library tree, preserving every reference.

The gigagraph identity is the CID; NODE_ID retires in favour of an OPTIONAL, pretty, TRAVELLING LABEL that also
serves as the intra-tree authoring handle. A launchable already carried a top-level LABEL (its variant name, e.g.
"GOTY") — that IS the node's pretty name, so it MERGES with the identity: one LABEL per node.

Because references (PARENTS / LIBRARYITEM / EXCLUDE / RUNNER) name a node by its handle VALUE, a naive key rename
would break them wherever the chosen LABEL differs from the old NODE_ID. So this is a TWO-PASS migration:

  Pass 1 — for every node build handle_map[old NODE_ID] = new LABEL, where new LABEL is the node's existing top-level
           LABEL if it has one (the pretty variant name), else the old NODE_ID. Warn on any collision (two nodes
           resolving to the same LABEL — a handle clash the scan would later deny).
  Pass 2 — rewrite every node: set LABEL = new LABEL, drop NODE_ID; and rewrite each reference string through
           handle_map so PARENTS/LIBRARYITEM/EXCLUDE/RUNNER still resolve.

NODE_ID was part of the hashed dag-json, so this changes every CID → the library must be RE-MINTED afterwards
(Verify & Publish), then re-shared.

Usage:
    tools/migrate_node_id_to_label.py <ROOT> [more roots...]         # dry run (prints the plan + any collisions)
    tools/migrate_node_id_to_label.py --apply <ROOT> [more roots]    # rewrite in place (JSON-only backup first)

Safety: JSON-ONLY backup per root before any write; key order preserved; sort_keys NEVER used.
"""
import json, os, sys, tarfile, time
from collections import OrderedDict

REF_LIST_KEYS = ("PARENTS", "EXCLUDE")      # arrays of handle strings
REF_STR_KEYS  = ("LIBRARYITEM", "RUNNER")   # single handle strings

def iter_nodes(doc):
    if isinstance(doc, list):
        for n in doc:
            if isinstance(n, dict): yield n
    elif isinstance(doc, dict):
        yield doc

def load(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f, object_pairs_hook=OrderedDict)
    except Exception:
        return None

def new_label(node):
    lbl = node.get("LABEL")
    if isinstance(lbl, str) and lbl:
        return lbl                      # existing pretty label (variant name) wins
    nid = node.get("NODE_ID")
    return nid if isinstance(nid, str) else None

def build_map(files):
    handle_map, collisions = {}, {}
    seen = {}
    for path in files:
        doc = load(path)
        if doc is None: continue
        for n in iter_nodes(doc):
            nid = n.get("NODE_ID")
            if not isinstance(nid, str) or not nid: continue
            lbl = new_label(n)
            if lbl is None: continue
            handle_map[nid] = lbl
            if lbl in seen and seen[lbl] != nid:
                collisions.setdefault(lbl, set()).update({seen[lbl], nid})
            seen[lbl] = nid
    return handle_map, collisions

def rewrite_node(node, hmap):
    out = OrderedDict()
    for k, v in node.items():
        if k == "NODE_ID":
            continue                    # dropped; re-emitted as LABEL below (or already present)
        out[k] = v
    lbl = new_label(node)
    if lbl is not None:
        out["LABEL"] = lbl              # single, canonical LABEL (order: replaces where LABEL/NODE_ID was)
    # rewrite references through the handle map
    for k in REF_LIST_KEYS:
        if isinstance(out.get(k), list):
            out[k] = [hmap.get(x, x) if isinstance(x, str) else x for x in out[k]]
    for k in REF_STR_KEYS:
        if isinstance(out.get(k), str):
            out[k] = hmap.get(out[k], out[k])
    return out

def rewrite_file(path, hmap, apply):
    doc = load(path)
    if doc is None: return 0
    changed = 0
    if isinstance(doc, list):
        new = []
        for n in doc:
            if isinstance(n, dict) and ("NODE_ID" in n or "LABEL" in n or any(k in n for k in REF_LIST_KEYS + REF_STR_KEYS)):
                nn = rewrite_node(n, hmap); new.append(nn); changed += (nn != n)
            else:
                new.append(n)
        doc2 = new
    else:
        doc2 = rewrite_node(doc, hmap) if isinstance(doc, dict) else doc
        changed = (doc2 != doc)
    if changed and apply:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(doc2, f, indent=2, ensure_ascii=False)
            f.write("\n")
    return 1 if changed else 0

def json_backup(root):
    ts = time.strftime("%Y%m%d-%H%M%S")
    dst = f"{root.rstrip('/')}.node-id-migration-backup-{ts}.tar"
    with tarfile.open(dst, "w") as tar:
        for dp, _, files in os.walk(root):
            for fn in files:
                if fn.endswith(".json"):
                    p = os.path.join(dp, fn)
                    tar.add(p, arcname=os.path.relpath(p, os.path.dirname(root)))
    return dst

def all_json(roots):
    out = []
    for root in roots:
        for dp, _, files in os.walk(root):
            for fn in files:
                if fn.endswith(".json"): out.append(os.path.join(dp, fn))
    return out

def main(argv):
    apply = "--apply" in argv
    roots = [a for a in argv[1:] if a != "--apply"]
    roots = [r for r in roots if os.path.isdir(r)]
    if not roots:
        print(__doc__); return 2
    files = all_json(roots)
    hmap, collisions = build_map(files)
    if collisions:
        print("!! LABEL COLLISIONS — these would clash as handles (fix the labels before applying):")
        for lbl, ids in collisions.items():
            print(f"   '{lbl}' <- {sorted(ids)}")
        if apply:
            print("!! refusing to apply with collisions present"); return 1
    if apply:
        for root in roots:
            print(f"== JSON backup {root} → {json_backup(root)}")
    n = sum(rewrite_file(p, hmap, apply) for p in files)
    verb = "rewrote" if apply else "would rewrite"
    print(f"== {verb} {n} file(s); {len(hmap)} handle(s) mapped NODE_ID→LABEL, references preserved")
    print("== NEXT: re-mint (Verify & Publish) — every CID changed — then re-share." if apply
          else "== dry run — re-run with --apply")
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
