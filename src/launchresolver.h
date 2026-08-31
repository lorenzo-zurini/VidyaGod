#ifndef LAUNCHRESOLVER_H
#define LAUNCHRESOLVER_H

#include <nlohmann/json.hpp>

#include "launchparams.h"    // ContainerParams
#include "manifestmodel.h"   // NodeIndex, Node

// LaunchResolver — resolves a launch request into a fully-populated ContainerParams from the global node graph:
// the param / recipe / runner-pick / persistence / exec / path resolution lifted out of ContainerWrapper. Free
// functions over the shared ContainerParams; the %token% engine they rely on lives in VarSubst.
//
// (These are Qt + PackageCatalog + RunnerWrapper coupled, so they are NOT in the light unit-test target — the
// pure substitution seam in VarSubst is what's unit-tested. They were a verbatim move out of ContainerWrapper.)
namespace LaunchResolver
{
//Reserved node id for a SYNTHESIZED native-passthrough terminal, used when no native runner node is authored in the
//graph (back-compat / safety net). The native terminal forwards the inner command unchanged in the host namespace.
inline constexpr const char *kNativeTerminalId = "__native__";

//Absolutize a layer's PATH / SOURCE.PATH against the owning node's BundleDir (in place) — shared between
//the resolution spine and the chain TU (P6 split).
void AbsolutizeLayerPaths(nlohmann::ordered_json &L, const std::filesystem::path &BundleDir);

//Resolves the shortest runner CHAIN (innermost→outermost runner node ids) to run a launch node's content on this
//machine: a BFS over runner edges GUEST→HOST from the launch node's platform to MachinePlatform(), ALWAYS terminated
//by a native runner (HOST==GUEST==machine) — an authored native runner if one is available, else kNativeTerminalId.
//A pinned chain (CP.RunnerChainIds, else USERSETTINGS RUNNER_CHAIN) is honored when it forms a valid path. Per-step
//default tie-break: RECOMMENDED > package-local > node-id. Empty if the platform is unreachable (no bridging runner).
//Used by the UI / CLI / tests and by ResolveRunnerChain.
std::vector<std::string> ResolveChainIds(const NodeIndex &Idx, const Node &Launch, const struct ContainerParams &CP,
                                         const nlohmann::ordered_json &GlobalConfigJSON);

//BFS-completes a chain from an ARBITRARY platform to the machine (+ native terminal) — the runner ids AFTER a step
//the user just changed in the prelaunch chain UI. Pure BFS (no pin honoring); Launch is used only for the package-
//local tie-break. Empty if FromPlatform can't reach the machine. If FromPlatform IS the machine, returns just the
//native terminal.
std::vector<std::string> ResolveChainTail(const NodeIndex &Idx, const std::string &FromPlatform, const Node &Launch,
                                          const nlohmann::ordered_json &GlobalConfigJSON);

//Builds the full resolved RunnerLink execution chain from ResolveChainIds (synthesizing a passthrough terminal for
//kNativeTerminalId or a missing node). The authoritative chain consumed by InitializeFromNode / BuildNestedInvocation.
//Empty if the platform is unreachable.
std::vector<RunnerLink> ResolveRunnerChain(const NodeIndex &Idx, const Node &Launch, const struct ContainerParams &CP,
                                           const nlohmann::ordered_json &GlobalConfigJSON);

//=====================================================================================================================
//                              CROSS-NAMESPACE NESTING (an inner runner inside the boundary's guest fs)
//=====================================================================================================================
//The CONTENT_ROOT-relative mount sub-dir where an INNER chain runner's build is placed inside the boundary runner's
//guest filesystem (e.g. snes9x's win32 binaries land under proton's drive_c at <CONTENT_ROOT>/__runner_snes9x__).
//Shared by vfsmount (mounting) and ComposeGuestTarget (path translation) so the two always agree.
inline std::string InnerRunnerMountRel(const std::string &RunnerNodeId) { return "__runner_" + RunnerNodeId + "__"; }

//True when the chain nests across a namespace boundary: a runner runs INSIDE another runner's guest fs (e.g. a win32
//emulator under proton). Equivalent to "there is a link before the runtime-boundary runner". False for every classic
//chain ([content-runner, native]) — so all cross-namespace code paths are gated off for existing packages.
bool ChainHasInnerLinks(const struct ContainerParams &CP);

//The boundary runner index in CP.RunnerChain: the OUTERMOST link that creates a guest fs (non-native namespace), or 0
//(the innermost) when the whole chain is native. The boundary owns the FUSE mount / wine prefix.
int BoundaryLinkIndex(const struct ContainerParams &CP);

//Map a CONTENT_ROOT-relative path to the boundary runner's GUEST path via its GUEST_PATH template (%REL% = Rel; other
//%tokens% from CP). Empty template → identity (native paths). E.g. template "C:\\%PackageUID%\\%REL%" + Rel
//"__runner_snes9x__/snes9x.exe" → "C:\\<UID>\\__runner_snes9x__/snes9x.exe".
std::string GuestPath(const std::string &Template, const std::string &Rel, struct ContainerParams &CP);

//Resolved cross-namespace launch target: what the BOUNDARY runner should actually run instead of the content. The
//inner chain (RunnerChain[0..boundary)) runs inside the boundary's guest fs; this composes their guest command.
struct GuestTarget
{
    bool        CrossNamespace = false;        // false → no inner links; the boundary runs the content directly (classic)
    std::string ContentRel;                    // CONTENT_ROOT-relative path the boundary's %ContentPath% must point at
                                               // (the innermost inner runner's guest exe location); "" when classic
    std::vector<std::string> TrailingArgs;     // guest args appended AFTER the boundary's ARGS (the inner runners' own
                                               // args, e.g. the ROM path) — already guest-translated + %token%-expanded
};

//Compose the cross-namespace guest target for CP (the resolved chain + content). Returns {CrossNamespace:false} when
//there are no inner links (every classic chain) — callers then run the content directly. Otherwise redirects the
//boundary runner at the inner runners' guest command (innermost exe + trailing args). Pure; does not mutate CP.
GuestTarget ComposeGuestTarget(struct ContainerParams &CP);

//Native node-graph init — populates ContainerParams + the component pool (ComponentPool, out) DIRECTLY from the
//node graph (ContainerParams.NodeIdx / .LaunchNodeId): runner pick, recipe, paths, custom vars, persistence.
[[nodiscard]] bool InitializeFromNode(struct ContainerParams &ContainerParams, nlohmann::ordered_json &ComponentPool, const nlohmann::ordered_json &GlobalConfigJSON);

//Picks the best ROLE:"runner" node for a launch node (GUEST ∋ launch host, HOST==machine, executable available),
//priority: explicit RunnerID pin > USERSETTINGS PREFERRED_RUNNER > first qualifying (node-id order). nullptr if none.
const Node *PickRunnerNode(const NodeIndex &Idx, const Node &Launch, const struct ContainerParams &CP, const nlohmann::ordered_json &GlobalConfigJSON);

//Derives the session paths (Temp/Runtime/WriteLayer/DefaultData/UserData/Program/DefPrefix + ContentRoot
//resolution + PrefixRoot + screen geometry) from already-set ContainerParams fields.
bool DerivePaths(struct ContainerParams &ContainerParams, const nlohmann::ordered_json &GlobalConfigJSON);

//Reads the launch node's EXEC (CONTENTPATH/EXEARGS/WORKDIR) into the exec fields. After BuildSubComponentsArray
//and before BuildContainerRuntime.
[[nodiscard]] bool ResolveExecutableDefinition(const nlohmann::ordered_json &MANIFESTJSON, struct ContainerParams &ContainerParams);

//Resolves every CustomVar subcomponent in the Recipe (priority: CLI override > USERSETTINGS > DEFAULT). Must run
//before BuildSubComponentsArray so custom %KEY% tokens are available for substitution.
bool ResolveCustomVariables(const nlohmann::ordered_json &MANIFESTJSON, struct ContainerParams &ContainerParams, const nlohmann::ordered_json &GlobalConfigJSON);

//Derives persistence from the unified Persist primitive (MODE/KEEP/DROP) on the runner keep-set + the Recipe.
bool DerivePersistence(const nlohmann::ordered_json &MANIFESTJSON, struct ContainerParams &ContainerParams);

//Collects all SUBCOMPONENTS from the Recipe's components into SubComponentsArray (with %VAR% substitution),
//skipping CustomVar / Persist* (handled by ResolveCustomVariables / DerivePersistence).
bool BuildSubComponentsArray(const nlohmann::ordered_json &MANIFESTJSON, struct ContainerParams &ContainerParams);
}

#endif // LAUNCHRESOLVER_H
