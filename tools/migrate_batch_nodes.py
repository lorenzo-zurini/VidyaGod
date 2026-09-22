#!/usr/bin/env python3
"""Migrate a package (or a whole library) from one-item-per-node to BATCHED nodes + one-file-per-node.

Collapses same-TYPE batchable nodes that currently share a file into ONE batched node:
  CustomVar      -> {TYPE:CustomVar,   VARS:[{KEY,DEFAULT,COMMENT,UI,WHEN}, ...]}
  DeclarePersist -> {TYPE:DeclarePersist, PERSISTS:[{SCOPE,PATH,TARGET,CLOUD,WHEN}, ...]}
  Content        -> {TYPE:VFSLayer,    LAYERS:[{FORM,PATH,TARGET,SOURCE,WHEN,TOGGLE,SUBMOUNTS,BASE_TARGETS,COMMENT}, ...]}
  DllOverride    -> {TYPE:DllOverride, OVERRIDES:{...merged...}}   (schema already multi; just merges)
Then writes exactly ONE node per .json file (filename from LABEL, de-duped).

Only same-TYPE members that (a) are in ONE connected component via intra-group PARENT edges (a var chain
collapses; two independent same-type nodes do NOT union — that would cross-wire their closures) and
(b) share an identical gating signature (node-level TOGGLE/EXCLUDE, plus WHEN for DllOverride which has no
per-item WHEN slot) collapse together. Node-level WHEN on the other TYPEs is pushed DOWN into each item's
per-item WHEN, so members differing only in WHEN still merge losslessly.

Wiring: the batched node's handle is the CID of the FIRST member that HAS one (never "" — a handle-less
group would otherwise remap refs to nothing); every other member's handle is remapped to it. Refs
(PARENTS/LIBRARYITEM) are rewritten with a GLOBAL refmap built across ALL packages first — batchable nodes
(runner/library vars, shared content) are referenced cross-package, so a per-package map would dangle those.
Edges internal to a collapsed group vanish (they are now one node). Launch follows PARENTS handles + content
SOURCE.CIDs (byte-hashes, unchanged), so the migrated tree launches as-is; Verify&Publish re-mints the (now
stale) node CIDs. Content SOURCE.CID bytes are NEVER touched. Files carrying any non-node JSON value are
left untouched; node files are rewritten atomically (write .new, delete old, commit).

Idempotency: a package already carrying VFSLayer/VARS/PERSISTS is skipped (refuse-guard).

Run it ONCE over the COMPLETE library root: the global refmap that remaps cross-package references is built in a
single pass across every package, so migrating packages in separate incremental runs would leave a later package's
reference to an already-collapsed member of an earlier package unremapped. (A re-run over a fully-migrated tree is a
safe no-op — every package is refused as already batched.)

Usage: migrate_batch_nodes.py <dir> [--dry-run]
  <dir> = a package bundle (dir with node .json files) OR a library root (recurses to package bundles).
  migrate_batch_nodes.py --self-test   # adversarial invariants (no data touched)
"""
import json, os, sys, re

BATCH = {"CustomVar": "VARS", "DeclarePersist": "PERSISTS", "Content": "LAYERS", "DllOverride": "OVERRIDES"}
ITEM_FIELDS = {
    "CustomVar":      ["KEY", "DEFAULT", "COMMENT", "UI", "WHEN"],
    "DeclarePersist": ["SCOPE", "PATH", "TARGET", "CLOUD", "WHEN"],
    # TOGGLE is deliberately absent: it is NODE-level gating (kept in gating_sig, so merged layers share it and it
    # rides on the batched node), and NodeLower does not read a per-LAYERS-entry TOGGLE — copying it here would only
    # emit inert data into every migrated file.
    "Content":        ["FORM", "PATH", "TARGET", "SOURCE", "WHEN", "SUBMOUNTS", "BASE_TARGETS", "COMMENT"],
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


# Gating signature: node-level attributes that CANNOT be pushed into per-item entries and so
# must be identical for members to merge. For CustomVar/DeclarePersist/VFSLayer, node-level WHEN is
# pushed DOWN into each item's WHEN (the schema carries per-item WHEN), so it is NOT in the signature;
# for DllOverride there is no per-item WHEN slot, so WHEN stays node-level and joins the signature.
def gating_sig(n):
    typ = n.get("TYPE")
    keys = ["TOGGLE", "EXCLUDE"] + (["WHEN"] if typ == "DllOverride" else [])
    return tuple(json.dumps(n.get(k), sort_keys=True) for k in keys)


# Connected components of a member LIST via INTRA-list PARENT edges (undirected), used to decide what merges.
def _components_by_edges(group):
    cids = {m.get("CID") for m in group if m.get("CID")}
    idx_by_cid = {m.get("CID"): i for i, m in enumerate(group) if m.get("CID")}
    parent = list(range(len(group)))
    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]; i = parent[i]
        return i
    for i, m in enumerate(group):
        for p in m.get("PARENTS", []) or []:
            if p in cids and p in idx_by_cid:
                parent[find(i)] = find(idx_by_cid[p])
    comps = {}
    for i, m in enumerate(group):
        comps.setdefault(find(i), []).append(m)
    return list(comps.values())


