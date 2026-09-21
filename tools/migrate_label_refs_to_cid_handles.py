#!/usr/bin/env python3
"""Migrate a library from LABEL-as-handle (Model B) to CID-as-handle (Model C).

Model C makes identity EXCLUSIVELY the CID and LABEL PURELY cosmetic. A node's authoring handle — what PARENTS and
LIBRARYITEM reference, and what GatherWorkingTree keys on — becomes its stored top-level "CID" (the CID it last minted
to). This migration does the pure-JSON half: it gives every node a UNIQUE, STABLE placeholder handle ("premint-N") in a
new "CID" field and rewrites PARENTS / LIBRARYITEM from label values to those handles. It does NOT compute real CIDs —
that happens on the next Verify & Publish, which mints every node and (StampNodeCids) writes the real CIDs back over the
placeholders, remapping the references with them. The migrated tree is already fully functional before that publish
(the catalog/resolver derive real CIDs on the fly; the editor wires by the placeholder handles).

Why placeholders instead of the label itself: LABEL is cosmetic and MAY repeat (RoC/TFT "v1.21b", two "Vanilla"
editions). A per-node placeholder is unique by construction, so distinct nodes never collide on their handle — exactly
the collision class Model C eliminates. Reference targets (content nodes, tiles) always have UNIQUE labels (only leaf
launchables share display names, and nothing references a leaf), so mapping a reference's label → the target's handle is
unambiguous.

Left UNTOUCHED (matched by label/NodeId at resolve time, only ever within one game's variant set where labels are
distinct): EXCLUDE and RUNNER. And LABEL itself stays, now purely cosmetic.

Usage:
    tools/migrate_label_refs_to_cid_handles.py <ROOT> [more roots...]        # dry run (plan + any dup-label notes)
    tools/migrate_label_refs_to_cid_handles.py --apply <ROOT> [more roots]   # rewrite in place (JSON-only backup first)

After --apply: open the app and Verify & Publish (mints real CIDs, replaces the placeholders), then re-share.
Safety: JSON-ONLY backup per root before any write; key order preserved; sort_keys NEVER used.
"""
import hashlib, json, os, sys, tarfile, time
from collections import OrderedDict

REF_LIST_KEYS = ("PARENTS",)        # arrays of handle strings that are TREE/BUILD edges (become CID handles)
REF_STR_KEYS  = ("LIBRARYITEM",)    # single handle string (the tile edge)
# NB: EXCLUDE and RUNNER are deliberately NOT remapped — they stay label/NodeId-matched.

def iter_nodes(doc):
    if isinstance(doc, list):
        for n in doc:
            if isinstance(n, dict): yield n
    elif isinstance(doc, dict):
        yield doc

def is_node(n):
    return isinstance(n, dict) and isinstance(n.get("TYPE"), str)

def load(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f, object_pairs_hook=OrderedDict)
    except Exception as e:
        print(f"!! WARNING: could not read {path} ({e}) — skipped; references into it will NOT be migrated")
        return None

def all_json(roots):
    out = []
    for root in roots:
        for dp, _, files in os.walk(root):
            for fn in files:
                if fn.endswith(".json"): out.append(os.path.join(dp, fn))
    return out

def node_positions(doc):
    """Yield (full-array-index, node_dict) for every node — indexed by POSITION IN THE WHOLE ARRAY (not just among
    node objects), so build_handles and rewrite_file agree on the key even when a file mixes nodes with stray non-node
    elements. A single-object file is position 0."""
    if isinstance(doc, list):
        for i, n in enumerate(doc):
            if is_node(n): yield i, n
    elif is_node(doc):
        yield 0, doc

def handle_for(abs_path, index):
    """A DETERMINISTIC handle from (absolute file path, position). Stable across re-runs and independent of the rest of
    the file set, so adding/removing one file never renumbers (hence never mis-wires) the others. (Premint handles are
    ephemeral — the next publish mints the real, content-derived CIDs — so cross-machine equality is not needed.)"""
    h = hashlib.sha1(f"{abs_path}#{index}".encode("utf-8")).hexdigest()[:16]
    return f"premint-{h}"

def build_handles(files):
    """Assign every node its deterministic handle; map each UNIQUE label → its handle (for reference rewriting)."""
    node_handle = {}            # (path, full-index) → handle
    label_to_handle = {}        # label → handle  (first-seen wins; only unique labels are trustworthy ref targets)
    dup_labels = {}             # label → count   (cosmetic duplicates)
    already_migrated = 0        # nodes already carrying a "CID" (a prior run / partial migration)
    for path in sorted(files):  # sorted so first-seen (for a duplicate label) is deterministic
        doc = load(path)
        if doc is None: continue
        ap = os.path.abspath(path)
        for i, n in node_positions(doc):
            if isinstance(n.get("CID"), str) and n["CID"]: already_migrated += 1
            h = handle_for(ap, i)
            node_handle[(path, i)] = h
            lbl = n.get("LABEL")
            if isinstance(lbl, str) and lbl:
                if lbl in label_to_handle: dup_labels[lbl] = dup_labels.get(lbl, 1) + 1
                else: label_to_handle[lbl] = h
    return node_handle, label_to_handle, dup_labels, already_migrated

