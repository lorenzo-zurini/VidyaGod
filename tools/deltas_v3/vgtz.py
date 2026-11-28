#!/usr/bin/env python3
"""Deltas v3 PROTOTYPE (USER 2026-11-28: "the delta format is very opaque now, very blackboxy... a zip or container of
some sorts, containing only the files that changed, with only the bits that changed").

A .vgtz is a STORE zip anyone can open:
    TREE.txt              the table, plain text (the .vgtree table with its ADD references made per file)
    add/<path>.add        a changed or new file's own added bytes, nothing else

TREE.txt is the canonical .vgtree text ("VGTREE 1", b/d/l/f lines) with one change: an ADD reference names the file's
own .add entry, never a shared region:
    f <path> <size> <mode> A                        the file is its add/<path>.add, whole
    f <path> <size> <mode> D <i> <base path> <pieces>    pieces c<off>+<len> (its base file's bytes) or a<off>+<len>
                                                    (bytes of ITS OWN add/<path>.add)
    f <path> <size> <mode> B <i> <base path>  /  R <i> <base path> <off>+<len>     as in .vgtree (nothing added)
so an unchanged file is one line, a changed one a line saying exactly which bytes are kept and which are new, and its
new bytes sit in one zip entry named after it.

Commands:
    convert <in.vgtree> <out.vgtz>                    re-pack (no matching, no base read): the v2 → v3 migration
    ls <x.vgtz>                                       the table
    cat <x.vgtz> <path> --bases bases.json            a file's bytes (to stdout)
    diff <x.vgtz> <path> --bases bases.json           what changed: a unified diff for text, the piece map for binary
    verify <x.vgtz> <in.vgtree> --bases bases.json    every file of the v3 container == the v2 tree's, byte for byte
bases.json: as `VidyaGod tree make` takes it ({"cid", "kind": "zip"|"file"|"view", "path", "content"}).
Deterministic: entries sorted, fixed timestamps, STORE.
"""
import difflib, io, json, os, struct, sys, tempfile, zipfile
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "gen6"))
import zstandard
from vgdelta import DeltaSource

EPOCH = (1980, 1, 1, 0, 0, 0)


# ---------------------------------------------------------------- the v2 tree (.vgtree), read
class Empty:
    size = 0
    def read(self, off, n): return b""


def read_vgtree(path, work):
    with open(path, "rb") as f:
        h = f.read(16)
        assert h[:4] == b"VGTR", "not a .vgtree"
        ver, zn = struct.unpack("<IQ", h[4:16])
        assert ver == 1
        table = zstandard.ZstdDecompressor().decompress(f.read(zn), max_output_size=1 << 30).decode("utf-8")
        region = f.read()
    rp = os.path.join(work, os.path.basename(path) + ".add-region")
    open(rp, "wb").write(region)
    return table, DeltaSource(rp, Empty())


def parse(table):
    """lines → (bases, entries); an entry: dict(kind, path, …) in table order."""
    lines = table.split("\n")
    assert lines[0] == "VGTREE 1"
    bases, ents = [], []
    for l in lines[1:]:
        if not l:
            continue
        F = l.split("\t")
        if F[0] == "b":
            bases.append(F[2]); continue
        if F[0] == "d":
            ents.append({"kind": "d", "path": F[1]}); continue
        if F[0] == "l":
            ents.append({"kind": "l", "path": F[1], "link": F[2]}); continue
        e = {"kind": "f", "path": F[1], "size": int(F[2]), "mode": F[3], "op": F[4]}
        if F[4] in ("B", "R", "D"):
            e["base"], e["bpath"] = int(F[5]), F[6]
        if F[4] == "R":
            o, n = F[7].split("+"); e["off"], e["len"] = int(o), int(n)
        if F[4] == "A" and len(F) > 5:                        # v2: a range of the shared region
            o, n = F[5].split("+"); e["off"], e["len"] = int(o), int(n)
        if F[4] == "D":
            e["pieces"] = [(p[0], int(p[1:].split("+")[0]), int(p[1:].split("+")[1])) for p in F[7].split(",")]
        ents.append(e)
    return bases, ents


def text(bases, ents):
    o = ["VGTREE 1"] + [f"b\t{i}\t{c}" for i, c in enumerate(bases)]
    for e in ents:
        if e["kind"] == "d": o.append(f"d\t{e['path']}"); continue
        if e["kind"] == "l": o.append(f"l\t{e['path']}\t{e['link']}"); continue
        head = f"f\t{e['path']}\t{e['size']}\t{e['mode']}\t{e['op']}"
        if e["op"] == "B": o.append(f"{head}\t{e['base']}\t{e['bpath']}")
        elif e["op"] == "R": o.append(f"{head}\t{e['base']}\t{e['bpath']}\t{e['off']}+{e['len']}")
        elif e["op"] == "A": o.append(head)                    # v3: its own add/<path>.add, whole
        else: o.append(f"{head}\t{e['base']}\t{e['bpath']}\t" + ",".join(f"{k}{a}+{n}" for k, a, n in e["pieces"]))
    return "\n".join(o) + "\n"


