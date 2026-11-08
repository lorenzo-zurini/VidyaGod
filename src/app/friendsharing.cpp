#include "friendsharing.h"
#include "appmodel.h"
#include "asyncwork.h"
#include "commonutils.h"
#include "downloadqueue.h"   // IpfsWrapper::EnqueueBatch — the ONE rolling queue that browse fetches ride
#include "ipfswrapper.h"
#include "nodegraph.h"
#include "nodestore.h"
#include "packagecatalog.h"

#include <QTimer>

#include <algorithm>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

FriendSharing::FriendSharing(AppModel & M)
    : QObject(&M), Model(M)
{
    // A friend sent us their COMPLETE shared set (a snapshot, per the bilateral protocol — pushed on any share change
    // and on our explicit requestFriendLibraries). We REPLACE our record for that peer wholesale: this is the only way
    // a withdraw can't be lost (a missed single "unshare" would otherwise leave a phantom entry). "{}" = they share
    // nothing with us → drop the peer. Persist + refresh the friend-catalog view ONLY when something actually changed,
    // so a redundant snapshot (e.g. a periodic re-push) doesn't churn the disk or the UI.
    connect(FriendsManager::instance(), &FriendsManager::friendLibrary, this,
        [this](const QString & peer, const QString & libsJson, quint64 seq) { applyFriendLibrarySnapshot(peer, libsJson, seq); });

    // Self-heal on reachability: when a friend we RECEIVE from comes online, re-materialise + re-request so the catalog
    // fills regardless of the order shares/accepts/toggles happened in (a node-ready request can fire before the friend
    // connection is up and fail silently; this retries the moment they're reachable). Fires only on the online transition.
    connect(FriendsManager::instance(), &FriendsManager::friendPresence, this,
        [this](const QString & peer, bool online) { if (online) reconcileReceivedFriend(peer); });

    // Periodic reconcile: presence-online and snapshot-apply are EDGE-triggered — a seeder killed mid-transfer and
    // restarted while we saw no edge (or a missed presence event) would otherwise leave the receiver waiting
    // forever. Every 10 min, re-plan + re-enqueue each receiving-from peer; the queue settles repeats silently
    // (a dest its job already wrote is a no-op at enqueue), so steady state costs nothing visible.
    {
        QTimer * Resync = new QTimer(this);
        Resync->setInterval(10 * 60 * 1000);
        connect(Resync, &QTimer::timeout, this, [this]{ reconcileReceivedLibraries(); });
        Resync->start();
    }

    // A received-share node block landed in the rolling queue (at its final library path) → (debounced) rebuild the
    // catalog so it shows what arrived. Same queue as any transfer; we react only to CIDs we ourselves enqueued for
    // received shares (FriendBrowseCids).
    connect(IpfsManager::instance(), &IpfsManager::transferFinished, this,
        [this](const QString & cid, bool ok, const QString &) { onFriendBlockLanded(cid, ok); });

    // Auto-accept ("server mode"): if enabled, accept every incoming friend request automatically and apply the
    // New-Peer defaults. Runs whether or not the Network tab is open.
    connect(FriendsManager::instance(), &FriendsManager::friendRequest, this,
        [this](const QString & peer, const QString &, const QString &) { if (autoAcceptEnabled()) acceptPeer(peer); });

    // A peer can become accepted WITHOUT our local acceptPeer() — a mutual-add "crossing" that auto-converges to
    // accepted (TestFriendMutualCrossingConverges). The New-Peer defaults (Receive/Presence/vLAN/Share) still must
    // apply, else e.g. Receive-on-by-default never kicks in. applyNewPeerDefaults is idempotent (toggles no-op if set).
    connect(FriendsManager::instance(), &FriendsManager::friendAccepted, this,
        [this](const QString & peer, const QString &, const QString &) { applyNewPeerDefaults(peer); });
}

