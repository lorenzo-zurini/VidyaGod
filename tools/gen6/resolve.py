#!/usr/bin/env python3
"""Generation 6 reference resolver (spec final_v2_final_this_time_v3 §4) — the executable definition the C++ engine
is gated against. Pure: node files in, an effective plan out. Nothing is mounted, fetched or written.

  plan = resolve(nodes, root_cid, instance_vars, builtins)
    ["seq"]     every applied content/EDIT layer, in fold order (bottom → top): {"kind", ...}
    ["reg"]     {(arch, key-path lower, value-name lower): (display path, name, value)}; keys {(arch, path lower)}
    ["dll"]     {name lower: order}
    ["env"]     {name: value | None}
    ["exec"]    {label: entry}, in first-declared order
    ["keep"]    {address: value}
    ["decls"]   {key: declaration}, the folded VARS
    ["order"]   the occurrences, in expansion order (node CIDs)
    ["events"]  moves / held mentions / cycles / unmet ANY / NOT hits

Usage (debug): resolve.py <library-root> <cid> [--var K=V ...]
"""
import copy, json, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

TYPE_KEYS = ["ZIP", "FILE", "DELTA", "DIR", "NODE", "EDIT", "REG", "VARS", "ENV", "DLL", "EXEC", "KEEP", "ANY", "NOT"]
CONTENT = ("ZIP", "FILE", "DELTA", "DIR")


def type_of(L):
    ks = [k for k in TYPE_KEYS if k in L]
    if len(ks) != 1:
        raise ValueError(f"layer with type keys {ks}: {L}")
    return ks[0]


