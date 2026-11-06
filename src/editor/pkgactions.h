#ifndef PKGACTIONS_H
#define PKGACTIONS_H

#include "pkggraph.h"
#include "manifestmodel.h"   // NodeIndex (a test launch hands the launch window its own)

#include <QObject>
#include <QString>

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class PackageEditorModel;
namespace PkgDoc { class Document; }
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
    //...and the same for a choice among options (a folder or a zip; which program starts the game): the index chosen,
    //-1 when the author cancelled.
    using ChooseFn = std::function<int(const QString &Title, const QString &Text, const QStringList &Options)>;
    void setChooseHandler(ChooseFn fn) { Choose = std::move(fn); }
    //Point the conversions at different programs. Exists so a test can prove that a tool which cannot run
    //unlocks the node and leaves the source intact — the path that previously skipped the completion handler
    //entirely and left a node locked forever against an already-deleted file.
    void setToolNamesForTest(const QString &zip, const QString &unzip) { ZipTool = zip; UnzipTool = unzip; }

signals:
    //A finding worth a line in the status bar, not a dialog (what auto-detect set, why a drop was refused).
    void status(const QString & text);
    //Test launch: open the launch window — the player's own door — on this node of this index (its own copy: the
    //window is modeless and outlives the editor's next save). The app opens it (MainWindow's editor hook).
    void testLaunchRequested(std::shared_ptr<const NodeIndex> index, const QString & nodeId);

public slots:
    void perform(const QString & nodeId, const QString & action);
    //Files dragged in from the file manager onto a node: a folder or a zip becomes a ZIP layer (packed / copied in),
    //an image the tile's cover, a .reg export a REG layer; anything else is refused, saying so.
    void dropFiles(const QString & nodeId, const QStringList & paths);
    void cancel(const QString & nodeId);
    //Probe the bundle's zips off-thread and push "deflate" hints, so the ⚠ re-store button appears only where it
    //is actually needed (a STORE zip must not nag).
    void refreshHints();

private:
    int  indexOf(const std::string & nodeId) const;
    std::string current(std::string Handle) const;               // the node's handle after any saves since
    void commitNode(int I, nlohmann::ordered_json N, bool Persist);
    bool saveFirst(const QString & Title);
    void browsePath(const std::string & nodeId);
    void bringIn(const std::string & nodeId);
    //Add=true: the files are ADDED (an empty content layer takes them, else a new one); false: the content layer is
    //repointed (the "bring in files" action on it).
    void bringInPath(const std::string & nodeId, const QString & picked, bool folder, bool add);
    //One file for a FILE layer: copied into the package (one already in it is just named), the layer then supplies it.
    void bringInFile(const std::string & nodeId, const QString & picked, bool add);
    void coverFrom(const std::string & nodeId, const QString & picked);
    void importRegFile(const std::string & nodeId, const QString & file);
    //After files come in: a game entry still without a program gets one, when the files make it clear which.
    void autoDetect(const std::string & nodeId);
    void compareWith(const std::string & nodeId, const std::string & other);
    void pickExe(const std::string & nodeId, const std::string & where);
    void browseCover(const std::string & nodeId);
    //The node's content layer, or (Which) its folder layer of that name - a node can have more than one.
    void convertZipDir(const std::string & nodeId, bool ToZip, const std::string & Which = {});
    void reStore(const std::string & nodeId);
    void testLaunch(const std::string & nodeId);
    void openCapture(const std::string & nodeId, int mode);
    void findUsages(const std::string & nodeId);
    void importReg(const std::string & nodeId);
    //Exposed for testing: the .reg text parser, without the file dialog. `deletions` counts the
    //`-`-form entries a write-only RegEdit layer cannot represent.
