#ifndef PEERSHARING_H
#define PEERSHARING_H

#include <QObject>
#include <QString>
#include <QStringList>

class QTimer;

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>

#include "nlohmann/json.hpp"

class AppModel;

// ---------------------------------------------------------------------------
// PeerSharing — the peer protocol, for the GUI: what this node shares with each peer, what peers share with
// it (their libraries as browsable catalog stubs, landed on demand), and the per-peer toggles of the Network tab.
// AppModel owns it (AppModel::peers()) and it reads and persists through the model: the config, the catalog, a
// rebuild, a publish. It reacts to the node's peer events (PeersManager) and to landed transfers by itself.
// ---------------------------------------------------------------------------
class PeerSharing : public QObject
{
    Q_OBJECT
public:
    explicit PeerSharing(AppModel & Model);

    // Peer library sharing (bilateral, per-(peer, library)). Seeder side: share/withdraw a named library's
    // share sheet entries with one peer (from config["ShareSheet"], produced by publishLibraries). Leecher side: ask a
    // peer for what they share with us (replies arrive via PeersManager::peerLibrary → stored in
    // config["PeerLibraries"][peer][lib]); install pulls a peer's game on demand (→ addGameByCid).
    void shareLibraryWithPeer(const QString & peer, const QString & lib);
    void stopSharingWithPeer(const QString & peer, const QString & lib);
    bool isSharingWithPeer(const QString & peer, const QString & lib) const;
    void requestPeerLibraries(const QString & peer);
    // Apply a peer's COMPLETE shared set (a snapshot, {name:[cids]} JSON) to config["PeerLibraries"][peer]: parse +
    // sanitize, then REPLACE that peer's record wholesale (empty ⇒ drop the peer). Persists + emits peerCatalogChanged
    // ONLY when something actually changed. The receiver half of the bilateral protocol — wired to the peer service in
    // the ctor; exposed so the snapshot semantics (wholesale-replace, drop-on-empty, dedup) are directly testable.
    //
    // seq is the sender's monotonic snapshot stamp: this is the SEQ AUTHORITY (last-writer-wins). Snapshots ride
    // independent, concurrently-handled streams, so a stale one can be delivered/emitted after a fresher one; we keep
    // only the highest seq seen per peer and drop anything older. seq==0 (unstamped) always applies. The high-water
    // marks are in-memory/session-scoped (they reset on restart, which is safe: the sender's stamps are clock-seeded).
    void applyPeerLibrarySnapshot(const QString & peer, const QString & libsJson, quint64 seq = 0);
    // The names of the libraries this node currently exposes (the share sheet's libraries). Empty until publishLibraries.
    QStringList myLibraryNames() const;
    // Re-push every active share (config["Sharing"]) into the Go layer with the CURRENT share sheet. Go's
    // share table is in-memory (rebuilt every launch); config["Sharing"] is the durable record. Called on startup (to
    // re-arm shares after a restart) and after a re-publish (the launchable CIDs may have moved). Without it the two
    // halves split-brain: the UI shows a peer as shared-with, but Go serves them nothing.
    void reRegisterShares();
    // Push to Go everything Peer receives: the libraries shared with them, closed over what their packages contain
    // (PackageCatalog::SharedWith). A library the last push sent that the closure no longer holds is unshared.
    void pushSharesTo(const std::string & Peer);
    // What Peer receives, {lib: [share rows]}: the libraries shared with them closed over what their packages contain,
    // plus an explicitly shared library with nothing published yet, EMPTY (an authoritative "nothing here").
    nlohmann::ordered_json sharedWith(const std::string & Peer) const;
    // Self-healing reconciliation (the sharing handshake is otherwise EDGE-triggered — it fires only when you flip a
    // toggle, so share-before-accept, a toggle made while the peer was offline, and an app restart all left the receiver
    // showing nothing). reconcileReceivedPeer re-enqueues a peer's shared blocks from our PERSISTED snapshot
    // (satisfied targets skip, so this is what retries a fetch that failed while they were offline) AND re-requests a
    // fresh snapshot in case their shares changed. Called on node-ready (for every receiving-from peer, via
    // reconcileReceivedLibraries) and whenever such a peer comes online (peerPresence).
    void reconcileReceivedPeer(const QString & peer);
    void reconcileReceivedLibraries();
    bool hasPeerLibraries(const QString & peer) const;
    // Consent with a peer ended (remove/block): forget both directions of the relationship's sharing state — stop
    // serving them (config["Sharing"][peer]) and drop what they shared with us (config["PeerLibraries"][peer]).
    void forgetPeer(const QString & peer);

