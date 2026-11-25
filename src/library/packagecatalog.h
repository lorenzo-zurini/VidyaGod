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

#include "manifestmodel.h"
#include "shelf.h"       // GraftChoice
#include "installed.h"   // Node / NodeIndex (node-graph catalog)
#include "commonutils.h"     // Diagnostics::Entry — what an index build logged
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
using Shelf::GraftChoice;



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
//peer's stubs). A save that renames a node follows it through all of them.
std::vector<std::filesystem::path> EditableRoots(const nlohmann::ordered_json &GlobalConfigJSON);
//The libraries this machine keeps packages in: the folders directly under LIBRARY ("VidyaGod", "VidyaGodRunners" ...).
std::vector<std::string> LibraryNames(const nlohmann::ordered_json &GlobalConfigJSON);
//"[749][v1.0] Age of Empires II" -> "Age of Empires II": a package's folder name without its bracketed tags.
std::string FolderTitle(const std::filesystem::path &Dir);
//A package's title as its shelf shows it: the TITLE of the tile its folder is named for ([<uid>]), read from its
//nodes - "Baldur's Gate" where the folder had to say "Baldur_s Gate"; the folder's title when no node has it.
std::string PackageTitle(const std::filesystem::path &Dir);
//Every package this machine authors, for the editor's Open: each library's package folders and the local packages
//outside LIBRARY (Library ""), by title.
struct AuthoredPackage { std::filesystem::path Dir; std::string Title, Library; };
std::vector<AuthoredPackage> AuthoredPackages(const nlohmann::ordered_json &GlobalConfigJSON);
//A new, empty package folder "<LIBRARY>/<Library>/[<Uid>][v1.0] <Title>" (the title made safe for a folder name), for
//the package editor's New package. Refuses (empty path, Err says why) an unknown library, a UID that is not digits,
//an empty title, or a UID some package in LIBRARY already uses.
std::filesystem::path CreatePackageFolder(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &Library,
                                          const std::string &Uid, const std::string &Title, std::string &Err);
//A new, empty LIBRARY package folder "<LIBRARY>/<Library>/<Name>" (named as a library is: by its name, no game ID -
//a library has no tile; the games that use it graft it). Refuses an unknown library, an empty name, or a name a
//folder of that library already has.
std::filesystem::path CreateLibraryFolder(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &Library,
                                          const std::string &Name, std::string &Err);
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
//Why: what is wrong when it is refused (no CID, not a CID, already added, a folder of that name already in the library).
[[nodiscard]] bool AddPackageSource(nlohmann::ordered_json &GlobalConfigJSON, const std::string &Cid, const std::string &Name,
                                    std::string *Why = nullptr);
// What a worker did to a config list on its own copy of the config (Before → After), applied to the live list: an entry
// it removed goes, one it added comes in, one it changed (it went and came under one Key) is replaced where it stands.
// Entries are matched by value, so two alike, or two of one key, are two. What the live list gained, lost or changed
// while the worker ran stays — an entry the worker changed that is no longer live as it saw it is left as it is now.
// Assigning the worker's copy back would undo all of that. LibraryEntryKey: a LIBRARY entry's PATH; SourceKey: a
// package source's NAME, else its CID.
void MergeListChange(nlohmann::ordered_json &Live, const nlohmann::ordered_json &Before, const nlohmann::ordered_json &After,
                     std::string (*Key)(const nlohmann::ordered_json &));