# Split same-TYPE members into the sets that may collapse together. Partition by gating signature FIRST, then take
# connected components WITHIN each signature group. A chain of vars (var_n -> ... -> var_1, all one signature) is
# ONE component and collapses; two independent same-type nodes with no edge between them are SEPARATE components and
# are NEVER unioned (that would cross-wire their closures / mount both layers everywhere). Doing it the other way —
# components over ALL members, then split by signature — would let two same-signature members that are connected
# only TRANSITIVELY through a differently-gated member merge, unioning their external referrers; grouping by
# signature first makes that edge invisible, so it cannot.
def components(members):
    bysig = {}
    for m in members:
        bysig.setdefault(gating_sig(m), []).append(m)
    out = []
    for group in bysig.values():
        out.extend(_components_by_edges(group))
    return out


def batch_group(typ, members):
    handle = next((m.get("CID") for m in members if m.get("CID")), "")   # never map refs to "" (finding #5)
    batched = {"TYPE": "VFSLayer" if typ == "Content" else typ}
    if handle:
        batched["CID"] = handle
    batched["_raw_parents"] = [p for m in members for p in m.get("PARENTS", []) or []]
    batched["LABEL"] = members[0].get("LABEL") or typ.lower()
    for k in ("TOGGLE", "EXCLUDE"):                        # identical across the group (gating_sig), safe to carry
        if k in members[0]:
            batched[k] = members[0][k]
    if typ == "DllOverride":
        batched["WHEN"] = members[0].get("WHEN")           # identical across the group (in gating_sig)
        if batched["WHEN"] is None:
            del batched["WHEN"]
        merged = {}
        for m in members:
            merged.update(m.get("OVERRIDES", {}))
        batched["OVERRIDES"] = merged
    else:
        batched[BATCH[typ]] = [{k: m[k] for k in ITEM_FIELDS[typ] if k in m} for m in members]
    refs = {m["CID"]: handle for m in members if m.get("CID")}
    return batched, refs


# PASS 1: collapse a package's batchable groups PER FILE, PER TYPE, PER CONNECTED-COMPONENT+GATING.
# Returns (out_nodes, refmap, node_files, preserve_files). out_nodes carry "_raw_parents" resolved in pass 2.
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
    node_files = []       # files that held ONLY nodes -> safe to delete on rewrite
    preserve_files = []   # files with any non-node value -> left untouched (finding #9)
    for f, ns in per_file.items():
        node_ct = sum(1 for n in ns if is_node(n))
        if node_ct == 0:
            preserve_files.append(f)
            continue       # a PURE non-node file — leave it untouched
        if node_ct != len(ns):
            # A MIXED file (nodes + non-node values) cannot be handled safely: its nodes need ref-rewriting and
            # re-filing, its non-node values need preserving verbatim. Preserving it whole would leave the nodes'
            # PARENTS/LIBRARYITEM dangling once a referenced member collapses. Refuse in PASS 1, before any write.
            raise RuntimeError(f"{f}: a file mixes node and non-node entries — split them into separate files and "
                               f"re-run (its nodes need ref-rewriting, its non-node values need preserving)")
        node_files.append(f)
        by_type = {}
        for n in ns:
            if n.get("TYPE") in BATCH:
                by_type.setdefault(n["TYPE"], []).append(n)
        collapsed = set()
        for typ, members in by_type.items():
            for group in components(members):
                batched, refs = batch_group(typ, group)
                refmap.update(refs)
                for m in group:
                    collapsed.add(id(m))
                out_nodes.append(batched)
        for n in ns:
            if is_node(n) and id(n) not in collapsed:
                out_nodes.append(n)
    return (out_nodes, refmap, node_files, preserve_files)


