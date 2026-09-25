#!/usr/bin/env python3
"""Generation 6 — the generic transform (spec final_v2_final_this_time_v3 §6 steps 1–4). Run once, never read two ways.

Per node (no node is created or deleted; no content CID moves; the CID handle stays):
  1. LAYERS := [bare OVER refs as NODE layers, in order] + [content layers] + [one layer per non-empty section], in
     gen-5's lowering order (LAYERS, PATCHES, FILEEDITS, REGEDITS, DLLOVERRIDES, VARS, PERSISTS) then ENV, EXEC.
     FORM zip|dir|file|delta → ZIP|DIR|FILE|DELTA (payload = PATH); SOURCE {CID,SIZE} → SOURCE (CID) + SIZE;
     TARGET t → "FILES/t". PATCHES + FILEEDITS → EDIT {TARGET: FILES/<FILE>}; OVERRIDE is dropped (listed).
     REGEDITS → REG (ARCHITECTURE → ARCH). DLLOVERRIDES → DLL. ENV + ENV_REMOVE → ENV (removed = null).
     ENTRYPOINTS → EXEC (PATH → EXE). PERSISTS → KEEP. A declaration's WHEN stays inside the VARS declaration
     (it gates the VALUE, as today); a layer's WHEN gates the layer.
  2. Game entries (no GUEST) are labelled "Play": gen 6 folds EXEC per label, so one label per face makes every
     version's entry the same entry (gen 5 replaced the whole block; distinct labels would accumulate).
  3. Tiles: a node TILE moves onto that node's "Play" entry (a partial entry when the node has none): UID = the
     face's own id (META.GAMEUID, else TGDBID for a child face), PARENTUID = the family id when it differs;
     META.GAMEUID is dropped (UID says it). COVER {PATH, SOURCE} → {FILE, SOURCE, SIZE}. RECOMMENDED: true → the
     UID list of the faces the node presents.
  4. TOGGLE'd grafts become the variant's own options: each variant the graft applied to (a member of its any-of,
     or a VARIANT it named) gets a bool VARS entry (KEY = the graft's LABEL, DEFAULT "1"/"0" from TOGGLE) and a
     {"NODE": graft, "WHEN": "%KEY%==1"} layer; the graft keeps only its composition refs. Layer TOGGLEs drop.

Invariants checked before anything is written: every node transformed; no content CID lost; every NODE ref
resolves; every variant presents exactly its gen-5 face; nothing left in the old vocabulary.

Usage: migrate.py <library-root> [--dry-run] [--report FILE]      # backs node files up to <root>.pre-gen6-<ts>.tar
       migrate.py --self-test
"""
import copy, json, os, sys, tarfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from collections import deque


class Fail(Exception):
    pass


OLD_FIELDS = {"CID", "LABEL", "WHEN", "TOGGLE", "POS", "COMMENT", "TILE", "ENTRYPOINTS", "OVER", "VARIANT", "RECOMMENDED",
              "LAYERS", "PATCHES", "FILEEDITS", "REGEDITS", "DLLOVERRIDES", "VARS", "PERSISTS", "ENV", "ENV_REMOVE"}
NEW_FIELDS = {"CID", "LABEL", "POS", "COMMENT", "VARIANT", "RECOMMENDED", "LAYERS"}
TYPE_KEYS = ["ZIP", "FILE", "DELTA", "DIR", "NODE", "EDIT", "REG", "VARS", "ENV", "DLL", "EXEC", "KEEP", "ANY", "NOT"]
FORMS = {"zip": "ZIP", "dir": "DIR", "file": "FILE", "delta": "DELTA"}
PLAY = "Play"


def is_old_node(n):
    return isinstance(n, dict) and "TYPE" not in n and "CID" in n and set(n) <= OLD_FIELDS


# ---------------------------------------------------------------- gather
def gather(root):
    """handle -> (node, file). One node per file."""
    nodes = {}
    for dp, dn, fns in os.walk(root):
        dn[:] = sorted(d for d in dn if not d.startswith("_friend_"))
        for fn in sorted(fns):
            if not fn.endswith(".json"):
                continue
            p = os.path.join(dp, fn)
            try:
                j = json.load(open(p))
            except Exception as e:
                raise Fail(f"unparseable {p}: {e}")
            if not isinstance(j, dict) or "CID" not in j:
                continue
            if not is_old_node(j):
                raise Fail(f"{p}: not a gen-5 node (fields {sorted(set(j) - OLD_FIELDS)}) — already migrated?")
            h = j["CID"]
            if h in nodes:
                raise Fail(f"duplicate handle {h} in {p} and {nodes[h][1]}")
            nodes[h] = (j, p)
    return nodes


