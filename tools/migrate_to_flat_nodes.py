#!/usr/bin/env python3
"""MetaPackageFormat: two-tier (NODE_ID + ordered LAYERS[]) -> flat node DAG.

One node per layer of one TYPE; plural payloads batch WITHIN a type.
13 layer types -> 8.  Order becomes PARENTS edges (mechanical: the array order is
preserved exactly, so the migration cannot change behaviour).  Declare* are lifted:
the library item becomes a ROOT (pure Meta) and the exec becomes the TERMINAL node
with the tile among its parents -- the shape Minecraft/WC3 already use.
"""
import json, os, re, sys, collections

SRC, OUT = sys.argv[1], sys.argv[2]

VFS_FORM = {"VFSZipLayer":"zip", "VFSDirLayer":"dir", "VFSFileLayer":"file", "VFSDeltaLayer":"delta"}
#A TUPLE, not a set: this drives the KEY ORDER of the emitted DeclareLibraryItem, and Python randomises set
#iteration per process (PYTHONHASHSEED). As a set it made the migration non-deterministic - two runs over the
#same input produced the same nodes with their keys in a different order, hence different bytes and different
#CIDs, which is indistinguishable from real content drift.
META_CORE = ("UID", "TITLE", "COVER")

def slug(s):
    return re.sub(r"_+", "_", re.sub(r"[^A-Za-z0-9]+", "_", str(s))).strip("_").lower()

def reg_tree(regpath, keyvalues):
    """'HKLM\\Software\\Ubi Soft\\TONICT' + {Version:1.00} -> nested JSON."""
    parts = [p for p in str(regpath).replace("/", "\\").split("\\") if p]
    node = dict(keyvalues or {})
    for p in reversed(parts[1:]):
        node = {p: node}
    return (parts[0] if parts else "HKLM"), node

def deep_merge(a, b, path=""):
    r"""Merge hive tree b into a. A registry key cannot be both a VALUE and a SUBKEY, and `a[k] = v` silently
    resolved that clash in favour of whoever merged last — destroying the other. In this corpus the only hit is
    a harmless empty parent key subsumed by a deeper one, but the same line would overwrite a real value
    (HKLM\Foo value "Bar" merged with a tree containing HKLM\Foo\Bar\Baz) with no diagnostic at all.
    Report it and keep BOTH sides unresolved rather than guessing."""
    for k, v in b.items():
        here = path + "\\" + k if path else k
        if k in a and isinstance(a[k], dict) and isinstance(v, dict):
            deep_merge(a[k], v, here)
        elif k in a and isinstance(a[k], dict) != isinstance(v, dict):
            # value-vs-subkey collision. An empty {} carries nothing, so subsuming it loses nothing; anything
            # else is real data and the author has to choose.
            losing = a[k] if not isinstance(a[k], dict) else v
            if losing == {} or losing is None:
                if isinstance(v, dict): a[k] = v          # the deeper key wins over "create this key, no values"
            else:
                raise SystemExit(f"registry value/subkey collision at {here!r}: "
                                 f"{a[k]!r} vs {v!r} — one would be destroyed silently. Fix the source package.")
        else:
            a[k] = v
    return a

class Split:
    """One source node -> an ordered chain of flat nodes."""
    def __init__(self, nid):
        self.nid = nid; self.chain = []; self.item = None; self.exec = None
        self.batch = {}          # batch key -> flat node dict (for coalescing)
        self.used = set()        # NODE_IDs already handed out in this chain

    def uid(self, base):
        """A node id is a KEY: two nodes sharing one silently become one node.

        The derived ids are built from a BASENAME (a layer's PATH stem, a patched FILE's name), which is not
        unique: "data/x.zip" and "patch/x.zip" share a stem, "x.zip" and a directory "x" share one, and a
        FileEdit on the same file in both the base and the OVERRIDE pass is two nodes over one name. Suffix
        the repeats instead of emitting a collision."""
        if base not in self.used:
            self.used.add(base); return base
        n = 2
        while f"{base}_{n}" in self.used: n += 1
        self.used.add(f"{base}_{n}"); return f"{base}_{n}"

    def emit(self, node):
        node["NODE_ID"] = self.uid(node["NODE_ID"])
        self.chain.append(node); return node

