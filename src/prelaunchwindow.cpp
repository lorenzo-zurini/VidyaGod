#include "prelaunchwindow.h"
#include "packageeditor.h"
#include "mainwindow.h"
#include "covercache.h"
#include "runnerwrapper.h"
#include <random>
#include <set>
#include <algorithm>
#include <functional>
#include <QBrush>

#include <QDir>
#include <QPixmap>
#include <QFont>
#include <QGuiApplication>
#include <QApplication>
#include <QScrollBar>
#include <QMessageBox>

// ============================================================================
// LaunchThread
// ============================================================================

// Forwards a kill request to the active ContainerWrapper (if any).
void LaunchThread::kill()
{
    QMutexLocker Locker(&wrapperMutex);
    if (wrapper)
        wrapper->KillGame();
}

// Full container lifecycle executed on the worker thread.
// Emits progress/status signals at key milestones so the UI can stay in sync.
void LaunchThread::run()
{
    // -----------------------------------------------------------------
    // Install the log callback: emit logLine() (queued automatically by
    // Qt's cross-thread signal delivery) and derive progress/status from
    // well-known context+message patterns.
    // -----------------------------------------------------------------
    SetLogCallback([this](LogLevel level, const std::string& ctx, const std::string& msg)
    {
        // Forward the raw line to the console widget.
        emit logLine(static_cast<int>(level), QString::fromStdString(ctx), QString::fromStdString(msg));

        // ---- progress / status heuristics ----
        if (ctx.find("InitializeContainer") != std::string::npos)
        {
            emit progressChanged(5);
            emit statusChanged("Initializing container...");
        }
        else if (ctx.find("InitializeDefPrefix") != std::string::npos)
        {
            if (msg.find("Initialising") != std::string::npos)
            {
                emit progressChanged(10);
                emit statusChanged("Initializing Wine prefix...");
            }
            else if (msg.find("successful") != std::string::npos ||
                     msg.find("SUC") != std::string::npos ||
                     msg.find("Prefix initialisation successful") != std::string::npos)
            {
                emit progressChanged(25);
                emit statusChanged("Prefix ready.");
            }
        }
        else if (ctx.find("BuildDefaultData") != std::string::npos)
        {
            emit progressChanged(35);
            emit statusChanged("Preparing package edits...");
            if (msg.find("DEFAULTDATA layer built") != std::string::npos)
                emit progressChanged(45);
        }
        else if (ctx.find("MountVFS") != std::string::npos && msg.find("Mounting via") != std::string::npos)
        {
            emit progressChanged(55);
            emit statusChanged("Assembling filesystem...");
        }
        else if (ctx.find("MountVFS") != std::string::npos &&
                 (msg.find("Successfully") != std::string::npos || msg.find("mounted VFS") != std::string::npos))
        {
            emit progressChanged(75);
            emit statusChanged("VFS mounted.");
        }
        else if (ctx.find("BuildContainerRuntime") != std::string::npos &&
                 msg.find("Runtime ready") != std::string::npos)
        {
            emit progressChanged(90);
            emit statusChanged("Runtime ready.");
        }
        else if (ctx.find("Execute") != std::string::npos && msg.find("Executing:") != std::string::npos)
        {
            emit progressChanged(95);
            emit statusChanged("Game running...");
        }
        else if (ctx.find("Execute") != std::string::npos && msg.find("Process exited") != std::string::npos)
        {
            emit progressChanged(100);
            emit statusChanged("Done.");
        }
    });

    // -----------------------------------------------------------------
    // Build ContainerParams and ContainerWrapper.
    // -----------------------------------------------------------------
    struct ContainerParams Params(PackagePath, SubgameID, ComponentID);
    Params.VariableOverrides = this->VariableOverrides;
    Params.ModuleStates      = this->ModuleStates;
    //Set the chosen variant AND runner BEFORE construction so DeriveContainerParams resolves the
    //runner (and its MODULES) and CreateRecipe mounts the enabled modules. No post-construction override needed.
    Params.VariantID = this->VariantID;
    Params.RunnerID  = this->RunnerID;
    //Screen geometry was captured on the main thread (see onLaunchClicked) — DeriveContainerParams
    //uses these instead of querying QGuiApplication from this worker thread.
    Params.ScreenWidth  = this->ScreenWidth;
    Params.ScreenHeight = this->ScreenHeight;

    ContainerWrapper* LocalWrapper = new ContainerWrapper(GlobalConfigJSON, MANIFESTJSON, Params);

    // Store wrapper pointer so kill() can reach it.
    {
        QMutexLocker Locker(&wrapperMutex);
        wrapper = LocalWrapper;
    }

    // -----------------------------------------------------------------
    // Step 1: Resolve executable definition (VariantID was set pre-construction).
    // -----------------------------------------------------------------
    if (!ContainerWrapper::ResolveExecutableDefinition(MANIFESTJSON, LocalWrapper->ContainerParams))
    {
        ClearLogCallback();
        {
            QMutexLocker Locker(&wrapperMutex);
            wrapper = nullptr;
        }
        delete LocalWrapper;
        emit launchFinished(false, "Could not resolve variant.\nCheck VARIANT_ID and MODULES in the manifest.");
        return;
    }

    // -----------------------------------------------------------------
    // Step 2: Build runtime (mounts, prefix, registry patches).
    // -----------------------------------------------------------------
    if (!LocalWrapper->BuildContainerRuntime())
    {
        LocalWrapper->Cleanup();
        ClearLogCallback();
        {
            QMutexLocker Locker(&wrapperMutex);
            wrapper = nullptr;
        }
        delete LocalWrapper;
        emit launchFinished(false, "Failed to build container runtime.\nCheck that all components are defined and their zip files exist.");
        return;
    }

    // -----------------------------------------------------------------
    // Step 3: Execute game (blocks until process exits or is killed).
    // -----------------------------------------------------------------
    bool ExecOk = LocalWrapper->Execute();

    // -----------------------------------------------------------------
    // Step 4: Cleanup (unless the user opted out).
    // -----------------------------------------------------------------
    std::filesystem::path WriteLayerPath = LocalWrapper->ContainerParams.WriteLayerPath;
    if (!SkipCleanup)
        LocalWrapper->Cleanup();

    if (this->DryRun)
    {
        //WRITELAYER is ephemeral and normally already removed by Cleanup(); this also covers
        //the SkipCleanup case so a dry run never leaves write-layer state behind.
        std::error_code ec;
        std::filesystem::remove_all(WriteLayerPath, ec);
        if (ec) LogWarn("LaunchThread", "Dry run: could not remove WRITELAYER: " + ec.message());
        else    LogSucc("LaunchThread", "Dry run: WRITELAYER deleted.");
    }

    ClearLogCallback();

    {
        QMutexLocker Locker(&wrapperMutex);
        wrapper = nullptr;
    }
    delete LocalWrapper;

    if (!ExecOk)
    {
        emit launchFinished(false, "The game process failed to execute or crashed.");
        return;
    }

    emit launchFinished(true, "");
}

// ============================================================================
// PreLaunchWindow
// ============================================================================

