#!/usr/bin/env python3
"""Drive the package editor the way a user does, OFF-SCREEN: a VidyaGod GUI on its own Xvfb display, its ImGui canvas
clicked BY WHAT AN ITEM SAYS (labels read from the canvas's probe file) with real mouse and keyboard input, and its
Qt widgets (toolbar, JSON panel, dialogs) by name through tools/a11ydrive.py.

  tools/edrive.py start [--data-dir D]      Xvfb :97 (1920x1080) + VidyaGod on it, probing the canvas
  tools/edrive.py stop                      kill that GUI and its Xvfb (never any other VidyaGod)
  tools/edrive.py shot OUT.png              screenshot of the whole Xvfb screen
  tools/edrive.py items [TEXT]              the canvas items of the last frame (label, window, rect), filtered
  tools/edrive.py click TEXT [--nth N] [--window W] [--right] [--double]
                                            click the canvas item whose visible label matches TEXT
  tools/edrive.py move TEXT                 hover it (menus that open on hover)
  tools/edrive.py at X Y [--right|--double] click at screen coordinates (empty canvas, a drop point)
  tools/edrive.py drag TEXT X Y             press on an item, move to (X, Y), release
  tools/edrive.py type TEXT                 type into whatever has focus
  tools/edrive.py key KEYS                  press keys (xdotool names: Return, Escape, ctrl+s ...)
  tools/edrive.py wait TEXT [--secs S]      block until a canvas item with that label is on screen
  tools/edrive.py qt VERB ARGS...           tools/a11ydrive.py VERB ARGS --pid <this GUI>

TEXT matches the label's VISIBLE part (ImGui hides everything from "##" on) case-insensitively: an exact match wins
over a substring one; --nth picks among several. Everything here is ordinary input to the app: nothing reaches into it
that a user could not do with the mouse and keyboard.

Why off-screen: game and GUI runs never appear on the user's screens. Why Xvfb + xcb: the build runs natively on
Wayland, where xdotool sees nothing; on its own X server the app is driven and screenshotted without touching the
desktop (WAYLAND_DISPLAY is removed so Qt cannot pick the real compositor).
"""
import math
import json, os, signal, subprocess, sys, time, argparse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DISPLAY = os.environ.get("VG_EDRIVE_DISPLAY", ":97")
STATE = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "vg-edrive.json")
PROBE = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "vg-edrive-probe.json")


def env():
    E = dict(os.environ, DISPLAY=DISPLAY, QT_QPA_PLATFORM="xcb", VG_UI_PROBE=PROBE, QT_LINUX_ACCESSIBILITY_ALWAYS_ON="1")
    E.pop("WAYLAND_DISPLAY", None)
    return E


def state():
    try:
        return json.load(open(STATE))
    except (OSError, ValueError):
        return {}


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except (OSError, TypeError):
        return False


def xdo(*a):
    subprocess.run(["xdotool", *a], env=env(), check=True)


