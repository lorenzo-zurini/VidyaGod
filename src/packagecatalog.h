#ifndef PACKAGECATALOG_H
#define PACKAGECATALOG_H

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>
#include <optional>
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

//Which grafts a content walk includes: the given list, or (unset) the row's pre-ticked grafts.
using GraftChoice = std::optional<std::vector<std::string>>;


// ----- per-package user settings (now the INSTANCE file — see InstanceStore) -----
// The blob lives at <root>/USERDATA/<uid>/<instance>/instance.json, NOT GlobalConfig. GlobalConfigJSON is passed
// ONLY to locate the USERDATA root. Instance="" ⇒ the active (newest-LASTRUN) instance; a READ never creates one,
// a WRITE creates DefaultInstance if the game has none. The setters no longer mutate GlobalConfigJSON (const ref);
// they persist to disk immediately.
nlohmann::ordered_json GetPackageUserSettings(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &PackageUID,
                                              const std::string &Instance = std::string());
void SetPackageUserSetting(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &PackageUID,
                           const std::string &Key, const nlohmann::ordered_json &Value, const std::string &Instance = std::string());

//The package's persisted VARIABLES map (instance.json VARIABLES), or an empty object. THE way to read persisted
//knob/secret values.
nlohmann::ordered_json GetPackageVariables(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &PackageUID,
                                           const std::string &Instance = std::string());
//Merges NewVars into the instance's persisted VARIABLES and writes it back. The single write path for knob/secrets.
void MergePackageVariables(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &PackageUID,
                           const std::map<std::string, std::string> &NewVars, const std::string &Instance = std::string());

// ----- on-disk locations -----
// The managed library root (hydrated content + disk-space checks): Settings.Paths.LibraryRoot or ~/.VidyaGod/LIBRARY.
std::string LibraryRootDir(const nlohmann::ordered_json &GlobalConfigJSON);
// CATALOG (received browse stubs) and ASSETS (content-addressed shared files, e.g. covers) — top-level siblings of
// LIBRARY. The catalog scan spans LIBRARY + CATALOG; publish scans LIBRARY only. See the .cpp for the three-dir model.
std::string CatalogRootDir(const nlohmann::ordered_json &GlobalConfigJSON);
std::string AssetsRootDir(const nlohmann::ordered_json &GlobalConfigJSON);

// The library (named collection dir under the LIBRARY root) a bundle belongs to — the first path segment of BundleDir
// relative to Root; "" if the package sits directly under the root. A package's library is chosen at publish time.
std::string LibraryOf(const std::filesystem::path &BundleDir, const std::filesystem::path &Root);

// ----- locally-added (external) packages -----
// A LIBRARY entry is a "local package" when its PATH is a bundle dir OUTSIDE every package-source dir (added via the
// Library's "Add Local Package", not fetched from a CID source). The bundle dirs of every such entry whose PATH still
// exists — fed to BuildNodeIndex's ExtraBundleDirs so they're indexed alongside CID-source packages.
std::vector<std::filesystem::path> LocalPackageDirs(const nlohmann::ordered_json &GlobalConfigJSON);
//Every tree holding packages this machine authors — LIBRARY and the local packages outside it (never CATALOG: a
//friend's stubs). A save that renames a node follows it through all of them.
std::vector<std::filesystem::path> EditableRoots(const nlohmann::ordered_json &GlobalConfigJSON);
// Drop LIBRARY entries for local packages whose bundle dir no longer exists (the user moved/deleted it). CID-source
// entries are never touched (their content may just be un-hydrated). Mutates GlobalConfigJSON; returns count removed.
int PruneMovedLocalPackages(nlohmann::ordered_json &GlobalConfigJSON);

