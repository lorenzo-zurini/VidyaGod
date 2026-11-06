#include "authoringsessionmodel.h"
#include "pkggraph.h"   // RegRowsInto — the captured delta becomes the schema's nested hive tree
#include "packageeditormodel.h"
#include "manifestmodel.h"     // NodeIndex / Node / ScanBundleNodes / MachinePlatform
#include "packagecatalog.h"    // BuildCatalogIndex

#include <QDir>
#include <QFileInfo>

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

QStringList CapturePreview::EncodeAnchors(const std::vector<std::pair<std::string, std::string>> &Layouts)
{
    QStringList Out;
    for (const auto & [A, Layout] : Layouts) Out << QString::fromStdString(A) + "\t" + QString::fromStdString(Layout);
    return Out;
}

QMap<QString, QString> CapturePreview::ParseAnchors(const QStringList &Lines)
{
    QMap<QString, QString> Out;
    for (const QString & L : Lines) Out.insert(L.section('\t', 0, 0), L.section('\t', 1));
    return Out;
}

QStringList CapturePreview::RunnerChoices(const std::vector<std::pair<std::string, std::string>> &IdLabel)
{
    //Numbers compare as numbers: 11_7 > 10_34 > 9_27.
    auto Key = [](const std::string &L) {
        std::vector<long> Nums; std::string Text;
        for (size_t I = 0; I < L.size();)
            if (std::isdigit((unsigned char)L[I])) { size_t E = I; while (E < L.size() && std::isdigit((unsigned char)L[E])) ++E; Nums.push_back(std::stol(L.substr(I, E - I))); I = E; }
            else Text += L[I++];
        return std::make_pair(Text, Nums);
    };
    std::vector<std::pair<std::string, std::string>> V = IdLabel;
    std::stable_sort(V.begin(), V.end(), [&](const auto &A, const auto &B) {
        const auto Ka = Key(A.second.empty() ? A.first : A.second), Kb = Key(B.second.empty() ? B.first : B.second);
        return Ka.first != Kb.first ? Ka.first < Kb.first : Ka.second > Kb.second;
    });
    QStringList Out;
    for (const auto &[Id, Label] : V) Out << QString::fromStdString((Label.empty() ? Id : Label) + "\t" + Id);
    return Out;
}

QString CapturePreview::WaitingTarget(const nlohmann::ordered_json &N)
{
    if (!N.is_object() || !N.contains("LAYERS") || !N["LAYERS"].is_array()) return {};
    for (const auto &L : N["LAYERS"])
        for (const char *T : {"ZIP", "FILE", "DIR", "DELTA"})
            if (L.is_object() && L.contains(T) && L[T].is_string() && L[T].get<std::string>().empty())
            {
                std::string Tg = L.contains("TARGET") && L["TARGET"].is_string() ? L["TARGET"].get<std::string>() : std::string("FILES");
                if (Tg.rfind("FILES", 0) == 0) Tg = Tg.substr(5);
                if (!Tg.empty() && Tg.front() == '/') Tg.erase(0, 1);
                return QString::fromStdString(Tg);
            }
    return {};
}

QString CapturePreview::CaptureName(const nlohmann::ordered_json &N, const std::string &Id)
{
    std::string L = N.is_object() && N.contains("LABEL") && N["LABEL"].is_string() ? N["LABEL"].get<std::string>() : std::string();
    std::string Safe;
    for (const char C : L)
        if (C == ':') Safe += " -";
        else if (std::string("/\\?*\"<>|").find(C) != std::string::npos || (unsigned char)C < 0x20) Safe += '_';
        else Safe += C;
    while (!Safe.empty() && (Safe.back() == '.' || Safe.back() == ' ')) Safe.pop_back();
    return QString::fromStdString(Safe.empty() ? Id + "_files" : Safe);
}

