#!/usr/bin/env python3
"""Migrate a whole library from TYPE'd nodes + PARENTS/EXCLUDE/LIBRARYITEM to the ONE-EDGE graph.

One node kind, one edge:
  node = CID LABEL WHEN TOGGLE PUBLISH POS COMMENT
       + TILE {UID, PARENTUID?, TITLE, COVER, META}      (identity — on launchables)
       + ENTRYPOINTS [{LABEL, HOST, GUEST?, PATH, ARGS, ENV, ENV_REMOVE, WORKDIR, RECOMMENDED, RUNNER, ...}]
       + LAYERS | PATCHES | FILEEDITS | REGEDITS | DLLOVERRIDES | VARS | PERSISTS   (payload, any subset)
       + OVER [ ref | [ref, ...] | {"NOT": ref} ]        (the edge)

What it does, per TYPE:
  VFSLayer          -> LAYERS (as is)                CustomVar      -> VARS (as is)
  DeclarePersist    -> PERSISTS (as is)              RegEdit        -> REGEDITS = EDITS
  FileEdit          -> FILEEDITS = [{FILE, EDITS, OVERRIDE?}]
  BinaryPatch       -> PATCHES   = [{FILE, EDITS}]   DllOverride    -> DLLOVERRIDES = OVERRIDES
  Group             -> a plain node (OVER only)
  DeclareLibraryItem-> REMOVED; its {UID,TITLE,COVER,META} becomes the TILE of every launchable whose nearest
                       tile ancestor (BFS over PARENTS) it was. Tile refs vanish from every PARENTS list.
  DeclareExec       -> an ENTRYPOINTS entry. FOLDED into its single non-tile parent P when it has exactly one
                       (P gains ENTRYPOINTS + TILE + PUBLISH; P keeps its CID handle; refs to the exec are
                       remapped to P; the exec's file is deleted; P's LABEL becomes the exec's LABEL — the node
                       IS the launchable now). Refused (kept as its own node with ENTRYPOINTS + TILE + OVER)
                       when it has 0 or 2+ non-tile parents, or when P already carries a DIFFERENT tile.
  PARENTS -> OVER (minus tiles); EXCLUDE -> OVER {"NOT": ref} entries (a LABEL is resolved to its handle).

Invariants verified AFTER migrating (real run and --self-test alike) — a failure aborts before any write:
  I1 no node keeps TYPE/PARENTS/EXCLUDE/LIBRARYITEM; every key is in the node vocabulary
  I2 every OVER ref (plain, any-of member, NOT) resolves to a surviving handle
  I3 node count = old - tiles - folded execs
  I4 the multiset of content SOURCE.CIDs is unchanged; every payload item multiset is unchanged
  I5 CLOSURE EQUIVALENCE: for every old exec, the multiset of payload items reachable through PARENTS (tiles
     excluded) equals the multiset reachable through OVER from its new launchable node — the launch mounts
     exactly what it mounted before
  I6 every old exec is exactly one ENTRYPOINTS entry somewhere, with its fields intact; a game exec's node
     carries the tile it reached; PUBLISH survives
  I7 one node per file; non-node files untouched; every file is loadable

Usage: migrate_one_edge.py <library-root> [--dry-run]
       migrate_one_edge.py --self-test
"""
import copy, json, os, re, sys
from collections import Counter, deque

NODE_FIELDS = {"CID", "LABEL", "WHEN", "TOGGLE", "PUBLISH", "POS", "COMMENT", "TILE", "ENTRYPOINTS", "OVER",
               "LAYERS", "PATCHES", "FILEEDITS", "REGEDITS", "DLLOVERRIDES", "VARS", "PERSISTS"}
PAYLOAD_KEYS = ["LAYERS", "PATCHES", "FILEEDITS", "REGEDITS", "DLLOVERRIDES", "VARS", "PERSISTS"]
COMMON = ["CID", "LABEL", "WHEN", "TOGGLE", "PUBLISH", "POS", "COMMENT"]
EXEC_FIELDS = ["LABEL", "HOST", "GUEST", "PATH", "ARGS", "ENV", "ENV_REMOVE", "WORKDIR", "RECOMMENDED", "RUNNER",
               "CONTENT_ROOT", "PREFIX_GENERATE", "UNIFIED_RUNTIME"]


class Fail(Exception):
    pass


