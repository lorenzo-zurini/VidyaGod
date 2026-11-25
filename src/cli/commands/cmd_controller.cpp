// `VidyaGod controller …` — the controller UI: controller mode on or off, a controller's presses (the same delivery a
// real controller's take), the exact state of what they act on (the window, the focus, the shell's section and page,
// the on-screen keyboard), the controllers connected, and the controller settings.
#include "command.h"
#include "cmdutil.h"
#include "uifind.h"   // Act — a press that opens a dialog answers at once

#include "appmodel.h"
#include "apppaths.h"
#include "decklist.h"
#include "networktab.h"
#include "peersharing.h"
#include "qrcode.h"
#include "controllerlayout.h"
#include "controllerservice.h"
#include "gameoverlay.h"
#include "matrixrain.h"
#include "screensaver.h"
#include "volumekeys.h"
#include "layouteditor.h"
#include "deckshell.h"
#include "deckstyle.h"
#include "libraryview.h"   // LibraryGameCard
#include "mainwindow.h"
#include "osk.h"
#include "padinput.h"
#include "padnav.h"

#include <QApplication>
#include <QFile>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <sstream>

namespace
{
using json = nlohmann::ordered_json;

MainWindow &Win(Command::Context &C)
{
    auto *W = qobject_cast<MainWindow *>(C.Window);
    if (!W) throw std::runtime_error("no main window");
    return *W;
}

DeckShell &Shell(Command::Context &C)
{
    MainWindow &W = Win(C);
    if (!W.deckShell() || !W.controllerMode()) throw Command::BadArgs("controller mode is off: VidyaGod controller mode --set on");
    return *W.deckShell();
}

//Let the UI settle after a press: queued events, a glide's frames (a tenth of a second is most of one).
void Settle(int Ms)
{
    QElapsedTimer T;
    T.start();
    do QApplication::processEvents(QEventLoop::AllEvents, 10);
    while (T.elapsed() < Ms);
}

json State(Command::Context &C)
{
    MainWindow &W = Win(C);
    json J{{"mode", W.controllerMode() ? "controller" : "desktop"}};
    json R = PadRouter::instance().state();
    for (auto It = R.begin(); It != R.end(); ++It) J[It.key()] = It.value();
    if (W.controllerMode() && W.deckShell()) J["shell"] = W.deckShell()->padState();
    return J;
}

json Pads()
{
    PadInput &In = PadInput::instance();
    json List = json::array();
    for (const PadInput::PadInfo &P : In.pads()) List.push_back(json{{"name", P.Name.toStdString()}, {"glyphs", Pad::GlyphsName(P.Glyphs)}});
    json Glyphs = json::object();
    for (const std::string &N : Pad::Names()) Glyphs[N] = DeckStyle::GlyphText(*Pad::FromName(N), In.glyphs()).toStdString();
    return json{{"enabled", In.enabled()}, {"suspended", In.suspended()}, {"deckDirect", In.deckDirect()}, {"onlyVirtual", In.onlyVirtual()},
                {"glyphs", Pad::GlyphsName(In.glyphs())},
                {"labels", Glyphs}, {"controllers", List}};
}

json ServiceState()
{
    ControllerService &S = ControllerService::instance();
    json Controllers = json::array();
    for (const ControllerService::Device &D : S.devices())
    {
        json Src = json::array();
        for (const QString &N : D.Sources) Src.push_back(N.toStdString());
        Controllers.push_back(json{{"path", D.Path.toStdString()}, {"name", D.Name.toStdString()}, {"layout", D.Profile.toStdString()}, {"sources", Src}});
    }
    json Needs = json::array();
    for (const QString &N : S.needs()) Needs.push_back(N.toStdString());
    return json{{"available", S.available()}, {"unavailable", S.unavailable().toStdString()}, {"running", S.running()},
                {"needs", Needs}, {"program", S.program().toStdString()}, {"bus", S.bus().toStdString()},
                {"controllers", Controllers}, {"hidden", S.hiddenNodes()}, {"customLayout", !S.layout().isEmpty()}};
}

std::string LayoutsDir() { return (AppPaths::DataRoot() / "ControllerLayouts").string(); }

json LayoutJson(const ControllerLayout::Layout &L)
{
    json Controls = json::array();
    for (const ControllerLayout::Control &C : ControllerLayout::Controls())
        Controls.push_back(json{{"control", C.Id}, {"label", C.Label}, {"group", C.Group}, {"does", L.summary(C.Id)}});
    return json{{"name", L.Name}, {"description", L.Description}, {"controls", Controls}, {"profile", json::parse(L.toProfile().dump())}};
}

// What --as-is, --nothing, --pad/--key/--mouse/--overlay (+ --also-pad/--also-key/--also-mouse), --stick, --keys and
// --speed make of a control's binding.
ControllerLayout::Binding BindingFrom(const std::string &ControlId, const json &A)
{
    using namespace ControllerLayout;
    Binding B;
    B.Control = ControlId;
    if (!FindControl(ControlId)) throw Command::BadArgs("no control '" + ControlId + "' — `controller layout-show Gamepad` lists them");
    if (const std::string S = Command::Str(A, "stick", ""); !S.empty())
    {
        B.Mode = S;
        B.Speed = static_cast<double>(Command::Int(A, "speed", 1000));
        if (const std::string K = Command::Str(A, "keys", ""); !K.empty())
        {
            std::stringstream In(K);
            for (std::string X; std::getline(In, X, ',');) B.Keys.push_back(X);
        }
        return B;
    }
    if (Command::Bool(A, "nothing", false)) { B.Mode = "nothing"; return B; }
    if (Command::Bool(A, "as-is", false)) return B;
    for (const auto &[Opt, T] : { std::pair{ "pad", Action::Type::Gamepad }, std::pair{ "key", Action::Type::Keyboard }, std::pair{ "mouse", Action::Type::Mouse } })
        if (const std::string V = Command::Str(A, Opt, ""); !V.empty()) B.Actions.push_back({ T, V });
    if (Command::Bool(A, "overlay", false)) B.Actions.push_back({ Action::Type::Overlay, "" });
    for (const auto &[Opt, T] : { std::pair{ "also-pad", Action::Type::Gamepad }, std::pair{ "also-key", Action::Type::Keyboard }, std::pair{ "also-mouse", Action::Type::Mouse } })
        if (const std::string V = Command::Str(A, Opt, ""); !V.empty()) B.Actions.push_back({ T, V });
    if (B.Actions.empty()) throw Command::BadArgs("say what it does: --pad, --key, --mouse, --overlay, --nothing, --as-is, or --stick for a stick");
    return B;
}

std::vector<Command::Arg> BindingArgs()
{
    return { Command::Opt("pad", "presses a pad button (South, East, DPadUp…)"), Command::Opt("key", "presses a key (KeyW, KeySpace…)"),
             Command::Opt("mouse", "presses a mouse button (Left, Right, WheelUp…)"), Command::Flag("overlay", "opens the overlay"),
             Command::Opt("also-pad", "and a pad button with it"), Command::Opt("also-key", "and a key with it"),
             Command::Opt("also-mouse", "and a mouse button with it"), Command::Flag("nothing", "does nothing"),
             Command::Flag("as-is", "does what the controller does"),
             Command::Opt("stick", "a stick works as: joystick, mouse, keys, dpad", Command::Type::String, false, { "joystick", "mouse", "keys", "dpad" }),
             Command::Opt("keys", "a stick's four keys: up,down,left,right (KeyW,KeyS,KeyA,KeyD)"),
             Command::Opt("speed", "a stick as the mouse: pixels a second", Command::Type::Int) };
}

LayoutEditor *OpenEditor()
{
    for (QWidget *W : QApplication::topLevelWidgets())
        if (auto *E = qobject_cast<LayoutEditor *>(W); E && E->isVisible()) return E;
    return nullptr;
}

json EditorState(LayoutEditor *E)
{
    if (!E) return json{{"open", false}};
    return json{{"open", true}, {"layout", LayoutJson(E->current())}, {"dirty", E->dirty()}, {"selected", E->selectedControl().toStdString()},
                {"status", E->status().toStdString()}};
}

ControllerService::Need NeedFrom(const std::string &N)
{
    if (N == "controller") return ControllerService::Need::ControllerMode;
    if (N == "game") return ControllerService::Need::Game;
    if (N == "session") return ControllerService::Need::Session;
    throw Command::BadArgs("no reason '" + N + "' — controller, game or session");
}

std::string Joined(const std::vector<std::string> &V)
{
    std::string Out;
    for (const std::string &S : V) Out += (Out.empty() ? "" : ", ") + S;
    return Out;
}
}

