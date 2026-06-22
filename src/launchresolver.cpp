#include "launchresolver.h"
#include "varsubst.h"        // VarSubst::StringVariableSubstitution / TranslateCustomVarValue
#include "packagecatalog.h"  // GetPackageUserSettings (catalog/user-settings service)
#include "runnerwrapper.h"   // RunnerWrapper::ExecutableAvailable / DefPrefixArtifact
#include "commonutils.h"     // Log*

#include <QDir>
#include <QGuiApplication>
#include <QScreen>
#include <QThread>

#include <filesystem>
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

//Resolves every CustomVar SUBCOMPONENT (TYPE:"CustomVar") of the components in the Recipe into
//ContainerParams.CustomVariables, keyed by the bare global token "KEY" (no COMPONENTID prefix).
//Components are walked in Recipe order, so a later component's var overrides an earlier one with the
//same KEY — bare keys are one shared knob: intentional sharing (a game seeding a runner's knob via
//FORCEVARS) is the feature, and the same logical knob declared across many version components collapses
//to one because only the selected variant's enabled components are ever in a recipe.
//
//Resolution priority for each variable (highest to lowest):
//  1. ContainerParams.VariableOverrides — set from --var KEY=VALUE CLI flags or UI picker
//  2. GlobalConfigJSON["USERSETTINGS"][PackageUID]["VARIABLES"] — persisted user choices
//  3. DEFAULT from the CustomVar definition
bool LaunchResolver::ResolveCustomVariables(const nlohmann::ordered_json &MANIFESTJSON, struct ContainerParams &ContainerParams, const nlohmann::ordered_json &GlobalConfigJSON)
{
    //Helper: resolve a single bare KEY/DEFAULT pair through the priority chain.
    auto ResolveOne = [&](const std::string &Key, const std::string &DefaultValue) -> std::string
    {
        if (ContainerParams.VariableOverrides.count(Key))
        {
            LogOut("ResolveCustomVariables", "CLI override: " + Key + " = " + ContainerParams.VariableOverrides.at(Key));
            return ContainerParams.VariableOverrides.at(Key);
        }
        auto US = GetPackageUserSettings(GlobalConfigJSON, ContainerParams.PackageUID);
        if (US.contains("VARIABLES") && US["VARIABLES"].contains(Key))
        {
            std::string Val = US["VARIABLES"][Key];
            LogOut("ResolveCustomVariables", "User setting: " + Key + " = " + Val);
            return Val;
        }
        LogOut("ResolveCustomVariables", "Default: " + Key + " = " + DefaultValue);
        return DefaultValue;
    };

    //Resolve a single CustomVar subcomponent into its bare global %KEY% — two-layer pipeline:
    //  Layer 1: StringVariableSubstitution expands %ScreenWidth%, %PackagePath%, and any
    //           already-resolved CustomVar (GetVariablesMap() is rebuilt per-var).
    //  Layer 2: TranslateCustomVarValue converts the display value to raw storage (e.g. dword).
    //DISPLAY is a UI-only flag; all vars resolve here regardless.
    auto ResolveCustomVar = [&](const nlohmann::ordered_json &CV)
    {
        std::string Key = CV.value("KEY", std::string());           // bare global token name
        if (Key.empty()) return;
        std::string VarType = CV.value("VARTYPE", std::string("string"));

        // random: pick a fresh value from OPTIONS each launch (CLI override still wins).
        if (VarType == "random")
        {
            if (ContainerParams.VariableOverrides.count(Key))
                ContainerParams.CustomVariables[Key] = ContainerParams.VariableOverrides.at(Key);
            else if (CV.contains("OPTIONS") && CV["OPTIONS"].is_array() && !CV["OPTIONS"].empty())
            {
                const auto &Opts = CV["OPTIONS"];
                static std::mt19937 Rng(std::random_device{}());
                std::uniform_int_distribution<size_t> Dist(0, Opts.size() - 1);
                std::string Picked = Opts[Dist(Rng)].value("VALUE", std::string());
                VarSubst::StringVariableSubstitution(Picked, ContainerParams.GetVariablesMap());
                ContainerParams.CustomVariables[Key] = Picked;
            }
            else { LogWarn("ResolveCustomVariables", "  " + Key + ": random has no OPTIONS."); ContainerParams.CustomVariables[Key] = ""; }
            LogOut("ResolveCustomVariables", "  " + Key + " = [random] " + ContainerParams.CustomVariables[Key]);
            return;
        }

        std::string Raw = ResolveOne(Key, CV.value("DEFAULT", std::string()));
        VarSubst::StringVariableSubstitution(Raw, ContainerParams.GetVariablesMap());
        ContainerParams.CustomVariables[Key] = VarSubst::TranslateCustomVarValue(Raw, VarType);
        LogOut("ResolveCustomVariables", "  " + Key + " = " + ContainerParams.CustomVariables[Key]);
    };

    //Walk the Recipe in order; for each component, resolve its CustomVar subcomponents. Later
    //components win (they re-assign the same bare key).
    LogOut("ResolveCustomVariables", "Resolving CustomVar subcomponents...");
    for (const std::string &CompID : ContainerParams.Recipe)
    {
        int Idx = FindComponentIndex(MANIFESTJSON, CompID);
        if (Idx == -1) continue;
        const auto &Comp = MANIFESTJSON["COMPONENTS"][Idx];
        if (!Comp.contains("SUBCOMPONENTS") || !Comp["SUBCOMPONENTS"].is_array()) continue;
        for (const auto &S : Comp["SUBCOMPONENTS"])
            if (S.is_object() && S.value("TYPE", std::string()) == "CustomVar")
                ResolveCustomVar(S);
    }

    //Also resolve the selected runner variant's CustomVars: its components mount separately (RunnerEndpoints
    //is empty), so they're never in the game Recipe — but a runner exposes tweakable knobs (referenced as
    //%KEY% in its ENV/ARGS) that a game seeds via FORCEVARS. This is how a game passes env/args to its runner,
    //no bespoke fields. Scope to the variant's enabled components (RunnerRecipe) so a monolithic multi-version
    //runner package doesn't let an inactive version's knob win.
    if (!ContainerParams.RunnerComponents.empty())
    {
        std::set<std::string> RunnerWant(ContainerParams.RunnerRecipe.begin(), ContainerParams.RunnerRecipe.end());
        for (const auto &Comp : ContainerParams.RunnerComponents)
        {
            if (!Comp.is_object() || !Comp.contains("SUBCOMPONENTS") || !Comp["SUBCOMPONENTS"].is_array()) continue;
            if (!RunnerWant.empty() && !RunnerWant.count(Comp.value("COMPONENTID", std::string()))) continue;
            for (const auto &S : Comp["SUBCOMPONENTS"])
                if (S.is_object() && S.value("TYPE", std::string()) == "CustomVar")
                    ResolveCustomVar(S);
        }
    }
    LogSucc("ResolveCustomVariables", "Resolved " + std::to_string(ContainerParams.CustomVariables.size()) + " custom variable(s).");
    return true;
}

