#!/usr/bin/env python3
"""Run every example of every VidyaGod command against a small fixture library, and fail on what no command may do.

  cli_sweep.py --vg BIN --work DIR [--live] [--only GROUP] [--timeout S] [--report FILE]

The examples are the commands' own help (`VidyaGod commands --json`): each one runs headless on a fixture data dir
made here (a package shaped like Age of Empires II — the one every example names — and a runner), and with --live
once more through an off-screen app on that data dir. An example may fail as a command fails (exit 1-3: refused,
bad arguments, nothing there); the sweep fails only on a crash (a signal), a hang (past --timeout), or a raw
library error reaching the user ("[json.exception…", "terminate called…") — what this sweep found by hand before.

Nothing here leaves DIR: HOME points inside it (so ~/ and the default data dir are DIR's), networking is off in the
fixture's config (so no command goes online), and examples that run a game or program, mount a filesystem, or
change the machine outside the data dir are never run.
"""
import argparse, json, os, re, shlex, signal, subprocess, sys, time

ap = argparse.ArgumentParser()
ap.add_argument('--vg', required=True)
ap.add_argument('--work', required=True)
ap.add_argument('--live', action='store_true', help='also run every example through an off-screen app on the data dir')
ap.add_argument('--only', default='')
ap.add_argument('--timeout', type=int, default=60)
ap.add_argument('--report', default='')
a = ap.parse_args()

WORK = os.path.abspath(a.work)
PEER = '12D3KooWQs9BMz6DtSWeFM6x98vjspCTVFSwTKwGmGDwjjgjXtuA'
CID = 'QmRCLYSTark8MGpXMSEWjzawjFoxcNj82AbAHkgsyiBqo7'

# ---- the fixture (tools/cli_sweep/fixture.py) ----
import importlib.util
_spec = importlib.util.spec_from_file_location('vg_fixture', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'fixture.py'))
_fx = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(_fx)
FX = _fx.Fixture(a.vg, WORK)
fixtures, restore = FX.make, FX.restore
DATA, HOME, FIX, ENV = FX.data, FX.home, FX.files, FX.env

# ---- what an example becomes ----
SUBST = {
    '<node>': "'Patch 2.0a'", '<game>': "'Patch 2.0a'", '<graft>': "'UserPatch 1.5'",
    'bafkrei…,bafkrei…': 'patch20a,kings', 'bafkrei…': 'patch20a', 'bafybei…': CID, 'Qm…,Qm…': f'{CID},{CID}', 'Qm…': CID,
    '12D3KooW…': PEER, '12D3Koo…': PEER,
    # what the examples name of a real library, as the fixture has it
    'bafkreiank43ketmdy2jtdcnn5utluobf53avcuubisi3ajtdxrcnmyteka': 'patch20a', 'geproton_10_34': 'fixture_wine',
    '~/.VidyaGod/LIBRARY': f'{DATA}/LIBRARY', '~/.VidyaGod/TEMP/…/vidyagodfs.spec.json': f'{FIX}/spec.json', '~/.VidyaGod': DATA,
    '~/x/pfx': f'{FIX}/pfx', '~/after/pfx': f'{FIX}/after/pfx', '~/before/pfx': f'{FIX}/before/pfx', '~/x/new': f'{FIX}/new',
    "~/x/'[123][v1.0] Some Game'": shlex.quote(f'{FIX}/[123][v1.0] Some Game'),
    '~/x/game.zip': f'{FIX}/game.zip', '~/x/': f'{FIX}/', '~/Games': f'{FIX}/Games', '~/cover.png': f'{FIX}/cover.png',
    '~/Storage/tmp': f'{FIX}/tmp', '/tmp/': f'{FIX}/tmp/', '~/': f'{HOME}/',
}
_RX = re.compile('|'.join(re.escape(k) for k in sorted(SUBST, key=len, reverse=True)))
def transform(ex): return _RX.sub(lambda m: SUBST[m.group(0)], ex)   # one pass: a replacement is never replaced again

# Never run: it plays a game, runs a program, mounts a filesystem, goes online, or changes the machine outside DIR.
NEVER = {'launch run': 'runs a game', 'author run': 'runs a program under wine', 'author run-windows': 'runs an installer',
         'launch build': 'mounts a runtime', 'author start': 'mounts a runtime', 'vfs mount': 'mounts a filesystem',
         'ipfs start': 'brings the node online', 'app autostart': 'writes the desktop autostart entry'}
LIVE_NEVER = {'build uid': 'renames the package under the running app', 'app remove-package': 'removes the package under the running app',
              'app quit': 'ends the app the rest of the sweep runs in'}
# Waits by design (until a signal, or for good): run with a short bound of their own, so a wait is no hang.
BOUNDED = {'events wait': ('--timeout', '2'), 'events follow': ('--for', '2')}
def bounded(line):
    for k, (opt, v) in BOUNDED.items():
        if line.startswith(k): return re.sub(r'\s' + re.escape(opt) + r'\s+\S+', '', line) + f' {opt} {v}'
    return line
