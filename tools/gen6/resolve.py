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
    ["events"]  moves / held mentions / cycles / an INTO with nothing beneath / a part whose INTO is off

Usage (debug): resolve.py <library-root> <cid> [--var K=V ...]
"""
import copy, json, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

TYPE_KEYS = ["ZIP", "FILE", "DELTA", "CODED", "DIR", "NODE", "EDIT", "REG", "VARS", "ENV", "DLL", "EXEC", "KEEP", "INTO"]
_NODE_CID = re.compile(r"bafkrei[a-z2-7]{52}")
CONTENT = ("ZIP", "FILE", "DELTA", "CODED", "DIR")


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
        dn[:] = [d for d in dn if not d.startswith("_peer_")]
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


def table_entries(table, d):
    """VarSubst::TableEntries: a MAP table in the one namespace, under names no variable can have (no ':' in a name)."""
    if not isinstance(d, dict) or not isinstance(d.get("MAP"), dict):
        return []
    out = [(table + ":?", "1")]
    out += [(table + ":=" + e, v) for e, v in d["MAP"].items() if isinstance(v, str)]
    if isinstance(d.get("ELSE"), str):
        out.append((table + ":!", d["ELSE"]))
    return out


def render_token(value, fmt, vars_):
    """VarSubst::RenderToken: map:TABLE → the entry, else the ELSE, else the value; otherwise render()."""
    if fmt.startswith("map:"):
        t = fmt[4:]
        if vars_.get(t + ":?") != "1":
            return value
        return vars_.get(t + ":=" + value, vars_.get(t + ":!", value))
    return render(value, fmt)


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
        out.append(render_token(vars_[key], fmt, vars_) if key in vars_ else "%" + tok + "%")
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


def when(expr, vars_, present=None):
    """VarSubst::EvaluateCondition. Unparseable → true (as gen 5, which warns). A bare word that is a node CID is
    "true" when present(cid) says that node is in the composition, else "false" (08 §8.6)."""
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
        elif _NODE_CID.fullmatch(t):
            toks.append(("v", "true" if present is not None and present(t) else "false"))
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


def evaluate_integer(expr):
    """EVAL: an integer expression, exactly as VarSubst::EvaluateInteger — C precedence (unary - ~ !, * / %, + -,
    << >>, &, ^, |), decimal or 0x hex, 64-bit two's complement wrapping; None (no value) for what has no defined
    result there: division/modulo by 0 or of INT64_MIN by -1, a shift count outside 0..63, a literal beyond 64 bits,
    nesting deeper than 64, an operand still a %token% or empty."""
    MIN = -(1 << 63)
    def w(v): return ((v - MIN) % (1 << 64)) + MIN
    toks = re.findall(r"0[xX][0-9a-fA-F]+|[0-9]+|<<|>>|[-+*/%&|^~!()]|[^ \t\n\v\f\r]", expr)   # only ASCII whitespace separates (C isspace)
    pos, depth = [0], [0]
    def peek(): return toks[pos[0]] if pos[0] < len(toks) else None
    def eat(t):
        if peek() == t:
            pos[0] += 1
            return True
        return False
    def primary():
        depth[0] += 1
        if depth[0] > 64: raise ValueError
        if eat("("):
            v = orr()
            if not eat(")"): raise ValueError
        elif eat("-"): v = w(-primary())
        elif eat("~"): v = w(~primary())
        elif eat("!"): v = int(not primary())
        else:
            t = peek()
            if t is None or not re.match(r"^(0[xX][0-9a-fA-F]+|[0-9]+)$", t): raise ValueError
            pos[0] += 1
            v = int(t, 16) if t[:2].lower() == "0x" else int(t)
            if v > (1 << 63) - 1: raise ValueError                      # stoll: out of range
        depth[0] -= 1
        return v
    def mul():
        v = primary()
        while peek() in ("*", "/", "%"):
            o = toks[pos[0]]; pos[0] += 1; r = primary()
            if o == "*": v = w(v * r); continue
            if r == 0 or (v == MIN and r == -1): raise ValueError
            q = abs(v) // abs(r) * (1 if (v < 0) == (r < 0) else -1)   # C: truncate toward zero
            v = q if o == "/" else v - q * r
        return v
    def add():
        v = mul()
        while peek() in ("+", "-"):
            o = toks[pos[0]]; pos[0] += 1; r = mul()
            v = w(v + r if o == "+" else v - r)
        return v
    def shift():
        v = add()
        while peek() in ("<<", ">>"):
            o = toks[pos[0]]; pos[0] += 1; n = add()
            if n < 0 or n > 63: raise ValueError
            v = w(v << n) if o == "<<" else v >> n
        return v
    def andd():
        v = shift()
        while eat("&"): v &= shift()
        return v
    def xor():
        v = andd()
        while eat("^"): v ^= andd()
        return v
    def orr():
        v = xor()
        while eat("|"): v |= xor()
        return v
    if not toks:
        return None
    try:
        v = orr()
    except (ValueError, IndexError):
        return None
    return str(v) if pos[0] == len(toks) else None


def resolve_vars(decls, builtins, instance):
    """decls: ordered {key: declaration} (runner's first, then the game's — later wins, as the fold). Returns values.
    Priority instance > DEFAULT; a declaration's WHEN gates its VALUE to "" (evaluated inside the fixpoint)."""
    src, gate, ev = {}, {}, set()
    for k, d in decls.items():
        src[k] = instance.get(k, d.get("DEFAULT", ""))
        (ev.add if d.get("EVAL") is True else ev.discard)(k)   # only a boolean true, as the engine
        w = d.get("WHEN") or (d.get("UI") or {}).get("WHEN")
        if w:
            gate[k] = w
        for e, v in table_entries(k, d):          # a MAP table: its entries, gated with it
            src[e] = v
            if w:
                gate[e] = w
            else:
                gate.pop(e, None)
    for k, v in instance.items():
        src.setdefault(k, v)
    cur = dict(src)
    for _ in range(16):
        m = dict(builtins); m.update(cur)
        nxt = {k: ("" if k in gate and not when(gate[k], m) else subst(src[k], m)) for k in src}
        nxt = {k: (evaluate_integer(v) if k in ev and evaluate_integer(v) is not None else v) for k, v in nxt.items()}
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
    def phase1(self, root, builtins, instance, grafts=()):
        decls, seen = {}, set()

        def walk(cid, view):
            key = (cid, json.dumps(list(view_key_steps(view)), sort_keys=True) if view_takes(view) else "")
            if key in seen or cid not in self.nodes:
                return
            seen.add(key)
            for L in self.nodes[cid][0]["LAYERS"]:
                if "WHEN" in L:
                    continue
                if "NODE" in L:
                    walk(L["NODE"], within(view, L.get("TAKE"), ""))
                elif "VARS" in L:
                    for k, d in L["VARS"].items():
                        a = self._map_fact(view, "VARS/" + k)
                        if a is None:
                            continue
                        k = a.split("/", 1)[1]
                        decls.pop(k, None); decls[k] = d
        walk(root, None)
        for g in grafts:                                   # grafts follow the variant: their declarations count too
            walk(g, None)
        return resolve_vars(decls, builtins, instance)


    def resolve(self, root, instance=None, builtins=None, grafts=(), selection=None):
        """grafts: the parts applied onto the end, in order (applied_grafts); selection: what a WHEN naming a node CID
        reads as selected (default: grafts)."""
        instance, builtins = dict(instance or {}), dict(builtins or {})
        wv = dict(builtins); wv.update(self.phase1(root, builtins, instance, grafts))
        self.wv = wv
        self.slots, self.first, self.events, self.order = [], {}, [], []
        self.count, self.selected = {}, set(selection if selection is not None else grafts)
        self._expand(root, None, "", ())
        for g in grafts:                                   # parts after the end, each after what it goes into
            self._expand(g, None, "", ())
        plan = self._fold(instance, builtins)
        plan["parts"] = list(grafts)
        return plan

    def _present(self, cid):
        return cid in self.count or cid in self.selected

    def _occ(self, cid, view, prefix):
        return (cid, json.dumps(list(view_key_steps(view)), sort_keys=True) if view_takes(view) else "", prefix)

    def _expand(self, cid, view, prefix, containers):
        """view: how this occurrence's addresses reach the root — its (TAKE, TARGET) steps, innermost first."""
        occ = self._occ(cid, view, prefix)
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
        node, bdir = self.nodes[cid]
        Ls = node["LAYERS"]
        if any("WHEN" in L and not when(L["WHEN"], self.wv, self._present) for L in into_layers(Ls)):
            self.events.append(("into-off", cid)); return  # not applied at all, and not in the composition
        # a part's footprint: its INTO layers' TAKEs place its files wherever it is expanded, in layer order (the
        # first INTO's innermost), inside the container's view; never facts
        for L in reversed(into_layers(Ls)):
            if "TAKE" in L:
                view = within(view, footprint_take(L["TAKE"]), "", True)
        self.first[occ] = containers
        self.order.append(occ)
        self.count[cid] = self.count.get(cid, 0) + 1
        inner = containers + (occ,)
        for i, L in enumerate(Ls):
            if "WHEN" in L and not when(L["WHEN"], self.wv, self._present):
                continue
            t = type_of(L)
            if t == "NODE":
                tgt = L.get("TARGET")
                sub, at = prefix, ""
                if tgt is not None:
                    ns, at = ns_path(tgt)
                    if ns != "FILES":
                        raise ValueError(f"NODE TARGET outside FILES: {tgt}")
                    sub = place(at, prefix)
                self._expand(L["NODE"], within(view, L.get("TAKE"), at), sub, inner)
            elif t == "INTO":
                if not any(m in self.count for m in L["INTO"]):
                    self.events.append(("into-unmet", cid))
            else:
                self.slots.append({"kind": t, "layer": L, "at": i, "occ": inner, "from": cid, "dir": bdir,
                                   "prefix": prefix, "view": view})

    def _remove(self, occ):
        gone = {o for o in self.first if o == occ or occ in self.first[o]}
        self.slots = [s for s in self.slots if occ not in s["occ"]]
        for o in gone:
            del self.first[o]
        for o in self.order:
            if o in gone:
                self.count[o[0]] -= 1
                if not self.count[o[0]]:
                    del self.count[o[0]]
        self.order = [o for o in self.order if o not in gone]

    # A fact's address through a view: each step's TAKE filters and renames it; facts are never re-rooted.
    def _map_fact(self, view, addr):
        if not view_takes(view):
            return addr
        v = view
        while v:
            take, files_only = v[0], v[4]
            v = v[2]
            if files_only:                                  # a footprint never filters facts
                continue
            addr = self._taken(take, addr)
            if addr is None:
                return None
        return addr

    # A FILES address through a view: each step's TAKE, then its TARGET — so an outer TAKE sees the inner node's
    # files where they landed in the node that placed them.
    def _map_files(self, view, addr, prefix=""):
        if not view_takes(view):                           # no take: the composed placement alone
            return "FILES/" + place(ns_path(addr)[1], prefix)
        v = view
        while v:
            take, at, files_only = v[0], v[1], v[4]
            v = v[2]
            a = self._taken(take, addr)
            if a is not None:
                addr = a
            elif not files_only:                           # a footprint renames an edit's file or a kept folder, never drops it
                return None
            if at:
                addr = "FILES/" + place(ns_path(addr)[1], at)
        return addr

    # TAKE: filter + rename one address; None = not taken
    def _taken(self, take, addr):
        if not take:
            return addr
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
        seq, reg, regkeys, dll, env, exe, keep, decls = [], {}, {}, {}, {}, {}, {}, {}
        for s in self.slots:
            k, L, view, prefix = s["kind"], s.get("layer"), s.get("view"), s.get("prefix", "")
            if k in CONTENT:
                ns, tp = ns_path(L.get("TARGET", "FILES"))
                item = {"kind": k, "payload": L[k], "dir": s["dir"], "source": L.get("SOURCE"), "size": L.get("SIZE"),
                        "target": place(tp, prefix), "own": tp,
                        "view": [[t, a] for t, a in view_steps(view)] if view_takes(view) else None,
                        "from": s["from"], "at": s["at"]}
                if k in ("DELTA", "CODED") and "BASE_SOURCES" in L:           # its bases, by SOURCE CID, in order
                    item["base_sources"] = L["BASE_SOURCES"]
                if k == "CODED":                                              # its description (a node, by CID) and its name
                    item["description"] = L.get("DESCRIPTION", "")
                    item["name"] = L.get("NAME", "")
                seq.append(item)
            elif k == "EDIT":
                ns, tp = ns_path(L["TARGET"])
                a = self._map_files(view, "FILES/" + tp, prefix)
                if a is None:
                    continue
                seq.append({"kind": "EDIT", "ops": L["EDIT"], "target": ns_path(a)[1], "from": s["from"], "at": s["at"]})
            elif k == "REG":
                for arch in (L.get("ARCH") or [None]):
                    self._fold_reg(L["REG"], arch, "", reg, regkeys, view)
            elif k == "DLL":
                for n, o in L["DLL"].items():
                    a = self._map_fact(view, "DLL/" + n)
                    if a is None:
                        continue
                    n2 = a.split("/", 1)[1].lower()
                    if o is None:
                        dll.pop(n2, None)
                    else:
                        dll.pop(n2, None); dll[n2] = o
            elif k == "ENV":
                for n, v in L["ENV"].items():
                    a = self._map_fact(view, "ENV/" + n)
                    if a is None:
                        continue
                    n = a.split("/", 1)[1]
                    env.pop(n, None); env[n] = v
            elif k == "VARS":
                for n, d in L["VARS"].items():
                    a = self._map_fact(view, "VARS/" + n)
                    if a is None:
                        continue
                    n = a.split("/", 1)[1]
                    decls.pop(n, None); decls[n] = d
            elif k == "EXEC":
                for e in L["EXEC"]:
                    a = self._map_fact(view, "EXEC/" + e["LABEL"])
                    if a is None:
                        continue
                    lab = a.split("/", 1)[1]
                    e = dict(e, LABEL=lab)
                    if prefix or view_takes(view):             # its paths go where the node's files went
                        for f in ("EXE", "WORKDIR"):
                            if isinstance(e.get(f), str):
                                m = self._map_files(view, "FILES/" + e[f], prefix)
                                e[f] = ns_path(m)[1] if m is not None else place(e[f], prefix)
                    exe[lab] = merge(exe.get(lab), e) if lab in exe else copy.deepcopy(e)
            elif k == "KEEP":
                for a0, v in L["KEEP"].items():
                    a = self._map_files(view, a0, prefix) if ns_path(a0)[0] == "FILES" else self._map_fact(view, a0)
                    if a is None:
                        continue
                    keep.pop(a, None); keep[a] = v
        return {"seq": seq, "reg": reg, "regkeys": regkeys, "dll": dll, "env": env, "exec": exe, "keep": keep,
                "decls": decls, "order": [o[0] for o in self.order], "events": self.events, "when_vars": self.wv}

    def _fold_reg(self, tree, arch, path, reg, regkeys, view):
        for k, v in tree.items():
            p = k if not path else path + "\\" + k
            if isinstance(v, dict):
                if not v:                                  # an empty key: "create this key" (keys with content are implied)
                    if self._map_fact(view, "REG/" + p.replace("\\", "/")) is not None:
                        regkeys[(arch, p.lower())] = p
                    continue
                self._fold_reg(v, arch, p, reg, regkeys, view)
            elif v is None:                                # null deletes: the value k, and the key p with all below it
                if self._map_fact(view, "REG/" + p.replace("\\", "/")) is None:
                    continue
                sub = p.lower()
                reg.pop((arch, path.lower(), k.lower()), None)
                for key in [key for key in reg if key[0] == arch and (key[1] == sub or key[1].startswith(sub + "\\"))]:
                    del reg[key]
                for key in [key for key in regkeys if key[0] == arch and (key[1] == sub or key[1].startswith(sub + "\\"))]:
                    del regkeys[key]
            else:
                if self._map_fact(view, "REG/" + p.replace("\\", "/")) is None:
                    continue
                reg[(arch, path.lower(), k.lower())] = (path, k, v)


