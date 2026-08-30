#ifndef APPMODEL_H
#define APPMODEL_H

#include <QObject>
#include <QString>

#include <utility>
#include <atomic>
#include <set>
#include <string>
#include <memory>

#include "nlohmann/json.hpp"
#include "manifestmodel.h"   // NodeIndex
#include "packagecatalog.h"  // SourceUpgradePlan (pending source-CID upgrade)

class QDir;
class QTimer;

// ---------------------------------------------------------------------------
// AppModel — the single source of truth for the GUI: it OWNS the live GlobalConfigJSON pointer, the cross-bundle
// catalog NodeIndex, the persisted card-pixel-width, and the AppDataDir. Every view (Library/Catalog/Settings
// subpages/IPFS/Packages) holds a pointer to it, reads state through it, calls its mutators, and reacts to its
// signals. Views NEVER call each other or reach into MainWindow — all cross-view communication flows through the
// model's signals. The model outlives every view (created first, destroyed last), so its worker threads can safely
// capture `this`; a delivered refresh that targets a since-destroyed view is auto-disconnected by Qt.
// ---------------------------------------------------------------------------
class AppModel : public QObject
{
    Q_OBJECT
public:
    AppModel(nlohmann::ordered_json * config, QDir * appDataDir, QObject * parent = nullptr);

    // ── Shared state (views read directly; the model owns the lifetime) ──
    nlohmann::ordered_json * config()       const { return Config; }
    NodeIndex &              catalogIndex()        { return CatalogIndex; }
    const NodeIndex &        catalogIndex()  const { return CatalogIndex; }
    QDir *                   appDataDir()    const { return AppDataDir; }
    int                      cardPixelWidth() const { return CardPixelWidth; }

    // ── Mutators (perform the change, persist, then emit so every view reacts) ──
    bool save();                                       // write GlobalConfigJSON to disk
    void rebuildCatalog();                             // re-scan CatalogIndex from disk → emit catalogChanged()
    // Import every valid package bundle found under Dir (recursively) into the LIBRARY (skipping duplicates / non-
    // launchable bundles); saves + rebuilds when any were added. Returns {added, skipped}. GUI-free (the caller owns
    // the file dialog + result message).
    std::pair<int, int> importPackagesFromDir(const QString & Dir);
    void setCardPixelWidth(int w);                     // persist + emit cardSizeChanged(w) (no-op if unchanged)

    // ── Networking (IPFS) — OFF by default; all download/seed activity is opt-in (Settings.IPFS.Enabled) ──
    bool networkingEnabled() const;                    // Settings.IPFS.Enabled (default false)
    void setNetworkingEnabled(bool on);                // persist + emit networkingChanged(on) (no-op if unchanged)
    void removePackage(const QString & uid);           // drop a LIBRARY entry (+ its managed files) → rebuild
    void notifyCoversReady();                          // a batch of covers finished loading → emit coversReady()

    // ── Async source / runner ops (do the IPFS work off-thread, then rebuild + emit on the GUI thread) ──
    void syncSources();                                // re-index CID package sources, fetching any not-yet-present one
    void pushSeedLevels();                             // hand the node the 3-level meta-CIDs so it announces seeded content ordered
    void pushLanRoster();                              // apply Settings.LanExcludedPeers to the node's Virtual-LAN
    // On-demand serve-reliability heal: if the node holds any ORPHANED no-copy reference (backing file moved/re-created,
    // so it reads locally but fails when a PEER requests it), re-point it to the content's current on-disk location.
    // Cheap-first (a filestore path scan; skips the heavy re-seed entirely when nothing is orphaned), off-thread,
    // single-flight, and remembers genuinely-gone content so it never loops. Driven by a background timer + the IPFS
    // tab's health check, so orphans are repaired the moment they're noticed rather than only on next launch.
    void healOrphansIfAny();
    void importRunner(const QString & runnerNodeId);   // emit runnerImportRequested → the ONE download pump (build fetch + DEFPREFIX)
    // Package sources by IPFS folder CID (dehydrated package sets; content hydrates on demand).
    bool addPackageSource(const QString & cid, const QString & name);   // append + fetch dehydrated tree off-thread; false if empty/duplicate
    void removePackageSource(int index);               // drop the source: config entry + fetched dir + LIBRARY entries
    // Move a source to a new collection CID. TWO PHASE on purpose: planning fetches the new manifest tree to a
    // staging dir and diffs it WITHOUT touching anything, so the user approves a concrete plan (what is kept, moved
    // and deprecated) before any content is relocated. Just rewriting the CID would be a silent no-op — the sync
    // only fetches when the source dir is missing — and remove+re-add deletes every hydrated file in the source.
    void planSourceUpgrade(const QString & name, const QString & cid);   // async → sourceUpgradePlanned / packageSourceFailed
    void applySourceUpgrade(bool force);                                 // async → applies the plan from the last planSourceUpgrade

signals:
    void catalogChanged();          // CatalogIndex rebuilt — Library/Catalog/Packages/IPFS refresh
    void cardSizeChanged(int w);    // card pixel width changed — Library/Catalog relayout
    void coversReady();             // lazy cover load(s) landed — repaint visible cards
    void packageSourcesChanged();   // a CID package source was added/removed/synced — refresh the Sources page + catalog
    void packageSourceFailed(QString message);   // a CID source fetch failed (e.g. node offline) — the dialog shows it
    void networkingChanged(bool enabled);   // user toggled IPFS networking — start/stop the node + grey Catalog/IPFS
    void runnerImportRequested(QString runnerNodeId);   // MainWindow routes this to DownloadManager::beginDownload (unified pump)
    void ipfsHealthChanged();       // orphaned refs were repaired — the IPFS tab should re-poll health
    // A source upgrade was planned and nothing has changed yet: `summary` describes it for confirmation, and
    // `unrelated` is true when the new tree shares NO packages with the current one (needs an explicit force).
    void sourceUpgradePlanned(QString summary, bool unrelated);
    // Content that can no longer be served as published (the file's bytes no longer hash to its recorded CID).
    // Emitted by the background self-check so the IPFS tab can show it as an ERROR — such a reference is
    // otherwise completely invisible until a peer requests it and hangs.
    void contentUnservable(QString cid, QString reason);

private:
    // The plan from the last planSourceUpgrade(), awaiting the user's confirmation in applySourceUpgrade().
    std::shared_ptr<PackageCatalog::SourceUpgradePlan> PendingUpgrade;
    nlohmann::ordered_json * Config;
    QDir *                   AppDataDir;
    NodeIndex                CatalogIndex;
    int                      CardPixelWidth = 185;
    QTimer *                 OrphanHealTimer = nullptr;   // periodic background orphan check (tab-independent)
    bool                     SyncRetryPending = false;    // a re-sync is scheduled for a source that failed to fetch
    std::atomic<bool>        HealInFlight{false};         // single-flight guard for healOrphansIfAny
    std::set<std::string>    KnownUnhealable;             // orphaned paths a heal couldn't fix (content truly gone) → don't re-loop
};

#endif // APPMODEL_H
