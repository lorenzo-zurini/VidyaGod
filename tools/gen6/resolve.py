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

# Case folding is ASCII-only, as the C++ engine's (fold.cpp Lower): addresses compare by it everywhere.
_LO = str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz")

TYPE_KEYS = ["ZIP", "FILE", "DELTA", "CODED", "DIR", "NODE", "EDIT", "REG", "VARS", "ENV", "DLL", "EXEC", "KEEP", "INTO"]
_NODE_CID = re.compile(r"bafkrei[a-z2-7]{52}")
CONTENT = ("ZIP", "FILE", "DELTA", "CODED", "DIR")
MEMBERSHIP_ROUNDS = 64                                      # the membership fixpoint's bound (12 §12.1): beyond it, refused


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
    return "b" + base64.b32encode(raw).decode().translate(_LO).rstrip("=")


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
                if isinstance(j, dict) and isinstance(j.get("LAYERS"), list):
                    try:                                       # a label-named node: its CID is that of its canonical bytes
                        out[j["CID"] if "CID" in j else cid_of(canonical(j))] = (j, dp)
                    except ValueError:
                        continue
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
        return "true" if value.translate(_LO) in ("1", "true", "yes") else "false"
    if fmt == "winpath":
        return value.replace("/", "\\")
    if fmt == "upper":
        return value.upper()
    if fmt == "lower":
        return value.translate(_LO)
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
            v = int(t, 16) if t[:2].translate(_LO) == "0x" else int(t)
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
        if path == a or path.translate(_LO).startswith(a.translate(_LO) + "/") or path.translate(_LO) == a.translate(_LO):
            return roots[a] + path[len(a):]
    return path


def is_absolute(path):
    return bool(ANCHOR.match(path))


def place(path, prefix):
    """A relative path inside a node placed at `prefix` lands under it; an anchored path stays."""
    if not prefix or is_absolute(path):
        return path
    return prefix + ("/" + path if path else "")


def reg_tree(L):
    """A REG layer's path tree with its KEY (the key its paths are under, 07) put in front: {full key path: values}."""
    base = L.get("KEY")
    if not base:
        return L["REG"]
    return {(base + "\\" + p) if p else base: v for p, v in L["REG"].items()}


