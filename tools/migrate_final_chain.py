#!/usr/bin/env python3
"""One-shot migration to the FINAL CHAIN (MetaPackageFormat generation 5) — run once, never read two ways.

What changes, per node (no node is created or deleted; no content CID moves):
  1. every old launchable (ENTRYPOINTS without GUEST) declares  VARIANT = its LABEL  — "on the shelf";
  2. RECOMMENDED leaves the entries and becomes a NODE facet (true iff any entry was recommended; runners too);
  3. the MAIN face's TILE sinks to the title's pristine — the deepest node carrying LAYERS that every variant of the
     face is built on and that only this title is built on; the main face's other tiles (equal faces) are dropped
     (the variants inherit the face); CHILD faces keep their tiles where they are (a tile above the main tile);
     a face with no such home keeps its tile on the variant (the variant is its own pristine);
  4. every TOGGLE'd node INSIDE a closure becomes a GRAFT: removed from its dependants' OVER, and OVER'd on the
     variants whose closures contained it (a group when several) plus whatever of its own refs those closures do not
     already provide (its private substance). Its TOGGLE stays ("pre-ticked").

Invariants verified AFTER migrating (real run and --self-test alike) — a failure aborts before any write:
  I1 vocabulary: only known fields; VARIANT non-empty; no RECOMMENDED on an entry
  I2 nothing created, nothing deleted, no payload section changed, no content CID changed
  I3 MOUNT EQUIVALENCE under the author's defaults: for every old launchable, closure(new variant) ∪ closures of its
     pre-ticked grafts == its old (gated) closure, as node sets
  I4 effective entries (own, else the nearest beneath over bare refs) == the old entries minus RECOMMENDED
  I5 the face beneath every variant (nearest tile, own counts) == its old tile
  I6 VARIANT == old LABEL; node RECOMMENDED iff any old entry was; PUBLISH untouched
  I7 every title has exactly ONE main face (a tile with no same-UID tile beneath), unless it had several before
  I8 every converted graft is applicable and pre-selected for each variant that used to contain it

Usage:
       migrate_final_chain.py <library-root> [--dry-run]     # backs LIBRARY up to <root>.pre-final-chain-backup-<ts>.tar
       migrate_final_chain.py --self-test
"""
import copy, json, os, re, sys, tarfile, time
from collections import defaultdict


class Fail(Exception):
    pass


NODE_FIELDS = {"CID", "LABEL", "WHEN", "TOGGLE", "PUBLISH", "POS", "COMMENT", "TILE", "ENTRYPOINTS", "OVER", "VARIANT", "RECOMMENDED",
               "LAYERS", "PATCHES", "FILEEDITS", "REGEDITS", "DLLOVERRIDES", "VARS", "PERSISTS"}
PAYLOAD = ["LAYERS", "PATCHES", "FILEEDITS", "REGEDITS", "DLLOVERRIDES", "VARS", "PERSISTS"]


def is_node(n):
    return isinstance(n, dict) and "TYPE" not in n and any(k in n for k in NODE_FIELDS)


def load(path):
    with open(path) as f:
        return json.load(f)


# ---------------------------------------------------------------- gather
def gather(root):
    """handle -> (node, file). One node per file (the one-edge layout); a file holding several is accepted."""
    nodes, files = {}, {}
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
            if not docs or not all(is_node(d) for d in docs):
                if any(isinstance(d, dict) and "TYPE" in d for d in docs):
                    raise Fail(f"{p}: legacy TYPE node — run tools/migrate_one_edge.py first")
                continue
            for d in docs:
                h = d.get("CID")
                if not isinstance(h, str) or not h:
                    raise Fail(f"{p}: node without a CID handle ({d.get('LABEL')!r})")
                if h in nodes:
                    raise Fail(f"duplicate handle {h} in {p} and {nodes[h][1]}")
                nodes[h] = (d, p)
                files.setdefault(p, []).append(h)
    return nodes, files


# ---------------------------------------------------------------- graph helpers (the NEW rules, in python)
def bare(n):
    return [x for x in n.get("OVER", []) if isinstance(x, str)]


