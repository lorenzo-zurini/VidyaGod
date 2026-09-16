#!/usr/bin/env python3
# migrate_persist_nodes.py — ONE-TIME migration of the flat-schema PERSIST primitive:
#
#   OLD:  { "TYPE":"Persist", "KEEP":[<target>...], "DROP":[<path>...] }
#   NEW:  { "TYPE":"DeclarePersist", "SCOPE":"file"|"registry", "PATH":<src>, "TARGET":<durable subdir>, "CLOUD":true }
#
# One old Persist node carried a LIST of KEEPs (the lowering expanded them into layers); the new primitive is
# one-node = one-persist. So a Persist node with N KEEPs becomes a CHAIN of N DeclarePersist nodes inserted where
# the Persist node sat: the first REUSES the original NODE_ID + inbound edges (so referrers still resolve), and each
# subsequent one is spliced in as its ancestor (PARENTS), ending at the original PARENTS. Persistence is purely
# additive, so the chain order is immaterial. DROP is GONE (pristine-by-default makes it redundant) — a DROP entry is
# reported and dropped. TARGET defaults to the PATH's last path segment (sanitized to one safe dir name).
#
# Registry KEEPs are classified by shape (matches DerivePersistence's OLD ClassifyKeep, so the result is identical):
#   "registry" (sentinel) or a bare hive root (HKCU/HKLM/HKCR/HKCC/HKU/HKEY_*) → SCOPE=registry, PATH "" (all hives)
#                                                                                 or the hive as a key
#   a deeper HK... path → SCOPE=registry, PATH=<key>          | a runtime-root-relative path → SCOPE=file
#
# DRY-RUN by default (prints the plan). Pass --apply to rewrite in place (each file atomically: temp + os.replace).
# NEVER touches a "Vidya Backup" tree (the golden backup). Idempotent: a package with no Persist node is left alone.
#
# Usage:  migrate_persist_nodes.py <library-root> [--apply]
#         library-root = the LIBRARY dir (e.g. ~/.VidyaGod/LIBRARY), scanned recursively for *.json packages.

import json, os, sys, tempfile

HIVE_ROOTS = {"HKCU", "HKLM", "HKCR", "HKCC", "HKU",
              "HKEY_CURRENT_USER", "HKEY_LOCAL_MACHINE", "HKEY_CLASSES_ROOT",
              "HKEY_CURRENT_CONFIG", "HKEY_USERS"}


def leaf(path):
    p = path.rstrip("/\\")
    s = max(p.rfind("/"), p.rfind("\\"))
    return p if s < 0 else p[s + 1:]


def san_seg(s):
    o = "".join(c if (c.isalnum() or c in "._- ") else "_" for c in s)
    o = o.rstrip(". ")   # Win32 strips trailing dots/spaces on disk — match SanSeg so a TARGET can't dodge the guards
    return "_" if o in ("", ".", "..") else o


def is_dir_target(t):
    if t and t[-1] in "/\\":
        return True
    return "." not in leaf(t)   # no extension ⇒ directory (matches the resolver's shape rule)


def classify_keep(keep):
    """Return a DeclarePersist field dict (without NODE_ID/PARENTS/TYPE) for one old KEEP target."""
    t = keep.strip()
    low = t.lower()
    # whole-RUNTIME sentinel (old KEEP %RuntimePath% / "." / "/") → the new whole-runtime persist: PATH "" needs an
    # explicit TARGET (there's no leaf to default from), so map to a named catch-all and let the note flag it.
    if low in ("%runtimepath%", ".", "./", "/"):
        return {"SCOPE": "file", "PATH": "", "TARGET": "AllData", "_whole_runtime": True}
    # whole-registry sentinel → all hives (PATH "")
    if low == "registry":
        return {"SCOPE": "registry", "PATH": ""}
    sep = min([i for i in (t.find("\\"), t.find("/")) if i >= 0], default=-1)
    root = (t if sep < 0 else t[:sep]).upper()
    if root in HIVE_ROOTS:
        # a bare hive root persisted the whole hive; a deeper key persisted that subtree. The new model expresses
        # both as SCOPE=registry with the key as PATH (a bare hive is just the broadest key).
        return {"SCOPE": "registry", "PATH": t}
    # file path
    target = san_seg(leaf(t))
    return {"SCOPE": "file", "PATH": t, "TARGET": target}