# ---------------------------------------------------------------- gen-5 facts the transform needs
def new_report():
    return {"overrides_dropped": [], "layer_toggles_dropped": [], "toggles": [], "graft_refs_to_variant_dropped": [],
            "reg_null_to_empty": [], "edits_moved": [], "canonical_layers": [], "grafts_kept": [], "overrides_taken_back": []}


def bare(n):
    return [x for x in n.get("OVER", []) if isinstance(x, str)]


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


def face_of(nodes, h):
    """gen 5's face: the nearest node carrying a TILE over bare refs (breadth-first, own counts)."""
    q, seen = deque([h]), {h}
    while q:
        x = q.popleft()
        n = nodes.get(x)
        if n is None:
            continue
        if "TILE" in n:
            return x
        for r in bare(n):
            if r not in seen:
                seen.add(r); q.append(r)
    return None


def tile_uids(tile):
    """(UID, PARENTUID|None) of a gen-5 TILE: the face's own id, and the family id when it differs."""
    fam = str(tile["UID"])
    meta = tile.get("META") or {}
    own = meta.get("GAMEUID") or None
    if own is None and meta.get("TGDBID") and str(meta["TGDBID"]) != fam:
        own = meta["TGDBID"]                      # a child face without a GAMEUID (MW4 Black Knight): its TGDB id
    own = str(own) if own is not None else fam
    return own, (fam if own != fam else None)


# ---------------------------------------------------------------- transform one node
def files_addr(t):
    return "FILES" + ("/" + t if t else "")


def content_layer(L, where, report):
    form = L.get("FORM")
    if form not in FORMS:
        raise Fail(f"{where}: LAYERS entry FORM {form!r}")
    out = {FORMS[form]: L["PATH"]}
    s = L.get("SOURCE")
    if s is not None:
        if not isinstance(s, dict) or s.get("TYPE") != "ipfs" or set(s) - {"TYPE", "CID", "SIZE"} or "CID" not in s:
            raise Fail(f"{where}: SOURCE shape {s!r}")
        out["SOURCE"] = s["CID"]
        if "SIZE" in s:
            out["SIZE"] = s["SIZE"]
    if "TARGET" in L:
        out["TARGET"] = files_addr(L["TARGET"])
    for k in ("SUBMOUNTS", "COMMENT", "WHEN"):          # SUBMOUNTS: until the library step (§6 step 5) replaces it
        if k in L:
            out[k] = L[k]
    if "TOGGLE" in L:
        report["layer_toggles_dropped"].append(where)
    extra = set(L) - {"FORM", "PATH", "SOURCE", "TARGET", "SUBMOUNTS", "COMMENT", "WHEN", "TOGGLE"}
    if extra:
        raise Fail(f"{where}: LAYERS entry fields {sorted(extra)}")
    return out


def edit_layer(E, where, kind, report):
    if set(E) - {"FILE", "EDITS", "OVERRIDE", "COMMENT"}:
        raise Fail(f"{where}: {kind} entry fields {sorted(set(E) - {'FILE', 'EDITS', 'OVERRIDE', 'COMMENT'})}")
    out = {"EDIT": copy.deepcopy(E["EDITS"]), "TARGET": files_addr(E["FILE"])}
    if E.get("OVERRIDE"):
        report["overrides_dropped"].append(f"{where}: {kind} {E['FILE']}")
        out["__override__"] = True                         # consumed by take_back_overrides(), never written
    if "COMMENT" in E:
        out["COMMENT"] = E["COMMENT"]
    return out


def reg_layer(E, where, report):
    tree = {k: v for k, v in E.items() if k not in ("ARCHITECTURE", "OVERRIDE", "WHEN", "COMMENT")}
    if E.get("OVERRIDE"):
        report["overrides_dropped"].append(f"{where}: REGEDITS {sorted(tree)}")
    def nulls_to_empty(t, path):
        # gen 5 writes a null value as an empty REG_SZ; in gen 6 null deletes — keep what gen 5 wrote
        o = {}
        for k, v in t.items():
            if v is None:
                report["reg_null_to_empty"].append(f"{where}: {path}\\{k}")
                o[k] = ""
            else:
                o[k] = nulls_to_empty(v, f"{path}\\{k}") if isinstance(v, dict) else copy.deepcopy(v)
        return o
    out = {"REG": nulls_to_empty(tree, "")}
    for k, nk in (("ARCHITECTURE", "ARCH"), ("WHEN", "WHEN"), ("COMMENT", "COMMENT")):
        if k in E:
            out[nk] = E[k]
    return out


