// `VidyaGod editor …` and `canvas …` — the package editor window and its canvas: open and close it, read what the
// canvas shows (its nodes, wires, camera, and every control the last frame drew), move its view, edit through it,
// press its controls with the panel's own input path, and run a node's actions with the answers to their questions
// given on the command line. And the action helpers the canvas's actions are built on, on their own.
#include "command.h"
#include "cmdutil.h"
#include "eventlog.h"
#include "imguiprobe.h"
#include "packageeditor.h"
#include "packageeditormodel.h"
#include "pkgactions.h"
#include "pkgcanvas.h"
#include "pkgcanvaspanel.h"
#include "pkgdoc.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QThread>
#include <QWheelEvent>
#include <fstream>
#include <sstream>

namespace
{
using json = nlohmann::ordered_json;
using namespace CmdUtil;
namespace fs = std::filesystem;

PackageEditor &Editor()
{
    PackageEditor *E = PackageEditor::live();
    if (!E || !E->model() || !E->canvas()) throw std::runtime_error("the package editor is not open (VidyaGod editor open <package>)");
    return *E;
}

PkgCanvas &Canvas() { return *Editor().canvas()->canvas(); }

// Let the app run until the canvas has drawn a frame after this point (or a second has passed).
long long Settle()
{
    ImGuiProbe::SetRecording(true);
    PkgCanvasPanel *P = Editor().canvas();
    const long long From = json::parse(ImGuiProbe::FrameJson()).value("frame", 0LL);
    P->requestFrame();
    QElapsedTimer T;
    T.start();
    long long Now = From;
    while (T.elapsed() < 1000 && Now == From)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        Now = json::parse(ImGuiProbe::FrameJson()).value("frame", 0LL);
        if (Now == From) QThread::msleep(5);
    }
    return Now;
}

std::string Handle(const std::string &Q)
{
    const PkgDoc::Document &D = Editor().model()->doc();
    if (D.IndexOf(Q) >= 0) return Q;
    for (int I = 0; I < D.Count(); ++I) if (D.Node(I).value("LABEL", std::string()) == Q) return D.Handle(I);
    if (!Q.empty() && Q.find_first_not_of("0123456789") == std::string::npos && std::stoi(Q) < D.Count()) return D.Handle(std::stoi(Q));
    throw Command::BadArgs("the open package has no node '" + Q + "'");
}

json Rect(bool Ok, float X0, float Y0, float X1, float Y1) { return Ok ? json{X0, Y0, X1, Y1} : json(nullptr); }

void Mouse(QWidget *W, QEvent::Type T, QPointF P, Qt::MouseButton B, Qt::MouseButtons Held)
{
    QMouseEvent E(T, P, W->mapToGlobal(P), B, Held, Qt::NoModifier);
    QCoreApplication::sendEvent(W, &E);
}

// A click (or double click, or a right click) at P in the panel, as the mouse would make it.
void Click(QPointF P, Qt::MouseButton B, bool Double)
{
    QWidget *W = Editor().canvas();
    Mouse(W, QEvent::MouseMove, P, Qt::NoButton, Qt::NoButton);
    Settle();
    Mouse(W, QEvent::MouseButtonPress, P, B, B);
    Settle();
    Mouse(W, QEvent::MouseButtonRelease, P, B, Qt::NoButton);
    if (Double)
    {
        Mouse(W, QEvent::MouseButtonDblClick, P, B, B);
        Mouse(W, QEvent::MouseButtonRelease, P, B, Qt::NoButton);
    }
    Settle();
}

std::string Slurp(const std::string &P)
{
    std::ifstream In(P, std::ios::binary);
    if (!In) throw Command::BadArgs("cannot read '" + P + "'");
    std::stringstream S;
    S << In.rdbuf();
    return S.str();
}

} // namespace

