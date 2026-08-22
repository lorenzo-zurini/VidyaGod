#include "launchresolver.h"
#include "apppaths.h"        // AppPaths::DataRoot — the app data root the launch TEMP hangs off of
#include "varsubst.h"        // VarSubst::StringVariableSubstitution / RenderValue
#include "packagecatalog.h"  // GetPackageUserSettings (catalog/user-settings service)
#include "runnerwrapper.h"   // RunnerWrapper::ExecutableAvailable / DefPrefixDir
#include "commonutils.h"     // Log*

#include <QDir>
#include <QGuiApplication>
#include <QScreen>
#include <QThread>

#include <algorithm>
#include <filesystem>
#include <map>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

//The pure manifest queries + VFS-layer helpers live in ManifestModel; the catalog/user-settings service in
//PackageCatalog. Bring both in unqualified so the resolver code (moved verbatim out of ContainerWrapper) reads
//naturally (FindComponentIndex / GetPackageUserSettings / MachinePlatform / IsVfsLayer / ...).
using namespace ManifestModel;
using namespace PackageCatalog;

//Resolves every CustomVar (TYPE:"CustomVar") in the closure into ONE GLOBAL NAMESPACE (ContainerParams.CustomVariables,
//keyed by the bare token "KEY"). ABSOLUTE SCOPE: a var's final value is visible to every reference — including inside
//another var's DEFAULT — regardless of declaration order. Hierarchy is respected: the LATER (most-specific) declaration
//of a key wins (closure order: parents before children, the launchable last; then the active runner components). Bare
//keys are one shared knob, so a game seeding a runner's knob (re-declaring its KEY) is the feature.
//
//Done in two phases (see body): (1) collect each key's winning RAW source by priority; (2) fixpoint-substitute all
//sources against the built-in tokens + every other var until stable, so forward references and post-override values
//resolve. No encoding here — that is a use-site concern (%KEY:format%). A reference cycle leaves a residual %token%.
//
//Per-key source priority (highest to lowest):
//  1. ContainerParams.VariableOverrides — set from --var KEY=VALUE CLI flags or UI picker
//  2. GlobalConfigJSON["USERSETTINGS"][PackageUID]["VARIABLES"] — persisted user choices
//  3. the winning DEFAULT (or, for a secret+POOL var with nothing persisted yet, one pool entry drawn ONCE as a
//     seed and reported in ContainerParams.PickedSecrets for the caller to persist)

// P6 split: the runner daisy-chain machinery (availability, BFS bridge, links, nesting) — spine in launchresolver.cpp.
const Node *LaunchResolver::PickRunnerNode(const NodeIndex &Idx, const Node &Launch, const struct ContainerParams &CP,
                                             const nlohmann::ordered_json &GlobalConfigJSON)
{
    std::string Preferred;
    {
        auto US = GetPackageUserSettings(GlobalConfigJSON, CP.PackageUID);
        if (US.contains("PREFERRED_RUNNER") && US["PREFERRED_RUNNER"].is_string())
            Preferred = std::string(US["PREFERRED_RUNNER"]);
    }
    auto Qualifies = [&](const Node &N) -> bool
    {
        if (!N.IsRunner()) return false;
        bool Guest = false;
        for (const auto &G : N.GuestPlatform) if (G == Launch.HostPlatform) { Guest = true; break; }
        if (!Guest) return false;
        if (N.HostPlatform != MachinePlatform()) return false;
        return RunnerWrapper::ExecutableAvailable(N.Exec);
    };
    if (!CP.RunnerID.empty()) { const Node *N = Idx.Find(CP.RunnerID); if (N && Qualifies(*N)) return N; }
    if (!Preferred.empty())   { const Node *N = Idx.Find(Preferred);   if (N && Qualifies(*N)) return N; }

    //Default pick — rank by (RECOMMENDED, package-local, node-id).
    auto Local  = [&](const Node &N) { return !Launch.BundleDir.empty() && N.BundleDir == Launch.BundleDir; };
    auto Better = [&](const Node &A, const Node *B) -> bool
    {
        if (!B) return true;
        if (A.Recommended != B->Recommended) return A.Recommended;   // RECOMMENDED runner wins
        if (Local(A) != Local(*B))           return Local(A);        // runner in the launch's own package wins
        return A.NodeId < B->NodeId;                                 // deterministic last resort
    };
    const Node *Best = nullptr;
    for (const auto &[Id, N] : Idx.Nodes) if (Qualifies(N) && Better(N, Best)) Best = &N;
    return Best;
}

