#include "prelaunchwindow.h"
#include "packageeditor.h"

#include <QDir>
#include <QPixmap>
#include <QFont>
#include <QGuiApplication>
#include <QApplication>
#include <QScrollBar>

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
        else if (ctx.find("MergeRegPatchFiles") != std::string::npos)
        {
            if (msg.find("RegPatch32") != std::string::npos)
            {
                emit progressChanged(35);
                emit statusChanged("Applying registry patches...");
            }
            else if (msg.find("RegPatch64") != std::string::npos)
            {
                emit progressChanged(45);
            }
        }
        else if (ctx.find("PreMountFilesystemComponents") != std::string::npos &&
                 msg.find("Mounting") != std::string::npos)
        {
            // Extract a short layer name from the message for the status label.
            // Messages look like: "Mounting VFSZipLayer /path/to/archive.zip at /tmp/..."
            QString Layer = QString::fromStdString(msg);
            int SlashPos = Layer.lastIndexOf('/');
            if (SlashPos >= 0)
                Layer = Layer.mid(SlashPos + 1).split(' ').first();
            emit progressChanged(55);
            emit statusChanged("Mounting " + Layer);
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

    ContainerWrapper* LocalWrapper = new ContainerWrapper(GlobalConfigJSON, MANIFESTJSON, Params);

    // Store wrapper pointer so kill() can reach it.
    {
        QMutexLocker Locker(&wrapperMutex);
        wrapper = LocalWrapper;
    }

    // Apply runner override if one was selected by the user.
    if (!SelectedRunner.is_null() && !SelectedRunner.empty())
    {
        LocalWrapper->ContainerParams.RunnerName       = SelectedRunner.value("NAME",      std::string());
        LocalWrapper->ContainerParams.RunnerExecutable = SelectedRunner.value("EXECUTABLE", std::string());
        std::string TypeStr                            = SelectedRunner.value("TYPE",       std::string("wine"));
        if      (TypeStr == "wine")      LocalWrapper->ContainerParams.RunnerTypeEnum = RunnerType::Wine;
        else if (TypeStr == "emulator")  LocalWrapper->ContainerParams.RunnerTypeEnum = RunnerType::Emulator;
        else if (TypeStr == "custom")    LocalWrapper->ContainerParams.RunnerTypeEnum = RunnerType::Custom;
        else                             LocalWrapper->ContainerParams.RunnerTypeEnum = RunnerType::Native;

        LocalWrapper->ContainerParams.RunnerEnv = SelectedRunner.contains("ENV")
            ? SelectedRunner["ENV"] : nlohmann::ordered_json::object();
        LocalWrapper->ContainerParams.RunnerRemoveEnv.clear();
        LocalWrapper->ContainerParams.RunnerArgs.clear();
        if (SelectedRunner.contains("REMOVE_ENV"))
            for (auto &E : SelectedRunner["REMOVE_ENV"])
                LocalWrapper->ContainerParams.RunnerRemoveEnv.push_back(std::string(E));
        if (SelectedRunner.contains("ARGS"))
            for (auto &A : SelectedRunner["ARGS"])
                LocalWrapper->ContainerParams.RunnerArgs.push_back(std::string(A));

        // Re-derive path layout when runner type was changed.
        if (LocalWrapper->ContainerParams.RunnerTypeEnum == RunnerType::Wine)
        {
            LocalWrapper->ContainerParams.ProgramPath =
                LocalWrapper->ContainerParams.RuntimePath / "drive_c" / LocalWrapper->ContainerParams.PackageUID;
            LocalWrapper->ContainerParams.DefPrefixPath =
                LocalWrapper->ContainerParams.TempPath / "DEFPREFIX";
            LocalWrapper->ContainerParams.WindowsProgramPath =
                "C:\\" + LocalWrapper->ContainerParams.PackageUID;
            LocalWrapper->ContainerParams.WindowsProgramPathDoubleBackSlash =
                "C:\\\\" + LocalWrapper->ContainerParams.PackageUID;
            LocalWrapper->ContainerParams.WorkDirPathComplete =
                LocalWrapper->ContainerParams.ProgramPath;
        }
        else
        {
            LocalWrapper->ContainerParams.ProgramPath =
                LocalWrapper->ContainerParams.RuntimePath;
            LocalWrapper->ContainerParams.WorkDirPathComplete =
                LocalWrapper->ContainerParams.ProgramPath;
        }
    }

    // Override ExecutableID with the user-selected entrypoint.
    if (!this->EntrypointID.empty())
        LocalWrapper->ContainerParams.ExecutableID = this->EntrypointID;

    // -----------------------------------------------------------------
    // Step 1: Resolve executable definition.
    // -----------------------------------------------------------------
    if (!ContainerWrapper::ResolveExecutableDefinition(MANIFESTJSON, LocalWrapper->ContainerParams))
    {
        ClearLogCallback();
        {
            QMutexLocker Locker(&wrapperMutex);
            wrapper = nullptr;
        }
        delete LocalWrapper;
        emit launchFinished(false, "Could not resolve entrypoint.\nCheck ENTRYPOINT_ID and LASTCOMPONENT in the manifest.");
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
    if (!SkipCleanup)
        LocalWrapper->Cleanup();

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
    // Top-level layout:
    //   QSplitter (top half)
    //     Left pane:  cover art
    //     Right pane: runner/variant pickers + progress + checkboxes
    //   Console (bottom, inside the splitter)
    //   Button row
    // ----------------------------------------------------------------
    QVBoxLayout* RootLayout = new QVBoxLayout(this);
    setLayout(RootLayout);

    // Vertical splitter: top section / console.
    QSplitter* VSplitter = new QSplitter(Qt::Vertical, this);
    RootLayout->addWidget(VSplitter, 1);

    // ---- Top section (horizontal splitter: cover | controls) ----
    QSplitter* HSplitter = new QSplitter(Qt::Horizontal, VSplitter);
    VSplitter->addWidget(HSplitter);

    // ----- Left pane: cover art -----
    QWidget*    CoverWidget = new QWidget(HSplitter);
    QVBoxLayout* CoverLayout = new QVBoxLayout(CoverWidget);
    CoverLayout->setContentsMargins(4, 4, 4, 4);
    CoverWidget->setLayout(CoverLayout);
    CoverWidget->setFixedWidth(210);

    CoverLabel = new QLabel(CoverWidget);
    CoverLabel->setAlignment(Qt::AlignCenter);
    CoverLabel->setScaledContents(false);
    CoverLabel->setMinimumHeight(280);
    CoverLayout->addWidget(CoverLabel);
    CoverLayout->addStretch();
    HSplitter->addWidget(CoverWidget);

    // Load cover from MANIFEST SUBGAMES[idx].METADATA.COVER
    int SubgameIdx = ContainerWrapper::FindSubgameIndex(*MANIFESTJSON, SubgameID);
    if (SubgameIdx != -1)
    {
        auto &TitleVal = (*MANIFESTJSON)["SUBGAMES"][SubgameIdx]["TITLE"];
        std::string Title = (!TitleVal.is_null() && TitleVal.is_string()) ? std::string(TitleVal) : SubgameID;
        setWindowTitle("Launch " + QString::fromStdString(Title));

        auto &SubgameMeta = (*MANIFESTJSON)["SUBGAMES"][SubgameIdx]["METADATA"];
        std::string CoverFile =
            (SubgameMeta.is_object() && SubgameMeta.contains("COVER") && SubgameMeta["COVER"].is_string())
            ? std::string(SubgameMeta["COVER"])
            : ((*MANIFESTJSON)["SUBGAMES"][SubgameIdx].contains("COVER")
               ? std::string((*MANIFESTJSON)["SUBGAMES"][SubgameIdx]["COVER"])
               : "");
        if (!CoverFile.empty())
        {
            QPixmap Pix(QDir::cleanPath(
                QString::fromStdString(PackagePath) + QDir::separator() +
                "METADATA" + QDir::separator() +
                QString::fromStdString(CoverFile)));
            if (!Pix.isNull())
                CoverLabel->setPixmap(Pix.scaledToWidth(200, Qt::SmoothTransformation));
        }
    }

    // ----- Right pane: pickers + progress + checkboxes -----
    QWidget*    ControlWidget = new QWidget(HSplitter);
    QVBoxLayout* ControlLayout = new QVBoxLayout(ControlWidget);
    ControlWidget->setLayout(ControlLayout);
    HSplitter->addWidget(ControlWidget);
    HSplitter->setStretchFactor(0, 0);
    HSplitter->setStretchFactor(1, 1);

    QFormLayout* PickerForm = new QFormLayout();
    ControlLayout->addLayout(PickerForm);

    // Runner combobox
    RunnerCombo = new QComboBox(ControlWidget);
    PickerForm->addRow("Runner:", RunnerCombo);

    // Collect runners from GlobalConfig and MANIFEST.
    std::string Platform = "Microsoft Windows";
    if (SubgameIdx != -1 && (*MANIFESTJSON)["SUBGAMES"][SubgameIdx].contains("PLATFORM") &&
        !(*MANIFESTJSON)["SUBGAMES"][SubgameIdx]["PLATFORM"].is_null())
        Platform = std::string((*MANIFESTJSON)["SUBGAMES"][SubgameIdx]["PLATFORM"]);

    auto CollectRunners = [&](const nlohmann::ordered_json &Source)
    {
        if (!Source.contains("RUNNERS")) return;
        if (!Source["RUNNERS"].contains(Platform)) return;
        for (auto &Runner : Source["RUNNERS"][Platform])
        {
            //Use explicit null check — .value() throws if key exists but value is JSON null.
            std::string Name = (Runner.contains("NAME") && Runner["NAME"].is_string())
                               ? std::string(Runner["NAME"]) : "(unnamed)";
            QString Label = QString::fromStdString(Name);
            Runners.push_back({Label, Runner});
            RunnerCombo->addItem(Label);
        }
    };
    CollectRunners(*GlobalConfigJSON);
    CollectRunners(*MANIFESTJSON);

    // Pre-select preferred runner from USERSETTINGS if stored.
    {
        auto US = ContainerWrapper::GetPackageUserSettings(*GlobalConfigJSON, PackageUID);
        if (US.contains("PREFERRED_RUNNER") && US["PREFERRED_RUNNER"].is_string())
        {
            int Idx = RunnerCombo->findText(QString::fromStdString(std::string(US["PREFERRED_RUNNER"])));
            if (Idx >= 0) RunnerCombo->setCurrentIndex(Idx);
        }
    }

    // Entrypoint combobox
    VariantCombo = new QComboBox(ControlWidget);
    PickerForm->addRow("Variant:", VariantCombo);

    // Collect entrypoints via ContainerWrapper helper.
    std::string DefaultEntrypointID;
    if (SubgameIdx != -1)
    {
        auto &DEF = (*MANIFESTJSON)["SUBGAMES"][SubgameIdx]["DEFAULT_ENTRYPOINT_ID"];
        if (!DEF.is_null() && DEF.is_string()) DefaultEntrypointID = std::string(DEF);
    }

    std::vector<EntrypointInfo> Entrypoints = ContainerWrapper::GetAvailableEntrypoints(*MANIFESTJSON, SubgameID);
    for (auto &E : Entrypoints)
        VariantCombo->addItem(QString::fromStdString(E.ComponentName), QString::fromStdString(E.EntrypointID));

    // Pre-select entrypoint: USERSETTINGS > DEFAULT_ENTRYPOINT_ID.
    std::string PreselEntrypointID = DefaultEntrypointID;
    {
        auto US = ContainerWrapper::GetPackageUserSettings(*GlobalConfigJSON, PackageUID);
        if (US.contains("PREFERRED_VARIANT_ID") && US["PREFERRED_VARIANT_ID"].is_string())
            PreselEntrypointID = std::string(US["PREFERRED_VARIANT_ID"]);
    }

    for (int k = 0; k < VariantCombo->count(); k++)
    {
        if (VariantCombo->itemData(k).toString().toStdString() == PreselEntrypointID)
        { VariantCombo->setCurrentIndex(k); break; }
    }

    // CustomVar pickers — rebuilt dynamically when the entrypoint changes.
    CustomVarGroup = new QGroupBox("Options", ControlWidget);
    CustomVarForm  = new QFormLayout(CustomVarGroup);
    CustomVarGroup->setLayout(CustomVarForm);
    CustomVarGroup->setVisible(false);
    ControlLayout->addWidget(CustomVarGroup);

    connect(VariantCombo, &QComboBox::currentIndexChanged, this, &PreLaunchWindow::onEntrypointChanged);
    RebuildCustomVarPickers();

    // Progress bar + status label
    ProgressBar = new QProgressBar(ControlWidget);
    ProgressBar->setRange(0, 100);
    ProgressBar->setValue(0);
    ProgressBar->setVisible(false);
    ControlLayout->addWidget(ProgressBar);

    StatusLabel = new QLabel(ControlWidget);
    StatusLabel->setText("");
    ControlLayout->addWidget(StatusLabel);

    // Checkboxes
    NoCleanupCheck = new QCheckBox("No cleanup (keep mounts after exit)", ControlWidget);
    ControlLayout->addWidget(NoCleanupCheck);

    RememberCheck = new QCheckBox("Remember selection and skip this dialog next time", ControlWidget);
    ControlLayout->addWidget(RememberCheck);

    CloseAfterLaunchCheck = new QCheckBox("Close window when game starts", ControlWidget);
    ControlLayout->addWidget(CloseAfterLaunchCheck);

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
    QWidget*    BtnWidget = new QWidget(this);
    QHBoxLayout* BtnLayout = new QHBoxLayout(BtnWidget);
    BtnWidget->setLayout(BtnLayout);
    RootLayout->addWidget(BtnWidget);

    //Package Editor button on the left — opens the editor for this specific package.
    QPushButton * PackageEditorButton = new QPushButton("Package Editor", BtnWidget);
    BtnLayout->addWidget(PackageEditorButton);
    connect(PackageEditorButton, &QPushButton::clicked, this, [this]()
    {
        PackageEditor * Editor = new PackageEditor(this->GlobalConfigJSON, this, QString::fromStdString(this->PackagePath));
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
void PreLaunchWindow::onEntrypointChanged()
{
    RebuildCustomVarPickers();
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
    // Clear existing picker rows.
    while (CustomVarForm->rowCount() > 0)
        CustomVarForm->removeRow(0);

    if (!MANIFESTJSON || !MANIFESTJSON->contains("CUSTOMVARS") || !(*MANIFESTJSON)["CUSTOMVARS"].is_array())
    {
        CustomVarGroup->setVisible(false);
        return;
    }

    // Resolve the selected entrypoint's LASTCOMPONENT.
    std::string SelEntrypointID;
    if (VariantCombo->currentIndex() >= 0)
        SelEntrypointID = VariantCombo->currentData().toString().toStdString();

    std::string LastComp = ContainerWrapper::FindComponentForEntrypoint(*MANIFESTJSON, SubgameID, SelEntrypointID);

    // Get the recipe (ancestor chain) for that component.
    ContainerParams TmpParams("", SubgameID, LastComp);
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
    nlohmann::ordered_json EntrypointSeed = nlohmann::ordered_json::object();
    {
        int SubgameIdx = ContainerWrapper::FindSubgameIndex(*MANIFESTJSON, SubgameID);
        if (SubgameIdx != -1 && (*MANIFESTJSON)["SUBGAMES"][SubgameIdx].contains("ENTRYPOINTS"))
            for (auto &EP : (*MANIFESTJSON)["SUBGAMES"][SubgameIdx]["ENTRYPOINTS"])
                if (EP.value("ENTRYPOINT_ID", std::string()) == SelEntrypointID && EP.contains("FORCEVARS"))
                    EntrypointSeed = EP["FORCEVARS"];
    }

    // Get persisted USERSETTINGS variables.
    std::string PackageUID_local;
    if (MANIFESTJSON->contains("PACKAGEUID") && !(*MANIFESTJSON)["PACKAGEUID"].is_null())
        PackageUID_local = std::string((*MANIFESTJSON)["PACKAGEUID"]);
    auto US = ContainerWrapper::GetPackageUserSettings(*GlobalConfigJSON, PackageUID_local);
    nlohmann::ordered_json SavedVars = (US.contains("VARIABLES") && US["VARIABLES"].is_object())
                                        ? US["VARIABLES"] : nlohmann::ordered_json::object();

    bool AnyVisible = false;

    for (auto &CV : (*MANIFESTJSON)["CUSTOMVARS"])
    {
        std::string Key = CV.value("KEY", std::string());
        if (Key.empty()) continue;

        // Only show if this key is actually referenced in the chain.
        std::string Token = "%" + Key + "%";
        if (ChainSubComponentsStr.find(Token) == std::string::npos) continue;

        // Resolve initial value: entrypoint seed → USERSETTINGS → DEFAULT.
        std::string InitialValue = CV.value("DEFAULT", std::string());
        if (SavedVars.contains(Key) && SavedVars[Key].is_string())
            InitialValue = std::string(SavedVars[Key]);
        if (EntrypointSeed.contains(Key) && EntrypointSeed[Key].is_string())
            InitialValue = std::string(EntrypointSeed[Key]);

        bool Display = CV.value("DISPLAY", true);

        if (!Display) continue; // collected directly from entrypoint seed in onLaunchClicked

        // Apply Layer 1 substitution to InitialValue so %ScreenWidth% shows as 1920 in the widget.
        // (Layer 2 / type translation is NOT applied — widget always shows the display value.)
        {
            ContainerParams TmpP("", SubgameID, LastComp);
            ContainerWrapper::StringVariableSubstitution(InitialValue, TmpP.GetVariablesMap());
        }

        AnyVisible = true;
        std::string VarType = CV.value("VARTYPE", std::string("string"));
        QString Label       = QString::fromStdString(CV.value("LABEL", Key));

        if (VarType == "options" && CV.contains("OPTIONS") && CV["OPTIONS"].is_array())
        {
            QComboBox * Combo = new QComboBox(CustomVarGroup);
            Combo->setProperty("CVKey", QString::fromStdString(Key));
            for (auto &Opt : CV["OPTIONS"])
            {
                // Layer 1 on option VALUES too — they may contain tokens.
                std::string OptVal = Opt.value("VALUE", std::string());
                ContainerParams TmpP2("", SubgameID, LastComp);
                ContainerWrapper::StringVariableSubstitution(OptVal, TmpP2.GetVariablesMap());
                QString OptLabel = QString::fromStdString(Opt.value("LABEL", std::string()));
                Combo->addItem(OptLabel, QString::fromStdString(OptVal));
            }
            for (int k = 0; k < Combo->count(); k++)
                if (Combo->itemData(k).toString().toStdString() == InitialValue)
                { Combo->setCurrentIndex(k); break; }
            CustomVarForm->addRow(Label + ":", Combo);
        }
        else if (VarType == "dword" || VarType == "qword" || VarType == "number")
        {
            QSpinBox * Spin = new QSpinBox(CustomVarGroup);
            Spin->setProperty("CVKey", QString::fromStdString(Key));
            Spin->setMinimum(0);
            // dword caps at 2^32-1; for QSpinBox (int) use INT_MAX. qword/number use INT_MAX too.
            Spin->setMaximum(VarType == "dword" ? 2147483647 : 2147483647);
            try { Spin->setValue(std::stoi(InitialValue)); } catch (...) { Spin->setValue(0); }
            CustomVarForm->addRow(Label + ":", Spin);
        }
        else if (VarType == "bool")
        {
            QCheckBox * Check = new QCheckBox(CustomVarGroup);
            Check->setProperty("CVKey", QString::fromStdString(Key));
            std::string lower = InitialValue;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            Check->setChecked(lower == "1" || lower == "true" || lower == "yes");
            CustomVarForm->addRow(Label + ":", Check);
        }
        else // string or unknown
        {
            QLineEdit * Field = new QLineEdit(CustomVarGroup);
            Field->setProperty("CVKey", QString::fromStdString(Key));
            Field->setText(QString::fromStdString(InitialValue));
            CustomVarForm->addRow(Label + ":", Field);
        }
    }

    CustomVarGroup->setVisible(AnyVisible);
}

// ---------------------------------------------------------------------------
// Slot: Launch button clicked
// ---------------------------------------------------------------------------
void PreLaunchWindow::onLaunchClicked()
{
    // Resolve the selected component from the chosen entrypoint.
    std::string SelectedEntrypointID;
    if (VariantCombo->currentIndex() >= 0)
        SelectedEntrypointID = VariantCombo->currentData().toString().toStdString();

    std::string SelectedComponentID =
        ContainerWrapper::FindComponentForEntrypoint(*MANIFESTJSON, SubgameID, SelectedEntrypointID);

    // Fallback: first available entrypoint's component.
    if (SelectedComponentID.empty())
    {
        auto Entrypoints = ContainerWrapper::GetAvailableEntrypoints(*MANIFESTJSON, SubgameID);
        if (!Entrypoints.empty())
            SelectedComponentID = Entrypoints.front().ComponentID;
    }

    // Resolve the selected runner JSON.
    nlohmann::ordered_json SelRunner;
    int RunnerIdx = RunnerCombo->currentIndex();
    if (RunnerIdx >= 0 && RunnerIdx < static_cast<int>(Runners.size()))
        SelRunner = Runners[RunnerIdx].second;

    // Seed CollectedVars with every CustomVar's resolved value:
    // 1. Start from entrypoint FORCEVARS (covers DISPLAY:false vars automatically).
    // 2. Override with visible picker selections (covers DISPLAY:true vars).
    std::map<std::string, std::string> CollectedVars;

    // Step 1: entrypoint seed for all vars (baseline, handles DISPLAY:false silently).
    if (MANIFESTJSON && (*MANIFESTJSON).contains("CUSTOMVARS"))
    {
        // Get entrypoint FORCEVARS seed.
        int SubgameIdx2 = ContainerWrapper::FindSubgameIndex(*MANIFESTJSON, SubgameID);
        if (SubgameIdx2 != -1 && (*MANIFESTJSON)["SUBGAMES"][SubgameIdx2].contains("ENTRYPOINTS"))
            for (auto &EP : (*MANIFESTJSON)["SUBGAMES"][SubgameIdx2]["ENTRYPOINTS"])
                if (EP.value("ENTRYPOINT_ID", std::string()) == SelectedEntrypointID && EP.contains("FORCEVARS"))
                    for (auto &[K, V] : EP["FORCEVARS"].items())
                        if (V.is_string()) CollectedVars[K] = std::string(V);

        // Fill in DEFAULT for any key not covered by the seed.
        for (auto &CV : (*MANIFESTJSON)["CUSTOMVARS"])
        {
            std::string K = CV.value("KEY", std::string());
            if (K.empty() || CollectedVars.count(K)) continue;
            CollectedVars[K] = CV.value("DEFAULT", std::string());
        }
    }

    // Step 2: visible picker widgets override for DISPLAY:true vars.
    for (int row = 0; row < CustomVarForm->rowCount(); row++)
    {
        QLayoutItem * FieldItem = CustomVarForm->itemAt(row, QFormLayout::FieldRole);
        if (!FieldItem) continue;
        QWidget * W = FieldItem->widget();
        if (!W) continue;
        QString Key = W->property("CVKey").toString();
        if (Key.isEmpty()) continue;
        if (auto * CB  = qobject_cast<QComboBox*>(W))
            CollectedVars[Key.toStdString()] = CB->currentData().toString().toStdString();
        else if (auto * SP = qobject_cast<QSpinBox*>(W))
            CollectedVars[Key.toStdString()] = std::to_string(SP->value());
        else if (auto * CK = qobject_cast<QCheckBox*>(W))
            CollectedVars[Key.toStdString()] = CK->isChecked() ? "1" : "0";
        else if (auto * LE = qobject_cast<QLineEdit*>(W))
            CollectedVars[Key.toStdString()] = LE->text().toStdString();
    }

    // Save preferences if "Remember" is checked.
    if (RememberCheck->isChecked())
    {
        std::string RunnerName = SelRunner.is_null() ? "" : SelRunner.value("NAME", std::string());
        savePreferences(RunnerName, SelectedEntrypointID, true);
    }

    // Disable pickers and launch button; show progress bar.
    RunnerCombo->setEnabled(false);
    VariantCombo->setEnabled(false);
    CustomVarGroup->setEnabled(false);
    NoCleanupCheck->setEnabled(false);
    RememberCheck->setEnabled(false);
    CloseAfterLaunchCheck->setEnabled(false);
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
    LaunchWorker->ComponentID      = SelectedComponentID;
    LaunchWorker->EntrypointID       = SelectedEntrypointID;
    LaunchWorker->VariableOverrides  = CollectedVars;
    LaunchWorker->SelectedRunner     = SelRunner;
    LaunchWorker->SkipCleanup      = NoCleanupCheck->isChecked();

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
