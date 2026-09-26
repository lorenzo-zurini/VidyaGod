#!/usr/bin/env python3
"""Before/after gate for a package change: what every affected launch resolves to, compared field by field.

  plan_diff.py snapshot <LIBRARY> <out> --work <dir> [--package SUBSTR ...] [--runners] [--all-variants] [--jobs N]
  plan_diff.py compare <before> <after> [--library <LIBRARY>] [--show N]

snapshot resolves (`VidyaGod --offline --resolve-only <handle>`) every variant whose closure reaches a node of a
package matching one of the --package substrings (or that such a package's graft applies to) — every variant when no
--package is given — and saves each plan as <out>/<handle>.json. A runner is never referenced (the chain finds it by
capability), so a runner or runner-library change needs --runners (resolve the runner roots in those packages) and
--all-variants (every game, each resolving its chain through the changed runner). Variants are named by their working-tree HANDLE
(the stored "CID"), which stays put while nodes are edited, so a before and an after snapshot pair up.

--work holds the worker data dirs (a LIBRARY symlink + a minimal GlobalConfig each). Reuse the SAME --work for the
before and the after snapshot: a variable drawn from a POOL on first resolve (WC3's CD keys) is instance data, and it
must be the same draw on both sides. A handle always resolves in the same worker.

compare prints, per variant, the fields that differ (mounts and applies, exec, runner chain, env, keep, variables).
Exit code 1 when anything differs — every difference is to be read and justified.
"""
import argparse, difflib, glob, hashlib, json, os, shutil, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

FIELDS = ["SubComponentsArray", "ComposedExec", "RunnerChain", "RunnerComponents", "LaunchEnv", "LaunchRemoveEnv",
          "KeepFiles", "KeepRegKeys", "KeepRegHives", "Variables", "GuestRoots", "PackageUID", "GameName"]


def load_nodes(lib):
    nodes = {}                                   # handle -> (node, package dir relative to LIBRARY)
    for d, dirs, fs in os.walk(lib):
        for f in fs:
            if not f.endswith(".json"):
                continue
            p = os.path.join(d, f)
            try:
                n = json.load(open(p))
            except Exception:
                continue
            if isinstance(n, dict) and isinstance(n.get("LAYERS"), list) and n.get("CID"):
                rel = os.path.relpath(d, lib).split(os.sep)
                nodes[n["CID"]] = (n, "/".join(rel[:2]))
    return nodes


def refs(n):
    out = []
    for L in n["LAYERS"]:
        if not isinstance(L, dict):
            continue
        if isinstance(L.get("NODE"), str):
            out.append(L["NODE"])
        if isinstance(L.get("ANY"), list):
            out += [x for x in L["ANY"] if isinstance(x, str)]
    return out


def is_runner_root(n):
    return any(isinstance(L, dict) and isinstance(L.get("EXEC"), list)
               and any(isinstance(E, dict) and E.get("GUEST") for E in L["EXEC"]) for L in n["LAYERS"])


def runner_roots(nodes, subs):
    """Runner roots (a node whose own EXEC declares GUEST) in the matching packages — every one without --package."""
    return sorted(h for h, (n, pkg) in nodes.items() if is_runner_root(n) and (not subs or any(s in pkg for s in subs)))


def affected(nodes, subs):
    variants = [h for h, (n, _) in nodes.items() if n.get("VARIANT")]
    if not subs:
        return sorted(variants)
    touched = {h for h, (_, pkg) in nodes.items() if any(s in pkg for s in subs)}
    memo = {}

    def reaches(h, path=()):
        if h in memo:
            return memo[h]
        if h in path or h not in nodes:
            return False
        memo[h] = h in touched or any(reaches(r, path + (h,)) for r in refs(nodes[h][0]))
        return memo[h]
    out = {v for v in variants if reaches(v)}
    # a graft in a touched package applies to the variants its ANY names (and whatever contains them)
    anchors = {a for h in touched for L in nodes[h][0]["LAYERS"][:1] if isinstance(L, dict) and "ANY" in L for a in L["ANY"]}
    if anchors:
        memo2 = {}

        def reaches_anchor(h, path=()):
            if h in memo2:
                return memo2[h]
            if h in path or h not in nodes:
                return False
            memo2[h] = h in anchors or any(reaches_anchor(r, path + (h,)) for r in refs(nodes[h][0]))
            return memo2[h]
        out |= {v for v in variants if reaches_anchor(v)}
    return sorted(out)


