#include "downloadmanager.h"
#include "appmodel.h"          // AppModel — catalog/config + rebuildCatalog()
#include "ipfsmodel.h"         // IpfsModel — per-CID transfer progress + size (single source of truth)
#include "libraryview.h"       // LibraryGameCard + IpfsFetchReady()
#include "packagecatalog.h"
#include "manifestmodel.h"
#include "containerwrapper.h"
#include "ipfswrapper.h"
#include "commonutils.h"
#include "variantpicker.h"     // virtualized, searchable variant list — scales to any variant count
#include "asyncwork.h"         // AsyncWork::Run — closure walks off the GUI thread, guarded by the dialog's lifetime

#include <QDialog>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
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
#include <functional>
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

    // ── Pre-download picker (per PACKAGE) — designed for an ARBITRARY variant count (a 903-version Minecraft tile
    // as readily as a 3-edition game). Two rules keep it instant: (1) the variant list is a virtualized model/view
    // (VariantPicker) with search — no widget per row; (2) NOTHING on the open path walks a node closure on the GUI
    // thread — optional content and selection size are derived ASYNCHRONOUSLY from the CURRENT SELECTION only
    // (checked variants), debounced, against one immutable index snapshot. Default selection = the recommended
    // edition, not every variant. ──
    const std::vector<std::string> Variants = card->GroupNodeIds;     // the base game's launchable nodes (RECOMMENDED first)
    std::vector<std::pair<std::string, QString>> Secondaries;         // (recommended-edition launch id, title)
    for (LibraryGameCard * sc : card->Secondaries)
        if (sc && !sc->RepNodeId.empty()) Secondaries.push_back({ sc->RepNodeId, sc->GameTitle });

    // Snapshot handle for every async walk. Double indirection: *Snap is REPLACED (GUI thread only) after a received
    // package's closure completes mid-dialog, so sizes/optionals/endpoints re-derive against the full graph; each
    // derivation copies the inner pointer on the GUI thread before handing it to its worker (no cross-thread swap race).
    auto Snap  = std::make_shared<std::shared_ptr<const NodeIndex>>(
                     std::make_shared<const NodeIndex>(Model.catalogIndex()));
    auto Alive = std::make_shared<std::atomic<bool>>(true);                 // false once the dialog closes

    QDialog Dlg(DialogParent);
    Dlg.setWindowTitle("Download — " + card->GameTitle);
    Dlg.setMinimumWidth(460);
    QVBoxLayout * DL = new QVBoxLayout(&Dlg);
    DL->addWidget(new QLabel("Choose what to download:", &Dlg));

    // ── Content: the tile's download ENDPOINTS — the sinks of its variants' combined dependency DAG. Closures NEST
    // (PARENTS = dependency), so an endpoint subsumes everything below it: MC shows ONE row ("1.21.4 (903 versions)")
    // whose tick means the whole chain; AoE2 shows its edition tips sharing their base. Variant-level choice is
    // deliberately NOT offered here (that's a launch-time decision) — except through the Custom expander below,
    // where a power user can tick arbitrary variants (each implying its closure, e.g. "only up to 1.8.9").
    // Derived ASYNC (a union closure walk) — the dialog opens instantly with a placeholder. ──
    QGroupBox *   EpBox = new QGroupBox("Content", &Dlg);
    QVBoxLayout * EpL   = new QVBoxLayout(EpBox);
    QLabel *      EpPending = new QLabel("Deriving content…", EpBox);
    EpPending->setStyleSheet("color:#8f98a0;");
    EpL->addWidget(EpPending);
    DL->addWidget(EpBox);
    // One row may carry SEVERAL variant ids (the "Everything else" fold) — ticked = download all their closures.
    auto EpChecks = std::make_shared<std::vector<std::pair<QCheckBox*, std::vector<std::string>>>>();
    auto EpReady  = std::make_shared<bool>(false);                           // async fill landed

    // Custom selection: full variant list (collapsed by default), none ticked — additive to the endpoints.
    QToolButton * CustomBtn = new QToolButton(&Dlg);
    CustomBtn->setText("Custom selection…");
    CustomBtn->setCheckable(true);
    CustomBtn->setArrowType(Qt::RightArrow);
    CustomBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    CustomBtn->setAutoRaise(true);
    DL->addWidget(CustomBtn);
    VariantPicker * CustomPicker = new VariantPicker(VariantPicker::Mode::Checkable, &Dlg);
    {
        std::vector<VariantPicker::Entry> Es;
        for (const std::string & Lid : Variants)
        {
            const Node * N = (*Snap)->Find(Lid);
            std::string Lbl = (N && !N->Label.empty()) ? N->Label
                              : (N && N->Meta.is_object() ? N->Meta.value("TITLE", Lid) : Lid);
            Es.push_back({ Lid, QString::fromStdString(Lbl), N && N->Recommended });
        }
        CustomPicker->setEntries(Es);                                  // nothing ticked — endpoints carry the default
    }
    CustomPicker->setVisible(false);
    DL->addWidget(CustomPicker);
    connect(CustomBtn, &QToolButton::toggled, &Dlg, [CustomBtn, CustomPicker](bool On){
        CustomBtn->setArrowType(On ? Qt::DownArrow : Qt::RightArrow);
        CustomPicker->setVisible(On);
    });

    std::map<std::string, QCheckBox*> SecChecks;                      // the package's other games — few, plain checkboxes
    if (!Secondaries.empty())
    {
        QGroupBox * Box = new QGroupBox("Other games in this package", &Dlg);
        QVBoxLayout * BL = new QVBoxLayout(Box);
        for (const auto & [Lid, Title] : Secondaries)
        {
            QCheckBox * cb = new QCheckBox(Title, Box);
            cb->setChecked(false);
            SecChecks[Lid] = cb; BL->addWidget(cb);
        }
        DL->addWidget(Box);
    }

    // The download selection (ticked endpoints ∪ ticked custom variants ∪ checked secondaries) — the unit every
    // async derivation is scoped to. Until the async endpoint derivation lands, fall back to ALL variants: the
    // all-endpoints default IS the variants' union, so semantics don't shift when the rows appear.
    auto SelectedLaunchIds = [EpChecks, EpReady, CustomPicker, &SecChecks, Variants]() {
        std::vector<std::string> Sel;
        std::set<std::string> Seen;
        auto Add = [&](const std::string & Id){ if (Seen.insert(Id).second) Sel.push_back(Id); };
        if (*EpReady)
            for (const auto & [cb, Ids] : *EpChecks) { if (cb->isChecked()) for (const std::string & Id : Ids) Add(Id); }
        else
            for (const std::string & Id : Variants) Add(Id);
        for (const std::string & Id : CustomPicker->checkedIds()) Add(Id);
        for (const auto & [Lid, cb] : SecChecks) if (cb->isChecked()) Add(Lid);
        return Sel;
    };

    // ── Optional content: rebuilt async from the SELECTION's closures; per-id choices are sticky across rebuilds. ──
    QGroupBox *   OptBox = new QGroupBox("Optional content", &Dlg);
    QVBoxLayout * OptL   = new QVBoxLayout(OptBox);
    OptBox->setVisible(false);
    DL->addWidget(OptBox);
    auto OptStates = std::make_shared<std::map<std::string, bool>>(); // node id → chosen (seeded by DEFAULT on first sight)
    // ── Available Runners: the package's EMBEDDED runner(s), plus a compatible GLOBAL runner only when none is
    //    installed — each downloadable runner an optional checkbox (default on). Downloading one installs it. This is
    //    a RE-DERIVABLE block (like DeriveEndpoints): a received game opens with its runner (proton) not yet in the
    //    index, so the section is empty at first; the closure-completion swap below re-derives it and the checkbox
    //    appears. Reads *Snap (swappable), not the live index. ──
    // Debounce created early: the re-derivable runner section connects each checkbox's toggle to it.
    QTimer * Debounce = new QTimer(&Dlg);
    Debounce->setSingleShot(true);
    Debounce->setInterval(200);

    std::map<std::string, QCheckBox*> RunnerChecks;                   // runner node id → checkbox (rebuilt on re-derive)
    QGroupBox *   RunBox = new QGroupBox("Available Runners", &Dlg);
    QVBoxLayout * RunL   = new QVBoxLayout(RunBox);
    RunBox->setVisible(false);
    DL->addWidget(RunBox);
    auto DeriveRunners = std::make_shared<std::function<void()>>();
    *DeriveRunners = [&Dlg, &RunnerChecks, Snap, Variants, RunBox, RunL, Debounce]() {
        const std::shared_ptr<const NodeIndex> S = *Snap;
        QLayoutItem * It; while ((It = RunL->takeAt(0)) != nullptr) { if (QWidget * W = It->widget()) W->deleteLater(); delete It; }
        RunnerChecks.clear();
        const Node * BaseGame = Variants.empty() ? nullptr : S->Find(Variants.front());
        std::vector<const Node*> EmbeddedR, GlobalR; bool AnyCompatInstalled = false;
        if (BaseGame)
            for (const Node * R : PackageCatalog::CompatibleRunners(*S, *BaseGame))
            {
                const std::string rid = R->NodeId;
                const bool Embedded   = (R->BundleDir == BaseGame->BundleDir);
                const bool Standalone = !PackageCatalog::IsEmbeddedRunner(*S, rid);
                if (!Embedded && !Standalone) continue;
                if (PackageCatalog::RunnerInstalled(*S, rid)) { AnyCompatInstalled = true; continue; }
                // A RECEIVED runner has no enumerable build YET (closure incomplete) but is downloadable — only a
                // complete-closure runner with no content is a PATH runner.
                if (PackageCatalog::NodeContentCids(*S, rid).empty() && !PackageCatalog::NodeClosureIncomplete(*S, rid)) continue;
                (Embedded ? EmbeddedR : GlobalR).push_back(R);
            }
        const bool ShowGlobal = !AnyCompatInstalled && !GlobalR.empty();
        auto addRunner = [&](const Node * R){
            const std::string rid = R->NodeId;
            const int Items = (int)PackageCatalog::NodeContentCids(*S, rid).size();
            const QString Suffix = Items > 0 ? QString("   (%1 file%2)").arg(Items).arg(Items == 1 ? "" : "s")
                                             : QStringLiteral("   (remote)");   // received: enumerates on install
            QCheckBox * cb = new QCheckBox(QString::fromStdString(R->Label.empty() ? rid : R->Label) + Suffix, RunBox);
            cb->setChecked(true);
            QObject::connect(cb, &QCheckBox::toggled, RunBox, [Debounce](bool){ Debounce->start(); });   // toggle → re-derive size
            RunnerChecks[rid] = cb; RunL->addWidget(cb);
        };
        if (!EmbeddedR.empty()) { QLabel * h = new QLabel("Embedded", RunBox); h->setStyleSheet("color:#8f98a0; font-weight:bold;"); RunL->addWidget(h); for (const Node * R : EmbeddedR) addRunner(R); }
        if (ShowGlobal)         { QLabel * h = new QLabel("Global", RunBox);   h->setStyleSheet("color:#8f98a0; font-weight:bold;"); RunL->addWidget(h); for (const Node * R : GlobalR)   addRunner(R); }
        RunBox->setVisible(!EmbeddedR.empty() || ShowGlobal);
    };

    // ── Disk-space display: free space at the library + the (async-derived) size of the current selection ──
    QLabel * SizeLabel = new QLabel("Free space: …", &Dlg);
    SizeLabel->setStyleSheet("color:#8f98a0;");
    DL->addWidget(SizeLabel);
    std::error_code DiskEc;
    const auto DiskSp = std::filesystem::space(PackageCatalog::LibraryRootDir(*Model.config()), DiskEc);
    const long long FreeBytes = DiskEc ? -1 : (long long)DiskSp.available;

    auto SizeCache = std::make_shared<std::map<std::string, long long>>();   // CID → bytes (filled async)
    auto Queried   = std::make_shared<std::set<std::string>>();              // CIDs already handed to the size prober

    // A toggle anywhere (variant / secondary / optional / runner) restarts Debounce; on fire, the optionals section
    // and the size line are re-derived for the CURRENT selection. Both derivations walk closures — so both run off
    // the GUI thread via AsyncWork against Snap, and only ever for what is actually checked. (Debounce is declared
    // above so the re-derivable runner section can connect to it.)
    auto RecomputeSize    = std::make_shared<std::function<void()>>();
    auto RebuildOptionals = std::make_shared<std::function<void()>>();

    *RecomputeSize = [this, &Dlg, &RunnerChecks, Snap, SelectedLaunchIds, OptStates, SizeCache, Queried,
                      SizeLabel, FreeBytes, Alive, Debounce]() {
        const std::vector<std::string> SelL = SelectedLaunchIds();
        std::vector<std::string> SelR;
        for (const auto & [Rid, cb] : RunnerChecks) if (cb->isChecked()) SelR.push_back(Rid);
        auto Tg  = std::make_shared<std::map<std::string, bool>>(*OptStates);
        auto Out = std::make_shared<std::set<std::string>>();
        auto Stamped = std::make_shared<std::map<std::string, long long>>();   // cid → SOURCE.SIZE (instant, offline)
        const std::shared_ptr<const NodeIndex> S = *Snap;   // GUI-thread copy — the worker never touches the handle
        AsyncWork::Run(&Dlg,
            [S, SelL, SelR, Tg, Out, Stamped]{
                for (const std::string & Lid : SelL)
                {
                    for (const auto & C : PackageCatalog::NodeContentCids(*S, Lid, *Tg)) Out->insert(C);
                    for (const auto & [C, Sz] : PackageCatalog::NodeContentSizes(*S, Lid, *Tg)) (*Stamped)[C] = Sz;
                }
                for (const std::string & Rid : SelR)
                {
                    for (const auto & C : PackageCatalog::NodeContentCids(*S, Rid)) Out->insert(C);
                    for (const auto & [C, Sz] : PackageCatalog::NodeContentSizes(*S, Rid)) (*Stamped)[C] = Sz;
                }
            },
            [this, Out, Stamped, SizeCache, Queried, SizeLabel, FreeBytes, Alive, Debounce]{
                // Stamped sizes are authoritative and free — seed the cache with them so the network prober only
                // ever runs for the rare UNSTAMPED legacy CID (it used to run for everything: ~35s each, serial —
                // the "no size for minutes" bug).
                for (const auto & [C, Sz] : *Stamped) (*SizeCache)[C] = Sz;
                long long Sum = 0; bool AllKnown = true;
                std::vector<std::string> Unknown;
                for (const std::string & C : *Out)
                {
                    auto It = SizeCache->find(C);
                    if (It != SizeCache->end() && It->second >= 0) Sum += It->second;
                    else { AllKnown = false; if (Queried->insert(C).second) Unknown.push_back(C); }
                }
                SizeLabel->setText(QString("Free space: %1      Download: %2%3")
                    .arg(FreeBytes < 0 ? QStringLiteral("?") : HumanBytesQ(FreeBytes))
                    .arg(HumanBytesQ(Sum)).arg(AllKnown ? "" : " (estimating…)"));
                SizeLabel->setStyleSheet((FreeBytes >= 0 && Sum > FreeBytes) ? "color:#c0726a; font-weight:bold;"
                                                                             : "color:#8f98a0;");
                if (Unknown.empty()) return;
                // Probe the new CIDs' sizes in the background; each result lands in the cache and nudges the
                // debounce, so the label refreshes shortly after data arrives. Alive-guarded — the dialog may close.
                std::thread([this, Unknown, SizeCache, Alive, Debounce]{
                    for (const std::string & C : Unknown)
                    {
                        if (!Alive->load()) return;
                        const long long S = IpfsWrapper::CidSize(C);
                        QMetaObject::invokeMethod(this, [SizeCache, C, S, Alive, Debounce]{
                            if (!Alive->load()) return;          // dialog closed — its widgets are gone
                            (*SizeCache)[C] = S;
                            Debounce->start();
                        }, Qt::QueuedConnection);
                    }
                }).detach();
            });
    };

    *RebuildOptionals = [&Dlg, Snap, SelectedLaunchIds, OptStates, OptBox, OptL, Debounce]() {
        struct OptEntry { std::string Id; std::string Label; bool Default; };
        const std::vector<std::string> SelL = SelectedLaunchIds();
        auto Found = std::make_shared<std::vector<OptEntry>>();
        const std::shared_ptr<const NodeIndex> S = *Snap;   // GUI-thread copy
        AsyncWork::Run(&Dlg,
            [S, SelL, Found]{
                //The optional pieces of a download are the GRAFTS offered on the selected variants — the author's
                //pre-ticked ones (a soundtrack, a fix) default on; the hydrate fetches whatever is ticked here.
                std::set<std::string> Seen;
                for (const std::string & Lid : SelL)
                    for (const ManifestModel::GraftOffer & O : ManifestModel::OfferedGrafts(*S, Lid, {},
                             [](const Node & N) { return !N.Received && !N.BundleDir.empty(); }))
                        if (Seen.insert(O.Graft->Key()).second)
                            Found->push_back({ O.Graft->Key(), O.Graft->NodeId.empty() ? O.Graft->Key().substr(0, 12) : O.Graft->NodeId, O.Selected });
            },
            [Found, OptStates, OptBox, OptL, Debounce]{
                QLayoutItem * It;
                while ((It = OptL->takeAt(0)) != nullptr) { if (QWidget * W = It->widget()) W->deleteLater(); delete It; }
                for (const OptEntry & E : *Found)
                {
                    if (!OptStates->count(E.Id)) (*OptStates)[E.Id] = E.Default;   // first sight → seed with DEFAULT
                    QCheckBox * cb = new QCheckBox(QString::fromStdString(E.Label), OptBox);
                    cb->setChecked((*OptStates)[E.Id]);
                    const std::string Id = E.Id;
                    QObject::connect(cb, &QCheckBox::toggled, OptBox, [Id, OptStates, Debounce](bool On){
                        (*OptStates)[Id] = On;
                        Debounce->start();               // an optional changes the selection's CID set → re-derive size
                    });
                    OptL->addWidget(cb);
                }
                OptBox->setVisible(!Found->empty());
            });
    };

    connect(Debounce, &QTimer::timeout, &Dlg, [RebuildOptionals, RecomputeSize]{ (*RebuildOptionals)(); (*RecomputeSize)(); });
    connect(CustomPicker, &VariantPicker::checkedChanged, &Dlg, [Debounce]{ Debounce->start(); });
    for (const auto & [Lid, cb] : SecChecks)    connect(cb, &QCheckBox::toggled, &Dlg, [Debounce](bool){ Debounce->start(); });
    (*RebuildOptionals)(); (*RecomputeSize)();   // initial async fill — the dialog itself shows instantly

    // Derive the endpoints off-thread and materialize their checkboxes (all ticked = the default "everything").
    // Re-runnable: a received package's closure completing mid-dialog re-derives against the full graph.
    auto DeriveEndpoints = std::make_shared<std::function<void()>>();
    *DeriveEndpoints = [&Dlg, Snap, Variants, EpChecks, EpReady, EpBox, EpL, Debounce]{
        auto Found = std::make_shared<std::vector<ManifestModel::EndpointInfo>>();
        const std::shared_ptr<const NodeIndex> S = *Snap;   // GUI-thread copy
        AsyncWork::Run(&Dlg,
            [S, Variants, Found]{ *Found = ManifestModel::TileEndpoints(*S, Variants); },
            [Found, EpChecks, EpReady, EpBox, EpL, Debounce]{
                // Keep TileEndpoints' order: greedy picks best-first, the "Everything else" fold last. A re-derive
                // (closure completed) replaces the rows wholesale — all ticked again, the "everything" default.
                QLayoutItem * It;
                while ((It = EpL->takeAt(0)) != nullptr) { if (QWidget * W = It->widget()) W->deleteLater(); delete It; }
                EpChecks->clear();
                for (const ManifestModel::EndpointInfo & E : *Found)
                {
                    QString Lbl = QString::fromStdString(E.Label);
                    if (E.LaunchableCount > 1)  Lbl += QString("   (%1 versions)").arg(E.LaunchableCount);
                    else if (E.Ids.size() > 1)  Lbl += QString("   (%1 items)").arg(E.Ids.size());
                    QCheckBox * cb = new QCheckBox(Lbl, EpBox);
                    cb->setChecked(true);
                    EpChecks->push_back({ cb, E.Ids });
                    EpL->addWidget(cb);
                    QObject::connect(cb, &QCheckBox::toggled, EpBox, [Debounce](bool){ Debounce->start(); });
                }
                EpBox->setVisible(!Found->empty());
                *EpReady = true;
                Debounce->start();
            });
    };
    (*DeriveEndpoints)();
    (*DeriveRunners)();

    // A RECEIVED package opens with an INCOMPLETE closure (only its exec + tile were shared) — nothing below it to
    // size or enumerate. Complete it NOW, through the same rolling queue (CompleteClosure — the node blocks are
    // KBs), then swap the snapshot and re-derive everything: real sizes, real optionals, real endpoints, and the
    // Download button resolves real content targets. No wire change, no size stamping — the graph itself arrives.
    {
        bool AnyIncomplete = false;
        const nlohmann::ordered_json CfgForGate = Model.config() ? *Model.config() : nlohmann::ordered_json::object();
        for (const std::string & V : Variants)
        {
            if (PackageCatalog::NodeClosureIncomplete(**Snap, V)) { AnyIncomplete = true; break; }
            for (const std::string & Rid : PackageCatalog::RunnerChainIds(**Snap, V, CfgForGate))
                if (PackageCatalog::NodeClosureIncomplete(**Snap, Rid)) { AnyIncomplete = true; break; }
            if (AnyIncomplete) break;
        }
        if (AnyIncomplete)
        {
            const std::shared_ptr<const NodeIndex> S = *Snap;
            nlohmann::ordered_json ConfigSnap = Model.config() ? *Model.config() : nlohmann::ordered_json::object();
            std::thread([this, S, Variants, Snap, Alive, DeriveEndpoints, DeriveRunners, Debounce, ConfigSnap = std::move(ConfigSnap)]{
                for (const std::string & V : Variants)
                {
                    std::string CErr;
                    if (PackageCatalog::NodeClosureIncomplete(*S, V) && !PackageCatalog::CompleteClosure(*S, V, &CErr))
                        LogWarn("DownloadManager::startDownload", "closure completion for '" + V + "': " + CErr);
                    // The RUNNER chain (proton/wine) is itself received — complete it too, so CompatibleRunners can
                    // enumerate it and the runner checkbox appears (else it auto-pools invisibly).
                    for (const std::string & Rid : PackageCatalog::RunnerChainIds(*S, V, ConfigSnap))
                        if (PackageCatalog::NodeClosureIncomplete(*S, Rid))
                        {
                            std::string RErr;
                            if (!PackageCatalog::CompleteClosure(*S, Rid, &RErr))
                                LogWarn("DownloadManager::startDownload", "runner closure '" + Rid + "': " + RErr);
                        }
                }
                QMetaObject::invokeMethod(this, [this, Snap, Alive, DeriveEndpoints, DeriveRunners, Debounce]{
                    Model.rebuildCatalog();                       // the landed node files are ordinary tree packages
                    if (!Alive->load()) return;                   // dialog closed meanwhile — catalog is fresh anyway
                    *Snap = std::make_shared<const NodeIndex>(Model.catalogIndex());
                    (*DeriveEndpoints)();
                    (*DeriveRunners)();                           // proton's node is present now → the choice appears
                    Debounce->start();                            // sizes + optionals re-derive on the full graph
                }, Qt::QueuedConnection);
            }).detach();
        }
    }

    QHBoxLayout * BR = new QHBoxLayout(); DL->addLayout(BR); BR->addStretch();
    QPushButton * CancelBtn = new QPushButton("Cancel", &Dlg);
    QPushButton * GoBtn = new QPushButton("Download", &Dlg); GoBtn->setDefault(true);
    BR->addWidget(CancelBtn); BR->addWidget(GoBtn);
    connect(CancelBtn, &QPushButton::clicked, &Dlg, &QDialog::reject);
    connect(GoBtn,     &QPushButton::clicked, &Dlg, &QDialog::accept);
    const int Res = Dlg.exec();
    Alive->store(false);                                         // stop the async workers from touching dialog widgets
    if (Res != QDialog::Accepted) return;

    const std::vector<std::string> LaunchIds = SelectedLaunchIds();  // checked variants + checked secondaries
    std::vector<std::string> RunnerIds;                              // selected runners to download + install
    for (auto & [Rid, cb] : RunnerChecks) if (cb->isChecked()) RunnerIds.push_back(Rid);
    if (LaunchIds.empty() && RunnerIds.empty()) return;              // nothing picked

    beginDownload(Key, LaunchIds, RunnerIds, *OptStates);            // OptStates = the sticky optional-content choices
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
        // A RECEIVED package's closure is incomplete (only its exec + tile were shared) — complete it FIRST, through
        // the same rolling queue (CompleteClosure: each missing node block is a plain FetchTarget into the package
        // dir), then resolve targets against a FRESH index: the snapshot predates the just-landed blocks.
        NodeIndex Idx = Snapshot;
        {
            bool Completed = false;
            std::vector<std::string> All = LaunchIds;
            All.insert(All.end(), RunnerIds.begin(), RunnerIds.end());
            for (const std::string & Lid : All)
                if (PackageCatalog::NodeClosureIncomplete(Idx, Lid))
                {
                    if (!PackageCatalog::CompleteClosure(Idx, Lid, &Err)) { Ok = false; break; }
                    Completed = true;
                }
            if (Ok && Completed) { Idx = PackageCatalog::BuildCatalogIndex(ConfigSnap); Completed = false; }
            // The AUTO-POOLED runtime too: a received game's resolved runner chain (proton/wine) is itself received
            // — exec + tile only — and CollectRunnerChainTargets can enumerate NOTHING from a headless runner graph
            // (the game downloaded but its runtime silently didn't). Resolve the chain on the fresh index, complete
            // each incomplete runner's closure the same way, and re-index once more.
            if (Ok)
                for (const std::string & Lid : LaunchIds)
                    for (const std::string & Rid : PackageCatalog::RunnerChainIds(Idx, Lid, ConfigSnap))
                        if (PackageCatalog::NodeClosureIncomplete(Idx, Rid))
                        {
                            std::string CErr;
                            if (!PackageCatalog::CompleteClosure(Idx, Rid, &CErr))
                                LogWarn("DownloadManager::beginDownload", "runner closure '" + Rid + "': " + CErr);
                            else Completed = true;
                        }
            if (Ok && Completed) Idx = PackageCatalog::BuildCatalogIndex(ConfigSnap);
        }
        // Pool EVERY fetch — all selected launchables' content layers + each launchable's RESOLVED runner chain build +
        // any extra runners the user ticked — into ONE concurrent batch, so a game downloads TOGETHER with the runtime
        // it needs (bounded by MaxConcurrentDownloads). Auto-pooling the resolved chain makes a downloaded game
        // immediately playable regardless of the runner checklist (dedup handles overlap with a ticked runner).
        std::vector<IpfsWrapper::FetchTarget> Targets;
        if (Ok) for (const std::string & Lid : LaunchIds)
        {
            if (!PackageCatalog::CollectContentTargets(Idx, Lid, Toggles, Targets, &Err)) { Ok = false; break; }
            //Best-effort by design — a game whose runtime cannot be resolved should still download. But silently
            //best-effort meant the download finished green and the game then would not launch, with nothing
            //anywhere connecting the two. Still non-fatal; now at least it is on the record.
            if (!PackageCatalog::CollectRunnerChainTargets(Idx, Lid, ConfigSnap, Targets, &Err))
                LogWarn("DownloadManager::beginDownload", "could not resolve the runner chain for '" + Lid
                            + "' — its runtime is NOT in this batch, so the download will complete but the game "
                              "will not be launchable" + (Err.empty() ? "" : " (" + Err + ")"));
        }
        if (Ok)
            for (const std::string & Rid : RunnerIds)
                if (!RunnerInstall::CollectRunnerNodeTargets(Idx, Rid, Targets, &Err)) { Ok = false; break; }
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
