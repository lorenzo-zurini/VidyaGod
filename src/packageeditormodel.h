#ifndef PACKAGEEDITORMODEL_H
#define PACKAGEEDITORMODEL_H

#include <QObject>
#include <QDir>
#include <QString>

#include <nlohmann/json.hpp>

#include "manifestmodel.h"   // NodeIndex
#include "pkgdoc.h"          // the document

#include <map>
#include <memory>
#include <string>
#include <vector>

class QThread;
class QTimer;
class QWidget;

// ---------------------------------------------------------------------------
// PackageEditorModel — the package editor's state: the package being edited (a PkgDoc::Document), this machine's
// canvas layout for it (GlobalConfig EDITORLAYOUT, never the package), validation, and the questions the canvas asks
// about the rest of the library (what another package's node is called, which nodes can be wired in).
//
// Edits live in the document until SAVED; saving is a generation-6 re-mint (see PkgDoc) that follows renamed nodes
// through the library and the instances. Nothing about looking at a package writes to it.
// ---------------------------------------------------------------------------
class PackageEditorModel : public QObject
{
    Q_OBJECT
public:
    PackageEditorModel(nlohmann::ordered_json * globalConfig, QWidget * dialogParent, QObject * parent = nullptr);
    ~PackageEditorModel() override;

    PkgDoc::Document &             doc()                 { return Doc; }
    const PkgDoc::Document &       doc() const           { return Doc; }
    QDir *                         packageDir() const    { return PackageDir; }
    QString                        packagePath() const;
    nlohmann::ordered_json *       globalConfig() const  { return GlobalConfigJSON; }

    // ---- the package on disk ----------------------------------------------------------------------------------
    void initPackage(const QString & preselectedPath, QWidget * dirPickerParent);   // pick a dir (if none) + load
    void LoadNodes();                              // (re)read the package; loses unsaved edits (the caller asks first)
    //Save the document (a re-mint + cascade). False with Error when it cannot be saved (a cycle, a write failure);
    //then nothing on disk changed that the report does not list.
    bool Save(QString * Error = nullptr);
    bool isDirty() const { return Doc.Dirty(); }
    //Handles the last Save renamed (old -> new): the canvas, the JSON panel and a pending run follow their nodes.
    const std::map<std::string, std::string> & lastRenames() const { return LastRenames; }
    void SaveLayout();                             // this machine's node positions -> GlobalConfig
    //Replace one node's JSON (the JSON panel): one undo step, the canvas repaints.
    void replaceNode(const std::string & Handle, nlohmann::ordered_json Node);

    // ---- validation -------------------------------------------------------------------------------------------
    void Revalidate();                             // the DOCUMENT as it is now (unsaved edits included), blocking
    //The same check on a worker thread, over a snapshot of the document: validateSoon() once edits settle (every edit
    //asks), validateNow() at once. validationChanged() when the result is in; a result for a document that changed
    //meanwhile is dropped and the check runs again, so what is shown is always about what is on the canvas.
    void validateSoon();
    void validateNow();
    bool validating() const { return ValThread != nullptr; }
    const std::vector<std::string> & validationErrors()   const { return ValErrors; }
    const std::vector<std::string> & validationWarnings() const { return ValWarnings; }
    bool validated() const { return Validated; }
    //Every message about a node, by that node's handle (the canvas draws them on the node).
    std::map<std::string, std::vector<std::string>> issuesByHandle() const;
    //The handle a validation message is about ("" if none of this package's).
    std::string handleInMessage(const std::string & Message) const;

    // ---- the rest of the library --------------------------------------------------------------------------------
    NodeIndex BuildExecIndex() const;              // the library, with this package's nodes AS EDITED
    struct External { std::string Label, Package, PackageDir; };
    External externalInfo(const std::string & Cid) const;          // another package's node, by CID
    struct Offer { std::string Cid, Label, Package; };
    std::vector<Offer> offers() const;             // every node another package offers for wiring in
    std::vector<std::string> KnownPlatforms();

    // ---- authoring ----------------------------------------------------------------------------------------------
    void RunInNode(const std::string & NodeId, const std::string & Exe = "");
    bool NodeTestable(const std::string & NodeId) const;
    std::vector<std::string> bundleNodeIds() const;                // this package's node LABELs (target picker)
    //Create a node from a payload containing `Parents` (handles), as a new draft; returns its handle. A capture IS a
    //node — one layer of one type — so a capture creates one rather than appending into somebody else's.
    std::string createNode(nlohmann::ordered_json Payload, const std::vector<std::string> & Parents,
                           const std::string & Label);

signals:
    void documentReloaded();                       // the whole document was replaced (load, reload)
    void nodeContentChanged();                     // a node's content changed outside the canvas
    void validationChanged();
    void dirtyChanged(bool dirty);
    void savedToDisk(const QString & packagePath);
    void handlesRenamed();                         // lastRenames() changed (emitted before savedToDisk)

public slots:
    void noteEdited();                             // the canvas edited the document: layout, dirty, index follow
    void notifyNodeChanged() { noteEdited(); emit nodeContentChanged(); }   // edited from outside the canvas

private:
    const NodeIndex & ExecIndex() const;           // BuildExecIndex, cached until the document changes
    const NodeIndex & LibraryIndex() const;        // the library as on disk (the other packages), cached
    void LoadLayout();

    PkgDoc::Document         Doc;
    std::map<std::string, PkgDoc::Pos> SavedLayout;   // what GlobalConfig holds, to write only on change
    std::map<std::string, std::string> LastRenames;
    QDir *                   PackageDir = nullptr;
    nlohmann::ordered_json * GlobalConfigJSON = nullptr;
    QWidget *                DialogParent = nullptr;
    struct ValResult { std::vector<std::string> Errors, Warnings; };
    static ValResult Validate(const NodeIndex & Idx, const std::vector<std::string> & Handles);
    std::shared_ptr<const NodeIndex> ExecSnapshot() const;
    std::vector<std::string> bundleHandles() const { std::vector<std::string> H; for (int I = 0; I < Doc.Count(); ++I) H.push_back(Doc.Handle(I)); return H; }
    void ValidationDone();

    std::vector<std::string> ValErrors, ValWarnings;
    bool                     Validated = false;
    QTimer *                 ValTimer = nullptr;       // validateSoon's settle delay
    QThread *                ValThread = nullptr;      // the check running, if any
    std::shared_ptr<ValResult> ValOut;                 // what it found (written by the worker, read once it finished)
    uint64_t                 ValContentRev = 0;        // the document the running check is about
    bool                     ValAgain = false;         // edited while checking: check again when it finishes
    bool                     WasDirty = false;
    //Immutable once built, so a background check can keep the one it started on while the canvas moves on.
    mutable std::shared_ptr<const NodeIndex> ExecIndexCache;
    mutable uint64_t         ExecIndexRev = 0;         // the ContentRevision it was built at (0 = stale)
    mutable std::unique_ptr<NodeIndex> LibraryCache;
};

#endif // PACKAGEEDITORMODEL_H
