#!/usr/bin/env python3
"""The generation-6 equivalence gate: gen 5's resolved launches (gen5_dumps.py) vs the gen-6 reference resolver
(resolve.py) over the migrated library. Both sides are normalised to the same effective state and compared per
category. Every difference must be on the allow-list of the migration step being gated; anything else blocks.

Per launchable (every VARIANT node, the runner found by the chain as in a real launch):
  content   the mount stack: (kind, source, target, submounts), bottom → top
  edits     per file, the ops in the order they apply (gen 5: base FileEdits, then OVERRIDE ones, then BinaryPatch —
            all above every content layer; gen 6: in fold order) + gen 6's covered-edit check
  reg/dll   the folded registry values / created keys and DLL overrides
  env       the game's folded environment
  exec      the launched entry (platform, exe, args, workdir) and the number of game entries offered
  face      the tile the variant presents (title, family = %PackageUID%)
  vars      every variable's resolved value
  keep      what persists
Per runner root (each resolved on its own node): content, edits, reg, dll, env, exec, variable declarations.

Usage: equivalence.py <gen5-work-dir> <gen6-library> [--allow STEP] [--show N] [--only CID] [--json OUT]
"""
import argparse, glob, json, os, sys
from collections import defaultdict
from resolve import Resolver, load_nodes, subst, subst_json, when, resolve_vars, covers, provided, to_layout, graft_index, offered_grafts

VFS = {"VFSZipLayer": "ZIP", "VFSDeltaLayer": "DELTA", "VFSDirLayer": "DIR", "VFSFileLayer": "FILE"}
EDITS = ("FileEdit", "BinaryPatch")
SESSION = ("VIDYAGOD_",)

# Intended differences, per migration step. Each entry: category -> reason. A category listed here is reported but
# does not block. Keep this list short and specific; widen it only with the user's agreement.
ALLOW = {
    "generic": {
        "vars-new-option": "§6 step 4: a TOGGLE'd graft is now the variant's own bool option (default = its TOGGLE)",
        "content-order": "the same layers in another order with every path's winner unchanged (§4.2 occurrences)",
        "shelf-order": "a tile's child faces all hang off the base game (PARENTUID = family) and sort by title; gen 5 "
                       "ordered them by how deep their content was built (user, 2026-09-25)",
    },
}


def rel(p, root):
    if p and "%" in p:                     # runtime-sourced: gen 5's AbsLayers glued a bundle dir in front of it
        return p[p.index("%"):]
    if p and "/LIBRARY/" in p:             # whichever data dir (a worker's) the library was read through
        return p[p.rindex("/LIBRARY/") + len("/LIBRARY/"):]
    return os.path.relpath(p, root) if p and os.path.isabs(p) else p


def cid_or(src, path, root):
    if isinstance(src, dict):
        src = src.get("CID")
    return src or rel(path, root)


# ---------------------------------------------------------------- gen 5
def g5_split(d):
    """SubComponentsArray = [runner prefix VFS] + [game] + [runner edits]; the counts come from RunnerComponents,
    exactly as InitializeFromNode builds them."""
    rc = d.get("RunnerComponents") or []
    want = set(d.get("RunnerRecipe") or [])
    pre = tail = 0
    for c in rc:
        for L in c.get("SUBCOMPONENTS", []):
            t = L.get("TYPE")
            if t in VFS and "%" in str(L.get("PATH", "")):
                pre += 1
            if (not want or c.get("COMPONENTID") in want) and t in ("FileEdit", "RegEdit", "DllOverride", "BinaryPatch"):
                if "WHEN" in L and not when(L["WHEN"], d["Variables"]):
                    continue
                tail += 1
    if d.get("UnifiedRuntime"):
        # the runner build is folded into the game's components, beneath the game (lowest priority)
        for c in rc:
            if want and c.get("COMPONENTID") not in want:
                continue
            for L in c.get("SUBCOMPONENTS", []):
                if L.get("TYPE") in ("CustomVar", "DeclarePersist", "DeclareExec", "DeclareLibraryItem", "DeclareRunner"):
                    continue
                if "WHEN" in L and not when(L["WHEN"], d["Variables"]):
                    continue
                pre += 1
    S = d["SubComponentsArray"]
    return S[:pre], S[pre:len(S) - tail], S[len(S) - tail:]


