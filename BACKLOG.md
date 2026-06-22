# VidyaGod — Backlog (from the 2026-06-22 test pass)

Living list of what's queued. Done items kept for context. Test plan checklist is in `TESTPLAN.md`.

## In progress
- **Download throughput** — sequential fetch was RTT-bound ~4 MB/s; parallel/windowed/session attempts gave
  150 MB/s bursts but periodic "waiting for peers" stalls (every ~3-4 s). Being benchmarked + optimized headlessly
  on the laptop (`--import-package`). Goal: steady link-speed downloads.

## Pending bugs
- **#2 — content-less package shows in Library AND Installed Packages.** A node with no content layers is
  *vacuously* `NodeHydrated`=true. Needs a real "has content" guard shared by `LibraryTab::buildCards` and
  `PackagesView::rebuildList`. (Surfaced by the intentional malformed "Dino Crisis 2" test case.)
- **#3 — card download progress is staggered.** `DownloadManager::applyProgress` averages per-CID percent
  *unweighted*; small files completing jerk the bar. Fix: weight each CID by its byte size (`CidSize`).
- **#5 — transfer progress-bar colors not status-coded.** Want: queued = purple, downloading = blue, stalled =
  yellow, errored = red (today queued + downloading are both light blue). In `ProgressBarDelegate` / IpfsTab.
- **#8 — Remove deletes the manifest, not just content.** `AppModel::removePackage` `remove_all`s the whole bundle
  dir incl. the git-tracked manifest/cover → the package vanishes from the Catalog instead of de-hydrating.
  Should delete only the hydrated content (the untracked layer files), keep the manifest so it returns to Catalog.
- **#9 — Remove doesn't unpin/dropRef.** The removed package's content CIDs stay pinned + become orphaned, still
  shown "seeded" in the IPFS tab. Remove must: read the manifest CIDs → unpin + dropRef → delete content → rebuild.
  (This is the root cause of the stale-ref "missing files" we kept hitting.)
- **#8 + #9 are one cohesive remove-flow rework** — the next big ledger item after downloads.
- Also for the remove rework: removing a **local/portable** package (added from outside LIBRARY) must NOT delete
  the user's own files (only drop the reference). Preserve this (TESTPLAN E28).

## Deferred tests (need state we didn't have)
- **B11** — collapse/expand a Series section: needs ≥2 games in one series in the Library.
- **I41** — edit a package while its PreLaunchWindow is open → the dialog reloads. Needs a launchable game (runner).
- **J42–J45** — launch flow (prelaunch layout, runner gate, inspect-runtime cleanup, run end-to-end). Needs an
  imported runner (Proton-GE) + a hydrated Windows game (AoE2).

## Larger tasks
- **De-god `PackageEditor`** (1646 LOC) — same AppModel/section-widget treatment as MainWindow. User: "packageeditor
  is a mess right now". Next de-god target; the resolver/`ContainerWrapper` (2212 LOC) is the other god object but
  higher risk — harden + unit-test the pure resolver seam first.
- **Commit `TESTPLAN.md`** (currently untracked).

## Done this session (for reference)
- #1 dup repo rejected · #4 download UI freeze (cover re-scale per tick + BuildCidLabels disk-rescan on GUI thread +
  progress throttle) · #6 slow pinning (batched finalize) · #7 cancelled row dropped · #10 app-data field grayed.
- Cancel purges the partial (DropCached) · crash/close persists + resumes (Settings.ActiveDownloads) · download
  drops a stale/orphaned ref and re-fetches instead of "missing files".
- Covers: re-pin by reference (additive/overwrite seed modes; orphans re-pointed); HydrateNode seeds a present
  cover; node primitives VgDropRef / VgHasLocal / VgDropCached.
- The whole AppModel signal-hub refactor (MainWindow 1929→148) — runtime-validated, no architecture bugs found.
