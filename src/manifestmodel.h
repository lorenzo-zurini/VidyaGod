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
// The one-edge graph ("everything is a node"). The whole library is ONE flat graph of globally-referenceable
// nodes; each node lives in its own .json file inside a "bundle" directory (which just groups node files + their
// shared content). There is ONE node kind and ONE edge:
//
//   node = facets (CID LABEL WHEN TOGGLE PUBLISH) + TILE + ENTRYPOINTS + payload arrays + OVER
//
// A node is one meaningful change, which may span kinds (a widescreen fix = PATCHES + FILEEDITS + VARS in one
// node). A node with no payload is just a node. Everything else is DERIVED, never stored: launchable (has
// ENTRYPOINTS), identity (own TILE, else inherited through OVER), runner (an entrypoint with GUEST), graft (in
// nobody's list), canonical (a lint).
//
// OVER = "I am made of you; you are under me" — a CNF list of requirements: a CID (must be present), a list of
// CIDs (any one), {"NOT": cid} (must be absent). Order is the mount order among a node's own entries (later =
// higher). No other edge exists.
//
// Selection ≠ closure: what the user CHOOSES (the launchable, a member per any-of group, ticked grafts) is the
// selected set; the MOUNT is its closure over plain OVER entries; a graft is OFFERED when the selected set
// satisfies its identity-bearing requirements; execution runs an entrypoint of a SELECTED node only — a plain
// OVER entry is never a choice and never a branch point (1.16.5 OVER [1.16.4] puts 1.16.4's bytes under you
// and nothing else: not its entrypoint, not its mods).
// ---------------------------------------------------------------------------

//One requirement of a node's OVER list. Plain entry: Any = {cid}. Any-of group: Any = {cid, cid, …}.
//Exclusion: Not = true, Any = {cid}.
struct OverReq {
    std::vector<std::string> Any;
    bool Not = false;
    bool IsGroup() const { return !Not && Any.size() > 1; }
};

//One node, parsed from a node .json file.
struct Node {
    // Cid — the node's IDENTITY in the gigagraph: the CID of its canonical dag-json block, computed recursively over
    // the CIDs it links (OVER/SOURCE/COVER). Empty for a working-tree node not yet frozen. When set, the index is
    // keyed by this (NodeId demotes to a human label). Set by the DAG traversal / disk ingest, never parsed from the
    // block (a block does not contain its own CID). See [[gigagraph]] plan + nodegraph.cpp.
    std::string Cid;
    std::string NodeId;                      // LABEL — the optional cosmetic name (may repeat; never a key)
    //Non-empty when the node could not be lowered (unknown field, unknown FORM, malformed EDITS…).
    //Such a node is STILL INDEXED, deliberately: dropping it made the whole node vanish, so a leaf mistake was
    //reported by NOTHING. It is indexed so validation can name it and so resolution can refuse to launch through
    //it; it contributes NO layers, so it can never be applied.
    std::string LowerError;
    //The node's own WHEN as authored. Kept because a payload-less node emits no layer for the condition to ride
    //on, so validation has nothing else to see it in — and a silently-dropped condition applies unconditionally.
    std::string RawWhen;

    // ---- ENTRYPOINTS (launchable iff non-empty). Each entry is a variant: {LABEL, HOST, PATH, ARGS, ENV, ENV_REMOVE,
    // WORKDIR, RECOMMENDED, RUNNER} for a launchable; a runner entry additionally carries GUEST (+ CONTENT_ROOT /
    // PREFIX_GENERATE / UNIFIED_RUNTIME). The fields below are the DEFAULT entry's view (the first RECOMMENDED one,
    // else the first), lowered to the exec block the engine consumes; ExecFor() selects another entry by LABEL. ----
    nlohmann::ordered_json Entrypoints;      // the raw ENTRYPOINTS array (empty array when none)
    bool HasExec   = false;                  // an entrypoint without GUEST ⇒ this node is launchable
    bool HasRunner = false;                  // an entrypoint with GUEST ⇒ this node is a runner
    std::string Label;                       // the default entrypoint's LABEL (else the node LABEL)
    bool Recommended = false;                // the default entrypoint's RECOMMENDED
    std::string RecommendedRunner;           // the default entrypoint's RUNNER — a soft package-side runner default
    std::string HostPlatform;                // the default entrypoint's HOST
    std::vector<std::string> GuestPlatform;  // the default entrypoint's GUEST[] (runner)
    nlohmann::ordered_json Exec;             // the default entrypoint lowered: CONTENTPATH/EXEARGS/PLATFORM/… or
                                             // EXECUTABLE/ARGS/HOST/GUEST/… for a runner (see NodeLower::LowerEntrypoint)

