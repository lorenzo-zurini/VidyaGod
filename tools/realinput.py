#!/usr/bin/env python3
"""Inject REAL input events through the kernel (/dev/uinput).

Why this exists: on a Wayland session, `xdotool`'s XTEST events reach Xwayland but the compositor does not
route them to the focused X client, so synthetic clicks silently do nothing — the game sits in poll() waiting
for input that never arrives, and it looks exactly like a hung/unresponsive UI. Creating a virtual input
device makes the events indistinguishable from a physical mouse/keyboard: they enter through libinput, KWin
routes them normally, and the game gets them.

Requires write access to /dev/uinput (an ACL grants it to the seat's user on this machine).

Positioning uses an ABSOLUTE axis pair (the virtio-tablet trick) rather than relative deltas. Relative motion
goes through libinput's pointer acceleration, so a synthetic "move 3040 right" lands somewhere else entirely
and the click misses silently. An absolute device without INPUT_PROP_DIRECT is treated as a pointer mapped
linearly onto the whole logical desktop, with no acceleration — the coordinate you ask for is the one you get.

Coordinates are GLOBAL logical desktop coordinates (see `kscreen-doctor -o` for the layout); with two side by
side 1920x1080 outputs that is a 3840x1080 space, so the right-hand monitor starts at x=1920.

Usage:
  realinput.py move X Y            # absolute screen coords
  realinput.py click X Y [button]  # move, settle, click (button: left|right|middle)
  realinput.py key NAME [count]    # e.g. Return, Escape, space, a, F1
  realinput.py type TEXT
  realinput.py combo NAME...       # keys pressed together, released in reverse (e.g. alt Tab)
  realinput.py seq "click 100 200; sleep 0.5; type Bob; key Return"

`seq` runs several actions through ONE virtual device. Creating a device costs ~1s of settle time before the
compositor will route its events, so a dialog that only waits a few seconds (Wipeout XL's config dialog gives
up after ~10s and starts the game anyway) cannot be driven by separate invocations — use seq for those.
"""
import fcntl, os, struct, sys, time

UINPUT = "/dev/uinput"
EV_SYN, EV_KEY, EV_REL, EV_ABS = 0x00, 0x01, 0x02, 0x03
SYN_REPORT = 0
REL_X, REL_Y, REL_WHEEL = 0x00, 0x01, 0x08
BTN_LEFT, BTN_RIGHT, BTN_MIDDLE = 0x110, 0x111, 0x112

UI_DEV_CREATE = 0x5501          # _IO('U', 1)
UI_DEV_DESTROY = 0x5502         # _IO('U', 2)
UI_SET_EVBIT = 0x40045564       # _IOW('U', 100, int)
UI_SET_KEYBIT = 0x40045565      # _IOW('U', 101, int)
UI_SET_RELBIT = 0x40045566      # _IOW('U', 102, int)
UI_SET_ABSBIT = 0x40045567      # _IOW('U', 103, int)
ABS_X, ABS_Y = 0x00, 0x01
ABS_MAX = 32767                 # the range a virtual tablet conventionally reports

# The absolute device spans the whole logical desktop; discovered once so callers can pass screen pixels.
def desktop_size():
    import subprocess
    try:
        out = subprocess.run(["xdpyinfo"], capture_output=True, text=True, timeout=10).stdout
        for line in out.splitlines():
            if "dimensions:" in line:
                w, h = line.split()[1].split("x")
                return int(w), int(h)
    except Exception:
        pass
    return 3840, 1080

# Linux keycodes for the names we actually use when driving a game's menus.
KEYS = {
    "Escape": 1, "escape": 1, "esc": 1,
    "Return": 28, "return": 28, "enter": 28, "Enter": 28, "KP_Enter": 96,
    "space": 57, "Space": 57,
    "Tab": 15, "tab": 15, "BackSpace": 14, "Delete": 111,
    "Up": 103, "Down": 108, "Left": 105, "Right": 106,
    "Home": 102, "End": 107, "Prior": 104, "Next": 109,
    "shift": 42, "ctrl": 29, "alt": 56, "super": 125,
    "minus": 12, "equal": 13, "period": 52, "comma": 51, "slash": 53,
}
for i, c in enumerate("1234567890"):
    KEYS[c] = 2 + i