// Push Settings.LanExcludedPeers (the GLOBAL Virtual-LAN roster's un-ticked members) into the node. Called at
// node-ready; the launch window's ticks call IpfsWrapper::SetLanExcluded directly on change.
void FriendSharing::pushLanRoster()
{
    std::vector<std::string> Ex;
    const auto & S = (*Model.config())["Settings"];
    if (S.contains("LanExcludedPeers") && S["LanExcludedPeers"].is_array())
        for (const auto & P : S["LanExcludedPeers"])
            if (P.is_string()) Ex.push_back(P.get<std::string>());
    IpfsWrapper::SetLanExcluded(Ex);
}
QStringList FriendSharing::myLibraryNames() const
{
    QStringList Out;
    if (Model.config()->contains("Libraries") && (*Model.config())["Libraries"].is_object())
        for (const auto & [Name, _] : (*Model.config())["Libraries"].items()) Out << QString::fromStdString(Name);
    return Out;
}

nlohmann::ordered_json FriendSharing::sharedWith(const std::string & Peer) const
{
    std::vector<std::string> Explicit;
    if (Model.config()->contains("Sharing") && (*Model.config())["Sharing"].contains(Peer) && (*Model.config())["Sharing"][Peer].is_array())
        for (const auto & X : (*Model.config())["Sharing"][Peer]) if (X.is_string()) Explicit.push_back(X.get<std::string>());
    nlohmann::ordered_json Shared = PackageCatalog::SharedWith(Model.config()->value("Libraries", nlohmann::ordered_json::object()), Explicit);
    for (const std::string & L : Explicit) if (!Shared.contains(L)) Shared[L] = nlohmann::ordered_json::array();
    return Shared;
}

void FriendSharing::pushSharesTo(const std::string & Peer)
{
    const nlohmann::ordered_json Shared = sharedWith(Peer);
    for (const auto & [L, Items] : Shared.items())
    {
        std::string Err;
        if (!IpfsWrapper::ShareLibrary(Peer, L, Items, &Err))
            LogWarn("FriendSharing::pushSharesTo", "share '" + L + "' with " + Peer + " failed: " + Err);
    }
    std::set<std::string> &Pushed = PushedShares[Peer];
    for (const std::string & L : Pushed)
        if (!Shared.contains(L)) { std::string Err; IpfsWrapper::UnshareLibrary(Peer, L, &Err); }
    Pushed.clear();
    for (const auto & [L, Items] : Shared.items()) { (void)Items; Pushed.insert(L); }
}

void FriendSharing::shareLibraryWithFriend(const QString & peer, const QString & lib)
{
    const std::string P = peer.toStdString(), L = lib.toStdString();
    auto & Sh = (*Model.config())["Sharing"][P];
    if (!Sh.is_array()) Sh = nlohmann::ordered_json::array();
    bool Found = false;
    for (const auto & X : Sh) if (X == L) { Found = true; break; }
    if (!Found) Sh.push_back(L);
    pushSharesTo(P);
    Model.save();
}

void FriendSharing::stopSharingWithFriend(const QString & peer, const QString & lib)
{
    const std::string P = peer.toStdString(), L = lib.toStdString();
    PushedShares[P].insert(L);   // so the push below unshares it even if nothing was ever published for it
    if ((*Model.config()).contains("Sharing") && (*Model.config())["Sharing"].contains(P) && (*Model.config())["Sharing"][P].is_array())
    {
        auto & Sh = (*Model.config())["Sharing"][P];
        for (auto It = Sh.begin(); It != Sh.end(); ++It) if (*It == L) { Sh.erase(It); break; }
        if (Sh.empty()) (*Model.config())["Sharing"].erase(P);
    }
    pushSharesTo(P);
    Model.save();
}

bool FriendSharing::isSharingWithFriend(const QString & peer, const QString & lib) const
{
    const std::string P = peer.toStdString(), L = lib.toStdString();
    if (!Model.config()->contains("Sharing") || !(*Model.config())["Sharing"].is_object()
        || !(*Model.config())["Sharing"].contains(P) || !(*Model.config())["Sharing"][P].is_array())
        return false;
    for (const auto & X : (*Model.config())["Sharing"][P]) if (X == L) return true;
    return false;
}

void FriendSharing::requestFriendLibraries(const QString & peer)
{
    std::string Err;
    if (!IpfsWrapper::RequestFriendLibraries(peer.toStdString(), &Err))
        LogWarn("FriendSharing::requestFriendLibraries", "request to " + peer.toStdString() + " failed: " + Err);
}