def convert(nid, j):
    S = Split(nid)
    for l in j.get("LAYERS", []):
        T = l.get("TYPE")

        if T in VFS_FORM:
            stem = os.path.splitext(os.path.basename(str(l.get("PATH", "x"))))[0]
            nd = {"TYPE": "Content", "FORM": VFS_FORM[T], "PATH": l.get("PATH")}
            for k in ("TARGET", "SOURCE", "SUBMOUNTS", "COMMENT", "WHEN"):
                if k in l: nd[k] = l[k]
            if "BASE_TARGET" in l: nd["BASE_TARGET"] = [l["BASE_TARGET"]]     # now arrayable (exposes baseTargets)
            nd["NODE_ID"] = f"{nid}_content_{slug(stem)}"
            S.emit(nd)

        elif T == "RegEdit":
            hive, tree = reg_tree(l.get("REGPATH", ""), l.get("KEYVALUES"))
            entry = {"ARCHITECTURE": [str(l["ARCHITECTURE"])] if l.get("ARCHITECTURE") is not None else [],
                     hive: tree}
            if l.get("OVERRIDE"): entry["OVERRIDE"] = True
            if l.get("WHEN"):     entry["WHEN"]     = l["WHEN"]
            b = S.batch.get("RegEdit")
            if b is None:
                b = S.emit({"NODE_ID": f"{nid}_registry", "TYPE": "RegEdit", "EDITS": []}); S.batch["RegEdit"] = b
            # merge into an existing entry with identical ARCHITECTURE+OVERRIDE
            for e in b["EDITS"]:
                if (e.get("ARCHITECTURE") == entry["ARCHITECTURE"] and e.get("OVERRIDE") == entry.get("OVERRIDE")
                        and e.get("WHEN") == entry.get("WHEN")):
                    deep_merge(e.setdefault(hive, {}), tree); break
            else:
                b["EDITS"].append(entry)

        elif T == "BinaryPatch":
            f = l.get("FILE", ""); key = ("BinaryPatch", f)
            b = S.batch.get(key)
            if b is None:
                b = S.emit({"NODE_ID": f"{nid}_patch_{slug(os.path.basename(f))}",
                            "TYPE": "BinaryPatch", "FILE": f, "EDITS": []}); S.batch[key] = b
            # WHITELISTS ARE A TRAP: a key the engine reads and this list forgets is dropped SILENTLY, across
            # every node at once. Copy everything except the keys the node itself now owns.
            b["EDITS"].append({k: v for k, v in l.items() if k not in ("TYPE", "FILE")})

        elif T == "FileEdit":
            f = l.get("FILE", ""); key = ("FileEdit", f, bool(l.get("OVERRIDE")))
            b = S.batch.get(key)
            if b is None:
                b = {"NODE_ID": f"{nid}_fileedit_{slug(os.path.basename(f))}", "TYPE": "FileEdit", "FILE": f, "EDITS": []}
                if l.get("OVERRIDE"): b["OVERRIDE"] = True
                S.emit(b); S.batch[key] = b
            b["EDITS"].append({k: v for k, v in l.items() if k not in ("TYPE", "FILE", "OVERRIDE")})

        elif T == "DllOverride":
            spec = str(l.get("DLLOVERRIDE", "")); dll, _, order = spec.partition("=")
            # A WHEN splits the batch. OVERRIDES is a MAP with no per-entry condition, so a conditioned layer
            # batched with unconditioned ones would either lose its condition or impose it on all of them.
            # Its own node, with the condition at node level, says exactly what the layer said.
            w = l.get("WHEN") or ""
            key = ("DllOverride", w)
            b = S.batch.get(key)
            if b is None:
                nd = {"NODE_ID": f"{nid}_dlloverrides" + (f"_{slug(w)}" if w else ""),
                      "TYPE": "DllOverride", "OVERRIDES": {}}
                if w: nd["WHEN"] = w
                b = S.emit(nd); S.batch[key] = b
            # An EMPTY order is meaningful: wine reads "dll=" as DISABLED. Defaulting it to "n,b"
            # inverts the setting. Only a spec with no "=" at all has no stated order.
            b["OVERRIDES"][dll] = order if "=" in spec else "n,b"

        elif T == "Persist":
            # Same reasoning as DllOverride: KEEP/DROP are plain string arrays with no per-entry condition.
            w = l.get("WHEN") or ""
            key = ("Persist", w)
            b = S.batch.get(key)
            if b is None:
                nd = {"NODE_ID": f"{nid}_persist" + (f"_{slug(w)}" if w else ""),
                      "TYPE": "Persist", "KEEP": [], "DROP": []}
                if w: nd["WHEN"] = w
                b = S.emit(nd); S.batch[key] = b
            for k in ("KEEP", "DROP"):
                if l.get(k): b[k].append(l[k])

        elif T == "CustomVar":
            nd = {"NODE_ID": f"{nid}_var_{slug(l.get('KEY',''))}", "TYPE": "CustomVar"}
            for k in ("KEY","DEFAULT","COMMENT","WHEN","UI"):
                if k in l: nd[k] = l[k]
            S.emit(nd)

        elif T == "DeclareLibraryItem":
            core = {k: l[k] for k in META_CORE if k in l}
            meta = {k: v for k, v in l.items() if k not in META_CORE and k != "TYPE"}
            if S.item: raise SystemExit(f"{nid}: two DeclareLibraryItem layers - one would be lost")
            S.item = {"NODE_ID": S.uid(f"{nid}_libraryitem"), "TYPE": "DeclareLibraryItem", "PARENTS": [], **core}
            if meta: S.item["META"] = meta

        elif T in ("DeclareExec", "DeclareRunner"):
            if S.exec: raise SystemExit(f"{nid}: two Declare{{Exec,Runner}} layers - one would be lost")
            nd = {"NODE_ID": S.uid(f"{nid}_exec"), "TYPE": "DeclareExec"}
            if T == "DeclareExec":
                nd["HOST"] = l.get("PLATFORM")
                if "CONTENTPATH" in l: nd["PATH"] = l["CONTENTPATH"]
                a = l.get("EXEARGS", [])
                if isinstance(a, list):
                    nd["ARGS"] = a
                elif isinstance(a, str) and a.strip():
                    # The legacy STRING form is space-separated and the resolver splits it; the ARRAY form is
                    # one-element-per-argv and is NOT split. Preserve the semantics, don't just change the type.
                    nd["ARGS"] = a.split()
                else:
                    nd["ARGS"] = []
                for k in ("LABEL","RECOMMENDED","WORKDIR","WHEN","RUNNER"):
                    if k in l: nd[k] = l[k]
            else:
                nd["HOST"] = l.get("HOST"); nd["GUEST"] = l.get("GUEST", [])
                nd["PATH"] = l.get("EXECUTABLE")
                nd["ARGS"] = l.get("ARGS", [])
                if l.get("ENV"): nd["ENV"] = l["ENV"]
                if l.get("REMOVE_ENV"): nd["ENV_REMOVE"] = l["REMOVE_ENV"]
                for k in ("CONTENT_ROOT","PREFIX_GENERATE","UNIFIED_RUNTIME"):
                    if k in l: nd[k] = l[k]
            S.exec = nd
        else:
            raise SystemExit(f"unknown layer TYPE {T!r} in {nid}")
    if not S.chain and not S.item and not S.exec:
        # Pure composition: no payload, only PARENTS. Needs a real node or every referrer loses the edge.
        S.emit({"NODE_ID": nid, "TYPE": "Group"})
    return S