//Derives the persistence policy from the Recipe's PersistDir/PersistFile/RegPersist subcomponents.
//Persistence is declared "at specific points" — a component that introduces a save folder, a config
//file, or registry state also declares that it should survive a session by carrying the matching
//Persist* subcomponent.
//
//  PersistDir    { "TYPE":"PersistDir",   "PATH":"<runtime-root-relative dir>"  } → bind-mounted live
//  PersistFile   { "TYPE":"PersistFile",  "PATH":"<runtime-root-relative file>" } → seeded/captured by copy
//  RegPersist    { "TYPE":"RegPersist" }                                         → whole user/system/userdef.reg
//  RegKeyPersist { "TYPE":"RegKeyPersist","REGPATH":"HKCU\\Software\\..." }       → just that key's subtree
//
//DEFAULT (no Persist* subcomponent anywhere in the Recipe): PersistAll=true — the durable
//UserDataPath becomes the union's RW branch and the entire overlay survives. Declaring ANY Persist*
//subcomponent switches to selective persistence (ephemeral WRITELAYER + only the declared targets).
//PATH strings are %VARIABLE%-substituted, so resolved CustomVars / system tokens are available.
bool LaunchResolver::DerivePersistence(const nlohmann::ordered_json &MANIFESTJSON, struct ContainerParams &ContainerParams)
{
    ContainerParams.PersistDirs.clear();
    ContainerParams.PersistFiles.clear();
    ContainerParams.PersistRegKeys.clear();
    ContainerParams.PersistRegistry = false;
    bool AnyDeclared = false;

    LogOut("DerivePersistence", "Scanning Recipe for Persist* subcomponents...");
    for (const std::string &CompID : ContainerParams.Recipe)
    {
        int Idx = FindComponentIndex(MANIFESTJSON, CompID);
        if (Idx == -1) continue;
        const auto &Comp = MANIFESTJSON["COMPONENTS"][Idx];
        if (!Comp.contains("SUBCOMPONENTS") || !Comp["SUBCOMPONENTS"].is_array()) continue;
        for (const auto &S : Comp["SUBCOMPONENTS"])
        {
            if (!S.is_object()) continue;
            std::string Type = S.value("TYPE", std::string());
            if (Type == "PersistDir" || Type == "PersistFile")
            {
                std::string Path = S.value("PATH", std::string());
                VarSubst::StringVariableSubstitution(Path, ContainerParams.GetVariablesMap());
                if (Path.empty()) { LogWarn("DerivePersistence", "  " + Type + " with empty PATH (skipped)."); continue; }
                AnyDeclared = true;
                if (Type == "PersistDir") { ContainerParams.PersistDirs.push_back(Path);  LogOut("DerivePersistence", "  PersistDir  " + Path); }
                else                      { ContainerParams.PersistFiles.push_back(Path); LogOut("DerivePersistence", "  PersistFile " + Path); }
            }
            else if (Type == "RegPersist")
            {
                AnyDeclared = true;
                ContainerParams.PersistRegistry = true;
                LogOut("DerivePersistence", "  RegPersist (whole-registry persist)");
            }
            else if (Type == "RegKeyPersist")
            {
                std::string RegPath = S.value("REGPATH", std::string());
                VarSubst::StringVariableSubstitution(RegPath, ContainerParams.GetVariablesMap());
                if (RegPath.empty()) { LogWarn("DerivePersistence", "  RegKeyPersist with empty REGPATH (skipped)."); continue; }
                AnyDeclared = true;
                ContainerParams.PersistRegKeys.push_back(RegPath);
                LogOut("DerivePersistence", "  RegKeyPersist " + RegPath);
            }
        }
    }

    //No selective persist points declared → persist the whole runtime overlay (durable RW branch).
    ContainerParams.PersistAll = !AnyDeclared;
    LogSucc("DerivePersistence",
            "PERSIST: ALL=" + std::string(ContainerParams.PersistAll ? "true" : "false") +
            " REGISTRY=" + std::string(ContainerParams.PersistRegistry ? "true" : "false") +
            " DIRS=" + std::to_string(ContainerParams.PersistDirs.size()) +
            " FILES=" + std::to_string(ContainerParams.PersistFiles.size()) +
            " REGKEYS=" + std::to_string(ContainerParams.PersistRegKeys.size()));
    return true;
}

