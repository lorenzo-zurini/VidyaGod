#include "authoringsessionmodel.h"
#include "packageeditormodel.h"
#include "manifestmodel.h"     // NodeIndex / Node / ScanBundleNodes / MachinePlatform
#include "packagecatalog.h"    // RepositoryDirs

#include <QDir>

#include <utility>

// ============================================================================ worker (off-thread)

void AuthoringWorker::emitDeltaList()
{
    QStringList D;
    if (Session && Session->Live())
        for (const std::string & Rel : AuthoringSession::EnumerateDelta(Session->WriteLayerPath()))
            D << QString::fromStdString(Rel);
    emit delta(D);
}

void AuthoringWorker::start(QString configDump, QString bundlePath, QString nodeId, QStringList runnerChain)
{
    if (Session) { Session->End(); Session.reset(); }
    TargetNodeId = nodeId.toStdString();

    nlohmann::ordered_json Config = nlohmann::ordered_json::parse(configDump.toStdString(), nullptr, /*allow_exceptions=*/false);
    if (Config.is_discarded()) Config = nlohmann::ordered_json::object();

    // Rebuild the catalog index (this bundle + every repo) here, off the GUI thread — mirrors PackageEditorModel::
    // BuildExecIndex, so nothing but Qt value types crosses the thread boundary.
    NodeIndex Idx;
    ManifestModel::ScanBundleNodes(bundlePath.toStdString(), Idx);
    for (const std::string & Repo : PackageCatalog::RepositoryDirs(Config))
    {
        QDir Dir(QString::fromStdString(Repo));
        if (!Dir.exists()) continue;
        for (const QString & Sub : Dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            ManifestModel::ScanBundleNodes(Dir.filePath(Sub).toStdString(), Idx);
    }

    // Candidate runners: those serving the target node's platform on this machine.
    QStringList Runners;
    const Node * N = Idx.Find(TargetNodeId);
    const std::string Plat = N ? N->HostPlatform : std::string();
    const std::string Machine = ManifestModel::MachinePlatform();
    for (const auto & [Id, R] : Idx.Nodes)
        if (R.IsRunner() && R.HostPlatform == Machine)
            for (const std::string & G : R.GuestPlatform)
                if (G == Plat) { Runners << QString::fromStdString(Id); break; }

    Session = std::make_unique<AuthoringSession>(Config, QDir(bundlePath));
    std::vector<std::string> ChainVec;
    for (const QString & C : runnerChain) ChainVec.push_back(C.toStdString());

    if (!Session->Begin(Idx, TargetNodeId, {}, ChainVec))
    {
        Session.reset();
        emit started(false, QString(), QString(), false, QString(), Runners);
        return;
    }
    emit started(true, QString::fromStdString(Session->RuntimePath().string()),
                 QString::fromStdString(Session->ContentRoot()), Session->PrefixGenerate(),
                 QString::fromStdString(Session->RunnerId()), Runners);
    emitDeltaList();
}

void AuthoringWorker::runExe(QString exe)
{
    const bool Ok = Session && Session->RunExe(exe.toStdString());
    emit runFinished(Ok);
    emitDeltaList();
}

void AuthoringWorker::refreshDelta() { emitDeltaList(); }

void AuthoringWorker::captureFiles(QStringList rels, QString strip, QString destDirAbs)
{
    if (!Session) { emit filesCopied(0); return; }
    std::vector<std::string> Rels;
    for (const QString & R : rels) Rels.push_back(R.toStdString());
    const int N = AuthoringSession::CopySelection(Session->WriteLayerPath(), Rels, destDirAbs.toStdString(), strip.toStdString());
    emit filesCopied(N);
}

void AuthoringWorker::captureRegistry()
{
    const nlohmann::ordered_json Delta = Session ? Session->CaptureRegistryDelta() : nlohmann::ordered_json::array();
    emit registryDelta(QString::fromStdString(Delta.dump()));
}

void AuthoringWorker::end()
{
    if (Session) { Session->End(); Session.reset(); }
    emit ended();
}

// ============================================================================ model (GUI thread)

AuthoringSessionModel::AuthoringSessionModel(PackageEditorModel * E, std::string Node, QObject * parent)
    : QObject(parent), Editor(E), TargetNodeId(std::move(Node))
{
    Worker = new AuthoringWorker;
    Worker->moveToThread(&Thread);
    connect(&Thread, &QThread::finished, Worker, &QObject::deleteLater);

    connect(this, &AuthoringSessionModel::requestStart,          Worker, &AuthoringWorker::start);
    connect(this, &AuthoringSessionModel::requestRunExe,         Worker, &AuthoringWorker::runExe);
    connect(this, &AuthoringSessionModel::requestRefresh,        Worker, &AuthoringWorker::refreshDelta);
    connect(this, &AuthoringSessionModel::requestCaptureFiles,   Worker, &AuthoringWorker::captureFiles);
    connect(this, &AuthoringSessionModel::requestCaptureRegistry,Worker, &AuthoringWorker::captureRegistry);
    connect(this, &AuthoringSessionModel::requestEnd,            Worker, &AuthoringWorker::end);

    connect(Worker, &AuthoringWorker::started,      this, &AuthoringSessionModel::onStarted);
    connect(Worker, &AuthoringWorker::delta,        this, &AuthoringSessionModel::onDelta);
    connect(Worker, &AuthoringWorker::runFinished,  this, &AuthoringSessionModel::onRunFinished);
    connect(Worker, &AuthoringWorker::filesCopied,  this, &AuthoringSessionModel::onFilesCopied);
    connect(Worker, &AuthoringWorker::registryDelta,this, &AuthoringSessionModel::onRegistryDelta);

    Thread.start();
}

AuthoringSessionModel::~AuthoringSessionModel()
{
    if (Thread.isRunning())
    {
        // Unmount the live runtime BEFORE the thread dies — save-safety (a dangling durable mount could be wiped).
        QMetaObject::invokeMethod(Worker, "end", Qt::BlockingQueuedConnection);
        Thread.quit();
        Thread.wait();
    }
}

QStringList AuthoringSessionModel::bundleNodeIds() const
{
    QStringList Out;
    if (Editor) for (const std::string & Id : Editor->bundleNodeIds()) Out << QString::fromStdString(Id);
    return Out;
}

void AuthoringSessionModel::start()
{
    emit busyChanged(true, "Building runtime…");
    const QString Cfg = (Editor && Editor->globalConfig()) ? QString::fromStdString(Editor->globalConfig()->dump()) : "{}";
    emit requestStart(Cfg, Editor ? Editor->packagePath() : QString(), QString::fromStdString(TargetNodeId), {});
}

void AuthoringSessionModel::switchRunner(const QString & RunnerId)
{
    emit busyChanged(true, "Switching runner…");
    const QString Cfg = (Editor && Editor->globalConfig()) ? QString::fromStdString(Editor->globalConfig()->dump()) : "{}";
    emit requestStart(Cfg, Editor ? Editor->packagePath() : QString(), QString::fromStdString(TargetNodeId), { RunnerId });
}

void AuthoringSessionModel::runExe(const QString & HostPath)  { emit busyChanged(true, "Running…");  emit requestRunExe(HostPath); }
void AuthoringSessionModel::runGuest(const QString & GuestCmd){ emit busyChanged(true, "Running…");  emit requestRunExe(GuestCmd); }
void AuthoringSessionModel::refreshDelta()                    { emit busyChanged(true, "Scanning…"); emit requestRefresh(); }

void AuthoringSessionModel::captureFiles(const QStringList & Rels, const QString & TargetNode,
                                         const QString & DestName, const QString & Strip, const QString & Target)
{
    PendTargetNode = TargetNode; PendDestName = DestName; PendTarget = Target;
    const QString DestDirAbs = (Editor ? Editor->packagePath() : QString()) + "/" + DestName;
    emit busyChanged(true, "Capturing files…");
    emit requestCaptureFiles(Rels, Strip, DestDirAbs);
}

void AuthoringSessionModel::captureRegistry(const QString & TargetNode)
{
    PendTargetNode = TargetNode;
    emit busyChanged(true, "Diffing registry…");
    emit requestCaptureRegistry();
}

void AuthoringSessionModel::onStarted(bool ok, QString runtimePath, QString contentRoot, bool isWine,
                                      QString runnerId, QStringList runners)
{
    emit busyChanged(false, QString());
    if (!ok) { emit failed("Couldn't build the runtime. The node needs a PLATFORM.HOST a runner serves (or pick a runner). Check the log."); return; }
    emit runnersChanged(runners, runnerId);
    emit sessionReady(runtimePath, contentRoot, isWine);
}

void AuthoringSessionModel::onDelta(QStringList paths) { emit busyChanged(false, QString()); emit deltaChanged(paths); }

void AuthoringSessionModel::onRunFinished(bool ok)
{
    emit busyChanged(false, QString());
    if (!ok) emit captured("The process exited with an error — check the log.");
}

void AuthoringSessionModel::onFilesCopied(int count)
{
    emit busyChanged(false, QString());
    if (count <= 0) { emit captured("Nothing copied — check the strip prefix."); return; }
    if (Editor) Editor->appendLayerToNode(PendTargetNode.toStdString(),
                                          AuthoringSession::MakeDirLayer(PendDestName.toStdString(), PendTarget.toStdString()));
    emit captured(QString("Captured %1 file(s) into '%2' → VFSDirLayer on '%3'.").arg(count).arg(PendDestName).arg(PendTargetNode));
}

void AuthoringSessionModel::onRegistryDelta(QString deltaJsonDump)
{
    emit busyChanged(false, QString());
    nlohmann::ordered_json Delta = nlohmann::ordered_json::parse(deltaJsonDump.toStdString(), nullptr, false);
    if (Delta.is_discarded() || !Delta.is_array() || Delta.empty()) { emit captured("No registry changes since the session started."); return; }
    if (Editor) Editor->mergeRegEditsIntoNode(PendTargetNode.toStdString(), Delta);
    emit captured(QString("Captured %1 RegEdit(s) into '%2'.").arg((int)Delta.size()).arg(PendTargetNode));
}
