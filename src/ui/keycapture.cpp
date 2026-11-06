#include "keycapture.h"

#include "keynames.h"
#include "commonutils.h"   // Log*

#define SDL_MAIN_HANDLED   // SDL must not rename main() (it does on Windows)
#include <SDL.h>

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPushButton>
#include <QTimer>
#include <QWheelEvent>
#include <QToolButton>

#include <map>
#include <set>

namespace
{
//SDL's game-controller subsystem, started the first time a capture begins (never at app start: most launches never
//rebind). Every controller is opened as it appears; Begin drops whatever was queued, so a button already held when
//the capture starts — or pressed while the window was idle — binds nothing. An axis binds when pushed past half; one
//already past half when the capture starts (a stick held, a trigger resting in) must come back first.
class Gamepads
{
public:
    static Gamepads &Get() { static Gamepads G; return G; }

    void Begin()
    {
        if (!Ok) return;
        SDL_PumpEvents();
        for (int I = 0; I < SDL_NumJoysticks(); ++I) Open(I);
        SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
        Held.clear();
        for (const auto &[Id, C] : Opened)
            for (int A = 0; A < SDL_CONTROLLER_AXIS_MAX; ++A)
                if (Past(SDL_GameControllerGetAxis(C, SDL_GameControllerAxis(A)))) Held.insert({Id, A});
    }

    //The name of the first new press since Begin (or the last call), else "".
    std::string Poll()
    {
        if (!Ok) return {};
        SDL_Event E;
        while (SDL_PollEvent(&E))
        {
            if (E.type == SDL_CONTROLLERDEVICEADDED) Open(E.cdevice.which);
            else if (E.type == SDL_CONTROLLERBUTTONDOWN)
            {
                if (std::string N = KeyCapture::NameOfPadButton(E.cbutton.button); !N.empty()) return N;
            }
            else if (E.type == SDL_CONTROLLERAXISMOTION)
            {
                const std::pair<SDL_JoystickID, int> Axis{E.caxis.which, E.caxis.axis};
                if (!Past(E.caxis.value)) { Held.erase(Axis); continue; }       // back inside half: it counts again
                if (Held.count(Axis)) continue;
                if (std::string N = KeyCapture::NameOfPadAxis(E.caxis.axis, E.caxis.value); !N.empty()) return N;
            }
        }
        return {};
    }

private:
    Gamepads()
    {
        //No window of SDL's own gets focus: the launch window has it, so controllers must report in the background.
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");     // Ctrl+C stays Qt's
        Ok = SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0;
        if (!Ok) LogWarn("KeyCapture", std::string("gamepads unavailable: ") + SDL_GetError());
    }
    void Open(int Index)
    {
        if (!SDL_IsGameController(Index)) return;
        const SDL_JoystickID Id = SDL_JoystickGetDeviceInstanceID(Index);
        if (!Opened.count(Id))
            if (SDL_GameController *C = SDL_GameControllerOpen(Index)) Opened[Id] = C;
    }
    static bool Past(int Value) { return Value > SDL_JOYSTICK_AXIS_MAX / 2 || Value < SDL_JOYSTICK_AXIS_MIN / 2; }
    bool Ok = false;
    std::map<SDL_JoystickID, SDL_GameController *> Opened;
    std::set<std::pair<SDL_JoystickID, int>> Held;     // axes past half when the capture began
};
}

