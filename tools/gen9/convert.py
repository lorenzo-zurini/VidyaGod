#!/usr/bin/env python3
"""Gen 8 → gen 9 node conversion (MetaPackageFormat gen 9; run ONCE over the library, then retired).

  convert.py <LIBRARY> <OUT> --baseline <dir>     node JSONs only, mirrored under OUT (content stays where it is)

Per node, in dependency order (children first, so references can be renamed to their new CIDs):
  - REG: the nested hive tree becomes a path tree {"HKCU\\\\Software\\\\X": {value: data}} (07);
  - EXEC: a list of entries becomes an object keyed by LABEL (09); LABEL leaves the entry;
  - RECOMMENDED: a part's moves onto its INTO layers ("RECOMMENDED": true); an end's onto the entries whose tile it names;
  - KEEP / TAKE registry addresses: re-spelled with backslash key paths; a KEEP address naming a value becomes the
    value-level form {"REG/<key>": {value: bool}} (13), decided from the keys the library's REG layers write;
  - entries filled whole (nothing folds): where the gen 9 composition of a node's entry differs from the gen 8 folded
    entry (the baseline), the node states that entry whole;
  - NODE / INTO / WHEN references renamed to the converted children's CIDs.
Reports to OUT/convert_report.json. Deterministic."""
import argparse, copy, json, os, re, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "gen6"))
import resolve as R

CONTENT = R.CONTENT


def reg_paths(tree, path=""):
    """A gen 8 nested REG tree → the gen 9 path tree (insertion order kept)."""
    out = {}
    def walk(t, p):
        vals, subs = {}, []
        for k, v in t.items():
            if isinstance(v, dict):
                subs.append((k, v))
            elif v is None and isinstance(t, dict) and p and False:
                pass
            else:
                vals[k] = v
        if p:
            if vals or (not subs and not t):
                out.setdefault(p, {}).update(vals)
        for k, v in subs:
            q = k if not p else (p if k == "" else p + "\\" + k)   # a subkey named "" is the key itself (Skyrim SE)
            if v == {}:
                out.setdefault(q, {})
            else:
                walk(v, q)
    # a null at a key position deletes the key: {"HKCU": {"T": null}} → {"HKCU\\T": null}
    def walk_nulls(t, p):
        for k, v in t.items():
            q = k if not p else (p if k == "" else p + "\\" + k)   # a subkey named "" is the key itself (Skyrim SE)
            if isinstance(v, dict):
                walk_nulls(v, q)
    for hive, sub in tree.items():
        if sub is None:
            out[hive] = None
        elif isinstance(sub, dict):
            walk(sub, hive)
            # nulls one level down that are keys (a key deleted): gen 8 spelled {"HKCU": {"T": null}} — a null VALUE
            # named T of HKCU is indistinguishable; gen 8's fold treated a null under a key as "delete the value k AND
            # the key p\\k with all below it". Keep both meanings: the value null stays in its key's values, and the
            # key path gets a null entry too.
            def nulls(t, p):
                for k, v in t.items():
                    q = p + "\\" + k
                    if v is None:
                        out[q] = None
                    elif isinstance(v, dict):
                        nulls(v, q)
            nulls(sub, hive)
    return out


def known_keys(nodes):
    """Every registry key path (lower) the library's REG layers write, and every (key, value) pair."""
    keys, vals = set(), set()
    for n, _ in nodes.values():
        for L in n.get("LAYERS", []):
            if isinstance(L, dict) and isinstance(L.get("REG"), dict):
                for p, vs in reg_paths(L["REG"]).items():
                    pl = p.lower()
                    # the key and every key above it: an address naming an ancestor of a written key is a KEY, never
                    # a value of its parent (Black & White's ...\Audio, whose values live in Audio\Override)
                    parts = pl.split("\\")
                    for i in range(1, len(parts) + 1):
                        keys.add("\\".join(parts[:i]))
                    for v in (vs or {}):
                        vals.add((pl, v.lower()))
    return keys, vals


