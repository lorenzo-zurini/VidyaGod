#!/usr/bin/env python3
"""Set/clear dgVoodoo CustomVars on the Wipeout XL attachment node, for A/B testing a crash.

Usage:
  woxl_setvars.py KEY=VALUE [KEY=VALUE ...]   # set (VALUE empty clears the override)
  woxl_setvars.py --reset                     # back to just the FPS limit
"""
import collections, json, os, sys

NODE = os.path.expanduser(
    "~/.VidyaGod/LIBRARY/VidyaGod/[17260][v1.0] Wipeout XL/wipeout_xl_dgvoodoo.json")
BASE = {"dgv_genext_FPSLimit": "30"}   # Wipeout XL is a 30fps game; this one always stays


def main():
    d = json.load(open(NODE), object_pairs_hook=collections.OrderedDict)
    layers = [l for l in d.get("LAYERS", []) if l.get("TYPE") != "CustomVar"]
    vars_ = collections.OrderedDict(BASE)
    if "--reset" not in sys.argv[1:]:
        for arg in sys.argv[1:]:
            k, _, v = arg.partition("=")
            if v == "":
                vars_.pop(k, None)
            else:
                vars_[k] = v
    for k, v in vars_.items():
        layers.append(collections.OrderedDict(
            [("TYPE", "CustomVar"), ("KEY", k), ("DEFAULT", v)]))
    d["LAYERS"] = layers
    json.dump(d, open(NODE, "w"), indent=1)
    open(NODE, "a").write("\n")
    print("vars:", ", ".join("%s=%s" % kv for kv in vars_.items()))


if __name__ == "__main__":
    main()