def positive(n):
    out = []
    for x in n.get("OVER", []):
        if isinstance(x, str):
            out.append(x)
        elif isinstance(x, list):
            out += [m for m in x if isinstance(m, str)]
    return out


def closure(nodes, h, refs=bare):
    """everything reachable from h through `refs` (h included)."""
    seen, st = set(), [h]
    while st:
        x = st.pop()
        if x in seen or x not in nodes:
            continue
        seen.add(x)
        st += refs(nodes[x])
    return seen


def old_closure(nodes, h):
    """the OLD gated closure under the author's defaults: bare refs, a TOGGLE'd node only when 'on', never
    descending into an off one (the hierarchy gate). Groups and NOTs did not occur inside closures (checked)."""
    seen, st = set(), [h]
    while st:
        x = st.pop()
        if x in seen or x not in nodes:
            continue
        n = nodes[x]
        if x != h and n.get("TOGGLE") == "off":
            continue
        seen.add(x)
        st += bare(n)
    return seen


def is_launch(n):
    return "ENTRYPOINTS" in n and not any(isinstance(e, dict) and e.get("GUEST") for e in n["ENTRYPOINTS"])


def is_runner(n):
    return "ENTRYPOINTS" in n and any(isinstance(e, dict) and e.get("GUEST") for e in n["ENTRYPOINTS"])


def face_key(tile):
    return json.dumps({"UID": tile.get("UID"), "TITLE": tile.get("TITLE"), "COVER": tile.get("COVER")}, sort_keys=True)


def nearest(nodes, h, pred, refs):
    """the nearest node beneath h (own = 0) satisfying pred, by distance over `refs`, ties by OVER order."""
    memo = {}

    def rec(x, stack):
        if x in memo:
            return memo[x]
        if x in stack or x not in nodes:
            return (None, -1)
        if pred(nodes[x]):
            memo[x] = (x, 0)
            return memo[x]
        best = (None, -1)
        for p in refs(nodes[x]):
            k, d = rec(p, stack | {x})
            if d < 0:
                continue
            if best[1] < 0 or d + 1 < best[1]:
                best = (k, d + 1)
        memo[x] = best
        return best
    return rec(h, frozenset())


def face_of(nodes, h):
    k, _ = nearest(nodes, h, lambda n: "TILE" in n, positive)
    return nodes[k]["TILE"] if k else None


def effective_entries(nodes, h):
    k, _ = nearest(nodes, h, lambda n: bool(n.get("ENTRYPOINTS")), bare)
    return nodes[k]["ENTRYPOINTS"] if k else []


def has_identity(nodes, h):
    return face_of(nodes, h) is not None


