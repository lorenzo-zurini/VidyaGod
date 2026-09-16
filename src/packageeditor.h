#ifndef PACKAGEEDITOR_H
#define PACKAGEEDITOR_H

#include <QDialog>
#include <QDir>
#include <QString>

#include "nlohmann/json.hpp"

// ---------------------------------------------------------------------------
// PackageEditor — the node-native bundle editor ("everything is a node"), a THIN COMPOSITION ROOT. A bundle is a
// directory of <node_id>.json files; the dialog frames the blueprint CANVAS (the editing surface) beside the raw
// JSON view and a docked ValidationPanel. The canvas edits the same document the model persists, so a wire
// dragged on screen is a PARENTS entry on disk and nothing has to be kept in sync. All state and logic live in
// PackageEditorModel (the working { "NODES":[...] } document, node I/O, validation, catalog/exec queries, the
// authoring runs); the editor owns the model, rebuilds its tabs on documentReloaded, and relays savedToDisk →
// packageSaved. The per-concern widgets talk only to the model — never to each other or back to this shell.
// ---------------------------------------------------------------------------
class PackageEditorModel;
class JsonRawEditor;   // the state/signal hub (packageeditormodel.h) — owned by PackageEditor
class PkgCanvasPanel;       // the blueprint canvas — the editing surface
class PkgActions;           // performs the node actions the canvas asks for

class PackageEditor : public QDialog
{
    Q_OBJECT

public:
    //If PackagePath is non-empty, the directory picker is skipped and that bundle is opened directly.
    explicit PackageEditor(nlohmann::ordered_json * GlobalConfigJSON, QWidget *parent = nullptr, const QString &PackagePath = "");
    ~PackageEditor();

    //THE way to open the editor. The canvas is Dear ImGui, which has ONE global context — a second editor
    //therefore cannot render, and used to appear as a blank/garbage widget with the reason only in the log.
    //So there is one editor: this raises the open one (saying so when it holds a different bundle) instead of
    //constructing a second that cannot work. Returns the live editor, or nullptr if the user cancelled the
    //directory picker. Ownership is the parent's, as before.
    //`Created` (optional) reports whether this call constructed the editor, so a caller only wires its
    //signals once - raising the existing editor must not stack another copy of the same connection.
    static PackageEditor *OpenFor(nlohmann::ordered_json *GlobalConfigJSON, QWidget *parent = nullptr,
                                  const QString &PackagePath = "", bool *Created = nullptr);
    //The bundle directory this editor has open ("" until one is chosen).
    QString bundleDir() const;

signals:
    //Emitted whenever the bundle's node files are written to disk, so open library tiles / prelaunch dialogs
    //can reload and re-render. Carries the bundle directory path.
    void packageSaved(const QString &PackagePath);
    //Emitted when the author asks to publish the just-dehydrated package to their IPNS library. The opener wires this
    //to AppModel::publishLibraries (off-thread re-mint + DHT put); an opener without an AppModel leaves it unconnected.
    void publishToLibraryRequested();

private:
    static PackageEditor *Live;      // the one open editor, or null

    //Re-validates and pushes the per-node findings onto the canvas. Called on documentReloaded.
    bool BuildUI();
    //Selects the node with this NODE_ID on the canvas.
    void SelectNodeTab(const std::string & NodeId);

    QDir *           PackageDir = nullptr;             // non-owning alias of Model->packageDir()
    PkgCanvasPanel * Canvas  = nullptr;                // the editing surface
    JsonRawEditor  * Json    = nullptr;                // the live, selection-driven raw-JSON panel
    PkgActions *     Actions = nullptr;                // node-action executor

    //The state/signal hub: owns the working document, node I/O, validation, authoring. PackageEditor is the thin
    //composition root over it; MANIFESTJSON is a non-owning alias of Model->doc() for the BuildUI tab loop.
    PackageEditorModel *     Model = nullptr;
    nlohmann::ordered_json * MANIFESTJSON = nullptr;
};

#endif // PACKAGEEDITOR_H
