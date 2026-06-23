#ifndef PACKAGEEDITOR_H
#define PACKAGEEDITOR_H

#include <QDialog>
#include <QDir>
#include <QString>

#include "nodegraphview.h"     // NodeGraphView — the clickable DAG sidebar (member)
#include "nlohmann/json.hpp"

// ---------------------------------------------------------------------------
// PackageEditor — the node-native bundle editor ("everything is a node"), now a THIN COMPOSITION ROOT. A bundle is
// a directory of <node_id>.json files; the dialog frames a NodeGraphView sidebar + a tab strip (tab 0 =
// JsonRawEditor, tabs 1.. = one NodeEditor per node) + a docked ValidationPanel. All state and logic live in
// PackageEditorModel (the working { "NODES":[...] } document, node I/O, validation, catalog/exec queries, the
// authoring runs); the editor owns the model, rebuilds its tabs on documentReloaded, and relays savedToDisk →
// packageSaved. The per-concern widgets talk only to the model — never to each other or back to this shell.
// ---------------------------------------------------------------------------
class PackageEditorModel;   // the state/signal hub (packageeditormodel.h) — owned by PackageEditor
class QTabWidget;

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