std::string LibraryEntryKey(const nlohmann::ordered_json &Entry);
std::string SourceKey(const nlohmann::ordered_json &Source);
// KeepInstalled: a package installed from it (hydrated content) stays, as a local package; only stub-only packages go.
// Files go only from a folder that is the source's (its marker — its fetch made it or the sync marked it — and not a
// library this machine publishes), and only its packages (its rows): otherwise the source and its entries go and the
// folder stays. Kept packages keep their rows without the source (KEPT_FROM).
void RemovePackageSource(nlohmann::ordered_json &GlobalConfigJSON, int Index, bool KeepInstalled = true);
//The configured source a package in the library came from (its LIBRARY row's CIDSOURCE, when that source is still
//configured), or "": only such a package can be dehydrated — something can download it again.
std::string SourceOfPackage(const nlohmann::ordered_json &GlobalConfigJSON, const std::filesystem::path &BundleDir);

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
// ----- the golden marker -----
// Whether this machine's user marked a package golden (its golden pass done), keyed by the family's UID
// (%PackageUID%: the same across re-mints and versions). Purely cosmetic and local: it lives in GlobalConfig
// Settings.GoldenPackages ({uid: the date it was marked}), never in a package, and nothing but the Library's badge
// reads it. GoldenSince: that date ("" = not golden). SetGolden marks (Date, YYYY-MM-DD) or clears it; the caller saves.
std::string GoldenSince(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &PackageUid);
void SetGolden(nlohmann::ordered_json &GlobalConfigJSON, const std::string &PackageUid, bool On, const std::string &Date);

// ----- publish -----
// Dehydrate a local bundle for sharing: seed each node LAYER's VFS content + META.COVER over IPFS, record
// SOURCE:{ipfs,CID} into the node files IN PLACE (content kept), and optionally export a node-files-only copy.
//Never writes canvas positions: a node's bytes (its CID) never carry where it sits on a canvas (POS is stripped by the
//canonical form; the layout lives in the config's EDITORLAYOUT).
//Roots/UserDataRoot: where the save's re-mint follows renamed nodes (other packages that contain them, the instances
//that remember them) — EditableRoots and InstanceStore::Root for a real library; none only for a throwaway folder.
//Why an export folder is not one to replace (empty: it is): the root, the package itself or a folder inside it, one that
//holds the package or the home folder, or a non-empty folder no export made (an export leaves .vgexport in it).
std::string ExportDestRefused(const std::string &SrcDir, const std::string &DestDir);
[[nodiscard]] bool PublishPackage(const std::string &PackageDir, const std::string &DehydratedDestDir,
                                  std::string *Error = nullptr, const std::vector<std::filesystem::path> &Roots = {},
                                  const std::string &UserDataRoot = std::string());

//What seeding a node's content did (see SeedNodeContent).
struct SeedReport
{
    int Walked = 0, Seeded = 0, Covers = 0, Repaired = 0, SizesStamped = 0, BadCovers = 0;
    int Read = 0;   // files read back to check their CID (a file unchanged since it was checked is not)
    std::vector<std::string> Unfetchable, Unshareable;
};
//Seed ONE node's content — each content layer's file and each tile cover — into IPFS where its SOURCE is missing or
//no longer serves its bytes, and write SOURCE and SIZE into the node. IN MEMORY: the node's bytes change, so the
//caller saves it as a re-mint (PkgDoc::Document::Save), never by rewriting its file. False (Error) when a layer's file
//exists and cannot be seeded; a cover that cannot be seeded is only reported.
//Reads and writes of CACHE/seedverified.json since start (a publish reads it once and writes it once).
long long DebugVerifiedFileIo();
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

// The share sheet (MetaPackageFormat chapter 15 §15.4): Config["ShareSheet"] = {node CID: "<library>/<package>/<file>"}.
// A package is its entries; what it uses from another package is read from its nodes, never recorded. Config["Pins"]
// = {"<library>/<package>": pin folder CID} is the publisher's own (what a pinning service pins), never shared.
struct SheetEntry { std::string Cid, Lib, Pkg, File; };
//The entries of a sheet ({cid: path}); a path that is not exactly three non-empty segments is skipped.
std::vector<SheetEntry> SheetEntries(const nlohmann::ordered_json &Sheet);
//The entries of a peer's snapshot as it travels ({lib: [{cid, pkg, node}]}, node = the file name), and back.
std::vector<SheetEntry> WireEntries(const nlohmann::ordered_json &Libs);
nlohmann::ordered_json Wire(const std::vector<SheetEntry> &Entries);
//Where a node lives, for naming it to a person: "<library>/<package>/<file>" from our sheet or a peer's, "" if none.
std::string SheetPath(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &Cid);

