#!/usr/bin/env python3
"""The GEN 9 switch gate: every end's plan after conversion (gen 9 resolver) against its gen 8 baseline.

  equiv.py <converted LIBRARY> <cid_map.json> <baseline dir> <out.jsonl> [--only <substring>]

Normalised so the intended spelling changes compare equal:
  - REG addresses use backslash key paths, KEEP value-level addresses (key + '\\\\' + value) vs gen 8's '/' paths;
  - entries lose LABEL (the key) and gain RECOMMENDED;
  - the bundle dir is compared relative to the library;
  - a gen 8 value named "@" is the key's default value "" (the converter's intended fix);
  - a row whose gen 9 tile is {UID} (D8: not its card's recommended row) is compared by the tile's UID;
  - node CIDs (order, from) are mapped through cid_map.
An end is IDENTICAL or DIFFERS (the fields and their first difference). Exit 0 when all are identical."""
import argparse, json, os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "gen6"))
import resolve as R


def load(lib):
    nodes = {}
    for dp, dn, fns in os.walk(lib):
        for fn in fns:
            if fn.endswith(".json") and fn not in ("cid_map.json", "convert_report.json"):
                p = os.path.join(dp, fn)
                try: j = json.load(open(p))
                except Exception: continue
                if isinstance(j, dict) and isinstance(j.get("LAYERS"), list):
                    nodes[R.cid_of(R.canonical(j))] = (j, dp)
    return nodes


def norm(plan, rel, cmap, gen8, uid_only=None):
    """A plan in comparable form."""
    m = (lambda c: cmap.get(c, c)) if gen8 else (lambda c: c)
    def regaddr(a):
        return a.replace("\\", "/").replace("#", "/").lower() if a.startswith("REG/") else a
    def regs(v):                                         # REG addresses inside TAKE views, spelled alike
        if isinstance(v, str):
            return regaddr(v)
        if isinstance(v, list):
            return [regs(x) for x in v]
        return v
    seq = []
    for it in plan["seq"]:
        e = dict(it)
        if e.get("view") is not None:
            e["view"] = regs(e["view"])
        if "dir" in e and e["dir"]:
            e["dir"] = os.path.relpath(e["dir"], rel)
        e["from"] = m(e.get("from", ""))
        seq.append(json.dumps(e, sort_keys=True))
    exe = {}
    for lab, e in plan["exec"]:
        e = {k: v for k, v in e.items() if k not in ("LABEL", "RECOMMENDED")}
        exe[lab] = e
    # D8: a row that is not its card's recommended one references the card by {UID}: compared by UID alone
    if uid_only is not None:
        for lab, e in exe.items():
            if isinstance(e.get("TILE"), dict) and lab in uid_only:
                e["TILE"] = {"UID": e["TILE"].get("UID")}
    keep = sorted((regaddr(k), json.dumps(v)) for k, v in plan["keep"])
    # gen 8 nodes naming a key's default value "@" (the converter renames it to "", what the writer reads: intended)
    # (and a gen 8 subkey named "" — a trailing backslash — is the key itself: the converter merges it)
    key = (lambda k: k.rstrip("\\")) if gen8 else (lambda k: k)
    reg = sorted(json.dumps([r[0], key(r[1].lower()), "" if gen8 and r[2] == "@" else r[2].lower(), r[5]]) for r in plan["reg"])
    regkeys = sorted(json.dumps([r[0], r[1].lower()]) for r in plan["regkeys"])
    return {"seq": seq, "exec": json.dumps(exe, sort_keys=True), "keep": keep, "reg": reg, "regkeys": regkeys,
            "dll": sorted(json.dumps(x) for x in plan["dll"]), "env": sorted(json.dumps(x) for x in plan["env"]),
            "decls": json.dumps(dict(plan["decls"]), sort_keys=True), "error": plan.get("error", "")}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("lib"); ap.add_argument("cidmap"); ap.add_argument("baseline"); ap.add_argument("out")
    ap.add_argument("--only", default=""); ap.add_argument("--orig-lib", default=os.path.expanduser("~/.VidyaGod/LIBRARY"))
    a = ap.parse_args()
    nodes = load(a.lib)
    cmap = json.load(open(a.cidmap))
    idx = [l.rstrip("\n").split("\t") for l in open(os.path.join(a.baseline, "index.tsv"))]
    same = diff = 0
    full, seen = set(), set()            # cards (tile UIDs) some end presents, and those with a full tile (a TITLE) somewhere
    with open(a.out, "w") as o:
        for old, path, status in idx:
            if a.only and a.only not in path:
                continue
            new = cmap.get(old)
            b = json.load(open(os.path.join(a.baseline, "plans", old + ".json")))
            rec = {"end": path, "old": old, "new": new}
            try:
                g9 = R.plan_json(R.Resolver(nodes).resolve(new))
                for lab, e in (g9["exec"] if nodes[new][0].get("VARIANT") else []):   # rows only: a tile node is no card
                    t = e.get("TILE") if isinstance(e, dict) else None
                    if isinstance(t, dict) and t.get("UID") and not e.get("GUEST"):
                        seen.add(t["UID"])
                        if t.get("TITLE"):
                            full.add(t["UID"])
                uid_only = {lab for lab, e in g9["exec"] if isinstance(e.get("TILE"), dict) and set(e["TILE"]) == {"UID"}}
                A = norm(b["plan"], a.orig_lib, cmap, True, uid_only)
                B = norm(g9, a.lib, cmap, False)
                rec["differs"] = [f for f in A if A[f] != B[f]]
                if rec["differs"]:
                    rec["first"] = {f: [str(A[f])[:600], str(B[f])[:600]] for f in rec["differs"][:3]}
            except Exception as ex:
                rec["differs"] = ["exception"]; rec["first"] = repr(ex)[:400]
            same += not rec["differs"]; diff += bool(rec["differs"])
            o.write(json.dumps(rec) + "\n")
    lost = sorted(seen - full)
    print(f"{same} identical, {diff} differ; {len(seen)} cards, {len(lost)} with no full tile on any end"
          + (": " + ", ".join(lost[:20]) if lost else ""))
    return 1 if diff or lost else 0


if __name__ == "__main__":
    sys.exit(main())