    // ── Network tab: per-peer toggles (config-backed; save + push on change) ──
    // Receive = accept the peer's shared libraries as browsable catalog stubs (config["ReceiveFrom"]); Presence =
    // share MY online presence with them (false ⇒ in config["PresenceDeny"]); vLAN = include them in the Virtual-LAN
    // (false ⇒ in Settings.LanExcludedPeers). acceptPeer = PeerAccept + apply the New-Peer defaults. Auto-accept
    // (config["AutoAcceptPeers"]) applies acceptPeer to every incoming request automatically ("server mode").
    bool isReceivingFrom(const QString & peer) const;
    void setReceivingFrom(const QString & peer, bool on);
    bool isPresenceSharedWith(const QString & peer) const;
    void setPresenceSharedWith(const QString & peer, bool on);
    bool isInVlan(const QString & peer) const;
    void setInVlan(const QString & peer, bool on);
    bool acceptPeer(const QString & peer);

    // ── Each peer's permissions, asked when you pair or accept ──
    // The table's columns, each side deciding for itself: the libraries of mine they get (Share: <library>), whether I
    // receive theirs, whether they see my presence, whether they are in my virtual LAN. Asked when pairing or accepting
    // (ticks that start as the "New peers" row), they are consent given on the screen that says them (the receive
    // gate's opt-in), and remembered as given (config["PeerChosen"]): an acceptance landing later never overwrites them.
    struct Permissions { QStringList Libraries; bool Receive = false, Presence = true, Vlan = false; };
    Permissions defaultPermissions() const;              // the "New peers" row: what pairing's ticks start as
    bool pairWith(const QString & peer, Permissions given, std::string * error = nullptr);   // the request + the permissions
    bool acceptPairing(const QString & peer, Permissions given);                             // the acceptance + the permissions
    void applyPairing(const QString & peer, Permissions given);
    bool hasChosen(const QString & peer) const;          // its settings were chosen (pairing, accepting, a toggle)
    // How much of what Peer shares with us is here: their packages, and those landed whole.
    struct Received { int Packages = 0, Here = 0; };
    Received receivedFrom(const QString & peer) const;

