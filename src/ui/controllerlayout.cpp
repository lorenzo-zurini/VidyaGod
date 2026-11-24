#include "controllerlayout.h"

#include <atomic>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

namespace ControllerLayout
{

const std::vector<Control> &Controls()
{
    static const std::vector<Control> V = {
        { "a", "A", "Buttons", Kind::Button, "South" },
        { "b", "B", "Buttons", Kind::Button, "East" },
        { "x", "X", "Buttons", Kind::Button, "West" },
        { "y", "Y", "Buttons", Kind::Button, "North" },
        { "dpad_up", "D-pad up", "D-pad", Kind::Button, "DPadUp" },
        { "dpad_down", "D-pad down", "D-pad", Kind::Button, "DPadDown" },
        { "dpad_left", "D-pad left", "D-pad", Kind::Button, "DPadLeft" },
        { "dpad_right", "D-pad right", "D-pad", Kind::Button, "DPadRight" },
        { "lb", "Left bumper", "Bumpers and triggers", Kind::Button, "LeftBumper" },
        { "rb", "Right bumper", "Bumpers and triggers", Kind::Button, "RightBumper" },
        { "lt", "Left trigger", "Bumpers and triggers", Kind::Trigger, "LeftTrigger" },
        { "rt", "Right trigger", "Bumpers and triggers", Kind::Trigger, "RightTrigger" },
        { "left_stick", "Left stick", "Sticks", Kind::Stick, "LeftStick" },
        { "right_stick", "Right stick", "Sticks", Kind::Stick, "RightStick" },
        { "l3", "Left stick click", "Sticks", Kind::Button, "LeftStick" },
        { "r3", "Right stick click", "Sticks", Kind::Button, "RightStick" },
        { "left_stick_touch", "Left stick touch", "Sticks", Kind::Button, "LeftStickTouch" },
        { "right_stick_touch", "Right stick touch", "Sticks", Kind::Button, "RightStickTouch" },
        { "left_pad", "Left trackpad", "Trackpads", Kind::Trackpad, "LeftPad" },
        { "right_pad", "Right trackpad", "Trackpads", Kind::Trackpad, "RightPad" },
        { "left_pad_press", "Left trackpad click", "Trackpads", Kind::Button, "LeftPad/Press" },
        { "right_pad_press", "Right trackpad click", "Trackpads", Kind::Button, "RightPad/Press" },
        { "left_pad_touch", "Left trackpad touch", "Trackpads", Kind::Button, "LeftPad/Touch" },
        { "right_pad_touch", "Right trackpad touch", "Trackpads", Kind::Button, "RightPad/Touch" },
        { "l4", "L4", "Back grips", Kind::Button, "LeftPaddle1" },
        { "l5", "L5", "Back grips", Kind::Button, "LeftPaddle2" },
        { "r4", "R4", "Back grips", Kind::Button, "RightPaddle1" },
        { "r5", "R5", "Back grips", Kind::Button, "RightPaddle2" },
        { "start", "Menu", "Menu buttons", Kind::Button, "Start" },
        { "select", "View", "Menu buttons", Kind::Button, "Select" },
        { "guide", "Guide (Steam)", "Menu buttons", Kind::Button, "Guide" },
        { "quick", "Quick access (…)", "Menu buttons", Kind::Button, "QuickAccess" },
        { "gyro", "Gyro", "Gyro", Kind::Gyro, "Gyro1" },
    };
    return V;
}

const Control *FindControl(const std::string &Id)
{
    for (const Control &C : Controls()) if (C.Id == Id) return &C;
    return nullptr;
}

std::string TypeName(Action::Type T)
{
    switch (T)
    {
    case Action::Type::Gamepad:  return "gamepad";
    case Action::Type::Keyboard: return "keyboard";
    case Action::Type::Mouse:    return "mouse";
    case Action::Type::Overlay:  return "overlay";
    case Action::Type::Nothing:  return "nothing";
    }
    return "nothing";
}

std::optional<Action::Type> TypeFrom(const std::string &N)
{
    for (Action::Type T : { Action::Type::Gamepad, Action::Type::Keyboard, Action::Type::Mouse, Action::Type::Overlay, Action::Type::Nothing })
        if (TypeName(T) == N) return T;
    return std::nullopt;
}

// The names InputPlumber's device profile schema lists AND its parser accepts (InputPlumber/src/input/capability.rs:
// the schema lists a few the code does not know, which would load as nothing).
const std::vector<std::string> &KeyboardKeys()
{
    static const std::vector<std::string> V = {
        "KeyEsc", "Key1", "Key2", "Key3", "Key4", "Key5", "Key6", "Key7", "Key8", "Key9", "Key0", "KeyMinus",
        "KeyEqual", "KeyBackspace", "KeyTab", "KeyQ", "KeyW", "KeyE", "KeyR", "KeyT", "KeyY", "KeyU", "KeyI", "KeyO",
        "KeyP", "KeyLeftBrace", "KeyRightBrace", "KeyEnter", "KeyLeftCtrl", "KeyA", "KeyS", "KeyD", "KeyF", "KeyG",
        "KeyH", "KeyJ", "KeyK", "KeyL", "KeySemicolon", "KeyApostrophe", "KeyGrave", "KeyLeftShift", "KeyBackslash",
        "KeyBrightnessDown", "KeyBrightnessUp", "KeyZ", "KeyX", "KeyC", "KeyV", "KeyB", "KeyN", "KeyM", "KeyComma",
        "KeyDot", "KeySlash", "KeyRightShift", "KeyKpAsterisk", "KeyLeftAlt", "KeySpace", "KeyCapslock", "KeyF1",
        "KeyF2", "KeyF3", "KeyF4", "KeyF5", "KeyF6", "KeyF7", "KeyF8", "KeyF9", "KeyF10", "KeyNumlock",
        "KeyScrollLock", "KeyKp7", "KeyKp8", "KeyKp9", "KeyKpMinus", "KeyKp4", "KeyKp5", "KeyKp6", "KeyKpPlus",
        "KeyKp1", "KeyKp2", "KeyKp3", "KeyKp0", "KeyZenkakuhankaku", "Key102nd", "KeyF11", "KeyF12", "KeyRo",
        "KeyKatakana", "KeyHiragana", "KeyHenkan", "KeyKatakanaHiragana", "KeyMuhenkan", "KeyKpJpComma",
        "KeyKpEnter", "KeyRightCtrl", "KeyKpSlash", "KeySysrq", "KeyRightAlt", "KeyHome", "KeyUp", "KeyPageUp",
        "KeyLeft", "KeyRight", "KeyEnd", "KeyDown", "KeyPageDown", "KeyInsert", "KeyDelete", "KeyMute",
        "KeyVolumeDown", "KeyVolumeUp", "KeyPower", "KeyKpEqual", "KeyPause", "KeyKpComma", "KeyHanja", "KeyYen",
        "KeyLeftMeta", "KeyRightMeta", "KeyCompose", "KeyStop", "KeyAgain", "KeyProps", "KeyUndo", "KeyFront",
        "KeyCopy", "KeyOpen", "KeyPaste", "KeyFind", "KeyCut", "KeyHelp", "KeyCalc", "KeySleep", "KeyWww", "KeyBack",
        "KeyForward", "KeyEjectCD", "KeyNextSong", "KeyPlayPause", "KeyPreviousSong", "KeyStopCD", "KeyRefresh",
        "KeyEdit", "KeyScrollUp", "KeyScrollDown", "KeyKpLeftParen", "KeyKpRightParen", "KeyF13", "KeyF14", "KeyF15",
        "KeyF16", "KeyF17", "KeyF18", "KeyF19", "KeyF20", "KeyF21", "KeyF22", "KeyF23", "KeyF24", "KeyProg1",
    };
    return V;
}

const std::vector<std::string> &GamepadButtons()
{
    static const std::vector<std::string> V = {
        "South", "East", "North", "West", "Start", "Select", "Guide", "QuickAccess", "QuickAccess2", "Keyboard",
        "Screenshot", "DPadUp", "DPadDown", "DPadLeft", "DPadRight", "LeftBumper", "LeftTop", "LeftTrigger",
        "LeftPaddle1", "LeftPaddle2", "LeftPaddle3", "LeftStick", "LeftStickTouch", "RightBumper", "RightTop",
        "RightTrigger", "RightPaddle1", "RightPaddle2", "RightPaddle3", "RightStick", "RightStickTouch",
    };
    return V;
}

const std::vector<std::string> &MouseButtons()
{
    static const std::vector<std::string> V = {
        "Left", "Right", "Middle", "WheelUp", "WheelDown", "WheelLeft", "WheelRight", "Extra1", "Extra2",
    };
    return V;
}

namespace
{
bool In(const std::vector<std::string> &V, const std::string &X) { return std::find(V.begin(), V.end(), X) != V.end(); }

// "VolumeDown" → "Volume Down"
std::string SplitCamel(const std::string &S)
{
    std::string Out;
    for (size_t I = 0; I < S.size(); ++I)
    {
        const bool Up = S[I] >= 'A' && S[I] <= 'Z', Digit = S[I] >= '0' && S[I] <= '9';
        const bool PrevLower = I > 0 && S[I - 1] >= 'a' && S[I - 1] <= 'z';
        //A word starts at a capital, or at a number after a lowercase word ("Volume Down", "Mute 2"; "F11" stays whole).
        if (I > 0 && (Up || (Digit && PrevLower)) && Out.back() != ' ') Out += ' ';
        Out += S[I];
    }
    return Out;
}

const char *StickDirs[4] = { "up", "down", "left", "right" };
}

std::string KeyLabel(const std::string &Key)
{
    static const std::map<std::string, std::string> Special = {
        { "KeyEsc", "Esc" }, { "KeyLeftCtrl", "Left Ctrl" }, { "KeyRightCtrl", "Right Ctrl" }, { "KeyLeftShift", "Left Shift" },
        { "KeyRightShift", "Right Shift" }, { "KeyLeftAlt", "Left Alt" }, { "KeyRightAlt", "Right Alt" }, { "KeyLeftMeta", "Left Super" },
        { "KeyRightMeta", "Right Super" }, { "KeyCapslock", "Caps Lock" }, { "KeyLeftBrace", "[" }, { "KeyRightBrace", "]" },
        { "KeySemicolon", ";" }, { "KeyApostrophe", "'" }, { "KeyGrave", "`" }, { "KeyBackslash", "\\" }, { "KeyComma", "," },
        { "KeyDot", "." }, { "KeySlash", "/" }, { "KeyMinus", "-" }, { "KeyEqual", "=" }, { "KeyPageUp", "Page Up" },
        { "KeyPageDown", "Page Down" }, { "KeyScrollLock", "Scroll Lock" }, { "KeyNumlock", "Num Lock" }, { "KeySysrq", "Print Screen" },
    };
    if (auto It = Special.find(Key); It != Special.end()) return It->second;
    std::string S = Key.rfind("Key", 0) == 0 ? Key.substr(3) : Key;
    if (S.rfind("Kp", 0) == 0 && S.size() > 2) return "Keypad " + SplitCamel(S.substr(2));
    return SplitCamel(S);
}

std::string GamepadLabel(const std::string &B)
{
    static const std::map<std::string, std::string> Names = {
        { "South", "A" }, { "East", "B" }, { "West", "X" }, { "North", "Y" }, { "DPadUp", "D-pad up" }, { "DPadDown", "D-pad down" },
        { "DPadLeft", "D-pad left" }, { "DPadRight", "D-pad right" }, { "LeftBumper", "Left bumper" }, { "RightBumper", "Right bumper" },
        { "LeftTrigger", "Left trigger" }, { "RightTrigger", "Right trigger" }, { "Start", "Menu" }, { "Select", "View" },
        { "LeftStick", "Left stick click" }, { "RightStick", "Right stick click" }, { "LeftPaddle1", "Left paddle 1" },
        { "LeftPaddle2", "Left paddle 2" }, { "RightPaddle1", "Right paddle 1" }, { "RightPaddle2", "Right paddle 2" },
    };
    if (auto It = Names.find(B); It != Names.end()) return It->second;
    return SplitCamel(B);
}

std::string MouseLabel(const std::string &B)
{
    static const std::map<std::string, std::string> Names = {
        { "Left", "Left button" }, { "Right", "Right button" }, { "Middle", "Middle button" }, { "WheelUp", "Wheel up" },
        { "WheelDown", "Wheel down" }, { "WheelLeft", "Wheel left" }, { "WheelRight", "Wheel right" }, { "Extra1", "Back button" },
        { "Extra2", "Forward button" },
    };
    if (auto It = Names.find(B); It != Names.end()) return It->second;
    return B;
}

std::string Check(const Binding &B)
{
    const Control *C = FindControl(B.Control);
    if (!C) return "no control '" + B.Control + "'";
    switch (C->K)
    {
    case Kind::Gyro: return "InputPlumber does not map the gyro yet";
    case Kind::Trackpad: return "a trackpad is always the pointer: bind its click or its touch";
    case Kind::Stick:
        if (B.Mode == "joystick" || B.Mode == "dpad") return {};
        if (B.Mode == "mouse") return B.Speed > 0 && B.Speed <= 20000 ? std::string() : "a mouse speed is 1 to 20000 pixels a second";
        if (B.Mode == "keys")
        {
            if (B.Keys.size() != 4) return "a stick as keys needs four keys: up, down, left, right";
            for (const std::string &K : B.Keys) if (!In(KeyboardKeys(), K)) return "no key '" + K + "'";
            return {};
        }
        return "a stick is a joystick, the mouse, keys or the D-pad — not '" + B.Mode + "'";
    case Kind::Button:
    case Kind::Trigger:
        if (B.Mode == "nothing") return B.Actions.empty() ? std::string() : "'nothing' presses nothing";
        if (!B.Mode.empty()) return "a button has no mode '" + B.Mode + "'";
        for (const Action &A : B.Actions)
            switch (A.T)
            {
            case Action::Type::Gamepad: if (!In(GamepadButtons(), A.Value)) return "no pad button '" + A.Value + "'"; break;
            case Action::Type::Keyboard: if (!In(KeyboardKeys(), A.Value)) return "no key '" + A.Value + "'"; break;
            case Action::Type::Mouse:
                if (C->K == Kind::Trigger) return "a trigger cannot press a mouse button (InputPlumber does not translate it): use a bumper";
                if (!In(MouseButtons(), A.Value)) return "no mouse button '" + A.Value + "'";
                break;
            case Action::Type::Overlay:
                if (C->K == Kind::Trigger) return "a trigger cannot open the overlay: use a button";
                break;
            case Action::Type::Nothing: return "'nothing' is the mode, not an action";
            }
        return {};
    }
    return {};
}

const Binding *Layout::find(const std::string &Id) const
{
    for (const Binding &B : Bindings) if (B.Control == Id) return &B;
    return nullptr;
}

void Layout::set(const Binding &B)
{
    Bindings.erase(std::remove_if(Bindings.begin(), Bindings.end(), [&](const Binding &X) { return X.Control == B.Control; }), Bindings.end());
    const bool AsIs = (B.Mode.empty() && B.Actions.empty()) || B.Mode == "joystick";
    if (!AsIs) Bindings.push_back(B);
    //Controls() order, so a file reads like the controller.
    auto Pos = [](const std::string &Id) {
        const auto &C = Controls();
        return std::find_if(C.begin(), C.end(), [&](const Control &X) { return X.Id == Id; }) - C.begin();
    };
    std::stable_sort(Bindings.begin(), Bindings.end(), [&](const Binding &L, const Binding &R) { return Pos(L.Control) < Pos(R.Control); });
}

std::string Layout::summary(const std::string &Id) const
{
    const Control *C = FindControl(Id);
    if (C && C->K == Kind::Gyro) return "Not available";
    if (C && C->K == Kind::Trackpad) return "Pointer";
    const Binding *B = find(Id);
    if (!B) return C && C->K == Kind::Stick ? "Joystick" : "As is";
    if (B->Mode == "nothing") return "Nothing";
    if (B->Mode == "mouse") return "Mouse";
    if (B->Mode == "dpad") return "D-pad";
    if (B->Mode == "keys")
    {
        std::string S = "Keys";
        for (const std::string &K : B->Keys) S += " " + KeyLabel(K);
        return S;
    }
    std::string S;
    for (const Action &A : B->Actions)
    {
        if (!S.empty()) S += " + ";
        switch (A.T)
        {
        case Action::Type::Gamepad: S += "Pad " + GamepadLabel(A.Value); break;
        case Action::Type::Keyboard: S += "Key " + KeyLabel(A.Value); break;
        case Action::Type::Mouse: S += "Mouse " + MouseLabel(A.Value); break;
        case Action::Type::Overlay: S += "Overlay"; break;
        case Action::Type::Nothing: break;
        }
    }
    return S.empty() ? "As is" : S;
}

ojson Layout::toJson() const
{
    ojson Bs = ojson::array();
    for (const Binding &B : Bindings)
    {
        ojson J = { { "control", B.Control } };
        if (!B.Mode.empty()) J["mode"] = B.Mode;
        if (!B.Actions.empty())
        {
            ojson As = ojson::array();
            for (const Action &A : B.Actions) As.push_back(A.T == Action::Type::Overlay ? ojson{ { "type", "overlay" } } : ojson{ { "type", TypeName(A.T) }, { "value", A.Value } });
            J["actions"] = As;
        }
        if (B.Mode == "keys") J["keys"] = B.Keys;
        if (B.Mode == "mouse") J["speed"] = B.Speed;
        Bs.push_back(J);
    }
    return ojson{ { "name", Name }, { "description", Description }, { "bindings", Bs } };
}

std::optional<Layout> Layout::FromJson(const ojson &J, std::string *Error)
{
    auto Fail = [&](const std::string &E) { if (Error) *Error = E; return std::optional<Layout>(); };
    if (!J.is_object() || !J.contains("name") || !J["name"].is_string()) return Fail("a layout needs a name");
    Layout L;
    L.Name = J["name"].get<std::string>();
    L.Description = J.value("description", std::string());
    if (J.contains("bindings") && !J["bindings"].is_array()) return Fail("bindings is a list");
    for (const ojson &Bj : J.value("bindings", ojson::array()))
    {
        if (!Bj.is_object() || !Bj.contains("control") || !Bj["control"].is_string()) return Fail("a binding names its control");
        Binding B;
        B.Control = Bj["control"].get<std::string>();
        B.Mode = Bj.value("mode", std::string());
        for (const ojson &Aj : Bj.value("actions", ojson::array()))
        {
            const auto T = Aj.is_object() ? TypeFrom(Aj.value("type", std::string())) : std::nullopt;
            if (!T) return Fail(B.Control + ": an action is gamepad, keyboard, mouse or overlay");
            B.Actions.push_back({ *T, Aj.value("value", std::string()) });
        }
        if (Bj.contains("keys") && Bj["keys"].is_array())
            for (const ojson &K : Bj["keys"]) if (K.is_string()) B.Keys.push_back(K.get<std::string>());
        if (Bj.contains("speed") && Bj["speed"].is_number()) B.Speed = Bj["speed"].get<double>();
        if (const std::string E = Check(B); !E.empty()) return Fail(B.Control + ": " + E);
        L.set(B);
    }
    return L;
}

ojson Layout::toProfile() const
{
    ojson Map = ojson::array();
    for (const Binding &B : Bindings)
    {
        const Control *C = FindControl(B.Control);
        if (!C || !Check(B).empty()) continue;
        if (C->K == Kind::Stick)
        {
            auto Axis = [&](const char *Dir) { return ojson{ { "gamepad", { { "axis", { { "name", C->Ip }, { "direction", Dir } } } } } }; };
            if (B.Mode == "mouse")
                for (const char *D : { "horizontal", "vertical" })
                    Map.push_back({ { "name", C->Label + " " + D }, { "source_event", Axis(D) },
                                    { "target_events", ojson::array({ { { "mouse", { { "motion", { { "direction", D }, { "speed_pps", static_cast<long long>(std::llround(B.Speed)) } } } } } } }) } });
            else if (B.Mode == "keys" || B.Mode == "dpad")
            {
                static const char *Dpad[4] = { "DPadUp", "DPadDown", "DPadLeft", "DPadRight" };
                for (int I = 0; I < 4; ++I)
                    Map.push_back({ { "name", C->Label + " " + StickDirs[I] }, { "source_event", Axis(StickDirs[I]) },
                                    { "target_events", ojson::array({ B.Mode == "keys" ? ojson{ { "keyboard", B.Keys[I] } }
                                                                                       : ojson{ { "gamepad", { { "button", Dpad[I] } } } } }) } });
            }
            continue;
        }
        //A trackpad's click or touch is a touchpad event ("LeftPad/Press"), not a pad button, to InputPlumber.
        const size_t Slash = C->Ip.find('/');
        const ojson Source = C->K == Kind::Trigger ? ojson{ { "gamepad", { { "trigger", { { "name", C->Ip } } } } } }
                           : Slash != std::string::npos
                               ? ojson{ { "touchpad", { { "name", C->Ip.substr(0, Slash) }, { "touch", { { "button", C->Ip.substr(Slash + 1) } } } } } }
                               : ojson{ { "gamepad", { { "button", C->Ip } } } };
        ojson Targets = ojson::array();
        for (const Action &A : B.Actions)
            switch (A.T)
            {
            case Action::Type::Gamepad: Targets.push_back({ { "gamepad", { { "button", A.Value } } } }); break;
            case Action::Type::Keyboard: Targets.push_back({ { "keyboard", A.Value } }); break;
            case Action::Type::Mouse: Targets.push_back({ { "mouse", { { "button", A.Value } } } }); break;
            case Action::Type::Overlay: Targets.push_back({ { "dbus", "ui_guide" } }); break;
            case Action::Type::Nothing: break;
            }
        Map.push_back({ { "name", C->Label }, { "source_event", Source }, { "target_events", Targets } });
    }
    //A handheld's volume keys (on InputPlumber's keyboard source) reach VidyaGod, which sets the volume
    //(VolumeKeys): a D-Bus event goes to VidyaGod whatever the intercept, a game running or not. Only in VidyaGod's
    //session: on a desktop, the desktop answers them.
    for (const auto &[Key, Event] : { std::pair{ "KeyVolumeUp", "ui_volume_up" }, std::pair{ "KeyVolumeDown", "ui_volume_down" },
                                     std::pair{ "KeyMute", "ui_volume_mute" } })
        if (VolumeKeysToVidyaGod()) Map.push_back({ { "name", std::string("Volume ") + Event }, { "source_event", { { "keyboard", Key } } },
                        { "target_events", ojson::array({ { { "dbus", Event } } }) } });
    ojson P = { { "version", 1 }, { "kind", "DeviceProfile" }, { "name", Name } };
    if (!Description.empty()) P["description"] = Description;
    P["mapping"] = Map;
    return P;
}

namespace { std::atomic<bool> VolumeRouted{false}; }
void SetVolumeKeysToVidyaGod(bool On) { VolumeRouted = On; }
bool VolumeKeysToVidyaGod() { return VolumeRouted; }

const std::vector<Layout> &Templates()
{
    static const std::vector<Layout> V = [] {
        auto Key = [](const char *K) { return Action{ Action::Type::Keyboard, K }; };
        auto Mouse = [](const char *M) { return Action{ Action::Type::Mouse, M }; };
        auto Btn = [](const char *Id, std::vector<Action> A) { return Binding{ Id, "", std::move(A), {}, 800 }; };
        std::vector<Layout> T;
        T.push_back({ "Gamepad", "Every control does what the controller does.", {} });

        Layout W{ "Keyboard and mouse (WASD)",
                  "For games without controller support: the left stick walks (W A S D), the right stick aims the mouse, the "
                  "bumpers are the mouse buttons (InputPlumber cannot press mouse buttons with the triggers).", {} };
        W.set({ "left_stick", "keys", {}, { "KeyW", "KeyS", "KeyA", "KeyD" }, 800 });
        W.set({ "right_stick", "mouse", {}, {}, 1200 });
        W.set(Btn("a", { Key("KeySpace") }));
        W.set(Btn("b", { Key("KeyC") }));
        W.set(Btn("x", { Key("KeyR") }));
        W.set(Btn("y", { Key("KeyE") }));
        W.set(Btn("dpad_up", { Key("Key1") }));
        W.set(Btn("dpad_right", { Key("Key2") }));
        W.set(Btn("dpad_down", { Key("Key3") }));
        W.set(Btn("dpad_left", { Key("Key4") }));
        W.set(Btn("lb", { Mouse("Right") }));
        W.set(Btn("rb", { Mouse("Left") }));
        W.set(Btn("lt", { Key("KeyLeftShift") }));
        W.set(Btn("rt", { Key("KeyF") }));
        W.set(Btn("l3", { Key("KeyLeftCtrl") }));
        W.set(Btn("r3", { Key("KeyV") }));
        W.set(Btn("start", { Key("KeyEsc") }));
        W.set(Btn("select", { Key("KeyTab") }));
        W.set(Btn("right_pad_press", { Mouse("Left") }));
        W.set(Btn("left_pad_press", { Mouse("Right") }));
        T.push_back(W);

        Layout D{ "Desktop", "Programs and menus: the right stick is the mouse, A clicks, B is Escape, the D-pad and left stick "
                             "are the arrow keys.", {} };
        D.set({ "right_stick", "mouse", {}, {}, 1000 });
        D.set({ "left_stick", "keys", {}, { "KeyUp", "KeyDown", "KeyLeft", "KeyRight" }, 800 });
        D.set(Btn("a", { Mouse("Left") }));
        D.set(Btn("x", { Mouse("Right") }));
        D.set(Btn("b", { Key("KeyEsc") }));
        D.set(Btn("y", { Key("KeyEnter") }));
        D.set(Btn("dpad_up", { Key("KeyUp") }));
        D.set(Btn("dpad_down", { Key("KeyDown") }));
        D.set(Btn("dpad_left", { Key("KeyLeft") }));
        D.set(Btn("dpad_right", { Key("KeyRight") }));
        D.set(Btn("lb", { Mouse("WheelUp") }));
        D.set(Btn("rb", { Mouse("WheelDown") }));
        D.set(Btn("start", { Key("KeyEnter") }));
        D.set(Btn("select", { Key("KeyTab") }));
        D.set(Btn("right_pad_press", { Mouse("Left") }));
        D.set(Btn("left_pad_press", { Mouse("Right") }));
        T.push_back(D);
        return T;
    }();
    return V;
}

bool ValidName(const std::string &N)
{
    if (N.empty() || N.size() > 64 || N[0] == '.' || N == "games") return false;
    for (const unsigned char Ch : N) if (Ch < 0x20 || Ch == '/' || Ch == '\\' || Ch == 0x7f) return false;
    return true;
}

namespace
{
bool IsTemplate(const std::string &N)
{
    for (const Layout &T : Templates()) if (T.Name == N) return true;
    return false;
}

std::optional<ojson> ReadJson(const fs::path &P)
{
    std::ifstream In(P);
    if (!In) return std::nullopt;
    ojson J = ojson::parse(In, nullptr, false);
    if (J.is_discarded()) return std::nullopt;
    return J;
}

bool WriteJson(const fs::path &P, const ojson &J)
{
    std::error_code Ec;
    fs::create_directories(P.parent_path(), Ec);
    const fs::path Tmp = P.string() + ".tmp";
    {
        std::ofstream Out(Tmp, std::ios::trunc);
        if (!Out) return false;
        Out << J.dump(2) << "\n";
        if (!Out) return false;
    }
    fs::rename(Tmp, P, Ec);
    return !Ec;
}
}

std::vector<Layout> Saved(const std::string &Dir)
{
    std::vector<Layout> Out;
    std::error_code Ec;
    for (const auto &E : fs::directory_iterator(Dir, Ec))
    {
        if (!E.is_regular_file() || E.path().extension() != ".json" || E.path().stem() == "games") continue;
        if (auto J = ReadJson(E.path()))
            if (auto L = Layout::FromJson(*J); L && L->Name == E.path().stem().string()) Out.push_back(*L);
    }
    std::sort(Out.begin(), Out.end(), [](const Layout &A, const Layout &B) { return A.Name < B.Name; });
    return Out;
}

bool Save(const std::string &Dir, const Layout &L, std::string *Error)
{
    auto Fail = [&](const std::string &E) { if (Error) *Error = E; return false; };
    if (!ValidName(L.Name)) return Fail("a layout's name is its file name: no slashes, no leading dot, at most 64 characters");
    if (IsTemplate(L.Name)) return Fail("'" + L.Name + "' is one of VidyaGod's own layouts: save yours under another name");
    for (const Binding &B : L.Bindings) if (const std::string E = Check(B); !E.empty()) return Fail(B.Control + ": " + E);
    if (!WriteJson(fs::path(Dir) / (L.Name + ".json"), L.toJson())) return Fail("cannot write in " + Dir);
    return true;
}

bool Remove(const std::string &Dir, const std::string &Name)
{
    if (!ValidName(Name) || IsTemplate(Name)) return false;
    std::error_code Ec;
    return fs::remove(fs::path(Dir) / (Name + ".json"), Ec);
}

std::optional<Layout> Named(const std::string &Dir, const std::string &Name)
{
    for (const Layout &T : Templates()) if (T.Name == Name) return T;
    if (!ValidName(Name)) return std::nullopt;
    if (auto J = ReadJson(fs::path(Dir) / (Name + ".json"))) return Layout::FromJson(*J);
    return std::nullopt;
}

std::string ForGame(const std::string &Dir, const std::string &GameKey)
{
    if (auto J = ReadJson(fs::path(Dir) / "games.json"); J && J->is_object() && J->contains(GameKey) && (*J)[GameKey].is_string())
        return (*J)[GameKey].get<std::string>();
    return {};
}

std::map<std::string, std::string> Games(const std::string &Dir)
{
    std::map<std::string, std::string> Out;
    if (auto J = ReadJson(fs::path(Dir) / "games.json"); J && J->is_object())
        for (auto It = J->begin(); It != J->end(); ++It) if (It.value().is_string()) Out[It.key()] = It.value().get<std::string>();
    return Out;
}

bool SetForGame(const std::string &Dir, const std::string &GameKey, const std::string &Name)
{
    if (GameKey.empty()) return false;
    ojson J = ReadJson(fs::path(Dir) / "games.json").value_or(ojson::object());
    if (!J.is_object()) J = ojson::object();
    if (Name.empty()) J.erase(GameKey);
    else J[GameKey] = Name;
    return WriteJson(fs::path(Dir) / "games.json", J);
}

}
