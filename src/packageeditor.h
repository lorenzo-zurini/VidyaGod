#ifndef PACKAGEEDITOR_H
#define PACKAGEEDITOR_H

#include <QWidget>
#include <iostream>
#include <QDialog>
#include <QScreen>
#include <QToolButton>
#include <QMenu>

#include <QDir>
#include <QFileDialog>
#include <QLineEdit>

#include <QLabel>
#include <QLayout>
#include <QScrollArea>

#include <QVBoxLayout>
#include <QFormLayout>
#include <QStackedLayout>

#include <QBoxLayout>
#include <QGroupBox>
#include <QComboBox>
#include <QCheckBox>

#include <QTextEdit>
#include <QAbstractItemModel>
#include <QItemDelegate>

#include <QSettings>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QEventLoop>
#include <QPixmap>
#include <QFileInfo>

#include "filesystemoperations.h"
#include "jsonoperations.h"
#include "containerwrapper.h"
#include "packagecatalog.h"
#include "manifestmodel.h"
#include "nlohmann/json.hpp"


// ---------------------------------------------------------------------------
// NodeGraphView — a compact, clickable DAG of a bundle's nodes. Laid out left→right: launchables on the left, their
// PARENTS chain flowing right, deepest base content on the right. A linear dependency chain stays on one row; extra
// parents branch onto new rows (so it reads "linear with branches"). Clicking a node chip emits nodeClicked(NODE_ID),
// which the editor uses to jump to that node's tab. Refreshed via SetGraph() on every edit.
// ---------------------------------------------------------------------------
class NodeGraphView : public QWidget
{
    Q_OBJECT
public:
    explicit NodeGraphView(QWidget * parent = nullptr);
    void SetGraph(const nlohmann::ordered_json & NodesArray);   // the working { NODES:[...] } array
    void SetCurrent(const std::string & NodeId);                // highlight the open node
    QSize sizeHint() const override;

signals:
    void nodeClicked(const QString & NodeId);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;

private:
    struct GNode { std::string Id; QString Label; std::string Role; int Col = 0, Row = 0; QRect Rect; };
    std::vector<GNode> Nodes;
    std::vector<std::pair<int,int>> Edges;   // (child index, parent index)
    std::string Current;
    int ContentW = 0, ContentH = 0;
    void Relayout();
};


// ---------------------------------------------------------------------------
// PackageEditor — the node-native bundle editor ("everything is a node"). A bundle is a directory of
// <node_id>.json files; this dialog lists them as tabs and edits each node's fields (NODE_ID / ROLE / GROUP /
// LABEL / META / PLATFORM / EXEC / OPTIONAL / DEFAULT / EXCLUDE), its global PARENTS (a catalog-wide id picker),
// and its LAYERS (the per-TYPE sub-editors: VFS / RegEdit / DllOverride / FileEdit / Persist* / CustomVar).
// The authoring tooling (Run EXE / Browse / Regedit / Execute / Analyze) drives the native node engine
// (ContainerWrapper::InitializeFromNode) against a freshly-saved-on-disk index of this bundle + the catalog.
//
// In-memory the working document is shaped { "NODES": [ <node>, ... ] } so the JSON-pointer field-edit machinery
// is reused unchanged (paths like /NODES/3/EXEC/CONTENTPATH, /NODES/3/LAYERS/2/PATH). Each node carries an
// editor-only "__FILE__" tag (its on-disk filename) so a NODE_ID rename re-files it. Save writes one file per node.
// ---------------------------------------------------------------------------
class PackageEditor : public QDialog
{
    Q_OBJECT

public:
    //If PackagePath is non-empty, the directory picker is skipped and that bundle is opened directly.
    explicit PackageEditor(nlohmann::ordered_json * GlobalConfigJSON, QWidget *parent = nullptr, const QString &PackagePath = "");
    ~PackageEditor();

signals:
    //Emitted whenever the bundle's node files are written to disk, so open library tiles / prelaunch dialogs
    //can reload and re-render. Carries the bundle directory path.
    void packageSaved(const QString &PackagePath);

private slots:
    void JSONQTextEditChanged();
    void JSONQLineEditChanged();
    void SaveJSONButtonPressed();
    // LAYERS sub-editor add-actions (append a TYPE-tagged layer to the owning node's LAYERS array).
    void AddVFSDirLayer();
    void AddVFSZipLayer();
    void AddVFSFileLayer();
    void AddRegEdit();
    void AddDllOverride();
    void AddFileEdit();
    void AddCustomVar();
    void AddPersistDir();
    void AddPersistFile();
    void AddRegPersist();
    void AddRegKeyPersist();

private:
    void SaveNodes();
    void RefreshJSONView();
    void InitPackage(const QString &PreselectedPath = "");
    bool BuildUI();

