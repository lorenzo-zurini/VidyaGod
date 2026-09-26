#!/usr/bin/env python3
"""The generation-6 ENGINE gate: the gen-6 binary's resolved launches (gen5_dumps.py --replay over the migrated
library) against gen 5's (the archived ground truth), launch for launch, field for field. The M1 gate proved the
reference resolver equals gen 5; fold_gate.py holds the C++ fold to the reference; this proves the whole engine —
lowering, placement through GUEST_ROOTS, the runner chain, variables, persistence — resolves what gen 5 did.

Compared per launch (both sides normalised the same way; content paths relative to each side's LIBRARY):
  content / edits / reg / regkeys / dll   the game's mount and applies (equivalence.g5_state on both dumps)
  edit-pass                               which pass each file edit runs in (before the mount / after / as a default)
  runner-*                                the boundary runner's build (flattened), exec, env
  exec                                    platform, exe, args, workdir (the entry LABEL is "Play" by design)
  vars                                    resolved values (new option variables allowed)
  env, keep, chain, face                  LaunchEnv/LaunchRemoveEnv, Keep*, RunnerChain, PackageUID/GameName

Usage: engine_gate.py <gen5-work> <gen6-work> [--show N]
"""
import argparse, glob, json, os, sys
from collections import defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from equivalence import g5_split, g5_state, g5_runner, dict_diff, first_diff
from resolve import subst, to_layout

ALLOW = {
    "vars-new-option": "a TOGGLE'd graft that named one variant is now its own bool option (§6 step 4)",
    "edit-pass": "a package file edit applies over the mounted files (gen 5's base pass wrote it into DEFAULTDATA, "
                 "shadowing any content file of that name) — an EDIT applies to the value beneath it",
}


def edit_passes(d):
    """FILE -> the passes its FileEdits run in: base (DEFAULTDATA, before the mount), post (over the mounted files),
    default (post, while the user has no saved copy)."""
    _, g, _ = g5_split(d)
    out = defaultdict(set)
    for L in g:
        if L.get("TYPE") == "FileEdit":
            out[L["FILE"]].add("default" if L.get("IF_UNSAVED") else "post" if L.get("OVERRIDE") else "base")
    return out


def norm_exec(e):
    return {k: e.get(k) for k in ("PLATFORM", "CONTENTPATH", "EXEARGS", "WORKDIR")}


def keep(d):
    return sorted((x.get("path", "").rstrip("/"), x.get("target"), x.get("cloud")) for x in (d.get("KeepDirs") or []) + (d.get("KeepFiles") or [])) \
        + sorted(d.get("KeepRegKeys") or []) + sorted(d.get("KeepRegHives") or [])


def env(d):
    e = dict(d.get("LaunchEnv") or {})
    for k in d.get("LaunchRemoveEnv") or []:
        e[k] = None
    return e


def runner_env(d):
    e = dict(d.get("RunnerEnv") or {})
    for k in d.get("RunnerRemoveEnv") or []:
        e[k] = None
    return e


def chain(d):
    return [(l["Host"], tuple(l["Guests"]), l["Executable"], l["Native"]) for l in d["RunnerChain"]]


def rel_layers(ls, root):
    out = []
    for L in ls or []:
        L = dict(L)
        p = L.get("PATH", "")
        if isinstance(p, str) and "/LIBRARY/" in p:
            L["PATH"] = p[p.rindex("/LIBRARY/") + len("/LIBRARY/"):]
        if L.get("TARGET") == "":                      # absent and "" are both the mount root
            del L["TARGET"]
        out.append(json.dumps(L, sort_keys=True))
    return sorted(out)