PreLaunchWindow::PreLaunchWindow(
    nlohmann::ordered_json* GlobalConfigJSON,
    nlohmann::ordered_json* MANIFESTJSON,
    const std::string&      PackagePath,
    const std::string&      SubgameID,
    QWidget*                parent)
    : QDialog(parent)
    , GlobalConfigJSON(GlobalConfigJSON)
    , MANIFESTJSON(MANIFESTJSON)
    , PackagePath(PackagePath)
    , SubgameID(SubgameID)
{
    setWindowTitle("Launch");
    setMinimumSize(800, 600);
    setAttribute(Qt::WA_DeleteOnClose);

    // Cache PackageUID for USERSETTINGS lookups.
    if (MANIFESTJSON && MANIFESTJSON->contains("PACKAGEUID") && !(*MANIFESTJSON)["PACKAGEUID"].is_null())
        PackageUID = std::string((*MANIFESTJSON)["PACKAGEUID"]);

    // ----------------------------------------------------------------
    // Top-level layout (horizontal):
    //   Left:  cover art — fixed width, spans full dialog height
    //   Right: QVBoxLayout
    //            VSplitter: controls (top) / console (bottom)
    //            Button row
    // ----------------------------------------------------------------
    QHBoxLayout* RootLayout = new QHBoxLayout(this);
    RootLayout->setContentsMargins(0, 0, 0, 0);
    RootLayout->setSpacing(0);
    setLayout(RootLayout);

    // ----- Left: cover art (full height) -----
    CoverLabel = new QLabel(this);
    CoverLabel->setFixedWidth(200);
    CoverLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    CoverLabel->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    CoverLabel->setContentsMargins(6, 6, 6, 6);
    RootLayout->addWidget(CoverLabel);

    // Load cover from MANIFEST SUBGAMES[idx].METADATA.COVER
    int SubgameIdx = ContainerWrapper::FindGameIndex(*MANIFESTJSON, SubgameID);
    if (SubgameIdx != -1)
    {
        auto &TitleVal = (*MANIFESTJSON)["GAMES"][SubgameIdx]["TITLE"];
        std::string Title = (!TitleVal.is_null() && TitleVal.is_string()) ? std::string(TitleVal) : SubgameID;
        setWindowTitle("Launch " + QString::fromStdString(Title));

        //Cover via CoverCache (dual-form COVER: filename or {PATH,SOURCE:{ipfs,CID}}); local-first, else a
        //background IPFS fetch that fires coverReady so we drop it in when it lands.
        auto &SubgameMeta = (*MANIFESTJSON)["GAMES"][SubgameIdx]["METADATA"];
        const nlohmann::ordered_json * CoverNode = nullptr;
        if (SubgameMeta.is_object() && SubgameMeta.contains("COVER"))            CoverNode = &SubgameMeta["COVER"];
        else if ((*MANIFESTJSON)["GAMES"][SubgameIdx].contains("COVER"))         CoverNode = &(*MANIFESTJSON)["GAMES"][SubgameIdx]["COVER"];
        if (CoverNode)
        {
            auto setFrom = [this](const QString & Path){
                QPixmap Pix(Path);
                if (!Pix.isNull()) CoverLabel->setPixmap(Pix.scaledToWidth(188, Qt::SmoothTransformation));
            };
            const QString Now = CoverCache::instance()->resolve(*CoverNode, QString::fromStdString(PackagePath));
            if (!Now.isEmpty()) setFrom(Now);
            else
            {
                QString F, Cid; CoverCache::Locate(*CoverNode, F, Cid);
                if (!Cid.isEmpty())
                {
                    const nlohmann::ordered_json CoverCopy = *CoverNode;
                    const QString Pkg = QString::fromStdString(PackagePath);
                    connect(CoverCache::instance(), &CoverCache::coverReady, this, [this, Cid, CoverCopy, Pkg, setFrom](QString Ready){
                        if (Ready != Cid) return;
                        const QString P = CoverCache::instance()->resolve(CoverCopy, Pkg);
                        if (!P.isEmpty()) setFrom(P);
                    });
                }
            }
        }
    }

    // ----- Right: VSplitter (controls | console) + button row -----
    QWidget*     RightWidget = new QWidget(this);
    QVBoxLayout* RightLayout = new QVBoxLayout(RightWidget);
    RightLayout->setContentsMargins(0, 0, 0, 0);
    RightWidget->setLayout(RightLayout);
    RootLayout->addWidget(RightWidget, 1);

    QSplitter* VSplitter = new QSplitter(Qt::Vertical, RightWidget);
    RightLayout->addWidget(VSplitter, 1);

    // ----- Controls pane (top of VSplitter) -----
    QWidget*    ControlWidget = new QWidget(VSplitter);
    QVBoxLayout* ControlLayout = new QVBoxLayout(ControlWidget);
    ControlWidget->setLayout(ControlLayout);
    VSplitter->addWidget(ControlWidget);

    QFormLayout* PickerForm = new QFormLayout();
    ControlLayout->addLayout(PickerForm);

    // Runner combobox
    RunnerCombo = new QComboBox(ControlWidget);
    PickerForm->addRow("Runner:", RunnerCombo);

    // Candidate runners = the package's own RUNNERS (bundle) + every GLOBAL_RUNNERS registry runner,
    // filtered by GUEST_PLATFORM ∋ the package's HOST_PLATFORM. Combo userData holds the RUNNER_ID;
    // dedup by RUNNER_ID (the package's own runners shadow registry runners).
    (void)SubgameIdx;
    // The package's target platforms = its game variants' HOST_PLATFORM (per-variant schema), with the legacy
    // top-level HOST_PLATFORM as a fallback. A runner qualifies if any of its variants' GUEST_PLATFORM (or its
    // legacy top-level GUEST_PLATFORM) serves one of them.
    std::set<std::string> GamePlatforms;
    if ((*MANIFESTJSON).contains("HOST_PLATFORM") && (*MANIFESTJSON)["HOST_PLATFORM"].is_string())
        GamePlatforms.insert(std::string((*MANIFESTJSON)["HOST_PLATFORM"]));
    if ((*MANIFESTJSON).contains("GAMES") && (*MANIFESTJSON)["GAMES"].is_array())
        for (const auto &G : (*MANIFESTJSON)["GAMES"])
            if (G.contains("VARIANTS") && G["VARIANTS"].is_array())
                for (const auto &V : G["VARIANTS"])
                    if (V.contains("HOST_PLATFORM") && V["HOST_PLATFORM"].is_string())
                        GamePlatforms.insert(std::string(V["HOST_PLATFORM"]));

    std::set<QString> SeenRunnerIds;
    auto Serves = [&](const nlohmann::ordered_json &Arr) {
        if (!Arr.is_array()) return false;
        for (const auto &P : Arr)
            if (P.is_string() && GamePlatforms.count(std::string(P))) return true;
        return false;
    };
    auto CollectRunner = [&](const nlohmann::ordered_json &Runner)
    {
        // A runner qualifies only if it has a platform-serving variant that can actually run here — a built-in
        // PATH runner whose executable isn't installed on this system is excluded (RunnerWrapper::ExecutableAvailable).
        bool Match = false;
        if (Runner.contains("VARIANTS") && Runner["VARIANTS"].is_array())
        {
            for (const auto &V : Runner["VARIANTS"])                                      // per-variant (current schema)
                if (Serves(V.value("GUEST_PLATFORM", nlohmann::ordered_json()))
                    && RunnerWrapper::ExecutableAvailable(V)) { Match = true; break; }
        }
        else
            Match = Serves(Runner.value("GUEST_PLATFORM", nlohmann::ordered_json()));     // legacy top-level (no variants)
        if (!Match) return;
        QString RID = QString::fromStdString(Runner.value("RUNNER_ID", std::string()));
        if (SeenRunnerIds.count(RID)) return;
        SeenRunnerIds.insert(RID);
        std::string Name = (Runner.contains("NAME") && Runner["NAME"].is_string())
                           ? std::string(Runner["NAME"]) : Runner.value("RUNNER_ID", std::string("(unnamed)"));
        RunnerCombo->addItem(QString::fromStdString(Name), RID);
    };
    if ((*MANIFESTJSON).contains("RUNNERS") && (*MANIFESTJSON)["RUNNERS"].is_array())
        for (auto &R : (*MANIFESTJSON)["RUNNERS"]) CollectRunner(R);
    for (auto &R : ContainerWrapper::RegistryRunners(*GlobalConfigJSON)) CollectRunner(R);

    // Pre-select: variant pin > USERSETTINGS PREFERRED_RUNNER > first. (Helper reads a variant's RUNNER_ID.)
    {
        std::string Pref;
        auto US = ContainerWrapper::GetPackageUserSettings(*GlobalConfigJSON, PackageUID);
        if (US.contains("PREFERRED_RUNNER") && US["PREFERRED_RUNNER"].is_string())
            Pref = std::string(US["PREFERRED_RUNNER"]);
        int Idx = Pref.empty() ? -1 : RunnerCombo->findData(QString::fromStdString(Pref));
        if (Idx >= 0) RunnerCombo->setCurrentIndex(Idx);
    }

    // Variant combobox
    VariantCombo = new QComboBox(ControlWidget);
    PickerForm->addRow("Variant:", VariantCombo);

    // Collect variants via ContainerWrapper helper.
    std::vector<VariantInfo> Variants = ContainerWrapper::GetAvailableVariants(*MANIFESTJSON, SubgameID);
    std::string RecommendedID;
    for (auto &V : Variants)
    {
        QString Label = QString::fromStdString(V.Name.empty() ? V.VariantID : V.Name);
        if (V.IsRecommended) { Label = "⭐ " + Label; RecommendedID = V.VariantID; }
        VariantCombo->addItem(Label, QString::fromStdString(V.VariantID));
    }

    // Pre-select: USERSETTINGS > RECOMMENDED variant > first.
    std::string PreselVariantID = RecommendedID;
    {
        auto US = ContainerWrapper::GetPackageUserSettings(*GlobalConfigJSON, PackageUID);
        if (US.contains("PREFERRED_VARIANT_ID") && US["PREFERRED_VARIANT_ID"].is_string())
            PreselVariantID = std::string(US["PREFERRED_VARIANT_ID"]);
    }

    for (int k = 0; k < VariantCombo->count(); k++)
    {
        if (VariantCombo->itemData(k).toString().toStdString() == PreselVariantID)
        { VariantCombo->setCurrentIndex(k); break; }
    }

    // Only show the variant picker when there's an actual choice (more than one variant).
    VariantLabel = PickerForm->labelForField(VariantCombo);
    {
        bool MultiVariant = VariantCombo->count() > 1;
        VariantCombo->setVisible(MultiVariant);
        if (VariantLabel) VariantLabel->setVisible(MultiVariant);
    }

    // Scrollable section: CustomVar pickers + checkboxes, all in one scroll area.
    QScrollArea * CVScrollArea = new QScrollArea(ControlWidget);
    CVScrollArea->setWidgetResizable(true);
    CVScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    CVScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    CVScrollArea->setFrameShape(QFrame::NoFrame);

    QWidget     * CVContainer     = new QWidget();
    QVBoxLayout * CVContainerLayout = new QVBoxLayout(CVContainer);
    CVContainerLayout->setContentsMargins(0, 0, 0, 0);
    CVContainer->setLayout(CVContainerLayout);
    CVScrollArea->setWidget(CVContainer);
    ControlLayout->addWidget(CVScrollArea);

    // Modules tree: enable/disable optional build modules (REQUIRED ones shown locked-on). Nested by
    // PARENTCOMPONENT — disabling a parent disables+locks its subtree.
    ModuleGroup = new QGroupBox("Modules", CVContainer);
    QVBoxLayout * ModuleLayout = new QVBoxLayout(ModuleGroup);
    ModuleTree = new QTreeWidget(ModuleGroup);
    ModuleTree->setHeaderHidden(true);
    ModuleTree->setRootIsDecorated(true);
    ModuleTree->setSelectionMode(QAbstractItemView::NoSelection);
    ModuleLayout->addWidget(ModuleTree);
    ModuleGroup->setLayout(ModuleLayout);
    ModuleGroup->setVisible(false);
    CVContainerLayout->addWidget(ModuleGroup);
    connect(ModuleTree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* It, int){ PropagateModuleItem(It); });

    CustomVarGroup = new QGroupBox("Options", CVContainer);
    CustomVarForm  = new QFormLayout(CustomVarGroup);
    CustomVarGroup->setLayout(CustomVarForm);
    CustomVarGroup->setVisible(false);
    CVContainerLayout->addWidget(CustomVarGroup);

    NoCleanupCheck     = new QCheckBox("No cleanup (keep mounts after exit)", CVContainer);
    RememberCheck      = new QCheckBox("Hide this dialog next time",          CVContainer);
    CloseAfterLaunchCheck = new QCheckBox("Close window when game starts",    CVContainer);
    DryRunCheck        = new QCheckBox("Dry test run (delete WRITELAYER on cleanup)", CVContainer);
    CVContainerLayout->addWidget(NoCleanupCheck);
    CVContainerLayout->addWidget(RememberCheck);
    CVContainerLayout->addWidget(CloseAfterLaunchCheck);
    CVContainerLayout->addWidget(DryRunCheck);
    CVContainerLayout->addStretch();

    CustomVarGroup->setProperty("ScrollArea", QVariant::fromValue<QWidget*>(CVScrollArea));

    connect(VariantCombo, &QComboBox::currentIndexChanged, this, &PreLaunchWindow::onVariantChanged);
    //The module tree also depends on the selected runner (runner MODULES join the recipe).
    connect(RunnerCombo, &QComboBox::currentIndexChanged, this, [this](int){ RebuildModuleTree(); });
    RebuildCustomVarPickers();
    RebuildModuleTree();

    // Progress bar + status label (outside the scroll area — always visible)
    ProgressBar = new QProgressBar(ControlWidget);
    ProgressBar->setRange(0, 100);
    ProgressBar->setValue(0);
    ProgressBar->setVisible(false);
    ControlLayout->addWidget(ProgressBar);

    StatusLabel = new QLabel(ControlWidget);
    StatusLabel->setText("");
    ControlLayout->addWidget(StatusLabel);

    ControlLayout->addStretch();

    // ---- Console (bottom of vertical splitter) ----
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

    // ---- Button row ----
    QWidget*    BtnWidget = new QWidget(RightWidget);
    QHBoxLayout* BtnLayout = new QHBoxLayout(BtnWidget);
    BtnWidget->setLayout(BtnLayout);
    RightLayout->addWidget(BtnWidget);

    //Package Editor button on the left — opens the editor for this specific package.
    QPushButton * PackageEditorButton = new QPushButton("Package Editor", BtnWidget);
    BtnLayout->addWidget(PackageEditorButton);
    connect(PackageEditorButton, &QPushButton::clicked, this, [this]()
    {
        PackageEditor * Editor = new PackageEditor(this->GlobalConfigJSON, this, QString::fromStdString(this->PackagePath));
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

    // ---- Connections ----
    connect(LaunchButton, &QPushButton::clicked, this, &PreLaunchWindow::onLaunchClicked);
    connect(KillButton,   &QPushButton::clicked, this, &PreLaunchWindow::onKillClicked);
    connect(CloseButton,  &QPushButton::clicked, this, &QDialog::accept);
}

PreLaunchWindow::~PreLaunchWindow()
{
    // If the thread is still running (e.g. window closed while game runs),
    // kill the process and wait for the thread to finish before destroying.
    if (LaunchWorker && LaunchWorker->isRunning())
    {
        LaunchWorker->kill();
        LaunchWorker->wait(5000);
    }
}

// ---------------------------------------------------------------------------
// Slot: Entrypoint selection changed — rebuild CustomVar pickers.
// ---------------------------------------------------------------------------
void PreLaunchWindow::onVariantChanged()
{
    RebuildCustomVarPickers();
    RebuildModuleTree();
}

// ---------------------------------------------------------------------------
// Rebuilds CustomVarGroup based on the currently selected entrypoint.
// Shows pickers only for CustomVars that are (a) referenced via %KEY% in the
// component chain and (b) have DISPLAY != false.
// DISPLAY:false vars are still resolved from the entrypoint seed so they flow
// into VariableOverrides at launch time without a picker.
// ---------------------------------------------------------------------------
void PreLaunchWindow::RebuildCustomVarPickers()
{
    // Clear existing picker rows (each row is now a per-component group box).
    while (CustomVarForm->rowCount() > 0)
        CustomVarForm->removeRow(0);

    if (!MANIFESTJSON) { CustomVarGroup->setVisible(false); return; }

    // Resolve the selected variant's ENDPOINTS.
    std::string SelVariantID;
    if (VariantCombo->currentIndex() >= 0)
        SelVariantID = VariantCombo->currentData().toString().toStdString();

    std::vector<std::string> SelEndpoints = ContainerWrapper::FindEndpointsForVariant(*MANIFESTJSON, SubgameID, SelVariantID);

    // Get the union recipe (all endpoint chains) so CustomVars referenced anywhere are in scope.
    ContainerParams TmpParams("", SubgameID, "");
    TmpParams.Endpoints = SelEndpoints;
    ContainerWrapper::CreateRecipe(*MANIFESTJSON, TmpParams);

    // Serialise the entire subcomponent JSON of every component in the chain
    // into one string so we can quickly scan for %KEY% token usage.
    std::string ChainSubComponentsStr;
    if ((*MANIFESTJSON).contains("COMPONENTS"))
        for (auto &Comp : (*MANIFESTJSON)["COMPONENTS"])
        {
            std::string CID = Comp.value("COMPONENTID", std::string());
            if (std::find(TmpParams.Recipe.begin(), TmpParams.Recipe.end(), CID) == TmpParams.Recipe.end()) continue;
            if (Comp.contains("SUBCOMPONENTS"))
                ChainSubComponentsStr += Comp["SUBCOMPONENTS"].dump();
        }

    // Get entrypoint-level CUSTOMVARS seed.
    nlohmann::ordered_json VariantSeed = nlohmann::ordered_json::object();
    {
        int SubgameIdx = ContainerWrapper::FindGameIndex(*MANIFESTJSON, SubgameID);
        if (SubgameIdx != -1 && (*MANIFESTJSON)["GAMES"][SubgameIdx].contains("VARIANTS"))
            for (auto &V : (*MANIFESTJSON)["GAMES"][SubgameIdx]["VARIANTS"])
                if (V.value("VARIANT_ID", std::string()) == SelVariantID && V.contains("FORCEVARS"))
                    VariantSeed = V["FORCEVARS"];
    }

    // Get persisted USERSETTINGS variables.
    std::string PackageUID_local;
    if (MANIFESTJSON->contains("PACKAGEUID") && !(*MANIFESTJSON)["PACKAGEUID"].is_null())
        PackageUID_local = std::string((*MANIFESTJSON)["PACKAGEUID"]);
    auto US = ContainerWrapper::GetPackageUserSettings(*GlobalConfigJSON, PackageUID_local);
    nlohmann::ordered_json SavedVars = (US.contains("VARIABLES") && US["VARIABLES"].is_object())
                                        ? US["VARIABLES"] : nlohmann::ordered_json::object();

    // Resolve the combo-selected runner variant so its knobs (referenced in the runner's ENV/ARGS) can be
    // surfaced as pickers too — this is how a game seeds env/args to its runner via FORCEVARS. Reuse
    // DeriveContainerParams' runner pick (same GUEST/HOST platform match). Scope to the variant's enabled
    // components (RunnerRecipe) so a monolithic multi-version runner package doesn't surface inactive knobs.
    std::vector<std::pair<std::string, nlohmann::ordered_json>> RunnerPickerComps;
    {
        std::string SelRunnerID;
        if (RunnerCombo && RunnerCombo->currentIndex() >= 0)
            SelRunnerID = RunnerCombo->currentData().toString().toStdString();

        ContainerParams RP("", SubgameID, "");
        RP.VariantID = SelVariantID;
        RP.RunnerID  = SelRunnerID;
        ContainerWrapper::DeriveContainerParams(*MANIFESTJSON, RP, *GlobalConfigJSON);

        // The runner's ENV/ARGS reference its knobs; add them to the chain string so those knobs pass the
        // "is referenced in chain" filter below.
        ChainSubComponentsStr += RP.RunnerEnv.dump();
        for (const auto &A : RP.RunnerArgs) ChainSubComponentsStr += A;

        std::set<std::string> Want(RP.RunnerRecipe.begin(), RP.RunnerRecipe.end());
        for (auto &C : RP.RunnerComponents)
        {
            if (!C.is_object()) continue;
            std::string CID = C.value("COMPONENTID", std::string());
            if (!Want.empty() && !Want.count(CID)) continue;
            if (C.contains("SUBCOMPONENTS") && C["SUBCOMPONENTS"].is_array())
                ChainSubComponentsStr += C["SUBCOMPONENTS"].dump();
            RunnerPickerComps.emplace_back(CID, C);
        }
    }

    bool AnyVisible = false;

    // CustomVars are subcomponents (TYPE:"CustomVar"), bare global %KEY%. Surface each displayable var that
    // is actually referenced elsewhere in the chain. Pickers are grouped into a box per owning component
    // (created lazily, only if it has a var). Both the game recipe and the selected runner variant's enabled
    // components are scanned, so a runner's env/args knobs show up alongside the game's.
    auto BuildPickers = [&](const nlohmann::ordered_json &Comp, const std::string &CompID)
    {
        if (!Comp.contains("SUBCOMPONENTS") || !Comp["SUBCOMPONENTS"].is_array()) return;

        QGroupBox *  CompBox  = nullptr;
        QFormLayout * CompForm = nullptr;
        std::string CompTitle = Comp.contains("NAME") && Comp["NAME"].is_string() && !std::string(Comp["NAME"]).empty()
                                ? std::string(Comp["NAME"]) : CompID;
        auto EnsureCompBox = [&]() {
            if (CompBox) return;
            CompBox  = new QGroupBox(QString::fromStdString(CompTitle), CustomVarGroup);
            CompForm = new QFormLayout(CompBox);
            CompBox->setLayout(CompForm);
        };

        for (auto &CV : Comp["SUBCOMPONENTS"])
        {
        if (!CV.is_object() || CV.value("TYPE", std::string()) != "CustomVar") continue;
        std::string Key = CV.value("KEY", std::string());   // bare global token name
        if (Key.empty()) continue;

        // Only show if this key is actually referenced in the chain.
        std::string Token = "%" + Key + "%";
        if (ChainSubComponentsStr.find(Token) == std::string::npos) continue;

        // Resolve initial value: entrypoint seed → USERSETTINGS → DEFAULT.
        std::string InitialValue = CV.value("DEFAULT", std::string());
        if (SavedVars.contains(Key) && SavedVars[Key].is_string())
            InitialValue = std::string(SavedVars[Key]);
        if (VariantSeed.contains(Key) && VariantSeed[Key].is_string())
            InitialValue = std::string(VariantSeed[Key]);

        bool Display = CV.value("DISPLAY", true);

        if (!Display) continue; // collected directly from entrypoint seed in onLaunchClicked

        // Apply Layer 1 substitution to InitialValue so %ScreenWidth% shows as 1920 in the widget.
        // (Layer 2 / type translation is NOT applied — widget always shows the display value.)
        {
            ContainerParams TmpP("", SubgameID, "");
            ContainerWrapper::StringVariableSubstitution(InitialValue, TmpP.GetVariablesMap());
        }

        AnyVisible = true;
        EnsureCompBox();
        std::string VarType = CV.value("VARTYPE", std::string("string"));
        QString Label       = QString::fromStdString(CV.value("LABEL", Key));

        if (VarType == "random")
        {
            // For display:true random vars, pre-populate with a random pick so the user
            // can see and optionally override it. The widget value flows into VariableOverrides,
            // bypassing the ResolveCustomVariables random-pick path cleanly.
            if (InitialValue.empty() && CV.contains("OPTIONS") && CV["OPTIONS"].is_array() && !CV["OPTIONS"].empty())
            {
                auto &Opts = CV["OPTIONS"];
                static std::mt19937 UiRng(std::random_device{}());
                std::uniform_int_distribution<size_t> D(0, Opts.size() - 1);
                InitialValue = Opts[D(UiRng)].value("VALUE", std::string());
            }
            QLineEdit * Field = new QLineEdit(CustomVarGroup);
            Field->setProperty("CVKey", QString::fromStdString(Key));
            Field->setText(QString::fromStdString(InitialValue));
            CompForm->addRow(Label + ":", Field);
        }
        else if (VarType == "options" && CV.contains("OPTIONS") && CV["OPTIONS"].is_array())
        {
            QComboBox * Combo = new QComboBox(CustomVarGroup);
            Combo->setProperty("CVKey", QString::fromStdString(Key));
            for (auto &Opt : CV["OPTIONS"])
            {
                // Layer 1 on option VALUES too — they may contain tokens.
                std::string OptVal = Opt.value("VALUE", std::string());
                ContainerParams TmpP2("", SubgameID, "");
                ContainerWrapper::StringVariableSubstitution(OptVal, TmpP2.GetVariablesMap());
                QString OptLabel = QString::fromStdString(Opt.value("LABEL", std::string()));
                Combo->addItem(OptLabel, QString::fromStdString(OptVal));
            }
            for (int k = 0; k < Combo->count(); k++)
                if (Combo->itemData(k).toString().toStdString() == InitialValue)
                { Combo->setCurrentIndex(k); break; }
            CompForm->addRow(Label + ":", Combo);
        }
        else if (VarType == "dword" || VarType == "qword" || VarType == "number")
        {
            QSpinBox * Spin = new QSpinBox(CustomVarGroup);
            Spin->setProperty("CVKey", QString::fromStdString(Key));
            Spin->setMinimum(0);
            // dword caps at 2^32-1; for QSpinBox (int) use INT_MAX. qword/number use INT_MAX too.
            Spin->setMaximum(VarType == "dword" ? 2147483647 : 2147483647);
            try { Spin->setValue(std::stoi(InitialValue)); } catch (...) { Spin->setValue(0); }
            CompForm->addRow(Label + ":", Spin);
        }
        else if (VarType == "bool")
        {
            QCheckBox * Check = new QCheckBox(CustomVarGroup);
            Check->setProperty("CVKey", QString::fromStdString(Key));
            std::string lower = InitialValue;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            Check->setChecked(lower == "1" || lower == "true" || lower == "yes");
            CompForm->addRow(Label + ":", Check);
        }
        else // string or unknown
        {
            QLineEdit * Field = new QLineEdit(CustomVarGroup);
            Field->setProperty("CVKey", QString::fromStdString(Key));
            Field->setText(QString::fromStdString(InitialValue));
            CompForm->addRow(Label + ":", Field);
        }
        } // for each CustomVar subcomponent

        // Add the component's box only if it ended up with at least one visible var.
        if (CompBox) CustomVarForm->addRow(CompBox);
    }; // BuildPickers

    // Game recipe components first, then the selected runner variant's enabled components.
    for (const std::string &CompID : TmpParams.Recipe)
    {
        int CIdx = ContainerWrapper::FindComponentIndex(*MANIFESTJSON, CompID);
        if (CIdx == -1) continue;
        BuildPickers((*MANIFESTJSON)["COMPONENTS"][CIdx], CompID);
    }
    for (const auto &[RCompID, RComp] : RunnerPickerComps)
        BuildPickers(RComp, RCompID);

    CustomVarGroup->setVisible(AnyVisible);
}

// ---------------------------------------------------------------------------
// Module tree: the selected variant's + runner's MODULES, nested by PARENTCOMPONENT, as toggle boxes.
// REQUIRED modules (and the ancestors a required module forces on) are shown locked-on/greyed.
// ---------------------------------------------------------------------------
void PreLaunchWindow::RebuildModuleTree()
{
    if (!ModuleTree || !MANIFESTJSON) { if (ModuleGroup) ModuleGroup->setVisible(false); return; }
    ModuleTree->blockSignals(true);
    ModuleTree->clear();

    const std::string VariantID = VariantCombo ? VariantCombo->currentData().toString().toStdString() : std::string();

    // Universal MODULES: variant's + the selected runner's, deduped by component (first wins).
    std::vector<ModuleInfo> Modules = ContainerWrapper::GetVariantModules(*MANIFESTJSON, SubgameID, VariantID);
    if (RunnerCombo && RunnerCombo->currentIndex() >= 0 && RunnerCombo->currentIndex() < (int)Runners.size())
        for (auto &M : ContainerWrapper::ParseModules(Runners[RunnerCombo->currentIndex()].second.value("MODULES", nlohmann::ordered_json::array())))
            Modules.push_back(M);
    {
        std::set<std::string> Seen; std::vector<ModuleInfo> Uniq;
        for (auto &M : Modules) if (Seen.insert(M.Component).second) Uniq.push_back(M);
        Modules.swap(Uniq);
    }
    if (Modules.empty()) { ModuleGroup->setVisible(false); ModuleTree->blockSignals(false); return; }

    std::map<std::string, ModuleInfo> ByComp;
    for (auto &M : Modules) ByComp[M.Component] = M;

    auto ParentComponentOf = [&](const std::string &Comp) -> std::string {
        if (!MANIFESTJSON->contains("COMPONENTS")) return "";
        for (const auto &C : (*MANIFESTJSON)["COMPONENTS"])
            if (C.value("COMPONENTID", std::string()) == Comp)
            {
                const auto &PF = C.contains("PARENTCOMPONENT") ? C["PARENTCOMPONENT"] : nlohmann::ordered_json();
                return PF.is_string() ? std::string(PF) : std::string();
            }
        return "";
    };
    auto NearestModuleAncestor = [&](const std::string &Comp) -> std::string {
        for (std::string C = ParentComponentOf(Comp); !C.empty(); C = ParentComponentOf(C))
            if (ByComp.count(C)) return C;
        return "";
    };

    // Locked-on set: required modules + (REQUIRED-propagates-up) all their module-ancestors.
    std::set<std::string> LockedOn;
    for (auto &M : Modules)
        if (M.Required)
        {
            LockedOn.insert(M.Component);
            for (std::string C = NearestModuleAncestor(M.Component); !C.empty(); C = NearestModuleAncestor(C))
                LockedOn.insert(C);
        }
    LockedModules = LockedOn; // hidden, always-on components — a toggle excluding one of these is rejected

    // Symmetric mutual-exclusion adjacency over ALL modules (incl. hidden), for PropagateModuleItem.
    ModuleExcludes.clear();
    for (auto &M : Modules)
        for (auto &E : M.Exclude) { ModuleExcludes[M.Component].insert(E); ModuleExcludes[E].insert(M.Component); }

    // Nearest module-ancestor that is itself visible (not hidden/locked) — for re-parenting under hiding.
    auto NearestVisibleAncestor = [&](const std::string &Comp) -> std::string {
        for (std::string C = NearestModuleAncestor(Comp); !C.empty(); C = NearestModuleAncestor(C))
            if (!LockedOn.count(C)) return C;
        return "";
    };

    auto US = ContainerWrapper::GetPackageUserSettings(*GlobalConfigJSON, PackageUID);
    const nlohmann::ordered_json Saved = (US.contains("MODULES") && US["MODULES"].is_object()) ? US["MODULES"] : nlohmann::ordered_json::object();

    // Build items only for genuinely toggleable (non-locked) modules — REQUIRED / required-forced ones are
    // hidden since the user can't change them. Initial state is normalized so no two mutually-exclusive
    // modules start enabled together (first-declared wins).
    std::map<std::string, QTreeWidgetItem*> Items;
    std::set<std::string> InitiallyOn;
    for (auto &M : Modules)
    {
        if (LockedOn.count(M.Component)) continue; // non-toggleable → hidden
        bool Desired = (Saved.contains(M.Component) && Saved[M.Component].is_boolean()) ? (bool)Saved[M.Component] : M.Default;
        if (Desired)
        {
            bool Conflict = false;
            auto ai = ModuleExcludes.find(M.Component);
            if (ai != ModuleExcludes.end())
                for (const auto &On : InitiallyOn) if (ai->second.count(On)) { Conflict = true; break; }
            if (Conflict) Desired = false; else InitiallyOn.insert(M.Component);
        }
        QTreeWidgetItem* It = new QTreeWidgetItem();
        It->setText(0, QString::fromStdString(M.Label.empty() ? M.Component : M.Label));
        It->setData(0, Qt::UserRole,     QString::fromStdString(M.Component));
        It->setData(0, Qt::UserRole + 1, false);   // visible items are always optional/toggleable
        It->setData(0, Qt::UserRole + 2, Desired);
        Items[M.Component] = It;
    }
    if (Items.empty()) { ModuleGroup->setVisible(false); ModuleTree->blockSignals(false); return; }
    for (auto &M : Modules)
    {
        if (!Items.count(M.Component)) continue; // hidden
        std::string P = NearestVisibleAncestor(M.Component);
        if (!P.empty() && Items.count(P)) Items[P]->addChild(Items[M.Component]);
        else                              ModuleTree->addTopLevelItem(Items[M.Component]);
    }
    ModuleTree->expandAll();
    ModuleGroup->setVisible(true);
    ModuleTree->blockSignals(false);
    RefreshModuleLocks();
}

void PreLaunchWindow::RefreshModuleLocks()
{
    if (!ModuleTree) return;
    ModuleTree->blockSignals(true);
    std::function<void(QTreeWidgetItem*, bool)> Apply = [&](QTreeWidgetItem* It, bool ParentEnabled)
    {
        bool Required  = It->data(0, Qt::UserRole + 1).toBool();
        bool Desired   = It->data(0, Qt::UserRole + 2).toBool();
        bool Effective = Required ? true : (ParentEnabled && Desired);
        bool Locked    = Required || !ParentEnabled;
        It->setFlags(Locked ? Qt::ItemFlags(Qt::ItemIsEnabled)
                            : Qt::ItemFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable));
        It->setCheckState(0, Effective ? Qt::Checked : Qt::Unchecked);
        It->setForeground(0, Locked ? QBrush(Qt::gray) : QBrush());
        It->setData(0, Qt::UserRole + 3, Locked);
        for (int i = 0; i < It->childCount(); ++i) Apply(It->child(i), Effective);
    };
    for (int i = 0; i < ModuleTree->topLevelItemCount(); ++i) Apply(ModuleTree->topLevelItem(i), true);
    ModuleTree->blockSignals(false);
}