    //Appends a layer object to the LAYERS of the node owning `Sender` (the Add… button/menu), then persists +
    //rebuilds. Shared body of the AddRegEdit/AddDllOverride/AddFileEdit/... slots.
    void AppendLayer(QObject * Sender, const nlohmann::ordered_json & Layer);
    //Brings the selected source file/dir into the bundle dir for a VFS layer: asks Copy or Move, aborts on a
    //name collision. Returns the in-bundle basename to store as the layer PATH, or "" if cancelled/failed.
    QString ImportLayerFile(const QString & Selected, bool IsDir);

    //NODE I/O (one file per node) ------------------------------------------------------------------
    //Loads every <node_id>.json fragment in the bundle into the in-memory NODES array, tagging each node with a
    //hidden "__FILE__" provenance key. Non-node *.json files (e.g. legacy MANIFEST.json) are ignored.
    void LoadNodes();
    //The on-disk filename a node should live in: its "__FILE__" tag, else <NODE_ID>.json.
    QString FileForNode(const nlohmann::ordered_json & Node) const;

    //AUTHORING EXECUTE (native node engine) -------------------------------------------------------
    //Builds a NodeIndex covering THIS bundle (authoritative — freshly saved) plus the catalog repos, so a node's
    //PARENTS (e.g. cross-bundle runner builds) resolve. The returned index is owned by the caller.
    NodeIndex BuildExecIndex() const;
    //The NODE_ID of the node-tab that owns `Sender`, or "" (walks up the JSONPath property like AppendLayer).
    std::string NodeIdOfSender(QObject * Sender) const;
    //Runs `Exe` (default = the launchable's CONTENTPATH) inside NodeId's resolved container (build → execute →
    //cleanup), node-natively. Used by Run-EXE / Browse / Regedit / Execute.
    void RunInNode(const std::string & NodeId, const std::string & Exe = "");
    //Registry-diff authoring: build NodeId's container, snapshot its baseline registry vs the carried-over
    //WRITELAYER, and merge the delta back into NodeId's LAYERS as RegEdit layers.
    void AnalyzeNodeRegistry(const std::string & NodeId);
    void MergeRegistryDeltaInNode(nlohmann::ordered_json * Delta, int NodeIndexInArray);

    //VALIDATION -----------------------------------------------------------------------------------
    //Runs ManifestModel::ValidateNodeGraph over this bundle's nodes (plus catalog, for cross-bundle parents).
    void Revalidate();
    void UpdateValidationBox();

    //META cover drop ------------------------------------------------------------------------------
    bool eventFilter(QObject *obj, QEvent *event) override;
    void ApplyCoverImage(QLabel *CoverLabel, const QByteArray &Data, const QString &Extension, int NodeIndexInArray);

    //The union of every platform some runner can serve (GUEST across the catalog runners + this bundle's runner
    //nodes) — drives the HOST / GUEST platform dropdown suggestions.
    std::vector<std::string> KnownPlatforms();
    //Every node id known (this bundle + catalog) — the global PARENTS picker source.
    std::vector<std::string> KnownNodeIds();

    //Saved UI state — restored after BuildUI() to keep the user on the same tab.
    int SavedMainTab = 1;

    QWidget * JSONTabWidget = nullptr;
    QPushButton * SaveJSONButton = nullptr;
    QTextEdit * JSONTextEdit = nullptr;
    QComboBox * JSONFileCombo = nullptr;   // selects which node file the raw JSON tab edits

    QDir * PackageDir = nullptr;

    std::vector<std::string> ValErrors, ValWarnings;

    //Selects the tab editing the node with this NODE_ID (tab 0 = JSON, tabs 1.. = NODES in array order).
    void SelectNodeTab(const std::string & NodeId);

    NodeGraphView * GraphView = nullptr;   // clickable DAG above the tabs
    QTabWidget * PackageEditorTabWidget = nullptr;
    //Persistent validation panel docked below the tabs (always visible), refreshed by UpdateValidationBox().
    QGroupBox * ValidationBox  = nullptr;
    QTextEdit * ValidationView = nullptr;

    //Working document: { "NODES": [ <node>, ... ] } (each node carries an editor-only "__FILE__" tag).
    nlohmann::ordered_json * MANIFESTJSON = nullptr;
    nlohmann::ordered_json * GlobalConfigJSON = nullptr;
    QNetworkAccessManager * NetMgr = nullptr;
};

#endif // PACKAGEEDITOR_H
