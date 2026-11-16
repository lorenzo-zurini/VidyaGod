#ifndef KEYNAMES_H
#define KEYNAMES_H

#include <string>
#include <vector>

// KeyNames — the names a `key` option holds (spec 08 §8.3): what the launch window's press-a-key capture stores. How
// a game wants a key written is the package's data (its VARS and edits), never this list's.
namespace KeyNames
{
//Every name, in the order the spec lists them.
const std::vector<std::string> &All();

//A `key` option's value: a name from All(), a combination (below), or "" (unbound).
bool IsValue(const std::string &Value);

//Modifiers held with a key, side-less. A combination is spelled with them in this order, then the key, joined by '+':
//"shift+1", "ctrl+a", "ctrl+alt+shift+meta+f9". It takes a keyboard key or a mouse button (the wheel too), never a
//pad's, and never a modifier key: a modifier pressed alone binds by its side ("left shift").
enum Mod : unsigned { Ctrl = 1, Alt = 2, Shift = 4, Meta = 8 };

//Key with Mods held: the combination's name; Key itself when no modifier is held or it takes none.
std::string Combine(unsigned Mods, const std::string &Key);

//Whether Key is a modifier's own name ("left shift" … "right meta").
bool IsModifier(const std::string &Key);
}

#endif // KEYNAMES_H