void PreLaunchWindow::PropagateModuleItem(QTreeWidgetItem* Item)
{
    if (!Item) return;
    if (Item->data(0, Qt::UserRole + 3).toBool()) { RefreshModuleLocks(); return; } // locked: snapped back by refresh

    const bool NowChecked = Item->checkState(0) == Qt::Checked;
    const std::string Comp = Item->data(0, Qt::UserRole).toString().toStdString();
    auto ai = ModuleExcludes.find(Comp);
    const bool HasExcl = (ai != ModuleExcludes.end());

    // Enabling a module that is mutually exclusive with a hidden always-on (REQUIRED-forced) module can't
    // win — that module can't yield. Warn and revert.
    if (NowChecked && HasExcl)
        for (const auto &E : ai->second)
            if (LockedModules.count(E))
            {
                QMessageBox::warning(this, "Mutually exclusive module",
                    QString::fromStdString("This option is mutually exclusive with an always-on module ('" + E +
                                           "'), so it can't be enabled."));
                ModuleTree->blockSignals(true);
                Item->setCheckState(0, Qt::Unchecked);
                ModuleTree->blockSignals(false);
                Item->setData(0, Qt::UserRole + 2, false);
                RefreshModuleLocks();
                return;
            }

    //Record the user's intent for this unlocked item.
    Item->setData(0, Qt::UserRole + 2, NowChecked);

    // At most one: ticking a module unticks every visible module it excludes (optional siblings yield).
    if (NowChecked && HasExcl)
    {
        std::function<void(QTreeWidgetItem*)> Walk = [&](QTreeWidgetItem* It) {
            std::string C = It->data(0, Qt::UserRole).toString().toStdString();
            if (It != Item && ai->second.count(C)) It->setData(0, Qt::UserRole + 2, false);
            for (int i = 0; i < It->childCount(); ++i) Walk(It->child(i));
        };
        for (int i = 0; i < ModuleTree->topLevelItemCount(); ++i) Walk(ModuleTree->topLevelItem(i));
    }
    RefreshModuleLocks();
}

