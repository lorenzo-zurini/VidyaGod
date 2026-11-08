#include "downloadmanager.h"
#include "installed.h"
#include "shelf.h"
#include "appmodel.h"          // AppModel — catalog/config + rebuildCatalog()
#include "ipfsmodel.h"         // IpfsModel — per-CID transfer progress + size (single source of truth)
#include "packagecatalog.h"
#include "manifestmodel.h"
#include "containerwrapper.h"
#include "ipfswrapper.h"
#include "downloadqueue.h"   // CancelDownload, CloseQueue/OpenQueue
#include "commonutils.h"
#include "asyncwork.h"         // AsyncWork::Run — closure walks off the GUI thread, guarded by the dialog's lifetime

#include <QTimer>

#include <algorithm>
#include <QString>

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>


DownloadManager::DownloadManager(AppModel &model, IpfsModel &ipfs, QObject *parent)
    : QObject(parent), Model(model), Ipfs(ipfs)
{
    IpfsWrapper::OpenQueue();   // a previous manager's exit closed it (tests build several in one process)
    // Per-CID progress lives in IpfsModel now; recompute a package's averaged bar whenever one of its CIDs changes.
    connect(&Ipfs, &IpfsModel::cidChanged, this, [this](const QString &cid){ recomputeProgress(cid); });
}

void DownloadManager::cancelKey(const QString &Key)
{
    if (Key.isEmpty() || !DownloadingUids.contains(Key)) return;
    CancellingUids.insert(Key);                                   // suppress the failure dialog on abort
    CancelledCids[Key] = cancellableCids(Key);
    for (const QString & c : CancelledCids[Key]) IpfsWrapper::RequestCancel(c.toStdString());
}

QStringList DownloadManager::purgeOnCancel(const QString &Key) const
{
    QStringList Out;
    for (const QString & c : CancelledCids.value(Key))
        if (IpfsWrapper::JobUnfinished(c.toStdString())                         // a finished or never-fetched file is kept
            && (DownloadCidToUid.value(c) - QSet<QString>{Key}).isEmpty())       // and so is what another card needs now
            Out << c;
    return Out;
}

QStringList DownloadManager::cancellableCids(const QString &Key) const
{
    QStringList Out;
    for (const QString & c : DownloadUidCids.value(Key))
        if (DownloadCidToUid.value(c) == QSet<QString>{Key}) Out << c;   // shared with another card: leave it running
    return Out;
}

void DownloadManager::recomputeProgress(const QString &cid)
{
    auto it = DownloadCidToUid.constFind(cid);
    if (it == DownloadCidToUid.constEnd()) return;
    for (const QString & Key : it.value()) recomputeKeyProgress(Key);
}

void DownloadManager::recomputeKeyProgress(const QString &Key)
{
    const QStringList Cids = DownloadUidCids.value(Key);
    if (Cids.isEmpty()) { emit downloadProgress(Key, -1.0); return; }
    // SIZE-WEIGHTED average from IpfsModel (per-CID pct/size): a package's bar reflects bytes downloaded, not files
    // completed — otherwise a tiny file finishing jerks the bar (the "staggered" bug). Falls back to an equal-weight
    // average until the per-CID sizes are known. A CID with pct==-1 (queued/indeterminate) counts as 0%.
    auto Pct = [this](const QString & c){ const double p = Ipfs.pct(c); return p >= 0 ? p : 0.0; };
    double WeightedBytes = 0, TotalBytes = 0; bool HaveSizes = false;
    for (const QString & c : Cids)
    {
        const qlonglong Sz = Ipfs.size(c);
        if (Sz > 0) { WeightedBytes += Pct(c) / 100.0 * double(Sz); TotalBytes += double(Sz); HaveSizes = true; }
    }
    double Avg;
    if (HaveSizes && TotalBytes > 0) Avg = WeightedBytes / TotalBytes * 100.0;
    else { double Sum = 0; for (const QString & c : Cids) Sum += Pct(c); Avg = Sum / Cids.size(); }
    emit downloadProgress(Key, Avg);   // the Catalog card(s) connect to this
}

