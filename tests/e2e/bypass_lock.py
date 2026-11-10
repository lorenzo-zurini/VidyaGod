#!/usr/bin/env python3
"""--bypass-single-instance-lock runs beside the app and never takes its lock: a window started with it on a data dir
whose lock is free (a harness's off-screen window, before the app is up) leaves the lock free, so the app itself still
starts there and takes the lock; and one started beside the running app leaves the app's command socket alone.
Teeth: a bypass run that takes the lock when it is free makes the app's own start abort "already running" here; one
that always listens takes the app's socket over.

    bypass_lock.py --vg build/VidyaGod --work build/testtmp/bypass_lock
"""
import argparse, fcntl, json, os, shutil, signal, subprocess, sys, time


def lock_free(path):
    with open(path, "a") as f:
        try:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return False
        fcntl.flock(f, fcntl.LOCK_UN)
        return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vg", required=True)
    ap.add_argument("--work", required=True)
    a = ap.parse_args()
    data = os.path.abspath(a.work)
    shutil.rmtree(data, ignore_errors=True)
    os.makedirs(os.path.join(data, "home"))
    json.dump({"Settings": {"LanEnabled": False, "IPFS": {"Enabled": False}}, "LIBRARY": []},
              open(f"{data}/GlobalConfig.JSON", "w"), indent=4)
    env = dict(os.environ, HOME=os.path.join(data, "home"), QT_QPA_PLATFORM="offscreen")
    lock = f"{data}/vidyagod.lock"
    procs, fail = [], []

    def start(*extra, log):
        p = subprocess.Popen([a.vg, "--data-dir", data, *extra], env=env, stdout=open(f"{data}/{log}", "w"),
                             stderr=subprocess.STDOUT, start_new_session=True)
        procs.append(p)
        return p

    def up(log):   # its window built and its command socket listening (or left to the app): the start is done
        text = open(f"{data}/{log}", errors="replace").read()
        return "answering commands at" in text or "the command socket stays with" in text

    try:
        beside = start("--bypass-single-instance-lock", log="beside.log")
        for _ in range(600):
            if up("beside.log") or beside.poll() is not None: break
            time.sleep(0.1)
        else:
            fail.append("the window run beside never came up")
        if beside.poll() is not None: fail.append("the window run beside ended")
        if not lock_free(lock): fail.append("a run beside the app took the data dir's lock")

        app = start(log="app.log")
        for _ in range(600):
            if up("app.log") or app.poll() is not None: break
            time.sleep(0.1)
        if app.poll() is not None or lock_free(lock):
            fail.append("the app could not start beside a bypass run: " + open(f"{data}/app.log").read()[-400:])
        if "already running" in open(f"{data}/app.log").read(): fail.append("the app said another instance runs")

        # With the app up, a window run beside it leaves the app's command socket alone.
        late = start("--bypass-single-instance-lock", log="late.log")
        for _ in range(600):
            if up("late.log") or late.poll() is not None: break
            time.sleep(0.1)
        if "the command socket stays with" not in open(f"{data}/late.log", errors="replace").read():
            fail.append("a run beside the app took over the app's command socket")
    finally:
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
        for p in procs:
            try: p.wait(30)
            except subprocess.TimeoutExpired: os.killpg(p.pid, signal.SIGKILL)

    for f in fail: print("FAIL:", f)
    print("bypass_lock:", "FAILED" if fail else "ok")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