def reg_addr(addr, keys, vals):
    """A gen 8 REG address ('REG/HKCU/Software/X/1.0[/value]', '/' between keys) → (key path, value name or None).
    Key names may hold '/' (MIME types, URLs): the split is chosen to match the keys the library writes."""
    segs = addr[4:].split("/")
    # try every way of joining segments with '\\' or '/', preferring known keys; keep it small (≤ 2^12 joins)
    def joins(ss):
        if len(ss) == 1:
            yield ss[0]; return
        if len(ss) > 13:
            yield "\\".join(ss); return
        for rest in joins(ss[1:]):
            yield ss[0] + "\\" + rest
            yield ss[0] + "/" + rest
    for j in joins(segs):
        if j.lower() in keys:
            return j, None
    if len(segs) > 1:
        for j in joins(segs[:-1]):
            if j.lower() in keys and (j.lower(), segs[-1].lower()) in vals:
                return j, segs[-1]
        for j in joins(segs[:-1]):
            if j.lower() in keys:
                return j, segs[-1]
    return "\\".join(segs), None


def conv_take(take, keys, vals):
    def one(a):
        if isinstance(a, str) and a.startswith("REG/"):
            k, v = reg_addr(a, keys, vals)
            return "REG/" + k + (("\\" + v) if v else "")
        return a
    out = []
    for sel in take:
        out.append([one(sel[0]), one(sel[1])] if isinstance(sel, list) else one(sel))
    return out


DEFAULT_RENAMED = []   # (node label, key path): a value named "@" that is the key's default value
REC_UNPLACED = []      # (node label, UIDs): a part with an entry recommended on cards it does not present


def convert_node(n, keys, vals):
    """The syntactic conversion of one node (references untouched)."""
    n = copy.deepcopy(n)
    rec = n.pop("RECOMMENDED", None)
    is_part = bool(R.into_layers(n.get("LAYERS", [])))
    for L in n.get("LAYERS", []):
        if not isinstance(L, dict):
            continue
        if isinstance(L.get("REG"), dict):
            L["REG"] = reg_paths(L["REG"])
            # A key's default value is the EMPTY name (what .reg files spell @=, and what the registry writer reads);
            # 12 hand-written gen 8 nodes named it "@", which wrote a value literally called @ (EA ergc CD keys among
            # them). Renamed here, where the key has no "" already.
            for path, vs in L["REG"].items():
                if isinstance(vs, dict) and "@" in vs and "" not in vs:
                    L["REG"][path] = {("" if k == "@" else k): v for k, v in vs.items()}
                    DEFAULT_RENAMED.append((n.get("LABEL"), path))
        if isinstance(L.get("EXEC"), list):
            ex = {}
            for e in L["EXEC"]:
                e = dict(e); lab = e.pop("LABEL")
                # the entry presenting a card the node was recommended on: its card's recommended row — for an end and
                # for a part with an entry alike (a part with an entry is a row: never ticked, chapter 09 §9.4)
                if rec and isinstance(e.get("TILE"), dict) and e["TILE"].get("UID") in rec and not e.get("GUEST"):
                    e["RECOMMENDED"] = True
                ex[lab] = e
            L["EXEC"] = ex
        if isinstance(L.get("KEEP"), dict):
            kp = {}
            for a, v in L["KEEP"].items():
                if a.startswith("REG/"):
                    k, vn = reg_addr(a, keys, vals)
                    # a value is only ever taken BACK (false) inside a kept key (13); anything kept is a key — a key
                    # the game creates at run time is unknown to the library (Rise of Nations' ...\RiseOfNations)
                    if vn and v is not False:
                        k, vn = k + "\\" + vn, None
                    kp["REG/" + k + ("#" + vn if vn else "")] = v   # a value: <key>#<value> (13)
                else:
                    kp[a] = v
            L["KEEP"] = kp
        if "TAKE" in L and "NODE" in L:
            L["TAKE"] = conv_take(L["TAKE"], keys, vals)
        if "TAKE" in L and "INTO" in L:
            L["TAKE"] = conv_take(L["TAKE"], keys, vals)
    # a plain part (no entry of its own) recommended: ticked by default, on its INTO layer(s)
    own_entry = any(isinstance(L, dict) and isinstance(L.get("EXEC"), dict) and
                    any(isinstance(e, dict) and not e.get("GUEST") for e in L["EXEC"].values()) for L in n.get("LAYERS", []))
    if rec and is_part and not own_entry:
        for L in R.into_layers(n["LAYERS"]):
            L["RECOMMENDED"] = True
    elif rec and is_part and not any(isinstance(L, dict) and isinstance(L.get("EXEC"), dict) and
                                     any(isinstance(e, dict) and e.get("RECOMMENDED") for e in L["EXEC"].values())
                                     for L in n.get("LAYERS", [])):
        # a part with an entry that presents none of the cards it was recommended on: no gen 9 place for the mark (a
        # row must present its card) — reported for its package's wave, never dropped silently
        REC_UNPLACED.append((n.get("LABEL"), rec))
    return n, rec


