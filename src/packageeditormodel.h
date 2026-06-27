#ifndef PACKAGEEDITORMODEL_H
#define PACKAGEEDITORMODEL_H

#include <QObject>
#include <QDir>
#include <QString>

#include <nlohmann/json.hpp>

#include "manifestmodel.h"   // NodeIndex

#include <string>
#include <vector>

class QWidget;

// ---------------------------------------------------------------------------
// PackageEditorModel — the state/signal hub for the node-native bundle editor (the AppModel-style central structure
// for PackageEditor). It OWNS the working document (`{ "NODES":[ <node>, ... ] }`, each node carrying an editor-only
// "__FILE__" tag), the bundle `QDir`, and the borrowed read-only GlobalConfigJSON, plus the latest validation
// results. It does all the NON-UI work: node file I/O, validation, the catalog/exec-index queries, and the
// authoring runs (build a container + execute / analyze registry against the live launch engine). The editor's
// widgets read state through it, call its mutators, and react to its signals — they never touch each other.
//
// A QWidget* DialogParent is held solely to parent the modal dialogs the authoring runs raise (mirrors how
// DownloadManager holds a dialog parent). The model never mutates GlobalConfigJSON (it only reads repo dirs and
// hands a copy to ContainerWrapper), so it borrows it by const.
// ---------------------------------------------------------------------------
class PackageEditorModel : public QObject
{
    Q_OBJECT
public:
    PackageEditorModel(const nlohmann::ordered_json * globalConfig, QWidget * dialogParent, QObject * parent = nullptr);
    ~PackageEditorModel() override;

    // ── State access (widgets read directly; the model owns the lifetime) ──
    nlohmann::ordered_json &       doc()                 { return Doc; }
    const nlohmann::ordered_json & doc() const           { return Doc; }
    QDir *                         packageDir() const    { return PackageDir; }
    const nlohmann::ordered_json * globalConfig() const  { return GlobalConfigJSON; }
    const std::vector<std::string> & validationErrors()   const { return ValErrors; }
    const std::vector<std::string> & validationWarnings() const { return ValWarnings; }

    // ── Bundle open + node file I/O (one file per node) ──
    void initPackage(const QString & preselectedPath, QWidget * dirPickerParent);   // pick dir (if empty) + LoadNodes
    void LoadNodes();
    void SaveNodes();                                              // write files → emit savedToDisk + validationChanged
    QString FileForNode(const nlohmann::ordered_json & Node) const;
    //Replace one node's whole JSON (preserving its __FILE__ tag), persist, and request a structural rebuild. Used
    //by the raw-JSON tab's Save Node.
    void replaceNodeJson(int nodeIndex, nlohmann::ordered_json node);
    //Ask the views to rebuild (emit documentReloaded) — used by a widget after a structural edit it made directly
    //on doc() (node move/remove, layer add/remove, role change, …).
    void requestReload() { emit documentReloaded(); }

    // ── Validation (ManifestModel::ValidateNodeGraph over this bundle + catalog) ──
    void Revalidate();

    // ── Catalog queries (exec index / PARENTS picker / platform suggestions) ──
    NodeIndex BuildExecIndex() const;
    std::vector<std::string> KnownNodeIds();
    std::vector<std::string> KnownPlatforms();

    // ── Authoring (native node engine): build NodeId's container and run / analyze it ──
    void RunInNode(const std::string & NodeId, const std::string & Exe = "");
    void AnalyzeNodeRegistry(const std::string & NodeId);
    // True if "Test launch" can do something for this node: it's launchable (has a DeclareExec) OR a launchable in the
    // bundle includes it (so the node can be tested in the context of the game that pulls it in). Uses the cached index.
    bool NodeTestable(const std::string & NodeId) const;

    // ── Authoring Session hooks (the held-open AuthoringSession window captures back into the document) ──
    QString                  packagePath() const;                        // the edited bundle dir ("" if none open)
    std::vector<std::string> bundleNodeIds() const;                      // NODE_IDs declared in THIS bundle (target picker)
    // Append a captured layer (VFSDirLayer / …) to NodeId's LAYERS, persist + rebuild. No-op if the node is gone.
    void appendLayerToNode(const std::string & NodeId, const nlohmann::ordered_json & Layer);
    // Merge a captured RegEdit delta array into NodeId (reusing MergeRegistryDeltaInNode), persist + rebuild.
    void mergeRegEditsIntoNode(const std::string & NodeId, nlohmann::ordered_json Delta);

signals:
    void documentReloaded();                       // structural change — views rebuild
    void validationChanged();                      // ValErrors/ValWarnings updated
    void savedToDisk(const QString & packagePath); // node files written (relayed to PackageEditor::packageSaved)

private:
    void MergeRegistryDeltaInNode(nlohmann::ordered_json * Delta, int NodeIndexInArray);
    // Cached BuildExecIndex(): the catalog scan (this bundle + every repo) is expensive and was rebuilt per node
    // section (KnownPlatforms) AND per save (Revalidate). The cache rebuilds once, invalidated whenever the bundle's
    // nodes change (SaveNodes). Returns a stable reference for callers that only read.
    const NodeIndex &              ExecIndex() const;
    void                           InvalidateExecIndex() { ExecIndexValid = false; }

    nlohmann::ordered_json         Doc;                       // working document { "NODES": [...] }
    QDir *                         PackageDir = nullptr;
    const nlohmann::ordered_json * GlobalConfigJSON = nullptr;
    QWidget *                      DialogParent = nullptr;
    std::vector<std::string>       ValErrors, ValWarnings;
    mutable NodeIndex              ExecIndexCache;            // BuildExecIndex() result, lazily (re)built by ExecIndex()
    mutable bool                   ExecIndexValid = false;
};

#endif // PACKAGEEDITORMODEL_H