//Finds the variant in MANIFESTJSON matching ContainerParams.VariantID
//under SUBGAMES[ContainerParams.subgame_id].VARIANTS and populates exe/work-dir/args fields.
//Must be called AFTER BuildSubComponentsArray and BEFORE BuildContainerRuntime.
bool LaunchResolver::ResolveExecutableDefinition(const nlohmann::ordered_json &MANIFESTJSON, struct ContainerParams &ContainerParams)
{
    if (ContainerParams.VariantID.empty())
    {
        LogWarn("ResolveExecutableDefinition", "No VariantID set — skipping.");
        return true;
    }
    // Source the exec definition: the launch node's EXEC (native node path) or the matching GAME variant (old path).
    const bool NodePath = (ContainerParams.NodeIdx != nullptr);
    nlohmann::ordered_json Resolved = nlohmann::ordered_json::object();
    if (NodePath)
    {
        const Node *L = ContainerParams.NodeIdx->Find(ContainerParams.subgame_id);
        if (L && L->Exec.is_object()) Resolved = L->Exec;     // empty EXEC = self-contained launchable (e.g. gemrb) — OK
    }
    else
    {
        int SubgameIdx = FindGameIndex(MANIFESTJSON, ContainerParams.subgame_id);
        if (SubgameIdx != -1 && MANIFESTJSON["GAMES"][SubgameIdx].contains("VARIANTS"))
            for (auto &V : MANIFESTJSON["GAMES"][SubgameIdx]["VARIANTS"])
                if (V.value("VARIANT_ID", std::string()) == ContainerParams.VariantID) { Resolved = V; break; }
        if (Resolved.is_null() || Resolved.empty())
        {
            LogErr("ResolveExecutableDefinition", "No variant found for VARIANT_ID='" + ContainerParams.VariantID + "' in subgame '" + ContainerParams.subgame_id + "'");
            return false;
        }
    }
    LogSucc("ResolveExecutableDefinition", "Resolved exec for: " + ContainerParams.subgame_id);
    // The variant declares ONE universal target path — CONTENTPATH: the path (relative to the program mount) to
    // whatever the runner runs or loads (an exe, a ROM, a data root, or nothing for a self-contained runner).
    // The runner composes how it's used from %ContentPath% (relative) / %Content% (absolute host) in its ARGS.
    {
        std::string ExeStr = Resolved.value("CONTENTPATH", std::string());
        VarSubst::StringVariableSubstitution(ExeStr, ContainerParams.GetVariablesMap());
        ContainerParams.ExePathRelative = std::filesystem::path(ExeStr);
    }
    ContainerParams.ExePathComplete = ContainerParams.ProgramPath / ContainerParams.ExePathRelative;
    LogOut("ResolveExecutableDefinition", "ContentPath: " + ContainerParams.ExePathRelative.string());
    LogOut("ResolveExecutableDefinition", "Content: " + ContainerParams.ExePathComplete.string());
    auto &WorkDirVal = Resolved["WORKDIR"];
    if (!WorkDirVal.is_null() && WorkDirVal.is_string() && !std::string(WorkDirVal).empty())
    {
        std::string WorkDirStr = std::string(WorkDirVal);
        VarSubst::StringVariableSubstitution(WorkDirStr, ContainerParams.GetVariablesMap());
        ContainerParams.WorkDirPathRelative = std::filesystem::path(WorkDirStr);
        ContainerParams.WorkDirPathComplete = ContainerParams.ProgramPath / ContainerParams.WorkDirPathRelative;
    }
    else if (!ContainerParams.ExePathRelative.empty())
    {
        // No explicit WORKDIR: default to the executable's OWN directory (not the content-mount root) — games
        // resolve their data/DLLs via paths relative to the exe's folder. This is the historical default and is
        // a no-op when the exe sits at the root (parent_path is empty → ProgramPath).
        ContainerParams.WorkDirPathRelative = ContainerParams.ExePathRelative.parent_path();
        ContainerParams.WorkDirPathComplete = ContainerParams.ExePathComplete.parent_path();
    }
    else ContainerParams.WorkDirPathComplete = ContainerParams.ProgramPath;   // self-contained runner (no CONTENTPATH)
    LogOut("ResolveExecutableDefinition", "WorkDirPathComplete: " + ContainerParams.WorkDirPathComplete.string());
    ContainerParams.ExeArgs.clear();
    auto &ExeArgsVal = Resolved["EXEARGS"];
    if (!ExeArgsVal.is_null() && ExeArgsVal.is_string() && !std::string(ExeArgsVal).empty())
    {
        std::string Args = std::string(ExeArgsVal);
        VarSubst::StringVariableSubstitution(Args, ContainerParams.GetVariablesMap());
        std::istringstream iss(Args);
        for (std::string tok; std::getline(iss, tok, ' ');) ContainerParams.ExeArgs.push_back(tok);
    }
    return true;
}

