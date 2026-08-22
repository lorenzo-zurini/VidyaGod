#include "downloadmanager.h"
#include "appmodel.h"          // AppModel — catalog/config + rebuildCatalog()
#include "ipfsmodel.h"         // IpfsModel — per-CID transfer progress + size (single source of truth)
#include "libraryview.h"       // LibraryGameCard + IpfsFetchReady()
#include "packagecatalog.h"
#include "manifestmodel.h"
#include "containerwrapper.h"
#include "ipfswrapper.h"
#include "commonutils.h"

#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QMessageBox>
#include <QString>

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "guiformat.h"   // HumanBytesQ — the one shared GUI byte formatter

DownloadManager::DownloadManager(AppModel &model, IpfsModel &ipfs, QWidget *dialogParent, QObject *parent)
    : QObject(parent), Model(model), Ipfs(ipfs), DialogParent(dialogParent)
{
    // Per-CID progress lives in IpfsModel now; recompute a package's averaged bar whenever one of its CIDs changes.
    connect(&Ipfs, &IpfsModel::cidChanged, this, [this](const QString &cid){ recomputeProgress(cid); });
}

void DownloadManager::requestCancel(LibraryGameCard *card)
{
    if (!card) return;
    const QString Key = card->GameKey;
    if (Key.isEmpty() || !DownloadingUids.contains(Key)) return;
    if (QMessageBox::question(DialogParent, "Cancel download?",
            "Stop downloading “" + card->GameTitle + "”?") != QMessageBox::Yes) return;
    CancellingUids.insert(Key);                                   // suppress the failure dialog on abort
    for (const QString & c : DownloadUidCids.value(Key)) IpfsWrapper::RequestCancel(c.toStdString());
}

