#include "keynames.h"

#include <set>

const std::vector<std::string> &KeyNames::All()
{
    static const std::vector<std::string> Names = {
        "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
        "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z",
        "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
        "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12",
        "space", "enter", "escape", "tab", "backspace",
        "insert", "delete", "home", "end", "page up", "page down",
        "up", "down", "left", "right",
        "left shift", "right shift", "left ctrl", "right ctrl", "left alt", "right alt", "left meta", "right meta", "menu",
        "caps lock", "num lock", "scroll lock", "print screen", "pause",
        "minus", "equals", "left bracket", "right bracket", "backslash", "semicolon", "quote", "backquote",
        "comma", "period", "slash",
        "keypad 0", "keypad 1", "keypad 2", "keypad 3", "keypad 4", "keypad 5", "keypad 6", "keypad 7", "keypad 8",
        "keypad 9", "keypad period", "keypad divide", "keypad multiply", "keypad minus", "keypad plus", "keypad enter",
        "mouse left", "mouse right", "mouse middle", "mouse 4", "mouse 5",
        "pad a", "pad b", "pad x", "pad y", "pad back", "pad guide", "pad start", "pad left stick", "pad right stick",
        "pad left shoulder", "pad right shoulder", "pad left trigger", "pad right trigger",
        "pad left stick up", "pad left stick down", "pad left stick left", "pad left stick right",
        "pad right stick up", "pad right stick down", "pad right stick left", "pad right stick right",
        "pad dpad up", "pad dpad down", "pad dpad left", "pad dpad right",
        "pad misc", "pad paddle 1", "pad paddle 2", "pad paddle 3", "pad paddle 4", "pad touchpad",
    };
    return Names;
}

bool KeyNames::IsValue(const std::string &Value)
{
    static const std::set<std::string> Known(All().begin(), All().end());
    return Value.empty() || Known.count(Value);
}