// Kick off (or resume) a download for an already-decided selection — the non-interactive core shared by the dialog
// path (startDownload) and crash/close resume (resumeAll). Persists the selection so an interrupted download survives
// a restart; the completion handler un-persists it (so only a genuinely interrupted one is left to resume).
void DownloadManager::beginDownload(const QString &Key, const std::vector<std::string> &LaunchIds,
                                    const std::vector<std::string> &RunnerIds, const std::map<std::string, bool> &Toggles)
{
    if (Key.isEmpty() || DownloadingUids.contains(Key)) return;       // already in flight
    DownloadingUids.insert(Key);
    persistActive(Key, LaunchIds, RunnerIds, Toggles);               // survive a crash/close → resumed next launch

    QStringList Cids;
    auto AddCids = [&](const std::vector<std::string> & CidList){
        for (const auto & C : CidList)
        {
            const QString Qc = QString::fromStdString(C);
            if (Cids.contains(Qc)) continue;
            Cids << Qc; DownloadCidToUid[Qc].insert(Key);
        }
    };
    for (const std::string & Lid : LaunchIds) AddCids(PackageCatalog::NodeContentCids(Model.catalogIndex(), Lid, TickedGrafts(Toggles)));
    for (const std::string & Rid : RunnerIds) AddCids(PackageCatalog::NodeContentCids(Model.catalogIndex(), Rid));  // runner build CIDs
    DownloadUidCids[Key] = Cids;

    // Queued rows + per-CID progress/sizes are owned by IpfsModel now (fed by the download queue's callback + the
    // node's transfer events). This just needs the cid→package map above so recomputeProgress can average the card.

    // Mark this tile's card(s) "Downloading…" in place (cheap — no pool rebuild / no filesystem restat).
    emit downloadStarted(Key);
    emit transfersChanged();

    // The node index as this worker reads it: concurrent downloads must not read the shared CatalogIndex while a
    // completing download reassigns it (rebuildCatalog). One immutable copy per rebuild, shared by every worker.
    std::shared_ptr<const NodeIndex> Snapshot = Model.catalogSnapshot();
    nlohmann::ordered_json ConfigSnap = Model.config() ? *Model.config() : nlohmann::ordered_json::object();
    //Owned, not detached: the destructor cancels and joins it, so it never posts to a destroyed manager.
    reapWorkers();
    auto Done = std::make_shared<std::atomic<bool>>(false);
    Workers.push_back({std::thread([this, Done, LaunchIds, RunnerIds, Toggles, Key, Snapshot = std::move(Snapshot), ConfigSnap = std::move(ConfigSnap)]() mutable {
        const IpfsWrapper::ForegroundHold Hold;   // closures, installs, content: one download, no gap for walks
        std::string Err; bool Ok = true;
        // Everything a download needs is in packages that have landed: a node a package contains comes with its own
        // package (a node has one home), never fetched into this one. A gap means that package is not here yet.
        std::shared_ptr<const NodeIndex> Idx = std::move(Snapshot);
        {
            std::vector<std::string> All = LaunchIds;
            All.insert(All.end(), RunnerIds.begin(), RunnerIds.end());
            for (const std::string & Lid : LaunchIds)
                for (const std::string & Rid : PackageCatalog::RunnerChainIds(*Idx, Lid, ConfigSnap)) All.push_back(Rid);
            for (const std::string & Id : All)
                if (const auto Missing = PackageCatalog::MissingFromClosure(*Idx, Id); !Missing.empty())
                {
                    Err = "'" + Id + "' contains " + std::to_string(Missing.size()) + " node(s) from a package that has not "
                          "arrived (or is not shared with you), e.g. " + Missing.front();
                    Ok = false;
                    break;
                }
        }
        // What it fetches, from an index: the selected launchables' content and each one's runner chain build, and
        // the runners ticked. Collected twice: first as a dry run over the received index, so a download that cannot
        // complete (content naming no source) fails BEFORE anything moves; then over the adopted packages' new home.
        // Moved first, a failed download left a package out of CATALOG and not installed: gone from both tabs.
        const auto Collect = [&](const NodeIndex &I, std::vector<IpfsWrapper::FetchTarget> &T, bool Warn) {
            for (const std::string & Lid : LaunchIds)
            {
                if (!PackageCatalog::CollectContentTargets(I, Lid, TickedGrafts(Toggles), T, &Err)) return false;
                //Best-effort by design — a game whose runtime cannot be resolved should still download. But silently
                //best-effort meant the download finished green and the game then would not launch, with nothing
                //anywhere connecting the two. Still non-fatal; now at least it is on the record.
                std::string CErr;
                if (!PackageCatalog::CollectRunnerChainTargets(I, Lid, ConfigSnap, T, &CErr) && Warn)
                    LogWarn("DownloadManager::beginDownload", "could not resolve the runner chain for '" + Lid
                                + "' — its runtime is NOT in this batch, so the download will complete but the game "
                                  "will not be launchable" + (CErr.empty() ? "" : " (" + CErr + ")"));
            }
            for (const std::string & Rid : RunnerIds)
                if (!PackageCatalog::CollectRunnerNodeTargets(I, Rid, T, &Err)) return false;
            return true;
        };
        if (Ok) { std::vector<IpfsWrapper::FetchTarget> Dry; Ok = Collect(*Idx, Dry, false); }
        // INSTALL = ADOPT: a received package (its nodes live under CATALOG) moves into LIBRARY/<lib>/<pkg> before a
        // byte of content is fetched, so the content lands in the library and the package is ours from here on —
        // launchable, its grafts offered, re-published with our library. Every package the closures reach
        // (PackagesToAdopt): whatever is received, adopt; then re-index once.
        if (Ok)
        {
            const std::set<std::filesystem::path> Adopt = PackageCatalog::PackagesToAdopt(*Idx, LaunchIds, RunnerIds, ConfigSnap);
            for (const std::filesystem::path & D : Adopt)
            {
                std::string AErr;
                if (!PackageCatalog::AdoptReceivedPackage(ConfigSnap, D, nullptr, &AErr)) { Err = AErr; Ok = false; break; }
            }
            if (Ok && !Adopt.empty()) Idx = PackageCatalog::FreshCatalogIndex(ConfigSnap);
        }
        // Pool EVERY fetch — all selected launchables' content layers + each launchable's RESOLVED runner chain build +
        // any extra runners the user ticked — into ONE concurrent batch, so a game downloads TOGETHER with the runtime
        // it needs (bounded by MaxConcurrentDownloads). Auto-pooling the resolved chain makes a downloaded game
        // immediately playable regardless of the runner checklist (dedup handles overlap with a ticked runner).
        std::vector<IpfsWrapper::FetchTarget> Targets;
        if (Ok) Ok = Collect(*Idx, Targets, true);
        Idx.reset();   // targets collected: the index is not held through the (long) fetch
        if (Ok && !IpfsWrapper::FetchTargetsConcurrent(Targets, &Err)) Ok = false;
        // Builds are now present locally → the runner is ready. Its prefix assembles from the build at launch
        // (node-declared layers), so there is NO post-fetch generation step — fetching the build IS the install.
        Done->store(true);   // before the post: whoever sees the result may reap this thread (its join returns at once)
        QMetaObject::invokeMethod(this, [this, Ok, Err, Key]{
            DownloadingUids.remove(Key);
            const bool Cancelled = CancellingUids.remove(Key);
            const QStringList DoneCids = DownloadUidCids.value(Key);
            const QStringList Purge = Cancelled ? purgeOnCancel(Key) : QStringList();   // before the maps forget Key
            CancelledCids.remove(Key);
            for (const QString & c : DoneCids)
            {
                auto It = DownloadCidToUid.find(c);
                if (It != DownloadCidToUid.end()) { It->remove(Key); if (It->isEmpty()) DownloadCidToUid.erase(It); }
                if (!DownloadCidToUid.contains(c)) IpfsWrapper::ClearCancel(c.toStdString());   // no card needs it now
                // IpfsModel owns the transfer rows now; a still-queued CID clears itself via the queue's callback and
                // the post-completion refresh() (transfersChanged → IpfsModel::refreshNow).
            }
            DownloadUidCids.remove(Key);
            unpersistActive(Key);           // it ended normally (success/cancel/fail) → don't resume it next launch
            if (Cancelled)
            {
                // Voluntary abort → purge the partial's cached blocks off the GUI thread so a re-download is a real
                // download (and we don't hoard the aborted bytes). A crash/close leaves them — they speed the resume.
                //Only what its cancel aborted and did not finish: purging a shared CID wiped another card's just-finished
                //files (their dag-pb blocks), silently unservable.
                std::thread([Purge]{ for (const QString & c : Purge) IpfsWrapper::DropCached(c.toStdString()); }).detach();
            }
            else if (!Ok) LogErr("DownloadManager::beginDownload", "Download failed: " + Err);   // shows as a "Failed" row
            emit downloadFinished(Key);   // the Catalog card(s) drop the "Downloading…" overlay BEFORE the rebuild reads state
            Model.rebuildCatalog();       // emits catalogChanged → Catalog rebuild + Library rebuild + IPFS refresh
            emit transfersChanged();
        }, Qt::QueuedConnection);
    }), Done});
}