// PublishLibrary: freeze every node of the on-disk library into its canonical bytes and STORE them (pinned, announced,
// seedable), name every package's node files by their labels, make each package's pin folder, and record the share
// sheet and the pins (Config["ShareSheet"], Config["Pins"]). Returns the packages published ("<library>/<package>";
// empty on failure, *Error says why). What did not publish whole — nodes that did not freeze, content this machine does
// not hold (a Pinata-only receiver could never fetch it) — is summed up in *Gaps ("" when everything published). Call
// OFF the UI thread, node online.
// What a peer receives for the libraries shared with them, as it travels ({lib: [{cid, pkg, node}]}): every entry of
// those libraries, and every package a node of theirs contains (Contains(cid): the node's NODE references),
// transitively, whatever library it is in. A package is shared whole and once.
nlohmann::ordered_json SharedWith(const nlohmann::ordered_json &Sheet, const std::vector<std::string> &SharedLibs,
                                  const std::function<std::vector<std::string>(const std::string &)> &Contains);
//What node Cid contains (its NODE references): from the index, or — a node the sheet names that the working tree has
//moved on from since (edited, not published yet) — from its published block, held here. Empty for a node neither has.
std::vector<std::string> NodeContains(const NodeIndex &Idx, const std::string &Cid);
//Packages ("<library>/<package>") and every package a node of theirs contains, transitively — what a share or a pin
//closes over. A package the sheet does not hold is left out.
std::vector<std::string> PackagesContained(const nlohmann::ordered_json &Sheet, const std::vector<std::string> &Packages,
                                           const std::function<std::vector<std::string>(const std::string &)> &Contains);
//Every publish holds the library's write lock — <the library root's parent>/library-write.lock, ~/.VidyaGod/library-write.lock
//by default, the lock the packaging scripts take too (`flock` on it) — for its whole run, the tree read under it. Held
//elsewhere, the publish does nothing and fails with LibraryBusy: the caller tries again later.
inline constexpr const char *LibraryBusy = "the library is being written by another tool: trying again shortly";
std::filesystem::path LibraryWriteLock(const nlohmann::ordered_json &Config);
std::vector<std::string> PublishLibrary(nlohmann::ordered_json &Config, std::string *Error = nullptr,
                                        std::string *Gaps = nullptr);
//Your libraries: the collection folders under the library root that hold a package (a peer's packages you only see
//live under CATALOG and are not among them; one you installed lives in your library and is), with any the share sheet
//still names. What can be shared, by name, whether or not it has been published yet.
std::vector<std::string> LibrariesOnDisk(const nlohmann::ordered_json &Config, const NodeIndex &Idx);
//What sharing these libraries needs published before a peer sees them as they are: each package whose nodes on disk
//(their CIDs, as the index froze them, which is what publishing records) are not exactly its share sheet entries — a
//new package, an edited one —, never one of a library the sheet names nothing of (never prepared: its first publish
//names every node file by its label, a step a person starts); and the packages the sheet names that are gone.
//Signature: per "<lib>/<pkg>" of every package of these libraries on disk, its node CIDs, so a caller can tell a
//package changed since it last tried (a package that cannot publish whole stays different from the sheet).
struct ShareStaleness
{
    struct Package { std::filesystem::path Dir; std::string Key; };   // Key: "<lib>/<pkg>"
    std::vector<Package> Stale;
    std::vector<std::string> Removed;               // "<lib>/<pkg>" the sheet names, no longer on disk
    std::set<std::string> Unprepared;               // libraries on disk the sheet names nothing of
    std::vector<Package> UnpreparedPackages;        // their packages (what preparing them publishes)
    std::map<std::string, std::string> Signature;
    std::vector<std::string> Refused;               // "<pkg>: why" of a node the index refused (too large, …): never shared as it is
};
ShareStaleness StaleSharedPackages(const nlohmann::ordered_json &Config, const NodeIndex &Idx, const std::set<std::string> &Libs);
//Takes packages ("<lib>/<pkg>") out of the share sheet and their pins — what a peer is given of a package deleted from
//disk. Nothing is minted or renamed. The number of entries taken out.
int DropFromShareSheet(nlohmann::ordered_json &Config, const std::vector<std::string> &Packages);
//PublishLibrary for some packages only (their folders under the library root): their nodes and those of every package
//they contain are minted; the named packages are rebuilt, a contained one only when its entries are stale; their
//entries replace their own in the sheet and every other entry is left untouched. Returns the packages published.
std::vector<std::string> PublishPackages(nlohmann::ordered_json &Config, const std::vector<std::filesystem::path> &PackageDirs,
                                         std::string *Error = nullptr, std::string *Gaps = nullptr);

