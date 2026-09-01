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

// P6 split: this TU keeps the RESOLUTION SPINE (InitializeFromNode) + the cross-TU layer-path helper.
// The concerns live in sibling TUs: launchresolver_vars/_persist/_exec/_chain.cpp.

//Absolutize a layer's PATH and SOURCE.PATH against the owning node's BundleDir (in place). Shared by
//the spine's AbsLayers and the chain TU's BuildLink.
void LaunchResolver::AbsolutizeLayerPaths(nlohmann::ordered_json &L, const std::filesystem::path &BundleDir)
{
    if (L.contains("PATH") && L["PATH"].is_string())
    { std::filesystem::path P = std::string(L["PATH"]); if (!P.is_absolute()) L["PATH"] = (BundleDir / P).string(); }
    if (L.contains("SOURCE") && L["SOURCE"].is_object() && L["SOURCE"].contains("PATH") && L["SOURCE"]["PATH"].is_string())
    { std::filesystem::path P = std::string(L["SOURCE"]["PATH"]); if (!P.is_absolute()) L["SOURCE"]["PATH"] = (BundleDir / P).string(); }
}

bool LaunchResolver::InitializeFromNode(struct ContainerParams &ContainerParams, nlohmann::ordered_json &ComponentPool, const nlohmann::ordered_json &GlobalConfigJSON)
{
    auto &CP = ContainerParams;
    const NodeIndex &Idx = *CP.NodeIdx;
    const std::string LaunchId = CP.LaunchNodeId;
    const Node *Launch = Idx.Find(LaunchId);
    if (!Launch) { LogErr("InitializeFromNode", "Launch node not found: " + LaunchId); return false; }
    if (!CP.AuthoringBare && !Launch->IsLaunchable())
        LogWarn("InitializeFromNode", "Node '" + LaunchId + "' has no DeclareExec (not launchable).");

    CP.subgame_id = LaunchId;  CP.VariantID = "default";
    CP.PackageUID = Launch->Uid.empty() ? LaunchId : Launch->Uid;
    CP.PackageName= Launch->Meta.is_object() ? Launch->Meta.value("TITLE", LaunchId) : LaunchId;
    CP.GameName   = CP.PackageName;
    CP.Platform   = Launch->HostPlatform;
    CP.PackagePath= AppPaths::PackagePathOverride().empty() ? Launch->BundleDir : AppPaths::PackagePathOverride();  // --package-dir / in-package
    CP.UMUID      = Launch->Meta.is_object() ? Launch->Meta.value("UMUID", std::string("0")) : "0";

    //Absolutize a node's LAYERS PATH/SOURCE.PATH against its OWN bundle dir (cross-bundle-correct).
    auto AbsLayers = [](const Node *N) -> nlohmann::ordered_json
    {
        nlohmann::ordered_json Out = nlohmann::ordered_json::array();
        if (!N->Layers.is_array()) return Out;
        for (nlohmann::ordered_json L : N->Layers)
        {
            LaunchResolver::AbsolutizeLayerPaths(L, N->BundleDir);
            Out.push_back(std::move(L));
        }
        return Out;
    };

    nlohmann::ordered_json Components = nlohmann::ordered_json::array();
    CP.Recipe.clear();
    auto AddComponent = [&](const Node *N)
    { Components.push_back({{"COMPONENTID", N->NodeId}, {"SUBCOMPONENTS", AbsLayers(N)}}); CP.Recipe.push_back(N->NodeId); };

    //AUTHORING BARE MODE: no runner, no prefix, content at the root. The session just wants the node's content overlay
    //mounted with a writable upper as a capture workbench (a fresh node has no content at all — that's fine). Runner-
    //driven tools (e.g. "run a Windows exe in a wine prefix") rebuild via the normal path below with a pinned runner.
    if (CP.AuthoringBare)
    {
      CP.ContentRoot.clear();
      CP.RunnerPersistLayers = nlohmann::ordered_json::array();   // no runner → no runner keep-set
    }
    else
    {
    //Resolve the runner CHAIN (daisy-chaining): innermost→outermost runner links from the content platform to the
    //machine, always terminated by a native runner. A length-1 bridge ([proton, native]) is the classic case.
    CP.RunnerChain = ResolveRunnerChain(Idx, *Launch, CP, GlobalConfigJSON);
    if (CP.RunnerChain.empty())
    {
        //No runner chain reaches this machine — the container cannot be built (there is no runnerless launch path;
        //even native games resolve through native-passthrough). Abort rather than building around an empty command.
        LogErr("InitializeFromNode", "No runner chain found for platform '" + CP.Platform
               + "' — cannot launch '" + LaunchId + "'. Install a compatible runner.");
        return false;
    }

    //The runtime BOUNDARY runner owns the FUSE mount / wine prefix: the OUTERMOST link that creates a guest fs
    //(non-native namespace, e.g. proton). If the whole chain is native (native content), it's the first link (the
    //terminal running content directly). The legacy single-runner fields below are this boundary runner's view —
    //a length-1 [proton, native] chain populates them exactly as the old single-runner path did.
    int BoundaryIdx = 0;
    for (int i = 0; i < (int)CP.RunnerChain.size(); ++i) if (!CP.RunnerChain[i].NativeNamespace()) BoundaryIdx = i;
    const RunnerLink &Boundary = CP.RunnerChain[BoundaryIdx];
    const Node *RunnerNode = Idx.Find(Boundary.NodeId);                          // null for a synthesized native terminal

    CP.RunnerID          = Boundary.NodeId;
    CP.RunnerName        = Boundary.Name;
    CP.RunnerPackagePath = Boundary.PackagePath;
    CP.RunnerExecutable  = Boundary.Executable;
    CP.ContentRoot       = Boundary.ContentRoot;
    CP.PrefixGenerate    = Boundary.PrefixGenerate;
    CP.RunnerEnv         = Boundary.Env;
    CP.RunnerRemoveEnv   = Boundary.RemoveEnv;
    CP.RunnerArgs        = Boundary.Args;
    CP.UnifiedRuntime    = Boundary.UnifiedRuntime;
    CP.RunnerLayers      = Boundary.Layers;
    CP.RunnerShipsBuild  = Boundary.ShipsBuild;

    //The boundary runner's platform keep-set: its OWN LAYERS' Persist entries (where THIS runner's user-state lives,
    //with prefix-correct paths — e.g. proton's "pfx/drive_c/users/steamuser" + "HKCU"). Folded into DerivePersistence
    //before the game's, so every launch persists the standard save/config locations with no per-game work. Runner-node
    //LAYERS are otherwise ignored (the build comes from PARENTS), but Persist is a policy declaration, not a VFS layer.
    CP.RunnerPersistLayers = (RunnerNode && RunnerNode->Layers.is_array()) ? RunnerNode->Layers : nlohmann::ordered_json::array();

    if (RunnerNode)
    {
        //Runner build = the boundary runner node's content closure (its PARENTS) — for runner CustomVar resolution
        //(RunnerComponents/RunnerRecipe) and the UNIFIED fold.
        nlohmann::ordered_json RunnerComps = nlohmann::ordered_json::array();
        std::vector<std::string> RunnerBuildIds;
        ManifestModel::ForEachClosureNode(Idx, RunnerNode->NodeId, CP.ModuleStates, [&](const Node &N) {
            if (N.IsRunner()) return;
            RunnerComps.push_back({{"COMPONENTID", N.NodeId}, {"SUBCOMPONENTS", AbsLayers(&N)}});
            RunnerBuildIds.push_back(N.NodeId);
        });
        //The runner NODE itself carries the placement CustomVars (e.g. %DXVK_TARGET%/%FONTS_TARGET% that mount its lib
        //components at the right prefix paths). The loop above skips it (it's the DeclareRunner boundary), so add it
        //back — otherwise those knobs never reach ResolveCustomVariables and the components mount at literal %TARGET%.
        RunnerComps.push_back({{"COMPONENTID", RunnerNode->NodeId}, {"SUBCOMPONENTS", AbsLayers(RunnerNode)}});
        RunnerBuildIds.push_back(RunnerNode->NodeId);
        CP.RunnerComponents = RunnerComps;
        CP.RunnerRecipe     = RunnerBuildIds;
        CP.RunnerEndpoints  = CP.UnifiedRuntime ? RunnerBuildIds : std::vector<std::string>{};
        //UNIFIED: fold the runner build into the game RUNTIME (mount first = lowest priority).
        if (CP.UnifiedRuntime)
            for (const std::string &Id : RunnerBuildIds) { const Node *N = Idx.Find(Id); if (N) AddComponent(N); }
    }
    }   // end !AuthoringBare

    //Game content nodes (resolved order; runners + the launch node excluded), then the launch node's own layers.
    std::vector<std::string> Missing;
    for (const std::string &Id : ManifestModel::ResolveNodeOrder(Idx, LaunchId, CP.ModuleStates, &Missing))
    {
        if (Id == LaunchId) continue;
        const Node *N = Idx.Find(Id);
        if (!N || N->IsRunner()) continue;
        AddComponent(N);
    }
    for (const auto &M : Missing) LogWarn("InitializeFromNode", "Unresolved parent: " + M);
    if (Launch->Layers.is_array() && !Launch->Layers.empty())
    { Components.push_back({{"COMPONENTID", LaunchId + "__self"}, {"SUBCOMPONENTS", AbsLayers(Launch)}}); CP.Recipe.push_back(LaunchId + "__self"); }

    //Compose the launch EXEC across the closure: every DeclareExec contributor merges field-by-field in closure order
    //(parents first, the launchable last → highest priority), so a base supplies CONTENTPATH/WORKDIR and a variant/mod
    //overrides EXEARGS, etc. Runners are excluded (their EXEC is the runner's own, resolved via the chain). Uses the same
    //ComposeAcrossClosure merge as the index-time DeclareLibraryItem/Meta inheritance and the CustomVar/Persist passes.
    CP.ComposedExec = ManifestModel::ComposeAcrossClosure(Idx, LaunchId, CP.ModuleStates,
        [](const Node &N) -> const nlohmann::ordered_json * {
            return (!N.IsRunner() && N.IsLaunchable() && N.Exec.is_object()) ? &N.Exec : nullptr;
        });

    //The internal component pool the generic iterators (BuildSubComponentsArray/ResolveCustomVariables/
    //DerivePersistence/BuildDefaultData) consume — built from nodes, never authored or read from disk.
    ComponentPool = nlohmann::ordered_json::object();
    ComponentPool["PACKAGEUID"]  = CP.PackageUID;
    ComponentPool["PACKAGENAME"] = CP.PackageName;
    ComponentPool["COMPONENTS"]  = Components;

    DerivePaths(CP, GlobalConfigJSON);
    ResolveCustomVariables(ComponentPool, CP, GlobalConfigJSON);
    BuildSubComponentsArray(ComponentPool, CP);
    //A prefix-generating runner contributes prefix-ASSEMBLY layers to the RUNTIME closure: default_pfx/DLL VFSDirLayers
    //(sourced from "%RunnerMount%/..." — the enabler substitutes the runtime path), config_info/version/marker FileEdits,
    //and wineboot RegEdits. They live on the runner NODE (its content comes from PARENTS, so its own LAYERS are otherwise
    //ignored), so route the VFS/FileEdit/RegEdit ones into SubComponentsArray here (their %RunnerMount%/%TempPath% resolve
    //downstream, at mount/edit time). The generic BuildLayerSpec loop + BuildDefaultData then assemble the prefix with NO
    //special-case branch. Persist/CustomVar stay handled by RunnerPersistLayers / the resolver.
    //
    //LAYER PRIORITY (vidyagodfs: later layer = higher priority): the wine prefix (default_pfx + system32/syswow64 builtin
    //DLLs) is the BASE SYSTEM — it must sit BENEATH the game/library content so a package's native override DLLs win over
    //wine's builtins at the same path (e.g. DirectPlay's native dplayx.dll in syswow64 overriding proton's builtin). So the
    //runner's prefix-assembly VFS layers are PREPENDED (lowest priority), NOT appended. Appending them (the proton-decompose
    //regression) put wine builtins ON TOP, shadowing every syswow64/system32 native override (DirectPlay broke; any
    //game-supplied system DLL was masked). FileEdit/RegEdit are order-independent (separate DEFAULTDATA/registry passes) so
    //they stay appended.
    //The non-VFS layers come from the runner's WHOLE CLOSURE, not just the runner node: a runner may pin library
    //nodes that carry DllOverride/RegEdit/FileEdit (e.g. a media stack that installs native DirectShow filters and
    //switches winegstreamer off). Previously only the runner NODE's own FileEdit/RegEdit were routed and
    //DllOverride was dropped entirely, so such a library silently did nothing when pinned by a runner — the runner
    //chain was strictly less capable than the content chain. VFS stays split by design: the runner node's own VFS
    //layers are prefix ASSEMBLY (below), while its PARENTS' VFS layers build the runner tree itself and are mounted
    //separately at RunnerMount — a runner-side layer therefore cannot target the game prefix, which is why
    //prefix-content libraries still belong on the game.
    if (const Node *RN = Idx.Find(CP.RunnerID); RN && RN->Layers.is_array())
    {
        nlohmann::ordered_json PrefixVfs = nlohmann::ordered_json::array();   // base system → front (low priority)
        for (const auto &L : RN->Layers)
            if (ManifestModel::IsVfsLayer(L.value("TYPE", std::string()))) PrefixVfs.push_back(L);
        if (!PrefixVfs.empty())
        {
            for (auto &L : CP.SubComponentsArray) PrefixVfs.push_back(std::move(L));   // game/library content ON TOP
            CP.SubComponentsArray = std::move(PrefixVfs);
        }
    }
    //Order-independent edits from every active runner component (scoped to RunnerRecipe so an inactive
    //multi-version component can't contribute), appended last — separate DEFAULTDATA/registry/override passes.
    {
        const std::set<std::string> RunnerWant(CP.RunnerRecipe.begin(), CP.RunnerRecipe.end());
        for (const auto &Comp : CP.RunnerComponents)
        {
            if (!Comp.is_object() || !Comp.contains("SUBCOMPONENTS") || !Comp["SUBCOMPONENTS"].is_array()) continue;
            if (!RunnerWant.empty() && !RunnerWant.count(Comp.value("COMPONENTID", std::string()))) continue;
            const std::map<std::string, std::string> RunnerVars = CP.GetVariablesMap();
            for (const auto &L : Comp["SUBCOMPONENTS"])
            {
                const std::string T = L.value("TYPE", std::string());
                if (T != "FileEdit" && T != "RegEdit" && T != "DllOverride" && T != "BinaryPatch") continue;
                if (L.contains("WHEN") && L["WHEN"].is_string()
                    && !VarSubst::EvaluateCondition(L["WHEN"], RunnerVars)) continue;   // WHEN false → inert
                CP.SubComponentsArray.push_back(L);
            }
        }
    }
    DerivePersistence(ComponentPool, CP);
    LogSucc("InitializeFromNode", "Resolved node '" + LaunchId + "' (runner " + CP.RunnerName + ", "
            + std::to_string(CP.Recipe.size()) + " component(s)).");
    return true;
}

