#ifndef FRIENDSHARING_H
#define FRIENDSHARING_H

#include <QObject>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <map>
#include <set>
#include <string>

#include "nlohmann/json.hpp"

class AppModel;

// ---------------------------------------------------------------------------
// FriendSharing — the friend protocol, for the GUI: what this node shares with each friend, what friends share with
// it (their libraries as browsable catalog stubs, landed on demand), and the per-peer toggles of the Network tab.
// AppModel owns it (AppModel::friends()) and it reads and persists through the model: the config, the catalog, a
// rebuild, a publish. It reacts to the node's friend events (FriendsManager) and to landed transfers by itself.
// ---------------------------------------------------------------------------
class FriendSharing : public QObject
{
    Q_OBJECT
public:
    explicit FriendSharing(AppModel & Model);

    // Friend library sharing (bilateral, per-(friend, library)). Seeder side: share/withdraw a named library's
    // launchable CIDs with one friend (from config["Libraries"], produced by publishLibraries). Leecher side: ask a
    // friend for what they share with us (replies arrive via FriendsManager::friendLibrary → stored in
    // config["FriendLibraries"][peer][lib]); install pulls a friend's game on demand (→ addGameByCid).
    void shareLibraryWithFriend(const QString & peer, const QString & lib);
    void stopSharingWithFriend(const QString & peer, const QString & lib);
    bool isSharingWithFriend(const QString & peer, const QString & lib) const;
    void requestFriendLibraries(const QString & peer);
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
    // Push to Go everything Peer receives: the libraries shared with them, closed over what their packages contain
    // (PackageCatalog::SharedWith). A library the last push sent that the closure no longer holds is unshared.
    void pushSharesTo(const std::string & Peer);
    // What Peer receives, {lib: [share rows]}: the libraries shared with them closed over what their packages contain,
    // plus an explicitly shared library with nothing published yet, EMPTY (an authoritative "nothing here").
    nlohmann::ordered_json sharedWith(const std::string & Peer) const;
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
    // The friendship itself, as the Network tab and the CLI act on it: ask a peer, set our nickname, and end a
    // friendship three ways (decline an incoming request, block, remove) — each forgets both directions of sharing.
    bool sendFriendRequest(const QString & code, std::string * error = nullptr);
    bool setNickname(const QString & nick, std::string * error = nullptr);
    void declineFriend(const QString & peer);
    void blockFriend(const QString & peer);
    void removeFriend(const QString & peer);
    bool autoAcceptEnabled() const;
    void setAutoAcceptEnabled(bool on);
    // The "New Peers" defaults row, applied the instant a peer is accepted. key ∈ {receive,presence,vlan}.
    bool newPeerDefault(const QString & key) const;
    void setNewPeerDefault(const QString & key, bool on);
    QStringList newPeerShareDefaults() const;
    void setNewPeerShareDefault(const QString & lib, bool on);
    void stopReceivingFromFriend(const QString & peer);
    void dropReceivedStubs(const std::string & peer);   // remove a friend's received browse stubs (installs kept) + cancel their fetches
    void pushPresenceDeny();                              // Settings/config PresenceDeny → Go (node-ready + on change)
    void applyNewPeerDefaults(const QString & peer);      // used by acceptPeer + the auto-accept path
    void enqueueReceivedShares(const QString & peer);     // shared node+tile CIDs → rolling queue, dest = final library path
    void onFriendBlockLanded(const QString & cid, bool ok); // a received node block landed → debounced catalog rebuild
    void completeReceivedClosures();                      // land the node-block closure of every received root (blocks only)
    void pushLanRoster();                              // apply Settings.LanExcludedPeers to the node's Virtual-LAN

signals:
    void friendCatalogChanged();                 // a friend shared/updated/withdrew a library — refresh the friend view

private:
    AppModel & Model;
    std::map<std::string, std::set<std::string>> PushedShares;   // peer -> the library names last pushed to them
    std::map<std::string, quint64> FriendLibSeq;         // per-peer highest applied snapshot stamp (last-writer-wins; session-only)
    std::set<std::string>          FriendBrowseCids;     // received-share CIDs we enqueued (roots+tiles) — scopes the transferFinished reaction
    bool                           FriendReconcilePending = false;  // debounce: coalesce a burst of landed blocks into one catalog rebuild
    bool                           FriendClosureRunning = false;    // a closure-completion worker is in flight
    bool                           FriendClosureAgain = false;      // blocks landed while it ran → run once more
    std::set<std::string>          FriendStubsDirtyPeers;           // peers whose stubs were dropped while a pass ran → drop again when it ends
    std::map<std::string, std::set<std::filesystem::path>> DroppedStubDirs;   // peer → the lib dirs last dropped: swept again after the snapshot is gone
};

#endif // FRIENDSHARING_H
