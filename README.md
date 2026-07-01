# VidyaGod

**VidyaGod is a peer-to-peer game-preservation launcher, and the reference implementation of the
[MetaPackageFormat](MetaPackageFormat).**

A package is not an installer and there is no setup step. A package is a **graph of nodes** — small JSON files, each
declaring typed *layers* (content, edits, persistence, variables, identity). To run a game, VidyaGod resolves a node's
dependency closure, mounts its content **zero-copy** through a custom FUSE filesystem, derives a runner chain for its
platform (Proton/Wine for Windows, emulators for consoles, native for Linux), and launches it — fetching any missing
content on demand over an **embedded IPFS node**. So **download = installed**: no extraction, no second copy on disk, a
multi-gigabyte game "installs" by mounting one zip.

> **The format is specified separately.** The node-graph package format is defined, implementation-agnostically, in the
> **[MetaPackageFormat](MetaPackageFormat)** spec — a submodule of this repo (`MetaPackageFormat/docs/00`–`19`).
> This README documents the *implementation*. **When the code and the spec disagree, the spec is the source of truth.**

---

## The model in one screen

- **Everything is a node.** A node is one `<node_id>.json` file: `NODE_ID` + `PARENTS` + `LAYERS`. There is no
  `MANIFEST.json` and no `ROLE` field.
- **Identity emerges from layers.** A `DeclareExec` layer makes a node *launchable*; `DeclareLibraryItem` makes it a
  *library tile* (a game); `DeclareRunner` makes it a *runner*. A node with none is plain *content*. One node can carry
  all three — a whole single-edition game in one file.
- **Composition is one edge.** `PARENTS` is a *dependency* edge. Mods, optional DLC, variant grouping, shared content
  and runner builds are all just nodes pointing at nodes; overlay priority is the resolved closure order (later = higher,
  the launched node highest).
- **Content is addressed, not installed.** Each content layer is a **STORE (uncompressed) zip** named by its IPFS `CID`;
  the runtime mounts it by offset, zero-copy. Fetching the CID *is* the install.

For the full, normative model — layers, resolution, runner chaining, persistence, variables, content-addressing,
validation, conformance — read the spec in `MetaPackageFormat/docs/`.

---

## Architecture

### Node graph & catalog
Nodes are discovered by scanning **library roots**: git-cloned repositories under `~/.VidyaGod/LIBRARY/<repo>/` plus any
locally-added bundles. `ManifestModel::BuildNodeIndex` / `PackageCatalog::BuildCatalogIndex` build one flat, cross-bundle
index keyed by `NODE_ID`; `LinkGames` groups launchable *variants* under their `DeclareLibraryItem` *tile* by the
`PARENTS` edge (no `GAME` string). `LaunchResolver::InitializeFromNode` resolves a launch node's closure, composes its
`DeclareExec`/metadata field-by-field along the chain, and populates a `ContainerParams`.

### vidyagodfs — the runtime filesystem
The whole runtime is assembled as **one FUSE mount** by the custom [`vidyagodfs`](VidyaGodFS) helper (built
alongside the app): a writable copy-on-write top layer over read-only `zip`/`dir`/`file` under-layers rooted at their
`TARGET`, plus RW passthroughs for persisted state. STORE zip entries are served zero-copy by `pread` at their offset —
no decompression, no scratch copy, any size. It replaces the old unionfs-fuse + fuse-zip + bindfs stack; `--watch-pid`
auto-unmounts if the app dies.

### Runner chaining
A launchable declares only its content's `PLATFORM`; the runtime *derives* how to run it. `ResolveRunnerChain` does a
shortest-path BFS over `GUEST→HOST` runner edges from the content platform to the machine, always terminated by a native
runner — so a Windows game resolves `[proton, native]`, and a console with only a Windows emulator resolves
`[emu, proton, native]` automatically. Runners are pure-data nodes; their build comes from their `PARENTS`.

### Persistence
Pristine-by-default. A single `Persist` layer declares `KEEP`/`DROP` targets (purely additive); the runtime keeps only
what's named, and runner keep-sets fold in the standard save/config locations (Proton/Wine user dirs + `HKCU`) so saves
survive with zero per-game work. `KEEP %RuntimePath%` persists the whole runtime.

### Embedded IPFS
Content distribution runs in-process via **[libvgipfs](VidyaGodIPFS)** — a Boxo (Go) node built into the app
(no external Kubo). It fetches content-addressed layers write-through (download = installed), seeds/reprovides what you
hold to the DHT, and drives the GUI's IPFS tab. Networking is opt-in (off by default; enable in Settings → IPFS).

