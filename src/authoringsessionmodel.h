#ifndef AUTHORINGSESSIONMODEL_H
#define AUTHORINGSESSIONMODEL_H

#include <QMap>
#include <QObject>
#include <QStringList>
#include <QThread>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "authoringsession.h"

class PackageEditorModel;

// What a session is FOR. The runtime it mounts is the same in every case; the mode decides which guest tool it
// opens for you, which half of the window is shown, and which kind of node the capture becomes.
enum class CaptureMode
{
    Setup,      // run an installer; capture files AND registry (up to two nodes)
    Files,      // open a file manager on the runtime; capture what appeared → a Content node
    Registry,   // open regedit on the prefix; capture what changed → a RegEdit node
};

// Where a capture will mount, for the window's preview. The runtime's anchor layouts travel from the worker thread as
// "%Anchor%\tlayout" lines (EncodeAnchors → ParseAnchors); a TARGET naming an anchor mounts where the runtime lays that
// anchor out, any other TARGET under the content root (MountBase).
namespace CapturePreview
{
QStringList EncodeAnchors(const std::vector<std::pair<std::string, std::string>> &Layouts);
QMap<QString, QString> ParseAnchors(const QStringList &Lines);
QString MountBase(QString Target, const QString &ContentRoot, const QMap<QString, QString> &Anchors);
//Whether one ticked folder is captured as its CONTENTS: it is the folder the TARGET mounts at (the game's folder ticked
//for TARGET %GameDir%), or the node waits for its files at Target. Otherwise each root lands at its own level, and the
//game's folder would arrive as a <uid>\ folder inside itself.
bool CapturesContents(const QStringList &Roots, const QString &Target, const QString &WaitTarget, const QString &ContentRoot,
                      const QMap<QString, QString> &Anchors);
//The "Run Windows program" tool's runner choices, as "label\tid" lines, newest version first (geproton_11_7 before
//geproton_10_34 before geproton_9_27): a person picks a runner by its name, the session by its id.
QStringList RunnerChoices(const std::vector<std::pair<std::string, std::string>> &IdLabel);
//Where the setup capture of the game's folder goes: the place the anchor waits for its files ("%GameDir%" from a
//waiting layer's FILES/%GameDir%), "" when it waits for nothing; and the captured folder named after the node.
QString WaitingTarget(const nlohmann::ordered_json &Anchor);
QString CaptureName(const nlohmann::ordered_json &Anchor, const std::string &Id);
//The preview of a capture of several roots, in at most MaxLines lines: the first ones, then how many more. A long
//list would push the tree it describes off the window.
QString PreviewLines(const QStringList &Lines, int MaxLines = 6);
//A capture's folder name that no folder of the package holds yet: Name, else "Name 2", "Name 3"...
QString FreeName(const QString &Name, const QString &PackageDir);
//A captured folder's place in the guest ("pfx/drive_c/GOG Games/X" -> "C:\GOG Games\X"; "" outside drive_c), and
//the anchor it now has in Windows spelling ("%GameDir%" captured as contents -> "%GameDir:winpath%"; a folder captured
//at its level under "%Documents%/Saves" -> "%Documents:winpath%\Saves\<its name>").
std::string GuestPathOf(const std::string &RootRel);
std::string AnchorWinpath(const std::string &Target, const std::string &RootRel, bool Contents);
//Every captured folder's guest path in a value, case-insensitively, as its anchor (an installer writes where it
//installed; the node names the place by anchor).
std::string Reanchor(std::string Value, const std::vector<std::pair<std::string, std::string>> &GuestToAnchor);
//A captured folder as the session may already spell it in a registry value, by each anchor of the runtime it lies
//under ("pfx/drive_c/Program Files (x86)/KONAMI/SH3" with %ProgramFiles32% laid out at "pfx/drive_c/Program Files
//(x86)" -> "%ProgramFiles32%\KONAMI\SH3"); none under none.
std::vector<std::string> AnchoredGuestsOf(const std::string &RootRel, const QMap<QString, QString> &Layouts);
//What a capture names by anchor: each captured root's guest path -> its anchor, and - captured at its level - its
//stripped parent folder -> the target itself. Each in both spellings: C:\... and, under an anchor of Layouts, the
//anchored one the session's registry capture already wrote.
std::vector<std::pair<std::string, std::string>> GuestAnchorsFor(const QStringList &Roots, const std::string &Target, bool Contents,
                                                                 const QMap<QString, QString> &Layouts = {});
}

