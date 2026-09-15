#ifndef PACKAGEEDITORMODEL_H
#define PACKAGEEDITORMODEL_H

#include <QObject>
#include <QDir>
#include <QString>

#include <nlohmann/json.hpp>

#include "manifestmodel.h"   // NodeIndex

#include <map>
#include <string>
#include <vector>

class QWidget;

// ---------------------------------------------------------------------------
// PackageEditorModel — the state/signal hub for the node-native bundle editor (the AppModel-style central structure
// for PackageEditor). It OWNS the working document (`{ "NODES":[ <node>, ... ] }`, each node carrying an editor-only
// "__FILE__" tag), the bundle `QDir`, and the borrowed GlobalConfigJSON, plus the latest validation
// results. It does all the NON-UI work: node file I/O, validation, the catalog/exec-index queries, and the
// authoring runs (build a container + execute / analyze registry against the live launch engine). The editor's
// widgets read state through it, call its mutators, and react to its signals — they never touch each other.
//
// A QWidget* DialogParent is held solely to parent the modal dialogs the authoring runs raise (mirrors how
// DownloadManager holds a dialog parent). The model itself only READS GlobalConfigJSON, but it is borrowed
// non-const because a test-launch hands it to PreLaunchWindow, which persists the player's CustomVar choices
// into USERSETTINGS.
// ---------------------------------------------------------------------------
class PackageEditorModel : public QObject
{
    Q_OBJECT
public:
    PackageEditorModel(nlohmann::ordered_json * globalConfig, QWidget * dialogParent, QObject * parent = nullptr);
    ~PackageEditorModel() override;

    // ── State access (widgets read directly; the model owns the lifetime) ──
    nlohmann::ordered_json &       doc()                 { return Doc; }
    const nlohmann::ordered_json & doc() const           { return Doc; }
    //THIS MACHINE's canvas-position override: NODE_ID -> [x,y], held in GlobalConfig under EDITORLAYOUT and
    //keyed by bundle PATH (PackageCatalog::EditorLayoutKey — the same key publishing looks up). The author's DEFAULT position is the node's own POS, stamped at publish
    //time; this is only what has been dragged since. Keeping drags out of the package is the whole point — a
    //Meta-CID is minted IN PLACE over the node files, so a position written back into a node would change the
    //package's bytes, and its CID, for every peer, on every mouse-up.
    nlohmann::ordered_json &       layout()              { return Layout; }
    //NOT const: it writes into GlobalConfigJSON and flushes it to disk.
    void SaveLayout();
    QDir *                         packageDir() const    { return PackageDir; }
    nlohmann::ordered_json *       globalConfig() const  { return GlobalConfigJSON; }
    const std::vector<std::string> & validationErrors()   const { return ValErrors; }
    const std::vector<std::string> & validationWarnings() const { return ValWarnings; }
    bool validated() const { return Validated; }   // false = not checked since last edit (validation is on-demand)

    // ── Bundle open + node file I/O (one file per node) ──
    void initPackage(const QString & preselectedPath, QWidget * dirPickerParent);   // pick dir (if empty) + LoadNodes
    void LoadNodes();
    void LoadLayout();
    void SaveNodes();                                              // write files → emit savedToDisk + validationChanged
    QString FileForNode(const nlohmann::ordered_json & Node) const;
    //Replace one node's whole JSON (preserving its __FILE__ tag), persist, and request a structural rebuild. Used
    //by the raw-JSON tab's Save Node.
    void replaceNodeJson(int nodeIndex, nlohmann::ordered_json node);
    //Live content edit from the JSON panel: replace the node, persist, and signal a CONTENT change — NOT a
    //structural documentReloaded (which rebuilds the shell and would destroy the JSON editor mid-keystroke). The
    //canvas repaints from the doc; only the graph cache needs poking.
    void updateNodeLive(int nodeIndex, nlohmann::ordered_json node);
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
    // True if "Test launch" can do something for this node: it's launchable (has a DeclareExec) OR a launchable in the
    // bundle includes it (so the node can be tested in the context of the game that pulls it in). Uses the cached index.
    bool NodeTestable(const std::string & NodeId) const;

    // ── Authoring Session hooks (the held-open AuthoringSession window captures back into the document) ──
    QString                  packagePath() const;                        // the edited bundle dir ("" if none open)
    std::vector<std::string> bundleNodeIds() const;                      // NODE_IDs declared in THIS bundle (target picker)
    //Create a node from a payload, parented at `Parents`, persist + rebuild; returns its NODE_ID. A capture IS a
    //node in the flat schema — one layer of one TYPE — so a capture creates one rather than appending into
    //somebody else's. `IdHint` seeds a unique id.
    std::string createNode(nlohmann::ordered_json Payload, const std::vector<std::string> & Parents,
                           const std::string & IdHint);

signals:
    void documentReloaded();                       // structural change — views rebuild
    void nodeContentChanged();                     // a node's CONTENT changed (live JSON edit); repaint, no rebuild
    void validationChanged();                      // ValErrors/ValWarnings updated
    void savedToDisk(const QString & packagePath); // node files written (relayed to PackageEditor::packageSaved)

private:
    // Cached BuildExecIndex(): the catalog scan (this bundle + every repo) is expensive and was rebuilt per node
    // section (KnownPlatforms) AND per save (Revalidate). The cache rebuilds once, invalidated whenever the bundle's
    // nodes change (SaveNodes). Returns a stable reference for callers that only read.
    const NodeIndex &              ExecIndex() const;
    void                           InvalidateExecIndex() { ExecIndexValid = false; }

    nlohmann::ordered_json         Doc;                       // working document { "NODES": [...] }
    nlohmann::ordered_json         Layout = nlohmann::ordered_json::object();   // NODE_ID -> [x,y], see layout()
    QDir *                         PackageDir = nullptr;
    //Entries in a multi-node file that are not nodes. Held so SaveNodes can write them back rather
    //than erasing them when it rewrites the file from the nodes it loaded.
    std::map<std::string, nlohmann::ordered_json> Carried;
    nlohmann::ordered_json *       GlobalConfigJSON = nullptr;
    QWidget *                      DialogParent = nullptr;
    std::vector<std::string>       ValErrors, ValWarnings;
    bool                           Validated = false;   // set by Revalidate(); cleared on any edit/load
    mutable NodeIndex              ExecIndexCache;            // BuildExecIndex() result, lazily (re)built by ExecIndex()
    mutable bool                   ExecIndexValid = false;
};

#endif // PACKAGEEDITORMODEL_H
