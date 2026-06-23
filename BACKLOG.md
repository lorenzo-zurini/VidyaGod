# VidyaGod — Backlog (from the 2026-06-22 test pass)

Living list of what's queued. Done items kept for context. Test plan checklist is in `TESTPLAN.md`.

## In progress
- **Download throughput** — sequential fetch was RTT-bound ~4 MB/s; parallel/windowed/session attempts gave
  150 MB/s bursts but periodic "waiting for peers" stalls (every ~3-4 s). Being benchmarked + optimized headlessly
  on the laptop (`--import-package`). Goal: steady link-speed downloads.

## De-god PackageEditor — DONE (local `-Werror`; remote + GUI UNVERIFIED)
- 1647-LOC `QDialog` → thin 249-LOC composition root + a `PackageEditorModel` hub and per-concern widgets
  (`NodeGraphView`, `ValidationPanel`, `JsonRawEditor`, `NodeEditor`), all talking via the model's signals
  (documentReloaded / validationChanged / savedToDisk). Config taken by `const` throughout (ContainerWrapper ctor's
  GlobalConfigJSON param now `const&`). Phases: 1 model hub · 2 graph/validation/json widgets · 3 NodeEditor
  (per-node tab) · 4 shell slimming + include cleanup.
- `NodeEditor` further decomposed: each section is now its own class — `NodeSection` base + `NodeIdentitySection`/
  `NodeSelectionSection`/`NodeParentsSection`/`NodePlatformSection`/`NodeExecSection`/`NodeMetaSection`/
  `NodeLayersSection` (nodesection.{h,cpp} + nodesections.{h,cpp}); NodeEditor is a 218-LOC toolbar+composer.
