#ifndef CMDUTIL_H
#define CMDUTIL_H

// What many commands share: saving the config the way the app does, the node graph to act on, and naming a package,
// a node or a list of grafts on the command line.
#include "command.h"
#include "packagecatalog.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

class PackageEditorModel;

namespace CmdUtil
{
using json = nlohmann::ordered_json;

// Persist the config: through the app's model when there is one (it owns the file and its signals), else the file.
void Save(Command::Context &C);
// The node graph: the app's own snapshot when it is up, else one scanned from disk for this call.
std::shared_ptr<const NodeIndex> Index(Command::Context &C);
// A node by CID, working handle or LABEL — BadArgs when none (never null).
const Node *NodeOf(const NodeIndex &Idx, const std::string &Id);
// A package folder named on the command line: a path, its folder name, its UID ("749" finds "[749][v1.0] …"), or a
// unique part of its title — in the library, or (with Received) among a friend's received packages too.
std::filesystem::path PackageDir(Command::Context &C, const std::string &Q, bool Received = true);
// "a,b,c" or a JSON array, as a list of strings ("" / absent: none).
std::vector<std::string> List(const json &Args, const char *Key);
// A list of paths: the option given once per path (--base a --base b), or one value. One value that names a file is
// that file, commas and all; otherwise a,b,c as List reads it.
std::vector<std::string> PathList(const json &Args, const char *Key);
// A number given as text, all of it ("12.5", never "12abc") — BadArgs naming What when it is not one.
double Number(const std::string &V, const std::string &What);
// An "x,y" option: false when it is absent, BadArgs when it is not two numbers.
bool Point(const json &Args, const char *Key, double &X, double &Y);
// ---- build sessions: the package builder's document a command acts on (cmd_build, cmd_author) ----
// The open editor's model when it has the package open; else the app's session for it (kept until `build close`);
// else, run on its own, one for this call (Own holds it).
struct BuildSession
{
    std::unique_ptr<PackageEditorModel> Own;
    PackageEditorModel *M = nullptr;
    const char *Where = "";   // "editor", "session", "this call"
};
BuildSession OpenBuild(Command::Context &C, const std::string &Pkg);
std::map<std::string, std::unique_ptr<PackageEditorModel>> &BuildSessions();   // package path -> its session
std::string Canon(const std::filesystem::path &P);
// A path as the CLI shows it.
inline std::string P(const std::filesystem::path &X) { return X.string(); }
}

#endif // CMDUTIL_H