def canonical(node):
    """The frozen form's bytes (§1.7): UTF-8, keys sorted, arrays in order, no whitespace, no CID/POS."""
    body = {k: v for k, v in node.items() if k not in ("CID", "POS")}
    return json.dumps(body, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def cid_of(data):
    """Kubo-parity file CID of ≤ 256 KiB bytes: CIDv1, raw codec, sha2-256, base32 multibase."""
    import base64, hashlib
    if len(data) > 256 * 1024:
        raise ValueError("node larger than one chunk — needs the UnixFS chunked form")
    raw = bytes([0x01, 0x55, 0x12, 0x20]) + hashlib.sha256(data).digest()
    return "b" + base64.b32encode(raw).decode().lower().rstrip("=")


def load_nodes(root):
    """cid handle -> (node, bundle dir)."""
    out = {}
    for dp, dn, fns in os.walk(root):
        dn[:] = [d for d in dn if not d.startswith("_friend_")]
        for fn in fns:
            if fn.endswith(".json"):
                try:
                    j = json.load(open(os.path.join(dp, fn)))
                except Exception:
                    continue
                if isinstance(j, dict) and "CID" in j and "LAYERS" in j:
                    out[j["CID"]] = (j, dp)
    return out


# ---------------------------------------------------------------- variables (today's semantics, ported)
def render(value, fmt):
    if not fmt:
        return value
    try:
        if fmt == "dword":
            return "dword:%08x" % (int(value) & 0xFFFFFFFF)
        if fmt == "qword":
            n = int(value)
            return "hex(b):" + ",".join("%02x" % ((n >> (8 * i)) & 0xFF) for i in range(8))
        if fmt in ("u8", "u16le", "u16be", "u32le", "u32be"):
            w = 1 if fmt == "u8" else 2 if fmt[1] == "1" else 4
            n = int(value)
            bs = [(n >> (8 * i)) & 0xFF for i in range(w)]
            return "".join("%02x" % b for b in (reversed(bs) if fmt.endswith("be") else bs))
    except ValueError:
        return value
    if fmt == "bool":
        return "true" if value.lower() in ("1", "true", "yes") else "false"
    if fmt == "winpath":
        return value.replace("/", "\\")
    if fmt == "upper":
        return value.upper()
    if fmt == "lower":
        return value.lower()
    if fmt in ("ascii", "asciiz"):
        return value.encode("latin-1", "replace").hex() + ("00" if fmt == "asciiz" else "")
    return value


def subst(s, vars_):
    """VarSubst::StringVariableSubstitution: %KEY% / %KEY:fmt%; unknown tokens stay; an unmatched % ends it."""
    out, pos = [], 0
    while pos < len(s):
        a = s.find("%", pos)
        if a < 0:
            out.append(s[pos:]); break
        b = s.find("%", a + 1)
        if b < 0:
            out.append(s[pos:]); break
        out.append(s[pos:a])
        tok = s[a + 1:b]
        key, _, fmt = tok.partition(":")
        out.append(render(vars_[key], fmt) if key in vars_ else "%" + tok + "%")
        pos = b + 1
    return "".join(out)


def subst_json(v, vars_):
    if isinstance(v, str):
        return subst(v, vars_)
    if isinstance(v, list):
        return [subst_json(x, vars_) for x in v]
    if isinstance(v, dict):
        return {subst(k, vars_): subst_json(x, vars_) for k, x in v.items()}
    return v


_TOK = re.compile(r"\s+|\(|\)|&&|\|\||==|!=|!|\"[^\"]*\"|'[^']*'|%[^%]*%|[^\s()!&|=\"']+")


def when(expr, vars_):
    """VarSubst::EvaluateCondition. Unparseable → true (as gen 5, which warns)."""
    if not expr or not expr.strip():
        return True
    toks, i = [], 0
    while i < len(expr):
        m = _TOK.match(expr, i)
        if not m:
            return True
        t = m.group(0); i = m.end()
        if t.isspace():
            continue
        if t in ("(", ")", "&&", "||", "==", "!=", "!"):
            toks.append(("op", t))
        elif t[0] in "\"'":
            toks.append(("v", t[1:-1]))
        elif t[0] == "%":
            toks.append(("v", vars_.get(t[1:-1], "")))
        else:
            toks.append(("v", t))
    p = [0]

    def at(o):
        return p[0] < len(toks) and toks[p[0]] == ("op", o)

    def por():
        v = pand()
        while at("||"):
            p[0] += 1; r = pand(); v = v or r
        return v

    def pand():
        v = pun()
        while at("&&"):
            p[0] += 1; r = pun(); v = v and r
        return v

    def pun():
        if at("!"):
            p[0] += 1; return not pun()
        return pprim()

    def pprim():
        if at("("):
            p[0] += 1; v = por()
            if at(")"):
                p[0] += 1
            return v
        if p[0] >= len(toks) or toks[p[0]][0] != "v":
            raise ValueError
        lhs = toks[p[0]][1]; p[0] += 1
        if at("==") or at("!="):
            eq = toks[p[0]][1] == "=="; p[0] += 1
            rhs = toks[p[0]][1]; p[0] += 1
            return (lhs == rhs) if eq else (lhs != rhs)
        return lhs not in ("", "0", "false", "no")
    try:
        v = por()
        return v if p[0] == len(toks) else True
    except (ValueError, IndexError):
        return True


def resolve_vars(decls, builtins, instance):
    """decls: ordered {key: declaration} (runner's first, then the game's — later wins, as the fold). Returns values.
    Priority instance > DEFAULT; a declaration's WHEN gates its VALUE to "" (evaluated inside the fixpoint)."""
    src, gate = {}, {}
    for k, d in decls.items():
        src[k] = instance.get(k, d.get("DEFAULT", ""))
        w = d.get("WHEN") or (d.get("UI") or {}).get("WHEN")
        if w:
            gate[k] = w
    for k, v in instance.items():
        src.setdefault(k, v)
    cur = dict(src)
    for _ in range(16):
        m = dict(builtins); m.update(cur)
        nxt = {k: ("" if k in gate and not when(gate[k], m) else subst(src[k], m)) for k in src}
        if nxt == cur:
            break
        cur = nxt
    return cur


# ---------------------------------------------------------------- addresses
ANCHOR = re.compile(r"^(%[^%]+%|[A-Za-z]:)(/|$)")


def ns_path(addr):
    ns, _, rest = addr.partition("/")
    return ns, rest


def to_layout(path, roots):
    """Guest coordinates → a runner's layout (its GUEST_ROOTS: {"C:": "%PrefixRoot%/drive_c", ...}); drives are
    case-insensitive. Anything not under an anchor the runner maps is returned unchanged."""
    for a in sorted(roots or {}, key=len, reverse=True):
        if path == a or path.lower().startswith(a.lower() + "/") or path.lower() == a.lower():
            return roots[a] + path[len(a):]
    return path


def is_absolute(path):
    return bool(ANCHOR.match(path))


def place(path, prefix):
    """A relative path inside a node placed at `prefix` lands under it; an anchored path stays."""
    if not prefix or is_absolute(path):
        return path
    return prefix + ("/" + path if path else "")


def take_match(addr, sel):
    """Does selection `sel` (an address or a namespace) cover `addr`? → the remainder after the selection's
    left-stripped base (the part the selection keeps), else None."""
    if sel == addr:
        return addr.rsplit("/", 1)[-1] if "/" in addr else ""
    base = sel[:-1] if sel.endswith("/") else sel
    if addr.startswith(base + "/"):
        rest = addr[len(base) + 1:]
        if sel.endswith("/") or "/" not in base:         # contents, or a whole namespace: nothing of base kept
            return rest
        return base.rsplit("/", 1)[-1] + "/" + rest      # a directory: kept under its own name
    return None


# ---------------------------------------------------------------- the resolver
class Resolver:
    def __init__(self, nodes):
        self.nodes = nodes

    # phase 1: variables a WHEN may read — ungated VARS, descending through ungated NODE layers
    def phase1(self, root, builtins, instance):
        decls, seen = {}, set()

        def walk(cid, take):
            key = (cid, json.dumps(take, sort_keys=True) if take else "")
            if key in seen or cid not in self.nodes:
                return
            seen.add(key)
            for L in self.nodes[cid][0]["LAYERS"]:
                if "WHEN" in L:
                    continue
                if "NODE" in L:
                    walk(L["NODE"], self._compose_take(take, L.get("TAKE")))
                elif "VARS" in L:
                    for k, d in L["VARS"].items():
                        a = self._taken(take, "VARS/" + k)
                        if a is None:
                            continue
                        k = a.split("/", 1)[1]
                        decls.pop(k, None); decls[k] = d
        walk(root, None)
        return resolve_vars(decls, builtins, instance)

    def resolve(self, root, instance=None, builtins=None, grafts=()):
        instance, builtins = dict(instance or {}), dict(builtins or {})
        wv = dict(builtins); wv.update(self.phase1(root, builtins, instance))
        self.wv = wv
        self.slots, self.first, self.events, self.order = [], {}, [], []
        self._expand(root, None, "", ())
        for g in grafts:                                   # grafts after the variant, in instance order
            self._expand(g, None, "", ())
        return self._fold(instance, builtins)

    def _occ(self, cid, take, prefix):
        return (cid, json.dumps(take, sort_keys=True) if take else "", prefix)

    def _expand(self, cid, take, prefix, containers):
        occ = self._occ(cid, take, prefix)
        if any(c[0] == cid for c in containers):
            self.events.append(("cycle", cid)); return
        if occ in self.first:
            prev = self.first[occ]
            if prev and containers and prev[-1] == containers[-1]:   # a later mention in the SAME list moves it
                self._remove(occ)
                self.events.append(("move", cid))
            else:
                self.events.append(("held", cid)); return
        if cid not in self.nodes:
            self.events.append(("missing", cid)); return
        self.first[occ] = containers
        self.order.append(occ)
        node, bdir = self.nodes[cid]
        inner = containers + (occ,)
        for i, L in enumerate(node["LAYERS"]):
            if "WHEN" in L and not when(L["WHEN"], self.wv):
                continue
            t = type_of(L)
            if t == "NODE":
                tgt = L.get("TARGET")
                sub = prefix
                if tgt is not None:
                    ns, p = ns_path(tgt)
                    if ns != "FILES":
                        raise ValueError(f"NODE TARGET outside FILES: {tgt}")
                    sub = place(p, prefix)
                self._expand(L["NODE"], self._compose_take(take, L.get("TAKE")), sub, inner)
            elif t == "ANY":
                present = {o[0] for o in self.order}
                if not any(m in present for m in L["ANY"]):
                    self.events.append(("any-unmet", cid))
            elif t == "NOT":
                self.slots.append({"kind": "NOT", "node": L["NOT"], "occ": inner, "from": cid})
            else:
                self.slots.append({"kind": t, "layer": L, "at": i, "occ": inner, "from": cid, "dir": bdir,
                                   "prefix": prefix, "take": take})

    def _compose_take(self, outer, inner):
        if not outer:
            return inner
        if not inner:
            return outer
        return {"outer": outer, "inner": inner}              # applied inner first, then outer

    def _remove(self, occ):
        gone = {o for o in self.first if o == occ or occ in self.first[o]}
        self.slots = [s for s in self.slots if occ not in s["occ"]]
        for o in gone:
            del self.first[o]
        self.order = [o for o in self.order if o not in gone]

    # TAKE: filter + rename a fact's addresses; None = not taken
    def _taken(self, take, addr):
        if not take:
            return addr
        if isinstance(take, dict):
            a = self._taken(take["inner"], addr)
            return None if a is None else self._taken(take["outer"], a)
        for sel in take:
            if isinstance(sel, list):                          # [address, new name]: rename a file/dir/key
                src, dst = sel
                ns = src.split("/", 1)[0]
                if not dst.startswith(ns + "/"):
                    dst = ns + "/" + dst
                if addr == src:
                    return dst
                if addr.startswith(src.rstrip("/") + "/"):
                    return dst + addr[len(src.rstrip("/")):]
            else:
                r = take_match(addr, sel)
                if r is not None:
                    return sel.split("/", 1)[0] + ("/" + r if r else "")
        return None

    def _fold(self, instance, builtins):
        seq, reg, regkeys, dll, env, exe, keep, decls, nots = [], {}, {}, {}, {}, {}, {}, {}, []
        for s in self.slots:
            k, L, take, prefix = s["kind"], s.get("layer"), s.get("take"), s.get("prefix", "")
            if k == "NOT":
                nots.append(s["node"]); continue
            if k in CONTENT:
                ns, tp = ns_path(L.get("TARGET", "FILES"))
                item = {"kind": k, "payload": L[k], "dir": s["dir"], "source": L.get("SOURCE"),
                        "target": place(tp, prefix), "submounts": L.get("SUBMOUNTS"), "take": take, "from": s["from"], "at": s["at"]}
                seq.append(item)
            elif k == "EDIT":
                ns, tp = ns_path(L["TARGET"])
                a = self._taken(take, "FILES/" + tp)
                if a is None:
                    continue
                seq.append({"kind": "EDIT", "ops": L["EDIT"], "target": place(ns_path(a)[1], prefix), "from": s["from"], "at": s["at"]})
            elif k == "REG":
                for arch in (L.get("ARCH") or [None]):
                    self._fold_reg(L["REG"], arch, "", reg, regkeys, take)
            elif k == "DLL":
                for n, o in L["DLL"].items():
                    a = self._taken(take, "DLL/" + n)
                    if a is None:
                        continue
                    n2 = a.split("/", 1)[1].lower()
                    if o is None:
                        dll.pop(n2, None)
                    else:
                        dll.pop(n2, None); dll[n2] = o
            elif k == "ENV":
                for n, v in L["ENV"].items():
                    a = self._taken(take, "ENV/" + n)
                    if a is None:
                        continue
                    n = a.split("/", 1)[1]
                    env.pop(n, None); env[n] = v
            elif k == "VARS":
                for n, d in L["VARS"].items():
                    a = self._taken(take, "VARS/" + n)
                    if a is None:
                        continue
                    n = a.split("/", 1)[1]
                    decls.pop(n, None); decls[n] = d
            elif k == "EXEC":
                for e in L["EXEC"]:
                    a = self._taken(take, "EXEC/" + e["LABEL"])
                    if a is None:
                        continue
                    lab = a.split("/", 1)[1]
                    e = dict(e, LABEL=lab)
                    if prefix:
                        for f in ("EXE", "WORKDIR"):
                            if f in e:
                                e[f] = place(e[f], prefix)
                    exe[lab] = merge(exe.get(lab), e) if lab in exe else copy.deepcopy(e)
            elif k == "KEEP":
                for a0, v in L["KEEP"].items():
                    a = self._taken(take, a0)
                    if a is None:
                        continue
                    ns, p = ns_path(a)
                    if ns == "FILES":
                        a = "FILES/" + place(p, prefix)
                    keep.pop(a, None); keep[a] = v
        present = {o[0] for o in self.order}
        for n in nots:
            if n in present:
                self.events.append(("not-hit", n))
        return {"seq": seq, "reg": reg, "regkeys": regkeys, "dll": dll, "env": env, "exec": exe, "keep": keep,
                "decls": decls, "order": [o[0] for o in self.order], "events": self.events, "when_vars": self.wv}

    def _fold_reg(self, tree, arch, path, reg, regkeys, take):
        for k, v in tree.items():
            p = k if not path else path + "\\" + k
            if isinstance(v, dict):
                if not v:                                  # an empty key: "create this key" (keys with content are implied)
                    if self._taken(take, "REG/" + p.replace("\\", "/")) is not None:
                        regkeys[(arch, p.lower())] = p
                    continue
                self._fold_reg(v, arch, p, reg, regkeys, take)
            elif v is None and path:                       # a null value deletes it
                reg.pop((arch, path.lower(), k.lower()), None)
            elif v is None:
                pass
            else:
                if self._taken(take, "REG/" + p.replace("\\", "/")) is None:
                    continue
                reg[(arch, path.lower(), k.lower())] = (path, k, v)


def merge(a, b):
    """EXEC entries fold per field (EXEC/<label>/<field>/…): objects merge, anything else is replaced; null deletes."""
    if not isinstance(a, dict) or not isinstance(b, dict):
        return copy.deepcopy(b)
    out = dict(a)
    for k, v in b.items():
        if v is None:
            out.pop(k, None)
        else:
            out[k] = merge(a.get(k), v) if isinstance(v, dict) and isinstance(a.get(k), dict) else copy.deepcopy(v)
    return out


# ---------------------------------------------------------------- the shelf (§2)
def shelf(nodes, builtins=None):
    """Tiles merged by UID and nested by PARENTUID; rows = variant nodes × the tiled entries in their fold.
    -> [{"uid", "title", "faces": [{"uid", "title", "depth", "rows": [variant cid]}]}], families by title."""
    R = Resolver(nodes)
    present = {}                                           # face uid -> [(variant cid, folded tile)]
    for h, (n, _) in nodes.items():
        if not n.get("VARIANT"):
            continue
        plan = R.resolve(h, {}, builtins or {})
        for e in plan["exec"].values():
            if not e.get("GUEST") and e.get("TILE"):
                present.setdefault(e["TILE"]["UID"], []).append((h, e["TILE"]))

    def rank(uid):
        def key(v):
            n = nodes[v[0]][0]
            return (uid not in (n.get("RECOMMENDED") or []), n.get("VARIANT", ""), v[0])
        return sorted(present[uid], key=key)
    tiles = {uid: rank(uid)[0][1] for uid in present}      # presentation: the recommended (else first) variant's

    def root(uid):
        seen = set()
        while tiles.get(uid, {}).get("PARENTUID") and uid not in seen:
            seen.add(uid); uid = tiles[uid]["PARENTUID"]
        return uid

    def depth(uid):
        d = 0
        while tiles.get(uid, {}).get("PARENTUID"):
            uid = tiles[uid]["PARENTUID"]; d += 1
        return d
    fams = {}
    for uid in present:
        fams.setdefault(root(uid), []).append(uid)
    out = []
    for r, uids in fams.items():
        faces = sorted(uids, key=lambda u: (depth(u), tiles[u]["TITLE"]))
        out.append({"uid": r, "title": tiles[faces[0]]["TITLE"],
                    "faces": [{"uid": u, "title": tiles[u]["TITLE"], "depth": depth(u), "rows": [v for v, _ in rank(u)]}
                              for u in faces]})
    return sorted(out, key=lambda f: f["title"])


# ---------------------------------------------------------------- what a content layer replaces (§1.5 covered edits)
_NAMES = {}


def _member_names(seq, j):
    """Lower-cased members of the zip seq[j] mounts (a DELTA: the zip its chain reconstructs — its base is the
    content beneath at the same target, down to the zip). None when unreadable."""
    import zipfile
    c = seq[j]
    chain = [os.path.join(c["dir"], c["payload"])]
    if c["kind"] == "DELTA":
        for i in range(j - 1, -1, -1):
            b = seq[i]
            if b["kind"] == "EDIT" or b["target"] != c["target"]:
                continue
            if b["kind"] not in ("ZIP", "DELTA") or not b.get("dir"):
                return None
            chain.insert(0, os.path.join(b["dir"], b["payload"]))
            if b["kind"] == "ZIP":
                break
        else:
            return None
    key = tuple(chain)
    if key not in _NAMES:
        try:
            if c["kind"] == "ZIP":
                with zipfile.ZipFile(chain[0]) as z:
                    _NAMES[key] = {n.rstrip("/").lower() for n in z.namelist()}
            else:
                from vgdelta import zip_names_of
                _NAMES[key] = zip_names_of(list(chain))
        except Exception:
            _NAMES[key] = None
    return _NAMES[key]


def provided(seq, j, vars_=None):
    """The file paths (lower-case, as the mount sees them) content seq[j] provides, or None = "everything under its
    target" (a DIR, an unreadable file). Mirrors VidyaGodFS: SUBMOUNTS "src:dst" mount only those sub-paths; a FILE
    lands at <target>/<its name>; otherwise the whole zip at the target."""
    c = seq[j]
    S = (lambda x: subst(x, vars_)) if vars_ else (lambda x: x)
    t = S(c["target"]).lower()
    if c["kind"] == "DIR" or not c.get("dir"):
        return None
    if c["kind"] == "FILE":
        return {(t + "/" if t else "") + os.path.basename(c["payload"]).lower()}
    names = _member_names(seq, j)
    if names is None:
        return None
    if not c.get("submounts"):
        return {(t + "/" if t else "") + n for n in names}
    out = set()
    for sm in c["submounts"]:
        src, _, dst = S(sm).partition(":")
        src, dst = src.strip("/").lower(), dst.strip("/").lower()
        for n in names:
            if n == src:
                out.add(dst)
            elif n.startswith(src + "/"):
                out.add(dst + n[len(src):])
    return out


def covers(seq, j, f, vars_=None):
    """Does content seq[j] replace file f (f spelled like the targets: substituted with the same vars_)?"""
    fl = f.lower()
    p = provided(seq, j, vars_)
    if p is None:
        t = (subst(seq[j]["target"], vars_) if vars_ else seq[j]["target"]).lower()
        return fl == t or fl.startswith(t + "/") or t == ""
    return fl in p


def self_test():
    def N(layers, **kw):
        return dict(kw, LAYERS=layers)
    def R(nodes):
        return Resolver({k: (dict(v, CID=k), "") for k, v in nodes.items()})
    Z = lambda name, t="FILES/C:/g": {"ZIP": name, "TARGET": t}
    labels = lambda plan: [it.get("payload") for it in plan["seq"] if it["kind"] != "EDIT"]

    # diamond: one occurrence at its first position; nothing moves when the first container is not a container here
    nodes = {"a": N([Z("a")]), "b": N([{"NODE": "a"}, Z("b")]), "c": N([{"NODE": "a"}, Z("c")]),
             "x": N([{"NODE": "b"}, {"NODE": "c"}])}
    p = R(nodes).resolve("x")
    assert labels(p) == ["a", "b", "c"] and ("held", "a") in p["events"], p["events"]
    # a later mention in the SAME container moves it (and says so)
    nodes["y"] = N([{"NODE": "a"}, {"NODE": "b"}, Z("y"), {"NODE": "a"}])
    p = R(nodes).resolve("y")
    assert labels(p) == ["b", "y", "a"] and ("move", "a") in p["events"], (labels(p), p["events"])
    # cycle guard
    p = R({"p": N([{"NODE": "q"}]), "q": N([{"NODE": "p"}, Z("q")])}).resolve("p")
    assert ("cycle", "p") in p["events"] and labels(p) == ["q"]
    # WHEN: phase 1 reads ungated VARS through ungated NODE layers; a gated NODE is skipped
    nodes = {"opt": N([Z("opt")]), "v": N([{"VARS": {"o": {"DEFAULT": "0"}}}, {"NODE": "opt", "WHEN": "%o%==1"}, Z("v")])}
    assert labels(R(nodes).resolve("v")) == ["v"]
    assert labels(R(nodes).resolve("v", {"o": "1"})) == ["opt", "v"]
    # placement: a relative TARGET lands under the NODE's TARGET; an anchored one stays
    nodes = {"lib": N([Z("l1", "FILES/sub"), Z("l2", "FILES/C:/abs")]), "g": N([{"NODE": "lib", "TARGET": "FILES/C:/g/lib"}])}
    assert [it["target"] for it in R(nodes).resolve("g")["seq"]] == ["C:/g/lib/sub", "C:/abs"]
    # TAKE: facts filtered + renamed; left-strip of a directory keeps its own name
    nodes = {"lib": N([{"DLL": {"d3d8": "n,b", "ddraw": "n,b"}}, {"VARS": {"a": {"DEFAULT": "1"}, "b": {"DEFAULT": "2"}}},
                       {"EXEC": [{"LABEL": "Play", "HOST": "win32", "EXE": "x.exe"}, {"LABEL": "Tool", "HOST": "win32"}]}]),
             "g": N([{"NODE": "lib", "TAKE": ["DLL/d3d8", ["VARS/a", "VARS/z"], "EXEC/Tool"]}])}
    p = R(nodes).resolve("g")
    assert p["dll"] == {"d3d8": "n,b"} and list(p["decls"]) == ["z"] and list(p["exec"]) == ["Tool"], p
    assert take_match("FILES/a/b/c.dll", "FILES/a/b/c.dll") == "c.dll"
    assert take_match("FILES/a/b/x", "FILES/a/b") == "b/x" and take_match("FILES/a/b/x", "FILES/a/b/") == "x"
    # EXEC folds per field: a partial entry beneath (a tile) + the variant's entry; first-declared order
    nodes = {"t": N([{"EXEC": [{"LABEL": "Play", "TILE": {"UID": "1", "TITLE": "T"}}, {"LABEL": "B", "HOST": "h"}]}]),
             "v": N([{"NODE": "t"}, {"EXEC": [{"LABEL": "Play", "HOST": "win32", "TILE": {"TITLE": "T2"}}]}])}
    e = R(nodes).resolve("v")["exec"]
    assert list(e) == ["Play", "B"] and e["Play"] == {"LABEL": "Play", "HOST": "win32", "TILE": {"UID": "1", "TITLE": "T2"}}, e
    # REG: later wins per value, null deletes, an empty key is created
    nodes = {"r": N([{"REG": {"HKCU": {"S": {"a": "1", "b": "2"}}}}, {"REG": {"HKCU": {"S": {"a": "3", "b": None}, "E": {}}}}])}
    p = R(nodes).resolve("r")
    assert {k[2]: v[2] for k, v in p["reg"].items()} == {"a": "3"} and (None, "hkcu\\e") in p["regkeys"], p["reg"]
    # ANY / NOT
    nodes = {"base": N([Z("b")]), "m": N([{"ANY": ["base"]}, Z("m")]), "w": N([{"NOT": "base"}]),
             "v1": N([{"NODE": "base"}, {"NODE": "m"}]), "v2": N([{"NODE": "m"}]), "v3": N([{"NODE": "base"}, {"NODE": "w"}])}
    assert not any(e[0] == "any-unmet" for e in R(nodes).resolve("v1")["events"])
    assert ("any-unmet", "m") in R(nodes).resolve("v2")["events"]
    assert ("not-hit", "base") in R(nodes).resolve("v3")["events"]
    # covering: a FILE lands at <target>/<name>; SUBMOUNTS mount only their sub-paths (listing faked)
    seq = [{"kind": "EDIT", "target": "C:/g/a.cfg"},
           {"kind": "FILE", "payload": "x/a.cfg", "target": "C:/g", "dir": "/d"},
           {"kind": "ZIP", "payload": "z.zip", "target": "", "dir": "/d", "submounts": ["dx/x.dll:C:/g/x.dll"]}]
    assert covers(seq, 1, "C:/g/a.cfg") and not covers(seq, 1, "C:/g/b.cfg")
    _NAMES[("/d/z.zip",)] = {"dx/x.dll", "dx/y.dll", "a.cfg"}
    assert covers(seq, 2, "C:/g/x.dll") and not covers(seq, 2, "C:/g/a.cfg")
    # guest coordinates → a runner's layout
    roots = {"C:": "pfx/drive_c", "%UserProfile%": "pfx/drive_c/users/steamuser"}
    assert to_layout("c:/g/x", roots) == "pfx/drive_c/g/x" and to_layout("%UserProfile%/s", roots) == "pfx/drive_c/users/steamuser/s"
    assert to_layout("C:x", roots) == "C:x" and to_layout("rel/p", roots) == "rel/p"
    # identity: Kubo-parity raw CID of the canonical bytes, CID/POS never part of them
    assert cid_of(b"hello\n") == "bafkreicysg23kiwv34eg2d7qweipxwosdo2py4ldv42nbauguluen5v6am"
    assert canonical({"CID": "x", "POS": [1], "LABEL": "l", "LAYERS": []}) == b'{"LABEL":"l","LAYERS":[]}'
    print("self-test OK")


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        self_test(); sys.exit(0)
    lib, cid = sys.argv[1], sys.argv[2]
    inst = dict(a.split("=", 1) for a in sys.argv[3:] if "=" in a)
    r = Resolver(load_nodes(lib)).resolve(cid, inst)
    r["reg"] = {"|".join(map(str, k)): v for k, v in r["reg"].items()}
    r["regkeys"] = {"|".join(map(str, k)): v for k, v in r["regkeys"].items()}
    print(json.dumps(r, indent=1, default=str)[:20000])