// ----- package sources by IPFS CID -----
// A package source is a `Settings.PackageSources[]` entry `{ "CID": "…", "NAME": "optional" }` — an IPFS folder CID of
// DEHYDRATED packages (manifests + covers, no content). Fetched into `<DataRoot>/LIBRARY/<name>` (LIBRARY IS the
// CID-source root now that git is gone), scanned as catalog roots, and hydrated on demand.
std::string PackageSourceCID(const nlohmann::ordered_json &Source);                     // the source's folder CID ("" = none)
std::string PackageSourceDir(const nlohmann::ordered_json &GlobalConfigJSON, const nlohmann::ordered_json &Source); // where a source syncs to
bool SourceDirSynced(const std::string &Dir, std::error_code &Ec);                          // exists, readable, non-empty
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
[[nodiscard]] bool AddPackageSource(nlohmann::ordered_json &GlobalConfigJSON, const std::string &Cid, const std::string &Name, bool Friend = false);
// Index of the PackageSources entry whose CID/name field == Cid, or -1. (Used to drop a friend's /ipns/ source.)
int PackageSourceIndexForCID(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &Cid);
// PreserveInstalled (friend sources): never rm a package the user has INSTALLED — convert dirs with hydrated content to
// local (clear their SOURCE tag, keep files), delete only stub-only packages. Default false = full removal (CID sources).
void RemovePackageSource(nlohmann::ordered_json &GlobalConfigJSON, int Index, bool PreserveInstalled = false);

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
[[nodiscard]] bool ApplySourceUpgrade(nlohmann::ordered_json &GlobalConfigJSON, const SourceUpgradePlan &Plan,
                        bool Force = false, std::string *Error = nullptr);

// True if BundleDir is a locally-added package — its path is OUTSIDE every configured package-source dir (used to badge
// such tiles in the library).
bool IsLocalPackagePath(const nlohmann::ordered_json &GlobalConfigJSON, const std::filesystem::path &BundleDir);

// ----- publish -----
// Dehydrate a local bundle for sharing: seed each node LAYER's VFS content + META.COVER over IPFS, record
// SOURCE:{ipfs,CID} into the node files IN PLACE (content kept), and optionally export a node-files-only copy.
//Does NOT stamp node positions. Stamping is an AUTHORING act and belongs to the paths that have both the
//author's intent and their drags — the editor's Publish button and RemintLibrary, which call
//StampNodePositions explicitly first. Doing it in here made every publish path an authoring path: the IPFS
//tab offers "Publish package CID" for ANY catalog entry, so a bundle fetched from someone else's CID source
//had POS written into its node files and its bytes stopped matching the CID that served them. It also meant
//the three call sites that pass no override minted a DIFFERENT CID from the editor's button for the same
//bundle, because they baked the computed layout where the editor bakes the author's.
//Roots/UserDataRoot: where the save's re-mint follows renamed nodes (other packages that contain them, the instances
//that remember them) — EditableRoots and InstanceStore::Root for a real library; none only for a throwaway folder.
[[nodiscard]] bool PublishPackage(const std::string &PackageDir, const std::string &DehydratedDestDir,
                                  std::string *Error = nullptr, const std::vector<std::filesystem::path> &Roots = {},
                                  const std::string &UserDataRoot = std::string());

//What seeding a node's content did (see SeedNodeContent).
struct SeedReport
{
    int Walked = 0, Seeded = 0, Covers = 0, Repaired = 0, SizesStamped = 0, BadCovers = 0;
    std::vector<std::string> Unfetchable, Unshareable;
};
//Seed ONE node's content — each content layer's file and each tile cover — into IPFS where its SOURCE is missing or
//no longer serves its bytes, and write SOURCE and SIZE into the node. IN MEMORY: the node's bytes change, so the
//caller saves it as a re-mint (PkgDoc::Document::Save), never by rewriting its file. False (Error) when a layer's file
//exists and cannot be seeded; a cover that cannot be seeded is only reported.
[[nodiscard]] bool SeedNodeContent(nlohmann::ordered_json &Node, const std::filesystem::path &PackageDir, SeedReport &R,
                                   bool &Changed, std::string *Error = nullptr);

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
// Every directory whose nodes' content this machine serves: its own LIBRARY (authored and installed packages) and each
// package source. Only package sources were ever re-seeded, so an authoring machine never re-added a LIBRARY file it
// did not hold, and re-pointed no orphaned reference there.
std::vector<std::string> SeedRoots(const nlohmann::ordered_json &GlobalConfigJSON);
// Adds by reference every content file under Dir that Dir's nodes name and this node does not hold WHOLE (a root without
// its leaves, a reference whose file is gone, nothing at all), when its bytes are the recorded CID — checked first, so
// wrong bytes are never added (or announced); those are returned in Failures. Returns how many files were added.
int SeedUnheld(const std::string &Dir, std::vector<SeedFailure> *Failures = nullptr);
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
std::string PublishMetaCid(const std::string &SrcDir, std::string *Error = nullptr,
                           const std::vector<std::filesystem::path> &Roots = {}, const std::string &UserDataRoot = std::string());