# ---------------------------------------------------------------- migrate
def migrate(old):
    new = copy.deepcopy(old)
    report = {"variants": 0, "recommended": 0, "tiles_sunk": {}, "tiles_dropped": 0, "grafts": {}, "kept_on_variant": []}
    launch = {h for h, n in old.items() if is_launch(n)}
    runners = {h for h, n in old.items() if is_runner(n)}

    # 1 + 2: VARIANT and node-level RECOMMENDED; entries lose RECOMMENDED (runners too).
    for h in launch | runners:
        m = new[h]
        rec = any(isinstance(e, dict) and e.get("RECOMMENDED") for e in m["ENTRYPOINTS"])
        for e in m["ENTRYPOINTS"]:
            if isinstance(e, dict):
                e.pop("RECOMMENDED", None)
        if rec:
            m["RECOMMENDED"] = True
            report["recommended"] += 1
        if h in launch and not m.get("VARIANT"):
            m["VARIANT"] = m.get("LABEL") or h
            report["variants"] += 1

    # 3: faces. Group the launchables by UID (own tile, else the face beneath), then by face.
    uid_of = {}
    for h in launch:
        t = old[h].get("TILE") or face_of(old, h)
        if t:
            uid_of[h] = t["UID"]
    by_uid = defaultdict(list)
    for h, u in uid_of.items():
        by_uid[u].append(h)
    owners = defaultdict(set)
    for h, u in uid_of.items():
        for x in closure(old, h):
            owners[x].add(u)

    def height(h, u):
        memo = {}

        def rec(x, stack):
            if x in memo:
                return memo[x]
            best = 0
            for r in bare(old.get(x, {})):
                if r in old and owners.get(r) == {u} and r not in stack:
                    best = max(best, 1 + rec(r, stack | {x}))
            memo[x] = best
            return best
        return rec(h, frozenset())

    for u, hs in by_uid.items():
        faces = defaultdict(list)          # face key -> launchables carrying that tile on themselves
        for h in hs:
            if "TILE" in old[h]:
                faces[face_key(old[h]["TILE"])].append(h)
        if not faces:
            continue
        # the main face = the face whose variants have the smallest chain height (the base is built on the least)
        main = min(faces, key=lambda k: (min(height(h, u) for h in faces[k]), k))
        members = faces[main]
        # the REQUIRED chain only: a pre-ticked optional piece (a TOGGLE'd node) and whatever hangs beneath it is
        # never the pristine — a library reachable only through an optional piece would otherwise pass as one
        def required(h):
            seen, st = set(), [h]
            while st:
                x = st.pop()
                if x in seen or x not in old or (x != h and "TOGGLE" in old[x]):
                    continue
                seen.add(x)
                st += bare(old[x])
            return seen
        common = set.intersection(*[required(h) for h in members])
        cands = [x for x in common if x not in launch and owners.get(x) == {u} and old[x].get("LAYERS")]
        if cands:
            # the pristine: the biggest content (a game's base zip dwarfs its patches and libraries), then the
            # deepest (the smallest closure), then by label — deterministic, and right for every title we have
            def size(x):
                return sum((L.get("SOURCE") or {}).get("SIZE", 0) for L in old[x]["LAYERS"] if isinstance(L, dict))
            home = min(cands, key=lambda x: (-size(x), len(closure(old, x)), old[x].get("LABEL", "")))
            tile = copy.deepcopy(old[members[0]]["TILE"])
            new[home]["TILE"] = tile
            for h in members:
                del new[h]["TILE"]
                report["tiles_dropped"] += 1
            report["tiles_sunk"][u] = (old[home].get("LABEL"), len(members))
        else:
            report["kept_on_variant"].append(u)
        # equal-face tiles OTHER than the main's: leave (each child face keeps one tile per variant; harmless)

    # 4: in-closure toggles become grafts.
    roots = launch | runners
    toggled = {}
    for r in roots:
        for x in closure(old, r):
            if x != r and "TOGGLE" in old[x]:
                toggled.setdefault(x, set()).add(r)
    # phase A: every toggled node leaves every dependant's OVER (all of them first — a toggled node still hanging
    # off a dependant would make a sibling's substance look "provided")
    for t in toggled:
        for d, m in new.items():
            if t in bare(m):
                m["OVER"] = [e for e in m["OVER"] if e != t]
                if not m["OVER"]:
                    del m["OVER"]
    # phase B: the graft's requirement = EVERY root (variant or runner) whose closure contained it — each is a
    # selection in its own right (Multiplayer is OVER Single Player and both are picked from the card) — a group
    # when several; plus its own refs the roots' closures do not already provide (its private substance)
    for t, roots_with in toggled.items():
        outer = sorted(roots_with)
        provided = set().union(*[closure(new, r) for r in outer]) if outer else set()
        own_refs = [x for x in bare(old[t]) if x not in provided]
        other = [e for e in old[t].get("OVER", []) if not isinstance(e, str)]
        req = [outer[0]] if len(outer) == 1 else ([outer] if outer else [])
        new[t]["OVER"] = req + own_refs + other
        report["grafts"][old[t].get("LABEL")] = [old[r].get("LABEL") for r in outer]
    return new, report


# ---------------------------------------------------------------- verify
def applicable(new, g, selected):
    """the NEW rule: a bare ref REQUIRES only when it names a variant; otherwise it composes (mounts beneath).
    Requirements are transitive over composition: every node the graft brings is checked the same way."""
    for n in [g] + [b for b in closure(new, g) if b != g]:
        for e in new[n].get("OVER", []):
            if isinstance(e, dict):
                if e.get("NOT") in selected:
                    return False
            elif isinstance(e, list):
                if not any(m in selected for m in e):
                    return False
            elif isinstance(e, str):
                if e in new and "VARIANT" in new[e] and e not in selected:
                    return False
    return True


