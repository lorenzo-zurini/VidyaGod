#!/usr/bin/env python3
"""Read-only random access to a .vgdelta chain (VidyaGodFS/src/vgdelta.h layout), enough to list the files of the
zip a delta chain reconstructs. Header 44 B: "VGD1" u32 ver u32 block u64 target u64 base u64 hash u32 nseg u32 nblk;
then (u64 clen, u64 ulen, zstd 17-B segment records), (u64 clen, u64 ulen, zstd 16-B block records), ADD frames.

    names = zip_names_of(["base.zip", "d1.vgdelta", "d2.vgdelta"])   # lower-cased member names of the top
"""
import bisect, io, struct, zipfile
import zstandard


class FileSource:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.f.seek(0, 2)
        self.size = self.f.tell()

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)


class DeltaSource:
    def __init__(self, path, base):
        self.base, self.f = base, open(path, "rb")
        h = self.f.read(44)
        if h[:4] != b"VGD1":
            raise ValueError(f"{path}: not a vgdelta")
        self.block = struct.unpack_from("<I", h, 8)[0]
        self.size = struct.unpack_from("<Q", h, 12)[0]
        nseg, nblk = struct.unpack_from("<II", h, 36)
        d = zstandard.ZstdDecompressor()
        off = 44
        clen, ulen = struct.unpack("<QQ", self._at(off, 16))
        raw = d.decompress(self._at(off + 16, clen), max_output_size=ulen)
        off += 16 + clen
        self.segs, self.starts, t = [], [], 0
        for i in range(nseg):
            kind, ln, src = struct.unpack_from("<BQQ", raw, 17 * i)
            self.starts.append(t); self.segs.append((kind, ln, src)); t += ln
        clen, ulen = struct.unpack("<QQ", self._at(off, 16))
        raw = d.decompress(self._at(off + 16, clen), max_output_size=ulen)
        off += 16 + clen
        self.blocks = [struct.unpack_from("<QII", raw, 16 * i) for i in range(nblk)]
        self.region = off
        self.cache = {}

    def _at(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def _block(self, b):
        if b not in self.cache:
            co, cl, ul = self.blocks[b]
            self.cache[b] = zstandard.ZstdDecompressor().decompress(self._at(self.region + co, cl), max_output_size=ul)
        return self.cache[b]

    def read(self, off, n):
        out = bytearray()
        n = max(0, min(n, self.size - off))
        i = bisect.bisect_right(self.starts, off) - 1
        while n > 0:
            kind, ln, src = self.segs[i]
            intra = off - self.starts[i]
            take = min(n, ln - intra)
            if kind == 0:
                out += self.base.read(src + intra, take)
            else:
                a, rem = src + intra, take
                while rem:
                    b, o = divmod(a, self.block)
                    chunk = self._block(b)[o:o + rem]
                    out += chunk; a += len(chunk); rem -= len(chunk)
            off += take; n -= take; i += 1
        return bytes(out)


class Reader(io.RawIOBase):
    def __init__(self, src):
        self.src, self.pos = src, 0

    def readable(self):
        return True

    def seekable(self):
        return True

    def seek(self, off, whence=0):
        self.pos = off if whence == 0 else self.pos + off if whence == 1 else self.src.size + off
        return self.pos

    def tell(self):
        return self.pos

    def readinto(self, b):
        data = self.src.read(self.pos, len(b))
        b[:len(data)] = data
        self.pos += len(data)
        return len(data)


def source_of(chain):
    """chain: [zip, delta, delta, ...] bottom → top."""
    src = FileSource(chain[0])
    for p in chain[1:]:
        src = DeltaSource(p, src)
    return src


def zip_names_of(chain):
    with zipfile.ZipFile(io.BufferedReader(Reader(source_of(chain)))) as z:
        return {n.rstrip("/").lower() for n in z.namelist()}


if __name__ == "__main__":
    import sys
    names = sorted(zip_names_of(sys.argv[1:]))
    print(len(names)); print("\n".join(names[:20]))