# ---- pass 1: split every node ----
srcnodes, splits, where = {}, {}, {}
for root, _, files in os.walk(SRC):
    for fn in files:
        if not fn.endswith(".json"): continue
        try: j = json.load(open(os.path.join(root, fn)))
        except Exception: continue
        if not isinstance(j, dict) or "NODE_ID" not in j: continue
        j.setdefault("PARENTS", []); j.setdefault("LAYERS", [])
        srcnodes[j["NODE_ID"]] = j
        where[j["NODE_ID"]] = os.path.relpath(root, SRC)
for nid, j in srcnodes.items():
    splits[nid] = convert(nid, j)

def tail(nid):
    S = splits.get(nid)
    if not S: return None
    if S.exec: return S.exec["NODE_ID"]
    if S.chain: return S.chain[-1]["NODE_ID"]
    if S.item: return S.item["NODE_ID"]
    return None

# ---- pass 2: wire edges ----
out_nodes = collections.OrderedDict()
for nid, j in srcnodes.items():
    S = splits[nid]
    ext = [t for t in (tail(p) for p in j["PARENTS"]) if t]
    toggle = ("on" if j.get("DEFAULT", True) else "off") if j.get("OPTIONAL") else None
    prev = None
    for nd in S.chain:
        nd["PARENTS"] = list(ext) if prev is None else [prev]
        prev = nd["NODE_ID"]
        out_nodes[nd["NODE_ID"]] = nd
    if S.item:
        out_nodes[S.item["NODE_ID"]] = S.item
    if S.exec:
        p = ([prev] if prev else list(ext))
        if S.item: p = p + [S.item["NODE_ID"]]
        S.exec["PARENTS"] = p
        out_nodes[S.exec["NODE_ID"]] = S.exec
    t = tail(nid)                                # TOGGLE and EXCLUDE live on the chain's TAIL
    if t and toggle: out_nodes[t]["TOGGLE"] = toggle
    if t and j.get("EXCLUDE"):
        out_nodes[t]["EXCLUDE"] = [x for x in (tail(e) for e in j["EXCLUDE"]) if x]
    # ...and so does anything ELSE the source node carried. A migration that only copies the keys it recognises
    # deletes the author's data silently: the first run of this script dropped MC_VERSION/MC_TYPE/MC_RELEASED
    # from 903 Minecraft nodes, and the layer-level conservation check did not notice because it only counted
    # LAYERS atoms. Carry every unrecognised key forward rather than enumerating what to keep.
    NODE_KEYS_CONSUMED = ("NODE_ID", "PARENTS", "LAYERS", "OPTIONAL", "DEFAULT", "EXCLUDE")
    if t:
        for k, v in j.items():
            if k not in NODE_KEYS_CONSUMED and k not in out_nodes[t]: out_nodes[t][k] = v

