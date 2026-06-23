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
- **Now partly covered by tests**: `vg_gui_tests` (headless Qt Test, offscreen — see the GUI-harness memory) drives
  the real PackageEditorModel + sections and asserts the de-god wiring (field edits → doc, replaceNodeJson/SaveNodes
  signals, Identity field edit → rebuild, Layers render). So the model+widget wiring is regression-tested.
- **Still UNVERIFIED on hardware**: the full editor GUI pass (TESTPLAN I40/I41 — add/remove/move nodes, each LAYER
  type, cover drop, the authoring runs Run EXE / Execute / Analyze, Publish) + a real game launch. Deferred to hardware.

## Remote build PENDING (laptop offline since the PackageEditor work)
- All PE phases (1–4) are committed + **local `-Werror` clean**, but NOT yet built on 192.168.1.134 (it went "No
  route to host" mid-phase-2). Catch up a remote `-Werror` build + ctest of latest `main` when the laptop returns.

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
