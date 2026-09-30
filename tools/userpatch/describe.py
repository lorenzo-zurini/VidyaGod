"""Explain a UserPatch byte op at the machine level, for its COMMENT: which instructions it changes and what that does
(a branch forced or skipped, a hook into UserPatch's own code, a constant changed, an option byte set).

The ops are runs of bytes where the installer's output differs from the all-off executable, so they start anywhere —
on an opcode, inside an immediate. The instruction that covers a run's first byte is found by decoding the pristine
bytes from several points before it and taking the boundary most of them agree on (x86 decoding resynchronises
within a few instructions); the pristine and patched bytes are then disassembled over the same span."""
import collections
import difflib
import struct

import capstone

MD = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
#Instructions no compiler or UserPatch routine emits: bytes that decode to them are data (a pointer table, a constant).
NOT_CODE = {"aaa", "aas", "daa", "das", "aam", "aad", "insb", "insd", "outsb", "outsd", "in", "out", "arpl", "bound",
            "into", "hlt", "les", "lds", "salc", "icebp"}


def sections(img):
    pe = struct.unpack_from("<I", img, 0x3c)[0]
    n, opt = struct.unpack_from("<H", img, pe + 6)[0], struct.unpack_from("<H", img, pe + 20)[0]
    base = struct.unpack_from("<I", img, pe + 24 + 28)[0]
    out = []
    for i in range(n):
        o = pe + 24 + opt + 40 * i
        vsz, va, rsz, raw, *_rest, ch = struct.unpack_from("<IIIIIIHHI", img, o + 8)
        out.append((img[o:o + 8].rstrip(b"\0").decode(), base + va, raw, rsz, bool(ch & 0x20000000)))
    return out


class Exe:
    def __init__(self, img):
        self.img = img
        self.secs = sections(img)

    def where(self, off):
        for name, va, raw, rsz, code in self.secs:
            if raw <= off < raw + rsz:
                return name, va + off - raw, code
        return None, off, False

    def section_of_va(self, va):
        for name, sva, raw, rsz, code in self.secs:
            if sva <= va < sva + rsz:
                return name
        return None

    def start_of(self, off):
        """File offset of the instruction covering `off` (majority vote of linear sweeps from 48..63 bytes back)."""
        votes = collections.Counter()
        for back in range(48, 64):
            at = off - back
            for ins in MD.disasm(self.img[at:off + 16], at):
                if ins.address <= off < ins.address + ins.size:
                    votes[ins.address] += 1
                    break
                if ins.address > off:
                    break
        return votes.most_common(1)[0][0] if votes else off


def insns(buf, start, end, va0):
    """Instructions decoded from buf[start:], until one ends at or past `end`; addresses as VAs."""
    out = []
    for ins in MD.disasm(buf[start:end + 16], va0):
        out.append(ins)
        if ins.address - va0 + start + ins.size >= end:
            break
    return out


def strings(b):
    """The printable runs of a NUL-separated byte string, if that is what it is (else None)."""
    runs = [r.decode("latin-1") for r in b.split(b"\0") if r]
    printable = sum(len(r) for r in runs if all(32 <= ord(c) < 127 for c in r))
    if not runs or printable < 0.9 * sum(len(r) for r in runs) or max(len(r) for r in runs) < 3:
        return None
    return runs


def quoted(runs, n):
    return clip(", ".join(repr(r) for r in runs), n)


def text(ins):
    return (ins.mnemonic + " " + ins.op_str).strip()


def target(ins):
    """A direct branch's destination, else None."""
    if not (ins.mnemonic.startswith("j") or ins.mnemonic in ("call", "loop")):
        return None
    try:
        return int(ins.op_str, 16)
    except ValueError:
        return None


