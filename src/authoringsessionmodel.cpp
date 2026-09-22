#include "authoringsessionmodel.h"
#include "pkggraph.h"   // RegRowsInto — the captured delta becomes the schema's nested hive tree
#include "packageeditormodel.h"
#include "manifestmodel.h"     // NodeIndex / Node / ScanBundleNodes / MachinePlatform
#include "packagecatalog.h"    // BuildCatalogIndex

#include <QDir>

#include <map>
#include <set>
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

void AuthoringWorker::emitSessionInfo()
{
    if (!Session) { emit started(false, QString(), QString(), false, QString(), WineRunners); return; }
    emit started(true, QString::fromStdString(Session->RuntimePath().string()),
                 QString::fromStdString(Session->ContentRoot()), Session->PrefixGenerate(),
                 QString::fromStdString(Session->RunnerId()), WineRunners);
}

void AuthoringWorker::start(QString configDump, QString bundlePath, QString nodeId)
{
    if (Session) { Session->End(); Session.reset(); }
    TargetNodeId = nodeId.toStdString();

    nlohmann::ordered_json Config = nlohmann::ordered_json::parse(configDump.toStdString(), nullptr, /*allow_exceptions=*/false);
    if (Config.is_discarded()) Config = nlohmann::ordered_json::object();

    // Rebuild the catalog index (this bundle + the whole catalog) here, off the GUI thread — mirrors PackageEditorModel::
    // BuildExecIndex, so nothing but Qt value types crosses the thread boundary.
    NodeIndex Idx;
    ManifestModel::ScanBundleNodes(bundlePath.toStdString(), Idx);              // this bundle wins (first-seen)
    NodeIndex Cat = PackageCatalog::BuildCatalogIndex(Config);                  // CID package sources + local bundles
    for (auto & [Id, N] : Cat.Nodes) Idx.Nodes.emplace(Id, N);
    ManifestModel::LinkGames(Idx);   // link variants to their game nodes (graph-edge grouping)

    // The "Run Windows program" tool's choices: every Windows-capable runner usable on this machine (guest covers
    // win32/win64). Independent of the package's platform — you might run a Windows editor on a Linux game's runtime.
    WineRunners.clear();
    const std::string Machine = ManifestModel::MachinePlatform();
    for (const auto & [Id, R] : Idx.Nodes)
        if (R.IsRunner() && R.HostPlatform == Machine)
            for (const std::string & G : R.GuestPlatform)
                if (G == "win32" || G == "win64") { WineRunners << QString::fromStdString(Id); break; }

    Session = std::make_unique<AuthoringSession>(Config, QDir(bundlePath));
    if (!Session->Begin(Idx, TargetNodeId, {}))   // BARE: no runner, no platform — just the content overlay
    {
        Session.reset();
        emit started(false, QString(), QString(), false, QString(), WineRunners);
        return;
    }
    emitSessionInfo();
    emitDeltaList();
}

void AuthoringWorker::runWindows(QString exe, QString runnerId)
{
    const bool Ok = Session && Session->RunWindows(exe.toStdString(), runnerId.toStdString());
    emitSessionInfo();   // the runtime may have just become a wine prefix → refresh content-root / wine flag / runner
    emit runFinished(Ok);
    emitDeltaList();
}

void AuthoringWorker::runExe(QString exe)
{
    const bool Ok = Session && Session->RunExe(exe.toStdString());
    emit runFinished(Ok);
    emitDeltaList();
}

void AuthoringWorker::refreshDelta() { emitDeltaList(); }

void AuthoringWorker::captureFiles(QStringList roots, QString destDirAbs)
{
    if (!Session) { emit filesCopied(0); return; }
    // Each checked root is captured "at its own level": strip its parent path, so a ticked folder lands as the top of
    // the layer without its ancestor folders (the author re-homes it later via the layer TARGET if needed).
    int Total = 0;
    for (const QString & Root : roots)
    {
        const std::string R = Root.toStdString();
        const auto Slash = R.find_last_of('/');
        const std::string Strip = (Slash == std::string::npos) ? std::string() : R.substr(0, Slash);
        Total += AuthoringSession::CopySelection(Session->WriteLayerPath(), { R }, destDirAbs.toStdString(), Strip);
    }
    emit filesCopied(Total);
}