for name, code in zip("qwertyuiop", range(16, 26)):
    KEYS[name] = code
for name, code in zip("asdfghjkl", range(30, 39)):
    KEYS[name] = code
for name, code in zip("zxcvbnm", range(44, 51)):
    KEYS[name] = code
for i in range(1, 13):
    KEYS["F%d" % i] = (59 + i - 1) if i <= 10 else (87 + i - 11)

SHIFTED = {"!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8",
           "(": "9", ")": "0", "_": "minus", "+": "equal", ":": None, "?": "slash", "<": "comma",
           ">": "period"}


class Device:
    def __init__(self):
        self.fd = os.open(UINPUT, os.O_WRONLY | os.O_NONBLOCK)
        self.dw, self.dh = desktop_size()
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_KEY)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_REL)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_ABS)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_SYN)
        for b in (REL_X, REL_Y, REL_WHEEL):
            fcntl.ioctl(self.fd, UI_SET_RELBIT, b)
        for b in (ABS_X, ABS_Y):
            fcntl.ioctl(self.fd, UI_SET_ABSBIT, b)
        for b in (BTN_LEFT, BTN_RIGHT, BTN_MIDDLE):
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, b)
        for code in set(KEYS.values()):
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, code)
        # Legacy uinput_user_dev setup: name[80] + input_id(4x u16) + ff_effects_max + absmax/min/fuzz/flat.
        dev = struct.pack("80sHHHHi", b"vidyagod-virtual-input", 0x03, 0x1209, 0x0001, 1, 0)
        absmax = [0] * 64
        absmax[ABS_X] = absmax[ABS_Y] = ABS_MAX
        dev += struct.pack("64i", *absmax)          # absmax
        dev += struct.pack("64i", *([0] * 64))      # absmin
        dev += struct.pack("64i", *([0] * 64))      # absfuzz
        dev += struct.pack("64i", *([0] * 64))      # absflat
        os.write(self.fd, dev)
        fcntl.ioctl(self.fd, UI_DEV_CREATE)
        time.sleep(1.0)  # let udev/libinput/KWin notice the new device before we type into it

    def emit(self, etype, code, value):
        os.write(self.fd, struct.pack("llHHi", 0, 0, etype, code, value))

    def syn(self):
        self.emit(EV_SYN, SYN_REPORT, 0)

    def move_rel(self, dx, dy):
        # Step in small increments: a single huge delta can be swallowed by pointer acceleration.
        while dx or dy:
            sx = max(-100, min(100, dx))
            sy = max(-100, min(100, dy))
            if sx:
                self.emit(EV_REL, REL_X, sx)
            if sy:
                self.emit(EV_REL, REL_Y, sy)
            self.syn()
            dx -= sx
            dy -= sy
            time.sleep(0.002)

    def move_abs(self, x, y):
        ax = int(round(int(x) * ABS_MAX / float(self.dw - 1)))
        ay = int(round(int(y) * ABS_MAX / float(self.dh - 1)))
        # Nudge first: some compositors ignore the very first absolute report from a just-created device.
        self.emit(EV_ABS, ABS_X, max(0, ax - 1))
        self.emit(EV_ABS, ABS_Y, max(0, ay - 1))
        self.syn()
        time.sleep(0.08)
        self.emit(EV_ABS, ABS_X, ax)
        self.emit(EV_ABS, ABS_Y, ay)
        self.syn()
        time.sleep(0.2)

    def button(self, code, down):
        self.emit(EV_KEY, code, 1 if down else 0)
        self.syn()

    def click(self, code=BTN_LEFT):
        self.button(code, True)
        time.sleep(0.08)
        self.button(code, False)
        time.sleep(0.05)

    def tap(self, code):
        self.emit(EV_KEY, code, 1)
        self.syn()
        time.sleep(0.06)
        self.emit(EV_KEY, code, 0)
        self.syn()
        time.sleep(0.06)

    def close(self):
        try:
            fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        except OSError:
            pass
        os.close(self.fd)