def keep_layer(persists, where):
    keep = {}
    for P in persists:
        scope = P.get("SCOPE", "file")
        path = P.get("PATH", "")
        if scope == "file":
            addr = files_addr(path)
        elif scope == "registry":
            addr = "REG" + ("/" + path.replace("\\", "/") if path else "")
        else:
            raise Fail(f"{where}: PERSISTS SCOPE {scope!r}")
        opts = {}
        if P.get("TARGET"):
            opts["NAME"] = P["TARGET"]
        if "CLOUD" in P:
            opts["CLOUD"] = P["CLOUD"]
        if set(P) - {"SCOPE", "PATH", "TARGET", "CLOUD"}:
            raise Fail(f"{where}: PERSISTS fields {sorted(set(P) - {'SCOPE', 'PATH', 'TARGET', 'CLOUD'})}")
        if addr in keep:
            raise Fail(f"{where}: two PERSISTS on {addr}")
        keep[addr] = opts or True
    return {"KEEP": keep}


def exec_entry(E, where):
    runner = bool(E.get("GUEST"))
    out = {"LABEL": E.get("LABEL", "") if runner else PLAY, "HOST": E["HOST"]}
    if runner:
        out["GUEST"] = E["GUEST"]
    if "PATH" in E:
        out["EXE"] = E["PATH"]
    known = {"LABEL", "HOST", "GUEST", "PATH", "ARGS", "WORKDIR", "CONTENT_ROOT", "PREFIX_GENERATE", "UNIFIED_RUNTIME"}
    if set(E) - known:
        raise Fail(f"{where}: ENTRYPOINTS fields {sorted(set(E) - known)}")
    for k in ("ARGS", "WORKDIR", "CONTENT_ROOT", "PREFIX_GENERATE", "UNIFIED_RUNTIME"):
        if k in E:
            out[k] = E[k]
    return out


def new_tile(T, parent):
    uid = tile_uids(T)[0]
    out = {"UID": uid}
    if parent:
        out["PARENTUID"] = parent
    out["TITLE"] = T["TITLE"]
    c = T.get("COVER")
    if c:
        s = c.get("SOURCE") or {}
        cov = {"FILE": c["PATH"]}
        if s.get("CID"):
            cov["SOURCE"] = s["CID"]
        if s.get("SIZE") is not None:
            cov["SIZE"] = s["SIZE"]
        out["COVER"] = cov
    meta = {k: v for k, v in (T.get("META") or {}).items() if k != "GAMEUID"}
    if meta:
        out["META"] = meta
    extra = set(T) - {"UID", "TITLE", "COVER", "META"}
    if extra:
        raise Fail(f"TILE fields {sorted(extra)}")
    return out


def transform(h, n, nodes, grafts, report):
    where = f"{n.get('LABEL')} ({h[:16]})"
    if "WHEN" in n:
        raise Fail(f"{where}: node-level WHEN (none expected; AND it onto the node's own layers if one appears)")
    layers = []
    for r in n.get("OVER", []):
        if isinstance(r, str):
            if h in grafts and r in grafts[h]["targets"]:
                continue                                           # a graft's requirement: the variant now contains it
            layers.append({"NODE": r})
        elif isinstance(r, list):
            if h not in grafts:
                raise Fail(f"{where}: any-of outside a graft")
            if grafts[h]["kept"]:
                layers.append({"ANY": list(r)})                    # a graft: what it applies onto
            continue
        elif isinstance(r, dict) and set(r) == {"NOT"}:
            layers.append({"NOT": r["NOT"]})
        else:
            raise Fail(f"{where}: OVER entry {r!r}")
    for L in n.get("LAYERS", []):
        layers.append(content_layer(L, where, report))
    for E in n.get("PATCHES", []):
        layers.append(edit_layer(E, where, "PATCHES", report))
    for E in n.get("FILEEDITS", []):
        layers.append(edit_layer(E, where, "FILEEDITS", report))
    for E in n.get("REGEDITS", []):
        layers.append(reg_layer(E, where, report))
    if n.get("DLLOVERRIDES"):
        layers.append({"DLL": dict(n["DLLOVERRIDES"])})
    if n.get("VARS"):
        decls = {}
        for V in n["VARS"]:
            k = V["KEY"]
            if k in decls:
                raise Fail(f"{where}: VARS key {k} declared twice in one node")
            decls[k] = {kk: copy.deepcopy(vv) for kk, vv in V.items() if kk != "KEY"}
        layers.append({"VARS": decls})
    if n.get("PERSISTS"):
        layers.append(keep_layer(n["PERSISTS"], where))
    if "ENV" in n or "ENV_REMOVE" in n:
        env = {k: None for k in n.get("ENV_REMOVE", [])}
        env.update(n.get("ENV", {}))                               # gen 5: removes first, then sets
        layers.append({"ENV": env})
    entries = [exec_entry(E, where) for E in n.get("ENTRYPOINTS", [])]
    if len({e["LABEL"] for e in entries}) != len(entries):
        raise Fail(f"{where}: two entries with one label after relabelling")
    if "TILE" in n:
        t = new_tile(n["TILE"], tile_uids(n["TILE"])[1])       # PARENTUID = the family (the base game)
        play = [e for e in entries if e["LABEL"] == PLAY]
        if play:
            play[0]["TILE"] = t
        else:
            entries.append({"LABEL": PLAY, "TILE": t})
    if entries:
        layers.append({"EXEC": entries})
    out = {"CID": h, "LABEL": n.get("LABEL", "")}
    for k in ("COMMENT", "POS", "VARIANT"):
        if k in n:
            out[k] = n[k]
    out["LAYERS"] = layers
    return out