//=====================================================================================================================
//                                          RUNNER DAISY-CHAINING
//=====================================================================================================================
//A runner is a directed edge GUEST→HOST in the platform graph. To run content whose platform has no direct-to-machine
//runner (e.g. a SNES ROM whose only emulator, snes9x, is a win32 program), VidyaGod constructs the SHORTEST chain of
//runners from the content's platform to the machine's platform, then ALWAYS appends a native terminal (HOST==GUEST==
//machine) it execve's directly — the uniform wrap point ("the basic linux runner is the final link"). The chain nests
//innermost→outermost: chain[0] runs the content, chain[i+1] runs chain[i]'s command, the terminal runs everything.
namespace {

//Strict-better ordering for the BFS default pick: RECOMMENDED > package-local (runner shipped in the launch's own
//bundle) > lowest node-id. Runners are pre-sorted better-first so the first edge that discovers a platform is best.
bool RunnerBetterPtr(const Node *A, const Node *B, const Node &Launch)
{
    if (A->Recommended != B->Recommended) return A->Recommended;
    const bool LA = !Launch.BundleDir.empty() && A->BundleDir == Launch.BundleDir;
    const bool LB = !Launch.BundleDir.empty() && B->BundleDir == Launch.BundleDir;
    if (LA != LB) return LA;
    return A->NodeId < B->NodeId;
}

bool RunnerServes(const Node *R, const std::string &Platform)
{ for (const auto &G : R->GuestPlatform) if (G == Platform) return true; return false; }

//True if a runner SHIPS ITS OWN BUILD: any VFS layer in its content closure (its PARENTS). Such a runner's executable
//resolves from the mounted build, not the system PATH — so it's "available" even when EXECUTABLE is a bare command
//(e.g. a win32 emulator "snes9x.exe" nested under proton). Local-PATH or CID layers both count.
bool RunnerShipsBuild(const NodeIndex &Idx, const Node &R)
{
    bool Ships = false;
    ManifestModel::ForEachClosureNode(Idx, R.NodeId, {}, [&](const Node &N) {
        if (Ships || N.IsRunner() || !N.Layers.is_array()) return;
        for (const auto &L : N.Layers) if (IsVfsLayer(LayerType(L))) { Ships = true; return; }
    });
    return Ships;
}

//A runner can be used on this machine when its executable resolves on the system PATH OR it ships its own build (the
//exe lives in the build). Used to filter the runner-edge graph in chain resolution.
bool RunnerAvailable(const NodeIndex &Idx, const Node &R)
{ return RunnerWrapper::ExecutableAvailable(R.Exec) || RunnerShipsBuild(Idx, R); }

//BFS shortest path of bridging runners carrying Start → Goal over the (pre-sorted better-first) runner edges. Empty
//bridge when Start==Goal (native content). Returns false when Goal is unreachable.
bool FindBridge(const std::string &Start, const std::string &Goal, const std::vector<const Node *> &Runners,
                std::vector<std::string> &OutBridge)
{
    OutBridge.clear();
    if (Start == Goal) return true;                                              // native content: terminal runs it directly
    std::map<std::string, std::pair<std::string, std::string>> Prev;             // platform -> {viaRunnerId, prevPlatform}
    std::set<std::string> Visited{Start};
    std::queue<std::string> Q; Q.push(Start);
    bool Found = false;
    while (!Q.empty() && !Found)
    {
        const std::string Cur = Q.front(); Q.pop();
        for (const Node *R : Runners)                                            // better-first → first discovery wins
        {
            if (!RunnerServes(R, Cur)) continue;
            const std::string &Next = R->HostPlatform;
            if (Visited.count(Next)) continue;
            Visited.insert(Next);
            Prev[Next] = {R->NodeId, Cur};
            if (Next == Goal) { Found = true; break; }
            Q.push(Next);
        }
    }
    if (!Found) return false;
    std::vector<std::string> Rev;
    for (std::string P = Goal; P != Start; )
    {
        auto It = Prev.find(P);
        if (It == Prev.end()) return false;
        Rev.push_back(It->second.first);
        P = It->second.second;
    }
    std::reverse(Rev.begin(), Rev.end());
    OutBridge = Rev;
    return true;
}

//The best authored native terminal (HOST==GUEST==machine) among the pre-sorted runners, or nullptr (→ synthesize).
const Node *PickNativeTerminal(const std::vector<const Node *> &Runners, const std::string &Machine)
{
    for (const Node *R : Runners)
        if (R->HostPlatform == Machine && RunnerServes(R, Machine)) return R;
    return nullptr;
}

//Build a resolved RunnerLink for a runner node id (synthesizing a passthrough terminal for kNativeTerminalId or a
//missing node). Build layers = the runner's content closure (PARENTS) minus the runner, in load order, absolutized.
RunnerLink BuildLink(const NodeIndex &Idx, const std::string &Id, const std::map<std::string, bool> &Toggles)
{
    RunnerLink L;
    const std::string Machine = MachinePlatform();
    if (Id == LaunchResolver::kNativeTerminalId)                                 // synthesized passthrough terminal
    { L.NodeId = Id; L.Name = "native"; L.HostPlatform = Machine; L.GuestPlatform = {Machine}; return L; }
    const Node *R = Idx.Find(Id);
    if (!R) { L.NodeId = Id; L.Name = Id; L.HostPlatform = Machine; L.GuestPlatform = {Machine}; return L; }

    const nlohmann::ordered_json E = R->Exec.is_object() ? R->Exec : nlohmann::ordered_json::object();
    L.NodeId = R->NodeId; L.Name = R->NodeId; L.PackagePath = R->BundleDir;
    L.Executable = E.value("EXECUTABLE", std::string());
    if (E.contains("ARGS") && E["ARGS"].is_array())             for (const auto &X : E["ARGS"])       L.Args.push_back(std::string(X));
    if (E.contains("ENV") && E["ENV"].is_object())              L.Env = E["ENV"];
    if (E.contains("REMOVE_ENV") && E["REMOVE_ENV"].is_array()) for (const auto &X : E["REMOVE_ENV"]) L.RemoveEnv.push_back(std::string(X));
    L.ContentRoot      = E.value("CONTENT_ROOT", std::string());
    L.PrefixGenerate   = E.value("PREFIX_GENERATE", false);
    L.UnifiedRuntime   = E.value("UNIFIED_RUNTIME", false);
    L.GuestPathTemplate= E.value("GUEST_PATH", std::string());
    L.HostPlatform     = R->HostPlatform;
    L.GuestPlatform    = R->GuestPlatform;
    ManifestModel::ForEachClosureNode(Idx, R->NodeId, Toggles, [&](const Node &N) {
        if (N.IsRunner() || !N.Layers.is_array()) return;
        for (nlohmann::ordered_json Lay : N.Layers)
        {
            if (!IsVfsLayer(LayerType(Lay))) continue;
            LaunchResolver::AbsolutizeLayerPaths(Lay, N.BundleDir);
            L.Layers.push_back(std::move(Lay));
        }
    });
    L.ShipsBuild = !L.Layers.empty();
    return L;
}

} // namespace