def compare(a, b, ra, rb):
    diffs = {}
    _, ga, _ = g5_split(a)
    _, gb, _ = g5_split(b)
    sa, sb = g5_state(ga, ra), g5_state(gb, rb)
    for cat in ("content", "edits", "reg", "regkeys", "dll"):
        if sa[cat] != sb[cat]:
            diffs[cat] = first_diff(sa[cat], sb[cat]) if cat == "content" else (
                dict_diff(sa[cat], sb[cat]) if isinstance(sa[cat], dict) else f"gen5 {sorted(sa[cat])[:3]} gen6 {sorted(sb[cat])[:3]}")
    if a.get("RunnerComponents") or b.get("RunnerComponents"):
        (ia, da), (ib, db) = g5_runner(a), g5_runner(b)
        rsa, rsb = g5_state(ia, ra), g5_state(ib, rb)
        for cat in ("content", "edits", "reg", "regkeys", "dll"):
            if rsa[cat] != rsb[cat]:
                diffs["runner-" + cat] = first_diff(rsa[cat], rsb[cat]) if cat == "content" else str(dict_diff(rsa[cat], rsb[cat]))[:300]
        dd = dict_diff(da, db)
        if dd:
            diffs["runner-vardecls"] = dd
    pa, pb = edit_passes(a), edit_passes(b)
    for f in sorted(set(pa) | set(pb)):
        if pa.get(f) == pb.get(f):
            continue
        cat = "edit-pass" if pa.get(f) == {"base"} and pb.get(f) == {"post"} else "edit-pass-changed"
        diffs[cat] = diffs.get(cat, "") + f"{f}: gen5 {sorted(pa.get(f, ()))} gen6 {sorted(pb.get(f, ()))}; "
    if rel_layers(a.get("RunnerLayers"), ra) != rel_layers(b.get("RunnerLayers"), rb):
        diffs["runner-layers"] = f"gen5 {len(a.get('RunnerLayers') or [])} gen6 {len(b.get('RunnerLayers') or [])}"
    for f in ("RunnerExecutable", "RunnerArgs", "ContentRoot", "PrefixRoot", "PrefixGenerate", "ContentPath", "ExeArgs",
              "PackageUID", "GameName", "Platform", "RunnerShipsBuild", "UnifiedRuntime", "RunnerName"):
        if a.get(f) != b.get(f):
            diffs.setdefault("fields", "")
            diffs["fields"] += f"{f}: gen5 {a.get(f)!r} gen6 {b.get(f)!r}; "
    if os.path.basename(a.get("Content", "")) != os.path.basename(b.get("Content", "")):
        diffs["content-exe"] = f"gen5 {a.get('Content')} gen6 {b.get('Content')}"
    # gen 6 keeps paths in guest coordinates until substituted, then maps them through the runner's GUEST_ROOTS
    roots = {k: subst(v, b.get("Variables") or {}) for k, v in (b.get("GuestRoots") or {}).items()}
    raw_roots = b.get("GuestRoots") or {}
    eb = dict(b.get("ComposedExec") or {})
    for f in ("CONTENTPATH", "WORKDIR"):
        if isinstance(eb.get(f), str):
            eb[f] = to_layout(eb[f], raw_roots)
    dd = dict_diff(norm_exec(a.get("ComposedExec") or {}), norm_exec(eb))
    if dd:
        diffs["exec"] = dd
    new_opts = {k for k in b["CustomVariables"] if k not in a["CustomVariables"] and b["CustomVariables"][k] in ("0", "1")}
    if new_opts:
        diffs["vars-new-option"] = ", ".join(sorted(new_opts))
    # a path-valued variable (dgv_dir) holds guest coordinates; every use of one is a mapped position (TARGET,
    # SUBMOUNTS), so what gen 5 held is its value mapped
    dd = dict_diff(a["CustomVariables"], {k: to_layout(v, roots) for k, v in b["CustomVariables"].items() if k not in new_opts})
    if dd:
        diffs["vars"] = dd
    dd = dict_diff(env(a), env(b))
    if dd:
        diffs["env"] = dd
    dd = dict_diff(runner_env(a), runner_env(b))
    if dd:
        diffs["runner-env"] = dd
    if keep(a) != keep(b):
        diffs["keep"] = f"gen5 {keep(a)} gen6 {keep(b)}"
    if chain(a) != chain(b):
        diffs["chain"] = f"gen5 {chain(a)} gen6 {chain(b)}"
    return diffs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("g5work"); ap.add_argument("g6work"); ap.add_argument("--show", type=int, default=3)
    a = ap.parse_args()
    ra, rb = os.path.join(a.g5work, "base", "LIBRARY"), os.path.join(a.g6work, "base", "LIBRARY")
    bycat, n, clean, missing = defaultdict(list), 0, 0, []
    for f in sorted(glob.glob(os.path.join(a.g5work, "dumps", "*.json"))):
        name = os.path.basename(f)[:-5]
        g = os.path.join(a.g6work, "dumps", name + ".json")
        if not os.path.exists(g):
            missing.append(name); continue
        n += 1
        d = compare(json.load(open(f)), json.load(open(g)), ra, rb)
        clean += not d
        for c, why in d.items():
            bycat[c].append((name[:40], why))
    blocking = {c: v for c, v in bycat.items() if c not in ALLOW}
    print(f"engine gate: {n} launches compared, {clean} identical, {len(missing)} not dumped by gen 6; blocking categories {len(blocking)}")
    for m in missing[:5]:
        err = os.path.join(a.g6work, "dumps", m + ".err")
        tail = open(err).read().strip().splitlines()[-3:] if os.path.exists(err) else []
        print(f"    NOT DUMPED {m[:50]}: {' | '.join(tail)[:300]}")
    for c, v in sorted(bycat.items(), key=lambda x: -len(x[1])):
        print(f"\n[{'ALLOWED' if c in ALLOW else 'BLOCK'}] {c}: {len(v)}")
        for lab, why in v[:a.show]:
            print(f"    {lab}: {why[:400]}")
    return 1 if blocking or missing else 0


if __name__ == "__main__":
    sys.exit(main())
