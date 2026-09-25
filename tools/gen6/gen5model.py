#!/usr/bin/env python3
"""Gen 5's runner build, recomputed from the gen-5 library (for the runner roots no launch dump covers: runners the
chain never picks by default). A port of ResolveNodeOrder (post-order DFS over bare OVER refs), NodeLower::Lower
(the layer types the gate compares) and FoldEnv. It is proven against every runner build the dumps DO contain
(equivalence.py --check-model) before it is trusted for the rest."""
import json, os


def load(root):
    out = {}
    for dp, dn, fns in os.walk(root):
        dn[:] = [d for d in dn if not d.startswith("_friend_")]
        for fn in fns:
            if fn.endswith(".json"):
                try:
                    j = json.load(open(os.path.join(dp, fn)))
                except Exception:
                    continue
                if isinstance(j, dict) and j.get("CID"):
                    out[j["CID"]] = (j, dp)
    return out


def order(nodes, root):
    out, seen, stack = [], set(), set()

    def emit(h):
        if h in seen or h in stack or h not in nodes:
            return
        stack.add(h)
        for r in nodes[h][0].get("OVER", []):
            if isinstance(r, str):
                emit(r)
        stack.discard(h)
        seen.add(h)
        out.append(h)
    emit(root)
    return out


FORMS = {"zip": "VFSZipLayer", "dir": "VFSDirLayer", "file": "VFSFileLayer", "delta": "VFSDeltaLayer"}


def reg_tree(path, tree, arch, override, when, out):
    values = {k: v for k, v in tree.items() if not isinstance(v, dict)}
    subs = [(k, v) for k, v in tree.items() if isinstance(v, dict)]
    if values or (not subs and not tree):
        L = {"TYPE": "RegEdit", "REGPATH": path, "KEYVALUES": values}
        if arch is not None:
            L["ARCHITECTURE"] = arch
        if override:
            L["OVERRIDE"] = True
        if when:
            L["WHEN"] = when
        out.append(L)
    for k, v in subs:
        reg_tree(path + "\\" + k, v, arch, override, when, out)


def lower(n, bdir):
    out = []
    for L in n.get("LAYERS", []):
        o = {"TYPE": FORMS[L["FORM"]]}
        for k in ("PATH", "TARGET", "SOURCE", "SUBMOUNTS", "COMMENT", "WHEN"):
            if k in L:
                o[k] = L[k]
        if "PATH" in o:
            o["PATH"] = os.path.join(bdir, o["PATH"])      # AbsLayers
        out.append(o)
    for P in n.get("PATCHES", []):
        for E in P["EDITS"]:
            out.append(dict(E, TYPE="BinaryPatch", FILE=P["FILE"]))
    for F in n.get("FILEEDITS", []):
        for E in F["EDITS"]:
            o = dict(E, TYPE="FileEdit", FILE=F["FILE"])
            if F.get("OVERRIDE"):
                o["OVERRIDE"] = True
            out.append(o)
    for E in n.get("REGEDITS", []):
        arches = E.get("ARCHITECTURE") or [None]
        for a in arches:
            for hive, tree in E.items():
                if hive in ("ARCHITECTURE", "OVERRIDE", "WHEN", "COMMENT"):
                    continue
                reg_tree(hive, tree, a, E.get("OVERRIDE", False), E.get("WHEN", ""), out)
    for d, o in (n.get("DLLOVERRIDES") or {}).items():
        out.append({"TYPE": "DllOverride", "DLLOVERRIDE": f"{d}={o}"})
    for V in n.get("VARS", []):
        out.append(dict({k: V[k] for k in ("KEY", "DEFAULT", "COMMENT", "UI", "WHEN") if k in V}, TYPE="CustomVar"))
    return out


def runner_dump(nodes, rid):
    """The fields of a --resolve-only dump that describe the runner build of `rid`."""
    ids = order(nodes, rid)
    comps = [{"COMPONENTID": nodes[h][0].get("LABEL"), "SUBCOMPONENTS": lower(*nodes[h])} for h in ids]
    env, removed = {}, []
    for h in ids:
        n = nodes[h][0]
        for k in n.get("ENV_REMOVE", []):
            env.pop(k, None)
            if k not in removed:
                removed.append(k)
        for k, v in (n.get("ENV") or {}).items():
            env[k] = v
            if k in removed:
                removed.remove(k)
    e = next(E for E in nodes[rid][0]["ENTRYPOINTS"] if E.get("GUEST"))
    return {"RunnerComponents": comps, "RunnerRecipe": [c["COMPONENTID"] for c in comps],
            "RunnerExecutable": e.get("PATH", ""), "RunnerArgs": e.get("ARGS", []),
            "ContentRoot": e.get("CONTENT_ROOT", ""), "PrefixGenerate": e.get("PREFIX_GENERATE", False),
            "UnifiedRuntime": e.get("UNIFIED_RUNTIME", False), "RunnerEnv": env, "RunnerRemoveEnv": removed,
            "Variables": {}}
