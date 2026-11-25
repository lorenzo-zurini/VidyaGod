#include <set>
#include "peersharing.h"
#include "appmodel.h"
#include "asyncwork.h"
#include "commonutils.h"
#include "downloadqueue.h"   // IpfsWrapper::EnqueueBatch — the ONE rolling queue that browse fetches ride
#include "ipfswrapper.h"
#include "nodegraph.h"
#include "nodestore.h"
#include "packagecatalog.h"

#include <QDateTime>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

PeerSharing::PeerSharing(AppModel & M)
    : QObject(&M), Model(M)
{
    // A peer sent us their COMPLETE shared set (a snapshot, per the bilateral protocol — pushed on any share change
    // and on our explicit requestPeerLibraries). We REPLACE our record for that peer wholesale: this is the only way
    // a withdraw can't be lost (a missed single "unshare" would otherwise leave a phantom entry). "{}" = they share
    // nothing with us → drop the peer. Persist + refresh the peer-catalog view ONLY when something actually changed,
    // so a redundant snapshot (e.g. a periodic re-push) doesn't churn the disk or the UI.
    connect(PeersManager::instance(), &PeersManager::peerLibrary, this,
        [this](const QString & peer, const QString & libsJson, quint64 seq) { applyPeerLibrarySnapshot(peer, libsJson, seq); });

    // Self-heal on reachability: when a peer we RECEIVE from comes online, re-materialise + re-request so the catalog
    // fills regardless of the order shares/accepts/toggles happened in (a node-ready request can fire before the peer
    // connection is up and fail silently; this retries the moment they're reachable). Fires only on the online transition.
    //A peer seen online again within PresenceReconcileMs is not re-enqueued again: on a flaky network its presence
    //flaps, and each "online" re-planned and re-enqueued its whole library (a fetch it stalled meanwhile retries on
    //its own: the queue rotates it, it never fails for a peer being away). It is still asked for its libraries — one
    //small request; a request made while it was away failed, and a changed snapshot re-enqueues when it lands.
    connect(PeersManager::instance(), &PeersManager::peerPresence, this,
        [this](const QString & peer, bool online) {
            if (!online) return;
            const qint64 Now = QDateTime::currentMSecsSinceEpoch();
            qint64 & Last = PresenceReconciled[peer];
            if (Last && Now - Last < PresenceReconcileMs)
            {
                if (isReceivingFrom(peer)) requestPeerLibraries(peer);
                return;
            }
            Last = Now;
            ++PresenceReconciles;
            reconcileReceivedPeer(peer);
        });

    // Periodic reconcile: presence-online and snapshot-apply are EDGE-triggered — a seeder killed mid-transfer and
    // restarted while we saw no edge (or a missed presence event) would otherwise leave the receiver waiting
    // forever. Every 10 min, re-plan + re-enqueue each receiving-from peer; the queue settles repeats silently
    // (a dest its job already wrote is a no-op at enqueue, and awaitReceived waits only for what is still fetched),
    // so steady state costs nothing visible.
    {
        QTimer * Resync = new QTimer(this);
        Resync->setInterval(10 * 60 * 1000);
        connect(Resync, &QTimer::timeout, this, [this]{ reconcileReceivedLibraries(); });
        Resync->start();
    }

    // What is shared stays current: re-checked when the library changes, a publish ends, networking comes up.
    connect(&Model, &AppModel::catalogChanged, this, &PeerSharing::checkSharesSoon);
    connect(&Model, &AppModel::nodeReady, this, [this] {   // a node up again: what failed is worth another try
        Attempted.clear();
        FailedPackages.clear();
        Alone.clear();
        checkSharesSoon();
    });
    connect(&Model, &AppModel::libraryPublished, this, [this] {
        for (const Queued & Q : InFlight) { FailedPackages.erase(Q.Key); Alone.erase(Q.Key); }
        if (InFlight.empty() && !WholeInFlight) LastPublishError.clear();
        InFlight.clear();
        WholeInFlight = false;
        checkSharesSoon();   // what still waits is checked against the library first, then published
    });
    connect(&Model, &AppModel::libraryPublishFailed, this, [this](const QString & M) {
        if (M == QLatin1String(PackageCatalog::LibraryBusy))
        {   // another tool writes the library: what was being published waits for it, and goes again after
            const bool Whole = WholeInFlight;
            Queue.insert(Queue.begin(), InFlight.begin(), InFlight.end());
            InFlight.clear();
            WholeInFlight = false;
            setState(ShareState::Updating, tr("Waiting for another tool writing the library; trying again shortly."));
            QTimer::singleShot(BusyRetryMs, this, [this, Whole] { if (Whole) prepareSharing(); else { pump(); checkSharesSoon(); } });
            return;
        }
        if (InFlight.size() == 1) FailedPackages[InFlight.front().Key] = M;   // that package's: the others go on
        else if (InFlight.size() > 1)                                         // a batch: each again, alone
            for (const Queued & Q : InFlight) { Alone.insert(Q.Key); Queue.push_back(Q); }
        else LastPublishError = M;
        InFlight.clear();
        WholeInFlight = false;
        checkSharesSoon();
    });

    // Auto-accept ("server mode"): if enabled, accept every incoming peer request automatically and apply the
    // New-Peer defaults. Runs whether or not the Network tab is open.
    //The peer ended the pairing (or we blocked it): what was chosen for it ends with it — its permissions, its received
    //library, what we share with it. A pairing made again starts from fresh ticks.
    connect(PeersManager::instance(), &PeersManager::peerDeclined, this, [this](const QString & peer) { forgetPeer(peer); });
    connect(PeersManager::instance(), &PeersManager::peerRequest, this,
        [this](const QString & peer, const QString &, const QString &) { if (autoAcceptEnabled()) acceptPeer(peer); });

    // A peer becomes a peer here whichever side accepted (our acceptPairing, theirs, a crossing).
    connect(PeersManager::instance(), &PeersManager::peerAccepted, this,
        [this](const QString & peer, const QString &, const QString &) {
            //Nobody chose for it (paired by a code with nothing ticked, a crossing): it sees nothing of ours and we keep
            //nothing of its — the receive gate's consent is a tick, never a default. Only a peer accepted by itself
            //(server mode) gets the defaults, there (acceptPeer). Presence and the virtual LAN take theirs.
            if (!hasChosen(peer))
            {
                setPresenceSharedWith(peer, newPeerDefault("presence"));
                setInVlan(peer, newPeerDefault("vlan"));
            }
            //What was ticked before it was a peer (pairing's ticks, set with the request) goes now: a push or an ask
            //made while it was not one was dropped there (peer.go only serves peers), and the next try would be
            //its next coming online.
            const std::string P = peer.toStdString();
            const auto & Sh = Model.config()->value("Sharing", nlohmann::ordered_json::object());
            if (Sh.contains(P) && Sh[P].is_array() && !Sh[P].empty()) pushSharesTo(P);
            if (isReceivingFrom(peer)) reconcileReceivedPeer(peer);
        });
}

