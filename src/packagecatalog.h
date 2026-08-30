#ifndef PACKAGECATALOG_H
#define PACKAGECATALOG_H

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <utility>
#include <functional>
#include <filesystem>

#include "manifestmodel.h"   // Node / NodeIndex (node-graph catalog)
#include "ipfswrapper.h"     // IpfsWrapper::FetchTarget (download collection)

// ---------------------------------------------------------------------------
// PackageCatalog — the sharing service: where packages live on disk (the LIBRARY + CID package sources), the catalog
// of everything known, per-package user settings, and the import/publish/sync operations. All stateless statics
// over JSON (no launch session). Built on ManifestModel; uses IpfsWrapper for content transfer.
//
// (Runner install — ImportRunner — stays in ContainerWrapper: it needs the launch engine's mount + wineboot
// machinery to generate a DEFPREFIX, so it lives with the code that owns prefix-building.)
// ---------------------------------------------------------------------------
namespace PackageCatalog {

// ----- per-package user settings (LIBRARY[i]["USERSETTINGS"]) -----
nlohmann::ordered_json GetPackageUserSettings(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &PackageUID);
void SetPackageUserSetting(nlohmann::ordered_json &GlobalConfigJSON, const std::string &PackageUID, const std::string &Key, const nlohmann::ordered_json &Value);

//The package's persisted VARIABLES map (USERSETTINGS.VARIABLES), or an empty object. This is THE way to
//read persisted knob/secret values — it replaces the `contains("VARIABLES") && is_object()` guard block
//that used to be copy-pasted at every consumer.
nlohmann::ordered_json GetPackageVariables(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &PackageUID);
//Merges NewVars into the package's persisted VARIABLES (creating USERSETTINGS/VARIABLES as needed) and
//writes it back via SetPackageUserSetting. The single write path for persisting knob/secret values.
void MergePackageVariables(nlohmann::ordered_json &GlobalConfigJSON, const std::string &PackageUID, const std::map<std::string, std::string> &NewVars);

// ----- on-disk locations -----
// The managed library root (hydrated content + disk-space checks): Settings.Paths.LibraryRoot or ~/.VidyaGod/LIBRARY.
std::string LibraryRootDir(const nlohmann::ordered_json &GlobalConfigJSON);

// ----- locally-added (external) packages -----
// A LIBRARY entry is a "local package" when its PATH is a bundle dir OUTSIDE every package-source dir (added via the
// Library's "Add Local Package", not fetched from a CID source). The bundle dirs of every such entry whose PATH still
// exists — fed to BuildNodeIndex's ExtraBundleDirs so they're indexed alongside CID-source packages.
std::vector<std::filesystem::path> LocalPackageDirs(const nlohmann::ordered_json &GlobalConfigJSON);
// Drop LIBRARY entries for local packages whose bundle dir no longer exists (the user moved/deleted it). CID-source
// entries are never touched (their content may just be un-hydrated). Mutates GlobalConfigJSON; returns count removed.
int PruneMovedLocalPackages(nlohmann::ordered_json &GlobalConfigJSON);

// ----- package sources by IPFS CID -----
// A package source is a `Settings.PackageSources[]` entry `{ "CID": "…", "NAME": "optional" }` — an IPFS folder CID of
// DEHYDRATED packages (manifests + covers, no content). Fetched into `<DataRoot>/LIBRARY/<name>` (LIBRARY IS the
// CID-source root now that git is gone), scanned as catalog roots, and hydrated on demand.
std::vector<std::string> PackageSourceDirs(const nlohmann::ordered_json &GlobalConfigJSON);   // existing source dirs
// True if BundleDir lives under a package-source dir (a CID-source package, vs a locally-added one).
bool IsPackageSourcePath(const nlohmann::ordered_json &GlobalConfigJSON, const std::filesystem::path &BundleDir);
// The NAME of the package source whose dir contains BundleDir, or "" if none — used to group catalog tiles into one
// named section per source (the catalog's grouping key).
std::string PackageSourceNameForPath(const nlohmann::ordered_json &GlobalConfigJSON, const std::filesystem::path &BundleDir);
// For each configured source: if its dir is empty/missing, recursively fetch the folder CID (dehydrated only — no
// content hydration; requires the IPFS node online), then upsert its bundles into LIBRARY. Mutates config; returns the
// number of packages indexed. On a fetch failure, sets *Error (if given) and leaves that source's dir untouched.
int SyncPackageSources(nlohmann::ordered_json &GlobalConfigJSON, std::string *Error = nullptr);
// True if any configured CID source hasn't materialized its LIBRARY dir yet (fetch failed / still pending) — the
// caller schedules a re-sync so a source that couldn't be fetched on a hostile network is retried, not abandoned.
bool HasMissingSources(const nlohmann::ordered_json &GlobalConfigJSON);
// Append a source (dedup on CID) — caller then SyncPackageSources + reindex + persists. Returns false if CID is empty
// or already present.
bool AddPackageSource(nlohmann::ordered_json &GlobalConfigJSON, const std::string &Cid, const std::string &Name);
// Remove source [index]: drop its config entry, delete its LIBRARY/<name> dir, and drop LIBRARY entries under it.
void RemovePackageSource(nlohmann::ordered_json &GlobalConfigJSON, int Index);

// ----- upgrading a source to a new collection CID -----
// Editing a source's CID in config does NOTHING on its own: SyncPackageSources fetches only when the source's
// LIBRARY/<NAME> dir is missing or empty, so an existing install keeps serving the OLD manifests forever. And the
// blunt alternative — remove + re-add — makes fetchDirOnce RemoveAll() the destination, destroying every hydrated
// content file in the source (gigabytes) plus anything else living there. Upgrading is therefore a MERGE:
// manifests are replaced, content whose CID did not change is KEPT, and the previous version is demoted rather
// than destroyed (so peers still on the old CID keep being served, and a rollback stays possible).
struct SourceUpgradePlan
{
    std::string Name, OldCid, NewCid, Dir, StagingDir, DeprecatedDir;
    std::vector<std::string> JsonAdded, JsonChanged, JsonRemoved;      // manifest paths, relative to the source dir
    std::map<std::string, std::string> ContentKeep;                    // path → CID, already correct: NOT re-downloaded
    std::map<std::string, std::pair<std::string, std::string>> ContentMove;  // CID → {from, to}, same bytes, new home
    std::map<std::string, std::string> ContentDeprecate;               // path → CID no longer referenced by the new tree
    std::vector<std::string> ContentNew;                               // CIDs the new tree adds (hydrate on demand)
    long long KeptBytes = 0, DeprecatedBytes = 0;
    int OldPackages = 0, NewPackages = 0, SharedPackages = 0;          // lineage evidence
    bool Valid = false;
};

// Fetch NewCid's manifest tree into a STAGING dir beside the source and diff it against what is on disk. Nothing is
// modified. A failed/partial fetch therefore leaves the live source completely untouched. *Error is set and Valid
// stays false on failure. SharedPackages==0 with a non-empty old tree means the new CID is a DIFFERENT collection,
// not a newer version of this one — ApplySourceUpgrade refuses that unless Force.
SourceUpgradePlan PlanSourceUpgrade(const nlohmann::ordered_json &GlobalConfigJSON,
                                    const std::string &SourceName, const std::string &NewCid,
                                    std::string *Error = nullptr);

// Apply a plan: demote the old version into LIBRARY/.deprecated/<NAME>/<oldCID>/ (a copy of its manifests plus any
// content the new tree no longer references, with refs re-pointed so the OLD CIDs keep serving from their new
// home), then write the new manifests, relocate content that merely moved, and set the source's CID. Mutates
// GlobalConfigJSON; the caller saves + re-syncs. Returns false (with *Error) without touching the live source if
// the lineage check fails and !Force.
bool ApplySourceUpgrade(nlohmann::ordered_json &GlobalConfigJSON, const SourceUpgradePlan &Plan,
                        bool Force = false, std::string *Error = nullptr);

// True if BundleDir is a locally-added package — its path is OUTSIDE every configured package-source dir (used to badge
// such tiles in the library).
bool IsLocalPackagePath(const nlohmann::ordered_json &GlobalConfigJSON, const std::filesystem::path &BundleDir);

// ----- publish -----
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
// One file that could not be seeded as published: its content no longer hashes to the recorded CID. Surfaced so
// the UI can show it as an ERROR instead of it living only in a log line — a stale reference is otherwise
// invisible until a peer requests it and hangs.
struct SeedFailure { std::string Path, RecordedCid, ActualCid; };
int SeedDirectory(const std::string &Dir,
                  const std::function<void(int, int, const std::string &)> &Progress = {},
                  int *Mismatched = nullptr, bool CoversOnly = false, bool Overwrite = false,
                  bool Verify = false, std::vector<SeedFailure> *Failures = nullptr);
// What one heal pass did, and — more importantly — what it could NOT fix. Anything left in Drift/Unrepaired is a
// content problem a machine must not silently "fix": both change what this node publishes, so they are reported
// loudly and left for a human. Ok() is the one thing callers should branch on.
struct HealReport
{
    int Repointed          = 0;   // files the cheap additive pass (re)seeded — NOT a count of orphans found
    int StaleRepaired      = 0;   // refs whose BYTES disagreed, drop-ref'd + re-added successfully (deep pass)
    int PrunedUnservable   = 0;   // unreferenced pins whose backing is gone
    int PrunedUnreferenced = 0;   // healthy pins nothing references (only when PruneUnreferenced)
    int Verified           = 0;   // CIDs read back and confirmed servable (deep pass)
    std::vector<std::string> Drift;       // recorded SOURCE.CID != actual content — needs a re-mint, NOT a silent rewrite
    std::vector<std::string> Unrepaired;  // still unservable after drop-ref + re-add — genuine data loss
    bool Ok() const { return Drift.empty() && Unrepaired.empty(); }
};

struct HealOptions
{
    // Read every referenced CID back out of the blockstore (IpfsWrapper::VerifyCid) instead of only stat-ing its
    // backing path. This is the ONLY way to catch a reference whose file exists but whose bytes changed — the class
    // that makes peers hang — and the only way to catch a recorded CID that never matched its content. I/O-bound:
    // it reads the whole library, so it belongs on an explicit `--heal`, never on the background timer.
    bool Deep = false;
    // Also drop pins that are healthy but referenced by nothing (superseded collection/package meta-CIDs — what the
    // IPFS tab shows as "unknown"). Off by default because a deliberately hand-seeded folder looks identical.
    bool PruneUnreferenced = false;
};

// Self-heal every registered package source. Always: additively re-seed each source dir so any ORPHANED no-copy
// reference (backing file moved/re-created since it was seeded) is re-pointed at its current on-disk content — an
// orphaned reference otherwise reads fine locally but fails the moment a peer requests those blocks. Then prune pins
// that are unreferenced AND unservable (leftovers of superseded publishes that no re-point can fix).
// With Deep: additionally READ every referenced CID back; a ref that fails is drop-ref'd and re-added (the plain
// re-add alone dedup-skips and silently changes nothing), and a re-add that yields a DIFFERENT CID is reported as
// Drift rather than rewritten. Node must be online-or-local. Call OFF the GUI thread.
HealReport HealSourceContent(const nlohmann::ordered_json &GlobalConfigJSON, const HealOptions &Options = {});

// Every CID a source directory contributes: its collection CID, its packages' meta-CIDs, and every SOURCE.CID inside
// its node JSONs. Used to unpin a source's content when it is REMOVED — otherwise its pins outlive it in the IPFS
// table and, once the fetched dir is deleted, become unservable references that hang any peer that asks.
std::set<std::string> SourceContentCids(const nlohmann::ordered_json &GlobalConfigJSON, const nlohmann::ordered_json &Source);
// The local files SeedDirectory would seed: {local path → recorded SOURCE CID} across a folder's node JSONs — VFS
// content LAYERS (unless CoversOnly) + cover art (the DeclareLibraryItem layer's COVER, and legacy top-level META.COVER).
// Pure (no IPFS node). Only includes files that exist on disk. Exposed for reuse + testing the cover-location handling.
std::map<std::string, std::string> SeedTargets(const std::string &Dir, bool CoversOnly = false);
// As SeedTargets, but ExistingOnly=false returns every RECORDED {path → CID} even when the file is absent. Seeding
// needs the existing-only view; the upgrade diff needs the recorded view, since a freshly fetched manifest tree has
// no content files at all and would otherwise look like it references nothing.
std::map<std::string, std::string> ManifestTargets(const std::string &Dir, bool CoversOnly = false, bool ExistingOnly = true);
// Recursively copy a bundle/collection's node JSON (*.json only — no content zips, cover images, or runtime dirs) into
// DestDir, preserving the relative tree. Returns count copied. This is what makes a published Meta-CID text-only.
int MirrorDehydrated(const std::string &SrcDir, const std::string &DestDir);

// Mint a JSON-only Meta-CID for SrcDir (single bundle OR a dir of bundle subdirs): idempotently content-address content
// + covers (PublishPackage), mirror the JSON-only tree into StagingDir (must persist — the CID seeds from there by
// reference), then AddNoCopy it. Returns the folder CID, or "" on failure.
std::string PublishMetaCid(const std::string &SrcDir, std::string *Error = nullptr);

// Re-mint every CID of a whole LIBRARY (a dir of source-collection subdirs, e.g. ~/.VidyaGod/LIBRARY) in ONE node
// session, across the 3-level schema: content CIDs (per file, written as SOURCE.CID into the node JSONs), package
// meta-CIDs (per package folder — recorded in Settings.PackageCids), and collection meta-CIDs (per source — updated
// into the matching Settings.PackageSources entry). Deterministic: unchanged content re-produces the same CID. Fills
// Out with {Level, Name, Cid} rows (Level = "package" | "collection") for listing. Returns false on the first failure.
struct RemintEntry { std::string Level, Name, Cid; };
bool RemintLibrary(const std::string &LibraryRoot, nlohmann::ordered_json &Config,
                   std::vector<RemintEntry> &Out, std::string *Error = nullptr);

// ----- node-graph catalog (everything-is-a-node) -----
// Build the global cross-bundle node graph from the configured CID package sources + locally-added bundles — the
// node-native catalog source.
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
// Bulk hydration for the WHOLE index in O(N+E): computes {Hydrated, HasContent} for every node via one topological
// pass over PARENTS (a node's value = its own layers ⊕ its parents'), memoizing per-node and stat-caching per path.
// Use this instead of calling NodeHydrated/NodeHasContent per-launchable when building library/catalog tiles —
// otherwise a package with a launchable per version over a deep delta chain is O(N²) (each closure re-walked + re-stat).
struct NodeHydration { bool Hydrated = true; bool HasContent = false; };
std::unordered_map<std::string, NodeHydration> HydrationMap(const NodeIndex &Idx);
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
// Gather (without fetching) the build download targets of the launchable's RESOLVED runner CHAIN, appending to Out —
// so a full-closure hydrate pulls the game's runtime (JRE / Proton build) alongside its content. The game's own PARENTS
// closure (library content nodes) is already covered by CollectContentTargets; this adds only the runner (which the
// closure walk skips). Best-effort — an unresolvable chain is not fatal.
bool CollectRunnerChainTargets(const NodeIndex &Idx, const std::string &LaunchNodeId,
                               const nlohmann::ordered_json &GlobalConfigJSON,
                               std::vector<IpfsWrapper::FetchTarget> &Out, std::string *Error = nullptr);
// Fetch (IPFS) every missing VFS layer in a launchable node's content closure + its cover, in place at each node's
// bundle PATH, concurrently (bounded by MaxConcurrentDownloads). Worker-thread (blocks). False (with *Error) on a
// failed required fetch. (= CollectContentTargets + IpfsWrapper::FetchTargetsConcurrent.) When GlobalConfigJSON is
// given, ALSO pools the resolved runner chain's build (CollectRunnerChainTargets) so the game is immediately playable.
bool HydrateNode(const NodeIndex &Idx, const std::string &LaunchNodeId,
                 const std::map<std::string, bool> &Toggles = {}, std::string *Error = nullptr,
                 const nlohmann::ordered_json *GlobalConfigJSON = nullptr);

} // namespace PackageCatalog

#endif // PACKAGECATALOG_H