//The GlobalConfig["EDITORLAYOUT"] key for a bundle (the package editor's positions on this machine): its path made
//ABSOLUTE, normalised, and stripped of any trailing separator, so every way of naming the folder finds the same entry.
std::string EditorLayoutKey(const std::filesystem::path &BundleDir);

// After a mint, write each node's freshly-minted CID back into its working-tree file (the stored top-level "CID"
// handle) and remap every reference (PARENTS / LIBRARYITEM) old-CID → new-CID via HandleToCid, so the on-disk tree
// re-stabilises at the current identities. Both the stored CID and the reference strings are stripped / re-resolved
// at freeze, so this NEVER perturbs a future mint's CIDs — it only keeps the on-disk handles honest, which is what
// makes "recompute ≠ stored CID" a truthful "edited since publish" signal for the editor. Atomic temp+rename per
// file, byte-identical dump(4), only files that actually change are rewritten. Returns files rewritten, -1 on I/O error.
int StampNodeCids(const std::filesystem::path &Root,
                  const std::map<std::string, std::string> &HandleToCid, std::string *Error = nullptr);

// {Level, Name, Cid} rows for a re-mint listing (Level = "package" | "collection"). Used by the per-package mint path.
struct RemintEntry { std::string Level, Name, Cid; };

// PublishLibrary: freeze every node of the on-disk library into its canonical bytes and STORE them (pinned, announced,
// seedable), make one UnixFS folder per package (+ its pin folder), and record Config["Libraries"] (the share sheet)
// and Config["PublishedList"] (the package folders), which it returns (empty on failure, *Error says why). What did
// not publish whole — nodes that did not freeze, content this machine does not hold (a Pinata-only receiver could
// never fetch it) — is summed up in *Gaps ("" when everything published). Call OFF the UI thread, node online.
std::vector<std::string> PublishLibrary(nlohmann::ordered_json &Config, std::string *Error = nullptr,
                                        std::string *Gaps = nullptr);

// Receiver: turn a friend's share snapshot into plain rolling-queue fetch targets whose destinations are the FINAL
// working-tree paths — LIBRARY/<nick> - <lib>/[uid] <title>/<node>.json — straight from the snapshot's routing
// metadata (each item = {cid, node, uid, title, tilecid, tilenode}; the tile block gets its own target in the same
// package dir). No intermediary dir, no post-fetch materialize: the queue writes + pins each node block at its
// library path like any file, and the ordinary catalog scan / hydration / install / re-publish take it from there
// (re-publishing identical nodes yields identical CIDs — the multi-seeder design). `Libs` = the peer's whole
// {libName: [item]} record; `NickLabel` prefixes the per-library dir names. UNTRUSTED input: every path segment is
// sanitized and item counts are bounded. Pure planning — touches no disk, fetches nothing.
struct ReceivedFetch { std::string Cid; std::string Dest; };
// True when a directory holds any file that is not a node (.json): content that was fetched or authored there.
// Source removal and stub replacement must never delete such a dir — it is an install, not a stub.
bool DirHasContent(const std::string &Dir);

// The queue targets of a plan: each package folder a folder fetch (Verify), and each planned package's old install
// redirect forgotten when that install is gone — else its files were sent into the deleted package, which then read as
// installed, and the package never came back.
std::vector<IpfsWrapper::FetchTarget> ReceivedFetchTargets(const std::vector<ReceivedFetch> &Plan);

// The label a friend's received libraries are filed under ("<label> - <lib>"): their nickname from the address book,
// else the tail of their peer ID. The ONE rule — the receiver plans with it and later finds its landed dirs by it.
std::string ReceivedNickLabel(const std::string &PeerID);

