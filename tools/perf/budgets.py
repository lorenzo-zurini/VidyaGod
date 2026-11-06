#!/usr/bin/env python3
"""Start-up, call latency and memory against budgets — warn-only: it reports, it does not fail (yet).

  budgets.py --vg BIN --work DIR [--data-dir DATA]

On the fixture library (tools/cli_sweep/fixture.py) by default, or on a copy of a real data dir (--data-dir; the
real library's budgets are the ones in the README's numbers, the fixture's are what a small library should cost):
  * time from starting the app until it answers commands,
  * a headless call that reads the index (median of 5) and one that does not,
  * the app's memory once idle.
Prints each with its budget; a figure over budget is a WARN line. Exit 1 only if the app does not start.
"""
import argparse, importlib.util, os, statistics, subprocess, sys, time

ap = argparse.ArgumentParser()
ap.add_argument('--vg', required=True)
ap.add_argument('--work', required=True)
ap.add_argument('--data-dir', default='')
a = ap.parse_args()

spec = importlib.util.spec_from_file_location('vg_fixture', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'cli_sweep', 'fixture.py'))
fx = importlib.util.module_from_spec(spec); spec.loader.exec_module(fx)
F = fx.Fixture(a.vg, a.work)
Real = bool(a.data_dir)
if Real:
    F.data = os.path.abspath(a.data_dir)
    F.env = dict(os.environ, QT_QPA_PLATFORM='offscreen', DISPLAY='', WAYLAND_DISPLAY='', NO_COLOR='1')
    os.makedirs(F.files, exist_ok=True)
else:
    F.make()

# (fixture, real library): seconds, seconds, seconds, MB
BUDGET = {'socket': (3.0, 2.0), 'index_call': (0.5, 0.6), 'plain_call': (0.3, 0.3), 'idle_mb': (350, 700)}
def budget(k): return BUDGET[k][1 if Real else 0]

def timed(args):
    t = time.time()
    subprocess.run([a.vg, '--data-dir', F.data] + args, env=F.env, capture_output=True, timeout=300)
    return time.time() - t

warn = 0
def report(name, value, unit, k):
    global warn
    over = value > budget(k)
    warn += over
    print(f"{'WARN' if over else 'ok':4}  {name:44} {value:8.2f} {unit:3}  (budget {budget(k)} {unit})")

timed(['util', 'bytes', '1'])                       # the index cache warm, as on any later call
report('headless call reading the index (median of 5)', statistics.median(timed(['catalog', 'stats']) for _ in range(5)), 's', 'index_call')
report('headless call reading nothing (median of 5)', statistics.median(timed(['util', 'bytes', '1']) for _ in range(5)), 's', 'plain_call')
t = time.time()
app = F.start_app(f'{F.work}/app.log')
if not app: sys.exit('the off-screen app did not come up')
try:
    report('app start until it answers commands', time.time() - t, 's', 'socket')
    time.sleep(8)                                   # idle: covers decoded, the first refresh done
    rss = int(next(l for l in open(f'/proc/{app.pid}/status') if l.startswith('VmRSS:')).split()[1]) / 1024
    report('app memory once idle', rss, 'MB', 'idle_mb')
finally:
    F.stop_app(app)
print(f"{warn} over budget" if warn else "all within budget")
