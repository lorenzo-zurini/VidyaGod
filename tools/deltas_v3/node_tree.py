#!/usr/bin/env python3
"""PROTOTYPE (USER 2026-11-28: "if this tree delta's tree is extended to include the other parts of a node's tree...
it also replaces TAKE and composition"): a composed node written out as ONE readable table, every address it ends up
with and where its value comes from — and which of them are fixed at mint (STATIC: the tree can hold them) and which
are decided at launch (DYNAMIC: options, %variables%, ticked parts — what still needs the recipe).

    node_tree.py <end label or CID> [--part <label>]... [--out TREE.txt]

Read-only: resolves with tools/gen6/resolve.py over the live library, lists each content layer's files (zips; a delta
chain's rebuilt zip through vgdelta.py), applies placement, TAKE views and later-wins, and what a delta hides.
"""
import io, json, os, re, sys, zipfile
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "gen6"))
from resolve import load_nodes, Resolver, graft_index, offered_grafts, place, ns_path
from vgdelta import source_of, Reader

TOK = re.compile(r"%[A-Za-z_][A-Za-z0-9_:.-]*%")
ANCHORS = {"GameDir", "UserProfile", "AppData", "LocalAppData", "Documents", "PrefixRoot", "Media", "SaveDir"}
def var_in(s):
    """A %variable% that is not an anchor (an anchor is where the layout puts things: fixed per machine)."""
    return any(t[1:-1].split(":")[0] not in ANCHORS for t in TOK.findall(s))


def members(it, by_source, nodes):
    """The member names of a content item (zip; delta: its rebuilt zip; file: itself)."""
    path = os.path.join(it["dir"], it["payload"])
    if it["kind"] == "ZIP":
        with zipfile.ZipFile(path) as z: return [n for n in z.namelist() if not n.endswith("/")]
    if it["kind"] == "FILE": return [it["payload"]]
    if it["kind"] == "DELTA":
        def src(item):
            p = os.path.join(item["dir"], item["payload"])
            if item["kind"] != "DELTA": return p
            return (p, tuple(src(by_source[b]) for b in item.get("base_sources", []) if b in by_source))
        with zipfile.ZipFile(io.BufferedReader(Reader(source_of(src(it))))) as z:
            return [n for n in z.namelist() if not n.endswith("/")]
    return []