// Push Settings.LanExcludedPeers (the GLOBAL Virtual-LAN roster's un-ticked members) into the node. Called at
// node-ready; the launch window's ticks call IpfsWrapper::SetLanExcluded directly on change.
void PeerSharing::pushLanRoster()
{
    std::vector<std::string> Ex;
    const auto & S = (*Model.config())["Settings"];
    if (S.contains("LanExcludedPeers") && S["LanExcludedPeers"].is_array())
        for (const auto & P : S["LanExcludedPeers"])
            if (P.is_string()) Ex.push_back(P.get<std::string>());
    IpfsWrapper::SetLanExcluded(Ex);
}
QStringList PeerSharing::myLibraryNames() const
{
    const std::shared_ptr<const NodeIndex> Idx = Model.catalogSnapshot();
    const auto & Cfg = *Model.config();
    const size_t Sheet = Cfg.contains("ShareSheet") && Cfg["ShareSheet"].is_object() ? Cfg["ShareSheet"].size() : 0;
    if (Idx && Model.catalogGeneration() == NamesOf && Sheet == NamesSheet) return Names;   // read once per index (asked per row, per poll)
    QStringList Out;
    if (Idx)
    {
        for (const std::string & N : PackageCatalog::LibrariesOnDisk(Cfg, *Idx)) Out << QString::fromStdString(N);
        NamesOf = Model.catalogGeneration();
        NamesSheet = Sheet;
        Names = Out;
    }
    else
    {
        std::set<std::string> Names;
        for (const auto & E : PackageCatalog::SheetEntries(Model.config()->value("ShareSheet", nlohmann::ordered_json::object())))
            Names.insert(E.Lib);
        for (const std::string & N : Names) Out << QString::fromStdString(N);
    }
    return Out;
}

nlohmann::ordered_json PeerSharing::sharedWith(const std::string & Peer) const
{
    std::vector<std::string> Explicit;
    if (Model.config()->contains("Sharing") && (*Model.config())["Sharing"].contains(Peer) && (*Model.config())["Sharing"][Peer].is_array())
        for (const auto & X : (*Model.config())["Sharing"][Peer]) if (X.is_string()) Explicit.push_back(X.get<std::string>());
    const std::shared_ptr<const NodeIndex> Idx = Model.catalogSnapshot();
    nlohmann::ordered_json Shared = PackageCatalog::SharedWith(Model.config()->value("ShareSheet", nlohmann::ordered_json::object()), Explicit,
        [&](const std::string & Cid) { return PackageCatalog::NodeContains(*Idx, Cid); });
    for (const std::string & L : Explicit) if (!Shared.contains(L)) Shared[L] = nlohmann::ordered_json::array();
    return Shared;
}

void PeerSharing::pushSharesTo(const std::string & Peer)
{
    const nlohmann::ordered_json Shared = sharedWith(Peer);
    for (const auto & [L, Items] : Shared.items())
    {
        std::string Err;
        if (!IpfsWrapper::ShareLibrary(Peer, L, Items, &Err))
            LogWarn("PeerSharing::pushSharesTo", "share '" + L + "' with " + Peer + " failed: " + Err);
    }
    std::set<std::string> &Pushed = PushedShares[Peer];
    for (const std::string & L : Pushed)
        if (!Shared.contains(L)) { std::string Err; IpfsWrapper::UnshareLibrary(Peer, L, &Err); }
    Pushed.clear();
    for (const auto & [L, Items] : Shared.items()) { (void)Items; Pushed.insert(L); }
}

void PeerSharing::shareLibraryWithPeer(const QString & peer, const QString & lib)
{
    markChosen(peer);
    const std::string P = peer.toStdString(), L = lib.toStdString();
    auto & Sh = (*Model.config())["Sharing"][P];
    if (!Sh.is_array()) Sh = nlohmann::ordered_json::array();
    bool Found = false;
    for (const auto & X : Sh) if (X == L) { Found = true; break; }
    if (!Found) Sh.push_back(L);
    pushSharesTo(P);
    Model.save();
    checkSharesSoon();
}

void PeerSharing::stopSharingWithPeer(const QString & peer, const QString & lib)
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

bool PeerSharing::isSharingWithPeer(const QString & peer, const QString & lib) const
{
    const std::string P = peer.toStdString(), L = lib.toStdString();
    if (!Model.config()->contains("Sharing") || !(*Model.config())["Sharing"].is_object()
        || !(*Model.config())["Sharing"].contains(P) || !(*Model.config())["Sharing"][P].is_array())
        return false;
    for (const auto & X : (*Model.config())["Sharing"][P]) if (X == L) return true;
    return false;
}

void PeerSharing::requestPeerLibraries(const QString & peer)
{
    ++LibraryRequests;
    std::string Err;
    if (!IpfsWrapper::RequestPeerLibraries(peer.toStdString(), &Err))
        LogWarn("PeerSharing::requestPeerLibraries", "request to " + peer.toStdString() + " failed: " + Err);
}