std::vector<ReceivedFetch> PlanReceivedFetches(const nlohmann::ordered_json &GlobalConfigJSON,
                                               const std::string &NickLabel, const nlohmann::ordered_json &Libs);

// Fetch a launchable's missing closure node blocks into its package dir, through the ONE rolling queue (plain
// FetchTargets, wave by wave), renaming landed files to their NODE_ID. Synchronous — call OFF the GUI thread.
// After it returns true, a fresh catalog index resolves the full closure (content CIDs, optionals, sizes).
[[nodiscard]] bool CompleteClosure(const NodeIndex &Idx, const std::string &LaunchId, std::string *Error = nullptr);

// True when a node's PARENTS reference ids missing from the index — a received package whose composition graph is
// not fetched yet. Such a node always has something to download (hydration fetches the closure) even though
// NodeContentCids can enumerate nothing from the incomplete graph.
bool NodeClosureIncomplete(const NodeIndex &Idx, const std::string &Id);

// Received PACKAGES: a friend's share entry is a package folder (landed as <pkg dir>/.package/). True while any
// landed folder lists a node block not yet on disk; LandReceivedPackages fetches them through the one rolling queue
// (synchronous — OFF the GUI thread); PruneStaleReceived removes received node files that neither their folder lists
// nor its closure reaches (an older generation's copy in a kept, installed dir). Returns the number removed.
bool ReceivedPackagesIncomplete(const nlohmann::ordered_json &GlobalConfigJSON);
[[nodiscard]] bool LandReceivedPackages(const nlohmann::ordered_json &GlobalConfigJSON, std::string *Error = nullptr);
int PruneStaleReceived(const NodeIndex &Idx, const nlohmann::ordered_json &GlobalConfigJSON);
// Installing a received package MOVES its dir out of CATALOG into LIBRARY/<lib>/<pkg> (the manifest file dropped):
// an ordinary local package from then on. False + *Error on a name collision or an unknown lib dir.
[[nodiscard]] bool AdoptReceivedPackage(const nlohmann::ordered_json &GlobalConfigJSON, const std::filesystem::path &PkgDir,
                                        std::filesystem::path *NewDir = nullptr, std::string *Error = nullptr);
// The package's folder CID — what it is shared as: our own published row (Libraries) for a package of ours, else the
// friend's share snapshot (FriendLibraries) it came from. Matched by the package dir's name and its library (the
// dir's parent: "<lib>" in LIBRARY, "<nick> - <lib>" in CATALOG), names compared as the receiver sanitises them.
// Empty if the package was never published nor received. Field "pin" names a publisher's pin folder (content +
// share folder, what a pinning service is given) instead of the share folder.
std::string PackageFolderCid(const nlohmann::ordered_json &GlobalConfigJSON, const std::filesystem::path &PkgDir,
                             const std::string &Field = "cid");
// The received package dirs a download must adopt before fetching: every package any node in the CLOSURE of each
// launchable, of each runner its chain resolves to, and of each ticked runner lives in — a runner's own NODE layers
// reach other packages too (proton contains proton-wine's chain).
std::set<std::filesystem::path> PackagesToAdopt(const NodeIndex &Idx, const std::vector<std::string> &LaunchIds,
                                                const std::vector<std::string> &RunnerIds,
                                                const nlohmann::ordered_json &GlobalConfigJSON);

// True if a PackageSources entry is an IPNS-name source (a friend / a manually-added /ipns/ address) rather than a
// content folder CID: an explicit IPNS/FRIEND flag, or a CID field with the /ipns/ prefix.
bool IsIpnsSource(const nlohmann::ordered_json &Source);