def migrate(nodes, report, paths=None):
    """nodes: handle -> gen-5 node. Returns handle -> gen-6 node."""
    variants = {h for h, n in nodes.items() if n.get("VARIANT")}
    referenced = {r for n in nodes.values() for x in n.get("OVER", []) for r in ([x] if isinstance(x, str) else x if isinstance(x, list) else [])}
    # grafts: every TOGGLE'd node. Targets = its any-of members + the VARIANTs it names bare.
    grafts = {}
    for h, n in nodes.items():
        if "TOGGLE" not in n:
            continue
        if h in referenced:
            raise Fail(f"TOGGLE'd node {n.get('LABEL')} is inside a closure — not a graft")
        targets = []
        for x in n.get("OVER", []):
            if isinstance(x, list):
                targets += x
            elif isinstance(x, str) and x in variants:
                targets.append(x)
        if not targets:
            raise Fail(f"graft {n.get('LABEL')} names no variant")
        # an any-of graft (Wipeout's patches) stays a graft; one naming a single variant is that variant's own option
        grafts[h] = {"targets": targets, "on": n["TOGGLE"] == "on", "key": n.get("LABEL"),
                     "kept": any(isinstance(x, list) for x in n.get("OVER", []))}
    out = {h: transform(h, n, nodes, grafts, report) for h, n in nodes.items()}

    # Inside what only a graft reaches, a ref to the variant it applies to is dropped: an option is contained by that
    # variant (the ref would be a cycle — nfsu2_asiloader named nfsu2_vanilla_plus), and a kept graft's anchor ANY
    # already requires it. Refs to anything else the variant contains stay NODE: they are held where the variant
    # placed them (gen 5 brought only what the base lacked). A leading ANY stays reserved for a graft's anchor.
    closures = {}
    def clo(h):
        if h not in closures:
            closures[h] = closure(nodes, h)
        return closures[h]
    reached_by_launch = set()
    for v in variants:
        reached_by_launch |= clo(v)
    for g, info in grafts.items():
        tset = [clo(t) for t in info["targets"]]
        private = {g} | {x for x in closure(nodes, g, lambda n: [r for r in bare(n) if r not in info["targets"]])
                         if not any(x in c for c in tset) and x not in reached_by_launch}
        for p in private:
            keep = []
            for L in out[p]["LAYERS"]:
                if L.get("NODE") in info["targets"]:
                    report["graft_refs_to_variant_dropped"].append(f"{nodes[p].get('LABEL')}: {nodes[L['NODE']].get('LABEL')}")
                    continue
                keep.append(L)
            out[p]["LAYERS"] = keep

    # tiles → RECOMMENDED lists; the face each variant presents (for the invariant)
    faces = {}
    for h in variants:
        f = face_of(nodes, h)
        if f is None:
            raise Fail(f"variant {nodes[h].get('LABEL')} has no face")
        faces[h] = tile_uids(nodes[f]["TILE"])[0]
        if nodes[h].get("RECOMMENDED"):
            out[h]["RECOMMENDED"] = [faces[h]]
    for h, n in nodes.items():
        if n.get("RECOMMENDED") and h not in variants:
            raise Fail(f"RECOMMENDED on a non-variant {n.get('LABEL')}")
    # a kept graft shipped pre-ticked (TOGGLE on) is RECOMMENDED under the tiles of the variants it applies onto
    for g, info in grafts.items():
        if info["kept"]:
            if out[g]["LAYERS"][0].get("ANY") is None:
                raise Fail(f"graft {info['key']}: its list does not begin with ANY")
            if info["on"]:
                out[g]["RECOMMENDED"] = sorted({faces[t] for t in info["targets"]})
            report["grafts_kept"].append(f"{info['key']}: ANY {len(info['targets'])} variant(s), "
                                         + ("pre-ticked" if info["on"] else "not pre-ticked"))

    # grafts → the variants' own options (bool var + gated NODE), in gen 5's graft order (label, then key)
    per_variant = {}
    for g, info in grafts.items():
        if info["kept"]:
            continue
        for v in info["targets"]:
            per_variant.setdefault(v, []).append(g)
    for v, gs in per_variant.items():
        gs.sort(key=lambda g: (nodes[g].get("LABEL", ""), g))
        decl = {grafts[g]["key"]: {"DEFAULT": "1" if grafts[g]["on"] else "0",
                                  "UI": {"LABEL": grafts[g]["key"], "CONTROL": "bool"}} for g in gs}
        L = out[v]["LAYERS"]
        at = next((i for i, x in enumerate(L) if "EXEC" in x), len(L))   # before the entry, after the payload
        L[at:at] = [{"VARS": decl}] + [{"NODE": g, "WHEN": f"%{grafts[g]['key']}%==1"} for g in gs]
        report["toggles"].append(f"{nodes[v].get('LABEL')}: " + ", ".join(f"{grafts[g]['key']}={'on' if grafts[g]['on'] else 'off'}" for g in gs))
    canonicalize(out, report)
    take_back_overrides(out, variants, report)
    place_edits(out, variants, paths or {}, report,
                {v: sorted((g for g, i in grafts.items() if i["kept"] and v in i["targets"]), key=lambda g: (nodes[g].get("LABEL", ""), g))
                 for v in variants})
    check(nodes, out, faces, [g for g, i in grafts.items() if i["kept"]])
    return out


