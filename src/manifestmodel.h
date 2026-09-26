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
// that used to be copy-pasted across the container code; (2) component lookups and the
// manifest-only package predicates (hydrated / has-content / referenced CIDs). The launch engine
// (ContainerWrapper) and the sharing service (PackageCatalog) both build on these.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The node graph (MetaPackageFormat generation 6). A node is a plain JSON file {LABEL, VARIANT?, RECOMMENDED?,
// LAYERS}: an ordered list of typed layers (ZIP FILE DELTA DIR NODE EDIT REG VARS ENV DLL EXEC KEEP ANY NOT, any
// with WHEN). Parents are layers: a NODE layer CONTAINS another node, selected by TAKE and placed by TARGET. What a
// node runs, which tile it presents and what it writes are all FOLDED from its list (Fold::Resolve) — nothing is
// inherited by graph distance. A node whose list begins with ANY is a graft.
// ---------------------------------------------------------------------------

//One node, parsed from a node .json file.
struct Node {
    std::string Cid;                         // the node's identity: the CID of its canonical bytes (or, in a working-tree
                                             // scan, its stored "CID" handle)
    std::string Handle;                      // its working-tree handle (the stored "CID" of the last freeze) when the
                                             // index was frozen from the working tree — still its name there
    std::string NodeId;                      // LABEL — the optional cosmetic name (may repeat; never a key)
    //Non-empty when the node is malformed (not exactly one type key on a layer, a mistyped field…). Such a node is
    //STILL INDEXED so validation can name it; resolution never routes through it.
    std::string LowerError;

    std::string Variant;                     // VARIANT — non-empty ⇒ on the shelf under this name
    std::vector<std::string> Recommended;    // RECOMMENDED — tile UIDs: the default row under them (a variant), or
                                             // pre-ticked under them (a graft)
    nlohmann::ordered_json Json;             // the node as authored — what Fold resolves
    //The node's OWN layers lowered to the engine's vocabulary (VFSZipLayer/FileEdit/RegEdit/CustomVar/…), where
    //they land relative to this node. For content bookkeeping (what files and CIDs a node carries), never for a
    //launch: a launch lowers the resolved plan, where TAKE and TARGET have placed everything.
    nlohmann::ordered_json Layers;
    std::vector<std::string> Refs;           // NODE refs, in order — what it contains (followed, fetched)
    std::vector<std::string> Requires;       // ANY members and NOT refs — compared, never followed
    bool IsGraft = false;                    // its list begins with ANY

    // ---- folded facts: this node's own resolve (default instance), derived by DeriveFacts ----
    nlohmann::ordered_json Entries;          // the folded EXEC: entry label -> entry, first-declared order
    bool HasExec   = false;                  // an effective entry without GUEST ⇒ runnable
    bool HasRunner = false;                  // an effective entry with GUEST ⇒ a runner
    bool OwnRunner = false;                  // its OWN list declares a GUEST entry (a runner root)
    std::string Label;                       // the default entry's LABEL
    std::string HostPlatform;                // the default entry's HOST
    std::vector<std::string> GuestPlatform;  // the default entry's GUEST[] (runner)
    nlohmann::ordered_json Exec;             // the default entry lowered for the engine (CONTENTPATH/EXEARGS/PLATFORM/…
                                             // or EXECUTABLE/ARGS/HOST/GUEST/CONTENT_ROOT/GUEST_ROOTS/…)
    std::vector<std::string> Faces;          // the tile UIDs its effective entries present
    std::string Uid;                         // the first face
    std::string PackageUid;                  // the root of that face's PARENTUID chain — %PackageUID%, the family
    nlohmann::ordered_json Meta;             // that face's tile (merged across the library), flat: UID PARENTUID TITLE
                                             // COVER{PATH,SOURCE} + the META fields
    bool OwnTile = false;                    // its OWN list carries an entry with a TILE

    std::filesystem::path File;              // source .json path
    std::filesystem::path BundleDir;         // owning bundle dir — content PATHs resolve here
    bool Received = false;                   // a browse stub gathered from a friend's catalog (not hydrated)