void PeerSharing::applyPeerLibrarySnapshot(const QString & peer, const QString & libsJson, quint64 seq)
{
    const std::string P = peer.toStdString();
    // RECEIVE GATE: a peer pushes its snapshot whenever its shares change, asked or not. Nothing of it is kept — not
    // the record, not its sequence stamp — until you choose to receive from them; Receive-on asks for a fresh one.
    if (!isReceivingFrom(peer)) return;
    PackageCatalog::ForgetRefusedNodes();   // a new snapshot: what it names is tried afresh
    // SEQ AUTHORITY (last-writer-wins): snapshots ride independent, concurrently-handled streams, so a stale one can be
    // delivered/emitted after a fresher one. Keep only the highest stamp per peer; drop anything older. seq==0
    // (unstamped) always applies. This is where ordering is enforced — a Go-side receive gate can't (its emit is
    // off-lock). Update the high-water mark only after the payload validates, so a malformed stale message can't bump it.
    if (seq != 0)
    {
        const auto It = PeerLibSeq.find(P);
        if (It != PeerLibSeq.end() && seq <= It->second) return;   // reordered straggler / duplicate → ignore
    }
    nlohmann::ordered_json Libs;                        // {name:[{cid,node,uid,title,…},…]} — parse + sanitize the snapshot
    Libs = NodeGraph::ParseBounded(libsJson.toStdString());   // a peer's bytes: never parsed past the depth guard
    if (Libs.is_discarded()) { LogWarn("PeerSharing::peerLibrary", "bad snapshot from " + P + ": not JSON, or nested too deep"); return; }
    if (!Libs.is_object()) return;                      // malformed → ignore, keep what we have
    if (seq != 0) PeerLibSeq[P] = seq;                // payload is well-formed → advance the high-water mark
    nlohmann::ordered_json Clean = nlohmann::ordered_json::object();
    for (const auto & [Name, Arr] : Libs.items())
    {
        if (!Arr.is_array()) continue;
        nlohmann::ordered_json C = nlohmann::ordered_json::array();
        for (const auto & X : Arr) if (X.is_object() && !X.value("cid", std::string()).empty()) C.push_back(X);
        if (!C.empty()) Clean[Name] = std::move(C);
    }
    std::optional<std::vector<PackageCatalog::ReceivedFetch>> NewPlan;   // the new snapshot's plan, once made
    auto & FL = (*Model.config())["PeerLibraries"];
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
        if (Had)
        {   // a CHANGED snapshot: the packages it no longer names go; the ones it still names stay, and only what changed
            // in them lands again (the prune drops what their entries no longer name)
            NewPlan = PackageCatalog::PlanReceivedFetches(*Model.config(), PackageCatalog::ReceivedNickLabel(P), Clean);
            std::map<std::string, std::string> Keep;
            for (const auto & T : *NewPlan) Keep[T.Dest] = T.Cid;
            dropReceivedStubs(P, Keep);
        }
        FL[P] = std::move(Clean);
    }
    Model.save();
    emit peerCatalogChanged();
    //Route the shared blocks into the library tree. The plan of the new snapshot made above (none, when the
    //snapshot is gone): the folders a plan names hang on the config's other peers too, unchanged meanwhile.
    if (isReceivingFrom(peer)) enqueueReceivedShares(peer, NewPlan ? &*NewPlan : nullptr);
}

void PeerSharing::reRegisterShares()
{
    // Go's per-peer share table is in-memory only; config["Sharing"] = {peer:[lib,…]} is the durable record. Replay
    // it into Go with the current share sheet so a restart or a re-publish (which moves node CIDs)
    // keeps the seeder half alive.
    std::set<std::string> Peers;
    const auto & Cfg = *Model.config();
    if (Cfg.contains("Sharing") && Cfg["Sharing"].is_object())
        for (const auto & [P, Names] : Cfg["Sharing"].items()) { (void)Names; Peers.insert(P); }
    for (const std::string & P : Peers) pushSharesTo(P);
}

bool PeerSharing::hasPeerLibraries(const QString & peer) const
{
    const std::string P = peer.toStdString();
    return Model.config()->contains("PeerLibraries") && (*Model.config())["PeerLibraries"].is_object()
        && (*Model.config())["PeerLibraries"].contains(P) && (*Model.config())["PeerLibraries"][P].is_object()
        && !(*Model.config())["PeerLibraries"][P].empty();
}

void PeerSharing::reconcileReceivedPeer(const QString & peer)
{
    // Make the receiver's view independent of WHEN the snapshot arrived. Two idempotent, self-healing steps:
    if (!isReceivingFrom(peer)) return;
    // 1. Re-enqueue from the snapshot we already hold — satisfied targets (file present + block local) skip, so this
    //    is what RETRIES a fetch that failed while the peer was offline. A fresh library_req reply would NO-OP when
    //    the snapshot is unchanged (seq high-water), so the retry has to come from here.
    if (hasPeerLibraries(peer)) enqueueReceivedShares(peer);
    // 2. Re-request in case their shares changed while we were away (a CHANGED snapshot re-enqueues on apply).
    requestPeerLibraries(peer);
}

void PeerSharing::reconcileReceivedLibraries()
{
    if (!Model.config()->contains("ReceiveFrom") || !(*Model.config())["ReceiveFrom"].is_array()) return;
    for (const auto & X : (*Model.config())["ReceiveFrom"])
        if (X.is_string()) reconcileReceivedPeer(QString::fromStdString(X.get<std::string>()));
}

void PeerSharing::forgetPeer(const QString & peer)
{
    const std::string P = peer.toStdString();
    bool Changed = false;
    if (Model.config()->contains("Sharing") && (*Model.config())["Sharing"].is_object() && (*Model.config())["Sharing"].contains(P))
    { (*Model.config())["Sharing"].erase(P); Changed = true; }   // Go already dropped its side via VgPeerRemove→purgeShares
    // Ending the pairing also stops their still-pending fetches (same as receive-off): plan from the snapshot
    // BEFORE erasing it, cancel what we enqueued, drop the browse stubs. LIBRARY installs are untouched.
    dropReceivedStubs(P);
    if (Model.config()->contains("PeerLibraries") && (*Model.config())["PeerLibraries"].is_object()
        && (*Model.config())["PeerLibraries"].contains(P))
    { (*Model.config())["PeerLibraries"].erase(P); Changed = true; }   // drops the peer's packages from future reconciles
    auto EraseFrom = [&](const char * Key, nlohmann::ordered_json & Root) {
        if (Root.contains(Key) && Root[Key].is_array())
            for (auto It = Root[Key].begin(); It != Root[Key].end(); ++It)
                if (*It == P) { Root[Key].erase(It); Changed = true; break; }
    };
    EraseFrom("ReceiveFrom", *Model.config());
    EraseFrom("PeerChosen", *Model.config());
    bool DenyChanged = false;
    if (Model.config()->contains("PresenceDeny") && (*Model.config())["PresenceDeny"].is_array())
        for (auto It = (*Model.config())["PresenceDeny"].begin(); It != (*Model.config())["PresenceDeny"].end(); ++It)
            if (*It == P) { (*Model.config())["PresenceDeny"].erase(It); Changed = true; DenyChanged = true; break; }
    if (Model.config()->contains("Settings") && (*Model.config())["Settings"].is_object())
        EraseFrom("LanExcludedPeers", (*Model.config())["Settings"]);
    PeerLibSeq.erase(P);   // forget the seq high-water mark too, so a re-add starts fresh (no stale-drop of the first snapshot)
    if (DenyChanged) pushPresenceDeny();
    if (Changed) { Model.save(); Model.rebuildCatalog(); emit peerCatalogChanged(); }
}