def regroup_reg(n, depth=4):
    """REG layers split by their keys' first `depth` segments, each under its own KEY (07): a registry dump's shared
    prefixes written once. Writes keep their order inside each group."""
    out = []
    for L in n["LAYERS"]:
        if not (isinstance(L, dict) and isinstance(L.get("REG"), dict) and not L.get("KEY") and len(L["REG"]) > 8):
            out.append(L); continue
        groups = {}
        if depth == 0:                                    # adaptive: the deepest prefix at least 16 keys share
            cnt = {}
            for p in L["REG"]:
                segs = p.split("\\")
                for d in range(2, len(segs)):
                    cnt["\\".join(segs[:d])] = cnt.get("\\".join(segs[:d]), 0) + 1
        for p, v in L["REG"].items():
            segs = p.split("\\")
            if depth == 0:
                key = next(("\\".join(segs[:d]) for d in range(len(segs) - 1, 1, -1) if cnt.get("\\".join(segs[:d]), 0) >= 16),
                           "\\".join(segs[:2]) if len(segs) > 2 else segs[0])
            else:
                key = "\\".join(segs[:depth]) if len(segs) > depth else "\\".join(segs[:-1]) or segs[0]
            rel = p[len(key):].lstrip("\\")
            groups.setdefault(key, {})[rel] = v
        for key, tree in groups.items():
            M = {k: v for k, v in L.items() if k not in ("REG",)}
            M["KEY"] = key; M["REG"] = tree
            out.append(M)
    n = dict(n); n["LAYERS"] = out
    return n


def split_reg(n, limit=200 * 1024):
    """A node over one chunk after conversion: its REG layers move, in order, into part nodes (each under `limit`), and
    the node contains them where its first REG layer was. The same facts in the same order (05: a contained node's
    layers apply where it is listed)."""
    regs = [L for L in n["LAYERS"] if isinstance(L, dict) and "REG" in L]
    parts, cur = [], []
    for L in regs:
        if cur and len(R.canonical({"LABEL": "x", "LAYERS": cur + [L]})) > limit:
            parts.append(cur); cur = []
        cur.append(L)
    if cur:
        parts.append(cur)
    lab = n.get("LABEL", "node")
    nodes = [{"LABEL": f"{lab} (registry part {i + 1})", "LAYERS": ls} for i, ls in enumerate(parts)]
    out, placed = [], False
    for L in n["LAYERS"]:
        if isinstance(L, dict) and "REG" in L:
            if not placed:
                out += [{"NODE": R.cid_of(R.canonical(x))} for x in nodes]; placed = True
            continue
        out.append(L)
    m = dict(n); m["LAYERS"] = out
    return m, nodes


def refs_rename(n, m):
    return R.remap_node(n, m)


