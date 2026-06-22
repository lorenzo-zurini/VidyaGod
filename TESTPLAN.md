# VidyaGod — Manual Test Plan (post-AppModel refactor)

Runtime verification for the AppModel signal-hub refactor (commit `6714db7`): MainWindow is now a thin
composition root; every tab + Settings subpage is its own `QWidget` talking to a central `AppModel` via signals.
This refactor is **compile- and startup-verified only** — the items below exercise the live flows on hardware.

Ordered so earlier sections set up state the later ones reuse. **⚠️ = a seam this refactor actually rerouted** —
if something broke, it's most likely there. **⚠️⚠️ = the core of the refactor.**

---

## A. Startup & persistence (AppModel owns this now)
- [ ] 1. **Cold start** — launch with `~/.VidyaGod` intact. All 5 tabs present, no console errors, catalog populated.
- [ ] 2. ⚠️ **Card size persists** — set Library to "Large", quit, relaunch. Reopens at Large, *and Catalog is also Large* (shared via AppModel).
- [ ] 3. ⚠️ **Sort persists** — set Library to "Date" then "Series", quit, relaunch. Reopens on that sort with correct grouping.
- [ ] 4. **Window geometry persists** — resize/move, quit, relaunch. Same size/position.
- [ ] 5. **Collapsed sections persist** — collapse a Series band and a Catalog repo. Quit, relaunch. Both stay collapsed.
- [ ] 6. **Max-concurrent-downloads persists** — set in Settings → Downloads, quit, relaunch, reopen page. Value retained.
- [ ] 7. **Fresh start** — move `~/.VidyaGod` aside, launch. Default repo cloned, empty-library message, no crash. (Restore after.)

