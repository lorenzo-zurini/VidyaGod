// `VidyaGod author …` — authoring: run an installer or a tool on a live runtime of a node and capture what it changed
// (files into a content node, registry keys into a REG node). A session is a mount: it lives in the app, kept between
// commands until `author end` — the same AuthoringSessionModel the authoring window drives. The capture helpers (where
// a capture lands, how its paths are anchored) work on their own.
#include "command.h"
#include "cmdutil.h"
#include "authoringsession.h"
#include "authoringsessionmodel.h"
#include "authoringsessionwindow.h"
#include "deltatree.h"
#include "packageeditor.h"
#include "packageeditormodel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

namespace
{
using json = nlohmann::ordered_json;
using namespace CmdUtil;
namespace fs = std::filesystem;

json Strs(const QStringList &L)
{
    json Out = json::array();
    for (const QString &S : L) Out.push_back(S.toStdString());
    return Out;
}

QStringList QStrs(const std::vector<std::string> &V)
{
    QStringList Out;
    for (const auto &S : V) Out << QString::fromStdString(S);
    return Out;
}

// What a session has reported so far (its model's signals, kept).
struct Session
{
    std::unique_ptr<AuthoringSessionModel> M;   // captures go into the package's build session (`build show`, `build save`)
    std::string Package, Node;
    bool Ready = false, Busy = false, Wine = false;
    std::string Runtime, ContentRoot, Runner, BusyWhat, Failed;
    json Anchors = json::array(), Runners = json::array(), Delta = json::array(), Registry = json::array(), Messages = json::array(),
         Nodes = json::array(), FilesCaptured = json::array(), KeysCaptured = json::array();
    long long Events = 0;
};

std::unique_ptr<Session> &The()
{
    static std::unique_ptr<Session> S;   // one session at a time: the runtime is one mount
    return S;
}

Session &Live()
{
    if (!The()) throw std::runtime_error("no authoring session (VidyaGod author start <package> <node>)");
    return *The();
}

// Pump the app until Done says so, or the timeout.
bool Until(const std::function<bool()> &Done, long long TimeoutMs)
{
    QElapsedTimer T;
    T.start();
    while (!Done())
    {
        if (T.elapsed() > TimeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(5);
    }
    return true;
}

// Wait for the session to finish what it was asked (busy → idle, or a failure), and report it.
json Settled(Session &S, long long From, long long TimeoutMs)
{
    const bool Ok = Until([&] { return S.Events > From && !S.Busy; }, TimeoutMs);
    json Out = {{"finished", Ok}, {"failed", S.Failed}, {"messages", S.Messages}, {"nodes_created", S.Nodes}};
    S.Failed.clear();
    S.Messages = json::array();
    return Out;
}

json State(const Session &S)
{
    return {{"package", S.Package}, {"node", S.Node}, {"mode", static_cast<int>(S.M->mode())}, {"ready", S.Ready}, {"busy", S.Busy},
            {"doing", S.BusyWhat}, {"runtime", S.Runtime}, {"content_root", S.ContentRoot}, {"wine", S.Wine}, {"runner", S.Runner},
            {"runners", S.Runners}, {"anchors", S.Anchors}, {"changed_files", S.Delta.size()}, {"changed_keys", S.Registry.size()},
            {"files_captured", S.FilesCaptured}, {"keys_captured", S.KeysCaptured}, {"nodes_created", S.Nodes},
            {"labels", Strs(S.M->bundleNodeIds())}};
}

std::map<std::string, std::string> Pairs(const json &A, const char *Key)
{
    std::map<std::string, std::string> Out;
    for (const auto &KV : List(A, Key))
    {
        const auto Eq = KV.find('=');
        if (Eq == std::string::npos) throw Command::BadArgs(std::string("--") + Key + " takes A=B pairs");
        Out[KV.substr(0, Eq)] = KV.substr(Eq + 1);
    }
    return Out;
}

Command::Arg Timeout() { return Command::Opt("timeout", "seconds to wait for it (default 600)", Command::Type::Int); }
long long Ms(const json &A) { return Command::Int(A, "timeout", 600) * 1000; }

} // namespace

VG_COMMANDS
{
    using Command::Type;
    using Command::Needs;
    using Command::Pos;
    using Command::Opt;
    using Command::Flag;
    Command::RegisterGroup("author", "run an installer or tool on a live runtime of a node, and capture what it changed");

    Command::Register({
        "author start", "open an authoring session: a live runtime of a node's closure",
        "Mounts a bare runtime of the node (its content and a writable layer on top; no runner yet) in the app — the "
        "authoring window's session, without the window. --mode says what the capture becomes: setup (files and "
        "registry, an installer), files, or registry. One session at a time; it lasts until `author end`. Needs the "
        "app running (it can be off-screen): a session is a mount, and a command on its own ends with its process.",
        { Pos("package", "the package"), Pos("node", "the node to author on (its handle, index, or LABEL)"),
          Opt("mode", "what to capture", Type::String, false, { "setup", "files", "registry" }), Timeout() },
        { "author start 749 'The Age of Kings' --mode setup" },
        { "AuthoringSessionModel::start", "AuthoringSessionModel::mode", "AuthoringWorker::start", "AuthoringSession::Begin",
          "AuthoringSession::WithoutWaiting", "AuthoringSession::Live", "AuthoringSession::RuntimePath", "AuthoringSession::ContentRoot",
          "AuthoringSession::AnchorLayouts", "AuthoringSession::RunnerId", "AuthoringSession::PrefixGenerate", "AuthoringSession::WriteLayerPath",
          "AuthoringSessionModel::bundleNodeIds" , "ContainerWrapper::SetNotAGame" }, Needs::App, false,
        [](Command::Context &C, const json &A) {
            if (!C.Live) throw std::runtime_error("an authoring session lives in the app: start VidyaGod on this data dir (off-screen is fine), then run this");
            if (The()) throw std::runtime_error("a session is open (" + The()->Package + "): VidyaGod author end");
            auto S = std::make_unique<Session>();
            const fs::path P = PackageDir(C, Command::Str(A, "package"), false);
            PackageEditorModel *E = CmdUtil::OpenBuild(C, Command::Str(A, "package")).M;   // live: the editor's, or a kept session
            const std::string Q = Command::Str(A, "node");
            int I = E->doc().IndexOf(Q);
            for (int K = 0; I < 0 && K < E->doc().Count(); ++K) if (E->doc().Node(K).value("LABEL", std::string()) == Q) I = K;
            if (I < 0) throw Command::BadArgs("the package has no node '" + Q + "'");
            static const std::map<std::string, CaptureMode> Modes = {{"setup", CaptureMode::Setup}, {"files", CaptureMode::Files}, {"registry", CaptureMode::Registry}};
            S->Package = P.string();
            S->Node = E->doc().Handle(I);
            S->M = std::make_unique<AuthoringSessionModel>(E, S->Node, Modes.at(Command::Str(A, "mode", "setup")));
            Session *X = S.get();
            AuthoringSessionModel *M = S->M.get();
            QObject::connect(M, &AuthoringSessionModel::sessionReady, M, [X](const QString &Rt, const QString &Cr, bool Wine, const QStringList &An) {
                X->Ready = true; X->Runtime = Rt.toStdString(); X->ContentRoot = Cr.toStdString(); X->Wine = Wine; X->Anchors = Strs(An); ++X->Events; });
            QObject::connect(M, &AuthoringSessionModel::busyChanged, M, [X](bool B, const QString &W) { X->Busy = B; X->BusyWhat = W.toStdString(); ++X->Events; });
            QObject::connect(M, &AuthoringSessionModel::runnersChanged, M, [X](const QStringList &R, const QString &Cur) { X->Runners = Strs(R); X->Runner = Cur.toStdString(); });
            QObject::connect(M, &AuthoringSessionModel::deltaChanged, M, [X](const QStringList &Paths) { X->Delta = Strs(Paths); ++X->Events; });
            QObject::connect(M, &AuthoringSessionModel::registryTreeChanged, M, [X](const QStringList &Keys) { X->Registry = Strs(Keys); ++X->Events; });
            QObject::connect(M, &AuthoringSessionModel::filesCaptured, M, [X](const QStringList &R) { for (const auto &F : R) X->FilesCaptured.push_back(F.toStdString()); });
            QObject::connect(M, &AuthoringSessionModel::registryCaptured, M, [X](const QStringList &K) { for (const auto &F : K) X->KeysCaptured.push_back(F.toStdString()); });
            QObject::connect(M, &AuthoringSessionModel::captured, M, [X](const QString &Msg) { X->Messages.push_back(Msg.toStdString()); ++X->Events; });
            QObject::connect(M, &AuthoringSessionModel::nodeCreated, M, [X](const QString &N) { X->Nodes.push_back(N.toStdString()); });
            QObject::connect(M, &AuthoringSessionModel::failed, M, [X](const QString &Msg) { X->Failed = Msg.toStdString(); ++X->Events; X->Busy = false; });
            The() = std::move(S);
            const long long From = X->Events;
            M->start();
            json Out = Settled(*X, From, Ms(A));
            Out["state"] = State(*X);
            if (!X->Ready) { The().reset(); throw std::runtime_error("the session did not start: " + Out["failed"].get<std::string>()); }
            return Out;
        }, {} });

    Command::Register({
        "author state", "the session: its runtime, the runner, anchors, what changed, what was captured", "",
        {}, { "author state" }, {}, Needs::App, false, [](Command::Context &, const json &) { return State(Live()); }, {} });

    Command::Register({
        "author run-windows", "run a Windows program (an installer, a setup tool) in the session, under a runner",
        "(Re)builds the session's runtime under --runner (its prefix and content root; `author state` lists the runners) "
        "and runs the program with --args. Waits until it exits (--timeout). Off-screen and silent as every run here.",
        { Pos("exe", "the program (a host path)"), Opt("runner", "the runner's id", Type::String, true), Opt("args", "its arguments (a,b)"), Timeout() },
        { "author run-windows ~/Downloads/setup.exe --runner geproton_10_34 --args /S" },
        { "AuthoringSessionModel::runWindows", "AuthoringWorker::runWindows", "AuthoringSession::RunWindows" }, Needs::App, false,
        [](Command::Context &, const json &A) {
            Session &S = Live();
            const long long From = S.Events;
            S.M->runWindows(QString::fromStdString(Command::Str(A, "exe")), QString::fromStdString(Command::Str(A, "runner")), QStrs(List(A, "args")));
            json Out = Settled(S, From, Ms(A));
            Out["state"] = State(S);
            return Out;
        }, {} });

    Command::Register({
        "author run", "run a tool in the session's guest (regedit.exe, explorer.exe, a program inside it)",
        "Runs a guest command against the live mount (after a Windows program set the prefix up). Waits until it exits.",
        { Pos("command", "the guest command"), Timeout() }, { "author run regedit.exe" },
        { "AuthoringSessionModel::runGuest", "AuthoringWorker::runExe", "AuthoringSession::RunExe" }, Needs::App, false,
        [](Command::Context &, const json &A) {
            Session &S = Live();
            const long long From = S.Events;
            S.M->runGuest(QString::fromStdString(Command::Str(A, "command")));
            return Settled(S, From, Ms(A));
        }, {} });

    Command::Register({
        "author changes", "what the session's runs changed: files, and (--registry) registry keys",
        "Re-reads the writable layer: every file a run added or changed (prefix churn left out). --registry diffs the "
        "registry against its start and lists the changed keys. --under limits the files to a folder.",
        { Flag("registry", "the changed registry keys"), Opt("under", "only files under this folder"), Timeout() },
        { "author changes", "author changes --registry" },
        { "AuthoringSessionModel::refreshDelta", "AuthoringWorker::refreshDelta", "AuthoringSession::EnumerateDelta", "AuthoringSession::IsChurn",
          "AuthoringSessionModel::scanRegistry", "AuthoringWorker::scanRegistry", "AuthoringSession::CaptureRegistryDelta" }, Needs::App, false,
        [](Command::Context &, const json &A) {
            Session &S = Live();
            const long long From = S.Events;
            if (Command::Bool(A, "registry", false)) S.M->scanRegistry(); else S.M->refreshDelta();
            json Out = Settled(S, From, Ms(A));
            if (Command::Bool(A, "registry", false)) { Out["keys"] = S.Registry; return Out; }
            const std::string U = Command::Str(A, "under", "");
            json F = json::array();
            for (const auto &P : S.Delta) if (U.empty() || P.get<std::string>().rfind(U, 0) == 0) F.push_back(P);
            Out["files"] = F;
            return Out;
        }, {} });

    Command::Register({
        "author capture", "capture changed files into a content node, or registry keys into a REG node",
        "--files ROOTS (folders or files the runs made, as `author changes` lists them) become a captured folder "
        "--name in the package, mounted at --target (a waiting node takes it in place); --keys KEYS become a REG node. "
        "Each capture is a new node of the package, built on the session's node, in the package's build session "
        "(unsaved: `build show` lists it, `build save` writes it).",
        { Opt("files", "the roots to capture (a,b)"), Opt("name", "(--files) the folder's name"), Opt("target", "(--files) where it mounts (%GameDir%, …)"),
          Opt("keys", "registry keys to capture (a,b)"), Timeout() },
        { "author capture --files 'pfx/drive_c/GOG Games/X' --name X --target %GameDir%", "author capture --keys 'HKLM\\Software\\X'" },
        { "AuthoringSessionModel::captureFiles", "AuthoringWorker::captureFiles", "AuthoringSession::CopySelection",
          "AuthoringSessionModel::captureSelectedRegistry", "PackageEditorModel::fillWaiting" }, Needs::App, false,
        [](Command::Context &, const json &A) {
            Session &S = Live();
            const long long From = S.Events;
            if (A.contains("files"))
                S.M->captureFiles(QStrs(List(A, "files")), QString::fromStdString(Command::Str(A, "name", "")), QString::fromStdString(Command::Str(A, "target", "")));
            else if (A.contains("keys")) S.M->captureSelectedRegistry(QStrs(List(A, "keys")));
            else throw Command::BadArgs("give --files or --keys");
            return Settled(S, From, Ms(A));
        }, {} });

    Command::Register({
        "author end", "end the session: the runtime is unmounted (captures already made stay in the package)", "",
        { Timeout() }, { "author end" }, { "AuthoringWorker::end", "AuthoringSession::End" }, Needs::App, false,
        [](Command::Context &, const json &A) {
            Session &S = Live();
            json Out = State(S);
            The().reset();
            Until([] { return false; }, std::min<long long>(Ms(A), 300));   // let the worker's teardown run
            Out["ended"] = true;
            return Out;
        }, {} });

    Command::Register({
        "author window", "open the authoring window on a node (the session with its window)",
        "The window the editor's capture actions open: its file and registry trees, runner choice, capture buttons. "
        "Drive it with `ui` (ui tree shows it).",
        { Pos("package", "the package"), Pos("node", "the node"), Opt("mode", "what to capture", Type::String, false, { "setup", "files", "registry" }) },
        { "author window 749 'The Age of Kings' --mode files" },
        { "DeltaTree::setPaths", "DeltaTree::setSeparator", "DeltaTree::MainFolder", "DeltaTree::markCaptured", "DeltaTree::reveal", "DeltaTree::checkedRoots",
          "DeltaTree::checkedEntries", "DeltaTree::checkAll" }, Needs::App, false,
        [](Command::Context &C, const json &A) {
            PackageEditor *Ed = PackageEditor::live();
            const fs::path P = PackageDir(C, Command::Str(A, "package"), false);
            if (!Ed || !Ed->model() || !fs::equivalent(Ed->bundleDir().toStdString(), P))
                throw std::runtime_error("open the package in the editor first (VidyaGod editor open " + Command::Str(A, "package") + ")");
            PackageEditorModel *E = Ed->model();
            const std::string Q = Command::Str(A, "node");
            int I = E->doc().IndexOf(Q);
            for (int K = 0; I < 0 && K < E->doc().Count(); ++K) if (E->doc().Node(K).value("LABEL", std::string()) == Q) I = K;
            if (I < 0) throw Command::BadArgs("the package has no node '" + Q + "'");
            static const std::map<std::string, CaptureMode> Modes = {{"setup", CaptureMode::Setup}, {"files", CaptureMode::Files}, {"registry", CaptureMode::Registry}};
            auto *W = new AuthoringSessionWindow(E, E->doc().Handle(I), Modes.at(Command::Str(A, "mode", "setup")));
            W->setAttribute(Qt::WA_DeleteOnClose);
            W->show();
            return json{{"window", W->objectName().toStdString()}, {"follow", "VidyaGod ui tree"}};
        }, {} });

    Command::Register({
        "author preview", "where a capture would land and how its paths are anchored (no session needed)",
        "The capture rules on their own. With --roots and --target (and --content-root, --anchors as \"%Anchor%=layout\" "
        "pairs): the folder the target mounts at, whether the roots are captured as contents, each root's guest path "
        "and its Windows-spelled anchor, every guest → anchor pair a capture names, a value re-anchored (--value), a free "
        "folder name in a package (--name, --package); with --runners id=label pairs, the runner menu; with --node, "
        "where that node waits for its files and the name its capture gets.",
        { Opt("roots", "captured roots (a,b)"), Opt("target", "the TARGET"), Opt("content-root", "the runner's content root"), Opt("anchors", "%Anchor%=layout pairs (a,b)"),
          Opt("waits-at", "where the node waits for its files"), Opt("value", "a registry value to re-anchor"), Opt("name", "a folder name"), Opt("package", "the package (for --name, --node)"),
          Opt("runners", "id=label pairs"), Opt("node", "a node of --package") },
        { "author preview --roots 'pfx/drive_c/GOG Games/X' --target %GameDir% --content-root pfx/drive_c/Games",
          "author preview --runners geproton_10_34=GE-Proton10-34,geproton_9_27=GE-Proton9-27" },
        { "CapturePreview::EncodeAnchors", "CapturePreview::ParseAnchors", "CapturePreview::MountBase", "CapturePreview::CapturesContents",
          "CapturePreview::PreviewLines", "CapturePreview::GuestPathOf", "CapturePreview::AnchorWinpath", "CapturePreview::GuestAnchorsFor",
          "CapturePreview::AnchoredGuestsOf", "CapturePreview::Reanchor", "CapturePreview::FreeName", "CapturePreview::RunnerChoices",
          "CapturePreview::WaitingTarget", "CapturePreview::CaptureName" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            json Out = json::object();
            std::vector<std::pair<std::string, std::string>> Lay;
            for (const auto &[K, V] : Pairs(A, "anchors")) Lay.emplace_back(K, V);
            const QMap<QString, QString> Anchors = CapturePreview::ParseAnchors(CapturePreview::EncodeAnchors(Lay));
            const QStringList Roots = QStrs(List(A, "roots"));
            const QString Target = QString::fromStdString(Command::Str(A, "target", "")), Cr = QString::fromStdString(Command::Str(A, "content-root", ""));
            if (!Roots.isEmpty())
            {
                const bool Contents = CapturePreview::CapturesContents(Roots, Target, QString::fromStdString(Command::Str(A, "waits-at", "")), Cr, Anchors);
                json Each = json::array();
                for (const QString &R : Roots)
                {
                    const std::string Rr = R.toStdString();
                    Each.push_back({{"root", Rr}, {"guest", CapturePreview::GuestPathOf(Rr)}, {"anchor", CapturePreview::AnchorWinpath(Target.toStdString(), Rr, Contents)},
                                    {"already_anchored_as", CapturePreview::AnchoredGuestsOf(Rr, Anchors)}});
                }
                json Pairs = json::array();
                const auto G = CapturePreview::GuestAnchorsFor(Roots, Target.toStdString(), Contents, Anchors);
                for (const auto &[Guest, An] : G) Pairs.push_back({{"guest", Guest}, {"anchor", An}});
                Out.update({{"mounts_at", CapturePreview::MountBase(Target, Cr, Anchors).toStdString()}, {"as_contents", Contents}, {"roots", Each},
                            {"names", Pairs}, {"preview", CapturePreview::PreviewLines(Roots).toStdString()}});
                if (A.contains("value")) Out["reanchored"] = CapturePreview::Reanchor(Command::Str(A, "value"), G);
            }
            if (A.contains("runners"))
            {
                std::vector<std::pair<std::string, std::string>> R;
                for (const auto &[Id, L] : Pairs(A, "runners")) R.emplace_back(Id, L);
                Out["runner_menu"] = Strs(CapturePreview::RunnerChoices(R));
            }
            if (A.contains("package"))
            {
                const fs::path P = PackageDir(C, Command::Str(A, "package"), false);
                if (A.contains("name")) Out["free_name"] = CapturePreview::FreeName(QString::fromStdString(Command::Str(A, "name")), QString::fromStdString(P.string())).toStdString();
                if (A.contains("node"))
                {
                    PackageEditorModel E(C.Config, nullptr);
                    E.initPackage(QString::fromStdString(P.string()), nullptr);
                    const int I = E.doc().IndexOf(Command::Str(A, "node"));
                    if (I < 0) throw Command::BadArgs("no such node (its handle)");
                    Out["waits_at"] = CapturePreview::WaitingTarget(E.doc().Node(I)).toStdString();
                    Out["capture_name"] = CapturePreview::CaptureName(E.doc().Node(I), E.doc().Handle(I)).toStdString();
                }
            }
            if (Out.empty()) throw Command::BadArgs("give --roots, --runners, or --package with --name / --node");
            return Out;
        }, {} });
}
