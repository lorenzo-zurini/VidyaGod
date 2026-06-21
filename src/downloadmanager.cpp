#include "downloadmanager.h"
#include "mainwindow.h"        // full MainWindow (DownloadManager is its friend → reaches the IPFS tab + cards)
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

// Compact human-readable byte size ("—" for unknown/negative) — local copy of the MainWindow helper.
static QString HumanBytes(long long N)
{
    if (N < 0) return QStringLiteral("—");
    double B = double(N); const char * U[] = { "B", "KB", "MB", "GB", "TB" }; int I = 0;
    while (B >= 1024.0 && I < 4) { B /= 1024.0; ++I; }
    return QString::number(B, 'f', I == 0 ? 0 : 1) + " " + U[I];
}

DownloadManager::DownloadManager(MainWindow &Owner)
    : QObject(&Owner), Mw(Owner) {}

void DownloadManager::requestCancel(LibraryGameCard *card)
{
    if (!card) return;
    const QString Key = card->GroupKey;
    if (Key.isEmpty() || !DownloadingUids.contains(Key)) return;
    if (QMessageBox::question(&Mw, "Cancel download?",
            "Stop downloading “" + card->GameTitle + "”?") != QMessageBox::Yes) return;
    CancellingUids.insert(Key);                                   // suppress the failure dialog on abort
    for (const QString & c : DownloadUidCids.value(Key)) IpfsWrapper::RequestCancel(c.toStdString());
}

void DownloadManager::applyProgress(const QString &cid, double pct)
{
    auto it = DownloadCidToUid.constFind(cid);
    if (it == DownloadCidToUid.constEnd()) return;
    const QString Key = it.value();
    if (pct >= 0) DownloadCidPct[cid] = pct;
    const QStringList & Cids = DownloadUidCids[Key];
    double Sum = 0; for (const QString & c : Cids) Sum += DownloadCidPct.value(c, 0.0);
    const double Avg = Cids.isEmpty() ? -1.0 : Sum / Cids.size();
    bool Any = false;
    for (LibraryGameCard * card : *Mw.AvailableGameCards)
        if (card && card->Downloading && card->GroupKey == Key)
        { card->DownloadPercent = Avg; Any = true; }
    if (Any && Mw.AvailableView) Mw.AvailableView->refreshVisuals();
}

double DownloadManager::busyPercent(const QString &groupKey) const
{
    const QStringList & cs = DownloadUidCids.value(groupKey);
    if (cs.isEmpty()) return -1.0;
    double s = 0; for (const QString & cc : cs) s += DownloadCidPct.value(cc, 0.0);
    return s / cs.size();
}

