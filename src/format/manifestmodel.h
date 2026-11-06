#ifndef MANIFESTMODEL_H
#define MANIFESTMODEL_H

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>
#include <memory>
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

//A node's JSON as authored, shared by every copy of the node: read-only once parsed, so copying a Node (the editor's
//working index, an authoring session's) costs a pointer instead of the whole tree. Reads as a const JSON value.
class SharedJson
{
public:
    SharedJson() = default;
    SharedJson(nlohmann::ordered_json J) : P(std::make_shared<const nlohmann::ordered_json>(std::move(J))) {}
    const nlohmann::ordered_json &operator*() const { return P ? *P : Null(); }
    const nlohmann::ordered_json *operator->() const { return &**this; }
    operator const nlohmann::ordered_json &() const { return **this; }
private:
    static const nlohmann::ordered_json &Null() { static const nlohmann::ordered_json N; return N; }
    std::shared_ptr<const nlohmann::ordered_json> P;
};

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
    SharedJson Json;                         // the node as authored — what Fold resolves
    //The node's OWN content layers (VFSZipLayer/VFSDeltaLayer/…) lowered to the engine's vocabulary, where they land
    //relative to this node: what files and CIDs it carries. Kept for every node — hydration, fetch and seeding read
    //them constantly. The rest of its lowered layers (edits, registry, variables…) is LoweredLayers(), made on demand:
    //held for every node of a library it was most of the index's memory, and only checks read it.
    nlohmann::ordered_json ContentLayers;
    std::vector<std::pair<std::string, std::string>> VarDefaults;   // its own CustomVars: KEY -> DEFAULT
    //Every one of its own layers lowered (an array; empty for a malformed node). Never for a launch: a launch lowers
    //the resolved plan, where TAKE and TARGET have placed everything.
    nlohmann::ordered_json LoweredLayers() const;
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
    std::filesystem::path BundleDir;         // its home package folder — content PATHs resolve here
    bool Received = false;                   // a browse stub gathered from a friend's catalog (not hydrated)

    std::string Key() const { return Cid.empty() ? NodeId : Cid; }
    bool Presentable()  const { return Meta.is_object() && !Meta.empty(); }
    bool IsRunner()     const { return HasRunner; }
    //A version on the shelf: VARIANT and an entry to run — its own, or (a graft that is a version) its base's.
    bool IsVariant()    const { return !Variant.empty() && (HasExec || IsGraft); }
    std::string GameKey() const { return PackageUid.empty() ? NodeId : PackageUid; }   // the card this node belongs to
    // The lowered exec block of the entry labelled `Label` ("" ⇒ the default entry). Null json if no such entry.
    nlohmann::ordered_json ExecFor(const std::string &Label) const;
    std::string HostFor(const std::string &Label) const;
    std::vector<std::string> EntrypointLabels() const;
};

//The global node graph: handle -> Node, built by scanning bundle dirs for node files.
struct NodeIndex {
    std::map<std::string, Node> Nodes;
    //The tiles: UID -> the tile (flat, merged across the library: its recommended variant's folded tile, else its first
    //variant's), derived by DeriveFacts. A tile is one presentable game; a node may present several.
    std::map<std::string, nlohmann::ordered_json> Tiles;
    //A tile a GRAFT presents (its own entry carries the TILE): UID -> that graft. Its card's rows are the variants it
    //applies onto, each launched with it applied (a mod with its own card: Forgotten Empires over The Conquerors).
    std::map<std::string, std::string> TileGraft;
    //A node has ONE home package. Other package folders found holding the same node (CID -> those folders) are an
    //authoring error the validator names: the node belongs in one package, the others contain it by CID.
    std::map<std::string, std::vector<std::filesystem::path>> Duplicates;
    //Node files the index refused (no CID: too large, or containing one that did not freeze). Without this a refused
    //file is only a "missing node" in its container, with the file sitting right there; the validator names each.
    //File is the node's file when the scan gathered it handle-less; else empty, and Handle is its stored CID.
    struct Refused { std::filesystem::path File, BundleDir; std::string Handle, Why; bool TooLarge = false; };
    std::vector<Refused> RefusedNodes;
    const Node *Find(const std::string &NodeId) const;
    //The tile a UID names (null when no node presents it).
    const nlohmann::ordered_json *Tile(const std::string &Uid) const
    { auto It = Tiles.find(Uid); return It == Tiles.end() ? nullptr : &It->second; }
};

namespace Fold { struct Library; struct Plan; }

namespace ManifestModel {

// ----- node graph -----
// The versions a graft applies onto: the variants (not grafts themselves) containing, transitively, a node its leading
// ANY names — what a graft that is a card, or a version, runs on. By VARIANT, then key.
std::vector<std::string> GraftBases(const NodeIndex &Idx, const std::string &Graft);
//GraftBases of every graft in the index at once (one pass over the containment, not one per graft): graft -> bases.
std::map<std::string, std::vector<std::string>> AllGraftBases(const NodeIndex &Idx);
// True iff J is a node object: an object with a LAYERS list. The ONE definition — every scanner uses it.
bool IsNodeObject(const nlohmann::ordered_json &J);
// Parse a single node object sourced from File in BundleDir. False if it is not a node.
bool ParseNode(const nlohmann::ordered_json &J, const std::filesystem::path &File,
               const std::filesystem::path &BundleDir, Node &Out);
// A node, and an index, as one JSON value and back — every field: what the on-disk index cache keeps and
// `catalog stats --dump` writes. A field added to Node or NodeIndex goes in both, or a cached index lacks it.
nlohmann::ordered_json NodeToJson(const Node &N);
Node NodeFromJson(nlohmann::ordered_json J);
nlohmann::ordered_json IndexTablesToJson(const NodeIndex &Idx);   // everything but the nodes
void IndexTablesFromJson(const nlohmann::ordered_json &J, NodeIndex &Idx);
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
// A VFS layer's locators: its expected LOCAL path (PackagePath/PATH, SOURCE.PATH overriding) and its ipfs CID
// (empty if none). The local path is the content's home; the CID is a remote fallback to fetch it from.
void LayerLocator(const nlohmann::ordered_json &Sub, const std::filesystem::path &PackagePath,
                  std::filesystem::path &Local, std::string &Cid);
// Convenience: a layer's resolved LOCAL absolute path (PackagePath/PATH). Pure, local-only (no cache).
std::string ResolveLayerSource(const nlohmann::ordered_json &Sub, const std::filesystem::path &PackagePath);

// ----- platform -----
std::string MachinePlatform();   // the host platform a runner variant's HOST_PLATFORM must match (e.g. "linux64")

// The platform of content only its OWN package can run: a game's own engine (GemRB, a source port, a recompilation)
// ships as a runner in the game's package with GUEST ["custom"], and the game's entry says HOST "custom".
inline constexpr const char *kCustomPlatform = "custom";
// Whether Runner can run Platform for a launch from LaunchBundleDir: GUEST lists it — and for "custom", the runner is
// in the launch's own package (another package's engine never runs this one). Every match against a launch's platform
// goes through this; the native-terminal checks (HOST==GUEST==machine) compare the machine platform directly.
bool ServesPlatform(const Node &Runner, const std::string &Platform, const std::filesystem::path &LaunchBundleDir);

} // namespace ManifestModel

#endif // MANIFESTMODEL_H