# ---------------------------------------------------------------- canonical guest coordinates (§1.3)
# A wine prefix's layout, as the runner that generates one now declares it (GUEST_ROOTS on its entry). Packages
# write drive-anchored paths; the runner maps them. Longest anchor first.
PREFIX_ROOTS = {"%UserProfile%": "%PrefixRoot%/drive_c/users/steamuser", "C:": "%PrefixRoot%/drive_c"}
SPELLINGS = [("%PrefixRoot%/drive_c/users/steamuser", "%UserProfile%"), ("pfx/drive_c/users/steamuser", "%UserProfile%"),
             ("%PrefixRoot%/drive_c", "C:"), ("pfx/drive_c", "C:")]


def canon(p):
    for old, new in SPELLINGS:
        if p == old or p.startswith(old + "/"):
            return new + p[len(old):]
    return p


def canonicalize(out, report):
    """Every package path → guest coordinates. Runner nodes keep their own layout (they define it); a
    prefix-generating runner declares the mapping instead."""
    runners = {h for h, n in out.items() if any(e.get("GUEST") for L in n["LAYERS"] for e in L.get("EXEC", []))}
    def fx(ns_addr):
        ns, _, p = ns_addr.partition("/")
        return ns + "/" + canon(p) if ns == "FILES" and p else ns_addr
    n_changed = 0
    for h, n in out.items():
        for L in n["LAYERS"]:
            for e in L.get("EXEC", []):
                if e.get("GUEST") and e.get("PREFIX_GENERATE"):
                    e["GUEST_ROOTS"] = dict(PREFIX_ROOTS)
            if h in runners:
                continue
            before = json.dumps(L, sort_keys=True)
            if "TARGET" in L:
                L["TARGET"] = fx(L["TARGET"])
            if L.get("SUBMOUNTS"):
                L["SUBMOUNTS"] = [src + ":" + canon(dst) for src, _, dst in (x.partition(":") for x in L["SUBMOUNTS"])]
            for e in L.get("EXEC", []):
                for f in ("EXE", "WORKDIR"):
                    if f in e:
                        e[f] = canon(e[f])
            if "KEEP" in L:
                L["KEEP"] = {fx(a): v for a, v in L["KEEP"].items()}
            for k, d in L.get("VARS", {}).items():
                if isinstance(d.get("DEFAULT"), str) and canon(d["DEFAULT"]) != d["DEFAULT"]:
                    d["DEFAULT"] = canon(d["DEFAULT"])                  # a path-valued default (asi_dir, DirectPlayPath)
            n_changed += before != json.dumps(L, sort_keys=True)
    report["canonical_layers"] += [n_changed]