    // ── What I share, kept current ──
    // A peer sees what the share sheet records. Once a library has been published once (its first publish names every
    // node file by its label: a step a person starts, prepareSharing), whatever of it is shared with anyone and changed
    // since — a new package, an edited one — is published by itself in the background, one package at a time (so a
    // failure is that package's), and a package deleted from disk leaves the sheet (nothing minted). A package that could
    // not be published is not tried again until it changes, or the node comes up again.
    enum class ShareState { NothingShared, Unpublished, Offline, UpToDate, Updating, Failed };
    ShareState shareState() const { return State; }
    QString shareStateText() const;
    void checkSharesSoon();                             // debounced: what is shared changed, or the library did
    void checkShares();                                 // the check itself (and the publishes it queues)
    void prepareSharing();                              // the first publish of the shared libraries never prepared
    // The pairing itself, as the Network tab and the CLI act on it: ask a peer, set our nickname, and end a
    // pairing three ways (decline an incoming request, block, remove) — each forgets both directions of sharing.
    bool sendPeerRequest(const QString & code, std::string * error = nullptr);
    bool setNickname(const QString & nick, std::string * error = nullptr);
    void declinePeer(const QString & peer);
    void blockPeer(const QString & peer);
    void removePeer(const QString & peer);
    bool autoAcceptEnabled() const;
    void setAutoAcceptEnabled(bool on);
    // The "New Peers" defaults row, applied the instant a peer is accepted. key ∈ {receive,presence,vlan}.
    bool newPeerDefault(const QString & key) const;
    void setNewPeerDefault(const QString & key, bool on);
    QStringList newPeerShareDefaults() const;
    void setNewPeerShareDefault(const QString & lib, bool on);
    void stopReceivingFromPeer(const QString & peer);
    // Remove a peer's received browse stubs (installs kept) + cancel their fetches; the package dirs in Keep (what the
    // peer still shares) stay, their files judged by the prune and the next landing.
    //Keep: the files the snapshot names now (path → CID); their package dirs stay, and in them only the files planned
    //for another CID, or no longer planned, are forgotten by the queue.
    void dropReceivedStubs(const std::string & peer, const std::map<std::string, std::string> & Keep = {});
    std::map<std::string, std::string> sharedStubFiles(const std::string & peer) const;   // the files its snapshot names → CID
    void pushPresenceDeny();                              // Settings/config PresenceDeny → Go (node-ready + on change)
    void applyNewPeerDefaults(const QString & peer);      // used by acceptPeer + the auto-accept path
    void enqueueReceivedShares(const QString & peer);     // shared node+tile CIDs → rolling queue, dest = final library path
    void onPeerBlockLanded(const QString & cid, bool ok); // a received node block landed → debounced catalog rebuild
    void completeReceivedClosures();                      // land the node-block closure of every received root (blocks only)
    void pushLanRoster();                              // apply Settings.LanExcludedPeers to the node's Virtual-LAN

signals:
    void peerCatalogChanged();                 // a peer shared/updated/withdrew a library — refresh the peer view
    void shareStateChanged();                    // shareState / shareStateText changed

private:
    void setState(ShareState S, const QString & Text);
    std::set<std::string> sharedLibraries() const;     // every library shared with anyone (all of mine for a share-all)
    void markChosen(const QString & peer);
    void pump();                                       // the next queued package's publish, when none runs

    AppModel & Model;
    ShareState State = ShareState::NothingShared;
    QString StateText;
    QTimer * ShareCheck = nullptr;
public:
    int BusyRetryMs = 45000;                           // the library busy with another writer: tried again after this
private:
    std::map<std::string, std::string> Attempted;      // "<lib>/<pkg>" -> its node signature when its publish was queued
    struct Queued { std::filesystem::path Dir; std::string Key; };
    std::vector<Queued> Queue;                         // what waits to be published, one package per pass
    std::vector<Queued> InFlight;                      // the packages being published now, in one pass
    std::set<std::string> Alone;                       // keys of a batch that failed: published one at a time, so a failure is theirs
    bool WholeInFlight = false;                        // a first publish of the whole library (prepareSharing) runs
    std::map<std::string, QString> FailedPackages;     // "<lib>/<pkg>" -> why its publish failed
    QString LastPublishError;                          // a publish of the user's own (not queued here) that failed
    mutable unsigned NamesOf = ~0u;                    // myLibraryNames' cache: the catalog generation it was read at…
    mutable size_t NamesSheet = 0;                     // …and the sheet's size then
    mutable QStringList Names;
    mutable std::map<std::string, std::pair<qint64, Received>> ReceivedCache;   // per peer: when, what
    std::map<std::string, std::set<std::string>> PushedShares;   // peer -> the library names last pushed to them
    std::map<std::string, quint64> PeerLibSeq;         // per-peer highest applied snapshot stamp (last-writer-wins; session-only)
    std::set<std::string>          PeerBrowseCids;     // received-share CIDs we enqueued (roots+tiles) — scopes the transferFinished reaction
    bool                           PeerReconcilePending = false;  // debounce: coalesce a burst of landed blocks into one catalog rebuild
    bool                           PeerClosureRunning = false;    // a closure-completion worker is in flight
    bool                           PeerClosureAgain = false;      // blocks landed while it ran → run once more
    std::set<std::string>          PeerStubsDirtyPeers;           // peers whose stubs were dropped while a pass ran → drop again when it ends
    std::map<std::string, std::set<std::filesystem::path>> DroppedStubDirs;   // peer → the lib dirs last dropped: swept again after the snapshot is gone
};

#endif // PEERSHARING_H