def verify(old, new):
    launch = {h for h, n in old.items() if is_launch(n)}
    # I1
    for h, m in new.items():
        bad = set(m) - NODE_FIELDS
        if bad:
            raise Fail(f"I1: {h} has unknown fields {bad}")
        if "VARIANT" in m and (not isinstance(m["VARIANT"], str) or not m["VARIANT"]):
            raise Fail(f"I1: {h} VARIANT must be a non-empty string")
        for e in m.get("ENTRYPOINTS", []):
            if isinstance(e, dict) and "RECOMMENDED" in e:
                raise Fail(f"I1: {h} entry still carries RECOMMENDED")
    # I2
    if set(old) != set(new):
        raise Fail("I2: node set changed")
    for h in old:
        for k in PAYLOAD:
            if old[h].get(k) != new[h].get(k):
                raise Fail(f"I2: {h} payload {k} changed")
        for k in ("PUBLISH", "LABEL", "WHEN", "TOGGLE", "COMMENT"):
            if old[h].get(k) != new[h].get(k):
                raise Fail(f"I2: {h} {k} changed")
    # I3 mount equivalence under defaults
    for h in launch:
        want = old_closure(old, h)
        got = set(closure(new, h))
        selected = {h}
        # the CONVERTED grafts (they were inside the old closure): pre-ticked and applicable against {h} they mount
        # above the variant, so the mount under the author's defaults is unchanged. Grafts that were already
        # grafts (widescreen) are untouched by the migration and mount exactly as before on both sides.
        for g, m in new.items():
            if g not in want or g in got or m.get("TOGGLE") != "on" or "VARIANT" in m or is_runner(m) or not m.get("OVER"):
                continue
            if applicable(new, g, selected):
                got |= closure(new, g)
                selected.add(g)
        if want != got:
            raise Fail(f"I3: mount of {old[h].get('LABEL')!r} changed: +{sorted(old[x].get('LABEL','?') for x in got - want)[:3]} -{sorted(old[x].get('LABEL','?') for x in want - got)[:3]}")
    # I4 / I5 / I6
    for h in launch:
        want = [{k: v for k, v in e.items() if k != "RECOMMENDED"} for e in old[h]["ENTRYPOINTS"]]
        if effective_entries(new, h) != want:
            raise Fail(f"I4: entries of {old[h].get('LABEL')!r} changed")
        if "TILE" in old[h]:
            f = face_of(new, h)
            if f is None or face_key(f) != face_key(old[h]["TILE"]):
                raise Fail(f"I5: face of {old[h].get('LABEL')!r} changed")
        if new[h].get("VARIANT") != (old[h].get("VARIANT") or old[h].get("LABEL") or h):
            raise Fail(f"I6: VARIANT of {h}")
        rec = any(isinstance(e, dict) and e.get("RECOMMENDED") for e in old[h]["ENTRYPOINTS"])
        if bool(new[h].get("RECOMMENDED")) != rec:
            raise Fail(f"I6: RECOMMENDED of {h}")
    # I7 one main face per UID (unless the old tree already had several)
    def mains(nodes):
        tiles = defaultdict(list)
        for h, n in nodes.items():
            if "TILE" in n:
                tiles[n["TILE"].get("UID")].append(h)
        out = {}
        for u, hs in tiles.items():
            ms = set()
            for h in hs:
                beneath = closure(nodes, h) - {h}
                if not any(x in tiles[u] and face_key(nodes[x]["TILE"]) != face_key(nodes[h]["TILE"]) for x in beneath):
                    ms.add(face_key(nodes[h]["TILE"]))
            out[u] = len(ms)
        return out
    om, nm = mains(old), mains(new)
    for u in nm:
        if nm[u] > 1 and nm[u] > om.get(u, 0):
            raise Fail(f"I7: UID {u} has {nm[u]} main faces after migration")
    # I8 converted grafts applicable for every root that contained them
    for t, n in old.items():
        if "TOGGLE" not in n:
            continue
        for r in launch:
            if t in old_closure(old, r) and t != r:
                if t in closure(new, r):
                    raise Fail(f"I8: {n.get('LABEL')!r} still inside the closure of {old[r].get('LABEL')!r}")
                if not applicable(new, t, {r}):
                    raise Fail(f"I8: graft {n.get('LABEL')!r} not applicable on {old[r].get('LABEL')!r}")