//Every usable runner on this machine, sorted better-first (RECOMMENDED > package-local > node-id) for
//deterministic BFS — shared by ResolveChainIds and ResolveChainTail (formerly duplicated verbatim).
static std::vector<const Node *> AvailableRunnersSorted(const NodeIndex &Idx, const Node &Launch)
{
    std::vector<const Node *> Runners;
    for (const auto &[Id, N] : Idx.Nodes)
        if (N.IsRunner() && RunnerAvailable(Idx, N)) Runners.push_back(&N);
    std::sort(Runners.begin(), Runners.end(), [&](const Node *A, const Node *B){ return RunnerBetterPtr(A, B, Launch); });
    return Runners;
}

std::vector<std::string> LaunchResolver::ResolveChainIds(const NodeIndex &Idx, const Node &Launch,
                                                         const struct ContainerParams &CP, const nlohmann::ordered_json &GlobalConfigJSON)
{
    const std::string Machine = MachinePlatform();

    //Available runners, sorted better-first (RECOMMENDED > package-local > node-id) for deterministic BFS.
    std::vector<const Node *> Runners = AvailableRunnersSorted(Idx, Launch);

    //1. Honor a pinned chain (passed CP.RunnerChainIds, else persisted RUNNER_CHAIN) when it forms a valid path to
    //   the machine. Append a terminal if the pin didn't include one.
    std::vector<std::string> Pinned = CP.RunnerChainIds;
    if (Pinned.empty())
    {
        auto US = GetPackageUserSettings(GlobalConfigJSON, CP.PackageUID);
        if (US.contains("RUNNER_CHAIN") && US["RUNNER_CHAIN"].is_array())
            for (const auto &X : US["RUNNER_CHAIN"]) if (X.is_string()) Pinned.push_back(std::string(X));
    }
    //A MULTI-VERSION package selects its runner through the PLATFORM GRAPH (below), not a per-node pin: e.g. each
    //Minecraft version declares PLATFORM "java_<N>" and the matching java_<N> runner declares GUEST ["java_<N>"], so
    //FindBridge routes it to exactly that runner — the native cross-platform mechanism, no runner recommendation needed.
    if (!Pinned.empty())
    {
        std::string Cur = Launch.HostPlatform;
        bool Ok = true;
        for (const std::string &Id : Pinned)
        {
            if (Id == kNativeTerminalId) { Ok = (Cur == Machine); break; }       // synthetic terminal valid only at machine
            const Node *R = Idx.Find(Id);
            if (!R || !R->IsRunner() || !RunnerAvailable(Idx, *R) || !RunnerServes(R, Cur)) { Ok = false; break; }
            Cur = R->HostPlatform;
        }
        if (Ok && Cur == Machine)
        {
            //Ensure the last link is a native terminal (HOST==GUEST==machine); append one if not.
            const std::string &Last = Pinned.back();
            const Node *LastR = (Last == kNativeTerminalId) ? nullptr : Idx.Find(Last);
            const bool LastIsTerminal = (Last == kNativeTerminalId) ||
                (LastR && LastR->HostPlatform == Machine && RunnerServes(LastR, Machine));
            if (!LastIsTerminal)
            { const Node *T = PickNativeTerminal(Runners, Machine); Pinned.push_back(T ? T->NodeId : std::string(kNativeTerminalId)); }
            return Pinned;
        }
    }

    //2. BFS default: bridge content-platform → machine, then append the native terminal.
    std::vector<std::string> Bridge;
    if (!FindBridge(Launch.HostPlatform, Machine, Runners, Bridge)) return {};   // unreachable on this machine
    const Node *Term = PickNativeTerminal(Runners, Machine);
    Bridge.push_back(Term ? Term->NodeId : std::string(kNativeTerminalId));
    return Bridge;
}

