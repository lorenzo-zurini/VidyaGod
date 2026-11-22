#pragma once

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Controller layouts — what each control of a controller does (Steam Input's "controller configuration"), turned into
// an InputPlumber device profile for VidyaGod's controller service. Only what InputPlumber can actually do is offered
// (its translations, src/input/event/value.rs in the InputPlumber submodule):
//   buttons  → a pad button, a keyboard key, a mouse button or the wheel, the overlay, or nothing (several = a chord)
//   triggers → as they are, a pad button, a keyboard key, or nothing (never a mouse button: not implemented there)
//   sticks   → a joystick (as is), the mouse (with a speed), four keys, or the D-pad
//   trackpads → always the pointer (InputPlumber's touchpad); their click and touch are buttons
//   gyro     → not yet (InputPlumber translates no gyro events)
// A control a layout does not name does what the controller does.
//
// Layouts are the user's: VidyaGod's own templates (Templates) plus those the user saves, one JSON file each in
// <data dir>/ControllerLayouts/; which game uses which is in games.json beside them.
// ---------------------------------------------------------------------------
namespace ControllerLayout
{
using ojson = nlohmann::ordered_json;

enum class Kind { Button, Trigger, Stick, Trackpad, Gyro };

// A control of the controller (a Steam Deck's, the superset; an Xbox pad has fewer).
struct Control
{
    std::string Id;      // stable key: "a", "dpad_up", "left_stick", "l4", "right_pad_press"
    std::string Label;   // as the controller labels it: "A", "D-pad up", "Left stick", "L4"
    std::string Group;   // "Buttons", "D-pad", "Bumpers and triggers", "Sticks", "Trackpads", "Back grips", "Menu buttons", "Gyro"
    Kind K = Kind::Button;
    std::string Ip;      // InputPlumber's name: a gamepad button ("South"), a trigger ("LeftTrigger"), an axis ("LeftStick"),
                         // a trackpad's click or touch ("LeftPad/Press": a touchpad event to InputPlumber)
};
const std::vector<Control> &Controls();
const Control *FindControl(const std::string &Id);

// One thing a button or trigger does.
struct Action
{
    enum class Type { Gamepad, Keyboard, Mouse, Overlay, Nothing };
    Type T = Type::Nothing;
    std::string Value;   // Gamepad: a button ("South", "DPadUp"); Keyboard: a key ("KeyW"); Mouse: "Left", "WheelUp"…
    bool operator==(const Action &) const = default;
};
std::string TypeName(Action::Type T);                 // "gamepad", "keyboard", "mouse", "overlay", "nothing"
std::optional<Action::Type> TypeFrom(const std::string &Name);

// What one control does. Buttons and triggers: Actions (empty with Mode "" = as it is). Sticks: Mode.
struct Binding
{
    std::string Control;                 // Control::Id
    std::string Mode;                    // sticks: "joystick" | "mouse" | "keys" | "dpad"; buttons/triggers: "" or "nothing"
    std::vector<Action> Actions;         // buttons, triggers: pressed together
    std::vector<std::string> Keys;       // stick "keys": up, down, left, right
    double Speed = 800;                  // stick "mouse": pixels per second at full tilt
    bool operator==(const Binding &) const = default;
};

struct Layout
{
    std::string Name, Description;
    std::vector<Binding> Bindings;       // in Controls() order; a control left out does what the controller does

    const Binding *find(const std::string &ControlId) const;
    void set(const Binding &B);          // replaces the control's binding (an "as is" one removes it)
    std::string summary(const std::string &ControlId) const;   // "Keyboard W", "Mouse", "W A S D", "As is"

    ojson toJson() const;                // VidyaGod's own file
    static std::optional<Layout> FromJson(const ojson &J, std::string *Error = nullptr);
    ojson toProfile() const;             // InputPlumber's DeviceProfile (JSON is YAML: InputPlumber reads it as is)
};

// Why a binding cannot be (""= it can): the control, mode, action types and names InputPlumber accepts for it.
std::string Check(const Binding &B);

// The choices the configurator offers.
const std::vector<std::string> &GamepadButtons();     // "South", "East", … as InputPlumber names them
const std::vector<std::string> &KeyboardKeys();       // "KeyEsc", "Key1", … (InputPlumber's names)
const std::vector<std::string> &MouseButtons();       // "Left", "Right", "Middle", "WheelUp", …
std::string KeyLabel(const std::string &Key);         // "KeyLeftCtrl" → "Left Ctrl", "KeyW" → "W"
std::string GamepadLabel(const std::string &Button);  // "South" → "A", "DPadUp" → "D-pad up"
std::string MouseLabel(const std::string &Button);    // "WheelUp" → "Wheel up"

// VidyaGod's own layouts, always there: "Gamepad" (everything as it is), "Keyboard and mouse (WASD)", "Desktop".
const std::vector<Layout> &Templates();

// The user's layouts, on disk under Dir (<data dir>/ControllerLayouts): saved by name, listed, removed, and which
// game uses which (games.json: {game key: layout name}). A name is a file name: no slashes, no leading dot.
std::vector<Layout> Saved(const std::string &Dir);
bool Save(const std::string &Dir, const Layout &L, std::string *Error = nullptr);
bool Remove(const std::string &Dir, const std::string &Name);
std::optional<Layout> Named(const std::string &Dir, const std::string &Name);   // a template or a saved one
std::string ForGame(const std::string &Dir, const std::string &GameKey);         // "" = none chosen
bool SetForGame(const std::string &Dir, const std::string &GameKey, const std::string &Name);   // "" clears
std::map<std::string, std::string> Games(const std::string &Dir);                              // every game's choice
bool ValidName(const std::string &Name);
}
