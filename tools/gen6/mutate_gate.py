#!/usr/bin/env python3
"""Teeth for the generation-6 equivalence gate: every mutant below must BLOCK it.

Data mutants break one migrated node (restored after); code mutants break the resolver or the gen-5 model in a
throw-away copy of tools/gen6. Exit status = number of mutants the gate missed.

Usage: mutate_gate.py <gen5-work-dir> <gen6-library>
"""
import json, os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from resolve import load_nodes


def gate(tools, g5, g6, only=None):
    """The gate as gate.sh runs it: the self-tests, then the equivalence check."""
    for t in ("resolve.py", "migrate.py"):
        r = subprocess.run([sys.executable, os.path.join(tools, t), "--self-test"], capture_output=True, text=True, cwd=tools)
        if r.returncode != 0:
            return True, [f"[BLOCK] {t} self-test: " + (r.stdout + r.stderr).strip().splitlines()[-1][:120]]
    cmd = [sys.executable, os.path.join(tools, "equivalence.py"), g5, g6] + (["--only", only] if only else [])
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=tools)
    return r.returncode != 0, [l for l in r.stdout.splitlines() if l.startswith("[BLOCK]")]


# ---- data mutants: (node LABEL, is a VARIANT, mutation, launch LABEL to gate)
def swap_patch_under_base(n):      # the patch's own zip beneath the game it patches
    L = n["LAYERS"]; z = [x for x in L if "ZIP" in x][0]; L.remove(z); L.insert(0, z)


def drop_reg(n):
    n["LAYERS"] = [x for x in n["LAYERS"] if "REG" not in x]


def edit_first(n):                 # an EDIT beneath the content it edits
    L = n["LAYERS"]; e = [x for x in L if "EDIT" in x][0]; L.remove(e); L.insert(0, e)


def var_default(n):
    for x in n["LAYERS"]:
        if "VARS" in x:
            k = next(iter(x["VARS"])); x["VARS"][k]["DEFAULT"] = "MUTATED"; return


def dll_order(n):
    for x in n["LAYERS"]:
        if "DLL" in x:
            k = next(iter(x["DLL"])); x["DLL"][k] = "b"; return


def exe_path(n):
    for x in n["LAYERS"]:
        for e in x.get("EXEC", []):
            if "EXE" in e:
                e["EXE"] += ".x"; return


def tile_title(n):
    for x in n["LAYERS"]:
        for e in x.get("EXEC", []):
            if "TILE" in e:
                e["TILE"]["TITLE"] += " (mutated)"; return


def graft_unticked(n):             # a pre-ticked graft no longer recommended: the default launch loses it
    n.pop("RECOMMENDED")


DATA = [("wipeout_xl_widescreen_patch_wipeout2_exe", False, graft_unticked, "Single Player"),
        ("w3d_patch_sp2_content", False, swap_patch_under_base, "w3d_anniversary"),
        ("sh2_enhanced_content_delta_from_sh2_base", False, drop_reg, "sh2_enhanced"),
        ("w4m_base_content_worms_4_mayhem", False, edit_first, "w4m_anniversary"),
        ("tonic_trouble_retail_content", False, var_default, "Tonic Trouble Retail"),
        ("sh2_enhanced", True, dll_order, "sh2_enhanced"),
        ("sh2_enhanced", True, exe_path, "sh2_enhanced"),
        ("sh2_enhanced_content_delta_from_sh2_base", False, tile_title, "sh2_enhanced")]

# ---- code mutants: (file, old, new)
CODE = [("resolve.py", 'reg[(arch, path.lower(), k.lower())] = (path, k, v)',
         'reg.setdefault((arch, path.lower(), k.lower()), (path, k, v))'),                       # registry: first wins
        ("resolve.py", '            if "WHEN" in L and not when(L["WHEN"], self.wv):\n                continue\n            t = type_of(L)',
         '            t = type_of(L)'),                                                           # layer WHEN ignored
        ("resolve.py", 'if not any(m in present for m in L["ANY"]):', 'if False:'),                    # ANY never checked
        ("resolve.py", '    present = set(plan["order"])', '    present = set(idx)'),                        # every graft offered everywhere
        ("resolve.py", 'exe[lab] = merge(exe.get(lab), e) if lab in exe else copy.deepcopy(e)',
         'exe[lab] = copy.deepcopy(e)'),                                                         # entries replaced whole
        ("gen5model.py", "    emit(root)\n    return out", "    emit(root)\n    return out[::-1]"),  # gen-5 closure order
        ("gen5model.py", '    for V in n.get("VARS", []):', '    for V in n.get("VARS", [])[1:]:')]


def main():
    g5, g6 = sys.argv[1], sys.argv[2]
    nodes = load_nodes(g6)
    by_label = lambda l, var: [c for c, (n, d) in nodes.items() if n["LABEL"] == l and (not var or n.get("VARIANT"))][0]
    missed = 0
    for label, isvar, fn, launch in DATA:
        c = by_label(label, isvar)
        p = next(os.path.join(dp, f) for dp, _, fs in os.walk(g6) for f in fs
                 if f.endswith(".json") and json.load(open(os.path.join(dp, f))).get("CID") == c)
        orig = open(p).read()
        n = json.loads(orig); fn(n); open(p, "w").write(json.dumps(n, indent=4))
        try:
            caught, why = gate(HERE, g5, g6, only=None if fn is tile_title else by_label(launch, True))
        finally:
            open(p, "w").write(orig)
        print(("CAUGHT " if caught else "MISSED ") + f"data {fn.__name__} on {label}", why)
        missed += not caught
    for f, old, new in CODE:
        with tempfile.TemporaryDirectory() as t:
            for x in os.listdir(HERE):
                if x.endswith(".py"):
                    shutil.copy(os.path.join(HERE, x), t)
            src = open(os.path.join(t, f)).read()
            assert src.count(old) == 1, f"mutant anchor not found in {f}: {old[:50]}"
            open(os.path.join(t, f), "w").write(src.replace(old, new))
            caught, why = gate(t, g5, g6)
        print(("CAUGHT " if caught else "MISSED ") + f"code {f}: {old.strip()[:50]}", why)
        missed += not caught
    return missed


if __name__ == "__main__":
    sys.exit(main())
