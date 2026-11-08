#ifndef APPMODEL_H
#define APPMODEL_H

#include <QObject>
#include <QString>

#include <utility>
#include <atomic>
#include <filesystem>
#include <set>
#include <map>
#include <string>
#include <memory>
#include <optional>

#include "nlohmann/json.hpp"
#include "manifestmodel.h"   // NodeIndex
#include "packagecatalog.h"  // SourceUpgradePlan (pending source-CID upgrade)
#include "installed.h"       // Installed::NodeHydration

#include <unordered_map>

class FriendSharing;
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
    const NodeIndex &        catalogIndex()  const { return *Catalog; }
    // The index itself, immutable and shared: a worker thread keeps reading it while a rebuild replaces it. One per
    // rebuild for everyone — each download worker once copied the whole index, twice (46 resumed downloads held ~90
    // copies, 1.6 GB), and every snapshot was another copy. GUI thread.
    std::shared_ptr<const NodeIndex> catalogSnapshot() const { return Catalog; }
    // What of the catalog is on disk (Installed::HydrationMap — a stat of every content file), worked out once per
    // rebuild: the Library, the Catalog and the Runners page all redraw on the same catalogChanged. GUI thread.
    const std::unordered_map<std::string, Installed::NodeHydration> & hydration() const;
    QDir *                   appDataDir()    const { return AppDataDir; }
    FriendSharing &          friends()       const { return *Friends; }   // the friend protocol (friendsharing.h)
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
    void markNodeReady() { emit nodeReady(); }         // MainWindow calls this once the async node start completes
    //A package (folder or UID): forget one added from outside LIBRARY (its folder untouched), or de-hydrate one its
    //config entry names (a source's — on a worker: *Async, done at catalogChanged). "" when done or under way; else
    //why nothing was done (a package of LIBRARY the config does not name is not removed: see the .cpp).
    QString removePackage(const QString & which, bool * Async = nullptr);
    void notifyCoversReady();                          // a batch of covers finished loading → emit coversReady()

    // ── Async source / runner ops (do the IPFS work off-thread, then rebuild + emit on the GUI thread) ──
    void syncSources();                                // re-index CID package sources, fetching any not-yet-present one
    void pushSeedLevels();                             // hand the node the 3-level meta-CIDs so it announces seeded content ordered
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
    // Publish side (Sharing tab): freeze your library into content-addressed blocks and seed them, and record the
    // shareable launchable-CID list. Runs off-thread; emits libraryPublished / libraryPublishFailed.
    void publishLibraries();
    bool publishing() const { return Publishing; }    // a publish is in flight (one at a time: a second runs after it)
    // Consumer side (gigagraph sharing): add a game by a launchable CID (a friend's / pasted). Fetches its whole
    // closure into the pretty on-disk checkout (NodeStore::HydratePackage) off-thread, then rebuilds the catalog so
    // it appears in the Library. Emits gameAdded / gameAddFailed.
    void addGameByCid(const QString & launchableCid);

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
    void libraryPublished(QString ipnsName, QString topCid, QString note);  // Verify & Publish finished (note = soft warning, "" if clean)
    void libraryPublishFailed(QString message);                             // Verify & Publish failed (mint error / offline)
    void gameAdded(QString dir);                 // addGameByCid succeeded — the checkout dir; Library now shows it
    void gameAddFailed(QString message);         // addGameByCid failed (bad CID / offline / fetch error)
    void networkingChanged(bool enabled);   // user toggled IPFS networking — start/stop the node + grey Catalog/IPFS
    void nodeReady();               // the embedded IPFS node finished coming up (Available() + friend code live) — tabs
                                    // that read node state (Network: friend code + Verify&Publish enable) must refresh,
                                    // since networkingChanged fires at TOGGLE time, before the async start completes
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
    FriendSharing *          Friends = nullptr;   // child QObject, made in the ctor
    std::shared_ptr<const NodeIndex> Catalog = std::make_shared<const NodeIndex>();
    mutable std::optional<std::unordered_map<std::string, Installed::NodeHydration>> Hydration;   // of Catalog; reset with it
    // Make Idx the index (built from Inputs — BuildCatalogIndex's): it is what the app shows, and what code holding only
    // the config reads (PackageCatalog::CurrentIndex) while no node file has changed since.
    void setCatalog(NodeIndex Idx, std::string Inputs, std::vector<Diagnostics::Entry> Said);
    int                      CardPixelWidth = 185;
    QTimer *                 OrphanHealTimer = nullptr;   // periodic background orphan check (tab-independent)
    bool                     SyncRetryPending = false;    // a re-sync is scheduled for a source that failed to fetch
    // single-flight guard for healOrphansIfAny — shared, because the worker clears it and may outlive this model
    std::shared_ptr<std::atomic<bool>> HealInFlight = std::make_shared<std::atomic<bool>>(false);
    bool                     SeededUnheld = false;   // SeedUnheld ran this process (healOrphansIfAny, first pass)
    std::set<std::string>    KnownUnhealable;             // orphaned paths a heal couldn't fix (content truly gone) → don't re-loop
    void runPublish();                                              // publishLibraries' one pass
    bool                           Publishing = false;              // publishLibraries in flight
    bool                           PublishAgain = false;            // asked again while it ran → once more after
};

#endif // APPMODEL_H
