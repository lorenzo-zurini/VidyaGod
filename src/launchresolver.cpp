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
//  3. the winning DEFAULT (or, for a secret+POOL var, one pool entry picked per launch)
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

    //----- ABSOLUTE SCOPE: resolve every CustomVar into ONE global namespace where each key's hierarchy-final value
    //is visible to every reference (including inside other vars' DEFAULTs), independent of declaration order. Two
    //phases: (1) collect each key's WINNING raw source respecting hierarchy (later/most-specific declaration wins);
    //(2) fixpoint-substitute all sources against built-ins + every var until stable. No encoding here — that's a
    //use-site concern (%KEY:format%). -----
    LogOut("ResolveCustomVariables", "Resolving CustomVar subcomponents (absolute scope)...");

    //Phase 1 — the winning declaration per key (last in the combined closure order: the game Recipe, then the active
    //runner components). A reference cycle is harmless (Phase 2 leaves the residual %token%).
    std::vector<std::string> KeyOrder;                              // first-seen order, for deterministic logging
    std::map<std::string, nlohmann::ordered_json> Winning;          // key -> its winning CustomVar declaration
    auto Collect = [&](const nlohmann::ordered_json &S)
    {
        if (!S.is_object() || S.value("TYPE", std::string()) != "CustomVar") return;
        const std::string Key = S.value("KEY", std::string());
        if (Key.empty()) return;
        if (!Winning.count(Key)) KeyOrder.push_back(Key);
        Winning[Key] = S;                                          // later declaration overwrites (hierarchy)
    };
    for (const std::string &CompID : ContainerParams.Recipe)
    {
        int Idx = FindComponentIndex(MANIFESTJSON, CompID);
        if (Idx == -1) continue;
        const auto &Comp = MANIFESTJSON["COMPONENTS"][Idx];
        if (Comp.contains("SUBCOMPONENTS") && Comp["SUBCOMPONENTS"].is_array())
            for (const auto &S : Comp["SUBCOMPONENTS"]) Collect(S);
    }
    //Runner knobs share the same global namespace (a runner exposes %KEY% in its ENV/ARGS; a game can seed them).
    //Scope to the active runner variant (RunnerRecipe) so an inactive multi-version component's knob can't win.
    if (!ContainerParams.RunnerComponents.empty())
    {
        std::set<std::string> RunnerWant(ContainerParams.RunnerRecipe.begin(), ContainerParams.RunnerRecipe.end());
        for (const auto &Comp : ContainerParams.RunnerComponents)
        {
            if (!Comp.is_object() || !Comp.contains("SUBCOMPONENTS") || !Comp["SUBCOMPONENTS"].is_array()) continue;
            if (!RunnerWant.empty() && !RunnerWant.count(Comp.value("COMPONENTID", std::string()))) continue;
            for (const auto &S : Comp["SUBCOMPONENTS"]) Collect(S);
        }
    }

    //Each key's RAW source value: priority CLI > USERSETTINGS > winning DEFAULT; a secret+POOL var picks one pool
    //entry ONCE per launch (CLI override still wins). Sources are unsubstituted; cross-references resolve in Phase 2.
    std::map<std::string, std::string> Sources;
    std::set<std::string> Secret;                                  // keys whose value must not be logged
    for (const std::string &Key : KeyOrder)
    {
        const nlohmann::ordered_json &CV = Winning[Key];
        const nlohmann::ordered_json UI = (CV.contains("UI") && CV["UI"].is_object()) ? CV["UI"] : nlohmann::ordered_json::object();
        const bool Pooled = UI.value("CONTROL", std::string()) == "secret"
                            && UI.contains("POOL") && UI["POOL"].is_array() && !UI["POOL"].empty();
        if (Pooled) Secret.insert(Key);
        if (Pooled && !ContainerParams.VariableOverrides.count(Key))
        {
            const auto &Pool = UI["POOL"];
            static std::mt19937 Rng(std::random_device{}());
            std::uniform_int_distribution<size_t> Dist(0, Pool.size() - 1);
            const size_t Pick = Dist(Rng);
            Sources[Key] = Pool[Pick].is_string() ? Pool[Pick].get<std::string>() : std::string();
        }
        else Sources[Key] = ResolveOne(Key, CV.value("DEFAULT", std::string()));
    }

    //Phase 2 — fixpoint. Seed raw, then substitute every source against (built-ins + all vars) until a full pass
    //changes nothing. A snapshot per pass (Jacobi iteration) makes resolution order-independent; the cap bounds
    //reference cycles (which terminate with their %token% left literal).
    for (const std::string &Key : KeyOrder) ContainerParams.CustomVariables[Key] = Sources[Key];
    constexpr int MaxPasses = 16;
    for (int Pass = 0; Pass < MaxPasses; ++Pass)
    {
        bool Changed = false;
        const std::map<std::string, std::string> Map = ContainerParams.GetVariablesMap();
        for (const std::string &Key : KeyOrder)
        {
            std::string Val = Sources[Key];
            VarSubst::StringVariableSubstitution(Val, Map);
            if (Val != ContainerParams.CustomVariables[Key]) { ContainerParams.CustomVariables[Key] = Val; Changed = true; }
        }
        if (!Changed) break;
    }
    for (const std::string &Key : KeyOrder)
        LogOut("ResolveCustomVariables", "  " + Key + " = " + (Secret.count(Key) ? std::string("[secret] (set)") : ContainerParams.CustomVariables[Key]));
    LogSucc("ResolveCustomVariables", "Resolved " + std::to_string(ContainerParams.CustomVariables.size()) + " custom variable(s).");
    return true;
}