//Iterates all COMPONENTS in MANIFEST order, collects SUBCOMPONENTS from those whose
//COMPONENTID appears in Recipe, and appends them to SubComponentsArray.
//Variable substitution is applied to each subcomponent's JSON string at this point
//so all downstream consumers receive already-expanded values.
//CustomVar and Persist* subcomponents are skipped — they are handled by ResolveCustomVariables and
//DerivePersistence. MANIFEST order is preserved, which determines VFS layer stacking order.
bool LaunchResolver::BuildSubComponentsArray(const nlohmann::ordered_json &MANIFESTJSON, struct ContainerParams &ContainerParams)
{
    //Iterate in Recipe order (ancestor-first) so VFS layers are stacked correctly regardless
    //of how components are ordered in the manifest. For each Recipe entry, find the component
    //by ID and collect its subcomponents.
    for (const std::string &RecipeComponentID : ContainerParams.Recipe)
    {
        int Idx = FindComponentIndex(MANIFESTJSON, RecipeComponentID);
        if (Idx == -1) continue;
        auto &Subs = MANIFESTJSON["COMPONENTS"][Idx]["SUBCOMPONENTS"];
        for (int j = 0; j < (int)Subs.size(); j++)
        {
            //CustomVar and Persist* subcomponents are not VFS/registry ops applied here — CustomVar
            //is resolved by ResolveCustomVariables; PersistDir/PersistFile/RegPersist are consumed by
            //DerivePersistence and the seed/capture/bind steps. Skip them all.
            if (Subs[j].is_object())
            {
                std::string T = Subs[j].value("TYPE", std::string());
                if (T == "CustomVar" || T == "PersistDir" || T == "PersistFile" || T == "RegPersist" || T == "RegKeyPersist") continue;
            }
            //Serialize to string, substitute %VAR% tokens, then re-parse.
            std::string SubJSON = Subs[j].dump();
            VarSubst::StringVariableSubstitution(SubJSON, ContainerParams.GetVariablesMap());
            ContainerParams.SubComponentsArray.push_back(nlohmann::ordered_json::parse(SubJSON));
            LogOut("BuildSubComponentsArray", "Added COMPONENT " + RecipeComponentID + " SUBCOMPONENT " + std::to_string(j + 1));
        }
    }
    LogOut("BuildSubComponentsArray", "Completed SubComponentsArray.");
    //std::cout << ContainerParams.SubComponentsArray.dump(4) << std::endl;
    return true;
}