void DownloadManager::reapWorkers()
{
    for (auto It = Workers.begin(); It != Workers.end(); )
        if (It->Done->load()) { It->T.join(); It = Workers.erase(It); }   // finished: the join is immediate
        else ++It;
}

DownloadManager::~DownloadManager()
{
    //Make every fetch return, then wait for the workers: none may still be running, or inside Qt, once the manager and
    //the application are gone. Cancelling only the CIDs mapped when a download began was not enough — a worker also
    //fetches content and runner chains it learns of later, and a retryable job never fails on
    //its own: quitting with an unprovidable fetch in flight hung the exit on join() for good, the lock still held.
    //Closing the queue fails everything queued, aborts everything active and fails whatever is enqueued next.
    //A worker's last act posts its result to this manager, which is still whole while it waits here; Qt drops that
    //event with the object.
    IpfsWrapper::CloseQueue();
    for (Worker &W : Workers) if (W.T.joinable()) W.T.join();
}

// ── Persistence of in-flight downloads (Settings.ActiveDownloads) so a crash/close resumes them next launch ──

void DownloadManager::persistActive(const QString &Key, const std::vector<std::string> &LaunchIds,
                                    const std::vector<std::string> &RunnerIds, const std::map<std::string, bool> &Toggles)
{
    auto & S = (*Model.config())["Settings"];
    if (!S.contains("ActiveDownloads") || !S["ActiveDownloads"].is_array()) S["ActiveDownloads"] = nlohmann::ordered_json::array();
    auto & Arr = S["ActiveDownloads"];
    for (auto It = Arr.begin(); It != Arr.end(); ++It)                 // replace any existing record for this Key
        if (It->is_object() && It->value("GameKey", std::string()) == Key.toStdString()) { Arr.erase(It); break; }
    nlohmann::ordered_json Rec = nlohmann::ordered_json::object();
    Rec["GameKey"] = Key.toStdString();
    Rec["Launch"]   = LaunchIds;
    Rec["Runners"]  = RunnerIds;
    Rec["Toggles"]  = Toggles;
    Arr.push_back(Rec);
    Model.save();
}