VG_COMMANDS
{
    using Command::Type;
    using Command::Needs;
    using Command::Pos;
    using Command::Opt;
    using Command::Flag;
    Command::RegisterGroup("controller", "the controller UI (a Steam Deck, a TV): its mode, a controller's presses, what they act on");

    Command::Register({
        "controller mode", "controller mode on or off — or which it is",
        "On, the window goes full screen showing the controller shell (Home, Library, Catalog, Downloads, Peers, Node, "
        "Settings) and controllers are read; off, the desktop tabs come back where they were. How the app STARTS is "
        "`controller settings --start`.",
        { Opt("set", "on or off", Type::String, false, { "on", "off" }) }, { "controller mode", "controller mode --set on" },
        { "MainWindow::setControllerMode", "MainWindow::controllerMode", "MainWindow::deckShell", "MainWindow::applyControllerSettings",
          "DeckShell::setActive", "DeckShell::active", "DeckShell::On", "DeckShell::desktopRequested", "DeckShell::scale",
          "PadInput::setEnabled", "PadInput::enabled", "PadRouter::install", "DeckHosted::setScale", "DeckHosted::page" }, Needs::Ui, false,
        [](Command::Context &C, const json &A) {
            if (A.contains("set")) { Win(C).setControllerMode(Command::Str(A, "set") == "on"); Settle(120); }
            return json{{"mode", Win(C).controllerMode() ? "controller" : "desktop"}};
        }, {} });

    Command::Register({
        "controller press", "press controller buttons, one after another, as a controller does",
        "Each press goes where a real controller's goes: the on-screen keyboard while it is open, an open popup, the "
        "focused widget's controller screens, then generic navigation (a direction focuses the nearest control, A "
        "presses, B backs out of a dialog, X opens a context menu, the shoulders turn tabs or the shell's sections). "
        "Buttons: up down left right a b x y lb rb lt rt start back guide l3 r3; a chord holds the first ones while the last is pressed (back+start: controller mode from the desktop). The result is `controller state` "
        "after the last press, with what took each press. A press that opens a dialog which waits for an answer "
        "answers at once with the dialog: `controller press` or `ui press` answers it.",
        { Pos("buttons", "the buttons, comma-separated: down,down,a"),
          Opt("settle", "milliseconds to let the UI settle after each press (default 60)", Type::Int) },
        { "controller press down,a", "controller press rb,rb,a --settle 200", "controller press start", "controller press back+start" },
        { "PadInput::inject", "PadInput::injectChord", "PadInput::pressed", "PadInput::released", "PadInput::used", "PadInput::tick", "PadRouter::route",
          "PadRouter::routed", "PadRouter::targetWindow", "PadRouter::overlay", "PadRouter::hintsChanged", "PadRouter::setPadMode",
          "PadRouter::padMode", "PadNav::Nearest", "PadNav::Focusable", "PadNav::FocusRect", "PadNav::Move", "PadNav::Activate", "PadNav::Back",
          "PadTarget::padPressed", "Pad::FromName", "UiFind::Act",
          "DeckShell::padPressed", "DeckShell::showSection", "DeckShell::openGame", "DeckShell::closeGame", "DeckShell::openMenu",
          "DeckShell::closeMenu", "DeckShell::quitRequested", "DeckGrid::padPressed", "DeckGrid::setCurrentIndex", "DeckGrid::activated",
          "DeckGrid::currentChanged", "DeckHome::padPressed", "DeckShelf::padPressed", "DeckShelf::cycleSort", "DeckShelf::setSort",
          "DeckGamePage::padPressed", "DeckGamePage::press", "DeckGamePage::showGame", "DeckDownloads::padPressed",
          "DeckMenu::padPressed", "DeckMenu::choose", "DeckMenu::present", "DeckMenu::dismiss",
          "PreLaunchWindow::padPressed",
          "OnScreenKeyboard::padPressed", "OnScreenKeyboard::Open", "OnScreenKeyboard::Ask", "OnScreenKeyboard::accept",
          "OnScreenKeyboard::cancel", "PadRouter::pushOverlay", "PadRouter::popOverlay" }, Needs::Ui, false,
        [](Command::Context &C, const json &A) {
            //Each press is a button, or a chord: buttons held, then the last one pressed (back+start).
            struct Press { std::vector<Pad::Button> Held; Pad::Button B; std::string Name; };
            std::vector<Press> Bs;
            std::stringstream In(Command::Str(A, "buttons"));
            for (std::string Tok; std::getline(In, Tok, ',');)
            {
                while (!Tok.empty() && Tok.front() == ' ') Tok.erase(Tok.begin());
                while (!Tok.empty() && Tok.back() == ' ') Tok.pop_back();
                if (Tok.empty()) continue;
                Press P{{}, Pad::Button::A, Tok};
                std::stringstream Parts(Tok);
                std::vector<Pad::Button> All;
                for (std::string Part; std::getline(Parts, Part, '+');)
                {
                    const std::optional<Pad::Button> B = Pad::FromName(Part);
                    if (!B) throw Command::BadArgs("no button '" + Part + "' — the buttons are " + Joined(Pad::Names()));
                    All.push_back(*B);
                }
                if (All.empty()) continue;
                P.B = All.back();
                P.Held.assign(All.begin(), All.end() - 1);
                Bs.push_back(P);
            }
            if (Bs.empty()) throw Command::BadArgs("no buttons given");
            const int Wait = A.contains("settle") ? static_cast<int>(Command::Int(A, "settle")) : 60;
            json Took = json::array();
            return UiFind::Act(C, [&] {
                for (const Press &Pr : Bs)
                {
                    QString What;
                    QMetaObject::Connection Cn = QObject::connect(&PadRouter::instance(), &PadRouter::routed, [&What](const QString &W) { What = W; });
                    if (Pr.Held.empty()) PadInput::instance().inject(Pr.B);
                    else PadInput::instance().injectChord(Pr.Held, Pr.B);
                    QObject::disconnect(Cn);
                    Took.push_back(json{{"button", Pr.Name}, {"took", What.isEmpty() ? json(nullptr) : json(What.toStdString())}});
                    Settle(Wait);
                }
            }, [&] { json S = State(C); S["presses"] = Took; return S; });
        }, {} });

    Command::Register({
        "controller state", "what a controller's presses act on now",
        "The window presses go to, the focused control, the controller screens it sits in (the shell's section, its "
        "page, the focused cover), the on-screen keyboard when it is open, what each button does now (the hint bar), "
        "and where the focus ring is drawn.",
        {}, { "controller state", "controller state --json" },
        { "PadRouter::state", "PadRouter::hints", "PadRouter::ringRect", "PadNav::Describe", "PadNav::Hints", "PadTarget::padHints",
          "PadTarget::padState", "DeckShell::padState", "DeckShell::padHints", "DeckShell::section", "DeckShell::gameOpen", "DeckShell::menuOpen",
          "DeckShell::statusText", "DeckShell::SectionName", "DeckGrid::padState", "DeckGrid::current", "DeckGrid::currentIndex", "DeckGrid::count",
          "DeckGrid::lit", "DeckGrid::tileSize", "DeckGrid::tileRect", "DeckHome::padState", "DeckHome::padHints", "DeckHome::statusText", "PreLaunchWindow::padHints",
          "DeckShelf::padState", "DeckShelf::padHints", "DeckShelf::statusText", "DeckShelf::sort", "DeckShelf::search", "DeckShelf::kind",
          "DeckShelf::grid", "DeckShelf::cards", "DeckGamePage::padState", "DeckGamePage::padHints", "DeckGamePage::buttons", "DeckGamePage::key",
          "DeckDownloads::padState", "DeckDownloads::padHints", "DeckDownloads::statusText", "DeckDownloads::keys", "DeckMenu::padState",
          "DeckMenu::padHints", "DeckMenu::items", "OnScreenKeyboard::padState", "OnScreenKeyboard::padHints", "OnScreenKeyboard::current",
          "OnScreenKeyboard::text", "DeckShell::model", "DeckShell::ipfs", "DeckShell::downloads" }, Needs::Ui, false,
        [](Command::Context &C, const json &) { return State(C); }, {} });

    Command::Register({
        "controller pads", "the controllers connected, and how their buttons are labelled",
        "Steam Input's virtual controller (a Steam Deck's, in Game Mode) is listed as the controller it says it is; its "
        "buttons are drawn as the Deck labels them. Suspended: a launched game is running, and has the controllers. "
        "deckDirect: a Steam Deck's own controller is read directly (controller mode; its lizard mode is off meanwhile).",
        {}, { "controller pads" },
        { "PadInput::pads", "PadInput::glyphs", "PadInput::suspended", "PadInput::deckDirect", "PadInput::setDeckDirect", "PadInput::onlyVirtual", "PadInput::setOnlyVirtual", "PadInput::padsChanged", "PadInput::held", "Pad::Names", "Pad::Name",
          "Pad::GlyphsName", "Pad::IsDirection", "DeckStyle::GlyphText", "DeckStyle::GlyphWidth", "LaunchThread::Running" }, Needs::Ui, false,
        [](Command::Context &, const json &) { return Pads(); }, {} });

    Command::Register({
        "controller service", "VidyaGod's controller service (its own InputPlumber): whether it runs, why, and what it owns",
        "It runs while a reason holds — controller mode, a game VidyaGod launched (which only keeps it running), or a "
        "handheld's VidyaGod session — and owns the controllers it has a configuration for, giving VidyaGod and its games "
        "virtual ones. `hidden`: the real controllers' nodes a game's sandbox covers. --want/--drop take or let go of a "
        "reason (controller, game, session), as the app does.",
        { Opt("want", "take a reason: controller, game or session"), Opt("drop", "let a reason go") },
        { "controller service", "controller service --want controller", "controller service --drop controller" },
        { "ControllerService::instance", "ControllerService::want", "ControllerService::wants", "ControllerService::needs",
          "ControllerService::NeedName", "ControllerService::unavailable", "ControllerService::available", "ControllerService::running",
          "ControllerService::devices", "ControllerService::sourceNodes", "ControllerService::hiddenNodes", "ControllerService::program",
          "ControllerService::setProgram", "ControllerService::bus", "ControllerService::setBus", "ControllerService::setDataDir",
          "ControllerService::changed", "IsVirtualInputNode", "JoystickSiblings", "SandboxLayer::SetHiddenDevicesSource" },
        Needs::Ui, false,
        [](Command::Context &, const json &A) {
            ControllerService &S = ControllerService::instance();
            if (const std::string W = Command::Str(A, "want", ""); !W.empty()) S.want(NeedFrom(W), true);
            if (const std::string D = Command::Str(A, "drop", ""); !D.empty()) S.want(NeedFrom(D), false);
            if (S.wants(ControllerService::Need::ControllerMode) && S.available() && !S.running())   // let it come up
            {
                QElapsedTimer T;
                T.start();
                while (!S.running() && T.elapsed() < 5000) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            }
            return ServiceState();
        }, {} });

    Command::Register({
        "controller overlay", "the in-game overlay (the Guide button over a game): its state, and what it does",
        "While a game VidyaGod launched runs, the controller service has InputPlumber intercept the Guide button; a press "
        "opens this overlay and every press after it reaches VidyaGod, not the game, until it closes. --event delivers an "
        "intercepted press as InputPlumber would (ui_guide, ui_accept, ui_down…); --game/--title give it a game without "
        "a launch (a look at it), --no-game takes it away. --edit opens the configurator for the game.",
        { Command::Flag("open", "open it"), Command::Flag("close", "close it"), Command::Opt("event", "an intercepted press (ui_guide, ui_accept…)"),
          Command::Opt("game", "its game's key (without a launch)"), Command::Opt("title", "its game's title (with --game)"),
          Command::Flag("no-game", "no game"), Command::Opt("layout", "switch the game's layout"), Command::Flag("quit", "Quit game (twice to quit)"), Command::Opt("type", "type this into the game (and Enter)"),
          Command::Flag("edit", "the configurator for the game") },
        { "controller overlay", "controller overlay --game 12345 --title 'Some Game' --open", "controller overlay --event ui_down",
          "controller overlay --layout 'Keyboard and mouse (WASD)'", "controller overlay --close" },
        { "GameOverlay::instance", "GameOverlay::attach", "GameOverlay::setGame", "GameOverlay::clearGame", "GameOverlay::hasGame",
          "GameOverlay::isOpen", "GameOverlay::openOverlay", "GameOverlay::closeOverlay", "GameOverlay::title", "GameOverlay::layoutName",
          "GameOverlay::setLayout", "GameOverlay::editLayout", "GameOverlay::quitGame", "GameOverlay::quitArmed", "GameOverlay::disarmQuit", "GameOverlay::applyScale", "GameOverlay::onUiEvent",
          "GameOverlay::onGameChanged", "GameOverlay::padPressed", "GameOverlay::padHints", "OverlayProperties", "MarkOverlayWindow",
          "PadButtonFromUiEvent", "GameOverlay::typeIntoGame", "ControllerService::typeText", "KeyStrokeFor", "ControllerService::setIntercept", "ControllerService::intercept", "ControllerService::uiEvent",
          "ControllerService::gameChanged", "ControllerService::setGameLayout", "ControllerService::gameLayout" }, Needs::Ui, false,
        [](Command::Context &, const json &A) {
            GameOverlay &O = GameOverlay::instance();
            if (Command::Bool(A, "no-game", false)) O.clearGame();
            if (const std::string G = Command::Str(A, "game", ""); !G.empty())
                O.setGame(QString::fromStdString(Command::Str(A, "title", G)), QString::fromStdString(G), [] {});
            if (Command::Bool(A, "open", false)) O.openOverlay();
            if (const std::string E = Command::Str(A, "event", ""); !E.empty()) O.onUiEvent(QString::fromStdString(E), 1.0);
            QString Err;
            if (const std::string L = Command::Str(A, "layout", ""); !L.empty() && !O.setLayout(QString::fromStdString(L), &Err))
                throw Command::BadArgs(Err.toStdString());
            if (Command::Bool(A, "quit", false)) O.quitGame();
            if (const std::string T = Command::Str(A, "type", ""); !T.empty() && !O.typeIntoGame(QString::fromStdString(T), &Err))
                throw Command::BadArgs(Err.toStdString());
            if (Command::Bool(A, "edit", false)) QTimer::singleShot(0, &O, [&O] { O.editLayout(); });
            if (Command::Bool(A, "close", false)) O.closeOverlay();
            Settle(60);
            ControllerService &S = ControllerService::instance();
            return json{{"open", O.isOpen()}, {"game", O.hasGame()}, {"title", O.title().toStdString()}, {"layout", O.layoutName().toStdString()},
                        {"quitArmed", O.quitArmed()}, {"intercept", S.running() ? S.intercept() : 0u},
                        {"gameLayout", !S.gameLayout().isEmpty()}};
        }, {} });

    Command::Register({
        "controller screensaver", "the screensaver: the digital rain over controller mode after a while with no input",
        "Controller mode's screen gives way to the digital rain after Settings.ScreenSaverSeconds with no input (a minute "
        "unless set; 0 turns it off), never while a game runs; any input ends it and does nothing else. --seconds sets the "
        "wait; --show starts it now, --hide ends it; --input stands for an input (the wait starts over); --frames steps the "
        "rain that many frames (a look at it without waiting).",
        { Command::Opt("seconds", "the wait, in seconds (0: off)", Command::Type::Int), Command::Flag("show", "start it now"),
          Command::Flag("hide", "end it"), Command::Flag("input", "an input: the wait starts over"),
          Command::Opt("frames", "step the rain this many frames", Command::Type::Int) },
        { "controller screensaver", "controller screensaver --seconds 300", "controller screensaver --show --frames 60", "controller screensaver --hide" },
        { "DeckShell::screenSaver", "ScreenSaver::SecondsIn", "ScreenSaver::setSuspended", "ScreenSaver::suspended", "ScreenSaver::showing",
          "ScreenSaver::showNow", "ScreenSaver::dismiss", "ScreenSaver::activity", "ScreenSaver::idleRemainingMs", "ScreenSaver::rain",
          "ScreenSaver::eventFilter", "MatrixRain::start", "MatrixRain::stop", "MatrixRain::running", "MatrixRain::advance",
          "MatrixRain::columns", "MatrixRain::rows", "MatrixRain::litCells", "MatrixRain::glyphs", "MatrixRain::glyph" }, Needs::Ui, false,
        [](Command::Context &C, const json &A) {
            DeckShell &S = Shell(C);
            ScreenSaver &Sv = *S.screenSaver();
            if (A.contains("seconds"))
            {
                const long long Sec = Command::Int(A, "seconds");
                if (Sec < 0) throw Command::BadArgs("--seconds: 0 or more");
                (*S.model().config())["Settings"]["ScreenSaverSeconds"] = Sec;
                S.model().save();
                Sv.activity();
            }
            if (Command::Bool(A, "input", false)) Sv.activity();
            if (Command::Bool(A, "show", false)) Sv.showNow();
            if (const long long F = Command::Int(A, "frames", 0); F > 0)
                for (long long I = 0; I < F; ++I) Sv.rain()->advance(1.0 / MatrixRain::FramesPerSecond);
            if (Command::Bool(A, "hide", false)) Sv.dismiss();
            Settle(30);
            const MatrixRain &R = *Sv.rain();
            return json{{"showing", Sv.showing()}, {"suspended", Sv.suspended()}, {"seconds", ScreenSaver::SecondsIn(*S.model().config())},
                        {"idleRemainingMs", Sv.idleRemainingMs()}, {"running", R.running()}, {"columns", R.columns()}, {"rows", R.rows()},
                        {"lit", R.litCells()}, {"glyphs", MatrixRain::glyphs().size()}};
        }, {} });

    Command::Register({
        "controller volume", "the volume keys: the system volume, stepped as the handheld's keys step it",
        "A handheld's volume keys reach VidyaGod as InputPlumber's D-Bus events (every layout maps them), and set the "
        "default output's volume through wpctl (PipeWire), else pactl, showing the level for a moment. --up/--down step it "
        "by 5%, --mute toggles mute, --event delivers a key as InputPlumber would (ui_volume_up, ui_volume_down, "
        "ui_volume_mute; pressed, then released).",
        { Command::Flag("up", "5% up"), Command::Flag("down", "5% down"), Command::Flag("mute", "toggle mute"),
          Command::Opt("event", "a key as InputPlumber sends it (ui_volume_up…)") },
        { "controller volume", "controller volume --up", "controller volume --event ui_volume_down" },
        { "VolumeKeys::instance", "VolumeKeys::follow", "VolumeKeys::step", "VolumeKeys::toggleMute", "VolumeKeys::refresh", "VolumeKeys::level",
          "VolumeKeys::tool", "VolumeKeys::onUiEvent", "Volume::ParseWpctl", "Volume::ParsePactl", "Volume::StepCommand", "Volume::MuteCommand",
          "Volume::ReadCommands", "VolumeToast::present", "VolumeToast::level", "MarkExternalOverlay", "ControllerLayout::SetVolumeKeysToVidyaGod", "ControllerLayout::VolumeKeysToVidyaGod" }, Needs::Ui, false,
        [](Command::Context &, const json &A) {
            VolumeKeys &V = VolumeKeys::instance();
            if (V.tool().isEmpty()) throw std::runtime_error("neither wpctl nor pactl is installed: no volume to set");
            QEventLoop L;
            QTimer::singleShot(3000, &L, &QEventLoop::quit);
            QObject::connect(&V, &VolumeKeys::changed, &L, &QEventLoop::quit);
            if (Command::Bool(A, "up", false)) V.step(VolumeKeys::StepPercent);
            else if (Command::Bool(A, "down", false)) V.step(-VolumeKeys::StepPercent);
            else if (Command::Bool(A, "mute", false)) V.toggleMute();
            else if (const std::string E = Command::Str(A, "event", ""); !E.empty())
            { V.onUiEvent(QString::fromStdString(E), 1.0); V.onUiEvent(QString::fromStdString(E), 0.0); }
            else V.refresh();
            L.exec();
            return json{{"tool", V.tool().toStdString()}, {"percent", V.level().Percent}, {"muted", V.level().Muted},
                        {"keys_routed", ControllerLayout::VolumeKeysToVidyaGod()}};
        }, {} });

    Command::Register({
        "controller layouts", "the controller layouts: VidyaGod's own, the user's, and which game uses which",
        "VidyaGod's own are always there (Gamepad: the controller as it is). The user's are files under the data dir's "
        "ControllerLayouts/, one per layout; games.json there says which game uses which.",
        {}, { "controller layouts" },
        { "ControllerLayout::Templates", "ControllerLayout::Saved", "ControllerLayout::Games" }, Needs::Config, false,
        [](Command::Context &, const json &) {
            json T = json::array(), Mine = json::array(), Games = json::object();
            for (const auto &L : ControllerLayout::Templates()) T.push_back(json{{"name", L.Name}, {"description", L.Description}});
            for (const auto &L : ControllerLayout::Saved(LayoutsDir())) Mine.push_back(json{{"name", L.Name}, {"description", L.Description}});
            for (const auto &[K, V] : ControllerLayout::Games(LayoutsDir())) Games[K] = V;
            return json{{"vidyagods", T}, {"yours", Mine}, {"games", Games}, {"dir", LayoutsDir()}};
        }, {} });

    Command::Register({
        "controller layout-show", "what every control does in a controller layout, and the InputPlumber profile it makes",
        "A control a layout does not name does what the controller does (\"As is\").",
        { Command::Pos("name", "the layout") }, { "controller layout-show 'Keyboard and mouse (WASD)'" },
        { "ControllerLayout::Named", "ControllerLayout::Controls", "ControllerLayout::FindControl", "ControllerLayout::Layout::summary",
          "ControllerLayout::Layout::toProfile", "ControllerLayout::Layout::find", "ControllerLayout::KeyLabel", "ControllerLayout::GamepadLabel",
          "ControllerLayout::MouseLabel", "ControllerLayout::TypeName" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const std::string N = Command::Str(A, "name");
            const auto L = ControllerLayout::Named(LayoutsDir(), N);
            if (!L) throw Command::BadArgs("no layout '" + N + "' — `controller layouts` lists them");
            return LayoutJson(*L);
        }, {} });

    {
        std::vector<Command::Arg> Args = { Command::Pos("name", "the layout (one of yours: copy one of VidyaGod's with layout-save first)"),
                                           Command::Pos("control", "the control (a, dpad_up, left_stick, rt, l4…)") };
        for (Command::Arg &Ar : BindingArgs()) Args.push_back(Ar);
        Command::Register({
            "controller layout-bind", "change what one control does in one of your controller layouts",
            "Only what InputPlumber can do is accepted: a trigger never presses a mouse button, a trackpad is always the "
            "pointer, the gyro is not mapped yet. Saved at once.",
            Args, { "controller layout-bind Mine a --key KeySpace", "controller layout-bind Mine right_stick --stick mouse --speed 1200",
                    "controller layout-bind Mine left_stick --stick keys --keys KeyW,KeyS,KeyA,KeyD", "controller layout-bind Mine lb --mouse Right" },
            { "ControllerLayout::Layout::set", "ControllerLayout::Check", "ControllerLayout::Save", "ControllerLayout::TypeFrom",
              "ControllerLayout::GamepadButtons", "ControllerLayout::KeyboardKeys", "ControllerLayout::MouseButtons" }, Needs::Config, false,
            [](Command::Context &, const json &A) {
                const std::string N = Command::Str(A, "name");
                auto L = ControllerLayout::Named(LayoutsDir(), N);
                if (!L) throw Command::BadArgs("no layout '" + N + "'");
                const ControllerLayout::Binding B = BindingFrom(Command::Str(A, "control"), A);
                if (const std::string E = ControllerLayout::Check(B); !E.empty()) throw Command::BadArgs(B.Control + ": " + E);
                L->set(B);
                std::string E;
                if (!ControllerLayout::Save(LayoutsDir(), *L, &E)) throw Command::BadArgs(E);
                return LayoutJson(*L);
            }, {} });
    }

    Command::Register({
        "controller layout-save", "save a copy of a controller layout under a name of yours",
        "From one of VidyaGod's own or one of yours. A name is a file name (no slashes, no leading dot); VidyaGod's own "
        "names are taken.",
        { Command::Pos("name", "the new layout's name"), Command::Opt("from", "the layout to copy", Command::Type::String, true),
          Command::Opt("description", "what it is for") },
        { "controller layout-save Mine --from 'Keyboard and mouse (WASD)'" },
        { "ControllerLayout::ValidName", "ControllerLayout::Layout::toJson", "ControllerLayout::Layout::FromJson" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            auto L = ControllerLayout::Named(LayoutsDir(), Command::Str(A, "from"));
            if (!L) throw Command::BadArgs("no layout '" + Command::Str(A, "from") + "'");
            L->Name = Command::Str(A, "name");
            if (const std::string D = Command::Str(A, "description", ""); !D.empty()) L->Description = D;
            std::string E;
            if (!ControllerLayout::Save(LayoutsDir(), *L, &E)) throw Command::BadArgs(E);
            return LayoutJson(*L);
        }, {} });

    Command::Register({
        "controller layout-remove", "delete one of your controller layouts",
        "VidyaGod's own cannot be deleted. A game that used it plays with the controller as it is.",
        { Command::Pos("name", "the layout") }, { "controller layout-remove Mine" },
        { "ControllerLayout::Remove" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const std::string N = Command::Str(A, "name");
            if (!ControllerLayout::Remove(LayoutsDir(), N)) throw Command::BadArgs("no layout of yours named '" + N + "'");
            return json{{"removed", N}};
        }, {} });

    Command::Register({
        "controller layout-game", "the controller layout a game uses (or choose one)",
        "By the game's key (its tile UID). While the game runs — launched by VidyaGod, with its controller service "
        "running — the layout is on the controller; when it ends VidyaGod's is back.",
        { Command::Opt("game", "the game's key", Command::Type::String, true), Command::Opt("layout", "use this layout"),
          Command::Flag("clear", "the controller as it is") },
        { "controller layout-game --game 12345", "controller layout-game --game 12345 --layout Mine", "controller layout-game --game 12345 --clear" },
        { "ControllerLayout::ForGame", "ControllerLayout::SetForGame", "ControllerLayout::GameLayout" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const std::string G = Command::Str(A, "game");
            if (Command::Bool(A, "clear", false)) ControllerLayout::SetForGame(LayoutsDir(), G, "");
            else if (const std::string N = Command::Str(A, "layout", ""); !N.empty())
            {
                if (!ControllerLayout::Named(LayoutsDir(), N)) throw Command::BadArgs("no layout '" + N + "'");
                ControllerLayout::SetForGame(LayoutsDir(), G, N == "Gamepad" ? "" : N);
            }
            return json{{"game", G}, {"layout", ControllerLayout::GameLayout(LayoutsDir(), G).Name}};
        }, {} });

    Command::Register({
        "controller layout-app", "VidyaGod's own controller layout (or choose one)",
        "What the controller does in VidyaGod itself (Settings → Controller); a game VidyaGod runs has its own, or the "
        "controller as it is. Gamepad, the controller as it is, until one is chosen; a chosen one that is gone is Gamepad "
        "again. Loaded on the controller at once when the controller service runs.",
        { Command::Opt("layout", "use this layout"), Command::Flag("clear", "the controller as it is (Gamepad)") },
        { "controller layout-app", "controller layout-app --layout Desktop", "controller layout-app --clear" },
        { "ControllerLayout::ForApp", "ControllerLayout::SetForApp", "ControllerLayout::AppLayout", "ControllerService::loadAppLayout",
          "ControllerService::appLayoutDir" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            bool Changed = false;
            if (Command::Bool(A, "clear", false)) { ControllerLayout::SetForApp(LayoutsDir(), ""); Changed = true; }
            else if (const std::string N = Command::Str(A, "layout", ""); !N.empty())
            {
                if (!ControllerLayout::Named(LayoutsDir(), N)) throw Command::BadArgs("no layout '" + N + "'");
                if (!ControllerLayout::SetForApp(LayoutsDir(), N)) throw std::runtime_error("cannot write in " + LayoutsDir());
                Changed = true;
            }
            QString Error;
            if (Changed && !ControllerService::instance().loadAppLayout(QString::fromStdString(LayoutsDir()), &Error))
                throw std::runtime_error("the controller refused it: " + Error.toStdString());
            const std::string Chosen = ControllerLayout::ForApp(LayoutsDir());
            return json{{"layout", ControllerLayout::AppLayout(LayoutsDir()).Name}, {"chosen", Chosen},
                        {"loaded_from", ControllerService::instance().appLayoutDir().toStdString()}};
        }, {} });

    {
        std::vector<Command::Arg> Args = { Command::Opt("game", "open it for this game (its key)"), Command::Opt("title", "the game's title"),
                                           Command::Opt("open", "open this layout"), Command::Opt("select", "select this control"),
                                           Command::Flag("bind", "bind the selected control (with the options below)"),
                                           Command::Opt("save-as", "save under this name"), Command::Flag("use", "use it for the game"),
                                           Command::Flag("use-app", "use it for VidyaGod (its own layout)"),
                                           Command::Flag("try", "load it into the controller now"), Command::Flag("close", "close the editor") };
        for (Command::Arg &Ar : BindingArgs()) Args.push_back(Ar);
        Command::Register({
            "controller layout-editor", "the controller layout configurator: open it, and do what its controls do",
            "Opens the configurator (for a game, with --game), or acts on the one open: --open a layout, --select a "
            "control, --bind it (as layout-bind's options say), --save-as, --use for the game, --use-app for VidyaGod, --try "
            "on the controller, --close (a layout tried is let go: VidyaGod's own is back). Shows what it holds. "
            "`ui screenshot --control LayoutEditor` shows it.",
            Args, { "controller layout-editor --game 12345 --title 'Crash Team'", "controller layout-editor --select a --bind --key KeySpace",
                    "controller layout-editor --save-as Mine --use", "controller layout-editor --close" },
            { "LayoutEditor::openLayout", "LayoutEditor::selectControl", "LayoutEditor::selectedControl", "LayoutEditor::bindSelected",
              "LayoutEditor::saveAs", "LayoutEditor::useForGame", "LayoutEditor::useForApp", "LayoutEditor::done", "LayoutEditor::tryNow",
              "LayoutEditor::status", "LayoutEditor::current",
              "LayoutEditor::dirty", "ControllerPicture::setSelected", "ControllerPicture::setChanged", "ControllerPicture::selected", "ControllerPicture::hotspot",
              "ControllerPicture::controlClicked", "ControllerPicture::sizeHint" }, Needs::Ui, false,
            [](Command::Context &C, const json &A) {
                LayoutEditor *E = OpenEditor();
                if (const std::string G = Command::Str(A, "game", ""); !G.empty() || !E)
                {
                    if (E) E->close();
                    E = new LayoutEditor(QString::fromStdString(LayoutsDir()), QString::fromStdString(G),
                                         QString::fromStdString(Command::Str(A, "title", "")), C.Window);
                    E->setAttribute(Qt::WA_DeleteOnClose);
                    E->show();
                    Settle(80);
                }
                QString Err;
                if (const std::string N = Command::Str(A, "open", ""); !N.empty() && !E->openLayout(QString::fromStdString(N)))
                    throw Command::BadArgs("no layout '" + N + "'");
                if (const std::string Ctl = Command::Str(A, "select", ""); !Ctl.empty() && !E->selectControl(QString::fromStdString(Ctl)))
                    throw Command::BadArgs("no control '" + Ctl + "'");
                if (Command::Bool(A, "bind", false) && !E->bindSelected(BindingFrom(E->selectedControl().toStdString(), A), &Err))
                    throw Command::BadArgs(Err.toStdString());
                if (const std::string N = Command::Str(A, "save-as", ""); !N.empty() && !E->saveAs(QString::fromStdString(N), &Err))
                    throw Command::BadArgs(Err.toStdString());
                if (Command::Bool(A, "use", false) && !E->useForGame(&Err)) throw Command::BadArgs(Err.toStdString());
                if (Command::Bool(A, "use-app", false) && !E->useForApp(&Err)) throw Command::BadArgs(Err.toStdString());
                if (Command::Bool(A, "try", false) && !E->tryNow(&Err)) throw Command::BadArgs(Err.toStdString());
                if (Command::Bool(A, "close", false)) { E->close(); Settle(40); return EditorState(nullptr); }
                Settle(40);
                return EditorState(E);
            }, {} });
    }

    Command::Register({
        "controller layout", "load a controller layout into the controller service",
        "A layout is an InputPlumber device profile (YAML: `kind: DeviceProfile`). It goes to every controller the "
        "service owns, and to any that comes later; --default goes back to InputPlumber's own.",
        { Command::AsPath(Command::OptPos("file", "the layout's YAML file")), Flag("default", "InputPlumber's own layout again") },
        { "controller layout trackpad-mouse.yaml", "controller layout --default" },
        { "ControllerService::loadLayout", "ControllerService::layout" }, Needs::Ui, false,
        [](Command::Context &, const json &A) {
            QString Yaml;
            if (!Command::Bool(A, "default", false))
            {
                const std::string Path = Command::Str(A, "file", "");
                if (Path.empty()) throw Command::BadArgs("name a layout file, or --default");
                QFile F(QString::fromStdString(Path));
                if (!F.open(QIODevice::ReadOnly)) throw Command::BadArgs("cannot read " + Path);
                Yaml = QString::fromUtf8(F.readAll());
            }
            QString Error;
            if (!ControllerService::instance().loadLayout(Yaml, &Error)) throw Command::BadArgs("refused: " + Error.toStdString());
            return ServiceState();
        }, {} });

    Command::Register({
        "controller type", "type into the open on-screen keyboard",
        "As if each key were pressed; --done finishes (the field gets the text, or the search runs), --cancel leaves "
        "things as they were.",
        { Pos("text", "what to type"), Flag("done", "then press Done"), Flag("cancel", "then cancel instead") },
        { "controller type doom --done", "controller type 'Half-Life'" },
        { "OnScreenKeyboard::typeText", "DeckShelf::setSearch", "CardPool::Matches" }, Needs::Ui, false,
        [](Command::Context &C, const json &A) {
            OnScreenKeyboard *K = OnScreenKeyboard::current();
            if (!K) throw Command::BadArgs("no on-screen keyboard is open: A on a text field opens one (controller press a)");
            K->typeText(QString::fromStdString(Command::Str(A, "text")));
            const json Typed{{"text", K->text().toStdString()}};
            if (Command::Bool(A, "cancel", false)) K->cancel();
            else if (Command::Bool(A, "done", false)) K->accept();
            Settle(60);
            json S = State(C);
            S["typed"] = Typed;
            return S;
        }, {} });

    Command::Register({
        "controller section", "show a section of the controller shell",
        "What the shoulders do one step at a time: Home, Library, Catalog, Downloads, Peers, Settings.",
        { Pos("section", "the section", Type::String) }, { "controller section Library", "controller section downloads" },
        { "DeckShell::showSection", "DeckShell::SectionFrom", "DeckShell::section", "DeckShelf::rebuild", "DeckHome::refresh",
          "DeckDownloads::refresh", "DeckShell::libraryCards", "DeckShell::catalogCards", "DeckShelf::refreshCovers",
          "CardPool::Installed", "CardPool::Available", "CardPool::Sort", "CardPool::SortBySection", "CardPool::SectionOf",
          "DeckGrid::setCards", "DeckGrid::setCurrentKey", "DeckGrid::setEmptyText", "DeckGrid::setLit", "DeckGrid::setStrip",
          "DeckGrid::repaintCards", "DeckStyle::ScaleFor", "DeckStyle::Font", "DeckStyle::HostedStyleSheet" }, Needs::Ui, false,
        [](Command::Context &C, const json &A) {
            const QString N = QString::fromStdString(Command::Str(A, "section"));
            const std::optional<DeckShell::Section> S = DeckShell::SectionFrom(N);
            if (!S) throw Command::BadArgs("no section '" + N.toStdString() + "' — Home, Library, Catalog, Downloads, Peers, Settings");
            Shell(C).showSection(*S);
            Settle(60);
            return State(C);
        }, {} });

    Command::Register({
        "controller advanced", "open the desktop page behind a controller page's Advanced… (or come back from it)",
        "Peers: the full Peers page. Node: every transfer and what is seeded. Settings: every setting (--page opens one of "
        "its pages: Runners, Controller, Sources…). --close comes back to the controller page, as B does.",
        { Opt("open", "Peers, Node or Settings"), Opt("page", "with Settings: the page of it to open"), Flag("close", "back to the controller page") },
        { "controller advanced --open Node", "controller advanced --open Settings --page Runners", "controller advanced --close" },
        { "DeckShell::openAdvanced", "DeckShell::closeAdvanced", "DeckShell::advanced", "DeckShell::AdvancedName", "DeckShell::AdvancedFrom" },
        Needs::Ui, false,
        [](Command::Context &C, const json &A) {
            DeckShell &S = Shell(C);
            if (Command::Bool(A, "close", false)) { S.closeAdvanced(); Settle(60); return State(C); }
            const QString N = QString::fromStdString(Command::Str(A, "open"));
            const std::optional<DeckShell::Advanced> W = DeckShell::AdvancedFrom(N);
            if (!W) throw Command::BadArgs("no page '" + N.toStdString() + "' — Peers, Node, Settings (or --close)");
            S.openAdvanced(*W, QString::fromStdString(Command::Str(A, "page")));
            Settle(60);
            return State(C);
        }, {} });

    Command::Register({
        "controller row", "go to a row of controller mode's Peers or Settings page, and press A on it",
        "By its id, as `controller state` lists the page's rows (controller.haptics, screen.saver, receive:<peer>, "
        "pair:<peer>, advanced…). --press presses A there (a toggle turns, a value steps, an action runs; Block / Remove "
        "ask a second press). --note shows the page's note (what its last press did).",
        { Pos("row", "the row's id"), Flag("press", "press A on it") },
        { "controller row controller.haptics --press", "controller row screen.saver" },
        { "DeckShell::peersPage", "DeckShell::settingsPage", "DeckList::setCurrent", "DeckList::press", "DeckList::current", "DeckList::rows",
          "DeckList::refresh", "DeckList::setNote", "DeckList::note", "DeckList::padState", "DeckList::padHints", "DeckList::padPressed",
          "DeckRow::Header", "DeckRow::Info", "DeckRow::stops", "PeerSharing::ShortCode", "PeerSharing::PeerForShortCode", "QrCode::Encode",
          "NetworkTab::Nearby" },
        Needs::Ui, false,
        [](Command::Context &C, const json &A) {
            DeckShell &S = Shell(C);
            DeckList *L = S.advanced() ? nullptr : S.section() == DeckShell::Section::Peers ? S.peersPage()
                        : S.section() == DeckShell::Section::Settings ? S.settingsPage() : nullptr;
            if (!L) throw Command::BadArgs("not on controller mode's Peers or Settings page: controller section Peers");
            L->refresh();
            const QString Id = QString::fromStdString(Command::Str(A, "row"));
            if (!(Command::Bool(A, "press", false) ? L->press(Id) : L->setCurrent(Id)))
                throw Command::BadArgs("no row '" + Id.toStdString() + "' to stop on (controller state lists them)");
            Settle(60);
            return State(C);
        }, {} });

    Command::Register({
        "controller game", "open a game's page in the controller shell (or close it)",
        "By its key (the tile UID, as `controller state` shows a card's) or a unique part of its title, from the Library "
        "or the Catalog. --close closes the page open.",
        { Opt("game", "the game's key or a part of its title"), Flag("close", "close the page open") },
        { "controller game --game 'Crash Team'", "controller game --close" },
        { "DeckShell::openGame", "DeckShell::closeGame", "DeckShell::card", "DeckShell::installed", "DeckGamePage::refresh", "DeckShelf::find" },
        Needs::Ui, false,
        [](Command::Context &C, const json &A) {
            DeckShell &S = Shell(C);
            if (Command::Bool(A, "close", false)) { S.closeGame(); Settle(40); return State(C); }
            const QString Want = QString::fromStdString(Command::Str(A, "game"));
            QString Key;
            if (S.card(Want)) Key = Want;
            else
            {
                QStringList Hits;
                for (const QList<LibraryGameCard *> *Pool : { &S.libraryCards(), &S.catalogCards() })
                    for (LibraryGameCard *Cd : *Pool)
                        if (Cd && Cd->GameTitle.contains(Want, Qt::CaseInsensitive) && !Hits.contains(Cd->GameKey)) Hits << Cd->GameKey;
                if (Hits.size() != 1)
                    throw Command::BadArgs(Hits.isEmpty() ? "no game on a shelf is '" + Want.toStdString() + "'"
                                                          : std::to_string(Hits.size()) + " games match '" + Want.toStdString() + "': give more of the title");
                Key = Hits.front();
            }
            S.openGame(Key);
            Settle(60);
            return State(C);
        }, {} });

    Command::Register({
        "controller settings", "how the app starts, and what a controller does on the desktop",
        "--start: auto (controller mode inside Steam's Game Mode, the desktop window elsewhere), controller or desktop. "
        "--desktop: a controller drives the desktop window too (Start with Back held turns controller mode on). "
        "--haptics: a short rumble as the focus moves. Without options: what they are.",
        { Opt("start", "auto, controller or desktop", Type::String, false, { "auto", "controller", "desktop" }),
          Opt("desktop", "on or off", Type::String, false, { "on", "off" }),
          Opt("haptics", "on or off", Type::String, false, { "on", "off" }) },
        { "controller settings", "controller settings --start controller", "controller settings --haptics on" },
        { "ControllerMode::StartSetting", "ControllerMode::SetStart", "ControllerMode::StartName", "ControllerMode::StartFrom",
          "ControllerMode::DesktopNavigation", "ControllerMode::SetDesktopNavigation", "ControllerMode::Haptics", "ControllerMode::SetHaptics",
          "ControllerMode::InGameMode", "ControllerMode::StartsInController", "PadInput::setHaptics", "MainWindow::applyControllerSettings" },
        Needs::Model, false,
        [](Command::Context &C, const json &A) {
            json &Cfg = *C.Model->config();
            bool Changed = false;
            if (A.contains("start")) { ControllerMode::SetStart(Cfg, *ControllerMode::StartFrom(Command::Str(A, "start"))); Changed = true; }
            if (A.contains("desktop")) { ControllerMode::SetDesktopNavigation(Cfg, Command::Str(A, "desktop") == "on"); Changed = true; }
            if (A.contains("haptics")) { ControllerMode::SetHaptics(Cfg, Command::Str(A, "haptics") == "on"); Changed = true; }
            if (Changed)
            {
                C.Model->save();
                if (auto *W = qobject_cast<MainWindow *>(C.Window)) W->applyControllerSettings();
            }
            return json{{"start", ControllerMode::StartName(ControllerMode::StartSetting(Cfg))},
                        {"desktop", ControllerMode::DesktopNavigation(Cfg)}, {"haptics", ControllerMode::Haptics(Cfg)},
                        {"inGameMode", ControllerMode::InGameMode()},
                        {"startsInController", ControllerMode::StartsInController(Cfg, false, false)}};
        }, {} });

    Command::Register({
        "controller scroll", "push the right stick: scroll what the controller is in",
        "As the right stick does: the focused controller screen scrolls (a shelf glides), else the scroll area the "
        "focus is in. --x and --y are pixels (right and down positive).",
        { Opt("x", "pixels right (negative: left)", Type::Int), Opt("y", "pixels down (negative: up)", Type::Int) },
        { "controller scroll --y 400", "controller scroll --x -200" },
        { "PadRouter::scroll", "PadTarget::padScrolled", "DeckGrid::padScrolled", "PadInput::scrolled" }, Needs::Ui, false,
        [](Command::Context &C, const json &A) {
            const double X = static_cast<double>(Command::Int(A, "x", 0)), Y = static_cast<double>(Command::Int(A, "y", 0));
            if (X == 0 && Y == 0) throw Command::BadArgs("give --x or --y");
            PadRouter::instance().scroll(X, Y);
            Settle(60);
            return State(C);
        }, {} });

    Command::Register({
        "controller rules", "how sticks, triggers and held buttons turn into presses",
        "The pure rules a controller is read by: --stick x,y (each -1..1, right and down positive) gives the directions "
        "held, --trigger v (0..1) whether a trigger is held, --hold ms how many repeats a direction held that long "
        "makes (the first after 380 ms, then every 85 ms, every 45 ms after 1.4 s).",
        { Opt("stick", "x,y", Type::String, false), Opt("trigger", "0..1", Type::String, false), Opt("hold", "milliseconds", Type::Int) },
        { "controller rules --stick 0.7,0.2", "controller rules --trigger 0.65 --hold 2000" },
        { "Pad::StickDirections", "Pad::TriggerHeld", "Pad::Repeater::press", "Pad::Repeater::release", "Pad::Repeater::releaseAll",
          "Pad::Repeater::due", "Pad::Repeater::held" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            json J = json::object();
            if (A.contains("stick"))
            {
                const QStringList P = QString::fromStdString(Command::Str(A, "stick")).split(',');
                bool Ox = false, Oy = false;
                const double X = P.value(0).toDouble(&Ox), Y = P.value(1).toDouble(&Oy);
                if (P.size() != 2 || !Ox || !Oy) throw Command::BadArgs("--stick takes x,y");
                const Pad::Stick S = Pad::StickDirections(X, Y, {});
                json D = json::array();
                if (S.Up) D.push_back("up");
                if (S.Down) D.push_back("down");
                if (S.Left) D.push_back("left");
                if (S.Right) D.push_back("right");
                J["stick"] = D;
            }
            if (A.contains("trigger"))
            {
                bool Ok = false;
                const double V = QString::fromStdString(Command::Str(A, "trigger")).toDouble(&Ok);
                if (!Ok) throw Command::BadArgs("--trigger takes a number 0..1");
                J["trigger"] = Pad::TriggerHeld(V, false);
            }
            if (A.contains("hold"))
            {
                Pad::Repeater R;
                R.press(Pad::Button::Right, 0);
                json At = json::array();
                for (long long T = 0; T <= Command::Int(A, "hold"); T += 5)
                    for (Pad::Button B : R.due(T)) { (void)B; At.push_back(T); }
                R.release(Pad::Button::Right);
                J["repeats"] = At;
                J["held"] = R.held(Pad::Button::Right);
            }
            return J;
        }, {} });
}