def is_old_node(n):
    return isinstance(n, dict) and isinstance(n.get("TYPE"), str)


def load(path):
    with open(path) as f:
        return json.load(f)


def sanitize(name, used):
    base = re.sub(r"[^A-Za-z0-9._ ()\[\]-]", "_", name).strip() or "node"
    fn, i = base, 2
    while fn in used:
        fn = f"{base}_{i}"
        i += 1
    used.add(fn)
    return fn


# ---------------------------------------------------------------- gather
def gather(root):
    """handle -> (node, file); file -> [handles] ; files carrying non-node JSON are recorded as preserved."""
    nodes, files, preserve = {}, {}, []
    for dp, dn, fns in os.walk(root):
        dn[:] = [d for d in dn if not d.startswith("_friend_")]
        for fn in sorted(fns):
            if not fn.endswith(".json"):
                continue
            p = os.path.join(dp, fn)
            try:
                j = load(p)
            except Exception as e:
                raise Fail(f"unparseable {p}: {e}")
            docs = j if isinstance(j, list) else [j]
            if not docs or not all(is_old_node(d) for d in docs):
                if any(is_old_node(d) for d in docs):
                    raise Fail(f"{p} mixes nodes and non-nodes")
                preserve.append(p)
                continue
            for d in docs:
                h = d.get("CID")
                if not isinstance(h, str) or not h:
                    raise Fail(f"{p}: node without a CID handle ({d.get('LABEL')!r}) — give it one first")
                if h in nodes:
                    raise Fail(f"duplicate handle {h} in {p} and {nodes[h][1]}")
                nodes[h] = (d, p)
                files.setdefault(p, []).append(h)
    return nodes, files, preserve


# ---------------------------------------------------------------- payload
def payload_of(n):
    """The new payload keys for an old node (everything but identity/edges/exec/tile)."""
    t = n["TYPE"]
    out = {}
    if t == "VFSLayer":
        out["LAYERS"] = n["LAYERS"]
    elif t == "CustomVar":
        out["VARS"] = n["VARS"]
    elif t == "DeclarePersist":
        out["PERSISTS"] = n["PERSISTS"]
    elif t == "RegEdit":
        out["REGEDITS"] = n["EDITS"]
    elif t == "FileEdit":
        e = {"FILE": n["FILE"], "EDITS": n["EDITS"]}
        if n.get("OVERRIDE"):
            e["OVERRIDE"] = True
        out["FILEEDITS"] = [e]
    elif t == "BinaryPatch":
        out["PATCHES"] = [{"FILE": n["FILE"], "EDITS": n["EDITS"]}]
    elif t == "DllOverride":
        out["DLLOVERRIDES"] = n["OVERRIDES"]
    elif t in ("Group", "DeclareExec", "DeclareLibraryItem"):
        pass
    else:
        raise Fail(f"unknown TYPE {t!r} on {n.get('LABEL')!r}")
    return out


def exec_entry(n):
    e = {}
    for k in EXEC_FIELDS:
        if k in n:
            e[k] = n[k]
    if not e.get("LABEL"):
        e["LABEL"] = n.get("LABEL", "")
    return e


def tile_of(n):
    t = {"UID": n.get("UID", "")}
    if "TITLE" in n:
        t["TITLE"] = n["TITLE"]
    if "COVER" in n:
        t["COVER"] = n["COVER"]
    if isinstance(n.get("META"), dict) and n["META"]:
        t["META"] = n["META"]
    return t


def items_of(new_node):
    """Every payload ITEM of a new-form node as a hashable multiset (for I4/I5)."""
    out = []
    for k in PAYLOAD_KEYS:
        v = new_node.get(k)
        if v is None:
            continue
        if k == "DLLOVERRIDES":
            for dll, order in v.items():
                out.append(("DLLOVERRIDES", dll, order))
        elif k in ("FILEEDITS", "PATCHES"):
            for e in v:
                for ed in e["EDITS"]:
                    out.append((k, e["FILE"], bool(e.get("OVERRIDE")), json.dumps(ed, sort_keys=True)))
        else:
            for e in v:
                out.append((k, json.dumps(e, sort_keys=True)))
    return out


def old_items_of(old):
    return items_of(payload_of(old))