// Receiver: turn a peer's share snapshot (their share sheet's entries, {lib: [{cid, pkg, node}]}) into plain
// rolling-queue file fetches whose destinations are the final paths — CATALOG/<nick> - <lib>/<pkg>/<file> — one per
// entry. The queue writes + pins each node file there like any file, and the ordinary catalog scan / hydration /
// install / re-publish take it from there (re-publishing identical nodes yields identical CIDs — the multi-seeder
// design). `NickLabel` prefixes the per-library dir names. UNTRUSTED input: every path segment is sanitized, a CID is
// plain characters, and item counts are bounded. A package installed here is not planned. Pure planning — touches no
// disk, fetches nothing.
struct ReceivedFetch { std::string Cid; std::string Dest; };
//The folder each of a peer's packages lands in ((library, package) → folder name): its name made safe, or — when names
//clash without case — every clashing one qualified by its name's hash, whatever order they arrive in.
std::map<std::pair<std::string, std::string>, std::string> ReceivedFolderNames(const nlohmann::ordered_json &Libs);
// True when a directory holds any file that is not a node (.json): content that was fetched or authored there.
// Source removal and stub replacement must never delete such a dir — it is an install, not a stub.
bool DirHasContent(const std::string &Dir);

// The queue targets of a plan: each entry a file fetch (Verify), and each planned package's old install
// redirect forgotten when that install is gone — else its files were sent into the deleted package, which then read as
// installed, and the package never came back.
std::vector<IpfsWrapper::FetchTarget> ReceivedFetchTargets(const std::vector<ReceivedFetch> &Plan);

// The label a peer's received libraries are filed under ("<label> - <lib>"): the one chosen at its first snapshot
// (Config.PeerLabels, AssignReceivedLabel), else its nickname from the address book, else the tail of its peer ID. The
// ONE rule — the receiver plans with it and later finds its landed dirs by it.
std::string ReceivedNickLabel(const nlohmann::ordered_json &Config, const std::string &PeerID);
// Chooses a peer's label once and keeps it (Config.PeerLabels): its nickname (LiveNick), unless another peer's label
// is that already (two "Deck"s: one would replace the other's node files) — then with its ID's tail. Kept: a peer
// that renames itself keeps its folders. Returns the label.
std::string AssignReceivedLabel(nlohmann::ordered_json &Config, const std::string &PeerID, const std::string &LiveNick);
// The nickname the address book has for the peer now ("" if none).
std::string ReceivedLiveNick(const std::string &PeerID);

std::vector<ReceivedFetch> PlanReceivedFetches(const nlohmann::ordered_json &GlobalConfigJSON,
                                               const std::string &NickLabel, const nlohmann::ordered_json &Libs);


