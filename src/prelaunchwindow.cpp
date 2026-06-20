#include "prelaunchwindow.h"
#include "packageeditor.h"
#include "mainwindow.h"
#include "covercache.h"
#include "packagecatalog.h"
#include "containerwrapper.h"   // StringVariableSubstitution / ContainerParams (CustomVar preview substitution)
#include "jsonoperations.h"

#include <set>
#include <algorithm>

#include <QBrush>
#include <QDir>
#include <QFile>
#include <QPixmap>
#include <QFont>
#include <QLineEdit>
#include <QScrollArea>
#include <QGuiApplication>
#include <QApplication>
#include <QScrollBar>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QResizeEvent>

// ============================================================================
// PreLaunchWindow — node-native launch dialog (one library tile = a GROUP of launchable nodes).
// ============================================================================

PreLaunchWindow::PreLaunchWindow(
    nlohmann::ordered_json*  GlobalConfigJSON,
    const NodeIndex*         Index,
    std::vector<std::string> GroupNodeIds,
    QWidget*                 parent)
    : QDialog(parent)
    , GlobalConfigJSON(GlobalConfigJSON)
    , Index(Index)
    , GroupNodeIds(std::move(GroupNodeIds))
{
    setWindowTitle("Launch");
    setMinimumSize(800, 600);
    setAttribute(Qt::WA_DeleteOnClose);

    // Initial edition = the first in the group (PresentableGroups put RECOMMENDED first).
    if (!this->GroupNodeIds.empty()) LaunchNodeId = this->GroupNodeIds.front();
    if (const Node* L = CurrentLaunch()) { BundleDir = L->BundleDir.string(); PackageUID = L->Uid; }

    // ----- Layout: cover (left) | controls+console (right) -----
    QHBoxLayout* RootLayout = new QHBoxLayout(this);
    RootLayout->setContentsMargins(0, 0, 0, 0);
    RootLayout->setSpacing(0);

    // Cover on the LEFT — shown large and centered vertically, scaled to fit its column (UpdateCoverScaled on resize)
    // while preserving aspect ratio.
    CoverLabel = new QLabel(this);
    CoverLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    CoverLabel->setMinimumWidth(200);
    CoverLabel->setAlignment(Qt::AlignCenter);
    CoverLabel->setContentsMargins(8, 8, 8, 8);
    RootLayout->addWidget(CoverLabel, 1);

    QWidget*     RightWidget = new QWidget(this);   // the RIGHT column (pickers/options/console/buttons)
    QVBoxLayout* RightLayout = new QVBoxLayout(RightWidget);
    RightLayout->setContentsMargins(0, 0, 0, 0);
    RootLayout->addWidget(RightWidget, 2);

    QSplitter* VSplitter = new QSplitter(Qt::Vertical, RightWidget);
    RightLayout->addWidget(VSplitter, 1);

    QWidget*     ControlWidget = new QWidget(VSplitter);
    QVBoxLayout* ControlLayout = new QVBoxLayout(ControlWidget);
    VSplitter->addWidget(ControlWidget);

    QFormLayout* PickerForm = new QFormLayout();
    ControlLayout->addLayout(PickerForm);

    // Edition combo (the group's launchable nodes) — hidden when there's only one.
    EditionCombo = new QComboBox(ControlWidget);
    PickerForm->addRow("Edition:", EditionCombo);
    EditionLabel = PickerForm->labelForField(EditionCombo);
    for (const std::string& Id : this->GroupNodeIds)
    {
        const Node* N = Index ? Index->Find(Id) : nullptr;
        if (!N) continue;
        std::string Lbl = !N->Label.empty() ? N->Label
                          : (N->Meta.is_object() ? N->Meta.value("TITLE", Id) : Id);
        if (N->Recommended) Lbl = "⭐ " + Lbl;
        EditionCombo->addItem(QString::fromStdString(Lbl), QString::fromStdString(Id));
    }
    {
        int Sel = EditionCombo->findData(QString::fromStdString(LaunchNodeId));
        if (Sel >= 0) EditionCombo->setCurrentIndex(Sel);
        bool Multi = EditionCombo->count() > 1;
        EditionCombo->setVisible(Multi);
        if (EditionLabel) EditionLabel->setVisible(Multi);
    }

    // Runner combo — populated per edition.
    RunnerCombo = new QComboBox(ControlWidget);
    PickerForm->addRow("Runner:", RunnerCombo);

    // Scrollable section: module toggles + CustomVar pickers.
    QScrollArea* CVScrollArea = new QScrollArea(ControlWidget);
    CVScrollArea->setWidgetResizable(true);
    CVScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    CVScrollArea->setFrameShape(QFrame::NoFrame);
    QWidget*     CVContainer       = new QWidget();
    QVBoxLayout* CVContainerLayout = new QVBoxLayout(CVContainer);
    CVContainerLayout->setContentsMargins(8, 6, 8, 6);   // breathing room so the options aren't crammed
    CVContainerLayout->setSpacing(12);
    CVScrollArea->setWidget(CVContainer);
    ControlLayout->addWidget(CVScrollArea, 1);            // stretch: fill the pane (no empty gap below the options)

    ModuleGroup = new QGroupBox("Modules", CVContainer);
    QVBoxLayout* ModuleLayout = new QVBoxLayout(ModuleGroup);
    ModuleTree = new QTreeWidget(ModuleGroup);
    ModuleTree->setHeaderHidden(true);
    ModuleTree->setRootIsDecorated(false);
    ModuleTree->setSelectionMode(QAbstractItemView::NoSelection);
    ModuleLayout->addWidget(ModuleTree);
    ModuleGroup->setVisible(false);
    CVContainerLayout->addWidget(ModuleGroup);
    connect(ModuleTree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* It, int){ PropagateModuleItem(It); });

    CustomVarGroup = new QGroupBox("Options", CVContainer);
    CustomVarForm  = new QFormLayout(CustomVarGroup);
    CustomVarForm->setVerticalSpacing(8);
    CustomVarForm->setHorizontalSpacing(12);
    CustomVarForm->setContentsMargins(10, 8, 10, 8);
    CustomVarGroup->setVisible(false);
    CVContainerLayout->addWidget(CustomVarGroup);

    RememberCheck         = new QCheckBox("Hide this dialog next time",          CVContainer);
    CloseAfterLaunchCheck = new QCheckBox("Close window when game starts",       CVContainer);
    DryRunCheck           = new QCheckBox("Dry test run (delete WRITELAYER on cleanup)", CVContainer);
    PreserveRuntimeCheck  = new QCheckBox("Inspect runtime after exit (pause before cleanup)", CVContainer);
    PreserveRuntimeCheck->setToolTip("After the game exits, pause with a dialog while this run's runtime (mounts + "
                                     "files) is still in place so you can inspect it. It's cleaned up (unmounted + "
                                     "deleted) as soon as you close that dialog — never left dangling.");
    CVContainerLayout->addWidget(RememberCheck);
    CVContainerLayout->addWidget(CloseAfterLaunchCheck);
    CVContainerLayout->addWidget(DryRunCheck);
    CVContainerLayout->addWidget(PreserveRuntimeCheck);
    CVContainerLayout->addStretch();

    ProgressBar = new QProgressBar(ControlWidget);
    ProgressBar->setRange(0, 100);
    ProgressBar->setValue(0);
    ProgressBar->setVisible(false);
    ControlLayout->addWidget(ProgressBar);

    StatusLabel = new QLabel(ControlWidget);
    ControlLayout->addWidget(StatusLabel);

    ConsoleEdit = new QTextEdit(VSplitter);
    ConsoleEdit->setReadOnly(true);
    QFont MonoFont("Monospace");
    MonoFont.setStyleHint(QFont::Monospace);
    MonoFont.setPointSize(9);
    ConsoleEdit->setFont(MonoFont);
    ConsoleEdit->setStyleSheet("QTextEdit { background-color: #1a1a1a; color: #e0e0e0; }");
    VSplitter->addWidget(ConsoleEdit);
    VSplitter->setStretchFactor(0, 1);
    VSplitter->setStretchFactor(1, 2);

    // ----- Button row -----
    QWidget*     BtnWidget = new QWidget(RightWidget);
    QHBoxLayout* BtnLayout = new QHBoxLayout(BtnWidget);
    RightLayout->addWidget(BtnWidget);

    QPushButton* PackageEditorButton = new QPushButton("Package Editor", BtnWidget);
    BtnLayout->addWidget(PackageEditorButton);
    connect(PackageEditorButton, &QPushButton::clicked, this, [this]()
    {
        PackageEditor* Editor = new PackageEditor(this->GlobalConfigJSON, this, QString::fromStdString(this->BundleDir));
        connect(Editor, &PackageEditor::packageSaved, &MainWindow::RefreshPackage);
        Editor->show();
    });
    BtnLayout->addStretch();

    KillButton = new QPushButton("Kill", BtnWidget);
    KillButton->setEnabled(false);
    BtnLayout->addWidget(KillButton);
    LaunchButton = new QPushButton("Launch", BtnWidget);
    LaunchButton->setDefault(true);
    BtnLayout->addWidget(LaunchButton);
    CloseButton = new QPushButton("Close", BtnWidget);
    BtnLayout->addWidget(CloseButton);

    connect(EditionCombo, &QComboBox::currentIndexChanged, this, &PreLaunchWindow::onEditionChanged);
    connect(RunnerCombo,  &QComboBox::currentIndexChanged, this, [this](int){ RebuildCustomVarPickers(); });
    connect(LaunchButton, &QPushButton::clicked, this, &PreLaunchWindow::onLaunchClicked);
    connect(KillButton,   &QPushButton::clicked, this, &PreLaunchWindow::onKillClicked);
    connect(CloseButton,  &QPushButton::clicked, this, &QDialog::accept);

    RebuildCover();
    RebuildRunnerCombo();
    RebuildModuleTree();
    RebuildCustomVarPickers();

    // Open at a size where every section is fully visible — no manual resize or splitter drag needed. The control
    // pane lives in a scroll area (whose own sizeHint is small), so derive the height it needs from its actual
    // content and seed the splitter to give the controls that height; the console takes the rest. Capped to the
    // screen so it never opens off-screen.
    CVContainer->adjustSize();
    const int ControlsH = CVContainer->sizeHint().height() + 96;   // + the pickers + status line above the scroll area
    int W = 980, H = ControlsH + 300 /*usable console*/ + 70 /*buttons + margins*/;
    if (QScreen* Scr = QGuiApplication::primaryScreen())
    {
        const QRect A = Scr->availableGeometry();
        W = std::min(W, A.width()  - 80);
        H = std::min(H, A.height() - 80);
    }
    resize(std::max(W, 800), std::max(H, 600));
    VSplitter->setSizes({ControlsH, std::max(200, height() - ControlsH)});
}

