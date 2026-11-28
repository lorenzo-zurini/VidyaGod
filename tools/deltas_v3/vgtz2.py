#!/usr/bin/env python3
"""Deltas v3, READABLE prototype (USER 2026-11-28: "the goal of this is making the format as human readable as
possible, while keeping all the other achievements" — random access with no rebuild, small, deterministic (CIDs),
decoded media as bases, per-file granularity).

A .vgtz is a STORE zip any tool opens:
    TREE.txt                  the table: one line a path, in words
    patch/<path>.patch        a changed TEXT file: a unified diff against its base file (patch(1) / git apply read it)
    add/<path>.add            new bytes of a text file, raw (readable)
    add/<path>.add.zst        new bytes of a binary file, zstd

TREE.txt ("VGTZ 1", then lines in path order, fields by tab; ranges a..b are [a, b) of the base file):
    base    <i>     <cid>
    dir     <path>
    link    <path>  <target>
    same    <path>  <size> <mode>  from <i> <base path>            the base file, whole
    range   <path>  <size> <mode>  from <i> <base path>  <a>..<b>  a part of it
    patch   <path>  <size> <mode>  from <i> <base path>            text: patch/<path>.patch over the base file
    pieces  <path>  <size> <mode>  from <i> <base path>  <pieces>  binary: "keep a..b", "new n" (the next n bytes of
                                                                   its add), "zero n", joined by ", "
    new     <path>  <size> <mode>                                  its add, whole (a 0-byte file: no add)
Random access: a patch is turned into byte pieces when its file is opened (text files are small); everything else is
byte pieces already.

Commands:
    convert <in.vgtree> <out.vgtz> --bases bases.json   from a v2 tree (reads only the base files of changed TEXT files)
    ls | cat | diff | verify                            as vgtz.py
"""
import json, os, re, sys, tempfile, zipfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import zstandard
from vgtz import read_vgtree, parse, BaseFiles, is_text, EPOCH

# ---------------------------------------------------------------- exact unified diffs (bytes in, bytes out)
def lines_of(b):
    return b.splitlines(keepends=True)


def make_patch(old, new, a_name, b_name, ctx=3):
    """A unified diff that rebuilds `new` from `old` byte for byte: lines keep their own endings; a last line with no
    newline is marked as diff marks it."""
    import difflib
    A, B = lines_of(old), lines_of(new)
    out = [f"--- a/{a_name}\n".encode(), f"+++ b/{b_name}\n".encode()]
    sm = difflib.SequenceMatcher(None, A, B, autojunk=False)
    for grp in sm.get_grouped_opcodes(ctx):
        i1, i2, j1, j2 = grp[0][1], grp[-1][2], grp[0][3], grp[-1][4]
        out.append(f"@@ -{i1 + 1},{i2 - i1} +{j1 + 1},{j2 - j1} @@\n".encode())
        def emit(tag, line):
            out.append(tag + line)
            if not line.endswith(b"\n"): out.append(b"\n\\ No newline at end of file\n")
        for op, a1, a2, b1, b2 in grp:
            if op == "equal":
                for l in A[a1:a2]: emit(b" ", l)
            else:
                for l in A[a1:a2]: emit(b"-", l)
                for l in B[b1:b2]: emit(b"+", l)
    return b"".join(out)


def apply_patch(old, patch):
    """`patch` over `old` → the new bytes, as pieces (('keep', a, b) of old, ('new', bytes)), or None if it does not fit."""
    A = lines_of(old)
    starts = [0]
    for l in A: starts.append(starts[-1] + len(l))
    P = patch.split(b"\n")
    pieces, pos, k = [], 0, 2
    def keep(i1, i2):
        if i2 > i1: pieces.append(("keep", starts[i1], starts[i2]))
    while k < len(P):
        m = re.match(rb"@@ -(\d+),(\d+) \+(\d+),(\d+) @@", P[k])
        if not m:
            if P[k] == b"": k += 1; continue
            return None
        a1 = int(m.group(1)) - 1 if int(m.group(2)) else int(m.group(1))
        keep(pos, a1); pos = a1; k += 1
        while k < len(P) and not P[k].startswith(b"@@"):
            t, body = P[k][:1], P[k][1:]
            nl = not (k + 1 < len(P) and P[k + 1] == b"\\ No newline at end of file")
            line = body + (b"\n" if nl else b"")
            if t == b" ":
                if A[pos] != line and A[pos].rstrip(b"\r\n") != body: return None
                keep(pos, pos + 1); pos += 1
            elif t == b"-": pos += 1
            elif t == b"+": pieces.append(("new", line))
            elif t == b"\\" or (t == b"" and k == len(P) - 1): pass
            else: return None
            k += 2 if not nl else 1
    keep(pos, len(A))
    return pieces


