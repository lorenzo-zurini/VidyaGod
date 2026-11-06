"""The fixture library the CLI sweep, the screenshot battery and the performance budgets run on: a data dir with a
package shaped like Age of Empires II (the one every help example names — its versions, a graft, an option) and a
runner, networking off, and the files the examples name. Made inside one work folder; HOME points inside it too.

  F = fixture.Fixture(vg, work); F.make(); ... F.restore()   # F.data, F.home, F.files, F.env
"""
import json, os, shutil, subprocess, zipfile

PKG = 'VidyaGod/[749][v1.0] Age of Empires II'


class Fixture:
    def __init__(self, vg, work):
        self.vg = vg
        self.work = os.path.abspath(work)
        self.data = f'{self.work}/data'
        self.home = f'{self.work}/home'
        self.files = f'{self.work}/files'        # the working directory: the files the examples name
        self.pristine = f'{self.work}/pristine'  # the fixture as made: restore() puts it back
        self.env = dict(os.environ, HOME=self.home, DISPLAY='', WAYLAND_DISPLAY='', QT_QPA_PLATFORM='offscreen', NO_COLOR='1',
                        XDG_CONFIG_HOME=f'{self.home}/.config', XDG_DATA_HOME=f'{self.home}/.local/share',
                        XDG_CACHE_HOME=f'{self.home}/.cache')

    @staticmethod
    def node(path, j):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'w') as f: json.dump(j, f, indent=1)

    @staticmethod
    def zipped(path, files):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with zipfile.ZipFile(path, 'w', zipfile.ZIP_STORED) as z:
            for n, t in files.items(): z.writestr(n, t)

    def library(self, lib):
        """A package with AoE2's shape and names (its versions, a graft, an option), small enough to make in a blink."""
        node, zipped = self.node, self.zipped
        p = f'{lib}/{PKG}'
        zipped(f'{p}/aok.zip', {'empires2.exe': 'MZ', 'language.dll': 'MZ', 'data/graphics.drs': 'x' * 64})
        zipped(f'{p}/conq.zip', {'age2_x1/age2_x1.exe': 'MZ'})
        zipped(f'{p}/nocd.zip', {'empires2.exe': 'MZ!'})
        zipped(f'{p}/userpatch.zip', {'age2_x1/age2_x1.exe': 'MZUP'})
        zipped(f'{p}/fe.zip', {'Games/Forgotten Empires/fe.dll': 'MZ'})
        tile = {'UID': '749', 'TITLE': 'Age of Empires II - The Age of Kings',
                'META': {'DEVELOPER': 'Ensemble Studios', 'PUBLISHER': 'Microsoft Studios', 'RELEASEDATE': '1999-09-30',
                         'SERIES': 'Age of Empires', 'LANMULTIPLAYER': 'Yes'}}
        play = lambda exe, t=None: {'EXEC': [dict({'LABEL': 'Play', 'HOST': 'win32', 'EXE': exe}, **({'TILE': t} if t else {}))]}
        node(f'{p}/kings.json', {'CID': 'kings', 'LABEL': 'The Age of Kings', 'LAYERS': [{'ZIP': 'aok.zip', 'TARGET': 'FILES/%GameDir%'}]})
        node(f'{p}/soundtrack.json', {'CID': 'soundtrack', 'LABEL': 'Soundtrack', 'LAYERS': [{'VARS': {'aoe2_skip_intro': {
            'DEFAULT': '1', 'UI': {'CONTROL': 'bool', 'LABEL': 'Skip intro videos'}}}}]})
        node(f'{p}/patch20a.json', {'CID': 'patch20a', 'LABEL': 'Patch 2.0a', 'VARIANT': 'Patch 2.0a (No-CD)', 'RECOMMENDED': ['749'], 'LAYERS': [
            {'NODE': 'kings'}, {'NODE': 'soundtrack'}, {'ZIP': 'nocd.zip', 'TARGET': 'FILES/%GameDir%'},
            {'REG': {'HKLM': {'Software': {'Microsoft': {'Microsoft Games': {'Age of Empires': {'2.0': {'EXE Path': '%GameDir%'}}}}}}}, 'ARCH': ['32']},
            play('%GameDir%/empires2.exe', tile)]})
        node(f'{p}/patch10c.json', {'CID': 'patch10c', 'LABEL': 'Patch 1.0c', 'LAYERS': [{'NODE': 'kings'}]})
        node(f'{p}/conq.json', {'CID': 'conq', 'LABEL': 'The Conquerors', 'LAYERS': [{'NODE': 'patch10c'}, {'ZIP': 'conq.zip', 'TARGET': 'FILES/%GameDir%'}]})
        node(f'{p}/patch10e.json', {'CID': 'patch10e', 'LABEL': 'Patch 1.0e (Unofficial)', 'VARIANT': 'Patch 1.0e', 'LAYERS': [
            {'NODE': 'conq'}, play('%GameDir%/age2_x1/age2_x1.exe', dict(tile, UID='13006', TITLE='Age of Empires II - The Conquerors'))]})
        node(f'{p}/userpatch.json', {'CID': 'userpatch', 'LABEL': 'UserPatch 1.5', 'LAYERS': [
            {'ANY': ['patch10e']}, {'ZIP': 'userpatch.zip', 'TARGET': 'FILES/%GameDir%'}]})
        node(f'{p}/fe22.json', {'CID': 'fe22', 'LABEL': 'Forgotten Empires 2.2', 'VARIANT': 'Version 2.2', 'LAYERS': [
            {'ANY': ['patch10e']}, {'ZIP': 'fe.zip', 'TARGET': 'FILES/%GameDir%'}]})
        # a runner for the Windows versions (its program need not exist: plans resolve, nothing runs)
        node(f'{lib}/VidyaGodRunners/fixture-wine/wine.json', {'CID': 'fixture-wine', 'LABEL': 'fixture_wine', 'LAYERS': [{'EXEC': [
            {'LABEL': 'run', 'HOST': 'linux64', 'GUEST': ['win32', 'win64'], 'EXE': '/usr/bin/true'}]}]})
        os.makedirs(f'{lib}/VidyaGodLibraries', exist_ok=True)   # the library a library package goes in

    def make(self):
        if os.path.exists(self.work): shutil.rmtree(self.work)
        for d in (self.data, self.home, self.files): os.makedirs(d)
        self.library(f'{self.data}/LIBRARY')
        subprocess.run([self.vg, '--data-dir', self.data, 'util', 'bytes', '1'], env=self.env, capture_output=True, timeout=120)   # its config
        cfg_path = f'{self.data}/GlobalConfig.JSON'
        cfg = json.load(open(cfg_path))
        cfg.setdefault('Settings', {}).setdefault('IPFS', {})['Enabled'] = False   # never online
        json.dump(cfg, open(cfg_path, 'w'), indent=1)
        shutil.copytree(f'{self.data}/LIBRARY', f'{self.pristine}/LIBRARY')
        shutil.copy(cfg_path, f'{self.pristine}/GlobalConfig.JSON')
        self.example_files()

    def example_files(self):
        F, zipped = self.files, self.zipped
        w = lambda p, t: (os.makedirs(os.path.dirname(f'{F}/{p}') or F, exist_ok=True), open(f'{F}/{p}', 'w').write(t))
        w('game.ini', '[Video]\nWidth=800\nHeight=600\n')
        w('settings.json', '{"video":{"width":800}}')
        w('cfg.xml', '<Config><Video w="800" h="600"/></Config>')
        open(f'{F}/game.exe', 'wb').write(b'MZ' + bytes(4094))
        w('nocd.json', json.dumps({'MODE': 'Replace', 'OFFSET': '0x2', 'EXPECT': '00 00', 'REPLACE': '00 00'}))
        w('reg.json', json.dumps([{'REGPATH': 'HKCU\\Software\\X', 'KEYVALUES': {'Res': '%res%'}}]))
        w('tonic.reg', 'Windows Registry Editor Version 5.00\n\n[HKEY_LOCAL_MACHINE\\Software\\Tonic]\n"Path"="C:\\\\Games"\n')
        w('ldconfig.txt', '\tlibvulkan.so.1 (libc6,x86-64) => /usr/lib/libvulkan.so.1\n')
        w('node.json', json.dumps({'LABEL': 'Sweep node', 'LAYERS': []}))
        w('x.json', json.dumps({'LABEL': 'x'}))
        w('names.txt', 'a\n')
        zipped(f'{F}/game.zip', {'data/readme.txt': 'hello', 'game.exe': 'MZ'})
        zipped(f'{F}/a.zip', {'f.txt': 'one two three' * 100})
        zipped(f'{F}/b.zip', {'f.txt': 'one two four' * 100})
        hive = 'WINE REGISTRY Version 2\n;; All keys relative to \\\\Machine\n\n#arch=win32\n\n[Software\\\\Microsoft\\\\Microsoft Games] 1700000000\n"x"="1"\n'
        for p in ('pfx', 'before/pfx', 'after/pfx'):
            w(f'{p}/system.reg', hive)
            w(f'{p}/user.reg', 'WINE REGISTRY Version 2\n;; All keys relative to \\\\User\\\\S-1-5-21-0-0-0-1000\n\n#arch=win64\n\n[Software\\\\X] 1700000000\n"Res"="800x600"\n')
        open(f'{F}/after/pfx/user.reg', 'a').write('\n[Software\\\\Y] 1700000001\n"New"="1"\n')
        for p in ('lower/drive_c/Games/AoE2/language.ini', 'lower/drive_c/Games/AoE2/empires2.exe', 'lower/drive_c/Games/AoE2/intro.avi',
                  'lower/a/old.ini', 'lower/a/run.sh', 'lower/x/y.bin'): w(p, 'x\n')
        for d in ('wl', 'mnt', 'Games/AoE2', 'new', 'tmp'): os.makedirs(f'{F}/{d}', exist_ok=True)
        w('spec.json', json.dumps({'mountpoint': f'{F}/mnt', 'uid': os.getuid(), 'gid': os.getgid(), 'readonly': False,
                                   'writelayer': f'{F}/wl', 'layers': [{'type': 'dir', 'source': f'{F}/lower', 'target': '',
                                                                        'submounts': [], 'rw': False}]}))
        open(f'{F}/cover.png', 'wb').write(b'')

    def restore(self):
        """The fixture as made (an example may rename, remove or rewrite it)."""
        shutil.rmtree(f'{self.data}/LIBRARY', ignore_errors=True)
        shutil.copytree(f'{self.pristine}/LIBRARY', f'{self.data}/LIBRARY')
        shutil.copy(f'{self.pristine}/GlobalConfig.JSON', f'{self.data}/GlobalConfig.JSON')

    def start_app(self, log):
        """VidyaGod off-screen on the fixture's data dir, answering commands; None if it does not come up."""
        import signal, time
        p = subprocess.Popen([self.vg, '--data-dir', self.data], cwd=self.files, env=self.env, stdout=open(log, 'w'),
                             stderr=subprocess.STDOUT, start_new_session=True)
        Until = time.time() + 300   # a real library's first start indexes it (minutes on a busy disk)
        while time.time() < Until:
            if p.poll() is not None: return None
            try:
                r = subprocess.run([self.vg, '--data-dir', self.data, '--no-wait', 'util', 'bytes', '1'], env=self.env,
                                   capture_output=True, timeout=30)
                if r.returncode == 0 and os.path.exists(f'{self.data}/vidyagod.sock'): return p
            except subprocess.TimeoutExpired:
                pass   # the app is up but busy: ask again
            time.sleep(0.5)
        os.killpg(p.pid, signal.SIGKILL)
        return None

    @staticmethod
    def stop_app(p):
        import signal
        os.killpg(p.pid, signal.SIGTERM)
        try: p.wait(timeout=20)
        except subprocess.TimeoutExpired: os.killpg(p.pid, signal.SIGKILL)


def load(here):
    """This module, for a script run with python3 -I (which keeps the script's folder off sys.path)."""
    import importlib.util
    spec = importlib.util.spec_from_file_location('vg_fixture', os.path.join(here, 'fixture.py'))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m
