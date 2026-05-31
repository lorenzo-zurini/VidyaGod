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
    std::string                        ComponentID;       // Pre-resolved from entrypoint selection
    std::string                        EntrypointID;      // ENTRYPOINT_ID of the selected entrypoint
    nlohmann::ordered_json             SelectedRunner;    // May be null if no override is needed
    std::map<std::string, std::string> VariableOverrides; // CustomVar values from picker / entrypoint seeds
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

private slots:
    void onLaunchClicked();
    void onKillClicked();
    void onEntrypointChanged();
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
    QGroupBox*    CustomVarGroup        = nullptr; // Rebuilt by RebuildCustomVarPickers()
    QFormLayout*  CustomVarForm         = nullptr;
    QTextEdit*    ConsoleEdit       = nullptr;
    QPushButton*  KillButton        = nullptr;
    QPushButton*  LaunchButton      = nullptr;
    QPushButton*  CloseButton       = nullptr;
};

#endif // PRELAUNCHWINDOW_H