    // ---- TILE / identity. A launchable carries its own TILE {UID, PARENTUID?, TITLE, COVER, META…}; every other node
    // inherits identity from what it is OVER. Uids = every UID this node belongs to (a mod for two games has two);
    // Uid = the first (its grouping key); Meta = the tile fields, flat (own, else the first inherited tile's). ----
    bool OwnTile = false;                    // carries a TILE of its own
    std::string Uid;                         // primary identity (own TILE.UID, else the first inherited)
    std::vector<std::string> Uids;           // every identity, in OVER order
    std::string ParentUid;                   // TILE.PARENTUID — the main game this title nests under ("" = a main game)
    nlohmann::ordered_json Meta;             // tile metadata, flat (TITLE/COVER/UID/PARENTUID + META fields)

    bool Optional = false;                   // TOGGLE present ⇒ user-toggleable
    bool Default  = true;                    // TOGGLE value ("on"/"off") — the author's default state
    bool Publish  = false;                   // PUBLISH — a share-list root

    std::vector<OverReq> Over;               // OVER — the one edge, as authored (CNF)
    std::vector<std::string> Parents;        // every node OVER names POSITIVELY (plain entries + group members), flat,
                                             // in list order — the reference set for validation/freeze/hydrate/canvas
    std::vector<std::string> Excludes;       // every node OVER names under NOT
    nlohmann::ordered_json Layers;           // the lowered payload: the executor's ordered layer sequence
    std::filesystem::path File;              // source .json path
    std::filesystem::path BundleDir;         // owning bundle dir — content PATHs inside LAYERS resolve here

    // The node's key in a NodeIndex: its CID in the gigagraph catalog (identity), or its LABEL in a legacy single-
    // bundle scan (Cid unset). Use this — never NodeId directly — whenever an id must index back into the catalog
    // (hydration maps, launch ids, download ids), or a CID-keyed index and a NodeId lookup silently miss.
    std::string Key() const { return Cid.empty() ? NodeId : Cid; }
    bool Presentable()  const { return Meta.is_object() && !Meta.empty(); }
    bool IsRunner()     const { return HasRunner; }
    bool IsLaunchable() const { return HasExec; }
    bool HasIdentity()  const { return !Uids.empty(); }
    std::string GameKey() const { return Uid.empty() ? NodeId : Uid; }   // the tile this node belongs to (group-by-UID key)
    // The lowered exec block of the entrypoint labelled `Label` ("" ⇒ the default entry). Null json if no such entry.
    nlohmann::ordered_json ExecFor(const std::string &Label) const;
    // The entrypoint labels, in ENTRYPOINTS order (a nameless entry reads as its index).
    std::vector<std::string> EntrypointLabels() const;
};

//The global node graph: NODE_ID → Node, built by scanning bundle dirs for node files.
struct NodeIndex {
    std::map<std::string, Node> Nodes;
    const Node *Find(const std::string &NodeId) const;
};