def keycode(name):
    if name in KEYS:
        return KEYS[name]
    low = name.lower()
    if low in KEYS:
        return KEYS[low]
    raise SystemExit("unknown key: %s" % name)


def run_one(d, cmd, args):
    """Execute one action on an already-created device."""
    if cmd == "move":
        d.move_abs(int(args[0]), int(args[1]))
    elif cmd == "click":
        btn = {"left": BTN_LEFT, "right": BTN_RIGHT, "middle": BTN_MIDDLE}[
            args[2] if len(args) > 2 else "left"]
        if len(args) >= 2:
            d.move_abs(int(args[0]), int(args[1]))
            time.sleep(0.4)   # games poll the cursor; let it settle where it landed
        d.click(btn)
    elif cmd == "key":
        n = int(args[1]) if len(args) > 1 else 1
        for _ in range(n):
            d.tap(keycode(args[0]))
            time.sleep(0.12)
    elif cmd == "type":
        for ch in " ".join(args):
            if ch == " ":
                d.tap(KEYS["space"])
            elif ch.isupper() or ch in SHIFTED:
                base = SHIFTED.get(ch, ch.lower())
                if base is None:
                    continue
                d.emit(EV_KEY, KEYS["shift"], 1); d.syn()
                d.tap(keycode(base))
                d.emit(EV_KEY, KEYS["shift"], 0); d.syn()
            else:
                d.tap(keycode(ch))
            time.sleep(0.05)
    elif cmd == "combo":
        codes = [keycode(a) for a in args]
        for c in codes:
            d.emit(EV_KEY, c, 1); d.syn(); time.sleep(0.05)
        time.sleep(0.1)
        for c in reversed(codes):
            d.emit(EV_KEY, c, 0); d.syn(); time.sleep(0.05)
    elif cmd == "sleep":
        time.sleep(float(args[0]))
    else:
        raise SystemExit("unknown action: %s" % cmd)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == "seq":
        d = Device()
        try:
            for step in " ".join(args).split(";"):
                parts = step.split()
                if parts:
                    run_one(d, parts[0], parts[1:])
        finally:
            time.sleep(0.2)
            d.close()
        return
    d = Device()
    try:
        if cmd == "move":
            d.move_abs(int(args[0]), int(args[1]))
        elif cmd == "click":
            btn = {"left": BTN_LEFT, "right": BTN_RIGHT, "middle": BTN_MIDDLE}[
                args[2] if len(args) > 2 else "left"]
            if len(args) >= 2:
                d.move_abs(int(args[0]), int(args[1]))
                time.sleep(0.4)   # games poll the cursor; let it settle where it landed
            d.click(btn)
        elif cmd == "key":
            n = int(args[1]) if len(args) > 1 else 1
            for _ in range(n):
                d.tap(keycode(args[0]))
                time.sleep(0.12)
        elif cmd == "type":
            for ch in " ".join(args):
                if ch == " ":
                    d.tap(KEYS["space"])
                elif ch.isupper() or ch in SHIFTED:
                    base = SHIFTED.get(ch, ch.lower())
                    if base is None:
                        continue
                    d.emit(EV_KEY, KEYS["shift"], 1); d.syn()
                    d.tap(keycode(base))
                    d.emit(EV_KEY, KEYS["shift"], 0); d.syn()
                else:
                    d.tap(keycode(ch))
                time.sleep(0.05)
        elif cmd == "combo":
            codes = [keycode(a) for a in args]
            for c in codes:
                d.emit(EV_KEY, c, 1); d.syn(); time.sleep(0.05)
            time.sleep(0.1)
            for c in reversed(codes):
                d.emit(EV_KEY, c, 0); d.syn(); time.sleep(0.05)
        else:
            raise SystemExit(__doc__)
    finally:
        time.sleep(0.2)
        d.close()


if __name__ == "__main__":
    main()
