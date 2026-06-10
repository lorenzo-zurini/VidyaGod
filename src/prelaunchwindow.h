#ifndef PRELAUNCHWINDOW_H
#define PRELAUNCHWINDOW_H

#include <QDialog>
#include <QThread>
#include <QMutex>
#include <QProgressBar>
#include <QTextEdit>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QSpinBox>
#include <QTreeWidget>

#include "nlohmann/json.hpp"
#include "containerwrapper.h"
#include "commonutils.h"
#include "jsonoperations.h"

// ---------------------------------------------------------------------------
// LaunchThread
// Runs the full container lifecycle (InitializeContainer → BuildContainerRuntime
// → Execute → Cleanup) on a worker thread so the UI stays responsive.
// ---------------------------------------------------------------------------
class LaunchThread : public QThread
{
    Q_OBJECT
public:
    // Fields must be populated before calling start().
    nlohmann::ordered_json GlobalConfigJSON;
    nlohmann::ordered_json MANIFESTJSON;
    std::string            PackagePath;
    std::string            SubgameID;
    std::string                        ComponentID;       // Optional direct component (editor/CLI); usually empty
    std::string                        VariantID;         // VARIANT_ID of the selected variant
    std::string                        RunnerID;          // RUNNER_ID chosen in the picker (resolved before construction)
    std::map<std::string, std::string> VariableOverrides; // CustomVar values from picker / variant FORCEVARS seeds
    std::map<std::string, bool>        ModuleStates;      // Optional-module toggles from the prelaunch tree (component → enabled)
    std::string                        ScreenWidth;       // Captured on the MAIN thread before start() — never query Qt GUI from run()
    std::string                        ScreenHeight;      // (QGuiApplication screen access off the main thread is undefined behaviour)
    bool                               DryRun = false;    // If true, WRITELAYER is deleted after cleanup
    bool                   SkipCleanup = false;

    // Forcibly kills the running game process (if any).
    void kill();

signals:
    // Raw log line forwarded from the Log() callback.
    void logLine(int level, QString context, QString message);
    // Short human-readable status string for the status label.
    void statusChanged(QString status);
    // Progress 0-100 for the progress bar.
    void progressChanged(int value);
    // Emitted once when the full lifecycle has completed (success or failure).
    void launchFinished(bool success, QString errorMsg);

protected:
    void run() override;

private:
    ContainerWrapper* wrapper = nullptr;
    QMutex            wrapperMutex;
};

// ---------------------------------------------------------------------------
// PreLaunchWindow
// Non-modal dialog that lets the user pick runner + variant, shows live log
// output, and provides Kill / Close buttons.
// ---------------------------------------------------------------------------
class PreLaunchWindow : public QDialog
{
    Q_OBJECT
public:
    explicit PreLaunchWindow(
        nlohmann::ordered_json* GlobalConfigJSON,
        nlohmann::ordered_json* MANIFESTJSON,
        const std::string&      PackagePath,
        const std::string&      SubgameID,
        QWidget*                parent = nullptr);

    ~PreLaunchWindow() override;

    //The package this dialog is for (used by MainWindow::RefreshPackage to target the right dialog).
    const std::string & packagePath() const { return PackagePath; }
    //Re-assembles the manifest from disk and rebuilds the variant combo + module tree + CustomVar pickers
    //after the package was edited. Safe to call on the shared manifest pointer (in-place re-assemble).
    void ReloadAndRebuild();

private slots:
    void onLaunchClicked();
    void onKillClicked();
    void onVariantChanged();
    void onLogLine(int level, QString context, QString message);
    void onStatusChanged(QString status);
    void onProgressChanged(int value);
    void onLaunchFinished(bool success, QString errorMsg);

private:
    // Saves PREFERRED_RUNNER / PREFERRED_VARIANT_ID / SKIP_LAUNCH_DIALOG to
    // GlobalConfigJSON and flushes it to ~/.VidyaGod/GlobalConfig.JSON.
    void savePreferences(const std::string& runnerName, const std::string& variantID, bool skipNext);
    // Rebuilds the CustomVar picker section based on the currently selected entrypoint.
    void RebuildCustomVarPickers();
    // Rebuilds the Modules tree from the selected variant's + runner's MODULES (nested by PARENTCOMPONENT).
    void RebuildModuleTree();
    // Reacts to a tree checkbox change: records the new desired state (ignored for locked items) then
    // recomputes the whole tree's locks.
    void PropagateModuleItem(QTreeWidgetItem* Item);
    // Recomputes every item's checkbox + locked/greyed state top-down: required → locked-on; an optional
    // item under a disabled ancestor → locked-off; otherwise its own desired state.
    void RefreshModuleLocks();
    // Collects the tree's current optional-module checkbox states into a component→enabled map.
    std::map<std::string, bool> CollectModuleStates() const;

    // ----- data -----
    nlohmann::ordered_json* GlobalConfigJSON = nullptr;
    nlohmann::ordered_json* MANIFESTJSON     = nullptr;
    std::string             PackagePath;
    std::string             SubgameID;
    std::string             PackageUID;   // Cached from MANIFEST for USERSETTINGS lookups

    LaunchThread* LaunchWorker = nullptr;

    // Collected runner list: display-name → runner JSON object
    std::vector<std::pair<QString, nlohmann::ordered_json>> Runners;

    // ----- widgets -----
    QLabel*       CoverLabel        = nullptr;
    QComboBox*    RunnerCombo       = nullptr;
    QComboBox*    VariantCombo      = nullptr;
    QProgressBar* ProgressBar       = nullptr;
    QLabel*       StatusLabel       = nullptr;
    QCheckBox*    NoCleanupCheck        = nullptr;
    QCheckBox*    RememberCheck         = nullptr;
    QCheckBox*    CloseAfterLaunchCheck = nullptr;
    QCheckBox*    DryRunCheck           = nullptr;
    QGroupBox*    CustomVarGroup        = nullptr; // Rebuilt by RebuildCustomVarPickers()
    QFormLayout*  CustomVarForm         = nullptr;
    QGroupBox*    ModuleGroup           = nullptr; // Rebuilt by RebuildModuleTree()
    QTreeWidget*  ModuleTree            = nullptr;
    QTextEdit*    ConsoleEdit       = nullptr;
    QPushButton*  KillButton        = nullptr;
    QPushButton*  LaunchButton      = nullptr;
    QPushButton*  CloseButton       = nullptr;
};

#endif // PRELAUNCHWINDOW_H
