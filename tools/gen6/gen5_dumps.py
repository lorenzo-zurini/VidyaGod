#!/usr/bin/env python3
"""Gen-5 ground truth for the generation-6 equivalence gate: `VidyaGod --resolve-only <CID>` for every VARIANT node.

Runs against a SCRATCH data dir (never the seeder): <work>/base holds GlobalConfig.JSON + LIBRARY (node files copied,
everything else symlinked to the seeder's bytes). Each worker gets its own data dir (own lock) sharing that LIBRARY.
Dumps land in <work>/dumps/<cid>.json; failures in <work>/dumps/<cid>.err.

The ground truth captured 2026-09-25 (gen 5, before any gen-6 change) is archived at
~/.local/share/vidyagod-gen6-gate/gen5-ground-truth-20260925.tar.zst — once the engine is gen 6 it cannot be
re-dumped. Restore: extract into <work>, then `gen5_dumps.py <seeder> <work> --relink` (content symlinks only).

Usage: gen5_dumps.py <seeder-data-dir> <work-dir> [--jobs N] [--binary PATH] [--only CID...] [--option-configs]
"""
import argparse, json, os, shutil, subprocess, sys
from concurrent.futures import ThreadPoolExecutor


def mirror(seeder, base, content_only=False):
    """LIBRARY with node files copied (the gate may rewrite them) and content symlinked (never copied).
    content_only: restore just the symlinks under an extracted ground-truth archive (its node files are gen 5's;
    the seeder's may not be any more)."""
    src, dst = os.path.join(seeder, "LIBRARY"), os.path.join(base, "LIBRARY")
    for dp, dn, fns in os.walk(src):
        dn[:] = [d for d in dn if not d.startswith("_friend_")]
        od = os.path.join(dst, os.path.relpath(dp, src))
        os.makedirs(od, exist_ok=True)
        for fn in fns:
            s, d = os.path.join(dp, fn), os.path.join(od, fn)
            if os.path.lexists(d):
                continue
            if fn.endswith(".json"):
                if not content_only:
                    shutil.copy2(s, d)
            else:
                os.symlink(s, d)
    if content_only:
        return
    g = json.load(open(os.path.join(seeder, "GlobalConfig.JSON")))
    g.setdefault("Settings", {})["IPFS"] = {"Enabled": False}
    json.dump(g, open(os.path.join(base, "GlobalConfig.JSON"), "w"), indent=2)


def variants(lib):
    out = []
    for dp, dn, fns in os.walk(lib):
        dn[:] = [d for d in dn if not d.startswith("_friend_")]
        for fn in fns:
            if fn.endswith(".json"):
                try:
                    j = json.load(open(os.path.join(dp, fn)))
                except Exception:
                    continue
                if isinstance(j, dict) and j.get("VARIANT") and j.get("CID"):
                    out.append(j["CID"])
    return sorted(out)