def g5_state(items, root, vars_=None):
    """Normalise lowered layers (substituted or not) into the comparable state."""
    content, base, over, bp = [], defaultdict(list), defaultdict(list), defaultdict(list)
    regb, rego, keysb, dll = [], [], set(), {}
    for L in items:
        t = L.get("TYPE")
        if t in VFS:
            content.append((VFS[t], cid_or(L.get("SOURCE"), L.get("PATH"), root), L.get("TARGET", ""),
                            json.dumps(L.get("SUBMOUNTS"), sort_keys=True) if L.get("SUBMOUNTS") else ""))
        elif t == "FileEdit":
            op = {k: v for k, v in L.items() if k not in ("TYPE", "FILE", "OVERRIDE", "WHEN")}
            (over if L.get("OVERRIDE") else base)[L["FILE"]].append(op)
        elif t == "BinaryPatch":
            bp[L["FILE"]].append({k: v for k, v in L.items() if k not in ("TYPE", "FILE", "WHEN")})
        elif t == "RegEdit":
            (rego if L.get("OVERRIDE") else regb).append(L)
        elif t == "DllOverride":
            n, _, o = L["DLLOVERRIDE"].partition("=")
            dll.pop(n.lower(), None); dll[n.lower()] = o
    reg = {}
    for L in regb + rego:
        arch = L.get("ARCHITECTURE")
        kv = L.get("KEYVALUES") or {}
        if not kv:
            keysb.add((arch, L["REGPATH"].lower()))
        for k, v in kv.items():
            reg[(arch, L["REGPATH"].lower(), k.lower())] = "" if v is None else v   # RegValueFromJson: null → ""
    edits = {}
    for f in set(base) | set(over) | set(bp):
        edits[f] = base.get(f, []) + over.get(f, []) + bp.get(f, [])
    return {"content": content, "edits": edits, "reg": reg, "regkeys": keysb, "dll": dll}


# ---------------------------------------------------------------- gen 6
def g6_state(plan, root, vars_, roots=None):
    """gen 6's plan in the launching runner's layout: every path field is substituted, then mapped from guest
    coordinates through the runner's GUEST_ROOTS (vars_ None = compare raw, as for a runner's own build)."""
    S = (lambda v: subst_json(v, vars_)) if vars_ is not None else (lambda v: v)
    P = lambda p: to_layout(S(p), roots) if roots else S(p)
    seq = []
    for it in plan["seq"]:
        it = dict(it, target=P(it["target"]))
        if it.get("submounts"):
            it["submounts"] = [src + ":" + P(dst) for src, _, dst in (S(x).partition(":") for x in it["submounts"])]
        seq.append(it)
    content, edits, events = [], defaultdict(list), []
    for i, it in enumerate(seq):
        if it["kind"] == "EDIT":
            f = it["target"]
            edits[f] += [S(op) for op in it["ops"]]
            for j in range(i + 1, len(seq)):                       # gen 6: a later content layer may cover it
                c = seq[j]
                if c["kind"] != "EDIT" and covers(seq, j, f):
                    events.append(f"edit on {f} beneath {c['kind']} {c['payload']} at {c['target'] or '/'}")
        else:
            p = it["payload"]
            path = p if p.startswith("%") else os.path.join(it["dir"], p)
            content.append((it["kind"], it["source"] or rel(path, root), it["target"],
                            json.dumps(it["submounts"], sort_keys=True) if it.get("submounts") else ""))
    reg, keys = {}, set()
    for (arch, p, n), (dp, dn, v) in plan["reg"].items():
        reg[(arch, S(dp).lower(), S(dn).lower())] = S(v)
    for (arch, p), dp in plan["regkeys"].items():
        keys.add((arch, S(dp).lower()))
    return {"content": content, "items": [it for it in seq if it["kind"] != "EDIT"], "vars": None,
            "edits": dict(edits), "reg": reg, "regkeys": keys,
            "dll": {S(k).lower(): S(v) for k, v in plan["dll"].items()},
            "covered": events}