# ---- pass 3: write, one file per source node (free-form grouping) ----
written = 0
for nid, j in srcnodes.items():
    S = splits[nid]
    group = [n for n in (S.chain + ([S.item] if S.item else []) + ([S.exec] if S.exec else []))]
    if not group: continue
    d = os.path.join(OUT, where[nid]); os.makedirs(d, exist_ok=True)
    ordered = [{"NODE_ID": n["NODE_ID"], "PARENTS": n.get("PARENTS", []),
                **{k: v for k, v in n.items() if k not in ("NODE_ID", "PARENTS")}} for n in group]
    json.dump(ordered if len(ordered) > 1 else ordered[0],
              open(os.path.join(d, nid + ".json"), "w"), indent=4)
    written += 1
owners = {nid: [n["NODE_ID"] for n in (splits[nid].chain
                                      + ([splits[nid].item] if splits[nid].item else [])
                                      + ([splits[nid].exec] if splits[nid].exec else []))] for nid in srcnodes}

# CONSERVATION CHECK. The first version of this script silently dropped MC_VERSION/MC_TYPE/MC_RELEASED from
# 903 nodes and nothing noticed, because the only check counted LAYER atoms.
#
# PER SOURCE NODE, not globally. A global presence test is covered by duplicates: 42% of this corpus's atom
# instances share a (level, key, value) with another node, so dropping the field from 101 of the 103 nodes
# whose MC_TYPE is "release" reported "0 lost" — the exact regression this check exists for, at 1/9th scale.
# Per-owner presence is still batching-tolerant (batching only ever collapses WITHIN one source node's chain)
# while being blind to nothing across nodes.
def atoms(node):
    """Every (level, key, value) the source node carried.

    LEVEL-AWARE: counting node-level and layer-level keys in one bucket made DEFAULT, FILE and OVERRIDE
    collide, and the only way to keep the check green was to exempt those keys wholesale — surrendering 1186
    atoms. Separating the levels lets all three stay guarded."""
    out = set()
    for k, v in node.items():
        # NODE_ID is deliberately reshaped: one source node becomes a CHAIN of ids.
        if k in ("NODE_ID", "PARENTS", "LAYERS"): continue
        out.add(("node", k, json.dumps(v, sort_keys=True)))
    for l in node.get("LAYERS", []):
        for k, v in l.items():
            if k == "TYPE": continue
            out.add(("layer", k, json.dumps(v, sort_keys=True)))
    return out

