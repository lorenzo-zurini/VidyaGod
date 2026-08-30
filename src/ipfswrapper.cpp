#include "ipfswrapper.h"
#include "downloadqueue.h"   // SetQueueCallback (queue → IpfsManager signal relay)
#include "commonutils.h"
#include "vgipfsapi.h"

#include <QString>
#include <QFileInfo>
#include <QCoreApplication>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <ctime>
#include <string>
#include <utility>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <vector>
#include <cstdlib>
#include <unordered_map>

// IpfsWrapper now drives the embedded in-process IPFS node (VidyaGodIPFS → libvgipfs.so) via its C ABI
// (vgipfsapi.h) instead of shelling out to the external Kubo `ipfs` CLI. The public API and the IpfsManager signal
// hub are unchanged, so every call site (hydration, launch-time layer fetch, cover cache, publish, the IPFS tab)
// is untouched. The node is started once at process startup in main.cpp (VgStart) and torn down at exit.

namespace IpfsWrapper {

// VG_FETCH_DEBUG: mirror the node's [fetchdbg] tracing on the C++ side (transfer events + FetchToPath entry/exit) so
// the GUI-facing phases interleave with the Go phases in one stderr stream. Zero-cost when the env var is unset.
static const bool g_FetchDbg = std::getenv("VG_FETCH_DEBUG") != nullptr;
static void FetchDbg(const std::string &Msg)
{
    if (!g_FetchDbg) return;
    const auto Now = std::chrono::system_clock::now();
    const auto T   = std::chrono::system_clock::to_time_t(Now);
    const auto Ms  = std::chrono::duration_cast<std::chrono::milliseconds>(Now.time_since_epoch()).count() % 1000;
    char Ts[16]; std::strftime(Ts, sizeof Ts, "%H:%M:%S", std::localtime(&T));
    std::fprintf(stderr, "[cppdbg %s.%03lld] %s\n", Ts, static_cast<long long>(Ms), Msg.c_str());
    std::fflush(stderr);
}

//Optional sink for transfer lifecycle events (installed by IpfsManager). Invoked on whatever thread the node's
//fetch runs on — the installed callback is responsible for marshalling to the GUI thread.
static TransferCallback g_TransferCb;
void SetTransferCallback(TransferCallback Callback) { g_TransferCb = std::move(Callback); }
static void Emit(const TransferEvent &E) { if (g_TransferCb) g_TransferCb(E); }

// Bridge installed into the node (VgSetTransferCb): the node reports a fetch's Started/Progress/Finished through
// this C callback; we repackage it as a TransferEvent and fan it out through Emit → the IpfsManager-installed
// callback (which marshals to the GUI thread). C linkage so it matches the node's function-pointer type.
extern "C" void IpfsNodeTransferCb(const char *cid, int kind, double percent, int ok, const char *err)
{
    TransferEvent E;
    E.Kind = kind == 0 ? TransferEvent::Started
           : kind == 1 ? TransferEvent::Progress
           : kind == 3 ? TransferEvent::Finalizing
                       : TransferEvent::Finished;
    E.Cid     = cid ? cid : "";
    E.Percent = percent;
    E.Ok      = ok != 0;
    E.Error   = err ? err : "";
    if (g_FetchDbg) {
        const char *K = E.Kind == TransferEvent::Started    ? "Started"
                      : E.Kind == TransferEvent::Progress    ? "Progress"
                      : E.Kind == TransferEvent::Finalizing  ? "Finalizing"
                                                             : "Finished";
        char Buf[256];
        std::snprintf(Buf, sizeof Buf, "TransferEvent %s cid=%s pct=%.1f ok=%d err=%s",
                      K, E.Cid.c_str(), E.Percent, E.Ok ? 1 : 0, E.Error.c_str());
        FetchDbg(Buf);
    }
    Emit(E);
}

// Take ownership of a char* the node returned through an out-param: copy into std::string and free it.
static std::string TakeStr(char *S) { std::string R = S ? S : std::string(); if (S) VgFree(S); return R; }

// Format a byte count the way `ipfs repo stat --human` used to, for the IPFS tab.
// ----- download concurrency throttle -----
// A resizable counting semaphore (mutex + condvar so the limit can change at runtime): DownloadSlot acquires before
// a fetch and releases after, and at most g_MaxConcurrent slots are held at once. Raising the limit wakes waiters;
// lowering it just lets the surplus drain as in-flight fetches finish (never interrupts a running one).
static std::mutex              g_ThrottleMu;
static std::condition_variable g_ThrottleCv;
static int g_MaxConcurrent = 3;   // default; overridden from Settings at startup
static int g_ActiveDownloads = 0;

void SetMaxConcurrentDownloads(int N)
{
    std::lock_guard<std::mutex> Lk(g_ThrottleMu);
    g_MaxConcurrent = std::clamp(N, 1, 32);
    g_ThrottleCv.notify_all();   // a higher limit may let blocked acquirers through
}

int MaxConcurrentDownloads()
{
    std::lock_guard<std::mutex> Lk(g_ThrottleMu);
    return g_MaxConcurrent;
}

DownloadSlot::DownloadSlot()
{
    std::unique_lock<std::mutex> Lk(g_ThrottleMu);
    g_ThrottleCv.wait(Lk, []{ return g_ActiveDownloads < g_MaxConcurrent; });
    ++g_ActiveDownloads;
}

DownloadSlot::~DownloadSlot()
{
    if (!Owned) return;   // moved-from: the slot now lives in another instance
    {
        std::lock_guard<std::mutex> Lk(g_ThrottleMu);
        --g_ActiveDownloads;
    }
    g_ThrottleCv.notify_one();
}

void RequestCancel(const std::string &Cid) { VgRequestCancel(Cid.c_str()); }
void ClearCancel(const std::string &Cid)   { VgClearCancel(Cid.c_str()); }

bool StartNode(const std::string &RepoPath, std::string *Error)
{
    char *Err = nullptr;
    if (VgStart(RepoPath.c_str(), &Err) != 0)
    {
        const std::string E = TakeStr(Err);
        const std::string Msg = E.empty() ? ("node failed to start at " + RepoPath) : E;
        if (Error) *Error = Msg;
        LogErr("IpfsWrapper::StartNode", Msg);
        return false;
    }
    static bool AtexitSet = false;
    if (!AtexitSet) { std::atexit(VgStop); AtexitSet = true; }   // best-effort clean leveldb shutdown on any exit path
    LogSucc("IpfsWrapper::StartNode", "embedded IPFS node started at " + RepoPath);
    return true;
}

void StopNode() { VgStop(); }

bool Available()
{
    // The node is compiled in; "available" means it actually opened its repo (StartNode succeeded). False before that
    // (deferred start) or after StopNode (networking disabled).
    return VgStarted() != 0;
}

bool DaemonRunning()
{
    // There is no external daemon any more — this reports whether the in-process node's network stack is up.
    return VgOnline() != 0;
}

std::string AddNoCopy(const std::string &PathStr, std::string *Error)
{
    if (PathStr.empty()) { if (Error) *Error = "empty path"; return std::string(); }
    char *Cid = nullptr, *Err = nullptr;
    const int Rc = VgAddNoCopy(PathStr.c_str(), &Cid, &Err);
    const std::string CidS = TakeStr(Cid);
    const std::string ErrS = TakeStr(Err);
    if (Rc != 0) { if (Error) *Error = ErrS.empty() ? ("add failed: " + PathStr) : ErrS; return std::string(); }
    return CidS;
}

std::string AddNoCopyMeta(const std::string &PathStr, std::string *Error)
{
    if (PathStr.empty()) { if (Error) *Error = "empty path"; return std::string(); }
    char *Cid = nullptr, *Err = nullptr;
    const int Rc = VgAddNoCopyMeta(PathStr.c_str(), &Cid, &Err);
    const std::string CidS = TakeStr(Cid);
    const std::string ErrS = TakeStr(Err);
    if (Rc != 0) { if (Error) *Error = ErrS.empty() ? ("meta add failed: " + PathStr) : ErrS; return std::string(); }
    return CidS;
}

std::string FetchToPath(const std::string &Cid, const std::string &DestPathStr, std::string *Error)
{
    if (Cid.empty())         { if (Error) *Error = "empty CID";              return std::string(); }
    if (DestPathStr.empty()) { if (Error) *Error = "empty destination path"; return std::string(); }

    // Already materialized — no work, no transfer events (the common case during launch of installed content).
    if (QFileInfo::exists(QString::fromStdString(DestPathStr)))
    {
        LogOut("IpfsWrapper::FetchToPath", "Already present: " + DestPathStr);
        return DestPathStr;
    }

    // The node fetches write-through to DestPath (no blockstore duplication), seeds it from there, and reports
    // Started/Progress/Finished through the transfer callback installed below.
    LogOut("IpfsWrapper::FetchToPath", "Fetching CID " + Cid + " -> " + DestPathStr);
    FetchDbg("FetchToPath ENTER (blocking VgFetchToPath call) cid=" + Cid + " dest=" + DestPathStr);
    char *Err = nullptr;
    const int Rc = VgFetchToPath(Cid.c_str(), DestPathStr.c_str(), &Err);
    const std::string ErrS = TakeStr(Err);
    FetchDbg("FetchToPath RETURN rc=" + std::to_string(Rc) + " err='" + ErrS + "' cid=" + Cid);
    if (Rc != 0)
    {
        const std::string Msg = ErrS.empty() ? ("fetch failed for CID " + Cid) : ErrS;
        LogErr("IpfsWrapper::FetchToPath", Msg);
        if (Error) *Error = Msg;
        return std::string();
    }
    LogSucc("IpfsWrapper::FetchToPath", "Materialized CID " + Cid + " at " + DestPathStr);
    return DestPathStr;
}

std::string FetchDirToPath(const std::string &Cid, const std::string &DestDirStr, std::string *Error)
{
    if (Cid.empty())        { if (Error) *Error = "empty CID";            return std::string(); }
    if (DestDirStr.empty()) { if (Error) *Error = "empty destination dir"; return std::string(); }

    // Recursively materialize a folder CID (the dehydrated package set) — the node fetches the whole small tree
    // (manifests + covers) over bitswap and writes it to DestDir. Requires networking to be up (checked node-side).
    LogOut("IpfsWrapper::FetchDirToPath", "Fetching folder CID " + Cid + " -> " + DestDirStr);
    char *Err = nullptr;
    const int Rc = VgFetchDirToPath(Cid.c_str(), DestDirStr.c_str(), &Err);
    const std::string ErrS = TakeStr(Err);
    if (Rc != 0)
    {
        const std::string Msg = ErrS.empty() ? ("folder fetch failed for CID " + Cid) : ErrS;
        LogErr("IpfsWrapper::FetchDirToPath", Msg);
        if (Error) *Error = Msg;
        return std::string();
    }
    LogSucc("IpfsWrapper::FetchDirToPath", "Materialized folder CID " + Cid + " at " + DestDirStr);
    return DestDirStr;
}

// FetchTargetsConcurrent now lives in downloadqueue.cpp — it enqueues the batch into the shared CID-addressed
// DownloadQueue (dedup by CID + already-seeded, cross-dest single-fetch, priority dispatch) and waits for it.

int PeerCount()
{
    return VgPeerCount();
}

BandwidthRates Bandwidth()
{
    BandwidthRates R;
    VgBandwidthRates(&R.DownBps, &R.UpBps);
    return R;
}

std::vector<std::string> ActiveUploads(int WindowMs)
{
    std::vector<std::string> Result;
    char *J = nullptr;
    const int Rc = VgActiveUploads(WindowMs, &J);
    const std::string Js = TakeStr(J);
    if (Rc != 0) return Result;
    try {
        for (const auto &C : nlohmann::json::parse(Js)) Result.push_back(C.get<std::string>());
    } catch (const std::exception &E) { LogWarn("IpfsWrapper::ActiveUploads", std::string("bad JSON from node: ") + E.what()); }
    return Result;
}

std::vector<UnservableRef> UnservableRefs()
{
    std::vector<UnservableRef> Result;
    char *J = nullptr;
    const int Rc = VgUnservableRefs(&J);
    const std::string Js = TakeStr(J);
    if (Rc != 0) return Result;
    try {
        for (const auto &E : nlohmann::json::parse(Js))
            Result.push_back({ E.value("cid", std::string()), E.value("path", std::string()),
                               E.value("err", std::string()), E.value("status", 0) });
    } catch (const std::exception &E) { LogWarn("IpfsWrapper::UnservableRefs", std::string("bad JSON from node: ") + E.what()); }
    return Result;
}

std::vector<std::string> OrphanedRefPaths()
{
    std::vector<std::string> Result;
    char *J = nullptr;
    const int Rc = VgOrphanedRefPaths(&J);
    const std::string Js = TakeStr(J);
    if (Rc != 0) return Result;
    try {
        for (const auto &P : nlohmann::json::parse(Js)) Result.push_back(P.get<std::string>());
    } catch (const std::exception &E) { LogWarn("IpfsWrapper::OrphanedRefPaths", std::string("bad JSON from node: ") + E.what()); }
    return Result;
}

std::string RepoSizeHuman()
{
    char *J = nullptr, *Err = nullptr;
    const int Rc = VgRepoStat(&J, &Err);
    const std::string Js = TakeStr(J);
    TakeStr(Err);
    if (Rc != 0) return std::string();
    try {
        const auto D = nlohmann::json::parse(Js);
        const long long Size = D.value("RepoSize",  (long long)-1);
        const long long Max  = D.value("StorageMax", (long long)-1);
        if (Size < 0) return std::string();
        const std::string S = HumanBytes(Size);
        return Max >= 0 ? (S + " / " + HumanBytes(Max)) : S;
    } catch (const std::exception &E) { LogWarn("IpfsWrapper::RepoUsage", std::string("bad JSON from node: ") + E.what()); return std::string(); }
}

long long CidSize(const std::string &Cid)
{
    if (Cid.empty()) return -1;
    return VgCidSize(Cid.c_str());
}

long long CidSizeLocal(const std::string &Cid)
{
    if (Cid.empty()) return -1;
    return VgCidSizeLocal(Cid.c_str());
}

int ProviderCount(const std::string &Cid)
{
    if (Cid.empty()) return -1;
    // "How replicated" signal via the DHT — a slow walk, so cap it at a short deadline (mirrors the old findprovs).
    return VgProviderCount(Cid.c_str(), 8000);
}

void SetSeedLevels(const std::vector<std::string> &Collections, const std::vector<std::string> &Packages)
{
    const nlohmann::ordered_json C = Collections, P = Packages;   // JSON arrays of CID strings
    VgSetSeedLevels(C.dump().c_str(), P.dump().c_str());
}

bool SeedAnnounced(const std::string &Cid)
{
    return !Cid.empty() && VgSeedAnnounced(Cid.c_str()) == 1;
}

bool CidMissing(const std::string &Cid)
{
    if (Cid.empty()) return false;
    return VgCidMissing(Cid.c_str()) == 1;
}

std::string ComputeCid(const std::string &Path, std::string *Error)
{
    if (Path.empty()) { if (Error) *Error = "empty path"; return std::string(); }
    char *Cid = nullptr, *Err = nullptr;
    const int Rc = VgComputeCid(Path.c_str(), &Cid, &Err);
    const std::string CidS = TakeStr(Cid), ErrS = TakeStr(Err);
    if (Rc != 0) { if (Error) *Error = ErrS.empty() ? ("could not hash " + Path) : ErrS; return std::string(); }
    return CidS;
}

long long CidFileSizeLocal(const std::string &Cid)
{
    if (Cid.empty()) return -1;
    return VgCidFileSizeLocal(Cid.c_str());
}

std::string VerifyCid(const std::string &Cid)
{
    if (Cid.empty()) return "empty cid";
    return TakeStr(VgVerifyCid(Cid.c_str()));
}

bool HasLocal(const std::string &Cid)
{
    if (Cid.empty()) return false;
    return VgHasLocal(Cid.c_str()) == 1;
}

bool DropRef(const std::string &Cid)
{
    if (Cid.empty()) return false;
    char *Err = nullptr;
    const int Rc = VgDropRef(Cid.c_str(), &Err);
    TakeStr(Err);
    return Rc == 0;
}

bool DropCached(const std::string &Cid)
{
    if (Cid.empty()) return false;
    char *Err = nullptr;
    const int Rc = VgDropCached(Cid.c_str(), &Err);
    TakeStr(Err);
    return Rc == 0;
}

std::string PeerID()
{
    char *Id = nullptr;
    if (VgPeerID(&Id) != 0) { TakeStr(Id); return {}; }
    return TakeStr(Id);
}

std::vector<std::string> ListenAddrs()
{
    char *J = nullptr;
    if (VgListenAddrs(&J) != 0) { TakeStr(J); return {}; }
    const std::string Js = TakeStr(J);
    std::vector<std::string> Out;
    try { for (const auto &A : nlohmann::json::parse(Js)) Out.push_back(A.get<std::string>()); }
    catch (const std::exception &E) { LogWarn("IpfsWrapper::ListenAddrs", std::string("bad JSON from node: ") + E.what()); }
    return Out;
}

bool Connect(const std::string &Multiaddr)
{
    if (Multiaddr.empty()) return false;
    char *Err = nullptr;
    const int Rc = VgConnect(Multiaddr.c_str(), &Err);
    TakeStr(Err);
    return Rc == 0;
}

std::vector<PinEntry> Pins()
{
    std::vector<PinEntry> Result;
    char *J = nullptr, *Err = nullptr;
    const int Rc = VgPinLs(&J, &Err);
    const std::string Js = TakeStr(J);
    TakeStr(Err);
    if (Rc != 0) return Result;
    try {
        for (const auto &C : nlohmann::json::parse(Js))
            Result.push_back({ C.get<std::string>() });
    } catch (const std::exception &E) { LogWarn("IpfsWrapper::Peers", std::string("bad JSON from node: ") + E.what()); }
    return Result;
}

bool Unpin(const std::string &Cid)
{
    if (Cid.empty()) return false;
    char *Err = nullptr;
    const int Rc = VgPinRm(Cid.c_str(), &Err);
    TakeStr(Err);
    return Rc == 0;
}

// ----- friends / multiplayer social layer -----

// Parse one contact JSON object ({peer,nick,pic,state,online,seen}) into a Contact.
static Contact ContactFromJson(const nlohmann::json &J)
{
    Contact C;
    C.PeerID   = J.value("peer",  std::string());
    C.Nick     = J.value("nick",  std::string());
    C.PicCID   = J.value("pic",   std::string());
    C.State    = J.value("state", std::string());
    C.Online   = J.value("online", false);
    C.LastSeen = J.value("seen",  (long long)0);
    return C;
}

// Sink for inbound friend events (installed by FriendsManager). Invoked on a node thread.
static FriendCallback g_FriendCb;
void SetFriendCallback(FriendCallback Callback) { g_FriendCb = std::move(Callback); }

// Bridge installed into the node (VgSetFriendCb): repackage the node's (kind, json) event as a FriendEvent and fan
// it out through the installed callback. C linkage so it matches the node's function-pointer type.
extern "C" void IpfsNodeFriendCb(int kind, const char *json)
{
    if (!g_FriendCb) return;
    FriendEvent E;
    E.Kind = kind == 0 ? FriendEvent::Request
           : kind == 1 ? FriendEvent::Accept
           : kind == 2 ? FriendEvent::Decline
           : kind == 3 ? FriendEvent::Presence
           : kind == 4 ? FriendEvent::Profile
                       : FriendEvent::Removed;
    try { E.C = ContactFromJson(nlohmann::json::parse(json ? json : "{}")); }
    catch (const std::exception &Ex) { LogWarn("IpfsWrapper::FriendEventTrampoline", std::string("bad JSON from node: ") + Ex.what()); }
    g_FriendCb(E);
}

std::string FriendCode()
{
    char *Id = nullptr;
    if (VgFriendCode(&Id) != 0) { TakeStr(Id); return {}; }
    return TakeStr(Id);
}

Profile GetProfile()
{
    Profile P;
    char *J = nullptr;
    if (VgGetProfile(&J) != 0) { TakeStr(J); return P; }
    const std::string Js = TakeStr(J);
    try { const auto D = nlohmann::json::parse(Js); P.Nick = D.value("nick", std::string()); P.PicCID = D.value("pic", std::string()); }
    catch (const std::exception &E) { LogWarn("IpfsWrapper::GetProfile", std::string("bad JSON from node: ") + E.what()); }
    return P;
}

bool SetProfile(const std::string &Nick, const std::string &PicCID, std::string *Error)
{
    char *Err = nullptr;
    const int Rc = VgSetProfile(Nick.c_str(), PicCID.c_str(), &Err);
    const std::string E = TakeStr(Err);
    if (Rc != 0) { if (Error) *Error = E.empty() ? "set profile failed" : E; return false; }
    return true;
}

std::vector<Contact> FriendList()
{
    std::vector<Contact> Out;
    char *J = nullptr;
    if (VgFriendList(&J) != 0) { TakeStr(J); return Out; }
    const std::string Js = TakeStr(J);
    try { for (const auto &E : nlohmann::json::parse(Js)) Out.push_back(ContactFromJson(E)); }
    catch (const std::exception &Ex) { LogWarn("IpfsWrapper::FriendList", std::string("bad JSON from node: ") + Ex.what()); }
    return Out;
}

// Small helper for the fallible peer-ID mutators that return 0/-1 with an errOut.
static bool FriendCall(int Rc, char *Err, std::string *Error, const char *What)
{
    const std::string E = TakeStr(Err);
    if (Rc != 0) { if (Error) *Error = E.empty() ? What : E; return false; }
    return true;
}

bool FriendAdd(const std::string &PeerID, const std::string &Note, std::string *Error)
{
    if (PeerID.empty()) { if (Error) *Error = "empty peer id"; return false; }
    char *Err = nullptr;
    return FriendCall(VgFriendAdd(PeerID.c_str(), Note.c_str(), &Err), Err, Error, "friend request failed");
}

bool FriendAccept(const std::string &PeerID, std::string *Error)
{
    char *Err = nullptr;
    return FriendCall(VgFriendAccept(PeerID.c_str(), &Err), Err, Error, "accept failed");
}

bool FriendDecline(const std::string &PeerID, std::string *Error)
{
    char *Err = nullptr;
    return FriendCall(VgFriendDecline(PeerID.c_str(), &Err), Err, Error, "decline failed");
}

bool FriendBlock(const std::string &PeerID, std::string *Error)
{
    char *Err = nullptr;
    return FriendCall(VgFriendBlock(PeerID.c_str(), &Err), Err, Error, "block failed");
}

bool FriendRemove(const std::string &PeerID)
{
    if (PeerID.empty()) return false;
    return VgFriendRemove(PeerID.c_str()) == 0;
}

int FriendPing(const std::string &PeerID)
{
    if (PeerID.empty()) return -1;
    return VgFriendPing(PeerID.c_str());
}

// ----- virtual LAN of friends (host-less; each peer's vIP is a pure function of its peer ID — friendlan.go) -----

std::map<std::string, std::string> LanLaunchVars()
{
    std::map<std::string, std::string> Out;
    char *J = nullptr;
    if (VgLanLaunchVars(&J) != 0) { TakeStr(J); return Out; }
    const std::string Js = TakeStr(J);
    try { for (const auto &[K, V] : nlohmann::json::parse(Js).items()) Out[K] = V.get<std::string>(); }
    catch (const std::exception &E) { LogWarn("IpfsWrapper::LanLaunchVars", std::string("bad JSON from node: ") + E.what()); }
    return Out;
}

std::vector<LanPeer> LanPeers()
{
    std::vector<LanPeer> Out;
    char *J = nullptr;
    if (VgLanPeers(&J) != 0) { TakeStr(J); return Out; }
    const std::string Js = TakeStr(J);
    try
    {
        for (const auto &E : nlohmann::json::parse(Js))
        {
            LanPeer P;
            P.Peer   = E.value("peer", std::string());
            P.Nick   = E.value("nick", std::string());
            P.Vip    = E.value("vip", std::string());
            P.Online = E.value("online", false);
            P.Link   = E.value("link", std::string("down"));
            P.RttMs  = E.value("rttMs", -1LL);
            Out.push_back(std::move(P));
        }
    }
    catch (const std::exception &E) { LogWarn("IpfsWrapper::LanPeers", std::string("bad JSON from node: ") + E.what()); }
    return Out;
}

void SetLanExcluded(const std::vector<std::string> &PeerIds)
{
    std::string Csv;
    for (const std::string &P : PeerIds) { if (!Csv.empty()) Csv += ","; Csv += P; }
    VgLanSetExcluded(Csv.c_str());
}

// ----- overlay tunnel -----

std::string OverlayStart(std::string *Error)
{
    char *Name = nullptr, *Err = nullptr;
    const int Rc = VgOverlayStart(&Name, &Err);
    const std::string N = TakeStr(Name);
    const std::string E = TakeStr(Err);
    if (Rc != 0) { if (Error) *Error = E.empty() ? "overlay start failed" : E; return std::string(); }
    LogSucc("IpfsWrapper::OverlayStart", "friend-LAN overlay up on " + N);
    return N;
}

bool OverlayServe(const std::string &SockPath, bool Bridge, bool HostRelay, std::string *Error)
{
    char *Err = nullptr;
    const int Rc = VgOverlayServe(SockPath.c_str(), Bridge ? 1 : 0, HostRelay ? 1 : 0, &Err);
    const std::string E = TakeStr(Err);
    if (Rc != 0) { if (Error) *Error = E.empty() ? "overlay serve failed" : E; return false; }
    return true;
}

void OverlayStop() { VgOverlayStop(); }
bool OverlayActive() { return VgOverlayActive() != 0; }

} // namespace IpfsWrapper