# A view is how an occurrence's addresses reach the root: (take, at, outer view, any step takes) — a shared linked
# list, innermost step first, so a chain 900 deep costs one tuple per level (not a copied list).
def footprint_take(take):
    """A footprint's TAKE as a NODE's reads it: a file or folder it names stays where it is (the pair naming it as
    itself); the whole namespace and pairs as they are."""
    out = []
    for sel in take:
        if not isinstance(sel, str):
            out.append(sel); continue
        s = sel.rstrip("/")
        out.append(s if "/" not in s else [s, s])
    return out


def within(view, take, at, files_only=False):
    """files_only: a part's footprint (its INTO's TAKE) places files and never filters facts."""
    return (take or None, at, view, bool(take) or view_takes(view), files_only)


def view_takes(view):
    """Whether any step of a view takes (a view with none only places)."""
    return bool(view) and view[3]


def view_steps(view):
    while view:
        yield view[0], view[1]
        view = view[2]


def view_key_steps(view):
    """The steps with their kind (a footprint's steps key apart from a NODE's: they treat facts differently)."""
    while view:
        yield ("F" if view[4] else "") , view[0], view[1]
        view = view[2]


def merge(a, b, field=""):
    """EXEC entries fold per field (EXEC/<label>/<field>/…): objects merge, anything else is replaced; null deletes.
    A tile's COVER is one image (FILE, SOURCE and SIZE describe the same bytes) and is replaced whole."""
    if not isinstance(a, dict) or not isinstance(b, dict):
        return copy.deepcopy(b)
    out = dict(a)
    for k, v in b.items():
        if v is None:
            out.pop(k, None)
        elif field == "TILE" and k == "COVER":
            out[k] = copy.deepcopy(v)
        else:
            out[k] = (merge(a.get(k), v, k if not field else field + "/" + k)
                      if isinstance(v, dict) and isinstance(a.get(k), dict) else copy.deepcopy(v))
    return out


