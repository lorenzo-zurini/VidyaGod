#ifndef INSTALLED_H
#define INSTALLED_H

#include "manifestmodel.h"   // NodeIndex, Node
#include "shelf.h"           // GraftChoice

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// Installed — what of a launch is here: its content (hydration) and the runners that can carry it to this machine.
// Queries of the index and the files it names.
namespace Installed
{

// A runner's BUILD: its closure, itself included (its own layers — the java runners carry the JRE zip on the node —
// plus what it contains — proton's wine chain), the nodes with content, in load order. The same rule as a game's mount.
std::vector<const Node *> RunnerBuildNodes(const NodeIndex &Idx, const std::string &RunnerNodeId);
// Every build layer of a runner here (its prefix assembles from the build at launch, so build-present is ready). A
// runner that ships no build is always present.
bool RunnerBuildPresent(const NodeIndex &Idx, const std::string &RunnerNodeId);

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

// Whether installed runners carry Launch's platform to this machine — directly or through a chain (a Windows-only
// engine under proton). The Play gate: a chain-only game has no UsableRunner yet runs fine.
bool HasUsableChain(const NodeIndex &Idx, const Node &Launch);
// Installed runners that can CONSUME InputPlatform (GUEST ∋ InputPlatform), regardless of their HOST — the choices
// for one step of the runner daisy-chain UI (a chain step's input platform → the runners that bridge it onward). For
// "custom", only the engine shipped in the launch's own package (LaunchBundleDir). Sorted by node id.
std::vector<const Node*> CandidateRunners(const NodeIndex &Idx, const std::string &InputPlatform,
                                         const std::filesystem::path &LaunchBundleDir);
// True when every VFS layer in a launchable node's content closure is present locally (its game is installed).
bool NodeHydrated(const NodeIndex &Idx, const std::string &LaunchNodeId);
// True iff the node's closure defines at least one VFS content layer — i.e. it's a real, downloadable game and not a
// content-less/malformed node (which is vacuously "hydrated"). Gate Library/Installed-Packages visibility on this.
bool NodeHasContent(const NodeIndex &Idx, const std::string &LaunchNodeId);
// Bulk hydration for the WHOLE index in O(N+E): computes {Hydrated, HasContent} for every node via one topological
// pass over PARENTS (a node's value = its own layers ⊕ its parents'), memoizing per-node and stat-caching per path.
// Use this instead of calling NodeHydrated/NodeHasContent per-launchable when building library/catalog tiles —
// otherwise a package with a launchable per version over a deep delta chain is O(N²) (each closure re-walked + re-stat).
// Hydrated: nothing to fetch is missing. HasContent: some content layer is DEFINED (a downloadable build). OnDisk: some
// content file is actually HERE (or supplied at run time) — what an installed game has and a node-only package lacks.
// HasCid: some content layer names a CID (something to fetch). Incomplete: the closure names a node not in the index
// (a package not landed yet). Together: what the Catalog offers to download, without a closure walk per edition.
struct NodeHydration { bool Hydrated = true; bool HasContent = false; bool OnDisk = false; bool HasCid = false;
                       bool Incomplete = false; };
std::unordered_map<std::string, NodeHydration> HydrationMap(const NodeIndex &Idx);

// Every content layer of a launchable's closure (options included: a download is whole chains), as Fn(layer, its
// bundle dir, its node): with WithGrafts the chosen grafts' closures too (what a fetch pools), WithBase the base a
// graft-row runs on. What the hydration verdicts and the library's content walks share.
void ForEachContentLayer(const NodeIndex &Idx, const std::string &LaunchNodeId, const Shelf::GraftChoice &Grafts,
    const std::function<void(const nlohmann::ordered_json&, const std::filesystem::path&, const std::string&)> &Fn,
    bool WithGrafts = false, bool WithBase = true);

} // namespace Installed

#endif // INSTALLED_H