def take_match(addr, sel):
    """Does selection `sel` (an address or a namespace) cover `addr`? → the remainder after the selection's
    left-stripped base (the part the selection keeps), else None. REG addresses are one key path after the namespace,
    its keys separated by backslashes (07): a key selects itself and every key below it; registry facts are never
    re-rooted."""
    if addr.startswith("EXEC/") and sel.startswith("EXEC/"):   # the EXEC tree (09): a subtree is kept at its own address
        base = sel.rstrip("/")
        return addr[5:] if addr == base or addr.startswith(base + "/") else None
    if addr.startswith("REG/") and sel.startswith("REG/"):
        base = sel.rstrip("/").rstrip("\\")
        if base == "REG" or addr == base or addr.translate(_LO).startswith(base.translate(_LO) + "\\") or addr.translate(_LO) == base.translate(_LO):
            return addr[4:]
        return None
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
        reads as selected (default: grafts). Gen 9 (12 §12.2): phase 0, the composition's members (a WHEN naming a
        node CID reads the FINAL composition: iterated until stable); phase 1, the variables; then each occurrence is
        composed once from its own list (provided values move to a later mention, carried ones never override a value
        already in the composition), and the facts fold over the result."""
        instance, builtins = dict(instance or {}), dict(builtins or {})
        wv = dict(builtins); wv.update(self.phase1(root, builtins, instance, grafts))
        self.wv = wv
        self.selected = set(selection if selection is not None else grafts)
        members, seen = frozenset(), []
        for _ in range(MEMBERSHIP_ROUNDS):
            self._members = members
            self.events, self.order, self.count, self.memo, self._into_checks = [], [], {}, {}, []
            self.error = ""
            res = self._compose(root, None, "", ())
            for g in grafts:                               # parts after the end, each after what it goes into
                res = self._combine(res, self._compose(g, None, "", ()), self._occ(g, None, ""), True)
            got = frozenset(self.count)
            if got == members:
                break
            if got in seen:
                self.error = "the conditions naming nodes contradict each other: " + ", ".join(sorted(got ^ members))
                break
            seen.append(members); members = got
        else:
            self.error = f"the conditions naming nodes did not settle in {MEMBERSHIP_ROUNDS} rounds"
        self.slots = res
        for cid, node_layers in self._into_checks:            # INTO judged on the final composition
            if not any(m in self.count or m in self.selected for m in node_layers):
                self.events.append(("into-unmet", cid))
        plan = self._fold(instance, builtins)
        plan["parts"] = list(grafts)
        return plan

    def _present(self, cid):
        return cid in self._members or cid in self.selected

    def _occ(self, cid, view, prefix):
        return (cid, json.dumps(list(view_key_steps(view)), sort_keys=True) if view_takes(view) else "", prefix)

    @staticmethod
    def _identity(s):
        """Content by what it is (its SOURCE at its placed target, through its view): one CID placed twice at one
        place is one value. EDITs and facts by their writer (the occurrence and layer)."""
        L = s["layer"]
        if s["kind"] in CONTENT and L.get("SOURCE"):
            return ("C", s["kind"], L["SOURCE"], L.get("TARGET", "FILES"), s["prefix"],
                    json.dumps(list(view_key_steps(s["view"])), sort_keys=True) if view_takes(s["view"]) else "")
        return ("W", s["owner"], s["at"])

    def _addr_pairs(self, x):
        """The addresses a slot writes, as (raw, mapped) pairs: raw is the address in the slot's own coordinates (what
        the fold checks an except against), mapped is where its view lands it (what coverage compares). EXEC is one
        coordinate (its restore works on mapped paths). Content has none (an archive's insides are not the resolver's)."""
        L, k, pre, view = x["layer"], x["kind"], x["prefix"], x["view"]
        out = []
        if k == "EDIT":
            f = "FILES/" + place(ns_path(L["TARGET"])[1], pre)
            out.append((f, f))
        elif k == "REG":
            for p, vals in reg_tree(L).items():
                if self._map_fact(view, "REG/" + p) is None:      # REG is taken where it is: the view only filters
                    continue
                if not vals:                                   # a key created ({}) or deleted (null) is the key's address
                    a = "REG/" + p.translate(_LO); out.append((a, a))
                for v in (vals or {}):
                    a = "REG/" + p.translate(_LO) + "#" + v.translate(_LO); out.append((a, a))
        elif k in ("DLL", "ENV", "VARS"):
            for n in L[k]:
                a = self._map_fact(view, k + "/" + n)
                if a is not None:
                    if k == "DLL":
                        out.append(("DLL/" + n.translate(_LO), "DLL/" + a.split("/", 1)[1].translate(_LO)))
                    else:
                        out.append((k + "/" + n, a))
        elif k == "KEEP":
            for a0 in L["KEEP"]:                               # mapped as the fold maps it (FILES placed, REG by key)
                inner, unit = a0, None
                if ns_path(a0)[0] == "REG" and "#" in a0:
                    inner, unit = a0.rsplit("#", 1)
                m = self._map_files(view, inner, pre) if ns_path(inner)[0] == "FILES" else self._map_fact(view, inner)
                if m is not None:
                    out.append(("KEEP/" + a0, "KEEP/" + m + ("#" + unit if unit is not None else "")))
        elif k == "EXEC":
            def walk(addr, v):
                a = self._map_fact(view, addr)
                if a is not None:
                    out.append((a, a)); return
                if isinstance(v, dict):
                    for f, w in v.items():
                        walk(addr + "/" + f, w)
            for n, e in L["EXEC"].items():
                walk("EXEC/" + n, e)
        return out

    def _addrs(self, x):
        """Where a slot's writes land (mapped)."""
        return {m for r, m in self._addr_pairs(x)}

    @staticmethod
    def _covers(a, m):
        """Address a covers m: the same, or a holds it (a key its values and subkeys, an entry its fields)."""
        return a == m or m.startswith(a + "/") or (a.startswith("REG/") and (m.startswith(a + "#") or m.startswith(a + "\\")))

    def _overrides(self, x, after):
        """Does re-providing x change what wins: something after its old place writes where it writes?"""
        if x["kind"] in CONTENT:
            t = place(ns_path(x["layer"].get("TARGET", "FILES"))[1], x["prefix"]).translate(_LO)
            def under(u):
                u = u.translate(_LO)
                return t == "" or u == "" or u == t or u.startswith(t + "/") or t.startswith(u + "/")
            for y in after:
                if y["kind"] in CONTENT and under(place(ns_path(y["layer"].get("TARGET", "FILES"))[1], y["prefix"])):
                    return True
                if y["kind"] == "EDIT" and under(place(ns_path(y["layer"]["TARGET"])[1], y["prefix"])):
                    return True
            return False
        mine = self._addrs(x)
        return any(self._covers(a, m) or self._covers(m, a) for y in after for a in self._addrs(y) for m in mine)

    def _except_for(self, x, later):
        """What a moved slot x must not re-provide: what CARRIED slots after it in its item rewrite — an address of
        x's they write or hold (a key deleted over x's value), or a part of an address x writes whole (the tile of an
        entry x writes): that part stays as below."""
        if x["kind"] in CONTENT:
            t = "FILES/" + place(ns_path(x["layer"].get("TARGET", "FILES"))[1], x["prefix"])
            return {a for y in later if y["kind"] == "EDIT" for a in self._addrs(y)
                    if a == t or a.startswith(t.rstrip("/") + "/") or t == "FILES/"}
        mine = self._addr_pairs(x)
        out = set()
        for y in later:
            for a in self._addrs(y):
                for r, m in mine:
                    if self._covers(a, m):
                        out.add(r)                             # the fold skips x's own address (raw)
                    elif self._covers(m, a):
                        # x writes the whole of it: an entry keeps that part from below (mapped: its restore path); a
                        # registry key written whole stays below whole; anything else, x's own address
                        out.add(a if m.startswith("EXEC/") else r)
        return out

    def _combine(self, cur, sub, owner, take_all):
        """An item's composition laid over what is composed so far: a carried slot already present is a stale copy
        (skipped); a provided one already present moves here (later wins) — except what carried slots after it in its
        item rewrite, which stays where it was (its old place kept, the moved copy excepting it); anything new is
        appended."""
        # what was composed BEFORE this item: its slots are matched against that only, never against each other (an
        # item's composition is already decided — its own re-provided copy among it)
        idx = {self._identity(x): i for i, x in enumerate(cur)}
        out, drop = list(cur), set()
        for j, x in enumerate(sub):
            k = self._identity(x)
            provided = take_all or x["owner"] == owner
            if k in idx and idx[k] not in drop:
                if not provided:
                    continue
                if self._overrides(x, out[idx[k] + 1:]):          # a re-mention raising it over what came after it
                    ev = ("raise", x["from"])
                    if ev not in self.events:
                        self.events.append(ev)
                exc = self._except_for(x, [y for y in sub[j + 1:] if not (take_all or y["owner"] == owner)])
                if exc:
                    x = dict(x, exc=sorted(exc | set(x.get("exc", ()))))
                    prev = out[idx[k]]                     # an earlier re-provided copy this one covers goes
                    if prev.get("exc") and set(prev["exc"]) <= set(x["exc"]):
                        drop.add(idx[k])
                else:
                    drop.add(idx[k])
            out.append(x)
        return [x for i, x in enumerate(out) if i not in drop]

    def _compose(self, cid, view, prefix, onpath):
        """view: how this occurrence's addresses reach the root: its (TAKE, TARGET) steps, innermost first."""
        occ = self._occ(cid, view, prefix)
        if occ in self.memo:
            return self.memo[occ]
        if cid in onpath:
            self.events.append(("cycle", cid)); return []
        if cid not in self.nodes:
            self.events.append(("missing", cid)); return []
        node, bdir = self.nodes[cid]
        Ls = node["LAYERS"]
        if any("WHEN" in L and not when(L["WHEN"], self.wv, self._present) for L in into_layers(Ls)):
            self.events.append(("into-off", cid)); self.memo[occ] = []; return []
        for L in reversed(into_layers(Ls)):
            if "TAKE" in L:
                view = within(view, footprint_take(L["TAKE"]), "", True)
        if occ not in self.order:
            self.order.append(occ)
        self.count[cid] = self.count.get(cid, 0) + 1
        res = []
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
                v2 = within(view, L.get("TAKE"), at)
                res = self._combine(res, self._compose(L["NODE"], v2, sub, onpath + (cid,)), self._occ(L["NODE"], v2, sub), "TAKE" in L)
            elif t == "INTO":
                self._into_checks.append((cid, L["INTO"]))
            else:
                s = {"kind": t, "layer": L, "at": i, "owner": occ, "from": cid, "dir": bdir, "prefix": prefix, "view": view}
                res = self._combine(res, [s], occ, True)
        self.memo[occ] = res
        return res

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
            exc = set(s.get("exc", ()))
            if k in CONTENT:
                ns, tp = ns_path(L.get("TARGET", "FILES"))
                item = {"kind": k, "payload": L[k], "dir": s["dir"], "source": L.get("SOURCE"), "size": L.get("SIZE"),
                        "target": place(tp, prefix), "own": tp,
                        "view": [[t, a] for t, a in view_steps(view)] if view_takes(view) else None,
                        "from": s["from"], "at": s["at"]}
                if k in ("DELTA", "CODED") and "BASE_SOURCES" in L:           # its bases, by SOURCE CID, in order
                    item["base_sources"] = L["BASE_SOURCES"]
                if k == "CODED":                                              # its description (inline) and its name
                    item["description"] = L.get("DESCRIPTION")
                    item["name"] = L.get("NAME", "")
                if exc:                                                       # a re-provided copy: these paths stay below
                    item["except"] = sorted(a.split("/", 1)[1] for a in exc)
                seq.append(item)
            elif k == "EDIT":
                ns, tp = ns_path(L["TARGET"])
                a = self._map_files(view, "FILES/" + tp, prefix)
                if a is None:
                    continue
                seq.append({"kind": "EDIT", "ops": L["EDIT"], "target": ns_path(a)[1], "from": s["from"], "at": s["at"]})
            elif k == "REG":
                for arch in (L.get("ARCH") or [None]):
                    self._fold_reg(reg_tree(L), arch, reg, regkeys, view, exc)
            elif k == "DLL":
                for n, o in L["DLL"].items():
                    if "DLL/" + n.translate(_LO) in exc:
                        continue
                    a = self._map_fact(view, "DLL/" + n)
                    if a is None:
                        continue
                    n2 = a.split("/", 1)[1].translate(_LO)
                    if o is None:
                        dll.pop(n2, None)
                    else:
                        dll.pop(n2, None); dll[n2] = o
            elif k == "ENV":
                for n, v in L["ENV"].items():
                    if "ENV/" + n in exc:
                        continue
                    a = self._map_fact(view, "ENV/" + n)
                    if a is None:
                        continue
                    n = a.split("/", 1)[1]
                    env.pop(n, None); env[n] = v
            elif k == "VARS":
                for n, d in L["VARS"].items():
                    if "VARS/" + n in exc:
                        continue
                    a = self._map_fact(view, "VARS/" + n)
                    if a is None:
                        continue
                    n = a.split("/", 1)[1]
                    decls.pop(n, None); decls[n] = d
            elif k == "EXEC":
                # nothing folds (09 §9.2): the EXEC tree is addressed EXEC/<label>/<field>/…; a write replaces the subtree
                # it names whole, a TAKE may select (and rename) any subtree
                before = copy.deepcopy(exe) if any(a.startswith("EXEC/") for a in exc) else None
                for lab0, e in L["EXEC"].items():
                    self._fold_exec(exe, "EXEC/" + lab0, e, view, prefix)
                for a in (exc if before is not None else ()):      # a re-provided copy: these parts stay as below
                    if a.startswith("EXEC/"):
                        restore_path(exe, before, a.split("/")[1:])
            elif k == "KEEP":
                for a0, v in L["KEEP"].items():
                    if "KEEP/" + a0 in exc:
                        continue
                    unit = None                                     # a value of a key: <key>#<value> (13, like INI
                    if ns_path(a0)[0] == "REG" and "#" in a0:      # units); the default value is the empty name, "<key>#"
                        a0, unit = a0.rsplit("#", 1)
                    a = self._map_files(view, a0, prefix) if ns_path(a0)[0] == "FILES" else self._map_fact(view, a0)
                    if a is None:
                        continue
                    if unit is not None:
                        a = a + "#" + unit
                    keep.pop(a, None); keep[a] = v
        return {"seq": seq, "reg": reg, "regkeys": regkeys, "dll": dll, "env": env, "exec": exe, "keep": keep,
                "decls": decls, "order": [o[0] for o in self.order], "events": self.events, "when_vars": self.wv,
                "error": getattr(self, "error", "")}

    def _place_entry_paths(self, e, view, prefix):
        """An entry's file paths go where the node's files went."""
        if (prefix or view_takes(view)) and isinstance(e, dict):
            for f in ("EXE", "WORKDIR"):
                if isinstance(e.get(f), str):
                    m = self._map_files(view, "FILES/" + e[f], prefix)
                    e[f] = ns_path(m)[1] if m is not None else place(e[f], prefix)
        return e

    def _fold_exec(self, exe, addr, value, view, prefix):
        """Write value at EXEC address addr (whole), as the view maps it; where the view takes only parts of it, each
        taken subtree lands at its own (possibly renamed) address."""
        a = self._map_fact(view, addr)
        if a is not None:
            parts = a.split("/")[1:]                           # [label, field, …]
            v = copy.deepcopy(value)
            if len(parts) == 1:
                exe.pop(parts[0], None)
                if v is None:                                  # null deletes the entry (an inherited label renamed)
                    return
                exe[parts[0]] = self._place_entry_paths(v, view, prefix)
            else:
                if parts[1] in ("EXE", "WORKDIR") and isinstance(v, str) and (prefix or view_takes(view)):
                    m = self._map_files(view, "FILES/" + v, prefix)
                    v = ns_path(m)[1] if m is not None else place(v, prefix)
                node = exe.setdefault(parts[0], {})
                for f in parts[1:-1]:
                    node = node.setdefault(f, {}) if isinstance(node.get(f), dict) else node.setdefault(f, {})
                node[parts[-1]] = v
            return
        if isinstance(value, dict):                            # not taken whole: perhaps a part of it is
            for f, v in value.items():
                self._fold_exec(exe, addr + "/" + f, v, view, prefix)

    def _fold_reg(self, tree, arch, reg, regkeys, view, exc=()):
        """REG is a path tree (07): key path (backslashes, the hive first) -> {value name: value}; {} creates the key;
        null deletes the key and all below it; a value null deletes the value. Addresses: REG/<key path>."""
        for p, vals in tree.items():
            if self._map_fact(view, "REG/" + p) is None or "REG/" + p.translate(_LO) in exc:
                continue
            if vals is None:
                sub = p.translate(_LO)
                for key in [key for key in reg if key[0] == arch and (key[1] == sub or key[1].startswith(sub + "\\"))]:
                    del reg[key]
                for key in [key for key in regkeys if key[0] == arch and (key[1] == sub or key[1].startswith(sub + "\\"))]:
                    del regkeys[key]
                continue
            if not vals:
                regkeys[(arch, p.translate(_LO))] = p
                continue
            for k, v in vals.items():
                if "REG/" + p.translate(_LO) + "#" + k.translate(_LO) in exc:
                    continue
                if v is None:
                    reg.pop((arch, p.translate(_LO), k.translate(_LO)), None)
                else:
                    reg[(arch, p.translate(_LO), k.translate(_LO))] = (p, k, v)