# ---------------------------------------------------------------- write
def write(root, nodes, files, new, dry):
    changed = [h for h in new if new[h] != nodes[h][0]]
    if dry:
        print(f"[dry-run] would rewrite {len(changed)} node file(s) in place")
        return
    ts = time.strftime("%Y%m%d-%H%M%S")
    bk = f"{root.rstrip('/')}.pre-final-chain-backup-{ts}.tar"
    with tarfile.open(bk, "w") as tar:
        for p in files:
            tar.add(p)
    plan = {}
    for h in changed:
        p = nodes[h][1]
        if len(files[p]) != 1:
            raise Fail(f"{p} holds several nodes — one node per file expected")
        plan[p] = new[h]
    for p, m in plan.items():
        with open(p + ".new", "w") as f:
            json.dump(m, f, indent=4)
    for p in plan:
        os.replace(p + ".new", p)
    print(f"rewrote {len(plan)} node file(s) in place; backup: {bk}")


# ---------------------------------------------------------------- self-test
def _fixture():
    N = {}

    def add(h, **kw):
        kw["CID"] = h
        kw.setdefault("LABEL", h)
        N[h] = kw
        return h
    L = lambda p: [{"FORM": "zip", "PATH": p, "TARGET": "t", "SOURCE": {"TYPE": "ipfs", "CID": "Qm" + p}}]
    # Wipeout-like: pristine, registry, two launchables with tiles; a soundtrack TOGGLE'd INSIDE the closure
    # (under the registry), a widescreen graft already OVER [[SP, MP]].
    add("woxl_content", LAYERS=L("woxl.zip"))
    add("dxwnd_dll", DLLOVERRIDES={"winmm": "n,b"})
    add("woxl_soundtrack", LAYERS=L("ost.zip"), TOGGLE="on", OVER=["dxwnd_dll"])
    add("woxl_registry", REGEDITS=[{"HKCU": {"A": {"v": "1"}}}], OVER=["woxl_content", "woxl_soundtrack"])
    add("SP", TILE={"UID": "17260", "TITLE": "Wipeout XL", "COVER": "c.png"}, OVER=["woxl_registry"], PUBLISH=True,
        ENTRYPOINTS=[{"LABEL": "SP", "HOST": "win32", "PATH": "w.exe", "RECOMMENDED": True}])
    add("MP", TILE={"UID": "17260", "TITLE": "Wipeout XL", "COVER": "c.png"}, OVER=["woxl_registry", "netpatch"],
        ENTRYPOINTS=[{"LABEL": "MP", "HOST": "win32", "PATH": "lobby.exe"}])
    add("netpatch", LAYERS=L("net.zip"))
    add("widescreen", PATCHES=[{"FILE": "w.exe", "EDITS": [{"MODE": "Replace", "OFFSET": "0x1", "EXPECT": "00", "REPLACE": "01"}]}],
        TOGGLE="on", OVER=[["SP", "MP"]])
    # AoE2-like: three faces over one pristine; the expansion chains through content, not the base launchable.
    add("aok_base", LAYERS=L("aok.zip"))
    add("aok_registry", REGEDITS=[{"HKLM": {"B": {"v": "2"}}}], OVER=["aok_base"])
    add("aok_nocd", LAYERS=L("nocd.zip"), OVER=["aok_registry"])
    add("aok_ost", LAYERS=L("aokost.zip"), TOGGLE="on", OVER=["aok_registry"])
    add("AoK", TILE={"UID": "749", "TITLE": "Age of Kings"}, OVER=["aok_nocd", "aok_ost"],
        ENTRYPOINTS=[{"LABEL": "Vanilla", "HOST": "win32", "PATH": "empires2.exe"}])
    add("tc_base", LAYERS=L("tc.zip"), OVER=["aok_registry"])
    add("Conq", TILE={"UID": "749", "TITLE": "The Conquerors"}, OVER=["tc_base"],
        ENTRYPOINTS=[{"LABEL": "Vanilla", "HOST": "win32", "PATH": "age2_x1.exe", "RECOMMENDED": True}])
    add("fe_base", LAYERS=L("fe.zip"), OVER=["tc_base"])
    add("FE", TILE={"UID": "749", "TITLE": "Forgotten Empires"}, OVER=["fe_base"],
        ENTRYPOINTS=[{"LABEL": "FE", "HOST": "win32", "PATH": "age2_x1.5.exe"}])
    # Minecraft-like: a version chain, every version a launchable with the same tile.
    add("mc_rd", LAYERS=L("rd.jar"))
    add("v1", TILE={"UID": "320", "TITLE": "Minecraft"}, OVER=["mc_rd"], LAYERS=L("v1.vgdelta"),
        ENTRYPOINTS=[{"LABEL": "v1", "HOST": "java8", "PATH": ""}])
    add("v2", TILE={"UID": "320", "TITLE": "Minecraft"}, OVER=["v1"], LAYERS=L("v2.vgdelta"),
        ENTRYPOINTS=[{"LABEL": "v2", "HOST": "java8", "PATH": ""}])
    # NFSU2-like: toggled nodes over a library (substance) inside the launchable's closure.
    add("asiloader", LAYERS=L("asi.zip"), DLLOVERRIDES={"dinput8": "n"})
    add("nfs_wide", LAYERS=L("wide.zip"), TOGGLE="on", OVER=["asiloader"])
    add("nfs_xinput", LAYERS=L("xinput.zip"), TOGGLE="off", OVER=["asiloader"])
    add("nfs_content", LAYERS=L("nfs.zip"))
    add("NFSU2", TILE={"UID": "77", "TITLE": "NFSU2"}, OVER=["nfs_content", "nfs_wide", "nfs_xinput"],
        ENTRYPOINTS=[{"LABEL": "Play", "HOST": "win32", "PATH": "speed2.exe"}])
    # a single-node game: content, tile and entry in one — its own pristine
    add("Solo", TILE={"UID": "5", "TITLE": "Solo"}, LAYERS=L("solo.zip"), ENTRYPOINTS=[{"HOST": "linux64", "PATH": "solo"}])
    # a runner with a recommended entry
    add("winebuild", LAYERS=L("wine.zip"))
    add("proton", OVER=["winebuild"], ENTRYPOINTS=[{"HOST": "linux64", "GUEST": ["win32"], "PATH": "%RunnerMount%/proton", "RECOMMENDED": True}])
    return N