# ---------------------------------------------------------------- v2 → v3
def convert(src, dst, work):
    table, region = read_vgtree(src, work)
    bases, ents = parse(table)
    adds = {}
    for e in ents:
        if e["kind"] != "f": continue
        if e["op"] == "A":
            adds[e["path"]] = region.read(e["off"], e["len"]); del e["off"], e["len"]
        elif e["op"] == "D":
            own, out = bytearray(), []
            for k, a, n in e["pieces"]:
                if k == "a":
                    out.append(("a", len(own), n)); own += region.read(a, n)
                else:
                    out.append((k, a, n))
            e["pieces"] = out
            if own: adds[e["path"]] = bytes(own)
    tmp = dst + ".part"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_STORED) as z:
        def put(name, data):
            zi = zipfile.ZipInfo(name, EPOCH); zi.compress_type = zipfile.ZIP_STORED; zi.external_attr = 0o644 << 16
            z.writestr(zi, data)
        put("TREE.txt", text(bases, ents).encode("utf-8"))
        for p in sorted(adds): put("add/" + p + ".add", adds[p])
    os.replace(tmp, dst)
    return {"entries": len(ents), "files_with_new_bytes": len(adds), "new_bytes": sum(map(len, adds.values())),
            "vgtree_bytes": os.path.getsize(src), "vgtz_bytes": os.path.getsize(dst)}


# ---------------------------------------------------------------- v3, read
class V3:
    def __init__(self, path, bases_json):
        self.z = zipfile.ZipFile(path)
        self.bases, self.ents = parse(self.z.read("TREE.txt").decode("utf-8"))
        self.by = {e["path"]: e for e in self.ents}
        self.base_files = []
        for b in json.load(open(bases_json)) if bases_json else []:
            self.base_files.append(BaseFiles(b))

    def add(self, path): return self.z.read("add/" + path + ".add")

    def bytes_of(self, path):
        e = self.by[path]; assert e["kind"] == "f", path
        if e["op"] == "A": return self.add(path)
        base = self.base_files[e["base"]].read(e["bpath"])
        if e["op"] == "B": return base
        if e["op"] == "R": return base[e["off"]:e["off"] + e["len"]]
        own = self.add(path) if any(k == "a" for k, _, _ in e["pieces"]) else b""
        return b"".join((own if k == "a" else base)[a:a + n] for k, a, n in e["pieces"])


class BaseFiles:
    def __init__(self, b):
        self.kind, self.path = b["kind"], b["path"]
        if self.kind == "zip": self.z = zipfile.ZipFile(self.path)
        if self.kind == "view":
            self.content = open(b["content"], "rb")
            self.files = {}
            for l in open(self.path, encoding="utf-8").read().split("\n")[1:]:
                F = l.split("\t")
                if len(F) == 4 and F[0] == "f":
                    self.files[F[1]] = [] if F[3] == "-" else [tuple(map(int, x.split("+"))) for x in F[3].split(",")]

    def read(self, p):
        if self.kind == "file": return open(self.path, "rb").read()
        if self.kind == "zip": return self.z.read(p)
        out = bytearray()
        for o, n in self.files[p]: self.content.seek(o); out += self.content.read(n)
        return bytes(out)


def is_text(b):
    if b"\0" in b[:65536]: return False
    try: b[:65536].decode("utf-8"); return True
    except UnicodeDecodeError:
        return sum(32 <= c < 127 or c in (9, 10, 13) for c in b[:65536]) > 0.95 * min(len(b), 65536)


def diff(v, path):
    e = v.by[path]
    new = v.bytes_of(path)
    if e["op"] == "B": return f"{path}: unchanged (base {e['base']} '{e['bpath']}')\n"
    old = v.base_files[e["base"]].read(e["bpath"]) if e["op"] in ("D", "R") else b""
    if is_text(new) and (not old or is_text(old)):
        a = old.decode("utf-8", "replace").splitlines(keepends=True)
        b = new.decode("utf-8", "replace").splitlines(keepends=True)
        return "".join(difflib.unified_diff(a, b, fromfile=f"base/{e.get('bpath', '/dev/null')}", tofile=f"tree/{path}"))
    if e["op"] == "A": return f"{path}: new file, {len(new)} bytes (add/{path}.add)\n"
    o, at = [f"{path}: {e['size']} bytes from base {e['base']} '{e['bpath']}' ({len(old)} bytes)"], 0
    for k, a, n in e.get("pieces", []):
        o.append(f"  [{at:>10}, {at + n:>10})  " + (f"kept   base [{a}, {a + n})" if k == "c" else f"NEW    {n} bytes (add/{path}.add [{a}, {a + n}))"))
        at += n
    return "\n".join(o) + "\n"


def main():
    a = sys.argv[1:]
    opt = lambda k: a[a.index(k) + 1] if k in a else None
    work = opt("--work") or tempfile.mkdtemp(dir=os.path.expanduser("~/Storage/tmp"))
    if a[0] == "convert": print(json.dumps(convert(a[1], a[2], work), indent=1))
    elif a[0] == "ls": print(V3(a[1], None).z.read("TREE.txt").decode("utf-8"), end="")
    elif a[0] == "cat": sys.stdout.buffer.write(V3(a[1], opt("--bases")).bytes_of(a[2]))
    elif a[0] == "diff": print(diff(V3(a[1], opt("--bases")), a[2]), end="")
    elif a[0] == "verify":
        v = V3(a[1], opt("--bases"))
        table, region = read_vgtree(a[2], work)
        bases, ents = parse(table)
        bf = v.base_files
        n = 0
        for e in ents:
            if e["kind"] != "f": continue
            if e["op"] == "A": want = region.read(e["off"], e["len"])
            elif e["op"] == "B": want = bf[e["base"]].read(e["bpath"])
            elif e["op"] == "R": want = bf[e["base"]].read(e["bpath"])[e["off"]:e["off"] + e["len"]]
            else:
                base = bf[e["base"]].read(e["bpath"])
                want = b"".join((region.read(x, m) if k == "a" else base[x:x + m]) for k, x, m in e["pieces"])
            assert v.bytes_of(e["path"]) == want, e["path"]
            n += 1
        print(json.dumps({"files": n, "identical": True}))
    else: sys.exit(__doc__)


if __name__ == "__main__":
    main()
