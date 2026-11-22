#include "padinput.h"

#include "commonutils.h"    // Log*
#include "keycapture.h"     // KeyCapture::NameOfPad* — the capture's names
#include "launchthread.h"   // LaunchThread::Running — a game is being played

#define SDL_MAIN_HANDLED   // SDL must not rename main() (it does on Windows)
#include <SDL.h>

#include <QDateTime>
#include <QGuiApplication>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <map>

// ═════════════════════════════════════════════════════════════════════════════
// Pad — names and the pure press rules
// ═════════════════════════════════════════════════════════════════════════════

namespace Pad
{
namespace
{
const std::array<const char *, ButtonCount> &Table()
{
    static const std::array<const char *, ButtonCount> T = { "up", "down", "left", "right", "a", "b", "x", "y", "lb", "rb",
                                                             "lt", "rt", "start", "back", "guide", "l3", "r3" };
    return T;
}
}

std::string Name(Button B) { return Table()[static_cast<size_t>(B)]; }

std::optional<Button> FromName(const std::string &N)
{
    std::string L;
    for (char C : N) L.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(C))));
    for (size_t I = 0; I < Table().size(); ++I)
        if (L == Table()[I]) return static_cast<Button>(I);
    return std::nullopt;
}

std::vector<std::string> Names() { return { Table().begin(), Table().end() }; }

bool IsDirection(Button B) { return B == Button::Up || B == Button::Down || B == Button::Left || B == Button::Right; }

std::string GlyphsName(Glyphs G)
{
    switch (G)
    {
    case Glyphs::PlayStation: return "playstation";
    case Glyphs::Nintendo:    return "nintendo";
    case Glyphs::SteamDeck:   return "steamdeck";
    default:                  return "xbox";
    }
}

Stick StickDirections(double X, double Y, const Stick &Was)
{
    //Each direction by itself, with hysteresis; then, of two held on different axes, only the stronger one — a
    //diagonal moves focus one way at a time, as a d-pad's diagonal never should.
    const auto Held = [](double V, bool W) { return W ? V > StickRelease : V > StickPress; };
    Stick S;
    S.Right = Held(X, Was.Right);
    S.Left  = Held(-X, Was.Left);
    S.Down  = Held(Y, Was.Down);
    S.Up    = Held(-Y, Was.Up);
    if ((S.Left || S.Right) && (S.Up || S.Down))
    {
        if (std::fabs(X) >= std::fabs(Y)) S.Up = S.Down = false;
        else S.Left = S.Right = false;
    }
    return S;
}

bool TriggerHeld(double V, bool Was) { return Was ? V > 0.3 : V > 0.6; }

void Repeater::press(Button B, long long Now)
{
    if (!IsDirection(B)) return;
    Holds[static_cast<size_t>(B)] = { true, Now, Now + First };
}

void Repeater::release(Button B) { Holds[static_cast<size_t>(B)].On = false; }

void Repeater::releaseAll() { for (Hold &H : Holds) H.On = false; }

bool Repeater::held(Button B) const { return Holds[static_cast<size_t>(B)].On; }

std::vector<Button> Repeater::due(long long Now)
{
    std::vector<Button> Out;
    for (size_t I = 0; I < Holds.size(); ++I)
    {
        Hold &H = Holds[I];
        if (!H.On || Now < H.Next) continue;
        Out.push_back(static_cast<Button>(I));
        //A poll that came late owes one repeat, not a burst: focus jumping five tiles after a hitch is worse than a
        //tile skipped.
        H.Next = Now + (Now - H.Since >= FastAfter ? Fast : Every);
    }
    return Out;
}
}

// ═════════════════════════════════════════════════════════════════════════════
// PadInput
// ═════════════════════════════════════════════════════════════════════════════

struct PadInput::Opened
{
    SDL_JoystickID Id = -1;
    SDL_GameController *C = nullptr;
    QString Name;
    Pad::Glyphs Glyphs = Pad::Glyphs::Xbox;
};