void DownloadManager::unpersistActive(const QString &Key)
{
    auto & S = (*Model.config())["Settings"];
    if (!S.contains("ActiveDownloads") || !S["ActiveDownloads"].is_array()) return;
    auto & Arr = S["ActiveDownloads"];
    for (auto It = Arr.begin(); It != Arr.end(); ++It)
        if (It->is_object() && It->value("GameKey", std::string()) == Key.toStdString()) { Arr.erase(It); Model.save(); return; }
}

void DownloadManager::resumeAll()
{
    auto & S = (*Model.config())["Settings"];
    if (!S.contains("ActiveDownloads") || !S["ActiveDownloads"].is_array()) return;
    // Copy out first — beginDownload re-persists (mutating the array) as we iterate.
    const nlohmann::ordered_json Records = S["ActiveDownloads"];
    for (const auto & Rec : Records)
    {
        if (!Rec.is_object()) continue;
        const QString Key = QString::fromStdString(Rec.value("GameKey", std::string()));
        if (Key.isEmpty()) continue;
        std::vector<std::string> LaunchIds = Rec.value("Launch",  std::vector<std::string>{});
        std::vector<std::string> RunnerIds = Rec.value("Runners", std::vector<std::string>{});
        std::map<std::string, bool> Toggles;
        if (Rec.contains("Toggles") && Rec["Toggles"].is_object())
            for (auto It = Rec["Toggles"].begin(); It != Rec["Toggles"].end(); ++It)
                if (It.value().is_boolean()) Toggles[It.key()] = It.value().get<bool>();
        LogOut("DownloadManager::resumeAll", "Resuming interrupted download: " + Key.toStdString());
        beginDownload(Key, LaunchIds, RunnerIds, Toggles);
    }
}