def plan_json(plan):
    """The plan in the shape src/fold.cpp's PlanToJson emits (what fold_gate.py compares)."""
    seq = []
    for it in plan["seq"]:
        e = {"kind": it["kind"]}
        if it["kind"] == "EDIT":
            e["ops"] = it["ops"]
        else:
            e.update(payload=it["payload"], dir=it["dir"], source=it["source"], size=it.get("size"), view=it["view"])
            if "base_sources" in it:
                e["base_sources"] = it["base_sources"]
            if it["kind"] == "CODED":
                e.update(description=it.get("description", ""), name=it.get("name", ""))
        e.update({"target": it["target"], "from": it["from"], "at": it["at"]})
        seq.append(e)
    return {"seq": seq,
            "reg": sorted([[a, p, n, dp, dn, v] for (a, p, n), (dp, dn, v) in plan["reg"].items()], key=lambda r: (r[0] or "", r[1], r[2])),
            "regkeys": sorted([[a, p, dp] for (a, p), dp in plan["regkeys"].items()], key=lambda r: (r[0] or "", r[1])),
            "dll": [[k, v] for k, v in plan["dll"].items()], "env": [[k, v] for k, v in plan["env"].items()],
            "exec": [[k, v] for k, v in plan["exec"].items()], "keep": [[k, v] for k, v in plan["keep"].items()],
            "decls": [[k, v] for k, v in plan["decls"].items()], "order": plan["order"], "parts": plan.get("parts", []),
            "events": [list(e) for e in plan["events"]], "when_vars": dict(sorted(plan["when_vars"].items())), "error": ""}


# ---------------------------------------------------------------- references (for tools: remint, repoint, scans)
_DRAFT = re.compile(r"draft-[0-9a-fA-F]{12}")


def when_cids(expr):
    """The node CIDs a WHEN condition names (08 §8.6), with where each starts: bare words shaped like a node CID (or an
    editor's draft handle), never inside a quoted literal or a %token%. [(start, cid), ...]"""
    out, i = [], 0
    while i < len(expr):
        m = _TOK.match(expr, i)
        if not m:
            break
        t = m.group(0)
        if (_NODE_CID.fullmatch(t) or _DRAFT.fullmatch(t)) and t[0] not in "\"'%":
            out.append((i, t))
        i = m.end()
    return out