    std::string Key() const { return Cid.empty() ? NodeId : Cid; }
    bool Presentable()  const { return Meta.is_object() && !Meta.empty(); }
    bool IsRunner()     const { return HasRunner; }
    bool IsRunnable()   const { return HasExec; }
    bool IsVariant()    const { return !Variant.empty() && HasExec; }
    bool HasIdentity()  const { return !Uid.empty(); }
    std::string GameKey() const { return PackageUid.empty() ? NodeId : PackageUid; }   // the card this node belongs to
    // The lowered exec block of the entry labelled `Label` ("" ⇒ the default entry). Null json if no such entry.
    nlohmann::ordered_json ExecFor(const std::string &Label) const;
    std::string HostFor(const std::string &Label) const;
    std::vector<std::string> EntrypointLabels() const;
};

//The global node graph: handle -> Node, built by scanning bundle dirs for node files.
struct NodeIndex {
    std::map<std::string, Node> Nodes;
    const Node *Find(const std::string &NodeId) const;
};

namespace Fold { struct Library; struct Plan; }

namespace ManifestModel {

// ----- node graph -----
// True iff J is a node object: an object with a LAYERS list. The ONE definition — every scanner uses it.
bool IsNodeObject(const nlohmann::ordered_json &J);
// The top-level vocabulary of a node: CID LABEL POS COMMENT VARIANT RECOMMENDED LAYERS.
const std::set<std::string> &NodeFields();
// Every ref a RAW node names, in order: NODE refs (what it contains) and, into *Requires, ANY members + NOT refs.
std::vector<std::string> NodeRefs(const nlohmann::ordered_json &J, std::vector<std::string> *Requires = nullptr);
// Rewrite every ref in a RAW node (NODE, ANY members, NOT) through Map. Returns whether anything changed. Shared by
// freeze and the CID write-back so they can never disagree on where refs live.
bool RemapNodeRefs(nlohmann::ordered_json &J, const std::function<std::string(const std::string &)> &Map);
// Parse a single node object sourced from File in BundleDir. False if it is not a node.
bool ParseNode(const nlohmann::ordered_json &J, const std::filesystem::path &File,
               const std::filesystem::path &BundleDir, Node &Out);
// Scan one bundle dir (non-recursive) for *.json node files, adding them to Idx. Duplicate handles are reported
// and the first-seen wins. Run DeriveFacts after assembling an index by hand.
void ScanBundleNodes(const std::filesystem::path &BundleDir, NodeIndex &Idx);
// Build the global index from library roots (each holds bundle dirs one level down) plus ExtraBundleDirs scanned as
// bundles directly (locally-added packages). Runs DeriveFacts.
NodeIndex BuildNodeIndex(const std::vector<std::filesystem::path> &LibraryRoots,
                         const std::vector<std::filesystem::path> &ExtraBundleDirs = {});
// The resolver's view of an index (borrows the nodes' JSON; cheap).
Fold::Library LibraryOf(const NodeIndex &Idx);
// Derive every node's folded facts (entries, runner/runnable, faces, the merged tile) from its own resolve, and the
// library-wide tile table (tiles merge by UID; a tile's presentation comes from its recommended variant's fold,
// else its first variant's).
void DeriveFacts(NodeIndex &Idx);

// Everything a node contains, following every NODE ref regardless of WHEN — what a download or a hydration check
// needs (options included: the user may switch them on). Post-order: contained nodes first, Root last. Refs missing
// from Idx go to *Missing. Cycles are broken.
std::vector<std::string> Closure(const NodeIndex &Idx, const std::string &Root, std::vector<std::string> *Missing = nullptr);

// The DOWNLOAD units of a tile: a greedy set-cover over the content its variants' closures carry (a delta chain is
// one pick; genuine branches each get a row; a long tail folds into "Everything else"). LaunchableCount = how many
// variants a row's pick dominates.
struct EndpointInfo {
    std::vector<std::string> Ids;
    std::string Label;
    int         LaunchableCount = 0;
};
std::vector<EndpointInfo> TileEndpoints(const NodeIndex &Idx, const std::vector<std::string> &VariantIds);

// Validate the node graph (generation 6): refs resolve, no cycles, one type key per layer, well-typed payloads,
// STORE zips, BinaryPatch shape, WHEN grammar and the phase rule, undefined %KEY%s, KEEP lint, a VARIANT presents a
// tiled entry, RECOMMENDED names a presented tile, and — per variant, over its resolved plan — no EDIT beneath a
// layer that replaces its file, and every DELTA sits on content. OnlyNodes scopes it (the rest of Idx is consulted).
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

// ----- component lookups -----
int FindComponentIndex(const nlohmann::ordered_json &MANIFESTJSON, const std::string &ComponentID); // index or -1

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
