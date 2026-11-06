#include <sys/stat.h>
#include "launchresolver.h"
#include "apppaths.h"        // AppPaths::DataRoot — the app data root the launch TEMP hangs off of
#include "varsubst.h"        // VarSubst::StringVariableSubstitution / RenderValue
#include "packagecatalog.h"  // GetPackageUserSettings (catalog/user-settings service)
#include "runnerwrapper.h"   // RunnerWrapper::ExecutableAvailable / DefPrefixDir
#include "commonutils.h"     // Log*
#include "builtinrunners.h"  // BuiltinRunners::IsBuiltin — the tie-break toward the built-in runner

#include <QDir>
#include <QGuiApplication>
#include <QScreen>
#include <QThread>

#include <algorithm>
#include <cctype>
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
//naturally (GetPackageUserSettings / MachinePlatform / IsVfsLayer / ...).
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

//A runner RECOMMENDED under the launched tile (or its family) is preferred for it.
static bool RecommendedFor(const Node &R, const Node &Launch)
{
    for (const auto &U : R.Recommended) if (U == Launch.Uid || U == Launch.PackageUid) return true;
    return false;
}

// P6 split: the runner daisy-chain machinery (availability, BFS bridge, links, nesting) — spine in launchresolver.cpp.
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
//bundle) > the specialist (fewer guest platforms: snes9x before a multi-system emulator that also runs SNES) > built
//into VidyaGod > LABEL > CID. Runners are pre-sorted better-first so the first edge that discovers a platform is best.
//The last two make the order total: a label may repeat, and two runners equal on every count were ordered by whim.
bool RunnerBetterPtr(const Node *A, const Node *B, const Node &Launch)
{
    if (RecommendedFor(*A, Launch) != RecommendedFor(*B, Launch)) return RecommendedFor(*A, Launch);
    const bool LA = !Launch.BundleDir.empty() && A->BundleDir == Launch.BundleDir;
    const bool LB = !Launch.BundleDir.empty() && B->BundleDir == Launch.BundleDir;
    if (LA != LB) return LA;
    if (A->GuestPlatform.size() != B->GuestPlatform.size()) return A->GuestPlatform.size() < B->GuestPlatform.size();
    const bool BA = BuiltinRunners::IsBuiltin(A->Cid), BB = BuiltinRunners::IsBuiltin(B->Cid);
    if (BA != BB) return BA;
    if (A->NodeId != B->NodeId) return A->NodeId < B->NodeId;
    return A->Key() < B->Key();
}

bool RunnerServes(const Node *R, const std::string &Platform)
{ for (const auto &G : R->GuestPlatform) if (G == Platform) return true; return false; }

//True if a runner SHIPS ITS OWN BUILD: any VFS layer in its closure (its own layers, or what it is OVER — the
//same rule as a game's mount). Such a runner's executable resolves from the mounted build, not the system PATH —
//so it's "available" even when EXECUTABLE is a bare command (e.g. a win32 emulator "snes9x.exe" nested under
//proton). Local-PATH or CID layers both count.
bool RunnerShipsBuild(const NodeIndex &Idx, const Node &R)
{
    for (const std::string &Id : ManifestModel::Closure(Idx, R.Key()))
        if (const Node *N = Idx.Find(Id))
            for (const auto &L : N->ContentLayers)
                if (ManifestModel::IsRunnerBuildLayer(L)) return true;
    return false;
}

//A runner can be used on this machine when its executable resolves on the system PATH OR it ships its own build (the
//exe lives in the build). Used to filter the runner-edge graph in chain resolution.
bool RunnerAvailable(const NodeIndex &Idx, const Node &R)
{ return RunnerWrapper::ExecutableAvailable(R.Exec) || RunnerShipsBuild(Idx, R); }

