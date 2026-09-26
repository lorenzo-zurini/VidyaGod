#!/usr/bin/env python3
"""The C++ resolver (src/fold.cpp, via build/vg_fold) against the reference (resolve.py), plan for plan, on exactly
the resolves the equivalence gate performs (equivalence.py --record). Also the graft offering per row.

Usage: fold_gate.py <gen6-library> <recorded.jsonl> [--vg-fold build/vg_fold]
       fold_gate.py --fixtures [--vg-fold build/vg_fold]      # the rule fixtures only (ctest fold_fixtures)
Exit 1 on the first mismatch (printed with the first differing field)."""
import argparse, json, os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from resolve import Resolver, load_nodes, plan_json, graft_index, offered_grafts, applied_grafts


def canon(x):
    return json.dumps(x, sort_keys=True)


def run_fixtures(vg_fold):
    """resolve.py's rule fixtures through both resolvers (the rules the real library does not exercise yet)."""
    import tempfile
    from resolve import fixtures
    bad = total = 0
    for name, fx, cases in fixtures():
        with tempfile.TemporaryDirectory() as d:
            for k, n in fx.items():
                json.dump(dict(n, CID=k), open(os.path.join(d, k + ".json"), "w"))
            nodes = load_nodes(d)
            R, gidx = Resolver(nodes), graft_index(nodes)
            inp, want = "", []
            for i, (root, inst, gs) in enumerate(cases):
                plan = R.resolve(root, inst, {}, gs)
                face = next((e["TILE"]["UID"] for e in plan["exec"].values() if not e.get("GUEST") and e.get("TILE")), None)
                want.append((plan_json(plan), [list(x) for x in offered_grafts(nodes, gidx, plan, face)]
                             + [applied_grafts(R, gidx, root, face, None, inst), applied_grafts(R, gidx, root, face, gs, inst)]))
                inp += json.dumps({"id": i, "root": root, "instance": inst, "grafts": gs, "face": face}) + "\n"
            r = subprocess.run([vg_fold, d], input=inp, capture_output=True, text=True)
            lines = r.stdout.splitlines()
            if r.returncode != 0 or len(lines) != len(want):   # a crash or a short answer is a failure, not a pass
                print(f"FIXTURE {name}: vg_fold exit {r.returncode}, {len(lines)}/{len(want)} answers\n{r.stderr[-2000:]}")
                total += len(want); bad += len(want)
                continue
            for (w, off), line in zip(want, lines):
                g = json.loads(line)
                total += 1
                if canon(w) != canon(g["plan"]) or off != [g.get("offered"), g.get("ticked"), g.get("applied"), g.get("applied_req")]:
                    bad += 1
                    diff = next((f for f in w if canon(w[f]) != canon(g["plan"].get(f))), "grafts")
                    print(f"FIXTURE MISMATCH {name} [{diff}]:\n  python {canon(w.get(diff, off))[:500]}\n  c++    {canon(g['plan'].get(diff, [g.get('offered'), g.get('ticked'), g.get('applied'), g.get('applied_req')]))[:500]}")
    print(f"fold gate fixtures: {total} resolves, {total - bad} identical, {bad} differ")
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("lib", nargs="?"); ap.add_argument("recorded", nargs="?")
    ap.add_argument("--vg-fold", default=os.path.join(os.path.dirname(__file__), "../../build/vg_fold"))
    ap.add_argument("--fixtures", action="store_true", help="only the rule fixtures (what ctest runs: no library needed)")
    a = ap.parse_args()
    if a.fixtures:
        return 1 if run_fixtures(a.vg_fold) else 0
    if not a.lib or not a.recorded:
        ap.error("lib and recorded are required (or --fixtures)")
    lib = os.path.abspath(a.lib)
    nodes = load_nodes(lib)
    R, gidx = Resolver(nodes), graft_index(nodes)
    cases, seen = [], set()
    for line in open(a.recorded):
        c = json.loads(line)
        k = canon(c)
        if k in seen:
            continue
        seen.add(k)
        c["id"] = len(cases)
        plan = R.resolve(c["root"], c["instance"], c["builtins"], c["grafts"])
        face = next((e["TILE"]["UID"] for e in plan["exec"].values() if not e.get("GUEST") and e.get("TILE")), None)
        c["face"] = face
        c["_want"] = plan_json(plan)
        c["_offer"] = offered_grafts(nodes, gidx, plan, face)
        cases.append(c)
    inp = "".join(json.dumps({k: v for k, v in c.items() if not k.startswith("_")}) + "\n" for c in cases)
    r = subprocess.run([a.vg_fold, lib], input=inp, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr); return 1
    got = [json.loads(l) for l in r.stdout.splitlines()]
    if len(got) != len(cases):
        print(f"vg_fold answered {len(got)}/{len(cases)} resolves"); return 1
    bad = 0
    for c, g in zip(cases, got):
        w, p = c["_want"], g["plan"]
        for f in w:
            if canon(w[f]) != canon(p.get(f)):
                bad += 1
                if bad <= 5:
                    print(f"MISMATCH {nodes[c['root']][0].get('LABEL')} [{f}]:\n  python {canon(w[f])[:600]}\n  c++    {canon(p.get(f))[:600]}")
                break
        else:
            if [list(c["_offer"][0]), list(c["_offer"][1])] != [g.get("offered"), g.get("ticked")]:
                bad += 1
                if bad <= 5:
                    print(f"MISMATCH {nodes[c['root']][0].get('LABEL')} [grafts]: python {c['_offer']} c++ {g.get('offered')} {g.get('ticked')}")
    print(f"fold gate: {len(cases)} resolves, {len(cases) - bad} identical, {bad} differ")
    fbad = run_fixtures(a.vg_fold)
    r = subprocess.run([a.vg_fold, lib, "--entries-check"], capture_output=True, text=True)   # the library-wide fast path
    print(r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr[-300:])
    fbad += r.returncode != 0
    return 1 if bad or fbad or len(got) != len(cases) else 0


if __name__ == "__main__":
    sys.exit(main())