std::string KeyCapture::NameOfPadButton(int B)
{
    static const std::map<int, const char *> Names = {
        {SDL_CONTROLLER_BUTTON_A, "pad a"}, {SDL_CONTROLLER_BUTTON_B, "pad b"},
        {SDL_CONTROLLER_BUTTON_X, "pad x"}, {SDL_CONTROLLER_BUTTON_Y, "pad y"},
        {SDL_CONTROLLER_BUTTON_BACK, "pad back"}, {SDL_CONTROLLER_BUTTON_GUIDE, "pad guide"},
        {SDL_CONTROLLER_BUTTON_START, "pad start"},
        {SDL_CONTROLLER_BUTTON_LEFTSTICK, "pad left stick"}, {SDL_CONTROLLER_BUTTON_RIGHTSTICK, "pad right stick"},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, "pad left shoulder"}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, "pad right shoulder"},
        {SDL_CONTROLLER_BUTTON_DPAD_UP, "pad dpad up"}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, "pad dpad down"},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, "pad dpad left"}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, "pad dpad right"},
        {SDL_CONTROLLER_BUTTON_MISC1, "pad misc"},
        {SDL_CONTROLLER_BUTTON_PADDLE1, "pad paddle 1"}, {SDL_CONTROLLER_BUTTON_PADDLE2, "pad paddle 2"},
        {SDL_CONTROLLER_BUTTON_PADDLE3, "pad paddle 3"}, {SDL_CONTROLLER_BUTTON_PADDLE4, "pad paddle 4"},
        {SDL_CONTROLLER_BUTTON_TOUCHPAD, "pad touchpad"},
    };
    const auto It = Names.find(B);
    return It == Names.end() ? std::string() : It->second;
}

std::string KeyCapture::NameOfPadAxis(int A, int Value)
{
    const bool Neg = Value < 0;
    switch (A)
    {
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT:  return Neg ? std::string() : "pad left trigger";
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return Neg ? std::string() : "pad right trigger";
    case SDL_CONTROLLER_AXIS_LEFTX:        return Neg ? "pad left stick left" : "pad left stick right";
    case SDL_CONTROLLER_AXIS_LEFTY:        return Neg ? "pad left stick up" : "pad left stick down";
    case SDL_CONTROLLER_AXIS_RIGHTX:       return Neg ? "pad right stick left" : "pad right stick right";
    case SDL_CONTROLLER_AXIS_RIGHTY:       return Neg ? "pad right stick up" : "pad right stick down";
    default: return {};
    }
}