RAW = re.compile(r'\[json\.exception|terminate called|what\(\):|Segmentation fault|std::bad_|Assertion .* failed')

def run(argv, cwd, timeout):
    p = subprocess.Popen(argv, cwd=cwd, env=ENV, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
    try:
        so, se = p.communicate(timeout=timeout)
        return p.returncode, so, se
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        so, se = p.communicate()
        return None, so or '', se or ''

def sweep(cmds, live, out):
    bad = []
    for c in cmds:
        if a.only and not c['name'].startswith(a.only): continue
        for ex in c['examples']:
            why = next((w for k, w in NEVER.items() if ex.startswith(k)), None)
            if not why and live: why = next((w for k, w in LIVE_NEVER.items() if ex.startswith(k)), None)
            rec = {'mode': 'live' if live else 'headless', 'command': c['name'], 'example': ex}
            if why:
                rec['skipped'] = why; out.append(rec); continue
            line = bounded(transform(ex))
            try: argv = [a.vg, '--data-dir', DATA] + shlex.split(line)
            except ValueError as e:
                rec['error'] = f'not one command line: {e}'; bad.append(rec); out.append(rec); continue
            if not live: restore()
            t = time.time()
            rc, so, se = run(argv, FIX, a.timeout)
            rec.update({'line': line, 'rc': rc, 'secs': round(time.time() - t, 2), 'stderr': se[-1500:]})
            fault = 'hung (no answer in %d s)' % a.timeout if rc is None else \
                    'crashed (signal %d)' % -rc if rc < 0 else 'crashed (exit %d)' % rc if rc >= 128 else \
                    'a raw library error reached the user' if RAW.search(se) or RAW.search(so) else None
            if fault: rec['fault'] = fault; bad.append(rec)
            out.append(rec)
            print(f"{'FAULT' if fault else 'ok':>5} {str(rc):>4} {rec['secs']:6.1f}s  {rec['mode']:8} {line[:100]}", flush=True)
    return bad

def start_app(): return FX.start_app(f'{WORK}/app.log')

# A command after the app's own start options is a command: it was read as an old flag mode, the words after it dropped,
# and the GUI opened on the data dir (an hour-long "hang" in a script). Each line must answer as the command does.
ROUTED = [['--bypass-single-instance-lock', 'help', 'vfs'], ['--offline', 'help', 'vfs'],
          ['--bypass-single-instance-lock', 'commands']]
# Beside the app (--bypass-single-instance-lock) a command that changes the data dir is refused before it does anything:
# a source's folder removed while the config still names it is worse than nothing done.
REFUSED_BESIDE = [['app', 'card-size', '--set', '333'], ['sources', 'remove', 'Fixture'], ['app', 'save']]
def routing():
    bad = []
    for argv in REFUSED_BESIDE:
        cfg = open(f'{DATA}/GlobalConfig.JSON').read() if os.path.exists(f'{DATA}/GlobalConfig.JSON') else ''
        rc, so, se = run([a.vg, '--data-dir', DATA, '--bypass-single-instance-lock'] + argv, FIX, 20)
        after = open(f'{DATA}/GlobalConfig.JSON').read() if os.path.exists(f'{DATA}/GlobalConfig.JSON') else ''
        ok = rc not in (0, None) and 'does not run beside' in se and cfg == after
        print(f"{'ok' if ok else 'FAULT':>5} {str(rc):>4}          beside   {' '.join(argv)}", flush=True)
        if not ok: bad.append({'mode': 'beside', 'fault': 'a change ran beside the data dir\'s holder', 'line': ' '.join(argv), 'stderr': se[-1500:]})
    for argv in ROUTED:
        rc, so, se = run([a.vg, '--data-dir', DATA] + argv, FIX, 20)
        ok = rc == 0 and ('VidyaGod vfs' in so or argv[-1] == 'commands' and 'launch run' in so)
        print(f"{'ok' if ok else 'FAULT':>5} {str(rc):>4}          routing  {' '.join(argv)}", flush=True)
        if not ok: bad.append({'mode': 'routing', 'fault': 'not answered as a command', 'line': ' '.join(argv), 'stderr': se[-1500:]})
    return bad

def main():
    fixtures()
    cmds = json.loads(subprocess.run([a.vg, 'commands', '--json'], capture_output=True, text=True, env=ENV).stdout)
    out = []
    bad = routing() + sweep(cmds, False, out)
    if a.live:
        restore()
        app = start_app()
        if not app:
            print('the off-screen app did not come up (see app.log)'); bad.append({'mode': 'live', 'fault': 'the app did not start'})
        else:
            try: bad += sweep(cmds, True, out)
            finally: FX.stop_app(app)
    if a.report: json.dump(out, open(a.report, 'w'), indent=1)
    ran = [r for r in out if 'rc' in r]
    print(f"\n{len(ran)} examples run ({len(out) - len(ran)} skipped), {len(bad)} fault(s)")
    for r in bad: print(f"  {r.get('mode')}: {r.get('fault') or r.get('error')} — {r.get('line') or r.get('example', '')}\n    {r.get('stderr', '')[-300:]}")
    sys.exit(1 if bad else 0)

main()