def referenced_labels(files):
    """Every label that appears in a PARENTS/LIBRARYITEM position — so we can tell a harmless cosmetic-duplicate label
    from one that is actually a reference target (where first-seen resolution matters)."""
    refd = set()
    for path in files:
        doc = load(path)
        if doc is None: continue
        for _, n in node_positions(doc):
            for k in REF_LIST_KEYS:
                if isinstance(n.get(k), list):
                    for x in n[k]:
                        if isinstance(x, str): refd.add(x)
            for k in REF_STR_KEYS:
                if isinstance(n.get(k), str): refd.add(n[k])
    return refd

def rewrite_ref(v, label_to_handle):
    """A reference value: a label → its target's handle; anything already a handle/CID/unknown passes through."""
    if isinstance(v, str) and v in label_to_handle:
        return label_to_handle[v]
    return v

def rewrite_node(n, handle, label_to_handle):
    out = OrderedDict()
    # Put "CID" first (cosmetic ordering only — dag-json canonicalises keys, so it never affects the minted CID).
    out["CID"] = handle
    for k, v in n.items():
        if k == "CID":
            continue                        # replace any stale handle
        out[k] = v
    for k in REF_LIST_KEYS:
        if isinstance(out.get(k), list):
            out[k] = [rewrite_ref(x, label_to_handle) for x in out[k]]
    for k in REF_STR_KEYS:
        if k in out:
            out[k] = rewrite_ref(out[k], label_to_handle)
    return out

def rewrite_file(path, node_handle, label_to_handle, apply):
    doc = load(path)
    if doc is None: return 0
    changed = 0
    if isinstance(doc, list):
        new = []
        for i, n in enumerate(doc):   # FULL-array index — must match node_positions() in build_handles
            if is_node(n) and (path, i) in node_handle:
                nn = rewrite_node(n, node_handle[(path, i)], label_to_handle); new.append(nn); changed += (nn != n)
            else:
                new.append(n)
        doc2 = new
    elif is_node(doc) and (path, 0) in node_handle:
        doc2 = rewrite_node(doc, node_handle[(path, 0)], label_to_handle); changed = (doc2 != doc)
    else:
        doc2 = doc
    if changed and apply:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(doc2, f, indent=2, ensure_ascii=False)
            f.write("\n")
    return 1 if changed else 0

def json_backup(root):
    ts = time.strftime("%Y%m%d-%H%M%S")
    dst = f"{root.rstrip('/')}.label-to-cid-handle-backup-{ts}.tar"
    with tarfile.open(dst, "w") as tar:
        for dp, _, files in os.walk(root):
            for fn in files:
                if fn.endswith(".json"):
                    p = os.path.join(dp, fn)
                    tar.add(p, arcname=os.path.relpath(p, os.path.dirname(root)))
    return dst

def main(argv):
    apply = "--apply" in argv
    roots = [a for a in argv[1:] if a not in ("--apply", "--force")]
    roots = [r for r in roots if os.path.isdir(r)]
    if not roots:
        print(__doc__); return 2
    force = "--force" in argv
    files = all_json(roots)
    node_handle, label_to_handle, dup_labels, already_migrated = build_handles(files)
    # A tree that already carries "CID" handles was migrated before. Re-running is SAFE (handles are deterministic per
    # (path,index), so refs — now handles, not labels — pass through unchanged and each "CID" is re-set to the same
    # value), but it is almost never intended. Refuse unless --force, so a stray re-run can't surprise anyone.
    if already_migrated and not force:
        print(f"!! REFUSING: {already_migrated} node(s) already have a \"CID\" handle — this tree looks already migrated.")
        print("   If you really mean to re-run (idempotent with the same file set), pass --force. To start clean,")
        print("   restore the .label-to-cid-handle-backup-*.tar first.")
        return 3
    if dup_labels:
        refd = referenced_labels(files)
        print("== NOTE: duplicate LABELs (cosmetic; each node still gets its OWN unique handle):")
        for lbl, c in sorted(dup_labels.items()):
            tag = "  ⚠ REFERENCED — a ref to this label resolves to the FIRST-seen node (sorted-path order)" if lbl in refd else ""
            print(f"   '{lbl}' x{c}{tag}")
    if apply:
        for root in roots:
            print(f"== JSON backup {root} → {json_backup(root)}")
    n = sum(rewrite_file(p, node_handle, label_to_handle, apply) for p in files)
    verb = "rewrote" if apply else "would rewrite"
    print(f"== {verb} {n} file(s); {len(node_handle)} node(s) given a placeholder CID handle, references rewritten")
    print("== NEXT: open the app and Verify & Publish (mints real CIDs, replaces placeholders), then re-share." if apply
          else "== dry run — re-run with --apply")
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