### GUI, Package Editor, Authoring
The GUI is built around **`AppModel`**, a state/signal hub; every tab (Library, Available, IPFS, Settings) is its own
`QWidget` talking to the model via signals. The **Package Editor** edits the node graph directly (per-node layer editors,
graph view, validation, raw JSON). The **Authoring Session** is a held-open, platform-agnostic runtime workbench: it
mounts a node's content overlay and lets you run tools against it — *Run a Windows program in a wine/proton prefix*
(picked per-invocation), native runs, file drops — then captures the write-delta into `VFSDirLayer` + `RegEdit` layers.

### Run modes
`Normal` (installed, `~/.VidyaGod`), `Portable`, `In-package` (a bundle is a self-contained runnable unit), and `CLI`
(headless). The mode determines the data dir and what's allowed (daemon tray, start-on-login).

---

## Building

Requires a C++20 compiler, CMake ≥ 3.16, Qt6 (Core/Widgets/Network/OpenGL/Test), `nlohmann_json`, `libzip`, FUSE3, and Go
(for the embedded IPFS node). The FUSE filesystem and IPFS node are submodules, built by the parent CMake.

```bash
git clone --recursive https://github.com/lorenzo-zurini/VidyaGod.git
# already cloned without --recursive?
git submodule update --init --recursive

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/VidyaGod
```

CMake options: `-DVIDYAGOD_WERROR=ON` (CI: warnings-as-errors + clang-tidy), `-DVIDYAGOD_SANITIZE=ON` (ASan/UBSan).
Tests: `ctest --test-dir build` (headless Qt Test suite, offscreen QPA — one executable per subsystem plus a Qt-free
engine suite). The `MetaPackageFormat` submodule is docs-only and is **not** built.

---

## Usage

### GUI
`./build/VidyaGod` — Library (installed games), Available (downloadable from repos), IPFS (transfers/peers), Settings
(repositories, runners, networking, paths). Add a repository, download a game, launch it. Author or import your own via
the Library toolbar (**Package Editor** / **Add Local Package**); locally-added packages are badged **LOCAL** until
published into a repo.

### CLI (headless)
| Flag | Effect |
|------|--------|
| `--node <id>` | Fully resolve and launch a node from the catalog. |
| `--resolve-only <id>` | Resolve the container and dump it to JSON (no launch). |
| `--validate-nodes` | Validate the whole node graph (dangling/cyclic `PARENTS`, layer paths, case, runner resolution). |
| `--list-nodes` | List the library's game tiles and their variants. |
| `--var KEY=VALUE` | Override a `CustomVar` for this run. |
| `--import-package <uid>` / `--import-runner <id>` | Fetch a package's / runner's content closure over IPFS. |
| `--publish[-to <dir>]` | Dehydrate a bundle: seed its content + covers over IPFS, write CIDs into the node files. |
| `--seed <dir>` / `--seed-covers` | Re-establish seeding from a publisher's master (e.g. `~/The Vidya`). |
| `--data-dir` / `--package-dir` / `--runtime-dir` / `--userdata-dir` | Path overrides (isolation, portable, in-package). |
| `--bypass-single-instance-lock` | Run a read-only/CLI check while the GUI holds the lock. |
| `--tray` | Start minimized to the system tray. |

```bash
./build/VidyaGod --node aoe2_tc                       # launch a game by node id
./build/VidyaGod --resolve-only aoe2_tc               # inspect the resolved container
./build/VidyaGod --validate-nodes --bypass-single-instance-lock
```

---

## Repository layout

```
src/                 The application (C++/Qt6). Engine split across launchresolver / vfsmount / registrylayer /
                     persistlayer / fileedits / launchsources / runnerinstall; GUI around AppModel + per-tab widgets.
tests/               Qt-free engine tests (vg_tests) + headless Qt Test suites (one per GUI subsystem).
VidyaGodFS/          submodule — the vidyagodfs FUSE filesystem (zip/dir/file overlay + COW).
VidyaGodIPFS/        submodule — libvgipfs, the embedded Boxo IPFS node.
MetaPackageFormat/   submodule — the package-format specification (docs only; the source of truth for the format).
```

Content lives in separate repositories the app clones into `~/.VidyaGod/LIBRARY/` (e.g. `VidyaGodPackages`,
`VidyaGodRunners`); a published package is dehydrated (node files + CIDs, no bytes) and hydrates in place on download.

---

## Development

- **Every build runs on two machines** (a local box + a remote Arch box) with `-Werror` + `ctest` before a change is
  considered done.
- Commit straight to `main`. Keep the [MetaPackageFormat](MetaPackageFormat) spec in sync whenever the format
  changes — the spec is normative, this implementation follows it.