std::map<std::string, bool> PreLaunchWindow::CollectModuleStates() const
{
    std::map<std::string, bool> States;
    if (!ModuleTree) return States;
    std::function<void(QTreeWidgetItem*)> Walk = [&](QTreeWidgetItem* It)
    {
        //Only optional modules carry a meaningful toggle; REQUIRED are always-on and not stored.
        if (!It->data(0, Qt::UserRole + 1).toBool())
            States[It->data(0, Qt::UserRole).toString().toStdString()] = It->data(0, Qt::UserRole + 2).toBool();
        for (int i = 0; i < It->childCount(); ++i) Walk(It->child(i));
    };
    for (int i = 0; i < ModuleTree->topLevelItemCount(); ++i) Walk(ModuleTree->topLevelItem(i));
    return States;
}

// ---------------------------------------------------------------------------
// Re-assemble the (just-edited) manifest from disk and rebuild the dynamic pickers.
// ---------------------------------------------------------------------------
void PreLaunchWindow::ReloadAndRebuild()
{
    if (!MANIFESTJSON) return;
    // Re-assemble in place into the shared pointer (the card that owns it sees the fresh data too).
    std::vector<std::string> Warn;
    JSONOps::AssembleManifest(QString::fromStdString(PackagePath), *MANIFESTJSON, Warn);
    PackageUID = MANIFESTJSON->value("PACKAGEUID", PackageUID);

    // Repopulate the variant combo (preserve the current selection if it still exists).
    if (VariantCombo)
    {
        QString Cur = VariantCombo->currentData().toString();
        QSignalBlocker B(VariantCombo);
        VariantCombo->clear();
        std::string RecommendedID;
        for (auto & V : ContainerWrapper::GetAvailableVariants(*MANIFESTJSON, SubgameID))
        {
            QString Label = QString::fromStdString(V.Name.empty() ? V.VariantID : V.Name);
            if (V.IsRecommended) { Label = "⭐ " + Label; RecommendedID = V.VariantID; }
            VariantCombo->addItem(Label, QString::fromStdString(V.VariantID));
        }
        int Idx = VariantCombo->findData(Cur);
        if (Idx < 0 && !RecommendedID.empty()) Idx = VariantCombo->findData(QString::fromStdString(RecommendedID));
        if (Idx >= 0) VariantCombo->setCurrentIndex(Idx);

        // Show the picker only when there's a real choice.
        bool MultiVariant = VariantCombo->count() > 1;
        VariantCombo->setVisible(MultiVariant);
        if (VariantLabel) VariantLabel->setVisible(MultiVariant);
    }

    // Rebuild the dependent sections from the fresh manifest.
    RebuildCustomVarPickers();
    RebuildModuleTree();
}