// ----- versions of an installed package: switching to a peer's version when the user says so -----
// There is no "newer": two people building one package have equally good versions. A peer's entries for a package
// installed here (Listing: node CID → file name) that differ from this package's nodes are "<peer> has a different
// version". Peer/Nick name the peer; Version names the listing (the same listing, the same name).
struct PackageUpdate
{
    std::filesystem::path Dir; std::string Lib, Pkg, Peer, Nick, Version;
    std::map<std::string, std::string> Listing;
};
// Every such version, for the libraries peers share with us (local: no network).
// A version every node of which is already here (one switched to before, the nodes it dropped kept for games that
// still contain them) is offered only once something kept may go.
// TEST ONLY: installed package folders whose node files the update scan read (one unchanged since is not read).
long long DebugNodeCidReads();
std::vector<PackageUpdate> AvailableUpdates(const nlohmann::ordered_json &GlobalConfigJSON);
// A peer's version fetched beside the library (never inside LIBRARY or CATALOG: nothing indexes it), and the plan to
// switch to it: every node it lists comes in; an installed node it does not list goes — unless another installed
// package (or the new version itself) still contains it, then it is KEPT. Content files follow the node set: a file
// of a removed node is deleted, two CIDs sharing a name live at bumped names (ContentFile), bytes already here under
// another name are moved rather than fetched again.
struct ContentDelete
{
    std::filesystem::path File; std::string Cid;
    std::vector<std::filesystem::path> Elsewhere;                                 // the CID's files that stay
};
struct StagedUpdate
{
    PackageUpdate U; std::filesystem::path Stage;
    std::vector<std::string> Listing, Added, Removed, Kept;
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> Moves;   // content: from → to
    std::vector<ContentDelete> Deletes;
    unsigned long long DeleteBytes = 0;
};
// Fetch U's nodes into its own stage (per peer and version) and plan the switch (through the ONE queue; waits —
// call off the GUI thread). Refused for a package holding never-minted (draft) node files. A stage left by a switch
// that stopped half-way is resumed as it was planned.
bool StageUpdate(const nlohmann::ordered_json &GlobalConfigJSON, const PackageUpdate &U, StagedUpdate *Out,
                 std::string *Error = nullptr);
// Switch the installed package to the staged version. The plan is made again under the package's lock (the library
// may have changed while the user read it) and refused if it would now delete a file the user was not shown. Refused
// while a game runs or a download lands in the package. Saves and kept state are untouched. The plan is journaled in
// the stage first: staging and applying again after a failure finishes the same switch.
bool ApplyStagedUpdate(const nlohmann::ordered_json &GlobalConfigJSON, const StagedUpdate &S, std::string *Error = nullptr);
// Drop a staged version without applying it (its fetch forgotten, its files and their references gone).
void DiscardStagedUpdate(const StagedUpdate &S);

// True when a node contains (NODE) a node missing from the index: a package it needs has not landed yet, or the
// peer does not share it. Nothing is ever fetched into the node's own package to fill the gap (a node has one home).
bool NodeClosureIncomplete(const NodeIndex &Idx, const std::string &Id);
// The nodes Id contains, at any depth, that are not in the index (sorted, unique). Empty: the closure is complete.
std::vector<std::string> MissingFromClosure(const NodeIndex &Idx, const std::string &Id);

// Received PACKAGES: a peer's share sheet entries, one node file each (CATALOG/<nick> - <lib>/<pkg>/<file>). True
// while an entry's file is not on disk; LandReceivedPackages fetches (or renames) them through the one rolling queue and
// verifies every file (synchronous — OFF the GUI thread); PruneStaleReceived removes the node files of a received
// package that its entries do not name (an older generation's). Returns the number removed.
//Missing node files of received packages — not counting one this run refused (it did not verify, or was too large).
bool ReceivedPackagesIncomplete(const nlohmann::ordered_json &GlobalConfigJSON);
//Forgets the node files refused because they did not verify (a peer's changed snapshot: tried afresh). One larger
//than a node stays refused for the run (that is the CID's own).
void ForgetRefusedNodes();
[[nodiscard]] bool LandReceivedPackages(const nlohmann::ordered_json &GlobalConfigJSON, std::string *Error = nullptr);
int PruneStaleReceived(const NodeIndex &Idx, const nlohmann::ordered_json &GlobalConfigJSON);
// Installing a received package MOVES its dir out of CATALOG into LIBRARY/<lib>/<pkg>: an ordinary local package from
// then on. False + *Error on a name collision or an unknown lib dir.
// Several at once: each judged first (one refused refuses all, nothing moved), their references moved in ONE scan.
[[nodiscard]] bool AdoptReceivedPackages(const nlohmann::ordered_json &GlobalConfigJSON, const std::vector<std::filesystem::path> &PkgDirs,
                                         std::vector<std::filesystem::path> *NewDirs = nullptr, std::string *Error = nullptr);
