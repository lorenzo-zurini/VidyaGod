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
#include "nodegraphview.h"
#include "nlohmann/json.hpp"




// ---------------------------------------------------------------------------
// PackageEditor — the node-native bundle editor ("everything is a node"). A bundle is a directory of
// <node_id>.json files; this dialog lists them as tabs and edits each node's fields (NODE_ID / ROLE / GROUP /
// LABEL / META / PLATFORM / EXEC / OPTIONAL / DEFAULT / EXCLUDE), its global PARENTS (a catalog-wide id picker),
// and its LAYERS (the per-TYPE sub-editors: VFS / RegEdit / DllOverride / FileEdit / Persist* / CustomVar).
// The authoring tooling (Run EXE / Browse / Regedit / Execute / Analyze) drives the native node engine
// (LaunchResolver::InitializeFromNode) against a freshly-saved-on-disk index of this bundle + the catalog.
//
// In-memory the working document is shaped { "NODES": [ <node>, ... ] } so the JSON-pointer field-edit machinery
// is reused unchanged (paths like /NODES/3/EXEC/CONTENTPATH, /NODES/3/LAYERS/2/PATH). Each node carries an
// editor-only "__FILE__" tag (its on-disk filename) so a NODE_ID rename re-files it. Save writes one file per node.
// ---------------------------------------------------------------------------
class PackageEditorModel;   // the state/signal hub (packageeditormodel.h) — owned by PackageEditor

class PackageEditor : public QDialog
{
    Q_OBJECT

public:
    //If PackagePath is non-empty, the directory picker is skipped and that bundle is opened directly.
    explicit PackageEditor(const nlohmann::ordered_json * GlobalConfigJSON, QWidget *parent = nullptr, const QString &PackagePath = "");
    ~PackageEditor();

signals:
    //Emitted whenever the bundle's node files are written to disk, so open library tiles / prelaunch dialogs
    //can reload and re-render. Carries the bundle directory path.
    void packageSaved(const QString &PackagePath);

private slots:
    void JSONQLineEditChanged();   // per-node field edit (sender's JSONPath property → working doc)
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
    bool BuildUI();

    //Appends a layer object to the LAYERS of the node owning `Sender` (the Add… button/menu), then persists +
    //rebuilds. Shared body of the AddRegEdit/AddDllOverride/AddFileEdit/... slots.
    void AppendLayer(QObject * Sender, const nlohmann::ordered_json & Layer);
    //Brings the selected source file/dir into the bundle dir for a VFS layer: asks Copy or Move, aborts on a
    //name collision. Returns the in-bundle basename to store as the layer PATH, or "" if cancelled/failed.
    QString ImportLayerFile(const QString & Selected, bool IsDir);

    //The NODE_ID of the node-tab that owns `Sender`, or "" (walks up the JSONPath property like AppendLayer).
    std::string NodeIdOfSender(QObject * Sender) const;

    //META cover drop ------------------------------------------------------------------------------
    bool eventFilter(QObject *obj, QEvent *event) override;
    void ApplyCoverImage(QLabel *CoverLabel, const QByteArray &Data, const QString &Extension, int NodeIndexInArray);

    //(Node I/O, validation, catalog/exec-index queries, and the authoring runs moved to PackageEditorModel.)

    //Saved UI state — restored after BuildUI() to keep the user on the same tab.
    int SavedMainTab = 1;

    QDir * PackageDir = nullptr;   // non-owning alias of Model->packageDir()

    //Selects the tab editing the node with this NODE_ID (tab 0 = JSON, tabs 1.. = NODES in array order).
    void SelectNodeTab(const std::string & NodeId);

    NodeGraphView * GraphView = nullptr;   // clickable DAG sidebar (left of the tabs)
    QTabWidget * PackageEditorTabWidget = nullptr;

    //The state/signal hub (owns the working document, node I/O, validation, authoring). The members below are
    //non-owning aliases into it, kept so the existing BuildUI machinery reads them unchanged.
    PackageEditorModel * Model = nullptr;
    //Working document: { "NODES": [ <node>, ... ] } — alias of Model->doc().
    nlohmann::ordered_json * MANIFESTJSON = nullptr;
    QNetworkAccessManager * NetMgr = nullptr;
};

#endif // PACKAGEEDITOR_H
