#ifndef PACKAGECATALOG_H
#define PACKAGECATALOG_H

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <utility>

#include "manifestmodel.h"   // Node / NodeIndex (node-graph catalog)

// ---------------------------------------------------------------------------
// PackageCatalog — the sharing service: where packages live on disk (the LIBRARY + git repos), the catalog of
// everything known, per-package user settings, and the import/publish/sync operations. All stateless statics
// over JSON (no launch session). Built on ManifestModel; uses IpfsWrapper for content transfer.
//
// (Runner install — ImportRunner — stays in ContainerWrapper: it needs the launch engine's mount + wineboot
// machinery to generate a DEFPREFIX, so it lives with the code that owns prefix-building.)
// ---------------------------------------------------------------------------
namespace PackageCatalog {

// ----- per-package user settings (LIBRARY[i]["USERSETTINGS"]) -----
nlohmann::ordered_json GetPackageUserSettings(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &PackageUID);
void SetPackageUserSetting(nlohmann::ordered_json &GlobalConfigJSON, const std::string &PackageUID, const std::string &Key, const nlohmann::ordered_json &Value);

// ----- on-disk locations -----
// The managed library root (repos clone here, one subfolder per repo): Settings.Paths.LibraryRoot or ~/.VidyaGod/LIBRARY.
std::string LibraryRootDir(const nlohmann::ordered_json &GlobalConfigJSON);
// The clone directories of every configured Settings.Repositories[] git repo (indexed in order).
std::vector<std::string> RepositoryDirs(const nlohmann::ordered_json &GlobalConfigJSON);

// ----- sync / publish -----
// Git clone/pull each repo into LIBRARY/<repo> and upsert one LIBRARY index entry per bundle (identity derived
// from its node files), reconciling away vanished repo entries. Mutates GlobalConfigJSON; caller persists it.
void SyncRepositories(nlohmann::ordered_json &GlobalConfigJSON);
// Dehydrate a local bundle for sharing: seed each node LAYER's VFS content + META.COVER over IPFS, record
// SOURCE:{ipfs,CID} into the node files IN PLACE (content kept), and optionally export a node-files-only copy.
bool PublishPackage(const std::string &PackageDir, const std::string &DehydratedDestDir, std::string *Error = nullptr);
// Copy a package's dehydrated manifest (top-level *.json only, no content/images) into DestDir. Returns count copied.
int MirrorDehydrated(const std::string &SrcDir, const std::string &DestDir);

// ----- node-graph catalog (everything-is-a-node) -----
// Build the global cross-bundle node graph from the configured repos — the node-native catalog source.
NodeIndex BuildCatalogIndex(const nlohmann::ordered_json &GlobalConfigJSON);
// Presentable launchable nodes (ROLE:"launchable" + META) grouped by GROUP for library tiles: one inner vector per
// tile (the group's editions, RECOMMENDED first then by id). Groups are ordered by the recommended edition's title.
std::vector<std::vector<const Node*>> PresentableGroups(const NodeIndex &Idx);
// Runner nodes that can serve a launchable on this machine (GUEST ∋ launch host, HOST==machine, executable
// available), in sorted node-id order — for the prelaunch runner dropdown.
std::vector<const Node*> RunnerCandidates(const NodeIndex &Idx, const Node &Launch);
// True when every VFS layer in a launchable node's content closure is present locally (its game is installed).
bool NodeHydrated(const NodeIndex &Idx, const std::string &LaunchNodeId);
// Every distinct ipfs CID a launchable node's content closure must fetch (its layers' SOURCE CIDs). Drives the
// Available-tab download-progress aggregation. Excludes the runner build; cover CIDs are not included.
std::vector<std::string> NodeContentCids(const NodeIndex &Idx, const std::string &LaunchNodeId);
// Fetch (IPFS) every missing VFS layer in a launchable node's content closure + its cover, in place at each node's
// bundle PATH. Worker-thread (may block on downloads). Returns false (with *Error) on a failed fetch.
bool HydrateNode(const NodeIndex &Idx, const std::string &LaunchNodeId, std::string *Error = nullptr);

} // namespace PackageCatalog

#endif // PACKAGECATALOG_H