[[nodiscard]] bool AdoptReceivedPackage(const nlohmann::ordered_json &GlobalConfigJSON, const std::filesystem::path &PkgDir,
                                        std::filesystem::path *NewDir = nullptr, std::string *Error = nullptr);
// A package of ours' pin folder (its node files + its content: what a pinning service is given), from Config["Pins"]
// by its library and folder name. Empty if it was never published here (a peer's package has none: a receiver never
// reads a pin folder).
std::string PackagePin(const nlohmann::ordered_json &GlobalConfigJSON, const std::filesystem::path &PkgDir);
// The received package dirs a download must adopt before fetching: every package any node in the CLOSURE of each
// launchable, of each runner its chain resolves to, and of each ticked runner lives in — a runner's own NODE layers
// reach other packages too (proton contains proton-wine's chain).
std::set<std::filesystem::path> PackagesToAdopt(const NodeIndex &Idx, const std::vector<std::string> &LaunchIds,
                                                const std::vector<std::string> &RunnerIds,
                                                const nlohmann::ordered_json &GlobalConfigJSON);


// ----- node-graph catalog (everything-is-a-node) -----
// Build the global cross-bundle node graph from the configured CID package sources + locally-added bundles — the
// node-native catalog source. Loaded from the on-disk index cache when no node file changed since it was written.
// *Inputs (if given) receives what it was built from: the files and their stamps (see CurrentIndex); *Said what the
// build logged as warnings and errors (the scan's, or the ones a cache hit logs again).
NodeIndex BuildCatalogIndex(const nlohmann::ordered_json &GlobalConfigJSON, std::string *Inputs = nullptr,
                            std::vector<Diagnostics::Entry> *Said = nullptr);
// The index the cache last kept, whatever changed in the library since (Inputs: what it was built from) — for a start
// that shows it at once and checks it after, whichever version wrote it: nothing when there is none of this shape.
std::optional<NodeIndex> LastCatalogIndex(std::string *Inputs = nullptr, std::vector<Diagnostics::Entry> *Said = nullptr);
// BuildCatalogIndex, unless the library is still what Have (an index's Inputs) says: then nothing, and no read.
std::optional<NodeIndex> RefreshCatalogIndex(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &Have,
                                             std::string *Inputs = nullptr, std::vector<Diagnostics::Entry> *Said = nullptr);
