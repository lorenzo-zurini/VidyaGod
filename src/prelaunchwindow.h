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
#include <QTimer>
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
    //Rescale the cover to fit its column on every resize, keeping aspect ratio + vertical centering.
    void resizeEvent(QResizeEvent* Event) override;
    //The cover label's OWN resize drives the rescale: the dialog's resizeEvent fires BEFORE the (deferred) child
    //layout pass, so with the label nested in the left column its rect is stale there — the pixmap froze at the
    //construction-time 9×14 rect ("comically tiny cover"). Filtering the label's Resize scales at the right moment.
    bool eventFilter(QObject* Obj, QEvent* Event) override;

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
    // Resolves the DEFAULT runner daisy-chain for the current variant (shortest route to this machine, terminated by
    // the native runner) into CurrentChain, then renders the per-step combos. Called on variant change / construction.
    void RebuildRunnerChain();
    // (Re)builds the per-step combo stack from CurrentChain: one row per link ("<input platform> →" + a combo of the
    // runners that can consume it), plus the target-platform hint. Also gates the Launch button on chain validity.
    void RenderChainCombos();
    // A chain step's combo changed: adopt the choice, BFS-re-resolve the tail from its HOST, re-render + refresh vars.
    void onChainStepChanged(int Step);
    // The input platform feeding chain step i (i==0 → the content's platform; else the previous link's HOST).
    std::string ChainStepInput(int Step) const;
    // True if a chosen runner id is a native terminal (HOST==GUEST==machine, or the synthesized passthrough sentinel).
    bool ChainIdIsTerminal(const std::string& Id) const;
    // Rebuilds the CustomVar picker section from the current variant's enabled node closure (UI-facet vars only,
    // grouped by UI.GROUP, with controls per UI.CONTROL and constraints).
    void RebuildCustomVarPickers();
    // Re-evaluates every var row's UI.WHEN condition against the current control values and shows/hides the row.
    // Wired to each control's change signal so dependent options appear/disappear live.
    void EvaluateVarConditions();
    // Builds the "Virtual LAN" panel under the cover (friends with per-member ticks + live link badges) and starts
    // its 2s poll; RefreshLanPanel reconciles rows against IpfsWrapper::LanPeers(). Ticks edit the GLOBAL roster
    // (Settings.LanExcludedPeers) and apply immediately via SetLanExcluded — mid-game included.
    void BuildLanPanel(QVBoxLayout * LeftCol);
    void RefreshLanPanel();

public:
    // Pre-fill this window as a JOIN of a friend (or a typed address) and preselect the variant they are running.
    // Call after construction, before show(). Order matters inside: selecting the variant rebuilds the CustomVar
    // widgets, so the join controls are set afterwards.
    void PresetJoin(const QString & TargetAddress, const QString & FriendPeerId,
                    const std::string & SelectNodeId, const QString & Warning);

private:
    // (Re)fills the variant combo from GroupNodeIds: recommended first, then natural version order (1.9 < 1.10);
    // beyond a dozen entries the combo turns editable with a contains-matching completer (type "1.20" to jump) —
    // so a tile with 903 variants is as navigable as one with 3.
    void FillVariantCombo();
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
    // Async-cover state: ONE coverReady connection for the window's lifetime (see RebuildCover), keyed on
    // the CID we're currently waiting for — variant switches just retarget these instead of re-connecting.
    bool                   CoverReadyConnected = false;
    QString                PendingCoverCid;
    QString                PendingCoverPkg;
    nlohmann::ordered_json PendingCoverNode;
    // Runner daisy-chain UI: a stack of per-step combos (innermost→outermost) + a target-platform hint.
    QWidget*      ChainContainer    = nullptr;  // holds the per-step rows
    QVBoxLayout*  ChainLayout       = nullptr;
    QLabel*       ChainHint         = nullptr;  // "→ linux64" validity hint under the chain
    std::vector<QComboBox*>  ChainCombos;       // one per chain step (rebuilt each render)
    std::vector<std::string> CurrentChain;      // the chosen chain (innermost→outermost runner node ids)
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
    QTextEdit*    ConsoleEdit       = nullptr;
    QPushButton*  KillButton        = nullptr;
    QPushButton*  LaunchButton      = nullptr;
    QPushButton*  CloseButton       = nullptr;

    // Virtual LAN panel (under the cover): per-friend tick + live link badge, 2s poll while the window lives.
    QGroupBox*    LanGroup          = nullptr;
    QVBoxLayout*  LanRows           = nullptr;
    QLabel*       LanStatus         = nullptr;   // "Your vIP: 10.66.x.y" / "LAN offline"
    QTimer*       LanTimer          = nullptr;
    std::map<std::string, std::pair<QCheckBox*, QLabel*>> LanRowByPeer;   // peer id → (tick, badge)

    // Join target: who (or what address) this launch should connect to. Feeds VIDYAGOD_JOIN_ADDRESS for THIS launch
    // only — deliberately not the global-roster pattern the per-friend ticks above use.
    QComboBox*    JoinCombo         = nullptr;   // "Host / solo" · friends · "Other address…"
    QLineEdit*    JoinAddress       = nullptr;
    QLabel*       JoinWarn          = nullptr;   // version mismatch / bridge warning; hidden when empty
    std::string   JoinSignature;                 // the peer tuple the combo was last built from
};

#endif // PRELAUNCHWINDOW_H