def node_refs(n):
    """Every reference a node makes, in layer order: NODE (contains), INTO members (goes into), CIDs its conditions
    name, the node holding a coded stream's description. [(how, cid), ...] with how in NODE / INTO / WHEN / DESCRIPTION."""
    out = []
    for L in n.get("LAYERS", []):
        if not isinstance(L, dict):
            continue
        if isinstance(L.get("NODE"), str):
            out.append(("NODE", L["NODE"]))
        if isinstance(L.get("INTO"), list):
            out += [("INTO", x) for x in L["INTO"] if isinstance(x, str)]
        if isinstance(L.get("WHEN"), str):
            out += [("WHEN", c) for _, c in when_cids(L["WHEN"])]
        if "CODED" in L and isinstance(L.get("DESCRIPTION"), str):
            out.append(("DESCRIPTION", L["DESCRIPTION"]))
    return out


def hard_links(n):
    """What a closure follows: what a node contains and what it goes into (never a condition's CIDs)."""
    return [c for how, c in node_refs(n) if how in ("NODE", "INTO")]


def remap_node(n, m):
    """A copy of node n with every reference renamed by m {old: new}: NODE, INTO members, CIDs inside a WHEN (the rest
    of the condition kept byte for byte), and a CODED layer's DESCRIPTION. Payload strings are never touched."""
    def when(e):
        for at, c in reversed(when_cids(e)):
            if c in m:
                e = e[:at] + m[c] + e[at + len(c):]
        return e
    o = json.loads(json.dumps(n))
    for L in o.get("LAYERS", []):
        if not isinstance(L, dict):
            continue
        if isinstance(L.get("NODE"), str) and L["NODE"] in m:
            L["NODE"] = m[L["NODE"]]
        if isinstance(L.get("INTO"), list):
            L["INTO"] = [m.get(x, x) if isinstance(x, str) else x for x in L["INTO"]]
        if isinstance(L.get("WHEN"), str):
            L["WHEN"] = when(L["WHEN"])
        if "CODED" in L and isinstance(L.get("DESCRIPTION"), str) and L["DESCRIPTION"] in m:
            L["DESCRIPTION"] = m[L["DESCRIPTION"]]
    return o


# ---------------------------------------------------------------- parts (12 §12.5, 23.0)
def into_layers(Ls):
    """A part's INTO layers: the run of INTO layers its list opens with (each a list of alternatives; the part goes
    into all of them)."""
    out = []
    for L in Ls:
        if type_of(L) != "INTO" or not isinstance(L["INTO"], list):
            break
        out.append(L)
    return out


def graft_index(nodes):
    """Member of a part's FIRST INTO layer -> the parts naming it (the offer checks the other layers)."""
    idx = {}
    for h, (n, _) in nodes.items():
        In = into_layers(n["LAYERS"])
        if In:
            for m in In[0]["INTO"]:
                idx.setdefault(m, set()).add(h)
    return idx


def _part_depth(nodes, g, seen=()):
    """INTO steps from an end: a part into an end 1, a part into that part 2, ..."""
    In = into_layers(nodes[g][0]["LAYERS"]) if g in nodes else []
    if not In or g in seen:
        return 0
    return 1 + max((_part_depth(nodes, m, seen + (g,)) for L in In for m in L["INTO"]), default=0)


def offered_grafts(nodes, idx, plan, face_uid=None):
    """(offered, pre-ticked). THE HARD LINK: a part is offered where what it goes INTO is the end being launched (the
    root) or a part applied to it, never a node merely beneath. A part a node of the row contains is part of it.
    No order is chosen between parts: shown after what each goes into, then LABEL, then CID."""
    points = ({plan["order"][0]} if plan["order"] else set()) | set(plan.get("parts", []))
    contained = {L["NODE"] for c in plan["order"] if c in nodes for L in nodes[c][0]["LAYERS"] if "NODE" in L}
    offer = {g for m in points for g in idx.get(m, ())}
    # several INTO layers: offered only once EVERY layer has a member that is the end or an applied part
    offer = {g for g in offer if all(any(m in points for m in L["INTO"]) for L in into_layers(nodes[g][0]["LAYERS"]))}
    offer -= contained
    offered = sorted(offer, key=lambda g: (_part_depth(nodes, g), nodes[g][0].get("LABEL", ""), g))
    ticked = [g for g in offered if face_uid is not None and face_uid in (nodes[g][0].get("RECOMMENDED") or [])]
    return offered, ticked


def unsatisfied(plan):
    """How many of the resolution's requirements fail: an INTO with nothing it goes into beneath it."""
    return sum(1 for e in plan["events"] if e[0] == "into-unmet")


def applied_grafts(R, idx, root, face_uid=None, requested=None, instance=None, builtins=None, dropped=None, every=False):
    """The parts a launch applies, in order. requested = the instance's selection, a SET (order ignored); applied in
    rounds — each round the selected parts offered with the ones before applied, siblings by CID — so a part follows
    what it goes into. A part whose INTO's WHEN does not hold on the selection, or that leaves a requirement unmet, is
    dropped, as is one never offered here. None = a fresh instance: the parts RECOMMENDED under the row's tile."""
    instance, builtins = instance or {}, builtins or {}
    want = list(dict.fromkeys(requested)) if requested is not None else None
    applied, refused = [], set()
    plan = R.resolve(root, instance, builtins, [])
    while True:
        offered, ticked = offered_grafts(R.nodes, idx, plan, face_uid)
        if want is not None:
            rnd = sorted(g for g in offered if g in want and g not in applied and g not in refused)
        else:
            rnd = sorted(g for g in (offered if every else ticked) if g not in applied and g not in refused)
        if not rnd:
            break
        anyp = False
        for g in rnd:
            sel = list(dict.fromkeys(rnd + applied + [g]))   # what can be present: offered this round, or applied
            trial = R.resolve(root, instance, builtins, applied + [g], sel)
            off = any(e[1] == g and e[0] in ("into-off", "into-unmet") for e in trial["events"])
            if not off and unsatisfied(trial) <= unsatisfied(plan):
                applied.append(g); plan = trial; anyp = True
            else:
                refused.add(g)
                if dropped is not None:
                    dropped.append(g)
        if not anyp:
            break
    # judged once more as it will launch: the composition is what is applied (a requested part not offered here is
    # not present); a part off or unmet there goes, until nothing changes
    while True:
        final = R.resolve(root, instance, builtins, applied, applied)
        bad = {e[1] for e in final["events"] if e[0] in ("into-off", "into-unmet") and e[1] in applied}
        if not bad:
            break
        applied = [g for g in applied if g not in bad]
        refused |= bad
        if dropped is not None:
            dropped.extend(sorted(bad))
    if want is not None and dropped is not None:
        dropped.extend(g for g in want if g not in applied and g not in refused)
    return applied


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


def base_indices(seq, j):
    """Which items the delta seq[j] is rebuilt from (chapter 04 §4.1): for each CID its BASE_SOURCES names, in order, an
    item carrying that SOURCE — a zip, a single file (a disc image) or a delta, anywhere in the fold (one CID is one
    file); None where none does."""
    out = []
    for c in seq[j].get("base_sources") or []:
        out.append(next((i for i, b in enumerate(seq) if b["kind"] in ("ZIP", "FILE", "DELTA") and b.get("source") == c), None))
    return out