// ----- node-graph catalog (everything-is-a-node) -----
// Build the global cross-bundle node graph from the configured CID package sources + locally-added bundles — the
// node-native catalog source.
NodeIndex BuildCatalogIndex(const nlohmann::ordered_json &GlobalConfigJSON);
// A catalog index built AFTER the call began, shared: callers waiting at once get one scan between them. Every download
// rebuilt the whole index itself (≈8 s, ≈17 MB) after landing closures and after installing — 46 resumed downloads
// ran up to 138 scans at once: 1700 % CPU, and 3 GB of heap glibc then kept. Any caller's config is used (they name
// the same library).
std::shared_ptr<const NodeIndex> FreshCatalogIndex(const nlohmann::ordered_json &GlobalConfigJSON);
// One presentable game on the shelf: a tile (its UID), its presentation (the merged tile), and its rows — the variant
// nodes whose fold presents it.
struct ShelfTile
{
    std::string Uid;
    nlohmann::ordered_json Tile;
    std::vector<const Node*> Rows;
    std::string Graft;            // the graft presenting this tile ("" for a variant's): every row launches with it
};
// The shelf: one entry per tile UID (faces merged by UID across the library). A family nests by PARENTUID — the base
// game, then its children by depth, then by title — and families order by the base game's title. Under a tile, the
// variant RECOMMENDED under it comes first, then by VARIANT name. A variant presenting two tiles is a row under each.
// A tile a graft presents (NodeIndex::TileGraft) is a card too: its rows are the variants the graft applies onto
// (GraftBases), and the card names the graft (ShelfTile::Graft). A graft that is a VERSION (it carries VARIANT) is a
// row: under the tiles its own entries present, else under its bases' tiles; it runs on a base with it applied.
std::vector<ShelfTile> ShelfTiles(const NodeIndex &Idx);
// The row a tile shows for a variant: the variant (a node with VARIANT) presenting tile Uid whose VARIANT is Variant;
// with Variant empty, the tile's default row — the variant RECOMMENDED under it, else the first by VARIANT name. ""
// when there is none, *Why saying what the tile offers instead.
std::string RowUnderTile(const NodeIndex &Idx, const std::string &Uid, const std::string &Variant, std::string *Why = nullptr);
// The versions a graft applies onto: the variants (not grafts themselves) containing, transitively, a node its leading
// ANY names — what a graft that is a card, or a version, runs on. Recommended-free order: by VARIANT, then key.
std::vector<std::string> GraftBases(const NodeIndex &Idx, const std::string &Graft);
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
// Catalog download-progress aggregation. Excludes the runner build; cover CIDs are not included. The whole closure is
// counted (options included: a download is whole chains); Grafts adds the chosen grafts' content.
std::vector<std::string> NodeContentCids(const NodeIndex &Idx, const std::string &LaunchNodeId,
                                         const GraftChoice &Grafts = std::nullopt);

// ---- grafts ----
// The grafts a row applies, in order (Fold::ApplyGrafts over the local grafts): Chosen = the instance's list, each
// kept when it is offered with those before it applied (the rest go to *Dropped); nullopt = a fresh instance's list,
// the grafts RECOMMENDED under the row's tile (Builtins' %UID% when given — the launched tile — else the node's first).
// A received browse stub never applies: it would mount un-hydrated.
std::vector<std::string> AppliedGrafts(const NodeIndex &Idx, const std::string &LaunchNodeId, const GraftChoice &Chosen,
                                       const std::map<std::string, std::string> &Instance = {},
                                       const std::map<std::string, std::string> &Builtins = {},
                                       std::vector<std::string> *Dropped = nullptr);
// Move the graft at Ticked[From] one place (By = -1 up, +1 down) in the instance's ordered graft list. The order is
// the order they apply in, and a graft is only applied when it is offered with those before it applied — so a move
// after which ANY graft that applied no longer does (a graft on a graft moved above the graft it needs, or one of two
// grafts that exclude each other swapped in ahead) is REFUSED, *Why naming it; Ticked is changed only on success.
// Judged as the launch judges it: Instance = the instance's values, Builtins = at least %UID% (the tile) and
// %PackageUID% (see AppliedGrafts).
bool MoveGraft(const NodeIndex &Idx, const std::string &LaunchNodeId, std::vector<std::string> &Ticked, size_t From,
               int By, std::string *Why = nullptr, const std::map<std::string, std::string> &Instance = {},
               const std::map<std::string, std::string> &Builtins = {});
