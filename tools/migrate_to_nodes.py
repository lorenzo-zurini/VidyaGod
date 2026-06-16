#!/usr/bin/env python3
"""Migrate a bundle's old-shape manifest(s) into per-node files (everything-is-a-node schema).

Merges EVERY old-shape *.json in the dir (those with GAMES/COMPONENTS/RUNNERS and no NODE_ID, e.g. MANIFEST.json
AND auxiliary files like gemrb_runner.json), then emits per-node files:
  COMPONENT      -> content node    (PARENTCOMPONENT -> PARENTS; SUBCOMPONENTS -> LAYERS)
  GAME variant   -> launchable node (METADATA+TITLE -> META; CONTENTPATH/EXEARGS/WORKDIR -> EXEC; MODULES -> PARENTS;
                    GROUP=<gameid>, LABEL=<variant id/name>; module REQUIRED:false -> target node's OPTIONAL/DEFAULT/EXCLUDE)
  RUNNER variant -> runner node     (exec fields -> EXEC; build MODULES -> PARENTS)
NODE_IDs are bare slugs; a launchable that collides with a component gets a '_game' suffix.
launchable UID = PACKAGEUID (bundle content id for %PackageUID%); GAMEUID kept in META.
Stale node files (any *.json carrying NODE_ID) are removed first for a clean regen.
"""
import json, sys, os, re, glob

def slug(s): return re.sub(r'[^a-z0-9]+', '_', str(s).lower()).strip('_')

def json_files(pkgdir):
    # os.listdir (not glob): bundle dir names contain brackets like [299] that glob treats as char classes.
    return [os.path.join(pkgdir, f) for f in sorted(os.listdir(pkgdir)) if f.endswith(".json")]

def load_oldshape(pkgdir):
    M = {"PACKAGEUID": "", "PACKAGENAME": "", "COMPONENTS": [], "GAMES": [], "RUNNERS": []}
    for f in json_files(pkgdir):
        try: j = json.load(open(f))
        except Exception: continue
        if not isinstance(j, dict) or "NODE_ID" in j: continue          # skip node files
        if not any(k in j for k in ("GAMES", "COMPONENTS", "RUNNERS", "PACKAGEUID")): continue
        if j.get("PACKAGEUID"):  M["PACKAGEUID"]  = str(j["PACKAGEUID"])
        if j.get("PACKAGENAME"): M["PACKAGENAME"] = j["PACKAGENAME"]
        for k in ("COMPONENTS", "GAMES", "RUNNERS"):
            if isinstance(j.get(k), list): M[k] += j[k]
    return M

def migrate(pkgdir):
    M = load_oldshape(pkgdir)
    pkguid = M["PACKAGEUID"]; comps = M["COMPONENTS"]; games = M["GAMES"]; runners = M["RUNNERS"]

    for old in json_files(pkgdir):
        try:
            j = json.load(open(old))
            if isinstance(j, dict) and "NODE_ID" in j: os.remove(old)
        except Exception: pass

    opt = {}
    def gather(modules):
        for mod in modules:
            cid = mod.get("COMPONENT")
            if cid and not mod.get("REQUIRED", True):
                o = opt.setdefault(cid, {"DEFAULT": mod.get("DEFAULT", True)})
                if mod.get("EXCLUDE"): o["EXCLUDE"] = mod["EXCLUDE"]
    for g in games:
        for v in g.get("VARIANTS", []): gather(v.get("MODULES", []))
    for r in runners:
        for v in r.get("VARIANTS", []): gather(v.get("MODULES", []))

    out = {}
    def uniq(base):
        if base not in out: return base
        c, i = base + "_game", 2
        while c in out: c, i = f"{base}_game{i}", i + 1
        return c
    def emit(n): out[n["NODE_ID"]] = n

    for c in comps:
        cid = c.get("COMPONENTID")
        if not cid: continue
        n = {"NODE_ID": cid, "ROLE": "content"}
        if c.get("PARENTCOMPONENT"): n["PARENTS"] = [c["PARENTCOMPONENT"]]
        if cid in opt:
            n["OPTIONAL"] = True; n["DEFAULT"] = opt[cid].get("DEFAULT", True)
            if opt[cid].get("EXCLUDE"): n["EXCLUDE"] = opt[cid]["EXCLUDE"]
        n["LAYERS"] = c.get("SUBCOMPONENTS", [])
        emit(n)

    for g in games:
        gid = g.get("GAMEID"); variants = g.get("VARIANTS", []); multi = len(variants) > 1
        for v in variants:
            vid = v.get("VARIANT_ID", "")
            nid = uniq(gid if not multi else f"{gid}_{slug(vid)}")
            n = {"NODE_ID": nid, "ROLE": "launchable", "UID": pkguid, "GROUP": gid}
            if v.get("NAME") or vid: n["LABEL"] = v.get("NAME", vid)
            if v.get("RECOMMENDED"): n["RECOMMENDED"] = True
            meta = dict(g.get("METADATA", {}))
            if g.get("TITLE"):   meta["TITLE"]   = g["TITLE"]
            if g.get("GAMEUID"): meta["GAMEUID"] = g["GAMEUID"]
            if meta: n["META"] = meta
            n["PLATFORM"] = {"HOST": v.get("HOST_PLATFORM", "")}
            ex = {}
            for k in ("CONTENTPATH", "EXEARGS", "WORKDIR"):
                if v.get(k): ex[k] = v[k]
            if ex: n["EXEC"] = ex
            parents = [m["COMPONENT"] for m in v.get("MODULES", []) if m.get("COMPONENT")]
            if parents: n["PARENTS"] = parents
            emit(n)

    for r in runners:
        rid = r.get("RUNNER_ID"); variants = r.get("VARIANTS", []); multi = len(variants) > 1
        for v in variants:
            nid = uniq(rid if not multi else f"{rid}_{slug(v.get('VARIANT_ID',''))}")
            n = {"NODE_ID": nid, "ROLE": "runner"}
            plat = {}
            if v.get("HOST_PLATFORM"):  plat["HOST"]  = v["HOST_PLATFORM"]
            if v.get("GUEST_PLATFORM"): plat["GUEST"] = v["GUEST_PLATFORM"]
            if plat: n["PLATFORM"] = plat
            ex = {}
            for k in ("EXECUTABLE","ARGS","ENV","REMOVE_ENV","CONTENT_ROOT","PREFIX_GENERATE","UNIFIED_RUNTIME"):
                if k in v: ex[k] = v[k]
            if ex: n["EXEC"] = ex
            parents = [m["COMPONENT"] for m in v.get("MODULES", []) if m.get("COMPONENT")]
            if parents: n["PARENTS"] = parents
            emit(n)

    for nid, node in out.items():
        with open(os.path.join(pkgdir, nid + ".json"), "w") as f:
            json.dump(node, f, indent=4, ensure_ascii=False); f.write("\n")
    return len(out)

if __name__ == "__main__":
    tot = 0
    for d in sys.argv[1:]: tot += migrate(d)
    print(f"wrote {tot} node files across {len(sys.argv)-1} dir(s)")