// ── Network tab per-peer toggles ─────────────────────────────────────────────

static bool ArrHas(const nlohmann::ordered_json & Arr, const std::string & V)
{
    if (!Arr.is_array()) return false;
    for (const auto & X : Arr) if (X == V) return true;
    return false;
}

bool PeerSharing::isReceivingFrom(const QString & peer) const
{
    return Model.config()->contains("ReceiveFrom") && ArrHas((*Model.config())["ReceiveFrom"], peer.toStdString());
}

void PeerSharing::setReceivingFrom(const QString & peer, bool on)
{
    markChosen(peer);
    const std::string P = peer.toStdString();
    auto & RF = (*Model.config())["ReceiveFrom"];
    if (!RF.is_array()) RF = nlohmann::ordered_json::array();
    const bool Present = ArrHas(RF, P);
    if (on == Present) return;                       // no change
    if (on) RF.push_back(P);
    else    for (auto It = RF.begin(); It != RF.end(); ++It) if (*It == P) { RF.erase(It); break; }
    Model.save();
    if (on) reconcileReceivedPeer(peer);           // ask for their snapshot: none is kept before Receive is on
    else    stopReceivingFromPeer(peer);           // drop what we've already materialised
}

std::map<std::string, std::string> PeerSharing::sharedStubFiles(const std::string & P) const
{
    std::map<std::string, std::string> Out;
    const nlohmann::ordered_json & C = *Model.config();
    if (C.contains("PeerLibraries") && C["PeerLibraries"].is_object() && C["PeerLibraries"].contains(P))
        for (const auto & T : PackageCatalog::PlanReceivedFetches(C, PackageCatalog::ReceivedNickLabel(P), C["PeerLibraries"][P]))
            Out[T.Dest] = T.Cid;
    return Out;
}