void FriendSharing::applyFriendLibrarySnapshot(const QString & peer, const QString & libsJson, quint64 seq)
{
    const std::string P = peer.toStdString();
    // RECEIVE GATE: a friend pushes its snapshot whenever its shares change, asked or not. Nothing of it is kept — not
    // the record, not its sequence stamp — until you choose to receive from them; Receive-on asks for a fresh one.
    if (!isReceivingFrom(peer)) return;
    // SEQ AUTHORITY (last-writer-wins): snapshots ride independent, concurrently-handled streams, so a stale one can be
    // delivered/emitted after a fresher one. Keep only the highest stamp per peer; drop anything older. seq==0
    // (unstamped) always applies. This is where ordering is enforced — a Go-side receive gate can't (its emit is
    // off-lock). Update the high-water mark only after the payload validates, so a malformed stale message can't bump it.
    if (seq != 0)
    {
        const auto It = FriendLibSeq.find(P);
        if (It != FriendLibSeq.end() && seq <= It->second) return;   // reordered straggler / duplicate → ignore
    }
    nlohmann::ordered_json Libs;                        // {name:[{cid,node,uid,title,…},…]} — parse + sanitize the snapshot
    Libs = NodeGraph::ParseBounded(libsJson.toStdString());   // a friend's bytes: never parsed past the depth guard
    if (Libs.is_discarded()) { LogWarn("FriendSharing::friendLibrary", "bad snapshot from " + P + ": not JSON, or nested too deep"); return; }
    if (!Libs.is_object()) return;                      // malformed → ignore, keep what we have
    if (seq != 0) FriendLibSeq[P] = seq;                // payload is well-formed → advance the high-water mark
    nlohmann::ordered_json Clean = nlohmann::ordered_json::object();
    for (const auto & [Name, Arr] : Libs.items())
    {
        if (!Arr.is_array()) continue;
        nlohmann::ordered_json C = nlohmann::ordered_json::array();
        for (const auto & X : Arr) if (X.is_object() && !X.value("cid", std::string()).empty()) C.push_back(X);
        if (!C.empty()) Clean[Name] = std::move(C);
    }
    auto & FL = (*Model.config())["FriendLibraries"];
    if (!FL.is_object()) FL = nlohmann::ordered_json::object();
    const bool Had = FL.contains(P);
    if (Clean.empty())
    {
        if (!Had) return;                               // nothing to drop, nothing changed
        FL.erase(P);                                    // stubs stay: an empty set is also what a seeder pushes while its
                                                        // library is still loading (a restart) — a transient, not a withdrawal
    }
    else
    {
        if (Had && FL[P] == Clean) return;              // identical snapshot → no-op (no disk/UI churn)
        if (Had) dropReceivedStubs(P);                  // a CHANGED snapshot replaces the last one's stubs, never joins them
        FL[P] = std::move(Clean);
    }
    Model.save();
    emit friendCatalogChanged();
    if (isReceivingFrom(peer)) enqueueReceivedShares(peer);   // route the shared blocks into the library tree
}

void FriendSharing::reRegisterShares()
{
    // Go's per-friend share table is in-memory only; config["Sharing"] = {peer:[lib,…]} is the durable record. Replay
    // it into Go with the current config["Libraries"] CIDs so a restart or a re-publish (which can move package CIDs)
    // keeps the seeder half alive.
    if (!Model.config()->contains("Sharing") || !(*Model.config())["Sharing"].is_object()) return;
    for (const auto & [P, Names] : (*Model.config())["Sharing"].items()) { (void)Names; pushSharesTo(P); }
}

bool FriendSharing::hasFriendLibraries(const QString & peer) const
{
    const std::string P = peer.toStdString();
    return Model.config()->contains("FriendLibraries") && (*Model.config())["FriendLibraries"].is_object()
        && (*Model.config())["FriendLibraries"].contains(P) && (*Model.config())["FriendLibraries"][P].is_object()
        && !(*Model.config())["FriendLibraries"][P].empty();
}