def _chain(seq, j):
    """The files whose bytes seq[j] mounts, as nested lists: a ZIP's or a FILE's file; a DELTA's [file, [base chains…]].
    None when a base it names is not here, or a file is not here."""
    c = seq[j]
    if c["kind"] not in ("ZIP", "FILE", "DELTA") or not c.get("dir"):
        return None
    f = os.path.join(c["dir"], c["payload"])
    if c["kind"] in ("ZIP", "FILE"):
        return f
    bases = []
    for i in base_indices(seq, j):
        b = None if i is None else _chain(seq, i)
        if b is None:
            return None
        bases.append(b)
    return (f, tuple(bases)) if bases else None


def _member_names(seq, j):
    """Lower-cased members of the zip seq[j] mounts (a DELTA: the zip it rebuilds over the bases it names). None when
    unreadable."""
    import zipfile
    c = seq[j]
    chain = _chain(seq, j)
    if chain is None:
        return None
    key = chain
    if key not in _NAMES:
        try:
            if c["kind"] == "ZIP":
                with zipfile.ZipFile(chain) as z:
                    _NAMES[key] = {n.rstrip("/").lower() for n in z.namelist()}
            else:
                from vgdelta import zip_names_of
                _NAMES[key] = zip_names_of(chain)
        except Exception:
            _NAMES[key] = None
    return _NAMES[key]


def provided(seq, j, vars_=None):
    """The file paths (lower-case, as the mount sees them) content seq[j] provides, or None = "everything under its
    target" (a DIR, an unreadable file). A FILE lands at <target>/<its name>; otherwise the whole zip at the target."""
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
    return {(t + "/" if t else "") + n for n in names}


def covers(seq, j, f, vars_=None):
    """Does content seq[j] replace file f (f spelled like the targets: substituted with the same vars_)?"""
    fl = f.lower()
    p = provided(seq, j, vars_)
    if p is None:
        t = (subst(seq[j]["target"], vars_) if vars_ else seq[j]["target"]).lower()
        return fl == t or fl.startswith(t + "/") or t == ""
    return fl in p


P1 = "bafkrei" + "b" * 52                                   # a part named in WHEN conditions: a node CID's shape


