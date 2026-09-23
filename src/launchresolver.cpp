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
    //A DeclarePersist's PATH is a RUNTIME-relative location or a registry key (e.g. "HKCU") — NOT a bundle file.
    //It must not be joined against the package dir the way a Content layer's payload PATH is (that would turn
    //"HKCU" into "<bundle>/HKCU" and every persist target into an absolute mess). Its PATH is %var%-substituted
    //and normalized by DerivePersistence, which is the only consumer.
    if (L.is_object() && L.value("TYPE", std::string()) == "DeclarePersist") return;
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
    if (!CP.AuthoringBare && !Launch->IsRunnable())
        LogWarn("InitializeFromNode", "Node '" + LaunchId + "' has no effective ENTRYPOINTS (nothing beneath declares one) — not runnable.");

    //The exec is the SELECTED entry of the launch node's EFFECTIVE entrypoints (own, else inherited — a fact that
    //folds along the chain), or of EntryNode's: a ticked graft carrying an entry (a mod loader) runs over the
    //selected variant's mount. Nothing beneath the variant is OFFERED as a way to run it.
    const Node *ExecNode = Launch;
    if (!CP.EntryNode.empty() && CP.EntryNode != LaunchId)
    {
        ExecNode = Idx.Find(CP.EntryNode);
        if (!ExecNode) { LogErr("InitializeFromNode", "Entry node not found: " + CP.EntryNode); return false; }
        // The entry node must be a graft that is SELECTED for this launch (ticked and applicable): its entry runs
        // over a mount that contains it. A stale or unticked one would exec a loader that is not on the mount.
        bool Selected = false;
        for (const auto &O : ManifestModel::OfferedGrafts(Idx, LaunchId, CP.ModuleStates))
            if (O.Graft == ExecNode || O.Graft->Key() == ExecNode->Key()) { Selected = O.Selected && O.Applicable; break; }
        if (!Selected) { LogErr("InitializeFromNode", "Entry node '" + CP.EntryNode + "' is not a selected graft of '" + LaunchId + "' — it would run off a mount that does not contain it."); return false; }
    }
    CP.ComposedExec = ExecNode->ExecFor(CP.Entrypoint);
    if (!CP.Entrypoint.empty() && !CP.ComposedExec.is_object())
    {
        LogErr("InitializeFromNode", "Node '" + ExecNode->NodeId + "' has no entrypoint labelled '" + CP.Entrypoint + "'.");
        return false;
    }
    CP.subgame_id = LaunchId;  CP.VariantID = CP.Entrypoint.empty() ? "default" : CP.Entrypoint;
    CP.PackageUID = Launch->Uid.empty() ? LaunchId : Launch->Uid;
    CP.PackageName= Launch->Meta.is_object() ? Launch->Meta.value("TITLE", LaunchId) : LaunchId;
    CP.GameName   = CP.PackageName;
    CP.Platform   = CP.ComposedExec.is_object() ? CP.ComposedExec.value("PLATFORM", Launch->HostPlatform) : Launch->HostPlatform;
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

    //The runner platform keep-set: the Persist entries anywhere in the runner CHAIN's closures (where a
    //runner's user-state lives, with prefix-correct paths — e.g. proton's "pfx/drive_c/users" + "HKCU").
    //Folded into DerivePersistence before the game's, so every launch persists the standard save/config
    //locations with no per-game work.
    //
    //CLOSURE, not the runner node's own layers — and EVERY runner in the chain, not just the boundary.
    //
    //This was the THIRD read of "the runner node's own LAYERS", and the one the flat cutover missed while
    //generalising the other two (the prefix-assembly VFS walk and the order-independent edit fold). Pre-flat
    //the keep-set sat on the runner node itself; now it is its own Persist node in the runner's PARENTS — so
    //the old read returned the lone DeclareRunner layer and the keep-set reached NOTHING. Every game silently
    //lost its saves and its HKCU at exit, while DerivePersistence printed a green summary and
    //--audit-packages reported 960/960 clean, because nothing else resolves a runner closure for persistence.
    //
    //A node is one layer of one TYPE, so a DeclareExec node can never also carry a Persist: there is no
    //"the runner node's own keep-set" case left to fall back to.
    nlohmann::ordered_json RunnerKeep = nlohmann::ordered_json::array();
    for (const RunnerLink &Lnk : CP.RunnerChain)
    {
        //Every link that IS a node. Not filtered on NativeNamespace(): that asks whether the runner gives the
        //content its own root, which has nothing to do with whether it has user-state to keep — and with a
        //chain of runners that declare no CONTENT_ROOT it skipped all but one of them.
        if (!Idx.Find(Lnk.NodeId)) continue;
        ManifestModel::ForEachClosureNode(Idx, Lnk.NodeId, CP.ModuleStates, [&](const Node &N) {
            if (!N.Layers.is_array()) return;
            for (const auto &L : N.Layers)
                if (L.is_object() && L.value("TYPE", std::string()) == "DeclarePersist") RunnerKeep.push_back(L);
        });
    }
    CP.RunnerPersistLayers = std::move(RunnerKeep);

    if (RunnerNode)
    {
        //Runner build = the boundary runner node's closure, itself LAST (highest priority: it carries the placement
        //CustomVars — %DXVK_TARGET%/%FONTS_TARGET% — and, for a folded runner, the build layer itself) — for runner
        //CustomVar resolution (RunnerComponents/RunnerRecipe) and the UNIFIED fold.
        nlohmann::ordered_json RunnerComps = nlohmann::ordered_json::array();
        std::vector<std::string> RunnerBuildIds;
        ManifestModel::ForEachClosureNode(Idx, RunnerNode->NodeId, CP.ModuleStates, [&](const Node &N) {
            RunnerComps.push_back({{"COMPONENTID", N.NodeId}, {"SUBCOMPONENTS", AbsLayers(&N)}});
            RunnerBuildIds.push_back(N.NodeId);
        });
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
    const std::vector<std::string> BaseOrder = ManifestModel::ResolveNodeOrder(Idx, LaunchId, CP.ModuleStates, &Missing);
    for (const std::string &Id : BaseOrder)
    {
        if (Id == LaunchId) continue;
        const Node *N = Idx.Find(Id);
        if (!N) continue;
        AddComponent(N);
    }
    for (const auto &M : Missing) LogWarn("InitializeFromNode", "Unresolved requirement: " + M);
    if (Launch->Layers.is_array() && !Launch->Layers.empty())
    { Components.push_back({{"COMPONENTID", LaunchId + "__self"}, {"SUBCOMPONENTS", AbsLayers(Launch)}}); CP.Recipe.push_back(LaunchId + "__self"); }

    //GRAFTS — the selected nodes that are OVER this title without being in anybody's list — mount ABOVE the
    //launchable's own closure (later component = higher priority), in instance precedence. Scope: never a
    //RECEIVED browse stub (a friend's node, not hydrated) — only nodes of our own tree.
    for (const std::string &Id : ManifestModel::ResolveGraftOrder(Idx, LaunchId, CP.ModuleStates, BaseOrder, CP.GraftPrecedence,
                                                                  [](const Node &N) { return !N.Received && !N.BundleDir.empty(); }))
    {
        const Node *N = Idx.Find(Id);
        if (!N) continue;
        AddComponent(N);
    }

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
    //and wineboot RegEdits. Route the VFS ones into SubComponentsArray here (their %RunnerMount%/%TempPath% resolve
    //downstream, at mount/edit time). The generic BuildLayerSpec loop + BuildDefaultData then assemble the prefix with NO
    //special-case branch. Persist/CustomVar stay handled by RunnerPersistLayers / the resolver.
    //
    //WHICH layers are assembly is decided by the layer, not by which node it sits on: a RUNTIME-SOURCED layer
    //(%variable% PATH) resolves against the live runner mount, so it assembles the prefix; a layer with real
    //on-disk content IS the runner build and is mounted separately at RunnerMount. That used to be implicit —
    //assembly layers happened to sit on the same node as DeclareRunner, so "the runner node's own LAYERS" picked
    //them out. One node per layer makes node membership meaningless, so the real property is tested directly.
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
    {
        //Taken from the closure NODES, deliberately un-absolutized: a runtime-sourced PATH substitutes to an
        //ABSOLUTE runtime path downstream and is used as-is, so prepending a bundle dir (which the resolved
        //RunnerComponents subcomponents carry) would corrupt it into <bundle>/<abs path>.
        nlohmann::ordered_json PrefixVfs = nlohmann::ordered_json::array();   // base system → front (low priority)
        // Gate with the USER's toggles — the same map that produced RunnerRecipe just above, so the two walks
        // see the same closure by construction. Reconstructing a toggle map by first walking with {} could only
        // ever move a node OFF: the default-gated walk never visits an off-by-default node, so a node the user
        // switched ON got no entry and was gated off again, silently dropping its prefix-assembly layers.
        ManifestModel::ForEachClosureNode(Idx, CP.RunnerID, CP.ModuleStates, [&](const Node &N) {
            if (!N.Layers.is_array()) return;
            for (const auto &L : N.Layers)
                if (ManifestModel::IsVfsLayer(L.value("TYPE", std::string()))
                    && ManifestModel::IsRuntimeSourcedLayer(L)) PrefixVfs.push_back(L);
        });
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