//BFS shortest path of bridging runners carrying Start → Goal over the (pre-sorted better-first) runner edges. Empty
//bridge when Start==Goal (native content). Returns false when Goal is unreachable.
bool FindBridge(const std::string &Start, const std::string &Goal, const std::vector<const Node *> &Runners,
                const std::filesystem::path &LaunchBundleDir, std::vector<std::string> &OutBridge)
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
            if (!ManifestModel::ServesPlatform(*R, Cur, LaunchBundleDir)) continue;   // "custom": the package's own engine only
            const std::string &Next = R->HostPlatform;
            if (Visited.count(Next)) continue;
            Visited.insert(Next);
            Prev[Next] = {R->Key(), Cur};
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
RunnerLink BuildLink(const NodeIndex &Idx, const std::string &Id)
{
    RunnerLink L;
    const std::string Machine = MachinePlatform();
    if (Id == LaunchResolver::kNativeTerminalId)                                 // synthesized passthrough terminal
    { L.NodeId = Id; L.Name = "native"; L.HostPlatform = Machine; L.GuestPlatform = {Machine}; return L; }
    const Node *R = Idx.Find(Id);
    if (!R) { L.NodeId = Id; L.Name = Id; L.HostPlatform = Machine; L.GuestPlatform = {Machine}; return L; }

    const nlohmann::ordered_json E = R->Exec.is_object() ? R->Exec : nlohmann::ordered_json::object();
    L.NodeId = R->Key(); L.Name = R->NodeId; L.PackagePath = R->BundleDir;   // the link is keyed by the INDEX key: a label may repeat (a received stub beside a local runner)
    L.Executable = E.value("EXECUTABLE", std::string());
    if (E.contains("ARGS") && E["ARGS"].is_array())             for (const auto &X : E["ARGS"])       L.Args.push_back(std::string(X));
    //The link's environment and build are its OWN resolve (the runner node and everything it contains), the same
    //fold as a game's.
    Fold::Plan P;
    const nlohmann::ordered_json Ops = LaunchResolver::RunnerOps(Idx, R->Key(), &P);
    L.Env = nlohmann::ordered_json::object();
    for (const auto &[K, V] : P.Env.items())
    {
        if (V.is_null()) L.RemoveEnv.push_back(K);
        else L.Env[K] = V;
    }
    L.ContentRoot      = E.value("CONTENT_ROOT", std::string());
    L.PrefixGenerate   = E.value("PREFIX_GENERATE", false);
    L.UnifiedRuntime   = E.value("UNIFIED_RUNTIME", false);
    L.GuestPathTemplate= E.value("GUEST_PATH", std::string());
    if (E.contains("GUEST_ROOTS") && E["GUEST_ROOTS"].is_object())
        L.GameDirGuest = E["GUEST_ROOTS"].value("%GameDir%", std::string());
    L.HostPlatform     = R->HostPlatform;
    L.GuestPlatform    = R->GuestPlatform;
    //A runtime-sourced layer (a %variable% PATH — the prefix-assembly mounts) is NOT part of the runner's importable
    //build: nothing on disk to hydrate or verify. LowerPlan already made the real ones absolute.
    for (const auto &Lay : Ops)
        if (ManifestModel::IsRunnerBuildLayer(Lay)) L.Layers.push_back(Lay);
    L.ShipsBuild = !L.Layers.empty();
    return L;
}

} // namespace

//Every usable runner on this machine, sorted better-first (RECOMMENDED > package-local > specialist > built-in > LABEL > CID) for
//deterministic BFS — shared by ResolveChainIds and ResolveChainTail (formerly duplicated verbatim).
static std::vector<const Node *> AvailableRunnersSorted(const NodeIndex &Idx, const Node &Launch)
{
    std::vector<const Node *> Runners;
    for (const auto &[Id, N] : Idx.Nodes)
        if (N.IsRunner() && RunnerAvailable(Idx, N)) Runners.push_back(&N);
    std::sort(Runners.begin(), Runners.end(), [&](const Node *A, const Node *B){ return RunnerBetterPtr(A, B, Launch); });
    return Runners;
}

//The installed runners among them (same order). A launch runs on installed runners when they make a chain, so the
//chain the resolver picks is the one the Play gate (PackageCatalog::HasUsableChain) found.
static std::vector<const Node *> InstalledOf(const NodeIndex &Idx, const std::vector<const Node *> &Runners)
{
    std::vector<const Node *> Out;
    for (const Node *R : Runners) if (PackageCatalog::RunnerInstalled(Idx, R->Key())) Out.push_back(R);
    return Out;
}