std::string KeyCapture::NameOf(const QKeyEvent &E)
{
    const int K = E.key();
    const bool Pad = E.modifiers() & Qt::KeypadModifier;
    const quint32 Vk = E.nativeVirtualKey(), Sc = E.nativeScanCode();
    if (K >= Qt::Key_A && K <= Qt::Key_Z) return std::string(1, char('a' + (K - Qt::Key_A)));
    if (K >= Qt::Key_0 && K <= Qt::Key_9) return (Pad ? "keypad " : "") + std::string(1, char('0' + (K - Qt::Key_0)));
    if (K >= Qt::Key_F1 && K <= Qt::Key_F12) return "f" + std::to_string(K - Qt::Key_F1 + 1);
    //Sides: X11/Wayland report the keysym (Shift_R 0xffe2, Control_R 0xffe4, Alt_R 0xffea); Windows the generic VK
    //(VK_SHIFT 0x10 / VK_CONTROL 0x11 / VK_MENU 0x12) with the scancode telling the side (right shift 0x36,
    //right ctrl/alt carry the extended bit 0x100).
    switch (K)
    {
    case Qt::Key_Shift:   return (Vk == 0xffe2 || (Vk == 0x10 && Sc == 0x36)) ? "right shift" : "left shift";
    case Qt::Key_Control: return (Vk == 0xffe4 || (Vk == 0x11 && (Sc & 0x100))) ? "right ctrl" : "left ctrl";
    case Qt::Key_Alt:     return (Vk == 0xffea || (Vk == 0x12 && (Sc & 0x100))) ? "right alt" : "left alt";
    case Qt::Key_AltGr:   return "right alt";
    case Qt::Key_Super_L: return "left meta";
    case Qt::Key_Super_R: return "right meta";
    case Qt::Key_Meta:    return (Vk == 0xffe8 || Vk == 0xffec || Vk == 0x5C) ? "right meta" : "left meta";
    case Qt::Key_Menu:    return "menu";
    case Qt::Key_Space:   return "space";
    case Qt::Key_Return:  return "enter";
    case Qt::Key_Enter:   return "keypad enter";
    case Qt::Key_Escape:  return "escape";
    case Qt::Key_Tab: case Qt::Key_Backtab: return "tab";
    case Qt::Key_Backspace: return "backspace";
    case Qt::Key_Insert:  return "insert";
    case Qt::Key_Delete:  return "delete";
    case Qt::Key_Home:    return "home";
    case Qt::Key_End:     return "end";
    case Qt::Key_PageUp:  return "page up";
    case Qt::Key_PageDown:return "page down";
    case Qt::Key_Up:      return "up";
    case Qt::Key_Down:    return "down";
    case Qt::Key_Left:    return "left";
    case Qt::Key_Right:   return "right";
    case Qt::Key_CapsLock:   return "caps lock";
    case Qt::Key_NumLock:    return "num lock";
    case Qt::Key_ScrollLock: return "scroll lock";
    case Qt::Key_Print:      return "print screen";
    case Qt::Key_Pause:      return "pause";
    case Qt::Key_Minus:    return Pad ? "keypad minus" : "minus";
    case Qt::Key_Plus:     return Pad ? "keypad plus" : "equals";       // unshifted, Plus is the = key's shift
    case Qt::Key_Asterisk: return Pad ? "keypad multiply" : "8";
    case Qt::Key_Slash:    return Pad ? "keypad divide" : "slash";
    case Qt::Key_Period:   return Pad ? "keypad period" : "period";
    case Qt::Key_Comma:    return Pad ? "keypad period" : "comma";       // a locale's keypad decimal comma
    default: break;
    }
    //Unshifted punctuation, and what Shift turns each key into on a US layout (the key bound is the key pressed).
    static const std::map<int, const char *> Punct = {
        {Qt::Key_Equal, "equals"}, {Qt::Key_BracketLeft, "left bracket"}, {Qt::Key_BracketRight, "right bracket"},
        {Qt::Key_Backslash, "backslash"}, {Qt::Key_Semicolon, "semicolon"}, {Qt::Key_Apostrophe, "quote"},
        {Qt::Key_QuoteLeft, "backquote"},
        {Qt::Key_Exclam, "1"}, {Qt::Key_At, "2"}, {Qt::Key_NumberSign, "3"}, {Qt::Key_Dollar, "4"}, {Qt::Key_Percent, "5"},
        {Qt::Key_AsciiCircum, "6"}, {Qt::Key_Ampersand, "7"}, {Qt::Key_ParenLeft, "9"}, {Qt::Key_ParenRight, "0"},
        {Qt::Key_Underscore, "minus"}, {Qt::Key_BraceLeft, "left bracket"}, {Qt::Key_BraceRight, "right bracket"},
        {Qt::Key_Bar, "backslash"}, {Qt::Key_Colon, "semicolon"}, {Qt::Key_QuoteDbl, "quote"},
        {Qt::Key_AsciiTilde, "backquote"}, {Qt::Key_Less, "comma"}, {Qt::Key_Greater, "period"}, {Qt::Key_Question, "slash"},
    };
    const auto It = Punct.find(K);
    return It == Punct.end() ? std::string() : It->second;
}

std::string KeyCapture::NameOf(Qt::MouseButton B)
{
    switch (B)
    {
    case Qt::LeftButton:    return "mouse left";
    case Qt::RightButton:   return "mouse right";
    case Qt::MiddleButton:  return "mouse middle";
    case Qt::BackButton:    return "mouse 4";
    case Qt::ForwardButton: return "mouse 5";
    default: return {};
    }
}

std::string KeyCapture::NameOfWheel(int DeltaY)
{
    return DeltaY > 0 ? "mouse wheel up" : DeltaY < 0 ? "mouse wheel down" : std::string();
}