def self_test():
    failures = []

    def check(name, cond):
        print(("  ok   " if cond else "  FAIL ") + name)
        if not cond:
            failures.append(name)
    old = _fixture()
    new, rep = migrate(copy.deepcopy(old))
    try:
        verify(old, new)
        check("verify passes on the fixture", True)
    except Fail as e:
        check(f"verify passes on the fixture ({e})", False)
    check("every launchable is a VARIANT named by its label", all(new[h].get("VARIANT") == h for h in ("SP", "MP", "AoK", "Conq", "FE", "v1", "v2", "NFSU2", "Solo")))
    check("RECOMMENDED moved to the node", new["SP"].get("RECOMMENDED") is True and "RECOMMENDED" not in new["SP"]["ENTRYPOINTS"][0]
          and new["proton"].get("RECOMMENDED") is True and "RECOMMENDED" not in new["proton"]["ENTRYPOINTS"][0])
    check("the Wipeout tile sank to the pristine, the variants dropped theirs", "TILE" in new["woxl_content"] and "TILE" not in new["SP"] and "TILE" not in new["MP"])
    check("the AoK tile sank to the AoK pristine; the child faces kept theirs", "TILE" in new["aok_base"] and "TILE" not in new["AoK"] and "TILE" in new["Conq"] and "TILE" in new["FE"])
    check("the Minecraft tile sank to the chain root", "TILE" in new["mc_rd"] and "TILE" not in new["v1"] and "TILE" not in new["v2"])
    check("a single-node game keeps its tile", "TILE" in new["Solo"])
    check("the soundtrack is a graft over both variants, its substance kept, the registry no longer OVER it",
          new["woxl_soundtrack"]["OVER"] == [["MP", "SP"], "dxwnd_dll"] and "woxl_soundtrack" not in new["woxl_registry"]["OVER"])
    check("the AoK soundtrack is a graft over AoK; the registry it was over is already provided",
          new["aok_ost"]["OVER"] == ["AoK"] and "aok_ost" not in new["AoK"]["OVER"])
    check("the NFSU2 toggles are grafts over NFSU2 with the library as substance",
          new["nfs_wide"]["OVER"] == ["NFSU2", "asiloader"] and new["nfs_xinput"]["OVER"] == ["NFSU2", "asiloader"]
          and new["NFSU2"]["OVER"] == ["nfs_content"])
    check("the existing widescreen graft is untouched", new["widescreen"] == old["widescreen"])
    check("faces: AoK main, Conq child, FE grandchild",
          face_of(new, "Conq")["TITLE"] == "The Conquerors" and "aok_base" in closure(new, "Conq") and "tc_base" in closure(new, "FE"))
    check("SP's mount under defaults == the old gated closure", set(closure(new, "SP")) | closure(new, "woxl_soundtrack") == old_closure(old, "SP"))
    # verify has teeth
    def broken(mutate):
        n2 = copy.deepcopy(new)
        mutate(n2)
        try:
            verify(old, n2)
            return False
        except Fail:
            return True
    check("verify catches a graft left inside a closure", broken(lambda n: n["woxl_registry"]["OVER"].append("woxl_soundtrack")))
    check("verify catches a lost face", broken(lambda n: n["woxl_content"].pop("TILE")))
    check("verify catches a changed entry", broken(lambda n: n["v1"]["ENTRYPOINTS"][0].__setitem__("PATH", "x")))
    check("verify catches a graft blocked on its own variant", broken(lambda n: n["aok_ost"].__setitem__("OVER", ["AoK", {"NOT": "AoK"}])))
    check("verify catches a RECOMMENDED left on an entry", broken(lambda n: n["SP"]["ENTRYPOINTS"][0].__setitem__("RECOMMENDED", True)))
    check("verify catches a changed payload", broken(lambda n: n["netpatch"].__setitem__("LAYERS", [])))
    print(f"\n{len(failures)} failure(s)")
    return 1 if failures else 0


