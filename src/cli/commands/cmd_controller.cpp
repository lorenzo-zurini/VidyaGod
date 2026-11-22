// `VidyaGod controller …` — the controller UI: controller mode on or off, a controller's presses (the same delivery a
// real controller's take), the exact state of what they act on (the window, the focus, the shell's section and page,
// the on-screen keyboard), the controllers connected, and the controller settings.
#include "command.h"
#include "cmdutil.h"
#include "uifind.h"   // Act — a press that opens a dialog answers at once

#include "appmodel.h"
#include "controllerservice.h"
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
        "On, the window goes full screen showing the controller shell (Home, Library, Catalog, Downloads, Friends, Node, "
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
        "controller layout", "load a controller layout into the controller service",
        "A layout is an InputPlumber device profile (YAML: `kind: DeviceProfile`). It goes to every controller the "
        "service owns, and to any that comes later; --default goes back to InputPlumber's own.",
        { Command::OptPos("file", "the layout's YAML file"), Flag("default", "InputPlumber's own layout again") },
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
        "What the shoulders do one step at a time: Home, Library, Catalog, Downloads, Friends, Node, Settings.",
        { Pos("section", "the section", Type::String) }, { "controller section Library", "controller section downloads" },
        { "DeckShell::showSection", "DeckShell::SectionFrom", "DeckShell::section", "DeckShelf::rebuild", "DeckHome::refresh",
          "DeckDownloads::refresh", "DeckShell::libraryCards", "DeckShell::catalogCards", "DeckShelf::refreshCovers",
          "CardPool::Installed", "CardPool::Available", "CardPool::Sort", "CardPool::SortBySection", "CardPool::SectionOf",
          "DeckGrid::setCards", "DeckGrid::setCurrentKey", "DeckGrid::setEmptyText", "DeckGrid::setLit", "DeckGrid::setStrip",
          "DeckGrid::repaintCards", "DeckStyle::ScaleFor", "DeckStyle::Font", "DeckStyle::HostedStyleSheet" }, Needs::Ui, false,
        [](Command::Context &C, const json &A) {
            const QString N = QString::fromStdString(Command::Str(A, "section"));
            const std::optional<DeckShell::Section> S = DeckShell::SectionFrom(N);
            if (!S) throw Command::BadArgs("no section '" + N.toStdString() + "' — Home, Library, Catalog, Downloads, Friends, Node, Settings");
            Shell(C).showSection(*S);
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