void FriendSharing::reconcileReceivedFriend(const QString & peer)
{
    // Make the receiver's view independent of WHEN the snapshot arrived. Two idempotent, self-healing steps:
    if (!isReceivingFrom(peer)) return;
    // 1. Re-enqueue from the snapshot we already hold — satisfied targets (file present + block local) skip, so this
    //    is what RETRIES a fetch that failed while the peer was offline. A fresh library_req reply would NO-OP when
    //    the snapshot is unchanged (seq high-water), so the retry has to come from here.
    if (hasFriendLibraries(peer)) enqueueReceivedShares(peer);
    // 2. Re-request in case their shares changed while we were away (a CHANGED snapshot re-enqueues on apply).
    requestFriendLibraries(peer);
}

void FriendSharing::reconcileReceivedLibraries()
{
    if (!Model.config()->contains("ReceiveFrom") || !(*Model.config())["ReceiveFrom"].is_array()) return;
    for (const auto & X : (*Model.config())["ReceiveFrom"])
        if (X.is_string()) reconcileReceivedFriend(QString::fromStdString(X.get<std::string>()));
}

void FriendSharing::forgetFriend(const QString & peer)
{
    const std::string P = peer.toStdString();
    bool Changed = false;
    if (Model.config()->contains("Sharing") && (*Model.config())["Sharing"].is_object() && (*Model.config())["Sharing"].contains(P))
    { (*Model.config())["Sharing"].erase(P); Changed = true; }   // Go already dropped its side via VgFriendRemove→purgeShares
    // Ending the friendship also stops their still-pending fetches (same as receive-off): plan from the snapshot
    // BEFORE erasing it, cancel what we enqueued, drop the browse stubs. LIBRARY installs are untouched.
    dropReceivedStubs(P);
    if (Model.config()->contains("FriendLibraries") && (*Model.config())["FriendLibraries"].is_object()
        && (*Model.config())["FriendLibraries"].contains(P))
    { (*Model.config())["FriendLibraries"].erase(P); Changed = true; }   // drops the peer's packages from future reconciles
    auto EraseFrom = [&](const char * Key, nlohmann::ordered_json & Root) {
        if (Root.contains(Key) && Root[Key].is_array())
            for (auto It = Root[Key].begin(); It != Root[Key].end(); ++It)
                if (*It == P) { Root[Key].erase(It); Changed = true; break; }
    };
    EraseFrom("ReceiveFrom", *Model.config());
    bool DenyChanged = false;
    if (Model.config()->contains("PresenceDeny") && (*Model.config())["PresenceDeny"].is_array())
        for (auto It = (*Model.config())["PresenceDeny"].begin(); It != (*Model.config())["PresenceDeny"].end(); ++It)
            if (*It == P) { (*Model.config())["PresenceDeny"].erase(It); Changed = true; DenyChanged = true; break; }
    if (Model.config()->contains("Settings") && (*Model.config())["Settings"].is_object())
        EraseFrom("LanExcludedPeers", (*Model.config())["Settings"]);
    FriendLibSeq.erase(P);   // forget the seq high-water mark too, so a re-add starts fresh (no stale-drop of the first snapshot)
    if (DenyChanged) pushPresenceDeny();
    if (Changed) { Model.save(); Model.rebuildCatalog(); emit friendCatalogChanged(); }
}

// ── Network tab per-peer toggles ─────────────────────────────────────────────

static bool ArrHas(const nlohmann::ordered_json & Arr, const std::string & V)
{
    if (!Arr.is_array()) return false;
    for (const auto & X : Arr) if (X == V) return true;
    return false;
}

bool FriendSharing::isReceivingFrom(const QString & peer) const
{
    return Model.config()->contains("ReceiveFrom") && ArrHas((*Model.config())["ReceiveFrom"], peer.toStdString());
}

void FriendSharing::setReceivingFrom(const QString & peer, bool on)
{
    const std::string P = peer.toStdString();
    auto & RF = (*Model.config())["ReceiveFrom"];
    if (!RF.is_array()) RF = nlohmann::ordered_json::array();
    const bool Present = ArrHas(RF, P);
    if (on == Present) return;                       // no change
    if (on) RF.push_back(P);
    else    for (auto It = RF.begin(); It != RF.end(); ++It) if (*It == P) { RF.erase(It); break; }
    Model.save();
    if (on) reconcileReceivedFriend(peer);           // ask for their snapshot: none is kept before Receive is on
    else    stopReceivingFromFriend(peer);           // drop what we've already materialised
}