public:
    static std::vector<PkgGraph::RegRow> ParseRegExport(const QString & text, int * deletions);
    //What a program runs on, from its name and its first bytes: win32 / win64 (PE machine), dos (MZ alone, .com,
    //.bat), linux64 / linux32 (ELF class, .sh, .x86_64, .AppImage); "" when it cannot tell.
    static std::string PlatformOf(const std::string & name, const std::string & head);
    //What GOG's installer does at install time (its goggame-<id>.script beside the game), as layers: a file it writes
    //(EDIT Overwrite), its registry values (one REG layer, ARCH = the game's bitness: "32"/"64"), a support file it
    //copies in (EDIT Overwrite of its text; a binary one as a FILE the caller copies into the package: Copies holds
    //source -> name), the save folder (KEEP, last). Placeholders become anchors ({app} -> %GameDir%, {userdocs} ->
    //%Documents% ...). Said is one line per layer's reason; Skipped names what cannot apply here (redistributable
    //installers, compatibility databases, GOG Galaxy's own keys).
    struct GogSteps
    {
        std::vector<nlohmann::ordered_json> Layers;
        std::vector<std::pair<std::filesystem::path, std::string>> Copies;
        std::vector<std::string> Said, Skipped;
    };
    static GogSteps GogInstallSteps(const nlohmann::ordered_json & script, const std::filesystem::path & supportDir,
                                    const std::string & arch);
    //A Unity game's <Game>_Data/app.info ("<company>\n<product>") -> the KEEP layer for where it saves (its LocalLow
    //folder, its PlayerPrefs key); null when the text is not that.
    static nlohmann::ordered_json UnityKeep(const std::string & appInfo);
    //Which of these programs is the game: the only one left once installers, uninstallers, crash reporters and
    //redistributables are set aside, or the only one at the top of the game's folder. -1 when it is not clear.
    static int LikelyProgram(const std::vector<std::string> & anchored);
    //What the game sees when this node runs: every file its fold provides (zips, folders, single files, in fold order,
    //later winning), each with the layer it comes from and the ones it covers. Notes says what is not listed (a
    //patch's rebuilt files, another package's node).
    struct SeenFile { std::string Path, From; std::vector<std::string> Under; };
    //Two versions side by side: the layers only one of them folds (by type and name), the files only one of them gives
    //the game, and the files both give but from different layers.
    struct VersionDiff { std::vector<std::string> LayersOnlyA, LayersOnlyB, FilesOnlyA, FilesOnlyB, FilesDiffer; };
    static VersionDiff Compare(const PkgDoc::Document & doc, const std::string & a, const std::string & b, const std::filesystem::path & bundle);
    //Another package's node, as the library holds it: its JSON, the folder its files resolve in, its name.
    struct OtherNode { const nlohmann::ordered_json * Node = nullptr; std::filesystem::path Bundle; std::string Label; };
    using OtherFn = std::function<OtherNode(const std::string & cid)>;
    //Other: how the walk reaches another package's node a fold contains - its files listed from its own folder, by
    //its own name; without it such a node is only noted.
    static std::vector<SeenFile> GameSees(const PkgDoc::Document & doc, const std::string & handle, const std::filesystem::path & bundle,
                                          std::vector<std::string> * notes = nullptr, const OtherFn & other = {});
private:
    void makeDelta(const std::string & nodeId);     // diff against a Content parent -> a .vgdelta
    void undelta(const std::string & nodeId);       // the exact inverse: .vgdelta -> a plain zip

    //Run a command, streaming its per-file output into the node's progress bar. `Total` is the expected line
    //count (0 ⇒ indeterminate). Calls Done(ok) on the GUI thread; always ends the node's action.
    //After a folder is brought in: offer GOG's install steps (its goggame-<id>.script) as layers on the node.
    void gogSteps(const std::string & nodeId, const std::filesystem::path & folder);
    void runWithProgress(const std::string & nodeId, const QString & what, const QString & program,
                         const QStringList & args, const QString & workDir, int total,
                         std::function<void(bool)> done);

    //Returns true to proceed. Defaults to a real modal question.
    bool ask(const QString & Title, const QString & Body);
    void tell(const QString & Title, const QString & Body);
    QString pick(const QString & Title, const QString & Dir, const QString & Filter, bool WantDir = false);
    //Buttons for a few options, a list for many.
    int choose(const QString & Title, const QString & Text, const QStringList & Options, bool AsList);
    ConfirmFn            Confirm;
    ChooseFn             Choose;
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
    std::map<std::string, std::string>                          Moves;    // handles renamed by saves (old -> new)
    std::map<std::string, QProcess *>                           Procs;    // process-backed actions, for kill()
    std::map<std::string, std::shared_ptr<std::atomic<bool>>>   Aborts;   // in-process actions, polled by the worker
};

#endif // PKGACTIONS_H
