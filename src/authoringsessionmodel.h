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
    void start(QString configDump, QString bundlePath, QString nodeId, QStringList runnerChain);
    void runExe(QString exe);             // host path or guest command (regedit.exe / explorer.exe)
    void refreshDelta();
    void captureFiles(QStringList rels, QString strip, QString destDirAbs);
    void captureRegistry();
    void end();

signals:
    void started(bool ok, QString runtimePath, QString contentRoot, bool isWine, QString runnerId, QStringList runners);
    void delta(QStringList paths);
    void runFinished(bool ok);
    void filesCopied(int count);
    void registryDelta(QString deltaJsonDump);
    void ended();

private:
    void emitDeltaList();
    std::unique_ptr<AuthoringSession> Session;
    std::string TargetNodeId;
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
    void start();                                       // first build (auto-resolve the runner)
    void switchRunner(const QString & RunnerId);        // rebuild under a chosen runner
    void runExe(const QString & HostPath);
    void runGuest(const QString & GuestCmd);            // regedit.exe / explorer.exe
    void refreshDelta();
    void captureFiles(const QStringList & Rels, const QString & TargetNode,
                      const QString & DestName, const QString & Strip, const QString & Target);
    void captureRegistry(const QString & TargetNode);

signals:
    // → worker (queued)
    void requestStart(QString configDump, QString bundlePath, QString nodeId, QStringList chain);
    void requestRunExe(QString exe);
    void requestRefresh();
    void requestCaptureFiles(QStringList rels, QString strip, QString destDirAbs);
    void requestCaptureRegistry();
    void requestEnd();
    // → subwidgets
    void busyChanged(bool busy, QString what);
    void sessionReady(QString runtimePath, QString contentRoot, bool isWine);
    void runnersChanged(QStringList runners, QString current);
    void deltaChanged(QStringList paths);
    void captured(QString message);
    void failed(QString message);

private slots:
    void onStarted(bool ok, QString runtimePath, QString contentRoot, bool isWine, QString runnerId, QStringList runners);
    void onDelta(QStringList paths);
    void onRunFinished(bool ok);
    void onFilesCopied(int count);
    void onRegistryDelta(QString deltaJsonDump);

private:
    PackageEditorModel * Editor = nullptr;
    std::string          TargetNodeId;
    QThread              Thread;
    AuthoringWorker *    Worker = nullptr;
    QString              PendTargetNode, PendDestName, PendTarget;   // capture context awaiting the worker result
};

#endif // AUTHORINGSESSIONMODEL_H