void FriendSharing::dropReceivedStubs(const std::string & P)
{
    // A friend's received library is a SNAPSHOT: the roots they share now, and the node closure under them. It lives
    // in CATALOG/<Nick> - <Lib>/ as browse stubs (blocks only; installs are in LIBRARY). Whenever that snapshot goes
    // away or is replaced, the stubs go with it — otherwise every publish they ever made stays on disk beside the
    // current one (three "v1.30.4" of three generations, each indexed, each a card). Plans from the snapshot we hold
    // to name this peer's dirs and CIDs, cancels what we enqueued for them, removes the dirs.
    // A landing pass in flight works from a config snapshot taken before this drop and could write the old
    // generation's files right back into the dirs removed here: remember to drop once more when it ends.
    if (FriendClosureRunning) FriendStubsDirtyPeers.insert(P);
    // The dirs come from the snapshot we hold — or, once that is gone (receive-off / forget erased it), from what
    // this peer's last drop swept: a late block from the pass can still recreate a dir after the record is gone.
    std::set<std::filesystem::path> LibDirs;   // CATALOG/<Nick> - <Lib> subtrees (pure browse stubs)
    if (Model.config()->contains("FriendLibraries") && (*Model.config())["FriendLibraries"].is_object()
        && (*Model.config())["FriendLibraries"].contains(P) && (*Model.config())["FriendLibraries"][P].is_object())
    {
        for (const auto & T : PackageCatalog::PlanReceivedFetches(*Model.config(), PackageCatalog::ReceivedNickLabel(P),
                                                                  (*Model.config())["FriendLibraries"][P]))
        {
            LibDirs.insert(std::filesystem::path(T.Dest).parent_path().parent_path());   // …/<Nick> - <Lib>
            if (!FriendBrowseCids.erase(T.Cid)) continue;   // only cancel what WE enqueued for this browse flow
            IpfsWrapper::CancelDownload(T.Cid);
        }
    }
    else if (const auto It = DroppedStubDirs.find(P); It != DroppedStubDirs.end()) LibDirs = It->second;
    if (LibDirs.empty()) return;
    DroppedStubDirs[P] = LibDirs;
    // A received package that was INSTALLED is fetched into the same CATALOG/<Nick> - <Lib>/<Pkg> dir the stubs
    // live in (there is no adopt-to-LIBRARY step), so the dir is removed per PACKAGE and only when it holds nothing
    // but node files: a dir with content is an install and stays, whatever the snapshot says. Its stale node files
    // are overwritten or joined by the new generation's — the installed bytes are never the thing at risk.
    std::error_code Ec;
    int Removed = 0, KeptN = 0;
    for (const auto & D : LibDirs)
    {
        if (!std::filesystem::is_directory(D, Ec)) continue;
        bool Kept = false;
        for (const auto & E : std::filesystem::directory_iterator(D, Ec))
        {
            if (!E.is_directory(Ec)) { std::filesystem::remove(E.path(), Ec); continue; }
            if (PackageCatalog::DirHasContent(E.path().string())) { Kept = true; ++KeptN; continue; }   // an install — never
            IpfsWrapper::ForgetDestsUnder(E.path().string());   // the queue must not re-materialise these paths later
            std::filesystem::remove_all(E.path(), Ec); ++Removed;
        }
        if (!Kept) { IpfsWrapper::ForgetDestsUnder(D.string()); std::filesystem::remove_all(D, Ec); }
    }
    LogOut("FriendSharing::dropReceivedStubs", "friend " + P.substr(P.size() > 8 ? P.size() - 8 : 0) + ": " + std::to_string(Removed)
           + " stub package dir(s) removed, " + std::to_string(KeptN) + " installed package(s) kept");
}

