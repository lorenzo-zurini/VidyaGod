#!/usr/bin/env python3
"""Runs the launch-matrix fixture and diffs each resolved plan against its golden.

    tools/launch_matrix.py            # verify (non-zero exit on any difference)
    tools/launch_matrix.py --update   # re-record the goldens, then read the diff

The fixture is generated fresh into a throwaway data dir every run, so the result depends on the engine and on
tests/fixtures/launchmatrix/make_fixture.py — never on the machine's real library, the network, or what was
installed yesterday. A golden diff is the review: it names exactly which plans a change moved.

WHAT THIS COVERS: two layers. The PLAN goldens are node lowering, closure, variable resolution, runner
selection and chaining, persistence classification, and the ordered layer list a mount is built from. The
RUNTIME golden goes further and actually launches: the mount composes, edits and patches apply, and a probe
process runs inside it and reports what it can see. The deltas are REAL (generated and verified by
vg_make_delta) and the BinaryPatch target is a real PE32, so byte reconstruction and patching are exercised.

NOT covered: wine. The runners are native, so PrefixRoot is empty, DLLOverrides resolves to [] and neither
prefix generation nor registry application is reached.
"""
import argparse, json, os, re, shutil, subprocess, sys, tempfile

ROOT     = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIXTURE  = os.path.join(ROOT, "tests", "fixtures", "launchmatrix")
GOLDEN   = os.path.join(FIXTURE, "golden")
BINARY   = os.path.join(ROOT, "build", "VidyaGod")

#The runtime probe's report, delimited so the surrounding launch log never reaches the golden.
#Every launchable that is meant to RUN, not just resolve. lm_run_chained goes through a two-hop runner
#chain, where the environment is assembled in a different order — a property no plan can show.
RUN_NODES = ["lm_run", "lm_run_chained"]
RUN_BEGIN = "=== argv"
RUN_END   = "=== done"

#Values that are a property of the MACHINE or the run, not of the package. Left in, every golden would differ on
#every computer and the harness would be noise.
VOLATILE_VARS = ("VIDYAGOD_SELF_NAME", "VIDYAGOD_SELF_VIP", "VIDYAGOD_PEER_NAMES",
                 "VIDYAGOD_PEER_VIPS", "VIDYAGOD_SUBNET")

def launchables(bundle):
    with open(os.path.join(bundle, "launchmatrix.json")) as F:
        nodes = json.load(F)
    #A launchable is a DeclareExec with no GUEST — the terminal link of a chain. Derived, not listed, so a
    #launchable added to the fixture is covered without touching this script.
    return sorted(n["NODE_ID"] for n in nodes
                  if n.get("TYPE") == "DeclareExec" and not n.get("GUEST"))