def migrate_nodes(nodes, pkg_tag):
    """Rewrite Persist nodes in `nodes` (a list) in place. Returns (changed, notes[])."""
    notes = []
    changed = False
    out = []
    used_ids = {n.get("NODE_ID") for n in nodes if isinstance(n, dict)}

    def fresh_id(base):
        i = 2
        while f"{base}_{i}" in used_ids:
            i += 1
        nid = f"{base}_{i}"
        used_ids.add(nid)
        return nid

    # File/dir persists all land at <instance>/<TARGET>, so the TARGET namespace must be unique (and disjoint from the
    # instance's reserved state) or two saves clobber at capture — the exact runtime collision DerivePersistence now
    # refuses. Disambiguate here (append _2, _3, …) so the migrated data never trips that refusal and loses a save.
    used_targets = {"instance.json", "registry", "regkeys"}   # reserved (case-insensitive) — the resolver refuses these

    def claim_target(base):
        cand = base
        i = 2
        while cand.lower() in used_targets:
            cand = f"{base}_{i}"
            i += 1
        used_targets.add(cand.lower())
        return cand

    for n in nodes:
        if not isinstance(n, dict) or n.get("TYPE") != "Persist":
            out.append(n)
            continue
        changed = True
        nid = n.get("NODE_ID", "persist")
        parents = n.get("PARENTS", [])
        when = n.get("WHEN")
        pos = n.get("POS")   # canvas position — carry it onto the first migrated node so the editor layout is stable
        keeps = n.get("KEEP") or []
        drops = n.get("DROP") or []
        if isinstance(keeps, str):
            keeps = [keeps]
        if isinstance(drops, str):
            drops = [drops]
        for d in drops:
            notes.append(f"{pkg_tag}: DROP '{d}' on {nid} is GONE (pristine-by-default) — not migrated")
        keeps = [k for k in keeps if isinstance(k, str) and k.strip()]
        if not keeps:
            # A Persist node that kept nothing (only DROPs, or empty) has no persist equivalent — but it may be an
            # ANCESTOR in the DAG (something PARENTs it, and it PARENTs others). Deleting it would sever that path and
            # orphan its ancestors from the closure. Convert it to a pass-through Group (same NODE_ID + PARENTS) so the
            # topology is preserved and it simply contributes no layer.
            notes.append(f"{pkg_tag}: {nid} kept nothing → converted to a pass-through Group (topology preserved)")
            grp = {"NODE_ID": nid, "TYPE": "Group", "PARENTS": parents}
            if when is not None:
                grp["WHEN"] = when
            if pos is not None:
                grp["POS"] = pos
            out.append(grp)
            continue
        # Build a chain: dp0(orig id, inbound edges) → dp1 → ... → dp(last) → original PARENTS.
        specs = [classify_keep(k) for k in keeps]
        chain = []
        for i, spec in enumerate(specs):
            spec = dict(spec)
            if spec.pop("_whole_runtime", False):
                notes.append(f"{pkg_tag}: {nid} KEEP of the whole runtime → PATH \"\" TARGET '{spec['TARGET']}' "
                             f"(authoring aid — narrow to specific dirs before shipping)")
            node = {"NODE_ID": nid if i == 0 else fresh_id(nid), "TYPE": "DeclarePersist"}
            if "TARGET" in spec:   # file scope only — dedup against reserved names + earlier targets in this file
                claimed = claim_target(spec["TARGET"])
                if claimed != spec["TARGET"]:
                    notes.append(f"{pkg_tag}: TARGET '{spec['TARGET']}' collided → renamed '{claimed}' (avoids clobber)")
                spec = dict(spec, TARGET=claimed)
            node.update(spec)
            node["CLOUD"] = True
            if when is not None:
                node["WHEN"] = when
            if i == 0 and pos is not None:
                node["POS"] = pos
            chain.append(node)
        # wire the chain: each node PARENTS the next; the last carries the original PARENTS.
        for i, node in enumerate(chain):
            node["PARENTS"] = [chain[i + 1]["NODE_ID"]] if i + 1 < len(chain) else parents
        for node in chain:
            desc = node.get("PATH") or "(all)"
            notes.append(f"{pkg_tag}: {node['NODE_ID']} → DeclarePersist SCOPE={node['SCOPE']} PATH='{desc}'"
                         + (f" TARGET={node['TARGET']}" if 'TARGET' in node else ""))
        out.extend(chain)
    return changed, out, notes


def process_file(path, apply):
    try:
        with open(path) as f:
            doc = json.load(f)
    except Exception as e:
        print(f"  !! {path}: unreadable ({e}) — skipped")
        return 0
    if not isinstance(doc, list):
        return 0   # a package file is a flat list of nodes
    if not any(isinstance(n, dict) and n.get("TYPE") == "Persist" for n in doc):
        return 0
    tag = os.path.basename(path)
    changed, new_nodes, notes = migrate_nodes(doc, tag)
    for ln in notes:
        print("   " + ln)
    if not changed:
        return 0
    if apply:
        d = os.path.dirname(path) or "."
        fd, tmp = tempfile.mkstemp(dir=d, suffix=".tmp")
        with os.fdopen(fd, "w") as f:
            json.dump(new_nodes, f, indent=2)
        os.replace(tmp, path)
        print(f"  REWROTE {path}")
    else:
        print(f"  would rewrite {path}")
    return 1


def main():
    args = [a for a in sys.argv[1:] if a != "--apply"]
    apply = "--apply" in sys.argv[1:]
    if len(args) != 1:
        print("usage: migrate_persist_nodes.py <library-root> [--apply]")
        return 2
    root = args[0]
    if not os.path.isdir(root):
        print(f"!! not a directory: {root}")
        return 1
    print(f"== library root: {root}")
    print("== MODE: " + ("APPLY" if apply else "DRY-RUN (pass --apply to execute)"))
    count = 0
    for dirpath, _dirs, files in os.walk(root):
        if "Vidya Backup" in dirpath:   # golden backup — never touched
            continue
        for fn in files:
            if fn.endswith(".json"):
                count += process_file(os.path.join(dirpath, fn), apply)
    print(f"== {count} package file(s) {'rewritten' if apply else 'to rewrite'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