std::vector<std::string> LaunchResolver::ResolveChainTail(const NodeIndex &Idx, const std::string &FromPlatform,
                                                          const Node &Launch, const nlohmann::ordered_json &GlobalConfigJSON)
{
    (void)GlobalConfigJSON;
    const std::string Machine = MachinePlatform();
    std::vector<const Node *> Runners = AvailableRunnersSorted(Idx, Launch);

    std::vector<std::string> Bridge;
    if (!FindBridge(FromPlatform, Machine, Runners, Bridge)) return {};          // FromPlatform can't reach the machine
    const Node *Term = PickNativeTerminal(Runners, Machine);
    Bridge.push_back(Term ? Term->NodeId : std::string(kNativeTerminalId));
    return Bridge;
}

std::vector<RunnerLink> LaunchResolver::ResolveRunnerChain(const NodeIndex &Idx, const Node &Launch,
                                                           const struct ContainerParams &CP, const nlohmann::ordered_json &GlobalConfigJSON)
{
    std::vector<RunnerLink> Out;
    for (const std::string &Id : ResolveChainIds(Idx, Launch, CP, GlobalConfigJSON))
        Out.push_back(BuildLink(Idx, Id, CP.ModuleStates));
    return Out;
}

//===================================================================================================================
//                                         CROSS-NAMESPACE NESTING
//===================================================================================================================