VG_COMMANDS
{
    using Command::Type;
    using Command::Needs;
    using Command::Pos;
    using Command::Opt;
    using Command::Flag;
    Command::RegisterGroup("editor", "the package editor window: open it on a package, close it");
    Command::RegisterGroup("canvas", "the editor's canvas: what it shows, its view, editing, pressing its controls, node actions");

    Command::Register({
        "editor open", "open the package editor on a package (the one editor window)",
        "Opens (or raises) the editor and loads the package — what Library → Edit package does. `build` commands then "
        "act on the editor's own document, and `canvas` commands on what it shows.",
        { Pos("package", "a folder path, its name, its UID, or part of its title") }, { "editor open 749" },
        { "PackageEditor::OpenFor", "PkgCanvas::setExternalLookup", "PkgCanvas::setOffers", "PkgCanvas::setPlatforms", "PkgCanvas::setImageLookup",
          "PkgCanvas::setPackageOpen", "PkgCanvas::documentReset", "PkgCanvas::setIssues", "PkgActions::refreshHints", "PkgCanvas::setNodeHints",
          "JsonRawEditor::refresh", "PkgInsight::refresh" }, Needs::Ui, false,
        [](Command::Context &C, const json &A) {
            const fs::path P = PackageDir(C, Command::Str(A, "package"), false);
            bool Created = false;
            PackageEditor *E = PackageEditor::OpenFor(C.Config, C.Window, QString::fromStdString(P.string()), &Created);
            if (!E) throw std::runtime_error("could not open the editor");
            E->show();
            if (Created) EventLog::AttachTree(E);   // its canvas, actions and panels: `events` sees what they do
            Settle();
            return json{{"package", E->bundleDir().toStdString()}, {"opened", Created}, {"nodes", E->model()->doc().Count()}};
        }, {} });

    Command::Register({
        "editor close", "close the package editor (unsaved edits: --discard to drop them)",
        "Closes the editor window. With unsaved edits it would ask; --discard drops them first.", { Flag("discard", "drop unsaved edits") },
        { "editor close", "editor close --discard" }, { "PackageEditor::reject" }, Needs::Ui, false,
        [](Command::Context &, const json &A) {
            PackageEditor &E = Editor();
            if (E.model()->isDirty())
            {
                if (!Command::Bool(A, "discard", false)) throw Command::BadArgs("the editor has unsaved edits: build save <package>, or --discard");
                E.model()->LoadNodes();
            }
            E.close();   // a dialog's close goes through reject(): with nothing unsaved, it asks nothing
            return json{{"closed", true}};
        }, {} });

    // ---------------- canvas: what it shows ----------------
    Command::Register({
        "canvas state", "what the canvas shows: its view, selection, every node's box, wires, chips",
        "The camera and zoom, the canvas's rectangle, what is selected, how many nodes and wires the last frame drew, "
        "the outside chips (with the text each shows), the tour; and per node its box on screen and in the world, its "
        "in / out ports, each row's position, its cover chip, and whether an action holds it.", {}, { "canvas state" },
        { "PkgCanvas::zoom", "PkgCanvas::cameraPos", "PkgCanvas::canvasRect", "PkgCanvas::selection", "PkgCanvas::selectedHandle",
          "PkgCanvas::visibleNodes", "PkgCanvas::visibleWires", "PkgCanvas::externalCount", "PkgCanvas::externalLabel", "PkgCanvas::touring",
          "PkgCanvas::packageOpen", "PkgCanvas::wantsFrames", "PkgCanvas::nodeRect", "PkgCanvas::worldRect", "PkgCanvas::portPos",
          "PkgCanvas::rowPos", "PkgCanvas::coverShown", "PkgCanvas::isBusy", "PackageEditor::canvas", "PkgCanvasPanel::canvas",
          "PkgCanvasPanel::requestFrame" }, Needs::Ui, false,
        [](Command::Context &, const json &) {
            Settle();
            PkgCanvas &Cv = Canvas();
            const PkgDoc::Document &D = Editor().model()->doc();
            float X = 0, Y = 0, X0 = 0, Y0 = 0, X1 = 0, Y1 = 0;
            Cv.cameraPos(X, Y);
            const bool HasRect = Cv.canvasRect(X0, Y0, X1, Y1);
            json Nodes = json::array();
            std::set<std::string> Ext;
            for (int I = 0; I < D.Count(); ++I)
            {
                const std::string H = D.Handle(I);
                float A0 = 0, B0 = 0, A1 = 0, B1 = 0, Wx = 0, Wy = 0, Ww = 0, Wh = 0, Ix = 0, Iy = 0, Ox = 0, Oy = 0;
                const bool Shown = Cv.nodeRect(H, A0, B0, A1, B1);
                const bool World = Cv.worldRect(H, Wx, Wy, Ww, Wh);
                const bool In = Cv.portPos(H, false, Ix, Iy), Out = Cv.portPos(H, true, Ox, Oy);
                json Rows = json::array();
                for (size_t L = 0; L < D.Node(I).value("LAYERS", json::array()).size(); ++L)
                {
                    float Rx = 0, Ry = 0;
                    Rows.push_back(Cv.rowPos(H, static_cast<int>(L), Rx, Ry) ? json{Rx, Ry} : json(nullptr));
                }
                Nodes.push_back({{"handle", H}, {"label", D.Node(I).value("LABEL", std::string())}, {"screen", Rect(Shown, A0, B0, A1, B1)},
                                 {"world", World ? json{Wx, Wy, Ww, Wh} : json(nullptr)}, {"in", In ? json{Ix, Iy} : json(nullptr)},
                                 {"out", Out ? json{Ox, Oy} : json(nullptr)}, {"rows", Rows}, {"cover", Cv.coverShown(H)}, {"busy", Cv.isBusy(H)}});
                PkgDoc::ForEachRef(D.Node(I), [&](const std::string &R) { if (D.IndexOf(R) < 0) Ext.insert(R); });
            }
            json Chips = json::object();
            for (const auto &E : Ext) Chips[E] = Cv.externalLabel(E);
            const auto Sel = Cv.selection();
            return json{{"package_open", Cv.packageOpen()}, {"zoom", Cv.zoom()}, {"camera", {X, Y}}, {"rect", Rect(HasRect, X0, Y0, X1, Y1)},
                        {"selected", Cv.selectedHandle()}, {"selection", std::vector<std::string>(Sel.begin(), Sel.end())},
                        {"drawn_nodes", Cv.visibleNodes()}, {"drawn_wires", Cv.visibleWires()}, {"chips", Cv.externalCount()}, {"chip_labels", Chips},
                        {"touring", Cv.touring()}, {"animating", Cv.wantsFrames()}, {"nodes", Nodes}};
        }, {} });

    Command::Register({
        "canvas items", "every control the canvas drew in its last frame: label, window, rectangle",
        "The canvas has no widgets; this is what its frame laid out — each button, field, row and menu item with its "
        "label as shown, the window it is in, and its rectangle in the canvas's coordinates. Filter with --label / "
        "--window (a part of the name).", { Opt("label", "only items whose label contains this"), Opt("window", "only in windows whose name contains this") },
        { "canvas items", "canvas items --label Save", "canvas items --window Patch --json" },
        { "ImGuiProbe::FrameJson", "ImGuiProbe::SetRecording", "ImGuiProbe::Enabled", "ImGuiProbe::BeginFrame" }, Needs::Ui, false,
        [](Command::Context &, const json &A) {
            Settle();
            json F = json::parse(ImGuiProbe::FrameJson());
            const std::string L = Command::Str(A, "label", ""), W = Command::Str(A, "window", "");
            json Out = json::array();
            for (const auto &I : F["items"])
                if ((L.empty() || I.value("label", std::string()).find(L) != std::string::npos) && (W.empty() || I.value("window", std::string()).find(W) != std::string::npos))
                    Out.push_back({{"label", I["label"]}, {"window", I["window"]}, {"rect", I["rect"]}, {"id", I["id"]}});
            return json{{"frame", F["frame"]}, {"items", Out}};
        }, {} });

    Command::Register({
        "canvas press", "press a control the canvas drew, by its label (a real click through the panel)",
        "Finds the Nth item labelled <label> (in a window whose name holds --window) in the last frame and clicks its "
        "centre with the mouse, through the canvas panel's own input; --right for a right click, --double for a "
        "double click. A label inside a menu needs the menu opened first (press its button).",
        { Pos("label", "the label as shown"), Opt("nth", "which of several (0-based)", Type::Int), Opt("window", "the window (a part of its name)"),
          Flag("right", "a right click"), Flag("double", "a double click") },
        { "canvas press 'bring in files...'", "canvas press 'Patch 2.0a' --double", "canvas press OK --window Confirm" },
        { "ImGuiProbe::FindItem", "ImGuiProbe::NameLast" }, Needs::Ui, false,
        [](Command::Context &, const json &A) {
            Settle();
            float X0 = 0, Y0 = 0, X1 = 0, Y1 = 0;
            const std::string L = Command::Str(A, "label"), W = Command::Str(A, "window", "");
            if (!ImGuiProbe::FindItem(L.c_str(), X0, Y0, X1, Y1, static_cast<int>(Command::Int(A, "nth", 0)), W.empty() ? nullptr : W.c_str()))
                throw Command::BadArgs("the canvas shows no '" + L + "' (canvas items lists what it shows)");
            Click(QPointF((X0 + X1) / 2, (Y0 + Y1) / 2), Command::Bool(A, "right", false) ? Qt::RightButton : Qt::LeftButton, Command::Bool(A, "double", false));
            return json{{"pressed", L}, {"at", {(X0 + X1) / 2, (Y0 + Y1) / 2}}};
        }, {} });

    Command::Register({
        "canvas mouse", "the mouse on the canvas: click, drag, move or wheel at canvas coordinates",
        "Raw input through the panel: --click (or --right, --double) at x,y; --drag-to x,y presses at x,y, moves and "
        "releases there; --move only moves; --wheel N scrolls N notches (+ up / zoom in). Also prints which node is "
        "under x,y and the world point there.",
        { Pos("x", "x", Type::Int), Pos("y", "y", Type::Int), Flag("click", "click there"), Flag("right", "a right click"), Flag("double", "a double click"),
          Opt("drag-to", "x,y to drag to"), Flag("move", "only move there"), Opt("wheel", "notches to scroll", Type::Int) },
        { "canvas mouse 400 300 --click", "canvas mouse 400 300 --drag-to 600 300", "canvas mouse 400 300 --wheel 2" },
        { "PkgCanvas::nodeAt", "PkgCanvas::screenToWorld" }, Needs::Ui, false,
        [](Command::Context &, const json &A) {
            const QPointF P(static_cast<double>(Command::Int(A, "x")), static_cast<double>(Command::Int(A, "y")));
            QWidget *W = Editor().canvas();
            float Wx = 0, Wy = 0;
            Canvas().screenToWorld(static_cast<float>(P.x()), static_cast<float>(P.y()), Wx, Wy);
            json Out = {{"node", Canvas().nodeAt(static_cast<float>(P.x()), static_cast<float>(P.y()))}, {"world", {Wx, Wy}}};
            if (A.contains("drag-to"))
            {
                const auto T = List(A, "drag-to");
                if (T.size() != 2) throw Command::BadArgs("--drag-to takes x,y");
                const QPointF Q(std::stod(T[0]), std::stod(T[1]));
                Mouse(W, QEvent::MouseMove, P, Qt::NoButton, Qt::NoButton);
                Settle();
                Mouse(W, QEvent::MouseButtonPress, P, Qt::LeftButton, Qt::LeftButton);
                Settle();
                for (int S = 1; S <= 8; ++S) { Mouse(W, QEvent::MouseMove, P + (Q - P) * (S / 8.0), Qt::NoButton, Qt::LeftButton); Settle(); }
                Mouse(W, QEvent::MouseButtonRelease, Q, Qt::LeftButton, Qt::NoButton);
                Settle();
            }
            else if (A.contains("wheel"))
            {
                QWheelEvent E(P, W->mapToGlobal(P), QPoint(), QPoint(0, static_cast<int>(Command::Int(A, "wheel")) * 120), Qt::NoButton, Qt::NoModifier,
                              Qt::NoScrollPhase, false);
                QCoreApplication::sendEvent(W, &E);
                Settle();
            }
            else if (Command::Bool(A, "move", false)) { Mouse(W, QEvent::MouseMove, P, Qt::NoButton, Qt::NoButton); Settle(); }
            else if (Command::Bool(A, "click", false) || Command::Bool(A, "right", false) || Command::Bool(A, "double", false))
                Click(P, Command::Bool(A, "right", false) ? Qt::RightButton : Qt::LeftButton, Command::Bool(A, "double", false));
            return Out;
        }, {} });

    Command::Register({
        "canvas keys", "keys or text into the canvas (shortcuts, typing into a field it shows)",
        "--keys sends key presses (\"Ctrl+Z\", \"Delete\", \"Return\", several comma-separated); --text types text into "
        "the focused field.", { Opt("keys", "key sequences (a,b)"), Opt("text", "text to type") },
        { "canvas keys --keys Ctrl+A,Delete", "canvas keys --text 'Patch 2.0b'" }, {}, Needs::Ui, false,
        [](Command::Context &, const json &A) {
            QWidget *W = Editor().canvas();
            W->setFocus();
            for (const auto &K : List(A, "keys"))
            {
                const QKeySequence Seq(QString::fromStdString(K));
                if (Seq.isEmpty()) throw Command::BadArgs("not a key: " + K);
                const QKeyCombination Kc = Seq[0];
                QKeyEvent Dn(QEvent::KeyPress, Kc.key(), Kc.keyboardModifiers()), Up(QEvent::KeyRelease, Kc.key(), Kc.keyboardModifiers());
                QCoreApplication::sendEvent(W, &Dn);
                Settle();
                QCoreApplication::sendEvent(W, &Up);
                Settle();
            }
            if (A.contains("text"))
            {
                QKeyEvent T(QEvent::KeyPress, 0, Qt::NoModifier, QString::fromStdString(Command::Str(A, "text")));
                QCoreApplication::sendEvent(W, &T);
                Settle();
            }
            return json{{"ok", true}};
        }, {} });

    // ---------------- canvas: its view and editing through it ----------------
    Command::Register({
        "canvas view", "move the canvas's view: select, frame, zoom, pan, expand, tidy, the tour",
        "--select NODE selects it (--frame also brings it into view); --frame-all shows every node; --frame-selection "
        "the selected ones; --zoom Z; --camera x,y puts that world point at the top-left; --expand NODE / --fold NODE "
        "opens or folds every row of a node; --tidy forgets this machine's positions (back to the layout); --tour "
        "starts the first-time tour.",
        { Opt("select", "a node"), Flag("frame", "(--select) bring it into view"), Flag("frame-all", "show every node"),
          Flag("frame-selection", "show the selected nodes"), Opt("zoom", "the zoom (1 = 100%)"), Opt("camera", "x,y"), Opt("expand", "a node"),
          Opt("fold", "a node"), Flag("tidy", "back to the computed layout"), Flag("tour", "start the tour") },
        { "canvas view --select 'Patch 2.0a' --frame", "canvas view --frame-all", "canvas view --zoom 0.5" },
        { "PkgCanvas::select", "PkgCanvas::frameAll", "PkgCanvas::frameSelection", "PkgCanvas::setZoom", "PkgCanvas::setCamera",
          "PkgCanvas::setExpanded", "PkgCanvas::tidyLayout", "PkgCanvas::startTour", "JsonRawEditor::showNode", "PkgInsight::showNode" },
        Needs::Ui, false,
        [](Command::Context &, const json &A) {
            PkgCanvas &Cv = Canvas();
            if (A.contains("select")) Cv.select(Handle(Command::Str(A, "select")), Command::Bool(A, "frame", false));
            if (Command::Bool(A, "frame-all", false)) Cv.frameAll();
            if (Command::Bool(A, "frame-selection", false)) Cv.frameSelection();
            if (A.contains("zoom")) Cv.setZoom(std::stof(Command::Str(A, "zoom")));
            if (A.contains("camera"))
            {
                const auto P = List(A, "camera");
                if (P.size() != 2) throw Command::BadArgs("--camera takes x,y");
                Cv.setCamera(std::stof(P[0]), std::stof(P[1]));
            }
            if (A.contains("expand")) Cv.setExpanded(Handle(Command::Str(A, "expand")), true);
            if (A.contains("fold")) Cv.setExpanded(Handle(Command::Str(A, "fold")), false);
            if (Command::Bool(A, "tidy", false)) Cv.tidyLayout();
            if (Command::Bool(A, "tour", false)) Cv.startTour();
            Settle();
            float X = 0, Y = 0;
            Cv.cameraPos(X, Y);
            return json{{"zoom", Cv.zoom()}, {"camera", {X, Y}}, {"selected", Cv.selectedHandle()}};
        }, {} });

    Command::Register({
        "canvas edit", "edit through the canvas: add a node, carry a layer, delete nodes, undo, redo",
        "What the canvas's own gestures do (each one undo step, the editor updates): --add TYPE puts a new node of that "
        "layer type at --at x,y in the world (--ref: one that names that node); --carry NODE:LAYER --to NODE[:AT] "
        "moves a layer between nodes (--copy keeps it); --delete NODES removes them; --undo / --redo.",
        { Opt("add", "a layer type"), Opt("at", "(--add) x,y in the world"), Opt("ref", "(--add) the node it names"), Opt("carry", "NODE:LAYER"),
          Opt("to", "(--carry) NODE or NODE:AT"), Flag("copy", "(--carry) copy"), Opt("delete", "nodes (a,b)"), Flag("undo", "undo"), Flag("redo", "redo") },
        { "canvas edit --add ZIP --at 100,200", "canvas edit --carry 'Patch 2.0a':4 --to 'Original'", "canvas edit --undo" },
        { "PkgCanvas::addNode", "PkgCanvas::MoveLayer", "PkgCanvas::removeNodes", "PkgCanvas::undo", "PkgCanvas::redo" }, Needs::Ui, false,
        [](Command::Context &, const json &A) {
            PkgCanvas &Cv = Canvas();
            json Out = json::object();
            if (A.contains("add"))
            {
                const auto P = List(A, "at");
                const std::string Ref = A.contains("ref") ? Handle(Command::Str(A, "ref")) : std::string();
                Out["added"] = Cv.addNode(Command::Str(A, "add"), P.size() == 2 ? std::stof(P[0]) : 0.f, P.size() == 2 ? std::stof(P[1]) : 0.f, Ref);
            }
            if (A.contains("carry"))
            {
                const std::string F = Command::Str(A, "carry"), T = Command::Str(A, "to", "");
                const auto Fc = F.rfind(':'), Tc = T.rfind(':');
                if (Fc == std::string::npos || T.empty()) throw Command::BadArgs("--carry NODE:LAYER --to NODE[:AT]");
                std::string Why;
                const bool Ok = Cv.MoveLayer(Handle(F.substr(0, Fc)), std::stoi(F.substr(Fc + 1)), Handle(Tc == std::string::npos ? T : T.substr(0, Tc)),
                                             Tc == std::string::npos ? -1 : std::stoi(T.substr(Tc + 1)), Command::Bool(A, "copy", false), &Why);
                if (!Ok) throw std::runtime_error(Why);
                Out["carried"] = true;
            }
            if (A.contains("delete"))
            {
                std::set<std::string> H;
                for (const auto &N : List(A, "delete")) H.insert(Handle(N));
                Out["deleted"] = Cv.removeNodes(H);
            }
            if (Command::Bool(A, "undo", false)) Cv.undo();
            if (Command::Bool(A, "redo", false)) Cv.redo();
            Settle();
            Out["dirty"] = Editor().model()->isDirty();
            return Out;
        }, {} });

    Command::Register({
        "canvas action", "run a node's action (bring in files, undelta, capture, test launch, …) with its answers given",
        "What a node's action button does (build node lists a node's actions). Its questions are answered here instead "
        "of in a dialog: --yes answers a confirmation yes (else no), --pick PATH is the file or folder a picker would "
        "return (none: cancelled), --choose N the option chosen; what it reports is printed. --drop PATHS drops files on "
        "the node as the file manager would; --cancel stops a running action. A long action runs on: follow it with "
        "`canvas state` (busy) or `events follow --object PkgActions`.",
        { Pos("node", "the node"), Opt("action", "the action's id"), Flag("yes", "answer confirmations yes"), Opt("pick", "what a file / folder picker returns"),
          Opt("choose", "the option chosen in a choice", Type::Int), Opt("drop", "files to drop on it (a,b)"), Flag("cancel", "cancel its running action") },
        { "canvas action 'Original' --action bring_in --pick ~/Games/AoE2", "canvas action 'Patch 2.0a' --action undelta --yes",
          "canvas action 'Original' --drop ~/cover.png" },
        { "PkgActions::perform", "PkgActions::dropFiles", "PkgActions::cancel", "PkgActions::setConfirmHandler", "PkgActions::setNotifyHandler",
          "PkgActions::setPickHandler", "PkgActions::setChooseHandler", "PackageEditor::actions", "PkgCanvas::beginAction", "PkgCanvas::setProgress",
          "PkgCanvas::endAction" }, Needs::Ui, false,
        [](Command::Context &, const json &A) {
            PackageEditor &E = Editor();
            PkgActions &Act = *E.actions();
            const QString H = QString::fromStdString(Handle(Command::Str(A, "node")));
            if (Command::Bool(A, "cancel", false)) { Act.cancel(H); return json{{"cancelled", H.toStdString()}}; }
            json Said = json::array();
            const bool Yes = Command::Bool(A, "yes", false);
            QString Picked = QString::fromStdString(Command::Str(A, "pick", ""));
            const int Choice = static_cast<int>(Command::Int(A, "choose", -1));
            Act.setConfirmHandler([&Said, Yes](const QString &T, const QString &B) { Said.push_back({{"asked", T.toStdString()}, {"text", B.toStdString()}, {"answer", Yes}}); return Yes; });
            Act.setNotifyHandler([&Said](const QString &T, const QString &B) { Said.push_back({{"told", T.toStdString()}, {"text", B.toStdString()}}); });
            Act.setPickHandler([&Said, Picked](const QString &T, const QString &, const QString &, bool Dir) {
                Said.push_back({{"pick", T.toStdString()}, {"folder", Dir}, {"answer", Picked.toStdString()}});
                return Picked;
            });
            Act.setChooseHandler([&Said, Choice](const QString &T, const QString &, const QStringList &O) {
                json Opts = json::array();
                for (const QString &X : O) Opts.push_back(X.toStdString());
                Said.push_back({{"choose", T.toStdString()}, {"options", Opts}, {"answer", Choice}});
                return Choice;
            });
            if (A.contains("drop"))
            {
                QStringList P;
                for (const auto &X : List(A, "drop")) P << QString::fromStdString(X);
                Act.dropFiles(H, P);
            }
            else if (A.contains("action")) Act.perform(H, QString::fromStdString(Command::Str(A, "action")));
            else throw Command::BadArgs("give --action, --drop or --cancel");
            Settle();
            // Back to the editor's own dialogs for anything the action asks once this call is over.
            Act.setConfirmHandler({});
            Act.setNotifyHandler({});
            Act.setPickHandler({});
            Act.setChooseHandler({});
            return json{{"node", H.toStdString()}, {"said", Said}, {"busy", Canvas().isBusy(H.toStdString())}, {"dirty", E.model()->isDirty()}};
        }, {} });

    // ---------------- the action helpers on their own ----------------
    Command::Register({
        "canvas helpers", "what the actions read from files: a .reg export, a program's platform, GOG's script, Unity's saves",
        "The pure parts of the node actions: --reg FILE reads a .reg export as registry rows (and counts the deletions a "
        "REG layer cannot hold); --platform-of FILE says what a program runs on from its name and first bytes; --gog "
        "SCRIPT turns a goggame-<id>.script into layers (--support DIR, --arch 32|64); --unity APP.INFO gives the KEEP "
        "layer for a Unity game's saves; --programs a,b picks which program is the game.",
        { Opt("reg", "a .reg file"), Opt("platform-of", "a program"), Opt("gog", "a goggame-*.script"), Opt("support", "(--gog) its support folder"),
          Opt("arch", "(--gog) 32 or 64"), Opt("unity", "an app.info"), Opt("programs", "programs, anchored (a,b)") },
        { "canvas helpers --reg tonic.reg", "canvas helpers --platform-of ~/Games/x/game.exe", "canvas helpers --programs '%GameDir%/setup.exe,%GameDir%/game.exe'" },
        { "PkgActions::ParseRegExport", "PkgActions::PlatformOf", "PkgActions::GogInstallSteps", "PkgActions::UnityKeep", "PkgActions::LikelyProgram" },
        Needs::Config, false,
        [](Command::Context &, const json &A) {
            json Out = json::object();
            if (A.contains("reg"))
            {
                int Del = 0;
                json Rows = json::array();
                for (const auto &R : PkgActions::ParseRegExport(QString::fromStdString(Slurp(Command::Str(A, "reg"))), &Del))
                    Rows.push_back({{"path", R.Path}, {"name", R.Name}, {"value", R.Value}, {"key_only", R.KeyOnly}});
                Out["rows"] = Rows;
                Out["deletions"] = Del;
            }
            if (A.contains("platform-of"))
            {
                const std::string P = Command::Str(A, "platform-of");
                std::ifstream In(P, std::ios::binary);
                std::string Head(4096, '\0');
                In.read(Head.data(), static_cast<std::streamsize>(Head.size()));
                Head.resize(static_cast<size_t>(In.gcount()));
                Out["platform"] = PkgActions::PlatformOf(fs::path(P).filename().string(), Head);
            }
            if (A.contains("gog"))
            {
                const json Script = json::parse(Slurp(Command::Str(A, "gog")), nullptr, false);
                if (Script.is_discarded()) throw Command::BadArgs("--gog: not JSON");
                const auto G = PkgActions::GogInstallSteps(Script, Command::Str(A, "support", ""), Command::Str(A, "arch", "32"));
                json Copies = json::array();
                for (const auto &[From, To] : G.Copies) Copies.push_back({{"from", From.string()}, {"as", To}});
                Out["gog"] = {{"layers", G.Layers}, {"copies", Copies}, {"said", G.Said}, {"skipped", G.Skipped}};
            }
            if (A.contains("unity")) Out["unity_keep"] = PkgActions::UnityKeep(Slurp(Command::Str(A, "unity")));
            if (A.contains("programs"))
            {
                const auto P = List(A, "programs");
                const int I = PkgActions::LikelyProgram(P);
                Out["game_program"] = I < 0 ? json(nullptr) : json(P[static_cast<size_t>(I)]);
            }
            if (Out.empty()) throw Command::BadArgs("give --reg, --platform-of, --gog, --unity or --programs");
            return Out;
        }, {} });
}