PreLaunchWindow::~PreLaunchWindow()
{
    if (LaunchWorker && LaunchWorker->isRunning())
    {
        LaunchWorker->kill();
        LaunchWorker->wait();
    }
    delete LaunchWorker;
}

const Node* PreLaunchWindow::CurrentLaunch() const
{
    return (Index && !LaunchNodeId.empty()) ? Index->Find(LaunchNodeId) : nullptr;
}

void PreLaunchWindow::RebuildCover()
{
    const Node* L = CurrentLaunch();
    if (!L) return;
    const std::string Title = L->Meta.is_object() ? L->Meta.value("TITLE", LaunchNodeId) : LaunchNodeId;
    setWindowTitle("Launch " + QString::fromStdString(Title));

    CoverLabel->clear();
    CoverPixmap = QPixmap();
    if (!L->Meta.is_object() || !L->Meta.contains("COVER")) return;
    const nlohmann::ordered_json& CoverNode = L->Meta["COVER"];
    const QString Pkg = QString::fromStdString(L->BundleDir.string());
    auto setFrom = [this](const QString& Path){
        QPixmap Pix(Path);
        if (!Pix.isNull()) { CoverPixmap = Pix; UpdateCoverScaled(); }
    };
    const QString Now = CoverCache::instance()->resolve(CoverNode, Pkg);
    if (!Now.isEmpty()) { setFrom(Now); return; }
    QString F, Cid; CoverCache::Locate(CoverNode, F, Cid);
    if (!Cid.isEmpty())
    {
        const nlohmann::ordered_json CoverCopy = CoverNode;
        connect(CoverCache::instance(), &CoverCache::coverReady, this, [this, Cid, CoverCopy, Pkg, setFrom](QString Ready){
            if (Ready != Cid) return;
            const QString P = CoverCache::instance()->resolve(CoverCopy, Pkg);
            if (!P.isEmpty()) setFrom(P);
        });
    }
}