# ---------------------------------------------------------------- overrides → ownership (§6 step 2)
def take_back_overrides(out, variants, report):
    """An OVERRIDE edit re-applied after the user's saved state was restored. Ownership says the same thing: where
    the edited address lies inside something a variant KEEPs, a KEEP false beside the edit takes it back (whole
    files for Overwrite/AppendLine/binary ops, #keys for ConfigWrite-only edits). Elsewhere it was a pass-order
    artefact and is just a layer."""
    import resolve as R6
    res = R6.Resolver({h: (n, "") for h, n in out.items()})
    kept = {}                                              # node -> the KEEP-true addresses of variants containing it
    for v in sorted(variants):
        plan = res.resolve(v)
        trues = [a for a, val in plan["keep"].items() if val is not False]
        for x in plan["order"]:
            kept.setdefault(x, set()).update(trues)
    for h, n in out.items():
        L = n["LAYERS"]
        i = 0
        while i < len(L):
            lay = L[i]
            if not lay.pop("__override__", False):
                i += 1; continue
            f = lay["TARGET"]
            inside = [k for k in kept.get(h, ()) if f == k.rstrip("/") or f.startswith(k.rstrip("/") + "/")]
            if inside:
                ops = lay["EDIT"]
                if all(o.get("MODE") == "ConfigWrite" for o in ops):
                    addrs = {f"{f}#{o['KEY']}": False for o in ops}
                else:
                    addrs = {f: False}
                L.insert(i + 1, {"KEEP": addrs})
                report["overrides_taken_back"].append(f"{n['LABEL']}: {', '.join(addrs)} (inside {inside[0]})")
                i += 1
            i += 1


# ---------------------------------------------------------------- edit placement (§6 step 1)
def place_edits(out, variants, paths, report, grafts_of=None):
    """gen 5 applied every base edit above all content; gen 6 applies it in place. An edit some later content would
    cover moves into a node of its own that each variant containing the edit's node places on top (before its
    entry). Repeated until nothing is covered (a moved edit is checked again)."""
    import resolve as R6
    for rnd in range(4):
        nodes6 = {h: (n, os.path.dirname(paths[h]) if h in paths else "") for h, n in out.items()}
        res = R6.Resolver(nodes6)
        covered, users = {}, {}
        for v in sorted(variants):
            opts = {k: "1" for L in out[v]["LAYERS"] for k, d in L.get("VARS", {}).items() if (d.get("UI") or {}).get("CONTROL") == "bool"}
            for inst, gs in (({}, []), (opts, (grafts_of or {}).get(v, []))):
                plan = res.resolve(v, inst, grafts=gs)
                for x in plan["order"]:
                    users.setdefault(x, set()).add(v)
                seq = plan["seq"]
                for i, it in enumerate(seq):
                    wv = plan["when_vars"]                       # the defaults; unknown built-ins stay literal
                    if it["kind"] == "EDIT" and any(R6.covers(seq, j, R6.subst(it["target"], wv), wv)
                                                    for j in range(i + 1, len(seq)) if seq[j]["kind"] != "EDIT"):
                        covered.setdefault(it["from"], set()).add(it["at"])
        if not covered:
            return
        for x, ats in covered.items():
            moved = [out[x]["LAYERS"][i] for i in sorted(ats)]
            out[x]["LAYERS"] = [L for i, L in enumerate(out[x]["LAYERS"]) if i not in ats]
            e = {"LABEL": out[x]["LABEL"] + "_edits", "LAYERS": moved}
            h = R6.cid_of(R6.canonical(e))
            e = {"CID": h, **e}
            out[h] = e
            if x in paths:
                paths[h] = os.path.splitext(paths[x])[0] + "_edits.json"
            for v in sorted(users.get(x, ())):
                L = out[v]["LAYERS"]
                at = next((i for i, y in enumerate(L) if "EXEC" in y), len(L))
                L.insert(at, {"NODE": h})
            report["edits_moved"].append(f"{out[x]['LABEL']}: {len(moved)} EDIT layer(s) → {h[:16]} on top of {len(users.get(x, ()))} variant(s): "
                                         + ", ".join(m["TARGET"] for m in moved))
    raise Fail("edit placement did not settle: " + "; ".join(report["edits_moved"][-6:]))


# ---------------------------------------------------------------- invariants
def sources(n):
    out = set()
    def walk(x):
        if isinstance(x, dict):
            for k, v in x.items():
                if k == "SOURCE":
                    out.add(v["CID"] if isinstance(v, dict) else v)
                else:
                    walk(v)
        elif isinstance(x, list):
            for v in x:
                walk(v)
    walk(n)
    return out


