"""Predict SetupAoC's age2_x1.exe for a -f flag string from single-feature installs (tools/userpatch/runr.sh).

  UP_WORK=<dir> python3 -c "import model; open('x.exe','wb').write(model.predict('0000000011111'))"

Needs out/rz.exe (-f all 0), out/r<k>.exe (only feature k = 1), out/q1.exe (only position 1 = 2: widescreen),
out/r24.exe (only 24 = 1: classic left-aligned), out/v24_1.exe (1 = 1 and 24 = 1). Position 1 = widescreen command bar
(with the core patches it brings), 24 = left-aligned; 0x293241 holds one bit per sync feature (OR), 0x293744 is water
animation's rate unless lower quality environment is on. Verified exact against 30 random installer runs.
Files and registry are separate: this is the executable only."""
import os
D=os.path.join(os.environ['UP_WORK'], 'out') + '/'   # runr.sh outputs: rz (all off), r<k> (feature k), q1, r24, v24_1
Z=open(D+'rz.exe','rb').read()
def diff(p):
    X=open(D+p,'rb').read(); return {i:X[i] for i in range(len(Z)) if Z[i]!=X[i]}
FLAGS=0x293241; WATER=0x293744
STYLE={'centered':{}, 'widescreen':diff('q1.exe'), 'left':diff('r24.exe'), 'left+core':diff('v24_1.exe')}
EXE_FEATURES=[k for k in list(range(9,14))+list(range(21,44)) if k not in (24,37) and os.path.exists(D+f'r{k}.exe')]
F={k:diff(f'r{k}.exe') for k in EXE_FEATURES}
BITS={k:F[k].pop(FLAGS) for k in list(F) if FLAGS in F[k]}
WATER_ON=F[9].pop(WATER)
def style_of(f):
    core = f[0] in '12'
    if f[23]=='1': return 'left+core' if core else 'left'
    return 'widescreen' if core else 'centered'
def predict(f):
    f=f.ljust(46,'0')
    out=bytearray(Z)
    for i,v in STYLE[style_of(f)].items(): out[i]=v
    for k in EXE_FEATURES:
        if f[k-1]=='1':
            for i,v in F[k].items(): out[i]=v
    b=0
    for k,bit in BITS.items():
        if f[k-1]=='1': b|=bit
    out[FLAGS]=b
    if f[8]=='1' and f[26]!='1': out[WATER]=WATER_ON
    return bytes(out)