KeyCaptureField::KeyCaptureField(const std::string &V, const std::string &Def, QWidget *Parent)
    : QWidget(Parent), Default(KeyNames::IsValue(Def) ? Def : std::string())
{
    auto *Row = new QHBoxLayout(this);
    Row->setContentsMargins(0, 0, 0, 0);
    Row->setSpacing(2);
    Button = new QPushButton(this);
    Button->setObjectName("keycapture");
    Clear = new QToolButton(this);
    Clear->setText("×");
    Clear->setToolTip("Unbind");
    Reset = new QToolButton(this);
    Reset->setText("↺");
    Reset->setToolTip("Back to the default");
    Row->addWidget(Button, 1);
    Row->addWidget(Reset);
    Row->addWidget(Clear);
    Button->installEventFilter(this);
    PadPoll = new QTimer(this);
    PadPoll->setInterval(16);
    connect(PadPoll, &QTimer::timeout, this, [this] {
        const std::string N = Gamepads::Get().Poll();
        if (N.empty()) return;
        stopCapture();
        setValue(N);
    });
    connect(Button, &QPushButton::clicked, this, [this] { if (!Capturing) startCapture(); });
    connect(Clear, &QToolButton::clicked, this, [this] { stopCapture(); setValue(""); });
    connect(Reset, &QToolButton::clicked, this, [this] { stopCapture(); setValue(Default); });
    Value = KeyNames::IsValue(V) ? V : std::string();
    refresh();
}

void KeyCaptureField::setValue(const std::string &V)
{
    if (!KeyNames::IsValue(V) || V == Value) { refresh(); return; }
    Value = V;
    refresh();
    emit changed();
}

void KeyCaptureField::setConflict(const QString &With)
{
    ConflictWith = With;
    refresh();
}

void KeyCaptureField::startCapture()
{
    Capturing = true;
    Button->setFocus(Qt::OtherFocusReason);
    Gamepads::Get().Begin();
    PadPoll->start();
    refresh();
}

void KeyCaptureField::stopCapture()
{
    Capturing = false;
    PadPoll->stop();
    refresh();
}

void KeyCaptureField::refresh()
{
    if (Capturing) { Button->setText("Press a key…"); Button->setStyleSheet({}); Button->setToolTip({}); return; }
    const auto Name = [](const std::string &K) {
        QString S = QString::fromStdString(K);
        if (S.isEmpty()) return QString("(none)");
        if (S.size() == 1) return S.toUpper();
        S[0] = S[0].toUpper();
        return S;
    };
    Button->setText(Value == Default ? Name(Value)
                                     : Name(Value) + "  (default " + (Default.empty() ? QString("none") : Name(Default)) + ")");
    Button->setStyleSheet(ConflictWith.isEmpty() ? QString() : QString("color: #d04040; font-weight: bold;"));
    Button->setToolTip(ConflictWith.isEmpty() ? QString("Click, then press a key or mouse button")
                                              : "Also bound to: " + ConflictWith);
    Clear->setEnabled(!Value.empty());
    Reset->setEnabled(Value != Default);
}

bool KeyCaptureField::eventFilter(QObject *Watched, QEvent *Ev)
{
    if (Watched != Button || !Capturing) return QWidget::eventFilter(Watched, Ev);
    switch (Ev->type())
    {
    case QEvent::KeyPress:
    {
        //Every key binds — Escape and Tab too: a game's menu key is often Escape. Focus leaving cancels instead.
        const std::string N = KeyCapture::NameOf(*static_cast<QKeyEvent *>(Ev));
        if (N.empty()) return true;                                  // a key no option can hold: keep waiting
        stopCapture();
        setValue(N);
        return true;
    }
    case QEvent::ShortcutOverride:                                   // Tab, Escape: ours while capturing
        Ev->accept();
        return true;
    case QEvent::MouseButtonPress:
    {
        const std::string N = KeyCapture::NameOf(static_cast<QMouseEvent *>(Ev)->button());
        if (N.empty()) return true;
        stopCapture();
        setValue(N);
        return true;                                                 // eaten: no click → no new capture
    }
    case QEvent::Wheel:                                              // a horizontal tilt alone binds nothing
    {
        const std::string N = KeyCapture::NameOfWheel(static_cast<QWheelEvent *>(Ev)->angleDelta().y());
        if (N.empty()) return true;
        stopCapture();
        setValue(N);
        return true;
    }
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
        return true;
    case QEvent::FocusOut:
        stopCapture();
        return false;
    default:
        return QWidget::eventFilter(Watched, Ev);
    }
}
