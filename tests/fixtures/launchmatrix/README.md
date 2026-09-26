# Launch matrix

A synthetic package that exercises the launch engine, and a golden plan for every launchable in it.

## Build

The harness runs the real binaries, so build them first (Linux; the matrix mounts and launches, so it is not
built or registered on Windows):

```sh
cmake -S . -B build && cmake --build build -j"$(nproc)"     # VidyaGod + vg_make_delta
```

## Run

```sh
python3 tools/launch_matrix.py            # verify — non-zero exit if any plan moved
python3 tools/launch_matrix.py --update   # re-record, then READ THE DIFF before committing it
python3 tools/launch_matrix.py --keep     # keep the generated data dir to poke at it
ctest --test-dir build -R launch_matrix
```

Nothing here depends on the machine's real library, the network, or anything installed. `make_fixture.py`
generates the whole thing — content zips included — into a throwaway data dir on every run, so a difference is
always the engine's doing.

## The deal

**Touch the launch engine, run this.** A diff is not a failure, it is the review: it names precisely which
plans a change moved and how. If the diff is what you meant, `--update` and commit it alongside the change, so
the golden always says what the engine does *today*.

**Add a feature, add it here.** A feature with no entry in the matrix is a feature nothing will notice the
loss of. Put it in `make_fixture.py`, `--update`, and the golden diff shows exactly what it touched.

## What the fixture contains

One bundle (generation 6: one node per file, parents are `NODE` layers), these variants:

| Variant | Purpose |
|---|---|
| `lm_minimal` | The least that can launch: one zip, one entry. The control — a regression that shows up *here* is in the spine, not in a feature. |
| `lm_all` | Everything below, on the native runner (plan only: it contains the un-hydrated branch). |
| `lm_chained` | The same content reached through a two-hop runner chain (`fixture32` → `linux64`). |
| `lm_run` | The runtime case: mounts for real and runs the probe. The grafts `ANY`-ing it mount above it when ticked. |
| `lm_run_chained` | `lm_run`'s resolution through the two-hop chain, at runtime (the exec-time environment order). |
| `lm_inherits` | A variant containing `lm_run` with content and **no entry**: runs `lm_run`'s entry (folded), and — `lm_run` being in its resolution — is offered and pre-ticked `lm_run`'s grafts too. |
| `lm_blocked` | A variant whose `NOT` names something it contains: the launch is **refused**, with the reason (no golden; the harness expects the refusal). |
| `lm_run@lm_graft_entry` | `lm_run` run through `--entrypoint lm_graft_entry`: a ticked graft that carries an entry (a mod loader) runs over the variant's mount + grafts. |

Covered, by type:

- **Content** — `ZIP`, `DIR`, `FILE`, `DELTA`; a patch zip overlapping the base at the same target;
  `SUBMOUNTS` relocating one file out of an archive; a delta over the nearest content beneath it at its target,
  opaque over the dir beneath that; content present only as a `SOURCE` CID (un-hydrated).
- **`NODE` + `TAKE`/`TARGET`** — a library contained selectively and placed (`lm_uses_lib`): a file out of a zip,
  renamed; a directory's contents (`FILES/doc/`) landing at the target; a loose `FILE`, renamed; a variable taken,
  one not, a `DLL` not taken at all. A diamond (`lm_shared` in two containers) mounts once.
- **`VARS`** — `DEFAULT`; `text`/`enum`/`bool` UI; a default **referencing another variable** (a fixpoint, so
  order must not matter); a declaration gated by `WHEN` (it gates the value).
- **`EDIT`** — `ConfigWrite`, `Overwrite`, `AppendLine`; `Replace` (with an `EXPECT` guard), `Poke`, `Cave`
  (`CAVE: auto`); a relative target (relative to where the node landed); a layer gated by `WHEN` (phase-1 rule);
  an edit of a KEEP'd file — a package **default**, applied while the user has no saved copy (`IF_UNSAVED`).