void DownloadManager::startDownload(LibraryGameCard *card)
{
    if (!card) return;
    if (!IpfsFetchReady(&Mw)) return;
    const QString Key = card->GroupKey;
    if (Key.isEmpty() || DownloadingUids.contains(Key)) return;       // already in flight

    // ── Pre-download picker (per PACKAGE): the base game's editions + the package's other games (the Catalog
    // overlay "secondaries") as optionals + optional content. Each secondary contributes its recommended edition. ──
    const std::vector<std::string> Editions = card->GroupNodeIds;     // the base game's launchable nodes
    std::vector<std::pair<std::string, QString>> Secondaries;         // (recommended-edition launch id, title)
    for (LibraryGameCard * sc : card->Secondaries)
        if (sc && !sc->RepNodeId.empty()) Secondaries.push_back({ sc->RepNodeId, sc->GameTitle });

    std::vector<std::string> AllLaunch = Editions;                    // every selectable launch node (base + secondaries)
    for (const auto & [Lid, T] : Secondaries) AllLaunch.push_back(Lid);

    std::vector<const Node*> Opts; std::set<std::string> OptSeen;     // union of OPTIONAL content across all of them
    for (const std::string & Lid : AllLaunch)
        for (const Node * O : ManifestModel::OptionalNodes(Mw.CatalogIndex, Lid))
            if (OptSeen.insert(O->NodeId).second) Opts.push_back(O);

    QDialog Dlg(&Mw);
    Dlg.setWindowTitle("Download — " + card->GameTitle);
    Dlg.setMinimumWidth(420);
    QVBoxLayout * DL = new QVBoxLayout(&Dlg);
    DL->addWidget(new QLabel("Choose what to download:", &Dlg));

    std::map<std::string, QCheckBox*> GameChecks;                     // launch id → checkbox (base editions + secondaries)
    {
        QGroupBox * Box = new QGroupBox(Editions.size() > 1 ? "Editions" : "Game", &Dlg);
        QVBoxLayout * BL = new QVBoxLayout(Box);
        for (const std::string & Lid : Editions)
        {
            const Node * N = Mw.CatalogIndex.Find(Lid);
            std::string Lbl = (N && !N->Label.empty()) ? N->Label
                              : (N && N->Meta.is_object() ? N->Meta.value("TITLE", Lid) : Lid);
            const int Items = (int)PackageCatalog::NodeContentCids(Mw.CatalogIndex, Lid).size();
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
            const int Items = (int)PackageCatalog::NodeContentCids(Mw.CatalogIndex, Lid).size();
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
        const Node * BaseGame = Editions.empty() ? nullptr : Mw.CatalogIndex.Find(Editions.front());
        if (BaseGame)
        {
            std::vector<const Node*> EmbeddedR, GlobalR; bool AnyCompatInstalled = false;
            for (const Node * R : PackageCatalog::CompatibleRunners(Mw.CatalogIndex, *BaseGame))
            {
                const std::string rid = R->NodeId;
                const bool Embedded   = (R->BundleDir == BaseGame->BundleDir);                    // embedded in THIS package
                const bool Standalone = !PackageCatalog::IsEmbeddedRunner(Mw.CatalogIndex, rid);  // not bundled in any game
                if (!Embedded && !Standalone) continue;                                           // another game's embedded runner
                if (PackageCatalog::RunnerInstalled(Mw.CatalogIndex, rid)) { AnyCompatInstalled = true; continue; }
                if (PackageCatalog::NodeContentCids(Mw.CatalogIndex, rid).empty()) continue;      // PATH runner — not downloadable
                (Embedded ? EmbeddedR : GlobalR).push_back(R);
            }
            const bool ShowGlobal = !AnyCompatInstalled && !GlobalR.empty();
            if (!EmbeddedR.empty() || ShowGlobal)
            {
                QGroupBox * Box = new QGroupBox("Available Runners", &Dlg);
                QVBoxLayout * BL = new QVBoxLayout(Box);
                auto addRunner = [&](const Node * R){
                    const std::string rid = R->NodeId;
                    const int Items = (int)PackageCatalog::NodeContentCids(Mw.CatalogIndex, rid).size();
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
    const auto DiskSp = std::filesystem::space(PackageCatalog::LibraryRootDir(*Mw.GlobalConfigJSON), DiskEc);
    const long long FreeBytes = DiskEc ? -1 : (long long)DiskSp.available;

    auto SizeCache = std::make_shared<std::map<std::string, long long>>();   // CID → bytes (filled async)
    auto Alive     = std::make_shared<std::atomic<bool>>(true);             // false once the dialog closes
    auto Recompute = [this, GameChecks, OptChecks, RunnerChecks, AllLaunch, SizeCache, SizeLabel, FreeBytes]() {
        std::map<std::string, bool> Tg; for (const auto & [Id, Cb] : OptChecks) Tg[Id] = Cb->isChecked();
        std::set<std::string> Sel;
        for (const std::string & Lid : AllLaunch)
            if (GameChecks.at(Lid)->isChecked())
                for (const auto & C : PackageCatalog::NodeContentCids(Mw.CatalogIndex, Lid, Tg)) Sel.insert(C);
        for (const auto & [Rid, Cb] : RunnerChecks)
            if (Cb->isChecked())
                for (const auto & C : PackageCatalog::NodeContentCids(Mw.CatalogIndex, Rid)) Sel.insert(C);
        long long Sum = 0; bool AllKnown = true;
        for (const std::string & C : Sel) { auto It = SizeCache->find(C); if (It != SizeCache->end() && It->second >= 0) Sum += It->second; else AllKnown = false; }
        SizeLabel->setText(QString("Free space: %1      Download: %2%3")
            .arg(FreeBytes < 0 ? QStringLiteral("?") : HumanBytes(FreeBytes))
            .arg(HumanBytes(Sum)).arg(AllKnown ? "" : " (estimating…)"));
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
            for (const auto & C : PackageCatalog::NodeContentCids(Mw.CatalogIndex, Lid, AllOn)) All.insert(C);
        for (const auto & [Rid, Cb] : RunnerChecks)
            for (const auto & C : PackageCatalog::NodeContentCids(Mw.CatalogIndex, Rid)) All.insert(C);
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

    DownloadingUids.insert(Key);
    QStringList Cids;
    auto AddCids = [&](const std::vector<std::string> & CidList){
        for (const auto & C : CidList)
        {
            const QString Qc = QString::fromStdString(C);
            if (DownloadCidToUid.contains(Qc)) continue;
            Cids << Qc; DownloadCidToUid[Qc] = Key; DownloadCidPct[Qc] = 0.0;
        }
    };
    for (const std::string & Lid : LaunchIds) AddCids(PackageCatalog::NodeContentCids(Mw.CatalogIndex, Lid, Toggles));
    for (const std::string & Rid : RunnerIds) AddCids(PackageCatalog::NodeContentCids(Mw.CatalogIndex, Rid));  // runner build CIDs
    DownloadUidCids[Key] = Cids;

    // Pre-show every CID as a "Queued" transfer; the worker fetches them, and transferStarted flips each to
    // "Fetching…" as it begins. Leftover queued rows are cleared when the worker finishes (below).
    if (Mw.IpfsTransfers)
        for (const QString & c : Cids) { Mw.EnsureTransferRow(c, QStringLiteral("Queued")); Mw.IpfsTransferQueued.insert(c); }

    // Mark this tile's card(s) "Downloading…" in place (cheap — no pool rebuild / no filesystem restat).
    for (LibraryGameCard * c : *Mw.AvailableGameCards)
        if (c && c->GroupKey == Key) { c->Downloading = true; c->DownloadPercent = 0.0; }
    Mw.AvailableView->refreshVisuals();
    Mw.RefreshIpfsTab();

    // Snapshot the node index + config for THIS worker: concurrent downloads must not read the shared
    // CatalogIndex while a completing download reassigns it (RebuildDynamicUI), and the worker must never touch the
    // GUI-owned live GlobalConfigJSON.
    NodeIndex Snapshot = Mw.CatalogIndex;
    auto CfgSnap = std::make_shared<nlohmann::ordered_json>(*Mw.GlobalConfigJSON);
    std::thread([this, LaunchIds, RunnerIds, Toggles, Key, Snapshot = std::move(Snapshot), CfgSnap]{
        std::string Err; bool Ok = true;
        for (const std::string & Lid : LaunchIds)
            if (!PackageCatalog::HydrateNode(Snapshot, Lid, Toggles, &Err)) { Ok = false; break; }
        // Install the selected runners (hydrate the build + generate the DEFPREFIX for proton/wine) — same call the
        // Settings "Import" button uses. Reads the config snapshot, never the live GlobalConfigJSON.
        if (Ok)
            for (const std::string & Rid : RunnerIds)
                if (!ContainerWrapper::ImportRunnerNode(*CfgSnap, Snapshot, Rid, &Err)) { Ok = false; break; }
        QMetaObject::invokeMethod(this, [this, Ok, Err, Key]{
            DownloadingUids.remove(Key);
            const bool Cancelled = CancellingUids.remove(Key);
            for (const QString & c : DownloadUidCids.value(Key))
            {
                IpfsWrapper::ClearCancel(c.toStdString()); DownloadCidToUid.remove(c); DownloadCidPct.remove(c);
                // Drop any row still "Queued" (never started — the CIDs after a failed/cancelled one).
                if (Mw.IpfsTransferQueued.remove(c))
                    if (QTableWidgetItem * p = Mw.IpfsTransferProgress.value(c, nullptr))
                    { const int r = Mw.IpfsTransfers ? Mw.IpfsTransfers->row(p) : -1; if (r >= 0) Mw.IpfsTransfers->removeRow(r);
                      Mw.IpfsTransferProgress.remove(c); Mw.IpfsTransferSpeed.remove(c); }
            }
            DownloadUidCids.remove(Key);
            if (Ok) Mw.RebuildDynamicUI();
            else if (!Cancelled) LogErr("DownloadManager::startDownload", "Download failed: " + Err);   // no dialog — the
                                                                          // failure shows as a "Failed" row in the IPFS tab
            Mw.RebuildAvailableTab(); Mw.RefreshIpfsTab();
        }, Qt::QueuedConnection);
    }).detach();
}
