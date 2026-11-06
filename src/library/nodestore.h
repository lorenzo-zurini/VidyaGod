#ifndef NODESTORE_H
#define NODESTORE_H

#include "manifestmodel.h"   // NodeIndex

#include <filesystem>
#include <map>
#include <string>
#include <vector>

// NodeStore — node blocks to and from the embedded node (the gigagraph's I/O; the pure transforms are NodeGraph's).
namespace NodeStore
{

// Build a CID-keyed index by walking the frozen node DAG from RootCids: dagGet each block, normalize its links,
// ParseNode it (NodeId = human label, Cid = the block CID = identity, Parents = CID strings), then recurse into its
// positive OVER refs (composition; a NOT ref is never followed). Content-leaf CIDs (SOURCE/COVER) are NOT recursed —
// they are dag-pb blobs fetched lazily at hydrate. A shared node reached many times is fetched once. A block that
// cannot be fetched/parsed, or is not a node, is recorded in Missing (if given) and skipped. Runs DeriveIdentity.
// Frozen nodes carry no BundleDir (browse-before-download); local content location is filled in at hydrate.
// Always the FULL node closure: there is no shallow "browse" view any more — a receiver lands a share's whole
// node-block closure (AppModel::completeReceivedClosures) so it derives everything the seeder derives.
NodeIndex BuildFrozenIndex(const std::vector<std::string> &RootCids, std::vector<std::string> *Missing = nullptr);

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

} // namespace NodeStore

#endif // NODESTORE_H