// ---------------------------------------------------------------------------
// IpfsManager
// ---------------------------------------------------------------------------
IpfsManager::IpfsManager(QObject * parent) : QObject(parent)
{
    //Relay backend transfer events (which fire on the node's fetch thread) onto this object's thread
    //(the GUI thread) as Qt signals, via a queued invocation.
    IpfsWrapper::SetTransferCallback([this](const IpfsWrapper::TransferEvent &E) {
        const QString Cid = QString::fromStdString(E.Cid);
        switch (E.Kind)
        {
        case IpfsWrapper::TransferEvent::Started:
            QMetaObject::invokeMethod(this, [this, Cid]{ emit transferStarted(Cid); }, Qt::QueuedConnection);
            break;
        case IpfsWrapper::TransferEvent::Progress: {
            const double Pct = E.Percent;
            // Throttle progress to ~4/sec PER CID. The node emits per network chunk (many/sec); without this every
            // consumer (Catalog overlay card-scan, IPFS table row + speed calc, progress averaging) runs per chunk.
            // 100% is always let through (so the bar reaches full before Finished); Started/Finalizing/Finished are
            // never throttled. Runs on the node's fetch thread(s), so the per-CID timestamps are mutex-guarded.
            {
                static std::mutex ProgMu;
                static std::unordered_map<std::string, long long> LastMs;
                const long long Now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch()).count();
                std::lock_guard<std::mutex> Lk(ProgMu);
                auto It = LastMs.find(E.Cid);
                if (Pct < 100.0 && It != LastMs.end() && Now - It->second < 250) break;   // drop this tick
                LastMs[E.Cid] = Now;
            }
            QMetaObject::invokeMethod(this, [this, Cid, Pct]{ emit transferProgress(Cid, Pct); }, Qt::QueuedConnection);
            break; }
        case IpfsWrapper::TransferEvent::Finalizing: {
            const double Pct = E.Percent;
            QMetaObject::invokeMethod(this, [this, Cid, Pct]{ emit transferFinalizing(Cid, Pct); }, Qt::QueuedConnection);
            break; }
        case IpfsWrapper::TransferEvent::Finished: {
            const bool Ok = E.Ok;
            const QString Err = QString::fromStdString(E.Error);
            QMetaObject::invokeMethod(this, [this, Cid, Ok, Err]{ emit transferFinished(Cid, Ok, Err); }, Qt::QueuedConnection);
            break; }
        }
    });

    //Route the embedded node's fetch lifecycle into the callback just installed (→ these signals).
    VgSetTransferCb(&IpfsWrapper::IpfsNodeTransferCb);

    //Relay the download QUEUE's queued/removed events (fire on the enqueue/cancel thread) as Qt signals too.
    IpfsWrapper::SetQueueCallback([this](const std::string & cid, bool queued) {
        const QString Cid = QString::fromStdString(cid);
        QMetaObject::invokeMethod(this, [this, Cid, queued]{
            if (queued) emit queueEnqueued(Cid); else emit queueRemoved(Cid);
        }, Qt::QueuedConnection);
    });
}