def text_encoding(b):
    """How a file is text: "" (bytes as they are: ASCII, UTF-8, a code page) or a UTF-16 with its byte-order mark."""
    for bom, enc in ((b"\xff\xfe", "utf-16-le"), (b"\xfe\xff", "utf-16-be")):
        if b.startswith(bom):
            try: b.decode(enc); return enc
            except UnicodeDecodeError: return None
    return "" if is_text(b) else None


def rebuild_text(old, patch):
    """A patch over its base file → the new bytes (a UTF-16 file: the patch is over its text as UTF-8, its first line
    says so, the result is encoded back)."""
    enc = ""
    if patch.startswith(b"# text "):
        head, patch = patch.split(b"\n", 1); enc = head[7:].decode()
        old = old.decode(enc).encode("utf-8")
    pcs = apply_patch(old, patch)
    if pcs is None: return None
    out = b"".join(old[x[1]:x[2]] if x[0] == "keep" else x[1] for x in pcs)
    return out.decode("utf-8").encode(enc) if enc else out


def text_patch(old, new, a_name, b_name):
    eo, en = text_encoding(old), text_encoding(new)
    if eo is None or en is None or eo != en: return None
    if eo:
        pt = b"# text " + eo.encode() + b"\n" + make_patch(old.decode(eo).encode("utf-8"), new.decode(en).encode("utf-8"), a_name, b_name)
    else:
        pt = make_patch(old, new, a_name, b_name)
    return pt if rebuild_text(old, pt) == new else None


# ---------------------------------------------------------------- v2 → readable v3
def convert(src, dst, bases_json, work):
    table, region = read_vgtree(src, work)
    bases, ents = parse(table)
    bf = [BaseFiles(b) for b in json.load(open(bases_json))]
    rows, files, stats = [f"VGTZ 1"] + [f"base\t{i}\t{c}" for i, c in enumerate(bases)], {}, {"same": 0, "range": 0, "patch": 0, "pieces": 0, "new": 0}
    zc = zstandard.ZstdCompressor(level=19)
    for e in ents:
        p = e["path"]
        if e["kind"] == "d": rows.append(f"dir\t{p}"); continue
        if e["kind"] == "l": rows.append(f"link\t{p}\t{e['link']}"); continue
        head = f"\t{p}\t{e['size']}\t{e['mode']}"
        if e["op"] == "B":
            rows.append(f"same{head}\tfrom {e['base']}\t{e['bpath']}"); stats["same"] += 1; continue
        if e["op"] == "R":
            rows.append(f"range{head}\tfrom {e['base']}\t{e['bpath']}\t{e['off']}..{e['off'] + e['len']}"); stats["range"] += 1; continue
        if e["op"] == "A":
            data = region.read(e["off"], e["len"])
            rows.append(f"new{head}"); stats["new"] += 1
            if data: files["add/" + p + (".add" if text_encoding(data) is not None else ".add.zst")] = data if text_encoding(data) is not None else zc.compress(data)
            continue
        # a changed file: its base, and its bytes
        old = bf[e["base"]].read(e["bpath"])
        new = b"".join(region.read(a, n) if k == "a" else old[a:a + n] for k, a, n in e["pieces"])
        pt = text_patch(old, new, e["bpath"], p)
        if pt is not None:
            rows.append(f"patch{head}\tfrom {e['base']}\t{e['bpath']}"); files["patch/" + p + ".patch"] = pt; stats["patch"] += 1; continue
        # binary: keep / new / zero, merged
        out, add = [], bytearray()
        def push(kind, a, b):
            if out and out[-1][0] == kind and (kind != "keep" or out[-1][2] == a):
                out[-1] = (kind, out[-1][1], b if kind == "keep" else out[-1][2] + (b - a)); return
            out.append((kind, a, b) if kind == "keep" else (kind, 0, b - a))
        at = 0
        for k, a, n in e["pieces"]:
            chunk = region.read(a, n) if k == "a" else old[a:a + n]
            if chunk.count(0) == n: push("zero", at, at + n)
            elif k == "c": push("keep", a, a + n)
            else: push("new", at, at + n); add += chunk
            at += n
        words = ", ".join(f"keep {a}..{b}" if kd == "keep" else f"{kd} {b}" for kd, a, b in out)
        rows.append(f"pieces{head}\tfrom {e['base']}\t{e['bpath']}\t{words}"); stats["pieces"] += 1
        if add: files["add/" + p + ".add.zst"] = zc.compress(bytes(add))
    tmp = dst + ".part"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_STORED) as z:
        def put(name, data):
            zi = zipfile.ZipInfo(name, EPOCH); zi.compress_type = zipfile.ZIP_STORED; zi.external_attr = 0o644 << 16
            z.writestr(zi, data)
        put("TREE.txt", ("\n".join(rows) + "\n").encode("utf-8"))
        for n in sorted(files): put(n, files[n])
    os.replace(tmp, dst)
    stats.update(vgtree_bytes=os.path.getsize(src), vgtz_bytes=os.path.getsize(dst), table_bytes=len("\n".join(rows)))
    return stats