// ---------------------------------------------------------------------------
// Slot: Launch button clicked
// ---------------------------------------------------------------------------
void PreLaunchWindow::onLaunchClicked()
{
    // Resolve the chosen variant. Endpoint resolution + recipe happen in the worker
    // (DecideComponent/CreateRecipe) once VariantID is set on the params.
    std::string SelectedVariantID;
    if (VariantCombo->currentIndex() >= 0)
        SelectedVariantID = VariantCombo->currentData().toString().toStdString();

    // Fallback: first available variant.
    if (SelectedVariantID.empty())
    {
        auto Variants = ContainerWrapper::GetAvailableVariants(*MANIFESTJSON, SubgameID);
        if (!Variants.empty())
            SelectedVariantID = Variants.front().VariantID;
    }

    // Resolve the selected runner by RUNNER_ID (combo userData).
    std::string SelectedRunnerID;
    if (RunnerCombo->currentIndex() >= 0)
        SelectedRunnerID = RunnerCombo->currentData().toString().toStdString();

    // Seed CollectedVars with every CustomVar's resolved value:
    // 1. Start from entrypoint FORCEVARS (covers DISPLAY:false vars automatically).
    // 2. Override with visible picker selections (covers DISPLAY:true vars).
    std::map<std::string, std::string> CollectedVars;

    // Step 1: variant FORCEVARS seed (keys are bare global %KEY%). Everything not seeded
    // or picked is left to the container to resolve (DEFAULT / random / USERSETTINGS).
    if (MANIFESTJSON)
    {
        int SubgameIdx2 = ContainerWrapper::FindGameIndex(*MANIFESTJSON, SubgameID);
        if (SubgameIdx2 != -1 && (*MANIFESTJSON)["GAMES"][SubgameIdx2].contains("VARIANTS"))
            for (auto &Var : (*MANIFESTJSON)["GAMES"][SubgameIdx2]["VARIANTS"])
                if (Var.value("VARIANT_ID", std::string()) == SelectedVariantID && Var.contains("FORCEVARS"))
                    for (auto &[K, V] : Var["FORCEVARS"].items())
                        if (V.is_string()) CollectedVars[K] = std::string(V);
    }

    // Step 2: visible picker widgets (DISPLAY:true) override. Collected via findChildren since the
    // pickers are nested in per-component group boxes. Each widget carries its bare CVKey.
    std::map<std::string, std::string> PickerVars;
    for (QWidget * W : CustomVarGroup->findChildren<QWidget*>())
    {
        QString Key = W->property("CVKey").toString();
        if (Key.isEmpty()) continue;
        std::string Val;
        if (auto * CB  = qobject_cast<QComboBox*>(W))      Val = CB->currentData().toString().toStdString();
        else if (auto * SP = qobject_cast<QSpinBox*>(W))   Val = std::to_string(SP->value());
        else if (auto * CK = qobject_cast<QCheckBox*>(W))  Val = CK->isChecked() ? "1" : "0";
        else if (auto * LE = qobject_cast<QLineEdit*>(W))  Val = LE->text().toStdString();
        else continue;
        PickerVars[Key.toStdString()] = Val;
        CollectedVars[Key.toStdString()] = Val;
    }

    // Auto-save the displayed picker values to USERSETTINGS["VARIABLES"] (bare keys) so they
    // pre-fill on the next launch, taking precedence over DEFAULT and random picks.
    if (!PackageUID.empty() && !PickerVars.empty())
    {
        auto US = ContainerWrapper::GetPackageUserSettings(*GlobalConfigJSON, PackageUID);
        nlohmann::ordered_json Vars = (US.contains("VARIABLES") && US["VARIABLES"].is_object())
                                       ? US["VARIABLES"] : nlohmann::ordered_json::object();
        for (const auto &[K, V] : PickerVars) Vars[K] = V;
        ContainerWrapper::SetPackageUserSetting(*GlobalConfigJSON, PackageUID, "VARIABLES", Vars);
        QDir AppDataDir(QDir::homePath() + "/.VidyaGod");
        JSONOps::SaveJSON(GlobalConfigJSON, new QFile(AppDataDir.filePath("GlobalConfig.JSON")));
    }

    // Persist the optional-module toggles to USERSETTINGS["MODULES"] (component → enabled) so they
    // pre-tick next launch. (REQUIRED modules are always-on and not stored.)
    if (!PackageUID.empty())
    {
        nlohmann::ordered_json Mods = nlohmann::ordered_json::object();
        for (const auto &[Comp, On] : CollectModuleStates()) Mods[Comp] = On;
        ContainerWrapper::SetPackageUserSetting(*GlobalConfigJSON, PackageUID, "MODULES", Mods);
        QDir AppDataDir(QDir::homePath() + "/.VidyaGod");
        JSONOps::SaveJSON(GlobalConfigJSON, new QFile(AppDataDir.filePath("GlobalConfig.JSON")));
    }

    // Always persist runner and entrypoint so next launch pre-selects them.
    if (!PackageUID.empty())
    {
        ContainerWrapper::SetPackageUserSetting(*GlobalConfigJSON, PackageUID, "PREFERRED_RUNNER",     SelectedRunnerID);
        ContainerWrapper::SetPackageUserSetting(*GlobalConfigJSON, PackageUID, "PREFERRED_VARIANT_ID", SelectedVariantID);
        // "Hide dialog next time" only when explicitly checked.
        if (RememberCheck->isChecked())
            ContainerWrapper::SetPackageUserSetting(*GlobalConfigJSON, PackageUID, "SKIP_LAUNCH_DIALOG", true);
        QDir AppDataDir(QDir::homePath() + "/.VidyaGod");
        JSONOps::SaveJSON(GlobalConfigJSON, new QFile(AppDataDir.filePath("GlobalConfig.JSON")));
    }

    // Disable pickers and launch button; show progress bar.
    RunnerCombo->setEnabled(false);
    VariantCombo->setEnabled(false);
    CustomVarGroup->setEnabled(false);
    NoCleanupCheck->setEnabled(false);
    RememberCheck->setEnabled(false);
    CloseAfterLaunchCheck->setEnabled(false);
    DryRunCheck->setEnabled(false);
    LaunchButton->setEnabled(false);
    CloseButton->setEnabled(false);
    ProgressBar->setValue(0);
    ProgressBar->setVisible(true);
    StatusLabel->setText("Starting...");

    // Build and start the worker thread.
    LaunchWorker = new LaunchThread();
    LaunchWorker->GlobalConfigJSON = *GlobalConfigJSON;
    LaunchWorker->MANIFESTJSON     = *MANIFESTJSON;
    LaunchWorker->PackagePath      = PackagePath;
    LaunchWorker->SubgameID        = SubgameID;
    LaunchWorker->ComponentID      = "";   // variant-driven; endpoints resolved in the worker
    LaunchWorker->VariantID          = SelectedVariantID;
    LaunchWorker->VariableOverrides  = CollectedVars;
    LaunchWorker->ModuleStates       = CollectModuleStates();
    LaunchWorker->RunnerID           = SelectedRunnerID;
    LaunchWorker->SkipCleanup      = NoCleanupCheck->isChecked();
    LaunchWorker->DryRun           = DryRunCheck->isChecked();
    //Capture the display geometry HERE, on the main thread. The worker must never touch
    //QGuiApplication (screen access off the GUI thread is undefined behaviour and can crash).
    if (QScreen * Scr = QGuiApplication::primaryScreen())
    {
        LaunchWorker->ScreenWidth  = std::to_string(Scr->geometry().width());
        LaunchWorker->ScreenHeight = std::to_string(Scr->geometry().height());
    }

    connect(LaunchWorker, &LaunchThread::logLine,        this, &PreLaunchWindow::onLogLine,        Qt::QueuedConnection);
    connect(LaunchWorker, &LaunchThread::statusChanged,  this, &PreLaunchWindow::onStatusChanged,  Qt::QueuedConnection);
    connect(LaunchWorker, &LaunchThread::progressChanged,this, &PreLaunchWindow::onProgressChanged,Qt::QueuedConnection);
    connect(LaunchWorker, &LaunchThread::launchFinished, this, &PreLaunchWindow::onLaunchFinished, Qt::QueuedConnection);

    LaunchWorker->start();
}