void DownloadManager::recomputeProgress(const QString &cid)
{
    auto it = DownloadCidToUid.constFind(cid);
    if (it == DownloadCidToUid.constEnd()) return;
    const QString Key = it.value();
    const QStringList & Cids = DownloadUidCids[Key];
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

void DownloadManager::startDownload(LibraryGameCard *card)
{
    if (!card) return;
    if (!IpfsFetchReady(DialogParent)) return;
    const QString Key = card->GameKey;
    if (Key.isEmpty() || DownloadingUids.contains(Key)) return;       // already in flight

    // ── Pre-download picker (per PACKAGE): the base game's editions + the package's other games (the Catalog
    // overlay "secondaries") as optionals + optional content. Each secondary contributes its recommended edition. ──
    const std::vector<std::string> Variants = card->GroupNodeIds;     // the base game's launchable nodes
    std::vector<std::pair<std::string, QString>> Secondaries;         // (recommended-edition launch id, title)
    for (LibraryGameCard * sc : card->Secondaries)
        if (sc && !sc->RepNodeId.empty()) Secondaries.push_back({ sc->RepNodeId, sc->GameTitle });

    std::vector<std::string> AllLaunch = Variants;                    // every selectable launch node (base + secondaries)
    for (const auto & [Lid, T] : Secondaries) AllLaunch.push_back(Lid);

    std::vector<const Node*> Opts; std::set<std::string> OptSeen;     // union of OPTIONAL content across all of them
    for (const std::string & Lid : AllLaunch)
        for (const Node * O : ManifestModel::OptionalNodes(Model.catalogIndex(), Lid))
            if (OptSeen.insert(O->NodeId).second) Opts.push_back(O);

    QDialog Dlg(DialogParent);
    Dlg.setWindowTitle("Download — " + card->GameTitle);
    Dlg.setMinimumWidth(420);
    QVBoxLayout * DL = new QVBoxLayout(&Dlg);
    DL->addWidget(new QLabel("Choose what to download:", &Dlg));

    std::map<std::string, QCheckBox*> GameChecks;                     // launch id → checkbox (base editions + secondaries)
    {
        QGroupBox * Box = new QGroupBox(Variants.size() > 1 ? "Variants" : "Game", &Dlg);
        QVBoxLayout * BL = new QVBoxLayout(Box);
        for (const std::string & Lid : Variants)
        {
            const Node * N = Model.catalogIndex().Find(Lid);
            std::string Lbl = (N && !N->Label.empty()) ? N->Label
                              : (N && N->Meta.is_object() ? N->Meta.value("TITLE", Lid) : Lid);
            const int Items = (int)PackageCatalog::NodeContentCids(Model.catalogIndex(), Lid).size();
            QCheckBox * cb = new QCheckBox(QString::fromStdString(Lbl) + QString("   (%1 file%2)").arg(Items).arg(Items == 1 ? "" : "s"), Box);
            cb->setChecked(true);
            GameChecks[Lid] = cb; BL->addWidget(cb);
        }
        DL->addWidget(Box);
    }
    if (!Secondaries.empty())                                         // the package's other games — optional, off by default
    {
        QGroupBox * Box = new QGroupBox("Other games in this package", &Dlg);
        QVBoxLayout * BL = new QVBoxLayout(Box);
        for (const auto & [Lid, Title] : Secondaries)
        {
            const int Items = (int)PackageCatalog::NodeContentCids(Model.catalogIndex(), Lid).size();
            QCheckBox * cb = new QCheckBox(Title + QString("   (%1 file%2)").arg(Items).arg(Items == 1 ? "" : "s"), Box);
            cb->setChecked(false);
            GameChecks[Lid] = cb; BL->addWidget(cb);
        }
        DL->addWidget(Box);
    }
    std::map<std::string, QCheckBox*> OptChecks;
    if (!Opts.empty())
    {
        QGroupBox * Box = new QGroupBox("Optional content", &Dlg);
        QVBoxLayout * BL = new QVBoxLayout(Box);
        for (const Node * O : Opts)
        {
            QCheckBox * cb = new QCheckBox(QString::fromStdString(O->Label.empty() ? O->NodeId : O->Label), Box);
            cb->setChecked(O->Default);
            OptChecks[O->NodeId] = cb; BL->addWidget(cb);
        }
        DL->addWidget(Box);
    }
    // ── Available Runners: the package's EMBEDDED runner(s), plus a compatible GLOBAL runner only when none is
    //    installed — each downloadable runner an optional checkbox (default on). Downloading one installs it. ──
    std::map<std::string, QCheckBox*> RunnerChecks;                   // runner node id → checkbox
    {
        const Node * BaseGame = Variants.empty() ? nullptr : Model.catalogIndex().Find(Variants.front());
        if (BaseGame)
        {
            std::vector<const Node*> EmbeddedR, GlobalR; bool AnyCompatInstalled = false;
            for (const Node * R : PackageCatalog::CompatibleRunners(Model.catalogIndex(), *BaseGame))
            {
                const std::string rid = R->NodeId;
                const bool Embedded   = (R->BundleDir == BaseGame->BundleDir);                    // embedded in THIS package
                const bool Standalone = !PackageCatalog::IsEmbeddedRunner(Model.catalogIndex(), rid);  // not bundled in any game
                if (!Embedded && !Standalone) continue;                                           // another game's embedded runner
                if (PackageCatalog::RunnerInstalled(Model.catalogIndex(), rid)) { AnyCompatInstalled = true; continue; }
                if (PackageCatalog::NodeContentCids(Model.catalogIndex(), rid).empty()) continue;      // PATH runner — not downloadable
                (Embedded ? EmbeddedR : GlobalR).push_back(R);
            }
            const bool ShowGlobal = !AnyCompatInstalled && !GlobalR.empty();
            if (!EmbeddedR.empty() || ShowGlobal)
            {
                QGroupBox * Box = new QGroupBox("Available Runners", &Dlg);
                QVBoxLayout * BL = new QVBoxLayout(Box);
                auto addRunner = [&](const Node * R){
                    const std::string rid = R->NodeId;
                    const int Items = (int)PackageCatalog::NodeContentCids(Model.catalogIndex(), rid).size();
                    QCheckBox * cb = new QCheckBox(QString::fromStdString(R->Label.empty() ? rid : R->Label)
                        + QString("   (%1 file%2)").arg(Items).arg(Items == 1 ? "" : "s"), Box);
                    cb->setChecked(true);
                    RunnerChecks[rid] = cb; BL->addWidget(cb);
                };
                if (!EmbeddedR.empty())
                {
                    QLabel * h = new QLabel("Embedded", Box); h->setStyleSheet("color:#8f98a0; font-weight:bold;");
                    BL->addWidget(h);
                    for (const Node * R : EmbeddedR) addRunner(R);
                }
                if (ShowGlobal)
                {
                    QLabel * h = new QLabel("Global", Box); h->setStyleSheet("color:#8f98a0; font-weight:bold;");
                    BL->addWidget(h);
                    for (const Node * R : GlobalR) addRunner(R);
                }
                DL->addWidget(Box);
            }
        }
    }
    // ── Disk-space display: free space at the library + the (async-gathered) size of the current selection ──
    QLabel * SizeLabel = new QLabel(&Dlg);
    DL->addWidget(SizeLabel);
    std::error_code DiskEc;
    const auto DiskSp = std::filesystem::space(PackageCatalog::LibraryRootDir(*Model.config()), DiskEc);
    const long long FreeBytes = DiskEc ? -1 : (long long)DiskSp.available;

    auto SizeCache = std::make_shared<std::map<std::string, long long>>();   // CID → bytes (filled async)
    auto Alive     = std::make_shared<std::atomic<bool>>(true);             // false once the dialog closes
    auto Recompute = [this, GameChecks, OptChecks, RunnerChecks, AllLaunch, SizeCache, SizeLabel, FreeBytes]() {
        std::map<std::string, bool> Tg; for (const auto & [Id, Cb] : OptChecks) Tg[Id] = Cb->isChecked();
        std::set<std::string> Sel;
        for (const std::string & Lid : AllLaunch)
            if (GameChecks.at(Lid)->isChecked())
                for (const auto & C : PackageCatalog::NodeContentCids(Model.catalogIndex(), Lid, Tg)) Sel.insert(C);
        for (const auto & [Rid, Cb] : RunnerChecks)
            if (Cb->isChecked())
                for (const auto & C : PackageCatalog::NodeContentCids(Model.catalogIndex(), Rid)) Sel.insert(C);
        long long Sum = 0; bool AllKnown = true;
        for (const std::string & C : Sel) { auto It = SizeCache->find(C); if (It != SizeCache->end() && It->second >= 0) Sum += It->second; else AllKnown = false; }
        SizeLabel->setText(QString("Free space: %1      Download: %2%3")
            .arg(FreeBytes < 0 ? QStringLiteral("?") : HumanBytesQ(FreeBytes))
            .arg(HumanBytesQ(Sum)).arg(AllKnown ? "" : " (estimating…)"));
        SizeLabel->setStyleSheet((FreeBytes >= 0 && Sum > FreeBytes) ? "color:#c0726a; font-weight:bold;" : "color:#8f98a0;");
    };
    for (const auto & [Id, Cb] : GameChecks)   connect(Cb, &QCheckBox::toggled, &Dlg, [Recompute](bool){ Recompute(); });
    for (const auto & [Id, Cb] : OptChecks)    connect(Cb, &QCheckBox::toggled, &Dlg, [Recompute](bool){ Recompute(); });
    for (const auto & [Id, Cb] : RunnerChecks) connect(Cb, &QCheckBox::toggled, &Dlg, [Recompute](bool){ Recompute(); });
    Recompute();
    {   // gather each CID's size in the background (it's a remote `files stat` on the downloader) and update as they land
        std::map<std::string, bool> AllOn; for (const auto & [Id, Cb] : OptChecks) AllOn[Id] = true;
        std::set<std::string> All;
        for (const std::string & Lid : AllLaunch)
            for (const auto & C : PackageCatalog::NodeContentCids(Model.catalogIndex(), Lid, AllOn)) All.insert(C);
        for (const auto & [Rid, Cb] : RunnerChecks)
            for (const auto & C : PackageCatalog::NodeContentCids(Model.catalogIndex(), Rid)) All.insert(C);
        std::thread([this, ToQuery = std::vector<std::string>(All.begin(), All.end()), SizeCache, Alive, Recompute]{
            for (const std::string & C : ToQuery) {
                if (!Alive->load()) return;
                const long long S = IpfsWrapper::CidSize(C);
                QMetaObject::invokeMethod(this, [SizeCache, C, S, Alive, Recompute]{
                    if (!Alive->load()) return;                  // dialog closed — its widgets are gone, don't touch them
                    (*SizeCache)[C] = S; Recompute();
                }, Qt::QueuedConnection);
            }
        }).detach();
    }

    QHBoxLayout * BR = new QHBoxLayout(); DL->addLayout(BR); BR->addStretch();
    QPushButton * CancelBtn = new QPushButton("Cancel", &Dlg);
    QPushButton * GoBtn = new QPushButton("Download", &Dlg); GoBtn->setDefault(true);
    BR->addWidget(CancelBtn); BR->addWidget(GoBtn);
    connect(CancelBtn, &QPushButton::clicked, &Dlg, &QDialog::reject);
    connect(GoBtn,     &QPushButton::clicked, &Dlg, &QDialog::accept);
    const int Res = Dlg.exec();
    Alive->store(false);                                         // stop the async size updater from touching dialog widgets
    if (Res != QDialog::Accepted) return;

    std::vector<std::string> LaunchIds;                              // selected games (base editions + secondaries)
    for (const std::string & Lid : AllLaunch) if (GameChecks[Lid]->isChecked()) LaunchIds.push_back(Lid);
    std::vector<std::string> RunnerIds;                              // selected runners to download + install
    for (auto & [Rid, cb] : RunnerChecks) if (cb->isChecked()) RunnerIds.push_back(Rid);
    if (LaunchIds.empty() && RunnerIds.empty()) return;              // nothing picked
    std::map<std::string, bool> Toggles;                             // optional-content selection
    for (auto & [Id, cb] : OptChecks) Toggles[Id] = cb->isChecked();

    beginDownload(Key, LaunchIds, RunnerIds, Toggles);
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
            if (DownloadCidToUid.contains(Qc)) continue;
            Cids << Qc; DownloadCidToUid[Qc] = Key;
        }
    };
    for (const std::string & Lid : LaunchIds) AddCids(PackageCatalog::NodeContentCids(Model.catalogIndex(), Lid, Toggles));
    for (const std::string & Rid : RunnerIds) AddCids(PackageCatalog::NodeContentCids(Model.catalogIndex(), Rid));  // runner build CIDs
    DownloadUidCids[Key] = Cids;

    // Queued rows + per-CID progress/sizes are owned by IpfsModel now (fed by the download queue's callback + the
    // node's transfer events). This just needs the cid→package map above so recomputeProgress can average the card.

    // Mark this tile's card(s) "Downloading…" in place (cheap — no pool rebuild / no filesystem restat).
    emit downloadStarted(Key);
    emit transfersChanged();

    // Snapshot the node index for THIS worker: concurrent downloads must not read the shared CatalogIndex while a
    // completing download reassigns it (rebuildCatalog). (DEFPREFIX generation reads only the index, not the config.)
    NodeIndex Snapshot = Model.catalogIndex();
    nlohmann::ordered_json ConfigSnap = Model.config() ? *Model.config() : nlohmann::ordered_json::object();
    std::thread([this, LaunchIds, RunnerIds, Toggles, Key, Snapshot = std::move(Snapshot), ConfigSnap = std::move(ConfigSnap)]{
        std::string Err; bool Ok = true;
        // Pool EVERY fetch — all selected launchables' content layers + each launchable's RESOLVED runner chain build +
        // any extra runners the user ticked — into ONE concurrent batch, so a game downloads TOGETHER with the runtime
        // it needs (bounded by MaxConcurrentDownloads). Auto-pooling the resolved chain makes a downloaded game
        // immediately playable regardless of the runner checklist (dedup handles overlap with a ticked runner).
        std::vector<IpfsWrapper::FetchTarget> Targets;
        for (const std::string & Lid : LaunchIds)
        {
            if (!PackageCatalog::CollectContentTargets(Snapshot, Lid, Toggles, Targets, &Err)) { Ok = false; break; }
            PackageCatalog::CollectRunnerChainTargets(Snapshot, Lid, ConfigSnap, Targets, &Err);   // best-effort runtime
        }
        if (Ok)
            for (const std::string & Rid : RunnerIds)
                if (!RunnerInstall::CollectRunnerNodeTargets(Snapshot, Rid, Targets, &Err)) { Ok = false; break; }
        if (Ok && !IpfsWrapper::FetchTargetsConcurrent(Targets, &Err)) Ok = false;
        // Builds are now present locally → the runner is ready. Its prefix assembles from the build at launch
        // (node-declared layers), so there is NO post-fetch generation step — fetching the build IS the install.
        QMetaObject::invokeMethod(this, [this, Ok, Err, Key]{
            DownloadingUids.remove(Key);
            const bool Cancelled = CancellingUids.remove(Key);
            const QStringList DoneCids = DownloadUidCids.value(Key);
            for (const QString & c : DoneCids)
            {
                IpfsWrapper::ClearCancel(c.toStdString()); DownloadCidToUid.remove(c);
                // IpfsModel owns the transfer rows now; a still-queued CID clears itself via the queue's callback and
                // the post-completion refresh() (transfersChanged → IpfsModel::refreshNow).
            }
            DownloadUidCids.remove(Key);
            unpersistActive(Key);           // it ended normally (success/cancel/fail) → don't resume it next launch
            if (Cancelled)
            {
                // Voluntary abort → purge the partial's cached blocks off the GUI thread so a re-download is a real
                // download (and we don't hoard the aborted bytes). A crash/close leaves them — they speed the resume.
                std::thread([DoneCids]{ for (const QString & c : DoneCids) IpfsWrapper::DropCached(c.toStdString()); }).detach();
            }
            else if (!Ok) LogErr("DownloadManager::beginDownload", "Download failed: " + Err);   // shows as a "Failed" row
            emit downloadFinished(Key);   // the Catalog card(s) drop the "Downloading…" overlay BEFORE the rebuild reads state
            Model.rebuildCatalog();       // emits catalogChanged → Catalog rebuild + Library rebuild + IPFS refresh
            emit transfersChanged();
        }, Qt::QueuedConnection);
    }).detach();
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