int LaunchResolver::BoundaryLinkIndex(const struct ContainerParams &CP)
{
    int B = 0;
    for (int i = 0; i < (int)CP.RunnerChain.size(); ++i) if (!CP.RunnerChain[i].NativeNamespace()) B = i;
    return B;
}

bool LaunchResolver::ChainHasInnerLinks(const struct ContainerParams &CP)
{
    return BoundaryLinkIndex(CP) > 0;
}

//The effective guest-path template for a boundary runner: its explicit EXEC.GUEST_PATH, else DERIVED from CONTENT_ROOT
//for a wine-family runner (a "…/drive_c/<sub>" mount means the guest sees it at C:\<sub>\…), else "" (identity). The
//derivation makes cross-namespace nesting work for proton/wine/umu with zero authoring — it falls out of CONTENT_ROOT.
static std::string EffectiveGuestTemplate(const RunnerLink &Boundary)
{
    if (!Boundary.GuestPathTemplate.empty()) return Boundary.GuestPathTemplate;
    const std::string &CR = Boundary.ContentRoot;                                // raw, e.g. "pfx/drive_c/%PackageUID%"
    const std::string Key = "drive_c/";
    const auto Pos = CR.find(Key);
    if (Pos == std::string::npos) return std::string();                          // not a wine drive → identity
    std::string After = CR.substr(Pos + Key.size());                             // "%PackageUID%"
    std::replace(After.begin(), After.end(), '/', '\\');                         // guest uses backslashes
    return "C:\\" + After + (After.empty() ? "" : "\\") + "%REL%";               // → "C:\\%PackageUID%\\%REL%"
}

std::string LaunchResolver::GuestPath(const std::string &Template, const std::string &Rel, struct ContainerParams &CP)
{
    if (Template.empty()) return Rel;                                            // identity (native namespace)
    std::string R = Rel;
    //Wine-style template (drive letter / backslashes) → the relative path's separators must be backslashes too.
    if (Template.find('\\') != std::string::npos || Template.find("C:") != std::string::npos)
        std::replace(R.begin(), R.end(), '/', '\\');
    std::map<std::string, std::string> Vars = CP.GetVariablesMap();
    Vars["REL"] = R;
    std::string Out = Template;
    VarSubst::StringVariableSubstitution(Out, Vars);
    return Out;
}

