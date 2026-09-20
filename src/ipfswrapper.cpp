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
           : kind == 4 ? TransferEvent::Phase
                       : TransferEvent::Finished;
    E.Cid     = cid ? cid : "";
    E.Percent = percent;
    E.Ok      = ok != 0;
    if (E.Kind == TransferEvent::Phase) E.Text = err ? err : "";   // the phase text rides the err parameter
    else                                E.Error = err ? err : "";
    if (g_FetchDbg) {
        const char *K = E.Kind == TransferEvent::Started    ? "Started"
                      : E.Kind == TransferEvent::Progress    ? "Progress"
                      : E.Kind == TransferEvent::Finalizing  ? "Finalizing"
                      : E.Kind == TransferEvent::Phase       ? "Phase"
                                                             : "Finished";
        char Buf[256];
        std::snprintf(Buf, sizeof Buf, "TransferEvent %s cid=%s pct=%.1f ok=%d %s=%s",
                      K, E.Cid.c_str(), E.Percent, E.Ok ? 1 : 0,
                      E.Kind == TransferEvent::Phase ? "text" : "err",
                      E.Kind == TransferEvent::Phase ? E.Text.c_str() : E.Error.c_str());
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
void SetExpectedSize(const std::string &Cid, long long Size) { VgSetExpectedSize(Cid.c_str(), Size); }

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

// ---- dag-json node graph (the gigagraph: one node = one dag-json block, identity = CID) ----
// The node's JSON is canonicalized in Go (deterministic — any key order yields the same CID); the block is
// direct-pinned and announced. Links (PARENTS/SOURCE.CID/COVER) travel as dag-json links {"/":cid} in the stored
// block; the caller passes/receives that form (nodegraph.cpp normalizes ↔ plain-string CIDs at the ingest/mint edge).

std::string DagPut(const std::string &Json, std::string *Error)
{
    if (Json.empty()) { if (Error) *Error = "empty node JSON"; return std::string(); }
    char *Cid = nullptr, *Err = nullptr;
    const int Rc = VgDagPut(Json.c_str(), &Cid, &Err);
    const std::string CidS = TakeStr(Cid);
    const std::string ErrS = TakeStr(Err);
    if (Rc != 0) { if (Error) *Error = ErrS.empty() ? "dag put failed" : ErrS; return std::string(); }
    return CidS;
}

std::string DagGet(const std::string &Cid, std::string *Error)
{
    if (Cid.empty()) { if (Error) *Error = "empty CID"; return std::string(); }
    char *Json = nullptr, *Err = nullptr;
    const int Rc = VgDagGet(Cid.c_str(), &Json, &Err);
    const std::string JsonS = TakeStr(Json);
    const std::string ErrS  = TakeStr(Err);
    if (Rc != 0) { if (Error) *Error = ErrS.empty() ? ("dag get failed: " + Cid) : ErrS; return std::string(); }
    return JsonS;
}

std::map<std::string, std::string> DagGetMany(const std::vector<std::string> &Cids)
{
    std::map<std::string, std::string> Out;
    if (Cids.empty()) return Out;
    nlohmann::json Arr = nlohmann::json::array();
    for (const std::string &C : Cids) Arr.push_back(C);
    char *OutJson = nullptr, *Err = nullptr;
    const int Rc = VgDagGetMany(Arr.dump().c_str(), &OutJson, &Err);
    const std::string JsonS = TakeStr(OutJson);
    const std::string ErrS  = TakeStr(Err);
    if (Rc != 0) { LogWarn("IpfsWrapper::DagGetMany", ErrS.empty() ? "batched dag-get failed" : ErrS); return Out; }
    try
    {
        const nlohmann::json O = nlohmann::json::parse(JsonS);
        if (O.is_object())
            for (auto It = O.begin(); It != O.end(); ++It)
                Out[It.key()] = It.value().dump();
    }
    catch (const std::exception &Ex)
    {
        LogWarn("IpfsWrapper::DagGetMany", std::string("bad batched dag-get JSON: ") + Ex.what());
    }
    return Out;
}

std::map<std::string, std::string> DagGetManyLocal(const std::vector<std::string> &Cids)
{
    std::map<std::string, std::string> Out;
    if (Cids.empty()) return Out;
    nlohmann::json Arr = nlohmann::json::array();
    for (const std::string &C : Cids) Arr.push_back(C);
    char *OutJson = nullptr, *Err = nullptr;
    const int Rc = VgDagGetManyLocal(Arr.dump().c_str(), &OutJson, &Err);
    const std::string JsonS = TakeStr(OutJson);
    const std::string ErrS  = TakeStr(Err);
    if (Rc != 0) { LogWarn("IpfsWrapper::DagGetManyLocal", ErrS.empty() ? "local batched dag-get failed" : ErrS); return Out; }
    try
    {
        const nlohmann::json O = nlohmann::json::parse(JsonS);
        if (O.is_object())
            for (auto It = O.begin(); It != O.end(); ++It)
                Out[It.key()] = It.value().dump();
    }
    catch (const std::exception &Ex)
    {
        LogWarn("IpfsWrapper::DagGetManyLocal", std::string("bad local batched dag-get JSON: ") + Ex.what());
    }
    return Out;
}

bool DagHas(const std::string &Cid)
{
    if (Cid.empty()) return false;
    return VgDagHas(Cid.c_str()) == 1;
}

std::string DagCid(const std::string &Json, std::string *Error)
{
    if (Json.empty()) { if (Error) *Error = "empty node JSON"; return std::string(); }
    char *Cid = nullptr, *Err = nullptr;
    const int Rc = VgDagCid(Json.c_str(), &Cid, &Err);
    const std::string CidS = TakeStr(Cid);
    const std::string ErrS = TakeStr(Err);
    if (Rc != 0) { if (Error) *Error = ErrS.empty() ? "dag cid failed" : ErrS; return std::string(); }
    return CidS;
}

// FetchOnce — the rolling queue's node primitive: ONE fetch attempt, classified. FetchToPath/FetchDirToPath (the
// blocking convenience wrappers) and the batch entry point now live in downloadqueue.cpp and route through the queue,
// whose dispatcher calls THIS to run each attempt and reads the rc to decide Done / rotate-and-retry / fail.
static std::mutex     g_FetchOnceHookMu;
static FetchOnceHook g_FetchOnceHook;
void SetFetchOnceHook(FetchOnceHook Hook)
{
    std::lock_guard<std::mutex> Lk(g_FetchOnceHookMu);
    g_FetchOnceHook = std::move(Hook);
}

// True when a scripted FetchOnce hook is installed (test mode). The dir/batch fetch paths consult this to bypass their
// DaemonRunning() offline-gate — under a hook there is no daemon, but the hook services the fetch. Production: false.
bool FetchOnceHookActive()
{
    std::lock_guard<std::mutex> Lk(g_FetchOnceHookMu);
    return static_cast<bool>(g_FetchOnceHook);
}

int FetchOnce(const std::string &Cid, const std::string &Dest, bool Dir, std::string *Error)
{
    // Copy the hook under the lock, invoke it OUTSIDE — a detached worker must never read the std::function while a
    // test swaps it (check-then-call UB). No-op in production (hook unset).
    FetchOnceHook Hook;
    { std::lock_guard<std::mutex> Lk(g_FetchOnceHookMu); Hook = g_FetchOnceHook; }
    if (Hook) return Hook(Cid, Dest, Dir, Error);
    if (Cid.empty())  { if (Error) *Error = "empty CID";              return 2; }
    if (Dest.empty()) { if (Error) *Error = "empty destination path"; return 2; }

    // File already materialized (and not a crashed mid-finalize, which leaves a `<dest>.part`) → Done with no node
    // call. A rotated job can re-enter here after another dest of the same CID landed it. Dir fetches always call in
    // (a partial tree has no single-file marker).
    if (!Dir && QFileInfo::exists(QString::fromStdString(Dest))
        && !QFileInfo::exists(QString::fromStdString(Dest + ".part")))
        return 0;

    FetchDbg("FetchOnce ENTER cid=" + Cid + " dest=" + Dest + (Dir ? " [dir]" : ""));
    char *Err = nullptr;
    const int Rc = VgFetchOnce(Cid.c_str(), Dest.c_str(), Dir ? 1 : 0, &Err);
    const std::string ErrS = TakeStr(Err);
    FetchDbg("FetchOnce RETURN rc=" + std::to_string(Rc) + " err='" + ErrS + "' cid=" + Cid);
    if (Rc != 0 && Error) *Error = ErrS.empty() ? ("fetch failed for CID " + Cid) : ErrS;
    return Rc;   // 0/1/2 straight through to the dispatcher
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

std::string CidServeStatus(const std::string &Cid)
{
    if (Cid.empty()) return "empty cid";
    return TakeStr(VgCidServeStatus(Cid.c_str()));
}

std::vector<ServeFailure> ServeFailures()
{
    std::vector<ServeFailure> Result;
    char *J = nullptr;
    const int Rc = VgServeFailures(&J);
    const std::string Js = TakeStr(J);
    if (Rc != 0) return Result;
    try {
        for (const auto &E : nlohmann::json::parse(Js))
            Result.push_back({ E.value("cid", std::string()), E.value("err", std::string()), E.value("when", 0LL) });
    } catch (const std::exception &E) { LogWarn("IpfsWrapper::ServeFailures", std::string("bad JSON: ") + E.what()); }
    return Result;
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

bool IpnsPublish(const std::string &Cid, int TtlSeconds, std::string *Error)
{
    if (Cid.empty()) { if (Error) *Error = "empty CID"; return false; }
    char *Err = nullptr;
    const int Rc = VgIpnsPublish(Cid.c_str(), TtlSeconds, &Err);
    const std::string ErrS = TakeStr(Err);
    if (Rc != 0 && Error) *Error = ErrS.empty() ? ("IPNS publish failed for " + Cid) : ErrS;
    return Rc == 0;
}

static std::mutex       g_IpnsResolveHookMu;
static IpnsResolveHook  g_IpnsResolveHook;
void SetIpnsResolveHook(IpnsResolveHook Hook)
{
    std::lock_guard<std::mutex> Lk(g_IpnsResolveHookMu);
    g_IpnsResolveHook = std::move(Hook);
}

std::string IpnsResolve(const std::string &Name, std::string *Error)
{
    if (Name.empty()) { if (Error) *Error = "empty IPNS name"; return {}; }
    { IpnsResolveHook Hook;
      { std::lock_guard<std::mutex> Lk(g_IpnsResolveHookMu); Hook = g_IpnsResolveHook; }
      if (Hook) return Hook(Name, Error); }
    char *Out = nullptr, *Err = nullptr;
    const int Rc = VgIpnsResolve(Name.c_str(), &Out, &Err);
    const std::string ErrS = TakeStr(Err);
    std::string Path = TakeStr(Out);
    if (Rc != 0) { if (Error) *Error = ErrS.empty() ? ("IPNS resolve failed for " + Name) : ErrS; return {}; }
    // The node returns a full "/ipfs/<cid>" path; hand callers the bare CID (the first path segment after /ipfs/).
    const std::string Prefix = "/ipfs/";
    if (Path.rfind(Prefix, 0) == 0) Path = Path.substr(Prefix.size());
    if (const auto Slash = Path.find('/'); Slash != std::string::npos) Path = Path.substr(0, Slash);
    return Path;
}

bool ExportIdentity(const std::string &DestPath, std::string *Error)
{
    if (DestPath.empty()) { if (Error) *Error = "empty destination path"; return false; }
    char *Err = nullptr;
    const int Rc = VgExportIdentity(DestPath.c_str(), &Err);
    const std::string ErrS = TakeStr(Err);
    if (Rc != 0 && Error) *Error = ErrS.empty() ? "identity export failed" : ErrS;
    return Rc == 0;
}

bool ImportIdentity(const std::string &SrcPath, std::string *Error)
{
    if (SrcPath.empty()) { if (Error) *Error = "empty source path"; return false; }
    char *Err = nullptr;
    const int Rc = VgImportIdentity(SrcPath.c_str(), &Err);
    const std::string ErrS = TakeStr(Err);
    if (Rc != 0 && Error) *Error = ErrS.empty() ? "identity import failed" : ErrS;
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
    //UNKNOWN kinds are DROPPED, never defaulted. The catch-all used to be `: Removed`, so the first event kind a
    //newer Go build adds would arrive as a contact REMOVAL and quietly delete contacts — a documented historical
    //bug this mapping re-armed once already (adversarial M4). Kind 5 is matched explicitly; anything else logs.
    // EVERY kind is mapped EXPLICITLY; anything with no case is DROPPED, never defaulted. The catch-all used to be
    // `: Removed`, so the first event kind a newer Go build adds would arrive as a contact REMOVAL and quietly delete
    // contacts — a documented historical bug (adversarial M4). A ternary catch-all for Library re-armed the same trap
    // (M7): the NEXT new kind would masquerade as a library event. So switch, no fall-through default.
    switch (kind)
    {
    case 0: E.Kind = FriendEvent::Request;  break;
    case 1: E.Kind = FriendEvent::Accept;   break;
    case 2: E.Kind = FriendEvent::Decline;  break;
    case 3: E.Kind = FriendEvent::Presence; break;
    case 4: E.Kind = FriendEvent::Profile;  break;
    case 5: E.Kind = FriendEvent::Removed;  break;
    case 6: E.Kind = FriendEvent::Library;  break;
    default:
        LogWarn("IpfsWrapper::FriendEventTrampoline", "unknown friend event kind " + std::to_string(kind) + " — dropped");
        return;
    }
    try
    {
        const nlohmann::json J = nlohmann::json::parse(json ? json : "{}");
        if (E.Kind == FriendEvent::Library)
        {
            // {peer, libs:{name:[cid,…]}, seq} — a friend's COMPLETE shared set (a snapshot we replace wholesale), not
            // a contact. Carry the libs object verbatim as JSON; AppModel parses + replaces its record for this peer,
            // ordered by seq (last-writer-wins) so a stale snapshot delivered late can't overwrite a fresher one.
            E.C.PeerID = J.value("peer", std::string());
            E.LibsJson = (J.contains("libs") && J["libs"].is_object()) ? J["libs"].dump() : "{}";
            if (J.contains("seq") && J["seq"].is_number_unsigned()) E.LibSeq = J["seq"].get<quint64>();
        }
        else E.C = ContactFromJson(J);
    }
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

bool ShareLibrary(const std::string &PeerID, const std::string &Lib, const nlohmann::json &Items, std::string *Error)
{
    char *Err = nullptr;
    const int Rc = VgShareLibrary(PeerID.c_str(), Lib.c_str(), Items.dump().c_str(), &Err);
    const std::string ErrS = TakeStr(Err);
    if (Rc != 0) { if (Error) *Error = ErrS.empty() ? "share library failed" : ErrS; return false; }
    return true;
}

bool UnshareLibrary(const std::string &PeerID, const std::string &Lib, std::string *Error)
{
    char *Err = nullptr;
    const int Rc = VgUnshareLibrary(PeerID.c_str(), Lib.c_str(), &Err);
    const std::string ErrS = TakeStr(Err);
    if (Rc != 0) { if (Error) *Error = ErrS.empty() ? "unshare library failed" : ErrS; return false; }
    return true;
}

bool RequestFriendLibraries(const std::string &PeerID, std::string *Error)
{
    char *Err = nullptr;
    const int Rc = VgRequestFriendLibraries(PeerID.c_str(), &Err);
    const std::string ErrS = TakeStr(Err);
    if (Rc != 0) { if (Error) *Error = ErrS.empty() ? "request friend libraries failed" : ErrS; return false; }
    return true;
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

std::vector<NetCheck> NetworkTest(bool *NodeOffline)
{
    std::vector<NetCheck> Out;
    if (NodeOffline) *NodeOffline = false;
    char *J = nullptr;
    if (VgNetworkTest(&J) != 0)
    {
        TakeStr(J);
        if (NodeOffline) *NodeOffline = true;
        return Out;
    }
    const std::string Js = TakeStr(J);
    try
    {
        for (const auto &E : nlohmann::json::parse(Js))
            Out.push_back({E.value("name", std::string()), E.value("status", std::string("fail")),
                           E.value("detail", std::string())});
    }
    catch (const std::exception &E) { LogWarn("IpfsWrapper::NetworkTest", std::string("bad JSON from node: ") + E.what()); }
    return Out;
}

std::vector<NetCheck> ServiceHealth()
{
    std::vector<NetCheck> Out;
    char *J = nullptr;
    if (VgHealth(&J) != 0) { TakeStr(J); return Out; }
    const std::string Js = TakeStr(J);
    try
    {
        for (const auto &E : nlohmann::json::parse(Js))
            Out.push_back({E.value("name", std::string()), E.value("status", std::string("down")),
                           E.value("detail", std::string())});
    }
    catch (const std::exception &E) { LogWarn("IpfsWrapper::ServiceHealth", std::string("bad JSON from node: ") + E.what()); }
    return Out;
}

void SetLanExcluded(const std::vector<std::string> &PeerIds)
{
    std::string Csv;
    for (const std::string &P : PeerIds) { if (!Csv.empty()) Csv += ","; Csv += P; }
    VgLanSetExcluded(Csv.c_str());
}

bool SetPresenceDeny(const std::vector<std::string> &PeerIds)
{
    nlohmann::json Arr = nlohmann::json::array();
    for (const std::string &P : PeerIds) Arr.push_back(P);
    return VgSetPresenceDeny(Arr.dump().c_str()) == 0;
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
namespace {
// Per-CID last narration line, for dedup of consecutive identical phase events. Entries live ONLY while a transfer
// is in flight (forgotten on Started and Finished), so the map is bounded and a retry is never deduped against the
// previous run. Touched from the node's fetch threads.
std::mutex g_PhaseMu;
std::unordered_map<std::string, std::string> g_LastPhase;
bool phaseChanged(const std::string & Cid, const std::string & Text)
{
    std::lock_guard<std::mutex> Lk(g_PhaseMu);
    auto It = g_LastPhase.find(Cid);
    if (It != g_LastPhase.end() && It->second == Text) return false;
    g_LastPhase[Cid] = Text;
    return true;
}
void phaseForget(const std::string & Cid) { std::lock_guard<std::mutex> Lk(g_PhaseMu); g_LastPhase.erase(Cid); }
} // namespace

IpfsManager::IpfsManager(QObject * parent) : QObject(parent)
{
    //Relay backend transfer events (which fire on the node's fetch thread) onto this object's thread
    //(the GUI thread) as Qt signals, via a queued invocation.
    IpfsWrapper::SetTransferCallback([this](const IpfsWrapper::TransferEvent &E) {
        const QString Cid = QString::fromStdString(E.Cid);
        switch (E.Kind)
        {
        case IpfsWrapper::TransferEvent::Started:
            phaseForget(E.Cid);   // a new lifecycle narrates from scratch (a retry's first line must not be deduped
                                  // against the previous run's last one — Started just cleared the row's activity)
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
        case IpfsWrapper::TransferEvent::Phase: {
            // One narration line per STATE CHANGE: consecutive identical texts per CID are dropped here so a
            // re-emitting loop can never spam the GUI queue. Runs on the node's fetch thread(s).
            const QString Text = QString::fromStdString(E.Text);
            if (!phaseChanged(E.Cid, E.Text)) break;
            QMetaObject::invokeMethod(this, [this, Cid, Text]{ emit transferPhase(Cid, Text); }, Qt::QueuedConnection);
            break; }
        case IpfsWrapper::TransferEvent::Finalizing: {
            const double Pct = E.Percent;
            QMetaObject::invokeMethod(this, [this, Cid, Pct]{ emit transferFinalizing(Cid, Pct); }, Qt::QueuedConnection);
            break; }
        case IpfsWrapper::TransferEvent::Finished: {
            phaseForget(E.Cid);   // no per-CID residue: the map is bounded by transfers currently IN FLIGHT
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
        case IpfsWrapper::FriendEvent::Library:
        {
            const QString LibsJson = QString::fromStdString(E.LibsJson);
            const quint64 Seq = E.LibSeq;
            QMetaObject::invokeMethod(this, [this, Peer, LibsJson, Seq]{ emit friendLibrary(Peer, LibsJson, Seq); }, Qt::QueuedConnection);
            break;
        }
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