# ---------------------------------------------------------------- readable v3, read
class V3:
    def __init__(self, path, bases_json):
        self.z = zipfile.ZipFile(path)
        self.names = set(self.z.namelist())
        self.rows = {}
        for l in self.z.read("TREE.txt").decode("utf-8").split("\n")[1:]:
            if l: F = l.split("\t"); self.rows[F[1] if F[0] != "base" else "\x00" + F[1]] = F
        self.bf = [BaseFiles(b) for b in json.load(open(bases_json))] if bases_json else []

    def add(self, p):
        if "add/" + p + ".add" in self.names: return self.z.read("add/" + p + ".add")
        if "add/" + p + ".add.zst" in self.names: return zstandard.ZstdDecompressor().decompress(self.z.read("add/" + p + ".add.zst"), max_output_size=1 << 31)
        return b""

    def bytes_of(self, p):
        F = self.rows[p]; op = F[0]
        if op == "new": return self.add(p)
        old = self.bf[int(F[4].split()[1])].read(F[5])
        if op == "same": return old
        if op == "range": a, b = map(int, F[6].split("..")); return old[a:b]
        if op == "patch": return rebuild_text(old, self.z.read("patch/" + p + ".patch"))
        add, at, out = self.add(p), 0, bytearray()
        for w in F[6].split(", "):
            kd, arg = w.split(" ", 1)
            if kd == "keep": a, b = map(int, arg.split("..")); out += old[a:b]
            elif kd == "zero": out += bytes(int(arg))
            else: n = int(arg); out += add[at:at + n]; at += n
        return bytes(out)


def main():
    a = sys.argv[1:]
    opt = lambda k: a[a.index(k) + 1] if k in a else None
    work = opt("--work") or tempfile.mkdtemp(dir=os.path.expanduser("~/Storage/tmp"))
    if a[0] == "convert": print(json.dumps(convert(a[1], a[2], opt("--bases"), work), indent=1))
    elif a[0] == "ls": print(zipfile.ZipFile(a[1]).read("TREE.txt").decode("utf-8"), end="")
    elif a[0] == "cat": sys.stdout.buffer.write(V3(a[1], opt("--bases")).bytes_of(a[2]))
    elif a[0] == "verify":
        v = V3(a[1], opt("--bases"))
        table, region = read_vgtree(a[2], work)
        bases, ents = parse(table)
        n = 0
        for e in ents:
            if e["kind"] != "f": continue
            if e["op"] == "A": want = region.read(e["off"], e["len"])
            else:
                base = v.bf[e["base"]].read(e["bpath"])
                if e["op"] == "B": want = base
                elif e["op"] == "R": want = base[e["off"]:e["off"] + e["len"]]
                else: want = b"".join(region.read(x, m) if k == "a" else base[x:x + m] for k, x, m in e["pieces"])
            assert v.bytes_of(e["path"]) == want, e["path"]
            n += 1
        print(json.dumps({"files": n, "identical": True}))
    else: sys.exit(__doc__)


if __name__ == "__main__":
    main()
