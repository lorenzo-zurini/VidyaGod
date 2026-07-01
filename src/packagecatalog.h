#ifndef PACKAGECATALOG_H
#define PACKAGECATALOG_H

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>
#include <utility>
#include <functional>
#include <filesystem>

#include "manifestmodel.h"   // Node / NodeIndex (node-graph catalog)
#include "ipfswrapper.h"     // IpfsWrapper::FetchTarget (download collection)

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

// ----- locally-added (external) packages -----
// A LIBRARY entry is a "local package" when its PATH is a bundle dir OUTSIDE every repository dir (added via the
// Library's "Add Local Package", not cloned from a repo). The bundle dirs of every such entry whose PATH still exists
// — fed to BuildNodeIndex's ExtraBundleDirs so they're indexed alongside repo packages.
std::vector<std::filesystem::path> LocalPackageDirs(const nlohmann::ordered_json &GlobalConfigJSON);
// Drop LIBRARY entries for local packages whose bundle dir no longer exists (the user moved/deleted it). Repo-rooted
// entries are never touched (their content may just be un-hydrated). Mutates GlobalConfigJSON; returns count removed.
int PruneMovedLocalPackages(nlohmann::ordered_json &GlobalConfigJSON);

// ----- package sources by IPFS CID -----
// A package source is a `Settings.PackageSources[]` entry `{ "CID": "…", "NAME": "optional" }` — an IPFS folder CID of
// DEHYDRATED packages (manifests + covers, no content). Fetched into `<DataRoot>/CIDPACKAGES/<name>` (a sibling of
// LIBRARY), scanned as catalog roots, and hydrated on demand exactly like repo packages.
std::vector<std::string> PackageSourceDirs(const nlohmann::ordered_json &GlobalConfigJSON);   // existing source dirs
// True if BundleDir lives under a package-source dir (used to group them as "CID Packages" in the catalog).
bool IsPackageSourcePath(const nlohmann::ordered_json &GlobalConfigJSON, const std::filesystem::path &BundleDir);
// For each configured source: if its dir is empty/missing, recursively fetch the folder CID (dehydrated only — no
// content hydration; requires the IPFS node online), then upsert its bundles into LIBRARY. Mutates config; returns the
// number of packages indexed. On a fetch failure, sets *Error (if given) and leaves that source's dir untouched.
int SyncPackageSources(nlohmann::ordered_json &GlobalConfigJSON, std::string *Error = nullptr);
// Append a source (dedup on CID) — caller then SyncPackageSources + reindex + persists. Returns false if CID is empty
// or already present.
bool AddPackageSource(nlohmann::ordered_json &GlobalConfigJSON, const std::string &Cid, const std::string &Name);
// Remove source [index]: drop its config entry, delete its CIDPACKAGES dir, and drop LIBRARY entries under it.
void RemovePackageSource(nlohmann::ordered_json &GlobalConfigJSON, int Index);

// True if BundleDir is a locally-added package — its path is OUTSIDE every configured repository AND package-source dir
// (used to badge such tiles in the library).
bool IsLocalPackagePath(const nlohmann::ordered_json &GlobalConfigJSON, const std::filesystem::path &BundleDir);

// ----- sync / publish -----
// Git clone/pull each repo into LIBRARY/<repo> and upsert one LIBRARY index entry per bundle (identity derived
// from its node files), reconciling away vanished repo entries. Mutates GlobalConfigJSON; caller persists it.
void SyncRepositories(nlohmann::ordered_json &GlobalConfigJSON);
// Dehydrate a local bundle for sharing: seed each node LAYER's VFS content + META.COVER over IPFS, record
// SOURCE:{ipfs,CID} into the node files IN PLACE (content kept), and optionally export a node-files-only copy.
bool PublishPackage(const std::string &PackageDir, const std::string &DehydratedDestDir, std::string *Error = nullptr);

// Re-establish seeding from a publisher's master: walk every node bundle under Dir and add each CID-referenced file
// (LAYER + META.COVER SOURCE.ipfs content) to the IPFS node BY REFERENCE, so the node serves it (and reprovides it
// to the DHT). Use after wiping ~/.VidyaGod, pointing at e.g. ~/The Vidya. Progress(done, total, filename) is called
// as it goes (off the GUI thread by the caller). Returns the number of files seeded whose recomputed CID matches the
// recorded SOURCE; *Mismatched (if given) counts files whose bytes changed since publish (recorded CID un-seedable).
// CoversOnly=true seeds only META.COVER references (skips LAYERS) — a fast re-pin of cover art without re-hashing
// the (much larger) game layers, e.g. when a download flow hydrated layers but not the cover.
// Modes: ADDITIVE (Overwrite=false, default) re-references only NEW or ORPHANED (backing-file-gone) content, skipping
// CIDs already held with an intact file; OVERWRITE (Overwrite=true) re-references every file. Orphans are re-pointed
// in BOTH modes.
int SeedDirectory(const std::string &Dir,
                  const std::function<void(int, int, const std::string &)> &Progress = {},
                  int *Mismatched = nullptr, bool CoversOnly = false, bool Overwrite = false);
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