//The chain over one set of runners: a valid pin, else the shortest bridge; then the native terminal — from the set,
//else any available one (All), else the synthesized passthrough. Empty when the set cannot carry the launch to this
//machine.
static std::vector<std::string> ChainOver(const NodeIndex &Idx, const Node &Launch, const std::string &Host,
                                          const std::vector<std::string> &Pinned, const std::vector<const Node *> &Runners,
                                          const std::vector<const Node *> &All)
{
    const std::string Machine = MachinePlatform();
    auto Usable = [&](const Node *R) { return std::find(Runners.begin(), Runners.end(), R) != Runners.end(); };
    auto Terminal = [&]() {
        const Node *T = PickNativeTerminal(Runners, Machine);
        if (!T) T = PickNativeTerminal(All, Machine);
        return T ? T->Key() : std::string(LaunchResolver::kNativeTerminalId);
    };
    if (!Pinned.empty())
    {
        std::string Cur = Host;
        bool Ok = true;
        for (const std::string &Id : Pinned)
        {
            if (Id == LaunchResolver::kNativeTerminalId) { Ok = (Cur == Machine); break; }   // synthetic terminal valid only at machine
            const Node *R = Idx.Find(Id);
            if (!R || !R->IsRunner() || !Usable(R) || !ManifestModel::ServesPlatform(*R, Cur, Launch.BundleDir)) { Ok = false; break; }
            Cur = R->HostPlatform;
        }
        if (Ok && Cur == Machine)
        {
            //Ensure the last link is a native terminal (HOST==GUEST==machine); append one if not.
            std::vector<std::string> Out = Pinned;
            const std::string &Last = Out.back();
            const Node *LastR = (Last == LaunchResolver::kNativeTerminalId) ? nullptr : Idx.Find(Last);
            const bool LastIsTerminal = (Last == LaunchResolver::kNativeTerminalId) ||
                (LastR && LastR->HostPlatform == Machine && RunnerServes(LastR, Machine));
            if (!LastIsTerminal) Out.push_back(Terminal());
            return Out;
        }
    }
    std::vector<std::string> Bridge;
    if (!FindBridge(Host, Machine, Runners, Launch.BundleDir, Bridge)) return {};   // unreachable with these runners
    //A bridge whose last runner is itself a terminal here (HOST == GUEST == this machine: windows-native carries win32 to
    //win64 and runs win64) already ends the chain. Appending the terminal named that runner a second time and nested it
    //inside itself — every win32 game on Windows got a command "<game>/__runner_<it>__/<the game's exe>".
    const Node *Last = Bridge.empty() ? nullptr : Idx.Find(Bridge.back());
    if (!(Last && Last->HostPlatform == Machine && RunnerServes(Last, Machine))) Bridge.push_back(Terminal());
    return Bridge;
}