- **Now broadly covered by tests** (headless Qt Test, offscreen — see the GUI-harness memory): one exe per subsystem
  via the CMake `add_gui_test()` helper, all green in ctest. Coverage:
  - `vg_tests` (pure, Qt-free): manifestmodel graph engine (9), varsubst (9), registrywrapper edit/diff (5).
  - `test_packageeditor_gui` (9): the de-god wiring + the focused-field rebuild crash regression.
  - `test_packageeditormodel` (9): the editor hub — LoadNodes/SaveNodes (rename re-files + orphan cleanup),
    replaceNodeJson, Revalidate signalling, KnownNodeIds/Platforms.
  - `test_launchresolver` (6): runner pick/pin, recipe, RunnerShipsBuild from parent closure, persistence, customvars.
  - `test_packagecatalog` (5): hydration predicates (#2), DehydrateNode (#8/#9), user settings, compatible runners.
  - `test_appmodel` (8): card-size persist, repo add/remove validation (#1), removePackage (#E28), save-to-disk,
    rebuildCatalog/repositoriesChanged signals.
  - `test_tabs` (6): construction/render smoke for every main tab + Settings subpages.
  So the engine + every GUI state/signal hub is regression-tested in-process.
- **Still UNVERIFIED on hardware**: the full editor GUI pass (TESTPLAN I40/I41 — add/remove/move nodes, each LAYER
  type, cover drop, the authoring runs Run EXE / Execute / Analyze, Publish) + a real game launch. Deferred to hardware.

## Remote build — CAUGHT UP (2026-06-23)
- Laptop (now `10.10.0.5` over WireGuard) synced to `main` @ 2d57f52, built clean `-Werror`, full ctest 7/7 green.
  Everything since the PackageEditor de-god (incl. this session's launch/path/validation fixes) is verified on
  BOTH machines. Default remote address is `10.10.0.5` (WG) — see [[feedback_build_both_machines]].

## Pending bugs
- (none open — #2/#3/#5/#8/#9 + the remove-freeze fixed; see "Done this session")

## Built but UNVERIFIED on hardware (resume testing here)
- **Remove-flow** — managed remove now de-hydrates off-thread (was freezing the GUI: heavy DAG-walk +
  per-block deletes + leveldb compaction on the GUI thread). Needs the laptop retest of: no freeze · TESTPLAN
  item 27 (leaves Library/Installed, **returns to Catalog**) · #9 (IPFS pins tree: CIDs unpinned/dropped) ·
  item 28 (local/portable remove keeps the user's own files).
- **#2/#3/#5** also only laptop-spot-checked green earlier — fine, but re-confirm after the remove retest.
- Deferred TESTPLAN: B11 (collapse-series, ≥2 in a series), I41 (edit-while-prelaunch), J42–45 (launch flow,
  needs Proton-GE runner + hydrated Windows game).

## Deferred tests (need state we didn't have)
- **B11** — collapse/expand a Series section: needs ≥2 games in one series in the Library.
- **I41** — edit a package while its PreLaunchWindow is open → the dialog reloads. Needs a launchable game (runner).
- **J42–J45** — launch flow (prelaunch layout, runner gate, inspect-runtime cleanup, run end-to-end). Needs an
  imported runner (Proton-GE) + a hydrated Windows game (AoE2).

## Larger tasks
- **De-god `PackageEditor`** (1646 LOC) — same AppModel/section-widget treatment as MainWindow. User: "packageeditor
  is a mess right now". Now the last remaining god object.
- **Commit `TESTPLAN.md`** (currently untracked).

## De-god ContainerWrapper — DONE (built -Werror both machines; launch UNVERIFIED on hardware)
- `containerwrapper.cpp` 2212 → 406 LOC: now a thin session orchestrator. Launch engine split into 11 cohesive
  TUs: `launchparams` (ContainerParams), `varsubst` (pure %token%/value-encode — UNIT-TESTED, tests/test_varsubst),
  `launchresolver` (param/recipe/runner/exec/persistence resolution), `fileedits`, `registrylayer`, `persistlayer`,
  `vfsmount`, `launchsources` (IPFS pre-flight/materialize), `runnerinstall`, and `RunCommand` moved to `processenv`.
- Pure-move refactor (verbatim bodies via brace-matching extraction); no behavior change intended. ctest green.
- **NOT yet exercised on hardware** — a real game launch (TESTPLAN J42–45) is the verification; deferred to the
  user's next hardware session (needs a Proton-GE runner + a hydrated Windows game), same as the remove-flow retest.

## Headless E2E testing campaign (2026-06-23) — real runtime path verified + fixed
Drove the whole engine via the CLI (`--node`/`--resolve-only`/`--publish`/`--fetch`/`--validate-nodes`, isolated
`--data-dir`) on a synthetic native-passthrough Linux package — see the [[reference_headless_e2e_testing]] memory.
- **Verified working**: full launch (FUSE mount + execute + clean exit), publish→fetch IPFS round-trip (byte/CID
  parity), cross-node P2P fetch, dehydrated write-through fetch-on-demand, validate/list, resolve dump.
- **Fixed: launch didn't fail loudly** — no-runner or unmountable-layer launches ran an empty command and reported
  "exit 0". `InitializeFromNode` now returns false on no-runner; the headless `--node` path checks
  `BuildContainerRuntime()` before `Execute()`. (GUI path already gated.)
- **Fixed: AppPaths::DataRoot** — every app-produced path (launch TEMP/RUNTIME/DEFPREFIX, LIBRARY default, prelaunch
  save) now hangs off the resolved data dir, so portable / `--data-dir` relocate the WHOLE footprint (was hardcoded
  `~/.VidyaGod/TEMP`, leaking out of a portable/test instance). USERDATA stays beside the package (by design).
- **Fixed: VFS layer publishability validation** — VFSDirLayer → warning (unzipped authoring intermediary; use
  "→ ZIP"); DEFLATE-compressed VFSZipLayer → error (won't mount), with a "⚠ Re-store" one-click fix in the editor.
- **Still open / by design**: VFSDirLayer can't be published (M1, files/zips only — now a warning, intentional);
  unreachable-repo git-sync still `git init`s the dir + double WARN (cosmetic).
- GUI-UNVERIFIED on hardware: the editor's "⚠ Re-store" button + "→ ZIP" flow (logic mirrors proven RunCommand
  machinery; built `-Werror` on both machines). Runtime GUI click-through still deferred to a hardware session.

### Heterogeneous-package pass (2026-06-23) — 8 packages × 6 runners, edge cases
Verified working: embedded runners (picked from the package bundle + launched), optional/default/EXCLUDE modules,
UnifiedRuntime build-fold, wine-heterogeneous (selective persist + CustomVars), multi-edition GROUP, diamond-dep
dedup, win32 + native launch (mocked). Improvements implemented this pass:
- **Runner default pick** no longer arbitrary (was first-by-node-id-sort) — `PickRunnerNode` ranks RECOMMENDED >
  package-local (runner in the launch node's own bundle; runners stay global, no embedded flag) > node-id.
- **Module EXCLUDE** — an explicit toggle-on now beats a conflicting default-on option (was silently dropped).
- **Validation**: runner-self-VFS-layers warning (silently ignored — build comes from PARENT nodes); CONTENTPATH
  nesting hint ("did you mean 'payload/game.exe'?").
- Open by design (user): runners are global wherever they live; duplicates resolve package-local > global.
- NOT YET: a RECOMMENDED edition driving the default launch within a GROUP (verify on hardware); persist paths
  appearing to resolve under the package source dir (verify the capture maps to USERDATA).

## Done this session (for reference)
- #1 dup repo rejected · #4 download UI freeze (cover re-scale per tick + BuildCidLabels disk-rescan on GUI thread +
  progress throttle) · #6 slow pinning (batched finalize) · #7 cancelled row dropped · #10 app-data field grayed.
- **#2** content guard — `PackageCatalog::NodeHasContent` (closure defines ≥1 VFS content layer) gates
  `LibraryTab::buildCards` + `PackagesView::rebuildList`, so vacuously-hydrated content-less packages are hidden.
- **#3** size-weighted progress — `DownloadManager::applyProgress` weights each CID's % by its byte size
  (`DownloadCidSize`, filled off-thread via `CidSize`), falling back to equal-weight; no more jerky bar.
- **#5** status-coded transfer bars — `StatusRole` + `TransferStatus` enum; `ProgressBarDelegate` colours queued
  purple / downloading blue / pinning dark-green / stalled amber / errored red, set at every transition.
- **#8 + #9** remove-flow rework — `PackageCatalog::DehydrateNode` (inverse of HydrateNode: deletes content-layer
  files, keeps manifest+cover, unpins+DropRefs the CIDs). `AppModel::removePackage` de-hydrates managed packages
  (under LibraryRoot) keeping manifest+cover+LIBRARY entry → package returns to Catalog; local/portable packages
  only drop the LIBRARY reference, never touch the user's files (TESTPLAN E28 preserved).
- Cancel purges the partial (DropCached) · crash/close persists + resumes (Settings.ActiveDownloads) · download
  drops a stale/orphaned ref and re-fetches instead of "missing files".
- Covers: re-pin by reference (additive/overwrite seed modes; orphans re-pointed); HydrateNode seeds a present
  cover; node primitives VgDropRef / VgHasLocal / VgDropCached.
- The whole AppModel signal-hub refactor (MainWindow 1929→148) — runtime-validated, no architecture bugs found.