def normalise(obj, data_dir):
    """Strip everything that is a property of WHERE this ran rather than WHAT was resolved."""
    subs = [(data_dir, "<DATA>"), (ROOT, "<REPO>"), (os.path.expanduser("~"), "<HOME>")]
    def walk(o):
        if isinstance(o, dict):
            out = {}
            for k, v in o.items():
                out[k] = "<volatile>" if k in VOLATILE_VARS else walk(v)
            return out
        if isinstance(o, list):
            return [walk(v) for v in o]
        if isinstance(o, str):
            s = o
            for frm, to in subs:
                if frm:
                    s = s.replace(frm, to)
            #Temp roots carry a per-run component even inside <DATA>.
            s = re.sub(r"/tmp/[^/\s\"]*", "<TMP>", s)
            return s
        return o
    return walk(obj)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--update", action="store_true", help="re-record the goldens instead of verifying")
    ap.add_argument("--keep", action="store_true", help="keep the generated data dir and print its path")
    #Passed by ctest as $<TARGET_FILE:...> so the harness always tests THIS build tree. The defaults are for
    #running it by hand from the repo root.
    ap.add_argument("--binary", default=BINARY, help="the VidyaGod binary to exercise")
    ap.add_argument("--make-delta", default=None, help="the vg_make_delta binary the fixture generates with")
    args = ap.parse_args()

    binary = os.path.abspath(args.binary)
    if not os.path.isfile(binary):
        print(f"build the app first: {binary} is missing", file=sys.stderr)
        return 2

    data = tempfile.mkdtemp(prefix="vglm", dir="/tmp")   # short: a unix socket path caps near 108 bytes
    try:
        env = dict(os.environ)
        if args.make_delta: env["VG_MAKE_DELTA"] = os.path.abspath(args.make_delta)
        gen = subprocess.run([sys.executable, os.path.join(FIXTURE, "make_fixture.py"), data],
                             capture_output=True, text=True, env=env)
        if gen.returncode != 0:
            print(gen.stderr, file=sys.stderr); return 2
        bundle = next(os.path.join(data, "fixture", d) for d in os.listdir(os.path.join(data, "fixture")))

        os.makedirs(GOLDEN, exist_ok=True)
        failures, checked = [], 0
        for node in launchables(bundle):
            #--bypass-single-instance-lock so the harness runs while the GUI is open; the lock is about one
            #app owning the real data dir, and this run owns a throwaway one.
            try:
                run = subprocess.run([binary, "--bypass-single-instance-lock", "--data-dir", data,
                                      "--resolve-only", node],
                                     capture_output=True, text=True, timeout=300)
            except subprocess.TimeoutExpired:
                #A hung resolve is a result, not a crash in the harness: report it the way every other failure
                #is reported instead of unwinding out of main() with a traceback.
                failures.append(f"{node}: resolve TIMED OUT after 300s")
                continue
            dump = os.path.join(data, f"vg_resolve_{node}.json")
            if run.returncode != 0 or not os.path.isfile(dump):
                failures.append(f"{node}: resolve FAILED (exit {run.returncode})")
                tail = [l for l in run.stdout.splitlines() if "[ERR" in l][-3:]
                failures += [f"    {l}" for l in tail]
                continue
            with open(dump) as F:
                got = normalise(json.load(F), data)
            text = json.dumps(got, indent=2, sort_keys=True) + "\n"
            gpath = os.path.join(GOLDEN, f"{node}.json")
            checked += 1
            if args.update:
                with open(gpath, "w") as F: F.write(text)
                continue
            if not os.path.isfile(gpath):
                failures.append(f"{node}: NO GOLDEN — run with --update and review the new file")
                continue
            with open(gpath) as F: want = F.read()
            if want != text:
                import difflib
                d = list(difflib.unified_diff(want.splitlines(True), text.splitlines(True),
                                              f"golden/{node}.json", "resolved", n=2))
                failures.append(f"{node}: PLAN CHANGED ({sum(1 for l in d if l.startswith(('+','-')) and not l.startswith(('+++','---')))} line(s))")
                failures += ["    " + l.rstrip("\n") for l in d[:40]]

        for node in RUN_NODES:
            # ---- the RUNTIME case: mount for real, run the probe, golden what it saw ------------------------
            # The plan goldens above stop at resolution. This one actually composes the mount, applies the edits
            # and patches, starts a process inside it, and records what that process could see — which is the only
            # way to catch a plan that is perfectly correct and mounts to the wrong thing.
            try:
                run = subprocess.run([binary, "--bypass-single-instance-lock", "--data-dir", data,
                                      "--node", node],
                                     capture_output=True, text=True, timeout=600)
            except subprocess.TimeoutExpired:
                failures.append(f"{node}: the run TIMED OUT after 600s — the probe never finished")
                run = None
            #The plan loop checks the exit code; this one did not, so a run that printed a good report and then
            #failed during teardown compared clean and the harness said "match".
            if run is not None and run.returncode != 0:
                failures.append(f"{node}: the run exited {run.returncode}")
                failures += ["    " + l for l in run.stdout.splitlines() if "[ERR" in l][-4:]
            report = []
            inside = False
            for line in (run.stdout.splitlines() if run else []):
                if line.startswith(RUN_BEGIN): inside = True
                if inside: report.append(line)
                if line.startswith(RUN_END):   inside = False
            rpath = os.path.join(GOLDEN, f"{node}.runtime.txt")
            if not report:
                failures.append(f"{node}: the probe produced NO report — it never ran inside the mount")
                failures += ["    " + l for l in (run.stdout.splitlines() if run else []) if "[ERR" in l][-4:]
            else:
                #INSIDE the else on purpose. Left outside, a run that produced no report fell through to the
                #recorder with `text` still holding the PREVIOUS iteration's value — so `--update` wrote the
                #last plan's JSON into the runtime golden and exited 0. On any machine where the mount or the
                #probe cannot run (no FUSE, no user namespaces, CI), re-recording poisoned the committed
                #artifact and said "recorded".
                text = "\n".join(normalise({"r": report}, data)["r"]) + "\n"
                if args.update:
                    with open(rpath, "w") as F: F.write(text)
                else:
                    checked += 1
                    want = open(rpath).read() if os.path.isfile(rpath) else None
                    if want is None:
                        failures.append(f"{node} runtime: NO GOLDEN — run with --update and review it")
                    elif want != text:
                        import difflib
                        d = list(difflib.unified_diff(want.splitlines(True), text.splitlines(True),
                                                      f"golden/{node}.runtime.txt", "observed", n=2))
                        failures.append(f"{node} runtime: WHAT THE GAME SEES CHANGED")
                        failures += ["    " + l.rstrip("\n") for l in d[:60]]

        if args.update:
            #A re-record that could not produce something is a FAILURE, not a quiet partial success. Reporting
            #"recorded" while a runtime node never ran is how a machine that cannot mount (no FUSE, no user
            #namespaces, CI) ends up committing goldens it never actually observed.
            if failures:
                print("\n".join(failures))
                print(f"\nNOT fully recorded — {os.path.relpath(GOLDEN, ROOT)} was left as it was for whatever "
                      f"could not be produced.")
                return 1
            print(f"recorded {checked} plan golden(s) + the runtime report(s) in {os.path.relpath(GOLDEN, ROOT)}")
            return 0
        if failures:
            print("\n".join(failures))
            print(f"\nlaunch matrix: {len(failures and [f for f in failures if not f.startswith('    ')])} "
                  f"of {checked} plan(s) differ")
            return 1
        print(f"launch matrix: {checked} plan(s) match golden")
        return 0
    finally:
        if args.keep: print(f"data dir kept: {data}")
        else:         shutil.rmtree(data, ignore_errors=True)

if __name__ == "__main__":
    sys.exit(main())