IpfsManager * IpfsManager::instance()
{
    //Parented to the application so it outlives any tab rebuild and is created on the GUI thread.
    static IpfsManager * Inst = new IpfsManager(qApp);
    return Inst;
}

// ---------------------------------------------------------------------------
// FriendsManager
// ---------------------------------------------------------------------------
FriendsManager::FriendsManager(QObject * parent) : QObject(parent)
{
    //Relay backend friend events (which fire on a node thread) onto this object's (GUI) thread as Qt signals.
    IpfsWrapper::SetFriendCallback([this](const IpfsWrapper::FriendEvent &E) {
        const QString Peer = QString::fromStdString(E.C.PeerID);
        const QString Nick = QString::fromStdString(E.C.Nick);
        const QString Pic  = QString::fromStdString(E.C.PicCID);
        const bool    On   = E.C.Online;
        switch (E.Kind)
        {
        case IpfsWrapper::FriendEvent::Request:
            QMetaObject::invokeMethod(this, [this, Peer, Nick, Pic]{ emit friendRequest(Peer, Nick, Pic); }, Qt::QueuedConnection);
            break;
        case IpfsWrapper::FriendEvent::Accept:
            QMetaObject::invokeMethod(this, [this, Peer, Nick, Pic]{ emit friendAccepted(Peer, Nick, Pic); }, Qt::QueuedConnection);
            break;
        case IpfsWrapper::FriendEvent::Decline:
            QMetaObject::invokeMethod(this, [this, Peer]{ emit friendDeclined(Peer); }, Qt::QueuedConnection);
            break;
        case IpfsWrapper::FriendEvent::Presence:
            QMetaObject::invokeMethod(this, [this, Peer, On]{ emit friendPresence(Peer, On); }, Qt::QueuedConnection);
            break;
        case IpfsWrapper::FriendEvent::Profile:
            QMetaObject::invokeMethod(this, [this, Peer, Nick, Pic]{ emit friendProfile(Peer, Nick, Pic); }, Qt::QueuedConnection);
            break;
        case IpfsWrapper::FriendEvent::Removed:
            QMetaObject::invokeMethod(this, [this, Peer]{ emit friendRemoved(Peer); }, Qt::QueuedConnection);
            break;
        }
    });

    //Route the embedded node's friend events into the callback just installed (→ these signals).
    VgSetFriendCb(&IpfsWrapper::IpfsNodeFriendCb);
}

FriendsManager * FriendsManager::instance()
{
    static FriendsManager * Inst = new FriendsManager(qApp);
    return Inst;
}

