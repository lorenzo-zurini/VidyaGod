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
// unit-tested). The blocks to and from the node (BuildFrozenIndex / HydratePackage / Mint) are NodeStore's.
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
// The same for bytes already in hand (a file read for its text first, a friend's message): depth pre-scanned, then a
// non-throwing parse. Discarded (is_discarded()) when too deep or not JSON.
nlohmann::ordered_json ParseBounded(const std::string &Bytes);

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
// The node files a GatherWorkingTree of Root reads, in the order it reads them (the same skips: fetch scratch dirs,
// and "_friend_*" when SkipReserved). What the index cache stats to know nothing changed.
std::vector<std::filesystem::path> ListTreeFiles(const std::filesystem::path &Root, bool SkipReserved = false);

// Deps-first (post-order) freeze order over the working tree: a node is ordered AFTER every intra-tree node it links
// (every OVER ref, NOT included), because its frozen CID embeds theirs. Refs not in WorkingTree are external (already frozen)
// and impose no order. A cycle is BROKEN (back edge dropped, warned), not fatal — the cyclic nodes stay in the order
// but fail to freeze downstream and are skipped, so one bad edge never aborts the whole freeze. Pure.
std::vector<std::string> TopoOrderForMint(const std::map<std::string, nlohmann::ordered_json> &WorkingTree);
// Each ordered node's level: 0 with no intra-tree link to an earlier node, else one more than the highest earlier node
// it links. Nodes of one level never link each other, so they freeze independently (in parallel) once the levels below
// are done; a broken cycle edge points to a later node, which always has a higher level. Pure.
std::vector<int> TopoLevels(const std::map<std::string, nlohmann::ordered_json> &WorkingTree, const std::vector<std::string> &Order);

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


// Freeze a gathered working tree directly into a CID-keyed NodeIndex (identity = CID) WITHOUT storing blocks — uses
// DagCid (side-effect-free), so it is safe at catalog-build time. Resolves OVER handles → CIDs, sets
// each node's Cid, and sets BundleDir from Dirs (the on-disk bundle each node came from) so the launch engine finds
// local content. This is how the pretty, handle-based on-disk library becomes the CID-addressed gigagraph in memory
// with NO on-disk rewrite (git-style: the working tree is the source of truth, the CID index is derived on load).
// Runs DeriveIdentity. RESILIENT: a node that can't freeze (dangling ref / bad link) is skipped and cycles are broken —
// one bad node never empties the index; the rest of the library is intact.
NodeIndex FreezeToIndex(const std::map<std::string, nlohmann::ordered_json> &WorkingTree,
                        const std::map<std::string, std::filesystem::path> &Dirs);




} // namespace NodeGraph

#endif // NODEGRAPH_H
