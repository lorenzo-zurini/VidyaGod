#ifndef APPMODEL_H
#define APPMODEL_H

#include <QObject>
#include <QString>

#include <utility>
#include <atomic>
#include <set>
#include <string>

#include "nlohmann/json.hpp"
#include "manifestmodel.h"   // NodeIndex

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

signals:
    void catalogChanged();          // CatalogIndex rebuilt — Library/Catalog/Packages/IPFS refresh
    void cardSizeChanged(int w);    // card pixel width changed — Library/Catalog relayout
    void coversReady();             // lazy cover load(s) landed — repaint visible cards
    void packageSourcesChanged();   // a CID package source was added/removed/synced — refresh the Sources page + catalog
    void packageSourceFailed(QString message);   // a CID source fetch failed (e.g. node offline) — the dialog shows it
    void networkingChanged(bool enabled);   // user toggled IPFS networking — start/stop the node + grey Catalog/IPFS
    void runnerImportRequested(QString runnerNodeId);   // MainWindow routes this to DownloadManager::beginDownload (unified pump)
    void ipfsHealthChanged();       // orphaned refs were repaired — the IPFS tab should re-poll health

private:
    nlohmann::ordered_json * Config;
    QDir *                   AppDataDir;
    NodeIndex                CatalogIndex;
    int                      CardPixelWidth = 185;
    QTimer *                 OrphanHealTimer = nullptr;   // periodic background orphan check (tab-independent)
    std::atomic<bool>        HealInFlight{false};         // single-flight guard for healOrphansIfAny
    std::set<std::string>    KnownUnhealable;             // orphaned paths a heal couldn't fix (content truly gone) → don't re-loop
};

#endif // APPMODEL_H
