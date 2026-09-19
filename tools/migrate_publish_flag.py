#!/usr/bin/env python3
"""migrate_publish_flag.py — stamp PUBLISH:true on each package's closure roots.

The share list is now driven by an explicit PUBLISH flag on a node (not the old "a DeclareExec without
GUEST" rule). This one-shot migration seeds that flag from the current implicit rule: a package's CLOSURE
ROOTS — the nodes that no other node in the SAME package references via PARENTS or LIBRARYITEM — that are
DeclareExec nodes become PUBLISH:true. That is GAMES + RUNNERS only (a game's launchable exec, a runner's
GUEST-bearing exec). Library packages (no-exec Content heads) are deliberately NOT stamped: a library reaches
a friend automatically inside a shared game's closure (its cross-package deps are fetched by CID on install),
so it needs no share-list entry. Hand-set PUBLISH on a library node later if you want it standalone-browsable.

Operates on the pretty on-disk working tree (handles, pre-mint). DRY-RUN by default; --apply writes the
flag (after tarring a pristine backup of LIBRARY first). Deterministic: sorted keys on write.

Usage:
    tools/migrate_publish_flag.py [LIBRARY_ROOT]           # dry run (default ~/.VidyaGod/LIBRARY)
    tools/migrate_publish_flag.py [LIBRARY_ROOT] --apply   # back up, then stamp
"""
import json, sys, tarfile, time
from pathlib import Path


def load_file(path):
    with open(path) as f:
        d = json.load(f)
    if isinstance(d, list):
        return d, True
    if isinstance(d, dict):
        return [d], False
    return None, False


def refs_of(node):
    out = set()
    p = node.get("PARENTS")
    if isinstance(p, list):
        out.update(x for x in p if isinstance(x, str))
    li = node.get("LIBRARYITEM")
    if isinstance(li, str) and li:
        out.add(li)
    return out


def package_dirs(root):
    root = Path(root)
    for collection in sorted(p for p in root.iterdir() if p.is_dir() and not p.name.startswith(".")):
        for pkg in sorted(p for p in collection.iterdir() if p.is_dir()):
            if any(pkg.glob("*.json")):
                yield pkg


def process_package(pkg, apply):
    files = {}            # path -> (nodes, was_array)
    all_ids, referenced = set(), set()
    for jf in sorted(pkg.glob("*.json")):
        try:
            nodes, was_array = load_file(jf)
        except Exception as e:
            print(f"  ! skip {jf.name}: {e}")
            continue
        if nodes is None:
            continue
        files[jf] = (nodes, was_array)
        for n in nodes:
            if isinstance(n, dict) and isinstance(n.get("NODE_ID"), str):
                all_ids.add(n["NODE_ID"])
                referenced |= refs_of(n)
    roots = all_ids - referenced            # nodes no sibling references = the package's closure roots
    stamped = 0
    for jf, (nodes, was_array) in files.items():
        changed = False
        for n in nodes:
            # Games + runners only: stamp a closure root ONLY if it is a DeclareExec (a launchable / runner). Skip
            # library content heads — they ride a game's closure by CID, so they need no share-list entry.
            if (isinstance(n, dict) and n.get("NODE_ID") in roots
                    and n.get("TYPE") == "DeclareExec" and n.get("PUBLISH") is not True):
                n["PUBLISH"] = True
                changed = True
                stamped += 1
                print(f"  + PUBLISH  {pkg.name}/{jf.name}  ::  {n['NODE_ID']}")
        if changed and apply:
            with open(jf, "w") as f:
                # Preserve the original key order (byte-fidelity) and only APPEND PUBLISH. sort_keys reordered content
                # (e.g. RegEdit rows), breaking the round-trip byte-identity guarantee for no benefit — the CID is
                # computed from CANONICAL dag-json (sorted) at mint time regardless of on-disk pretty order.
                json.dump(nodes if was_array else nodes[0], f, indent=2)
                f.write("\n")
    return stamped


def main():
    argv = sys.argv[1:]
    apply = "--apply" in argv
    positional = [a for a in argv if not a.startswith("--")]
    root = Path(positional[0]) if positional else Path.home() / ".VidyaGod" / "LIBRARY"
    if not root.is_dir():
        print(f"no LIBRARY directory at {root}")
        return 2
    if apply:
        # Back up node JSON ONLY (not the multi-GB hydrated content — the migration edits only *.json). A whole-tree
        # tar was 59 GB of redundant content; the JSON is a few MB and is all that can change.
        backup = root.parent / f"LIBRARY-json.pre-publish-{time.strftime('%Y%m%d-%H%M%S')}.tar"
        print(f"backing up node JSON only  {root}  ->  {backup}")
        with tarfile.open(backup, "w") as t:
            for jf in sorted(root.rglob("*.json")):
                t.add(jf, arcname=str(jf.relative_to(root.parent)))
    total, pkgs = 0, 0
    for pkg in package_dirs(root):
        pkgs += 1
        total += process_package(pkg, apply)
    print(f"\n{'APPLIED' if apply else 'DRY RUN'}: {total} node(s) stamped PUBLISH across {pkgs} package(s)")
    if not apply:
        print("re-run with --apply to write the flag (a pristine tar backup of LIBRARY is made first)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