def main():
    if "--self-test" in sys.argv:
        sys.exit(self_test())
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 1:
        print(__doc__)
        sys.exit(2)
    root, dry = args[0], "--dry-run" in sys.argv
    try:
        nodes, files = gather(root)
        if not nodes:
            print("no nodes found")
            sys.exit(0)
        old = {h: n for h, (n, _) in nodes.items()}
        if any("VARIANT" in n for n in old.values()):
            raise Fail("VARIANT already present — already migrated?")
        new, rep = migrate(copy.deepcopy(old))
        verify(old, new)
        print(f"{len(old)} node(s): {rep['variants']} variant(s) declared, {rep['recommended']} RECOMMENDED moved to nodes, "
              f"{len(rep['tiles_sunk'])} main tile(s) sunk ({rep['tiles_dropped']} copies dropped), "
              f"{len(rep['kept_on_variant'])} title(s) keep the tile on the variant, {len(rep['grafts'])} in-closure toggle(s) → grafts; all invariants hold")
        for u, (home, n) in sorted(rep["tiles_sunk"].items()):
            print(f"   UID {u}: tile → {home!r} ({n} variant(s))")
        for g, vs in rep["grafts"].items():
            print(f"   graft {g!r} over {vs}")
        write(root, nodes, files, new, dry)
    except Fail as e:
        print(f"REFUSED: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