def start(data_dir):
    S = state()
    if alive(S.get("app")):
        sys.exit(f"already running (pid {S['app']}); `stop` first")
    xvfb = subprocess.Popen(["Xvfb", DISPLAY, "-screen", "0", "1920x1080x24", "-nolisten", "tcp"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    for _ in range(50):
        if subprocess.run(["xdpyinfo"], env=env(), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
            break
        time.sleep(0.1)
    else:
        xvfb.kill()
        sys.exit(f"Xvfb {DISPLAY} did not come up")
    args = [os.path.join(ROOT, "build", "VidyaGod"), "--bypass-single-instance-lock"]
    if data_dir:
        args += ["--data-dir", data_dir]
    log = open(os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "vg-edrive.log"), "w")
    app = subprocess.Popen(args, env=env(), stdout=log, stderr=subprocess.STDOUT, start_new_session=True, cwd=ROOT)
    json.dump({"xvfb": xvfb.pid, "app": app.pid}, open(STATE, "w"))
    print(f"VidyaGod pid {app.pid} on {DISPLAY} (Xvfb pid {xvfb.pid}); log {log.name}")


def stop():
    S = state()
    for k in ("app", "xvfb"):
        if alive(S.get(k)):
            os.killpg(S[k], signal.SIGTERM)
    for p in (STATE, PROBE):
        try:
            os.remove(p)
        except OSError:
            pass


def frame():
    try:
        return json.load(open(PROBE))
    except (OSError, ValueError):
        sys.exit("no canvas frame yet - is the Package Editor open?")


def visible(label):
    """What the item shows; an input with nothing shown answers to its hidden name ("##Zip" is the Zip box)."""
    shown = label.split("##", 1)[0]
    if shown.strip() or "##" not in label or "###" in label:
        return shown
    return label.rsplit("##", 1)[1]


def find(text, nth=0, window=None, quiet=False):
    F = frame()
    t = text.lower()
    items = [i for i in F["items"] if visible(i["label"]).strip()]
    if window:
        items = [i for i in items if window.lower() in i["window"].lower()]
    exact = [i for i in items if visible(i["label"]).strip().lower() == t]
    hits = exact or [i for i in items if t in visible(i["label"]).lower()]
    if not hits:
        if quiet:
            return None
        sys.exit(f"no canvas item says {text!r} (try `items`)")
    if nth >= len(hits):
        sys.exit(f"{len(hits)} item(s) say {text!r}; --nth {nth} is out of range")
    i = hits[nth]
    x0, y0, x1, y1 = i["rect"]
    ox, oy = F["origin"]
    return i, int(ox + (x0 + x1) / 2), int(oy + (y0 + y1) / 2)


def click_at(x, y, right=False, double=False):
    xdo("mousemove", str(x), str(y))
    time.sleep(0.15)                                       # hover first: a move and a click in one burst misses it
    xdo("click", *(["--repeat", "2", "--delay", "80"] if double else []), "3" if right else "1")
    time.sleep(0.25)


def main():
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("verb")
    ap.add_argument("args", nargs="*")
    ap.add_argument("--nth", type=int, default=0)
    ap.add_argument("--window")
    ap.add_argument("--right", action="store_true")
    ap.add_argument("--double", action="store_true")
    ap.add_argument("--secs", type=float, default=20)
    ap.add_argument("--data-dir")
    a, rest = ap.parse_known_args()
    V, A = a.verb, a.args
    if V == "start":
        start(a.data_dir)
    elif V == "stop":
        stop()
    elif V == "shot":
        subprocess.run(["import", "-window", "root", A[0]], env=env(), check=True)
    elif V == "items":
        F = frame()
        t = (A[0] if A else "").lower()
        for i in F["items"]:
            L = visible(i["label"])
            if L.strip() and t in L.lower():
                print(f"{L!r:40} {[round(v) for v in i['rect']]}  in {i['window'][-50:]!r}")
    elif V in ("click", "move"):
        _, x, y = find(A[0], a.nth, a.window)
        if V == "move":
            xdo("mousemove", str(x), str(y))
        else:
            click_at(x, y, a.right, a.double)
    elif V == "at":
        click_at(int(A[0]), int(A[1]), a.right, a.double)
    elif V == "drag":
        _, x, y = find(A[0], a.nth, a.window)
        xdo("mousemove", str(x), str(y)); time.sleep(0.15)
        xdo("mousedown", "1"); time.sleep(0.15)
        #ImGui frames render on input only: a drag source starts on one held frame and a drop target sees the
        #hover on later ones, so a single jump drops nothing. Move in steps, then settle over the target.
        tx, ty = int(A[1]), int(A[2])
        n = max(12, int(math.hypot(tx - x, ty - y) // 25))            # ~25 px a step (one jump drops nothing)
        for i in range(1, n + 1):
            xdo("mousemove", str(x + (tx - x) * i // n), str(y + (ty - y) * i // n)); time.sleep(0.06)
        time.sleep(0.3)
        xdo("mouseup", "1")
    elif V == "type":
        xdo("type", "--clearmodifiers", "--delay", "60", "--", A[0])  # slower types swap case otherwise
    elif V == "key":
        xdo("key", "--clearmodifiers", *A)
    elif V == "wait":
        end = time.time() + a.secs
        while time.time() < end:
            if os.path.exists(PROBE) and find(A[0], a.nth, a.window, quiet=True):
                return
            time.sleep(0.3)
        sys.exit(f"no canvas item said {A[0]!r} within {a.secs:g}s")
    elif V == "qt":
        S = state()
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "a11ydrive.py"), *A, *rest, "--pid", str(S.get("app"))],
                       env=env())
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