- **`REG`** — both architecture views; a key's **default value** (the empty name) and a key-only entry; a gated layer.
- **`DLL`** — several load orders, plus an empty one.
- **`KEEP`** — a dir, a file, a machine-local dir (`CLOUD: false`), a whole hive and a single key.
- **`EXEC`** — game entries (`Play`, folded with the tile's partial entry) and runner entries (`GUEST`,
  `GUEST_ROOTS`, `CONTENT_ROOT`); `ENV` beneath, on and above the variant (null removes), a two-hop chain.
- **Tiles** — `TILE` (`UID`, `TITLE`, `META`) on the `Play` entry every variant folds.
- **Grafts** — pre-ticked (`RECOMMENDED`), offered-not-ticked, a graft on a graft (offered once its base is
  applied), one whose `ANY` names another variant, one containing a library, one carrying an entry, and one whose
  `NOT` hits the row (pre-ticked, never applied).

## Two layers of golden

**Plans** (`golden/<node>.json`) — the resolved `ContainerParams` for every launchable: closure, variables,
runner chain, persistence, the ordered layer list.

**Runtime** (`golden/*.runtime.txt`) — `lm_run` (and the other `RUN_CASES`) is launched *for real*. The mount is composed, the edits
and patches are applied, and a probe process runs inside it and reports what it can actually see: the merged
tree, which layer won a contested path, the contents of every edited file, the patched bytes of a PE, the
reconstructed delta output, and whether a `KEEP` dir is writable. A plan can be perfectly correct and still
mount to the wrong thing; this is the layer that notices.

The delta is **real** — generated by `vg_make_delta` (built from the same `vgdelta` the mount uses, and
verified to reconstruct before it is written).
The PE that `BinaryPatch` edits is a real minimal PE32 built by `make_fixture.py`, so `Replace`, `Poke` and
`Cave` all apply and the resulting bytes — jmp rel32 at the site, payload + displaced original + jump back in
the cave — are in the golden.

## What it does NOT cover — read this before trusting a pass

- **Wine is not exercised.** The runners are native, so `PrefixRoot` is empty, `DLLOverrides` resolves to `[]`
  despite four `DllOverride` layers being present, and prefix generation and registry application are never
  reached. Everything wine-only is plan-level at best.
- **One machine, one platform.** Nothing here covers the Windows port, a second machine, or the network.

## Known engine behaviour the goldens currently encode

These are recorded as they are **today**, not as they should be — when one is fixed, the matrix will diff, and
that diff is the proof the fix landed.

- `DLLOverrides` resolves empty for a native runner despite four DLL overrides being present.

## Traps this fixture found (they are why the fixture is shaped the way it is)

- **`%PrefixRoot%/x` is relative on wine and ABSOLUTE natively.** `PrefixRoot` is `pfx` under a prefix runner
  and `""` under a native one, so the same string becomes `/drive_c/...`. Packages now write guest coordinates
  (`C:/…`) and the runner's `GUEST_ROOTS` lays them out; the edit pass re-anchors a root-relative path.
- **A base-pass edit cannot see content.** Gen 5's base pass wrote into DEFAULTDATA before the mount, so a
  `ConfigWrite` of a zip's file there produced a one-line stub shadowing it. Every file `EDIT` now runs after the
  mount (the matrix's `config.ini` caught the regression when the pass was dropped).
- **A `FILE` layer's `TARGET` is the containing DIRECTORY**; the file appears at `TARGET/<its name>`.
  Naming the file in `TARGET` makes a directory of that name.
- **`SUBMOUNTS` relocate a FILE out of the archive** — they do not mount a nested archive — and declaring any
  submount REPLACES the layer's whole-archive mount, so only the listed files appear.
- **A `Cave`'s `EXPECT` must be at least 5 bytes** — the site is overwritten with a jmp rel32.
- **A delta's opacity cannot be tested with a zip below it.** A base zip at the delta's target is folded into
  the chain as a byte input and never becomes a layer, so there is nothing for opacity to mask. It takes a
  `dir` layer (not a byte view, not folded) to observe it — beneath the base zip, since the delta's base is the
  nearest content beneath it.
