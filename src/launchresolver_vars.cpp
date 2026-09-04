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
//of a key wins (closure order: the active runner components first as least-specific defaults, then parents before
//children, the launchable last). Bare keys are one shared knob, so a game seeding a runner's knob (re-declaring
//its KEY) is the feature.
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

// P6 split: CustomVar resolution + subcomponent assembly (see launchresolver.cpp for the spine).
bool LaunchResolver::ResolveCustomVariables(const nlohmann::ordered_json &MANIFESTJSON, struct ContainerParams &ContainerParams, const nlohmann::ordered_json &GlobalConfigJSON)
{
    //ONE snapshot of the package's persisted VARIABLES for the whole resolve. The old code re-ran the
    //O(LIBRARY) settings scan (with a deep USERSETTINGS copy) for EVERY custom-var key.
    const nlohmann::ordered_json SavedVars = GetPackageVariables(GlobalConfigJSON, ContainerParams.PackageUID);

    //Helper: resolve a single bare KEY/DEFAULT pair through the priority chain.
    auto ResolveOne = [&](const std::string &Key, const std::string &DefaultValue) -> std::string
    {
        //Trace-gated: ResolveOne runs once per key per FIXPOINT ITERATION, so these lines repeat until the
        //var set converges (240 of one resolve's 2036 log lines, measured). The final resolved table below is
        //printed once per launch regardless — that is the line a human actually reads.
        if (ContainerParams.VariableOverrides.count(Key))
        {
            if (VerboseLogging()) LogOut("ResolveCustomVariables", "CLI override: " + Key + " = " + ContainerParams.VariableOverrides.at(Key));
            return ContainerParams.VariableOverrides.at(Key);
        }
        if (SavedVars.contains(Key))
        {
            std::string Val = SavedVars[Key];
            if (VerboseLogging()) LogOut("ResolveCustomVariables", "User setting: " + Key + " = " + Val);
            return Val;
        }
        if (VerboseLogging()) LogOut("ResolveCustomVariables", "Default: " + Key + " = " + DefaultValue);
        return DefaultValue;
    };

    //----- ABSOLUTE SCOPE: resolve every CustomVar into ONE global namespace where each key's hierarchy-final value
    //is visible to every reference (including inside other vars' DEFAULTs), independent of declaration order. Two
    //phases: (1) collect each key's WINNING raw source respecting hierarchy (later/most-specific declaration wins);
    //(2) fixpoint-substitute all sources against built-ins + every var until stable. No encoding here — that's a
    //use-site concern (%KEY:format%). -----
    LogOut("ResolveCustomVariables", "Resolving CustomVar subcomponents (absolute scope)...");

    //Phase 1 — the winning declaration per key (last in the combined closure order: the active runner components
    //FIRST as least-specific defaults, then the game Recipe — parents before children, the launchable last).
    //A reference cycle is harmless (Phase 2 leaves the residual %token%).
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
    //Runner knobs share the same global namespace (a runner exposes %KEY% in its ENV/ARGS; a game can seed them).
    //Scope to the active runner variant (RunnerRecipe) so an inactive multi-version component's knob can't win.
    //Collected FIRST: the runner's declaration is the least-specific default of the whole composition, so a
    //game-side declaration of the same key must overwrite it ("a game can seed them" — e.g. a runner declares
    //PROTON_DXVK_D3D8=0 as the safe global default and a D3D8 game's package flips it to 1). Collecting the
    //runner last silently made runner defaults unoverridable by packages.
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
    for (const std::string &CompID : ContainerParams.Recipe)
    {
        int Idx = FindComponentIndex(MANIFESTJSON, CompID);
        if (Idx == -1) continue;
        const auto &Comp = MANIFESTJSON["COMPONENTS"][Idx];
        if (Comp.contains("SUBCOMPONENTS") && Comp["SUBCOMPONENTS"].is_array())
            for (const auto &S : Comp["SUBCOMPONENTS"]) Collect(S);
    }

    //Each key's RAW source value: priority CLI > USERSETTINGS > winning DEFAULT; a secret+POOL var falls back to a
    //one-time pool seed only when nothing is persisted. Sources are unsubstituted; cross-references resolve in Phase 2.
    std::map<std::string, std::string> Sources;
    std::set<std::string> Secret;                                  // keys whose value must not be logged
    std::map<std::string, std::string> WhenCond;                   // key -> its WHEN condition (gates value to "")
    for (const std::string &Key : KeyOrder)
    {
        const nlohmann::ordered_json &CV = Winning[Key];
        const nlohmann::ordered_json UI = (CV.contains("UI") && CV["UI"].is_object()) ? CV["UI"] : nlohmann::ordered_json::object();
        //A WHEN condition (top-level; UI.WHEN accepted as a UI-era alias) gates the VALUE: when it does not hold, the
        //var resolves to "" so its %token% references vanish (and a single-token EXEARGS arg drops). The condition may
        //reference other vars, so it is evaluated inside the Phase-2 fixpoint against the settling map.
        std::string When = CV.value("WHEN", std::string());
        if (When.empty() && UI.contains("WHEN") && UI["WHEN"].is_string()) When = UI["WHEN"];
        if (!When.empty()) WhenCond[Key] = When;
        const bool Pooled = UI.value("CONTROL", std::string()) == "secret"
                            && UI.contains("POOL") && UI["POOL"].is_array() && !UI["POOL"].empty();
        if (Pooled) Secret.insert(Key);
        if (Pooled && !ContainerParams.VariableOverrides.count(Key))
        {
            //A POOL is a one-time SEED, not a per-launch rotation: once a value is persisted (seeded here on the
            //first launch, or typed by the user in the prelaunch window) it must stick, or a game that binds the
            //value to durable state — a CD key written into its prefix, an account name — would see it change
            //under it every launch. So: persisted value wins; only when there is none do we draw from the pool,
            //and we hand the draw back for the caller to persist.
            std::string Saved;
            if (SavedVars.contains(Key) && SavedVars[Key].is_string())
                Saved = SavedVars[Key];
            if (!Saved.empty())
            {
                LogOut("ResolveCustomVariables", "User setting: " + Key + " = [secret]");
                Sources[Key] = Saved;
            }
            else
            {
                const auto &Pool = UI["POOL"];
                //Per-call generator: the old function-local static was shared across launch threads with no
                //lock (a data race, however benign). A pool draw is once-per-secret-per-first-launch — cold.
                std::mt19937 Rng(std::random_device{}());
                std::uniform_int_distribution<size_t> Dist(0, Pool.size() - 1);
                const size_t Pick = Dist(Rng);
                Sources[Key] = Pool[Pick].is_string() ? Pool[Pick].get<std::string>() : std::string();
                ContainerParams.PickedSecrets[Key] = Sources[Key];
                LogOut("ResolveCustomVariables", "Pool seed (first launch): " + Key + " = [secret]");
            }
        }
        else Sources[Key] = ResolveOne(Key, CV.value("DEFAULT", std::string()));
    }

    //Engine-injected session facts (friend LAN vIPs + SELF_NAME) enter here, as if they were declared with
    //that value as their DEFAULT — so ResolveOne gives them the documented precedence for free (a --var/picker
    //override or a persisted USERSETTING still wins). A node that DECLARES one of these keys keeps full control:
    //the guard below leaves its own entry alone.
    //
    //They must be seeded at THIS point, not merged into CustomVariables later: only keys in KeyOrder reach the
    //Phase-2 fixpoint, and the fixpoint is what makes a %token% usable in EXEARGS and in another CustomVar's
    //DEFAULT. Injecting after the resolve (as the LAN merge in ContainerWrapper does) is too late for both — the
    //token would survive into argv as a literal.
    for (const auto &[Key, Value] : ContainerParams.SessionVars)
    {
        if (Sources.count(Key)) continue;                          // a package declaration wins
        KeyOrder.push_back(Key);
        Sources[Key] = ResolveOne(Key, Value);
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
            std::string Val;
            const auto Wit = WhenCond.find(Key);
            if (Wit != WhenCond.end() && !VarSubst::EvaluateCondition(Wit->second, Map))
                Val.clear();                                       // WHEN false → gated off (empty)
            else { Val = Sources[Key]; VarSubst::StringVariableSubstitution(Val, Map); }
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

bool LaunchResolver::BuildSubComponentsArray(const nlohmann::ordered_json &MANIFESTJSON, struct ContainerParams &ContainerParams)
{
    //Vars are fully resolved before this step (ResolveCustomVariables ran) — ONE map for the whole pass.
    //The old code rebuilt the entire variables map for EVERY subcomponent.
    const std::map<std::string, std::string> FrozenVars = ContainerParams.GetVariablesMap();
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
                //A WHEN condition gates whether the layer is applied at all: false → the layer is inert (not mounted,
                //not edited). Evaluated against the resolved var map — the data-driven "this only applies when that".
                if (Subs[j].contains("WHEN") && Subs[j]["WHEN"].is_string()
                    && !VarSubst::EvaluateCondition(Subs[j]["WHEN"], FrozenVars))
                {
                    if (VerboseLogging())
                        LogOut("BuildSubComponentsArray", "Skipped " + T + " (WHEN false: " + std::string(Subs[j]["WHEN"]) + ")");
                    continue;
                }
            }
            //Serialize to string, substitute %VAR% tokens, then re-parse.
            std::string SubJSON = Subs[j].dump();
            VarSubst::StringVariableSubstitution(SubJSON, FrozenVars);
            ContainerParams.SubComponentsArray.push_back(nlohmann::ordered_json::parse(SubJSON));
            if (VerboseLogging())   // per-subcomponent trace — 81 lines per resolve, gated like the rest
                LogOut("BuildSubComponentsArray", "Added COMPONENT " + RecipeComponentID + " SUBCOMPONENT " + std::to_string(j + 1));
        }
    }
    LogOut("BuildSubComponentsArray", "Completed SubComponentsArray.");
    //std::cout << ContainerParams.SubComponentsArray.dump(4) << std::endl;
    return true;
}

//The platform the HOST runs as. Phase-1 stub — only Linux is supported, so this is constant.
//TO BE REPLACED by real OS/arch detection when VidyaGod runs on other hosts (e.g. Windows).

//(Library path helpers + the catalog/sync/import/publish service moved to PackageCatalog — see packagecatalog.h.
//The launch-engine code below calls PackageCatalog::BuildCatalogIndex / GetPackageUserSettings unqualified via the
//using-directive above.)

//(Catalog/RegistryRunners + git plumbing + Sync/Upsert/Reconcile moved to PackageCatalog — see packagecatalog.h.)

//=====================================================================================================================
//                                          NATIVE NODE-GRAPH LAUNCH
//=====================================================================================================================

//Derives the session paths from already-set ContainerParams fields (PackageUID/Platform/ContentRoot/
//PrefixGenerate/RunnerShipsBuild/UnifiedRuntime/RunnerPackagePath). Pure of MANIFESTJSON.