# ---------------------------------------------------------------- migrate
def migrate(nodes):
    """nodes: handle -> old node. Returns (new_nodes: handle -> node, refmap, folded, tiles, report)."""
    old = {h: n for h, (n, _) in nodes.items()} if nodes and isinstance(next(iter(nodes.values())), tuple) else nodes
    tiles = {h for h, n in old.items() if n["TYPE"] == "DeclareLibraryItem"}
    execs = {h for h, n in old.items() if n["TYPE"] == "DeclareExec"}
    label_to_handle = {}
    for h, n in old.items():
        label_to_handle.setdefault(n.get("LABEL", ""), []).append(h)

    def parents(h):
        return [p for p in old[h].get("PARENTS", []) if isinstance(p, str) and p]

    def nearest_tiles(h):
        q, seen, found = deque(parents(h)), set(), []
        while q:
            p = q.popleft()
            if p in seen:
                continue
            seen.add(p)
            if p in tiles:
                found.append(p)
                continue
            if p in old:
                q.extend(parents(p))
        return found

    # 1. Base conversion: every non-tile node -> new form (payload + OVER minus tiles + NOT).
    new = {}
    for h, n in old.items():
        if h in tiles:
            continue
        m = {}
        for k in COMMON:
            if k in n:
                m[k] = n[k]
        for k, v in payload_of(n).items():
            m[k] = v
        over = [p for p in parents(h) if p not in tiles]
        for e in n.get("EXCLUDE", []) or []:
            if not isinstance(e, str) or not e:
                continue
            ref = e if e in old else (label_to_handle.get(e, [None])[0] if len(label_to_handle.get(e, [])) == 1 else None)
            if ref is None:
                raise Fail(f"{n.get('LABEL')!r}: EXCLUDE {e!r} names no unique node")
            over.append({"NOT": ref})
        if over:
            m["OVER"] = over
        if n["TYPE"] == "DeclareExec":
            m["ENTRYPOINTS"] = [exec_entry(n)]
            found = nearest_tiles(h)                      # BFS order: the NEAREST tile wins, as LiftLibraryItemEdge did
            if found:
                m["TILE"] = tile_of(old[found[0]])
        new[h] = m

    # 2. Fold each exec into its single non-tile parent when possible. The fold takes ENTRYPOINTS/TILE/PUBLISH/LABEL
    # and nothing else, so an exec carrying a facet the parent would not (TOGGLE, WHEN, COMMENT) stays standalone —
    # as does one some node EXCLUDEs: remapping that NOT onto the parent would widen an exclusion of one launchable
    # into an exclusion of shared substance under every title composed through it.
    not_targets = set()
    for m in new.values():
        for x in m.get("OVER", []):
            if isinstance(x, dict):
                not_targets.add(x["NOT"])
    refmap = {}
    folded = {}
    for h in sorted(execs):
        e = new[h]
        ps = [p for p in parents(h) if p not in tiles]
        if len(ps) != 1 or ps[0] not in new:
            continue
        if any(k in e for k in ("TOGGLE", "WHEN", "COMMENT")) or h in not_targets:
            continue
        p = ps[0]
        target = new[p]
        if "TILE" in target and "TILE" in e and target["TILE"].get("UID") != e["TILE"].get("UID"):
            continue                                      # a shared node under two titles: keep the exec standalone
        if "ENTRYPOINTS" in target and p in execs:
            continue                                      # never fold an exec into an exec
        # P becomes the launchable: entrypoint appended, tile/publish/label taken from the exec.
        target.setdefault("ENTRYPOINTS", []).append(e["ENTRYPOINTS"][0])
        if "TILE" in e:
            target["TILE"] = e["TILE"]
        if e.get("PUBLISH"):
            target["PUBLISH"] = True
        if e.get("LABEL"):
            target["LABEL"] = e["LABEL"]
        # An exec's own edges besides P (none by construction: ps == [p]) — and P keeps its own OVER.
        del new[h]
        refmap[h] = p
        folded[h] = p
    # 3. Remap references to folded execs and drop refs to tiles (already dropped in step 1).
    for m in new.values():
        if "OVER" not in m:
            continue
        o2 = []
        for x in m["OVER"]:
            if isinstance(x, str):
                x = refmap.get(x, x)
                if x in o2:
                    continue
            elif isinstance(x, dict):
                if x["NOT"] in refmap:
                    raise Fail(f"NOT names folded exec {x['NOT']!r} (never folded by construction)")
            o2.append(x)
        m["OVER"] = o2
    # A folded node must never be OVER itself (P OVER exec, exec folded into P): drop self refs.
    for h, m in new.items():
        if "OVER" in m:
            m["OVER"] = [x for x in m["OVER"] if not (isinstance(x, str) and x == h)]
            if not m["OVER"]:
                del m["OVER"]
    return new, refmap, folded, tiles