def emitted(ids):
    """Every atom the flat nodes produced FROM one source node carry."""
    out = set()
    def credit(k, v):
        d = json.dumps(v, sort_keys=True)
        out.add(("node", k, d)); out.add(("layer", k, d))
    for nid in ids:
        n = out_nodes.get(nid)
        if not n: continue
        for k, v in n.items():
            if k in ("NODE_ID", "PARENTS", "TYPE"): continue
            credit(k, v)
            if isinstance(v, list):                      # batched EDITS / KEEP / DROP
                for e in v:
                    if isinstance(e, dict):
                        for ek, ev in e.items(): credit(ek, ev)
                    else: credit(k, e)
            if isinstance(v, dict):                      # OVERRIDES / META / hive trees
                for ek, ev in v.items(): credit(ek, ev)
    return out

# Keys the flat schema genuinely RESHAPES — a value moves into a tree, a name changes, a string becomes an
# array — so they cannot be matched atom-for-atom. Deliberately MINIMAL: derived by narrowing until the check
# passed on the real corpus. Every key NOT here must survive verbatim.
RESHAPED = {"EXECUTABLE", "REMOVE_ENV", "ENV", "DLLOVERRIDE", "REGPATH", "ARCHITECTURE",
            "KEYVALUES", "PLATFORM", "EXEARGS", "CONTENTPATH", "OPTIONAL"}
# Two atoms are CONSUMED rather than copied, exempt by exact VALUE rather than by key (exempting the key would
# have surrendered 1108 DEFAULTs and 19 OVERRIDEs with them):
#   node DEFAULT:true/false -> folded into TOGGLE together with OPTIONAL (BOTH values: the corpus this was
#                              derived from has only `true`, so listing only that would abort a re-run against
#                              any newer snapshot — the live library already has an off-by-default module)
#   layer OVERRIDE:false -> the default; the migration writes OVERRIDE only when true
CONSUMED = {("node", "DEFAULT", "true"), ("node", "DEFAULT", "false"), ("layer", "OVERRIDE", "false")}

lost, checked = [], 0
for nid, j in srcnodes.items():
    have_here = emitted(owners.get(nid, []))
    for a in atoms(j):
        if a[1] in RESHAPED or a in CONSUMED: continue
        checked += 1
        if a not in have_here: lost.append((nid, a))
if lost:
    by_key = collections.Counter(a[1] for _, a in lost)
    for nid, a in lost[:8]: print(f"  LOST [{a[0]}] {a[1]} = {a[2][:70]}   (from {nid})", file=sys.stderr)
    for k, n in by_key.most_common(10): print(f"  LOST {k} x{n}", file=sys.stderr)
    raise SystemExit(f"conservation check FAILED: {len(lost)} atom instance(s) did not survive")
