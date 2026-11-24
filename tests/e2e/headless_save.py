#!/usr/bin/env python3
"""A command run on its own (no app up) that changes the config leaves the change on disk: the model saves a moment
after it is asked (saves asked together are one), and a call's process ends before that moment — without the model's
destructor — so the save still waiting is written at the call's end (adversary review 2026-11-24). Teeth: drop the
flush in CliApp::Main (the change is lost: the file keeps the old value).

    headless_save.py --vg build/VidyaGod --work build/testtmp/headless_save
"""
import argparse, json, os, shutil, subprocess, sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vg", required=True)
    ap.add_argument("--work", required=True)
    a = ap.parse_args()
    data = os.path.abspath(a.work)
    shutil.rmtree(data, ignore_errors=True)
    os.makedirs(os.path.join(data, "home"))
    cfg = f"{data}/GlobalConfig.JSON"
    json.dump({"Settings": {"LanEnabled": False, "IPFS": {"Enabled": False}}, "LIBRARY": []}, open(cfg, "w"), indent=4)
    env = dict(os.environ, VIDYAGOD_NO_CONTROLLER_SERVICE="1", HOME=os.path.join(data, "home"), QT_QPA_PLATFORM="offscreen")
    fail = []
    for start in ("controller", "desktop"):
        r = subprocess.run([a.vg, "--data-dir", data, "--no-color", "controller", "settings", "--start", start],
                           env=env, capture_output=True, text=True, timeout=120)
        if r.returncode != 0: fail.append(f"--start {start}: exit {r.returncode}: {r.stderr[-300:]}")
        saved = json.load(open(cfg)).get("Settings", {}).get("Controller", {}).get("Start")
        if saved != start: fail.append(f"--start {start}: the saved config says {saved!r}")
        r = subprocess.run([a.vg, "--data-dir", data, "--no-color", "--json", "controller", "settings"],
                           env=env, capture_output=True, text=True, timeout=120)
        try: read = json.loads(r.stdout[r.stdout.index("{"):]).get("start")
        except ValueError: read = None
        if read != start: fail.append(f"--start {start}: a fresh call reads {read!r}")
    print("headless_save:", "FAILED " + "; ".join(fail) if fail else "ok")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