## B. Library tab
- [ ] 8. **Sort Name / Date / Series** — instant re-order, no cover flicker to black, Series shows collapsible bands + "Other".
- [ ] 9. **Search** — live filter; in Series view, groups recompute over the visible subset.
- [ ] 10. ⚠️ **Size Large/Medium/Small** — covers re-scale crisply (not blurry/black), layout reflows.
- [ ] 11. **Collapse/expand a series** — cards hide/show; choice remembered (see #5).
- [ ] 12. **Empty search result** — search gibberish. Graceful empty grid, no crash.

## C. Catalog + download flow (DownloadManager ↔ Catalog ↔ IPFS, all signals now) ⚠️
- [ ] 13. **Catalog populated** — un-hydrated games grouped by repo; multi-game packages show secondary cover overlays.
- [ ] 14. **Open download dialog** — hover a card, Download. Dialog shows editions / other games / optional content / available runners; free-space + size estimate update as sizes resolve.
- [ ] 15. **Download a single game** — confirm. Card shows "Downloading…" overlay with %; IPFS tab shows Queued → Fetching; on completion the card **leaves Catalog, appears in Library**, Installed-Packages list gains it.
- [ ] 16. ⚠️ **Progress overlay accuracy** — multi-file download shows averaged %, advances, ends ~100, then card disappears.
- [ ] 17. **Rebuild during download** — change Catalog sort/search mid-download. Overlay/% **survives the rebuild** (CatalogTab restores it from its own in-flight set).
- [ ] 18. ⚠️ **Cancel a download** — click the downloading card → confirm cancel. Overlay clears, queued IPFS rows drop, card stays in Catalog.
- [ ] 19. **Concurrent downloads** — start 2–3 at once. All show progress simultaneously, none blocks the others, each completes independently.
- [ ] 20. **Download a package that includes a runner** — leave the runner checked. After completion it shows **Imported** in Settings → Runners.
- [ ] 21. **Failure path** — (force it, e.g. yank network mid-fetch) "Failed" row in IPFS tab, overlay clears, no modal error, no hang.

## D. IPFS tab (refreshes on catalogChanged + DownloadManager signals now) ⚠️
- [ ] 22. **Tab activation** — switch to IPFS: status + peer count populate; switch away → periodic refresh stops (only active tab polls).
- [ ] 23. **Transfers table** — during a download: rows with Name/Size/Progress/Speed/Status/CID, progress bar fills, speed shown.
- [ ] 24. **Seeded/pinned tree** — after a download: new content appears under its package group in the pins tree.
- [ ] 25. **Stall handling** — a stalled transfer is flagged, speed ages out, others continue.

## E. Settings → Installed Packages (PackagesView)
- [ ] 26. **List correctness** — only hydrated, launchable packages (no runner-only bundles, no remote-only entries).
- [ ] 27. ⚠️ **Remove a managed package** — gone from list, **reappears in Catalog**, Library tile gone, files deleted under the library root.
- [ ] 28. **Remove a local/portable package** — reference dropped but **the user's own files are NOT deleted**.
- [ ] 29. **Add Local Package** — point at a folder with valid package(s): scans recursively, adds new ones, "Added N / skipped M", list + Library + Catalog update.
- [ ] 30. **Add duplicate** — counted as skipped, no duplicate entry.

## F. Settings → Runners (RunnersPage, imports via model now) ⚠️
- [ ] 31. **Listing rules** — global runners always shown; embedded runners only after install; correct status badges.
- [ ] 32. **Import a runner** — button → "Importing…", IPFS tab shows the fetch, page **auto-rebuilds to "Imported"** on completion (no manual refresh).
- [ ] 33. **IPFS-unavailable guard** — (node down) Import hidden / "IPFS unavailable" shown.

## G. Settings → Repositories (RepositoriesPage, off-thread via model) ⚠️
- [ ] 34. **Sync now** — button → "Syncing…", then **re-enables itself** when done (via repositoriesChanged); Catalog reflects upstream changes.
- [ ] 35. **Add repository** — valid git URL + name → "Cloning…", repo clones, appears in list, packages show in Catalog, button resets.
- [ ] 36. **Add with empty URL** — warning, no-op.
- [ ] 37. **Remove a repository** — card gone, its packages disappear from Catalog, default repo still works.

## H. Settings → Downloads & Paths
- [ ] 38. **Max simultaneous downloads** — change it; verify #19 reflects the new cap.
- [ ] 39. **Temp root / Library folder** — set + Browse, edit, clear (→ default). Persisted; App-data dir shown read-only.

## I. Package editor + prelaunch (RefreshPackage hook) ⚠️
- [ ] 40. **Edit from Settings → Package Editor** — save a change: catalog rebuilds, the edited tile updates **in place across Library/Catalog/Packages**.
- [ ] 41. **Edit while a prelaunch dialog is open** — edit+save that same package elsewhere: the open prelaunch **reloads/rebuilds** to the new manifest (no stale state, no crash).

## J. Launch
- [ ] 42. **Launch a hydrated game** — Play → prelaunch window: cover left full-size, options laid out without dead space.
- [ ] 43. **Runner gate** — try to play a game with no usable runner: blocked with a clear reason (not a silent fail).
- [ ] 44. **Inspect-runtime checkbox** — tick it, launch: dialog appears; on OK/close runtime is cleaned up (no dangling runtime).
- [ ] 45. **Run a game end-to-end** — launches, runs, exits cleanly; relaunch a second time (no stale mount/lock).

## K. Cross-cutting signal sync (the core of this refactor) ⚠️⚠️
- [ ] 46. **One change, all tabs react** — download a game, then switch through Library / Catalog / Installed Packages: all already reflect it (single `catalogChanged` fan-out).
- [ ] 47. **Card size cross-tab** — change size on Catalog, switch to Library: already at that size, correct button checked.
- [ ] 48. **Cover lazy-load** — first paint with many uncached covers: covers fill in within ~200ms batches, no per-cover flicker, both tabs repaint.
- [ ] 49. **No double-refresh jank** — at download completion (we emit `downloadFinished` → `rebuildCatalog` → `transfersChanged`): smooth single visible update, no relayout storm.

## L. Stress / edge
- [ ] 50. **Rapid tab switching** during an active download/sync — no crash, no orphaned threads.
- [ ] 51. **Quit mid-download** — close the window while downloading: clean exit (geometry saved), next launch recovers (leveldb, single-instance lock).
- [ ] 52. **Two instances** — launch a second copy: "already running" dialog, second aborts.
