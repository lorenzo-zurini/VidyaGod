#ifndef AUTHORINGSESSIONMODEL_H
#define AUTHORINGSESSIONMODEL_H

#include <QObject>
#include <QStringList>
#include <QThread>

#include <memory>
#include <string>

#include "authoringsession.h"

class PackageEditorModel;

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
    void runWindows(QString exe, QString runnerId);   // wine tool: (re)build under runnerId, then run exe
    void runExe(QString exe);             // guest command on the live (wine) runtime — regedit.exe / explorer.exe
    void refreshDelta();
    void captureFiles(QStringList roots, QString destDirAbs);   // each root captured at its own level (parent stripped)
    void scanRegistry();                  // baseline-vs-now diff → the changed-key list + the full delta
    void end();

signals:
    // `runners` carries the wine-family runners offered by the "Run Windows program" tool (NOT a session-level runner).
    void started(bool ok, QString runtimePath, QString contentRoot, bool isWine, QString runnerId, QStringList runners);
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
    AuthoringSessionModel(PackageEditorModel * Editor, std::string TargetNodeId, QObject * parent = nullptr);
    ~AuthoringSessionModel() override;

    QString     targetNode() const { return QString::fromStdString(TargetNodeId); }
    QStringList bundleNodeIds() const;

public slots:
    void start();                                       // open the bare runner-less runtime
    void runWindows(const QString & Exe, const QString & RunnerId);   // the "Run Windows program" tool
    void runGuest(const QString & GuestCmd);            // regedit.exe / explorer.exe (post-wine)
    void refreshDelta();
    void captureFiles(const QStringList & Roots, const QString & TargetNode, const QString & DestName, const QString & Target);
    void scanRegistry();                                                            // diff → populate the registry tree
    void captureSelectedRegistry(const QStringList & RegPaths, const QString & TargetNode);  // merge the picked keys

signals:
    // → worker (queued)
    void requestStart(QString configDump, QString bundlePath, QString nodeId);
    void requestRunWindows(QString exe, QString runnerId);
    void requestRunExe(QString exe);
    void requestRefresh();
    void requestCaptureFiles(QStringList roots, QString destDirAbs);
    void requestScanRegistry();
    void requestEnd();
    // → subwidgets
    void busyChanged(bool busy, QString what);
    void sessionReady(QString runtimePath, QString contentRoot, bool isWine);
    void runnersChanged(QStringList runners, QString current);
    void deltaChanged(QStringList paths);
    void registryTreeChanged(QStringList regPaths);
    void filesCaptured(QStringList roots);              // the file roots just captured (→ tint them green)
    void registryCaptured(QStringList keys);            // the registry keys just captured (→ tint them green)
    void captured(QString message);
    void failed(QString message);

private slots:
    void onStarted(bool ok, QString runtimePath, QString contentRoot, bool isWine, QString runnerId, QStringList runners);
    void onDelta(QStringList paths);
    void onRunFinished(bool ok);
    void onFilesCopied(int count);
    void onRegistryScan(QString deltaJsonDump, QStringList regPaths);

private:
    PackageEditorModel *   Editor = nullptr;
    std::string            TargetNodeId;
    QThread                Thread;
    AuthoringWorker *      Worker = nullptr;
    QString                PendTargetNode, PendDestName, PendTarget;  // file-capture context awaiting the worker result
    QStringList            PendRoots;                                  // the file roots being captured (echoed back on success)
    nlohmann::ordered_json LastRegDelta = nlohmann::ordered_json::array();  // the scanned registry diff (filtered on capture)
};

#endif // AUTHORINGSESSIONMODEL_H
