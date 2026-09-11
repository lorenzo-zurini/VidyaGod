# VidyaGod

**A peer-to-peer game-preservation launcher, and the reference implementation of the
[MetaPackageFormat](MetaPackageFormat).**

> ### Keep old software runnable forever — and make the knowledge of *how* to run it something you can ship.
>
> A game stops working and the fix already exists somewhere: a forum post from 2009, a wiki table, a DLL someone
> remembers to drop in. **That knowledge is more fragile than the bytes.** VidyaGod turns it into data — every fix is a
> declarative node applied copy-on-write at launch over **unmodified pristine bytes**, so the original files stay
> exactly as they were pressed and everything that makes them run travels as a few kilobytes of JSON.
> **[Why this matters →](#why-this-exists)**

## Build

<details open><summary><b>Arch / Manjaro</b></summary>

```bash
sudo pacman -S --needed base-devel git cmake ninja go qt6-base qt6-svg libzip nlohmann-json fuse3
git clone --recursive https://github.com/lorenzo-zurini/VidyaGod.git && cd VidyaGod
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build && ./build/VidyaGod
```
</details>

<details><summary><b>Debian / Ubuntu</b> (needs libzip ≥ 1.10 — 22.04 ships 1.7)</summary>

```bash
sudo apt install -y build-essential g++-12 cmake ninja-build golang git pkg-config \
  qt6-base-dev qt6-tools-dev libqt6opengl6-dev libgl1-mesa-dev libxkbcommon-dev \
  nlohmann-json3-dev fuse3 libfuse3-dev zlib1g-dev libbz2-dev liblzma-dev libzstd-dev
# 22.04 only: newer libzip into /usr/local
sudo apt remove -y libzip-dev; wget -q https://libzip.org/download/libzip-1.10.1.tar.gz && tar xf libzip-1.10.1.tar.gz
cmake -S libzip-1.10.1 -B lz -DCMAKE_BUILD_TYPE=Release && cmake --build lz -j"$(nproc)" && sudo cmake --install lz && sudo ldconfig

git clone --recursive https://github.com/lorenzo-zurini/VidyaGod.git && cd VidyaGod
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build && ./build/VidyaGod
```
</details>

<details><summary><b>Windows</b> (MSYS2 / MinGW-w64 + WinFsp)</summary>

Install [MSYS2](https://www.msys2.org) and [WinFsp](https://winfsp.dev) (tick the *Developer* feature), then in the
**MINGW64** shell:

```bash
pacman -S --needed git mingw-w64-x86_64-{toolchain,cmake,ninja,pkgconf,qt6-base,qt6-tools,libzip,zstd,nlohmann-json,go}
export GOROOT=/mingw64/lib/go                       # MSYS2's Go is trimmed and needs this
git clone --recursive https://github.com/lorenzo-zurini/VidyaGod.git && cd VidyaGod
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DWINFSP_ROOT="C:/Program Files (x86)/WinFsp"
cmake --build build && ./build/VidyaGod.exe
```
</details>

No macOS build (the runtime needs FUSE3 or WinFsp). Tests: `ctest --test-dir build`. Requirements, CMake options and
the one-shot update command are under [Building — details](#building--details).

---

> **The format is specified separately.** The node-graph package format is defined, implementation-agnostically, in the
> **[MetaPackageFormat](MetaPackageFormat)** spec — a submodule of this repo (`MetaPackageFormat/docs/00`–`19`).
> This README documents the *implementation*. **When the code and the spec disagree, the spec is the source of truth.**

---

A package is not an installer and there is no setup step. A package is a **graph of nodes** — small JSON files, each of
which *is* one typed layer (content, a registry edit, a config patch, a byte patch, a persistence rule, a variable, a
declaration of what to run). To run a game, VidyaGod resolves a node's dependency closure, mounts its content
**zero-copy** through a custom FUSE filesystem, derives a runner chain for its platform (Proton/Wine for Windows,
emulators for consoles, native for Linux), and launches it — fetching any missing content on demand over an **embedded
IPFS node**. So **download = installed**: no extraction, no second copy on disk, a multi-gigabyte game "installs" by
mounting one zip.

## The model in one screen

- **Everything is a node, and a node is one layer.** `NODE_ID` + `TYPE` + `PARENTS` + that type's payload, hoisted onto
  the node itself. There is no `MANIFEST.json`, no `ROLE` field and no nested `LAYERS` array — a node that used to carry
  twenty layers is twenty nodes in a chain, and the order those layers had is the `PARENTS` edges between them.
- **Identity is the TYPE.** `Content` is files; `RegEdit`/`FileEdit`/`BinaryPatch`/`DllOverride` transform them;
  `Persist` and `CustomVar` declare policy; `DeclareExec` says what to run (**no `GUEST` ⇒ it is launchable; with
  `GUEST` ⇒ it is a runner providing those platforms** — one type, not two); `DeclareLibraryItem` is the library tile
  and the **parent** of the launchables it groups; `Group` is pure composition with no payload.
- **Composition and order are the same edge.** `PARENTS` is a *dependency* edge, and a parent is applied *before* its
  child — so a child wins over its parents in the overlay, and the launchable, being the terminal node of its chain,
  wins over everything. Mods, optional DLC, variant grouping, shared content and runner builds are all just nodes
  pointing at nodes.
- **Sibling order is unspecified.** Two parents of the same node are *not* ordered with respect to each other. If two
  nodes write the same thing and it matters which goes last, make one a parent of the other; that is the only way to
  say it. (The implementation is deterministic anyway — post-order DFS in list order — but a package may not rely on it.)
- **A node is on or off.** `TOGGLE: "on" | "off"` — the whole of what used to be `OPTIONAL` + `DEFAULT`, which were
  never independent. Absent means "not user-toggleable at all"; `"on"` means "toggleable, starts on".
- **Content is addressed, not installed.** Each `Content` node with `FORM: "zip"` is a **STORE (uncompressed) zip**
  named by its IPFS `CID`; the runtime mounts it by offset, zero-copy. Fetching the CID *is* the install. `FORM:
  "delta"` ships a version as a random-access binary diff of the one below it, so 900 versions of a game are one chain.

For the full, normative model — types, resolution, runner chaining, persistence, variables, content-addressing,
validation, conformance — read the spec in `MetaPackageFormat/docs/`.

---

## Why this exists

The files survive in a dozen archives; the recipe for making them run dies with the thread. A registry key, a
resolution patch, an emulator flag, the one DLL that has to be native — none of that is *stored* anywhere durable, and
none of it is *testable* once the person who knew it moves on.

So VidyaGod turns the recipe into **data**. Every fix — a mounted file, a registry write, a config edit, a byte patch over
an executable, a DLL resolution order, a compatibility toggle — is one declarative node in a graph, applied
copy-on-write at launch over **unmodified pristine bytes**. Nothing is installed. Nothing is patched in place. The
original files stay exactly as they were pressed, and everything that makes them run travels as a few kilobytes of JSON
next to them.

That single decision buys the rest:

- **Preservation that is checkable.** Content is addressed by hash, so "the game" and "the fix" are separable and each
  is verifiable forever. A package is a *claim about bytes* you can prove.
- **Distribution with nothing in the middle.** Every payload has a content address and every catalogue is a folder
  hash. No storefront, no account, no server, no company that can switch it off. A popular package is served by
  everyone who has it.
- **Any platform, derived rather than declared.** A package states only what its content *is* and what it *needs* —
  never how to get there. A SNES ROM finds its native emulator in one hop; a console whose *only* emulator is a Windows
  program resolves `[emulator → Proton → native]` on a Linux box without anyone authoring that route. Add an ARM runner
  tomorrow and ARM hosts light up with no package changes.
- **Scale that makes history cheap.** Every Minecraft version ever released — 903 of them, 2009 to 2026 — ships as
  **one 3.7 GB delta chain, 86% smaller than the jars**, and any single version mounts on demand in `O(log n)` reads.
  Keeping the whole history costs less than keeping a tenth of it.
- **Multiplayer after the lights went out.** Dead lobby servers don't end a game. A host-less virtual LAN over the DHT
  put a real Age of Empires II match across the open internet, and raced Wipeout XL between two machines — using each
  game's own netcode, with nothing to tell it the year had changed.

Games are not preserved by being stored. They are preserved by **still starting**.

---

## Architecture

### Node graph & catalog
Nodes are discovered by scanning **library roots** under `~/.VidyaGod/LIBRARY/`: one directory per configured
**package source**, each source being an immutable IPFS folder CID listed in `Settings.PackageSources[]`, plus any
locally-added bundles. (There is no git anywhere in the distribution path — a source is a CID, and updating one means
pointing at a new CID.) `ManifestModel::BuildNodeIndex` / `PackageCatalog::BuildCatalogIndex` build one flat,
cross-bundle index keyed by `NODE_ID`; `LinkGames` groups launchable *variants* under their `DeclareLibraryItem` *tile*
by the `PARENTS` edge (no `GAME` string). `LaunchResolver::InitializeFromNode` resolves a launch node's closure,
composes its exec/metadata field-by-field along the chain, and populates a `ContainerParams`.

`src/nodelower.{h,cpp}` is the schema front-end and the only place the on-disk format meets the engine: it expands one
flat node into the ordered layer sequence the executor consumes. It is **total** — a malformed or type-confused payload
becomes a named refusal, never an exception, because a node file is untrusted input (it arrives from a peer) and the
index is built during startup.

### vidyagodfs — the runtime filesystem
The whole runtime is assembled as **one FUSE mount** by the custom [`vidyagodfs`](VidyaGodFS) helper (built
alongside the app): a writable copy-on-write top layer over read-only `zip`/`dir`/`file`/`delta` under-layers rooted at
their `TARGET`, plus RW passthroughs for persisted state. STORE zip entries are served zero-copy by `pread` at their
offset — no decompression, no scratch copy, any size. It replaces the old unionfs-fuse + fuse-zip + bindfs stack;
`--watch-pid` auto-unmounts if the app dies.

### Runner chaining
A launchable declares only the `HOST` platform its content needs; the runtime *derives* how to run it.
`ResolveRunnerChain` does a shortest-path BFS over `GUEST→HOST` runner edges from the content platform to the machine,
always terminated by a native runner — so a Windows game resolves `[proton, native]`, and a console with only a Windows
emulator resolves `[emu, proton, native]` automatically. Runners are ordinary `DeclareExec` nodes that happen to declare
`GUEST` platforms; their build comes from their `PARENTS`.

### Persistence
Pristine-by-default. A `Persist` node declares `KEEP[]`/`DROP[]` target arrays (purely additive); the runtime keeps only
what is named. Runner keep-sets fold in the standard save/config locations (Proton/Wine user dirs + `HKCU`) so saves
survive with zero per-game work — collected from **every runner in the resolved chain's closure**, not from the runner
node itself. `KEEP %RuntimePath%` persists the whole runtime.

### Embedded IPFS
Content distribution runs in-process via **[libvgipfs](VidyaGodIPFS)** — a Boxo (Go) node built into the app
(no external Kubo). It fetches content-addressed layers write-through (download = installed), seeds/reprovides what you
hold to the DHT, and drives the GUI's IPFS tab. Networking is opt-in (off by default; enable in Settings → IPFS).

### GUI
Built around **`AppModel`**, a state/signal hub; every tab (Library, Catalog, Settings, IPFS, Friends) is its own
`QWidget` talking to the model via signals. **Friends** backs the host-less virtual LAN: peers you have exchanged codes
with appear as machines on one flat LAN, so a game's own network multiplayer works over the DHT overlay with no lobby
server and nothing for the game to know about.

### Package Editor — a blueprint canvas
The editor is a **Dear ImGui + imnodes node graph**, in the spirit of Unreal's blueprints, and it edits *the same
document the model persists*: a wire dragged on screen **is** a `PARENTS` entry on disk. There is no second
representation to keep in sync, and the JSON view sits beside the canvas showing the same bytes.

- Every editable row is **declared** per `TYPE` in `src/pkggraph.cpp`, not hand-built — so a field with N legal values
  is a combo box rather than a free-text line edit, and adding a field to the format adds it to the editor.
- **Actions live on the nodes** as contextual buttons: `zip ↔ dir`, `→ delta` / `undelta`, `re-store` (offered only when
  a zip is actually DEFLATE-compressed and therefore unmountable), `import .reg`, `capture setup`, `test launch`.
  The long ones run off the GUI thread with a progress bar, a cancel button, and the node locked read-only while it runs.
- **Destructive actions are guarded**: a conversion refuses a name a sibling layer already owns, refuses a `PATH` that
  does not resolve *inside* the bundle, and never deletes a source until its replacement exists and verifies.
- Canvas layout lives in a sidecar (`<bundle>/LAYOUT.vglayout`), never in the node files — a package is
  content-addressed, so storing positions there would mean dragging a box republishes the package for every peer.

`src/pkgcanvas.cpp` contains no GL and no `QWidget`, which is what makes the whole canvas testable headlessly: the test
suite drives real clicks and real keystrokes through ImGui and finds widgets *by effect* rather than by pixel.

### Authoring Session
A held-open, platform-agnostic runtime workbench: it mounts a node's content overlay and lets you run tools against it —
*Run a Windows program in a wine/proton prefix* (picked per-invocation), native runs, file drops — then captures the
write-delta into new `Content` and `RegEdit` **nodes, parented at the point in the chain you captured from**. Because a
node is only one layer, a capture never has to be spliced into an existing node.

### Validation
`--validate-nodes` (and the editor's *Check Package Validity*) enforce the spec's rules over the whole graph or one
bundle: dangling/cyclic `PARENTS`, unknown `TYPE`/`FORM`, STORE-zip-only, case-exact launch paths, cross-layer case
collisions, a tile's required `UID`, and **at most one reachable tile per launchable** — a launchable that reaches two
`DeclareLibraryItem` ancestors would otherwise be bound to whichever came first in an array, which the format says
carries no meaning. `--audit-packages` is the loud counterpart: it resolves *every* launchable in the library and reports
what would fail at launch.

### Run modes
`Normal` (installed, `~/.VidyaGod`), `Portable`, `In-package` (a bundle is a self-contained runnable unit), and `CLI`
(headless). The mode determines the data dir and what's allowed (daemon tray, start-on-login).

---

## Building — details

The [Build](#build) section at the top is the whole thing for a fresh machine. What it does not say:

**Requirements.** A C++23 compiler (gcc ≥ 12 — `std::atomic<std::shared_ptr>`), CMake ≥ 3.16, Qt6
(Core/Widgets/Network/OpenGL/Test), `nlohmann_json`, `libzip` ≥ 1.10 (`zip_source_zip_file`, used by the case-conflict
resolver), FUSE3 *or* WinFsp, and Go (for the embedded IPFS node). VidyaGodFS and VidyaGodIPFS are submodules built by
the parent CMake; Dear ImGui and imnodes are vendored under `third_party/` and need no package.

Already cloned without `--recursive`? `git submodule update --init --recursive`.

### One-shot update-and-build

Idempotent command to set up a fresh machine **and** to keep it current — clones on first run, pulls on every run,
re-syncs submodules, and rebuilds only what changed. Paste it any time (e.g. on a friend's PC) to get the latest build:

```bash
mkdir -p ~/Code && cd ~/Code && { [ -d VidyaGod/.git ] && (cd VidyaGod && git pull) || git clone https://github.com/lorenzo-zurini/VidyaGod.git; } && cd ~/Code/VidyaGod && git submodule update --init --recursive && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

Then run `~/Code/VidyaGod/build/VidyaGod`. (Arch prerequisites: `sudo pacman -S --needed base-devel git cmake ninja go qt6-base qt6-svg libzip nlohmann-json fuse3`.)

CMake options: `-DVIDYAGOD_WERROR=ON` (CI: warnings-as-errors + clang-tidy), `-DVIDYAGOD_SANITIZE=ON` (ASan/UBSan),
`-DWINFSP_ROOT=<dir>` (Windows). Tests: `ctest --test-dir build` (headless Qt Test suite, offscreen QPA — one
executable per subsystem plus a Qt-free engine suite). The `MetaPackageFormat` submodule is docs-only and is **not**
built.

`.github/workflows/build.yml` is the authority on what a from-scratch build needs — it is the only thing that proves
one works, on both platforms, from nothing. Keep the [Build](#build) block in step with it.

---

## Usage

### GUI
`./build/VidyaGod` — **Library** (installed games), **Catalog** (everything the configured sources offer),
**Settings** (package sources, runners, networking, paths), **IPFS** (transfers/peers), **Friends** (the virtual LAN).
Add a source CID, download a game, launch it. Author or import your own via the Library toolbar (**Package Editor** /
**Add Local Package**); locally-added packages are badged **LOCAL** until published.

### CLI (headless)

**Running and inspecting**

| Flag | Effect |
|------|--------|
| `--node <id>` | Fully resolve and launch a node from the catalog. |
| `--resolve-only <id>` | Resolve the container and dump it to JSON (no launch). |
| `--var KEY=VALUE` | Override a `CustomVar` for this run. |
| `--module <id>=on\|off` | Toggle a `TOGGLE` node for this run. |
| `--list-nodes` | List the library's game tiles and their variants. |
| `--validate-nodes [scope]` | Validate the whole graph, or one bundle/UID. Static; the pre-publish check. |
| `--audit-packages` | Resolve **every** launchable and report what would fail at launch. |
| `--tray` | Start minimized to the system tray. |
| `--bypass-single-instance-lock` | Run a read-only/CLI check while the GUI holds the lock. |

**Content & distribution**

| Flag | Effect |
|------|--------|
| `--import-package <uid>` / `--import-runner <id>` | Fetch a package's / runner's content closure over IPFS. |
| `--publish [--publish-to <dir>]` | Dehydrate a bundle: seed its content + covers, write CIDs into the node files. |
| `--publish-meta <dir>` | Mint a text-only Meta-CID for a bundle or a whole collection. |
| `--remint-library <dir>` | Re-seed and re-mint a library tree. **Lazy** — strip a layer's `SOURCE.CID` first to pick up changed bytes, and re-derive every CID afterwards to prove nothing drifted. |
| `--seed <dir>` / `--seed-covers` | Re-establish seeding from a publisher's master. |
| `--fetch <cid>` / `--fetch-dir <cid> <dir>` | Fetch a CID (used to verify a mint round-trips). |
| `--verify-cid` / `--pin-ls` / `--unpin` / `--drop-ref` / `--heal` | Content-store inspection and repair. |
| `--convert-delta-chain <dir>` | Convert a chain of full archives into `.vgdelta` layers. |
| `--fix-case-conflicts` | Canonicalise cross-layer case collisions to the base layer's case. |

**Paths & networking**

| Flag | Effect |
|------|--------|
| `--data-dir` / `--package-dir` / `--runtime-dir` / `--userdata-dir` | Path overrides (isolation, portable, in-package). |
| `--peer-id` / `--connect` / `--net-test` / `--lan` | Node identity, manual dial, connectivity check, virtual LAN. |
| `--friend-add` / `--friend-ls` / `--friend-code` / `--friend-nick` | The friend graph backing the host-less virtual LAN. |

```bash
./build/VidyaGod --node aoe2_tc                       # launch a game by node id
./build/VidyaGod --resolve-only aoe2_tc               # inspect the resolved container
./build/VidyaGod --validate-nodes --bypass-single-instance-lock
./build/VidyaGod --audit-packages                     # sweep every launchable in the library
```

---

## Repository layout

```
src/                 The application (C++/Qt6). Launch engine split across launchresolver / vfsmount / registrylayer /
                     persistlayer / fileedits / launchsources / runnerinstall; nodelower is the schema front-end;
                     GUI around AppModel + per-tab widgets; the package editor is pkgcanvas / pkggraph / pkgactions.
tests/               Qt-free engine tests (vg_tests) + headless Qt Test suites (one per GUI subsystem).
third_party/         Vendored Dear ImGui + imnodes (the blueprint canvas). Not a submodule; no package needed.
tools/               Operational scripts — the schema migrations, GUI drive harnesses (AT-SPI), the E2E battery.
VidyaGodFS/          submodule — the vidyagodfs FUSE filesystem (zip/dir/file/delta overlay + COW).
VidyaGodIPFS/        submodule — libvgipfs, the embedded Boxo IPFS node.
MetaPackageFormat/   submodule — the package-format specification (docs only; the source of truth for the format).
```

Content lives in **package sources**: immutable IPFS folder CIDs configured in Settings and mirrored into
`~/.VidyaGod/LIBRARY/<source>/`. A published package is dehydrated (node files + CIDs, no bytes) and hydrates in place
on download.

---

## Development

- **Every build runs on two machines** (a local box + a remote Arch box) with `-Werror` + `ctest` before a change is
  considered done.
- **Every change tests the flow it adds**, and a test is not finished until *mutating the code it covers makes it fail*.
  Pinning a predicate proves nothing about its call sites; a unit test that sets its own input by hand proves nothing
  about the path that produces that input. Both have shipped silent data loss here behind a green suite.
- **Every commit is reviewed adversarially before it is pushed** (`.claude/agents/adversary.md`).
- Commit straight to `main`. Keep the [MetaPackageFormat](MetaPackageFormat) spec in sync whenever the format
  changes — the spec is normative, this implementation follows it.