namespace
{
long long NowMs() { return QDateTime::currentMSecsSinceEpoch(); }

Pad::Glyphs GlyphsOf(SDL_GameController *C)
{
    switch (SDL_GameControllerGetType(C))
    {
    case SDL_CONTROLLER_TYPE_PS3:
    case SDL_CONTROLLER_TYPE_PS4:
    case SDL_CONTROLLER_TYPE_PS5:
        return Pad::Glyphs::PlayStation;
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return Pad::Glyphs::Nintendo;
    default:
    {
        //Steam Input's virtual controller says it is an Xbox 360 pad; on a Steam Deck it is the Deck's own buttons
        //(labelled L1/R1, L2/R2), so they are drawn as the Deck draws them.
        const char *N = SDL_GameControllerName(C);
        if (qEnvironmentVariable("SteamDeck") == "1" || (N && QString::fromUtf8(N).contains("Steam Deck", Qt::CaseInsensitive)))
            return Pad::Glyphs::SteamDeck;
        return Pad::Glyphs::Xbox;   // Xbox, Steam Input's virtual controller elsewhere, generic pads
    }
    }
}

std::optional<Pad::Button> ButtonOf(int B)
{
    static const std::map<int, Pad::Button> M = {
        {SDL_CONTROLLER_BUTTON_DPAD_UP, Pad::Button::Up}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, Pad::Button::Down},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, Pad::Button::Left}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, Pad::Button::Right},
        {SDL_CONTROLLER_BUTTON_A, Pad::Button::A}, {SDL_CONTROLLER_BUTTON_B, Pad::Button::B},
        {SDL_CONTROLLER_BUTTON_X, Pad::Button::X}, {SDL_CONTROLLER_BUTTON_Y, Pad::Button::Y},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, Pad::Button::LB}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, Pad::Button::RB},
        {SDL_CONTROLLER_BUTTON_START, Pad::Button::Start}, {SDL_CONTROLLER_BUTTON_BACK, Pad::Button::Back},
        {SDL_CONTROLLER_BUTTON_GUIDE, Pad::Button::Guide},
        {SDL_CONTROLLER_BUTTON_LEFTSTICK, Pad::Button::L3}, {SDL_CONTROLLER_BUTTON_RIGHTSTICK, Pad::Button::R3},
    };
    const auto It = M.find(B);
    return It == M.end() ? std::nullopt : std::optional<Pad::Button>(It->second);
}

double Axis(Sint16 V) { return V < 0 ? V / 32768.0 : V / 32767.0; }
bool PastHalf(int V) { return V > SDL_JOYSTICK_AXIS_MAX / 2 || V < SDL_JOYSTICK_AXIS_MIN / 2; }
}

PadInput &PadInput::instance()
{
    static PadInput *P = new PadInput();   // never destroyed: the app ends by _Exit, and SDL must not quit under a poll
    return *P;
}

PadInput::PadInput() : Pads(new std::vector<Opened>())
{
    Timer = new QTimer(this);
    Timer->setInterval(8);   // 125 Hz: a tap shorter than a frame is never lost, and a repeat lands within a frame
    connect(Timer, &QTimer::timeout, this, &PadInput::poll);
}

bool PadInput::start()
{
    if (Started) return Ok;
    Started = true;
    //No window of SDL's own gets focus (Qt's has it), so controllers must report in the background; whether the app
    //takes a press is decided here (active()), not by SDL.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");     // Ctrl+C stays Qt's
    //The Steam Deck's own controller through hidraw would turn off its trackpad mouse ("lizard mode") for as long as
    //it is open — the desktop's pointer. Steam Input's virtual controller is what to read there, in Game Mode and out.
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_STEAMDECK", "0");
    Ok = SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0;
    if (!Ok) LogWarn("PadInput", std::string("game controllers unavailable: ") + SDL_GetError());
    return Ok;
}

void PadInput::setEnabled(bool On)
{
    if (On == Enabled) return;
    Enabled = On;
    if (!On) { Timer->stop(); Repeat.releaseAll(); Down.fill(false); DpadHeld.fill(false); return; }
    if (!start()) return;
    SDL_PumpEvents();
    for (int I = 0; I < SDL_NumJoysticks(); ++I) open(I);
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);   // what was pressed while nobody read is not a press now
    LastPoll = NowMs();
    Timer->start();
}

void PadInput::open(int Index)
{
    if (!SDL_IsGameController(Index)) return;
    const SDL_JoystickID Id = SDL_JoystickGetDeviceInstanceID(Index);
    for (const Opened &O : *Pads) if (O.Id == Id) return;
    SDL_GameController *C = SDL_GameControllerOpen(Index);
    if (!C) return;
    const char *N = SDL_GameControllerName(C);
    Pads->push_back({ Id, C, QString::fromUtf8(N ? N : "controller"), GlyphsOf(C) });
    if (LastPad < 0) { LastPad = Id; LastGlyphs = Pads->back().Glyphs; }
    LogOut("PadInput", "controller connected: " + Pads->back().Name.toStdString());
    emit padsChanged();
}