void FriendSharing::stopReceivingFromFriend(const QString & peer)
{
    const std::string P = peer.toStdString();
    bool Changed = false;
    // Receive-OFF must also stop the peer's still-pending fetches — an explicit user stop, so their data must not
    // keep arriving into the library. Re-plan from the snapshot (cheap, pure) to name this peer's CIDs, cancel each
    // in the queue, and drop them from the landed-block reaction set. Files already landed stay (they are ordinary
    // packages now; withdrawal never deletes).
    dropReceivedStubs(P);
    // Dropping the peer's snapshot removes its packages from future reconciles (enqueueReceivedShares plans only
    // from FriendLibraries of peers we still Receive from).
    if (Model.config()->contains("FriendLibraries") && (*Model.config())["FriendLibraries"].is_object()
        && (*Model.config())["FriendLibraries"].contains(P))
    { (*Model.config())["FriendLibraries"].erase(P); Changed = true; }
    FriendLibSeq.erase(P);
    if (Changed) { Model.save(); Model.rebuildCatalog(); emit friendCatalogChanged(); }
}

bool FriendSharing::isPresenceSharedWith(const QString & peer) const
{
    return !(Model.config()->contains("PresenceDeny") && ArrHas((*Model.config())["PresenceDeny"], peer.toStdString()));
}

void FriendSharing::setPresenceSharedWith(const QString & peer, bool on)
{
    const std::string P = peer.toStdString();
    auto & D = (*Model.config())["PresenceDeny"];
    if (!D.is_array()) D = nlohmann::ordered_json::array();
    const bool Denied = ArrHas(D, P);
    if (on && Denied)  { for (auto It = D.begin(); It != D.end(); ++It) if (*It == P) { D.erase(It); break; } }
    else if (!on && !Denied) D.push_back(P);
    else return;                                     // no change
    Model.save();
    pushPresenceDeny();
}

void FriendSharing::pushPresenceDeny()
{
    std::vector<std::string> D;
    if (Model.config()->contains("PresenceDeny") && (*Model.config())["PresenceDeny"].is_array())
        for (const auto & X : (*Model.config())["PresenceDeny"]) if (X.is_string()) D.push_back(X.get<std::string>());
    IpfsWrapper::SetPresenceDeny(D);
}

bool FriendSharing::isInVlan(const QString & peer) const
{
    if (!Model.config()->contains("Settings") || !(*Model.config())["Settings"].is_object()) return true;
    return !ArrHas((*Model.config())["Settings"].value("LanExcludedPeers", nlohmann::ordered_json::array()), peer.toStdString());
}

void FriendSharing::setInVlan(const QString & peer, bool on)
{
    const std::string P = peer.toStdString();
    auto & E = (*Model.config())["Settings"]["LanExcludedPeers"];
    if (!E.is_array()) E = nlohmann::ordered_json::array();
    const bool Excluded = ArrHas(E, P);
    if (on && Excluded)  { for (auto It = E.begin(); It != E.end(); ++It) if (*It == P) { E.erase(It); break; } }
    else if (!on && !Excluded) E.push_back(P);
    else return;                                     // no change
    Model.save();
    pushLanRoster();
}

void FriendSharing::acceptPeer(const QString & peer)
{
    std::string Err;
    if (!IpfsWrapper::FriendAccept(peer.toStdString(), &Err))
    { LogWarn("FriendSharing::acceptPeer", "accept " + peer.toStdString() + " failed: " + Err); return; }
    applyNewPeerDefaults(peer);
}

void FriendSharing::applyNewPeerDefaults(const QString & peer)
{
    setPresenceSharedWith(peer, newPeerDefault("presence"));
    setInVlan(peer, newPeerDefault("vlan"));
    for (const QString & Lib : newPeerShareDefaults()) shareLibraryWithFriend(peer, Lib);
    if (newPeerDefault("receive")) setReceivingFrom(peer, true);
}

bool FriendSharing::autoAcceptEnabled() const { return Model.config()->value("AutoAcceptPeers", false); }

void FriendSharing::setAutoAcceptEnabled(bool on)
{
    if (on == autoAcceptEnabled()) return;
    (*Model.config())["AutoAcceptPeers"] = on;
    Model.save();
}