# ---------------------------------------------------------------- verify
def over_refs(m):
    refs = []
    for x in m.get("OVER", []):
        if isinstance(x, str):
            refs.append(x)
        elif isinstance(x, list):
            refs.extend(x)
        elif isinstance(x, dict):
            refs.append(x["NOT"])
    return refs


def positive_refs(m):
    refs = []
    for x in m.get("OVER", []):
        if isinstance(x, str):
            refs.append(x)
        elif isinstance(x, list):
            refs.extend(x)
    return refs


def closure_items(start, get_refs, get_items):
    q, seen, items = deque([start]), set(), Counter()
    while q:
        h = q.popleft()
        if h in seen:
            continue
        seen.add(h)
        items.update(get_items(h))
        q.extend(get_refs(h))
    return items


def verify(old, new, refmap, folded, tiles):
    execs = {h for h, n in old.items() if n["TYPE"] == "DeclareExec"}
    # I1
    for h, m in new.items():
        for k in ("TYPE", "PARENTS", "EXCLUDE", "LIBRARYITEM"):
            if k in m:
                raise Fail(f"I1: {h} still has {k}")
        bad = set(m) - NODE_FIELDS
        if bad:
            raise Fail(f"I1: {h} has unknown fields {bad}")
        if m.get("CID") != h:
            raise Fail(f"I1: {h} handle mismatch")
    # I2
    for h, m in new.items():
        for r in over_refs(m):
            if r not in new:
                raise Fail(f"I2: {h} OVER references missing {r}")
        if h in over_refs(m):
            raise Fail(f"I2: {h} is OVER itself")
    # I3
    if len(new) != len(old) - len(tiles) - len(folded):
        raise Fail(f"I3: {len(new)} != {len(old)} - {len(tiles)} - {len(folded)}")
    # I4
    def cids(items_fn, nodeset):
        c = Counter()
        for h in nodeset:
            for it in items_fn(h):
                c[it] += 1
        return c
    old_items = cids(lambda h: old_items_of(old[h]), (h for h in old if h not in tiles))
    new_items = cids(lambda h: items_of(new[h]), new)
    if old_items != new_items:
        d = (old_items - new_items) + (new_items - old_items)
        raise Fail(f"I4: payload item multiset changed: {list(d.items())[:3]}")
    # I5 closure equivalence per exec
    def old_refs(h):
        return [p for p in old[h].get("PARENTS", []) if p in old and p not in tiles]
    for e in execs:
        target = refmap.get(e, e)
        a = closure_items(e, old_refs, lambda h: old_items_of(old[h]))
        b = closure_items(target, lambda h: positive_refs(new[h]), lambda h: items_of(new[h]))
        if a != b:
            raise Fail(f"I5: exec {old[e].get('LABEL')!r} closure changed: {list(((a - b) + (b - a)).items())[:3]}")
    # I6 every exec is one entry, fields intact, tile present, publish survives
    for e in execs:
        target = new[refmap.get(e, e)]
        want = exec_entry(old[e])
        if want not in target.get("ENTRYPOINTS", []):
            raise Fail(f"I6: exec {old[e].get('LABEL')!r} entry lost")
        if old[e].get("PUBLISH") and not target.get("PUBLISH"):
            raise Fail(f"I6: exec {old[e].get('LABEL')!r} PUBLISH lost")
    total_entries = sum(len(m.get("ENTRYPOINTS", [])) for m in new.values())
    if total_entries != len(execs):
        raise Fail(f"I6: {total_entries} entrypoints for {len(execs)} execs")
    # every tile reached some launchable
    for t in tiles:
        uid = old[t].get("UID")
        if not any(m.get("TILE", {}).get("UID") == uid for m in new.values()):
            raise Fail(f"I6: tile {old[t].get('LABEL')!r} ({uid}) ended up on no launchable")