def fixtures():
    """Rule fixtures shared by the self-test and fold_gate.py (which runs them through the C++ port too):
    [(name, {cid: node}, [(root, instance, grafts), ...])]."""
    N = lambda layers, **kw: dict(kw, LAYERS=layers)
    Z = lambda name, t="FILES/C:/g": {"ZIP": name, "TARGET": t}
    return [
        ("diamond+move", {"a": N([Z("a")]), "b": N([{"NODE": "a"}, Z("b")]), "c": N([{"NODE": "a"}, Z("c")]),
                          "x": N([{"NODE": "b"}, {"NODE": "c"}]), "y": N([{"NODE": "a"}, {"NODE": "b"}, Z("y"), {"NODE": "a"}])},
         [("x", {}, []), ("y", {}, [])]),
        ("cycle+missing", {"p": N([{"NODE": "q"}, {"NODE": "nope"}]), "q": N([{"NODE": "p"}, Z("q")])}, [("p", {}, [])]),
        ("graft-when", {"base": N([Z("b")]), "v": N([{"NODE": "base"}]),
                        "g": N([{"INTO": ["v"]}, {"VARS": {"win": {"DEFAULT": "1"}}}, {"DIR": "wnd", "TARGET": "FILES/C:/g", "WHEN": "%win%==1"},
                                {"DIR": "cls", "TARGET": "FILES/C:/g", "WHEN": "%win%==0"}])},
         [("v", {}, ["g"]), ("v", {"win": "0"}, ["g"])]),
        ("eval", {"v": N([{"VARS": {"red": {"DEFAULT": "1"}, "purple": {"DEFAULT": "0"}, "grey": {"DEFAULT": "1"},
                                    "mm": {"DEFAULT": "%red%*2 | %purple%*32 | (1-%grey%)*64", "EVAL": True},
                                    "raw": {"DEFAULT": "%red%*2"}}},
                          {"REG": {"HKCU": {"K": {"Mini-map Colors": "%mm:dword%"}}}}])},
         [("v", {}, []), ("v", {"grey": "0", "purple": "1"}, [])]),
        ("map", {"v": N([{"VARS": {"k": {"DEFAULT": "keypad 0", "UI": {"CONTROL": "key"}}, "dir": {"DEFAULT": "C:/x"},
                                   "keys": {"MAP": {"keypad 0": "[0]", "enter": "return", "home": "%dir%/h"}},
                                   "strict": {"MAP": {"a": "A"}, "ELSE": "none"},
                                   "off": {"MAP": {"keypad 0": "NO"}, "WHEN": "%dir%==never"},
                                   "bound": {"DEFAULT": "%k:map:keys%"}, "sb": {"DEFAULT": "%k:map:strict%"},
                                   "ob": {"DEFAULT": "%k:map:off%"}}},
                         {"REG": {"HKCU": {"K": {"Bind": "%k:map:keys%", "Strict": "%k:map:strict%", "Off": "%k:map:off%"}}}},
                         # phase 1 resolves the mapped values; these WHENs make them visible in the plan
                         {"DLL": {"kp0": "n"}, "WHEN": "%bound%==[0]"}, {"DLL": {"plain_w": "n"}, "WHEN": "%bound%==w"},
                         {"DLL": {"tok": "n"}, "WHEN": "%bound%==C:/x/h"}, {"DLL": {"else": "n"}, "WHEN": "%sb%==none"},
                         {"DLL": {"strict_a": "n"}, "WHEN": "%sb%==A"}, {"DLL": {"off_leak": "n"}, "WHEN": "%ob%==NO"}])},
         [("v", {}, []), ("v", {"k": "w"}, []), ("v", {"k": "home"}, []), ("v", {"k": "a"}, [])]),
        ("when", {"opt": N([Z("opt"), {"VARS": {"inner": {"DEFAULT": "%o%x"}}}]),
                  "v": N([{"VARS": {"o": {"DEFAULT": "0"}, "g": {"DEFAULT": "a", "WHEN": "%o%==1"}}},
                          {"NODE": "opt", "WHEN": "%o%==1 && %g%==a"}, Z("v"), {"DLL": {"x": "n"}, "WHEN": "!(%o%==1)"}])},
         [("v", {}, []), ("v", {"o": "1"}, [])]),
        # a cover folds whole: the variant's new FILE does not keep the base cover's SOURCE; META still merges
        ("cover-whole", {"base": N([{"EXEC": [{"LABEL": "Play", "HOST": "win32", "EXE": "C:/g/g.exe",
                                               "TILE": {"UID": "1", "TITLE": "A", "META": {"YEAR": "1999"},
                                                        "COVER": {"FILE": "a.png", "SOURCE": "QmA", "SIZE": 5}}}]}]),
                         "v": N([{"NODE": "base"}, {"EXEC": [{"LABEL": "Play", "TILE": {"UID": "2", "META": {"DEV": "x"},
                                                                                         "COVER": {"FILE": "b.png"}}}]}])},
         [("v", {}, []), ("base", {}, [])]),
        # a delta over two bases, seen whole and placed by a containing node
        ("delta-bases", {"r": N([dict(Z("r", "FILES/g"), SOURCE="Qmr"), dict(Z("m", "FILES/g/music"), SOURCE="Qmm")]),
                         "f": N([{"NODE": "r"}, {"DELTA": "f.vgdelta", "TARGET": "FILES/g",
                                                 "BASE_SOURCES": ["Qmr", "Qmm"]}]),
                         "s": N([{"NODE": "f", "TARGET": "FILES/sub"}])},
         [("f", {}, []), ("s", {}, [])]),
        # a coded stream: its content named by CID, its description a node (data in its DESCRIPTION); placed by a
        # containing node like any content
        ("coded", {"d": dict(N([]), DESCRIPTION={"NAME": "BSA 105", "ISA": 6, "CODER": "BITS", "ENTRY": "@s", "CODE": ["@s", "end"]}),
                   "c": N([{"NODE": "d"}, {"FILE": "a.bsa.content", "SOURCE": "Qmc", "TARGET": "FILES/g/Data"},
                           {"CODED": "a.bsa.vgcr", "SOURCE": "Qmr", "NAME": "a.bsa", "DESCRIPTION": "d",
                            "BASE_SOURCES": ["Qmc"], "TARGET": "FILES/g/Data"}]),
                   "s": N([{"NODE": "c", "TARGET": "FILES/sub"}])},
         [("c", {}, []), ("s", {}, [])]),
        ("placement", {"lib": N([Z("l1", "FILES/sub"), Z("l2", "FILES/C:/abs"), {"NODE": "leaf", "TARGET": "FILES/deep"},
                                 {"EDIT": [{"MODE": "Overwrite", "VALUE": "v"}], "TARGET": "FILES/sub/f.ini"},
                                 {"KEEP": {"FILES/sub/save/": True, "REG/HKCU/S": False}},
                                 {"EXEC": [{"LABEL": "L", "HOST": "h", "EXE": "bin/x.exe", "WORKDIR": "bin"}]}]),
                       "leaf": N([Z("leafz", "FILES/in")]),
                       "g": N([{"NODE": "lib", "TARGET": "FILES/C:/g/lib"}])}, [("g", {}, [])]),
        ("take", {"lib": N([{"DLL": {"d3d8": "n,b", "ddraw": "n,b"}}, {"VARS": {"a": {"DEFAULT": "1"}, "b": {"DEFAULT": "2"}}},
                            {"ENV": {"E1": "1", "E2": None}}, Z("lz", "FILES/MS"),
                            {"EDIT": [{"MODE": "Poke", "OFFSET": "0x1", "VALUE": "00"}], "TARGET": "FILES/MS/x86/D3D8.dll"},
                            {"REG": {"HKCU": {"A": {"v": "1"}, "B": {"w": "2"}, "E": {}}}},
                            {"EXEC": [{"LABEL": "Play", "HOST": "win32", "EXE": "x.exe"}, {"LABEL": "Tool", "HOST": "win32"}]}]),
                  "mid": N([{"NODE": "lib", "TAKE": ["DLL", "VARS/a", "FILES/MS/x86/D3D8.dll", "REG/HKCU/A", "REG/HKCU/E", "ENV/E1"]}]),
                  "g": N([{"NODE": "lib", "TAKE": ["DLL/d3d8", ["VARS/a", "VARS/z"], "EXEC/Tool", ["ENV/E2", "ENV/E9"],
                                                  "FILES/MS/x86/", ["REG/HKCU/B", "REG/HKCU/C"]]},
                          {"NODE": "mid", "TAKE": ["DLL/ddraw", "FILES/MS/"], "TARGET": "FILES/C:/g"}])},
         [("g", {}, [])]),
        ("take-placed", {"lib": N([{"DIR": "lz", "TARGET": "FILES/bin"},
                                   {"EDIT": [{"MODE": "Poke", "OFFSET": "0x1", "VALUE": "00"}], "TARGET": "FILES/bin/a.dll"},
                                   {"EXEC": [{"LABEL": "Tool", "HOST": "h", "EXE": "bin/a.exe", "WORKDIR": "bin"}]},
                                   {"KEEP": {"FILES/bin/save/": True, "REG/HKCU/S": True}}]),
                         "mid": N([{"NODE": "lib", "TAKE": [["FILES/bin/a.dll", "FILES/b.dll"], "FILES/bin/save", "FILES/bin/a.exe",
                                                            "EXEC", "REG"], "TARGET": "FILES/lib"}]),
                         "g": N([{"NODE": "mid", "TAKE": ["FILES/lib/", "EXEC", "REG"], "TARGET": "FILES/C:/g"}])},
         [("g", {}, []), ("mid", {}, [])]),
        ("exec-fold", {"t": N([{"EXEC": [{"LABEL": "Play", "TILE": {"UID": "1", "TITLE": "T", "META": {"A": 1, "B": 2}}},
                                          {"LABEL": "B", "HOST": "h", "ARGS": ["x"]}]}]),
                       "v": N([{"NODE": "t"}, {"EXEC": [{"LABEL": "Play", "HOST": "win32", "TILE": {"TITLE": "T2", "META": {"B": None, "C": 3}}},
                                                         {"LABEL": "B", "ARGS": None}]}])}, [("v", {}, [])]),
        ("reg+dll+env", {"r": N([{"REG": {"HKCU": {"S": {"a": "1", "b": "2"}}}, "ARCH": ["32", "64"]},
                                 {"REG": {"HKCU": {"S": {"a": "0", "b": "1", "c": "k"}}}},
                                 {"REG": {"HKCU": {"S": {"a": "3", "b": None}, "E": {}}, "HKLM": {"X": {"v": 1}}}},
                                 {"REG": {"HKCU": {"T": {"x": "1", "Sub": {"y": "2"}, "K": {}}, "TT": {"z": "3"}}}},
                                 {"REG": {"HKCU": {"T": None}}},
                                 {"DLL": {"D3D8": "n,b", "ddraw": "b"}}, {"DLL": {"d3d8": None}},
                                 {"ENV": {"A": "1", "B": "2"}}, {"ENV": {"A": None}}])}, [("r", {}, [])]),
        # parts (23.0): INTO is a hard link to the end launched or an applied part; WHEN names node CIDs; no order
        ("into+parts", {"base": N([Z("b")]),
                        "v1": N([{"NODE": "base"}, {"EXEC": [{"LABEL": "Play", "HOST": "h", "TILE": {"UID": "9", "TITLE": "t"}}]}]),
                        "vup": N([{"NODE": "v1"}, Z("vup")]),                          # a later end containing v1
                        "m": N([{"INTO": ["base"]}, Z("m")]), "v2": N([{"NODE": "base"}, {"NODE": "m"}]), "v4": N([{"NODE": "m"}]),
                        P1: N([{"INTO": ["v1", "zzz"]}, Z("g1"), {"NODE": "base"}], LABEL="b-part", RECOMMENDED=["9"]),
                        "g2": N([{"INTO": ["v1"]}, Z("g2")], LABEL="a-part"),
                        "g3": N([{"INTO": [P1]}, Z("g3")], LABEL="a-on-b", RECOMMENDED=["9"]),
                        "g4": N([{"INTO": ["v1"], "WHEN": P1 + " == false"}, Z("g4")], LABEL="c-not-with-b", RECOMMENDED=["9"]),
                        "low": N([{"INTO": ["base"]}, Z("low")], LABEL="into-a-lower-node"),
                        "g5": N([{"INTO": ["v1"]}, {"NODE": "base", "WHEN": P1 + " == true"}, Z("g5", "FILES/C:/h")], LABEL="d-detects-b"),
                        "pp": N([{"INTO": [P1]}, {"INTO": ["g2"]}, Z("pp")], LABEL="e-patch-between-b-and-a")},
         [("v1", {}, []), ("v1", {}, ["g2", P1]), ("v1", {}, ["g3", P1]), ("v1", {}, [P1, "g3"]),
          ("v1", {}, [P1, "g4"]), ("v1", {}, ["g4"]), ("v1", {}, ["g5", P1]), ("v1", {}, ["g5"]),
          ("vup", {}, []), ("vup", {}, [P1]), ("v2", {}, []), ("v4", {}, [])]),
        # a part's FOOTPRINT (its INTO's TAKE): files placed (renamed, left out), facts untouched; applied wherever the
        # part is expanded (inside a pack too); two INTO TAKEs in layer order (the first innermost)
        ("footprint", {"end": N([Z("base"), {"EXEC": [{"LABEL": "Game v1.0", "HOST": "win32", "EXE": "C:/g/g.exe"}]}], VARIANT="Windows"),
                       "rs": N([{"INTO": ["end"], "TAKE": [["FILES/C:/g/d3d11.dll", "FILES/C:/g/dxgi.dll"]]},
                                {"FILE": "d3d11.dll", "TARGET": "FILES/C:/g"}, Z("shaders"), {"DLL": {"dxgi": "native,builtin"}},
                                {"VARS": {"preset": {"DEFAULT": "a"}}}, {"KEEP": {"FILES/C:/g/ReShade.ini": True}},
                                {"EDIT": [{"MODE": "ConfigWrite", "SECTION": "[GENERAL]", "KEY": "PresetPath", "VALUE": "x"}], "TARGET": "FILES/C:/g/ReShade.ini"},
                                {"EDIT": [{"MODE": "ConfigWrite", "SECTION": "[S]", "KEY": "K", "VALUE": "1"}], "TARGET": "FILES/C:/g/d3d11.dll"}]),
                       "pack": N([{"INTO": ["end"]}, {"NODE": "rs"}, Z("pack")]),
                       "two": N([{"INTO": ["end"], "TAKE": [["FILES/C:/g/a.dll", "FILES/C:/g/b.dll"]]},
                                 {"INTO": ["end"], "TAKE": ["FILES/C:/g/b.dll"]}, {"FILE": "a.dll", "TARGET": "FILES/C:/g"}])},
         [("end", {}, ["rs"]), ("end", {}, ["pack"]), ("end", {}, ["two"])]),
    ]


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
    # placement between takes: an outer TAKE sees the inner node's files where the middle node PLACED them (lib/…),
    # so left-stripping "FILES/lib/" brings them to the outer TARGET; facts pass through, never re-rooted
    fx = dict(next(f for n, f, _ in fixtures() if n == "take-placed"))
    p = R(fx).resolve("g")
    assert [it["target"] for it in p["seq"] if it["kind"] == "EDIT"] == ["C:/g/b.dll"], p["seq"]
    assert p["exec"]["Tool"]["EXE"] == "C:/g/a.exe" and p["exec"]["Tool"]["WORKDIR"] == "C:/g/lib/bin", p["exec"]
    assert list(p["keep"]) == ["FILES/C:/g/save/", "REG/HKCU/S"], p["keep"]
    assert [it["target"] for it in p["seq"] if it["kind"] == "DIR"] == ["C:/g/lib/bin"]      # the plan: its placement
    p = R(fx).resolve("mid")
    assert [it["target"] for it in p["seq"] if it["kind"] == "EDIT"] == ["lib/b.dll"] and list(p["keep"])[0] == "FILES/lib/save/"
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
    # a null key deletes its whole subtree (values, subkeys, created keys) — and nothing that merely shares a prefix
    nodes = {"r": N([{"REG": {"HKCU": {"T": {"x": "1", "Sub": {"y": "2"}, "K": {}}, "TT": {"z": "3"}}}}, {"REG": {"HKCU": {"T": None}}}])}
    p = R(nodes).resolve("r")
    assert [k[1] for k in p["reg"]] == ["hkcu\\tt"] and not p["regkeys"], (p["reg"], p["regkeys"])
    # INTO beneath: a part a node contains is satisfied by what that node placed beneath it; nothing beneath = unmet
    nodes = {"base": N([Z("b")]), "m": N([{"INTO": ["base"]}, Z("m")]),
             "v1": N([{"NODE": "base"}, {"NODE": "m"}]), "v2": N([{"NODE": "m"}])}
    assert not any(e[0] == "into-unmet" for e in R(nodes).resolve("v1")["events"])
    assert ("into-unmet", "m") in R(nodes).resolve("v2")["events"]
    # WHEN over node CIDs: true when the node is in the composition (beneath, or selected), false otherwise
    cid_a = "bafkrei" + "a" * 52
    assert when(cid_a + " == true", {}, lambda c: c == cid_a) and not when(cid_a + " == true", {}, lambda c: False)
    assert when(cid_a + " == false", {}, lambda c: False) and when(cid_a, {}, lambda c: True) and not when(cid_a, {})
    assert when("%x%==1 && " + cid_a + " == true", {"x": "1"}, lambda c: True)
    assert when("bafkrei" + "a" * 51 + " == true", {}) is False    # not a node CID: a plain word, unequal to "true"
    # covering: a FILE lands at <target>/<name>; a zip's members under its target (listing faked)
    seq = [{"kind": "EDIT", "target": "C:/g/a.cfg"},
           {"kind": "FILE", "payload": "x/a.cfg", "target": "C:/g", "dir": "/d"},
           {"kind": "ZIP", "payload": "z.zip", "target": "C:/g", "dir": "/d"}]
    assert covers(seq, 1, "C:/g/a.cfg") and not covers(seq, 1, "C:/g/b.cfg")
    _NAMES["/d/z.zip"] = {"dx/x.dll", "a.cfg"}          # a zip's key is its file (_chain)
    assert covers(seq, 2, "C:/g/dx/x.dll") and not covers(seq, 2, "C:/g/x.dll")
    # guest coordinates → a runner's layout
    roots = {"C:": "pfx/drive_c", "%UserProfile%": "pfx/drive_c/users/steamuser"}
    assert to_layout("c:/g/x", roots) == "pfx/drive_c/g/x" and to_layout("%UserProfile%/s", roots) == "pfx/drive_c/users/steamuser/s"
    assert to_layout("C:x", roots) == "C:x" and to_layout("rel/p", roots) == "rel/p"
    # identity: Kubo-parity raw CID of the canonical bytes, CID/POS never part of them
    assert cid_of(b"hello\n") == "bafkreicysg23kiwv34eg2d7qweipxwosdo2py4ldv42nbauguluen5v6am"
    assert canonical({"CID": "x", "POS": [1], "LABEL": "l", "LAYERS": []}) == b'{"LABEL":"l","LAYERS":[]}'
    for name, fx, cases in fixtures():
        r = Resolver({k: (dict(v, CID=k), "") for k, v in fx.items()})
        for root, inst, gs in cases:
            r.resolve(root, inst, {}, gs)
        if name == "graft-when":
            # a graft's own declaration gates its own layer: the default applies it, the instance can turn it off
            labs = lambda p: [it.get("payload") for it in p["seq"]]
            assert labs(r.resolve("v", {}, {}, ["g"])) == ["b", "wnd"], labs(r.resolve("v", {}, {}, ["g"]))
            assert labs(r.resolve("v", {"win": "0"}, {}, ["g"])) == ["b", "cls"]
        if name == "into+parts":
            # the reference helpers tools use: NODE, INTO, and a condition's CIDs, renamed in place
            g4 = r.nodes["g4"][0]
            assert node_refs(g4) == [("INTO", "v1"), ("WHEN", P1)], node_refs(g4)
            assert hard_links(g4) == ["v1"]
            new = "bafkrei" + "q" * 52
            g4b = remap_node(g4, {P1: new, "v1": "v9"})
            assert g4b["LAYERS"][0]["INTO"] == ["v9"] and g4b["LAYERS"][0]["WHEN"] == new + " == false", g4b
            assert when_cids("'" + P1 + "' == %x% && " + P1) == [(len(P1) + 13, P1)], when_cids("'" + P1 + "' == %x% && " + P1)
            gi = graft_index(r.nodes)
            off = lambda root, parts=(): offered_grafts(r.nodes, gi, r.resolve(root, {}, {}, list(parts)), "9")[0]
            # the hard link: offered on the end it names, never on a later end that merely contains it, never for a
            # part that names a lower node; a part on a part only once that part is applied
            assert off("v1") == ["g2", P1, "g4", "g5"], off("v1")
            assert off("vup") == [] and "low" not in off("v1")
            assert "g3" not in off("v1") and "g3" in off("v1", [P1])
            # several INTO layers: into ALL of them (a patch between two mods), offered once both are applied
            assert "pp" not in off("v1", [P1]) and "pp" not in off("v1", ["g2"]) and "pp" in off("v1", [P1, "g2"])
            assert applied_grafts(r, gi, "v1", "9", ["pp", "g2", P1]) == [P1, "g2", "pp"], applied_grafts(r, gi, "v1", "9", ["pp", "g2", P1])
            assert applied_grafts(r, gi, "v1", "9", ["pp", P1]) == [P1], applied_grafts(r, gi, "v1", "9", ["pp", P1])
            # the selection is a set: a part follows what it goes into, whatever the order asked
            assert applied_grafts(r, gi, "v1", "9", ["g3", P1]) == [P1, "g3"], applied_grafts(r, gi, "v1", "9", ["g3", P1])
            assert applied_grafts(r, gi, "v1", "9", [P1, "g3", P1]) == [P1, "g3"]
            assert applied_grafts(r, gi, "v1", "9") == [P1, "g3"], applied_grafts(r, gi, "v1", "9")   # fresh: g4 excluded by P1
            # a part whose INTO's WHEN fails on the selection is dropped; alone it applies
            d = []
            assert applied_grafts(r, gi, "v1", "9", [P1, "g4"], dropped=d) == [P1] and d == ["g4"], d
            assert applied_grafts(r, gi, "v1", "9", ["g4"]) == ["g4"]
            # a layer gated on another part's presence (FOMOD auto-detect as structure)
            labs = lambda p: [it.get("payload") for it in p["seq"]]
            assert "g5" in labs(r.resolve("v1", {}, {}, applied_grafts(r, gi, "v1", "9", ["g5"])))
            assert labs(r.resolve("v1", {}, {}, ["g5"])).count("b") == 1
            assert labs(r.resolve("v1", {}, {}, [P1, "g5"], [P1, "g5"])).count("b") == 1   # base: one occurrence (held)
            # a part never offered here is dropped and said so
            d = []
            assert applied_grafts(r, gi, "vup", "9", [P1], dropped=d) == [] and d == [P1]
            assert applied_grafts(r, gi, "v1", None) == []          # no tile: nothing is pre-ticked
    print("self-test OK")
    # EVAL arithmetic: the same table as test_varsubst.cpp's eval_integer_undefined_results_fail (64-bit, C semantics)
    for bad in ["(~0x7fffffffffffffff)/-1", "(~0x7fffffffffffffff)%-1", "1<<64", "1<<-1", "8>>64", "0x1ffffffffffffffff",
                "99999999999999999999", "(" * 200 + "1" + ")" * 200, "-" * 200 + "1", "1\u00a0+1", "", "%a%*2", "3+", "(1", "4/0", "12abc", "٣+1"]:
        assert evaluate_integer(bad) is None, bad
    for expr, want in [("1*2 | 0*32 | (1-0)*64", 66), ("0x40 & ~0x40", 0), ("1 << 3 | 2", 10), ("2 + 3 | 8", 13),
                       ("0x7fffffffffffffff + 1", -(1 << 63)), ("-1 << 63", -(1 << 63)), ("-8 >> 1", -4), ("-7 / 2", -3),
                       ("-7 % 2", -1), ("9007199254740993 / 1", 9007199254740993), ("(" * 30 + "1" + ")" * 30, 1)]:
        assert evaluate_integer(expr) == str(want), (expr, evaluate_integer(expr))
    assert resolve_vars({"x": {"DEFAULT": "1+1", "EVAL": "true"}}, {}, {})["x"] == "1+1"   # only a boolean true evaluates

if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        self_test(); sys.exit(0)
    lib, cid = sys.argv[1], sys.argv[2]
    inst = dict(a.split("=", 1) for a in sys.argv[3:] if "=" in a)
    r = Resolver(load_nodes(lib)).resolve(cid, inst)
    r["reg"] = {"|".join(map(str, k)): v for k, v in r["reg"].items()}
    r["regkeys"] = {"|".join(map(str, k)): v for k, v in r["regkeys"].items()}
    print(json.dumps(r, indent=1, default=str)[:20000])