std::vector<std::string> LaunchResolver::ResolveChainIds(const NodeIndex &Idx, const Node &Launch,
                                                         const struct ContainerParams &CP, const nlohmann::ordered_json &GlobalConfigJSON)
{
    //Available runners, sorted better-first (RECOMMENDED > package-local > specialist > built-in > LABEL > CID) for deterministic BFS.
    const std::vector<const Node *> Runners = AvailableRunnersSorted(Idx, Launch);

    //A pinned chain (passed CP.RunnerChainIds, else persisted RUNNER_CHAIN) is honoured when it forms a valid path to
    //the machine; a terminal is appended if the pin didn't include one.
    std::vector<std::string> Pinned = CP.RunnerChainIds;
    if (Pinned.empty() && !CP.IgnoreSavedChain)
    {
        auto US = GetPackageUserSettings(GlobalConfigJSON, CP.PackageUID, CP.InstanceName);
        if (US.contains("RUNNER_CHAIN") && US["RUNNER_CHAIN"].is_array())
            for (const auto &X : US["RUNNER_CHAIN"]) if (X.is_string()) Pinned.push_back(std::string(X));
    }
    const std::string Host = !CP.Platform.empty() ? CP.Platform : Launch.HostFor(CP.Entrypoint);
    //Nothing pinned: the entry's RECOMMENDEDRUNNER (a runner LABEL — a version the game needs, as a Proton build with a
    //working GameInput) is the chain's first runner, a soft pin. Installed chains still come first, so a recommended
    //runner not installed while another carries the launch is passed over, and one the library lacks changes nothing:
    //the default chain, as without it.
    bool Soft = false;
    if (Pinned.empty())
    {
        const nlohmann::ordered_json E = Launch.ExecFor(CP.Entrypoint);
        const std::string Rec = E.is_object() ? E.value("RECOMMENDEDRUNNER", std::string()) : std::string();
        if (!Rec.empty())
        {
            //An installed runner with the label first (a friend's catalog may hold another node of that label), and a
            //runner that is not itself this machine's gets the rest of its chain, as a pin through it would.
            const Node *Pick = nullptr;
            for (const Node *R : InstalledOf(Idx, Runners)) if (R->NodeId == Rec && RunnerServes(R, Host)) { Pick = R; break; }
            if (!Pick) for (const Node *R : Runners) if (R->NodeId == Rec && RunnerServes(R, Host)) { Pick = R; break; }
            if (Pick)
            {
                Pinned = {Pick->Key()};
                if (Pick->HostPlatform != ManifestModel::MachinePlatform())
                    for (const std::string &T : ResolveChainTail(Idx, Pick->HostPlatform, Launch)) Pinned.push_back(T);
                Soft = true;
            }
            if (!Soft)
                LogOut("LaunchResolver::ResolveChainIds", "the entry recommends runner '" + Rec + "', which the library does not "
                       "have for platform '" + Host + "' — the default chain");
        }
    }
    //A MULTI-VERSION package selects its runner through the PLATFORM GRAPH, not a per-node pin: e.g. each Minecraft
    //version declares PLATFORM "java_<N>" and the matching java_<N> runner declares GUEST ["java_<N>"], so FindBridge
    //routes it to exactly that runner — the native cross-platform mechanism, no runner recommendation needed.
    //Installed runners first: a longer chain of installed runners beats a shorter one that still has to be
    //downloaded. Only when nothing installed carries it is the chain the one to install (what a download fetches).
    std::vector<std::string> Out = ChainOver(Idx, Launch, Host, Pinned, InstalledOf(Idx, Runners), Runners);
    if (Out.empty()) Out = ChainOver(Idx, Launch, Host, Pinned, Runners, Runners);
    //The pin is the instance's, and an instance may run variants of different platforms: only a pin whose first runner
    //runs this launch's platform was meant for it.
    const Node *PinFirst = Pinned.empty() ? nullptr : Idx.Find(Pinned.front());
    const bool PinMeantHere = PinFirst && PinFirst->IsRunner() && RunnerServes(PinFirst, Host);
    if (Soft)
    {
        if (!Out.empty() && Out.front() != Pinned.front())
            LogOut("LaunchResolver::ResolveChainIds", "the recommended runner '" + PinFirst->NodeId + "' is not installed or does "
                   "not carry the launch here — the default chain");
        return Out;
    }
    if (!Pinned.empty() && !PinFirst && Pinned.front() != kNativeTerminalId)
        LogWarn("LaunchResolver::ResolveChainIds", "the pinned runner chain names '" + Pinned.front()
                + "', which the library does not have — using the default chain");
    if (PinMeantHere && !Out.empty() && !std::equal(Pinned.begin(), Pinned.end(), Out.begin(), Out.begin() + std::min(Out.size(), Pinned.size())))
        LogWarn("LaunchResolver::ResolveChainIds", "the pinned runner chain does not run this launch here (a runner is "
                "missing, or it is another package's engine) — using the default chain");
    return Out;
}

std::vector<std::string> LaunchResolver::ResolveChainTail(const NodeIndex &Idx, const std::string &FromPlatform, const Node &Launch)
{
    const std::vector<const Node *> Runners = AvailableRunnersSorted(Idx, Launch);
    std::vector<std::string> Out = ChainOver(Idx, Launch, FromPlatform, {}, InstalledOf(Idx, Runners), Runners);
    return Out.empty() ? ChainOver(Idx, Launch, FromPlatform, {}, Runners, Runners) : Out;
}