// What of the library was on disk when it was last worked out (Installed::HydrationMap), kept beside the index cache
// (<data dir>/CACHE/hydration.cbor) so a start can show the Library before stat'ing every content file again.
std::optional<std::unordered_map<std::string, Installed::NodeHydration>> LastHydration();
void SaveLastHydration(const std::unordered_map<std::string, Installed::NodeHydration> &Hydration);
// The same, always from the files — never the cache (what the cache is checked against). BuildCatalogIndex is what to
// call.
NodeIndex ScanCatalogIndex(const nlohmann::ordered_json &GlobalConfigJSON);
// The on-disk index cache (<data dir>/CACHE/index.cbor): whether it is there, how big, and whether it matches the
// library as it is now (every node file unchanged, this program); and its removal.
struct IndexCacheState { std::string Path; bool Exists = false, Current = false; std::uintmax_t Bytes = 0; std::size_t Files = 0; };
IndexCacheState IndexCacheStatus(const nlohmann::ordered_json &GlobalConfigJSON);
// The cached index itself when it matches the library now (what `catalog cache --check` compares), else nothing.
std::optional<NodeIndex> ReadIndexCache(const nlohmann::ordered_json &GlobalConfigJSON);
bool ClearIndexCache();   // the index cache, and the hydration kept beside it
// The index the running app holds: AppModel publishes each one it makes, with what BuildCatalogIndex gave it.
void PublishAppIndex(std::shared_ptr<const NodeIndex> Idx, std::string Inputs, std::vector<Diagnostics::Entry> Said);
// The node index as the library is on disk now, for code that has only the config (a launch prepared from the command
// line, the package editor's library, an authoring session): the app's own when it is up and no node file changed
// since it built it — a stat of each file, no read — else a build. One index in the app instead of one per reader.
// Either way it logs what building that index logged, as a build of its own would (a launch's verdict counts them).
std::shared_ptr<const NodeIndex> CurrentIndex(const nlohmann::ordered_json &GlobalConfigJSON);
// A catalog index built AFTER the call began, shared: callers waiting at once get one scan between them. Every download
// rebuilt the whole index itself (≈8 s, ≈17 MB) after landing closures and after installing — 46 resumed downloads
// ran up to 138 scans at once: 1700 % CPU, and 3 GB of heap glibc then kept. Any caller's config is used (they name
// the same library).
std::shared_ptr<const NodeIndex> FreshCatalogIndex(const nlohmann::ordered_json &GlobalConfigJSON);
// Inverse of Download: delete the launchable's OWN content-layer files — those in its package folder that carry a CID
// (keep manifests + cover; a library another package holds and a file nothing could download again are left) — and
// unpin + drop-ref their CIDs, so the package returns to the Catalog as re-downloadable and the node stops seeding it.
// Returns the number of files removed. Use only for managed (library-root) packages, never local/portable ones.
int DehydrateNode(const NodeIndex &Idx, const std::string &LaunchNodeId);
//The packages installed here, from the index: every package folder (LIBRARY's, or added locally; never a peer's
//CATALOG stubs) with a launchable whose content is all on disk — Settings → Installed Packages. Uid and Title are its
//identity as ScanBundleIdentity tells it (no disk read: the index has the nodes); Variants its shelf rows; Local when
//it was added from outside LIBRARY; Bytes what its folder holds on disk (with Sizes; a walk of each folder).
//HasEntry: the config's LIBRARY list names it (a package added from outside, or a package source's) — what Remove takes.
struct InstalledPackage { std::filesystem::path Dir; std::string Uid, Title, Library; bool Local = false, HasEntry = false;
                          std::vector<std::string> Variants; std::uintmax_t Bytes = 0; };
//What uninstalling a package does, judged before anything is touched (Settings → Installed Packages, `app remove-package`):
//  Forget    — added from outside LIBRARY: its entry goes, its folder is untouched;
//  Download  — its content goes and it stays in the Catalog, to download again From a source of the user's or a peer that
//              shares this very version (every node of it);
//  OnlyCopy  — nothing could download it again: its folder goes only when the user says so, and then to the trash;
//  Nothing   — no package is there.
//NeededBy: the other installed packages whose content lives in its folder (a library they contain) — what its content
//going breaks, or what refuses deleting it. Shared: the user's own share sheet lists it (peers download it from here).
struct UninstallVerdict
{
    enum class Kind { Nothing, Forget, Download, OnlyCopy } K = Kind::Nothing;
    std::string From;
    std::vector<std::string> NeededBy;
    bool Shared = false;
};
UninstallVerdict JudgeUninstall(const nlohmann::ordered_json &GlobalConfigJSON, const NodeIndex &Idx, const std::filesystem::path &Dir);
//Deletes the package folder Dir the only way a slip can be undone: moved to the trash (refused when it cannot be — a
//file system with none), then unpins and drops the references of its node files and its own content (what lies in it:
//another package's library is not its own). The user's saves are not in it. Blocks (IPFS): call off the GUI thread.
[[nodiscard]] bool TrashPackage(const NodeIndex &Idx, const std::filesystem::path &Dir, std::string *Error = nullptr);
//The nickname of a peer whose shared libraries list every node of the installed package at Dir (it can be downloaded
//again from them), "" when none does.
std::string PeerSharingPackage(const nlohmann::ordered_json &GlobalConfigJSON, const std::filesystem::path &Dir);
std::vector<InstalledPackage> InstalledPackages(const nlohmann::ordered_json &GlobalConfigJSON, const NodeIndex &Idx,
                                                bool Sizes = true);
