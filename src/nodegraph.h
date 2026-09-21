#ifndef NODEGRAPH_H
#define NODEGRAPH_H

#include "manifestmodel.h"   // NodeIndex, Node
#include <nlohmann/json.hpp>
#include <map>
#include <string>
#include <vector>

// NodeGraph — the gigagraph's IPFS boundary: reading a frozen node DAG (dag-json blocks addressed by CID) into a
// CID-keyed NodeIndex, and MINTING a working tree into blocks. manifestmodel stays pure/local. Links travel in
// dag-json form ({"/":cid}) but are normalized to plain-string CIDs at ingest, so ParseNode and the whole resolver
// keep operating on plain strings unchanged.
//
// Two layers: the PURE transforms (normalize / linkify / freeze / topo-order — no IPFS, no Qt; nodegraphpure.cpp,
// unit-tested) and the I/O orchestration (BuildFrozenIndex / Mint — call the node; nodegraph.cpp).
namespace NodeGraph {

// ---- pure transforms (nodegraphpure.cpp) ----

// Normalize dag-json links IN PLACE: any object of the exact shape {"/":"<string>"} becomes that string, recursively.
// After this, PARENTS entries and SOURCE.CID / COVER.SOURCE.CID / LIBRARYITEM read as plain CID strings — what
// ParseNode and every consumer expect. A dag-json byte value ({"/":{"bytes":...}}) is left alone (non-string "/").
void NormalizeLinks(nlohmann::ordered_json &J);

// Freeze one working-tree node's raw JSON into its canonical-intent dag-json form: strip POS (canvas coords — the one
// non-semantic field), rewrite intra-tree PARENTS/LIBRARYITEM handles → their frozen CIDs (via HandleToCid; a ref not
// in HandleToCid is an already-frozen external dep and passes through), and linkify every CID-bearing field
// (PARENTS[], LIBRARYITEM, and any SOURCE.CID) into {"/":cid}. The returned JSON is handed to DagPut/DagCid (which
// canonicalizes, so C++ key order is irrelevant). Pure — no node required.
nlohmann::ordered_json FreezeNodeJson(nlohmann::ordered_json Raw,
                                      const std::map<std::string, std::string> &HandleToCid);

// Read every *.json node under Root (RECURSIVELY) into a working tree: stored "CID" handle → raw node JSON, and handle
// → the bundle dir it came from (for BundleDir). A node with no stored "CID" (a fetched/received block, whose handle
// was stripped on landing, or a never-minted draft) gets a synthetic per-node key. A file holds one node or an array
// of them; FIRST-seen wins on a duplicate handle (a stale/forged stored CID) and the loser is RE-KEYED, never dropped.
// Recursion is required so a package's cross-package edges resolve against the whole library. Pure — FS + JSON, no IPFS.
// SkipReserved: skip reserved "_friend_*" received-stub dirs (used by PublishLibrary so a friend's stub can never enter
// the mint tree — no re-share, no hostile handle shadowing our nodes). Default false (the catalog gather wants them).
// Cheap string-aware bracket-depth pre-scan: true iff the JSON text nests no deeper than MaxDepth. nlohmann's parser
// is recursive-descent and NormalizeLinks recurses per level, so UNTRUSTED bytes (a fetched block, a landed received
// node file) must pass this BEFORE any parse — a hostile deep block otherwise overflows the stack (no exception).
bool JsonDepthWithinLimit(const std::string &S, int MaxDepth);

// Bounded read of a library-tree .json file: size cap (8 MiB — a node block is never bigger) + depth pre-scan +
// non-throwing parse. False on oversize/too-deep/unparseable. EVERY walker of the library root must use this —
// received shares land hostile bytes there verbatim, and one unguarded recursive parse is a crash.
bool ReadTreeJsonBounded(const std::filesystem::path &File, nlohmann::ordered_json &J);

void GatherWorkingTree(const std::filesystem::path &Root,
                       std::map<std::string, nlohmann::ordered_json> &Tree,
                       std::map<std::string, std::filesystem::path> &Dirs,
                       bool SkipReserved = false);

// Lift the metadata edge (flat → gigagraph): for each DeclareExec in the tree, move a PARENTS entry that names a
// DeclareLibraryItem node (present in the tree) into the exec's LIBRARYITEM field, removing it from PARENTS — so the
// tile stops being a composition parent and becomes the dedicated metadata link. Idempotent (a node that already has
// LIBRARYITEM is left alone). Pure — the core transform of both `--mint` gathering and the migration.
void LiftLibraryItemEdge(std::map<std::string, nlohmann::ordered_json> &Tree);

// Deps-first (post-order) freeze order over the working tree: a node is ordered AFTER every intra-tree node it links
// (PARENTS + LIBRARYITEM), because its frozen CID embeds theirs. Refs not in WorkingTree are external (already frozen)
// and impose no order. A cycle is BROKEN (back edge dropped, warned), not fatal — the cyclic nodes stay in the order
// but fail to freeze downstream and are skipped, so one bad edge never aborts the whole freeze. Always returns true. Pure.
bool TopoOrderForMint(const std::map<std::string, nlohmann::ordered_json> &WorkingTree,
                      std::vector<std::string> &Order, std::string *Error = nullptr);

// ---- I/O orchestration (nodegraph.cpp) ----

// Build a CID-keyed index by walking the frozen node DAG from RootCids: dagGet each block, normalize its links,
// ParseNode it (NodeId = human label, Cid = the block CID = identity, Parents = CID strings), then recurse into its
// PARENTS (composition) AND its LIBRARYITEM (the tile — reachable ONLY this way now, never via PARENTS). Content-leaf
// CIDs (SOURCE/COVER) are NOT recursed — they are dag-pb blobs fetched lazily at hydrate. A shared node reached many
// times is fetched once. A block that cannot be fetched/parsed, or is not a node, is recorded in Missing (if given)
// and skipped. Runs LinkGames. Frozen nodes carry no BundleDir (browse-before-download); local content location is
// filled in at hydrate.
// Shallow=false: the full closure (PARENTS + LIBRARYITEM) — for launch/hydrate. Shallow=true: fetch each root plus
// ONLY its LIBRARYITEM tile, NOT the PARENTS composition graph — the cheap BROWSE view for a friend's shared library
// (the tile's title/cover is enough to render a card; the full graph is walked only on install). Content leaves are
// never fetched either way, so Shallow bounds only the node-block fan-out.
// LocalOnly=true reads only blocks already in the store (no bitswap) — for catalog-build over friend share CIDs that
// may not all be fetched yet. Only meaningful with Shallow=true.
NodeIndex BuildFrozenIndex(const std::vector<std::string> &RootCids, std::vector<std::string> *Missing = nullptr,
                           bool Shallow = false, bool LocalOnly = false);

// Freeze a gathered working tree directly into a CID-keyed NodeIndex (identity = CID) WITHOUT storing blocks — uses
// DagCid (side-effect-free), so it is safe at catalog-build time. Resolves PARENTS/LIBRARYITEM handles → CIDs, sets
// each node's Cid, and sets BundleDir from Dirs (the on-disk bundle each node came from) so the launch engine finds
// local content. This is how the pretty, handle-based on-disk library becomes the CID-addressed gigagraph in memory
// with NO on-disk rewrite (git-style: the working tree is the source of truth, the CID index is derived on load).
// Runs LinkGames. RESILIENT: a node that can't freeze (dangling ref / bad link) is skipped and cycles are broken —
// one bad node never empties the index; the rest of the library is intact.
NodeIndex FreezeToIndex(const std::map<std::string, nlohmann::ordered_json> &WorkingTree,
                        const std::map<std::string, std::filesystem::path> &Dirs, std::string *Error = nullptr);

// The result of minting a working tree.
struct MintResult {
    std::map<std::string, std::string> HandleToCid; // every node's NODE_ID handle → its frozen CID
    std::vector<std::string>           Launchables;  // LAUNCH axis: DeclareExec nodes with NO GUEST (a game's playable
                                                     // variants; GUEST-bearing DeclareExecs are runners) — used for
                                                     // launch/CLI semantics, NOT the share list
    std::vector<std::string>           Published;    // SHARE axis: nodes carrying PUBLISH=true (games, runners AND
                                                     // no-exec library heads) — the share-list roots
};

// Hydrate a launchable's closure from IPFS into the pretty on-disk checkout under DestRoot: fetch the frozen DAG
// (BuildFrozenIndex), name the bundle dir from the tile ([uid] title), write each node's JSON there (readable
// plain-CID form — re-freezes to the identical CID), and fetch every content leaf to its PATH. After this the package
// is local + hydrated, so GatherWorkingTree picks it up and it launches like any local game. This is the sharing
// CONSUMER: a pasted/friend launchable CID becomes a playable game. Requires the node online for not-yet-local blocks.
// Returns the checkout dir ("" + Error on failure — e.g. a block that could not be fetched). NOTE (MVP): a launchable's
// whole closure (game content + its small shared libraries) lands in one dir; shared-lib content may duplicate across
// games until a shared-bundle layout lands. Runners are separate roots, hydrated on their own.
std::string HydratePackage(const std::filesystem::path &DestRoot, const std::string &LaunchableCid,
                           std::string *Error = nullptr);

// Mint a working tree (NODE_ID handle → raw node JSON) into dag-json blocks, deps-first: FreezeNodeJson each node,
// DagPut it (stored, direct-pinned, announced), record its CID for its referrers. Requires a started node. Resilient:
// a node that can't freeze (dangling ref / bad link) is skipped, cycles are broken. On success Out.HandleToCid maps
// every frozen node and Out.Launchables lists the playable roots to add to the library list.
bool Mint(const std::map<std::string, nlohmann::ordered_json> &WorkingTree, MintResult &Out,
          std::string *Error = nullptr);

} // namespace NodeGraph

#endif // NODEGRAPH_H
