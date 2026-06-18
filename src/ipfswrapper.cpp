#include "ipfswrapper.h"
#include "commonutils.h"
#include "vgipfsapi.h"

#include <QString>
#include <QFileInfo>
#include <QCoreApplication>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <string>
#include <utility>

// IpfsWrapper now drives the embedded in-process IPFS node (external/VidyaGodIPFS → libvgipfs.so) via its C ABI
// (vgipfsapi.h) instead of shelling out to the external Kubo `ipfs` CLI. The public API and the IpfsManager signal
// hub are unchanged, so every call site (hydration, launch-time layer fetch, cover cache, publish, the IPFS tab)
// is untouched. The node is started once at process startup in main.cpp (VgStart) and torn down at exit.

namespace IpfsWrapper {

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
                       : TransferEvent::Finished;
    E.Cid     = cid ? cid : "";
    E.Percent = percent;
    E.Ok      = ok != 0;
    E.Error   = err ? err : "";
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

void RequestCancel(const std::string &Cid) { VgRequestCancel(Cid.c_str()); }
void ClearCancel(const std::string &Cid)   { VgClearCancel(Cid.c_str()); }

bool Available()
{
    // The node is compiled in; "available" means it actually opened its repo at startup (VgStart succeeded).
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
    char *Err = nullptr;
    const int Rc = VgFetchToPath(Cid.c_str(), DestPathStr.c_str(), &Err);
    const std::string ErrS = TakeStr(Err);
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

int PeerCount()
{
    return VgPeerCount();
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
            QMetaObject::invokeMethod(this, [this, Cid, Pct]{ emit transferProgress(Cid, Pct); }, Qt::QueuedConnection);
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
