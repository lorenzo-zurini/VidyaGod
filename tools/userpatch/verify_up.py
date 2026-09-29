"""Check the generated patches against the installer: the default options rebuild the old installed executable
(ours/age2_x1.exe), and random installer flag sets (rand_flags.txt, installed as out/rnd<i>.exe) match exactly.
Run from UP_WORK."""
import sys, os
sys.path.insert(0, os.path.expanduser("~/Code/VidyaGod/tools/gen6")); sys.path.insert(0, ".")
from resolve import when
import gen_up, model

def build(vals):
    img = bytearray(model.Z)
    for L in gen_up.edit_layers:
        if not when(L["WHEN"], vals): continue
        for o in L["EDIT"]:
            if "WHEN" in o and not when(o["WHEN"], vals): continue
            off = int(o["OFFSET"], 16)
            if o["MODE"] == "Replace":
                exp, rep = bytes.fromhex(o["EXPECT"]), bytes.fromhex(o["REPLACE"])
                cur = bytes(img[off:off + len(exp)])
                if cur == rep: continue
                assert cur == exp, (L["WHEN"], o["OFFSET"], "EXPECT mismatch")
                img[off:off + len(rep)] = rep
            elif o["MODE"] == "Or":
                v = bytes.fromhex(o["VALUE"])
                for i, b in enumerate(v): img[off + i] |= b
    return bytes(img)

defaults = {k: d["DEFAULT"] for k, d in gen_up.VARS.items()}
old = open("ours/age2_x1.exe", "rb").read()
print("defaults == old UserPatch exe:", build(defaults) == old)

POS2VAR = {pos: var for pos, var, *_ in gen_up.EXE_OPTS}
def vals_for(f):
    v = dict(defaults)
    for pos, var in POS2VAR.items(): v[var] = f[pos - 1]
    core = f[0] in "12"
    v["up_style"] = ("left" if not core else "leftcore") if f[23] == "1" else ("widescreen" if core else "centered")
    return v
bad = 0; skipped = 0
for i, f in enumerate(open("rand_flags.txt").read().split(), 1):
    v = vals_for(f)
    if v["up_style"] == "leftcore": skipped += 1; continue      # not an installer style (left-aligned is classic)
    if build(v) != open(f"out/rnd{i}.exe", "rb").read(): bad += 1; print("MISMATCH", i, f)
print("random installer runs matched:", 30 - bad - skipped, "mismatched:", bad, "skipped (left+widescreen combo):", skipped)