def snapshot(a):
    lib = os.path.abspath(a.library)
    nodes = load_nodes(lib)
    todo = affected(nodes, a.package)
    if a.all_variants:
        todo = affected(nodes, [])
    if a.runners:                                  # a runner is found by capability, never referenced: resolve it too
        todo = sorted(set(todo) | set(runner_roots(nodes, a.package)))
    os.makedirs(a.out, exist_ok=True)
    cfg = {"Settings": {}}
    seeder_cfg = os.path.join(os.path.dirname(lib), "GlobalConfig.JSON")
    if os.path.exists(seeder_cfg):
        cfg = {"Settings": json.load(open(seeder_cfg)).get("Settings", {})}
    workers = []
    for i in range(a.jobs):
        w = os.path.join(a.work, f"w{i}")
        os.makedirs(w, exist_ok=True)
        link = os.path.join(w, "LIBRARY")
        if not os.path.lexists(link):
            os.symlink(lib, link)
        if not os.path.exists(os.path.join(w, "GlobalConfig.JSON")):
            json.dump(cfg, open(os.path.join(w, "GlobalConfig.JSON"), "w"), indent=2)
        workers.append(w)
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    binary = os.path.abspath(a.binary)

    # A worker dir is used by one process at a time (its data dir holds the instance lock): packages are bucketed by
    # worker — stably, so a variant always resolves in the same worker (the same instance draws) — and each worker
    # resolves its bucket in turn.
    buckets = {}
    for h in todo:
        wi = int(hashlib.sha1(nodes[h][1].encode()).hexdigest(), 16) % len(workers)
        buckets.setdefault(wi, []).append(h)

    def run_bucket(item):
        wi, hs = item
        return [run(h, workers[wi]) for h in hs]

    def run(h, w):
        r = subprocess.run([binary, "--offline", "--data-dir", w, "--resolve-only", h], env=env,
                           capture_output=True, text=True, timeout=300)
        src = os.path.join(w, f"vg_resolve_{h}.json")
        if r.returncode == 0 and os.path.exists(src):
            shutil.move(src, os.path.join(a.out, f"{h}.json"))
            return h, None
        return h, (r.stderr or r.stdout)[-400:]
    failed = []
    with ThreadPoolExecutor(a.jobs) as ex:
        for results in ex.map(run_bucket, sorted(buckets.items())):
            failed += [(h, err) for h, err in results if err is not None]
    json.dump({"variants": todo, "failed": [h for h, _ in failed],
               "labels": {h: nodes[h][0].get("LABEL") for h in todo}}, open(os.path.join(a.out, "_index.json"), "w"), indent=1)
    print(f"snapshot: {len(todo) - len(failed)}/{len(todo)} variant(s) resolved into {a.out}")
    for h, err in failed[:10]:
        print(f"  FAILED {nodes[h][0].get('LABEL')} ({h[:16]}): {err.strip().splitlines()[-1] if err.strip() else '?'}")
    return 1 if failed else 0


def pool_values(lib):
    """Every value a variable may draw from its POOL (a secret drawn on first resolve, e.g. WC3's CD keys)."""
    vals = set()
    for n, _ in load_nodes(lib).values():
        for L in n["LAYERS"]:
            if isinstance(L, dict) and isinstance(L.get("VARS"), dict):
                for d in L["VARS"].values():
                    ui = d.get("UI") if isinstance(d, dict) else None
                    if isinstance(ui, dict) and isinstance(ui.get("POOL"), list):
                        vals |= {v for v in ui["POOL"] if isinstance(v, str) and v}
    return vals


def mask(o, vals):
    if isinstance(o, dict):
        return {k: mask(v, vals) for k, v in o.items()}
    if isinstance(o, list):
        return [mask(v, vals) for v in o]
    if isinstance(o, str):
        for v in vals:
            if v in o:
                o = o.replace(v, "<POOL>")
    return o


def compare(a):
    pools = pool_values(os.path.abspath(a.library)) if a.library else set()
    ib = json.load(open(os.path.join(a.before, "_index.json")))
    ia = json.load(open(os.path.join(a.after, "_index.json")))
    labels = {**ib["labels"], **ia["labels"]}
    only_b = sorted(set(ib["variants"]) - set(ia["variants"]))
    only_a = sorted(set(ia["variants"]) - set(ib["variants"]))
    same = diff = 0
    for h in sorted(set(ib["variants"]) & set(ia["variants"])):
        pb, pa = os.path.join(a.before, f"{h}.json"), os.path.join(a.after, f"{h}.json")
        if not (os.path.exists(pb) and os.path.exists(pa)):
            print(f"== {labels.get(h)} ({h[:16]}): missing on one side")
            diff += 1
            continue
        b, af = mask(json.load(open(pb)), pools), mask(json.load(open(pa)), pools)   # a pool draw is not a change
        bad = [f for f in FIELDS if b.get(f) != af.get(f)]
        if not bad:
            same += 1
            continue
        diff += 1
        print(f"== {labels.get(h)} ({h[:16]}): {', '.join(bad)}")
        for f in bad:
            lb = json.dumps(b.get(f), indent=1, sort_keys=True).splitlines()
            la = json.dumps(af.get(f), indent=1, sort_keys=True).splitlines()
            lines = [l for l in difflib.unified_diff(lb, la, lineterm="", n=1) if not l.startswith(("---", "+++"))]
            for l in lines[:a.show]:
                print("   " + l[:220])
            if len(lines) > a.show:
                print(f"   … {len(lines) - a.show} more line(s)")
    for h in only_b:
        print(f"== only before: {labels.get(h)} ({h[:16]})")
    for h in only_a:
        print(f"== only after: {labels.get(h)} ({h[:16]})")
    print(f"compare: {same} identical, {diff} differ, {len(only_b)} only before, {len(only_a)} only after")
    return 1 if (diff or only_b or only_a) else 0


def main():
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest="cmd", required=True)
    s = sp.add_parser("snapshot")
    s.add_argument("library"); s.add_argument("out")
    s.add_argument("--work", required=True)
    s.add_argument("--package", action="append", default=[])
    s.add_argument("--runners", action="store_true", help="also resolve the runner roots in the matching packages")
    s.add_argument("--all-variants", action="store_true", help="every variant, whatever --package matches")
    s.add_argument("--jobs", type=int, default=8)
    s.add_argument("--binary", default=os.path.join(os.path.dirname(__file__), "../../build/VidyaGod"))
    c = sp.add_parser("compare")
    c.add_argument("before"); c.add_argument("after")
    c.add_argument("--show", type=int, default=40)
    c.add_argument("--library", help="mask values drawn from a variable's POOL (they are drawn per resolve)")
    a = ap.parse_args()
    return snapshot(a) if a.cmd == "snapshot" else compare(a)


if __name__ == "__main__":
    sys.exit(main())
