#include "ipfswrapper.h"
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
#include <atomic>
#include <thread>
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
static std::string HumanBytes(long long B)
{
    if (B < 0) return "?";
    static const char *U[] = { "B", "KB", "MB", "GB", "TB", "PB" };
    double V = double(B); int I = 0;
    while (V >= 1024.0 && I < 5) { V /= 1024.0; ++I; }
    char Buf[32];
    std::snprintf(Buf, sizeof Buf, (I > 0 && V < 10.0) ? "%.1f %s" : "%.0f %s", V, U[I]);
    return Buf;
}

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

bool FetchTargetsConcurrent(const std::vector<FetchTarget> &Targets, std::string *Error)
{
    std::atomic<bool> Failed{false};
    std::mutex ErrMu; std::string Err;
    std::vector<std::thread> Workers;
    Workers.reserve(Targets.size());

    // Acquire a global download slot BEFORE spawning each worker, so live workers (and concurrent FetchToPath
    // calls) never exceed MaxConcurrentDownloads — when one finishes and releases its slot, the loop unblocks and
    // starts the next. A required fetch failing flips Failed, which stops dispatching further work.
    for (const FetchTarget &T : Targets)
    {
        if (Failed.load()) break;
        DownloadSlot Slot;                     // blocks here until a concurrency slot frees
        if (Failed.load()) break;              // a worker may have failed while we waited for the slot
        Workers.emplace_back([&, T, Slot = std::move(Slot)]() mutable {
            std::string E;
            if (!FetchToPath(T.Cid, T.LocalPath, &E).empty()) return;
            if (T.Optional) { LogWarn("IpfsWrapper::FetchTargetsConcurrent", "optional fetch failed for CID " + T.Cid + " (" + E + ")"); return; }
            { std::lock_guard<std::mutex> Lk(ErrMu); if (Err.empty()) Err = "could not fetch CID " + T.Cid + " (" + E + ")"; }
            Failed.store(true);
        });
    }
    for (std::thread &W : Workers) W.join();

    if (Failed.load()) { if (Error) *Error = Err; return false; }
    return true;
}

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
    } catch (...) {}
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
    } catch (...) { return std::string(); }
}

long long CidSize(const std::string &Cid)
{
    if (Cid.empty()) return -1;
    return VgCidSize(Cid.c_str());
}

int ProviderCount(const std::string &Cid)
{
    if (Cid.empty()) return -1;
    // "How replicated" signal via the DHT — a slow walk, so cap it at a short deadline (mirrors the old findprovs).
    return VgProviderCount(Cid.c_str(), 8000);
}

bool CidMissing(const std::string &Cid)
{
    if (Cid.empty()) return false;
    return VgCidMissing(Cid.c_str()) == 1;
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
    try { for (const auto &A : nlohmann::json::parse(Js)) Out.push_back(A.get<std::string>()); } catch (...) {}
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
    } catch (...) {}
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
}

IpfsManager * IpfsManager::instance()
{
    //Parented to the application so it outlives any tab rebuild and is created on the GUI thread.
    static IpfsManager * Inst = new IpfsManager(qApp);
    return Inst;
}
