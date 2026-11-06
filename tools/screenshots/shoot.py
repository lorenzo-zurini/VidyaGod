#!/usr/bin/env python3
"""Shoot every tab, Settings page and window of VidyaGod off-screen, beside the previous run's shots.

  shoot.py --vg BIN --out DIR [--data-dir DATA]

Without --data-dir it runs on the fixture library (tools/cli_sweep/fixture.py) made in DIR/fixture; with it, on that
data dir (a copy of a real one — the app is started on it, so not one a running VidyaGod has open). Each run's PNGs go
to DIR/<date-time>/; DIR/index.html shows every shot beside the same shot of the run before, to flip through.
"""
import argparse, datetime, html, importlib.util, json, os, signal, subprocess, sys, time

ap = argparse.ArgumentParser()
ap.add_argument('--vg', required=True)
ap.add_argument('--out', required=True)
ap.add_argument('--data-dir', default='')
a = ap.parse_args()
OUT = os.path.abspath(a.out)
os.makedirs(OUT, exist_ok=True)

spec = importlib.util.spec_from_file_location('vg_fixture', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'cli_sweep', 'fixture.py'))
fx = importlib.util.module_from_spec(spec); spec.loader.exec_module(fx)
F = fx.Fixture(a.vg, f'{OUT}/fixture')
if a.data_dir:
    F.data = os.path.abspath(a.data_dir)
    F.env = dict(os.environ, QT_QPA_PLATFORM='offscreen', DISPLAY='', WAYLAND_DISPLAY='', NO_COLOR='1')
    os.makedirs(F.files, exist_ok=True)
else:
    F.make()

def vg(*args, timeout=120):
    try:
        r = subprocess.run([a.vg, '--data-dir', F.data] + list(args), env=F.env, capture_output=True, text=True, timeout=timeout)
        return r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return 124, '', 'no answer in %d s' % timeout

Run = datetime.datetime.now().strftime('%Y-%m-%d_%H%M%S')
Dir = f'{OUT}/{Run}'
os.makedirs(Dir)
app = F.start_app(f'{Dir}/app.log')
if not app: sys.exit('the off-screen app did not come up (see app.log)')
shots = []
def shoot(name, *control):
    rc, so, se = vg('ui', 'screenshot', f'{Dir}/{name}.png', *(['--control', control[0]] if control else []))
    if rc == 0: shots.append(name)
    else: print(f'{name}: {se.strip()[:200]}')
try:
    #A real library's app is busy for a while after it answers (covers, the first refresh): shots wait until it is not.
    for _ in range(60):
        t = time.time()
        if vg('ui', 'tab', 'Library')[0] == 0 and time.time() - t < 1.0: break
        time.sleep(2)
    for tab in ('Library', 'Catalog', 'IPFS', 'Network'):
        vg('ui', 'tab', tab); time.sleep(1.5); shoot(f'tab_{tab}')
    vg('ui', 'tab', 'Settings')
    rc, so, _ = vg('ui', 'show', 'Settings pages', '--json')
    pages = [r[0] if isinstance(r, list) else r for r in json.loads(so).get('view', {}).get('rows', [])] if rc == 0 else []
    for p in pages:
        vg('ui', 'set', 'Settings pages', p); time.sleep(3 if p in ('Installed Packages', 'Storage & Paths', 'Runners') else 1.5)
        shoot('settings_' + ''.join(c if c.isalnum() else '_' for c in p))
    vg('library', 'play', '749'); time.sleep(3); shoot('window_launch', 'PreLaunchWindow')
    vg('editor', 'open', '749'); time.sleep(4); shoot('window_editor', 'PackageEditor')
finally:
    F.stop_app(app)

# the index: this run beside the one before (the newest earlier folder)
runs = sorted(d for d in os.listdir(OUT) if d[:2] == '20' and os.path.isdir(f'{OUT}/{d}'))
prev = runs[-2] if len(runs) > 1 else None
rows = []
for s in shots:
    before = f'{prev}/{s}.png' if prev and os.path.exists(f'{OUT}/{prev}/{s}.png') else None
    rows.append(f'<h2>{html.escape(s)}</h2><div class=pair><figure><img src="{Run}/{s}.png"><figcaption>{Run}</figcaption></figure>'
                + (f'<figure><img src="{before}"><figcaption>{prev}</figcaption></figure>' if before else '<figure class=none>no earlier shot</figure>')
                + '</div>')
open(f'{OUT}/index.html', 'w').write(
    '<!doctype html><meta charset=utf-8><title>VidyaGod screenshots</title><style>body{background:#15191e;color:#d8dade;font:14px sans-serif;'
    'margin:16px}.pair{display:flex;gap:12px;flex-wrap:wrap}figure{margin:0;flex:1 1 600px}img{max-width:100%;border:1px solid #333}'
    'figcaption{color:#8f98a0}.none{color:#8f98a0;padding:40px}</style>'
    f'<h1>VidyaGod — {len(shots)} shots, {Run}' + (f' beside {prev}' if prev else '') + '</h1>' + ''.join(rows))
print(f'{len(shots)} shots in {Dir}; {OUT}/index.html')