std::vector<RunnerLink> LaunchResolver::ResolveRunnerChain(const NodeIndex &Idx, const Node &Launch,
                                                           const struct ContainerParams &CP, const nlohmann::ordered_json &GlobalConfigJSON)
{
    std::vector<RunnerLink> Out;
    for (const std::string &Id : ResolveChainIds(Idx, Launch, CP, GlobalConfigJSON))
        Out.push_back(BuildLink(Idx, Id));
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

//The effective guest-path template for a boundary runner: its explicit EXEC.GUEST_PATH, else where its guest sees the
//game's content (GUEST_ROOTS %GameDir%: C:\%PackageUID% for a prefix runner) + the relative path, else "" (identity).
static std::string EffectiveGuestTemplate(const RunnerLink &Boundary)
{
    if (!Boundary.GuestPathTemplate.empty()) return Boundary.GuestPathTemplate;
    if (Boundary.GameDirGuest.empty()) return std::string();                     // no guest namespace → identity
    //A Windows guest path (a drive, backslashes) joins with '\\'; a native guest's %GameDir%, a runtime path
    //(%RuntimePath%/…), joins with '/'.
    const std::string &G = Boundary.GameDirGuest;
    const bool Windows = G.find('\\') != std::string::npos
                      || (G.size() >= 2 && std::isalpha(static_cast<unsigned char>(G[0])) && G[1] == ':');
    return G + (Windows ? "\\%REL%" : "/%REL%");                         // → "C:\%PackageUID%\%REL%"
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
    const std::string Template = EffectiveGuestTemplate(Boundary);   // explicit GUEST_PATH, else %GameDir% in the guest

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
        //This inner runner's own executable, as a CONTENT_ROOT-relative path then a guest path. Its build is mounted
        //at its own __runner_<id>__ folder, so %RunnerMount% names THAT folder: "" in the build-relative exe path, its
        //guest path in arguments. (Left to the base map, %RunnerMount% was the BOUNDARY's host mount — a Windows engine
        //runner written "%RunnerMount%/x.exe", as it must be to run directly on Windows, nested under proton as
        //"__runner_<id>__/tmp/…/RUNNER/x.exe".)
        const std::string MountGuest = GuestPath(Template, InnerRunnerMountRel(L.NodeId), CP);
        std::string ExeRel = L.Executable;
        { std::map<std::string, std::string> V = NestBaseVars; V["RunnerMount"] = ""; VarSubst::StringVariableSubstitution(ExeRel, V); }
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
            V["Content"] = Target; V["ContentPath"] = Target; V["RunnerMount"] = MountGuest;
            for (const std::string &Raw : L.Args)
            { std::string A = Raw; VarSubst::StringVariableSubstitution(A, V); ThisArgs.push_back(A); }
            //Its environment, read the same way. Links run innermost first, so a later (outer) link never
            //overwrites what an inner one set.
            for (const auto &[K, Val] : L.Env.items())
            {
                if (!Val.is_string())
                {
                    LogWarn("LaunchResolver::ComposeGuestTarget", "runner '" + L.NodeId + "' ENV key '" + K + "' is "
                            + std::string(Val.type_name()) + ", not a string — skipped. Environment values are strings (quote the number).");
                    continue;
                }
                if (T.Env.contains(K) || std::find(T.RemoveEnv.begin(), T.RemoveEnv.end(), K) != T.RemoveEnv.end()) continue;
                //Kept escaped: Execute substitutes the composed ENV once more, and writes it out then.
                std::string E = Val.get<std::string>(); VarSubst::StringVariableSubstitution(E, V, /*KeepEscapes=*/true); T.Env[K] = E;
            }
            for (const std::string &K : L.RemoveEnv)
                if (!T.Env.contains(K) && std::find(T.RemoveEnv.begin(), T.RemoveEnv.end(), K) == T.RemoveEnv.end())
                    T.RemoveEnv.push_back(K);
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
    { std::map<std::string, std::string> V = CP.GetVariablesMap(); V["RunnerMount"] = ""; VarSubst::StringVariableSubstitution(OuterExeRel, V); }
    while (!OuterExeRel.empty() && OuterExeRel.front() == '/') OuterExeRel.erase(OuterExeRel.begin());
    T.ContentRel = InnerRunnerMountRel(Outermost.NodeId) + (OuterExeRel.empty() ? "" : "/" + OuterExeRel);
    for (size_t i = 1; i < Argv.size(); ++i) T.TrailingArgs.push_back(Argv[i]);
    return T;
}

void LaunchResolver::ApplyInnerEnv(struct ContainerParams &CP, const GuestTarget &T)
{
    //The game's own ENV / REMOVE_ENV were merged into RunnerEnv already, and the game wins over every runner.
    auto GameSays = [&](const std::string &K) {
        return (CP.LaunchEnv.is_object() && CP.LaunchEnv.contains(K))
            || std::find(CP.LaunchRemoveEnv.begin(), CP.LaunchRemoveEnv.end(), K) != CP.LaunchRemoveEnv.end();
    };
    if (!CP.RunnerEnv.is_object()) CP.RunnerEnv = nlohmann::ordered_json::object();
    for (const std::string &K : T.RemoveEnv)
    {
        if (GameSays(K)) continue;
        CP.RunnerEnv.erase(K);
        if (std::find(CP.RunnerRemoveEnv.begin(), CP.RunnerRemoveEnv.end(), K) == CP.RunnerRemoveEnv.end())
            CP.RunnerRemoveEnv.push_back(K);
    }
    for (const auto &[K, V] : T.Env.items())
        if (!GameSays(K)) CP.RunnerEnv[K] = V;
}

//Native node-graph init — populates ContainerParams + the internal component pool DIRECTLY from the node graph.

//Derives the wine/proton prefix LAYOUT variables (%DefaultPfxDir%, %WineSys32Dir%, …) from the runner mount.
//Extracted from the launch sequence so the AUDIT can call it too: it builds every mount plan without mounting
//anything, and without these the prefix-assembly layers' PATHs keep their %tokens% — 168 warnings and 56 errors
//of pure context artifact, on a library where every one of them resolves fine at launch. Existence-probing
//degrades to the documented last-resort path when nothing is mounted, which is exactly what a plan needs.
void LaunchResolver::ProbePrefixLayout(struct ContainerParams &ContainerParams)
{
    //THE GATE LIVES HERE, not at the call sites. These variables exist only for a prefix-generating runner that
    //ships a build; defining them for anything else does not "fix" an undefined %token%, it HIDES one — a layer
    //under a native runner that references %DefaultPfxDir% mounts at that literal path and the game sees none of
    //its files. The audit defining them unconditionally would have reported such a package clean.
    if (!ContainerParams.PrefixGenerate || !ContainerParams.RunnerShipsBuild
        || ContainerParams.RunnerMountPath.empty())
        return;

    const std::filesystem::path RM = ContainerParams.RunnerMountPath;
    std::error_code Pec;
    const std::string Root = std::filesystem::exists(RM / "files" / "lib" / "wine", Pec) ? "files" : "dist";
    const std::string Lib  = (Root == "files") ? "lib" : "lib64";
    //The two PE dirs are probed INDEPENDENTLY: newer protons unify both arches under lib/wine, but
    //GE-Proton7-era builds split them (lib/wine/i386-windows + lib64/wine/x86_64-windows). Deriving both from
    //one Lib pointed WineSys32Dir at a nonexistent dir on GE-7 — the system32 assembly layer mounted EMPTY
    //(caught by the mount plan's missing-source check during the 2026-09-01 overnight run; harmless for a
    //32-bit game, fatal for any 64-bit one on that runner).
    auto PeDir = [&](const char *Arch) -> std::string {
        for (const char *L : {"lib", "lib64"})
        {
            std::error_code E;
            const std::filesystem::path P = RM / Root / L / "wine" / Arch;
            if (std::filesystem::exists(P, E)) return P.string();
        }
        return (RM / Root / Lib / "wine" / Arch).string();   // old behavior as the last resort
    };
    auto &V = ContainerParams.CustomVariables;
    V["DefaultPfxDir"]   = (RM / Root / "share" / "default_pfx").string() + "/";
    V["WineFontsDir"]    = (RM / Root / "share" / "fonts").string() + "/";
    V["WineLibDir"]      = (RM / Root / Lib).string() + "/";
    V["WineSys32Dir"]    = PeDir("x86_64-windows");
    V["WineSysWow64Dir"] = PeDir("i386-windows");
    struct stat St{};
    const std::string SysReg = (RM / Root / "share" / "default_pfx" / "system.reg").string();
    V["SysRegMtime"] = (::stat(SysReg.c_str(), &St) == 0) ? (std::to_string((long long)St.st_mtime) + ".0") : "0";
    LogOut("LaunchResolver::ProbePrefixLayout", "Probed prefix layout: " + Root + " (sysreg mtime " + V["SysRegMtime"] + ")");
}