print(f"conservation: {checked} atom instance(s) checked across {len(srcnodes)} source node(s), 0 lost")

# REGISTRY CONSERVATION. REGPATH/KEYVALUES/ARCHITECTURE are on the RESHAPED list because the flat schema turns
# a flat key path plus a value map into a nested hive TREE — so they cannot be matched atom-for-atom, and the
# atom check above says nothing at all about them. That is the payload this script reshapes MOST (reg_tree +
# deep_merge, which can even collide a value against a subkey), so it is the last thing that should be taken
# on trust. Flatten BOTH sides to (hive-path, value-name, value, arch, override, when) and compare those.
def src_reg(node):
    out = collections.Counter()
    for l in node.get("LAYERS", []):
        if l.get("TYPE") != "RegEdit": continue
        path = str(l.get("REGPATH", "")).replace("/", "\\").strip("\\")
        arch = str(l["ARCHITECTURE"]) if l.get("ARCHITECTURE") is not None else ""
        kv = l.get("KEYVALUES") or {}
        common = (path, arch, bool(l.get("OVERRIDE")), l.get("WHEN") or "")
        if not kv: out[(path, "", None) + common[1:]] += 1          # create-key-only
        for n, v in kv.items(): out[(path, n, json.dumps(v, sort_keys=True)) + common[1:]] += 1
    return out

def flat_reg(ids):
    out = collections.Counter()
    def walk(path, tree, arch, ov, wh):
        vals = {k: v for k, v in tree.items() if not isinstance(v, dict)}
        subs = {k: v for k, v in tree.items() if isinstance(v, dict)}
        if vals or (not subs and not tree):
            if not vals: out[(path, "", None, arch, ov, wh)] += 1
            for n, v in vals.items(): out[(path, n, json.dumps(v, sort_keys=True), arch, ov, wh)] += 1
        for k, v in subs.items(): walk(path + "\\" + k, v, arch, ov, wh)
    for nid in ids:
        n = out_nodes.get(nid)
        if not n or n.get("TYPE") != "RegEdit": continue
        for e in n.get("EDITS", []):
            arches = [str(a) for a in e.get("ARCHITECTURE", [])] or [""]
            for a in arches:
                for hive, tree in e.items():
                    if hive in ("ARCHITECTURE", "OVERRIDE", "WHEN") or not isinstance(tree, dict): continue
                    walk(hive, tree, a, bool(e.get("OVERRIDE")), e.get("WHEN") or "")
    return out

reg_lost, reg_checked = [], 0
for nid, j in srcnodes.items():
    want_r, have_r = src_reg(j), flat_reg(owners.get(nid, []))
    for k, c in want_r.items():
        reg_checked += c
        if have_r[k] < c: reg_lost.append((nid, k))
if reg_lost:
    # The one benign case is a key with NO VALUES subsumed by a deeper key that creates it anyway. It is
    # distinguished by its VALUE being None, not by its NAME being empty: the empty NAME is the Windows
    # DEFAULT value (@=), and LAVFilters alone has 48 of them carrying the whole DirectShow COM registration.
    # Keying the exemption on the name exempted every one of those — losing them silently breaks every codec.
    real = [(nid, k) for nid, k in reg_lost if k[1] != "" or k[2] is not None]
    for nid, k in reg_lost[:8]: print(f"  REG LOST {k[0]}\\{k[1]} arch={k[3]} (from {nid})", file=sys.stderr)
    if real: raise SystemExit(f"registry conservation FAILED: {len(real)} value(s) did not survive")
    print(f"registry: {reg_checked} entries checked, {len(reg_lost)} empty parent key(s) subsumed by a deeper one")
else:
    print(f"registry: {reg_checked} entries checked, 0 lost")

print(f"source nodes {len(srcnodes)}  ->  flat nodes {len(out_nodes)}   ({written} files)")
json.dump({n: v for n, v in out_nodes.items()}, open(os.path.join(OUT, "_index.json"), "w"))
json.dump(owners, open(os.path.join(OUT, "_owners.json"), "w"))
