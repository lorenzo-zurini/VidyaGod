#ifndef NODEFILES_H
#define NODEFILES_H

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// NodeFiles — the node files of a package folder (MetaPackageFormat chapter 02 §2.5). A node file is named by its
// LABEL, made a safe file name; when two nodes of a package claim one name, or a node claims a content file's name,
// each is named "<name> [<cid tail>].json". The name is presentation and depends only on the package's nodes; a
// node's identity is the CID of its bytes, never its file name.
// ---------------------------------------------------------------------------
namespace NodeFiles
{

//One file or folder name from any text — a label, or a peer's (untrusted) library, package or file name: ASCII
//letters, digits, space and _ - . [ ] ( ) kept, every other byte '_'; at most 120 bytes; never empty, "." or "..";
//no trailing '.' or ' ' and no Windows device name, so every platform holds the name as given.
std::string SafeName(const std::string &In);

//The file name the rule gives each node of one package: CID → "<name>.json". Package: CID → node.
std::map<std::string, std::string> Names(const std::map<std::string, nlohmann::ordered_json> &Package);

//A node file directly in a package folder. Frozen: its bytes are its canonical form (no "CID" handle, no POS) — what
//publishing writes and a peer receives; anything else is a working-tree draft.
struct File
{
    std::filesystem::path Path;
    nlohmann::ordered_json Node;
    bool Frozen = false;
};

//Every node file directly in Dir, by the CID of its node. A second file holding the same node (the same CID) is not
//listed; it goes to *Twins.
//Skip: files not even read (a caller that knows what they hold).
std::map<std::string, File> In(const std::filesystem::path &Dir, std::vector<std::filesystem::path> *Twins = nullptr,
                               const std::set<std::filesystem::path> *Skip = nullptr);

//Names Dir's frozen node files by the rule (over the frozen nodes: a draft is not published, and keeps its name):
//renamed in two steps, through a .json name, so swapped names never overwrite each other and a rename that stops
//half-way leaves a file the next pass finds. A twin (a second file of one node) is removed. *Moved gets each (old
//path, new path) — a twin's new path is where the file it repeated ends up — so the caller can move what refers to the
//old paths. A name taken by a file that is not a node file of the package leaves that node where it was, and says so
//in *Error (false).
//The name a rename parks a node file under on its way: a .json file, so a rename that stops half-way leaves one In() reads.
std::string ParkedName(const std::string &Cid);

bool Normalize(const std::filesystem::path &Dir, std::vector<std::pair<std::filesystem::path, std::filesystem::path>> *Moved,
               std::string *Error = nullptr);

}

#endif // NODEFILES_H