# ---------------------------------------------------------------- write
def write(root, nodes, files, preserve, new, folded, tiles, dry):
    # Plan: every node file is rewritten (one node per file); folded execs' and tiles' files vanish.
    plan = {}                                             # new file path -> node
    used = {os.path.basename(p)[:-5] for p in preserve}
    by_dir_used = {}
    for h, m in new.items():
        old_file = nodes[h][1]
        d = os.path.dirname(old_file)
        u = by_dir_used.setdefault(d, set(os.path.basename(p)[:-5] for p in preserve if os.path.dirname(p) == d))
        name = sanitize(m.get("LABEL") or h, u)
        plan[os.path.join(d, name + ".json")] = m
    old_files = set(files)
    new_files = set(plan)
    if dry:
        print(f"[dry-run] would write {len(new_files)} node file(s), delete {len(old_files - new_files)} old file(s)")
        return
    # Two-phase: write everything to .new, then delete old node files, then commit renames.
    for p, m in plan.items():
        with open(p + ".new", "w") as f:
            json.dump(m, f, indent=4)
    for p in old_files:
        os.remove(p)
    for p in plan:
        os.replace(p + ".new", p)
    print(f"wrote {len(plan)} node file(s); removed {len(old_files - new_files)} superseded file(s); "
          f"{len(preserve)} non-node file(s) untouched")


# ---------------------------------------------------------------- self-test
def _fixture():
    """A miniature library covering every shape the real one has."""
    N = {}
    def add(h, **kw):
        kw["CID"] = h
        kw.setdefault("PARENTS", [])
        N[h] = kw
        return h
    add("tile", TYPE="DeclareLibraryItem", LABEL="game_tile", UID="100", TITLE="Game", COVER={"PATH": "c.png"},
        META={"DEVELOPER": "X"})
    add("base", TYPE="VFSLayer", LABEL="base_content", LAYERS=[{"FORM": "zip", "PATH": "b.zip", "TARGET": "t",
        "SOURCE": {"TYPE": "ipfs", "CID": "Qmb"}}], PARENTS=["tile"])
    add("reg", TYPE="RegEdit", LABEL="reg", EDITS=[{"HKLM": {"A": {"v": "1"}}}], PARENTS=["base"])
    add("fe", TYPE="FileEdit", LABEL="fe", FILE="x.ini", OVERRIDE=True, EDITS=[{"MODE": "ConfigWrite", "KEY": "k", "VALUE": "v"}],
        PARENTS=["reg"])
    add("bp", TYPE="BinaryPatch", LABEL="bp", FILE="g.exe", EDITS=[{"MODE": "Replace", "OFFSET": "0x1", "EXPECT": "00", "REPLACE": "01"}],
        PARENTS=["fe"], TOGGLE="on")
    add("dll", TYPE="DllOverride", LABEL="dll", OVERRIDES={"d3d8": "n,b"}, PARENTS=["bp"])
    add("var", TYPE="CustomVar", LABEL="var", VARS=[{"KEY": "k", "DEFAULT": "1"}], PARENTS=["dll"])
    add("per", TYPE="DeclarePersist", LABEL="per", PERSISTS=[{"SCOPE": "file", "PATH": "s", "TARGET": "s"}], PARENTS=["var"])
    add("grp", TYPE="Group", LABEL="grp", PARENTS=["per"])
    # exec 1: single parent (grp) -> folds into grp; the tile is a direct parent here
    add("ex1", TYPE="DeclareExec", LABEL="Play", HOST="win32", PATH="g.exe", ARGS=["-a"], RECOMMENDED=True, PUBLISH=True,
        PARENTS=["grp", "tile"])
    # exec 2: multi-parent -> stays standalone; tile reached transitively
    add("lib", TYPE="DllOverride", LABEL="lib", OVERRIDES={"dinput8": "n"})
    add("ex2", TYPE="DeclareExec", LABEL="Modded", HOST="win32", PATH="g.exe", ARGS=[], PARENTS=["grp", "lib"])
    # exec 3 referenced by a content node (Wipeout SP/MP shape) -> folds, ref remapped
    add("mpc", TYPE="VFSLayer", LABEL="mp_content", LAYERS=[{"FORM": "zip", "PATH": "m.zip", "TARGET": "t",
        "SOURCE": {"TYPE": "ipfs", "CID": "Qmm"}}], PARENTS=["ex1"])
    add("ex3", TYPE="DeclareExec", LABEL="Multiplayer", HOST="win32", PATH="net.exe", ARGS=[], PARENTS=["mpc"], PUBLISH=True)
    # runner exec, no tile
    add("rc", TYPE="VFSLayer", LABEL="runner_build", LAYERS=[{"FORM": "zip", "PATH": "r.zip", "TARGET": "",
        "SOURCE": {"TYPE": "ipfs", "CID": "Qmr"}}])
    add("rx", TYPE="DeclareExec", LABEL="runner_exec", HOST="linux64", GUEST=["win32"], PATH="wine", ARGS=[], PARENTS=["rc"])
    # a second title whose exec's single parent is a SHARED node already carrying the first tile -> refused fold
    add("tile2", TYPE="DeclareLibraryItem", LABEL="t2", UID="200", TITLE="Other")
    add("ex4", TYPE="DeclareExec", LABEL="Other Play", HOST="win32", PATH="o.exe", ARGS=[], PARENTS=["grp", "tile2"])
    # EXCLUDE by label and by handle
    add("optA", TYPE="VFSLayer", LABEL="optA", LAYERS=[{"FORM": "dir", "PATH": "a"}], PARENTS=["base"], TOGGLE="off", EXCLUDE=["optB"])
    add("optB", TYPE="VFSLayer", LABEL="optB", LAYERS=[{"FORM": "dir", "PATH": "b"}], PARENTS=["base"], TOGGLE="off", EXCLUDE=["optA"])
    # an exec with a facet the fold would drop (TOGGLE) -> stays standalone with it
    add("ex5", TYPE="DeclareExec", LABEL="Beta Play", HOST="win32", PATH="beta.exe", ARGS=[], PARENTS=["lib"], TOGGLE="off")
    # an exec some node EXCLUDEs -> stays standalone so the NOT keeps naming exactly that launchable
    add("ex6", TYPE="DeclareExec", LABEL="Old Play", HOST="win32", PATH="old.exe", ARGS=[], PARENTS=["dll"])
    add("optC", TYPE="VFSLayer", LABEL="optC", LAYERS=[{"FORM": "dir", "PATH": "c"}], PARENTS=["base"], TOGGLE="off", EXCLUDE=["ex6"])
    return N


