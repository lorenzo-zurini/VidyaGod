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

private:
    //Rebuilds the tab strip: tab 0 = JsonRawEditor, tabs 1.. = one NodeEditor per node. Called on documentReloaded.
    bool BuildUI();
    //Selects the tab editing the node with this NODE_ID (tab 0 = JSON, tabs 1.. = NODES in array order).
    void SelectNodeTab(const std::string & NodeId);

    //Saved UI state — restored after BuildUI() to keep the user on the same tab.
    int SavedMainTab = 1;

    QDir *          PackageDir = nullptr;              // non-owning alias of Model->packageDir()
    NodeGraphView * GraphView = nullptr;               // clickable DAG sidebar (left of the tabs)
    QTabWidget *    PackageEditorTabWidget = nullptr;

    //The state/signal hub: owns the working document, node I/O, validation, authoring. PackageEditor is the thin
    //composition root over it; MANIFESTJSON is a non-owning alias of Model->doc() for the BuildUI tab loop.
    PackageEditorModel *     Model = nullptr;
    nlohmann::ordered_json * MANIFESTJSON = nullptr;
};

#endif // PACKAGEEDITOR_H
