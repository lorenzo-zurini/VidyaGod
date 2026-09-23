#!/usr/bin/env python3
"""Sink each CHILD face's tile from its variants down to the content that begins the child (generation 5 shape).

`migrate_final_chain.py` sinks a card's MAIN face to the pristine and leaves every child face where generation 4
had it: on the variants. Nesting is containment — a face nests under the nearest same-UID face BENEATH it — so a
child tile on a variant is a sibling of every other child (The Conquerors and Forgotten Empires side by side under
Age of Kings) instead of a chain (AoK > TC > FE). This moves each child tile down to the biggest content node that
all of the face's variants are made of and no shallower face is, processing faces by closure size so a deeper
expansion is subtracted from its base's candidates and never inherits the wrong tile.

  python3 tools/sink_child_faces.py <LIBRARY dir> [--dry-run]

Backs up the library to <root>.pre-sink-child-faces-backup-<ts>.tar before writing. Idempotent: a child face whose
tile already sits on a non-variant node is left alone.
"""
import json, os, sys, tarfile, time, copy
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib.util
_spec = importlib.util.spec_from_file_location("mfc", os.path.join(os.path.dirname(os.path.abspath(__file__)), "migrate_final_chain.py"))
mfc = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(mfc)
gather, closure, bare, face_key, is_node = mfc.gather, mfc.closure, mfc.bare, mfc.face_key, mfc.is_node


def required(nodes, h):
    seen, st = set(), [h]
    while st:
        x = st.pop()
        if x in seen or x not in nodes or (x != h and "TOGGLE" in nodes[x]):
            continue
        seen.add(x); st += bare(nodes[x])
    return seen


def size(n):
    return sum((L.get("SOURCE") or {}).get("SIZE", 0) for L in n.get("LAYERS", []) if isinstance(L, dict))


def plan(nodes):
    by_uid = defaultdict(lambda: defaultdict(list))       # uid -> face key -> variants carrying that tile
    main_variants = defaultdict(list)                      # uid -> variants with NO tile of their own (the sunk main face's)
    sunk = defaultdict(set)                                # uid -> nodes carrying a tile that are not variants
    owners = defaultdict(set)                              # node -> UIDs whose variants reach it
    for h, n in nodes.items():
        if not n.get("VARIANT"): continue
        u = n["TILE"]["UID"] if "TILE" in n else None
        if u is None:
            # identity ascends: the nearest tile beneath (any tile in the required chain — one UID per card here)
            for x in closure(nodes, h):
                if "TILE" in nodes[x]: u = nodes[x]["TILE"]["UID"]; break
        if u is None: continue
        for x in closure(nodes, h): owners[x].add(u)
        if "TILE" in n: by_uid[u][face_key(n["TILE"])].append(h)
        else: main_variants[u].append(h)
    for h, n in nodes.items():
        if "TILE" in n and not n.get("VARIANT"): sunk[n["TILE"]["UID"]].add(h)
    moves = {}                                             # face key -> (home handle, [variants])
    for u, faces in by_uid.items():
        if not sunk[u]:
            continue                                       # no sunk main face: nothing to nest under, leave as is
        main_tile_closure = set().union(*[closure(nodes, h) for h in sunk[u]])
        main_closure = set().union(*[closure(nodes, v) for v in main_variants[u]]) if main_variants[u] else set(main_tile_closure)
        taken = set(main_closure)
        order = sorted(faces, key=lambda k: (min(len(required(nodes, v)) for v in faces[k]), k))
        for k in order:
            vs = faces[k]
            common = set.intersection(*[required(nodes, v) for v in vs]) - set(vs)
            cands = [x for x in common if x not in taken and nodes[x].get("LAYERS") and "TILE" not in nodes[x]
                     and owners[x] == {u}                              # this card's alone — never a shared library
                     and main_tile_closure & closure(nodes, x)]         # the main face must be beneath it
            if not cands:
                print(f"   UID {u}: face '{nodes[vs[0]]['TILE'].get('TITLE')}' has no private content above the base — tile stays on {len(vs)} variant(s)")
                continue
            # where the child BEGINS: the candidate with the smallest private chain (the first node above what is
            # already mounted), ties by content size
            home = min(cands, key=lambda x: (len(closure(nodes, x) - taken), -size(nodes[x]), nodes[x].get("LABEL", "")))
            moves[k] = (home, vs)
            taken |= set().union(*[closure(nodes, v) for v in vs]) | set(vs)
            print(f"   UID {u}: '{nodes[vs[0]]['TILE'].get('TITLE')}' → {nodes[home].get('LABEL')} ({len(vs)} variant(s))")
    return moves


def apply(nodes, moves):
    new = copy.deepcopy(nodes)
    for k, (home, vs) in moves.items():
        new[home]["TILE"] = copy.deepcopy(nodes[vs[0]]["TILE"])
        for v in vs: del new[v]["TILE"]
    return new


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args: sys.exit(__doc__)
    root = args[0]; dry = "--dry-run" in sys.argv
    raw, files = gather(root)                      # handle -> (node, path)
    nodes = {h: v[0] for h, v in raw.items()}
    moves = plan(nodes)
    if not moves:
        print("nothing to move"); return
    new = apply(nodes, moves)
    if dry:
        print(f"[dry-run] would rewrite {sum(1 for h in new if new[h] != nodes[h])} node file(s)"); return
    ts = time.strftime("%Y%m%d-%H%M%S")
    tar = os.path.normpath(root) + f".pre-sink-child-faces-backup-{ts}.tar"
    with tarfile.open(tar, "w") as T: T.add(root, arcname=os.path.basename(os.path.normpath(root)))
    for h in new:
        if new[h] == nodes[h]: continue
        with open(raw[h][1], "w") as F: json.dump(new[h], F, indent=2, ensure_ascii=False); F.write("\n")
    print(f"rewrote {sum(1 for h in new if new[h] != nodes[h])} node file(s); backup: {tar}")


if __name__ == "__main__":
    main()
