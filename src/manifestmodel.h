#ifndef MANIFESTMODEL_H
#define MANIFESTMODEL_H

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <functional>
#include <filesystem>

// ---------------------------------------------------------------------------
// ManifestModel — pure, stateless queries over a package MANIFEST (no launch session, no instance state).
//
// Two groups: (1) VFS-layer helpers that centralise the "is this a content layer / where does it live" logic
// that used to be copy-pasted across the container code; (2) game/variant/module/component lookups and the
// manifest-only package predicates (hydrated / has-content / referenced CIDs). The launch engine
// (ContainerWrapper) and the sharing service (PackageCatalog) both build on these.
// ---------------------------------------------------------------------------

//A toggleable build module: a reference to a component (leaf OR internal node) the user can enable or disable.
//REQUIRED modules are always in the recipe and locked in the UI; optional ones (REQUIRED:false) are user-toggled,
//starting at DEFAULT. Used identically by game variants and runners (universal schema).
struct ModuleInfo {
    std::string Component;        // COMPONENT — the component id this module pulls in
    std::string Label;            // optional LABEL for the tree (falls back to component NAME/id)
    bool        Required = true;  // REQUIRED — defaults true (modules are required unless opted out)
    bool        Default  = true;  // DEFAULT — initial enabled state when optional; defaults true
    std::vector<std::string> Exclude; // EXCLUDE — component ids this module is mutually exclusive with (symmetric)
};

//One available variant for a subgame (or a runner). Selecting a variant = selecting which MODULES to build.
struct VariantInfo {
    std::string VariantID;             // VARIANT_ID field on the variant
    std::string Name;                  // optional NAME; falls back to VARIANT_ID for display
    bool        IsRecommended = false; // RECOMMENDED:true — shown with ⭐ in the picker
    std::string HostPlatform;          // HOST_PLATFORM — per-variant (game: target platform; runner: host OS)
    std::vector<std::string> GuestPlatform; // GUEST_PLATFORM — runner variants only (guests this variant serves)
    std::vector<ModuleInfo> Modules;   // MODULES array (toggleable component references, load order)
};

// ---------------------------------------------------------------------------
// The unified node graph ("everything is a node"). The whole library is ONE flat graph of globally-
// referenceable nodes; each node lives in its own <node_id>.json file inside a "bundle" directory
// (the old package dir, which just groups node files + their shared content). A Node is equivalent for
// every former taxonomy level (game / variant / component / runner / subcomponent-owner): it either
// groups/selects other nodes (via PARENTS) or contributes concrete layers (via LAYERS), or both.
// ---------------------------------------------------------------------------

//One node, parsed from a <node_id>.json file. Edges are bare global NODE_IDs in Parents (later = higher
//CFS priority). Selection attributes (Optional/Default/Exclude) live on the node itself, not on the edge.
struct Node {
    std::string NodeId;                      // NODE_ID — globally unique bare slug (e.g. "aoe2_aok_base", "wine")
    //Non-empty when the node's payload could not be lowered (unknown TYPE, unknown FORM, malformed EDITS…).
    //Such a node is STILL INDEXED, deliberately: dropping it made the whole node vanish, so a leaf mistake —
    //an unknown FORM on a Content node, a typo'd TYPE — was reported by NOTHING. --validate-nodes printed a
    //perfect package while a layer had silently disappeared. It is indexed so validation can name it and so
    //resolution can refuse to launch through it; it contributes NO layers, so it can never be applied.
    std::string LowerError;
    //The node's own WHEN as authored. Kept because a payload-less node (Group) emits no layer for the
    //condition to ride on, so validation has nothing else to see it in — and a silently-dropped condition is
    //a module that applies unconditionally.
    std::string RawWhen;
    // Identity is DERIVED from the node's Declare* layers (no ROLE field): DeclareExec ⇒ launchable, DeclareRunner ⇒
    // runner, DeclareLibraryItem ⇒ a library tile. The fields below are populated by ParseNode from those layers (or,
    // transitionally, from the legacy top-level ROLE/EXEC/META/PLATFORM until packages are migrated).
    bool HasExec   = false;                  // a DeclareExec layer (or legacy ROLE:launchable) ⇒ this node is launchable
    bool HasRunner = false;                  // a DeclareRunner layer (or legacy ROLE:runner) ⇒ this node is a runner
    std::string Uid;                         // UID — numeric id for presentable nodes (from DeclareLibraryItem)
    std::string Game;                        // the game-node id this variant belongs to (linked by LinkGames via the
                                             // DeclareLibraryItem ancestor edge; "" ⇒ self = single-variant tile)
    std::string Label;                       // variant label for the picker (DeclareExec.LABEL)
    bool Recommended = false;                // RECOMMENDED — the default variant within its game (DeclareExec.RECOMMENDED)
    std::string RecommendedRunner;           // RUNNER — a runner node id this launchable recommends (DeclareExec.RUNNER); a
                                             // soft, package-side default that seeds USERSETTINGS.PREFERRED_RUNNER (register.
                                             // time) — NOT a runner-pick tier; the user still overrides in the picker
    nlohmann::ordered_json Meta;             // the library tile metadata (DeclareLibraryItem); inherited onto variants by LinkGames
    std::string HostPlatform;                // DeclareExec.PLATFORM (launchable) or DeclareRunner.HOST
    std::vector<std::string> GuestPlatform;  // DeclareRunner.GUEST[]
    nlohmann::ordered_json Exec;             // the resolved invocation block: DeclareExec (CONTENTPATH/…) or DeclareRunner (EXECUTABLE/…)
    bool Optional = false;                   // OPTIONAL — a toggleable add-on when referenced as a parent
    bool Default  = true;                    // DEFAULT — initial enabled state when OPTIONAL
    std::vector<std::string> Exclude;        // EXCLUDE — node ids mutually exclusive with this one (symmetric)
    std::vector<std::string> Parents;        // PARENTS — bare global node ids (load order: later = higher priority)
    nlohmann::ordered_json Layers;           // LAYERS — contribution payloads (array of TYPE-tagged objects, incl. Declare*)
    std::filesystem::path File;              // source <node_id>.json path
    std::filesystem::path BundleDir;         // owning bundle dir — content PATHs inside LAYERS resolve here

