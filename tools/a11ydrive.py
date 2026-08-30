#!/usr/bin/env python3
"""Drive the VidyaGod GUI BY WIDGET NAME over AT-SPI — no screen coordinates, no XCB shim.

This is the good way to drive the LIVE app: it talks to Qt's accessibility tree, so it works on native
Wayland, survives the window moving, and can see MODAL dialogs (which the headless Qt Test harness cannot
answer — QInputDialog::exec() will not yield to a QTimer under the offscreen platform).

  tools/a11ydrive.py tree  [--pid N]                 dump the widget tree (role + name)
  tools/a11ydrive.py click <name> [--pid N]          activate the control whose name matches
  tools/a11ydrive.py settext <name> <text> [--pid N] set an editable field's text
  tools/a11ydrive.py wait  <name> [--secs 20]        block until a control/dialog with that name exists
  tools/a11ydrive.py text  <name>                    print a control's text (read a dialog's message back)
  tools/a11ydrive.py state <name>                    checked/enabled/visible/focused for a control
  tools/a11ydrive.py check <name> [on|off]           set a checkbox (no-op if already in that state)
  tools/a11ydrive.py select <name> <value>           pick a combo-box / list entry by its visible text

<name> is matched case-insensitively as a SUBSTRING, so "Upgrade" finds "Upgrade…".

PREREQUISITE — the bridge is LAZY. Qt only publishes its accessibility tree when the a11y bus says
accessibility is enabled; otherwise the app is simply absent from the bus and this prints "no VidyaGod".
Turn it on once per session (harmless, no screen reader starts):

    busctl --user set-property org.a11y.Bus /org/a11y/bus org.a11y.Status IsEnabled b true

⚠ Arch's qt6-base ships no separate accessiblebridge plugin dir; the bridge is built in and activates purely
off that property. If the app still does not appear, it was started BEFORE the property was set — restart it.

--pid disambiguates when several instances are running (the live GUI and a scratch --data-dir one).
"""
import sys, time, argparse

try:
    import pyatspi
except ImportError:
    sys.exit("pyatspi missing — install it with:  sudo pacman -S python-atspi")


def find_app(pid=None):
    desktop = pyatspi.Registry.getDesktop(0)
    hits = []
    for app in desktop:
        try:
            if app and app.name == "VidyaGod":
                if pid is None or app.get_process_id() == pid:
                    hits.append(app)
        except Exception:
            pass
    if not hits:
        sys.exit("no VidyaGod on the a11y bus — is accessibility enabled? (see this file's header)")
    if len(hits) > 1 and pid is None:
        sys.exit(f"{len(hits)} VidyaGod instances — disambiguate with --pid "
                 f"({', '.join(str(a.get_process_id()) for a in hits)})")
    return hits[0]


def walk(node, depth=0, max_depth=40):
    """Yield (depth, role, name) for the whole subtree."""
    try:
        role, name = node.getRoleName(), node.name
    except Exception:
        return
    yield depth, role, name
    if depth >= max_depth:
        return
    for child in node:
        if child is not None:
            yield from walk(child, depth + 1, max_depth)


def _actionable(node):
    try:
        return node.queryAction().nActions > 0
    except Exception:
        return False


def find(node, needle, want_action=False):
    """Best node whose name contains <needle> (case-insensitive).

    Ranked, because a bare substring match is not enough: the word "Settings" also appears inside a status
    LABEL, and clicking that raises "exposes no actions" instead of switching tabs. Exact-name and actionable
    candidates win over incidental prose.
    """
    needle_l = needle.lower()
    best = None
    best_rank = -1
    for child in _iter(node):
        try:
            name = child.name
        except Exception:
            continue
        if not name or needle_l not in name.lower():
            continue
        act = _actionable(child)
        if want_action and not act:
            continue
        rank = (2 if name.lower() == needle_l else 0) + (1 if act else 0)
        if rank > best_rank:
            best, best_rank = child, rank
    return best


def _iter(node, depth=0, max_depth=40):
    yield node
    if depth >= max_depth:
        return
    for child in node:
        if child is not None:
            yield from _iter(child, depth + 1, max_depth)