def main():
    a = sys.argv[1:]
    want, parts, out = a[0], [a[i + 1] for i, x in enumerate(a) if x == "--part"], a[a.index("--out") + 1] if "--out" in a else None
    L = os.path.realpath(os.path.expanduser("~/.VidyaGod/LIBRARY"))
    nodes = load_nodes(L)
    end = want if want in nodes else next(c for c, (n, d) in nodes.items() if n.get("LABEL") == want and n.get("VARIANT"))
    label = lambda c: nodes[c][0].get("LABEL", c[:12]) if c in nodes else c[:12]
    R, gi = Resolver(nodes), graft_index(nodes)
    p0 = R.resolve(end)
    face = next((e["TILE"]["UID"] for e in p0["exec"].values() if not e.get("GUEST") and e.get("TILE")), None)
    ticked = list(offered_grafts(nodes, gi, p0, face)[1])
    ticked += [c for c, (n, d) in nodes.items() if n.get("LABEL") in parts and c not in ticked]
    P = R.resolve(end, {}, {}, ticked)
    by_source = {it["source"]: it for it in P["seq"] if it.get("source")}
    rows, static, dynamic = [], 0, 0
    def row(kind, addr, how, dyn):
        nonlocal static, dynamic
        rows.append((addr, f"{kind}\t{addr}\t{how}\t{'DYNAMIC' if dyn else 'static'}"))
        if dyn: dynamic += 1
        else: static += 1
    # ---- files: later wins; a delta hides what its bases put at its place
    files = {}                                                   # path (lower) → (shown path, how, from item index)
    for k, it in enumerate(P["seq"]):
        if it["kind"] == "EDIT": continue
        if it["kind"] == "DELTA":                                # what it was built over goes, at its place
            hid = set(it.get("base_sources", []))
            for f in [f for f, v in files.items() if P["seq"][v[2]].get("source") in hid]: del files[f]
        try: ms = members(it, by_source, nodes)
        except Exception as e: rows.append(("~", f"#\tcould not list {it['payload']}: {e}")); continue
        for m in ms:
            if it["view"]:                                       # TAKE steps, innermost first: each filters/renames, then places
                ad = "FILES/" + "/".join(x for x in (it.get("own", ""), m) if x)
                for take, at in it["view"]:
                    ad = R._taken(take, ad)
                    if ad is None: break
                    if at: ad = "FILES/" + place(ns_path(ad)[1], at)
                if ad is None: continue
                path = ad[6:]
            else:
                path = "/".join(x for x in (it["target"], m) if x)
            files[path.lower()] = (path, f"same\t{it['kind'].lower()} '{it['payload']}' of {label(it['from'])}: {m}", k)
    edits = {}
    for it in P["seq"]:
        if it["kind"] == "EDIT":
            L0 = nodes[it["from"]][0]["LAYERS"][it["at"]] if it["from"] in nodes else {}
            dyn = "WHEN" in L0 or var_in(json.dumps(it["ops"]))
            edits.setdefault(it["target"].lower(), []).append((len(it["ops"]), label(it["from"]), dyn))
    for f, (path, how, k) in sorted(files.items()):
        e = edits.pop(f, None)
        if e: row("file", path, how + f" + {sum(n for n, _, _ in e)} edit op(s) from " + ", ".join(sorted({l for _, l, _ in e})), any(d for *_, d in e) or var_in(path))
        else: row("file", path, how, var_in(path))
    for f, e in sorted(edits.items()): row("file", f, f"edited, its file from elsewhere ({sum(n for n, _, _ in e)} ops)", any(d for *_, d in e))
    # ---- the registry, DLLs, environment, variables, entries, keeps
    for (arch, kp, vn), (dp, dn, v) in sorted(P["reg"].items(), key=lambda x: (x[0][0] or "", x[0][1], x[0][2])):
        row("reg", f"{dp}#{dn}" + (f" [{arch}]" if arch else ""), f"= {json.dumps(v)}", var_in(str(v)))
    for (arch, kp), dp in sorted(P["regkeys"].items(), key=lambda x: (x[0][0] or "", x[0][1])): row("regkey", dp, "created", False)
    for k, v in P["dll"].items(): row("dll", k, f"= {v}", var_in(str(v)))
    for k, v in P["env"].items(): row("env", k, f"= {json.dumps(v)}", var_in(str(v)))
    for k, v in P["exec"].items(): row("exec", k, json.dumps(v, sort_keys=True)[:160], var_in(json.dumps(v)))
    for k, v in P["keep"].items(): row("keep", k, json.dumps(v), False)
    for k, v in P["decls"].items():
        opt = isinstance(v, dict) and isinstance(v.get("UI"), dict)
        row("var", k, (f"option '{v['UI'].get('LABEL', k)}' " if opt else "") + f"default {json.dumps(v.get('DEFAULT') if isinstance(v, dict) else v)}", opt)
    # ---- what is decided at launch: layers whose presence a condition on variables decides
    gated = []
    for c in P["order"]:
        for i, L0 in enumerate(nodes[c][0].get("LAYERS", []) if c in nodes else []):
            if isinstance(L0, dict) and "WHEN" in L0 and "%" in str(L0["WHEN"]):
                gated.append(f"gated\t{label(c)} layer {i}\tWHEN {L0['WHEN']}\tDYNAMIC")
    offered = [label(g) for g in offered_grafts(nodes, gi, p0, face)[0]]
    head = [f"VGNODE 1 (prototype)\t{label(end)} + parts ticked: {', '.join(label(t) for t in ticked) or 'none'}",
            f"# offered parts (a launch-time choice): {', '.join(offered) or 'none'}"]
    text = "\n".join(head + [r for _, r in rows] + gated) + "\n"
    if out: open(out, "w").write(text)
    print(json.dumps({"end": label(end), "parts": [label(t) for t in ticked], "addresses": len(rows), "static": static,
                      "dynamic": dynamic + len(gated), "option_gated_layers": len(gated), "files": sum(1 for r in rows if r[1].startswith("file")),
                      "out": out}, indent=1))


if __name__ == "__main__":
    main()