// Platform-compatible runners for a launchable on this machine (GUEST ∋ launch host, HOST==machine) WITHOUT any
// install/executable gate — i.e. runners that COULD run it once installed. Sorted by node id.
std::vector<const Node*> CompatibleRunners(const NodeIndex &Idx, const Node &Launch);
// A runner is usable now: a PATH runner present on the system, OR a shipped-build runner fully imported
// (build hydrated + DEFPREFIX). This is the gate for launching.
bool RunnerInstalled(const NodeIndex &Idx, const std::string &RunnerNodeId);
// True if the runner is bundled INSIDE a game package (some launchable node shares its BundleDir), as opposed to a
// standalone runner package (e.g. VidyaGodRunners/ge-proton10-30).
bool IsEmbeddedRunner(const NodeIndex &Idx, const std::string &RunnerNodeId);
// Compatible AND installed — the runners a game can actually launch with right now. Sorted by node id.
std::vector<const Node*> UsableRunners(const NodeIndex &Idx, const Node &Launch);
// Installed runners that can CONSUME InputPlatform (GUEST ∋ InputPlatform), regardless of their HOST — the choices
// for one step of the runner daisy-chain UI (a chain step's input platform → the runners that bridge it onward).
// Sorted by node id.
std::vector<const Node*> CandidateRunners(const NodeIndex &Idx, const std::string &InputPlatform);
// True when every VFS layer in a launchable node's content closure is present locally (its game is installed).
bool NodeHydrated(const NodeIndex &Idx, const std::string &LaunchNodeId);
// True iff the node's closure defines at least one VFS content layer — i.e. it's a real, downloadable game and not a
// content-less/malformed node (which is vacuously "hydrated"). Gate Library/Installed-Packages visibility on this.
bool NodeHasContent(const NodeIndex &Idx, const std::string &LaunchNodeId);
// Inverse of HydrateNode: delete the node closure's local content-layer files (keep manifests + cover) and unpin +
// drop-ref their CIDs, so the package returns to the Catalog as re-downloadable and the node stops seeding it.
// Returns the number of files removed. Use only for managed (library-root) packages, never local/portable ones.
int DehydrateNode(const NodeIndex &Idx, const std::string &LaunchNodeId);
// Every distinct ipfs CID a launchable node's content closure must fetch (its layers' SOURCE CIDs). Drives the
// Catalog download-progress aggregation. Excludes the runner build; cover CIDs are not included. Toggles select
// optional content (node-id -> enabled; absent optional nodes fall back to their DEFAULT).
std::vector<std::string> NodeContentCids(const NodeIndex &Idx, const std::string &LaunchNodeId,
                                         const std::map<std::string, bool> &Toggles = {});
// Gather (without fetching) every missing content-layer + cover target for a launchable's closure, appending to Out.
// Lets a caller pool a game's content with its runners' build layers into ONE concurrent download batch. Returns
// false (with *Error) if a required layer is locally missing AND has no IPFS source. Also seeds an already-present
// cover by reference (best-effort) so a downloader serves it too.
bool CollectContentTargets(const NodeIndex &Idx, const std::string &LaunchNodeId,
                           const std::map<std::string, bool> &Toggles,
                           std::vector<IpfsWrapper::FetchTarget> &Out, std::string *Error = nullptr);
// Fetch (IPFS) every missing VFS layer in a launchable node's content closure + its cover, in place at each node's
// bundle PATH, concurrently (bounded by MaxConcurrentDownloads). Worker-thread (blocks). False (with *Error) on a
// failed required fetch. (= CollectContentTargets + IpfsWrapper::FetchTargetsConcurrent.)
bool HydrateNode(const NodeIndex &Idx, const std::string &LaunchNodeId,
                 const std::map<std::string, bool> &Toggles = {}, std::string *Error = nullptr);

} // namespace PackageCatalog

#endif // PACKAGECATALOG_H
