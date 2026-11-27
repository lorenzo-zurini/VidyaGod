#ifndef PACKAGECATALOG_P_H
#define PACKAGECATALOG_P_H

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include <filesystem>
#include <mutex>
#include <optional>
#include <functional>
#include <set>
#include "manifestmodel.h"   // NodeIndex

// Internal (cross-TU) helpers of the PackageCatalog service, shared by its catalog_*.cpp files. Not its public API.
namespace PackageCatalog {

//The library root (Settings.Paths.LibraryRoot, else <data>/LIBRARY), and whether Path is at or under Base (lexical).
std::string LibraryDir(const nlohmann::ordered_json &GlobalConfigJSON);
bool PathUnder(const std::filesystem::path &Base, const std::filesystem::path &Path);
//One change of a package's folder at a time (a version's staging, applying, discarding; its trash), by the folder.
std::mutex &PackageLock(const std::filesystem::path &Dir);

//Removes a landed node file F (and its content claim) unless one of Elsewhere still holds that node.
void RemoveLandedNode(const std::filesystem::path &F, const std::string &Cid, const std::vector<std::filesystem::path> &Elsewhere = {});
//RemoveLandedNode of each, their references dropped together (one drop each was its own commit, unpins and compaction).
struct LandedNode { std::filesystem::path File; std::string Cid; std::vector<std::filesystem::path> Elsewhere; };
void RemoveLandedNodes(const std::vector<LandedNode> &Nodes);
//A package folder under a package source's dir (managed: Remove takes its content; never a local package).
bool PathUnderManagedSource(const nlohmann::ordered_json &GlobalConfigJSON, const std::filesystem::path &Path);

//What one bundle dir contains, per a cheap node scan: its first DeclareLibraryItem UID/name + whether any
//node is launchable / a runner. Valid=false when the dir holds no nodes at all.
struct BundleIdentity { std::string Uid, Name; bool HasLaunchable = false, HasRunner = false; bool Valid = false; };
BundleIdentity ScanBundleIdentity(const std::string &BundleDir);

//The content CIDs a node records for its own bytes — its content layers' SOURCE and its tiles' COVER SOURCE, in
//order, deduplicated. What a package's pin folder links (packagecatalog_publish.cpp).
std::vector<std::string> NodeContentCids(const nlohmann::ordered_json &Node);

//The installed nodes of Dir, among Candidates, that something still holds by a hard link (NODE or INTO) once Dir
//lists Listing (catalog_versions.cpp; exposed for its test).
std::vector<std::string> KeptNodes(const NodeIndex &Idx, const std::filesystem::path &Dir, const std::vector<std::string> &Listing,
                                   const std::set<std::string> &Candidates,
                                   const std::function<nlohmann::ordered_json(const std::string &)> &NodeAt);

} // namespace PackageCatalog

#endif // PACKAGECATALOG_P_H