bool FriendSharing::newPeerDefault(const QString & key) const
{
    const std::string K = key.toStdString();
    const bool Fallback = (K == "presence");         // presence ON by default; receive / vlan OFF
    if (!Model.config()->contains("NewPeerDefaults") || !(*Model.config())["NewPeerDefaults"].is_object()) return Fallback;
    return (*Model.config())["NewPeerDefaults"].value(K, Fallback);
}

void FriendSharing::setNewPeerDefault(const QString & key, bool on)
{
    auto & D = (*Model.config())["NewPeerDefaults"];
    if (!D.is_object()) D = nlohmann::ordered_json::object();
    D[key.toStdString()] = on;
    Model.save();
}

QStringList FriendSharing::newPeerShareDefaults() const
{
    QStringList Out;
    if (Model.config()->contains("NewPeerDefaults") && (*Model.config())["NewPeerDefaults"].is_object()
        && (*Model.config())["NewPeerDefaults"].contains("share") && (*Model.config())["NewPeerDefaults"]["share"].is_array())
        for (const auto & X : (*Model.config())["NewPeerDefaults"]["share"])
            if (X.is_string()) Out << QString::fromStdString(X.get<std::string>());
    return Out;
}

void FriendSharing::setNewPeerShareDefault(const QString & lib, bool on)
{
    auto & D = (*Model.config())["NewPeerDefaults"];
    if (!D.is_object()) D = nlohmann::ordered_json::object();
    auto & Sh = D["share"];
    if (!Sh.is_array()) Sh = nlohmann::ordered_json::array();
    const std::string L = lib.toStdString();
    const bool In = ArrHas(Sh, L);
    if (on && !In) Sh.push_back(L);
    else if (!on && In) { for (auto It = Sh.begin(); It != Sh.end(); ++It) if (*It == L) { Sh.erase(It); break; } }
    else return;
    Model.save();
}

void FriendSharing::enqueueReceivedShares(const QString & peer)
{
    // Receiver = ONE step, zero special machinery: each shared PACKAGE FOLDER becomes a plain folder FetchTarget whose
    // dest is its final dir — CATALOG/<Nick> - <Lib>/<pkg>/.package/ — computed from the snapshot's routing metadata
    // (PackageCatalog::PlanReceivedFetches). The ONE rolling queue lands every node file verbatim and pins it like any
    // file; from there the ordinary catalog scan / hydration / install / re-publish handle it (a received library
    // re-publishes to identical CIDs — the multi-seeder design). Re-runs on node-ready, presence-online and snapshot
    // apply; a satisfied target is skipped, so repeats converge to a no-op. A re-publish (a NEW folder, same dest)
    // re-enqueues naturally: the new folder isn't local yet.
    if (!isReceivingFrom(peer)) return;
    const std::string P = peer.toStdString();
    if (!Model.config()->contains("FriendLibraries") || !(*Model.config())["FriendLibraries"].is_object()
        || !(*Model.config())["FriendLibraries"].contains(P) || !(*Model.config())["FriendLibraries"][P].is_object()) return;

    const auto Plan = PackageCatalog::PlanReceivedFetches(*Model.config(), PackageCatalog::ReceivedNickLabel(P),
                                                          (*Model.config())["FriendLibraries"][P]);
    // No local "satisfied" guess: presence and even HasLocal both LIE for reusable node dests (HasLocal is GLOBAL
    // block membership — another friend's fetch of v2 would mark this friend's stale v1 file settled forever). Every
    // planned target is enqueued with Verify semantics: the QUEUE settles repeats for free (a dest its job already
    // wrote is a no-op at enqueue), and the Go fetch hash-verifies a reused dest exactly.
    for (const auto & T : Plan) FriendBrowseCids.insert(T.Cid);
    const std::vector<IpfsWrapper::FetchTarget> Batch = PackageCatalog::ReceivedFetchTargets(Plan);
    if (!Batch.empty()) IpfsWrapper::EnqueueBatch(Batch);
}

void FriendSharing::onFriendBlockLanded(const QString & cid, bool /*ok*/)
{
    if (!FriendBrowseCids.count(cid.toStdString())) return;   // not a received-share node block -> ignore
    if (FriendReconcilePending) return;                       // coalesce a burst of completions into one rebuild
    FriendReconcilePending = true;
    QTimer::singleShot(2000, this, [this] {
        FriendReconcilePending = false;
        Model.rebuildCatalog();               // the landed blocks ARE ordinary tree packages — just re-scan
        emit friendCatalogChanged();
        completeReceivedClosures();
    });
}