def option_configs(lib):
    """Non-default launches of every variant that has TOGGLE'd grafts: all on, all off, each flipped alone — and for a
    graft declaring an enum var, each choice with it on. -> [(cid, tag, {"modules": {graft cid: bool}, "vars": {}})]"""
    nodes = {}
    for dp, dn, fns in os.walk(lib):
        dn[:] = [d for d in dn if not d.startswith("_friend_")]
        for fn in fns:
            if fn.endswith(".json"):
                try:
                    j = json.load(open(os.path.join(dp, fn)))
                except Exception:
                    continue
                if isinstance(j, dict) and j.get("CID"):
                    nodes[j["CID"]] = j
    variants = {h for h, n in nodes.items() if n.get("VARIANT")}
    per = {}
    for h, n in nodes.items():
        if "TOGGLE" not in n:
            continue
        for x in n.get("OVER", []):
            for t in (x if isinstance(x, list) else [x] if isinstance(x, str) and x in variants else []):
                per.setdefault(t, []).append(h)
    out = []
    for v, gs in sorted(per.items()):
        dflt = {g: nodes[g]["TOGGLE"] == "on" for g in gs}
        out.append((v, "all-on", {"modules": {g: True for g in gs}, "vars": {}}))
        out.append((v, "all-off", {"modules": {g: False for g in gs}, "vars": {}}))
        for g in gs:
            out.append((v, "flip-" + g[-8:], {"modules": dict(dflt, **{g: not dflt[g]}), "vars": {}}))
            for var in nodes[g].get("VARS", []):
                for ch in ((var.get("UI") or {}).get("CHOICES") or []):
                    if (var.get("UI") or {}).get("CONTROL") == "enum" and ch["VALUE"] != var.get("DEFAULT"):
                        out.append((v, f"{g[-8:]}-{var['KEY']}={ch['VALUE']}",
                                    {"modules": dict(dflt, **{g: True}), "vars": {var["KEY"]: ch["VALUE"]}}))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("seeder"); ap.add_argument("work")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--binary", default=os.path.join(os.path.dirname(__file__), "../../build/VidyaGod"))
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--option-configs", action="store_true", help="dump the non-default option launches instead")
    ap.add_argument("--relink", action="store_true", help="only restore content symlinks under an extracted archive")
    a = ap.parse_args()
    base, dumps = os.path.join(a.work, "base"), os.path.join(a.work, "dumps")
    os.makedirs(dumps, exist_ok=True)
    if a.relink:
        mirror(a.seeder, base, content_only=True)
        print("content relinked under", base)
        return 0
    if not os.path.isdir(os.path.join(base, "LIBRARY")):
        os.makedirs(base, exist_ok=True); mirror(a.seeder, base)
    configs = {}
    if a.option_configs:
        for cid, tag, cfg in option_configs(os.path.join(base, "LIBRARY")):
            configs[f"{cid}@{tag}"] = (cid, cfg)
        cids = sorted(configs)
    else:
        cids = a.only or variants(os.path.join(base, "LIBRARY"))
    workers = []
    for i in range(a.jobs):
        w = os.path.join(a.work, f"w{i}")
        os.makedirs(w, exist_ok=True)
        if not os.path.lexists(os.path.join(w, "LIBRARY")):
            os.symlink(os.path.abspath(os.path.join(base, "LIBRARY")), os.path.join(w, "LIBRARY"))
        shutil.copy2(os.path.join(base, "GlobalConfig.JSON"), os.path.join(w, "GlobalConfig.JSON"))
        workers.append(w)
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    binary = os.path.abspath(a.binary)

    def run(i_cid):
        i, name = i_cid
        w = workers[i % len(workers)]
        cid, extra = name, []
        if name in configs:
            cid, cfg = configs[name]
            for m, on in cfg["modules"].items():
                extra += ["--module", f"{m}={'on' if on else 'off'}"]
            for k, v in cfg["vars"].items():
                extra += ["--var", f"{k}={v}"]
        r = subprocess.run([binary, "--offline", "--data-dir", w] + extra + ["--resolve-only", cid], env=env, capture_output=True, text=True, timeout=120)
        out = os.path.join(w, f"vg_resolve_{cid}.json")
        if r.returncode == 0 and os.path.exists(out):
            os.replace(out, os.path.join(dumps, name + ".json"))
            if name in configs:
                json.dump(configs[name][1], open(os.path.join(dumps, name + ".cfg"), "w"))
            return True
        cid = name
        open(os.path.join(dumps, cid + ".err"), "w").write(r.stdout[-20000:] + r.stderr[-5000:])
        return False

    # one worker dir per thread at a time: thread k only ever uses workers[k]
    ok = 0
    with ThreadPoolExecutor(a.jobs) as ex:
        chunks = [[(k, c) for c in cids[k::a.jobs]] for k in range(a.jobs)]
        for res in ex.map(lambda ch: [run(x) for x in ch], chunks):
            ok += sum(res)
    print(f"dumped {ok}/{len(cids)}")
    # the shelf: --list-nodes prints one line per tile: "  <title>  (game <uid>): <label>[state] <label>[state] ..."
    import re
    r = subprocess.run([binary, "--offline", "--data-dir", workers[0], "--list-nodes"], env=env, capture_output=True, text=True, timeout=120)
    shelf = []
    for line in (r.stdout + r.stderr).splitlines():
        line = re.sub(r"\x1b\[[0-9;]*m", "", line)
        m = re.search(r"list-nodes   (.*)  \(game ([^)]*)\): (.*)$", line)
        if m:
            rows = re.findall(r"(.*?)\[(?:hydrated|remote)\] ?", m.group(3))
            shelf.append({"title": m.group(1), "uid": m.group(2), "rows": rows})
    json.dump(shelf, open(os.path.join(dumps, "shelf.list"), "w"), indent=1)
    print(f"shelf: {len(shelf)} tiles")
    return 0 if ok == len(cids) else 1


if __name__ == "__main__":
    sys.exit(main())