namespace ManifestModel {

// ----- node graph (schema: everything is a node) -----
// True iff J is a node object: an object carrying at least one node field (CID/LABEL/OVER/TILE/ENTRYPOINTS or a
// payload array). The ONE definition of "is this JSON a node" — every scanner of node files must use it. A legacy
// TYPE-bearing object is NOT a node (the library is migrated, never read two ways).
bool IsNodeObject(const nlohmann::ordered_json &J);
// The node fields the format defines (the whole top-level vocabulary). Anything else on a node is refused at lower.
const std::set<std::string> &NodeFields();
// The payload array keys (LAYERS PATCHES FILEEDITS REGEDITS DLLOVERRIDES VARS PERSISTS).
const std::vector<std::string> &PayloadKeys();
// Parse an OVER value (the raw JSON list) into requirements. Malformed entries are refused via *Error (if given).
// Tolerant of dag-json links already normalized to strings.
bool ParseOver(const nlohmann::ordered_json &Over, std::vector<OverReq> &Out, std::string *Error = nullptr);
// The positive refs (plain entries + group members) of a RAW node JSON's OVER, in order — for readers that walk
// node JSON without ParseNode (freeze, hydrate, publish). Negative (NOT) refs go to *Excludes if given.
std::vector<std::string> OverRefs(const nlohmann::ordered_json &J, std::vector<std::string> *Excludes = nullptr);
// Rewrite every ref in a RAW node JSON's OVER (plain, group members, NOT) through Map (unmapped refs pass
// through). Returns whether anything changed. Shared by freeze and the CID write-back so they can never disagree
// on where refs live.
bool RemapOverRefs(nlohmann::ordered_json &J, const std::function<std::string(const std::string &)> &Map);
// Parse a single node object (already-loaded JSON) sourced from File in BundleDir. False if it is not a node.
bool ParseNode(const nlohmann::ordered_json &J, const std::filesystem::path &File,
               const std::filesystem::path &BundleDir, Node &Out);
// Scan one bundle dir (non-recursive) for *.json node files, adding them to Idx. Duplicate handles are reported
// and the first-seen wins.
void ScanBundleNodes(const std::filesystem::path &BundleDir, NodeIndex &Idx);
// Build the global index from library roots; each root holds bundle dirs (one level down). Runs DeriveIdentity.
// Scans each LibraryRoot's immediate subdirectories as bundles, plus any ExtraBundleDirs as bundles DIRECTLY (a
// locally-added package's own dir, not a root of bundles). ExtraBundleDirs lets externally-added local packages
// (their PATH is the bundle itself) be indexed alongside the repo-rooted ones.
NodeIndex BuildNodeIndex(const std::vector<std::filesystem::path> &LibraryRoots,
                         const std::vector<std::filesystem::path> &ExtraBundleDirs = {});

// Post-parse pass over a fully-assembled index: derive every node's identity. A node with its own TILE is its own
// identity; every other node inherits the union of its positive OVER requirements' identities (memoized, O(N+E)).
// Sets Uid/Uids/Meta/ParentUid so the catalog/library group nodes under tiles BY UID. Called by BuildNodeIndex and
// must be re-run by any caller that assembles an index manually (ScanBundleNodes).
void DeriveIdentity(NodeIndex &Idx);

// Resolve the load-ordered node closure for launching LaunchNodeId: walk OVER across the global graph, keeping
// plain requirements always (a TOGGLE'd one per Toggles, else its DEFAULT), choosing ONE member per any-of group
// (an already-kept member, else the first present), refusing a node whose NOT names a kept node (symmetric,
// first-kept wins), and the hierarchy gate (a node only enters if a kept dependant pulls it). Toggles are keyed by
// node Key() (CID). Output is topo-ordered requirements-before-dependants, so the launchable is LAST (= highest
// CFS priority); OVER list order is the tie-break (later = higher). Detects cycles. Any ref missing from Idx is
// appended to Missing.
std::vector<std::string> ResolveNodeOrder(const NodeIndex &Idx, const std::string &LaunchNodeId,
                                          const std::map<std::string, bool> &Toggles,
                                          std::vector<std::string> *Missing = nullptr);

// ----- grafts (selection ≠ closure) -----
// A graft is a node in nobody's OVER list that is OVER something; it enters a mount only when SELECTED (Toggles[key]
// == true). It is APPLICABLE when every identity-bearing positive requirement is satisfied by the SELECTED set
// (the launchable + selected grafts; a group by any member) — never by the closure (a mod OVER [1.16.4] is a
// sibling branch off a node you did not choose when you play 1.16.5) — every identity-less requirement (a library:
// no tile, nothing tiled under it) is SUBSTANCE, satisfied by mounting, and no NOT names a selected node. The
// selected set grows to a fixpoint as grafts are ticked.
struct GraftOffer {
    const Node *Graft = nullptr;
    bool Applicable = false;       // every requirement satisfied by the selected set
    bool Selected   = false;       // ticked (Toggles), or TOGGLE "on" by default when Toggles has no entry
    std::string Blocker;           // when !Applicable: the first unsatisfied requirement (a key), for the UI
};
// Every graft with the launchable's identity, in a deterministic order, with its applicability against Toggles.
// Scope (optional) restricts candidates — the GUI passes "hydrated + in LIBRARY" so a CATALOG stub never grafts.
std::vector<GraftOffer> OfferedGrafts(const NodeIndex &Idx, const std::string &LaunchNodeId,
                                      const std::map<std::string, bool> &Toggles,
                                      const std::function<bool(const Node &)> &Scope = nullptr);
// The extra nodes a launch mounts ABOVE the launchable's own closure: the closure of every selected applicable
// graft (minus nodes already in BaseOrder), grafts ordered by Precedence (higher = later = wins) then by key.
// Returns them topo-ordered, requirements first. Substance requirements are pulled in here.
std::vector<std::string> ResolveGraftOrder(const NodeIndex &Idx, const std::string &LaunchNodeId,
                                           const std::map<std::string, bool> &Toggles,
                                           const std::vector<std::string> &BaseOrder,
                                           const std::map<std::string, int> &Precedence = {},
                                           const std::function<bool(const Node &)> &Scope = nullptr);

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
