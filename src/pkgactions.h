#ifndef PKGACTIONS_H
#define PKGACTIONS_H

#include "pkggraph.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class PackageEditorModel;
class PkgCanvas;
class QProcess;
class QWidget;

// ---------------------------------------------------------------------------
// PkgActions — performs the node actions the canvas asks for. The canvas draws buttons and reports clicks; it
// does no IO and knows nothing about the engine. Everything that touches disk, spawns a process, opens a dialog
// or builds a runtime happens HERE, so the canvas stays headlessly testable.
//
// Every heavy action follows the same contract: begin it on the canvas (which locks the node and swaps its
// buttons for a progress bar), report progress as real work completes, and end it on success, failure OR
// cancel. A node must never be left locked — so the end call is paired with the begin on every path out.
//
// Cancellation is honest rather than cosmetic: a process-backed action kills its QProcess, and an in-process
// one flips an atomic the worker polls. An action that genuinely cannot be interrupted does not offer Cancel.
// ---------------------------------------------------------------------------
class PkgActions : public QObject
{
    Q_OBJECT
public:
    PkgActions(PackageEditorModel * model, PkgCanvas * canvas, QWidget * dialogParent, QObject * parent = nullptr);
    ~PkgActions() override;

    //The destructive actions ASK before they delete anything, which makes them untestable from a headless
    //test: a modal dialog blocks forever with nobody to click it. Injecting the prompt lets a test drive the
    //real conversion code — including its rollback paths — instead of leaving the riskiest file-deleting code
    //in the project uncovered.
    using ConfirmFn = std::function<bool(const QString &Title, const QString &Body)>;
    void setConfirmHandler(ConfirmFn fn) { Confirm = std::move(fn); }
    //...and the same for the outcome messages. An action that refuses (name already taken, tool missing,
    //verify failed) reports by dialog, which blocks a headless run just as hard as a question does — so the
    //refusal paths, which are exactly the ones that must not delete anything, were untestable.
    using NotifyFn = std::function<void(const QString &Title, const QString &Body)>;
    void setNotifyHandler(NotifyFn fn) { Notify = std::move(fn); }
    //...and the same for the file dialogs. `getOpenFileName` blocks a headless run exactly like a question
    //does, so every action that begins with a pick — browse, cover, import .reg — was unreachable from a test,
    //including the paths that REFUSE the pick (outside the bundle, unreadable file, a payload of the wrong
    //shape). `WantDir` distinguishes the folder pick from the file pick; empty means the author cancelled.
    using PickFn = std::function<QString(const QString &Title, const QString &Dir, const QString &Filter,
                                         bool WantDir)>;
    void setPickHandler(PickFn fn) { Pick = std::move(fn); }
    //Point the conversions at different programs. Exists so a test can prove that a tool which cannot run
    //unlocks the node and leaves the source intact — the path that previously skipped the completion handler
    //entirely and left a node locked forever against an already-deleted file.
    void setToolNamesForTest(const QString &zip, const QString &unzip) { ZipTool = zip; UnzipTool = unzip; }

public slots:
    void perform(const QString & nodeId, const QString & action);
    void cancel(const QString & nodeId);
    //Probe the bundle's zips off-thread and push "deflate" hints, so the ⚠ re-store button appears only where it
    //is actually needed (a STORE zip must not nag).
    void refreshHints();

private:
    int  indexOf(const std::string & nodeId) const;
    void browsePath(const std::string & nodeId);
    void browseCover(const std::string & nodeId);
    void convertZipDir(const std::string & nodeId, bool ToZip);
    void reStore(const std::string & nodeId);
    void testLaunch(const std::string & nodeId);
    void openCapture(const std::string & nodeId, int mode);
    void findUsages(const std::string & nodeId);
    void importReg(const std::string & nodeId);
    //Exposed for testing: the .reg text parser, without the file dialog. `deletions` counts the
    //`-`-form entries a write-only RegEdit layer cannot represent.
public:
    static std::vector<PkgGraph::RegRow> ParseRegExport(const QString & text, int * deletions);
private:
    void makeDelta(const std::string & nodeId);     // diff against a Content parent -> a .vgdelta
    void undelta(const std::string & nodeId);       // the exact inverse: .vgdelta -> a plain zip

    //Run a command, streaming its per-file output into the node's progress bar. `Total` is the expected line
    //count (0 ⇒ indeterminate). Calls Done(ok) on the GUI thread; always ends the node's action.
    void runWithProgress(const std::string & nodeId, const QString & what, const QString & program,
                         const QStringList & args, const QString & workDir, int total,
                         std::function<void(bool)> done);

    //Returns true to proceed. Defaults to a real modal question.
    bool ask(const QString & Title, const QString & Body);
    void tell(const QString & Title, const QString & Body);
    QString pick(const QString & Title, const QString & Dir, const QString & Filter, bool WantDir = false);
    ConfirmFn            Confirm;
    NotifyFn             Notify;
    PickFn               Pick;
    QString              ZipTool   = "zip";
    QString              UnzipTool = "unzip";
    PackageEditorModel * Model  = nullptr;
    PkgCanvas *          Canvas = nullptr;
    QWidget *            Parent = nullptr;
    //Cleared in the destructor: a worker thread running a multi-minute diff must be able to tell that the
    //editor went away before it touches this object or the canvas again.
    std::shared_ptr<std::atomic<bool>>                          Alive_ = std::make_shared<std::atomic<bool>>(true);
    std::map<std::string, QProcess *>                           Procs;    // process-backed actions, for kill()
    std::map<std::string, std::shared_ptr<std::atomic<bool>>>   Aborts;   // in-process actions, polled by the worker
};

#endif // PKGACTIONS_H
