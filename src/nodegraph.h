#ifndef NODEGRAPH_H
#define NODEGRAPH_H

#include "manifestmodel.h"   // NodeIndex, Node
#include <nlohmann/json.hpp>
#include <filesystem>
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

// A received package folder lands in <package dir>/.package/ — a dir its fetch owns whole (a re-publish replaces it).
// The nodes in it belong to the package dir itself: their bundle (where content hydrates) is the parent.
inline constexpr const char *kPackageFolderDir = ".package";

// A fetch's SCRATCH dir: a folder fetch materializes into "<dest>.tmp" and renames it into place when complete. Its
// half-landed node files are not a package — indexing them made the closure pass land blocks INTO the scratch dir
// while the folder fetch was still writing it (Minecraft's 1 872-node folder never landed: the two kept colliding).
inline bool IsFetchScratchDir(const std::filesystem::path &Dir)
{
    const std::string N = Dir.filename().string();
    return N.size() > 4 && N.compare(N.size() - 4, 4, ".tmp") == 0;
}

// ---- pure transforms (nodegraphpure.cpp) ----


// Freeze one working-tree node's raw JSON: strip its handle ("CID") and canvas coordinates ("POS") and rewrite every
// NODE/ANY/NOT ref naming a working-tree node to that node's frozen CID (via HandleToCid). A ref absent from
// HandleToCid is an already-frozen external node and passes through — unless it names a node of Tree: that node did
// not freeze (a bad node, or a cycle), so the ref would publish a handle nothing holds. The first such ref is
// reported in *Dangling and the caller skips this node too. Pure — no node required.
nlohmann::ordered_json FreezeNodeJson(nlohmann::ordered_json Raw,
                                      const std::map<std::string, std::string> &HandleToCid,
                                      const std::map<std::string, nlohmann::ordered_json> *Tree = nullptr,
                                      std::string *Dangling = nullptr);

// Read every *.json node under Root (RECURSIVELY) into a working tree: stored "CID" handle → raw node JSON, and handle
// → the bundle dir it came from (for BundleDir). A node with no stored "CID" (a fetched/received block, whose handle
// was stripped on landing, or a never-minted draft) gets a synthetic per-node key. A file holds one node or an array
// of them; FIRST-seen wins on a duplicate handle (a stale/forged stored CID) and the loser is RE-KEYED, never dropped.
// Recursion is required so a package's cross-package edges resolve against the whole library. Pure — FS + JSON, no IPFS.
// SkipReserved: skip reserved "_friend_*" received-stub dirs (used by PublishLibrary so a friend's stub can never enter
// the mint tree — no re-share, no hostile handle shadowing our nodes). Default false (the catalog gather wants them).
// Cheap string-aware bracket-depth pre-scan: true iff the JSON text nests no deeper than MaxDepth. nlohmann's parser
// is recursive-descent, so UNTRUSTED bytes (a fetched block, a landed received
// node file) must pass this BEFORE any parse — a hostile deep block otherwise overflows the stack (no exception).
bool JsonDepthWithinLimit(const std::string &S, int MaxDepth);

// Bounded read of a library-tree .json file: size cap (8 MiB — a node block is never bigger) + depth pre-scan +
// non-throwing parse. False on oversize/too-deep/unparseable. EVERY walker of the library root must use this —
// received shares land hostile bytes there verbatim, and one unguarded recursive parse is a crash.
bool ReadTreeJsonBounded(const std::filesystem::path &File, nlohmann::ordered_json &J);

// TrustStoredCid: whether a node's stored "CID" field may be used as its handle. TRUE for the user's OWN roots
// (LIBRARY, externally-added local bundles) — those files are authored/installed under our control. FALSE for the
// UNTRUSTED received root (CATALOG browse stubs a friend controls): a malicious stub can embed a "CID" equal to a
// local node's external-dep CID to make that dep resolve intra-tree to hostile content (or forge an "authored"
// handle to steal a launch's BundleDir). An honest frozen stub never carries "CID" (stripped at freeze), so ignoring
// it costs nothing and makes every received node synthetic-keyed (browse-only, un-referenceable, never a handle).
void GatherWorkingTree(const std::filesystem::path &Root,
                       std::map<std::string, nlohmann::ordered_json> &Tree,
                       std::map<std::string, std::filesystem::path> &Dirs,
                       bool SkipReserved = false, bool TrustStoredCid = true);