QString CapturePreview::PreviewLines(const QStringList &Lines, int MaxLines)
{
    if (Lines.size() <= MaxLines) return Lines.join("\n  ");
    return Lines.mid(0, MaxLines - 1).join("\n  ") + QString("\n  ... and %1 more").arg(Lines.size() - (MaxLines - 1));
}

QString CapturePreview::FreeName(const QString &Name, const QString &PackageDir)
{
    if (PackageDir.isEmpty() || !QFileInfo::exists(PackageDir + "/" + Name)) return Name;
    for (int N = 2;; ++N)
        if (const QString Try = Name + " " + QString::number(N); !QFileInfo::exists(PackageDir + "/" + Try)) return Try;
}

std::string CapturePreview::GuestPathOf(const std::string &Rel)
{
    const size_t At = Rel.find("drive_c");
    if (At == std::string::npos || (At > 0 && Rel[At - 1] != '/')) return {};
    std::string Rest = Rel.substr(At + 7);
    if (!Rest.empty() && Rest.front() != '/') return {};
    std::replace(Rest.begin(), Rest.end(), '/', '\\');
    return "C:" + (Rest.empty() ? std::string("\\") : Rest);
}

std::string CapturePreview::AnchorWinpath(const std::string &Target, const std::string &Rel, bool Contents)
{
    std::string T = Target;
    if (T.rfind("FILES/", 0) == 0) T = T.substr(6);
    if (T.size() < 2 || T.front() != '%') return {};
    const size_t Close = T.find('%', 1);
    if (Close == std::string::npos) return {};
    std::string Out = T.substr(0, Close) + ":winpath%" + T.substr(Close + 1);
    if (!Contents) Out += "/" + Rel.substr(Rel.find_last_of('/') + 1);
    std::replace(Out.begin(), Out.end(), '/', '\\');
    return Out;
}

std::vector<std::string> CapturePreview::AnchoredGuestsOf(const std::string &Rel, const QMap<QString, QString> &Layouts)
{
    std::vector<std::string> Out;
    for (auto It = Layouts.begin(); It != Layouts.end(); ++It)
    {
        const std::string L = It.value().toStdString();
        if (L.empty() || Rel.compare(0, L.size(), L) != 0 || (Rel.size() > L.size() && Rel[L.size()] != '/')) continue;
        std::string Rest = Rel.substr(L.size());
        std::replace(Rest.begin(), Rest.end(), '/', '\\');
        Out.push_back(It.key().toStdString() + Rest);
    }
    return Out;
}

std::vector<std::pair<std::string, std::string>> CapturePreview::GuestAnchorsFor(const QStringList &Roots, const std::string &Target, bool Contents,
                                                                                 const QMap<QString, QString> &Layouts)
{
    std::vector<std::pair<std::string, std::string>> Out;
    auto Both = [&](const std::string &Rel, const std::string &Anchor) {
        Out.push_back({GuestPathOf(Rel), Anchor});
        for (const std::string &A : AnchoredGuestsOf(Rel, Layouts)) Out.push_back({A, Anchor});
    };
    for (const QString &R : Roots)
    {
        Both(R.toStdString(), AnchorWinpath(Target, R.toStdString(), Contents));
        //Captured at its level, a root's parent is stripped: that parent folder (the game's install folder when
        //the author ticks its parts) IS the target.
        if (!Contents && R.contains('/'))
        {
            const std::string Parent = R.left(R.lastIndexOf('/')).toStdString();
            Both(Parent, AnchorWinpath(Target, Parent, true));
        }
    }
    return Out;
}

std::string CapturePreview::Reanchor(std::string V, const std::vector<std::pair<std::string, std::string>> &Map)
{
    auto Lower = [](std::string S) { for (char &C : S) C = (char)std::tolower((unsigned char)C); return S; };
    for (const auto &[Guest, Anchor] : Map)
    {
        if (Guest.empty() || Anchor.empty()) continue;
        const std::string G = Lower(Guest);
        for (size_t At = 0; (At = Lower(V).find(G, At)) != std::string::npos;)
        {
            //Only a whole path component: "C:\Game" is not a prefix of "C:\Games".
            const size_t End = At + G.size();
            if (End < V.size() && V[End] != '\\' && V[End] != '/' && V[End] != '"' && V[End] != ';') { At = End; continue; }
            V.replace(At, G.size(), Anchor);
            At += Anchor.size();
        }
    }
    return V;
}

