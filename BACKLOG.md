# VidyaGod — Backlog (from the 2026-06-22 test pass)

Living list of what's queued. Done items kept for context. Test plan checklist is in `TESTPLAN.md`.

## In progress
- **De-god PackageEditor** (MainWindow/AppModel pattern) — phases 1–2 DONE, 3–4 queued.
  - **Phase 1 (done)**: `PackageEditorModel` hub (packageeditormodel.{h,cpp}) owns the working doc + node I/O +
    validation + catalog/exec queries + authoring runs; PackageEditor delegates. Config taken by `const` end-to-end
    (incl. ContainerWrapper ctor's GlobalConfigJSON param now `const&`).
  - **Phase 2 (done)**: extracted `NodeGraphView`, `ValidationPanel`, `JsonRawEditor` into their own widget TUs;
    PackageEditor reacts to model signals (documentReloaded / validationChanged / savedToDisk).
  - **Phase 3 (TODO, the big one)**: extract `NodeEditor` (per-node tab, ~720 LOC of BuildUI) + its section widgets
    (Identity/Selection/Parents/Platform/Exec/Meta/Layers). ~40 lambdas; structural edits must drive the shell
    rebuild via a signal (e.g. `NodeEditor::rebuildRequested(tab)`) instead of calling BuildUI directly.
  - **Phase 4 (TODO)**: slim PackageEditor to a composition root + shared field-bind helper; finalize.
  - **Checkpointed before Phase 3** because the laptop (2nd-machine build + only runtime-test path) is OFFLINE —
    resume Phase 3 when it's back so the 720-LOC move can be verified on both machines, or push it locally if desired.
- **Download throughput** — sequential fetch was RTT-bound ~4 MB/s; parallel/windowed/session attempts gave
  150 MB/s bursts but periodic "waiting for peers" stalls (every ~3-4 s). Being benchmarked + optimized headlessly
  on the laptop (`--import-package`). Goal: steady link-speed downloads.

## Remote build PENDING (laptop offline since the PackageEditor work)
- PE phases 1–2 are committed + **local `-Werror` clean**, but NOT yet built on 192.168.1.134 (it went "No route to
  host" mid-phase-2). Catch up a remote `-Werror` build + ctest of latest `main` when the laptop returns.

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