// ---------------------------------------------------------------------------
// Slot: Kill button clicked
// ---------------------------------------------------------------------------
void PreLaunchWindow::onKillClicked()
{
    if (LaunchWorker)
        LaunchWorker->kill();
}

// ---------------------------------------------------------------------------
// Slot: log line received from worker thread
// ---------------------------------------------------------------------------
void PreLaunchWindow::onLogLine(int level, QString context, QString message)
{
    // Color map: ERR=red, WAR=yellow, SUC=green, OUT=no color
    QString color;
    switch (static_cast<LogLevel>(level))
    {
        case LogLevel::ERR:  color = "#cc0000"; break;
        case LogLevel::WARN: color = "#cccc00"; break;
        case LogLevel::SUCC: color = "#00cc00"; break;
        default:             color = "";        break;
    }

    // Build HTML line: escape special chars then wrap in color span if needed.
    QString Text = context.toHtmlEscaped() + " " + message.toHtmlEscaped();
    QString Html;
    if (!color.isEmpty())
        Html = QString("<span style=\"color:%1\">%2</span>").arg(color, Text);
    else
        Html = Text;

    ConsoleEdit->append(Html);

    // Auto-scroll to bottom.
    QScrollBar* SB = ConsoleEdit->verticalScrollBar();
    SB->setValue(SB->maximum());
}

