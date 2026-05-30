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

    // -----------------------------------------------------------------
    // Step 1: Resolve executable definition.
    // -----------------------------------------------------------------
    if (!ContainerWrapper::ResolveExecutableDefinition(LocalWrapper->ContainerParams))
    {
        ClearLogCallback();
        {
            QMutexLocker Locker(&wrapperMutex);
            wrapper = nullptr;
        }
        delete LocalWrapper;
        emit launchFinished(false, "Could not resolve variant definition.\nCheck EXECUTABLE_ID and VARIANT_ID in the manifest.");
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
        setWindowTitle("Launch " + QString::fromStdString(
            std::string((*MANIFESTJSON)["SUBGAMES"][SubgameIdx]["TITLE"])));

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
            QString Label = QString::fromStdString(Runner.value("NAME", std::string("(unnamed)")));
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

    // Variant combobox
    VariantCombo = new QComboBox(ControlWidget);
    PickerForm->addRow("Variant:", VariantCombo);

    // Collect variants via ContainerWrapper helper.
    std::string ExecutableID;
    std::string DefaultVariantID;
    if (SubgameIdx != -1)
    {
        auto &EF = (*MANIFESTJSON)["SUBGAMES"][SubgameIdx]["EXECUTABLE_ID"];
        if (!EF.is_null() && EF.is_string()) ExecutableID = std::string(EF);
        auto &DF = (*MANIFESTJSON)["SUBGAMES"][SubgameIdx]["DEFAULT_VARIANT_ID"];
        if (!DF.is_null() && DF.is_string()) DefaultVariantID = std::string(DF);
    }

    std::vector<VariantInfo> Variants = ContainerWrapper::GetAvailableVariants(*MANIFESTJSON, ExecutableID);
    for (auto &V : Variants)
        VariantCombo->addItem(QString::fromStdString(V.ComponentName), QString::fromStdString(V.VariantID));

    // Pre-select variant: USERSETTINGS > DEFAULT_VARIANT_ID.
    std::string PreselVariantID = DefaultVariantID;
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
// Slot: Launch button clicked
// ---------------------------------------------------------------------------
void PreLaunchWindow::onLaunchClicked()
{
    // Resolve the selected component from the chosen variant.
    std::string ExecutableID;
    int SubgameIdx = ContainerWrapper::FindSubgameIndex(*MANIFESTJSON, SubgameID);
    if (SubgameIdx != -1)
    {
        auto &EF = (*MANIFESTJSON)["SUBGAMES"][SubgameIdx]["EXECUTABLE_ID"];
        if (!EF.is_null() && EF.is_string()) ExecutableID = std::string(EF);
    }

    std::string SelectedVariantID;
    if (VariantCombo->currentIndex() >= 0)
        SelectedVariantID = VariantCombo->currentData().toString().toStdString();

    std::string SelectedComponentID =
        ContainerWrapper::FindComponentForVariant(*MANIFESTJSON, ExecutableID, SelectedVariantID);

    // Fallback: first available variant's component.
    if (SelectedComponentID.empty())
    {
        auto Variants = ContainerWrapper::GetAvailableVariants(*MANIFESTJSON, ExecutableID);
        if (!Variants.empty())
            SelectedComponentID = Variants.front().ComponentID;
    }

    // Resolve the selected runner JSON.
    nlohmann::ordered_json SelRunner;
    int RunnerIdx = RunnerCombo->currentIndex();
    if (RunnerIdx >= 0 && RunnerIdx < static_cast<int>(Runners.size()))
        SelRunner = Runners[RunnerIdx].second;

    // Save preferences if "Remember" is checked.
    if (RememberCheck->isChecked())
    {
        std::string RunnerName = SelRunner.is_null() ? "" : SelRunner.value("NAME", std::string());
        savePreferences(RunnerName, SelectedVariantID, true);
    }

    // Disable pickers and launch button; show progress bar.
    RunnerCombo->setEnabled(false);
    VariantCombo->setEnabled(false);
    NoCleanupCheck->setEnabled(false);
    RememberCheck->setEnabled(false);
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
    LaunchWorker->SelectedRunner   = SelRunner;
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
        KillButton->setEnabled(true);
}

// ---------------------------------------------------------------------------
// Slot: worker thread finished
// ---------------------------------------------------------------------------
void PreLaunchWindow::onLaunchFinished(bool success, QString errorMsg)
{
    KillButton->setEnabled(false);
    CloseButton->setEnabled(true);

    if (success)
    {
        StatusLabel->setText("Finished.");
        ProgressBar->setValue(100);
    }
    else
    {
        StatusLabel->setText("Error: " + errorMsg);
    }
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
