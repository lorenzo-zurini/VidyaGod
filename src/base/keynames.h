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

//A `key` option's value: a name from All(), or "" (unbound).
bool IsValue(const std::string &Value);
}

#endif // KEYNAMES_H