// The saved graft list as the current library sees it: a list naming a graft that no longer exists was saved against
// an older version of the package (every edit re-mints its grafts), so the choice is stale and the tile falls back to
// its defaults (nullopt: the grafts RECOMMENDED under it). A list whose grafts all exist is the user's choice.
GraftChoice CurrentGraftChoice(const NodeIndex &Idx, const GraftChoice &Saved);
// Tick graft G in the instance's ordered graft list: G goes last, and every ticked graft it cannot apply beside is
// unticked (into *Unticked) — one whose NOT names G, or one G's NOT names. The graft just ticked wins, so grafts that
// exclude each other behave as a choice of one. Judged in list order with the grafts kept so far in place (G or X may
// need another ticked graft): X is unticked when adding it is what stops G applying. Instance/Builtins as MoveGraft.
std::vector<std::string> TickGraft(const NodeIndex &Idx, const std::string &LaunchNodeId, const std::vector<std::string> &Ticked,
                                   const std::string &G, std::vector<std::string> *Unticked = nullptr,
                                   const std::map<std::string, std::string> &Instance = {},
                                   const std::map<std::string, std::string> &Builtins = {});
// The grafts this row offers with Chosen applied (a graft on a graft is offered once the graft it needs is), in the
// default order (LABEL, then CID); into *PreTicked, a fresh instance's list (AppliedGrafts with nullopt).
std::vector<std::string> OfferedGrafts(const NodeIndex &Idx, const std::string &LaunchNodeId,
                                       std::vector<std::string> *PreTicked = nullptr,
                                       const GraftChoice &Chosen = std::nullopt,
                                       const std::string &FaceUid = std::string());   // "" = the node's first tile

// Gather (without fetching) every missing content-layer + cover target for a launchable's closure, appending to Out.
// Lets a caller pool a game's content with its runners' build layers into ONE concurrent download batch. Returns
// false (with *Error) if a required layer is locally missing AND has no IPFS source. Also seeds an already-present
// cover by reference (best-effort) so a downloader serves it too.
// The stamped SOURCE.SIZE per content CID of a launchable's selection — instant + offline (sizes-in-JSON); only
// stamped entries are returned, the caller network-probes the rare unstamped rest.
std::map<std::string, long long> NodeContentSizes(const NodeIndex &Idx, const std::string &LaunchNodeId,
                                                  const GraftChoice &Grafts = std::nullopt);

// The launchable's RESOLVED runner-chain node ids (native terminal excluded) — what CollectRunnerChainTargets pools;
// exposed so an installer can COMPLETE a received runner's closure before collecting its build.
std::vector<std::string> RunnerChainIds(const NodeIndex &Idx, const std::string &LaunchNodeId,
                                        const nlohmann::ordered_json &GlobalConfigJSON);

[[nodiscard]] bool CollectContentTargets(const NodeIndex &Idx, const std::string &LaunchNodeId,
                           const GraftChoice &Grafts,
                           std::vector<IpfsWrapper::FetchTarget> &Out, std::string *Error = nullptr);
// Gather (without fetching) the build download targets of the launchable's RESOLVED runner CHAIN, appending to Out —
// so a full-closure hydrate pulls the game's runtime (JRE / Proton build) alongside its content. The game's own PARENTS
// closure (library content nodes) is already covered by CollectContentTargets; this adds only the runner (which the
// closure walk skips). Best-effort — an unresolvable chain is not fatal.
[[nodiscard]] bool CollectRunnerChainTargets(const NodeIndex &Idx, const std::string &LaunchNodeId,
                               const nlohmann::ordered_json &GlobalConfigJSON,
                               std::vector<IpfsWrapper::FetchTarget> &Out, std::string *Error = nullptr);
// Fetch (IPFS) every missing VFS layer in a launchable node's content closure + its cover, in place at each node's
// bundle PATH, concurrently (bounded by MaxConcurrentDownloads). Worker-thread (blocks). False (with *Error) on a
// failed required fetch. (= CollectContentTargets + IpfsWrapper::FetchTargetsConcurrent.) When GlobalConfigJSON is
// given, ALSO pools the resolved runner chain's build (CollectRunnerChainTargets) so the game is immediately playable.
[[nodiscard]] bool HydrateNode(const NodeIndex &Idx, const std::string &LaunchNodeId,
                 const GraftChoice &Grafts = std::nullopt, std::string *Error = nullptr,
                 const nlohmann::ordered_json *GlobalConfigJSON = nullptr);

} // namespace PackageCatalog

#endif // PACKAGECATALOG_H