# PASS 2: rewrite refs with the GLOBAL refmap, then write one node per file (atomically).
def rewrite_and_write(pkg, out_nodes, refmap, node_files, preserve_files, dry):
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
    # write new node files to .new siblings FIRST (never lose data if interrupted mid-write),
    # then delete the old node files, then commit the .new files. Non-node files are preserved.
    # sanitize() de-dupes on the EXTENSION-LESS stem (it appends ".json" itself), so seed the taken-names set with
    # the preserved files' stems — seeding "meta.json" would let a node LABELled "meta" sail past the collision check
    # and os.replace() would then clobber the preserved meta.json.
    used = {os.path.splitext(os.path.basename(f))[0] for f in preserve_files}
    staged = []
    for n in out_nodes:
        n.pop("POS", None)                                   # canvas recomputes layout at publish
        name = sanitize(n.get("LABEL") or n.get("TYPE", "node"), used) + ".json"
        dst = os.path.join(pkg, name)
        with open(dst + ".new", "w") as fh:
            json.dump(n, fh, indent=1)
        staged.append(dst)
    for f in node_files:
        os.remove(f)
    for dst in staged:
        os.replace(dst + ".new", dst)


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


def self_test():
    """Adversarial invariants (run: migrate_batch_nodes.py --self-test). Each asserts a finding is fixed."""
    import tempfile, shutil
    fails = []
    def check(name, cond):
        print(("  ok  " if cond else "  FAIL ") + name)
        if not cond: fails.append(name)

    # #5: a group whose FIRST member is handle-less must NOT remap refs to "" — the handle is a
    # member that HAS a CID, and every ref to a group member survives.
    m0 = {"TYPE": "CustomVar", "KEY": "a", "DEFAULT": "1"}                       # no CID (draft-ish)
    m1 = {"TYPE": "CustomVar", "CID": "Qm_HAS", "KEY": "b", "DEFAULT": "2", "PARENTS": []}
    m0["PARENTS"] = ["Qm_HAS"]                                                   # chains m0->m1 (one component)
    batched, refs = batch_group("CustomVar", [m0, m1])
    check("#5 handle is a member-with-CID, never ''", batched.get("CID") == "Qm_HAS")
    check("#5 no ref remapped to ''", "" not in refs.values() and refs.get("Qm_HAS") == "Qm_HAS")

    # #6a: two INDEPENDENT same-type nodes (no intra-edge) referenced by different parents must NOT be
    # unioned into one node (that cross-wires closures). components() keeps them separate.
    a = {"TYPE": "Content", "CID": "QmA", "PARENTS": ["QmLaunchA"], "SOURCE": {"CID": "cA"}}
    b = {"TYPE": "Content", "CID": "QmB", "PARENTS": ["QmLaunchB"], "SOURCE": {"CID": "cB"}}
    comps = components([a, b])
    check("#6a independent nodes stay in separate components", len(comps) == 2)

    # a genuine chain (a<-b) IS one component and collapses to a single node.
    a2 = {"TYPE": "CustomVar", "CID": "QmA", "KEY": "x", "PARENTS": []}
    b2 = {"TYPE": "CustomVar", "CID": "QmB", "KEY": "y", "PARENTS": ["QmA"]}
    check("chain collapses to one component", len(components([a2, b2])) == 1)

    # #6b: differing node-level TOGGLE/EXCLUDE must NOT be silently hoisted from the first member — such
    # members land in separate gating groups.
    t1 = {"TYPE": "CustomVar", "CID": "QmA", "KEY": "x", "TOGGLE": {"KEY": "opt"}, "PARENTS": []}
    t2 = {"TYPE": "CustomVar", "CID": "QmB", "KEY": "y", "PARENTS": ["QmA"]}     # chained but no TOGGLE
    check("#6b differing TOGGLE splits the group", len(components([t1, t2])) == 2)

    # per-item WHEN: node-level WHEN differences are pushed into each VARS entry (NOT a split, NOT lost).
    w1 = {"TYPE": "CustomVar", "CID": "QmA", "KEY": "x", "WHEN": "%f%==60", "PARENTS": []}
    w2 = {"TYPE": "CustomVar", "CID": "QmB", "KEY": "y", "WHEN": "%f%==120", "PARENTS": ["QmA"]}
    bw, _ = batch_group("CustomVar", [w1, w2])
    whens = {v.get("WHEN") for v in bw["VARS"]}
    check("per-item WHEN preserved on each entry", whens == {"%f%==60", "%f%==120"})
    check("node-level WHEN not hoisted for CustomVar", "WHEN" not in bw)

    # interleaved gating: A(sig X) — B(sig Y) — C(sig X), a chain where A and C share a signature but are connected
    # only THROUGH the differently-gated B. They must NOT merge (that would union their external referrers). Grouping
    # by signature before taking components makes B's edge invisible to A/C, so all three stay separate.
    ia = {"TYPE": "CustomVar", "CID": "QmIA", "KEY": "a", "PARENTS": []}
    ib = {"TYPE": "CustomVar", "CID": "QmIB", "KEY": "b", "TOGGLE": {"KEY": "opt"}, "PARENTS": ["QmIA"]}
    ic = {"TYPE": "CustomVar", "CID": "QmIC", "KEY": "c", "PARENTS": ["QmIB"]}
    check("interleaved gating: same-sig members bridged by a differently-gated one do not merge",
          len(components([ia, ib, ic])) == 3)

    # #6c: DllOverride has no per-item WHEN slot, so differing node-WHEN must keep them separate.
    d1 = {"TYPE": "DllOverride", "CID": "QmA", "OVERRIDES": {"a": "n"}, "WHEN": "%m%==x", "PARENTS": []}
    d2 = {"TYPE": "DllOverride", "CID": "QmB", "OVERRIDES": {"b": "n"}, "WHEN": "%m%==y", "PARENTS": ["QmA"]}
    check("#6c DllOverride differing WHEN not merged", len(components([d1, d2])) == 2)
    ds = components([d1, {"TYPE": "DllOverride", "CID": "QmC", "OVERRIDES": {"b": "n"}, "WHEN": "%m%==x", "PARENTS": ["QmA"]}])
    check("#6c DllOverride same WHEN merges + carries WHEN",
          len(ds) == 1 and batch_group("DllOverride", ds[0])[0].get("WHEN") == "%m%==x")

    # #9: a package dir with a non-node .json keeps it; node files are rewritten. The node here is LABELled "meta"
    # so its natural filename ("meta.json") collides with the preserved non-node file — the migration must NOT
    # clobber the preserved bytes (the finding-#3 regression).
    d = tempfile.mkdtemp()
    try:
        json.dump({"TYPE": "CustomVar", "CID": "QmX", "LABEL": "meta", "KEY": "k", "DEFAULT": "1"}, open(os.path.join(d, "n.json"), "w"))
        json.dump({"not": "a node", "meta": True}, open(os.path.join(d, "meta.json"), "w"))
        out, refmap, nf, pf = collapse_package(d)
        rewrite_and_write(d, out, refmap, nf, pf, dry=False)
        left = set(os.listdir(d))
        check("#9 non-node file preserved intact (not clobbered by a same-named node)",
              "meta.json" in left and json.load(open(os.path.join(d, "meta.json"))) == {"not": "a node", "meta": True})
        check("#9 the node was still written (under a de-duped name)",
              any(f != "meta.json" and f.endswith(".json") and json.load(open(os.path.join(d, f))).get("TYPE") == "CustomVar" for f in left))
        check("#9 no stray .new files", not any(x.endswith(".new") for x in left))
    finally:
        shutil.rmtree(d)

    print(("\nSELF-TEST FAILED: " + ", ".join(fails)) if fails else "\nself-test: all invariants hold")
    sys.exit(1 if fails else 0)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    dry = "--dry-run" in sys.argv
    if "--self-test" in sys.argv:
        self_test()
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
        out_nodes, refmap, node_files, preserve_files = r
        global_refmap.update(refmap)
        plans[p] = (out_nodes, node_files, preserve_files)
    # PASS 2: rewrite refs with the global map + write.
    for p, (out_nodes, node_files, preserve_files) in plans.items():
        rewrite_and_write(p, out_nodes, global_refmap, node_files, preserve_files, dry)
        print(f"  [{'dry' if dry else 'ok'}] {os.path.basename(p)}: {len(out_nodes)} node(s)")
    print(f"\n{len(plans)} package(s) migrated, {n_skip} skipped; global refmap {len(global_refmap)} handle(s) remapped.")


if __name__ == "__main__":
    main()