QString CapturePreview::MountBase(QString Target, const QString &ContentRoot, const QMap<QString, QString> &Anchors)
{
    Target = Target.trimmed();
    if (Target.startsWith("FILES/")) Target = Target.mid(6);
    const int Close = Target.startsWith('%') ? Target.indexOf('%', 1) : -1;
    if (Close > 0 && Anchors.contains(Target.left(Close + 1))) return Anchors.value(Target.left(Close + 1)) + Target.mid(Close + 1);
    return Target.isEmpty() ? ContentRoot : ContentRoot + "/" + Target;
}

bool CapturePreview::CapturesContents(const QStringList &Roots, const QString &Target, const QString &WaitTarget,
                                      const QString &ContentRoot, const QMap<QString, QString> &Anchors)
{
    if (Roots.size() != 1 || Target.trimmed().isEmpty()) return false;
    if (!WaitTarget.isEmpty() && Target.trimmed() == WaitTarget) return true;
    auto Clean = [](QString P) { while (P.endsWith('/')) P.chop(1); return P; };
    return Clean(Roots.front()) == Clean(MountBase(Target, ContentRoot, Anchors));
}

//The ERR lines a step logged, one per line (at most a dozen): what its failure message says instead of "see the log".
QString AuthoringWorker::Errors(LogRecorder &Said)
{
    QStringList Lines;
    for (const auto &E : Said.Take())
        if (E.Level == LogLevel::ERR && !Lines.contains(QString::fromStdString(E.Message)) && Lines.size() < 12)
            Lines << QString::fromStdString(E.Message);
    return Lines.join('\n');
}

void AuthoringWorker::emitSessionInfo()
{
    if (!Session) { emit started(false, QString(), QString(), false, QString(), WineRunners); return; }
    const QStringList Anchors = CapturePreview::EncodeAnchors(Session->AnchorLayouts());
    emit started(true, QString::fromStdString(Session->RuntimePath().string()),
                 QString::fromStdString(Session->ContentRoot()), Session->PrefixGenerate(),
                 QString::fromStdString(Session->RunnerId()), WineRunners, Anchors);
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
    const auto Cat = PackageCatalog::CurrentIndex(Config);                      // CID package sources + local bundles
    for (const auto & [Id, N] : Cat->Nodes) Idx.Nodes.emplace(Id, N);
    ManifestModel::DeriveFacts(Idx);   // entries, runners and tiles are folded facts

    // The "Run Windows program" tool's choices: every Windows-capable runner usable on this machine (guest covers
    // win32/win64). Independent of the package's platform — you might run a Windows editor on a Linux game's runtime.
    WineRunners.clear();
    std::vector<std::pair<std::string, std::string>> Choices;
    const std::string Machine = ManifestModel::MachinePlatform();
    for (const auto & [Id, R] : Idx.Nodes)
        if (R.IsRunner() && R.HostPlatform == Machine)
            for (const std::string & G : R.GuestPlatform)
                if (G == "win32" || G == "win64") { Choices.push_back({Id, R.NodeId}); break; }
    WineRunners = CapturePreview::RunnerChoices(Choices);

    Session = std::make_unique<AuthoringSession>(Config, QDir(bundlePath));
    LogRecorder Said({});   // why a mount failed (the helper's own output): said with the failure, not left in the log
    if (!Session->Begin(Idx, TargetNodeId, {}))   // BARE: no runner, no platform — just the content overlay
    {
        Session.reset();
        emit why(Errors(Said));
        emit started(false, QString(), QString(), false, QString(), WineRunners);
        return;
    }
    emitSessionInfo();
    emitDeltaList();
}