// Deps-first (post-order) freeze order over the working tree: a node is ordered AFTER every intra-tree node it links
// (every OVER ref, NOT included), because its frozen CID embeds theirs. Refs not in WorkingTree are external (already frozen)
// and impose no order. A cycle is BROKEN (back edge dropped, warned), not fatal — the cyclic nodes stay in the order
// but fail to freeze downstream and are skipped, so one bad edge never aborts the whole freeze. Always returns true. Pure.
bool TopoOrderForMint(const std::map<std::string, nlohmann::ordered_json> &WorkingTree,
                      std::vector<std::string> &Order, std::string *Error = nullptr);

// ---- landing untrusted node bytes (nodegraph.cpp) ----

// Node bytes a peer sent, checked before anything reads them: at most one block, hashing to ExpectCid, a node object
// (depth-checked parse), canonical, and carrying no working-tree field (CID, POS — a handle would hijack a local
// node once the package is installed). Parsed into J on success; the reason in *Error otherwise.
bool VerifyNodeBytes(const std::string &Bytes, const std::string &ExpectCid, nlohmann::ordered_json &J, std::string *Error = nullptr);
// True iff P resolves within Base (no ".." escape, no absolute path elsewhere; works on not-yet-existent paths). What
// guards every write a node's content can direct (a download, a hydrate) to its own package dir.
bool PathWithin(const std::filesystem::path &Base, const std::filesystem::path &P);
// VerifyNodeBytes over a landed file (a received <cid>.json).
bool VerifyLanded(const std::filesystem::path &File, const std::string &ExpectCid, nlohmann::ordered_json *Out = nullptr,
                  std::string *Error = nullptr);

// ---- I/O orchestration (nodegraph.cpp) ----

// Build a CID-keyed index by walking the frozen node DAG from RootCids: dagGet each block, normalize its links,
// ParseNode it (NodeId = human label, Cid = the block CID = identity, Parents = CID strings), then recurse into its
// positive OVER refs (composition; a NOT ref is never followed). Content-leaf CIDs (SOURCE/COVER) are NOT recursed —
// they are dag-pb blobs fetched lazily at hydrate. A shared node reached many times is fetched once. A block that
// cannot be fetched/parsed, or is not a node, is recorded in Missing (if given) and skipped. Runs DeriveIdentity.
// Frozen nodes carry no BundleDir (browse-before-download); local content location is filled in at hydrate.
// Always the FULL node closure: there is no shallow "browse" view any more — a receiver lands a share's whole
// node-block closure (AppModel::completeReceivedClosures) so it derives everything the seeder derives.
NodeIndex BuildFrozenIndex(const std::vector<std::string> &RootCids, std::vector<std::string> *Missing = nullptr);

// Freeze a gathered working tree directly into a CID-keyed NodeIndex (identity = CID) WITHOUT storing blocks — uses
// DagCid (side-effect-free), so it is safe at catalog-build time. Resolves OVER handles → CIDs, sets
// each node's Cid, and sets BundleDir from Dirs (the on-disk bundle each node came from) so the launch engine finds
// local content. This is how the pretty, handle-based on-disk library becomes the CID-addressed gigagraph in memory
// with NO on-disk rewrite (git-style: the working tree is the source of truth, the CID index is derived on load).
// Runs DeriveIdentity. RESILIENT: a node that can't freeze (dangling ref / bad link) is skipped and cycles are broken —
// one bad node never empties the index; the rest of the library is intact.
NodeIndex FreezeToIndex(const std::map<std::string, nlohmann::ordered_json> &WorkingTree,
                        const std::map<std::string, std::filesystem::path> &Dirs, std::string *Error = nullptr);

// The result of minting a working tree.
struct MintResult {
    std::map<std::string, std::string> HandleToCid; // every node's NODE_ID handle → its frozen CID
    std::vector<std::string>           Launchables;  // LAUNCH axis: nodes with an entrypoint that has NO GUEST (a
                                                     // game's playable variants; GUEST entrypoints are runners) —
                                                     // used for launch/CLI semantics, NOT the share list
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