void AuthoringWorker::scanRegistry()
{
    const nlohmann::ordered_json Delta = Session ? Session->CaptureRegistryDelta() : nlohmann::ordered_json::array();
    QStringList Paths;
    if (Delta.is_array())
        for (const auto & E : Delta)
        {
            const std::string P = E.value("REGPATH", std::string());
            if (!P.empty()) Paths << QString::fromStdString(P);
        }
    emit registryScan(QString::fromStdString(Delta.dump()), Paths);
}

void AuthoringWorker::end()
{
    if (Session) { Session->End(); Session.reset(); }
    emit ended();
}

// ============================================================================ model (GUI thread)

AuthoringSessionModel::AuthoringSessionModel(PackageEditorModel * E, std::string AnchorNodeId,
                                             CaptureMode M, QObject * parent)
    : QObject(parent), Editor(E), TargetNodeId(std::move(AnchorNodeId)), Mode(M)
{
    Worker = new AuthoringWorker;
    Worker->moveToThread(&Thread);
    connect(&Thread, &QThread::finished, Worker, &QObject::deleteLater);

    connect(this, &AuthoringSessionModel::requestStart,        Worker, &AuthoringWorker::start);
    connect(this, &AuthoringSessionModel::requestRunWindows,   Worker, &AuthoringWorker::runWindows);
    connect(this, &AuthoringSessionModel::requestRunExe,       Worker, &AuthoringWorker::runExe);
    connect(this, &AuthoringSessionModel::requestRefresh,      Worker, &AuthoringWorker::refreshDelta);
    connect(this, &AuthoringSessionModel::requestCaptureFiles, Worker, &AuthoringWorker::captureFiles);
    connect(this, &AuthoringSessionModel::requestScanRegistry, Worker, &AuthoringWorker::scanRegistry);
    connect(this, &AuthoringSessionModel::requestEnd,          Worker, &AuthoringWorker::end);

    connect(Worker, &AuthoringWorker::started,     this, &AuthoringSessionModel::onStarted);
    connect(Worker, &AuthoringWorker::delta,       this, &AuthoringSessionModel::onDelta);
    connect(Worker, &AuthoringWorker::runFinished, this, &AuthoringSessionModel::onRunFinished);
    connect(Worker, &AuthoringWorker::filesCopied, this, &AuthoringSessionModel::onFilesCopied);
    connect(Worker, &AuthoringWorker::registryScan,this, &AuthoringSessionModel::onRegistryScan);

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
    emit busyChanged(true, "Mounting runtime…");
    const QString Cfg = (Editor && Editor->globalConfig()) ? QString::fromStdString(Editor->globalConfig()->dump()) : "{}";
    emit requestStart(Cfg, Editor ? Editor->packagePath() : QString(), QString::fromStdString(TargetNodeId));
}

void AuthoringSessionModel::runWindows(const QString & Exe, const QString & RunnerId)
{
    emit busyChanged(true, "Running in a wine prefix…");
    emit requestRunWindows(Exe, RunnerId);
}

void AuthoringSessionModel::runGuest(const QString & GuestCmd){ emit busyChanged(true, "Running…");  emit requestRunExe(GuestCmd); }
void AuthoringSessionModel::refreshDelta()                    { emit busyChanged(true, "Scanning…"); emit requestRefresh(); }

void AuthoringSessionModel::captureFiles(const QStringList & Roots, const QString & DestName, const QString & Target)
{
    PendDestName = DestName; PendTarget = Target; PendRoots = Roots;
    const QString DestDirAbs = (Editor ? Editor->packagePath() : QString()) + "/" + DestName;
    emit busyChanged(true, "Capturing files…");
    emit requestCaptureFiles(Roots, DestDirAbs);
}

void AuthoringSessionModel::scanRegistry()
{
    emit busyChanged(true, "Diffing registry…");
    emit requestScanRegistry();
}

