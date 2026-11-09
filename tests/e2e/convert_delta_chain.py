#!/usr/bin/env python3
"""--convert-delta-chain end to end: three versions of a game, each a node with a zip; the newer two become deltas.
Each delta must NAME its base by CID (BASE_SOURCES) — the first version's zip, stamped with the CID it is named by,
then each version's own delta — and the result must validate. Teeth: a delta naming nothing, or the wrong layer, or a
base left without its SOURCE, fails here.

    convert_delta_chain.py --vg build/VidyaGod --work build/testtmp/convert_delta
"""
import argparse, base64, hashlib, json, os, shutil, subprocess, sys, zipfile


def cid(path):
    """A file of one 256 KiB chunk or less is one raw leaf: CIDv1 raw sha2-256 (what ComputeCid returns for it)."""
    data = open(path, "rb").read()
    assert len(data) <= 256 * 1024, path
    return "b" + base64.b32encode(bytes([0x01, 0x55, 0x12, 0x20]) + hashlib.sha256(data).digest()).decode().lower().rstrip("=")


def store_zip(path, files):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_STORED) as z:
        for name, body in files.items():
            z.writestr(zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0)), body)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vg", required=True)
    ap.add_argument("--work", required=True)
    a = ap.parse_args()
    data = os.path.abspath(a.work)
    shutil.rmtree(data, ignore_errors=True)
    bundle = f"{data}/fixture/[90000000000002][v1.0] Delta Chain"
    os.makedirs(bundle)
    game = "".join(f"game data line {i:05d}\n" for i in range(3000))
    store_zip(f"{bundle}/v1.zip", {"game.bin": game, "old.txt": "dropped later\n"})
    store_zip(f"{bundle}/v2.zip", {"game.bin": game + "version two\n"})
    store_zip(f"{bundle}/v3.zip", {"game.bin": game + "version two\nversion three\n"})
    v1cid = cid(f"{bundle}/v1.zip")
    for v in ("v1", "v2", "v3"):
        layers = [{"ZIP": f"{v}.zip", "TARGET": "FILES/%GameDir%"}]
        if v == "v1":
            layers.append({"EXEC": [{"LABEL": "Play", "HOST": "win32", "EXE": "game.bin", "TILE": {"UID": "90000000000002", "TITLE": "Delta Chain"}}]})
        # Published-shape nodes: a file each, no handle (the working tree is the editor's).
        json.dump({"LABEL": v, "VARIANT": v, "LAYERS": layers}, open(f"{bundle}/{v}.json", "w"), indent=2)
    json.dump({"Settings": {"LanEnabled": False, "IPFS": {"Enabled": False}}, "LIBRARY": [{"PATH": bundle}]},
              open(f"{data}/GlobalConfig.JSON", "w"), indent=4)

    env = dict(os.environ, HOME=os.path.join(data, "home"))
    os.makedirs(env["HOME"], exist_ok=True)
    fail = []
    def vg(*args):
        return subprocess.run([a.vg, "--bypass-single-instance-lock", "--offline", "--data-dir", data, *args],
                              env=env, capture_output=True, text=True, timeout=300)
    # A node outside the chain containing a version the conversion would rewrite (and so rename) refuses it whole:
    # nothing written, no delta left behind.
    key = json.loads(vg("--json", "node", "facts", "v2").stdout).get("key")
    json.dump({"LABEL": "pack", "LAYERS": [{"NODE": key}]}, open(f"{bundle}/pack.json", "w"), indent=2)
    before = {v: open(f"{bundle}/{v}.json").read() for v in ("v1", "v2", "v3")}
    ref = vg("--convert-delta-chain", "v1", "v2", "v3")
    if ref.returncode == 0 or "contains" not in (ref.stdout + ref.stderr):
        fail.append(f"a node outside the chain containing v2 must refuse the conversion: exit {ref.returncode}\n{ref.stdout[-1500:]}")
    if {v: open(f"{bundle}/{v}.json").read() for v in ("v1", "v2", "v3")} != before or any(f.endswith(".vgdelta") for f in os.listdir(bundle)):
        fail.append("a refused conversion must write nothing")
    os.remove(f"{bundle}/pack.json")
    run = vg("--convert-delta-chain", "v1", "v2", "v3")
    if run.returncode != 0:
        fail.append(f"convert-delta-chain exited {run.returncode}:\n{run.stdout[-3000:]}\n{run.stderr[-3000:]}")
    nodes = {v: json.load(open(f"{bundle}/{v}.json")) for v in ("v1", "v2", "v3")}
    def content(n):
        return next((l for l in n["LAYERS"] if "ZIP" in l or "DELTA" in l), None)
    base = content(nodes["v1"])
    if not base or base.get("SOURCE") != v1cid:
        fail.append(f"v1's zip must carry the CID it is named by ({v1cid}): {base}")
    d2, d3 = content(nodes["v2"]), content(nodes["v3"])
    if not d2 or "DELTA" not in d2 or d2.get("BASE_SOURCES") != [v1cid]:
        fail.append(f"v2 must be a delta naming v1's zip {v1cid}: {d2}")
    if not d3 or "DELTA" not in d3 or not d2 or d3.get("BASE_SOURCES") != [d2.get("SOURCE")] or not d2.get("SOURCE"):
        fail.append(f"v3 must be a delta naming v2's delta (its SOURCE {d2 and d2.get('SOURCE')}): {d3}")
    for v, d in (("v2", d2), ("v3", d3)):
        if d and "DELTA" in d and d.get("SOURCE") != cid(f"{bundle}/{d['DELTA']}"):
            fail.append(f"{v}'s delta must carry its own CID: {d}")
    if not fail:
        # The chain resolves whole from its tip: each version contains the one before it as it is NOW (a rewrite renames
        # a node), and each delta's bases are found beneath it — v3's is v2's delta, v2's is v1's zip.
        out = subprocess.run([a.vg, "--bypass-single-instance-lock", "--offline", "--data-dir", data, "--json", "fold", "bases", "v3"],
                             env=env, capture_output=True, text=True, timeout=120)
        try:
            got = {d["file"]: [b and b["file"] for b in d["bases"]] for d in json.loads(out.stdout)["deltas"]}
        except Exception:
            got = None
        want = {d2["DELTA"]: ["v1.zip"], d3["DELTA"]: [d2["DELTA"]]}
        if got != want:
            fail.append(f"fold bases v3: want {want}, got {got}\n{out.stdout[-2000:]}{out.stderr[-2000:]}")
        val = subprocess.run([a.vg, "--bypass-single-instance-lock", "--offline", "--data-dir", data, "--validate-nodes"],
                             env=env, capture_output=True, text=True, timeout=300)
        bad = [l for l in (val.stdout + val.stderr).splitlines()
               if "cannot rebuild" in l or ("DELTA" in l and "built over" in l)]
        if bad:
            fail.append("the converted chain must validate:\n" + "\n".join(bad))
    if fail:
        print("\n\n".join(fail)); return 1
    print("convert-delta-chain: v2 and v3 are deltas naming their bases by CID; the chain validates")
    shutil.rmtree(data, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