def empty_keys(plan_regkeys, reg, vars_):
    """gen 6 records every key it walked; gen 5 only keys written with no values. Keep the value-less ones."""
    with_vals = {(a, p) for (a, p, n) in reg}
    return {k for k in plan_regkeys if k not in with_vals and not any(v[0] == k[0] and v[1].startswith(k[1] + "\\") for v in with_vals)}


# ---------------------------------------------------------------- compare
def first_diff(a, b):
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return f"@{i}: gen5 {x} | gen6 {y}"
    return f"length gen5 {len(a)} gen6 {len(b)}; extra " + str((a[len(b):] or b[len(a):])[:2])


def dict_diff(a, b, n=4):
    ks = sorted(set(a) | set(b), key=str)
    out = [f"{k}: gen5 {a.get(k, '∅')!r} | gen6 {b.get(k, '∅')!r}" for k in ks if a.get(k, "∅") != b.get(k, "∅")]
    return "; ".join(out[:n]) + (f" (+{len(out) - n})" if len(out) > n else "") if out else None


def winners(items, vars_):
    """path -> the content item that wins it, bottom → top. None when some item's files cannot be enumerated."""
    win = {}
    for j, it in enumerate(items):
        p = provided(items, j, vars_)
        if p is None:
            return None
        for f in p:
            win[f] = (it["kind"], it["source"] or it["payload"], it["target"])
    return win


def compare_state(g5, g6, diffs, pre=""):
    if g5["content"] != g6["content"]:
        same_set = sorted(g5["content"]) == sorted(g6["content"])
        w6 = w5 = None
        if same_set and g6.get("items") is not None:
            # the same layers in another order: equivalent iff every path keeps its winner
            by = {}
            for tup, it in zip(g6["content"], g6["items"]):
                by.setdefault(tup, []).append(it)
            items5 = [by[t].pop(0) for t in g5["content"]]
            w5, w6 = winners(items5, g6["vars"]), winners(g6["items"], g6["vars"])
        if w5 is not None and w5 == w6:
            diffs[pre + "content-order"] = "same winners; " + first_diff(g5["content"], g6["content"])
        elif w5 is not None:
            changed = sorted(f for f in set(w5) | set(w6) if w5.get(f) != w6.get(f))
            diffs[pre + "content"] = f"{len(changed)} path(s) change winner, e.g. " + "; ".join(
                f"{f}: gen5 {w5.get(f)} gen6 {w6.get(f)}" for f in changed[:3])
        else:
            diffs[pre + "content"] = first_diff(g5["content"], g6["content"])
    d = dict_diff(g5["edits"], g6["edits"])
    if d:
        diffs[pre + "edits"] = d
    if g6.get("covered"):
        diffs[pre + "edit-covered"] = "; ".join(g6["covered"][:3])
    d = dict_diff(g5["reg"], g6["reg"])
    if d:
        diffs[pre + "reg"] = d
    if g5["regkeys"] != g6["regkeys"]:
        diffs[pre + "regkeys"] = f"gen5-only {sorted(g5['regkeys'] - g6['regkeys'])[:3]} gen6-only {sorted(g6['regkeys'] - g5['regkeys'])[:3]}"
    d = dict_diff(g5["dll"], g6["dll"])
    if d:
        diffs[pre + "dll"] = d


PARENT = {}                                                # tile UID -> PARENTUID, over the whole library
GRAFTS, GIDX = set(), {}                                   # graft CIDs; member -> grafts (ANY index)


def pooled(decls):
    return {k for k, d in decls.items() if (d.get("UI") or {}).get("CONTROL") == "secret" and (d.get("UI") or {}).get("POOL")}