def check(old, new, faces, grafts_kept=()):
    if not set(old) <= set(new):
        raise Fail("a node disappeared")
    for h in set(new) - set(old):
        if set(new[h]) != {"CID", "LABEL", "LAYERS"} or any("EDIT" not in L for L in new[h]["LAYERS"]):
            raise Fail(f"new node {h} is not an edits-only node")
    for h, n in new.items():
        if set(n) - NEW_FIELDS:
            raise Fail(f"{n['LABEL']}: old fields left {sorted(set(n) - NEW_FIELDS)}")
        for L in n["LAYERS"]:
            ks = [k for k in TYPE_KEYS if k in L]
            if len(ks) != 1:
                raise Fail(f"{n['LABEL']}: a layer with type keys {ks}")
            if "NODE" in L and L["NODE"] not in new:
                raise Fail(f"{n['LABEL']}: NODE {L['NODE']} does not resolve")
        if h in old and sources(old[h]) != sources(n):
            raise Fail(f"{n['LABEL']}: content CIDs changed")
    anchored = {h for h, n in new.items() if n["LAYERS"] and "ANY" in n["LAYERS"][0]}
    if anchored != set(grafts_kept):
        raise Fail(f"leading ANY (= a graft) on {sorted(new[h]['LABEL'] for h in anchored ^ set(grafts_kept))}")
    # every variant presents its gen-5 face: the TILE on the nearest Play entry beneath (own counts)
    for h, uid in faces.items():
        q, seen, got = deque([h]), {h}, None
        while q and got is None:
            x = q.popleft()
            for L in new[x]["LAYERS"]:
                for e in L.get("EXEC", []):
                    if e["LABEL"] == PLAY and "TILE" in e:
                        got = e["TILE"]["UID"]
            for L in new[x]["LAYERS"]:
                if "NODE" in L and "WHEN" not in L and L["NODE"] not in seen:
                    seen.add(L["NODE"]); q.append(L["NODE"])
        if got != uid:
            raise Fail(f"{new[h]['LABEL']}: presents {got}, gen 5 face {uid}")


# ---------------------------------------------------------------- IO
def canonical_pretty(n):
    return json.dumps(n, indent=4, ensure_ascii=False) + "\n"


def run(root, dry, report_path):
    gathered = gather(root)
    nodes = {h: v[0] for h, v in gathered.items()}
    report = new_report()
    paths = {h: p for h, (_, p) in gathered.items()}
    out = migrate(nodes, report, paths)
    print(f"{len(nodes)} nodes transformed, {len(out) - len(nodes)} edits node(s) added; "
          + "; ".join(f"{k} {sum(v) if k == 'canonical_layers' else len(v)}" for k, v in report.items()))
    if report_path:
        json.dump(report, open(report_path, "w"), indent=2)
    if dry:
        return 0
    ts = time.strftime("%Y%m%d-%H%M%S")
    backup = os.path.abspath(root.rstrip("/")) + f".pre-gen6-{ts}.tar"
    with tarfile.open(backup, "w") as tf:
        for h, (_, p) in sorted(gathered.items()):
            tf.add(p, arcname=os.path.relpath(p, os.path.dirname(os.path.abspath(root.rstrip("/")))))
    print("backup:", backup)
    for h, n in out.items():
        p = paths[h]
        if h not in gathered and os.path.exists(p):
            raise Fail(f"{p} exists — refusing to overwrite it with a new node")
        tmp = p + ".gen6tmp"
        open(tmp, "w").write(canonical_pretty(n))
        os.replace(tmp, p)
    return 0