//Derives the persistence policy from the unified Persist primitive — ONE LAYERS type, purely ADDITIVE (no mode flag):
//
//  { "TYPE":"Persist", "KEEP":"<target>" }   persist this target (durable)
//  { "TYPE":"Persist", "DROP":"<path>"   }   make this path ephemeral (writes discarded)
//
//A KEEP target is SELF-DESCRIBING (the dir/file/registry kind is derived, not a separate type):
//  - "%RuntimePath%" (the mount root)→ persist the WHOLE runtime (the durable UserDataPath becomes the writable
//                                     branch) — the elegant replacement for the old MODE:all
//  - "registry" (sentinel)          → all prefix hives (user/system/userdef.reg)
//  - a registry root ("HKCU", ...)  → that whole hive (its .reg file)            [was RegPersist, scoped]
//  - a deeper registry path         → that key's subtree only                   [was RegKeyPersist]
//  - a runtime-root-relative path   → a directory (live RW passthrough) [was PersistDir] or a single file
//                                     (copy seed/capture) [was PersistFile], by shape
//
//DEFAULT (no Persist anywhere): a PRISTINE runtime each launch; only KEEP targets persist. The active runner
//contributes a platform keep-set (RunnerPersistLayers: e.g. the user profile + HKCU) so the standard save/config
//locations survive with no per-game work. Persistence is purely additive: KEEP adds, DROP removes — there is no
//mode to override, and "keep everything" is just `KEEP %RuntimePath%`. Targets are %VARIABLE%-substituted.
bool LaunchResolver::DerivePersistence(const nlohmann::ordered_json &MANIFESTJSON, struct ContainerParams &ContainerParams)
{
    ContainerParams.PersistAll = false;        // set true only by a KEEP of the runtime root (whole-runtime persist)
    ContainerParams.KeepDirs.clear();
    ContainerParams.KeepFiles.clear();
    ContainerParams.KeepRegKeys.clear();
    ContainerParams.KeepRegHives.clear();
    ContainerParams.DropPaths.clear();

    const std::map<std::string, std::string> Vars = ContainerParams.GetVariablesMap();

    auto ToUpper = [](std::string S){ for (char &C : S) C = (char)std::toupper((unsigned char)C); return S; };
    auto ToLower = [](std::string S){ for (char &C : S) C = (char)std::tolower((unsigned char)C); return S; };
    auto AddUnique = [](std::vector<std::string> &V, const std::string &S){ if (std::find(V.begin(), V.end(), S) == V.end()) V.push_back(S); };

    //A registry hive root → its Wine .reg file (HKCU→user, HKLM/HKCR/HKCC→system, HKU→userdef). "" if not a root.
    auto HiveRootFile = [&](const std::string &Root) -> std::string {
        const std::string R = ToUpper(Root);
        if (R == "HKCU" || R == "HKEY_CURRENT_USER")   return "user.reg";
        if (R == "HKLM" || R == "HKEY_LOCAL_MACHINE")  return "system.reg";
        if (R == "HKCR" || R == "HKEY_CLASSES_ROOT")   return "system.reg";
        if (R == "HKCC" || R == "HKEY_CURRENT_CONFIG") return "system.reg";
        if (R == "HKU"  || R == "HKEY_USERS")          return "userdef.reg";
        return "";
    };
    //A runtime-root-relative path: directory (→ live RW passthrough) or single file (→ copy). Trailing slash or an
    //existing dir ⇒ directory; an existing file ⇒ file; otherwise a dotted leaf (an extension) ⇒ file, else directory.
    auto IsDirTarget = [&](const std::string &T) -> bool {
        if (!T.empty() && (T.back() == '/' || T.back() == '\\')) return true;
        std::error_code Ec;
        const std::filesystem::path Abs = ContainerParams.UserDataPath / T;
        if (std::filesystem::is_directory(Abs, Ec))    return true;
        if (std::filesystem::is_regular_file(Abs, Ec)) return false;
        const auto Slash = T.find_last_of("/\\");
        const std::string Leaf = (Slash == std::string::npos) ? T : T.substr(Slash + 1);
        return Leaf.find('.') == std::string::npos;   // no extension ⇒ directory
    };
    auto NormalizeRel = [](std::string T){
        while (!T.empty() && (T.back() == '/' || T.back() == '\\')) T.pop_back();
        while (!T.empty() && (T.front() == '/' || T.front() == '\\')) T.erase(T.begin());
        return T;
    };
    auto StripTrail = [](std::string S){ while (S.size() > 1 && (S.back() == '/' || S.back() == '\\')) S.pop_back(); return S; };
    //A KEEP target that names the runtime mount root itself (`%RuntimePath%`, or the bare root "."/"/") ⇒ persist the
    //WHOLE runtime: the durable UserDataPath becomes the writable branch. The elegant replacement for the old MODE:all.
    auto IsRuntimeRoot = [&](const std::string &T) -> bool {
        if (T == "." || T == "/" || T == "./") return true;
        if (ContainerParams.RuntimePath.empty()) return false;
        return StripTrail(T) == StripTrail(ContainerParams.RuntimePath.string());
    };

    auto ClassifyKeep = [&](std::string T){
        VarSubst::StringVariableSubstitution(T, Vars);
        if (T.empty()) { LogWarn("DerivePersistence", "  KEEP with empty target (skipped)."); return; }
        if (T.rfind("host:", 0) == 0) { LogWarn("DerivePersistence", "  KEEP host: target not yet supported (reserved for native containment): " + T); return; }
        if (IsRuntimeRoot(T)) { ContainerParams.PersistAll = true; LogOut("DerivePersistence", "  KEEP %RuntimePath% (whole runtime durable)"); return; }
        if (ToLower(T) == "registry")
        { AddUnique(ContainerParams.KeepRegHives, "user.reg"); AddUnique(ContainerParams.KeepRegHives, "system.reg"); AddUnique(ContainerParams.KeepRegHives, "userdef.reg"); LogOut("DerivePersistence", "  KEEP registry (all hives)"); return; }
        const auto Sep = T.find_first_of("\\/");
        const std::string Root = (Sep == std::string::npos) ? T : T.substr(0, Sep);
        if (const std::string Hive = HiveRootFile(Root); !Hive.empty())
        {
            if (Sep == std::string::npos) { AddUnique(ContainerParams.KeepRegHives, Hive); LogOut("DerivePersistence", "  KEEP hive " + Root + " (" + Hive + ")"); }
            else                          { AddUnique(ContainerParams.KeepRegKeys, T);     LogOut("DerivePersistence", "  KEEP regkey " + T); }
            return;
        }
        if (IsDirTarget(T)) { AddUnique(ContainerParams.KeepDirs,  NormalizeRel(T)); LogOut("DerivePersistence", "  KEEP dir  " + NormalizeRel(T)); }
        else                { AddUnique(ContainerParams.KeepFiles, NormalizeRel(T)); LogOut("DerivePersistence", "  KEEP file " + NormalizeRel(T)); }
    };
    auto ClassifyDrop = [&](std::string T){
        VarSubst::StringVariableSubstitution(T, Vars);
        if (T.empty()) { LogWarn("DerivePersistence", "  DROP with empty target (skipped)."); return; }
        if (ToLower(T) == "registry" || !HiveRootFile((T.find_first_of("\\/") == std::string::npos) ? T : T.substr(0, T.find_first_of("\\/"))).empty())
        { LogWarn("DerivePersistence", "  DROP of a registry target is not supported (paths only): " + T); return; }
        AddUnique(ContainerParams.DropPaths, NormalizeRel(T)); LogOut("DerivePersistence", "  DROP path " + NormalizeRel(T));
    };

    auto Scan = [&](const nlohmann::ordered_json &S){
        if (!S.is_object() || S.value("TYPE", std::string()) != "Persist") return;
        if (S.contains("KEEP") && S["KEEP"].is_string()) ClassifyKeep(S["KEEP"]);
        if (S.contains("DROP") && S["DROP"].is_string()) ClassifyDrop(S["DROP"]);
    };

    LogOut("DerivePersistence", "Resolving Persist policy (runner keep-set + Recipe)...");
    //Persistence is purely additive (KEEP adds, DROP removes), so scan order is immaterial — the runner's platform
    //keep-set and the game's Persist layers simply union.
    if (ContainerParams.RunnerPersistLayers.is_array())
        for (const auto &S : ContainerParams.RunnerPersistLayers) Scan(S);
    for (const std::string &CompID : ContainerParams.Recipe)
    {
        int Idx = FindComponentIndex(MANIFESTJSON, CompID);
        if (Idx == -1) continue;
        const auto &Comp = MANIFESTJSON["COMPONENTS"][Idx];
        if (!Comp.contains("SUBCOMPONENTS") || !Comp["SUBCOMPONENTS"].is_array()) continue;
        for (const auto &S : Comp["SUBCOMPONENTS"]) Scan(S);
    }

    LogSucc("DerivePersistence",
            "PERSIST: ALL=" + std::string(ContainerParams.PersistAll ? "true" : "false") +
            " KEEP[dirs=" + std::to_string(ContainerParams.KeepDirs.size()) +
            " files=" + std::to_string(ContainerParams.KeepFiles.size()) +
            " hives=" + std::to_string(ContainerParams.KeepRegHives.size()) +
            " regkeys=" + std::to_string(ContainerParams.KeepRegKeys.size()) + "]" +
            " DROP=" + std::to_string(ContainerParams.DropPaths.size()));
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
            //CustomVar and Persist subcomponents are not VFS/registry ops applied here — CustomVar is resolved by
            //ResolveCustomVariables; the unified Persist layer is consumed by DerivePersistence and the seed/capture/
            //passthrough steps. Skip them.
            if (Subs[j].is_object())
            {
                std::string T = Subs[j].value("TYPE", std::string());
                if (T == "CustomVar" || T == "Persist"
                    || T == "DeclareExec" || T == "DeclareLibraryItem" || T == "DeclareRunner") continue;
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
//PrefixGenerate/RunnerShipsBuild/UnifiedRuntime/RunnerPackagePath). Pure of MANIFESTJSON.
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
    std::filesystem::path TempRoot = AppPaths::DataRoot() / "TEMP";
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
    //CLI / in-package overrides (--runtime-dir / --userdata-dir): point these exact paths wherever asked (in-package
    //sets both = the package dir, making the package a self-contained, portable runnable unit).
    if (!AppPaths::RuntimePathOverride().empty())  ContainerParams.RuntimePath  = AppPaths::RuntimePathOverride();
    if (!AppPaths::UserDataPathOverride().empty()) ContainerParams.UserDataPath = AppPaths::UserDataPathOverride();
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
            ? std::filesystem::path(RunnerWrapper::DefPrefixDir(ContainerParams.RunnerPackagePath.string()))
            : ContainerParams.TempPath / "DEFPREFIX";
    ContainerParams.WorkDirPathComplete = ContainerParams.ProgramPath;
    LogOut("DerivePaths", "RuntimePath: " + ContainerParams.RuntimePath.string()
           + " | ContentRoot: " + ContainerParams.ContentRoot + " (PrefixRoot: '" + ContainerParams.PrefixRoot + "')");
    return true;
}

//Picks the best ROLE:"runner" node for a launch node (GUEST ∋ launch host, HOST==machine, executable available).
//Priority: explicit RunnerID pin > USERSETTINGS PREFERRED_RUNNER > best qualifying default, where "best" ranks
//RECOMMENDED runners first, then a runner shipped in the launch node's OWN package (embedded > global — a package
//that carries a runner uses its own copy; runners are global wherever they live, there is no embedded flag), then
//the lowest node-id for determinism. This keeps the default from being an arbitrary alphabetical accident.
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
    for (const std::string &Id : ManifestModel::ResolveNodeOrder(Idx, R.NodeId, {}))
    {
        if (Id == R.NodeId) continue;
        const Node *N = Idx.Find(Id);
        if (!N || N->IsRunner() || !N->Layers.is_array()) continue;
        for (const auto &L : N->Layers) if (IsVfsLayer(LayerType(L))) return true;
    }
    return false;
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
    for (const std::string &Cid : ManifestModel::ResolveNodeOrder(Idx, R->NodeId, Toggles))
    {
        if (Cid == R->NodeId) continue;
        const Node *N = Idx.Find(Cid);
        if (!N || N->IsRunner() || !N->Layers.is_array()) continue;
        for (nlohmann::ordered_json Lay : N->Layers)
        {
            if (!IsVfsLayer(LayerType(Lay))) continue;
            if (Lay.contains("PATH") && Lay["PATH"].is_string())
            { std::filesystem::path P = std::string(Lay["PATH"]); if (!P.is_absolute()) Lay["PATH"] = (N->BundleDir / P).string(); }
            if (Lay.contains("SOURCE") && Lay["SOURCE"].is_object() && Lay["SOURCE"].contains("PATH") && Lay["SOURCE"]["PATH"].is_string())
            { std::filesystem::path P = std::string(Lay["SOURCE"]["PATH"]); if (!P.is_absolute()) Lay["SOURCE"]["PATH"] = (N->BundleDir / P).string(); }
            L.Layers.push_back(std::move(Lay));
        }
    }
    L.ShipsBuild = !L.Layers.empty();
    return L;
}

} // namespace

