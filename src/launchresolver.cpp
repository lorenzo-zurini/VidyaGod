#include "launchresolver.h"
#include "apppaths.h"        // AppPaths::DataRoot — the app data root the launch TEMP hangs off of
#include "varsubst.h"        // VarSubst::StringVariableSubstitution / RenderValue
#include "packagecatalog.h"  // GetPackageUserSettings (catalog/user-settings service)
#include "runnerwrapper.h"   // RunnerWrapper::ExecutableAvailable / DefPrefixDir
#include "commonutils.h"     // Log*
#include "fold.h"            // Fold::Resolve — the row, resolved
#include "nodelower.h"       // NodeLower::LowerPlan / LowerEntry — the plan as engine ops

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

//A runner's build, resolved on its own node (§4.5: each runner resolves the same way a game does) and lowered in its
//own layout (a runner's layers are written in it; GUEST_ROOTS map only a PACKAGE's guest coordinates).
nlohmann::ordered_json LaunchResolver::RunnerOps(const NodeIndex &Idx, const std::string &RunnerId, Fold::Plan *PlanOut)
{
    const Fold::Plan P = Fold::Resolve(ManifestModel::LibraryOf(Idx), RunnerId);
    if (!P.Error.empty()) LogWarn("LaunchResolver::RunnerOps", "runner '" + RunnerId + "': " + P.Error);
    if (PlanOut) *PlanOut = P;
    return NodeLower::LowerPlan(P);
}

std::string LaunchResolver::GuestToLayout(struct ContainerParams &CP, const std::string &Path)
{
    //An anchor resolves to its guest path (C:\802 — the variables map says so: the same spelling a registry value
    //gets); a guest path lands where its drive lives in the runner's layout.
    const std::map<std::string, std::string> Vars = CP.GetVariablesMap();
    std::string P = Path;
    if (P.find('%') != std::string::npos) VarSubst::StringVariableSubstitution(P, Vars);
    if (!CP.Drives.is_object() || CP.Drives.empty()) return P;           // no drives: nothing is a guest path to map
    const bool Guest = P.size() >= 2 && std::isalpha(static_cast<unsigned char>(P[0])) && P[1] == ':';
    if (!Guest) return P;
    std::replace(P.begin(), P.end(), '\\', '/');                        // a guest path: Windows separators
    for (size_t I; (I = P.find("//")) != std::string::npos;) P.erase(I, 1);   // "E:\\" + "/x" is E:/x
    nlohmann::ordered_json Drives = nlohmann::ordered_json::object();
    for (const auto &[D, V] : CP.Drives.items())
    {
        std::string R = V.is_string() ? V.get<std::string>() : std::string();
        VarSubst::StringVariableSubstitution(R, Vars);
        Drives[D] = R;
    }
    return Fold::ToLayout(P, Drives);
}

nlohmann::ordered_json LaunchResolver::AnchorRegEdits(struct ContainerParams &CP, nlohmann::ordered_json Edits)
{
    const std::map<std::string, std::string> Vars = CP.GetVariablesMap();
    std::map<std::string, std::string> Anchors;                          // %Anchor% -> its guest path, resolved
    if (CP.GuestRoots.is_object())
        for (const auto &[A, V] : CP.GuestRoots.items())
        {
            const auto It = A.size() > 2 ? Vars.find(A.substr(1, A.size() - 2)) : Vars.end();
            if (It != Vars.end()) Anchors[A] = It->second;
        }
    if (Anchors.empty() || !Edits.is_array()) return Edits;
    for (auto &E : Edits)
        if (E.is_object() && E.contains("KEYVALUES") && E["KEYVALUES"].is_object())
            for (auto &[K, V] : E["KEYVALUES"].items())
                if (V.is_string()) V = Fold::ToAnchors(V.get<std::string>(), Anchors);
    return Edits;
}

std::vector<std::pair<std::string, std::string>> LaunchResolver::AnchorLayouts(struct ContainerParams &CP)
{
    std::vector<std::pair<std::string, std::string>> Out;
    if (CP.GuestRoots.is_object())
        for (const auto &[A, V] : CP.GuestRoots.items()) Out.emplace_back(A, GuestToLayout(CP, A));
    return Out;
}

