#ifndef CMDUTIL_H
#define CMDUTIL_H

// What many commands share: saving the config the way the app does, the node graph to act on, and naming a package,
// a node or a list of grafts on the command line.
#include "command.h"
#include "packagecatalog.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

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
// A path as the CLI shows it.
inline std::string P(const std::filesystem::path &X) { return X.string(); }
}

#endif // CMDUTIL_H
