#ifndef COMMAND_H
#define COMMAND_H

#include <nlohmann/json.hpp>
#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <memory>
#include <string>
#include <vector>

class AppModel;
struct NodeIndex;
class QDir;
class QWidget;

// ---------------------------------------------------------------------------
// Command — everything the program can do, and everything it shows, from the command line (standing rule: every
// function of every module is drivable from the CLI, by the same code path the GUI takes).
//
//   VidyaGod friends share <peer> <library>       git-style: group, command, positionals, --options
//   VidyaGod help [group [command]]               help at every level, with examples
//   VidyaGod … --json                             the exact result as JSON (default: tables for people)
//
// A command runs where the app is. With a GUI up on this data dir, the call goes to it over its command socket
// (CommandServer) and runs on its main thread against its live model and windows. With none, it runs in this
// process: headless, with the model (and the windows, offscreen, for UI commands) brought up for the call.
//
// Each command names the functions it COVERS ("AppModel::shareLibraryWithFriend"). tools/cli_coverage.py holds
// every function of every module against those names; a function that cannot be a command is listed there with
// its reason. Nothing is uncovered silently.
// ---------------------------------------------------------------------------
namespace Command
{

// What a command runs against: the config, the model, the windows (Ui implies the model) — brought up for a call run on
// its own — or App: what only the running app holds (its editor, an authoring session, its live events, a runtime it
// keeps up). An App command run with no app up says so at once instead of bringing up windows that cannot have it.
enum class Needs : std::uint8_t { Config, Model, Ui, App };
enum class Type : std::uint8_t { String, Bool, Int, Json };

struct Context
{
    nlohmann::ordered_json *Config = nullptr;
    QDir *AppDataDir = nullptr;
    AppModel *Model = nullptr;            // set for Needs::Model and Needs::Ui
    QWidget *Window = nullptr;            // the main window, for Needs::Ui
    bool Live = false;                    // running inside the user's GUI (not a headless bring-up)
    //The index snapshot a Background command reads (taken on the app's thread before it runs off it); else empty.
    std::shared_ptr<const NodeIndex> Snapshot;
    //Answers the call now with this result, though the command has not returned: an action that opened a dialog
    //(whose exec() holds the command until someone answers it) reports the dialog at once. Its eventual return is then
    //dropped. Unset where nothing can be answered early.
    std::function<void(const nlohmann::ordered_json &Result)> Early;
};

// One argument. A positional one is given in order (`share <peer> <library>`); the rest are --options (a Bool one is
// a flag: --on / --no-on). Every argument reaches the command as a field of one JSON object, named by Name.
struct Arg
{
    std::string Name;
    Type Kind = Type::String;
    std::string Help;
    bool Positional = false;
    bool Required = true;                 // positionals are always required; an option is optional unless said
    std::vector<std::string> Choices;     // the values a String may take (empty: any)
    //A Json one: a one-line sample of what it takes, shown in its help. Its first character says the shape the value
    //must have ('{' an object, '[' an array; anything else: any), checked before the command runs.
    std::string Sample;
};

// A Json option's sample (see Arg::Sample): Opt("patch", "the patch", Type::Json, true) | Sample("{\"MODE\": …}").
inline Arg WithSample(Arg A, std::string S) { A.Sample = std::move(S); return A; }

// The three kinds, spelled at the registration: Pos("peer", "…"), Opt("depth", "…", Type::Int), Flag("all", "…").
inline Arg Pos(std::string Name, std::string Help, Type K = Type::String, std::vector<std::string> Choices = {})
{ return Arg{std::move(Name), K, std::move(Help), true, true, std::move(Choices), {}}; }
inline Arg Opt(std::string Name, std::string Help, Type K = Type::String, bool Required = false, std::vector<std::string> Choices = {})
{ return Arg{std::move(Name), K, std::move(Help), false, Required, std::move(Choices), {}}; }
// A positional that may be left out (only after the required ones): `launch plan [<game>]`.
inline Arg OptPos(std::string Name, std::string Help)
{ return Arg{std::move(Name), Type::String, std::move(Help), true, false, {}, {}}; }
inline Arg Flag(std::string Name, std::string Help)
{ return Arg{std::move(Name), Type::Bool, std::move(Help), false, false, {}, {}}; }

using Fn = std::function<nlohmann::ordered_json(Context &, const nlohmann::ordered_json &Args)>;
// How a result reads for a person (default: Render::Generic). Colour already decided by the caller.
using Renderer = std::function<std::string(const nlohmann::ordered_json &Result, bool Colour)>;
// A Node command that also works with no node (networking off): what it needs the network for is only a fallback —
// a launch fetches sources only when some are missing, as Play does offline; the node's status reports "off".
bool WorksOffline(const std::string &Name);

// -v on the command line: a renderer that sums up (repeated warnings, one line per kind) lists everything instead.
void SetVerboseOutput(bool On);
bool VerboseOutput();

struct Spec
{
    std::string Name;                     // its words: "friends share"
    std::string Summary;                  // one line, for the group's listing
    std::string Help;                     // what it does, in full (may be several lines)
    std::vector<Arg> Args;
    std::vector<std::string> Examples;    // whole command lines, after "VidyaGod "
    std::vector<std::string> Covers;      // the functions it drives, qualified ("AppModel::shareLibraryWithFriend")
    Needs Requires = Needs::Config;
    bool Node = false;                    // needs the IPFS node up (started for a headless run)
    Fn Run;
    Renderer Render;                      // empty: the generic table/fields rendering
    //In the running app, run it on a worker thread, so a long one leaves the window (and other commands) answering:
    //for a command that reads nothing but its arguments and the index snapshot (CmdUtil::Index) it is handed.
    bool Background = false;
};

// An argument error: the caller's mistake, reported with the command's usage (exit 2), never a crash.
struct BadArgs : std::runtime_error { using std::runtime_error::runtime_error; };

void Register(Spec S);
void RegisterGroup(const std::string &Name, const std::string &Summary);
const std::map<std::string, Spec> &All();
const std::map<std::string, std::string> &Groups();
const Spec *Find(const std::string &Name);

// Run one command: {"ok": true, "result": …} or {"ok": false, "error": "…", "usage": bool}. Never throws.
nlohmann::ordered_json Run(const std::string &Name, const nlohmann::ordered_json &Args, Context &Ctx);

// Every command as JSON (name, summary, help, args, examples, covers, needs): `VidyaGod commands --json`.
nlohmann::ordered_json List();

// The one of Of a mistyped W most likely meant (a few edits away), or "": for "did you mean".
std::string Nearest(const std::string &W, const std::vector<std::string> &Of);

// Argument readers for a command body: missing or mistyped is BadArgs naming it.
std::string Str(const nlohmann::ordered_json &Args, const char *Key);
std::string Str(const nlohmann::ordered_json &Args, const char *Key, const std::string &Default);
bool Bool(const nlohmann::ordered_json &Args, const char *Key);
bool Bool(const nlohmann::ordered_json &Args, const char *Key, bool Default);
long long Int(const nlohmann::ordered_json &Args, const char *Key);
long long Int(const nlohmann::ordered_json &Args, const char *Key, long long Default);

// Registers at static initialisation: `VG_COMMANDS { Command::Register({…}); … }` in a commands TU. A registration
// that fails (two commands of one name) is a programming error: it stops the program at start, naming it.
struct Registrar { explicit Registrar(void (*F)()) noexcept; };

} // namespace Command

#define VG_COMMAND_CAT2(A, B) A##B
#define VG_COMMAND_CAT(A, B) VG_COMMAND_CAT2(A, B)
#define VG_COMMANDS                                                                                           \
    static void VG_COMMAND_CAT(VgRegister_, __LINE__)();                                                      \
    static const Command::Registrar VG_COMMAND_CAT(VgRegistrar_, __LINE__)(&VG_COMMAND_CAT(VgRegister_, __LINE__)); \
    static void VG_COMMAND_CAT(VgRegister_, __LINE__)()

#endif // COMMAND_H