// ---------------------------------------------------------------------------
// Slot: status text changed
// ---------------------------------------------------------------------------
void PreLaunchWindow::onStatusChanged(QString status)
{
    StatusLabel->setText(status);
}

// ---------------------------------------------------------------------------
// Slot: progress value changed
// ---------------------------------------------------------------------------
void PreLaunchWindow::onProgressChanged(int value)
{
    ProgressBar->setValue(value);

    // Enable Kill button once the game is actually running (progress >= 95).
    if (value >= 95)
    {
        KillButton->setEnabled(true);
        if (CloseAfterLaunchCheck->isChecked())
            accept();
    }
}

// ---------------------------------------------------------------------------
// Slot: worker thread finished
// ---------------------------------------------------------------------------
void PreLaunchWindow::onLaunchFinished(bool success, QString errorMsg)
{
    KillButton->setEnabled(false);
    CloseButton->setEnabled(true);

    if (success)
        StatusLabel->setText("Finished.");
    else
        StatusLabel->setText("Error: " + errorMsg);

    // Reset controls so the user can launch again.
    RunnerCombo->setEnabled(true);
    VariantCombo->setEnabled(true);
    CustomVarGroup->setEnabled(true);
    NoCleanupCheck->setEnabled(true);
    RememberCheck->setEnabled(true);
    CloseAfterLaunchCheck->setEnabled(true);
    DryRunCheck->setEnabled(true);
    LaunchButton->setEnabled(true);
    ProgressBar->setValue(0);
    ProgressBar->setVisible(false);
}

// ---------------------------------------------------------------------------
// Save user preferences to GlobalConfigJSON and flush to disk.
// ---------------------------------------------------------------------------
void PreLaunchWindow::savePreferences(const std::string& runnerName,
                                      const std::string& variantID,
                                      bool               skipNext)
{
    if (PackageUID.empty()) return;

    ContainerWrapper::SetPackageUserSetting(*GlobalConfigJSON, PackageUID, "PREFERRED_RUNNER",    runnerName);
    ContainerWrapper::SetPackageUserSetting(*GlobalConfigJSON, PackageUID, "PREFERRED_VARIANT_ID", variantID);
    ContainerWrapper::SetPackageUserSetting(*GlobalConfigJSON, PackageUID, "SKIP_LAUNCH_DIALOG",   skipNext);

    // Write to ~/.VidyaGod/GlobalConfig.JSON.
    QDir AppDataDir(QDir::homePath() + "/.VidyaGod");
    JSONOps::SaveJSON(GlobalConfigJSON, new QFile(AppDataDir.filePath("GlobalConfig.JSON")));
}