//The package folders (of the index) whose identity UID is Uid, or whose folder name begins with "[<Uid>]" — what a
//package named by its UID means.
std::vector<std::filesystem::path> PackageDirsWithUid(const NodeIndex &Idx, const std::string &Uid);
//Gather (without fetching) a runner node's missing build-layer download targets, appending to Out — so one download
//batch pools a game's content with its runners' builds. False (with *Error) only for a node that is not a runner;
//"nothing to fetch" is success with Out unchanged.
bool CollectRunnerNodeTargets(const NodeIndex &Idx, const std::string &RunnerNodeId,
                              std::vector<IpfsWrapper::FetchTarget> &Out, std::string *Error = nullptr);
// Every distinct ipfs CID a launchable node's content closure must fetch (its layers' SOURCE CIDs). Drives the
// Catalog download-progress aggregation. Excludes the runner build; cover CIDs are not included. The whole closure is
// counted (options included: a download is whole chains); Grafts adds the chosen grafts' content.
std::vector<std::string> NodeContentCids(const NodeIndex &Idx, const std::string &LaunchNodeId,
                                         const GraftChoice &Grafts = std::nullopt);


// Gather (without fetching) every missing content-layer + cover target for a launchable's closure, appending to Out.
// Lets a caller pool a game's content with its runners' build layers into ONE concurrent download batch. Returns
// false (with *Error) if a required layer is locally missing AND has no IPFS source. Also seeds an already-present
// cover by reference (best-effort) so a downloader serves it too.
// The stamped SOURCE.SIZE per content CID of a launchable's selection — instant + offline (sizes-in-JSON); only
// stamped entries are returned, the caller network-probes the rare unstamped rest.
std::map<std::string, long long> NodeContentSizes(const NodeIndex &Idx, const std::string &LaunchNodeId,
                                                  const GraftChoice &Grafts = std::nullopt);

// The launchable's RESOLVED runner-chain node ids (native terminal excluded), a graft's being the version's it applies
// onto: the default entry's chain — what the launch runs on, and what a download gates and adopts by — or, with
// EveryEntry, every entry's merged (a native and a win32 build each run on their own chain): what a download fetches,
// best-effort, so whichever entry is picked can run.
std::vector<std::string> RunnerChainIds(const NodeIndex &Idx, const std::string &LaunchNodeId,
                                        const nlohmann::ordered_json &GlobalConfigJSON, bool EveryEntry = false);

[[nodiscard]] bool CollectContentTargets(const NodeIndex &Idx, const std::string &LaunchNodeId,
                           const GraftChoice &Grafts,
                           std::vector<IpfsWrapper::FetchTarget> &Out, std::string *Error = nullptr);
// Gather (without fetching) the build download targets of every entry's runner chain (RunnerChainIds), appending
// to Out — so a download pulls the game's runtime (JRE / Proton build) alongside its content. Best-effort: a runner
// whose build cannot be collected is warned about, and the game content still downloads.
void CollectRunnerChainTargets(const NodeIndex &Idx, const std::string &LaunchNodeId,
                               const nlohmann::ordered_json &GlobalConfigJSON, std::vector<IpfsWrapper::FetchTarget> &Out);
// The ONE download: the selected launchables (their content, the grafts ticked, each one's runner chain build) and the
// runners asked for, fetched in one concurrent batch. Refused first when a closure reaches a package that has not landed,
// or content names no source; then every received package the closures reach is adopted into the library (in Config,
// a copy) and the index rebuilt before a byte moves. Worker thread (blocks). The Catalog's download, `packages
// hydrate` and --fetch all come here. False with *Error on any failure.
[[nodiscard]] bool Download(std::shared_ptr<const NodeIndex> Idx, const std::vector<std::string> &LaunchIds,
                            const std::vector<std::string> &RunnerIds, const GraftChoice &Grafts,
                            nlohmann::ordered_json Config, std::string *Error = nullptr);

} // namespace PackageCatalog

#endif // PACKAGECATALOG_H
