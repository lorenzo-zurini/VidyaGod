#ifndef APPMODEL_H
#define APPMODEL_H

#include <QObject>
#include <QString>

#include <utility>
#include <atomic>
#include <set>
#include <map>
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
    // Publish side (Sharing tab): freeze your library into content-addressed blocks and seed them, and record the
    // shareable launchable-CID list. Runs off-thread; emits libraryPublished / libraryPublishFailed.
    void publishLibraries();
    // Consumer side (gigagraph sharing): add a game by a launchable CID (a friend's / pasted). Fetches its whole
    // closure into the pretty on-disk checkout (NodeGraph::HydratePackage) off-thread, then rebuilds the catalog so
    // it appears in the Library. Emits gameAdded / gameAddFailed.
    void addGameByCid(const QString & launchableCid);

    // Friend library sharing (bilateral, per-(friend, library)). Seeder side: share/withdraw a named library's
    // launchable CIDs with one friend (from config["Libraries"], produced by publishLibraries). Leecher side: ask a
    // friend for what they share with us (replies arrive via FriendsManager::friendLibrary → stored in
    // config["FriendLibraries"][peer][lib]); install pulls a friend's game on demand (→ addGameByCid).
    void shareLibraryWithFriend(const QString & peer, const QString & lib);
    void stopSharingWithFriend(const QString & peer, const QString & lib);
    bool isSharingWithFriend(const QString & peer, const QString & lib) const;
    void requestFriendLibraries(const QString & peer);
    void installFriendGame(const QString & launchableCid) { addGameByCid(launchableCid); }
    // Apply a friend's COMPLETE shared set (a snapshot, {name:[cids]} JSON) to config["FriendLibraries"][peer]: parse +
    // sanitize, then REPLACE that peer's record wholesale (empty ⇒ drop the peer). Persists + emits friendCatalogChanged
    // ONLY when something actually changed. The receiver half of the bilateral protocol — wired to the friend service in
    // the ctor; exposed so the snapshot semantics (wholesale-replace, drop-on-empty, dedup) are directly testable.
    //
    // seq is the sender's monotonic snapshot stamp: this is the SEQ AUTHORITY (last-writer-wins). Snapshots ride
    // independent, concurrently-handled streams, so a stale one can be delivered/emitted after a fresher one; we keep
    // only the highest seq seen per peer and drop anything older. seq==0 (unstamped) always applies. The high-water
    // marks are in-memory/session-scoped (they reset on restart, which is safe: the sender's stamps are clock-seeded).
    void applyFriendLibrarySnapshot(const QString & peer, const QString & libsJson, quint64 seq = 0);
    // The names of the libraries this node currently exposes (config["Libraries"] keys). Empty until publishLibraries.
    QStringList myLibraryNames() const;
    // Re-push every active share (config["Sharing"]) into the Go layer with the CURRENT config["Libraries"] CIDs. Go's
    // share table is in-memory (rebuilt every launch); config["Sharing"] is the durable record. Called on startup (to
    // re-arm shares after a restart) and after a re-publish (the launchable CIDs may have moved). Without it the two
    // halves split-brain: the UI shows a friend as shared-with, but Go serves them nothing.
    void reRegisterShares();
    // Self-healing reconciliation (the sharing handshake is otherwise EDGE-triggered — it fires only when you flip a
    // toggle, so share-before-accept, a toggle made while the peer was offline, and an app restart all left the receiver
    // showing nothing). reconcileReceivedFriend re-enqueues a peer's shared blocks from our PERSISTED snapshot
    // (satisfied targets skip, so this is what retries a fetch that failed while they were offline) AND re-requests a
    // fresh snapshot in case their shares changed. Called on node-ready (for every receiving-from peer, via
    // reconcileReceivedLibraries) and whenever such a peer comes online (friendPresence).
    void reconcileReceivedFriend(const QString & peer);
    void reconcileReceivedLibraries();
    bool hasFriendLibraries(const QString & peer) const;
    // Consent with a friend ended (remove/block): forget both directions of the relationship's sharing state — stop
    // serving them (config["Sharing"][peer]) and drop what they shared with us (config["FriendLibraries"][peer]).
    void forgetFriend(const QString & peer);

    // ── Network tab: per-peer toggles (config-backed; save + push on change) ──
    // Receive = accept the friend's shared libraries as browsable catalog stubs (config["ReceiveFrom"]); Presence =
    // share MY online presence with them (false ⇒ in config["PresenceDeny"]); vLAN = include them in the Virtual-LAN
    // (false ⇒ in Settings.LanExcludedPeers). acceptPeer = FriendAccept + apply the New-Peer defaults. Auto-accept
    // (config["AutoAcceptPeers"]) applies acceptPeer to every incoming request automatically ("server mode").
    bool isReceivingFrom(const QString & peer) const;
    void setReceivingFrom(const QString & peer, bool on);
    bool isPresenceSharedWith(const QString & peer) const;
    void setPresenceSharedWith(const QString & peer, bool on);
    bool isInVlan(const QString & peer) const;
    void setInVlan(const QString & peer, bool on);
    void acceptPeer(const QString & peer);
    bool autoAcceptEnabled() const;
    void setAutoAcceptEnabled(bool on);
    // The "New Peers" defaults row, applied the instant a peer is accepted. key ∈ {receive,presence,vlan}.
    bool newPeerDefault(const QString & key) const;
    void setNewPeerDefault(const QString & key, bool on);
    QStringList newPeerShareDefaults() const;
    void setNewPeerShareDefault(const QString & lib, bool on);
    void stopReceivingFromFriend(const QString & peer);   // drop the friend's stub sources + LIBRARY entries
    void pushPresenceDeny();                              // Settings/config PresenceDeny → Go (node-ready + on change)
    void applyNewPeerDefaults(const QString & peer);      // used by acceptPeer + the auto-accept path
    void enqueueReceivedShares(const QString & peer);     // shared node+tile CIDs → rolling queue, dest = final library path
    void onFriendBlockLanded(const QString & cid, bool ok); // a received node block landed → debounced catalog rebuild

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
    void friendCatalogChanged();                 // a friend shared/updated/withdrew a library — refresh the friend view
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
    std::map<std::string, quint64> FriendLibSeq;         // per-peer highest applied snapshot stamp (last-writer-wins; session-only)
    std::set<std::string>          FriendBrowseCids;     // received-share CIDs we enqueued (roots+tiles) — scopes the transferFinished reaction
    bool                           FriendReconcilePending = false;  // debounce: coalesce a burst of landed blocks into one catalog rebuild
};

#endif // APPMODEL_H