// Scale the full-res cover to fit CoverLabel's current content area, preserving aspect ratio. AlignCenter on the
// label then centers it vertically (and horizontally) in its column. No-op until the label has a real size.
void PreLaunchWindow::UpdateCoverScaled()
{
    if (!CoverLabel || CoverPixmap.isNull()) return;
    const QSize Avail = CoverLabel->contentsRect().size();
    if (Avail.width() <= 1 || Avail.height() <= 1) return;
    CoverLabel->setPixmap(CoverPixmap.scaled(Avail, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void PreLaunchWindow::resizeEvent(QResizeEvent* Event)
{
    QDialog::resizeEvent(Event);
    UpdateCoverScaled();
}

void PreLaunchWindow::RebuildRunnerCombo()
{
    QSignalBlocker B(RunnerCombo);
    RunnerCombo->clear();
    const Node* L = CurrentLaunch();
    if (!L) return;
    for (const Node* R : PackageCatalog::RunnerCandidates(*Index, *L))
        RunnerCombo->addItem(QString::fromStdString(R->NodeId), QString::fromStdString(R->NodeId));

    // Pre-select USERSETTINGS PREFERRED_RUNNER, else the first candidate.
    auto US = PackageCatalog::GetPackageUserSettings(*GlobalConfigJSON, PackageUID);
    if (US.contains("PREFERRED_RUNNER") && US["PREFERRED_RUNNER"].is_string())
    {
        int Idx = RunnerCombo->findData(QString::fromStdString(std::string(US["PREFERRED_RUNNER"])));
        if (Idx >= 0) RunnerCombo->setCurrentIndex(Idx);
    }
}

void PreLaunchWindow::RebuildModuleTree()
{
    if (!ModuleTree) return;
    QSignalBlocker B(ModuleTree);
    ModuleTree->clear();
    ModuleExcludes.clear();
    LockedModules.clear();

    const Node* L = CurrentLaunch();
    if (!L) { ModuleGroup->setVisible(false); return; }

    auto US = PackageCatalog::GetPackageUserSettings(*GlobalConfigJSON, PackageUID);
    const nlohmann::ordered_json SavedMods = (US.contains("MODULES") && US["MODULES"].is_object())
                                              ? US["MODULES"] : nlohmann::ordered_json::object();

    const std::vector<const Node*> Opts = ManifestModel::OptionalNodes(*Index, LaunchNodeId);
    for (const Node* N : Opts)
    {
        bool Desired = N->Default;
        if (SavedMods.contains(N->NodeId) && SavedMods[N->NodeId].is_boolean())
            Desired = bool(SavedMods[N->NodeId]);

        QTreeWidgetItem* It = new QTreeWidgetItem(ModuleTree);
        It->setText(0, QString::fromStdString(N->Label.empty() ? N->NodeId : N->Label));
        It->setData(0, Qt::UserRole,     QString::fromStdString(N->NodeId));   // node id
        It->setData(0, Qt::UserRole + 1, false);                              // required (optional nodes only here)
        It->setData(0, Qt::UserRole + 2, Desired);                            // desired state
        for (const std::string& E : N->Exclude) { ModuleExcludes[N->NodeId].insert(E); ModuleExcludes[E].insert(N->NodeId); }
    }
    ModuleGroup->setVisible(ModuleTree->topLevelItemCount() > 0);
    // Size the tree to its content (capped at 8 rows) so all modules are visible without an inner scrollbar — the
    // group then claims its proper space in the control pane instead of being squashed to a couple of rows.
    if (const int Rows = ModuleTree->topLevelItemCount(); Rows > 0)
    {
        int RowH = ModuleTree->sizeHintForRow(0);
        if (RowH <= 0) RowH = 22;
        const int VisibleRows = std::min(Rows, 8);
        const int H = VisibleRows * RowH + 2 * ModuleTree->frameWidth() + 4;
        ModuleTree->setMinimumHeight(H);
        ModuleTree->setMaximumHeight(Rows <= 8 ? H : QWIDGETSIZE_MAX);   // exact fit if few; scroll if many
    }
    RefreshModuleLocks();
}

void PreLaunchWindow::RefreshModuleLocks()
{
    if (!ModuleTree) return;
    QSignalBlocker B(ModuleTree);
    for (int i = 0; i < ModuleTree->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem* It = ModuleTree->topLevelItem(i);
        bool Desired = It->data(0, Qt::UserRole + 2).toBool();
        It->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        It->setCheckState(0, Desired ? Qt::Checked : Qt::Unchecked);
        It->setData(0, Qt::UserRole + 3, false);   // never locked (all optional)
    }
}

void PreLaunchWindow::PropagateModuleItem(QTreeWidgetItem* Item)
{
    if (!Item) return;
    const bool NowChecked = Item->checkState(0) == Qt::Checked;
    const std::string Node = Item->data(0, Qt::UserRole).toString().toStdString();
    Item->setData(0, Qt::UserRole + 2, NowChecked);

    // Mutual exclusion: ticking one unticks every sibling it excludes.
    auto ai = ModuleExcludes.find(Node);
    if (NowChecked && ai != ModuleExcludes.end())
        for (int i = 0; i < ModuleTree->topLevelItemCount(); ++i)
        {
            QTreeWidgetItem* It = ModuleTree->topLevelItem(i);
            if (It != Item && ai->second.count(It->data(0, Qt::UserRole).toString().toStdString()))
                It->setData(0, Qt::UserRole + 2, false);
        }
    RefreshModuleLocks();
    RebuildCustomVarPickers();   // toggling a module changes the enabled closure's CustomVars
}

std::map<std::string, bool> PreLaunchWindow::CollectModuleStates() const
{
    std::map<std::string, bool> States;
    if (!ModuleTree) return States;
    for (int i = 0; i < ModuleTree->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem* It = ModuleTree->topLevelItem(i);
        States[It->data(0, Qt::UserRole).toString().toStdString()] = It->data(0, Qt::UserRole + 2).toBool();
    }
    return States;
}

void PreLaunchWindow::RebuildCustomVarPickers()
{
    while (CustomVarForm->rowCount() > 0) CustomVarForm->removeRow(0);
    const Node* L = CurrentLaunch();
    if (!L) { CustomVarGroup->setVisible(false); return; }

    const std::map<std::string, bool> Toggles = CollectModuleStates();

    // The enabled content-node closure (honouring the current module toggles).
    const std::vector<std::string> Order = ManifestModel::ResolveNodeOrder(*Index, LaunchNodeId, Toggles);

    // The selected runner's content closure — its CustomVars are tweakable knobs the engine resolves too
    // (ResolveCustomVariables, scoped to RunnerRecipe), so surface them here as if they were package vars.
    std::vector<std::string> RunnerOrder;
    if (RunnerCombo->currentIndex() >= 0)
        RunnerOrder = ManifestModel::ResolveNodeOrder(*Index, RunnerCombo->currentData().toString().toStdString(), Toggles);

    // Build a scan string so we only surface CustomVars actually referenced by %KEY% somewhere in the closure /
    // runner closure (their layers) or the selected runner's EXEC (ARGS/ENV — how a game seeds knobs to its runner).
    std::string ScanStr;
    for (const std::string& Id : Order)
    {
        const Node* N = Index->Find(Id);
        if (N && N->Layers.is_array()) ScanStr += N->Layers.dump();
    }
    for (const std::string& Id : RunnerOrder)
    {
        const Node* N = Index->Find(Id);
        if (N && N->Layers.is_array()) ScanStr += N->Layers.dump();
    }
    if (RunnerCombo->currentIndex() >= 0)
        if (const Node* R = Index->Find(RunnerCombo->currentData().toString().toStdString()))
            if (R->Exec.is_object()) ScanStr += R->Exec.dump();

    auto US = PackageCatalog::GetPackageUserSettings(*GlobalConfigJSON, PackageUID);
    const nlohmann::ordered_json SavedVars = (US.contains("VARIABLES") && US["VARIABLES"].is_object())
                                              ? US["VARIABLES"] : nlohmann::ordered_json::object();

    bool AnyVisible = false;
    std::set<std::string> SeenKeys;   // a KEY surfaces once; package nodes are walked first so they win on collision

    // Emit a group of CustomVar pickers for one node's LAYERS. GroupPrefix distinguishes runner-provided knobs.
    auto EmitNodeVars = [&](const Node* N, const QString& GroupPrefix)
    {
        if (!N || !N->Layers.is_array()) return;
        QGroupBox*   Box  = nullptr;
        QFormLayout* Form = nullptr;
        auto EnsureBox = [&]() {
            if (Box) return;
            Box  = new QGroupBox(GroupPrefix + QString::fromStdString(N->Label.empty() ? N->NodeId : N->Label), CustomVarGroup);
            Form = new QFormLayout(Box);
            Form->setVerticalSpacing(8);
            Form->setHorizontalSpacing(12);
            Form->setContentsMargins(10, 8, 10, 8);
        };
        for (const auto& CV : N->Layers)
        {
            if (!CV.is_object() || CV.value("TYPE", std::string()) != "CustomVar") continue;
            std::string Key = CV.value("KEY", std::string());
            if (Key.empty() || !CV.value("DISPLAY", true)) continue;
            if (SeenKeys.count(Key)) continue;                                  // already surfaced (package wins)
            if (ScanStr.find("%" + Key + "%") == std::string::npos) continue;   // not referenced — skip
            //"random" vars are auto-picked from OPTIONS by the engine at launch; never show an editable picker —
            //collecting its value would pass a VariableOverride that the engine honours OVER the random pick
            //(e.g. an empty field → empty Warcraft III CD key). Leave it to the engine.
            if (CV.value("VARTYPE", std::string("string")) == "random") continue;

            std::string Initial = CV.value("DEFAULT", std::string());
            if (SavedVars.contains(Key) && SavedVars[Key].is_string()) Initial = std::string(SavedVars[Key]);
            { ContainerParams TmpP("", "", ""); ContainerWrapper::StringVariableSubstitution(Initial, TmpP.GetVariablesMap()); }

            AnyVisible = true; SeenKeys.insert(Key); EnsureBox();
            const std::string VarType = CV.value("VARTYPE", std::string("string"));
            const QString Label = QString::fromStdString(CV.value("LABEL", Key));

            if (VarType == "options" && CV.contains("OPTIONS") && CV["OPTIONS"].is_array())
            {
                QComboBox* Combo = new QComboBox(CustomVarGroup);
                Combo->setProperty("CVKey", QString::fromStdString(Key));
                for (const auto& Opt : CV["OPTIONS"])
                {
                    std::string OptVal = Opt.value("VALUE", std::string());
                    ContainerParams TmpP("", "", ""); ContainerWrapper::StringVariableSubstitution(OptVal, TmpP.GetVariablesMap());
                    Combo->addItem(QString::fromStdString(Opt.value("LABEL", std::string())), QString::fromStdString(OptVal));
                }
                for (int k = 0; k < Combo->count(); k++)
                    if (Combo->itemData(k).toString().toStdString() == Initial) { Combo->setCurrentIndex(k); break; }
                Form->addRow(Label + ":", Combo);
            }
            else if (VarType == "dword" || VarType == "qword" || VarType == "number")
            {
                QSpinBox* Spin = new QSpinBox(CustomVarGroup);
                Spin->setProperty("CVKey", QString::fromStdString(Key));
                Spin->setMaximum(2147483647);
                try { Spin->setValue(std::stoi(Initial)); } catch (...) { Spin->setValue(0); }
                Form->addRow(Label + ":", Spin);
            }
            else if (VarType == "bool")
            {
                QCheckBox* Check = new QCheckBox(CustomVarGroup);
                Check->setProperty("CVKey", QString::fromStdString(Key));
                std::string lo = Initial; std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
                Check->setChecked(lo == "1" || lo == "true" || lo == "yes");
                Form->addRow(Label + ":", Check);
            }
            else
            {
                QLineEdit* Field = new QLineEdit(CustomVarGroup);
                Field->setProperty("CVKey", QString::fromStdString(Key));
                Field->setText(QString::fromStdString(Initial));
                Form->addRow(Label + ":", Field);
            }
        }
        if (Box) CustomVarForm->addRow(Box);
    };

    // Package closure first (so a KEY declared by both wins from the package), then the runner's own knobs.
    for (const std::string& Id : Order) EmitNodeVars(Index->Find(Id), QString());
    for (const std::string& Id : RunnerOrder)
    {
        const Node* N = Index->Find(Id);
        if (N && !N->IsRunner()) EmitNodeVars(N, "Runner · ");   // skip runner nodes (match engine's RunnerComponents)
    }

    CustomVarGroup->setVisible(AnyVisible);
}

void PreLaunchWindow::onEditionChanged()
{
    if (EditionCombo->currentIndex() < 0) return;
    LaunchNodeId = EditionCombo->currentData().toString().toStdString();
    if (const Node* L = CurrentLaunch()) { BundleDir = L->BundleDir.string(); PackageUID = L->Uid; }
    RebuildCover();
    RebuildRunnerCombo();
    RebuildModuleTree();
    RebuildCustomVarPickers();
}

void PreLaunchWindow::ReloadAndRebuild()
{
    // Drop editions that no longer exist (e.g. after an edit) and rebuild everything from the (shared) index.
    std::vector<std::string> Live;
    for (const std::string& Id : GroupNodeIds) if (Index && Index->Find(Id)) Live.push_back(Id);
    GroupNodeIds = Live;
    QSignalBlocker B(EditionCombo);
    EditionCombo->clear();
    for (const std::string& Id : GroupNodeIds)
    {
        const Node* N = Index->Find(Id);
        std::string Lbl = !N->Label.empty() ? N->Label : (N->Meta.is_object() ? N->Meta.value("TITLE", Id) : Id);
        if (N->Recommended) Lbl = "⭐ " + Lbl;
        EditionCombo->addItem(QString::fromStdString(Lbl), QString::fromStdString(Id));
    }
    int Sel = EditionCombo->findData(QString::fromStdString(LaunchNodeId));
    EditionCombo->setCurrentIndex(Sel >= 0 ? Sel : 0);
    onEditionChanged();
}

void PreLaunchWindow::persistGlobalConfig()
{
    QDir AppDataDir(QDir::homePath() + "/.VidyaGod");
    JSONOps::SaveJSON(GlobalConfigJSON, new QFile(AppDataDir.filePath("GlobalConfig.JSON")));
}

void PreLaunchWindow::onLaunchClicked()
{
    if (LaunchNodeId.empty()) return;
    const std::string SelectedRunnerID = (RunnerCombo->currentIndex() >= 0)
                                         ? RunnerCombo->currentData().toString().toStdString() : std::string();

    // Collect the visible CustomVar picker values (bare KEY -> value).
    std::map<std::string, std::string> PickerVars;
    for (QWidget* W : CustomVarGroup->findChildren<QWidget*>())
    {
        QString Key = W->property("CVKey").toString();
        if (Key.isEmpty()) continue;
        std::string Val;
        if (auto* CB = qobject_cast<QComboBox*>(W))      Val = CB->currentData().toString().toStdString();
        else if (auto* SP = qobject_cast<QSpinBox*>(W))  Val = std::to_string(SP->value());
        else if (auto* CK = qobject_cast<QCheckBox*>(W)) Val = CK->isChecked() ? "1" : "0";
        else if (auto* LE = qobject_cast<QLineEdit*>(W)) Val = LE->text().toStdString();
        else continue;
        PickerVars[Key.toStdString()] = Val;
    }

    // Persist prefs (keyed by the bundle UID, so the engine's GetPackageUserSettings sees them).
    if (!PackageUID.empty())
    {
        if (!PickerVars.empty())
        {
            auto US = PackageCatalog::GetPackageUserSettings(*GlobalConfigJSON, PackageUID);
            nlohmann::ordered_json Vars = (US.contains("VARIABLES") && US["VARIABLES"].is_object())
                                           ? US["VARIABLES"] : nlohmann::ordered_json::object();
            for (const auto& [K, V] : PickerVars) Vars[K] = V;
            PackageCatalog::SetPackageUserSetting(*GlobalConfigJSON, PackageUID, "VARIABLES", Vars);
        }
        nlohmann::ordered_json Mods = nlohmann::ordered_json::object();
        for (const auto& [N, On] : CollectModuleStates()) Mods[N] = On;
        PackageCatalog::SetPackageUserSetting(*GlobalConfigJSON, PackageUID, "MODULES", Mods);
        PackageCatalog::SetPackageUserSetting(*GlobalConfigJSON, PackageUID, "PREFERRED_RUNNER", SelectedRunnerID);
        if (RememberCheck->isChecked())
            PackageCatalog::SetPackageUserSetting(*GlobalConfigJSON, PackageUID, "SKIP_LAUNCH_DIALOG", true);
        persistGlobalConfig();
    }

    // Disable controls + show progress.
    RunnerCombo->setEnabled(false); EditionCombo->setEnabled(false); CustomVarGroup->setEnabled(false);
    ModuleGroup->setEnabled(false);
    RememberCheck->setEnabled(false);
    CloseAfterLaunchCheck->setEnabled(false); DryRunCheck->setEnabled(false); PreserveRuntimeCheck->setEnabled(false);
    LaunchButton->setEnabled(false); CloseButton->setEnabled(false);
    ProgressBar->setValue(0); ProgressBar->setVisible(true);
    StatusLabel->setText("Starting...");

    // Build + start the worker — native node launch via LaunchNodeId.
    LaunchWorker = new LaunchThread();
    LaunchWorker->GlobalConfigJSON = *GlobalConfigJSON;
    LaunchWorker->LaunchNodeId     = LaunchNodeId;
    LaunchWorker->VariableOverrides = PickerVars;
    LaunchWorker->ModuleStates      = CollectModuleStates();
    LaunchWorker->RunnerID          = SelectedRunnerID;
    LaunchWorker->DryRun            = DryRunCheck->isChecked();
    LaunchWorker->PreserveRuntime   = PreserveRuntimeCheck->isChecked();
    if (QScreen* Scr = QGuiApplication::primaryScreen())
    {
        LaunchWorker->ScreenWidth  = std::to_string(Scr->geometry().width());
        LaunchWorker->ScreenHeight = std::to_string(Scr->geometry().height());
    }
    connect(LaunchWorker, &LaunchThread::logLine,         this, &PreLaunchWindow::onLogLine,         Qt::QueuedConnection);
    connect(LaunchWorker, &LaunchThread::statusChanged,   this, &PreLaunchWindow::onStatusChanged,   Qt::QueuedConnection);
    connect(LaunchWorker, &LaunchThread::progressChanged, this, &PreLaunchWindow::onProgressChanged, Qt::QueuedConnection);
    connect(LaunchWorker, &LaunchThread::launchFinished,  this, &PreLaunchWindow::onLaunchFinished,  Qt::QueuedConnection);
    LaunchWorker->start();
}

void PreLaunchWindow::onKillClicked()
{
    if (LaunchWorker) LaunchWorker->kill();
}

void PreLaunchWindow::onLogLine(int level, QString context, QString message)
{
    QString color;
    switch (static_cast<LogLevel>(level))
    {
        case LogLevel::ERR:  color = "#cc0000"; break;
        case LogLevel::WARN: color = "#cccc00"; break;
        case LogLevel::SUCC: color = "#00cc00"; break;
        default:             color = "";        break;
    }
    QString Text = context.toHtmlEscaped() + " " + message.toHtmlEscaped();
    ConsoleEdit->append(color.isEmpty() ? Text : QString("<span style=\"color:%1\">%2</span>").arg(color, Text));
    QScrollBar* SB = ConsoleEdit->verticalScrollBar();
    SB->setValue(SB->maximum());
}

void PreLaunchWindow::onStatusChanged(QString status) { StatusLabel->setText(status); }

void PreLaunchWindow::onProgressChanged(int value)
{
    ProgressBar->setValue(value);
    if (value >= 95)
    {
        KillButton->setEnabled(true);
        if (CloseAfterLaunchCheck->isChecked()) accept();
    }
}

void PreLaunchWindow::onLaunchFinished(bool success, QString errorMsg)
{
    KillButton->setEnabled(false);
    CloseButton->setEnabled(true);
    if (success) StatusLabel->setText("Finished.");
    else { StatusLabel->setText("Error: " + errorMsg); QMessageBox::warning(this, "Launch failed", errorMsg); }

    RunnerCombo->setEnabled(true); EditionCombo->setEnabled(true); CustomVarGroup->setEnabled(true);
    ModuleGroup->setEnabled(true);
    RememberCheck->setEnabled(true);
    CloseAfterLaunchCheck->setEnabled(true); DryRunCheck->setEnabled(true); PreserveRuntimeCheck->setEnabled(true);
    LaunchButton->setEnabled(true);
    ProgressBar->setValue(0); ProgressBar->setVisible(false);
}