def check_game(cid, d, R, g6root, g5root, runner_plans, config=None):
    """config: a non-default gen-5 launch ({"modules": {graft: on}, "vars": {K: V}}) → the same choices as gen-6
    instance values: a graft's on/off is its bool option (keyed by its LABEL), vars pass through."""
    diffs = {}
    chosen, picked = {}, None
    if config:
        picked = []
        for g, on in config["modules"].items():
            if g in GRAFTS:                                  # a graft: ticked = in the instance's graft list
                if on:
                    picked.append(g)
            else:                                            # a variant's own option: its bool variable
                chosen[R.nodes[g][0]["LABEL"]] = "1" if on else "0"
        chosen.update(config["vars"])
    custom = set(d["CustomVariables"])
    builtins = {k: v for k, v in d["Variables"].items() if k not in custom}
    rid = d["RunnerChain"][[i for i, l in enumerate(d["RunnerChain"]) if not l["Native"]][-1] if any(not l["Native"] for l in d["RunnerChain"]) else 0]["NodeId"]
    if rid not in runner_plans:
        runner_plans[rid] = R.resolve(rid, {}, builtins) if rid in R.nodes else None
    rplan = runner_plans[rid]
    # the pool draw is instance data: give gen 6 the same draw gen 5 made
    plan0 = R.resolve(cid, chosen, builtins)
    face = next((e["TILE"]["UID"] for e in plan0["exec"].values() if not e.get("GUEST") and e.get("TILE")), None)
    offered, ticked = offered_grafts(R.nodes, GIDX, plan0, face)
    grafts = [g for g in offered if g in picked] if picked is not None else ticked
    inst = dict(chosen, **{k: d["CustomVariables"][k] for k in pooled(plan0["decls"]) if k in d["CustomVariables"]})
    plan = R.resolve(cid, inst, builtins, grafts=grafts)
    decls = dict((rplan or {}).get("decls", {}))
    for k, v in plan["decls"].items():
        decls.pop(k, None); decls[k] = v
    sess = {k: v for k, v in d["CustomVariables"].items() if k.startswith(SESSION) and k not in decls}
    vals = resolve_vars(decls, builtins, dict(inst, **sess))
    vmap = dict(builtins); vmap.update(vals)
    rent = [e for e in (rplan or {}).get("exec", {}).values() if e.get("GUEST")]
    roots = {k: subst(v, vmap) for k, v in (rent[0].get("GUEST_ROOTS") or {}).items()} if rent else {}
    P = lambda p: to_layout(subst(p, vmap), roots)
    # vars
    g5v = {k: v for k, v in d["CustomVariables"].items()}
    options = {k for k, dcl in decls.items() if (dcl.get("UI") or {}).get("CONTROL") == "bool" and k not in g5v}
    if options:
        diffs["vars-new-option"] = ", ".join(sorted(options))
    dv = dict_diff(g5v, {k: to_layout(v, roots) for k, v in vals.items() if k not in options})
    if dv:
        diffs["vars"] = dv
    # game state
    pre, game, post = g5_split(d)
    g5 = g5_state(game, g5root)
    g6 = g6_state(plan, g6root, vmap, roots)
    g6["regkeys"] = empty_keys(g6["regkeys"], g6["reg"], vmap)
    compare_state(g5, g6, diffs)
    # env
    e5 = dict(d.get("LaunchEnv") or {})
    for k in d.get("LaunchRemoveEnv") or []:
        e5[k] = None
    dd = dict_diff(e5, plan["env"])
    if dd:
        diffs["env"] = dd
    # exec
    games = [e for e in plan["exec"].values() if not e.get("GUEST")]
    ce = d.get("ComposedExec") or {}
    if not games:
        diffs["exec"] = "no game entry"
    else:
        e = games[0]
        a = {"PLATFORM": ce.get("PLATFORM"), "CONTENTPATH": ce.get("CONTENTPATH"), "EXEARGS": ce.get("EXEARGS"), "WORKDIR": ce.get("WORKDIR")}
        # the dump's exec is unsubstituted: map guest coordinates back to the spelling gen 5 kept
        raw_roots = rent[0].get("GUEST_ROOTS") or {} if rent else {}
        b = {"PLATFORM": e.get("HOST"), "CONTENTPATH": to_layout(e["EXE"], raw_roots) if "EXE" in e else None,
             "EXEARGS": e.get("ARGS"), "WORKDIR": to_layout(e["WORKDIR"], raw_roots) if "WORKDIR" in e else None}
        dd = dict_diff(a, b)
        if dd:
            diffs["exec"] = dd
        if len(games) > 1:
            diffs["exec-count"] = f"{len(games)} game entries: {[x['LABEL'] for x in games][:4]}"
        t = e.get("TILE") or {}
        fam, seen = t.get("UID"), set()
        while PARENT.get(fam) and fam not in seen:          # %PackageUID% = the root of the PARENTUID chain
            seen.add(fam); fam = PARENT[fam]
        if t.get("TITLE") != d["GameName"] or fam != d["PackageUID"]:
            diffs["face"] = f"gen5 {d['GameName']}/{d['PackageUID']} gen6 {t.get('TITLE')}/{fam}"
    # keep
    k5 = sorted((x.get("path", "").rstrip("/"), x.get("target"), x.get("cloud")) for x in (d.get("KeepDirs") or []) + (d.get("KeepFiles") or []))
    k6 = sorted((P(a.split("/", 1)[1]).rstrip("/") if "/" in a else "", (v or {}).get("NAME") if isinstance(v, dict) else None,
                 (v or {}).get("CLOUD", True) if isinstance(v, dict) else True) for a, v in plan["keep"].items()
                if a.startswith("FILES") and v is not False)             # what persists = the user-owned addresses
    if k5 != k6:
        diffs["keep"] = f"gen5 {k5} gen6 {k6}"
    for ev in plan["events"]:
        if ev[0] in ("cycle", "missing", "any-unmet", "not-hit"):     # moves/held: judged by their effect above
            diffs.setdefault("events", "")
            diffs["events"] += f"{ev[0]} {ev[1][:20]}; "
    return diffs, rid