void PadInput::closeAll()
{
    if (Pads->empty()) return;
    for (Opened &O : *Pads) SDL_GameControllerClose(O.C);
    Pads->clear();
    Repeat.releaseAll();
    Down.fill(false);
    DpadHeld.fill(false);
    Left = {};
    LtHeld = RtHeld = false;
    emit padsChanged();
}

QList<PadInput::PadInfo> PadInput::pads() const
{
    QList<PadInfo> Out;
    for (const Opened &O : *Pads) Out.push_back({ O.Name, O.Glyphs });
    return Out;
}

bool PadInput::suspended() const { return LaunchThread::Running() > 0; }

bool PadInput::active() const
{
    return QGuiApplication::applicationState() == Qt::ApplicationActive && !suspended();
}

void PadInput::inject(Pad::Button B)
{
    emit used();
    emit pressed(B);
    emit released(B);
}

void PadInput::injectChord(const std::vector<Pad::Button> &Held, Pad::Button B)
{
    for (Pad::Button H : Held) Down[static_cast<size_t>(H)] = true;
    inject(B);
    for (Pad::Button H : Held) Down[static_cast<size_t>(H)] = false;
}

void PadInput::tick()
{
    if (!Haptics || !Ok) return;
    for (const Opened &O : *Pads)
        if (O.Id == LastPad) { SDL_GameControllerRumble(O.C, 0x2000, 0x3000, 18); return; }
}

void PadInput::pace()
{
    //125 polls a second while a controller is in use (a tap shorter than a frame is never lost, a repeat lands on
    //time); 30 once it has been idle a few seconds (a first press waits at most a frame); 4 while the app is in the
    //background or a game runs; 2 with no controller at all (a hotplug is still seen within half a second). A
    //tray app seeding all day on a Deck's battery must not wake 125 times a second for a controller nobody holds.
    int Ms = 8;
    if (Pads->empty()) Ms = 500;
    else if (!active()) Ms = 250;
    else if (QDateTime::currentMSecsSinceEpoch() - LastInput > 3000) Ms = 33;
    if (Timer->interval() != Ms) Timer->setInterval(Ms);
}

void PadInput::deliver(Pad::Button B, bool IsDown, bool /*Synthetic*/)
{
    const size_t I = static_cast<size_t>(B);
    if (Down[I] == IsDown) return;
    Down[I] = IsDown;
    LastInput = QDateTime::currentMSecsSinceEpoch();
    if (!IsDown) { Repeat.release(B); emit released(B); return; }
    Repeat.press(B, NowMs());
    emit used();
    emit pressed(B);
}