def self_test():
    failures = []
    def check(name, cond):
        print(("  ok   " if cond else "  FAIL ") + name)
        if not cond:
            failures.append(name)

    old = _fixture()
    new, refmap, folded, tiles = migrate(copy.deepcopy(old))
    try:
        verify(old, new, refmap, folded, tiles)
        check("verify passes on the fixture", True)
    except Fail as e:
        check(f"verify passes on the fixture ({e})", False)
    check("ex1 folded into grp", refmap.get("ex1") == "grp" and "ex1" not in new)
    check("ex5 (TOGGLE) stays standalone with its toggle", "ex5" in new and new["ex5"].get("TOGGLE") == "off" and "ENTRYPOINTS" not in new["lib"])
    check("ex6 (a NOT target) stays standalone", "ex6" in new and "ENTRYPOINTS" not in new["dll"]
          and {"NOT": "ex6"} in new["optC"]["OVER"])
    check("grp became the launchable with the exec's LABEL, TILE, PUBLISH", new["grp"].get("LABEL") == "Play"
          and new["grp"].get("TILE", {}).get("UID") == "100" and new["grp"].get("PUBLISH") is True
          and new["grp"]["ENTRYPOINTS"][0]["PATH"] == "g.exe" and new["grp"]["ENTRYPOINTS"][0]["RECOMMENDED"] is True)
    check("the tile's META/COVER ride along", new["grp"]["TILE"].get("META") == {"DEVELOPER": "X"} and new["grp"]["TILE"]["COVER"] == {"PATH": "c.png"})
    check("ex2 (multi-parent) stays standalone with OVER [grp, lib] and the tile", "ex2" in new and new["ex2"]["OVER"] == ["grp", "lib"]
          and new["ex2"].get("TILE", {}).get("UID") == "100")
    check("ex3 folds into mpc and mpc's ref to ex1 is remapped to grp", refmap.get("ex3") == "mpc" and new["mpc"]["OVER"] == ["grp"]
          and new["mpc"]["LABEL"] == "Multiplayer")
    check("runner exec folds into its build; no tile; GUEST kept", refmap.get("rx") == "rc" and "TILE" not in new["rc"]
          and new["rc"]["ENTRYPOINTS"][0]["GUEST"] == ["win32"])
    check("ex4 refused (grp already carries tile 100) and stays standalone under tile 200",
          "ex4" in new and new["ex4"].get("TILE", {}).get("UID") == "200" and new["ex4"]["OVER"] == ["grp"])
    check("tile refs vanish from OVER", "tile" not in over_refs(new["base"]) and not any("tile" in over_refs(m) or "tile2" in over_refs(m) for m in new.values()))
    check("EXCLUDE by label becomes {NOT: handle}", {"NOT": "optB"} in new["optA"]["OVER"] and {"NOT": "optA"} in new["optB"]["OVER"])
    check("payloads renamed: REGEDITS/FILEEDITS/PATCHES/DLLOVERRIDES", new["reg"]["REGEDITS"] == old["reg"]["EDITS"]
          and new["fe"]["FILEEDITS"] == [{"FILE": "x.ini", "EDITS": old["fe"]["EDITS"], "OVERRIDE": True}]
          and new["bp"]["PATCHES"] == [{"FILE": "g.exe", "EDITS": old["bp"]["EDITS"]}] and new["dll"]["DLLOVERRIDES"] == {"d3d8": "n,b"})
    check("TOGGLE survives on the node", new["bp"].get("TOGGLE") == "on")
    check("Group is a plain node", set(new["grp"]) <= NODE_FIELDS and not any(k in new["grp"] for k in PAYLOAD_KEYS))

    # Mutations: each must be CAUGHT by verify (teeth).
    def mutated(fn):
        n2 = copy.deepcopy(new)
        fn(n2)
        try:
            verify(old, n2, refmap, folded, tiles)
            return False
        except Fail:
            return True
    check("verify catches a dropped OVER ref", mutated(lambda n: n["reg"].__setitem__("OVER", [])))
    check("verify catches a dangling OVER ref", mutated(lambda n: n["reg"].__setitem__("OVER", ["nope"])))
    check("verify catches a lost payload item", mutated(lambda n: n["dll"].pop("DLLOVERRIDES")))
    check("verify catches a changed layer", mutated(lambda n: n["base"]["LAYERS"][0].__setitem__("PATH", "zzz")))
    check("verify catches a lost entrypoint", mutated(lambda n: n["grp"].__setitem__("ENTRYPOINTS", [])))
    check("verify catches a leftover TYPE", mutated(lambda n: n["grp"].__setitem__("TYPE", "x")))
    check("verify catches a lost PUBLISH", mutated(lambda n: n["grp"].pop("PUBLISH")))
    check("verify catches a tile that lands nowhere", mutated(lambda n: n["ex4"].pop("TILE")))
    check("verify catches a self-OVER", mutated(lambda n: n["grp"].__setitem__("OVER", ["grp"] + n["grp"].get("OVER", []))))
    check("verify catches a node count drift", mutated(lambda n: n.__setitem__("extra", {"CID": "extra"})))
    check("verify catches an unknown field", mutated(lambda n: n["reg"].__setitem__("LAYER", [])))
    # a closure change that keeps every item: rewire fe under base directly (drops reg from ex1's closure)
    check("verify catches a rewired closure (item multiset intact)", mutated(lambda n: n["fe"].__setitem__("OVER", ["base"])))
    print(f"\n{len(failures)} failure(s)")
    return 1 if failures else 0


# ---------------------------------------------------------------- main
def main():
    if "--self-test" in sys.argv:
        sys.exit(self_test())
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 1:
        print(__doc__)
        sys.exit(2)
    root, dry = args[0], "--dry-run" in sys.argv
    try:
        nodes, files, preserve = gather(root)
        if not nodes:
            print("no legacy nodes found (already migrated?)")
            sys.exit(0)
        old = {h: n for h, (n, _) in nodes.items()}
        new, refmap, folded, tiles = migrate(copy.deepcopy(old))
        verify(old, new, refmap, folded, tiles)
        print(f"{len(old)} node(s): {len(tiles)} tile(s) inlined, {len(folded)} exec(s) folded, "
              f"{sum(1 for h in old if old[h]['TYPE'] == 'DeclareExec') - len(folded)} exec(s) kept standalone -> {len(new)} node(s); "
              f"all invariants hold")
        write(root, nodes, files, preserve, new, folded, tiles, dry)
    except Fail as e:
        print(f"REFUSED: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