# ---------------------------------------------------------------- self-test
def self_test():
    H = lambda s: "bagu" + s
    nodes = {
        H("base"): {"CID": H("base"), "LABEL": "base",
                    "LAYERS": [{"FORM": "zip", "PATH": "g.zip", "TARGET": "%PrefixRoot%/drive_c/%PackageUID%",
                                "SOURCE": {"TYPE": "ipfs", "CID": "QmG", "SIZE": 5}}],
                    "FILEEDITS": [{"FILE": "%PrefixRoot%/drive_c/%PackageUID%/a.cfg", "OVERRIDE": True,
                                   "EDITS": [{"MODE": "ConfigWrite", "KEY": "/W:", "VALUE": "%ScreenWidth%"}]}],
                    "TILE": {"UID": "7", "TITLE": "Game", "COVER": {"PATH": "c.png", "SOURCE": {"TYPE": "ipfs", "CID": "QmC"}},
                             "META": {"GAMEUID": "7", "TGDBID": "7"}}},
        H("child"): {"CID": H("child"), "LABEL": "child", "OVER": [H("base")],
                     "TILE": {"UID": "7", "TITLE": "Game: Addon", "META": {"GAMEUID": "70", "TGDBID": "70"}}},
        H("v1"): {"CID": H("v1"), "LABEL": "v1", "OVER": [H("base")], "VARIANT": "v1", "RECOMMENDED": True,
                  "ENTRYPOINTS": [{"LABEL": "v1", "HOST": "win32", "PATH": "%PrefixRoot%/drive_c/%PackageUID%/g.exe", "ARGS": []}],
                  "ENV": {"A": "1"}, "ENV_REMOVE": ["B"], "VARS": [{"KEY": "k", "DEFAULT": "x", "WHEN": "%y%==1"}]},
        H("v2"): {"CID": H("v2"), "LABEL": "v2", "OVER": [H("child")], "VARIANT": "v2",
                  "ENTRYPOINTS": [{"LABEL": "v2", "HOST": "win32", "ARGS": ["-x"]}]},
        H("lib"): {"CID": H("lib"), "LABEL": "lib", "DLLOVERRIDES": {"d3d8": "n,b"}},
        H("mod"): {"CID": H("mod"), "LABEL": "mod", "TOGGLE": "on", "OVER": [[H("v1"), H("v2")], H("lib")],
                   "PATCHES": [{"FILE": "%PrefixRoot%/drive_c/%PackageUID%/g.exe", "EDITS": [{"MODE": "Poke", "OFFSET": "0x10", "VALUE": "00"}]}]},
        H("opt"): {"CID": H("opt"), "LABEL": "opt", "TOGGLE": "off", "OVER": [H("v1"), H("lib")],
                   "LAYERS": [{"FORM": "zip", "PATH": "o.zip", "TARGET": "%PrefixRoot%/drive_c/%PackageUID%/opt"}]},
    }
    report = new_report()
    out = migrate(copy.deepcopy(nodes), report)
    base = out[H("base")]["LAYERS"]
    assert base[0] == {"ZIP": "g.zip", "SOURCE": "QmG", "SIZE": 5, "TARGET": "FILES/C:/%PackageUID%"}, base[0]
    assert base[1]["TARGET"] == "FILES/C:/%PackageUID%/a.cfg" and "OVERRIDE" not in base[1]
    assert base[2]["EXEC"] == [{"LABEL": "Play", "TILE": {"UID": "7", "TITLE": "Game", "COVER": {"FILE": "c.png", "SOURCE": "QmC"},
                                                          "META": {"TGDBID": "7"}}}], base[2]
    assert out[H("child")]["LAYERS"][1]["EXEC"][0]["TILE"]["PARENTUID"] == "7"
    assert out[H("child")]["LAYERS"][1]["EXEC"][0]["TILE"]["UID"] == "70"
    v1 = out[H("v1")]
    assert v1["RECOMMENDED"] == ["7"] and v1["LAYERS"][0] == {"NODE": H("base")}
    assert v1["LAYERS"][1] == {"VARS": {"k": {"DEFAULT": "x", "WHEN": "%y%==1"}}}
    assert v1["LAYERS"][2] == {"ENV": {"B": None, "A": "1"}}
    assert v1["LAYERS"][3] == {"VARS": {"opt": {"DEFAULT": "0", "UI": {"LABEL": "opt", "CONTROL": "bool"}}}}, v1["LAYERS"][3]
    assert v1["LAYERS"][4] == {"NODE": H("opt"), "WHEN": "%opt%==1"}
    assert v1["LAYERS"][5]["EXEC"] == [{"LABEL": "Play", "HOST": "win32", "EXE": "C:/%PackageUID%/g.exe", "ARGS": []}]
    assert canon("pfx/drive_c/users/steamuser/Saved Games/x") == "%UserProfile%/Saved Games/x"
    assert canon("%PrefixRoot%/drive_cx") == "%PrefixRoot%/drive_cx"          # a prefix of a name is not a path prefix
    assert "RECOMMENDED" not in out[H("v2")] and out[H("v2")]["LAYERS"][-1]["EXEC"][0]["LABEL"] == "Play"
    mod = out[H("mod")]                     # an any-of graft stays a graft: ANY first, pre-ticked = RECOMMENDED
    assert mod["LAYERS"][0] == {"ANY": [H("v1"), H("v2")]} and mod["LAYERS"][1] == {"NODE": H("lib")}, mod
    assert mod["RECOMMENDED"] == ["70", "7"] or mod["RECOMMENDED"] == sorted(["7", "70"]), mod
    assert out[H("opt")]["LAYERS"][0] == {"NODE": H("lib")} and "RECOMMENDED" not in out[H("opt")]
    assert not any(L.get("NODE") == H("mod") for L in out[H("v2")]["LAYERS"])
    assert report["overrides_dropped"] and report["toggles"]
    # refusals
    bad = copy.deepcopy(nodes); bad[H("v1")]["OVER"].append(H("mod"))
    try:
        migrate(bad, report); raise AssertionError("a TOGGLE inside a closure must refuse")
    except Fail:
        pass
    bad = copy.deepcopy(nodes); bad[H("v2")]["OVER"] = []
    try:
        migrate(bad, report); raise AssertionError("a variant without a face must refuse")
    except Fail:
        pass
    print("self-test OK")
    return 0


if __name__ == "__main__":
    a = sys.argv[1:]
    if a == ["--self-test"]:
        sys.exit(self_test())
    if not a:
        print(__doc__); sys.exit(2)
    rp = a[a.index("--report") + 1] if "--report" in a else None
    try:
        sys.exit(run(a[0], "--dry-run" in a, rp))
    except Fail as e:
        print("REFUSED:", e); sys.exit(1)