void PadInput::poll()
{
    if (!Ok || Capturing) return;   // a capture owns the queue
    //A launch is running: the game has the controllers. Close them (an open device can still be grabbed from under
    //the game — the Deck's), and open them again when it ends.
    if (suspended())
    {
        if (!WasSuspended) { WasSuspended = true; closeAll(); }
        if (Timer->interval() != 250) Timer->setInterval(250);   // only to see the game end
        return;
    }
    if (WasSuspended)
    {
        WasSuspended = false;
        SDL_PumpEvents();
        for (int I = 0; I < SDL_NumJoysticks(); ++I) open(I);
        SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);   // nothing pressed in the game counts here
    }
    const long long Now = NowMs();
    const double Dt = std::clamp(static_cast<double>(Now - LastPoll), 0.0, 50.0);
    LastPoll = Now;
    const bool Take = active();
    std::array<bool, 4> &Dpad = DpadHeld;
    SDL_Event E;
    while (SDL_PollEvent(&E))
    {
        switch (E.type)
        {
        case SDL_CONTROLLERDEVICEADDED: open(E.cdevice.which); break;
        case SDL_CONTROLLERDEVICEREMOVED:
            for (auto It = Pads->begin(); It != Pads->end(); ++It)
                if (It->Id == E.cdevice.which)
                {
                    LogOut("PadInput", "controller disconnected: " + It->Name.toStdString());
                    SDL_GameControllerClose(It->C);
                    Pads->erase(It);
                    emit padsChanged();
                    break;
                }
            break;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
        {
            const bool IsDown = E.type == SDL_CONTROLLERBUTTONDOWN;
            if (IsDown && E.cbutton.which != LastPad)
                for (const Opened &O : *Pads)
                    if (O.Id == E.cbutton.which) { LastPad = O.Id; LastGlyphs = O.Glyphs; }
            const std::optional<Pad::Button> B = ButtonOf(E.cbutton.button);
            if (!B) break;
            if (Pad::IsDirection(*B)) { Dpad[static_cast<size_t>(*B)] = IsDown; break; }
            if (Take || !IsDown) deliver(*B, IsDown, false);
            break;
        }
        default: break;
        }
    }
    //Sticks and triggers by their state now, from the controller used last (two controllers' sticks never fight).
    double Rx = 0, Ry = 0;
    Pad::Stick Stick;
    bool Lt = false, Rt = false;
    for (const Opened &O : *Pads)
    {
        if (O.Id != LastPad) continue;
        Stick = Pad::StickDirections(Axis(SDL_GameControllerGetAxis(O.C, SDL_CONTROLLER_AXIS_LEFTX)),
                                     Axis(SDL_GameControllerGetAxis(O.C, SDL_CONTROLLER_AXIS_LEFTY)), Left);
        Lt = Pad::TriggerHeld(Axis(SDL_GameControllerGetAxis(O.C, SDL_CONTROLLER_AXIS_TRIGGERLEFT)), LtHeld);
        Rt = Pad::TriggerHeld(Axis(SDL_GameControllerGetAxis(O.C, SDL_CONTROLLER_AXIS_TRIGGERRIGHT)), RtHeld);
        Rx = Axis(SDL_GameControllerGetAxis(O.C, SDL_CONTROLLER_AXIS_RIGHTX));
        Ry = Axis(SDL_GameControllerGetAxis(O.C, SDL_CONTROLLER_AXIS_RIGHTY));
    }
    Left = Stick;
    const std::array<bool, 4> Dir{ Dpad[0] || Stick.Up, Dpad[1] || Stick.Down, Dpad[2] || Stick.Left, Dpad[3] || Stick.Right };
    for (size_t I = 0; I < 4; ++I)
        if (Take || !Dir[I]) deliver(static_cast<Pad::Button>(I), Dir[I], false);
    if (Take || !Lt) { deliver(Pad::Button::LT, Lt, false); LtHeld = Lt; }
    if (Take || !Rt) { deliver(Pad::Button::RT, Rt, false); RtHeld = Rt; }
    pace();
    if (!Take) { Repeat.releaseAll(); return; }
    for (Pad::Button B : Repeat.due(Now)) emit pressed(B);
    //The right stick scrolls: a dead zone, then speed rising with the square of the push (fine control near the
    //centre, fast at the edge), up to 2400 px/s.
    const auto Speed = [](double V) { const double A = std::fabs(V); return A < 0.18 ? 0.0 : std::copysign((A - 0.18) * (A - 0.18) / 0.6724, V); };
    const double Sx = Speed(Rx) * 2.4 * Dt, Sy = Speed(Ry) * 2.4 * Dt;
    if (Sx != 0 || Sy != 0) { LastInput = Now; emit used(); emit scrolled(Sx, Sy); }
}

void PadInput::captureBegin()
{
    if (!start()) return;
    Capturing = true;
    SDL_PumpEvents();
    for (int I = 0; I < SDL_NumJoysticks(); ++I) open(I);
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    CaptureHeld.clear();
    for (const Opened &O : *Pads)
        for (int A = 0; A < SDL_CONTROLLER_AXIS_MAX; ++A)
            if (PastHalf(SDL_GameControllerGetAxis(O.C, SDL_GameControllerAxis(A)))) CaptureHeld.push_back({ O.Id, A });
}

std::string PadInput::capturePoll()
{
    if (!Ok || !Capturing) return {};
    SDL_Event E;
    while (SDL_PollEvent(&E))
    {
        if (E.type == SDL_CONTROLLERDEVICEADDED) open(E.cdevice.which);
        else if (E.type == SDL_CONTROLLERBUTTONDOWN)
        {
            if (std::string N = KeyCapture::NameOfPadButton(E.cbutton.button); !N.empty()) return N;
        }
        else if (E.type == SDL_CONTROLLERAXISMOTION)
        {
            const std::pair<int, int> A{ E.caxis.which, E.caxis.axis };
            const auto Held = std::find(CaptureHeld.begin(), CaptureHeld.end(), A);
            if (!PastHalf(E.caxis.value)) { if (Held != CaptureHeld.end()) CaptureHeld.erase(Held); continue; }   // back inside half: it counts again
            if (Held != CaptureHeld.end()) continue;
            if (std::string N = KeyCapture::NameOfPadAxis(E.caxis.axis, E.caxis.value); !N.empty()) return N;
        }
    }
    return {};
}

void PadInput::captureEnd()
{
    if (!Capturing) return;
    Capturing = false;
    //What was held when the capture took it is not pressed now: the button that bound must not also navigate.
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    Repeat.releaseAll();
    Down.fill(false);
    DpadHeld.fill(false);
    Left = {};
}