void PeerSharing::dropReceivedStubs(const std::string & P, const std::map<std::string, std::string> & KeepFiles)
{
    std::set<std::filesystem::path> Keep;              // the package dirs the snapshot still names
    for (const auto & [F, C] : KeepFiles) Keep.insert(std::filesystem::path(F).parent_path());
    // A peer's received library is a SNAPSHOT: the roots they share now, and the node closure under them. It lives
    // in CATALOG/<Nick> - <Lib>/ as browse stubs (blocks only; installs are in LIBRARY). Whenever that snapshot goes
    // away or is replaced, the stubs go with it — otherwise every publish they ever made stays on disk beside the
    // current one (three "v1.30.4" of three generations, each indexed, each a card). Plans from the snapshot we hold
    // to name this peer's dirs and CIDs, cancels what we enqueued for them, removes the dirs.
    // A landing pass in flight works from a config snapshot taken before this drop and could write the old
    // generation's files right back into the dirs removed here: remember to drop once more when it ends.
    if (PeerClosureRunning) PeerStubsDirtyPeers.insert(P);
    // The dirs come from the snapshot we hold — or, once that is gone (receive-off / forget erased it), from what
    // this peer's last drop swept: a late block from the pass can still recreate a dir after the record is gone.
    std::set<std::filesystem::path> LibDirs;   // CATALOG/<Nick> - <Lib> subtrees (pure browse stubs)
    if (Model.config()->contains("PeerLibraries") && (*Model.config())["PeerLibraries"].is_object()
        && (*Model.config())["PeerLibraries"].contains(P) && (*Model.config())["PeerLibraries"][P].is_object())
    {
        for (const auto & T : PackageCatalog::PlanReceivedFetches(*Model.config(), PackageCatalog::ReceivedNickLabel(P),
                                                                  (*Model.config())["PeerLibraries"][P]))
        {
            LibDirs.insert(std::filesystem::path(T.Dest).parent_path().parent_path());   // …/<Nick> - <Lib>
            if (Keep.count(std::filesystem::path(T.Dest).parent_path()))
            {   // still shared: its fetches stand — but a file planned for another node now (a new version of it), or
                // for none, is this node's no more, and its fetch never lands there
                const auto It = KeepFiles.find(T.Dest);
                if (It == KeepFiles.end() || It->second != T.Cid) IpfsWrapper::ForgetDest(T.Cid, T.Dest);
                continue;
            }
            if (!PeerBrowseCids.erase(T.Cid)) continue;   // only cancel what WE enqueued for this browse flow
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
    int KeptN = 0;
    std::vector<std::filesystem::path> Gone;   // what goes: decided first, then forgotten by the queue in one pass
    for (const auto & D : LibDirs)
    {
        if (!std::filesystem::is_directory(D, Ec)) continue;
        bool Kept = false;
        std::vector<std::filesystem::path> Pkgs;
        for (const auto & E : std::filesystem::directory_iterator(D, Ec))
        {
            if (!E.is_directory(Ec)) { std::filesystem::remove(E.path(), Ec); continue; }
            if (Keep.count(E.path())) { Kept = true; continue; }                                          // still shared
            if (PackageCatalog::DirHasContent(E.path().string())) { Kept = true; ++KeptN; continue; }   // an install — never
            Pkgs.push_back(E.path());
        }
        if (Kept) Gone.insert(Gone.end(), Pkgs.begin(), Pkgs.end());
        else Gone.push_back(D);                                       // the library's folder, every package in it
    }
    std::vector<std::string> GoneS;
    for (const auto & G : Gone) GoneS.push_back(G.string());
    IpfsWrapper::ForgetDestsUnder(GoneS);   // the queue must not re-materialise these paths later
    int Removed = 0;
    for (const auto & G : Gone) { std::filesystem::remove_all(G, Ec); ++Removed; }
    LogOut("PeerSharing::dropReceivedStubs", "peer " + P.substr(P.size() > 8 ? P.size() - 8 : 0) + ": " + std::to_string(Removed)
           + " stub package dir(s) removed, " + std::to_string(KeptN) + " installed package(s) kept");
}

void PeerSharing::stopReceivingFromPeer(const QString & peer)
{
    const std::string P = peer.toStdString();
    bool Changed = false;
    // Receive-OFF must also stop the peer's still-pending fetches — an explicit user stop, so their data must not
    // keep arriving into the library. Re-plan from the snapshot (cheap, pure) to name this peer's CIDs, cancel each
    // in the queue, and drop them from the landed-block reaction set. Files already landed stay (they are ordinary
    // packages now; withdrawal never deletes).
    dropReceivedStubs(P);
    // Dropping the peer's snapshot removes its packages from future reconciles (enqueueReceivedShares plans only
    // from PeerLibraries of peers we still Receive from).
    if (Model.config()->contains("PeerLibraries") && (*Model.config())["PeerLibraries"].is_object()
        && (*Model.config())["PeerLibraries"].contains(P))
    { (*Model.config())["PeerLibraries"].erase(P); Changed = true; }
    PeerLibSeq.erase(P);
    if (Changed) { Model.save(); Model.rebuildCatalog(); emit peerCatalogChanged(); }
}

bool PeerSharing::isPresenceSharedWith(const QString & peer) const
{
    return !(Model.config()->contains("PresenceDeny") && ArrHas((*Model.config())["PresenceDeny"], peer.toStdString()));
}

void PeerSharing::setPresenceSharedWith(const QString & peer, bool on)
{
    markChosen(peer);
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

void PeerSharing::pushPresenceDeny()
{
    std::vector<std::string> D;
    if (Model.config()->contains("PresenceDeny") && (*Model.config())["PresenceDeny"].is_array())
        for (const auto & X : (*Model.config())["PresenceDeny"]) if (X.is_string()) D.push_back(X.get<std::string>());
    IpfsWrapper::SetPresenceDeny(D);
}

bool PeerSharing::isInVlan(const QString & peer) const
{
    if (!Model.config()->contains("Settings") || !(*Model.config())["Settings"].is_object()) return true;
    return !ArrHas((*Model.config())["Settings"].value("LanExcludedPeers", nlohmann::ordered_json::array()), peer.toStdString());
}

void PeerSharing::setInVlan(const QString & peer, bool on)
{
    markChosen(peer);
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

bool PeerSharing::acceptPeer(const QString & peer)
{
    std::string Err;
    if (!IpfsWrapper::PeerAccept(peer.toStdString(), &Err))
    { LogWarn("PeerSharing::acceptPeer", "accept " + peer.toStdString() + " failed: " + Err); return false; }
    applyNewPeerDefaults(peer);
    return true;
}

bool PeerSharing::pairWith(const QString & peer, Permissions given, std::string * error)
{
    if (!sendPeerRequest(peer, error)) return false;
    applyPairing(peer, given);
    return true;
}

bool PeerSharing::acceptPairing(const QString & peer, Permissions given)
{
    std::string Err;
    if (!IpfsWrapper::PeerAccept(peer.toStdString(), &Err))
    { LogWarn("PeerSharing::acceptPairing", "accept " + peer.toStdString() + " failed: " + Err); return false; }
    applyPairing(peer, given);
    return true;
}

void PeerSharing::applyPairing(const QString & peer, Permissions given)
{
    markChosen(peer);
    setPresenceSharedWith(peer, given.Presence);
    setInVlan(peer, given.Vlan);
    for (const QString & L : given.Libraries) shareLibraryWithPeer(peer, L);
    setReceivingFrom(peer, given.Receive);
}

namespace
{
bool In(const nlohmann::ordered_json & Cfg, const char * Key, const std::string & P)
{
    if (!Cfg.contains(Key) || !Cfg[Key].is_array()) return false;
    for (const auto & X : Cfg[Key]) if (X == P) return true;
    return false;
}
//Puts P in (or takes it out of) the array at Key; true when that changed something.
bool Put(nlohmann::ordered_json & Cfg, const char * Key, const std::string & P, bool On)
{
    auto & A = Cfg[Key];
    if (!A.is_array()) A = nlohmann::ordered_json::array();
    for (auto It = A.begin(); It != A.end(); ++It)
        if (*It == P) { if (On) return false; A.erase(It); return true; }
    if (!On) return false;
    A.push_back(P);
    return true;
}
}

bool PeerSharing::hasChosen(const QString & peer) const { return In(*Model.config(), "PeerChosen", peer.toStdString()); }

void PeerSharing::markChosen(const QString & peer)
{
    if (Put(*Model.config(), "PeerChosen", peer.toStdString(), true)) Model.save();
}

PeerSharing::Received PeerSharing::receivedFrom(const QString & peer) const
{
    const std::string P = peer.toStdString();
    const qint64 Now = QDateTime::currentMSecsSinceEpoch();
    if (const auto C = ReceivedCache.find(P); C != ReceivedCache.end() && Now - C->second.first < 3000) return C->second.second;
    Received R;
    const auto & Cfg = *Model.config();
    if (Cfg.contains("PeerLibraries") && Cfg["PeerLibraries"].is_object() && Cfg["PeerLibraries"].contains(P)
        && Cfg["PeerLibraries"][P].is_object())
    {
        //Their packages: every (library, package) the snapshot names. Not here: a planned one with a file still missing
        //(a package installed from them is planned no more: it is here, in the library).
        std::set<std::pair<std::string, std::string>> Theirs;
        for (const auto & E : PackageCatalog::WireEntries(Cfg["PeerLibraries"][P])) Theirs.insert({ E.Lib, E.Pkg });
        std::map<std::string, bool> Whole;           // planned package folder → every file of it here
        std::error_code Ec;
        for (const auto & T : PackageCatalog::PlanReceivedFetches(Cfg, PackageCatalog::ReceivedNickLabel(P), Cfg["PeerLibraries"][P]))
        {
            const bool Here = std::filesystem::exists(T.Dest, Ec);
            auto [It, New] = Whole.emplace(std::filesystem::path(T.Dest).parent_path().string(), Here);
            if (!New) It->second = It->second && Here;
        }
        int Missing = 0;
        for (const auto & [Dir, Here] : Whole) if (!Here) ++Missing;
        R.Packages = int(Theirs.size());
        R.Here = std::max(0, R.Packages - Missing);
    }
    ReceivedCache[P] = { Now, R };
    return R;
}

std::set<std::string> PeerSharing::sharedLibraries() const
{
    std::set<std::string> Out;
    const auto & Cfg = *Model.config();
    if (Cfg.contains("Sharing") && Cfg["Sharing"].is_object())
        for (const auto & [P, Libs] : Cfg["Sharing"].items())
            if (Libs.is_array()) for (const auto & L : Libs) if (L.is_string()) Out.insert(L.get<std::string>());
    return Out;
}

void PeerSharing::setState(ShareState S, const QString & Text)
{
    if (S == State && Text == StateText) return;
    State = S;
    StateText = Text;
    emit shareStateChanged();
}

QString PeerSharing::shareStateText() const { return StateText; }

void PeerSharing::checkSharesSoon()
{
    if (!ShareCheck)
    {
        ShareCheck = new QTimer(this);
        ShareCheck->setSingleShot(true);
        ShareCheck->setInterval(1500);   // a burst of changes (a rebuild after a publish, several ticks) is one check
        connect(ShareCheck, &QTimer::timeout, this, &PeerSharing::checkShares);
    }
    ShareCheck->start();
}

void PeerSharing::checkShares()
{
    const std::set<std::string> Libs = sharedLibraries();
    if (Libs.empty()) { setState(ShareState::NothingShared, tr("You share nothing yet.")); return; }
    const std::shared_ptr<const NodeIndex> Idx = Model.catalogSnapshot();
    if (!Idx) return;                                // the library is still being read: the rebuild brings this back
    const PackageCatalog::ShareStaleness St = PackageCatalog::StaleSharedPackages(*Model.config(), *Idx, Libs);
    //A package deleted from disk: out of the sheet, and those it was shared with told — nothing minted, nothing renamed.
    //Not while a publish runs: its end writes back the sheet it read, the package in it (the next check drops it).
    if (!St.Removed.empty() && !Model.publishing() && PackageCatalog::DropFromShareSheet(*Model.config(), St.Removed) > 0)
    {
        Model.save();
        reRegisterShares();
    }
    //Waiting no more: a queued package published meanwhile (the user's own publish), a failure no longer stale (fixed,
    //published another way, deleted).
    std::set<std::string> StaleKeys;                 // still to publish: changed, or awaiting a preparation asked for
    for (const auto & P : St.Stale) StaleKeys.insert(P.Key);
    for (const auto & P : St.UnpreparedPackages) StaleKeys.insert(P.Key);
    Queue.erase(std::remove_if(Queue.begin(), Queue.end(), [&](const Queued & Q) { return !StaleKeys.count(Q.Key); }), Queue.end());
    for (auto It = FailedPackages.begin(); It != FailedPackages.end();) It = StaleKeys.count(It->first) ? std::next(It) : FailedPackages.erase(It);
    //What changed since it was last tried: a package that could not publish is not tried again until it changes.
    for (const auto & P : St.Stale)
    {
        const auto Sig = St.Signature.find(P.Key);
        const std::string S = Sig == St.Signature.end() ? std::string() : Sig->second;
        if (const auto A = Attempted.find(P.Key); A != Attempted.end() && A->second == S) continue;
        if (std::any_of(InFlight.begin(), InFlight.end(), [&](const Queued & Q) { return Q.Key == P.Key; })) continue;
        if (std::any_of(Queue.begin(), Queue.end(), [&](const Queued & Q) { return Q.Key == P.Key; })) continue;
        Attempted[P.Key] = S;
        FailedPackages.erase(P.Key);
        Queue.push_back({ P.Dir, P.Key });
    }
    pump();
    const size_t Pending = Queue.size() + InFlight.size();
    if (Pending > 0 || WholeInFlight)
    {
        if (!IpfsWrapper::Available())
            setState(ShareState::Offline, tr("Networking is off: what you share is updated when it is back on."));
        else setState(ShareState::Updating, WholeInFlight ? tr("Preparing your library for sharing…")
                                                         : tr("Updating what you share (%n package(s))…", nullptr, int(Pending)));
        return;
    }
    if (!St.Unprepared.empty())
    {
        QStringList L;
        for (const std::string & U : St.Unprepared) L << QString::fromStdString(U);
        setState(ShareState::Unpublished, tr("Not prepared for sharing yet: %1. Until it is, peers see none of it; "
                                             "prepare it once, and from then on what you share stays up to date by itself.")
                                              .arg(L.join(", ")));
        return;
    }
    const auto Some = [](QStringList L) { const int N = int(L.size()); while (L.size() > 3) L.removeLast(); return L.join("; ") + (N > 3 ? "…" : ""); };
    if (!FailedPackages.empty())   // its publish failed: tried again when it changes, or networking comes up again
    {
        QStringList Names;
        for (const auto & [K, Why] : FailedPackages) Names << QString::fromStdString(K) + ": " + Why;
        setState(ShareState::Failed, tr("%n package(s) could not be published: ", nullptr, int(Names.size())) + Some(Names));
        return;
    }
    if (!St.Refused.empty())       // a node the index refuses: never shared until it is fixed
    {
        QStringList Names;
        for (const std::string & R : St.Refused) Names << QString::fromStdString(R);
        setState(ShareState::Failed, tr("%n package(s) cannot be shared as they are (VidyaGod catalog validate says why): ", nullptr,
                                        int(Names.size())) + Some(Names));
        return;
    }
    if (!LastPublishError.isEmpty())
    { setState(ShareState::Failed, tr("Publishing failed: %1").arg(LastPublishError)); return; }
    setState(ShareState::UpToDate, tr("What you share is up to date."));
}

void PeerSharing::pump()
{
    if (!InFlight.empty() || WholeInFlight || Queue.empty() || Model.publishing() || !IpfsWrapper::Available()) return;
    //All that waits, in one pass (one read of the library); a package of a batch that failed goes alone, so a failure
    //is that package's and the others go on.
    if (Alone.count(Queue.front().Key)) { InFlight.push_back(Queue.front()); Queue.erase(Queue.begin()); }
    else
    {
        for (auto It = Queue.begin(); It != Queue.end();)
            if (Alone.count(It->Key)) ++It;
            else { InFlight.push_back(*It); It = Queue.erase(It); }
    }
    QStringList Dirs;
    for (const Queued & Q : InFlight) Dirs << QString::fromStdString(Q.Dir.string());
    Model.publishPackages(Dirs);
}

void PeerSharing::prepareSharing()
{
    const std::set<std::string> Libs = sharedLibraries();
    const std::shared_ptr<const NodeIndex> Idx = Model.catalogSnapshot();
    const auto & Cfg = *Model.config();
    const bool NoSheet = !Cfg.contains("ShareSheet") || !Cfg["ShareSheet"].is_object() || Cfg["ShareSheet"].empty();
    if (NoSheet || !Idx)                             // the library's first publish: all of it, once
    {
        WholeInFlight = true;
        Model.publishLibraries();
        setState(ShareState::Updating, tr("Preparing your library for sharing…"));
        return;
    }
    for (const auto & P : PackageCatalog::StaleSharedPackages(Cfg, *Idx, Libs).UnpreparedPackages)
        if (!std::any_of(Queue.begin(), Queue.end(), [&](const Queued & Q) { return Q.Key == P.Key; }))
            Queue.push_back({ P.Dir, P.Key });
    pump();
    checkShares();
}

void PeerSharing::applyNewPeerDefaults(const QString & peer)
{
    applyPairing(peer, defaultPermissions());
}

PeerSharing::Permissions PeerSharing::defaultPermissions() const
{
    Permissions D;
    for (const QString & L : newPeerShareDefaults()) if (myLibraryNames().contains(L)) D.Libraries << L;
    D.Receive = newPeerDefault("receive");
    D.Presence = newPeerDefault("presence");
    D.Vlan = newPeerDefault("vlan");
    return D;
}

bool PeerSharing::autoAcceptEnabled() const { return Model.config()->value("AutoAcceptPeers", false); }

void PeerSharing::setAutoAcceptEnabled(bool on)
{
    if (on == autoAcceptEnabled()) return;
    (*Model.config())["AutoAcceptPeers"] = on;
    Model.save();
}

bool PeerSharing::newPeerDefault(const QString & key) const
{
    const std::string K = key.toStdString();
    const bool Fallback = (K == "presence");         // presence ON by default; receive / vlan OFF
    if (!Model.config()->contains("NewPeerDefaults") || !(*Model.config())["NewPeerDefaults"].is_object()) return Fallback;
    return (*Model.config())["NewPeerDefaults"].value(K, Fallback);
}

void PeerSharing::setNewPeerDefault(const QString & key, bool on)
{
    auto & D = (*Model.config())["NewPeerDefaults"];
    if (!D.is_object()) D = nlohmann::ordered_json::object();
    D[key.toStdString()] = on;
    Model.save();
}

QStringList PeerSharing::newPeerShareDefaults() const
{
    QStringList Out;
    if (Model.config()->contains("NewPeerDefaults") && (*Model.config())["NewPeerDefaults"].is_object()
        && (*Model.config())["NewPeerDefaults"].contains("share") && (*Model.config())["NewPeerDefaults"]["share"].is_array())
        for (const auto & X : (*Model.config())["NewPeerDefaults"]["share"])
            if (X.is_string()) Out << QString::fromStdString(X.get<std::string>());
    return Out;
}

void PeerSharing::setNewPeerShareDefault(const QString & lib, bool on)
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

void PeerSharing::enqueueReceivedShares(const QString & peer, const std::vector<PackageCatalog::ReceivedFetch> * Given)
{
    // Receiver = ONE step, zero special machinery: each entry of the peer's share sheet becomes a plain file
    // FetchTarget whose dest is its final path — CATALOG/<Nick> - <Lib>/<pkg>/<file> (PackageCatalog::PlanReceivedFetches).
    // The ONE rolling queue lands every node file verbatim and pins it like any file; from there the ordinary catalog
    // scan / hydration / install / re-publish handle it (a received library re-publishes to identical CIDs — the
    // multi-seeder design). Re-runs on node-ready, presence-online and snapshot apply; a satisfied target is skipped, so
    // repeats converge to a no-op. A re-publish (new CIDs) re-enqueues naturally: the new nodes aren't local yet.
    if (!isReceivingFrom(peer)) return;
    const std::string P = peer.toStdString();
    if (!Model.config()->contains("PeerLibraries") || !(*Model.config())["PeerLibraries"].is_object()
        || !(*Model.config())["PeerLibraries"].contains(P) || !(*Model.config())["PeerLibraries"][P].is_object()) return;

    const auto Plan = Given ? *Given : PackageCatalog::PlanReceivedFetches(*Model.config(), PackageCatalog::ReceivedNickLabel(P),
                                                                           (*Model.config())["PeerLibraries"][P]);
    // No local "satisfied" guess: presence and even HasLocal both LIE for reusable node dests (HasLocal is GLOBAL
    // block membership — another peer's fetch of v2 would mark this peer's stale v1 file settled forever). Every
    // planned target is enqueued with Verify semantics: the QUEUE settles repeats for free (a dest its job already
    // wrote is a no-op at enqueue), and the Go fetch hash-verifies a reused dest exactly.
    for (const auto & T : Plan) PeerBrowseCids.insert(T.Cid);
    const std::vector<IpfsWrapper::FetchTarget> Batch = PackageCatalog::ReceivedFetchTargets(Plan);
    if (!Batch.empty()) awaitReceived(IpfsWrapper::EnqueueBatch(Batch));
}

//What a peer shares lands as ONE batch, and the catalog is rebuilt once, when it has: a rebuild per landed file (every
//2 s while thousands land) held the GUI thread for seconds at a time, a receiving device barely usable for minutes.
//Only what a batch still fetches is waited for — a re-enqueue of what is in place (the 10-min resync, a peer coming
//online) adds nothing, waits for nothing and rebuilds nothing — and ONE wait covers every batch: a peer gone
//mid-batch leaves fetches retrying for ever, and a wait per enqueue piled up behind them. A wait still running after
//ReceiveProgressMs shows what has landed so far, if anything did, and waits on; the rest lands, then one rebuild.
void PeerSharing::awaitReceived(const IpfsWrapper::BatchHandle & H)
{
    //What the queue says this enqueue will land (queued, re-queued, joined in flight) — under its own lock, so a fetch
    //that ends before this line is still awaited — and what it put in place itself (a copy of a file already here).
    ReceivePending.insert(H.Fetching.begin(), H.Fetching.end());
    ReceivedUnshown = ReceivedUnshown || H.Placed > 0;
    if (ReceiveWaiting) return;
    if (!ReceivePending.empty()) waitReceived();
    else if (ReceivedUnshown) showReceived();
}

void PeerSharing::showReceived()
{
    ReceivedUnshown = false;
    Model.rebuildCatalog();         // the landed blocks ARE ordinary tree packages — just re-scan
    emit peerCatalogChanged();
    completeReceivedClosures();     // land what they contain — a stuck file elsewhere holds up nothing
}

void PeerSharing::waitReceived()
{
    ReceiveWaiting = true;
    IpfsWrapper::BatchHandle H;
    for (const std::string & Cid : ReceivePending) H.Items.emplace_back(Cid, /*Optional=*/true);
    auto OutOfTime = std::make_shared<bool>(false);
    AsyncWork::Run(this,
        [H, OutOfTime, Ms = ReceiveProgressMs]{
            *OutOfTime = !IpfsWrapper::WaitBatch(H, Ms);   // optional targets: false only when the time is up
        },
        [this, OutOfTime] {
            ReceiveWaiting = false;
            //Settled here, on this thread, over everything awaited NOW: a file that joined during the round and landed
            //counts, and one re-asked for meanwhile (failed, then queued again) is still awaited.
            for (auto It = ReceivePending.begin(); It != ReceivePending.end(); )
            {
                bool Landed = false;
                if (!IpfsWrapper::JobSettled(*It, &Landed)) { ++It; continue; }
                ReceivedUnshown = ReceivedUnshown || Landed;
                It = ReceivePending.erase(It);
            }
            //Shown when everything has settled, or — a landing that takes long — when a round ran out of time; a round
            //that ended while files joined meanwhile waits on for them.
            if (ReceivedUnshown && (ReceivePending.empty() || *OutOfTime)) showReceived();
            if (!ReceivePending.empty()) waitReceived();
        });
}

void PeerSharing::completeReceivedClosures()
{
    // A peer's share is their share sheet's entries, closed over what they contain. The receiver lands every node
    // file an entry names (blocks only — kilobytes per game; content stays lazy until install), so it holds the same
    // graph the seeder holds and derives the same cards, offers and needs from it. Re-entrant-safe: a share changing
    // while a pass runs queues exactly one more pass.
    if (PeerClosureRunning) { PeerClosureAgain = true; return; }
    //Each package lands whole, in its own folder; a node a package contains comes with ITS package (a share is
    //closed over what its packages contain), never fetched into the package that needs it.
    if (!PackageCatalog::ReceivedPackagesIncomplete(*Model.config()))
    {   // nothing to land: still drop what no entry names (older generations)
        if (PackageCatalog::PruneStaleReceived(*Model.catalogSnapshot(), *Model.config()) > 0) { Model.rebuildCatalog(); emit peerCatalogChanged(); }
        return;
    }
    PeerClosureRunning = true;
    ++ClosurePasses;
    LogOut("PeerSharing::completeReceivedClosures", "landing received package(s)");
    auto Cfg  = std::make_shared<const nlohmann::ordered_json>(*Model.config());
    AsyncWork::Run(this,   // the pass touches nothing of this model: it can outlive it
        [Cfg]{
            const IpfsWrapper::ForegroundHold Hold;
            std::string PErr;
            if (!PackageCatalog::LandReceivedPackages(*Cfg, &PErr))
                LogWarn("PeerSharing::completeReceivedClosures", "received package(s): " + PErr);
        },
        [this]{
            PeerClosureRunning = false;
            if (!PeerStubsDirtyPeers.empty())
            {   // a drop happened mid-pass: what the pass wrote after it is the old generation's — drop those peers'
                // stubs again (a stopped peer's dirs are remembered, so this works after its record is gone), re-enqueue
                const std::set<std::string> Peers = std::move(PeerStubsDirtyPeers);
                PeerStubsDirtyPeers.clear();
                for (const std::string & P : Peers) dropReceivedStubs(P, sharedStubFiles(P));
                PeerStubsDirtyPeers.clear();   // the drops above ran with no pass in flight: nothing new to remember
                reconcileReceivedLibraries();    // the CURRENT generation's roots, into the now-clean dirs
            }
            Model.rebuildCatalog();           // the landed closure blocks are ordinary tree nodes, marked Received by the scan
            if (const int Pruned = PackageCatalog::PruneStaleReceived(*Model.catalogSnapshot(), *Model.config()); Pruned > 0)
            {   // older generations' node files in kept (installed) dirs — gone now that the current ones landed
                LogOut("PeerSharing::completeReceivedClosures", "pruned " + std::to_string(Pruned) + " stale received node file(s)");
                Model.rebuildCatalog();
            }
            emit peerCatalogChanged();
            if (PeerClosureAgain) { PeerClosureAgain = false; completeReceivedClosures(); }
            else if (PackageCatalog::ReceivedPackagesIncomplete(*Model.config()))
                //Files still to land (a peer away, a share that changed): tried again in a minute, not back to back — a
                //pass holds the queue in the foreground and rebuilds the catalog twice.
                QTimer::singleShot(60 * 1000, this, [this] {
                    if (!PeerClosureRunning && PackageCatalog::ReceivedPackagesIncomplete(*Model.config())) completeReceivedClosures();
                });
        });
}
bool PeerSharing::sendPeerRequest(const QString & code, std::string * error)
{
    return IpfsWrapper::PeerAdd(code.trimmed().toStdString(), std::string(), error);
}

bool PeerSharing::setNickname(const QString & nick, std::string * error)
{
    return IpfsWrapper::SetProfile(nick.trimmed().toStdString(), std::string(), error);
}

void PeerSharing::declinePeer(const QString & peer)
{
    IpfsWrapper::PeerDecline(peer.toStdString());
    forgetPeer(peer);
}

void PeerSharing::blockPeer(const QString & peer)
{
    IpfsWrapper::PeerBlock(peer.toStdString());
    forgetPeer(peer);
}

void PeerSharing::removePeer(const QString & peer)
{
    IpfsWrapper::PeerRemove(peer.toStdString());
    forgetPeer(peer);
}