# A view is how an occurrence's addresses reach the root: (take, at, outer view, any step takes) — a shared linked
# list, innermost step first, so a chain 900 deep costs one tuple per level (not a copied list).
def restore_path(cur, before, parts):
    """cur[parts] back to what it was in before (removed where before had none), in place."""
    src = before
    for q in parts:
        src = src.get(q) if isinstance(src, dict) else None
        if src is None:
            break
    node = cur
    for q in parts[:-1]:
        if not isinstance(node.get(q), dict):
            if src is None:
                return
            node[q] = {}
        node = node[q]
    if src is None:
        node.pop(parts[-1], None)
    else:
        node[parts[-1]] = copy.deepcopy(src)


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
            if it.get("except"):
                e["except"] = it["except"]
            if it["kind"] == "CODED":
                e.update(description=it.get("description"), name=it.get("name", ""))
        e.update({"target": it["target"], "from": it["from"], "at": it["at"]})
        seq.append(e)
    return {"seq": seq,
            "reg": sorted([[a, p, n, dp, dn, v] for (a, p, n), (dp, dn, v) in plan["reg"].items()], key=lambda r: (r[0] or "", r[1], r[2])),
            "regkeys": sorted([[a, p, dp] for (a, p), dp in plan["regkeys"].items()], key=lambda r: (r[0] or "", r[1])),
            "dll": [[k, v] for k, v in plan["dll"].items()], "env": [[k, v] for k, v in plan["env"].items()],
            "exec": [[k, v] for k, v in plan["exec"].items()], "keep": [[k, v] for k, v in plan["keep"].items()],
            "decls": [[k, v] for k, v in plan["decls"].items()], "order": plan["order"], "parts": plan.get("parts", []),
            "events": [list(e) for e in plan["events"]], "when_vars": dict(sorted(plan["when_vars"].items())), "error": plan.get("error", "")}


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
    name. [(how, cid), ...] with how in NODE / INTO / WHEN. (A coded stream's description is inline: no reference.)"""
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
    return out


def hard_links(n):
    """What a closure follows: what a node contains and what it goes into (never a condition's CIDs)."""
    return [c for how, c in node_refs(n) if how in ("NODE", "INTO")]


def remap_node(n, m):
    """A copy of node n with every reference renamed by m {old: new}: NODE, INTO members, CIDs inside a WHEN (the rest
    of the condition kept byte for byte). Payload strings are never touched."""
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
    return o


# ---------------------------------------------------------------- parts (12 §12.5, 23.0)
def owns_game_entry(n):
    """A node's own layers write a way to run (an entry object without GUEST): for a part, it is a row, never ticked
    (09 §9.4). One definition for the engine, the shelf and the validator (fold.cpp OwnsGameEntry)."""
    return any(isinstance(L, dict) and isinstance(L.get("EXEC"), dict) and
               any(isinstance(e, dict) and not e.get("GUEST") for e in L["EXEC"].values()) for L in n.get("LAYERS", []))


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
    # pre-ticked: a part whose INTO layer naming what it goes into here says RECOMMENDED; never one that carries an entry
    # of its own (a graft card or row is launched from its own face, 09 / 23)
    def ticks(g):
        Ls = nodes[g][0]["LAYERS"]
        if owns_game_entry(nodes[g][0]):
            return False
        return any(L.get("RECOMMENDED") is True and any(m in points for m in L["INTO"]) for L in into_layers(Ls))
    ticked = [g for g in offered if ticks(g)]
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


def graft_bases(nodes, g):
    """The ends a part runs on (09 §9.4, the C++ GraftBases): what it goes INTO, followed through parts it goes into,
    down to variants; ordered by VARIANT, then CID."""
    seen, todo, out = set(), [m for L in into_layers(nodes[g][0]["LAYERS"]) for m in L["INTO"]], []
    while todo:
        x = todo.pop()
        if x in seen or x not in nodes:
            continue
        seen.add(x)
        n = nodes[x][0]
        if into_layers(n.get("LAYERS", [])):
            todo += [m for L in into_layers(n["LAYERS"]) for m in L["INTO"]]
        elif n.get("VARIANT"):
            out.append(x)
    return sorted(out, key=lambda c: (nodes[c][0].get("VARIANT", ""), c))


def into_chain(nodes, g, base):
    """The parts between part g and the end base (FE → UserPatch → 1.0e over 1.0c: [1.0e, UserPatch]), innermost
    first; None when g does not run on base."""
    def walk(x, path):
        for L in into_layers(nodes[x][0].get("LAYERS", [])):
            for m in L["INTO"]:
                if m == base:
                    return path
                if m in nodes and into_layers(nodes[m][0].get("LAYERS", [])) and m not in path:
                    r = walk(m, [m] + path)
                    if r is not None:
                        return r
        return None
    return walk(g, [])


def launch_row(nodes, row, base=None, requested=None, instance=None, builtins=None):
    """What launching a row resolves to, as the engine launches it (launchresolver's InitializeFromNode): a variant
    with its parts (requested, else a fresh instance's ticks); a part that is a row (a graft card's row, or a graft
    version) runs on an end it goes INTO (base, else the first of graft_bases), with the parts between applied and the
    end's pre-ticked plain mods. Returns (root, applied grafts, plan)."""
    R, idx = Resolver(nodes), graft_index(nodes)
    n = nodes[row][0]
    face = next((e["TILE"]["UID"] for L in n["LAYERS"] if isinstance(L.get("EXEC"), dict) for e in L["EXEC"].values()
                 if isinstance(e, dict) and isinstance(e.get("TILE"), dict) and not e.get("GUEST")), None)
    if not into_layers(n["LAYERS"]):
        applied = applied_grafts(R, idx, row, face, requested, instance, builtins)
        return row, applied, R.resolve(row, instance, builtins, applied)
    base = base or (graft_bases(nodes, row) or [None])[0]
    chain = into_chain(nodes, row, base) if base else None
    if chain is None:
        raise ValueError(f"{row} runs on no end here")
    if requested is None:   # a fresh instance: the end's plain mods ticked by default, never a row (a part with an entry)
        requested = [g for g in applied_grafts(R, idx, base, face, None, instance, builtins)
                     if not any(isinstance(L.get("EXEC"), dict) for L in nodes[g][0]["LAYERS"])]
    want = list(dict.fromkeys(list(requested) + chain + [row]))
    applied = applied_grafts(R, idx, base, face, want, instance, builtins)
    return base, applied, R.resolve(base, instance, builtins, applied)


# ---------------------------------------------------------------- the shelf (§2)
def shelf(nodes, builtins=None):
    """Tiles merged by UID and nested by PARENTUID; rows = variant nodes × the tiled entries in their fold.
    -> [{"uid", "title", "faces": [{"uid", "title", "depth", "rows": [variant cid]}]}], families by title."""
    R = Resolver(nodes)
    present = {}                                           # face uid -> [(variant cid, tile, recommended)]
    for h, (n, _) in nodes.items():
        if not n.get("VARIANT"):
            continue
        plan = R.resolve(h, {}, builtins or {})
        for e in plan["exec"].values():
            if not e.get("GUEST") and e.get("TILE"):
                present.setdefault(e["TILE"]["UID"], []).append((h, e["TILE"], e.get("RECOMMENDED") is True))

    def rank(uid):
        def key(v):
            return (not v[2], nodes[v[0]][0].get("VARIANT", ""), v[0])
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
        faces = sorted(uids, key=lambda u: (depth(u), tiles[u].get("TITLE", "")))
        out.append({"uid": r, "title": tiles[faces[0]].get("TITLE", ""),
                    "faces": [{"uid": u, "title": tiles[u].get("TITLE", ""), "depth": depth(u), "rows": [v for v, _, _ in rank(u)]}
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
                    _NAMES[key] = {n.rstrip("/").translate(_LO) for n in z.namelist()}
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
    t = S(c["target"]).translate(_LO)
    if c["kind"] == "DIR" or not c.get("dir"):
        return None
    if c["kind"] == "FILE":
        return {(t + "/" if t else "") + os.path.basename(c["payload"]).translate(_LO)}
    names = _member_names(seq, j)
    if names is None:
        return None
    return {(t + "/" if t else "") + n for n in names}


def covers(seq, j, f, vars_=None):
    """Does content seq[j] replace file f (f spelled like the targets: substituted with the same vars_)?"""
    fl = f.translate(_LO)
    p = provided(seq, j, vars_)
    if p is None:
        t = (subst(seq[j]["target"], vars_) if vars_ else seq[j]["target"]).translate(_LO)
        return fl == t or fl.startswith(t + "/") or t == ""
    return fl in p


P1 = "bafkrei" + "b" * 52                                   # a part named in WHEN conditions: a node CID's shape
DLC, F1, F2 = ("bafkrei" + c * 52 for c in "dfg")            # nodes conditions name (cid-when)
CHAIN = ["bafkrei" + "h" * 50 + chr(97 + i // 26) + chr(97 + i % 26) for i in range(70)]   # (unsettled)


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
                          {"REG": {"HKCU\\K": {"Mini-map Colors": "%mm:dword%"}}}])},
         [("v", {}, []), ("v", {"grey": "0", "purple": "1"}, [])]),
        ("map", {"v": N([{"VARS": {"k": {"DEFAULT": "keypad 0", "UI": {"CONTROL": "key"}}, "dir": {"DEFAULT": "C:/x"},
                                   "keys": {"MAP": {"keypad 0": "[0]", "enter": "return", "home": "%dir%/h"}},
                                   "strict": {"MAP": {"a": "A"}, "ELSE": "none"},
                                   "off": {"MAP": {"keypad 0": "NO"}, "WHEN": "%dir%==never"},
                                   "bound": {"DEFAULT": "%k:map:keys%"}, "sb": {"DEFAULT": "%k:map:strict%"},
                                   "ob": {"DEFAULT": "%k:map:off%"}}},
                         {"REG": {"HKCU\\K": {"Bind": "%k:map:keys%", "Strict": "%k:map:strict%", "Off": "%k:map:off%"}}},
                         # phase 1 resolves the mapped values; these WHENs make them visible in the plan
                         {"DLL": {"kp0": "n"}, "WHEN": "%bound%==[0]"}, {"DLL": {"plain_w": "n"}, "WHEN": "%bound%==w"},
                         {"DLL": {"tok": "n"}, "WHEN": "%bound%==C:/x/h"}, {"DLL": {"else": "n"}, "WHEN": "%sb%==none"},
                         {"DLL": {"strict_a": "n"}, "WHEN": "%sb%==A"}, {"DLL": {"off_leak": "n"}, "WHEN": "%ob%==NO"}])},
         [("v", {}, []), ("v", {"k": "w"}, []), ("v", {"k": "home"}, []), ("v", {"k": "a"}, [])]),
        ("when", {"opt": N([Z("opt"), {"VARS": {"inner": {"DEFAULT": "%o%x"}}}]),
                  "v": N([{"VARS": {"o": {"DEFAULT": "0"}, "g": {"DEFAULT": "a", "WHEN": "%o%==1"}}},
                          {"NODE": "opt", "WHEN": "%o%==1 && %g%==a"}, Z("v"), {"DLL": {"x": "n"}, "WHEN": "!(%o%==1)"}])},
         [("v", {}, []), ("v", {"o": "1"}, [])]),
        # nothing folds (09 §9.2): a later entry of one label replaces the earlier whole; a TAKE reaches any subtree of
        # an entry (the tile, its cover), renamed by a pair; a whole write sets every field below it
        ("exec-whole", {"base": N([{"EXEC": {"Play": {"HOST": "win32", "EXE": "C:/g/g.exe",
                                                       "TILE": {"UID": "1", "TITLE": "A", "META": {"YEAR": "1999"},
                                                                "COVER": {"FILE": "a.png", "SOURCE": "QmA", "SIZE": 5}}}}}]),
                        "v": N([{"NODE": "base"}, {"EXEC": {"Play": {"HOST": "win32", "EXE": "C:/g/v.exe", "TILE": {"UID": "1"}}}}]),
                        "iron": N([{"EXEC": {"Quake - Ironwail": {"HOST": "linux", "EXE": "ironwail", "RECOMMENDED": True}}},
                                   {"NODE": "base", "TAKE": [["EXEC/Play/TILE", "EXEC/Quake - Ironwail/TILE"]]}]),
                        "cov": N([{"NODE": "base"}, {"EXEC": {"Play": {"HOST": "win32", "EXE": "C:/g/g.exe", "TILE": {"UID": "1"}}}},
                                  {"NODE": "base", "TAKE": ["EXEC/Play/TILE/COVER"]}]),
                        "args": N([{"NODE": "base", "TAKE": ["EXEC/Play"]}, {"EXEC": {"Play": {"HOST": "win32", "EXE": "C:/g/g.exe", "ARGS": ["-w"]}}}]),
                        # null deletes the entry: a version renaming the entry it inherits
                        "ren": N([{"NODE": "base"}, {"EXEC": {"Play": None, "A v2": {"HOST": "win32", "EXE": "C:/g/g2.exe", "TILE": {"UID": "1"}}}}])},
         [("v", {}, []), ("base", {}, []), ("iron", {}, []), ("cov", {}, []), ("args", {}, []), ("ren", {}, [])]),
        # composition by history (gen 9, 05 §5.4): a re-mention re-provides that node's OWN layers (later wins); a carried
        # copy of what is already composed never comes back; an edit applies once
        ("raise", {"dgv": N([Z("dgv", "FILES/C:/g"), {"NODE": "cfg"}]),
                   "cfg": N([{"EDIT": [{"MODE": "AppendLine", "VALUE": "x"}], "TARGET": "FILES/C:/g/dgVoodoo.conf"}]),
                   "a": N([Z("game"), {"NODE": "dgv"}]), "b": N([{"NODE": "a"}, Z("b-over")]),
                   "c": N([{"NODE": "b"}, {"NODE": "dgv"}]),                    # dgVoodoo raised over b's files
                   "c2": N([{"NODE": "dgv"}, {"NODE": "b"}]),                   # ...and not, the other way round
                   "g": N([{"NODE": "c"}, {"EDIT": [{"MODE": "ConfigWrite", "KEY": "Res=", "VALUE": "4k"}], "TARGET": "FILES/C:/g/dgVoodoo.conf"}]),
                   "h": N([{"NODE": "g"}, {"NODE": "dgv"}]),                   # tuning survives: the config edit is carried
                   "w": N([{"NODE": "c"}])},                                    # wrapped, c keeps its raise (an item's own copies)
         [("c", {}, []), ("c2", {}, []), ("h", {}, []), ("w", {}, [])]),
        # a re-provided copy leaves below what later carried values of its item write or hold: a key deleted over its
        # value (REG), a tile taken onto its entry (EXEC, through a TAKE rename)
        ("reprovide-cover", {"c": N([{"REG": {"HKCU\\S": None}, "ARCH": ["32"]}]),
                             "v": N([{"REG": {"HKCU\\S": {"a": "1"}}, "ARCH": ["32"]}, {"NODE": "c"}]),
                             "r": N([{"NODE": "v"}, {"NODE": "v"}]),
                             "t": N([{"EXEC": {"Card": {"HOST": "win32", "TILE": {"UID": "1", "TITLE": "Full"}}}}]),
                             "w": N([{"NODE": "t", "TAKE": [["EXEC/Card/TILE", "EXEC/Play/TILE"]]}]),
                             "e": N([{"EXEC": {"Play": {"HOST": "win32", "EXE": "g.exe", "TILE": {"UID": "1"}}}}, {"NODE": "w"}]),
                             "re": N([{"NODE": "e"}, {"NODE": "e"}])},
         [("v", {}, []), ("r", {}, []), ("e", {}, []), ("re", {}, [])]),
        # an except is checked in the slot's own coordinates, coverage in mapped ones: DLL by name (any case), a fact
        # renamed by a TAKE, a KEEP under a TAKE that does not select KEEP
        ("reprovide-coords", {"c": N([{"VARS": {"a": {"DEFAULT": "2"}}}, {"DLL": {"D3D9": "b"}}, {"KEEP": {"FILES/C:/g/save/": False}}, {"ENV": {"X": "2"}}]),
                              "L": N([{"VARS": {"a": {"DEFAULT": "1"}}}, {"DLL": {"D3D9": "n"}}, {"KEEP": {"FILES/C:/g/save/": True}}, {"ENV": {"X": "1"}}, {"NODE": "c"}]),
                              "h": N([{"NODE": "L"}, {"NODE": "L"}]),
                              "r": N([{"NODE": "h", "TAKE": [["VARS/a", "VARS/z"], ["DLL/D3D9", "DLL/d3d8"]]}]),
                              "k": N([{"NODE": "h", "TAKE": ["FILES", "ENV"]}])},
         [("L", {}, []), ("h", {}, []), ("r", {}, []), ("k", {}, [])]),
        # more of the same, one rule each: a moved node's own later layers never except it; a key holds its subkeys;
        # REG seen through a view is only what the view takes; an empty created key is an address; a value take-back is
        # not covered by a key-level KEEP; a whole key delete stays below whole; an entry keeps only the part taken onto
        # it, and only what was below (a part nothing wrote stays absent, no parent made for it); a REG KEEP value taken
        # with its key
        ("reprovide-more", {"d1": N([{"REG": {"HKCU\\K": None}}, {"REG": {"HKCU\\K": {"a": "2"}}}]),
                            "r1": N([{"NODE": "d1"}, {"REG": {"HKCU\\K": {"b": "3"}}}, {"NODE": "d1"}]),
                            "cdel": N([{"REG": {"HKCU\\S": None}}]),
                            "vs": N([{"REG": {"HKCU\\S\\Sub": {"a": "1"}}}, {"NODE": "cdel"}]),
                            "rs": N([{"NODE": "vs"}, {"NODE": "vs"}]),
                            "dt": N([{"REG": {"HKCU\\K": {"a": "1"}, "HKCU\\J": {"b": "1"}}}]),
                            "rt": N([{"NODE": "dt", "TAKE": ["REG/HKCU\\J"]}, {"REG": {"HKCU\\K": {"a": "2"}}},
                                     {"NODE": "dt", "TAKE": ["REG/HKCU\\J"]}]),
                            "edel": N([{"REG": {"HKCU\\E": None}}]),
                            "de": N([{"REG": {"HKCU\\E": {}}}, {"NODE": "edel"}]),
                            "re": N([{"NODE": "de"}, {"NODE": "de"}]),
                            "dk": N([{"KEEP": {"REG/HKCU\\K#v": False}}]),
                            "rk": N([{"NODE": "dk"}, {"KEEP": {"REG/HKCU\\K": True}}, {"NODE": "dk"}]),
                            "ca": N([{"REG": {"HKCU\\K": {"a": "1"}}}]),
                            "dd": N([{"REG": {"HKCU\\K": None}}, {"NODE": "ca"}]),
                            "rd": N([{"NODE": "dd"}, {"REG": {"HKCU\\K": {"b": "2"}}}, {"NODE": "dd"}]),
                            "t": N([{"EXEC": {"Card": {"HOST": "win32", "TILE": {"UID": "1", "TITLE": "Full", "COVER": {"FILE": "a.png"}}}}}]),
                            "w": N([{"NODE": "t", "TAKE": [["EXEC/Card/TILE", "EXEC/Play/TILE"]]}]),
                            "e": N([{"EXEC": {"Play": {"HOST": "win32", "EXE": "g.exe", "TILE": {"UID": "1"}}}}, {"NODE": "w"}]),
                            "ra": N([{"NODE": "e"}, {"EXEC": {"Play": {"HOST": "win32", "EXE": "g.exe", "ARGS": ["-x"], "TILE": {"UID": "1"}}}},
                                     {"NODE": "e"}]),
                            "rn": N([{"NODE": "e"}, {"EXEC": {"Play": None}}, {"NODE": "e"}]),
                            "wc": N([{"NODE": "t", "TAKE": [["EXEC/Card/TILE/COVER", "EXEC/Play/TILE/COVER"]]}]),
                            "ec": N([{"EXEC": {"Play": {"HOST": "win32", "EXE": "g.exe"}}}, {"NODE": "wc"}]),
                            "rc": N([{"NODE": "ec"}, {"EXEC": {"Play": {"HOST": "win32", "EXE": "h.exe"}}}, {"NODE": "ec"}]),
                            "kv": N([{"KEEP": {"REG/HKCU\\X#v": False, "REG/HKCU\\X": True}}]),
                            "kt": N([{"NODE": "kv", "TAKE": ["REG/HKCU\\X"]}])},
         [(r, {}, []) for r in ("r1", "rs", "rt", "re", "rk", "rd", "ra", "rn", "rc", "kt")]),
        # a membership chain longer than the fixpoint's bound: refused, never cut short
        ("unsettled", dict({CHAIN[i]: N([Z(f"c{i}")]) for i in range(70)},
                           end=N([{"NODE": CHAIN[0]}] + [{"NODE": CHAIN[i], "WHEN": CHAIN[i - 1] + " == true"} for i in range(1, 70)])),
         [("end", {}, [])]),
        ("pin-back", {"lib1": N([Z("lib1", "FILES/C:/g/lib")]), "lib2": N([Z("lib2", "FILES/C:/g/lib")]),
                      "n1": N([Z("n1"), {"NODE": "lib1"}]), "n2": N([{"NODE": "n1"}, {"NODE": "lib2"}]),
                      "n3": N([{"NODE": "n2"}, {"NODE": "lib1"}]), "n3b": N([{"NODE": "lib1"}, {"NODE": "n2"}])},
         [("n3", {}, []), ("n3b", {}, [])]),
        ("no-resurrection", {"x": N([dict(Z("x"), SOURCE="QmX")]), "y": N([dict(Z("y"), SOURCE="QmY")]),
                             "z": N([{"NODE": "x"}, Z("z", "FILES/C:/other")]), "e": N([{"NODE": "x"}, {"NODE": "y"}, {"NODE": "z"}]),
                             "same": N([dict(Z("d1"), SOURCE="QmSame"), {"NODE": "twin"}]), "twin": N([dict(Z("d2"), SOURCE="QmSame")])},
         [("e", {}, []), ("same", {}, [])]),
        ("wc3-orders", {"w121": N([dict(Z("1.21b"), SOURCE="Qm121")]),
                        "w127": N([{"NODE": "w121"}, {"DELTA": "1.27a.vgdelta", "SOURCE": "Qm127", "TARGET": "FILES/C:/g", "BASE_SOURCES": ["Qm121"]}]),
                        "tft": N([{"NODE": "w121"}, {"EXEC": {"TFT": {"HOST": "win32", "EXE": "C:/g/Frozen Throne.exe"}}}]),
                        "e1": N([{"NODE": "tft"}, {"NODE": "w127"}]), "e2": N([{"NODE": "w127"}, {"NODE": "tft"}])},
         [("e1", {}, []), ("e2", {}, [])]),
        ("cid-when", {"base": N([Z("b")]), DLC: N([Z("dlc")]), F1: N([Z("f1")]), F2: N([Z("f2")]),
                      "end": N([{"NODE": "base"}, {"NODE": DLC, "WHEN": "%dlc_on%==1"},
                                {"DLL": {"dlc_seen": "n"}, "WHEN": DLC + " == true"},
                                {"VARS": {"dlc_on": {"DEFAULT": "1"}}}]),
                      "flip": N([{"NODE": F1, "WHEN": F2 + " == false"}, {"NODE": F2, "WHEN": F1 + " == true"}])},
         [("end", {}, []), ("end", {"dlc_on": "0"}, []), ("flip", {}, [])]),
        # a delta over two bases, seen whole and placed by a containing node
        ("delta-bases", {"r": N([dict(Z("r", "FILES/g"), SOURCE="Qmr"), dict(Z("m", "FILES/g/music"), SOURCE="Qmm")]),
                         "f": N([{"NODE": "r"}, {"DELTA": "f.vgdelta", "TARGET": "FILES/g",
                                                 "BASE_SOURCES": ["Qmr", "Qmm"]}]),
                         "s": N([{"NODE": "f", "TARGET": "FILES/sub"}])},
         [("f", {}, []), ("s", {}, [])]),
        # a coded stream: its content named by CID, its description inline (data, the layer's DESCRIPTION); placed by
        # a containing node like any content
        ("coded", {"c": N([{"FILE": "a.bsa.content", "SOURCE": "Qmc", "TARGET": "FILES/g/Data"},
                           {"CODED": "a.bsa.vgcr", "SOURCE": "Qmr", "NAME": "a.bsa",
                            "DESCRIPTION": {"NAME": "BSA 105", "ISA": 6, "CODER": "BITS", "ENTRY": "@s", "CODE": ["@s", "end"]},
                            "BASE_SOURCES": ["Qmc"], "TARGET": "FILES/g/Data"}]),
                   "s": N([{"NODE": "c", "TARGET": "FILES/sub"}])},
         [("c", {}, []), ("s", {}, [])]),
        ("placement", {"lib": N([Z("l1", "FILES/sub"), Z("l2", "FILES/C:/abs"), {"NODE": "leaf", "TARGET": "FILES/deep"},
                                 {"EDIT": [{"MODE": "Overwrite", "VALUE": "v"}], "TARGET": "FILES/sub/f.ini"},
                                 {"KEEP": {"FILES/sub/save/": True, "REG/HKCU\\S": False, "REG/HKCU\\V#a": False, "REG/HKCU\\V#b": True, "REG/HKCU\\W": {"ARCH": ["32"]}}},
                                 {"EXEC": {"L": {"HOST": "h", "EXE": "bin/x.exe", "WORKDIR": "bin"}}}]),
                       "leaf": N([Z("leafz", "FILES/in")]),
                       "g": N([{"NODE": "lib", "TARGET": "FILES/C:/g/lib"}])}, [("g", {}, [])]),
        ("take", {"lib": N([{"DLL": {"d3d8": "n,b", "ddraw": "n,b"}}, {"VARS": {"a": {"DEFAULT": "1"}, "b": {"DEFAULT": "2"}}},
                            {"ENV": {"E1": "1", "E2": None}}, Z("lz", "FILES/MS"),
                            {"EDIT": [{"MODE": "Poke", "OFFSET": "0x1", "VALUE": "00"}], "TARGET": "FILES/MS/x86/D3D8.dll"},
                            {"REG": {"HKCU\\A": {"v": "1"}, "HKCU\\B": {"w": "2"}, "HKCU\\E": {}, "HKCU\\A\\Sub": {"s": "3"}}},
                            {"EXEC": {"Play": {"HOST": "win32", "EXE": "x.exe"}, "Tool": {"HOST": "win32"}}}]),
                  "mid": N([{"NODE": "lib", "TAKE": ["DLL", "VARS/a", "FILES/MS/x86/D3D8.dll", "REG/HKCU\\A", "REG/HKCU\\E", "ENV/E1"]}]),
                  "g": N([{"NODE": "lib", "TAKE": ["DLL/d3d8", ["VARS/a", "VARS/z"], "EXEC/Tool", ["ENV/E2", "ENV/E9"],
                                                  "FILES/MS/x86/", "REG/HKCU\\B"]},
                          {"NODE": "mid", "TAKE": ["DLL/ddraw", "FILES/MS/"], "TARGET": "FILES/C:/g"}])},
         [("g", {}, [])]),
        ("take-placed", {"lib": N([{"DIR": "lz", "TARGET": "FILES/bin"},
                                   {"EDIT": [{"MODE": "Poke", "OFFSET": "0x1", "VALUE": "00"}], "TARGET": "FILES/bin/a.dll"},
                                   {"EXEC": {"Tool": {"HOST": "h", "EXE": "bin/a.exe", "WORKDIR": "bin"}}},
                                   {"KEEP": {"FILES/bin/save/": True, "REG/HKCU\\S": True}}]),
                         "mid": N([{"NODE": "lib", "TAKE": [["FILES/bin/a.dll", "FILES/b.dll"], "FILES/bin/save", "FILES/bin/a.exe",
                                                            "EXEC", "REG"], "TARGET": "FILES/lib"}]),
                         "g": N([{"NODE": "mid", "TAKE": ["FILES/lib/", "EXEC", "REG"], "TARGET": "FILES/C:/g"}])},
         [("g", {}, []), ("mid", {}, [])]),
        ("reg+dll+env", {"r": N([{"REG": {"HKCU\\S": {"a": "1", "b": "2"}}, "ARCH": ["32", "64"]},
                                 {"REG": {"HKCU\\S": {"a": "0", "b": "1", "c": "k"}}},
                                 {"REG": {"HKCU\\S": {"a": "3", "b": None}, "HKCU\\E": {}, "HKLM\\X": {"v": 1}}},
                                 {"REG": {"HKCU\\T": {"x": "1"}, "HKCU\\T\\Sub": {"y": "2"}, "HKCU\\T\\K": {}, "HKCU\\TT": {"z": "3"},
                                          "HKCU\\Content Type\\application/xml": {"Extension": ".xml"}}},
                                 {"REG": {"HKCU\\T": None}},
                                 {"DLL": {"D3D8": "n,b", "ddraw": "b"}}, {"DLL": {"d3d8": None}},
                                 {"ENV": {"A": "1", "B": "2"}}, {"ENV": {"A": None}}])}, [("r", {}, [])]),
        # parts (23.0): INTO is a hard link to the end launched or an applied part; WHEN names node CIDs; no order
        ("into+parts", {"base": N([Z("b")]),
                        "v1": N([{"NODE": "base"}, {"EXEC": {"Play": {"HOST": "h", "TILE": {"UID": "9", "TITLE": "t"}, "RECOMMENDED": True}}}]),
                        "vup": N([{"NODE": "v1"}, Z("vup")]),                          # a later end containing v1
                        "m": N([{"INTO": ["base"]}, Z("m")]), "v2": N([{"NODE": "base"}, {"NODE": "m"}]), "v4": N([{"NODE": "m"}]),
                        P1: N([{"INTO": ["v1", "zzz"], "RECOMMENDED": True}, Z("g1"), {"NODE": "base"}], LABEL="b-part"),
                        "g2": N([{"INTO": ["v1"]}, Z("g2")], LABEL="a-part"),
                        "g3": N([{"INTO": [P1], "RECOMMENDED": True}, Z("g3")], LABEL="a-on-b"),
                        "g4": N([{"INTO": ["v1"], "WHEN": P1 + " == false", "RECOMMENDED": True}, Z("g4")], LABEL="c-not-with-b"),
                        "card": N([{"INTO": [P1], "RECOMMENDED": True}, Z("card"), {"EXEC": {"Card v1": {"HOST": "h", "TILE": {"UID": "90", "TITLE": "c", "PARENTUID": "9"}}}}], LABEL="graft-card"),
                        "low": N([{"INTO": ["base"]}, Z("low")], LABEL="into-a-lower-node"),
                        "g5": N([{"INTO": ["v1"]}, {"NODE": "base", "WHEN": P1 + " == true"}, Z("g5", "FILES/C:/h")], LABEL="d-detects-b"),
                        "pp": N([{"INTO": [P1]}, {"INTO": ["g2"]}, Z("pp")], LABEL="e-patch-between-b-and-a"),
                        # a part whose only entry is a runner's (GUEST) is an add-on, not a row: still ticked
                        "rp": N([{"INTO": ["v1"], "RECOMMENDED": True}, Z("rp"), {"EXEC": {"Engine": {"HOST": "linux", "GUEST": ["custom"]}}}],
                                LABEL="f-runner-part"),
                        # a part whose untiled entry takes its base's tile (a NODE layer's TAKE): a row under that card
                        "tk": N([{"INTO": ["v1"], "RECOMMENDED": True}, {"EXEC": {"Port": {"HOST": "h", "EXE": "port"}}},
                                 {"NODE": "v1", "TAKE": [["EXEC/Play/TILE", "EXEC/Port/TILE"]]}], LABEL="h-port-row")},
         [("v1", {}, []), ("v1", {}, ["g2", P1]), ("v1", {}, ["g3", P1]), ("v1", {}, [P1, "g3"]),
          ("v1", {}, [P1, "g4"]), ("v1", {}, ["g4"]), ("v1", {}, ["g5", P1]), ("v1", {}, ["g5"]),
          ("vup", {}, []), ("vup", {}, [P1]), ("v2", {}, []), ("v4", {}, [])]),
        # a part's FOOTPRINT (its INTO's TAKE): files placed (renamed, left out), facts untouched; applied wherever the
        # part is expanded (inside a pack too); two INTO TAKEs in layer order (the first innermost)
        ("footprint", {"end": N([Z("base"), {"EXEC": {"Game v1.0": {"HOST": "win32", "EXE": "C:/g/g.exe"}}}], VARIANT="Windows"),
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
    assert labels(p) == ["a", "b", "c"], labels(p)
    # a later mention re-provides that node's own layers (later wins)
    nodes["y"] = N([{"NODE": "a"}, {"NODE": "b"}, Z("y"), {"NODE": "a"}])
    p = R(nodes).resolve("y")
    assert labels(p) == ["b", "y", "a"], labels(p)
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
                       {"EXEC": {"Play": {"HOST": "win32", "EXE": "x.exe"}, "Tool": {"HOST": "win32"}}}]),
             "g": N([{"NODE": "lib", "TAKE": ["DLL/d3d8", ["VARS/a", "VARS/z"], "EXEC/Tool"]}])}
    p = R(nodes).resolve("g")
    assert p["dll"] == {"d3d8": "n,b"} and list(p["decls"]) == ["z"] and list(p["exec"]) == ["Tool"], p
    # placement between takes: an outer TAKE sees the inner node's files where the middle node PLACED them (lib/…),
    # so left-stripping "FILES/lib/" brings them to the outer TARGET; facts pass through, never re-rooted
    fx = dict(next(f for n, f, _ in fixtures() if n == "take-placed"))
    p = R(fx).resolve("g")
    assert [it["target"] for it in p["seq"] if it["kind"] == "EDIT"] == ["C:/g/b.dll"], p["seq"]
    assert p["exec"]["Tool"]["EXE"] == "C:/g/a.exe" and p["exec"]["Tool"]["WORKDIR"] == "C:/g/lib/bin", p["exec"]
    assert list(p["keep"]) == ["FILES/C:/g/save/", "REG/HKCU\\S"], p["keep"]
    assert [it["target"] for it in p["seq"] if it["kind"] == "DIR"] == ["C:/g/lib/bin"]      # the plan: its placement
    p = R(fx).resolve("mid")
    assert [it["target"] for it in p["seq"] if it["kind"] == "EDIT"] == ["lib/b.dll"] and list(p["keep"])[0] == "FILES/lib/save/"
    assert take_match("FILES/a/b/c.dll", "FILES/a/b/c.dll") == "c.dll"
    assert take_match("FILES/a/b/x", "FILES/a/b") == "b/x" and take_match("FILES/a/b/x", "FILES/a/b/") == "x"
    # nothing folds: a later entry of one label replaces the earlier one whole
    nodes = {"t": N([{"EXEC": {"Play": {"TILE": {"UID": "1", "TITLE": "T"}}, "B": {"HOST": "h"}}}]),
             "v": N([{"NODE": "t"}, {"EXEC": {"Play": {"HOST": "win32", "TILE": {"TITLE": "T2"}}}}])}
    e = R(nodes).resolve("v")["exec"]
    assert e["Play"] == {"HOST": "win32", "TILE": {"TITLE": "T2"}} and e["B"] == {"HOST": "h"}, e
    # ...and a TAKE reaches into an entry: a tile renamed onto another entry; a cover alone
    fx = dict(next(f for n, f, _ in fixtures() if n == "exec-whole"))
    e = R(fx).resolve("iron")["exec"]
    assert e["Quake - Ironwail"]["TILE"]["TITLE"] == "A" and e["Quake - Ironwail"]["EXE"] == "ironwail" and "Play" not in e, e
    e = R(fx).resolve("cov")["exec"]
    assert e["Play"]["TILE"] == {"UID": "1", "COVER": {"FILE": "a.png", "SOURCE": "QmA", "SIZE": 5}}, e
    e = R(fx).resolve("args")["exec"]
    assert e["Play"] == {"HOST": "win32", "EXE": "C:/g/g.exe", "ARGS": ["-w"]}, e
    fr = dict(next(f for n, f, _ in fixtures() if n == "raise"))
    w = R(fr).resolve("w")
    assert [i.get("payload") for i in w["seq"] if i["kind"] == "ZIP"][-1] == "dgv", w["seq"]     # the raise survives wrapping
    fc = dict(next(f for n, f, _ in fixtures() if n == "reprovide-cover"))
    assert not R(fc).resolve("r")["reg"], R(fc).resolve("r")["reg"]                          # the deleted key stays deleted
    assert R(fc).resolve("re")["exec"]["Play"]["TILE"]["TITLE"] == "Full"                     # the taken tile stays
    fk = dict(next(f for n, f, _ in fixtures() if n == "reprovide-coords"))
    h = R(fk).resolve("h")
    assert h["dll"] == {"d3d9": "b"} and h["decls"]["a"]["DEFAULT"] == "2" and h["env"]["X"] == "2", (h["dll"], h["decls"])
    assert R(fk).resolve("r")["decls"]["z"]["DEFAULT"] == "2"                                 # renamed: c's value still wins
    assert list(R(fk).resolve("k")["keep"].values()) == [False]                              # the take-back stays
    fm = dict(next(f for n, f, _ in fixtures() if n == "reprovide-more"))
    rv = lambda root: {k[1] + "#" + k[2]: v[2] for k, v in R(fm).resolve(root)["reg"].items()}
    assert rv("r1") == {"hkcu\\k#a": "2"} and rv("rs") == {} and rv("rd") == {"hkcu\\k#a": "1", "hkcu\\k#b": "2"}, (rv("r1"), rv("rd"))
    p = R(fm).resolve("rt")
    assert rv("rt") == {"hkcu\\k#a": "2", "hkcu\\j#b": "1"} and not p["events"], p["events"]   # K was never taken
    assert not R(fm).resolve("re")["regkeys"]                                                # the created key stays deleted
    p = R(fm).resolve("rk")
    assert dict(p["keep"]) == {"REG/HKCU\\K": True, "REG/HKCU\\K#v": False} and not p["events"], p
    assert R(fm).resolve("ra")["exec"]["Play"] == {"HOST": "win32", "EXE": "g.exe", "TILE": {"UID": "1"}}   # ARGS gone, tile kept
    assert R(fm).resolve("rn")["exec"]["Play"] == {"HOST": "win32", "EXE": "g.exe"}         # nothing below: no tile
    assert R(fm).resolve("rc")["exec"]["Play"] == {"HOST": "win32", "EXE": "g.exe"}         # ...and no empty TILE made
    assert dict(R(fm).resolve("kt")["keep"]) == {"REG/HKCU\\X#v": False, "REG/HKCU\\X": True}
    fu = dict(next(f for n, f, _ in fixtures() if n == "unsettled"))
    ru = R(fu); ru.resolve("end")
    assert ru.error == "the conditions naming nodes did not settle in 64 rounds", ru.error
    # a graft card's row launches on the end beneath its INTO chain, the chain applied (FE → UP → 1.0e over 1.0c)
    gx = {"c": {"VARIANT": "1.0c", "LAYERS": [{"ZIP": "c", "TARGET": "FILES/C:/g"}, {"EXEC": {"Play": {"HOST": "win32", "TILE": {"UID": "1"}}}}]},
          "e": {"LAYERS": [{"INTO": ["c"]}, {"ZIP": "e", "TARGET": "FILES/C:/g"}]},
          "up": {"LAYERS": [{"INTO": ["e"], "RECOMMENDED": True}, {"ZIP": "up", "TARGET": "FILES/C:/g"}]},
          "fe": {"VARIANT": "FE", "LAYERS": [{"INTO": ["up"]}, {"ZIP": "fe", "TARGET": "FILES/C:/g"},
                                             {"EXEC": {"FE": {"HOST": "win32", "TILE": {"UID": "2", "TITLE": "FE"}, "RECOMMENDED": True}}}]}}
    gn = {k: (v, "/b") for k, v in gx.items()}
    assert graft_bases(gn, "fe") == ["c"] and into_chain(gn, "fe", "c") == ["e", "up"], (graft_bases(gn, "fe"), into_chain(gn, "fe", "c"))
    root, applied, plan = launch_row(gn, "fe")
    assert root == "c" and applied == ["e", "up", "fe"] and [i["payload"] for i in plan["seq"]] == ["c", "e", "up", "fe"], (root, applied)
    e = R(fx).resolve("ren")["exec"]
    assert list(e) == ["A v2"], e                                  # the inherited Play deleted, the new label alone
    # REG: a path tree; later wins per value, null deletes, an empty key is created; a "/" in a key name is a character
    nodes = {"r": N([{"REG": {"HKCU\\S": {"a": "1", "b": "2"}}}, {"REG": {"HKCU\\S": {"a": "3", "b": None}, "HKCU\\E": {}}}])}
    p = R(nodes).resolve("r")
    assert {k[2]: v[2] for k, v in p["reg"].items()} == {"a": "3"} and (None, "hkcu\\e") in p["regkeys"], p["reg"]
    # a null key deletes its whole subtree (values, subkeys, created keys) — and nothing that merely shares a prefix
    nodes = {"r": N([{"REG": {"HKCU\\T": {"x": "1"}, "HKCU\\T\\Sub": {"y": "2"}, "HKCU\\T\\K": {}, "HKCU\\TT": {"z": "3"}}},
                     {"REG": {"HKCU\\T": None}}])}
    p = R(nodes).resolve("r")
    assert [k[1] for k in p["reg"]] == ["hkcu\\tt"] and not p["regkeys"], (p["reg"], p["regkeys"])
    assert take_match("REG/HKCU\\A\\Sub", "REG/HKCU\\A") is not None and take_match("REG/HKCU\\AB", "REG/HKCU\\A") is None
    # composition by history: a re-mention raises a node's own layers; a carried copy never comes back; the tuning a
    # node's carried child made survives a re-mention of that node (its paths stay below the re-provided copy)
    fx = dict(next(f for n, f, _ in fixtures() if n == "raise"))
    assert ("raise", "dgv") in R(fx).resolve("c")["events"] and not any(e[0] == "raise" for e in R(fx).resolve("c2")["events"])
    p = R(fx).resolve("c")                                  # raised over b, except what its own config child edited
    assert labels(p) == ["game", "dgv", "b-over", "dgv"] and p["seq"][-1].get("except") == ["C:/g/dgVoodoo.conf"], p["seq"]
    assert labels(R(fx).resolve("c2")) == ["dgv", "game", "b-over"], labels(R(fx).resolve("c2"))
    p = R(fx).resolve("h")
    dg = [it for it in p["seq"] if it.get("payload") == "dgv"]
    assert len(dg) == 2 and dg[-1].get("except") == ["C:/g/dgVoodoo.conf"] and "except" not in dg[0], dg
    assert [it["kind"] for it in p["seq"]].count("EDIT") == 2                       # each edit once
    fx = dict(next(f for n, f, _ in fixtures() if n == "pin-back"))
    assert labels(R(fx).resolve("n3"))[-1] == "lib1" and labels(R(fx).resolve("n3b"))[-1] == "lib2"
    fx = dict(next(f for n, f, _ in fixtures() if n == "no-resurrection"))
    assert labels(R(fx).resolve("e")) == ["x", "y", "z"], labels(R(fx).resolve("e"))
    assert len(labels(R(fx).resolve("same"))) == 1, labels(R(fx).resolve("same"))       # one CID at one place: one value
    fx = dict(next(f for n, f, _ in fixtures() if n == "wc3-orders"))
    for root in ("e1", "e2"):
        p = R(fx).resolve(root)
        assert [it["kind"] for it in p["seq"]] == ["ZIP", "DELTA"] and list(p["exec"]) == ["TFT"], (root, p["seq"])
    # a WHEN naming a node reads the FINAL composition; a contradiction is refused
    fx = dict(next(f for n, f, _ in fixtures() if n == "cid-when"))
    assert R(fx).resolve("end")["dll"] == {"dlc_seen": "n"} and R(fx).resolve("end", {"dlc_on": "0"})["dll"] == {}
    assert "contradict" in R(fx).resolve("flip")["error"], R(fx).resolve("flip")["error"]
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
            assert off("v1") == ["g2", P1, "g4", "g5", "rp", "tk"], off("v1")
            tk = offered_grafts(r.nodes, gi, r.resolve("v1"), "9")[1]
            assert "rp" in tk and "tk" not in tk, tk                   # a runner part is ticked; a row never is
            assert off("vup") == [] and "low" not in off("v1")
            assert "g3" not in off("v1") and "g3" in off("v1", [P1])
            # several INTO layers: into ALL of them (a patch between two mods), offered once both are applied
            assert "pp" not in off("v1", [P1]) and "pp" not in off("v1", ["g2"]) and "pp" in off("v1", [P1, "g2"])
            assert applied_grafts(r, gi, "v1", "9", ["pp", "g2", P1]) == [P1, "g2", "pp"], applied_grafts(r, gi, "v1", "9", ["pp", "g2", P1])
            assert applied_grafts(r, gi, "v1", "9", ["pp", P1]) == [P1], applied_grafts(r, gi, "v1", "9", ["pp", P1])
            # the selection is a set: a part follows what it goes into, whatever the order asked
            assert applied_grafts(r, gi, "v1", "9", ["g3", P1]) == [P1, "g3"], applied_grafts(r, gi, "v1", "9", ["g3", P1])
            assert applied_grafts(r, gi, "v1", "9", [P1, "g3", P1]) == [P1, "g3"]
            assert applied_grafts(r, gi, "v1", "9") == [P1, "rp", "g3"], applied_grafts(r, gi, "v1", "9")   # fresh: g4 excluded by P1
            # a part whose INTO's WHEN fails on the selection is dropped; alone it applies
            d = []
            assert applied_grafts(r, gi, "v1", "9", [P1, "g4"], dropped=d) == [P1] and d == ["g4"], d
            assert applied_grafts(r, gi, "v1", "9", ["g4"]) == ["g4"]
            # a layer gated on another part's presence (FOMOD auto-detect as structure)
            labs = lambda p: [it.get("payload") for it in p["seq"]]
            assert "g5" in labs(r.resolve("v1", {}, {}, applied_grafts(r, gi, "v1", "9", ["g5"])))
            assert labs(r.resolve("v1", {}, {}, ["g5"])).count("b") == 1
            assert labs(r.resolve("v1", {}, {}, [P1, "g5"], [P1, "g5"])).count("b") == 1   # base: one value
            # a part never offered here is dropped and said so
            d = []
            assert applied_grafts(r, gi, "vup", "9", [P1], dropped=d) == [] and d == [P1]
            assert applied_grafts(r, gi, "v1", None) == [P1, "rp", "g3"]  # the INTO layers' RECOMMENDED decide, not the face
            assert "card" in off("v1", [P1]) and "card" not in applied_grafts(r, gi, "v1", "9")   # a graft with an entry: never ticked
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
