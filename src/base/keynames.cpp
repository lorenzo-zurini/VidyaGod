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
        "mouse left", "mouse right", "mouse middle", "mouse 4", "mouse 5", "mouse wheel up", "mouse wheel down",
        "pad a", "pad b", "pad x", "pad y", "pad back", "pad guide", "pad start", "pad left stick", "pad right stick",
        "pad left shoulder", "pad right shoulder", "pad left trigger", "pad right trigger",
        "pad left stick up", "pad left stick down", "pad left stick left", "pad left stick right",
        "pad right stick up", "pad right stick down", "pad right stick left", "pad right stick right",
        "pad dpad up", "pad dpad down", "pad dpad left", "pad dpad right",
        "pad misc", "pad paddle 1", "pad paddle 2", "pad paddle 3", "pad paddle 4", "pad touchpad",
    };
    return Names;
}

namespace
{
const char *const ModNames[] = { "ctrl", "alt", "shift", "meta" };   // the order a combination spells them

//A name a modifier may be held with: a keyboard key or a mouse button, not a modifier and not a pad's.
bool Combinable(const std::string &Key)
{
    static const std::set<std::string> Known(KeyNames::All().begin(), KeyNames::All().end());
    return Known.count(Key) && Key.rfind("pad ", 0) != 0 && !KeyNames::IsModifier(Key);
}
}

bool KeyNames::IsModifier(const std::string &Key)
{
    static const std::set<std::string> Mods = { "left shift", "right shift", "left ctrl", "right ctrl",
                                                "left alt", "right alt", "left meta", "right meta" };
    return Mods.count(Key) > 0;
}

std::string KeyNames::Combine(unsigned Mods, const std::string &Key)
{
    if (!Combinable(Key)) return Key;
    std::string Out;
    for (unsigned I = 0; I < 4; ++I)
        if (Mods & (1u << I)) Out += std::string(ModNames[I]) + "+";
    return Out + Key;
}

bool KeyNames::IsValue(const std::string &Value)
{
    static const std::set<std::string> Known(All().begin(), All().end());
    if (Value.empty() || Known.count(Value)) return true;
    //A combination: modifiers in their order, each once, then a key that takes them. No name holds a '+'.
    size_t At = 0;
    unsigned Next = 0;
    bool Any = false;
    for (;;)
    {
        const size_t Plus = Value.find('+', At);
        if (Plus == std::string::npos) break;
        const std::string M = Value.substr(At, Plus - At);
        unsigned I = Next;
        while (I < 4 && M != ModNames[I]) ++I;
        if (I == 4) return false;                                  // not a modifier, or out of order, or twice
        Next = I + 1;
        Any = true;
        At = Plus + 1;
    }
    return Any && Combinable(Value.substr(At));
}