    bool Presentable()  const { return Meta.is_object() && !Meta.empty(); }
    bool IsRunner()     const { return HasRunner; }
    bool IsLaunchable() const { return HasExec; }
    std::string GameKey() const { return Game.empty() ? NodeId : Game; }   // the game this variant belongs to (library-tile key)
};

//The global node graph: NODE_ID → Node, built by scanning bundle dirs for node files.
struct NodeIndex {
    std::map<std::string, Node> Nodes;
    const Node *Find(const std::string &NodeId) const;
};

namespace ManifestModel {

// ----- node graph (schema: everything is a node) -----
// Parse a single node object (already-loaded JSON) sourced from File in BundleDir. False if it has no NODE_ID.
bool ParseNode(const nlohmann::ordered_json &J, const std::filesystem::path &File,
               const std::filesystem::path &BundleDir, Node &Out);
// Scan one bundle dir (non-recursive) for *.json node files (those carrying NODE_ID), adding them to Idx.
// Duplicate NODE_IDs are reported and the first-seen wins.
void ScanBundleNodes(const std::filesystem::path &BundleDir, NodeIndex &Idx);
// Build the global index from library roots; each root holds bundle dirs (one level down). Runs LinkGames.
// Scans each LibraryRoot's immediate subdirectories as bundles, plus any ExtraBundleDirs as bundles DIRECTLY (a
// locally-added package's own dir, not a root of bundles), then links games. ExtraBundleDirs lets externally-added
// local packages (their PATH is the bundle itself) be indexed alongside the repo-rooted ones.
NodeIndex BuildNodeIndex(const std::vector<std::filesystem::path> &LibraryRoots,
                         const std::vector<std::filesystem::path> &ExtraBundleDirs = {});

// Post-parse pass over a fully-assembled index: link each launchable variant that lacks its own library metadata to
// its game node (the nearest DeclareLibraryItem ancestor via PARENTS) — setting its Game key and inheriting the tile's
// Meta/UID — so the catalog/library group + present variants under one tile by a graph edge, not a GAME string. Called
// by BuildNodeIndex and must be re-run by any caller that assembles an index manually (ScanBundleNodes).
void LinkGames(NodeIndex &Idx);

// Field-level last-wins composition of an object-shaped identity layer across a node's resolved closure. Walks
// ResolveNodeOrder (parents first, the node itself last = highest priority) and merges, key-by-key, the object
// returned by Pick() for every node it accepts (Pick returns nullptr to skip a node). The one merge mechanism
// behind both the launch-time DeclareExec composition (a base supplies CONTENTPATH, a variant overrides EXEARGS)
// and the index-time DeclareLibraryItem/Meta inheritance (a variant inherits its tile's TITLE/COVER) — the two
// object-shaped Declare* layers. Mirrors how CustomVar/Persist aggregate across the same closure.
nlohmann::ordered_json ComposeAcrossClosure(
    const NodeIndex &Idx, const std::string &NodeId, const std::map<std::string, bool> &Toggles,
    const std::function<const nlohmann::ordered_json *(const Node &)> &Pick);

// Resolve the load-ordered node closure for launching LaunchNodeId: walk PARENTS across the global graph,
// keeping required parents always and optional ones per Toggles (else DEFAULT), applying EXCLUDE (symmetric,
// first-kept wins) and the hierarchy gate (a node only enters if a kept child pulls it). Output is topo-ordered
// parents-before-children, so the launchable is LAST (= highest CFS priority); PARENTS list order is the
// tie-break (later parent = higher). Detects cycles. Any PARENTS id missing from Idx is appended to Missing.
std::vector<std::string> ResolveNodeOrder(const NodeIndex &Idx, const std::string &LaunchNodeId,
                                          const std::map<std::string, bool> &Toggles,
                                          std::vector<std::string> *Missing = nullptr);

// Visits every resolvable node in RootId's closure (ResolveNodeOrder order) EXCEPT RootId itself — the
// "walk a runner's content closure" skeleton that used to be hand-rolled at four call sites (RunnerBuildNodes,
// RunnerShipsBuild, BuildLink, InitializeFromNode). Each caller keeps its own per-node filter/body.
void ForEachClosureNode(const NodeIndex &Idx, const std::string &RootId,
                        const std::map<std::string, bool> &Toggles,
                        const std::function<void(const Node &)> &Visit);

// The OPTIONAL nodes reachable from a launchable via the PARENTS closure (for the picker's toggle list),
// in discovery order. Runner nodes are excluded. Gating/EXCLUDE between them is resolved at launch by
// ResolveNodeOrder; the picker just lists them as checkboxes.
std::vector<const Node*> OptionalNodes(const NodeIndex &Idx, const std::string &LaunchNodeId);

// The DOWNLOAD units of a tile, derived from its CONTENT structure (not raw graph sinks — every launchable variant
// is typically a graph sink, e.g. MC's content-free per-version "_game" wrappers hanging off the delta chain).
// Ticking a row means downloading the closures of its Ids; the rows together always cover the tile's whole required
// content (all ticked = "everything"). Rows come from a GREEDY SET-COVER over the content bitsets: each pick is the
// variant covering the most still-uncovered content — so nesting collapses (a delta chain is one pick) and genuine
// branches (divergent editions, forked snapshot chains) each get a row. Real graphs also carry many small unique-
// content leaves (per-era natives bundles, wrapper scripts) whose "owning" variants would make meaningless rows —
// picks beyond the first few are folded into a single "Everything else" row. Content reached only through Optional
// nodes never surfaces here (that's the optional-content section's domain). LaunchableCount = how many of the
// tile's variants the row's first pick DOMINATES (content ⊆ its content) — the "(270 versions)" hint; 0 on the
// folded row. Cost: one shared-visited union walk + bitset closure algebra — never per-variant walks.
struct EndpointInfo {
    std::vector<std::string> Ids;  // the variant(s) this row downloads (closure union); 1 for real picks, many when folded
    std::string Label;             // first pick's Label (else Meta.TITLE, else id), or "Everything else"
    int         LaunchableCount = 0;
};
std::vector<EndpointInfo> TileEndpoints(const NodeIndex &Idx, const std::vector<std::string> &VariantIds);

// Validate the whole node graph: every PARENTS id resolves + no cycles; VFS layers declare a PATH; each
// launchable resolves a runner (platform match) + has content or is self-contained; runner nodes declare GUEST
// platforms; a PREFIX_GENERATE runner routes content through drive_c; EXCLUDE is symmetric. Errors should block
// (e.g. in the editor/CI), warnings advise. NODE_ID uniqueness is enforced earlier by the index (dups dropped).
// When OnlyNodes is non-null, only those node ids are validated (the rest of Idx is still consulted for cross-
// references like runner availability and PARENTS) — used by the launch gate to validate just the package being
// launched, not the whole catalog.
void ValidateNodeGraph(const NodeIndex &Idx, std::vector<std::string> &Errors, std::vector<std::string> &Warnings,
                       const std::set<std::string> *OnlyNodes = nullptr);

// Resolves cross-layer case conflicts (the FindCrossLayerCaseCollisions errors) by canonicalizing the
// higher-priority layers' zip entries to the base layer's case (unpack→rename→repackage STORE, same
// structure; the base zip is never touched). Canonicalization always uses the full index (correct base-case
// resolution); ScopeDir, if given, restricts which zips are actually rewritten to those under that directory
// (so the Package Editor button fixes only the open bundle). Returns the number of zips rewritten; Log gets a
// human report. Used by --fix-case-conflicts and the Package Editor "Fix case conflicts" action.
int FixCaseConflicts(const NodeIndex &Idx, std::vector<std::string> &Log,
                     const std::filesystem::path *ScopeDir = nullptr);

// ----- VFS layer helpers (the dedup foundation) -----
// True if every file entry in the zip at ZipPath is STOREd (uncompressed) — the form VidyaGodFS can mount (it
// reads entries at their backing offset and cannot inflate DEFLATE). A missing/unreadable zip is treated as true
// (other checks cover presence). If FirstCompressed is non-null, it receives the first compressed entry's name.
bool ZipFullyStored(const std::string &ZipPath, std::string *FirstCompressed = nullptr);
// The vidyagodfs mount-spec type for a package layer TYPE ("zip"/"dir"/"file"/"delta"), "" if not a VFS layer. The
// single source of truth for the layer→spec kind, shared by IsVfsLayer and every mount-spec builder.
std::string VfsSpecType(const std::string &Type);
// True if a subcomponent TYPE string names a VFS content layer (Zip/Dir/File/Delta).
bool IsVfsLayer(const std::string &Type);

//True when a VFS layer's SOURCE is a LIVE RUNTIME PATH rather than package content on disk — i.e. its PATH
//(or SOURCE.PATH) carries a %variable%, e.g. a runner's prefix-assembly mount from "%DefaultPfxDir%" or
//"%RunnerMount%/files/share/default_pfx". Such a layer resolves at BuildLayerSpec time, so it has nothing to
//hydrate, fetch or verify, and it is not part of a runner's importable BUILD.
//This used to be implicit: a runner's prefix-assembly layers sat on the same node as its DeclareRunner, so the
//"skip runner nodes" rule in the chain walk excluded them as a side effect. One node per layer makes that
//coincidence impossible, so the property is stated directly here and applied wherever build content is counted.
bool IsRuntimeSourcedLayer(const nlohmann::ordered_json &Sub);

//The %Name% tokens in a path: a matched pair of '%' around a non-empty identifier ([A-Za-z_][A-Za-z0-9_]*)
//containing no path separator. This is the tokenizer for the "is this path runtime-sourced?" question, and
//every asker of THAT question must use it. It is deliberately stricter than VarSubst's substitution scanner,
//which also accepts the "%KEY:format%" render syntax — a path is never rendered, so a ':' there is a filename. Exposed because IsRuntimeSourcedLayer is
//not the only question asked of a templated path — "which variables would this need?" is another — and a
//SECOND hand-rolled scanner is how the two answers drifted: one classified "100%25%20done.zip" as real
//content while the other called it a runtime mount, so a fetchable-missing layer reported as hydrated.
std::vector<std::string> PathVariableTokens(const std::string &P);

//A VFS layer's local path string, exactly as IsRuntimeSourcedLayer reads it (SOURCE.PATH overrides PATH).
std::string LayerPathString(const nlohmann::ordered_json &Sub);

//THE question every site that walks a runner's content closure is actually asking: "is this layer part of the
//runner's BUILD?" — real bytes to fetch, stat, import and mount at %RunnerMount%. It is `IsVfsLayer(TYPE) &&
//!IsRuntimeSourcedLayer`, and it exists as one named function because spelling that conjunction out at each call
//site is a bug waiting to happen: five sites ask it, the flat schema turned the prefix-assembly layers into
//ordinary Content nodes INSIDE the closure, and two sites that forgot the second half started reporting every
//GE-Proton runner as not installed — which kills the Play button on every Windows game with no diagnostic.
//Any NEW site that walks a runner's build MUST call this rather than re-deriving it.
bool IsRunnerBuildLayer(const nlohmann::ordered_json &Sub);
// A subcomponent's TYPE ("" if absent).
std::string LayerType(const nlohmann::ordered_json &Sub);
// Normalize a layer TARGET / runtime-relative path: backslashes → slashes, leading/trailing slashes trimmed.
// This is the parent-side twin of VidyaGodFS's NormalizeVPath (layerspec.cpp) MINUS the FS's zip-name context —
// the two must agree so target strings match the FS's per-target base map (a parity test pins that; the FS side
// additionally never sees backslashes, its inputs being zip entry names). Was inlined ×3 across the engine.
std::string NormalizeTargetPath(std::string P);
// The ONE answer to "which key holds this delta's byte-bases, and in what order?". A delta may be based on the
// CONCATENATION of several composed views (a wine build ‖ a DXVK build ‖ the previous prefix), so the answer is
// always a LIST, under ONE key: `BASE_TARGETS`. There is no singular spelling anywhere in the format — two
// spellings for one idea is how the plural came to be emitted, documented and never read by the mounter.
// Returns the DECLARED strings, unresolved and unsubstituted — each caller applies its own target resolution.
// Empty for a non-delta layer, and for a delta with no declared base (whose base is implicitly the composed view
// at its own TARGET). Every question of this shape asked separately per call site has broken separately; this one
// is asked once. Order is load-bearing: it must match what generation concatenated.
std::vector<std::string> LayerBaseTargets(const nlohmann::ordered_json &Sub);
// True when a string still holds an unresolved %TOKEN% after substitution. The single definition of "surviving
// token" — the mount builders and --audit-packages must agree, or one reports a launch broken that the other
// calls clean.
bool HasLiveToken(const std::string &S);
// Build one vidyagodfs spec-layer object from a package VFS layer, with caller-resolved Source/Target (and, for a
// delta, its resolved byte-BASES in order). Null json if `Sub` is not a VFS layer. Centralizes the entry skeleton so
// mount builders never drift.
//
// A delta's bases go on the wire as ONE key, `baseTargets`, however many there are — an empty string is a real
// target (the VFS root), so a singular key could not distinguish "based at the root" from "no base declared".
nlohmann::ordered_json MakeVfsSpecLayer(const nlohmann::ordered_json &Sub, const std::string &Source,
                                        const std::string &Target,
                                        const std::vector<std::string> &BaseTargets = {});
// Invokes Fn on every VFS-layer subcomponent across a COMPONENTS array (any json shape is tolerated).
void ForEachVfsLayer(const nlohmann::ordered_json &Components,
                     const std::function<void(const nlohmann::ordered_json &Sub)> &Fn);
// A VFS layer's locators: its expected LOCAL path (PackagePath/PATH, SOURCE.PATH overriding) and its ipfs CID
// (empty if none). The local path is the content's home; the CID is a remote fallback to fetch it from.
void LayerLocator(const nlohmann::ordered_json &Sub, const std::filesystem::path &PackagePath,
                  std::filesystem::path &Local, std::string &Cid);
// Convenience: a layer's resolved LOCAL absolute path (PackagePath/PATH). Pure, local-only (no cache).
std::string ResolveLayerSource(const nlohmann::ordered_json &Sub, const std::filesystem::path &PackagePath);

// ----- platform -----
std::string MachinePlatform();   // the host platform a runner variant's HOST_PLATFORM must match (e.g. "linux64")
std::string HostPlatform();      // the platform token the host runs as (Phase-1 stub: "linux64")

// ----- game / component / variant / module lookups -----
int FindGameIndex(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID);     // index or -1
int FindComponentIndex(const nlohmann::ordered_json &MANIFESTJSON, const std::string &ComponentID); // index or -1
std::vector<VariantInfo> GetAvailableVariants(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID);
std::vector<ModuleInfo>  GetVariantModules(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID, const std::string &VariantID);
std::vector<ModuleInfo>  ParseModules(const nlohmann::ordered_json &ModulesArray);
// Resolves which of `Modules` are enabled into component ids (load order): REQUIRED||(state?:DEFAULT), REQUIRED
// propagating up the PARENTCOMPONENT chain, EXCLUDE mutual exclusion, and the hierarchy gate. Variants AND runners.
std::vector<std::string> ResolveEnabledModules(const std::vector<ModuleInfo> &Modules,
                                               const std::map<std::string, bool> &ModuleStates,
                                               const nlohmann::ordered_json &MANIFESTJSON);
// Convenience: a variant's default-enabled module components (REQUIRED||DEFAULT, no overrides).
std::vector<std::string> FindEndpointsForVariant(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID, const std::string &VariantID);

// ----- manifest-only package predicates -----
// Every distinct ipfs CID referenced by the package's content layers (the set an install must fetch).
std::vector<std::string> PackageIpfsCids(const nlohmann::ordered_json &Manifest);
// Every distinct cover-art CID (GAMES[].METADATA.COVER / legacy game-level COVER) — drives the IPFS "Assets" group.
std::vector<std::string> PackageCoverCids(const nlohmann::ordered_json &Manifest);
// True when every VFS content layer is present locally at its expected path (vacuously true if there are none).
bool PackageHydrated(const nlohmann::ordered_json &Manifest, const std::string &PackageDir);
// True iff the manifest declares any VFS content layer (distinguishes a real package from a content-less one).
bool PackageHasContent(const nlohmann::ordered_json &Manifest);

} // namespace ManifestModel

#endif // MANIFESTMODEL_H
