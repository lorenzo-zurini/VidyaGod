#ifndef OPTIONREADBACK_H
#define OPTIONREADBACK_H

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

struct ContainerParams;

// OptionReadBack — launch options that go both ways (spec 08 §8.8). An option shown in the launch window that the
// package writes into a file the user keeps is also changed by the game, in its own settings menu; the game's file is
// then the truth. When the launch window opens (and before a command-line launch) the value is read back out of the
// user's saved copy and becomes the instance's value, so the window shows it and the launch writes it back unchanged.
//
// An option reads back when it is written by a keyed edit (ConfigWrite, JsonSet, XmlSet) whose VALUE is the option
// once, with literal text around it, in a format that can be undone (none, bool, winpath, or map:TABLE with distinct,
// literal entries and no WHEN); a value read must also be one the option can hold (an enum's choice, a key name, a
// number within MIN/MAX, text matching PATTERN). A hidden variable (no UI), a secret, an EVAL and anything else stay
// one-way. A per-launch override is not touched: it wins as before.
namespace OptionReadBack
{
//One place an option's value is written into a file: the option, the runtime-relative file, the op, and the VALUE
//template around the option's token.
struct Site
{
    std::string Option, File, Mode, Key, Prefix, Suffix, Format;
    bool IfUnsaved = false;   // the package's default only (no take-back): applied while the user has no saved copy
};
std::vector<Site> Sites(const ContainerParams &CP);

//What an option's saved files hold: the value (as the option holds it), and whether every place it is written is only
//a package default (the launcher never writes it once the user has a copy, so the file alone decides).
struct Reading
{
    std::string Value;
    bool DefaultOnly = false;
};
//The options that read back and what the user's saved files hold for them now. Left out: no saved copy yet, a value
//the game removed (what that means is the game's own default, which the package may not know), a value that does not
//undo or that the option cannot hold, two places disagreeing, a link or an outsized file where the copy should be.
std::map<std::string, Reading> Read(const ContainerParams &CP);

//Each readable option's value in this launch, as Read would report it (bool as 1/0, winpath with '/').
std::map<std::string, std::string> Current(const ContainerParams &CP);

//Takes what Read found into an instance's config (instance.json). LAST_IN_FILE is what each file last held as far as
//VidyaGod knows (what a launch wrote, or what was read back). A file holding something else was changed by the game,
//and that value becomes the option's saved value — unless the option was changed in the launcher too since then: the
//launcher's choice wins. A package-default-only option always follows its file. Returns the options whose saved value
//changed; Touched says whether the config changed at all.
std::vector<std::string> Adopt(nlohmann::ordered_json &InstanceConfig, const std::map<std::string, Reading> &Found,
                               const std::map<std::string, std::string> &Now, bool &Touched);

//After a launch's edits: each readable option whose value its file in the runtime now holds (the edit really wrote it).
//Committed to LAST_IN_FILE once the session has stored what it keeps (Commit), so a session that died before its
//capture does not make the next launch take the user's older saved value for a change made in the game.
std::map<std::string, std::string> Written(const ContainerParams &CP);
bool Commit(nlohmann::ordered_json &InstanceConfig, const std::map<std::string, std::string> &Written);
}

#endif // OPTIONREADBACK_H