void AuthoringSessionModel::captureSelectedRegistry(const QStringList & RegPaths)
{
    // Filter the last scanned delta to the picked keys (cheap JSON, GUI thread) and merge them into the node.
    std::set<std::string> Want;
    for (const QString & P : RegPaths) Want.insert(P.toStdString());
    nlohmann::ordered_json Picked = nlohmann::ordered_json::array();
    if (LastRegDelta.is_array())
        for (const auto & E : LastRegDelta)
            if (Want.count(E.value("REGPATH", std::string()))) Picked.push_back(E);
    if (Picked.empty()) { emit captured("No registry keys checked."); return; }
    if (!Editor) return;
    // The scan yields flat {REGPATH, KEYVALUES} entries; the schema stores the registry as the tree it actually
    // is. Group by architecture, then let RegRowsInto build the hives.
    std::map<std::string, std::vector<PkgGraph::RegRow>> ByArch;
    for (const auto & E : Picked)
    {
        const std::string Path = E.value("REGPATH", std::string());
        const std::string Arch = E.contains("ARCHITECTURE") && E["ARCHITECTURE"].is_string()
                                     ? E["ARCHITECTURE"].get<std::string>() : std::string("32");
        if (Path.empty()) continue;
        // DiffToRegEdits OMITS KEYVALUES entirely for a key-only edit (registrywrapper.cpp) — which is the
        // single most common registry capture, an installer creating a key. const operator[] on a missing key
        // asserts (or dereferences end() under NDEBUG), so look it up without inserting or throwing.
        const nlohmann::ordered_json KV = E.value("KEYVALUES", nlohmann::ordered_json::object());
        //FLAGGED as key-only. An empty NAME is the key's DEFAULT value, not "no values" — leaving the flag
        //off here wrote a spurious `@=""` into the prefix on the most common capture there is.
        if (!KV.is_object() || KV.empty())
        { PkgGraph::RegRow R{Path, "", ""}; R.KeyOnly = true; ByArch[Arch].push_back(std::move(R)); continue; }
        for (const auto & [K, V] : KV.items())
            ByArch[Arch].push_back({Path, K, V.is_string() ? V.get<std::string>() : V.dump()});
    }
    nlohmann::ordered_json Edits = nlohmann::ordered_json::array();
    for (const auto & [Arch, Rows] : ByArch)
    {
        nlohmann::ordered_json Entry = nlohmann::ordered_json::object({
            {"ARCHITECTURE", nlohmann::ordered_json::array({Arch})}});
        PkgGraph::RegRowsInto(Entry, Rows);
        Edits.push_back(std::move(Entry));
    }
    const std::string NewId = Editor->createNode(
        nlohmann::ordered_json::object({{"TYPE", "RegEdit"}, {"EDITS", Edits}}), {TargetNodeId}, "captured_registry");
    emit registryCaptured(RegPaths);
    emit nodeCreated(QString::fromStdString(NewId));
    emit captured(QString("Captured %1 registry key(s) → new RegEdit node '%2'.")
                      .arg((int)Picked.size()).arg(QString::fromStdString(NewId)));
}

void AuthoringSessionModel::onStarted(bool ok, QString runtimePath, QString contentRoot, bool isWine,
                                      QString runnerId, QStringList runners)
{
    emit busyChanged(false, QString());
    if (!ok) { emit failed("Couldn't mount the authoring runtime — check the log."); return; }
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
    if (count <= 0) { emit captured("Nothing copied — check the selection."); return; }
    if (!Editor) return;
    // A capture IS a node: one Content node holding what the run wrote, parented at the anchor so it applies
    // exactly where the capture was taken.
    //Batched: a capture is one VFSLayer node holding a single dir layer.
    nlohmann::ordered_json Layer = nlohmann::ordered_json::object({
        {"FORM", "dir"}, {"PATH", PendDestName.toStdString()}});
    if (!PendTarget.isEmpty()) Layer["TARGET"] = PendTarget.toStdString();
    nlohmann::ordered_json Payload = nlohmann::ordered_json::object({
        {"TYPE", "VFSLayer"}, {"LAYERS", nlohmann::ordered_json::array({std::move(Layer)})}});
    const std::string NewId = Editor->createNode(Payload, {TargetNodeId}, PendDestName.toStdString() + "_files");
    emit filesCaptured(PendRoots);
    emit nodeCreated(QString::fromStdString(NewId));
    emit captured(QString("Captured %1 file(s) → new Content node '%2'.").arg(count).arg(QString::fromStdString(NewId)));
}

void AuthoringSessionModel::onRegistryScan(QString deltaJsonDump, QStringList regPaths)
{
    emit busyChanged(false, QString());
    LastRegDelta = nlohmann::ordered_json::parse(deltaJsonDump.toStdString(), nullptr, false);
    if (LastRegDelta.is_discarded() || !LastRegDelta.is_array()) LastRegDelta = nlohmann::ordered_json::array();
    emit registryTreeChanged(regPaths);
    if (regPaths.isEmpty()) emit captured("No registry changes since the session started.");
}
