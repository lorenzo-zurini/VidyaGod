#!/usr/bin/env python3
"""Hold every #include in src/ to the layering: a file may include its own layer's headers and the layers before it.

  check_layers.py [--src DIR] [--list]

The layers, lowest first, are the folders of src/ (the same list as VG_LAYERS in CMakeLists.txt):
  base     logging, paths, text and path helpers, threads, the platform seam
  format   the package format: nodes, the index, fold, lowering, validation, CIDs — pure
  net      the IPFS node and the fetch queue
  plan     resolving a launch from the index (the runner chain, variables, persistence, instances)
  library  the catalog: sources, the library index, hydration, publish, covers
  engine   running a plan: mounts, sandbox, registry, persistence, the launch session
  app      models and services with state (AppModel and the rest): no widgets
  editor   the package editor
  ui       the windows, tabs and pages
  cli      the command line, the command server's commands, main()
A violation is a file including a header of a later layer. Exit 1 on any; --list prints each (file:line → header).
"""
import argparse, os, re, sys

LAYERS = ['base', 'format', 'net', 'plan', 'library', 'engine', 'app', 'editor', 'ui', 'cli']
ap = argparse.ArgumentParser()
ap.add_argument('--src', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'src'))
ap.add_argument('--list', action='store_true')
a = ap.parse_args()
SRC = os.path.abspath(a.src)

def layer(path):
    rel = os.path.relpath(path, SRC).split(os.sep)
    return rel[0] if rel[0] in LAYERS else None

files = [os.path.join(d, f) for d, _, fs in os.walk(SRC) for f in fs if f.endswith(('.cpp', '.h', '.inc'))]
search = [SRC, os.path.join(SRC, 'cli')] + [os.path.join(SRC, l) for l in LAYERS]
INC = re.compile(r'^\s*#\s*include\s+"([^"]+)"')
bad = []
for f in sorted(files):
    mine = layer(f)
    if mine is None: continue
    for n, line in enumerate(open(f, errors='replace'), 1):
        m = INC.match(line)
        if not m: continue
        for d in [os.path.dirname(f)] + search:
            t = os.path.normpath(os.path.join(d, m.group(1)))
            if os.path.isfile(t): break
        else:
            continue                                   # not ours (Qt, third party, VidyaGodFS)
        theirs = layer(t)
        if theirs and LAYERS.index(theirs) > LAYERS.index(mine):
            bad.append((os.path.relpath(f, SRC), n, mine, os.path.relpath(t, SRC), theirs))

if a.list or bad:
    for f, n, mine, t, theirs in bad: print(f'{f}:{n}  ({mine}) includes {t} ({theirs})')
print(f'{len(bad)} include(s) reach up a layer' if bad else 'every include reaches its own layer or one below')
sys.exit(1 if bad else 0)