bool LaunchResolver::InitializeFromNode(struct ContainerParams &ContainerParams, nlohmann::ordered_json &ComponentPool, const nlohmann::ordered_json &GlobalConfigJSON)
{
    auto &CP = ContainerParams;
    const NodeIndex &Idx = *CP.NodeIdx;
    const std::string LaunchId = CP.LaunchNodeId;
    const Node *Launch = Idx.Find(LaunchId);
    if (!Launch) { LogErr("InitializeFromNode", "Launch node not found: " + LaunchId); return false; }
    if (!Launch->LowerError.empty()) { LogErr("InitializeFromNode", Launch->LowerError); return false; }
    const std::string LaunchKey = Launch->Key();
    const Fold::Library Lib = ManifestModel::LibraryOf(Idx);

    //The launched TILE: the one asked for (a card is one tile; a version may present several), else the node's first.
    const std::string Face = !CP.LaunchFace.empty() ? CP.LaunchFace : Launch->Uid;
    if (!CP.LaunchFace.empty() && std::find(Launch->Faces.begin(), Launch->Faces.end(), Face) == Launch->Faces.end())
    { LogErr("InitializeFromNode", "Node '" + LaunchId + "' does not present tile '" + Face + "'."); return false; }
    const nlohmann::ordered_json *FaceTile = Idx.Tile(Face);
    const nlohmann::ordered_json &FaceMeta = FaceTile ? *FaceTile : Launch->Meta;
    CP.subgame_id = LaunchId;  CP.VariantID = CP.Entrypoint.empty() ? "default" : CP.Entrypoint;
    CP.PackageUID = Launch->PackageUid.empty() ? LaunchId : Launch->PackageUid;
    CP.PackageName= FaceMeta.is_object() ? FaceMeta.value("TITLE", LaunchId) : LaunchId;
    CP.GameName   = CP.PackageName;
    CP.PackagePath= AppPaths::PackagePathOverride().empty() ? Launch->BundleDir : AppPaths::PackagePathOverride();  // --package-dir / in-package
    CP.UMUID      = FaceMeta.is_object() ? FaceMeta.value("UMUID", std::string("0")) : "0";

    //Phase 1 reads the instance's values and the built-ins (%UID% = the launched face, %PackageUID% = its family).
    Fold::Vars Instance;
    {
        const nlohmann::ordered_json Saved = GetPackageVariables(GlobalConfigJSON, CP.PackageUID, CP.InstanceName);
        if (Saved.is_object()) for (const auto &[K, V] : Saved.items()) if (V.is_string()) Instance[K] = V.get<std::string>();
        for (const auto &[K, V] : CP.VariableOverrides) Instance[K] = V;
    }
    Fold::Vars Builtins = CP.GetVariablesMap();
    Builtins["UID"] = Face;
    Builtins["PackageUID"] = CP.PackageUID;

    //The grafts: the instance's list in its order — each applies when it is offered with those before it applied (a
    //graft may need another graft) — else a fresh instance's: the ones RECOMMENDED under this tile.
    std::vector<std::string> Dropped;
    CP.AppliedGrafts = PackageCatalog::AppliedGrafts(Idx, LaunchKey, CP.Grafts, Instance, Builtins, &Dropped);
    for (const std::string &G : Dropped)
        LogWarn("InitializeFromNode", "Graft '" + G + "' is not offered to '" + LaunchId + "' at its position (its ANY does not hold, or it is not installed) — not applied.");
    const Fold::Plan Plan = Fold::Resolve(Lib, LaunchKey, Instance, Builtins, CP.AppliedGrafts);
    if (!Plan.Error.empty()) { LogErr("InitializeFromNode", "Node '" + LaunchId + "': " + Plan.Error); return false; }
    const auto Name = [&](const std::string &Cid) { const Node *N = Idx.Find(Cid); return N && !N->NodeId.empty() ? N->NodeId : Cid; };
    for (const auto &[Ev, Cid] : Plan.Events)
        if (Ev == "missing" || Ev == "cycle")
            LogWarn("InitializeFromNode", "Resolving '" + LaunchId + "': " + Ev + " " + Name(Cid));
    //A row whose requirements fail is blocked (§4): a NOT names something it contains, or an ANY finds none.
    if (Fold::Unsatisfied(Plan) > 0)
    {
        for (const auto &[Ev, Cid] : Plan.Events)
        {
            if (Ev == "not-hit")
                LogErr("InitializeFromNode", "'" + LaunchId + "' is blocked: it contains '" + Name(Cid) + "', which a NOT in it excludes.");
            else if (Ev == "any-unmet")
                LogErr("InitializeFromNode", "'" + LaunchId + "' is blocked: '" + Name(Cid) + "' requires one of the nodes its ANY names, and none is present.");
        }
        return false;
    }

    //The entry that runs: the one named (the row's folded EXEC, grafts' entries included), else the launched tile's
    //entry, else the first game entry.
    const nlohmann::ordered_json *Entry = nullptr;
    const auto IsGame = [](const nlohmann::ordered_json &E) {
        return !(E.contains("GUEST") && E["GUEST"].is_array() && !E["GUEST"].empty()) && E.contains("HOST"); };
    if (!CP.Entrypoint.empty())
    {
        if (!Plan.Exec.contains(CP.Entrypoint))
        { LogErr("InitializeFromNode", "Node '" + LaunchId + "' has no entry labelled '" + CP.Entrypoint + "'."); return false; }
        Entry = &Plan.Exec[CP.Entrypoint];
    }
    else
    {
        for (const auto &[L, E] : Plan.Exec.items())
            if (IsGame(E) && E.contains("TILE") && E["TILE"].is_object() && E["TILE"].value("UID", std::string()) == Face) { Entry = &E; break; }
        if (!Entry)
            for (const auto &[L, E] : Plan.Exec.items())
                if (IsGame(E)) { Entry = &E; break; }
    }
    if (!Entry && !CP.AuthoringBare)
        LogWarn("InitializeFromNode", "Node '" + LaunchId + "' has no entry to run (nothing in its fold declares one) — not runnable.");
    CP.ComposedExec = Entry ? NodeLower::LowerEntry(*Entry) : nlohmann::ordered_json::object();
    CP.Platform = CP.ComposedExec.value("PLATFORM", Launch->HostPlatform);

    nlohmann::ordered_json Components = nlohmann::ordered_json::array();
    CP.Recipe.clear();
    nlohmann::ordered_json GuestRoots;                                   // the boundary runner's guest-coordinate map
    nlohmann::ordered_json RunnerOpsJ = nlohmann::ordered_json::array();  // the boundary runner's build, lowered

    //AUTHORING BARE MODE: no runner, no prefix, content at the root — the session wants the node's content overlay
    //mounted with a writable upper as a capture workbench.
    if (CP.AuthoringBare)
    {
      CP.ContentRoot.clear();
      CP.RunnerPersistLayers = nlohmann::ordered_json::array();
      //A runner-less workbench has no guest: it lays each anchor any runner declares out as a top-level folder of
      //its name (%GameDir%/x → GameDir/x), so the content it mounts, the capture preview and a captured TARGET all
      //spell the same place.
      CP.GuestRoots = nlohmann::ordered_json::object();
      CP.Drives = nlohmann::ordered_json::object();
      for (const auto &[Id, N] : Idx.Nodes)
          if (N.OwnRunner && N.Exec.is_object() && N.Exec.contains("GUEST_ROOTS") && N.Exec["GUEST_ROOTS"].is_object())
              for (const auto &[A, V] : N.Exec["GUEST_ROOTS"].items())
                  if (A.size() > 2) CP.GuestRoots[A] = A.substr(1, A.size() - 2);
    }
    else
    {
    //The runner CHAIN (daisy-chaining): innermost→outermost links from the entry's platform to the machine, always
    //terminated by a native runner — found by capability, never named by the package.
    CP.RunnerChain = ResolveRunnerChain(Idx, *Launch, CP, GlobalConfigJSON);
    if (CP.RunnerChain.empty())
    {
        LogErr("InitializeFromNode", "No runner chain found for platform '" + CP.Platform
               + "' — cannot launch '" + LaunchId + "'. Install a compatible runner.");
        return false;
    }
    //The runtime BOUNDARY runner owns the FUSE mount / wine prefix: the OUTERMOST link that creates a guest fs.
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
    if (RunnerNode && RunnerNode->Exec.is_object() && RunnerNode->Exec.contains("GUEST_ROOTS")) GuestRoots = RunnerNode->Exec["GUEST_ROOTS"];
    CP.GuestRoots = GuestRoots;
    CP.Drives = RunnerNode && RunnerNode->Exec.is_object() && RunnerNode->Exec.contains("DRIVES")
              ? RunnerNode->Exec["DRIVES"] : nlohmann::ordered_json::object();

    //Where user state lives by the runner's choice: the KEEP entries of every runner in the chain (ours keep
    //nothing; the author decides). Folded into DerivePersistence before the game's.
    nlohmann::ordered_json RunnerKeep = nlohmann::ordered_json::array();
    for (const RunnerLink &Lnk : CP.RunnerChain)
    {
        if (!Idx.Find(Lnk.NodeId)) continue;
        for (const auto &L : RunnerOps(Idx, Lnk.NodeId))
            if (LayerType(L) == "DeclarePersist") RunnerKeep.push_back(L);
    }
    CP.RunnerPersistLayers = std::move(RunnerKeep);

    if (RunnerNode)
    {
        RunnerOpsJ = RunnerOps(Idx, RunnerNode->Key());
        CP.RunnerComponents = nlohmann::ordered_json::array({ { {"COMPONENTID", RunnerNode->NodeId}, {"SUBCOMPONENTS", RunnerOpsJ} } });
        CP.RunnerRecipe     = { RunnerNode->NodeId };
        CP.RunnerEndpoints  = CP.UnifiedRuntime ? CP.RunnerRecipe : std::vector<std::string>{};
        //UNIFIED: the runner build is mounted in the game RUNTIME, beneath the game (first = lowest priority).
        if (CP.UnifiedRuntime)
        {
            Components.push_back({ {"COMPONENTID", RunnerNode->NodeId + "__runner"}, {"SUBCOMPONENTS", RunnerOpsJ} });
            CP.Recipe.push_back(RunnerNode->NodeId + "__runner");
        }
    }
    }   // end !AuthoringBare

    //The game: the resolved row, lowered. Its paths stay in guest coordinates until substituted (GuestToLayout).
    Components.push_back({ {"COMPONENTID", LaunchId}, {"SUBCOMPONENTS", NodeLower::LowerPlan(Plan)} });
    CP.Recipe.push_back(LaunchId);

    //The environment folds per name like everything else (null = removed) — the GAME's, merged over the runner's at exec.
    CP.LaunchEnv = nlohmann::ordered_json::object();
    CP.LaunchRemoveEnv.clear();
    for (const auto &[K, V] : Plan.Env.items())
    {
        if (V.is_null()) CP.LaunchRemoveEnv.push_back(K);
        else CP.LaunchEnv[K] = V;
    }

    //The internal component pool the generic iterators (BuildSubComponentsArray/ResolveCustomVariables/
    //DerivePersistence/BuildDefaultData) consume — built from the plan, never authored or read from disk.
    ComponentPool = nlohmann::ordered_json::object();
    ComponentPool["PACKAGEUID"]  = CP.PackageUID;
    ComponentPool["PACKAGENAME"] = CP.PackageName;
    ComponentPool["COMPONENTS"]  = Components;

    DerivePaths(CP, GlobalConfigJSON);
    ResolveCustomVariables(ComponentPool, CP, GlobalConfigJSON);
    BuildSubComponentsArray(ComponentPool, CP);
    //A prefix-generating runner assembles the prefix from RUNTIME-SOURCED layers (a %variable% PATH resolved against
    //the live runner mount: default_pfx, the builtin DLL dirs). They are the BASE SYSTEM — prepended beneath the game
    //so a package's native DLLs win over wine's builtins at the same path. A layer with real bytes is the runner
    //BUILD, mounted separately at RunnerMount.
    {
        nlohmann::ordered_json PrefixVfs = nlohmann::ordered_json::array();
        for (const auto &L : RunnerOpsJ)
            if (ManifestModel::IsVfsLayer(LayerType(L)) && ManifestModel::IsRuntimeSourcedLayer(L)) PrefixVfs.push_back(L);
        if (!PrefixVfs.empty())
        {
            for (auto &L : CP.SubComponentsArray) PrefixVfs.push_back(std::move(L));
            CP.SubComponentsArray = std::move(PrefixVfs);
        }
    }
    //The runner's edits, registry and DLL overrides, appended — separate DEFAULTDATA/registry/override passes.
    {
        const std::map<std::string, std::string> RunnerVars = CP.GetVariablesMap();
        for (const auto &L : RunnerOpsJ)
        {
            const std::string T = LayerType(L);
            if (T != "FileEdit" && T != "RegEdit" && T != "DllOverride" && T != "BinaryPatch") continue;
            if (L.contains("WHEN") && L["WHEN"].is_string() && !VarSubst::EvaluateCondition(L["WHEN"], RunnerVars)) continue;
            CP.SubComponentsArray.push_back(L);
        }
    }
    DerivePersistence(ComponentPool, CP);
    LogSucc("InitializeFromNode", "Resolved node '" + LaunchId + "' (runner " + CP.RunnerName + ", "
            + std::to_string(Plan.Order.size()) + " node(s), " + std::to_string(CP.AppliedGrafts.size()) + " graft(s)).");
    return true;
}

