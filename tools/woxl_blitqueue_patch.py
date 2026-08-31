#!/usr/bin/env python3
"""Patch NET-WOXL.EXE so leaving the title screen does not blit from a surface it just released.

THE BUG (found 2026-08-31, with a watchpoint on dgVoodoo's internal surface object):
Wipeout XL queues sprites into two small "deferred blit" lists and flushes them later. The counts live at
0x90d090 (4 dwords: page 0/1 x list 0/1) and the 40-byte entries at 0x90d0a0; each entry holds the source
IDirectDrawSurface at +4. The teardown that runs when you press Enter at the title screen releases two
surfaces via IDirectDrawSurface::Release ([0x903fe8] and [0x903fe0], vtable slot 8) and clears its own
bookkeeping globals -- but it never clears those queues. The very next call flushes them and blits from the
surface it just released:

    0x0044bdfb  call [eax+8]              ; Release([0x903fe0])
    0x0044bdfe  mov  [0x9341a8], 0
    0x0044be08  mov  [0x4d7dd0], 1        ; <- we splice in here
    ...
    0x0044be44  call 0x452776             ; -> flush -> 0x47fa48 -> Blt(src = released surface)

Real DirectDraw survives this; dgVoodoo frees the surface's backing resource on Release, then dereferences
that NULL resource inside Blt while comparing source and destination pixel formats
(`mov eax,[esi+0x2d0]` / `imul edx,[eax+0x254],0x220`), so the game dies with a page fault reading 0x254
(0x250 on 2.81.3 -- same bug, different field offset). This is why the network build had never been seen to
get past its title screen.

THE FIX: zero the four queue counts right after the releases, which is what the teardown should have done.
The queued sprites are per-frame and their surfaces are gone, so dropping them is the correct behaviour.
Proven live first by writing the same four zeros with a debugger at that exact moment: the game then went
straight through to the multiplayer menu and on into "RACE CUSTOMIZATION / CREATE SESSION MODE".

Only NET-WOXL.EXE is touched; single player (Wipeout2.exe) has its own copy of this code and is unaffected.

Usage: woxl_blitqueue_patch.py <in.exe> <out.exe>

The patched exe is baked into "Wipeout XL Network Patch No-CD Patch.zip" in the package (that zip is already
a derivative; the pristine NET-WOXL.EXE stays untouched in "Wipeout XL Network Patch.zip"). Re-running this
script against the pristine exe reproduces the shipped binary byte for byte.
"""
import sys

IMAGE_BASE = 0x400000
TEXT_VA = 0x401000
TEXT_RAW = 0x400

PATCH_VA = 0x0044BE08      # mov dword [0x4d7dd0], 1   (10 bytes, displaced into the cave)
RESUME_VA = 0x0044BE12     # pop edi
CAVE_VA = 0x004CF360       # zero padding at the tail of .text, file-backed and mapped executable

COUNTS = [0x90D090, 0x90D094, 0x90D098, 0x90D09C]

ORIGINAL_AT_PATCH = bytes.fromhex("c705d07d4d0001000000")   # mov dword [0x4d7dd0], 1


def va_to_off(va):
    return va - TEXT_VA + TEXT_RAW


def build_cave():
    b = bytearray()
    b += b"\x50"                      # push eax   (Release's return value is still in eax)
    b += b"\x31\xc0"                  # xor eax, eax
    for addr in COUNTS:
        b += b"\xa3" + addr.to_bytes(4, "little")     # mov [addr], eax
    b += b"\x58"                      # pop eax
    b += ORIGINAL_AT_PATCH            # the instruction we displaced
    # jmp back
    jmp_from = CAVE_VA + len(b) + 5
    b += b"\xe9" + ((RESUME_VA - jmp_from) & 0xFFFFFFFF).to_bytes(4, "little")
    return bytes(b)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    data = bytearray(open(src, "rb").read())

    off = va_to_off(PATCH_VA)
    if bytes(data[off:off + len(ORIGINAL_AT_PATCH)]) != ORIGINAL_AT_PATCH:
        raise SystemExit("patch site does not match at 0x%08x -- wrong or already-patched binary" % PATCH_VA)

    cave = build_cave()
    coff = va_to_off(CAVE_VA)
    if any(data[coff:coff + len(cave)]):
        raise SystemExit("cave at 0x%08x is not free" % CAVE_VA)
    data[coff:coff + len(cave)] = cave

    # jmp cave, then pad the rest of the displaced instruction with nops
    rel = (CAVE_VA - (PATCH_VA + 5)) & 0xFFFFFFFF
    stub = b"\xe9" + rel.to_bytes(4, "little") + b"\x90" * (len(ORIGINAL_AT_PATCH) - 5)
    data[off:off + len(ORIGINAL_AT_PATCH)] = stub

    open(dst, "wb").write(bytes(data))
    print("patched %s -> %s" % (src, dst))
    print("  splice at 0x%08x: jmp 0x%08x (+%d nops)" % (PATCH_VA, CAVE_VA, len(ORIGINAL_AT_PATCH) - 5))
    print("  cave 0x%08x: %d bytes, clears counts %s" % (
        CAVE_VA, len(cave), ", ".join("0x%x" % c for c in COUNTS)))


if __name__ == "__main__":
    main()