def do_click(node):
    """Activate a control the way a user would.

    A LIST ITEM is the awkward case: its only action is 'Toggle', which flips selection state without
    telling the owning view to switch pages — clicking "Sources" that way appeared to work and changed
    nothing. Selecting it through the PARENT's Selection interface is what actually raises the page.
    """
    if node.getRoleName() in ("list item", "table cell", "tree item"):
        # Qt separates SELECTION from CURRENT ITEM, and side panels switch pages on currentRowChanged. Selecting
        # alone therefore highlights the row and switches nothing (this cost a debugging round). grabFocus() sets
        # the current item, which is what actually raises the page; selection is kept as a fallback.
        done = []
        try:
            node.queryComponent().grabFocus(); done.append("GrabFocus")
        except Exception:
            pass
        try:
            idx = node.getIndexInParent()
            node.parent.querySelection().selectChild(idx); done.append(f"Select({idx})")
        except Exception:
            pass
        if done:
            return "+".join(done)
    action = node.queryAction()
    for i in range(action.nActions):
        if action.getName(i).lower() in ("click", "press", "activate", "toggle", "jump", "open"):
            action.doAction(i)
            return action.getName(i)
    if action.nActions:
        action.doAction(0)
        return action.getName(0)
    raise RuntimeError(f"{node.name!r} exposes no actions")


def main():
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("verb")
    ap.add_argument("args", nargs="*")
    ap.add_argument("--pid", type=int, default=None)
    ap.add_argument("--secs", type=int, default=20)
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__)
        return 0
    opts = ap.parse_args()
    app = find_app(opts.pid)

    if opts.verb == "tree":
        for depth, role, name in walk(app):
            if name or role in ("push button", "page tab", "dialog", "text"):
                print("  " * depth + f"[{role}] {name!r}")
        return 0

    if opts.verb == "wait":
        target = opts.args[0]
        deadline = time.time() + opts.secs
        while time.time() < deadline:
            node = find(app, target)
            if node is not None:
                print(f"present: [{node.getRoleName()}] {node.name!r}")
                return 0
            time.sleep(0.4)
        print(f"TIMEOUT waiting for {target!r}", file=sys.stderr)
        return 1

    node = find(app, opts.args[0], want_action=(opts.verb == "click"))
    if node is None:
        print(f"no control matching {opts.args[0]!r}", file=sys.stderr)
        return 1

    if opts.verb == "click":
        print(f"{do_click(node)} → [{node.getRoleName()}] {node.name!r}")
        return 0
    if opts.verb == "settext":
        node.queryEditableText().setTextContents(opts.args[1])
        print(f"set [{node.getRoleName()}] {node.name!r} = {opts.args[1]!r}")
        return 0
    if opts.verb == "state":
        st = [str(x) for x in node.getState().getStates()]
        import pyatspi as _p
        flags = {n: getattr(_p, "STATE_" + n.upper(), None) for n in
                 ("checked", "enabled", "sensitive", "visible", "showing", "focused", "selected")}
        have = node.getState()
        print("[%s] %r" % (node.getRoleName(), node.name))
        for n, f in flags.items():
            if f is not None:
                print("   %-10s %s" % (n, have.contains(f)))
        return 0

    if opts.verb == "check":
        import pyatspi as _p
        want = (opts.args[1].lower() in ("on", "true", "1", "yes")) if len(opts.args) > 1 else True
        is_on = node.getState().contains(_p.STATE_CHECKED)
        if is_on == want:
            print(f"already {'checked' if want else 'unchecked'}: {node.name!r}")
            return 0
        # A checkbox's action toggles; there is no "set to X", so only act when the state actually differs.
        print(f"{do_click(node)} → {node.name!r} now {'checked' if want else 'unchecked'}")
        return 0

    if opts.verb == "select":
        # Combo boxes and lists: pick the CHILD whose visible text matches, rather than sending arrow keys and
        # hoping. Falls back to the parent's Selection interface for list-style widgets.
        target = opts.args[1].lower()
        for child in _iter(node):
            try:
                if child.name and child.name.lower() == target and child is not node:
                    print(f"{do_click(child)} → {child.name!r}")
                    return 0
            except Exception:
                pass
        print(f"no entry {opts.args[1]!r} inside {node.name!r}", file=sys.stderr)
        return 1

    if opts.verb == "text":
        try:
            t = node.queryText()
            print(t.getText(0, t.characterCount))
        except Exception:
            print(node.name)
        return 0

    print(f"unknown verb {opts.verb!r}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