def describe(exe, op):
    """One line explaining a Replace op over exe.img (the pristine all-off executable)."""
    off = int(op["OFFSET"], 16)
    old, new = bytes.fromhex(op["EXPECT"]), bytes.fromhex(op["REPLACE"])
    sec, va, code = exe.where(off)
    if not code:
        so, sn = strings(old), strings(new)
        if sn and (so or not old.strip(b"\0")):
            return f"strings in {sec} (VA {va:#x}): {quoted(so or [], 45)} -> {quoted(sn, 60)}"
        if len(old) == 1:
            return f"option byte {old.hex()} -> {new.hex()} in UserPatch's {sec} data (VA {va:#x})"
        return f"UserPatch data in {sec} (VA {va:#x}): {old.hex()} -> {new.hex()}"
    s = exe.start_of(off)
    end = off + len(old)
    patched = bytearray(exe.img)
    patched[off:end] = new
    svar = va - (off - s)
    was, now = insns(exe.img, s, end, svar), insns(bytes(patched), s, end, svar)
    where = f"VA {svar:#x}" + ("" if sec == ".text" else f" in {sec}")
    w0, n0 = (was[0] if was else None), (now[0] if now else None)
    cave = len(old) >= 8 and not old.strip(b"\0")                  # written into zero-filled space
    if cave and strings(new):
        return f"new UserPatch strings ({where}): {quoted(strings(new), 90)}"
    odd = lambda i: i.mnemonic in NOT_CODE or not i.bytes.strip(b"\0")      # impossible, or a run of zero bytes
    if cave and (not now or odd(now[0]) or sum(map(odd, now)) * 4 > len(now)):
        return f"new UserPatch data, {len(new)} bytes: {clip(new.hex(), 48)} ({where})"
    if cave:
        return f"new UserPatch code, {len(new)} bytes: {clip('; '.join(map(text, now)), 80)} ({where})"
    if w0 is not None and n0 is not None and w0.address == n0.address and w0.mnemonic == n0.mnemonic \
            and w0.mnemonic in ("jmp", "call") and target(w0) and target(n0) and target(w0) != target(n0):
        return f"{w0.mnemonic} retargeted {target(w0):#x} -> {target(n0):#x}" \
               + (" (UserPatch code)" if exe.section_of_va(target(n0)) == ".patch" else "") + f" ({where})"
    jcc = lambda i: i is not None and i.mnemonic.startswith("j") and i.mnemonic not in ("jmp",)
    if jcc(w0) and n0 is not None and n0.mnemonic == "jmp" and target(n0) == target(w0):
        return f"{text(w0)} -> jmp: always take this branch ({where})"
    if jcc(w0) and n0 is not None and n0.mnemonic == "nop":
        return f"{text(w0)} -> nop: never take this branch ({where})"
    if jcc(w0) and n0 is not None and n0.mnemonic == "jmp" and exe.section_of_va(target(n0) or 0) == ".patch":
        return f"{text(w0)} -> jmp {target(n0):#x}: branch into UserPatch code ({where})"
    for i in now:
        t = target(i)
        if i.mnemonic in ("jmp", "call") and t is not None and exe.section_of_va(t) == ".patch" and \
                not any(target(w) == t for w in was):
            replaced = "; ".join(text(w) for w in was)
            kind = "call" if i.mnemonic == "call" else "jump"
            return f"hook: {kind} to UserPatch code at {t:#x}, replacing '{clip(replaced, 60)}' ({where})"
    if len(was) == len(now) and all(w.mnemonic == n.mnemonic for w, n in zip(was, now)):
        diffs = [f"{text(w)} -> {n.op_str}" for w, n in zip(was, now) if w.op_str != n.op_str]
        return f"operand: {clip('; '.join(diffs), 90)} ({where})"
    pad = any(not i.bytes.strip(b"\0") for i in now)
    now = [i for i in now if i.bytes.strip(b"\0")]                 # zero fill after code that moved up
    was = [i for i in was if i.bytes.strip(b"\0")]
    a, b = [text(i) for i in was], [text(i) for i in now]
    gone, added = [], []
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(a=a, b=b, autojunk=False).get_opcodes():
        if tag != "equal":
            gone += a[i1:i2]
            added += b[j1:j2]
    parts = ([f"removes '{clip('; '.join(gone), 50)}'"] if gone else []) + ([f"adds '{clip('; '.join(added), 50)}'"] if added else [])
    if pad:
        parts.append("pads the freed bytes with zeros")
    return (", ".join(parts) or "same instructions") + f" ({where})"


def clip(s, n):
    return s if len(s) <= n else s[:n - 2] + ".."
