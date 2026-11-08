#include "downloaddialog.h"
#include "theme.h"   // the named colours
#include "appmodel.h"
#include "downloadmanager.h"
#include "asyncwork.h"         // AsyncWork::Run — closure walks off the GUI thread, guarded by the dialog's lifetime
#include "guiformat.h"         // HumanBytesQ — the one shared GUI byte formatter
#include "installed.h"
#include "ipfswrapper.h"
#include "libraryview.h"       // LibraryGameCard, PlainLabel
#include "manifestmodel.h"
#include "packagecatalog.h"
#include "variantpicker.h"     // virtualized, searchable variant list — scales to any variant count

#include <QCheckBox>
#include <QCoreApplication>
#include <QDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <thread>

std::optional<DownloadSelection> AskDownload(AppModel &Model, const LibraryGameCard &Card, QWidget *Parent)
{
    const QString Key = Card.GameKey;
    // ── Pre-download picker (per PACKAGE) — designed for an ARBITRARY variant count (a 903-version Minecraft tile
    // as readily as a 3-edition game). Two rules keep it instant: (1) the variant list is a virtualized model/view
    // (VariantPicker) with search — no widget per row; (2) NOTHING on the open path walks a node closure on the GUI
    // thread — optional content and selection size are derived ASYNCHRONOUSLY from the CURRENT SELECTION only
    // (checked variants), debounced, against one immutable index snapshot. Default selection = the recommended
    // edition, not every variant. ──
    const std::vector<std::string> Variants = Card.GroupNodeIds;     // the base game's launchable nodes (RECOMMENDED first)
    std::vector<std::pair<std::string, QString>> Secondaries;         // (recommended-edition launch id, title)
    for (LibraryGameCard * sc : Card.Secondaries)
        if (sc && !sc->RepNodeId.empty()) Secondaries.push_back({ sc->RepNodeId, sc->GameTitle });

    // Snapshot handle for every async walk. Double indirection: *Snap is REPLACED (GUI thread only) after a received
    // package's closure completes mid-dialog, so sizes/optionals/endpoints re-derive against the full graph; each
    // derivation copies the inner pointer on the GUI thread before handing it to its worker (no cross-thread swap race).
    auto Snap  = std::make_shared<std::shared_ptr<const NodeIndex>>(Model.catalogSnapshot());
    auto Alive = std::make_shared<std::atomic<bool>>(true);                 // false once the dialog closes

    QDialog Dlg(Parent);
    Dlg.setWindowTitle("Download — " + Card.GameTitle);
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
    EpPending->setStyleSheet("color:" + Theme::Dim + ";");
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
            std::string Lbl = (N && !N->Variant.empty()) ? N->Variant
                              : (N && N->Meta.is_object() ? N->Meta.value("TITLE", Lid) : Lid);
            const bool Rec = N && std::find(N->Recommended.begin(), N->Recommended.end(), N->Uid) != N->Recommended.end();
            Es.push_back({ Lid, QString::fromStdString(Lbl), Rec });
        }
        CustomPicker->setEntries(Es);                                  // nothing ticked — endpoints carry the default
    }
    CustomPicker->setVisible(false);
    DL->addWidget(CustomPicker);
    QObject::connect(CustomBtn, &QToolButton::toggled, &Dlg, [CustomBtn, CustomPicker](bool On){
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
            QCheckBox * cb = new QCheckBox(PlainLabel(Title), Box);
            cb->setChecked(false);
            SecChecks[Lid] = cb; BL->addWidget(cb);
        }
        DL->addWidget(Box);
    }

    // The download selection (ticked endpoints ∪ ticked custom variants ∪ checked secondaries) — the unit every
    // async derivation is scoped to. Until the async endpoint derivation lands, fall back to ALL variants: the
    // all-endpoints default IS the variants' union, so semantics don't shift when the rows appear.
    //A card a graft presents is that graft on whichever versions are picked: it is downloaded with them.
    const auto CardIt = Model.catalogIndex().TileGraft.find(Key.toStdString());
    const std::string CardGraft = CardIt == Model.catalogIndex().TileGraft.end() ? std::string() : CardIt->second;
    auto SelectedLaunchIds = [EpChecks, EpReady, CustomPicker, &SecChecks, Variants, CardGraft]() {
        std::vector<std::string> Sel;
        std::set<std::string> Seen;
        auto Add = [&](const std::string & Id){ if (Seen.insert(Id).second) Sel.push_back(Id); };
        if (*EpReady)
            for (const auto & [cb, Ids] : *EpChecks) { if (cb->isChecked()) for (const std::string & Id : Ids) Add(Id); }
        else
            for (const std::string & Id : Variants) Add(Id);
        for (const std::string & Id : CustomPicker->checkedIds()) Add(Id);
        for (const auto & [Lid, cb] : SecChecks) if (cb->isChecked()) Add(Lid);
        if (!Sel.empty() && !CardGraft.empty()) Add(CardGraft);
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
            for (const Node * R : Installed::CompatibleRunners(*S, *BaseGame))
            {
                const std::string rid = R->NodeId;
                const bool Embedded   = (R->BundleDir == BaseGame->BundleDir);
                const bool Standalone = !Installed::IsEmbeddedRunner(*S, rid);
                if (!Embedded && !Standalone) continue;
                if (Installed::RunnerInstalled(*S, rid)) { AnyCompatInstalled = true; continue; }
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
            QCheckBox * cb = new QCheckBox(PlainLabel(QString::fromStdString(R->Label.empty() ? rid : R->Label) + Suffix), RunBox);
            cb->setChecked(true);
            QObject::connect(cb, &QCheckBox::toggled, RunBox, [Debounce](bool){ Debounce->start(); });   // toggle → re-derive size
            RunnerChecks[rid] = cb; RunL->addWidget(cb);
        };
        if (!EmbeddedR.empty()) { QLabel * h = new QLabel("Embedded", RunBox); h->setStyleSheet("color:" + Theme::Dim + "; font-weight:bold;"); RunL->addWidget(h); for (const Node * R : EmbeddedR) addRunner(R); }
        if (ShowGlobal)         { QLabel * h = new QLabel("Global", RunBox);   h->setStyleSheet("color:" + Theme::Dim + "; font-weight:bold;"); RunL->addWidget(h); for (const Node * R : GlobalR)   addRunner(R); }
        RunBox->setVisible(!EmbeddedR.empty() || ShowGlobal);
    };

    // ── Disk-space display: free space at the library + the (async-derived) size of the current selection ──
    QLabel * SizeLabel = new QLabel("Free space: …", &Dlg);
    SizeLabel->setStyleSheet("color:" + Theme::Dim + ";");
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

    *RecomputeSize = [&Dlg, &RunnerChecks, Snap, SelectedLaunchIds, OptStates, SizeCache, Queried,
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
                    for (const auto & C : PackageCatalog::NodeContentCids(*S, Lid, TickedGrafts(*Tg))) Out->insert(C);
                    for (const auto & [C, Sz] : PackageCatalog::NodeContentSizes(*S, Lid, TickedGrafts(*Tg))) (*Stamped)[C] = Sz;
                }
                for (const std::string & Rid : SelR)
                {
                    for (const auto & C : PackageCatalog::NodeContentCids(*S, Rid)) Out->insert(C);
                    for (const auto & [C, Sz] : PackageCatalog::NodeContentSizes(*S, Rid)) (*Stamped)[C] = Sz;
                }
            },
            [Out, Stamped, SizeCache, Queried, SizeLabel, FreeBytes, Alive, Debounce]{
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
                SizeLabel->setStyleSheet((FreeBytes >= 0 && Sum > FreeBytes) ? "color:" + Theme::Bad + "; font-weight:bold;"
                                                                             : "color:" + Theme::Dim + ";");
                if (Unknown.empty()) return;
                // Probe the new CIDs' sizes in the background; each result lands in the cache and nudges the
                // debounce, so the label refreshes shortly after data arrives. Alive-guarded — the dialog may close.
                //Posted to qApp, not to the manager: the alive check runs there, on the GUI thread (asyncwork.h).
                std::thread([Unknown, SizeCache, Alive, Debounce]{
                    for (const std::string & C : Unknown)
                    {
                        if (!Alive->load()) return;
                        const long long Sz = IpfsWrapper::CidSize(C);
                        QMetaObject::invokeMethod(qApp, [SizeCache, C, Sz, Alive, Debounce]{
                            if (!Alive->load()) return;          // dialog closed — its widgets are gone
                            (*SizeCache)[C] = Sz;
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
                {
                    std::vector<std::string> Pre;
                    for (const std::string & G : Shelf::OfferedGrafts(*S, Lid, &Pre))
                        if (const Node * Gn = S->Find(G); Gn && Seen.insert(G).second)
                            Found->push_back({ G, Gn->NodeId.empty() ? G.substr(0, 12) : Gn->NodeId,
                                               std::find(Pre.begin(), Pre.end(), G) != Pre.end() });
                }
            },
            [Found, OptStates, OptBox, OptL, Debounce]{
                QLayoutItem * It;
                while ((It = OptL->takeAt(0)) != nullptr) { if (QWidget * W = It->widget()) W->deleteLater(); delete It; }
                for (const OptEntry & E : *Found)
                {
                    if (!OptStates->count(E.Id)) (*OptStates)[E.Id] = E.Default;   // first sight → seed with DEFAULT
                    QCheckBox * cb = new QCheckBox(PlainLabel(QString::fromStdString(E.Label)), OptBox);
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

    QObject::connect(Debounce, &QTimer::timeout, &Dlg, [RebuildOptionals, RecomputeSize]{ (*RebuildOptionals)(); (*RecomputeSize)(); });
    QObject::connect(CustomPicker, &VariantPicker::checkedChanged, &Dlg, [Debounce]{ Debounce->start(); });
    for (const auto & [Lid, cb] : SecChecks)    QObject::connect(cb, &QCheckBox::toggled, &Dlg, [Debounce](bool){ Debounce->start(); });
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
                    QCheckBox * cb = new QCheckBox(PlainLabel(Lbl), EpBox);
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

    // A package a variant (or its runner chain) contains has not landed yet, or is not shared with this user: nothing
    // is ever fetched INTO this package to fill the gap (a node has one home), so the download waits for that package.
    bool AnyIncomplete = false;
    {
        const nlohmann::ordered_json CfgForGate = Model.config() ? *Model.config() : nlohmann::ordered_json::object();
        for (const std::string & V : Variants)
        {
            if (PackageCatalog::NodeClosureIncomplete(**Snap, V)) { AnyIncomplete = true; break; }
            for (const std::string & Rid : PackageCatalog::RunnerChainIds(**Snap, V, CfgForGate))
                if (PackageCatalog::NodeClosureIncomplete(**Snap, Rid)) { AnyIncomplete = true; break; }
            if (AnyIncomplete) break;
        }
    }
    if (AnyIncomplete)
    {
        QLabel * Wait = new QLabel("Part of this game is in another package that has not arrived yet, that is not "
                                   "shared with you, or that you have installed in an older version. The download is "
                                   "possible once that package is here as the game needs it.", &Dlg);
        Wait->setWordWrap(true);
        DL->addWidget(Wait);
    }

    QHBoxLayout * BR = new QHBoxLayout(); DL->addLayout(BR); BR->addStretch();
    QPushButton * CancelBtn = new QPushButton("Cancel", &Dlg);
    QPushButton * GoBtn = new QPushButton("Download", &Dlg); GoBtn->setDefault(true);
    GoBtn->setEnabled(!AnyIncomplete);
    BR->addWidget(CancelBtn); BR->addWidget(GoBtn);
    QObject::connect(CancelBtn, &QPushButton::clicked, &Dlg, &QDialog::reject);
    QObject::connect(GoBtn,     &QPushButton::clicked, &Dlg, &QDialog::accept);
    const int Res = Dlg.exec();
    Alive->store(false);                                         // stop the async workers from touching dialog widgets
    if (Res != QDialog::Accepted) return std::nullopt;

    const std::vector<std::string> LaunchIds = SelectedLaunchIds();  // checked variants + checked secondaries
    std::vector<std::string> RunnerIds;                              // selected runners to download + install
    for (auto & [Rid, cb] : RunnerChecks) if (cb->isChecked()) RunnerIds.push_back(Rid);
    if (LaunchIds.empty() && RunnerIds.empty()) return std::nullopt;   // nothing picked
    return DownloadSelection{ LaunchIds, RunnerIds, *OptStates };    // OptStates = the sticky optional-content choices

}

void StartCardDownload(DownloadManager &Downloads, AppModel &Model, LibraryGameCard *Card, QWidget *Parent)
{
    if (!Card) return;
    if (!IpfsFetchReady(Parent)) return;
    const QString Key = Card->GameKey;
    if (Key.isEmpty() || Downloads.downloading(Key)) return;       // already in flight
    if (const auto Sel = AskDownload(Model, *Card, Parent)) Downloads.beginDownload(Key, Sel->LaunchIds, Sel->RunnerIds, Sel->Toggles);
}

void CancelCardDownload(DownloadManager &Downloads, LibraryGameCard *Card, QWidget *Parent)
{
    if (!Card) return;
    const QString Key = Card->GameKey;
    if (Key.isEmpty() || !Downloads.downloading(Key)) return;
    if (QMessageBox::question(Parent, "Cancel download?",
            "Stop downloading “" + Card->GameTitle + "”?") != QMessageBox::Yes) return;
    Downloads.cancelKey(Key);
}