void FriendSharing::completeReceivedClosures()
{
    // A friend's share is a set of package folders, closed over what they contain. The receiver lands every node file
    // each folder lists (blocks only — kilobytes per game; content stays lazy until install), so it holds the same
    // graph the seeder holds and derives the same cards, offers and needs from it. Re-entrant-safe: a share changing
    // while a pass runs queues exactly one more pass.
    if (FriendClosureRunning) { FriendClosureAgain = true; return; }
    //Each package lands whole, in its own folder; a node a package contains comes with ITS package (a share is
    //closed over what its packages contain), never fetched into the package that needs it.
    if (!PackageCatalog::ReceivedPackagesIncomplete(*Model.config()))
    {   // nothing to land: still drop what no landed folder lists (older generations, an older build's copies)
        if (PackageCatalog::PruneStaleReceived(*Model.catalogSnapshot(), *Model.config()) > 0) { Model.rebuildCatalog(); emit friendCatalogChanged(); }
        return;
    }
    FriendClosureRunning = true;
    LogOut("FriendSharing::completeReceivedClosures", "landing received package(s)");
    auto Cfg  = std::make_shared<const nlohmann::ordered_json>(*Model.config());
    AsyncWork::Run(this,   // the pass touches nothing of this model: it can outlive it
        [Cfg]{
            const IpfsWrapper::ForegroundHold Hold;
            std::string PErr;
            if (!PackageCatalog::LandReceivedPackages(*Cfg, &PErr))
                LogWarn("FriendSharing::completeReceivedClosures", "received package(s): " + PErr);
        },
        [this]{
            FriendClosureRunning = false;
            if (!FriendStubsDirtyPeers.empty())
            {   // a drop happened mid-pass: what the pass wrote after it is the old generation's — drop those peers'
                // stubs again (a stopped peer's dirs are remembered, so this works after its record is gone), re-enqueue
                const std::set<std::string> Peers = std::move(FriendStubsDirtyPeers);
                FriendStubsDirtyPeers.clear();
                for (const std::string & P : Peers) dropReceivedStubs(P);
                FriendStubsDirtyPeers.clear();   // the drops above ran with no pass in flight: nothing new to remember
                reconcileReceivedLibraries();    // the CURRENT generation's roots, into the now-clean dirs
            }
            Model.rebuildCatalog();           // the landed closure blocks are ordinary tree nodes, marked Received by the scan
            if (const int Pruned = PackageCatalog::PruneStaleReceived(*Model.catalogSnapshot(), *Model.config()); Pruned > 0)
            {   // older generations' node files in kept (installed) dirs — gone now that the current ones landed
                LogOut("FriendSharing::completeReceivedClosures", "pruned " + std::to_string(Pruned) + " stale received node file(s)");
                Model.rebuildCatalog();
            }
            emit friendCatalogChanged();
            if (FriendClosureAgain) { FriendClosureAgain = false; completeReceivedClosures(); }
            else if (PackageCatalog::ReceivedPackagesIncomplete(*Model.config()))
                completeReceivedClosures();   // packages still to land (a share that changed meanwhile)
        });
}
bool FriendSharing::sendFriendRequest(const QString & code, std::string * error)
{
    return IpfsWrapper::FriendAdd(code.trimmed().toStdString(), std::string(), error);
}

bool FriendSharing::setNickname(const QString & nick, std::string * error)
{
    return IpfsWrapper::SetProfile(nick.trimmed().toStdString(), std::string(), error);
}

void FriendSharing::declineFriend(const QString & peer)
{
    IpfsWrapper::FriendDecline(peer.toStdString());
    forgetFriend(peer);
}

void FriendSharing::blockFriend(const QString & peer)
{
    IpfsWrapper::FriendBlock(peer.toStdString());
    forgetFriend(peer);
}

void FriendSharing::removeFriend(const QString & peer)
{
    IpfsWrapper::FriendRemove(peer.toStdString());
    forgetFriend(peer);
}