void AuthoringWorker::runWindows(QString exe, QString runnerId, QStringList args)
{
    std::vector<std::string> Args;
    for (const QString & A : args) Args.push_back(A.toStdString());
    LogRecorder Said({});
    const bool Ok = Session && Session->RunWindows(exe.toStdString(), runnerId.toStdString(), Args);
    if (!Ok) emit why(Errors(Said));
    emitSessionInfo();   // the runtime may have just become a wine prefix → refresh content-root / wine flag / runner
    emit runFinished(Ok);
    emitDeltaList();
}

void AuthoringWorker::runExe(QString exe)
{
    LogRecorder Said({});
    const bool Ok = Session && Session->RunExe(exe.toStdString());
    if (!Ok) emit why(Errors(Said));
    emit runFinished(Ok);
    emitDeltaList();
}

void AuthoringWorker::refreshDelta() { emitDeltaList(); }

void AuthoringWorker::captureFiles(QStringList roots, QString destDirAbs, bool contents)
{
    if (!Session) { emit filesCopied(0); return; }
    // Each checked root is captured "at its own level": strip its parent path, so a ticked folder lands as the top of
    // the layer without its ancestor folders (the author re-homes it later via the layer TARGET if needed).
    int Total = 0;
    for (const QString & Root : roots)
    {
        const std::string R = Root.toStdString();
        const auto Slash = R.find_last_of('/');
        const std::string Strip = contents ? R : (Slash == std::string::npos) ? std::string() : R.substr(0, Slash);
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

    connect(Worker, &AuthoringWorker::why,         this, [this](QString W) { LastWhy = W; });   // queued: before started/runFinished
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

void AuthoringSessionModel::runWindows(const QString & Exe, const QString & RunnerId, const QStringList & Args)
{
    emit busyChanged(true, "Running in a wine prefix…");
    emit requestRunWindows(Exe, RunnerId, Args);
}

void AuthoringSessionModel::runGuest(const QString & GuestCmd){ emit busyChanged(true, "Running…");  emit requestRunExe(GuestCmd); }
void AuthoringSessionModel::refreshDelta()                    { emit busyChanged(true, "Scanning…"); emit requestRefresh(); }

void AuthoringSessionModel::captureFiles(const QStringList & Roots, const QString & DestName, const QString & Target)
{
    PendDestName = DestName; PendTarget = Target; PendRoots = Roots;
    const QString DestDirAbs = (Editor ? Editor->packagePath() : QString()) + "/" + DestName;
    emit busyChanged(true, "Capturing files…");
    //One folder captured where the anchor waits for its files IS that folder: its contents, not it, become them.
    const int At = Editor ? Editor->doc().IndexOf(TargetNodeId) : -1;
    const bool Contents = CapturePreview::CapturesContents(Roots, Target, At >= 0 ? CapturePreview::WaitingTarget(Editor->doc().Node(At)) : QString(),
                                                           ContentRoot, AnchorLayouts);
    PendContents = Contents;
    emit requestCaptureFiles(Roots, DestDirAbs, Contents);
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
            ByArch[Arch].push_back({Path, K, CapturePreview::Reanchor(V.is_string() ? V.get<std::string>() : V.dump(), GuestAnchors)});
    }
    //One REG layer per view the keys were captured in.
    nlohmann::ordered_json Layers = nlohmann::ordered_json::array();
    for (const auto & [Arch, Rows] : ByArch)
    {
        nlohmann::ordered_json Tree = nlohmann::ordered_json::object();
        PkgGraph::RegRowsInto(Tree, Rows);
        Layers.push_back(nlohmann::ordered_json{{"REG", std::move(Tree)}, {"ARCH", nlohmann::ordered_json::array({Arch})}});
    }
    const std::string NewId = Editor->createNode(
        nlohmann::ordered_json::object({{"LAYERS", Layers}}), {TargetNodeId}, "captured_registry");
    emit registryCaptured(RegPaths);
    emit nodeCreated(QString::fromStdString(NewId));
    emit captured(QString("Captured %1 registry key(s) → new REG node '%2'.")
                      .arg((int)Picked.size()).arg(QString::fromStdString(NewId)));
}

void AuthoringSessionModel::onStarted(bool ok, QString runtimePath, QString contentRoot, bool isWine,
                                      QString runnerId, QStringList runners, QStringList anchorLayouts)
{
    emit busyChanged(false, QString());
    if (!ok)
    {
        emit failed("Couldn't mount the authoring runtime" + (LastWhy.isEmpty() ? QString(".") : ":\n" + LastWhy));
        LastWhy.clear();
        return;
    }
    AnchorLayouts = CapturePreview::ParseAnchors(anchorLayouts);
    ContentRoot = contentRoot;
    emit runnersChanged(runners, runnerId);
    emit sessionReady(runtimePath, contentRoot, isWine, anchorLayouts);
}

void AuthoringSessionModel::onDelta(QStringList paths) { emit busyChanged(false, QString()); emit deltaChanged(paths); }

void AuthoringSessionModel::onRunFinished(bool ok)
{
    emit busyChanged(false, QString());
    if (!ok) emit captured("The process exited with an error" + (LastWhy.isEmpty() ? QString(".") : ":\n" + LastWhy));
    LastWhy.clear();
}

void AuthoringSessionModel::onFilesCopied(int count)
{
    emit busyChanged(false, QString());
    if (count <= 0) { emit captured("Nothing copied — check the selection."); return; }
    if (!Editor) return;
    // A capture IS a node: one DIR layer holding what the run wrote, over the anchor (createNode puts the anchor
    // first, as a NODE layer) so it applies exactly where the capture was taken. The target is a guest path; a
    // layer's TARGET is its FILES address.
    nlohmann::ordered_json Layer = nlohmann::ordered_json::object({{"DIR", PendDestName.toStdString()}});
    if (!PendTarget.isEmpty())
        Layer["TARGET"] = PendTarget.startsWith("FILES/") ? PendTarget.toStdString() : "FILES/" + PendTarget.toStdString();
    //Where the captured folders were in the guest is where an installer's registry names them: remembered by anchor.
    for (auto & G : CapturePreview::GuestAnchorsFor(PendRoots, PendTarget.toStdString(), PendContents, AnchorLayouts)) GuestAnchors.push_back(G);
    //Longest guest path first: a folder's own anchor wins over its parent's.
    std::stable_sort(GuestAnchors.begin(), GuestAnchors.end(), [](const auto & A, const auto & B) { return A.first.size() > B.first.size(); });
    //What the anchor was still waiting for (a new package's files, before its install): they take that place.
    if (Editor->fillWaiting(TargetNodeId, Layer))
    {
        emit filesCaptured(PendRoots);
        emit captured(QString("Captured %1 file(s) → the files this node was waiting for.").arg(count));
        return;
    }
    nlohmann::ordered_json Payload = nlohmann::ordered_json::object({
        {"LAYERS", nlohmann::ordered_json::array({std::move(Layer)})}});
    const std::string NewId = Editor->createNode(Payload, {TargetNodeId}, PendDestName.toStdString() + "_files");
    emit filesCaptured(PendRoots);
    emit nodeCreated(QString::fromStdString(NewId));
    emit captured(QString("Captured %1 file(s) → new node '%2'.").arg(count).arg(QString::fromStdString(NewId)));
}

void AuthoringSessionModel::onRegistryScan(QString deltaJsonDump, QStringList regPaths)
{
    emit busyChanged(false, QString());
    LastRegDelta = nlohmann::ordered_json::parse(deltaJsonDump.toStdString(), nullptr, false);
    if (LastRegDelta.is_discarded() || !LastRegDelta.is_array()) LastRegDelta = nlohmann::ordered_json::array();
    emit registryTreeChanged(regPaths);
    if (regPaths.isEmpty()) emit captured("No registry changes since the session started.");
}