def topo(nodes):
    """Children before parents (NODE + INTO + WHEN references); cycles broken by first visit."""
    order, state = [], {}
    def visit(c):
        if state.get(c) == 2 or c not in nodes:
            return
        if state.get(c) == 1:
            return                                        # a cycle: its back edge skipped (the resolver does the same)
        state[c] = 1
        for _, r in R.node_refs(nodes[c][0]):
            visit(r)
        state[c] = 2; order.append(c)
    for c in sorted(nodes):
        visit(c)
    return order


def norm_entry(e):
    e = {k: v for k, v in e.items() if k not in ("LABEL", "RECOMMENDED")}
    return e


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("lib"); ap.add_argument("out"); ap.add_argument("--baseline", required=True)
    a = ap.parse_args()
    lib = os.path.abspath(a.lib)
    nodes = {}                                            # old cid -> (node, dir, path)
    for dp, dn, fns in os.walk(lib):
        dn[:] = sorted(d for d in dn if not d.startswith("_peer_"))
        for fn in sorted(fns):
            if fn.endswith(".json"):
                p = os.path.join(dp, fn)
                try: j = json.load(open(p))
                except Exception: continue
                if isinstance(j, dict) and isinstance(j.get("LAYERS"), list):
                    nodes[j.get("CID") or R.cid_of(R.canonical(j))] = (j, dp, p)
    keys, vals = known_keys({c: (j, d) for c, (j, d, p) in nodes.items()})
    order = topo({c: (j, d) for c, (j, d, p) in nodes.items()})
    newcid, newnodes, report = {}, {}, {"filled": [], "recommended_moved": 0, "reg_layers": 0, "errors": [], "uid_only_rows": 0}
    # D8: the cards some row is recommended on (gen 8: a variant's node-level RECOMMENDED); on those, every other row's
    # tile is a reference, {UID}. A card no row recommends keeps its rows' tiles (the validator reports it: wave 1).
    rec_cards = set()
    for c, (j, d, p) in nodes.items():
        if j.get("VARIANT") and isinstance(j.get("RECOMMENDED"), list):     # a row (a part that is a version too)
            rec_cards.update(u for u in j["RECOMMENDED"] if isinstance(u, str))
    for c in order:
        j, d, p = nodes[c]
        n, rec = convert_node(j, keys, vals)
        n = refs_rename(n, newcid)
        if rec and not any(x[0] == n.get("LABEL") for x in REC_UNPLACED[-1:]): report["recommended_moved"] += 1
        # nothing folds: fill the node's entries where the gen 9 composition differs from the gen 8 fold (baseline)
        bpath = os.path.join(a.baseline, "plans", c + ".json")
        is_row = bool(j.get("VARIANT"))
        base = (json.load(open(bpath)).get("plan") or {}) if os.path.exists(bpath) else {}
        def game_tiles(es):
            return [e["TILE"].get("UID") for lab, e in es if isinstance(e.get("TILE"), dict) and not e.get("GUEST")]
        # a row writes its own entries (whole EXEC per end) where it must say something of its own: the card it is
        # the recommended row of, or (D8) the {UID} reference on a card another row recommends
        must_own = is_row and any(u in (rec or []) or u in rec_cards for u in game_tiles(base.get("exec", [])))
        if base and must_own and not any(isinstance(L, dict) and "EXEC" in L for L in n["LAYERS"]):
            n["LAYERS"].append({"EXEC": {}})
        if base and any(isinstance(L, dict) and "EXEC" in L for L in n["LAYERS"]):
            want = {lab: norm_entry(e) for lab, e in base.get("exec", [])}
            lib_view = dict(newnodes); lib_view["__self__"] = (n, d)
            try:
                got = R.Resolver(lib_view).resolve("__self__")["exec"]
            except Exception as ex:
                report["errors"].append({"node": p, "error": repr(ex)[:300]}); got = {}
            own = [L for L in n["LAYERS"] if isinstance(L, dict) and "EXEC" in L][-1]   # (the last: it is written last)
            for lab, e in want.items():
                t = e.get("TILE") if isinstance(e.get("TILE"), dict) else None
                is_rec = bool(rec and t and not e.get("GUEST") and t.get("UID") in rec)
                if (is_row and t and not e.get("GUEST") and t.get("UID") in rec_cards and not is_rec and set(t) != {"UID"}):
                    # a row that is not its card's recommended one: its own whole entry, the tile a reference (D8)
                    e = dict(copy.deepcopy(e), TILE={"UID": t["UID"]})
                    report["uid_only_rows"] += 1
                if is_rec and own["EXEC"].get(lab, {}).get("RECOMMENDED") is not True:
                    # the recommended row's entry, inherited: written here, whole, with the mark
                    own["EXEC"][lab] = dict(copy.deepcopy(e), RECOMMENDED=True)
                    report["filled"].append({"node": os.path.relpath(p, lib), "label": lab, "why": "recommended"})
                    continue
                if norm_entry(got.get(lab, {})) != e:
                    old_rec = own["EXEC"].get(lab, {}).get("RECOMMENDED")
                    own["EXEC"][lab] = copy.deepcopy(e)
                    if old_rec or (rec and isinstance(e.get("TILE"), dict) and e["TILE"].get("UID") in rec and not e.get("GUEST")):
                        own["EXEC"][lab]["RECOMMENDED"] = True
                    report["filled"].append({"node": os.path.relpath(p, lib), "label": lab})
        if len(R.canonical(n)) > 200 * 1024:                # a big registry dump: its paths under shared KEYs
            n = min((regroup_reg(n, d) for d in (0, 3, 4, 5, 6, 7)), key=lambda x: len(R.canonical(x)))
        if len(R.canonical(n)) > 250 * 1024:                # still over one chunk: its REG layers into part nodes, in order
            n, parts = split_reg(n)
            for i, part in enumerate(parts):
                pc = R.cid_of(R.canonical(part))
                newnodes[pc] = (part, d)
                rel = os.path.relpath(p, lib)[:-5] + f" (registry part {i + 1}).json"
                os.makedirs(os.path.join(a.out, os.path.dirname(rel)), exist_ok=True)
                open(os.path.join(a.out, rel), "wb").write(R.canonical(part))        # a node file is its canonical bytes
            report.setdefault("split", []).append({"node": os.path.relpath(p, lib), "parts": len(parts)})
        try:
            nc = R.cid_of(R.canonical(n))
        except ValueError:
            report["errors"].append({"node": os.path.relpath(p, lib), "error": "over 256 KiB after conversion",
                                     "bytes_before": len(R.canonical(j)), "bytes_after": len(R.canonical(n))})
            nc = "oversize-" + c
        newcid[c] = nc; newnodes[nc] = (n, d)
        rel = os.path.relpath(p, lib)
        os.makedirs(os.path.join(a.out, os.path.dirname(rel)), exist_ok=True)
        open(os.path.join(a.out, rel), "wb").write(R.canonical(n))           # a node file is its canonical bytes (02 §2.5)
    json.dump(newcid, open(os.path.join(a.out, "cid_map.json"), "w"), indent=0, sort_keys=True)
    report["nodes"] = len(order); report["filled_count"] = len(report["filled"])
    report["default_value_renamed"] = [list(x) for x in DEFAULT_RENAMED]
    report["recommended_unplaced"] = [list(x) for x in REC_UNPLACED]
    json.dump(report, open(os.path.join(a.out, "convert_report.json"), "w"), indent=1)
    print(f"{len(order)} nodes converted; {len(report['filled'])} entries filled; {report['uid_only_rows']} rows' tiles cut to {{UID}}; "
          f"{report['recommended_moved']} RECOMMENDED moved; {len(report['errors'])} errors")


if __name__ == "__main__":
    main()