def g5_runner(d):
    items, decl5 = [], {}
    for c in d["RunnerComponents"]:
        for L in c.get("SUBCOMPONENTS", []):
            if L.get("TYPE") == "CustomVar":
                decl5.pop(L["KEY"], None)
                decl5[L["KEY"]] = {k: v for k, v in L.items() if k not in ("TYPE", "KEY")}
            else:
                items.append(L)
    return items, decl5


def model_matches_dump(d, m, g5root):
    """Is gen5model's runner build the one the engine dumped (same normalised state, declarations, entry, env)?"""
    (i1, d1), (i2, d2) = g5_runner(d), g5_runner(m)
    s1, s2 = g5_state(i1, g5root), g5_state(i2, g5root)
    e1 = (d["RunnerExecutable"], d["RunnerArgs"], d["PrefixGenerate"], d["UnifiedRuntime"])
    e2 = (m["RunnerExecutable"], m["RunnerArgs"], m["PrefixGenerate"], m["UnifiedRuntime"])
    env1 = dict(d["RunnerEnv"], **{k: None for k in d["RunnerRemoveEnv"]})
    env2 = dict(m["RunnerEnv"], **{k: None for k in m["RunnerRemoveEnv"]})
    return s1 == s2 and d1 == d2 and e1 == e2 and env1 == env2 and \
        subst(m["ContentRoot"], d["Variables"]) == d["ContentRoot"]


