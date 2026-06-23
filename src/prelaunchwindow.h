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
#include <QPixmap>

#include <set>
#include <map>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "manifestmodel.h"  // NodeIndex / Node
#include "commonutils.h"
#include "launchthread.h"   // LaunchThread (worker)


// ---------------------------------------------------------------------------
// PreLaunchWindow — node-native launch dialog. Built from a library tile = a GROUP of launchable nodes
// (the editions). Lets the user pick variant + runner + optional-node toggles + CustomVars, shows live log,
// and launches the selected node via LaunchThread.LaunchNodeId (the engine resolves the rest from the graph).
// ---------------------------------------------------------------------------
class PreLaunchWindow : public QDialog
{
    Q_OBJECT
public:
    PreLaunchWindow(
        nlohmann::ordered_json* GlobalConfigJSON,
        const NodeIndex*        Index,           // shared global node graph (owned by MainWindow)
        std::vector<std::string> GroupNodeIds,   // the tile's launchable node ids (editions)
        QWidget*                parent = nullptr);

    ~PreLaunchWindow() override;

    //The owning bundle dir of the current variant — used by MainWindow::RefreshPackage to target this dialog.
    const std::string & packagePath() const { return BundleDir; }
    //Rebuilds the variant combo + runner + module/CustomVar pickers (e.g. after the package was edited).
    void ReloadAndRebuild();

protected:
    //Rescale the (right-hand) cover to fit its column on every resize, keeping aspect ratio + vertical centering.
    void resizeEvent(QResizeEvent* Event) override;

private slots:
    void onLaunchClicked();
    void onKillClicked();
    void onVariantChanged();
    void onLogLine(int level, QString context, QString message);
    void onStatusChanged(QString status);
    void onProgressChanged(int value);
    void onLaunchFinished(bool success, QString errorMsg);

private:
    // Saves PREFERRED_RUNNER / SKIP_LAUNCH_DIALOG / VARIABLES / MODULES to GlobalConfigJSON (keyed by the
    // bundle UID) and flushes it to ~/.VidyaGod/GlobalConfig.JSON.
    void persistGlobalConfig();
    // The currently-selected launchable node (the chosen variant). nullptr if the group is empty.
    const Node* CurrentLaunch() const;
    // Refresh the cover from the current variant's META.COVER.
    void RebuildCover();
    // Scale CoverPixmap to fit CoverLabel's current size (aspect-preserving) — called from RebuildCover + resizeEvent.
    void UpdateCoverScaled();
    // Rebuilds the runner dropdown for the current variant (runner candidate nodes).
    void RebuildRunnerCombo();
    // Rebuilds the CustomVar picker section from the current variant's enabled node closure.
    void RebuildCustomVarPickers();
    // Rebuilds the optional-node toggle tree from the current variant's optional ancestors.
    void RebuildModuleTree();
    void PropagateModuleItem(QTreeWidgetItem* Item);
    void RefreshModuleLocks();
    // Collects the toggle states into a node-id -> enabled map (passed to the engine as ModuleStates).
    std::map<std::string, bool> CollectModuleStates() const;

    // ----- data -----
    nlohmann::ordered_json* GlobalConfigJSON = nullptr;
    const NodeIndex*        Index            = nullptr;
    std::vector<std::string> GroupNodeIds;
    std::string             LaunchNodeId;   // current variant's node id
    std::string             BundleDir;      // current variant's bundle dir
    std::string             PackageUID;     // current variant's UID — USERSETTINGS key

    LaunchThread* LaunchWorker = nullptr;

    // ----- widgets -----
    QLabel*       CoverLabel        = nullptr;
    QPixmap       CoverPixmap;                  // full-res cover; scaled to fit CoverLabel on resize
    QComboBox*    RunnerCombo       = nullptr;
    QComboBox*    VariantCombo      = nullptr;
    QWidget*      VariantLabel      = nullptr; // the "Variant:" form label — hidden when only one variant
    QProgressBar* ProgressBar       = nullptr;
    QLabel*       StatusLabel       = nullptr;
    QCheckBox*    RememberCheck         = nullptr;
    QCheckBox*    CloseAfterLaunchCheck = nullptr;
    QCheckBox*    DryRunCheck           = nullptr;
    QCheckBox*    PreserveRuntimeCheck  = nullptr;
    QGroupBox*    CustomVarGroup        = nullptr;
    QFormLayout*  CustomVarForm         = nullptr;
    QGroupBox*    ModuleGroup           = nullptr;
    QTreeWidget*  ModuleTree            = nullptr;
    std::map<std::string, std::set<std::string>> ModuleExcludes; // node -> mutually-exclusive nodes (symmetric)
    std::set<std::string>                        LockedModules;  // (unused for nodes; kept for the tree helpers)
    QTextEdit*    ConsoleEdit       = nullptr;
    QPushButton*  KillButton        = nullptr;
    QPushButton*  LaunchButton      = nullptr;
    QPushButton*  CloseButton       = nullptr;
};

#endif // PRELAUNCHWINDOW_H