std::vector<std::string> LaunchResolver::ResolveChainIds(const NodeIndex &Idx, const Node &Launch,
                                                         const struct ContainerParams &CP, const nlohmann::ordered_json &GlobalConfigJSON)
{
    const std::string Machine = MachinePlatform();

    //Available runners, sorted better-first (RECOMMENDED > package-local > node-id) for deterministic BFS.
    std::vector<const Node *> Runners;
    for (const auto &[Id, N] : Idx.Nodes)
        if (N.IsRunner() && RunnerAvailable(Idx, N)) Runners.push_back(&N);
    std::sort(Runners.begin(), Runners.end(), [&](const Node *A, const Node *B){ return RunnerBetterPtr(A, B, Launch); });

    //1. Honor a pinned chain (passed CP.RunnerChainIds, else persisted RUNNER_CHAIN) when it forms a valid path to
    //   the machine. Append a terminal if the pin didn't include one.
    std::vector<std::string> Pinned = CP.RunnerChainIds;
    if (Pinned.empty())
    {
        auto US = GetPackageUserSettings(GlobalConfigJSON, CP.PackageUID);
        if (US.contains("RUNNER_CHAIN") && US["RUNNER_CHAIN"].is_array())
            for (const auto &X : US["RUNNER_CHAIN"]) if (X.is_string()) Pinned.push_back(std::string(X));
    }
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
    std::vector<const Node *> Runners;
    for (const auto &[Id, N] : Idx.Nodes)
        if (N.IsRunner() && RunnerAvailable(Idx, N)) Runners.push_back(&N);
    std::sort(Runners.begin(), Runners.end(), [&](const Node *A, const Node *B){ return RunnerBetterPtr(A, B, Launch); });

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
    for (int i = 0; i < B; ++i)
    {
        const RunnerLink &L = CP.RunnerChain[i];
        //This inner runner's own executable, as a CONTENT_ROOT-relative path then a guest path. EXECUTABLE is a
        //build-relative path for a nestable inner runner (no %RunnerMount% — that mount doesn't exist inside the guest).
        std::string ExeRel = L.Executable;
        { std::map<std::string, std::string> V = CP.GetVariablesMap(); VarSubst::StringVariableSubstitution(ExeRel, V); }
        while (!ExeRel.empty() && (ExeRel.front() == '/' )) ExeRel.erase(ExeRel.begin());
        const std::string ExeRelUnderRoot = InnerRunnerMountRel(L.NodeId) + (ExeRel.empty() ? "" : "/" + ExeRel);
        const std::string ExeGuest = GuestPath(Template, ExeRelUnderRoot, CP);

        //This runner's args: %Content%/%ContentPath% = what it runs (the previous inner command, or the content for
        //the innermost), expressed as a guest path. Other %tokens% expand normally.
        const std::string Target = (i == 0) ? ContentGuest : PrevGuestCmd;
        std::vector<std::string> ThisArgs;
        for (const std::string &Raw : L.Args)
        {
            std::map<std::string, std::string> V = CP.GetVariablesMap();
            V["Content"] = Target; V["ContentPath"] = Target;
            std::string A = Raw; VarSubst::StringVariableSubstitution(A, V);
            ThisArgs.push_back(A);
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
    CP.PackagePath= AppPaths::PackagePathOverride().empty() ? Launch->BundleDir : AppPaths::PackagePathOverride();  // --package-dir / in-package
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
        for (const std::string &Id : ManifestModel::ResolveNodeOrder(Idx, RunnerNode->NodeId, CP.ModuleStates))
        {
            if (Id == RunnerNode->NodeId) continue;
            const Node *N = Idx.Find(Id);
            if (!N || N->IsRunner()) continue;
            RunnerComps.push_back({{"COMPONENTID", N->NodeId}, {"SUBCOMPONENTS", AbsLayers(N)}});
            RunnerBuildIds.push_back(N->NodeId);
        }
        CP.RunnerComponents = RunnerComps;
        CP.RunnerRecipe     = RunnerBuildIds;
        CP.RunnerEndpoints  = CP.UnifiedRuntime ? RunnerBuildIds : std::vector<std::string>{};
        //UNIFIED: fold the runner build into the game RUNTIME (mount first = lowest priority).
        if (CP.UnifiedRuntime)
            for (const std::string &Id : RunnerBuildIds) { const Node *N = Idx.Find(Id); if (N) AddComponent(N); }
    }

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