LaunchResolver::GuestTarget LaunchResolver::ComposeGuestTarget(struct ContainerParams &CP)
{
    GuestTarget T;
    const int B = BoundaryLinkIndex(CP);
    if (B <= 0 || CP.RunnerChain.empty()) return T;                              // no inner links → classic (CrossNamespace=false)
    T.CrossNamespace = true;

    const RunnerLink &Boundary = CP.RunnerChain[B];
    const std::string Template = EffectiveGuestTemplate(Boundary);   // explicit GUEST_PATH, else derived from CONTENT_ROOT

    //The actual content (e.g. the ROM), as the boundary's guest path — what the innermost inner runner consumes.
    const std::string ContentGuest = GuestPath(Template, CP.ExePathRelative.string(), CP);

    //Compose the inner runners' guest command, innermost(content-runner) → just-inside-the-boundary. Each inner link
    //Ri runs Ri-1's command (or the content for the innermost); its build lives at <CONTENT_ROOT>/__runner_<id>__.
    //The result's program (the innermost-toward-boundary runner's guest exe) becomes the boundary's content target;
    //the rest are trailing args. For the common single-inner case (snes9x under proton) this is exactly
    //[snes9x_guest_exe, rom_guest].
    std::vector<std::string> Argv;     // full guest argv of the inner stack (program first)
    std::string PrevGuestCmd;          // inner command rendered as a single guest path token (for %Content% of the next)
    const std::map<std::string, std::string> NestBaseVars = CP.GetVariablesMap();   // hoisted out of the link/arg loops
    for (int i = 0; i < B; ++i)
    {
        const RunnerLink &L = CP.RunnerChain[i];
        //This inner runner's own executable, as a CONTENT_ROOT-relative path then a guest path. EXECUTABLE is a
        //build-relative path for a nestable inner runner (no %RunnerMount% — that mount doesn't exist inside the guest).
        std::string ExeRel = L.Executable;
        VarSubst::StringVariableSubstitution(ExeRel, NestBaseVars);
        while (!ExeRel.empty() && (ExeRel.front() == '/' )) ExeRel.erase(ExeRel.begin());
        const std::string ExeRelUnderRoot = InnerRunnerMountRel(L.NodeId) + (ExeRel.empty() ? "" : "/" + ExeRel);
        const std::string ExeGuest = GuestPath(Template, ExeRelUnderRoot, CP);

        //This runner's args: %Content%/%ContentPath% = what it runs (the previous inner command, or the content for
        //the innermost), expressed as a guest path. Other %tokens% expand normally.
        const std::string Target = (i == 0) ? ContentGuest : PrevGuestCmd;
        std::vector<std::string> ThisArgs;
        {
            //One override map per LINK (the old code rebuilt the whole variables map per ARG).
            std::map<std::string, std::string> V = NestBaseVars;
            V["Content"] = Target; V["ContentPath"] = Target;
            for (const std::string &Raw : L.Args)
            { std::string A = Raw; VarSubst::StringVariableSubstitution(A, V); ThisArgs.push_back(A); }
        }

        //Nest: this runner's [exe, its args] runs the previous inner command, so prepend it to the growing argv.
        std::vector<std::string> Nested;
        Nested.push_back(ExeGuest);
        for (const std::string &A : ThisArgs) Nested.push_back(A);
        for (const std::string &A : Argv)     Nested.push_back(A);
        Argv.swap(Nested);
        PrevGuestCmd = ExeGuest;                                                 // outer runner targets this exe
    }

    //Argv[0] is the program the boundary must run; the boundary composes its guest path from %ContentPath%, so report
    //the innermost-toward-boundary runner's CONTENT_ROOT-relative exe as ContentRel, and Argv[1..] as trailing args.
    const RunnerLink &Outermost = CP.RunnerChain[B - 1];
    std::string OuterExeRel = Outermost.Executable;
    { std::map<std::string, std::string> V = CP.GetVariablesMap(); VarSubst::StringVariableSubstitution(OuterExeRel, V); }
    while (!OuterExeRel.empty() && OuterExeRel.front() == '/') OuterExeRel.erase(OuterExeRel.begin());
    T.ContentRel = InnerRunnerMountRel(Outermost.NodeId) + (OuterExeRel.empty() ? "" : "/" + OuterExeRel);
    for (size_t i = 1; i < Argv.size(); ++i) T.TrailingArgs.push_back(Argv[i]);
    return T;
}

//Native node-graph init — populates ContainerParams + the internal component pool DIRECTLY from the node graph.