// ---------------------------------------------------------------------------
// AuthoringWorker — owns the AuthoringSession and runs every heavy op (wineboot, installer runs, multi-GB captures,
// huge write-delta walks) on its OWN thread, so the GUI never blocks. It lives on the worker thread; the model talks
// to it only through queued signal/slot connections. Cross-thread arguments are Qt value types only (QString/
// QStringList/int/bool) — the catalog NodeIndex is rebuilt here from the config + bundle path, so no custom
// metatypes need registering.
// ---------------------------------------------------------------------------
class AuthoringWorker : public QObject
{
    Q_OBJECT
public slots:
    void start(QString configDump, QString bundlePath, QString nodeId);   // open a BARE runner-less runtime
    void runWindows(QString exe, QString runnerId, QStringList args);   // wine tool: (re)build under runnerId, then run exe
    void runExe(QString exe);             // guest command on the live (wine) runtime — regedit.exe / explorer.exe
    void refreshDelta();
    //Each root captured at its own level (parent stripped) - or, Contents, its contents at the top (the root IS the
    //folder the anchor waits for, e.g. the installed game folder that becomes %GameDir%).
    void captureFiles(QStringList roots, QString destDirAbs, bool contents);
    void scanRegistry();                  // baseline-vs-now diff → the changed-key list + the full delta
    void end();

signals:
    // `runners` carries the wine-family runners offered by the "Run Windows program" tool (NOT a session-level runner).
    void started(bool ok, QString runtimePath, QString contentRoot, bool isWine, QString runnerId, QStringList runners,
                 QStringList anchorLayouts = {});   // "%Anchor%\tlayout path" per anchor of the runtime
    void delta(QStringList paths);
    void runFinished(bool ok);
    void filesCopied(int count);
    void registryScan(QString deltaJsonDump, QStringList regPaths);
    void ended();

private:
    void emitDeltaList();
    void emitSessionInfo();               // (re)broadcast the current runtime's path/content-root/wine-flag + wine runners
    std::unique_ptr<AuthoringSession> Session;
    std::string TargetNodeId;
    QStringList WineRunners;               // wine-family runners usable on this machine (the Run-Windows tool's choices)
};

// ---------------------------------------------------------------------------
// AuthoringSessionModel — the GUI-thread hub. Subwidgets talk ONLY to this (never to each other): they call its slots
// and react to its signals. It forwards heavy work to the worker (queued) and, when results return, applies captures
// back into the editor's document on the GUI thread (PackageEditorModel). Owns the worker thread; tearing the model
// down ends the session (a blocking unmount — save-safety) before the thread joins.
// ---------------------------------------------------------------------------
class AuthoringSessionModel : public QObject
{
    Q_OBJECT
public:
    //`AnchorNodeId` is the point in the chain the runtime is built up to, AND the parent of whatever the capture
    //creates — "capture at this point" means exactly "the new node's PARENTS is this node".
    AuthoringSessionModel(PackageEditorModel * Editor, std::string AnchorNodeId, CaptureMode Mode,
                          QObject * parent = nullptr);
    CaptureMode mode() const { return Mode; }
    ~AuthoringSessionModel() override;

    QString     targetNode() const { return QString::fromStdString(TargetNodeId); }
    QStringList bundleNodeIds() const;

public slots:
    void start();                                       // open the bare runner-less runtime
    void runWindows(const QString & Exe, const QString & RunnerId, const QStringList & Args = {});   // the "Run Windows program" tool
    void runGuest(const QString & GuestCmd);            // regedit.exe / explorer.exe (post-wine)
    void refreshDelta();
    void captureFiles(const QStringList & Roots, const QString & DestName, const QString & Target);
    void scanRegistry();                                                            // diff → populate the registry tree
    void captureSelectedRegistry(const QStringList & RegPaths);   // the picked keys become a new RegEdit node

signals:
    // → worker (queued)
    void requestStart(QString configDump, QString bundlePath, QString nodeId);
    void requestRunWindows(QString exe, QString runnerId, QStringList args);
    void requestRunExe(QString exe);
    void requestRefresh();
    void requestCaptureFiles(QStringList roots, QString destDirAbs, bool contents);
    void requestScanRegistry();
    void requestEnd();
    // → subwidgets
    void busyChanged(bool busy, QString what);
    void sessionReady(QString runtimePath, QString contentRoot, bool isWine, QStringList anchorLayouts);
    void runnersChanged(QStringList runners, QString current);
    void deltaChanged(QStringList paths);
    void registryTreeChanged(QStringList regPaths);
    void filesCaptured(QStringList roots);              // the file roots just captured (→ tint them green)
    void registryCaptured(QStringList keys);            // the registry keys just captured (→ tint them green)
    void captured(QString message);
    void nodeCreated(QString nodeId);                   // a capture produced this node (parented at the anchor)
    void failed(QString message);

private slots:
    void onStarted(bool ok, QString runtimePath, QString contentRoot, bool isWine, QString runnerId, QStringList runners,
                   QStringList anchorLayouts);
    void onDelta(QStringList paths);
    void onRunFinished(bool ok);
    void onFilesCopied(int count);
    void onRegistryScan(QString deltaJsonDump, QStringList regPaths);

private:
    PackageEditorModel *   Editor = nullptr;
    std::string            TargetNodeId;               // the anchor: runtime built to here, captures parented here
    CaptureMode            Mode = CaptureMode::Setup;
    QThread                Thread;
    AuthoringWorker *      Worker = nullptr;
    QString                PendDestName, PendTarget;   // file-capture context awaiting the worker result
    QStringList            PendRoots;                                  // the file roots being captured (echoed back on success)
    bool                   PendContents = false;                       // ...captured as contents (the anchor's waiting files)
    std::vector<std::pair<std::string, std::string>> GuestAnchors;     // captured folders' guest paths -> their anchors
    QMap<QString, QString> AnchorLayouts;                              // the runtime's anchors -> where it lays them out
    QString ContentRoot;                                                // the runtime's content root (pfx/drive_c/<uid>)
    nlohmann::ordered_json LastRegDelta = nlohmann::ordered_json::array();  // the scanned registry diff (filtered on capture)
};

#endif // AUTHORINGSESSIONMODEL_H