def check_runner(rid, d, R, g6root, g5root):
    """The runner build: gen 5's RunnerComponents (the runner's closure, lowered, unsubstituted) vs gen 6's fold."""
    diffs = {}
    items, decl5 = g5_runner(d)
    plan = R.resolve(rid, {}, {})
    vmap = d["Variables"]
    g5 = g5_state(items, g5root)
    g6 = g6_state(plan, g6root, None)
    g6["regkeys"] = empty_keys(g6["regkeys"], g6["reg"], {})
    compare_state(g5, g6, diffs, "runner-")
    dd = dict_diff(decl5, plan["decls"])
    if dd:
        diffs["runner-vardecls"] = dd
    ents = [e for e in plan["exec"].values() if e.get("GUEST")]
    if len(ents) != 1:
        diffs["runner-exec"] = f"{len(ents)} runner entries"
    else:
        e = ents[0]
        a = {"EXE": d["RunnerExecutable"], "ARGS": d["RunnerArgs"], "CONTENT_ROOT": d["ContentRoot"] or None,
             "PREFIX_GENERATE": d["PrefixGenerate"] or None, "UNIFIED_RUNTIME": d["UnifiedRuntime"] or None}
        b = {"EXE": e.get("EXE"), "ARGS": e.get("ARGS", []), "CONTENT_ROOT": subst(e["CONTENT_ROOT"], vmap) if e.get("CONTENT_ROOT") else None,
             "PREFIX_GENERATE": e.get("PREFIX_GENERATE") or None, "UNIFIED_RUNTIME": e.get("UNIFIED_RUNTIME") or None}
        dd = dict_diff(a, b)
        if dd:
            diffs["runner-exec"] = dd
    e5 = dict(d.get("RunnerEnv") or {})
    for k in d.get("RunnerRemoveEnv") or []:
        e5[k] = None
    dd = dict_diff(e5, plan["env"])
    if dd:
        diffs["runner-env"] = dd
    return diffs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("g5work"); ap.add_argument("g6lib")
    ap.add_argument("--allow", default="generic"); ap.add_argument("--show", type=int, default=3)
    ap.add_argument("--only"); ap.add_argument("--json")
    ap.add_argument("--record", help="append every resolve this run performs to FILE (JSONL) — fold_gate.py replays them")
    a = ap.parse_args()
    g5root = os.path.join(a.g5work, "base", "LIBRARY")
    R = Resolver(load_nodes(a.g6lib))
    if a.record:
        rec, orig = open(a.record, "w"), R.resolve
        def recording(root, instance=None, builtins=None, grafts=()):
            rec.write(json.dumps({"root": root, "instance": instance or {}, "builtins": builtins or {}, "grafts": list(grafts)}) + "\n")
            return orig(root, instance, builtins, grafts)
        R.resolve = recording
    GIDX.update(graft_index(R.nodes))
    GRAFTS.update(g for gs in GIDX.values() for g in gs)
    for n, _ in R.nodes.values():
        for L in n["LAYERS"]:
            for e in L.get("EXEC", []):
                if e.get("TILE", {}).get("PARENTUID"):
                    PARENT[e["TILE"]["UID"]] = e["TILE"]["PARENTUID"]
    allow = {}
    for step in a.allow.split(","):
        allow.update(ALLOW.get(step, {}))
    bycat, report, runner_plans, runners = defaultdict(list), {}, {}, {}
    dumps = sorted(glob.glob(os.path.join(a.g5work, "dumps", "*.json")))
    # which grafts each row offers: gen 5 offered a graft to the variants its any-of names; gen 6 to the rows whose
    # expansion holds its ANY
    import gen5model
    g5nodes = gen5model.load(g5root)
    anyof = {g: {m for x in n.get("OVER", []) if isinstance(x, list) for m in x} for g, (n, _) in g5nodes.items()}
    for f in dumps:
        cid = os.path.basename(f)[:-5]
        if "@" in cid or (a.only and cid != a.only) or cid not in R.nodes:
            continue
        want = sorted(g for g, ms in anyof.items() if cid in ms)
        got = sorted(offered_grafts(R.nodes, GIDX, R.resolve(cid, {}, {}))[0])
        if want != got:
            bycat["grafts-offered"].append((R.nodes[cid][0].get("LABEL"), f"gen5 {len(want)} gen6 {len(got)}"))
    for f in dumps:
        name = os.path.basename(f)[:-5]
        cid = name.split("@")[0]
        if a.only and cid != a.only:
            continue
        d = json.load(open(f))
        cfg = json.load(open(f[:-5] + ".cfg")) if "@" in name else None
        diffs, rid = check_game(cid, d, R, a.g6lib, g5root, runner_plans, cfg)
        if cfg:
            report[name] = diffs
            label = R.nodes[cid][0].get("LABEL") + " @" + name.split("@")[1]
            for c, why in diffs.items():
                bycat[c].append((label, why))
            continue
        # the runner's own view: the dump's RunnerEnv has the launched game's env merged in, so judge each runner
        # from a launch whose game sets no env of its own
        if not d.get("LaunchEnv") and not d.get("LaunchRemoveEnv"):
            runners.setdefault(rid, d)
        else:
            runners.setdefault(rid, None)
        label = R.nodes[cid][0].get("LABEL") if cid in R.nodes else cid
        for c, why in diffs.items():
            bycat[c].append((label, why))
        report[cid] = diffs
    for rid, d in runners.items():
        if rid not in R.nodes:
            continue
        if d is None:
            bycat["runner-unjudged"].append((R.nodes[rid][0].get("LABEL"), "every dump using it sets game env"))
            continue
        diffs = check_runner(rid, d, R, a.g6lib, g5root)
        for c, why in diffs.items():
            bycat[c].append((R.nodes[rid][0].get("LABEL"), why))
        report["runner:" + rid] = diffs
    # runner roots no launch covers (the chain never picks them by default): gen 5's build from gen5model, which
    # must first reproduce every runner build the dumps DO contain
    for rid, d in runners.items():
        if d is not None and rid in g5nodes and not model_matches_dump(d, gen5model.runner_dump(g5nodes, rid), g5root):
            bycat["runner-model"].append((R.nodes[rid][0].get("LABEL"), "gen5model does not reproduce the dumped build"))
    for rid, (n, _) in sorted(R.nodes.items()):
        if a.only or not any(e.get("GUEST") for L in n["LAYERS"] for e in L.get("EXEC", [])):
            continue
        if runners.get(rid) is not None:
            continue
        diffs = check_runner(rid, gen5model.runner_dump(g5nodes, rid), R, a.g6lib, g5root)
        for c, why in diffs.items():
            bycat[c].append((n.get("LABEL") + " (model)", why))
        report["runner:" + rid] = diffs
        runners[rid] = "model"
    # the shelf: gen 5's tiles (--list-nodes) vs gen 6's (tiles on entries, merged by UID, nested by PARENTUID)
    sp = os.path.join(a.g5work, "dumps", "shelf.list")
    if os.path.exists(sp) and not a.only:
        from resolve import shelf
        g5s = json.load(open(sp))
        g6s = shelf(R.nodes)
        lab = lambda c: R.nodes[c][0].get("LABEL")
        a5 = [(t["title"], t["uid"], t["rows"]) for t in g5s]
        a6 = [(f["title"], f["uid"], [lab(v) for face in f["faces"] for v in face["rows"]]) for f in g6s]
        def reordered_faces(rows5, fam6):
            """gen 5's rows = gen 6's face blocks (each in its own order), faces in another order."""
            blocks = [[lab(v) for v in face["rows"]] for face in fam6["faces"]]
            if sorted(rows5) != sorted(r for b in blocks for r in b):
                return False
            i = 0
            while i < len(rows5):
                b = next((b for b in blocks if b and rows5[i:i + len(b)] == b), None)
                if b is None:
                    return False
                blocks.remove(b); i += len(b)
            return True
        worst = None
        if len(a5) != len(a6):
            bycat["shelf"].append(("tiles", f"gen5 {len(a5)} gen6 {len(a6)}")); worst = "shelf"
        for x, y, fam in zip(a5, a6, g6s):
            if x == y:
                continue
            same_tile = x[:2] == y[:2]
            cat = "shelf-order" if same_tile and reordered_faces(x[2], fam) else "shelf"
            bycat[cat].append((x[0], f"gen5 {x[1]} {x[2][:6]}… | gen6 {y[0]} {y[1]} {y[2][:6]}…"))
            worst = "shelf" if cat == "shelf" or worst == "shelf" else cat
        report["shelf"] = {} if worst is None else {worst: "differs"}
    n = len(report)
    clean = sum(1 for v in report.values() if not v)
    blocking = {c: v for c, v in bycat.items() if c not in allow}
    print(f"checked {n} ({sum(1 for v in runners.values() if v is not None)} runner roots); clean {clean}; categories {len(bycat)}; blocking {len(blocking)}")
    for c, v in sorted(bycat.items(), key=lambda x: -len(x[1])):
        tag = "ALLOWED" if c in allow else "BLOCK"
        print(f"\n[{tag}] {c}: {len(v)}" + (f" — {allow[c]}" if c in allow else ""))
        for lab, why in v[:a.show]:
            print(f"    {lab}: {why[:400]}")
    if a.json:
        json.dump({k: v for k, v in report.items()}, open(a.json, "w"), indent=1, default=str)
    return 1 if blocking else 0


if __name__ == "__main__":
    sys.exit(main())