//The platform the HOST runs as. Phase-1 stub — only Linux is supported, so this is constant.
//TO BE REPLACED by real OS/arch detection when VidyaGod runs on other hosts (e.g. Windows).

//(Library/repo path helpers + the catalog/sync/import/publish service moved to PackageCatalog — see
//packagecatalog.h. The launch-engine code below calls PackageCatalog::RepositoryDirs / GetPackageUserSettings
//unqualified via the using-directive above.)

//(Catalog/RegistryRunners + git plumbing + Sync/Upsert/Reconcile moved to PackageCatalog — see packagecatalog.h.)

//=====================================================================================================================
//                                          NATIVE NODE-GRAPH LAUNCH
//=====================================================================================================================

//Derives the session paths from already-set ContainerParams fields (PackageUID/Platform/ContentRoot/
//PrefixGenerate/RunnerShipsBuild/UnifiedRuntime/RunnerVariantID/RunnerPackagePath). Pure of MANIFESTJSON.
bool LaunchResolver::DerivePaths(struct ContainerParams &ContainerParams, const nlohmann::ordered_json &GlobalConfigJSON)
{
    if (ContainerParams.ScreenWidth.empty() || ContainerParams.ScreenHeight.empty())
    {
        if (qApp && QThread::currentThread() == qApp->thread())
            if (QScreen * Scr = QGuiApplication::primaryScreen())
            {
                ContainerParams.ScreenWidth  = std::to_string(Scr->geometry().width());
                ContainerParams.ScreenHeight = std::to_string(Scr->geometry().height());
            }
        if (ContainerParams.ScreenWidth.empty())  ContainerParams.ScreenWidth  = "0";
        if (ContainerParams.ScreenHeight.empty()) ContainerParams.ScreenHeight = "0";
    }
    std::filesystem::path TempRoot = std::filesystem::path(QDir::homePath().toStdString()) / ".VidyaGod" / "TEMP";
    if (GlobalConfigJSON.contains("Settings") && GlobalConfigJSON["Settings"].is_object())
    {
        const auto &S = GlobalConfigJSON["Settings"];
        if (S.contains("Paths") && S["Paths"].is_object()
            && S["Paths"].contains("TempRoot") && S["Paths"]["TempRoot"].is_string()
            && !std::string(S["Paths"]["TempRoot"]).empty())
            TempRoot = std::filesystem::path(std::string(S["Paths"]["TempRoot"]));
    }
    ContainerParams.TempPath = TempRoot / ContainerParams.PackageUID;
    if (ContainerParams.RunnerShipsBuild && !ContainerParams.UnifiedRuntime)
        ContainerParams.RunnerMountPath = ContainerParams.TempPath / "RUNNER";
    ContainerParams.RuntimePath     = ContainerParams.TempPath / "RUNTIME";
    ContainerParams.WriteLayerPath  = ContainerParams.TempPath / "WRITELAYER";
    ContainerParams.DefaultDataPath = ContainerParams.TempPath / "DEFAULTDATA";
    ContainerParams.UserDataPath    = ContainerParams.PackagePath / "USERDATA";
    VarSubst::StringVariableSubstitution(ContainerParams.ContentRoot, ContainerParams.GetVariablesMap());
    ContainerParams.ProgramPath = ContainerParams.ContentRoot.empty()
        ? ContainerParams.RuntimePath : ContainerParams.RuntimePath / ContainerParams.ContentRoot;
    {
        const std::string &CR = ContainerParams.ContentRoot;
        const auto Pos = CR.find("drive_c");
        ContainerParams.PrefixRoot = (Pos == std::string::npos) ? std::string()
            : (Pos == 0 ? std::string() : CR.substr(0, Pos - 1));
    }
    if (ContainerParams.PrefixGenerate)
        ContainerParams.DefPrefixPath = ContainerParams.RunnerShipsBuild
            ? std::filesystem::path(RunnerWrapper::DefPrefixArtifact(ContainerParams.RunnerPackagePath.string(), ContainerParams.RunnerVariantID))
            : ContainerParams.TempPath / "DEFPREFIX";
    ContainerParams.WorkDirPathComplete = ContainerParams.ProgramPath;
    LogOut("DerivePaths", "RuntimePath: " + ContainerParams.RuntimePath.string()
           + " | ContentRoot: " + ContainerParams.ContentRoot + " (PrefixRoot: '" + ContainerParams.PrefixRoot + "')");
    return true;
}

//Picks the best ROLE:"runner" node for a launch node (GUEST ∋ launch host, HOST==machine, executable available),
//priority: explicit RunnerID pin > USERSETTINGS PREFERRED_RUNNER > first qualifying (sorted node-id order).
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
    for (const auto &[Id, N] : Idx.Nodes) if (Qualifies(N)) return &N;
    return nullptr;
}

//Native node-graph init — populates ContainerParams + the internal component pool DIRECTLY from the node graph.
bool LaunchResolver::InitializeFromNode(struct ContainerParams &ContainerParams, nlohmann::ordered_json &ComponentPool, const nlohmann::ordered_json &GlobalConfigJSON)
{
    auto &CP = ContainerParams;
    const NodeIndex &Idx = *CP.NodeIdx;
    const std::string LaunchId = CP.LaunchNodeId;
    const Node *Launch = Idx.Find(LaunchId);
    if (!Launch) { LogErr("InitializeFromNode", "Launch node not found: " + LaunchId); return false; }
    if (!Launch->IsLaunchable()) LogWarn("InitializeFromNode", "Node '" + LaunchId + "' is not ROLE:launchable.");

    CP.subgame_id = LaunchId;  CP.VariantID = "default";
    CP.PackageUID = Launch->Uid.empty() ? LaunchId : Launch->Uid;
    CP.PackageName= Launch->Meta.is_object() ? Launch->Meta.value("TITLE", LaunchId) : LaunchId;
    CP.GameName   = CP.PackageName;
    CP.Platform   = Launch->HostPlatform;
    CP.PackagePath= Launch->BundleDir;
    CP.UMUID      = Launch->Meta.is_object() ? Launch->Meta.value("UMUID", std::string("0")) : "0";

    //Absolutize a node's LAYERS PATH/SOURCE.PATH against its OWN bundle dir (cross-bundle-correct).
    auto AbsLayers = [](const Node *N) -> nlohmann::ordered_json
    {
        nlohmann::ordered_json Out = nlohmann::ordered_json::array();
        if (!N->Layers.is_array()) return Out;
        for (nlohmann::ordered_json L : N->Layers)
        {
            if (L.contains("PATH") && L["PATH"].is_string())
            { std::filesystem::path P = std::string(L["PATH"]); if (!P.is_absolute()) L["PATH"] = (N->BundleDir / P).string(); }
            if (L.contains("SOURCE") && L["SOURCE"].is_object() && L["SOURCE"].contains("PATH") && L["SOURCE"]["PATH"].is_string())
            { std::filesystem::path P = std::string(L["SOURCE"]["PATH"]); if (!P.is_absolute()) L["SOURCE"]["PATH"] = (N->BundleDir / P).string(); }
            Out.push_back(std::move(L));
        }
        return Out;
    };

    nlohmann::ordered_json Components = nlohmann::ordered_json::array();
    CP.Recipe.clear();
    auto AddComponent = [&](const Node *N)
    { Components.push_back({{"COMPONENTID", N->NodeId}, {"SUBCOMPONENTS", AbsLayers(N)}}); CP.Recipe.push_back(N->NodeId); };

    //Pick the runner FIRST (paths + unified-fold depend on RunnerShipsBuild/UnifiedRuntime).
    const Node *RunnerNode = PickRunnerNode(Idx, *Launch, CP, GlobalConfigJSON);
    if (RunnerNode)
    {
        const auto &E = RunnerNode->Exec;
        CP.RunnerID          = RunnerNode->NodeId;
        CP.RunnerName        = RunnerNode->NodeId;
        CP.RunnerVariantID   = "default";
        CP.RunnerPackagePath = RunnerNode->BundleDir;
        CP.RunnerExecutable  = E.is_object() ? E.value("EXECUTABLE", std::string()) : std::string();
        CP.ContentRoot       = E.is_object() ? E.value("CONTENT_ROOT", std::string()) : std::string();
        CP.PrefixGenerate    = E.is_object() ? E.value("PREFIX_GENERATE", false) : false;
        if (E.is_object() && E.contains("ENV") && E["ENV"].is_object()) CP.RunnerEnv = E["ENV"];
        if (E.is_object() && E.contains("REMOVE_ENV") && E["REMOVE_ENV"].is_array()) for (auto &X : E["REMOVE_ENV"]) CP.RunnerRemoveEnv.push_back(std::string(X));
        if (E.is_object() && E.contains("ARGS") && E["ARGS"].is_array())             for (auto &X : E["ARGS"])       CP.RunnerArgs.push_back(std::string(X));
        CP.UnifiedRuntime    = E.is_object() ? E.value("UNIFIED_RUNTIME", false) : false;

        //Runner build = the runner node's content closure (its PARENTS).
        std::vector<std::string> RunnerOrder = ManifestModel::ResolveNodeOrder(Idx, RunnerNode->NodeId, CP.ModuleStates);
        nlohmann::ordered_json RunnerComps = nlohmann::ordered_json::array();
        std::vector<std::string> RunnerBuildIds;
        for (const std::string &Id : RunnerOrder)
        {
            if (Id == RunnerNode->NodeId) continue;
            const Node *N = Idx.Find(Id);
            if (!N || N->IsRunner()) continue;
            nlohmann::ordered_json Subs = AbsLayers(N);
            RunnerComps.push_back({{"COMPONENTID", N->NodeId}, {"SUBCOMPONENTS", Subs}});
            RunnerBuildIds.push_back(N->NodeId);
            for (auto &S : Subs) if (IsVfsLayer(S.value("TYPE", std::string()))) CP.RunnerLayers.push_back(S);
        }
        CP.RunnerComponents = RunnerComps;
        CP.RunnerRecipe     = RunnerBuildIds;
        CP.RunnerShipsBuild = !CP.RunnerLayers.empty();
        CP.RunnerEndpoints  = CP.UnifiedRuntime ? RunnerBuildIds : std::vector<std::string>{};
        //UNIFIED: fold the runner build into the game RUNTIME (mount first = lowest priority).
        if (CP.UnifiedRuntime)
            for (const std::string &Id : RunnerBuildIds) { const Node *N = Idx.Find(Id); if (N) AddComponent(N); }
    }
    else LogErr("InitializeFromNode", "No runner found for platform '" + CP.Platform + "'.");

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

    //The internal component pool the generic iterators (BuildSubComponentsArray/ResolveCustomVariables/
    //DerivePersistence/BuildDefaultData) consume — built from nodes, never authored or read from disk.
    ComponentPool = nlohmann::ordered_json::object();
    ComponentPool["PACKAGEUID"]  = CP.PackageUID;
    ComponentPool["PACKAGENAME"] = CP.PackageName;
    ComponentPool["COMPONENTS"]  = Components;

    DerivePaths(CP, GlobalConfigJSON);
    ResolveCustomVariables(ComponentPool, CP, GlobalConfigJSON);
    BuildSubComponentsArray(ComponentPool, CP);
    DerivePersistence(ComponentPool, CP);
    LogSucc("InitializeFromNode", "Resolved node '" + LaunchId + "' (runner " + CP.RunnerName + ", "
            + std::to_string(CP.Recipe.size()) + " component(s)).");
    return true;
}
