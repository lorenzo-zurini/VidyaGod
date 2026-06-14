#include "containerwrapper.h"
#include "commonutils.h"
#include "jsonoperations.h"
#include "processenv.h"
#include "registrywrapper.h"
#include "ipfswrapper.h"
#include "runnerwrapper.h"
#include <random>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <set>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <QThread>
#include <QMetaObject>

//Resolves a layer/runner SOURCE locator to an absolute host path (defined below; forward-declared so
//DeriveContainerParams can resolve the selected runner's SOURCE).
static std::string ResolveLayerSource(const nlohmann::ordered_json &Sub, const std::filesystem::path &PackagePath);
//True if any subcomponent is a matching-scope RegEdit (defined below; forward-declared for the post-mount
//installed-runner base-reg-edit pass in BuildContainerRuntime).
static bool HasRegEdits(const struct ContainerParams &ContainerParams, bool WantOverride);

//The host platform this build runs on — a runner variant is only eligible if its HOST_PLATFORM matches.
static std::string MachinePlatform() { return "linux64"; }

//Stores the JSON references and immediately runs the full initialization pipeline.
//After construction, ContainerParams is fully populated and ready for BuildContainerRuntime().
ContainerWrapper::ContainerWrapper(nlohmann::ordered_json &Passed_GlobalConfigJSON, nlohmann::ordered_json &Passed_MANIFESTJSON, struct ContainerParams &Passed_ContainerParams)
    : GlobalConfigJSON(Passed_GlobalConfigJSON), MANIFESTJSON(Passed_MANIFESTJSON), ContainerParams(Passed_ContainerParams)
{
    this->InitializeContainer();
}

//Minimal constructor — stores only the three PASSED values; everything else is derived later
//by DeriveContainerParams once the MANIFEST and GlobalConfig are available.
ContainerParams::ContainerParams(std::filesystem::path Passed_PackagePath, std::string Passed_subgame_id, std::string Passed_component_id)
    : PackagePath(Passed_PackagePath), subgame_id(Passed_subgame_id), component_id(Passed_component_id)
{
    LogOut("ContainerParams::ContainerParams", "ContainerParams object created...");
}

//Linear scan for a subgame by its SUBGAMEID string.
//Returns the array index into MANIFEST["GAMES"], or -1 if not found.
int ContainerWrapper::FindGameIndex(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID)
{
    for (int i = 0; i < (int)MANIFESTJSON["GAMES"].size(); i++)
    {
        if (!MANIFESTJSON["GAMES"][i]["GAMEID"].is_null() && MANIFESTJSON["GAMES"][i]["GAMEID"] == SubgameID)
        {
            return i;
        }
    }
    LogErr("ContainerWrapper::FindGameIndex", "Subgame ID not found: " + SubgameID);
    return -1;
}

//Linear scan for a component by its COMPONENTID string.
//Returns the array index into MANIFEST["COMPONENTS"], or -1 if not found or if ComponentID is empty.
int ContainerWrapper::FindComponentIndex(const nlohmann::ordered_json &MANIFESTJSON, const std::string &ComponentID)
{
    if (ComponentID.empty()) return -1;
    for (int i = 0; i < (int)MANIFESTJSON["COMPONENTS"].size(); i++)
    {
        if (!MANIFESTJSON["COMPONENTS"][i]["COMPONENTID"].is_null() && MANIFESTJSON["COMPONENTS"][i]["COMPONENTID"] == ComponentID)
        {
            return i;
        }
    }
    LogErr("ContainerWrapper::FindComponentIndex", "Component ID not found: " + ComponentID);
    return -1;
}

//NOTE: the registry is now its own class (RegistryWrapper) and the filesystem is its own binary
//(vidyagodfs, the VidyaGodFS submodule). The remaining refactor would be to lift the VFS orchestration
//(BuildLayerSpec / MountVFS / CleanStaleRuntime / Cleanup) out of ContainerWrapper into a dedicated
//class, but it's not blocking — left as a future cleanup.

//Runs the initialization steps in the required order:
//  DecideComponent → DeriveContainerParams → CreateRecipe → ResolveCustomVariables → BuildSubComponentsArray
//ResolveCustomVariables must precede BuildSubComponentsArray so that custom %KEY% tokens
//are already in GetVariablesMap() when subcomponent strings are substituted.
bool ContainerWrapper::InitializeContainer()
{
    LogOut("ContainerWrapper::InitializeContainer", "Initializing container...");
    if(!this->DecideComponent(this->MANIFESTJSON, this->ContainerParams))
    {
        LogErr("ContainerWrapper::InitializeContainer", "ContainerWrapper::DecideComponent failed, aborting....");
        return false;
    }
    LogSucc("ContainerWrapper::InitializeContainer", "ContainerWrapper::DecideComponent successful.");

    if(!this->DeriveContainerParams(this->MANIFESTJSON, this->ContainerParams, this->GlobalConfigJSON))
    {
        LogErr("ContainerWrapper::InitializeContainer", "ContainerWrapper::DeriveContainerParams failed, aborting....");
        return false;
    }
    LogSucc("ContainerWrapper::InitializeContainer", "ContainerWrapper::DeriveContainerParams successful.");

    //Fold the selected runner's own components (from its registry package, if any) into the component
    //pool so its ENDPOINTS resolve and its CustomVar subcomponents are visible to the steps below.
    //(Empty for an embedded/PATH runner whose components already live in MANIFESTJSON.)
    //EXCEPTION: a separate-mount installed runner (RunnerShipsBuild && !UnifiedRuntime) keeps its build out
    //of the game pool — its layers mount at their own RunnerMountPath instead (see MountRunnerBuild).
    if (this->ContainerParams.RunnerShipsBuild && !this->ContainerParams.UnifiedRuntime)
        LogOut("ContainerWrapper::InitializeContainer", "Runner ships a build — mounted separately, not folded into the game pool.");
    else if (this->ContainerParams.RunnerComponents.is_array() && !this->ContainerParams.RunnerComponents.empty())
    {
        if (!this->MANIFESTJSON.contains("COMPONENTS") || !this->MANIFESTJSON["COMPONENTS"].is_array())
            this->MANIFESTJSON["COMPONENTS"] = nlohmann::ordered_json::array();
        //Dedupe by COMPONENTID: an embedded runner's components already live in MANIFESTJSON (we pass the
        //package's own COMPONENTS through GatherRunners), so re-adding them would mount the game twice.
        std::set<std::string> PoolIds;
        for (auto &C : this->MANIFESTJSON["COMPONENTS"]) PoolIds.insert(C.value("COMPONENTID", std::string()));
        int Folded = 0;
        for (auto &C : this->ContainerParams.RunnerComponents)
            if (PoolIds.insert(C.value("COMPONENTID", std::string())).second)
                { this->MANIFESTJSON["COMPONENTS"].push_back(C); ++Folded; }
        LogOut("ContainerWrapper::InitializeContainer", "Folded " + std::to_string(Folded) + " runner component(s) into the pool.");
    }

    if(!this->CreateRecipe(this->MANIFESTJSON, this->ContainerParams))
    {
        LogErr("ContainerWrapper::InitializeContainer", "ContainerWrapper::CreateRecipe failed, aborting....");
        return false;
    }
    LogSucc("ContainerWrapper::InitializeContainer", "ContainerWrapper::CreateRecipe successful.");

    if(!this->ResolveCustomVariables(this->MANIFESTJSON, this->ContainerParams, this->GlobalConfigJSON))
    {
        LogErr("ContainerWrapper::InitializeContainer", "ContainerWrapper::ResolveCustomVariables failed, aborting....");
        return false;
    }
    LogSucc("ContainerWrapper::InitializeContainer", "ContainerWrapper::ResolveCustomVariables successful.");

    if(!this->BuildSubComponentsArray(this->MANIFESTJSON, this->ContainerParams))
    {
        LogErr("ContainerWrapper::InitializeContainer", "ContainerWrapper::BuildSubComponentsArray failed, aborting....");
        return false;
    }
    LogSucc("ContainerWrapper::InitializeContainer", "ContainerWrapper::BuildSubComponentsArray successful.");

    if(!this->DerivePersistence(this->MANIFESTJSON, this->ContainerParams))
    {
        LogErr("ContainerWrapper::InitializeContainer", "ContainerWrapper::DerivePersistence failed, aborting....");
        return false;
    }
    LogSucc("ContainerWrapper::InitializeContainer", "ContainerWrapper::DerivePersistence successful.");
    /*
    MOVE HERE: VARIABLE SUBSTITUTION FOR ARGS
    VARIABLE SUBSTITUTUION FOR SUBCOMPONENTSARRAY
    */
    LogSucc("ContainerWrapper::InitializeContainerParams", "Container initialisation complete!");
    return true;
}

//Orchestrates the runtime build sequence:
//  1. AUTO-DETECTS whether any VFS layers are present in SubComponentsArray.
//  2. For Wine: initialises the prefix, applies registry patches, then mounts VFS layers.
//  3. For non-Wine without VFS: considered malformed — returns false.
//  4. Mounts the final union filesystem and checks for case conflicts.
//  5. For Wine: applies DLL overrides and file edits.
//Must be called after InitializeContainer() (done in constructor).
bool ContainerWrapper::BuildContainerRuntime()
{
    //Clear any runtime left under TempPath by a previously crashed/incomplete run BEFORE mounting.
    //Stale union/zip/bind mounts and leftover RUNTIME/DEFPREFIX/staging dirs would otherwise corrupt
    //this build (or cause spurious "already mounted"/EEXIST failures).
    ContainerWrapper::CleanStaleRuntime(ContainerParams.TempPath);

    //Verify every dependency is satisfiable (runner build cached, DEFPREFIX present, game layers local-or-fetchable),
    //then materialize any missing game-layer content from a backend (IPFS) to its expected local path. Both run
    //before InitializeDefPrefix/mount — the content must exist on disk by then. Abort if anything can't be provided.
    if (!ContainerWrapper::EnsureSources(this->ContainerParams))
    {
        LogErr("ContainerWrapper::BuildContainerRuntime", "Required sources unavailable — aborting.");
        return false;
    }
    if (!ContainerWrapper::MaterializeLayers(this->ContainerParams))
    {
        LogErr("ContainerWrapper::BuildContainerRuntime", "Could not materialize required layer content — aborting.");
        return false;
    }

    //AUTO-DETECT whether any subcomponent requires a VFS mount.
    for (auto &Sub : ContainerParams.SubComponentsArray)
    {
        std::string Type = Sub.value("TYPE", std::string());
        if (Type == "VFSZipLayer" || Type == "VFSDirLayer" || Type == "VFSFileLayer")
        {
            ContainerParams.UsesVFS = true;
            break;
        }
    }

    bool WineMode = (ContainerParams.RunnerTypeEnum == RunnerType::Wine);

    //Non-Wine packages without VFS layers have no content to run — treat as malformed.
    if (!WineMode && !ContainerParams.UsesVFS)
    {
        LogWarn("ContainerWrapper::BuildContainerRuntime", "No VFS layers found. Package may be malformed.");
        return false;
    }

    //Provision the Wine prefix (DEFPREFIX) — Wine only. Left pristine; base edits go to DEFAULTDATA below.
    //One code path, no installed-vs-not branching for edits.
    if (WineMode)
    {
        if (ContainerParams.RunnerShipsBuild)
            //Installed runner: build is pre-provisioned. Mount it read-only at RunnerMountPath; DEFPREFIX is
            //the one-time read-only artifact (no wineboot). We only READ its hives when building DEFAULTDATA.
            { if (!this->MountRunnerBuild(this->ContainerParams)) return false; }
        else
            this->InitializeDefPrefix(this->ContainerParams);
    }

    //Materialise every package-encoded BASE edit into the DEFAULTDATA layer (between the component layers
    //and the WRITELAYER). FileEdits apply to any runner; base RegEdits are Wine-only (handled inside).
    this->BuildDefaultData(this->ContainerParams);

    //Seed any persisted registry into the ephemeral WRITELAYER so it shadows DEFPREFIX.
    //No-op under PersistAll (durable RW branch already holds the reg files).
    if (ContainerParams.PersistRegistry)
        this->SeedPersistRegistry(this->ContainerParams);

    //Seed any persisted PersistFiles into the WRITELAYER so they shadow their lower layers.
    //No-op under PersistAll or when none are declared.
    if (!ContainerParams.PersistFiles.empty())
        this->SeedPersistFiles(this->ContainerParams);

    //Verify every layer's locator resolves before mounting — a missing source (e.g. a moved local file,
    //or a remote SOURCE not yet fetched) is surfaced now rather than silently mounting nothing.
    //TODO(sharing): block / offer to fetch when a remote SOURCE is missing; check runner + cross-package deps.
    if (std::vector<std::string> Missing = VerifyDependencies(this->ContainerParams); !Missing.empty())
        LogWarn("ContainerWrapper::BuildContainerRuntime",
                std::to_string(Missing.size()) + " layer source(s) missing — the runtime may be incomplete.");

    //Build the layer-spec (DEFPREFIX + VFS subcomponents target-rooted + PERSIST dirs as RW
    //passthrough) and mount it as a single vidyagodfs filesystem at RuntimePath.
    if (!this->MountVFS(this->ContainerParams)) return false;
    this->CheckCaseConflicts(ContainerParams.RuntimePath);

    //Post-mount: OVERRIDE edits operate on the mounted runtime (COW directly to WRITELAYER) — they win
    //unconditionally, over both the package content AND the user's persisted state. OVERRIDE FileEdits apply
    //to any runner; DLL overrides and OVERRIDE RegEdits are Wine-only. Base edits already live in DEFAULTDATA.
    this->ProcessFileEdits(this->ContainerParams, /*OverridePass=*/true);
    if (WineMode)
    {
        this->ProcessDLLOverrides(this->ContainerParams);
        this->ApplyOverrideRegEdits(this->ContainerParams);
    }

    LogSucc("ContainerWrapper::BuildContainerRuntime", "Runtime ready.");
    return true;
}

//STUB — VFS construction is currently handled inline in BuildContainerRuntime.
bool ContainerWrapper::BuildVirtualFilesystem()
{
    LogOut("ContainerWrapper::BuildVirtualFilesystem", "Building virtual filesystem.");
    return true;
}

//Returns a copy of LIBRARY[i]["USERSETTINGS"] for the given PackageUID, or empty object if not found.
nlohmann::ordered_json ContainerWrapper::GetPackageUserSettings(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &PackageUID)
{
    if (!GlobalConfigJSON.contains("LIBRARY")) return nlohmann::ordered_json::object();
    for (auto &Entry : GlobalConfigJSON["LIBRARY"])
    {
        if (Entry.contains("PACKAGEUID") && std::string(Entry["PACKAGEUID"]) == PackageUID)
            return Entry.contains("USERSETTINGS") ? Entry["USERSETTINGS"] : nlohmann::ordered_json::object();
    }
    return nlohmann::ordered_json::object();
}

//Writes Key=Value into LIBRARY[i]["USERSETTINGS"] for the given PackageUID.
void ContainerWrapper::SetPackageUserSetting(nlohmann::ordered_json &GlobalConfigJSON, const std::string &PackageUID, const std::string &Key, const nlohmann::ordered_json &Value)
{
    if (!GlobalConfigJSON.contains("LIBRARY")) return;
    for (auto &Entry : GlobalConfigJSON["LIBRARY"])
    {
        if (Entry.contains("PACKAGEUID") && std::string(Entry["PACKAGEUID"]) == PackageUID)
        {
            if (!Entry.contains("USERSETTINGS") || !Entry["USERSETTINGS"].is_object())
                Entry["USERSETTINGS"] = nlohmann::ordered_json::object();
            Entry["USERSETTINGS"][Key] = Value;
            return;
        }
    }
}

//Resolves which component to use based on the combination of subgame_id and component_id:
//  - Both empty:    no specific component needed (e.g. prefix-only launch).
//  - Subgame only:  reads the subgame's COMPONENT field to resolve component_id automatically.
//  - Component only: used directly (bypasses subgame lookup).
//  - Both set:      both are used as-is (component_id overrides the subgame's default).
bool ContainerWrapper::DecideComponent(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams)
{
    LogOut("ContainerWrapper::DecideComponent", "Deciding: subgame_id: " + ContainerParams.subgame_id + " VariantID: " + ContainerParams.VariantID + " component_id: " + ContainerParams.component_id);

    //Subgame path: resolve the variant (its ENDPOINTS become the build targets).
    if (!ContainerParams.subgame_id.empty())
    {
        int SubgameIdx = FindGameIndex(MANIFESTJSON, ContainerParams.subgame_id);
        if (SubgameIdx == -1) return false;

        //A directly-supplied component_id (with no variant chosen) overrides variant selection —
        //it becomes the single endpoint (CLI --component, direct-launch testing).
        if (ContainerParams.VariantID.empty() && !ContainerParams.component_id.empty())
        {
            ContainerParams.Endpoints = { ContainerParams.component_id };
            LogOut("ContainerWrapper::DecideComponent", "Direct component override: " + ContainerParams.component_id);
            return true;
        }

        std::vector<VariantInfo> Variants = GetAvailableVariants(MANIFESTJSON, ContainerParams.subgame_id);

        //If no VariantID was supplied by the caller, pick the RECOMMENDED variant (else first).
        if (ContainerParams.VariantID.empty())
        {
            for (auto &V : Variants)
                if (V.IsRecommended) { ContainerParams.VariantID = V.VariantID; break; }
            if (ContainerParams.VariantID.empty() && !Variants.empty())
            {
                ContainerParams.VariantID = Variants.front().VariantID;
                LogWarn("ContainerWrapper::DecideComponent", "No RECOMMENDED variant found, falling back to first: '" + ContainerParams.VariantID + "'");
            }
        }

        //Resolve the chosen variant's MODULES into the enabled build-target list (REQUIRED + user-enabled,
        //hierarchy-gated). ModuleStates carries the user's toggles (UI tree / --module); absent = REQUIRED||DEFAULT.
        ContainerParams.Endpoints = ResolveEnabledModules(
            GetVariantModules(MANIFESTJSON, ContainerParams.subgame_id, ContainerParams.VariantID),
            ContainerParams.ModuleStates, MANIFESTJSON);
        LogOut("ContainerWrapper::DecideComponent", "Variant '" + ContainerParams.VariantID + "' resolved to " + std::to_string(ContainerParams.Endpoints.size()) + " enabled module(s).");
        LogOut("ContainerWrapper::DecideComponent", "Running subgame " + std::string(MANIFESTJSON["GAMES"][SubgameIdx].value("TITLE", ContainerParams.subgame_id)));
        return true;
    }
    //Direct component path (editor / comparator): a single component_id is the only endpoint.
    else if (!ContainerParams.component_id.empty())
    {
        int ComponentIdx = FindComponentIndex(MANIFESTJSON, ContainerParams.component_id);
        if (ComponentIdx == -1) return false;
        ContainerParams.Endpoints = { ContainerParams.component_id };
        LogOut("ContainerWrapper::DecideComponent", "Running component " + std::string(MANIFESTJSON["COMPONENTS"][ComponentIdx].value("NAME", ContainerParams.component_id)));
        return true;
    }
    //Neither: bare defprefix.
    LogOut("ContainerWrapper::DecideComponent", "Neither subgame nor component specified, mounting defprefix.");
    return true;
}

//Builds the ordered Recipe as the UNION of every endpoint's PARENTCOMPONENT chain.
//For each endpoint (in array order), its leaf-to-root chain is collected and reversed to
//root-first, then appended to the Recipe skipping any component already present. This yields:
//  * parent-before-child (each chain is appended root-first),
//  * shared ancestors mounted exactly once (at their first appearance),
//  * independent branches ordered by endpoint array order — and because later-in-Recipe means
//    higher unionfs priority downstream, later endpoints override earlier ones (load order).
bool ContainerWrapper::CreateRecipe(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams)
{
    ContainerParams.Recipe.clear();

    //Runner-provided components mount first (base layer), then the variant's endpoints on top.
    std::vector<std::string> Endpoints = ContainerParams.RunnerEndpoints;
    for (const auto &E : ContainerParams.Endpoints) Endpoints.push_back(E);
    //Fall back to a single endpoint from component_id if Endpoints wasn't populated (direct mode).
    if (Endpoints.empty() && !ContainerParams.component_id.empty())
        Endpoints = { ContainerParams.component_id };

    std::unordered_set<std::string> Seen;
    for (const std::string &Endpoint : Endpoints)
    {
        //Walk this endpoint's chain leaf-first.
        std::vector<std::string> Chain;
        std::string CurrentID = Endpoint;
        while (!CurrentID.empty())
        {
            int Idx = FindComponentIndex(MANIFESTJSON, CurrentID);
            if (Idx == -1) break;
            Chain.push_back(CurrentID);
            auto &ParentField = MANIFESTJSON["COMPONENTS"][Idx]["PARENTCOMPONENT"];
            if (ParentField.is_null() || ParentField == "") break;
            CurrentID = ParentField;
        }
        //Append root-first, skipping components already pulled in by an earlier endpoint.
        for (auto It = Chain.rbegin(); It != Chain.rend(); ++It)
            if (Seen.insert(*It).second)
                ContainerParams.Recipe.push_back(*It);
    }

    LogOut("ContainerWrapper::CreateRecipe", "Recipe: " + [&](const std::vector<std::string>& v){ std::string r; for (const auto &x : v) r += x + " "; return r;}(ContainerParams.Recipe));
    return true;
}

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
bool ContainerWrapper::ResolveCustomVariables(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams, nlohmann::ordered_json GlobalConfigJSON)
{
    //Helper: resolve a single bare KEY/DEFAULT pair through the priority chain.
    auto ResolveOne = [&](const std::string &Key, const std::string &DefaultValue) -> std::string
    {
        if (ContainerParams.VariableOverrides.count(Key))
        {
            LogOut("ContainerWrapper::ResolveCustomVariables", "CLI override: " + Key + " = " + ContainerParams.VariableOverrides.at(Key));
            return ContainerParams.VariableOverrides.at(Key);
        }
        auto US = GetPackageUserSettings(GlobalConfigJSON, ContainerParams.PackageUID);
        if (US.contains("VARIABLES") && US["VARIABLES"].contains(Key))
        {
            std::string Val = US["VARIABLES"][Key];
            LogOut("ContainerWrapper::ResolveCustomVariables", "User setting: " + Key + " = " + Val);
            return Val;
        }
        LogOut("ContainerWrapper::ResolveCustomVariables", "Default: " + Key + " = " + DefaultValue);
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
                ContainerWrapper::StringVariableSubstitution(Picked, ContainerParams.GetVariablesMap());
                ContainerParams.CustomVariables[Key] = Picked;
            }
            else { LogWarn("ContainerWrapper::ResolveCustomVariables", "  " + Key + ": random has no OPTIONS."); ContainerParams.CustomVariables[Key] = ""; }
            LogOut("ContainerWrapper::ResolveCustomVariables", "  " + Key + " = [random] " + ContainerParams.CustomVariables[Key]);
            return;
        }

        std::string Raw = ResolveOne(Key, CV.value("DEFAULT", std::string()));
        ContainerWrapper::StringVariableSubstitution(Raw, ContainerParams.GetVariablesMap());
        ContainerParams.CustomVariables[Key] = TranslateCustomVarValue(Raw, VarType);
        LogOut("ContainerWrapper::ResolveCustomVariables", "  " + Key + " = " + ContainerParams.CustomVariables[Key]);
    };

    //Walk the Recipe in order; for each component, resolve its CustomVar subcomponents. Later
    //components win (they re-assign the same bare key).
    LogOut("ContainerWrapper::ResolveCustomVariables", "Resolving CustomVar subcomponents...");
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
    LogSucc("ContainerWrapper::ResolveCustomVariables", "Resolved " + std::to_string(ContainerParams.CustomVariables.size()) + " custom variable(s).");
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
bool ContainerWrapper::DerivePersistence(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams)
{
    ContainerParams.PersistDirs.clear();
    ContainerParams.PersistFiles.clear();
    ContainerParams.PersistRegKeys.clear();
    ContainerParams.PersistRegistry = false;
    bool AnyDeclared = false;

    LogOut("ContainerWrapper::DerivePersistence", "Scanning Recipe for Persist* subcomponents...");
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
                ContainerWrapper::StringVariableSubstitution(Path, ContainerParams.GetVariablesMap());
                if (Path.empty()) { LogWarn("ContainerWrapper::DerivePersistence", "  " + Type + " with empty PATH (skipped)."); continue; }
                AnyDeclared = true;
                if (Type == "PersistDir") { ContainerParams.PersistDirs.push_back(Path);  LogOut("ContainerWrapper::DerivePersistence", "  PersistDir  " + Path); }
                else                      { ContainerParams.PersistFiles.push_back(Path); LogOut("ContainerWrapper::DerivePersistence", "  PersistFile " + Path); }
            }
            else if (Type == "RegPersist")
            {
                AnyDeclared = true;
                ContainerParams.PersistRegistry = true;
                LogOut("ContainerWrapper::DerivePersistence", "  RegPersist (whole-registry persist)");
            }
            else if (Type == "RegKeyPersist")
            {
                std::string RegPath = S.value("REGPATH", std::string());
                ContainerWrapper::StringVariableSubstitution(RegPath, ContainerParams.GetVariablesMap());
                if (RegPath.empty()) { LogWarn("ContainerWrapper::DerivePersistence", "  RegKeyPersist with empty REGPATH (skipped)."); continue; }
                AnyDeclared = true;
                ContainerParams.PersistRegKeys.push_back(RegPath);
                LogOut("ContainerWrapper::DerivePersistence", "  RegKeyPersist " + RegPath);
            }
        }
    }

    //No selective persist points declared → persist the whole runtime overlay (durable RW branch).
    ContainerParams.PersistAll = !AnyDeclared;
    LogSucc("ContainerWrapper::DerivePersistence",
            "PERSIST: ALL=" + std::string(ContainerParams.PersistAll ? "true" : "false") +
            " REGISTRY=" + std::string(ContainerParams.PersistRegistry ? "true" : "false") +
            " DIRS=" + std::to_string(ContainerParams.PersistDirs.size()) +
            " FILES=" + std::to_string(ContainerParams.PersistFiles.size()) +
            " REGKEYS=" + std::to_string(ContainerParams.PersistRegKeys.size()));
    return true;
}

//Reads a MODULES json array (objects: COMPONENT/REQUIRED/DEFAULT/LABEL) into ModuleInfo entries.
//MODULES-only: non-object entries and entries without a COMPONENT are skipped (no legacy tolerance).
std::vector<ModuleInfo> ContainerWrapper::ParseModules(const nlohmann::ordered_json &ModulesArray)
{
    std::vector<ModuleInfo> Modules;
    if (!ModulesArray.is_array()) return Modules;
    for (const auto &M : ModulesArray)
    {
        if (!M.is_object()) continue;
        ModuleInfo Info;
        Info.Component = M.value("COMPONENT", std::string());
        if (Info.Component.empty()) continue;
        Info.Label    = M.value("LABEL", std::string());
        Info.Required = M.value("REQUIRED", true);
        Info.Default  = M.value("DEFAULT", true);
        if (M.contains("EXCLUDE") && M["EXCLUDE"].is_array())
            for (const auto &E : M["EXCLUDE"])
                if (E.is_string() && !std::string(E).empty()) Info.Exclude.push_back(std::string(E));
        Modules.push_back(std::move(Info));
    }
    return Modules;
}

//Returns all variants listed under SUBGAMES[SubgameID].VARIANTS (each with its parsed MODULES).
std::vector<VariantInfo> ContainerWrapper::GetAvailableVariants(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID)
{
    std::vector<VariantInfo> Variants;
    int SubgameIdx = FindGameIndex(MANIFESTJSON, SubgameID);
    if (SubgameIdx == -1) return Variants;
    auto &Subgame = MANIFESTJSON["GAMES"][SubgameIdx];
    if (!Subgame.contains("VARIANTS") || !Subgame["VARIANTS"].is_array()) return Variants;
    for (auto &V : Subgame["VARIANTS"])
    {
        VariantInfo Info;
        Info.VariantID     = V.value("VARIANT_ID", std::string());
        Info.Name          = V.value("NAME", Info.VariantID);
        Info.IsRecommended = V.value("RECOMMENDED", false);
        Info.HostPlatform  = V.value("HOST_PLATFORM", std::string());
        if (V.contains("GUEST_PLATFORM") && V["GUEST_PLATFORM"].is_array())   // runner variants only
            for (const auto &P : V["GUEST_PLATFORM"]) if (P.is_string()) Info.GuestPlatform.push_back(std::string(P));
        Info.Modules       = ParseModules(V.value("MODULES", nlohmann::ordered_json::array()));
        Variants.push_back(std::move(Info));
    }
    return Variants;
}

//Returns the MODULES of the variant matching { SubgameID, VariantID } (empty if not found).
std::vector<ModuleInfo> ContainerWrapper::GetVariantModules(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID, const std::string &VariantID)
{
    int SubgameIdx = FindGameIndex(MANIFESTJSON, SubgameID);
    if (SubgameIdx == -1) return {};
    auto &Subgame = MANIFESTJSON["GAMES"][SubgameIdx];
    if (!Subgame.contains("VARIANTS") || !Subgame["VARIANTS"].is_array()) return {};
    for (auto &V : Subgame["VARIANTS"])
        if (V.value("VARIANT_ID", std::string()) == VariantID)
            return ParseModules(V.value("MODULES", nlohmann::ordered_json::array()));
    return {};
}

//Resolves the enabled subset of `Modules` to component ids (in load order). See header for the rules:
//raw enable (REQUIRED || (state ? : DEFAULT)), REQUIRED propagated up the PARENTCOMPONENT chain, then
//the hierarchy gate (a disabled module-ancestor force-disables descendants).
std::vector<std::string> ContainerWrapper::ResolveEnabledModules(const std::vector<ModuleInfo> &Modules,
                                                                 const std::map<std::string, bool> &ModuleStates,
                                                                 const nlohmann::ordered_json &MANIFESTJSON)
{
    std::map<std::string, bool> InModules; // component → is a module here
    for (const auto &M : Modules) InModules[M.Component] = true;

    //All module-ancestors of a component (PARENTCOMPONENT chain entries that are themselves modules).
    auto ModuleAncestors = [&](const std::string &Component) {
        std::vector<std::string> Ancestors;
        int Idx = FindComponentIndex(MANIFESTJSON, Component);
        while (Idx != -1)
        {
            const auto &Comp = MANIFESTJSON["COMPONENTS"][Idx];
            if (!Comp.contains("PARENTCOMPONENT")) break;          // a component may have no parent (root / runner build)
            const auto &ParentField = Comp["PARENTCOMPONENT"];
            if (ParentField.is_null() || ParentField == "") break;
            std::string Parent = ParentField;
            if (InModules.count(Parent)) Ancestors.push_back(Parent);
            Idx = FindComponentIndex(MANIFESTJSON, Parent);
        }
        return Ancestors;
    };

    //Step 1: raw enable.
    std::map<std::string, bool> Enabled;
    for (const auto &M : Modules)
        Enabled[M.Component] = M.Required ? true
                             : (ModuleStates.count(M.Component) ? ModuleStates.at(M.Component) : M.Default);

    //Step 2: a REQUIRED module forces all its module-ancestors enabled (a required child keeps its parents).
    for (const auto &M : Modules)
        if (M.Required)
            for (const auto &A : ModuleAncestors(M.Component)) Enabled[A] = true;

    //Step 2.5: mutual exclusion (EXCLUDE) — no two components linked by an exclusion edge stay enabled. The
    //relation is symmetric (A excludes B ⇔ B excludes A). REQUIRED modules are kept first (they win); then each
    //still-enabled optional, in declaration order, is dropped if it conflicts with anything already kept — so
    //the first-declared of a mutually-exclusive set survives. Deterministic, so a saved state or --module flag
    //enabling both members can never leak both into the recipe.
    std::map<std::string, std::set<std::string>> ExclAdj;
    for (const auto &M : Modules)
        for (const auto &E : M.Exclude) { ExclAdj[M.Component].insert(E); ExclAdj[E].insert(M.Component); }
    if (!ExclAdj.empty())
    {
        std::set<std::string> Kept;
        auto Conflicts = [&](const std::string &C) {
            auto it = ExclAdj.find(C);
            if (it == ExclAdj.end()) return false;
            for (const auto &K : Kept) if (it->second.count(K)) return true;
            return false;
        };
        for (const auto &M : Modules) if (Enabled[M.Component] && M.Required)  Kept.insert(M.Component);
        for (const auto &M : Modules)
        {
            if (!Enabled[M.Component] || M.Required) continue;
            if (Conflicts(M.Component)) Enabled[M.Component] = false;
            else                        Kept.insert(M.Component);
        }
    }

    //Step 3: hierarchy gate — drop any module with a disabled module-ancestor (full chain).
    std::vector<std::string> EnabledComponents;
    for (const auto &M : Modules)
    {
        if (!Enabled[M.Component]) continue;
        bool AncestorsOk = true;
        for (const auto &A : ModuleAncestors(M.Component))
            if (!Enabled[A]) { AncestorsOk = false; break; }
        if (AncestorsOk) EnabledComponents.push_back(M.Component);
    }
    return EnabledComponents;
}

//Convenience: the default-enabled module components of a variant (REQUIRED||DEFAULT, no user overrides).
//Used where a representative recipe scope is needed (e.g. the prelaunch CustomVar picker rebuild).
std::vector<std::string> ContainerWrapper::FindEndpointsForVariant(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID, const std::string &VariantID)
{
    return ResolveEnabledModules(GetVariantModules(MANIFESTJSON, SubgameID, VariantID), {}, MANIFESTJSON);
}

//Finds the variant in MANIFESTJSON matching ContainerParams.VariantID
//under SUBGAMES[ContainerParams.subgame_id].VARIANTS and populates exe/work-dir/args fields.
//Must be called AFTER BuildSubComponentsArray and BEFORE BuildContainerRuntime.
bool ContainerWrapper::ResolveExecutableDefinition(const nlohmann::ordered_json &MANIFESTJSON, struct ContainerParams &ContainerParams)
{
    if (ContainerParams.VariantID.empty())
    {
        LogWarn("ContainerWrapper::ResolveExecutableDefinition", "No VariantID set — skipping.");
        return true;
    }
    // Find the variant with matching VARIANT_ID in the subgame.
    int SubgameIdx = FindGameIndex(MANIFESTJSON, ContainerParams.subgame_id);
    nlohmann::ordered_json Resolved;
    if (SubgameIdx != -1 && MANIFESTJSON["GAMES"][SubgameIdx].contains("VARIANTS"))
    {
        for (auto &V : MANIFESTJSON["GAMES"][SubgameIdx]["VARIANTS"])
        {
            if (V.value("VARIANT_ID", std::string()) == ContainerParams.VariantID)
            { Resolved = V; break; }
        }
    }
    if (Resolved.is_null() || Resolved.empty())
    {
        LogErr("ContainerWrapper::ResolveExecutableDefinition", "No variant found for VARIANT_ID='" + ContainerParams.VariantID + "' in subgame '" + ContainerParams.subgame_id + "'");
        return false;
    }
    LogSucc("ContainerWrapper::ResolveExecutableDefinition", "Resolved variant: " + ContainerParams.VariantID);
    // Pick exe key by runner type.
    std::string ExeKey = "EXEPATH";
    if      (ContainerParams.RunnerTypeEnum == RunnerType::Emulator) ExeKey = "ROM";
    else if (ContainerParams.RunnerTypeEnum == RunnerType::Custom)   ExeKey = "DATAPATH";
    auto &ExeVal = Resolved[ExeKey];
    {
        std::string ExeStr = (!ExeVal.is_null() && ExeVal.is_string()) ? std::string(ExeVal) : std::string();
        ContainerWrapper::StringVariableSubstitution(ExeStr, ContainerParams.GetVariablesMap());
        ContainerParams.ExePathRelative = std::filesystem::path(ExeStr);
    }
    ContainerParams.ExePathComplete = ContainerParams.ProgramPath / ContainerParams.ExePathRelative;
    LogOut("ContainerWrapper::ResolveExecutableDefinition", "ExePathRelative: " + ContainerParams.ExePathRelative.string());
    LogOut("ContainerWrapper::ResolveExecutableDefinition", "ExePathComplete: " + ContainerParams.ExePathComplete.string());
    if (ContainerParams.RunnerTypeEnum == RunnerType::Wine)
    {
        ContainerParams.ExePathInPrefix        = std::filesystem::path("C:") / ContainerParams.PackageUID / ContainerParams.ExePathRelative;
        ContainerParams.WindowsExePathComplete = "C:\\" + ContainerParams.PackageUID + "\\" + ContainerParams.ExePathRelative.string();
        LogOut("ContainerWrapper::ResolveExecutableDefinition", "ExePathInPrefix: " + ContainerParams.ExePathInPrefix.string());
    }
    auto &WorkDirVal = Resolved["WORKDIR"];
    if (!WorkDirVal.is_null() && WorkDirVal.is_string() && !std::string(WorkDirVal).empty())
    {
        std::string WorkDirStr = std::string(WorkDirVal);
        ContainerWrapper::StringVariableSubstitution(WorkDirStr, ContainerParams.GetVariablesMap());
        ContainerParams.WorkDirPathRelative = std::filesystem::path(WorkDirStr);
        ContainerParams.WorkDirPathComplete = ContainerParams.ProgramPath / ContainerParams.WorkDirPathRelative;
    }
    else ContainerParams.WorkDirPathComplete = ContainerParams.ProgramPath;
    LogOut("ContainerWrapper::ResolveExecutableDefinition", "WorkDirPathComplete: " + ContainerParams.WorkDirPathComplete.string());
    ContainerParams.ExeArgs.clear();
    auto &ExeArgsVal = Resolved["EXEARGS"];
    if (!ExeArgsVal.is_null() && ExeArgsVal.is_string() && !std::string(ExeArgsVal).empty())
    {
        std::string Args = std::string(ExeArgsVal);
        ContainerWrapper::StringVariableSubstitution(Args, ContainerParams.GetVariablesMap());
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
bool ContainerWrapper::BuildSubComponentsArray(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams)
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
            ContainerWrapper::StringVariableSubstitution(SubJSON, ContainerParams.GetVariablesMap());
            ContainerParams.SubComponentsArray.push_back(nlohmann::ordered_json::parse(SubJSON));
            LogOut("ContainerWrapper::BuildSubComponentsArray", "Added COMPONENT " + RecipeComponentID + " SUBCOMPONENT " + std::to_string(j + 1));
        }
    }
    LogOut("ContainerWrapper::BuildSubComponentsArray", "Completed SubComponentsArray.");
    //std::cout << ContainerParams.SubComponentsArray.dump(4) << std::endl;
    return true;
}

//The platform the HOST runs as. Phase-1 stub — only Linux is supported, so this is constant.
//TO BE REPLACED by real OS/arch detection when VidyaGod runs on other hosts (e.g. Windows).
std::string ContainerWrapper::HostPlatform()
{
    return "linux64";
}

//The managed library root — repos are git-cloned here (one subfolder per repo) and every package hydrates in
//place beside its manifest. This is the ONE location for everything; there is no DOWNLOADS / ipfs cache.
//Overridable via Settings.Paths.LibraryRoot; default ~/.VidyaGod/LIBRARY.
static std::string LibraryDir(const nlohmann::ordered_json &GlobalConfigJSON)
{
    if (GlobalConfigJSON.contains("Settings") && GlobalConfigJSON["Settings"].is_object())
    {
        const auto &S = GlobalConfigJSON["Settings"];
        if (S.contains("Paths") && S["Paths"].is_object() && S["Paths"].contains("LibraryRoot")
            && S["Paths"]["LibraryRoot"].is_string() && !std::string(S["Paths"]["LibraryRoot"]).empty())
            return QDir::cleanPath(QString::fromStdString(std::string(S["Paths"]["LibraryRoot"]))).toStdString();
    }
    return QDir::cleanPath(QDir::homePath() + "/.VidyaGod/LIBRARY").toStdString();
}

std::string ContainerWrapper::LibraryRootDir(const nlohmann::ordered_json &GlobalConfigJSON) { return LibraryDir(GlobalConfigJSON); }

//A repository's clone URL. Each Settings.Repositories[] entry is a git repo: an object with a "PATH"
//(the URL), or a bare URL string.
static std::string RepositoryURL(const nlohmann::ordered_json &R)
{
    if (R.is_object() && R.contains("PATH") && R["PATH"].is_string()) return std::string(R["PATH"]);
    if (R.is_string()) return std::string(R);
    return std::string();
}

//The clone directory name for a git repository: its NAME, else the URL basename minus a ".git" suffix.
static std::string GitRepoName(const nlohmann::ordered_json &R)
{
    std::string Name = R.is_object() ? R.value("NAME", std::string()) : std::string();
    if (Name.empty())
    {
        std::string U = RepositoryURL(R);
        while (!U.empty() && U.back() == '/') U.pop_back();
        const auto Slash = U.find_last_of('/');
        Name = (Slash == std::string::npos) ? U : U.substr(Slash + 1);
        const std::string Suffix = ".git";
        if (Name.size() > Suffix.size() && Name.compare(Name.size() - Suffix.size(), Suffix.size(), Suffix) == 0)
            Name = Name.substr(0, Name.size() - Suffix.size());
    }
    return Name.empty() ? std::string("repo") : Name;
}

//A repository's git working tree: LIBRARY/<name> (cloned/pulled by SyncRepositories; whether or not it exists
//yet). The clone IS the library — its dehydrated package dirs hydrate content in place.
static std::string RepositoryLocalDir(const nlohmann::ordered_json &GlobalConfigJSON, const nlohmann::ordered_json &R)
{
    return QDir::cleanPath(QString::fromStdString(LibraryDir(GlobalConfigJSON) + "/" + GitRepoName(R))).toStdString();
}

//Returns the clone directories of every configured Settings.Repositories[] git repo (indexed in order).
static std::vector<std::string> RepositoryDirs(const nlohmann::ordered_json &GlobalConfigJSON)
{
    std::vector<std::string> Dirs;
    const nlohmann::ordered_json *Settings = (GlobalConfigJSON.contains("Settings") && GlobalConfigJSON["Settings"].is_object())
                                             ? &GlobalConfigJSON["Settings"] : nullptr;
    if (Settings && Settings->contains("Repositories") && (*Settings)["Repositories"].is_array())
        for (const auto &R : (*Settings)["Repositories"])
            if (!RepositoryURL(R).empty()) Dirs.push_back(RepositoryLocalDir(GlobalConfigJSON, R));
    return Dirs;
}

//Every package manifest known across all Repositories — the catalog. Kind-agnostic (games, runners,
//libraries) and globally cross-referenceable.
//TODO(sharing): also fold in LIBRARY entries + DOWNLOADS, dedupe by PACKAGEUID, and persist the index
//rather than rescanning every call.
std::vector<std::pair<nlohmann::ordered_json, std::string>> ContainerWrapper::CatalogPackagesWithDir(const nlohmann::ordered_json &GlobalConfigJSON)
{
    std::vector<std::pair<nlohmann::ordered_json, std::string>> Packages;
    std::set<std::string> SeenUID; // first occurrence wins → earlier repository shadows later ones
    for (const auto &RepoDir : RepositoryDirs(GlobalConfigJSON))
    {
        QDir Dir(QString::fromStdString(RepoDir));
        if (!Dir.exists()) continue;
        for (const QString &Sub : Dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        {
            const QString PkgDir = Dir.filePath(Sub);
            nlohmann::ordered_json Pkg; std::vector<std::string> Warn;
            if (!JSONOps::AssembleManifest(PkgDir, Pkg, Warn)) continue;
            std::string Uid = Pkg.value("PACKAGEUID", std::string());
            if (!Uid.empty() && !SeenUID.insert(Uid).second) continue; // already provided by a higher-priority repo
            Packages.emplace_back(std::move(Pkg), PkgDir.toStdString());
        }
    }
    return Packages;
}

std::vector<nlohmann::ordered_json> ContainerWrapper::CatalogPackages(const nlohmann::ordered_json &GlobalConfigJSON)
{
    std::vector<nlohmann::ordered_json> Packages;
    for (auto &[Pkg, Dir] : CatalogPackagesWithDir(GlobalConfigJSON)) { (void)Dir; Packages.push_back(std::move(Pkg)); }
    return Packages;
}

//Gathers every runner object from every catalog package that HasRunners (a filtered view of the catalog).
std::vector<nlohmann::ordered_json> ContainerWrapper::RegistryRunners(const nlohmann::ordered_json &GlobalConfigJSON)
{
    std::vector<nlohmann::ordered_json> Runners;
    for (const auto &Pkg : CatalogPackages(GlobalConfigJSON))
        if (JSONOps::HasRunners(Pkg))
            for (const auto &R : Pkg["RUNNERS"]) Runners.push_back(R);
    return Runners;
}

//Runs git with the given arguments, blocking up to a couple of minutes. Returns true on a clean exit.
static bool RunGit(const QStringList &Args, const QString &WorkDir = QString())
{
    QProcess P;
    P.setProcessEnvironment(SystemToolEnv());                                   // system git must not inherit the AppImage LD_LIBRARY_PATH
    if (!WorkDir.isEmpty()) P.setWorkingDirectory(WorkDir);
    P.start("git", Args);
    if (!P.waitForStarted(10000)) return false;
    if (!P.waitForFinished(120000)) { P.kill(); P.waitForFinished(2000); return false; }
    return P.exitStatus() == QProcess::NormalExit && P.exitCode() == 0;
}

//Clones (first run) or updates (subsequent runs) a git repository into CloneDir. Self-heals from a rewritten
//remote history (e.g. a force-push / squash): a plain fast-forward pull fails there, so fall back to fetch +
//hard-reset to the remote, and re-clone from scratch only if even that fails.
static bool SyncGitRepository(const std::string &Url, const std::string &CloneDir)
{
    if (Url.empty()) return false;
    const QString Dir = QString::fromStdString(CloneDir);
    if (QDir(Dir + "/.git").exists())
    {
        if (RunGit({"pull", "--ff-only"}, Dir)) return true;                      // normal fast-forward
        if (RunGit({"fetch", "origin"}, Dir) && RunGit({"reset", "--hard", "@{u}"}, Dir))
            return true;                                                          // diverged/rewritten → realign
        QDir(Dir).removeRecursively();                                            // unrecoverable → re-clone below
    }
    else if (QDir(Dir).exists())
        QDir(Dir).removeRecursively();                                           // a non-git dir (old copied mirror) → replace with a clone
    QDir().mkpath(QString::fromStdString(std::filesystem::path(CloneDir).parent_path().string())); // LIBRARY root must exist
    return RunGit({"clone", "--depth", "1", QString::fromStdString(Url), Dir});
}

//Upserts a slim index entry {PACKAGEUID,PACKAGENAME,PATH,REPO} into Arr (LIBRARY or RUNNERS), keyed by
//PACKAGEUID. A repo sync owns entries carrying a REPO; a local (no-REPO) entry with the same UID wins and is
//left untouched (the user's own package shadows a repo copy). (Versioning is by git commit — no PACKAGEVERSION.)
static void UpsertIndexEntry(nlohmann::ordered_json &Arr, const std::string &Uid, const std::string &Repo,
                             const std::string &MirrorDir, const nlohmann::ordered_json &Pkg)
{
    for (auto &E : Arr)
    {
        if (E.value("PACKAGEUID", std::string()) != Uid) continue;
        if (!E.contains("REPO")) return;                                          // local entry owns this UID
        E["PACKAGENAME"] = Pkg.value("PACKAGENAME", std::string());
        E["PATH"]        = MirrorDir;
        E["REPO"]        = Repo;
        E.erase("PACKAGEVERSION");                                                // drop the legacy field if present
        return;
    }
    nlohmann::ordered_json Slim;
    Slim["PACKAGEUID"]  = Uid;
    Slim["PACKAGENAME"] = Pkg.value("PACKAGENAME", std::string());
    Slim["PATH"]        = MirrorDir;
    Slim["REPO"]        = Repo;
    Arr.push_back(std::move(Slim));
}

//True if any variant of a runner package has been imported (build cached + DEFPREFIX) — its "hydrated" state.
static bool RunnerPkgImported(const nlohmann::ordered_json &Pkg, const std::string &PackageDir)
{
    for (const std::string &Vid : RunnerWrapper::VariantIds(Pkg))
        if (RunnerWrapper::IsImported(Pkg, PackageDir, Vid)) return true;
    return false;
}

//Reconciliation: drops REPO-sourced LIBRARY entries whose PACKAGEUID is no longer provided by any repo (not in
//SeenUids) AND that hold nothing the user downloaded, deleting their mirror dir (only under LibRoot). Local
//(no-REPO) entries and downloaded orphans are kept. "Downloaded" is kind-agnostic: a game whose content layers
//are present, or a runner that's been imported (the same package can be both).
static void ReconcileIndex(nlohmann::ordered_json &Arr, const std::set<std::string> &SeenUids, const std::string &LibRoot)
{
    for (int i = (int)Arr.size() - 1; i >= 0; --i)
    {
        nlohmann::ordered_json &E = Arr[i];
        if (!E.contains("REPO")) continue;                                        // local — keep
        if (SeenUids.count(E.value("PACKAGEUID", std::string()))) continue;       // still in a repo — keep
        const std::string Path = E.value("PATH", std::string());
        nlohmann::ordered_json Pkg; std::vector<std::string> Warn;
        bool Downloaded = false;
        if (JSONOps::AssembleManifest(QString::fromStdString(Path), Pkg, Warn))
            Downloaded = (JSONOps::HasGames(Pkg)   && !ContainerWrapper::PackageIpfsCids(Pkg).empty()
                                                   && ContainerWrapper::PackageHydrated(Pkg, Path))
                      || (JSONOps::HasRunners(Pkg) && RunnerPkgImported(Pkg, Path));
        if (Downloaded) continue;                                                 // downloaded orphan — keep
        if (!Path.empty() && Path.rfind(LibRoot, 0) == 0) { std::error_code Ec; std::filesystem::remove_all(Path, Ec); }
        Arr.erase(Arr.begin() + i);
    }
}

//Syncs the configured Repositories: git clone/pull each directly into LIBRARY/<repo> (the clone IS the library —
//no separate mirror, no DOWNLOADS), then upsert ONE LIBRARY index entry per package pointing at its cloned dir.
//Kind (game/runner/dependency) is emergent from the manifest, so a repo's games AND runners (a package can be
//both) all live in the single LIBRARY. Importing later fetches a package's content in place beside its manifest.
//Reconciles away repo entries that vanished. Caller persists GlobalConfigJSON.
void ContainerWrapper::SyncRepositories(nlohmann::ordered_json &GlobalConfigJSON)
{
    if (!GlobalConfigJSON.contains("Settings") || !GlobalConfigJSON["Settings"].is_object()) return;
    if (!GlobalConfigJSON["Settings"].contains("Repositories") || !GlobalConfigJSON["Settings"]["Repositories"].is_array()) return;

    //Ensure LIBRARY exists BEFORE iterating — adding a top-level key reallocates the object's storage, so
    //creating it later would dangle a cached reference into the Repositories array.
    if (!GlobalConfigJSON.contains("LIBRARY") || !GlobalConfigJSON["LIBRARY"].is_array())
        GlobalConfigJSON["LIBRARY"] = nlohmann::ordered_json::array();

    const std::string LibRoot = LibraryDir(GlobalConfigJSON);
    std::set<std::string> SeenUid;

    for (auto &R : GlobalConfigJSON["Settings"]["Repositories"])
    {
        const std::string Url = RepositoryURL(R);
        if (Url.empty()) continue;
        const std::string RepoName = GitRepoName(R);
        const std::string Local = RepositoryLocalDir(GlobalConfigJSON, R);
        LogOut("ContainerWrapper::SyncRepositories", "Syncing git repository " + Url + " -> " + Local);
        if (!SyncGitRepository(Url, Local))
            LogWarn("ContainerWrapper::SyncRepositories", "git clone/pull failed for " + Url + " (using last-synced clone, if any).");

        QDir D(QString::fromStdString(Local));
        if (!D.exists()) { LogWarn("ContainerWrapper::SyncRepositories", "Repository missing (skipped): " + Local); continue; }

        int Games = 0, Runners = 0;
        for (const QString &Sub : D.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        {
            const QString ClonePkgDir = D.filePath(Sub);
            nlohmann::ordered_json Pkg; std::vector<std::string> Warn;
            if (!JSONOps::AssembleManifest(ClonePkgDir, Pkg, Warn)) continue;
            const std::string Uid = Pkg.value("PACKAGEUID", std::string());
            if (Uid.empty()) continue;

            //The cloned package dir IS the library package — content hydrates here in place. Index it directly.
            SeenUid.insert(Uid);
            UpsertIndexEntry(GlobalConfigJSON["LIBRARY"], Uid, RepoName, ClonePkgDir.toStdString(), Pkg);
            if (JSONOps::HasGames(Pkg))   ++Games;
            if (JSONOps::HasRunners(Pkg)) ++Runners;
        }
        LogOut("ContainerWrapper::SyncRepositories", "Indexed " + Local + " ("
               + std::to_string(Games) + " game(s), " + std::to_string(Runners) + " runner(s)).");
    }

    ReconcileIndex(GlobalConfigJSON["LIBRARY"], SeenUid, LibRoot);
}

//Fills all derived fields in ContainerParams from MANIFEST and GlobalConfigJSON.
//Must be called after DecideComponent so ContainerParams.subgame_id and platform are set.
//
//Runner resolution: candidates are runners (the package's own + every GLOBAL_RUNNERS registry
//package) whose GUEST_PLATFORM contains the package's HOST_PLATFORM (the package's own runners shadow
//registry runners by RUNNER_ID). The chosen one, in priority order:
//  1. ContainerParams.RunnerID            (explicit pick from the picker / --runner)
//  2. the selected variant's RUNNER_ID    (variant pin)
//  3. USERSETTINGS[PackageUID].PREFERRED_RUNNER
//  4. the first candidate
//
//Path layout differs by runner type:
//  Wine:  ProgramPath = RuntimePath/drive_c/PackageUID, prefix at TempPath/DEFPREFIX.
//  Other: ProgramPath = RuntimePath (files mounted directly at the union root).
bool ContainerWrapper::DeriveContainerParams(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams, nlohmann::ordered_json GlobalConfigJSON)
{
    //Package-specific:
    ContainerParams.PackageName                         = MANIFESTJSON["PACKAGENAME"];
    LogOut("ContainerWrapper::DeriveContainerParams", "PackageName: " + ContainerParams.PackageName);

    ContainerParams.PackageUID                          = MANIFESTJSON["PACKAGEUID"];
    LogOut("ContainerWrapper::DeriveContainerParams", "PackageUID: " + ContainerParams.PackageUID);

    //Game specific — only populated when a game was specified.
    int SubgameIdx = FindGameIndex(MANIFESTJSON, ContainerParams.subgame_id);

    //HOST_PLATFORM is per-variant: the platform the SELECTED game variant targets, matched against each
    //runner variant's GUEST_PLATFORM. (Free-form: "win32", "linux64", "snes", …) Empty only on the
    //no-game tooling path (prefix-only), where no runner match is expected.
    ContainerParams.Platform = std::string();
    if (SubgameIdx != -1 && MANIFESTJSON["GAMES"][SubgameIdx].contains("VARIANTS") && MANIFESTJSON["GAMES"][SubgameIdx]["VARIANTS"].is_array())
        for (const auto &V : MANIFESTJSON["GAMES"][SubgameIdx]["VARIANTS"])
            if (V.value("VARIANT_ID", std::string()) == ContainerParams.VariantID)
            { ContainerParams.Platform = V.value("HOST_PLATFORM", std::string()); break; }

    if (!ContainerParams.subgame_id.empty() && SubgameIdx != -1)
    {
        ContainerParams.GameName                        = MANIFESTJSON["GAMES"][SubgameIdx]["TITLE"];
        //UMUID lives under the game's METADATA. "0" means no Steam AppID — umu-run treats that
        //as a generic Wine launch.
        auto &Meta      = MANIFESTJSON["GAMES"][SubgameIdx]["METADATA"];
        ContainerParams.UMUID                           = (Meta.is_object() && Meta.contains("UMUID") && Meta["UMUID"].is_string())
                                                          ? std::string(Meta["UMUID"]) : "0";
    }
    else
    {
        ContainerParams.UMUID                           = "0";
    }
    LogOut("ContainerWrapper::DeriveContainerParams", "GameName: " + ContainerParams.GameName);
    LogOut("ContainerWrapper::DeriveContainerParams", "UMUID: " + ContainerParams.UMUID);
    LogOut("ContainerWrapper::DeriveContainerParams", "HOST_PLATFORM: " + ContainerParams.Platform);

    // Runner resolution — the package's own RUNNERS (embedded bundle) plus every runner package in the
    // GLOBAL_RUNNERS registry, filtered by GUEST_PLATFORM ∋ HOST_PLATFORM.
    // Precedence: explicit RunnerID (picker/CLI) > variant pin (VARIANT.RUNNER_ID) > PREFERRED_RUNNER > first candidate.
    std::string PreferredRunner;
    {
        auto US = GetPackageUserSettings(GlobalConfigJSON, ContainerParams.PackageUID);
        if (US.contains("PREFERRED_RUNNER") && US["PREFERRED_RUNNER"].is_string())
            PreferredRunner = std::string(US["PREFERRED_RUNNER"]);
    }
    //Variant pin: the selected variant may force a specific runner.
    std::string VariantPin;
    if (SubgameIdx != -1 && MANIFESTJSON["GAMES"][SubgameIdx].contains("VARIANTS"))
        for (auto &V : MANIFESTJSON["GAMES"][SubgameIdx]["VARIANTS"])
            if (V.value("VARIANT_ID", std::string()) == ContainerParams.VariantID)
            { VariantPin = V.value("RUNNER_ID", std::string()); break; }

    //A candidate carries its runner ENTITY, the matched runner VARIANT (its exec params + build modules),
    //and its owning package's COMPONENTS (so the runner's component subcomponents resolve). The launching
    //package's own runners carry no extra components — those already live in MANIFESTJSON.
    struct RunnerCandidate { nlohmann::ordered_json Runner; nlohmann::ordered_json Variant; nlohmann::ordered_json Components; std::string PackageDir; };
    std::vector<RunnerCandidate> Candidates;
    std::unordered_set<std::string> SeenRunnerIds;
    auto GatherRunners = [&](const nlohmann::ordered_json &Source, const nlohmann::ordered_json &Components, const std::string &SourceDir)
    {
        if (!Source.contains("RUNNERS") || !Source["RUNNERS"].is_array()) return;
        for (const auto &R : Source["RUNNERS"])
        {
            std::string RID = R.value("RUNNER_ID", std::string());
            if (!RID.empty() && SeenRunnerIds.count(RID)) continue;
            if (!R.contains("VARIANTS") || !R["VARIANTS"].is_array()) continue;
            //Pick the runner VARIANT that serves the game's platform (GUEST_PLATFORM ∋ Platform) and runs on
            //this machine (HOST_PLATFORM == MachinePlatform); prefer a RECOMMENDED one.
            const nlohmann::ordered_json *Best = nullptr;
            for (const auto &V : R["VARIANTS"])
            {
                bool Guest = false;
                if (V.contains("GUEST_PLATFORM") && V["GUEST_PLATFORM"].is_array())
                    for (const auto &P : V["GUEST_PLATFORM"])
                        if (P.is_string() && std::string(P) == ContainerParams.Platform) { Guest = true; break; }
                if (!Guest || V.value("HOST_PLATFORM", std::string()) != MachinePlatform()) continue;
                if (!Best || V.value("RECOMMENDED", false)) Best = &V;
                if (V.value("RECOMMENDED", false)) break;
            }
            if (!Best) continue;
            SeenRunnerIds.insert(RID);
            Candidates.push_back({ R, *Best, Components, SourceDir });
        }
    };
    //1. The launching package's own runners first (a bundle shadows a registry runner of the same id) — its
    //   build lives in the game's own dir. Pass the package's own COMPONENTS so an embedded runner's build
    //   layers (e.g. a bundled gemrb engine) are discoverable; the fold below dedupes them against the pool.
    {
        const nlohmann::ordered_json OwnComps = (MANIFESTJSON.contains("COMPONENTS") && MANIFESTJSON["COMPONENTS"].is_array())
                                                ? MANIFESTJSON["COMPONENTS"] : nlohmann::ordered_json::array();
        GatherRunners(MANIFESTJSON, OwnComps, ContainerParams.PackagePath.string());
    }
    //2. Every runner package in each configured repository.
    //TODO(sharing): resolve from CatalogPackages once it carries each package's owning dir for components.
    for (const auto &RegDir : RepositoryDirs(GlobalConfigJSON))
    {
        QDir Dir(QString::fromStdString(RegDir));
        if (!Dir.exists()) continue;
        for (const QString &Sub : Dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        {
            nlohmann::ordered_json Pkg; std::vector<std::string> Warn;
            if (!JSONOps::AssembleManifest(Dir.filePath(Sub), Pkg, Warn)) continue;
            if (!JSONOps::HasRunners(Pkg)) continue;
            nlohmann::ordered_json Comps = (Pkg.contains("COMPONENTS") && Pkg["COMPONENTS"].is_array())
                                           ? Pkg["COMPONENTS"] : nlohmann::ordered_json::array();
            GatherRunners(Pkg, Comps, Dir.filePath(Sub).toStdString());          // the runner package's library dir
        }
    }

    //The selected runner's PREFIX_SUBPATH (default ""): the wine prefix (drive_c + hives) lives at
    //<base>/<subpath>. "" → prefix == base (umu/wine, unchanged); "pfx" → Proton's STEAM_COMPAT_DATA_PATH/pfx.
    std::string PrefixSubpath;
    const RunnerCandidate * Selected = nullptr;
    auto PickById = [&](const std::string &Id) -> bool
    {
        if (Id.empty()) return false;
        for (const auto &C : Candidates) if (C.Runner.value("RUNNER_ID", std::string()) == Id) { Selected = &C; return true; }
        return false;
    };
    //Explicit pick > variant pin > preferred; otherwise the first candidate for the platform.
    if (!PickById(ContainerParams.RunnerID) && !PickById(VariantPin) && !PickById(PreferredRunner)
        && !Candidates.empty())
        Selected = &Candidates.front();

    if (Selected)
    {
        const nlohmann::ordered_json &SelectedRunner  = Selected->Runner;   // entity: RUNNER_ID, NAME
        const nlohmann::ordered_json &SelectedVariant = Selected->Variant;  // exec params + build modules + platform
        ContainerParams.RunnerComponents = Selected->Components;
        ContainerParams.RunnerPackagePath = Selected->PackageDir;                 // runner build + DEFPREFIX live here
        //Use explicit null guards — direct JSON-to-string conversion throws on null values.
        ContainerParams.RunnerID         = SelectedRunner.value("RUNNER_ID", std::string());
        ContainerParams.RunnerName       = (SelectedRunner.contains("NAME") && SelectedRunner["NAME"].is_string()) ? std::string(SelectedRunner["NAME"]) : ContainerParams.RunnerID;
        ContainerParams.RunnerVariantID  = SelectedVariant.value("VARIANT_ID", std::string("default"));
        ContainerParams.RunnerExecutable = (SelectedVariant.contains("EXECUTABLE") && SelectedVariant["EXECUTABLE"].is_string()) ? std::string(SelectedVariant["EXECUTABLE"]) : "";
        std::string RunnerTypeStr        = (SelectedVariant.contains("TYPE") && SelectedVariant["TYPE"].is_string()) ? std::string(SelectedVariant["TYPE"]) : "custom";
        //Map the string type to the enum so the rest of the code can branch without string comparisons.
        if (RunnerTypeStr == "wine")           ContainerParams.RunnerTypeEnum = RunnerType::Wine;
        else if (RunnerTypeStr == "emulator")  ContainerParams.RunnerTypeEnum = RunnerType::Emulator;
        else if (RunnerTypeStr == "custom")    ContainerParams.RunnerTypeEnum = RunnerType::Custom;
        else                                   ContainerParams.RunnerTypeEnum = RunnerType::Native;
        if (SelectedVariant.contains("ENV"))        ContainerParams.RunnerEnv = SelectedVariant["ENV"];
        if (SelectedVariant.contains("REMOVE_ENV")) for (auto &E : SelectedVariant["REMOVE_ENV"]) ContainerParams.RunnerRemoveEnv.push_back(std::string(E));
        if (SelectedVariant.contains("ARGS"))       for (auto &A : SelectedVariant["ARGS"])       ContainerParams.RunnerArgs.push_back(std::string(A));
        if (SelectedVariant.contains("PREFIX_SUBPATH") && SelectedVariant["PREFIX_SUBPATH"].is_string())
            PrefixSubpath = std::string(SelectedVariant["PREFIX_SUBPATH"]);
        ContainerParams.UnifiedRuntime = SelectedVariant.value("UNIFIED_RUNTIME", false);

        //Installed-runner model: the runner build = the VFSZipLayer subcomponents of the components this
        //runner variant's MODULES enable. When present, the build mounts read-only at its own RunnerMountPath
        //(%RunnerMount%) and DEFPREFIX is the one-time per-(runner,variant) artifact — no per-launch wineboot.
        nlohmann::ordered_json RunnerManifest; RunnerManifest["COMPONENTS"] = ContainerParams.RunnerComponents;
        const std::vector<std::string> RunnerEnabled = ResolveEnabledModules(
            ParseModules(SelectedVariant.value("MODULES", nlohmann::ordered_json::array())),
            ContainerParams.ModuleStates, RunnerManifest);
        ContainerParams.RunnerRecipe = RunnerEnabled; //scopes runner CustomVar resolution to this variant's enabled components
        std::set<std::string> WantComps(RunnerEnabled.begin(), RunnerEnabled.end());
        for (auto &C : ContainerParams.RunnerComponents)
            if (WantComps.count(C.value("COMPONENTID", std::string())) && C.contains("SUBCOMPONENTS") && C["SUBCOMPONENTS"].is_array())
                for (auto &S : C["SUBCOMPONENTS"])
                {
                    const std::string T = S.value("TYPE", std::string());
                    if (T == "VFSZipLayer" || T == "VFSDirLayer" || T == "VFSFileLayer")
                        ContainerParams.RunnerLayers.push_back(S);
                }
        ContainerParams.RunnerShipsBuild = !ContainerParams.RunnerLayers.empty();
        //A UNIFIED runner shares the game's RUNTIME: its enabled components must enter the game recipe so their
        //layers mount alongside the game (CreateRecipe prepends RunnerEndpoints as the base). A separate-mount
        //runner instead mounts its build at %RunnerMount% via MountRunnerBuild, so it stays out of the recipe.
        ContainerParams.RunnerEndpoints  = ContainerParams.UnifiedRuntime ? RunnerEnabled : std::vector<std::string>{};
    }
    else
    {
        //No runner serves this platform — a malformed/incomplete package. Fail loudly rather than
        //guessing; the launch will abort with an empty runner.
        LogErr("ContainerWrapper::DeriveContainerParams", "No runner found for platform '" + ContainerParams.Platform + "'.");
    }
    LogOut("ContainerWrapper::DeriveContainerParams", "Runner: " + ContainerParams.RunnerName + " (" + ContainerParams.RunnerExecutable + ")");

    //System Variables — screen geometry. QGuiApplication screen access is only valid on the main
    //(GUI) thread; the GUI pre-populates ScreenWidth/Height before launching on its worker thread.
    //We only query live when we are actually on the main thread with a valid primary screen (so the
    //values stay accurate for direct main-thread callers), and otherwise trust what was passed in.
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
    LogOut("ContainerWrapper::DeriveContainerParams", "ScreenWidth: " + ContainerParams.ScreenWidth);
    LogOut("ContainerWrapper::DeriveContainerParams", "ScreenHeight: " + ContainerParams.ScreenHeight);

    //All ephemeral session state lives outside the package, under <TempRoot>/PackageUID,
    //so the package directory itself stays pristine. RUNTIME, WRITELAYER, DEFPREFIX and the
    //per-layer pre-mount dirs are all nested inside TempPath and wiped together by Cleanup().
    //TempRoot is configurable via Settings > Storage & Paths; default is ~/.VidyaGod/TEMP.
    std::filesystem::path TempRoot = std::filesystem::path(QDir::homePath().toStdString()) / ".VidyaGod" / "TEMP";
    if (GlobalConfigJSON.contains("Settings") && GlobalConfigJSON["Settings"].is_object())
    {
        const auto &S = GlobalConfigJSON["Settings"];
        if (S.contains("Paths") && S["Paths"].is_object()
            && S["Paths"].contains("TempRoot") && S["Paths"]["TempRoot"].is_string()
            && !std::string(S["Paths"]["TempRoot"]).empty())
            TempRoot = std::filesystem::path(std::string(S["Paths"]["TempRoot"]));
    }
    ContainerParams.TempPath                            = TempRoot / ContainerParams.PackageUID;
    LogOut("ContainerWrapper::DeriveContainerParams", "TempPath: " + ContainerParams.TempPath.string());

    //Installed-runner separate mount: the build mounts read-only at TempPath/RUNNER, exposed as %RunnerMount%.
    //(Unified runners share the game RUNTIME — handled by folding instead.)
    if (ContainerParams.RunnerShipsBuild && !ContainerParams.UnifiedRuntime)
        ContainerParams.RunnerMountPath = ContainerParams.TempPath / "RUNNER";

    //The runtime mount base; the actual launch prefix is RuntimeBasePath/<PREFIX_SUBPATH> (empty for umu/wine
    //→ prefix == base; "pfx" for Proton → STEAM_COMPAT_DATA_PATH=RuntimeBasePath, pfx==RuntimePath).
    ContainerParams.RuntimeBasePath                     = ContainerParams.TempPath / "RUNTIME";
    ContainerParams.RuntimePath                         = PrefixSubpath.empty() ? ContainerParams.RuntimeBasePath
                                                                                : ContainerParams.RuntimeBasePath / PrefixSubpath;
    LogOut("ContainerWrapper::DeriveContainerParams", "RuntimePath: " + ContainerParams.RuntimePath.string());

    ContainerParams.WriteLayerPath                      = ContainerParams.TempPath / "WRITELAYER";
    LogOut("ContainerWrapper::DeriveContainerParams", "WriteLayerPath: " + ContainerParams.WriteLayerPath.string());

    ContainerParams.DefaultDataPath                     = ContainerParams.TempPath / "DEFAULTDATA";
    LogOut("ContainerWrapper::DeriveContainerParams", "DefaultDataPath: " + ContainerParams.DefaultDataPath.string());

    //Durable persist store stays inside the package so saves travel with it.
    ContainerParams.UserDataPath                        = ContainerParams.PackagePath / "USERDATA";
    LogOut("ContainerWrapper::DeriveContainerParams", "UserDataPath: " + ContainerParams.UserDataPath.string());

    //Persistence is no longer a top-level object — it is derived from PersistDir/PersistFile/
    //RegPersist subcomponents in DerivePersistence (called after the Recipe is built).

    // Wine-specific: prefix lives under TEMP/DEFPREFIX, programs under drive_c/PackageUID
    // Other runners: ProgramPath is the root of the RUNTIME mount
    if (ContainerParams.RunnerTypeEnum == RunnerType::Wine)
    {
        //Game files sit at drive_c/PackageUID so Wine sees them as C:\PackageUID.
        ContainerParams.ProgramPath                     = ContainerParams.RuntimePath / "drive_c" / ContainerParams.PackageUID;
        //Def-prefix base + the prefix Wine actually uses (base/<PREFIX_SUBPATH>); mirrors the runtime layout.
        //Installed runners use their one-time read-only DEFPREFIX artifact (generated at install); other wine
        //runners build it per-launch under TempPath/DEFPREFIX (InitializeDefPrefix).
        ContainerParams.DefPrefixBasePath               = ContainerParams.RunnerShipsBuild
            ? std::filesystem::path(RunnerWrapper::DefPrefixArtifact(ContainerParams.RunnerPackagePath.string(), ContainerParams.RunnerVariantID))
            : ContainerParams.TempPath / "DEFPREFIX";
        ContainerParams.DefPrefixPath                   = PrefixSubpath.empty() ? ContainerParams.DefPrefixBasePath
                                                                                : ContainerParams.DefPrefixBasePath / PrefixSubpath;
        ContainerParams.WindowsProgramPath              = "C:\\" + ContainerParams.PackageUID;
        //Double-backslash variant for use in .reg file string values.
        ContainerParams.WindowsProgramPathDoubleBackSlash = "C:\\\\" + ContainerParams.PackageUID;
    }
    else
    {
        ContainerParams.ProgramPath                     = ContainerParams.RuntimePath;
    }
    LogOut("ContainerWrapper::DeriveContainerParams", "ProgramPath: " + ContainerParams.ProgramPath.string());
    LogOut("ContainerWrapper::DeriveContainerParams", "DefPrefixPath: " + ContainerParams.DefPrefixPath.string());
    LogOut("ContainerWrapper::DeriveContainerParams", "WindowsProgramPath: " + ContainerParams.WindowsProgramPath);
    LogOut("ContainerWrapper::DeriveContainerParams", "WindowsProgramPathDoubleBackSlash: " + ContainerParams.WindowsProgramPathDoubleBackSlash);

    //VariantID (and its Endpoints) are already resolved in DecideComponent.
    //Exe/args/workdir resolution is deferred to ResolveExecutableDefinition().
    //WorkDir defaults to ProgramPath; overridden by ResolveExecutableDefinition once SubComponentsArray is built.
    {
        ContainerParams.WorkDirPathComplete             = ContainerParams.ProgramPath;
    }
    LogOut("ContainerWrapper::DeriveContainerParams", "Completed ContainerParams!");
    return true;
}

//Returns a flat string→string map of every ContainerParams path/value field,
//keyed by the %VARIABLE% token name used in MANIFEST JSON strings and runner ENV values.
//Called by StringVariableSubstitution and at subcomponent collection time.
std::map<std::string, std::string> ContainerParams::GetVariablesMap()
{
    std::map<std::string, std::string> VariablesMap;
    VariablesMap["PackagePath"] = this->PackagePath;
    VariablesMap["PackageName"] = this->PackageName;
    VariablesMap["PackageUID"] = this->PackageUID;
    VariablesMap["GameName"] = this->GameName;
    VariablesMap["UMUID"] = this->UMUID;
    VariablesMap["ScreenWidth"] = this->ScreenWidth;
    VariablesMap["ScreenHeight"] = this->ScreenHeight;
    VariablesMap["RuntimePath"] = this->RuntimePath;
    VariablesMap["RuntimeBasePath"] = this->RuntimeBasePath;
    VariablesMap["RunnerRuntimePath"] = this->RunnerRuntimePath;
    //The runner build's mount (installed-runner model); unified runners run from inside the game RUNTIME.
    VariablesMap["RunnerMount"] = this->UnifiedRuntime ? this->RuntimePath.string() : this->RunnerMountPath.string();
    //Phase-context prefix: %WinePrefix% is the prefix dir for the current phase (def-prefix at init, runtime
    //at launch); %PrefixBase% its base (Proton's STEAM_COMPAT_DATA_PATH). Default to the runtime when unset.
    VariablesMap["WinePrefix"] = this->ActivePrefix.empty() ? this->RuntimePath : this->ActivePrefix;
    VariablesMap["PrefixBase"] = this->ActivePrefixBase.empty() ? this->RuntimeBasePath : this->ActivePrefixBase;
    VariablesMap["WriteLayerPath"] = this->WriteLayerPath;
    VariablesMap["UserDataPath"] = this->UserDataPath;
    VariablesMap["TempPath"] = this->TempPath;
    VariablesMap["ProgramPath"] = this->ProgramPath;
    VariablesMap["DefPrefixPath"] = this->DefPrefixPath;
    VariablesMap["DefaultData"] = this->DefaultDataPath;
    VariablesMap["ExePathRelative"] = this->ExePathRelative;
    VariablesMap["ExePathComplete"] = this->ExePathComplete;
    VariablesMap["ExePathInPrefix"] = this->ExePathInPrefix;
    VariablesMap["WindowsProgramPath"] = this->WindowsProgramPath;
    VariablesMap["WindowsExePathComplete"] = this->WindowsExePathComplete;
    VariablesMap["WindowsProgramPathDoubleBackSlash"] = this->WindowsProgramPathDoubleBackSlash;
    VariablesMap["WorkDirPathRelative"] = this->WorkDirPathRelative;
    VariablesMap["WorkDirPathComplete"] = this->WorkDirPathComplete;
    //Custom variables are appended last; they can shadow built-in names if needed.
    for (auto &[Key, Value] : this->CustomVariables)
        VariablesMap[Key] = Value;
    return VariablesMap;
}

//Replaces all %KEY% tokens in SourceString with values from VariablesMap.
//Scans left-to-right looking for paired '%' delimiters; an unmatched '%' stops
//Translates a display-layer value into its raw storage format based on VARTYPE.
//Called after Layer-1 substitution so the value is already a concrete string.
//  dword  : decimal integer → "dword:XXXXXXXX" (8-digit hex, 32-bit unsigned)
//  qword  : decimal integer → "hex(b):XX,XX,...,XX" (8 bytes little-endian)
//  bool   : "1"/"true"/"yes" → "dword:00000001"; anything else → "dword:00000000"
//  string / number / options / unknown → returned unchanged
std::string ContainerWrapper::TranslateCustomVarValue(const std::string &Value, const std::string &VarType)
{
    if (VarType == "dword")
    {
        try {
            uint32_t n = static_cast<uint32_t>(std::stoul(Value));
            std::ostringstream oss;
            oss << "dword:" << std::hex << std::setw(8) << std::setfill('0') << n;
            return oss.str();
        } catch (...) {
            LogWarn("ContainerWrapper::TranslateCustomVarValue", "Could not parse dword value: '" + Value + "', leaving unchanged.");
            return Value;
        }
    }
    else if (VarType == "qword")
    {
        try {
            uint64_t n = std::stoull(Value);
            std::ostringstream oss;
            oss << "hex(b):";
            for (int i = 0; i < 8; i++) {
                if (i > 0) oss << ",";
                oss << std::hex << std::setw(2) << std::setfill('0') << ((n >> (8 * i)) & 0xFF);
            }
            return oss.str();
        } catch (...) {
            LogWarn("ContainerWrapper::TranslateCustomVarValue", "Could not parse qword value: '" + Value + "', leaving unchanged.");
            return Value;
        }
    }
    else if (VarType == "bool")
    {
        std::string lower = Value;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        bool isTrue = (lower == "1" || lower == "true" || lower == "yes");
        return isTrue ? "dword:00000001" : "dword:00000000";
    }
    // string, number, options — no translation
    return Value;
}

//further substitution and logs a warning (the remainder is appended unchanged).
//Unknown keys are left as %KEY% and logged as warnings.
//Returns true if at least one replacement was made.
bool ContainerWrapper::StringVariableSubstitution(
    std::string &SourceString,
    const std::map<std::string, std::string>& VariablesMap)
{
    LogOut("ContainerWrapper::StringVariableSubstitution", "Starting substitution.");
    LogOut("ContainerWrapper::StringVariableSubstitution", "Original string: \"" + SourceString + "\"");

    std::string result;
    bool replaced = false;
    size_t pos = 0;

    while (pos < SourceString.size())
    {
        size_t start = SourceString.find('%', pos);

        //No more '%' characters — append the rest of the string unchanged.
        if (start == std::string::npos)
        {
            result += SourceString.substr(pos);
            break;
        }

        size_t end = SourceString.find('%', start + 1);

        //Unmatched opening '%' — stop substitution, preserve remainder.
        if (end == std::string::npos)
        {
            LogWarn("ContainerWrapper::StringVariableSubstitution", "Unmatched '%' at position " + std::to_string(start) + ". Aborting further substitution.");
            result += SourceString.substr(pos);
            break;
        }

        // Append text before variable
        result += SourceString.substr(pos, start - pos);

        std::string key = SourceString.substr(start + 1, end - start - 1);

        LogOut("ContainerWrapper::StringVariableSubstitution", "Found variable: %" + key + "%");

        auto it = VariablesMap.find(key);
        if (it != VariablesMap.end())
        {
            LogOut("ContainerWrapper::StringVariableSubstitution", "Replacing with: \"" + it->second + "\"");
            result += it->second;
            replaced = true;
        }
        else
        {
            //Leave the token in place so the caller can diagnose the missing variable.
            LogWarn("ContainerWrapper::StringVariableSubstitution", "Variable not found in map. Leaving unchanged.");
            result += "%" + key + "%";
        }

        pos = end + 1;
    }

    LogOut("ContainerWrapper::StringVariableSubstitution", "Final string: \"" + result + "\"");
    LogOut("ContainerWrapper::StringVariableSubstitution", std::string("Substitution performed: ") + (replaced ? "YES" : "NO"));

    SourceString = std::move(result);
    return replaced;
}

//Synchronously runs Program with Arguments in ProcessEnvironment.
//Waits indefinitely (timeout = -1) so long-running installers and wine boots don't time out.
//Dumps stderr and stdout after completion so output appears in the application log.
//Returns the process exit code, or -1 if the process failed to start or crashed.
int ContainerWrapper::RunCommand(std::string Program, std::vector<std::string> Arguments, QProcessEnvironment ProcessEnvironment, const std::string &WorkingDirectory)
{
    auto toQStringList = [](const std::vector<std::string>& v)
    {
        QStringList l; for (auto& s : v) l << QString::fromStdString(s); return l;
    };

    QStringList ArgumentsQList = toQStringList(Arguments);
    LogOut("ContainerWrapper::RunCommand", "Running program " + Program + " with arguments: " + ArgumentsQList.join(" ").toStdString());

    QProcess Process;
    Process.setProcessEnvironment(ProcessEnvironment);
    if (!WorkingDirectory.empty()) Process.setWorkingDirectory(QString::fromStdString(WorkingDirectory));
    Process.setProgram(QString::fromStdString(Program));
    Process.setArguments(ArgumentsQList);

    Process.start();

    if (!Process.waitForFinished(-1))
    {
        return -1; //Failed to start or crashed
    }

    std::cout << Process.readAllStandardError().toStdString();
    std::cout << Process.readAllStandardOutput().toStdString();

    if (Process.exitStatus() == QProcess::NormalExit)
    {
        return Process.exitCode();
    }
    else
    {
        return -1; //Crashed
    }
}

//=====================================================================================================================================================================
//                                                                    REGISTRYWRAPPER CLASS
//=====================================================================================================================================================================

//Returns true if SubComponentsArray contains at least one RegEdit whose OVERRIDE flag == WantOverride.
static bool HasRegEdits(const struct ContainerParams &ContainerParams, bool WantOverride)
{
    for (const auto &Sub : ContainerParams.SubComponentsArray)
        if (Sub.value("TYPE", std::string()) == "RegEdit" && Sub.value("OVERRIDE", false) == WantOverride)
            return true;
    return false;
}

//Builds the DEFAULTDATA layer: a dedicated, regenerated-each-launch read-only layer that holds every
//package-encoded BASE (non-OVERRIDE) edit. It mounts between the component layers and the WRITELAYER
//(see BuildLayerSpec), so its edits override the package's own content but the user's persisted writes
//(WRITELAYER) shadow them in turn — PortableApps-style "factory defaults under live user data".
//
//Contents:
//  - Base FileEdits  → files at their root-relative paths under DEFAULTDATA. Applied for ANY runner type
//                      (Wine: drive_c/…; emulator/native: the runtime root).
//  - Base RegEdits   → full user.reg/system.reg/userdef.reg = DEFPREFIX hives + edits (shadow DEFPREFIX's;
//                      vidyagodfs COWs from this highest layer when Wine writes the registry). WINE-ONLY.
//  - Persisted RegKeyPersist subtrees merged in AFTER base edits, so the user's saved key state wins. WINE-ONLY.
//
//DEFPREFIX itself is NEVER mutated (read-only source for the hives) — pristine for both runner models.
//OVERRIDE edits are NOT handled here; they go post-mount straight to the runtime (COW → WRITELAYER).
//Creates nothing when there are no base edits, so edit-less packages get no (empty) DEFAULTDATA layer.
//MUST BE RUN AFTER VARIABLE SUBSTITUTION (BuildSubComponentsArray) and after DEFPREFIX is provisioned.
bool ContainerWrapper::BuildDefaultData(struct ContainerParams &ContainerParams)
{
    const bool WineMode = (ContainerParams.RunnerTypeEnum == RunnerType::Wine);

    bool HaveBaseFileEdits = false;
    for (const auto &S : ContainerParams.SubComponentsArray)
        if (S.value("TYPE", std::string()) == "FileEdit" && S.value("OVERRIDE", false) == false)
            { HaveBaseFileEdits = true; break; }

    //Registry is Wine-only; emulator/native runners have no hives.
    const bool HaveBaseReg = WineMode && HasRegEdits(ContainerParams, /*WantOverride=*/false);
    const std::filesystem::path RegKeyStore = ContainerParams.UserDataPath / "__REGKEYS__";
    const bool HavePersistKeys = WineMode && !ContainerParams.PersistAll && !ContainerParams.PersistRegKeys.empty()
                                 && std::filesystem::exists(RegKeyStore);

    if (!HaveBaseFileEdits && !HaveBaseReg && !HavePersistKeys) return true; // nothing to materialise

    std::error_code Ec;
    std::filesystem::create_directories(ContainerParams.DefaultDataPath, Ec);

    //Base FileEdits → DEFAULTDATA (at their root-relative paths).
    if (HaveBaseFileEdits)
        ContainerWrapper::ProcessFileEdits(ContainerParams, /*OverridePass=*/false, ContainerParams.DefaultDataPath);

    //Base RegEdits + persisted reg-key subtrees → DEFAULTDATA hives, built from DEFPREFIX (never mutating it).
    if (HaveBaseReg || HavePersistKeys)
    {
        //No wineserver quiesce needed: InitializeDefPrefix / the installed artifact leave DEFPREFIX quiescent,
        //and we only READ its hives here — the edited copies are written into DEFAULTDATA.
        RegistryWrapper RW;
        RW.LoadPrefix(ContainerParams.DefPrefixPath);
        if (HaveBaseReg)
            RW.ApplyRegEdits(ContainerParams.SubComponentsArray, /*WantOverride=*/false);
        if (HavePersistKeys)
        {
            RegistryWrapper Durable;
            Durable.LoadPrefix(RegKeyStore);
            for (const std::string &RegPath : ContainerParams.PersistRegKeys)
                if (RW.MergeKeyFrom(Durable, RegPath))
                    LogOut("ContainerWrapper::BuildDefaultData", "Seeded persisted key " + RegPath);
        }
        if (!RW.SavePrefix(ContainerParams.DefaultDataPath))
            LogWarn("ContainerWrapper::BuildDefaultData", "Failed to write DEFAULTDATA hives.");
    }
    LogSucc("ContainerWrapper::BuildDefaultData", "DEFAULTDATA layer built at " + ContainerParams.DefaultDataPath.string());
    return true;
}

//Applies OVERRIDE:true RegEdit subcomponents into the mounted runtime hives, post-MountVFS. Loading
//RuntimePath reads the effective union view (DEFPREFIX shadowed by any persisted seed); saving it
//back COWs the whole hive file into the RW WRITELAYER (file-level union shadowing), so the override
//values win over DEFPREFIX and prior COW state — matching the former `reg import` outcome. No wine
//runs on RuntimePath before launch, so no wineserver quiesce is needed here.
bool ContainerWrapper::ApplyOverrideRegEdits(struct ContainerParams &ContainerParams)
{
    if (!HasRegEdits(ContainerParams, /*WantOverride=*/true)) return true; // no overrides → leave hives untouched

    LogOut("ContainerWrapper::ApplyOverrideRegEdits", "Applying OVERRIDE RegEdits to mounted runtime hives.");

    RegistryWrapper RW;
    RW.LoadPrefix(ContainerParams.RuntimePath);
    RW.ApplyRegEdits(ContainerParams.SubComponentsArray, /*WantOverride=*/true);
    if (!RW.SavePrefix(ContainerParams.RuntimePath))
    {
        LogErr("ContainerWrapper::ApplyOverrideRegEdits", "Failed to write runtime hives.");
        return false;
    }
    LogSucc("ContainerWrapper::ApplyOverrideRegEdits", "OVERRIDE RegEdits applied to runtime.");
    return true;
}

//=====================================================================================================================================================================
//                                                                          VFSWRAPPER CLASS
//=====================================================================================================================================================================

//Initializes the Wine prefix at DefPrefixPath by running `wineboot` via the runner.
//Exclusive for wine / proton runners.
//Must be made to work with any runner, not just UMU...
//
//After a successful wineboot, DefPrefixPath becomes the base (lowest-priority) read-only layer of the
//vidyagodfs spec (see BuildLayerSpec), so the prefix's drive_c structure shows at the runtime root.
//The prefix must be initialized before any game layers are stacked on top.
bool ContainerWrapper::InitializeDefPrefix(struct ContainerParams &ContainerParams)
{
    //Build the base wine prefix by running `wineboot` through the runner. Fully manifest-driven: the umu vs
    //proton difference is just the runner's EXECUTABLE/ARGS/ENV plus the prefix layout. The phase-context
    //prefix vars (%WinePrefix% / %PrefixBase%) bind to the DEF-prefix here so a runner ENV like
    //WINEPREFIX=%WinePrefix% (umu) or STEAM_COMPAT_DATA_PATH=%PrefixBase% (proton) points at what we build.
    ContainerParams.ActivePrefix     = ContainerParams.DefPrefixPath;
    ContainerParams.ActivePrefixBase = ContainerParams.DefPrefixBasePath;
    LogOut("ContainerWrapper::InitializeDefPrefix", "Initialising DefPrefix, path: " + ContainerParams.DefPrefixPath.string());
    std::filesystem::create_directories(ContainerParams.DefPrefixBasePath);
    std::filesystem::create_directories(ContainerParams.DefPrefixPath);

    //EXECUTABLE may reference %RunnerRuntimePath% (e.g. the Proton build's proton script).
    std::string Program = ContainerParams.RunnerExecutable;
    ContainerWrapper::StringVariableSubstitution(Program, ContainerParams.GetVariablesMap());

    QProcessEnvironment RunProcessEnvironment = SystemToolEnv();                 // system runner, not AppImage libs
    QProcess * RunProcess = new QProcess;
    RunProcess->setProgram(QString::fromStdString(Program));

    //Args: the runner's ARGS (the launcher verb, e.g. proton's "waitforexitandrun"; empty for umu) then wineboot.
    QStringList Arguments;
    for (std::string Arg : ContainerParams.RunnerArgs)
    {
        ContainerWrapper::StringVariableSubstitution(Arg, ContainerParams.GetVariablesMap());
        Arguments.append(QString::fromStdString(Arg));
    }
    Arguments.append("wineboot");
    RunProcess->setArguments(Arguments);

    //Env: runner REMOVE_ENV then the runner ENV block (with %var% expansion), exactly as at launch.
    for (const std::string &Key : ContainerParams.RunnerRemoveEnv)
        RunProcessEnvironment.remove(QString::fromStdString(Key));
    for (auto &[Key, Value] : ContainerParams.RunnerEnv.items())
    {
        std::string ExpandedValue = Value.get<std::string>();
        ContainerWrapper::StringVariableSubstitution(ExpandedValue, ContainerParams.GetVariablesMap());
        RunProcessEnvironment.insert(QString::fromStdString(Key), QString::fromStdString(ExpandedValue));
    }

    LogOut("ContainerWrapper::InitializeDefPrefix", "wineboot: " + Program + " " + Arguments.join(' ').toStdString());
    RunProcess->setProcessEnvironment(RunProcessEnvironment);
    RunProcess->start();
    RunProcess->waitForFinished(-1);

    std::cout << RunProcess->readAllStandardError().toStdString() << std::endl;
    std::cout << RunProcess->readAllStandardOutput().toStdString() << std::endl;

    if (RunProcess->exitCode() == 0)
    {
        delete RunProcess;
        //The prefix is added to the layer-spec as the base (root) layer by BuildLayerSpec.
        LogSucc("ContainerWrapper::InitializeDefPrefix", "Prefix initialisation successful!");
        return true;
    }
    else
    {
        delete RunProcess;
        LogErr("ContainerWrapper::InitializeDefPrefix", "Prefix initialisation failed!");
        return false;
    }
}

//Locates the vidyagodfs helper: next to the running binary first (via /proc/self/exe, which works
//in headless mode before any QApplication exists), else on PATH.
static std::string VidyagodfsPath()
{
    std::error_code ec;
    std::filesystem::path Self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec)
    {
        std::filesystem::path CoLocated = Self.parent_path() / "vidyagodfs";
        if (std::filesystem::exists(CoLocated, ec)) return CoLocated.string();
    }
    return "vidyagodfs";
}

//Returns the name of the first COMPRESSED (non-STORE) entry in the zip at Path, or "" if every entry
//is STORED (or the archive can't be parsed). Self-contained (no libzip): walks the zip central
//directory (ZIP64-aware) and checks each record's compression-method field (offset 10; 0 == STORE).
//Used to block compressed zip layers before mounting — vidyagodfs serves zip layers zero-copy, which
//requires STORE.
static std::string ZipFirstCompressedEntry(const std::string &Path)
{
    auto rd16 = [](const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); };
    auto rd32 = [](const uint8_t *p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); };
    auto rd64 = [](const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; --i) v = (v << 8) | p[i]; return v; };

    int fd = ::open(Path.c_str(), O_RDONLY);
    if (fd < 0) return "";
    struct stat stt; if (::fstat(fd, &stt) != 0) { ::close(fd); return ""; }
    uint64_t fsize = (uint64_t)stt.st_size;
    auto preadAll = [&](void *b, size_t n, uint64_t off) -> bool {
        uint8_t *p = (uint8_t *)b; size_t d = 0;
        while (d < n) { ssize_t r = ::pread(fd, p + d, n - d, (off_t)(off + d)); if (r <= 0) return false; d += (size_t)r; }
        return true;
    };

    std::string result;
    if (fsize >= 22)
    {
        uint64_t tail = std::min<uint64_t>(fsize, 22 + 65535);
        std::vector<uint8_t> buf(tail);
        if (preadAll(buf.data(), tail, fsize - tail))
        {
            long eocd = -1;
            for (long i = (long)tail - 22; i >= 0; --i) if (rd32(&buf[i]) == 0x06054b50u) { eocd = i; break; }
            if (eocd >= 0)
            {
                const uint8_t *E = &buf[eocd];
                uint64_t total = rd16(E + 10), cdoff = rd32(E + 16), eocdOff = fsize - tail + (uint64_t)eocd;
                if (cdoff == 0xFFFFFFFFu || total == 0xFFFFu) // ZIP64
                {
                    uint8_t loc[20], z64[56];
                    if (eocdOff >= 20 && preadAll(loc, 20, eocdOff - 20) && rd32(loc) == 0x07064b50u)
                    {
                        uint64_t z = rd64(loc + 8);
                        if (preadAll(z64, 56, z) && rd32(z64) == 0x06064b50u) { total = rd64(z64 + 32); cdoff = rd64(z64 + 48); }
                    }
                }
                uint64_t pos = cdoff;
                for (uint64_t i = 0; i < total && result.empty(); ++i)
                {
                    uint8_t rec[46];
                    if (!preadAll(rec, 46, pos) || rd32(rec) != 0x02014b50u) break;
                    uint16_t method = rd16(rec + 10), nl = rd16(rec + 28), el = rd16(rec + 30), cl = rd16(rec + 32);
                    if (method != 0) // not STORE
                    {
                        std::vector<uint8_t> nm(nl);
                        result = (nl && preadAll(nm.data(), nl, pos + 46)) ? std::string((const char *)nm.data(), nl) : "(entry)";
                    }
                    pos += 46u + nl + el + cl;
                }
            }
        }
    }
    ::close(fd);
    return result;
}

//A VFS layer's locators: its expected LOCAL path (PackagePath/PATH, SOURCE.PATH overriding) and its ipfs CID
//(empty if none). The local path is the content's home; the CID is one of (eventually several) remote fallback
//sources to fetch it from. (Future backends — torrent, LAN — read their own SOURCE fields here.)
static void LayerLocator(const nlohmann::ordered_json &Sub, const std::filesystem::path &PackagePath,
                         std::filesystem::path &Local, std::string &Cid)
{
    std::string PathStr = Sub.value("PATH", std::string());
    Cid.clear();
    if (Sub.contains("SOURCE") && Sub["SOURCE"].is_object())
    {
        const auto &Src = Sub["SOURCE"];
        PathStr = Src.value("PATH", PathStr);                                  // SOURCE.PATH overrides the local path
        if (Src.value("TYPE", std::string("path")) == "ipfs") Cid = Src.value("CID", std::string());
    }
    std::filesystem::path P(PathStr);
    Local = P.is_absolute() ? P : (PackagePath / P);
}

//Resolves a File/Dir/Zip layer's content to its LOCAL absolute path (PackagePath/PATH). Content always lives
//in place inside the package's LIBRARY dir — hydrated there by ImportPackage/ImportRunner (or already present
//for a local package). The CID in SOURCE is the permanent identity used to fetch/seed it; this resolution is
//pure and local-only (no cache). A missing path means the package isn't hydrated (EnsureSources blocks launch).
static std::string ResolveLayerSource(const nlohmann::ordered_json &Sub, const std::filesystem::path &PackagePath)
{
    std::filesystem::path Local; std::string Cid;
    LayerLocator(Sub, PackagePath, Local, Cid);
    return Local.string();
}

//Returns the unresolved dependency locators for a built container — drives the portable/standalone
//readiness warning. Iteration 1 verifies every VFS layer's resolved source exists.
//TODO(sharing): also verify the resolved runner + its RUNTIME + cross-package component references, and
//distinguish a fully-bundled (portable) chain from one needing external/global packages.
std::vector<std::string> ContainerWrapper::VerifyDependencies(const struct ContainerParams &ContainerParams)
{
    std::vector<std::string> Missing;
    for (const auto &Sub : ContainerParams.SubComponentsArray)
    {
        const std::string Type = Sub.value("TYPE", std::string());
        if (Type != "VFSZipLayer" && Type != "VFSDirLayer" && Type != "VFSFileLayer") continue;
        std::filesystem::path Local; std::string Cid;
        LayerLocator(Sub, ContainerParams.PackagePath, Local, Cid);
        std::error_code Ec;
        if (std::filesystem::exists(Local, Ec)) continue;        // present locally (highest priority)
        if (!Cid.empty()) continue;                              // fetchable from a backend — not "missing"
        LogErr("ContainerWrapper::VerifyDependencies", "Layer content unavailable (no local file, no source): " + Local.string());
        Missing.push_back(Local.string());
    }
    return Missing;
}

//Pre-flight CHECK (never fetches — safe to call from the GUI thread, e.g. the play() gate). Returns false,
//blocking the launch, only when something can't be satisfied:
//  - a RUNNER build CID isn't cached (runners are fetched ahead of time by ImportRunner — "import the runner"),
//  - the wine runner's DEFPREFIX artifact is missing,
//  - a GAME VFS layer has neither local content NOR a backend (CID) to fetch it from.
//A game layer that is missing locally but HAS a CID is fine here — MaterializeLayers fetches it on the worker.
bool ContainerWrapper::EnsureSources(struct ContainerParams &ContainerParams)
{
    bool Ok = true;
    //Runner build layers must be hydrated locally in the runner's library dir (ImportRunner fetches them ahead
    //of time — "import the runner"). Missing → the runner isn't installed; block the launch.
    for (const auto &Sub : ContainerParams.RunnerLayers)
    {
        const std::string Type = Sub.value("TYPE", std::string());
        if (Type != "VFSZipLayer" && Type != "VFSDirLayer" && Type != "VFSFileLayer") continue;
        std::filesystem::path Local; std::string Cid;
        LayerLocator(Sub, ContainerParams.RunnerPackagePath, Local, Cid);
        std::error_code Ec;
        if (std::filesystem::exists(Local, Ec))
        {
            //IPFS fetch (FetchToPath) drops the executable bit, so a loose runner binary (e.g. a bundled
            //AppImage as a VFSFileLayer) lands as 0644 and can't be exec'd once mounted. Zip/Dir layers keep
            //their internal entry modes, so this only matters for a single-file runner build. Restore +x.
            if (Type == "VFSFileLayer")
                std::filesystem::permissions(Local, std::filesystem::perms::owner_exec
                    | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
                    std::filesystem::perm_options::add, Ec);
            continue;
        }
        Ok = false;
        LogErr("ContainerWrapper::EnsureSources", "Runner not imported: missing build " + Local.string());
    }

    for (const auto &Sub : ContainerParams.SubComponentsArray)                   // game VFS layers: local OR fetchable
    {
        const std::string Type = Sub.value("TYPE", std::string());
        if (Type != "VFSZipLayer" && Type != "VFSDirLayer" && Type != "VFSFileLayer") continue;
        std::filesystem::path Local; std::string Cid;
        LayerLocator(Sub, ContainerParams.PackagePath, Local, Cid);
        std::error_code Ec;
        if (std::filesystem::exists(Local, Ec) || !Cid.empty()) continue;        // present, or a backend can provide it
        Ok = false;
        LogErr("ContainerWrapper::EnsureSources", "Missing layer content (no local file, no source): " + Local.string());
    }

    //An installed wine runner must have its one-time DEFPREFIX artifact (generated at import).
    if (ContainerParams.RunnerShipsBuild && ContainerParams.RunnerTypeEnum == RunnerType::Wine)
    {
        std::error_code Ec;
        if (ContainerParams.DefPrefixPath.empty() || !std::filesystem::exists(ContainerParams.DefPrefixPath, Ec))
        {
            Ok = false;
            LogErr("ContainerWrapper::EnsureSources", "Runner not imported: missing DEFPREFIX artifact " + ContainerParams.DefPrefixPath.string());
        }
    }

    if (!Ok) LogErr("ContainerWrapper::EnsureSources", "Required dependencies are unavailable — launch blocked.");
    return Ok;
}

//Hierarchical fallback MATERIALIZER (worker thread only — may block on a download). For each GAME VFS layer
//whose local content is missing, fetches it from a backend (IPFS now) straight to its expected local PATH, so
//the package self-heals and subsequent launches are fully local. Runner builds are not materialized here (they
//stay in the shared cache, fetched at runner import). Returns false if a required layer could not be fetched.
bool ContainerWrapper::MaterializeLayers(struct ContainerParams &ContainerParams)
{
    bool Ok = true;
    for (const auto &Sub : ContainerParams.SubComponentsArray)
    {
        const std::string Type = Sub.value("TYPE", std::string());
        if (Type != "VFSZipLayer" && Type != "VFSDirLayer" && Type != "VFSFileLayer") continue;
        std::filesystem::path Local; std::string Cid;
        LayerLocator(Sub, ContainerParams.PackagePath, Local, Cid);
        std::error_code Ec;
        if (std::filesystem::exists(Local, Ec)) continue;                        // already local — highest priority
        if (Cid.empty()) continue;                                              // no backend (flagged by EnsureSources)
        std::string Err;                                                        // fetch from IPFS to the expected path
        if (IpfsWrapper::FetchToPath(Cid, Local.string(), &Err).empty())
        {
            Ok = false;
            LogErr("ContainerWrapper::MaterializeLayers", "Could not fetch layer CID " + Cid + " -> " + Local.string() + " (" + Err + ")");
        }
    }
    return Ok;
}

//Builds the vidyagodfs JSON layer-spec from the resolved container: the DEFPREFIX base (Wine), every
//VFS subcomponent rooted at its TARGET (logically — no staging dirs), the PERSIST dirs as RW
//passthrough layers, and the writable top branch. Array order is union priority (lowest first).
nlohmann::ordered_json ContainerWrapper::BuildLayerSpec(struct ContainerParams &ContainerParams, bool WineMode)
{
    nlohmann::ordered_json Spec;
    Spec["mountpoint"] = ContainerParams.RuntimePath.string();
    Spec["uid"] = 1000;
    Spec["gid"] = 1000;
    Spec["readonly"] = ContainerParams.ReadOnlyVFS;
    if (ContainerParams.ReadOnlyVFS)
        Spec["writelayer"] = nullptr;
    else
    {
        const std::filesystem::path RW = ContainerParams.PersistAll ? ContainerParams.UserDataPath : ContainerParams.WriteLayerPath;
        std::filesystem::create_directories(RW);
        Spec["writelayer"] = RW.string();
    }

    nlohmann::ordered_json Layers = nlohmann::ordered_json::array();

    //DEFPREFIX base (Wine) — lowest priority, at the runtime root.
    if (WineMode && !ContainerParams.DefPrefixPath.empty())
        Layers.push_back({{"type", "dir"}, {"source", ContainerParams.DefPrefixPath.string()}, {"target", ""}, {"rw", false}});

    //In Wine mode game content lives under drive_c/PackageUID so Wine's C: maps correctly.
    const std::string WineRoot = WineMode ? ("drive_c/" + ContainerParams.PackageUID) : std::string();

    for (auto &Sub : ContainerParams.SubComponentsArray)
    {
        std::string Type = Sub.value("TYPE", std::string());
        std::string LType;
        if      (Type == "VFSZipLayer")  LType = "zip";
        else if (Type == "VFSDirLayer")  LType = "dir";
        else if (Type == "VFSFileLayer") LType = "file";
        else continue;

        std::filesystem::path Source = ResolveLayerSource(Sub, ContainerParams.PackagePath);
        std::string Target = WineRoot;
        if (Sub.contains("TARGET") && Sub["TARGET"].is_string() && !std::string(Sub["TARGET"]).empty())
        {
            std::string T = Sub["TARGET"];
            Target = Target.empty() ? T : (Target + "/" + T);
        }
        Layers.push_back({{"type", LType}, {"source", Source.string()}, {"target", Target},
                          {"submounts", Sub.value("SUBMOUNTS", nlohmann::ordered_json::array())}, {"rw", false}});
    }

    //DEFAULTDATA — package-encoded base (non-OVERRIDE) Reg/File edits, at the runtime root. Above the
    //component layers (so the edits override the package's own content) but below the writelayer (so the
    //user's persisted changes win). Built by BuildDefaultData for any runner; absent for edit-less packages.
    if (!ContainerParams.DefaultDataPath.empty()
        && std::filesystem::exists(ContainerParams.DefaultDataPath))
        Layers.push_back({{"type", "dir"}, {"source", ContainerParams.DefaultDataPath.string()}, {"target", ""}, {"rw", false}});

    //PERSIST dirs → RW passthrough layers (highest priority, appended last). Root-relative target,
    //matching the old MountPersistDirs bind of UserDataPath/<rel> onto RuntimePath/<rel>.
    if (!ContainerParams.PersistAll)
        for (const std::string &Rel : ContainerParams.PersistDirs)
        {
            std::filesystem::path Src = ContainerParams.UserDataPath / Rel;
            std::filesystem::create_directories(Src);
            Layers.push_back({{"type", "dir"}, {"source", Src.string()}, {"target", Rel}, {"rw", true}});
        }

    Spec["layers"] = Layers;
    return Spec;
}

//Writes the layer-spec and mounts it by spawning vidyagodfs onto RuntimePath, replacing the former
//unionfs+fuse-zip+bindfs+staging pipeline with a single FUSE mount. The helper daemonizes once the
//mount is live, so the spawn returns; we then poll mountinfo to confirm readiness before proceeding.
//RuntimePath registers for the non-lazy save-safe unmount whenever durable data is reachable through
//the mount (PersistAll writelayer or any RW passthrough persist dir), else the lazy path.
bool ContainerWrapper::MountVFS(struct ContainerParams &ContainerParams)
{
    const bool WineMode = (ContainerParams.RunnerTypeEnum == RunnerType::Wine);
    nlohmann::ordered_json Spec = BuildLayerSpec(ContainerParams, WineMode);

    //Zip layers are served zero-copy, which requires STORE (uncompressed). Block any compressed
    //archive up front with a re-zip dialog (GUI) / log (headless), before mounting.
    for (const auto &L : Spec.value("layers", nlohmann::ordered_json::array()))
    {
        if (L.value("type", std::string()) != "zip") continue;
        std::string Src = L.value("source", std::string());
        std::string Bad = ZipFirstCompressedEntry(Src);
        if (Bad.empty()) continue;

        LogErr("ContainerWrapper::MountVFS", "Compressed zip layer (not STORE): " + Src + " — entry '" + Bad + "'");
        if (qApp)
        {
            QString T = "Compressed zip layer — re-zip as STORE";
            QString M = QString::fromStdString(
                "This package contains a COMPRESSED zip layer, which is no longer supported.\n\n"
                + Src + "\n\nfirst compressed entry:  " + Bad + "\n\n"
                "VidyaGod serves zip layers zero-copy, which requires STORED (uncompressed) archives. "
                "Re-create the archive with STORE:\n\n    zip -0 -r <archive.zip> <files>\n\n"
                "or run tools/store-zips.sh on your library to convert everything at once.");
            QMetaObject::invokeMethod(qApp, [T, M]() { QMessageBox::critical(nullptr, T, M); }, Qt::QueuedConnection);
        }
        return false;
    }

    std::filesystem::path SpecPath = ContainerParams.TempPath / "vidyagodfs.spec.json";
    if (!ContainerWrapper::SpawnVidyagodfs(Spec, ContainerParams.RuntimePath, SpecPath))
    {
        LogErr("ContainerWrapper::MountVFS", "vidyagodfs mount did not appear at " + ContainerParams.RuntimePath.string());
        return false;
    }

    if (ContainerParams.PersistAll || !ContainerParams.PersistDirs.empty())
        ContainerParams.CleanupPersistPaths.push_back(ContainerParams.RuntimePath);
    else
        ContainerParams.CleanupUnmountPaths.push_back(ContainerParams.RuntimePath);
    LogSucc("ContainerWrapper::MountVFS", "Successfully mounted VFS.");
    return true;
}

//Low-level vidyagodfs mount: write Spec to SpecPath, spawn the helper onto Mountpoint, poll until live.
bool ContainerWrapper::SpawnVidyagodfs(const nlohmann::ordered_json &Spec, const std::filesystem::path &Mountpoint,
                                       const std::filesystem::path &SpecPath)
{
    std::filesystem::create_directories(Mountpoint);
    {
        std::ofstream Out(SpecPath);
        if (!Out) { LogErr("ContainerWrapper::SpawnVidyagodfs", "Cannot write spec " + SpecPath.string()); return false; }
        Out << Spec.dump(2);
    }
    const std::string Helper = VidyagodfsPath();
    LogOut("ContainerWrapper::SpawnVidyagodfs", "Mounting " + Mountpoint.string() + " via " + Helper);
    //--watch-pid: the helper's watchdog auto-unmounts if this process dies, instead of leaking a mount.
    int result = ContainerWrapper::RunCommand(Helper, {SpecPath, Mountpoint,
                                                       "--watch-pid", std::to_string(getpid()), "-o", "auto_cache"});
    LogOut("ContainerWrapper::SpawnVidyagodfs", "vidyagodfs spawn exit: " + std::to_string(result));
    for (int i = 0; i < 50; ++i)
    {
        if (!ContainerWrapper::MountpointsUnder(Mountpoint).empty()) return true;
        QThread::msleep(100);
    }
    return false;
}

//Mounts the selected runner's build (its VFSZipLayer subcomponents) read-only at RunnerMountPath — the
//separate-mount, installed-runner model. The zips are mounted (never extracted) at the mount root by their
//resolved (cached) source. Registers the mount for lazy cleanup. No-op when the runner ships no build or is
//unified into the game RUNTIME.
bool ContainerWrapper::MountRunnerBuild(struct ContainerParams &ContainerParams)
{
    if (!ContainerParams.RunnerShipsBuild || ContainerParams.UnifiedRuntime) return true;
    if (ContainerParams.RunnerLayers.empty()) return true;

    nlohmann::ordered_json Spec;
    Spec["mountpoint"] = ContainerParams.RunnerMountPath.string();
    Spec["uid"] = 1000; Spec["gid"] = 1000;
    Spec["readonly"] = true; Spec["writelayer"] = nullptr;
    nlohmann::ordered_json Layers = nlohmann::ordered_json::array();
    for (auto &Sub : ContainerParams.RunnerLayers)
    {
        const std::string Type = Sub.value("TYPE", std::string());
        std::string LType = (Type == "VFSZipLayer") ? "zip" : (Type == "VFSDirLayer") ? "dir" : (Type == "VFSFileLayer") ? "file" : "";
        if (LType.empty()) continue;
        std::string Target = Sub.value("TARGET", std::string());            // beside the runner-mount root
        Layers.push_back({{"type", LType}, {"source", ResolveLayerSource(Sub, ContainerParams.RunnerPackagePath)},
                          {"target", Target}, {"submounts", Sub.value("SUBMOUNTS", nlohmann::ordered_json::array())}, {"rw", false}});
    }
    Spec["layers"] = Layers;

    const std::filesystem::path SpecPath = ContainerParams.TempPath / "vidyagodfs.runner.spec.json";
    if (!ContainerWrapper::SpawnVidyagodfs(Spec, ContainerParams.RunnerMountPath, SpecPath))
    {
        LogErr("ContainerWrapper::MountRunnerBuild", "runner build mount did not appear at " + ContainerParams.RunnerMountPath.string());
        return false;
    }
    ContainerParams.CleanupUnmountPaths.push_back(ContainerParams.RunnerMountPath);  // ephemeral, lazy unmount
    LogSucc("ContainerWrapper::MountRunnerBuild", "Mounted runner build at " + ContainerParams.RunnerMountPath.string());
    return true;
}

//Installs a runner package: fetches its VFSZipLayer CIDs (IPFS), and for wine runners generates the
//one-time read-only DEFPREFIX artifact by mounting the build and running `wineboot` once. Idempotent.
bool ContainerWrapper::ImportRunner(nlohmann::ordered_json &GlobalConfigJSON, const nlohmann::ordered_json &RunnerPkg, const std::string &PackageDir, const std::string &VariantId, std::string *Error)
{
    (void)GlobalConfigJSON;
    auto Fail = [&](const std::string &M) -> bool { if (Error) *Error = M; LogErr("ContainerWrapper::ImportRunner", M); return false; };

    const std::string Id  = RunnerWrapper::RunnerId(RunnerPkg);
    const std::string Vid = VariantId.empty() ? RunnerWrapper::DefaultVariantId(RunnerPkg) : VariantId;
    if (Id.empty())  return Fail("runner package has no RUNNER_ID");
    if (Vid.empty()) return Fail("runner '" + Id + "' has no variant to install");
    if (PackageDir.empty()) return Fail("runner '" + Id + "' has no package directory");
    const nlohmann::ordered_json R = RunnerWrapper::Variant(RunnerPkg, Vid);   // the runner VARIANT (exec params)
    if (R.empty())   return Fail("runner '" + Id + "' has no variant '" + Vid + "'");
    const std::string Label = Id + ":" + Vid;
    const std::filesystem::path Pkg(PackageDir);
    std::error_code Ec;

    //The components this variant's MODULES enable — the runner build is their VFS layers.
    std::vector<std::string> WantComps;
    for (const auto &M : R.value("MODULES", nlohmann::ordered_json::array()))
        if (M.is_object()) { std::string C = M.value("COMPONENT", std::string()); if (!C.empty()) WantComps.push_back(C); }
    auto Wanted = [&](const nlohmann::ordered_json &C){ for (const auto &W : WantComps) if (C.value("COMPONENTID", std::string()) == W) return true; return false; };

    //1. Hydrate this variant's build layers IN PLACE in the runner's library dir (FetchToPath to PackageDir/PATH).
    int FetchedLayers = 0;
    for (const auto &C : RunnerPkg.value("COMPONENTS", nlohmann::ordered_json::array()))
    {
        if (!Wanted(C) || !C.contains("SUBCOMPONENTS") || !C["SUBCOMPONENTS"].is_array()) continue;
        for (const auto &S : C["SUBCOMPONENTS"])
        {
            const std::string T = S.value("TYPE", std::string());
            if (T != "VFSZipLayer" && T != "VFSDirLayer" && T != "VFSFileLayer") continue;
            std::filesystem::path Local; std::string Cid;
            LayerLocator(S, Pkg, Local, Cid);
            if (Cid.empty() || Local == Pkg || std::filesystem::exists(Local, Ec)) continue;
            std::string Err;
            if (IpfsWrapper::FetchToPath(Cid, Local.string(), &Err).empty())
                return Fail("could not fetch runner build CID " + Cid + " (" + Err + ")");
            ++FetchedLayers;
        }
    }
    LogSucc("ContainerWrapper::ImportRunner", "Hydrated runner build for " + Label + " (" + std::to_string(FetchedLayers) + " layer(s))");
    if (!RunnerWrapper::IsWineRunner(RunnerPkg, Vid)) return true;       // non-wine: no prefix to build

    //2. Generate the one-time DEFPREFIX in the runner's library dir (idempotent): PackageDir/__DEFPREFIX__/<vid>.
    const std::filesystem::path DefArtifact = RunnerWrapper::DefPrefixArtifact(PackageDir, Vid);
    const std::string Subpath = R.value("PREFIX_SUBPATH", std::string());
    const std::filesystem::path PrefixDir = Subpath.empty() ? DefArtifact : (DefArtifact / Subpath);
    if (std::filesystem::exists(PrefixDir, Ec) && !std::filesystem::is_empty(PrefixDir, Ec))
    { LogOut("ContainerWrapper::ImportRunner", "DEFPREFIX already present for " + Label); return true; }

    //Mount the (now-local) build read-only at a temp mount under __DEFPREFIX__ so `wineboot` can run from it.
    const std::filesystem::path MountDir = Pkg / "__DEFPREFIX__" / (".buildmount-" + Vid);
    std::filesystem::create_directories(MountDir.parent_path(), Ec);
    nlohmann::ordered_json Spec;
    Spec["mountpoint"] = MountDir.string(); Spec["uid"] = 1000; Spec["gid"] = 1000;
    Spec["readonly"] = true; Spec["writelayer"] = nullptr;
    nlohmann::ordered_json Layers = nlohmann::ordered_json::array();
    for (const auto &C : RunnerPkg.value("COMPONENTS", nlohmann::ordered_json::array()))
    {
        if (!Wanted(C)) continue;
        for (const auto &S : C.value("SUBCOMPONENTS", nlohmann::ordered_json::array()))
        {
            const std::string T = S.value("TYPE", std::string());
            const std::string LType = (T == "VFSZipLayer") ? "zip" : (T == "VFSDirLayer") ? "dir" : (T == "VFSFileLayer") ? "file" : "";
            if (LType.empty()) continue;
            Layers.push_back({{"type", LType}, {"source", ResolveLayerSource(S, Pkg)},
                              {"target", S.value("TARGET", std::string())},
                              {"submounts", S.value("SUBMOUNTS", nlohmann::ordered_json::array())}, {"rw", false}});
        }
    }
    Spec["layers"] = Layers;
    if (!SpawnVidyagodfs(Spec, MountDir, Pkg / "__DEFPREFIX__" / (".buildmount-" + Vid + ".spec.json")))
        return Fail("could not mount runner build for install");

    //Build the runner ENV/ARGS/EXECUTABLE with the install-time variable bindings (prefix → the artifact).
    std::map<std::string, std::string> Vars;
    Vars["RunnerMount"]     = MountDir.string();
    Vars["PrefixBase"]      = DefArtifact.string();        // STEAM_COMPAT_DATA_PATH
    Vars["RuntimeBasePath"] = DefArtifact.string();
    Vars["WinePrefix"]      = PrefixDir.string();
    Vars["TempPath"]        = DefArtifact.string();

    std::string Program = R.value("EXECUTABLE", std::string("%RunnerMount%/proton"));
    ContainerWrapper::StringVariableSubstitution(Program, Vars);
    QStringList Args;
    for (const auto &A : R.value("ARGS", nlohmann::ordered_json::array()))
    { std::string a = std::string(A); ContainerWrapper::StringVariableSubstitution(a, Vars); Args << QString::fromStdString(a); }
    Args << "wineboot";

    QProcessEnvironment Env = SystemToolEnv();                                   // system proton/wine, not AppImage libs
    const nlohmann::ordered_json RemoveEnv = R.value("REMOVE_ENV", nlohmann::ordered_json::array());
    const nlohmann::ordered_json RunnerEnv = R.value("ENV", nlohmann::ordered_json::object());
    for (const auto &K : RemoveEnv) Env.remove(QString::fromStdString(std::string(K)));
    for (auto &[K, V] : RunnerEnv.items())
    { std::string v = V.get<std::string>(); ContainerWrapper::StringVariableSubstitution(v, Vars); Env.insert(QString::fromStdString(K), QString::fromStdString(v)); }

    std::filesystem::create_directories(DefArtifact, Ec);
    LogOut("ContainerWrapper::ImportRunner", "Generating DEFPREFIX: " + Program + " " + Args.join(' ').toStdString());
    QProcess P; P.setProgram(QString::fromStdString(Program)); P.setArguments(Args); P.setProcessEnvironment(Env);
    P.start(); P.waitForFinished(-1);
    std::cout << P.readAllStandardError().toStdString() << std::endl << P.readAllStandardOutput().toStdString() << std::endl;
    const bool BootOk = (P.exitStatus() == QProcess::NormalExit && P.exitCode() == 0 && std::filesystem::exists(PrefixDir, Ec));

    ContainerWrapper::RunCommand("fusermount3", {"-uz", MountDir.string()}, SystemToolEnv());
    std::filesystem::remove_all(MountDir, Ec);                                   // drop the transient build mountpoint
    if (!BootOk) { std::filesystem::remove_all(DefArtifact, Ec); return Fail("wineboot failed to build DEFPREFIX for " + Label); }
    LogSucc("ContainerWrapper::ImportRunner", "Installed runner " + Label + " (DEFPREFIX at " + DefArtifact.string() + ")");
    return true;
}

//Every distinct ipfs CID a package's content references — the SOURCE:{TYPE:"ipfs",CID} of every VFS layer
//(VFSZip/Dir/FileLayer) across its COMPONENTS. This is the content a game install must fetch to be playable.
std::vector<std::string> ContainerWrapper::PackageIpfsCids(const nlohmann::ordered_json &Manifest)
{
    std::vector<std::string> Cids;
    std::set<std::string> Seen;
    if (!Manifest.contains("COMPONENTS") || !Manifest["COMPONENTS"].is_array()) return Cids;
    for (const auto &C : Manifest["COMPONENTS"])
    {
        if (!C.is_object() || !C.contains("SUBCOMPONENTS") || !C["SUBCOMPONENTS"].is_array()) continue;
        for (const auto &S : C["SUBCOMPONENTS"])
        {
            const std::string T = S.value("TYPE", std::string());
            if (T != "VFSZipLayer" && T != "VFSDirLayer" && T != "VFSFileLayer") continue;
            if (!S.contains("SOURCE") || !S["SOURCE"].is_object()) continue;
            const auto &Src = S["SOURCE"];
            if (Src.value("TYPE", std::string()) != "ipfs") continue;
            const std::string Cid = Src.value("CID", std::string());
            if (!Cid.empty() && Seen.insert(Cid).second) Cids.push_back(Cid);
        }
    }
    return Cids;
}

//Every distinct cover-art CID a package references — the SOURCE:{ipfs,CID} of each GAMES[].METADATA.COVER object
//(and the legacy game-level COVER). Covers are content-addressed like layers; this drives the IPFS-tab "Assets".
std::vector<std::string> ContainerWrapper::PackageCoverCids(const nlohmann::ordered_json &Manifest)
{
    std::vector<std::string> Cids;
    std::set<std::string> Seen;
    if (!Manifest.contains("GAMES") || !Manifest["GAMES"].is_array()) return Cids;
    auto Consider = [&](const nlohmann::ordered_json &Holder)
    {
        if (!Holder.contains("COVER") || !Holder["COVER"].is_object()) return;
        const auto &Cv = Holder["COVER"];
        if (!Cv.contains("SOURCE") || !Cv["SOURCE"].is_object() || Cv["SOURCE"].value("TYPE", std::string()) != "ipfs") return;
        const std::string Cid = Cv["SOURCE"].value("CID", std::string());
        if (!Cid.empty() && Seen.insert(Cid).second) Cids.push_back(Cid);
    };
    for (const auto &G : Manifest["GAMES"])
    {
        if (!G.is_object()) continue;
        if (G.contains("METADATA") && G["METADATA"].is_object()) Consider(G["METADATA"]);
        Consider(G);
    }
    return Cids;
}

//HYDRATES a package IN PLACE: fetches every ipfs content layer AND every cover to its declared PATH inside the
//package's LIBRARY dir (the git clone), turning an un-hydrated entry into a fully-local one — identical to a local
//package. The LIBRARY entry already exists from sync; this ensures it points here. Idempotent: re-importing
//fetches nothing already present. Caller persists GlobalConfig.
bool ContainerWrapper::ImportPackage(nlohmann::ordered_json &GlobalConfigJSON, const nlohmann::ordered_json &Manifest,
                                      const std::string &PackageDir, std::string *Error)
{
    auto Fail = [&](const std::string &M) -> bool { if (Error) *Error = M; LogErr("ContainerWrapper::ImportPackage", M); return false; };

    const std::string Uid = Manifest.value("PACKAGEUID", std::string());
    if (Uid.empty()) return Fail("package has no PACKAGEUID");
    if (!GlobalConfigJSON.contains("LIBRARY") || !GlobalConfigJSON["LIBRARY"].is_array())
        GlobalConfigJSON["LIBRARY"] = nlohmann::ordered_json::array();

    const std::filesystem::path Dest(PackageDir);                                 // the cloned package dir = the library package
    std::error_code Ec;
    int Fetched = 0;

    //Fetch each VFS layer's content in place at Dest/PATH (CID is the permanent identity; PATH the local home).
    if (Manifest.contains("COMPONENTS") && Manifest["COMPONENTS"].is_array())
        for (const auto &C : Manifest["COMPONENTS"])
        {
            if (!C.is_object() || !C.contains("SUBCOMPONENTS") || !C["SUBCOMPONENTS"].is_array()) continue;
            for (const auto &S : C["SUBCOMPONENTS"])
            {
                const std::string T = S.value("TYPE", std::string());
                if (T != "VFSZipLayer" && T != "VFSDirLayer" && T != "VFSFileLayer") continue;
                std::filesystem::path Local; std::string Cid;
                LayerLocator(S, Dest, Local, Cid);
                if (Cid.empty() || Local == Dest) continue;                      // no backend, or no PATH
                if (std::filesystem::exists(Local, Ec)) continue;                // already present
                std::string Err;
                if (IpfsWrapper::FetchToPath(Cid, Local.string(), &Err).empty())
                    return Fail("could not fetch layer CID " + Cid + " (" + Err + ")");
                ++Fetched;
            }
        }

    //Fetch covers in place too (COVER objects are {PATH, SOURCE:{ipfs,CID}} — LayerLocator reads them like a layer).
    auto FetchCover = [&](const nlohmann::ordered_json &Holder)
    {
        if (!Holder.contains("COVER") || !Holder["COVER"].is_object()) return;
        std::filesystem::path Local; std::string Cid;
        LayerLocator(Holder["COVER"], Dest, Local, Cid);
        if (Cid.empty() || Local == Dest || std::filesystem::exists(Local, Ec)) return;
        std::string Err;
        if (IpfsWrapper::FetchToPath(Cid, Local.string(), &Err).empty())
            LogWarn("ContainerWrapper::ImportPackage", "could not fetch cover CID " + Cid + " (" + Err + ")");
        else ++Fetched;
    };
    if (Manifest.contains("GAMES") && Manifest["GAMES"].is_array())
        for (const auto &G : Manifest["GAMES"])
        {
            if (!G.is_object()) continue;
            if (G.contains("METADATA") && G["METADATA"].is_object()) FetchCover(G["METADATA"]);
            FetchCover(G);
        }
    LogSucc("ContainerWrapper::ImportPackage", "Hydrated package " + Uid + " in place at " + Dest.string()
            + " (" + std::to_string(Fetched) + " file(s) fetched)");

    //Ensure a LIBRARY entry points at the hydrated dir (sync usually created it already; add if missing — e.g. a
    //manual external import → a local entry with no REPO).
    bool Found = false;
    for (auto &E : GlobalConfigJSON["LIBRARY"])
        if (E.value("PACKAGEUID", std::string()) == Uid) { E["PATH"] = Dest.string(); Found = true; break; }
    if (!Found)
    {
        nlohmann::ordered_json Slim;
        Slim["PACKAGEUID"]  = Uid;
        Slim["PACKAGENAME"] = Manifest.value("PACKAGENAME", std::string());
        Slim["PATH"]        = Dest.string();
        GlobalConfigJSON["LIBRARY"].push_back(std::move(Slim));
    }
    return true;
}

//PUBLISH — the inverse of ImportPackage. Dehydrates a local package for sharing: seeds each VFS layer's local
//content over IPFS and records its CID into the manifest fragments IN PLACE (content kept), then optionally
//exports a manifest-only copy (no content) ready to commit into a sharing repo. See the header for the contract.
bool ContainerWrapper::PublishPackage(const std::string &PackageDir, const std::string &DehydratedDestDir, std::string *Error)
{
    auto Fail = [&](const std::string &M) -> bool { if (Error) *Error = M; LogErr("ContainerWrapper::PublishPackage", M); return false; };

    std::error_code Ec;
    const std::filesystem::path Pkg(PackageDir);
    if (!std::filesystem::is_directory(Pkg, Ec)) return Fail("not a package directory: " + PackageDir);

    if (!IpfsWrapper::DaemonRunning())
        LogWarn("ContainerWrapper::PublishPackage",
                "no IPFS daemon running — CIDs will be computed but content seeds to peers only once `ipfs daemon` is up.");

    auto IsLayer = [](const std::string &T) { return T == "VFSZipLayer" || T == "VFSDirLayer" || T == "VFSFileLayer"; };

    int Seeded = 0, Walked = 0, Covers = 0;

    //Walk every *.json fragment directly (no assemble/decompose round-trip — this preserves each subcomponent's
    //exact file placement). Content-address VFS layers AND cover assets in place; re-save only mutated fragments.
    for (const auto &Entry : std::filesystem::directory_iterator(Pkg, Ec))
    {
        if (!Entry.is_regular_file() || Entry.path().extension() != ".json") continue;
        QFile FragFile(QString::fromStdString(Entry.path().string()));
        nlohmann::ordered_json Frag;
        if (JSONOps::LoadJSON(&FragFile, &Frag)) continue;                       // LoadJSON returns true on FAILURE

        bool Mutated = false;

        //Content layers: keep PATH, add SOURCE:{ipfs,CID} (idempotent — skip those already carrying a CID).
        if (Frag.contains("COMPONENTS") && Frag["COMPONENTS"].is_array())
        for (auto &C : Frag["COMPONENTS"])
        {
            if (!C.is_object() || !C.contains("SUBCOMPONENTS") || !C["SUBCOMPONENTS"].is_array()) continue;
            for (auto &S : C["SUBCOMPONENTS"])
            {
                if (!IsLayer(S.value("TYPE", std::string()))) continue;
                ++Walked;
                std::filesystem::path Local; std::string Cid;
                LayerLocator(S, Pkg, Local, Cid);

                std::error_code Rc;
                if (!Cid.empty()) continue;                                      // already has an ipfs CID — idempotent
                if (!std::filesystem::exists(Local, Rc)) continue;               // no local content to seed
                std::string Err;
                const std::string NewCid = IpfsWrapper::AddNoCopy(Local.string(), &Err);
                if (NewCid.empty()) return Fail("could not seed layer " + Local.string() + " (" + Err + ")");

                nlohmann::ordered_json Src = (S.contains("SOURCE") && S["SOURCE"].is_object())
                                                 ? S["SOURCE"] : nlohmann::ordered_json::object();
                Src["TYPE"] = "ipfs";                                            // keep any existing SOURCE.PATH override
                Src["CID"]  = NewCid;
                S["SOURCE"] = std::move(Src);
                Mutated = true;
                ++Seeded;
            }
        }

        //Cover art: content-address the curated GAMES[].METADATA.COVER exactly like a layer — keep the filename in
        //PATH, add SOURCE:{ipfs,CID}. Upgrades the legacy bare-string form; idempotent once a CID is present.
        auto SeedCover = [&](nlohmann::ordered_json &Holder)
        {
            if (!Holder.contains("COVER")) return;
            nlohmann::ordered_json &Cover = Holder["COVER"];
            std::string File;
            if (Cover.is_string()) File = Cover.get<std::string>();
            else if (Cover.is_object())
            {
                if (Cover.contains("SOURCE") && Cover["SOURCE"].is_object()
                    && !Cover["SOURCE"].value("CID", std::string()).empty()) return;   // already addressed
                File = Cover.value("PATH", std::string());
            }
            else return;
            if (File.empty()) return;
            std::error_code Rc;
            const std::filesystem::path Local = Pkg / File;
            if (!std::filesystem::exists(Local, Rc)) return;                      // not a local file (CID-only ref)
            std::string Err;
            const std::string NewCid = IpfsWrapper::AddNoCopy(Local.string(), &Err);
            if (NewCid.empty()) { LogWarn("ContainerWrapper::PublishPackage", "could not seed cover " + Local.string() + " (" + Err + ")"); return; }
            Cover = nlohmann::ordered_json{ {"PATH", File}, {"SOURCE", {{"TYPE", "ipfs"}, {"CID", NewCid}}} };
            Mutated = true;
            ++Covers;
        };
        if (Frag.contains("GAMES") && Frag["GAMES"].is_array())
        for (auto &G : Frag["GAMES"])
        {
            if (!G.is_object()) continue;
            if (G.contains("METADATA") && G["METADATA"].is_object()) SeedCover(G["METADATA"]);
            SeedCover(G);                                                         // legacy game-level COVER
        }

        if (Mutated && !JSONOps::SaveJSON(&Frag, &FragFile))
            return Fail("could not write annotated manifest fragment: " + Entry.path().string());
    }
    LogSucc("ContainerWrapper::PublishPackage", "Dehydrated " + PackageDir + " (" + std::to_string(Seeded)
            + " of " + std::to_string(Walked) + " layer(s) + " + std::to_string(Covers) + " cover(s) newly seeded)");

    //Export the dehydrated manifest, if requested — a clean manifest-only copy (JSON fragments, no image bytes;
    //covers travel as CIDs). remove_all first so the export dir holds only the dehydration.
    if (!DehydratedDestDir.empty())
    {
        std::filesystem::remove_all(DehydratedDestDir, Ec);
        const int Copied = MirrorDehydrated(PackageDir, DehydratedDestDir);
        LogSucc("ContainerWrapper::PublishPackage", "Exported dehydrated manifest to " + DehydratedDestDir
                + " (" + std::to_string(Copied) + " JSON fragment(s))");
    }
    return true;
}

//Copies a package's DEHYDRATED manifest — ONLY its top-level *.json fragments — into DestDir. Never copies content
//or image bytes (covers travel as CIDs in the manifest); never deletes anything already in DestDir (so it can
//refresh a mirror over hydrated content). Returns the number of fragments copied.
int ContainerWrapper::MirrorDehydrated(const std::string &SrcDir, const std::string &DestDir)
{
    std::error_code Ec;
    std::filesystem::create_directories(DestDir, Ec);
    int Copied = 0;
    for (const auto &Entry : std::filesystem::directory_iterator(SrcDir, Ec))
    {
        if (!Entry.is_regular_file() || Entry.path().extension() != ".json") continue;   // manifests only
        std::error_code Ce;
        std::filesystem::copy_file(Entry.path(), std::filesystem::path(DestDir) / Entry.path().filename(),
                                   std::filesystem::copy_options::overwrite_existing, Ce);
        if (Ce) LogWarn("ContainerWrapper::MirrorDehydrated", "skip " + Entry.path().string() + " (" + Ce.message() + ")");
        else ++Copied;
    }
    return Copied;
}

//True when every VFS content layer of a game is present locally at its expected path (vacuously true if the
//package has no content layers — nothing to fetch). This is the "hydrated" state the library/store split keys
//off; it mirrors launch's local-first resolution via LayerLocator.
bool ContainerWrapper::PackageHydrated(const nlohmann::ordered_json &Manifest, const std::string &PackageDir)
{
    if (!Manifest.contains("COMPONENTS") || !Manifest["COMPONENTS"].is_array()) return true;
    const std::filesystem::path Pkg(PackageDir);
    for (const auto &C : Manifest["COMPONENTS"])
    {
        if (!C.is_object() || !C.contains("SUBCOMPONENTS") || !C["SUBCOMPONENTS"].is_array()) continue;
        for (const auto &S : C["SUBCOMPONENTS"])
        {
            const std::string T = S.value("TYPE", std::string());
            if (T != "VFSZipLayer" && T != "VFSDirLayer" && T != "VFSFileLayer") continue;
            std::filesystem::path Local; std::string Cid;
            LayerLocator(S, Pkg, Local, Cid);
            std::error_code Ec;
            if (!std::filesystem::exists(Local, Ec)) return false;                // a layer's content is missing
        }
    }
    return true;
}

//True iff the manifest declares at least one VFS content layer. A content-less package (a malformed game with
//no layers, e.g. Dino Crisis 2, or a PATH-only runner like wine/umu-proton/native-passthrough/snes9x) returns
//false — paired with PackageHydrated it keeps those out of the Library/Packages tabs. Ignores SOURCE so a
//locally-added package (content present, no CID) still counts as having content.
bool ContainerWrapper::PackageHasContent(const nlohmann::ordered_json &Manifest)
{
    if (!Manifest.contains("COMPONENTS") || !Manifest["COMPONENTS"].is_array()) return false;
    for (const auto &C : Manifest["COMPONENTS"])
    {
        if (!C.is_object() || !C.contains("SUBCOMPONENTS") || !C["SUBCOMPONENTS"].is_array()) continue;
        for (const auto &S : C["SUBCOMPONENTS"])
        {
            const std::string T = S.value("TYPE", std::string());
            if (T == "VFSZipLayer" || T == "VFSDirLayer" || T == "VFSFileLayer") return true;
        }
    }
    return false;
}

//True when a package is installed: its PACKAGEUID is in LIBRARY and every content CID is locally cached.
bool ContainerWrapper::IsPackageImported(const nlohmann::ordered_json &GlobalConfigJSON, const nlohmann::ordered_json &Manifest)
{
    const std::string Uid = Manifest.value("PACKAGEUID", std::string());
    if (Uid.empty()) return false;
    bool InLibrary = false;
    if (GlobalConfigJSON.contains("LIBRARY") && GlobalConfigJSON["LIBRARY"].is_array())
        for (const auto &E : GlobalConfigJSON["LIBRARY"])
            if (E.value("PACKAGEUID", std::string()) == Uid) { InLibrary = true; break; }
    if (!InLibrary) return false;
    //Imported = hydrated: its content is present locally in its LIBRARY dir.
    for (const auto &E : GlobalConfigJSON["LIBRARY"])
        if (E.value("PACKAGEUID", std::string()) == Uid)
            return PackageHydrated(Manifest, E.value("PATH", std::string()));
    return false;
}

//The 3 Wine registry files that hold per-prefix state.
static const char *const kRegFiles[] = { "system.reg", "user.reg", "userdef.reg" };

//Seeds previously-persisted reg files from UserDataPath/__REGISTRY__/ into the ephemeral
//WRITELAYER before the union mounts, so they shadow the DEFPREFIX base. Wine always writes the
//complete file, so a whole persisted reg file is correct (no stripped-delta shadowing).
bool ContainerWrapper::SeedPersistRegistry(struct ContainerParams &ContainerParams)
{
    if (ContainerParams.PersistAll) return true; //durable RW branch already holds the reg files
    const std::filesystem::path RegStore = ContainerParams.UserDataPath / "__REGISTRY__";
    std::error_code ec;
    std::filesystem::create_directories(ContainerParams.WriteLayerPath, ec);
    for (const char *const Name : kRegFiles)
    {
        const std::filesystem::path SrcReg = RegStore / Name;
        if (!std::filesystem::exists(SrcReg)) continue;
        const std::filesystem::path DstReg = ContainerParams.WriteLayerPath / Name;
        std::filesystem::copy_file(SrcReg, DstReg, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) LogWarn("ContainerWrapper::SeedPersistRegistry", "Could not seed " + std::string(Name) + ": " + ec.message());
        else    LogOut("ContainerWrapper::SeedPersistRegistry", "Seeded persisted " + std::string(Name));
    }
    return true;
}

//Captures the session's registry by copying RuntimePath/*.reg into UserDataPath/__REGISTRY__/.
//Runs during Cleanup BEFORE the runtime is unmounted/wiped. Bounded copy of small metadata files.
bool ContainerWrapper::CapturePersistRegistry(struct ContainerParams &ContainerParams)
{
    if (ContainerParams.PersistAll) return true; //already durable
    const std::filesystem::path RegStore = ContainerParams.UserDataPath / "__REGISTRY__";
    std::error_code ec;
    std::filesystem::create_directories(RegStore, ec);
    for (const char *const Name : kRegFiles)
    {
        const std::filesystem::path SrcReg = ContainerParams.RuntimePath / Name;
        if (!std::filesystem::exists(SrcReg)) continue;
        const std::filesystem::path DstReg = RegStore / Name;
        std::filesystem::copy_file(SrcReg, DstReg, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) LogWarn("ContainerWrapper::CapturePersistRegistry", "Could not capture " + std::string(Name) + ": " + ec.message());
        else    LogOut("ContainerWrapper::CapturePersistRegistry", "Captured " + std::string(Name));
    }
    return true;
}

//Seeds each PersistFile from its durable home (UserDataPath/<rel>) into WriteLayerPath/<rel> before
//the union mounts, so the persisted file shadows the lower read-only layers. Single files are
//copied (not bind-mounted): bindfs operates on directories, and copy mirrors the registry model.
//No-op under PersistAll (the durable RW branch already holds everything) or when nothing is stored yet.
bool ContainerWrapper::SeedPersistFiles(struct ContainerParams &ContainerParams)
{
    if (ContainerParams.PersistAll) return true; //durable RW branch already holds the files
    std::error_code ec;
    for (const std::string &Rel : ContainerParams.PersistFiles)
    {
        const std::filesystem::path SrcFile = ContainerParams.UserDataPath  / Rel; //durable source (in package)
        if (!std::filesystem::exists(SrcFile)) continue;                            //nothing persisted yet
        const std::filesystem::path DstFile = ContainerParams.WriteLayerPath / Rel; //shadow in the RW top layer
        std::filesystem::create_directories(DstFile.parent_path(), ec);
        std::filesystem::copy_file(SrcFile, DstFile, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) LogWarn("ContainerWrapper::SeedPersistFiles", "Could not seed " + Rel + ": " + ec.message());
        else    LogOut("ContainerWrapper::SeedPersistFiles", "Seeded persisted file " + Rel);
    }
    return true;
}

//Captures each PersistFile by copying RuntimePath/<rel> into its durable home UserDataPath/<rel>.
//Runs during Cleanup BEFORE the runtime is unmounted/wiped, mirroring CapturePersistRegistry.
//No-op under PersistAll.
bool ContainerWrapper::CapturePersistFiles(struct ContainerParams &ContainerParams)
{
    if (ContainerParams.PersistAll) return true; //already durable
    std::error_code ec;
    for (const std::string &Rel : ContainerParams.PersistFiles)
    {
        const std::filesystem::path SrcFile = ContainerParams.RuntimePath  / Rel; //session result in the mounted union
        if (!std::filesystem::exists(SrcFile)) continue;                          //game never created it
        const std::filesystem::path DstFile = ContainerParams.UserDataPath / Rel; //durable home (in package)
        std::filesystem::create_directories(DstFile.parent_path(), ec);
        std::filesystem::copy_file(SrcFile, DstFile, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) LogWarn("ContainerWrapper::CapturePersistFiles", "Could not capture " + Rel + ": " + ec.message());
        else    LogOut("ContainerWrapper::CapturePersistFiles", "Captured file " + Rel);
    }
    return true;
}

//Extracts each RegKeyPersist subtree from the mounted RuntimePath hives and merges it into the
//durable store UserDataPath/__REGKEYS__ (partial hive files holding only the persisted keys). Runs
//during Cleanup BEFORE unmount. A key absent from the session (never created) is left as-is in the
//store rather than dropped. No-op under PersistAll.
bool ContainerWrapper::CapturePersistRegKeys(struct ContainerParams &ContainerParams)
{
    if (ContainerParams.PersistAll || ContainerParams.PersistRegKeys.empty()) return true;
    const std::filesystem::path Store = ContainerParams.UserDataPath / "__REGKEYS__";

    RegistryWrapper Session;
    Session.LoadPrefix(ContainerParams.RuntimePath);

    RegistryWrapper Durable;
    if (std::filesystem::exists(Store)) Durable.LoadPrefix(Store); // accumulate across sessions

    int Captured = 0;
    for (const std::string &RegPath : ContainerParams.PersistRegKeys)
        if (Durable.MergeKeyFrom(Session, RegPath)) { LogOut("ContainerWrapper::CapturePersistRegKeys", "Captured " + RegPath); ++Captured; }
        else LogOut("ContainerWrapper::CapturePersistRegKeys", "Key absent in session (kept prior): " + RegPath);

    if (Captured > 0)
    {
        std::error_code ec;
        std::filesystem::create_directories(Store, ec);
        if (!Durable.SavePrefix(Store))
            LogWarn("ContainerWrapper::CapturePersistRegKeys", "Failed to write durable __REGKEYS__ store.");
    }
    return true;
}

//Walks DirectoryPath recursively, lowercasing every path and checking for duplicates.
//Collisions mean two files differ only in case — problematic under Wine because its
//filesystem emulation may resolve to the wrong file depending on access order.
//Reports all conflicts via a QMessageBox warning and returns false if any are found.
bool ContainerWrapper::CheckCaseConflicts(std::filesystem::path DirectoryPath)
{
    std::unordered_set<std::string> FilePathList;
    std::unordered_set<std::string> CaseConflictList;
    bool NoConflict = true;
    //Walking a freshly-mounted FUSE tree can throw filesystem_error (a transient entry, a permission
    //issue) — catch it so the launch worker thread never dies here. skip_permission_denied avoids the
    //most common throw outright.
    try
    {
        for (const auto& FilePath : std::filesystem::recursive_directory_iterator(
                 DirectoryPath, std::filesystem::directory_options::skip_permission_denied))
        {
            std::string FilePathLowercase = FilePath.path().string();
            std::transform(FilePathLowercase.begin(), FilePathLowercase.end(),FilePathLowercase.begin(),[](unsigned char c){ return std::tolower(c); });

            if (FilePathList.find(FilePathLowercase) != FilePathList.end())
            {
                NoConflict = false;
                CaseConflictList.insert(FilePathLowercase);
            }
            else
            {
                FilePathList.insert(FilePathLowercase);
            }
        }
    }
    catch (const std::exception &e)
    {
        LogWarn("ContainerWrapper::CheckCaseConflicts", std::string("Stopped scanning early: ") + e.what());
    }

    if(!NoConflict)
    {
        std::ostringstream oss;
        std::for_each(CaseConflictList.begin(), CaseConflictList.end(),[&oss](const std::string& s){ oss << s << '\n'; });
        std::cerr << "CASE CONFLICTS:\n" << oss.str() << std::endl;
        LogWarn("ContainerWrapper::CheckCaseConflicts", "Case conflicts detected in the runtime (see above).");
        //This runs on the launch worker thread; a QWidget dialog must be created on the GUI thread.
        //Marshal it there (and skip entirely when headless — no QApplication). It's only a warning;
        //the launch is not aborted (caller ignores the return).
        if (qApp)
        {
            QString Msg = "Files differing only in case were found in the runtime — Windows games are\n"
                          "case-insensitive, so Wine may open the wrong one. The game will still launch.\n\n"
                          + QString::fromStdString(oss.str());
            QMetaObject::invokeMethod(qApp, [Msg]() { QMessageBox::warning(nullptr, "Case conflicts", Msg); }, Qt::QueuedConnection);
        }
        return false;
    }
    return true;
}


//Collects DLLOVERRIDE values from all DllOverride subcomponents into DLLOverrides.
//The collected strings are later joined and assigned to WINEDLLOVERRIDES in Execute().
//Returns false immediately if any DllOverride subcomponent has a null DLLOVERRIDE field.
bool ContainerWrapper::ProcessDLLOverrides(struct ContainerParams &ContainerParams)
{
    LogOut("ContainerWrapper::ProcessDLLOverrides", "Processing DLL Overrides.");
    for (int i = 0; i < ContainerParams.SubComponentsArray.size(); i++)
    {
        nlohmann::ordered_json SubComponentJSON = ContainerParams.SubComponentsArray[i];
        if (SubComponentJSON["TYPE"] == "DllOverride")
        {
            if(!SubComponentJSON["DLLOVERRIDE"].is_null())
            {
                ContainerParams.DLLOverrides.push_back(SubComponentJSON["DLLOVERRIDE"]);
            }
            else
            {
                return false;
            }
        }
    }
    return true;
}

//Processes FileEdit subcomponents in two passes, split by the OVERRIDE flag.
//  OverridePass=false (base): writes under BaseDir — the DEFAULTDATA layer, between the component
//    layers and the WRITELAYER, so the user's persisted writes naturally shadow them.
//  OverridePass=true  (override): writes to RuntimePath via the union, COW-ing directly into
//    WRITELAYER — wins unconditionally over any previous state, including the user's.
//BaseDir defaults to RuntimePath (override) / DefPrefixPath (base) when not supplied.
//MUST BE RUN AFTER VARIABLE SUBSTITUTION (already done in BuildSubComponentsArray).
bool ContainerWrapper::ProcessFileEdits(struct ContainerParams &ContainerParams, bool OverridePass,
                                        const std::filesystem::path &BaseDir)
{
    std::filesystem::path BasePath = !BaseDir.empty() ? BaseDir
                                     : (OverridePass ? ContainerParams.RuntimePath : ContainerParams.DefPrefixPath);
    LogOut("ContainerWrapper::ProcessFileEdits",
           std::string(OverridePass ? "Override" : "Base") + " FileEdit pass. Base: " + BasePath.string());

    for (auto &Sub : ContainerParams.SubComponentsArray)
    {
        if (Sub.value("TYPE", std::string()) != "FileEdit") continue;
        if (Sub.value("OVERRIDE", false) != OverridePass) continue;

        std::string Mode  = Sub.value("MODE",  std::string());
        std::string File  = Sub.value("FILE",  std::string());
        std::string Value = Sub.value("VALUE", std::string());
        std::filesystem::path FilePath = BasePath / File;

        if (Mode == "ConfigWrite")
            ContainerWrapper::ConfigWrite(Sub.value("KEY", std::string()), Value, FilePath);
        else if (Mode == "Overwrite")
            ContainerWrapper::FileOverwrite(Value, FilePath);
        else
            LogWarn("ContainerWrapper::ProcessFileEdits", "Unknown MODE: '" + Mode + "' — skipping.");
    }
    return true;
}

//Rewrites FilePath in-place: any line whose content starts with Key is replaced by Key+Value.
//All other lines are preserved verbatim. Used to patch INI-style config files where
//the key acts as a line prefix (e.g. "Resolution=") rather than a standalone token.
//Returns false if the file cannot be opened for reading or writing.
bool ContainerWrapper::ConfigWrite(std::string Key, std::string Value, std::filesystem::path FilePath)
{
    LogOut("ContainerWrapper::ConfigWrite", "FilePath: " + FilePath.string() + " Key: " + Key + " Value: " + Value);
    std::ifstream inFile(FilePath);
    if (!inFile.is_open())
    {
        LogErr("ContainerWrapper::ConfigWrite", "Could not open file for reading: " + FilePath.string());
        return false;
    }

    //Read all lines first so the file can be truncated and rewritten without a temporary file.
    std::vector<std::string> lines;
    std::string line;

    while (std::getline(inFile, line))
    {
        lines.push_back(line);
    }

    inFile.close();

    std::ofstream outFile(FilePath, std::ios::trunc);
    if (!outFile.is_open())
    {
        LogErr("ContainerWrapper::ConfigWrite", "Could not open file for writing: " + FilePath.string());
        return false;
    }

    for (auto& currentLine : lines)
    {
        //Match by prefix: if the line starts with Key, replace the whole line with Key+Value.
        if (currentLine.length() >= Key.length() && currentLine.compare(0, Key.length(), Key) == 0)
        {
            currentLine = Key + Value;
            outFile << currentLine << '\n';
        }
        else
        {
            outFile << currentLine << '\n';
        }
    }

    outFile.close();
    return true;
}

//Writes Value as the complete content of FilePath, creating parent directories if needed.
//Used for files whose entire content is the value (e.g. Warcraft III .w3k CD key files).
bool ContainerWrapper::FileOverwrite(const std::string &Value, const std::filesystem::path &FilePath)
{
    LogOut("ContainerWrapper::FileOverwrite", "Writing: " + FilePath.string());
    std::error_code ec;
    std::filesystem::create_directories(FilePath.parent_path(), ec);
    if (ec) { LogErr("ContainerWrapper::FileOverwrite", "Could not create parent dirs: " + ec.message()); return false; }
    std::ofstream Out(FilePath, std::ios::out | std::ios::trunc);
    if (!Out) { LogErr("ContainerWrapper::FileOverwrite", "Could not open for writing: " + FilePath.string()); return false; }
    Out << Value;
    return true;
}


//=====================================================================================================================================================================
//                                                                          RUNNER CLASS
//=====================================================================================================================================================================

//Launches the game using the resolved runner and environment.
//
//Exe resolution order:
//  1. OverrideExe if non-empty (used for tooling / install steps).
//  2. ExePathInPrefix for Wine runners (Windows-style path passed to Wine).
//  3. ExePathComplete for all other runners (absolute host path).
//
//Argument ordering differs by runner type:
//  Wine:     exe first, then ExeArgs (game's own arguments).
//  Other:    RunnerArgs first (runner flags), then exe (ROM/data path).
//
//Runner ENV values undergo %VARIABLE% substitution before being inserted into the
//process environment, so tokens like %WINEPREFIX% are expanded at launch time.
//
//WorkDir falls back to RuntimePath (VFS) or PackagePath if the configured path
//does not exist in the mounted runtime.
bool ContainerWrapper::Execute(std::string OverrideExe)
{
    //Launch phase: bind the context prefix vars to the mounted runtime (InitializeDefPrefix bound them to
    //the def-prefix). %WinePrefix% → RuntimePath, %PrefixBase% → RuntimeBasePath (= proton STEAM_COMPAT_DATA_PATH).
    ContainerParams.ActivePrefix     = ContainerParams.RuntimePath;
    ContainerParams.ActivePrefixBase = ContainerParams.RuntimeBasePath;

    //When the prefix lives in a subdir (PREFIX_SUBPATH, e.g. Proton's pfx), the launcher keeps compat-data
    //bookkeeping (version / tracked_files / config_info) at the *base* level. Init wrote those into
    //DefPrefixBasePath; seed them into RuntimeBasePath so the launcher sees an already-provisioned prefix and
    //skips its launch-time upgrade (which would otherwise fail reading the missing base-level files). The pfx
    //itself is the mounted union at RuntimePath; only the sibling base files are carried over.
    if (ContainerParams.RuntimePath != ContainerParams.RuntimeBasePath
        && std::filesystem::exists(ContainerParams.DefPrefixBasePath))
    {
        std::error_code Ec;
        for (const auto &Entry : std::filesystem::directory_iterator(ContainerParams.DefPrefixBasePath, Ec))
        {
            if (!Entry.is_regular_file(Ec)) continue;          // skip pfx/ (the prefix dir / mountpoint)
            const std::string Name = Entry.path().filename().string();
            if (Name == "pfx.lock") continue;                  // don't carry a stale lock
            std::filesystem::copy_file(Entry.path(), ContainerParams.RuntimeBasePath / Name,
                                       std::filesystem::copy_options::overwrite_existing, Ec);
        }
    }

    std::string FinalExe;
    if (!OverrideExe.empty())
    {
        FinalExe = OverrideExe;
    }
    else if (ContainerParams.RunnerTypeEnum == RunnerType::Wine)
    {
        //Pass the Windows-style path so Wine resolves it through its own drive mappings.
        FinalExe = ContainerParams.ExePathInPrefix.string();
    }
    else if (!ContainerParams.ExePathRelative.empty())
    {
        //Non-Wine: only set FinalExe when the VariantDefinition specified a file (ROM, EXEPATH, DATAPATH).
        //If ExePathRelative is empty the runner is self-contained (e.g. a bare AppImage) and
        //needs no positional argument — passing ProgramPath would be meaningless and likely fatal.
        FinalExe = ContainerParams.ExePathComplete.string();
    }

    if (FinalExe.empty() && ContainerParams.RunnerTypeEnum == RunnerType::Wine)
    {
        LogErr("ContainerWrapper::Execute", "No exe to run (Wine requires an EXEPATH). Aborting.");
        return false;
    }

    //Substitute variables in the runner executable path — manifest runners reference
    //%ProgramPath% to point to a bundled binary mounted into RUNTIME via a VFS layer.
    ContainerWrapper::StringVariableSubstitution(ContainerParams.RunnerExecutable, ContainerParams.GetVariablesMap());
    LogOut("ContainerWrapper::Execute", "Runner: " + ContainerParams.RunnerExecutable);
    LogOut("ContainerWrapper::Execute", "Executing: " + FinalExe);
    LogOut("ContainerWrapper::Execute", "WorkDirPath: " + ContainerParams.WorkDirPathComplete.string());

    QProcessEnvironment RunProcessEnvironment = SystemToolEnv();                 // system runner, not AppImage libs

    // Apply runner-level env removes first so they can't be re-added by the ENV block below.
    for (const std::string &Key : ContainerParams.RunnerRemoveEnv)
    {
        RunProcessEnvironment.remove(QString::fromStdString(Key));
    }

    // Apply runner ENV with template substitution — expands %VARIABLE% tokens in values.
    for (auto &[Key, Value] : ContainerParams.RunnerEnv.items())
    {
        std::string ExpandedValue = Value.get<std::string>();
        ContainerWrapper::StringVariableSubstitution(ExpandedValue, ContainerParams.GetVariablesMap());
        RunProcessEnvironment.insert(QString::fromStdString(Key), QString::fromStdString(ExpandedValue));
    }

    // Wine-specific: WINEDLLOVERRIDES
    //Joins all DLL override strings; umu-run/Wine interprets the combined value.
    if (ContainerParams.RunnerTypeEnum == RunnerType::Wine && !ContainerParams.DLLOverrides.empty())
    {
        RunProcessEnvironment.insert("WINEDLLOVERRIDES", QString::fromStdString(std::accumulate(ContainerParams.DLLOverrides.begin(), ContainerParams.DLLOverrides.end(), std::string{}, [](auto a, auto b){ return a+b;})));
    }

    //If the configured working directory doesn't exist in the mounted runtime, fall back
    //gracefully: prefer the RuntimePath (VFS root) if VFS was used, otherwise PackagePath.
    std::filesystem::path FinalWorkDir = ContainerParams.WorkDirPathComplete;
    if (!std::filesystem::exists(FinalWorkDir))
    {
        FinalWorkDir = ContainerParams.UsesVFS ? ContainerParams.RuntimePath : ContainerParams.PackagePath;
        LogOut("ContainerWrapper::Execute", "WorkDirPath does not exist, falling back to " + FinalWorkDir.string());
    }

    QProcess RunProcess;
    RunProcess.setProgram(QString::fromStdString(ContainerParams.RunnerExecutable));
    RunProcess.setWorkingDirectory(QString::fromStdString(FinalWorkDir));

    //Unified argument order for every runner type: RunnerArgs (template-expanded) → exe/ROM path (if any)
    //→ ExeArgs. This is data-driven: a launcher verb lives in the runner's ARGS (e.g. proton's
    //"waitforexitandrun"), so umu (ARGS=[]) yields [exe, ExeArgs] exactly as before, while proton yields
    //[waitforexitandrun, exe, ExeArgs]. Emulators/native put their flags in ARGS and the ROM as the exe.
    QStringList Arguments;
    for (std::string Arg : ContainerParams.RunnerArgs)
    {
        ContainerWrapper::StringVariableSubstitution(Arg, ContainerParams.GetVariablesMap());
        Arguments.append(QString::fromStdString(Arg));
    }
    if (!FinalExe.empty())
        Arguments.append(QString::fromStdString(FinalExe));
    //ExeArgs from the VariantDefinition (e.g. -c config.cfg for GemRB); skipped for an OverrideExe (tooling).
    if (OverrideExe.empty())
        for (std::string Arg : ContainerParams.ExeArgs)
            Arguments.append(QString::fromStdString(Arg));
    RunProcess.setArguments(Arguments);
    RunProcess.setProcessEnvironment(RunProcessEnvironment);
    //Stream the runner's output (and its grandchildren — proton's python wrapper, wine, the game) straight to our
    //stdout/stderr. Captured pipes can swallow what subprocesses write to the inherited terminal fds, which hides
    //early proton/wine failures; forwarding makes those visible live.
    RunProcess.setProcessChannelMode(QProcess::ForwardedChannels);

    //Register the process pointer so KillGame() can signal it while Execute() blocks.
    {
        QMutexLocker Locker(&ActiveRunMutex);
        ActiveRunProcess = &RunProcess;
    }

    RunProcess.start();
    RunProcess.waitForFinished(-1);

    //Clear the pointer before reading output so KillGame() does not dereference a finished process.
    {
        QMutexLocker Locker(&ActiveRunMutex);
        ActiveRunProcess = nullptr;
    }

    //(Runner output was forwarded live to our stdout/stderr above — nothing buffered to print here.)

    if (RunProcess.exitStatus() == QProcess::CrashExit)
    {
        LogErr("ContainerWrapper::Execute", "Process crashed.");
        return false;
    }
    LogOut("ContainerWrapper::Execute", "Process exited normally with code " + std::to_string(RunProcess.exitCode()));
    return true;
}

//Sends SIGKILL to the active game process if one is running.
//Called from the UI thread while Execute() blocks on the worker thread.
void ContainerWrapper::KillGame()
{
    QMutexLocker Locker(&ActiveRunMutex);
    if (!ActiveRunProcess) return;
    LogOut("ContainerWrapper::KillGame", "Killing game process tree.");

    qint64 Pid = ActiveRunProcess->processId();
    if (Pid > 0)
    {
        //Kill the entire process group (wine, wineserver, game children) in one shot.
        //SIGKILL the group first, then the direct process as a fallback. (System tools — strip the
        //AppImage LD_LIBRARY_PATH so they load host libs, via SystemToolEnv.)
        auto RunSys = [](const QString &Prog, const QStringList &A){
            QProcess Pr; Pr.setProcessEnvironment(SystemToolEnv()); Pr.start(Prog, A); Pr.waitForFinished(-1);
        };
        RunSys("kill", {"-9", "--", "-" + QString::number(Pid)});

        //Also walk /proc to kill any stragglers that escaped the group.
        RunSys("bash", {"-c",
            "pgrep -P " + QString::number(Pid) + " | xargs -r kill -9 2>/dev/null; "
            "kill -9 " + QString::number(Pid) + " 2>/dev/null"});
    }

    ActiveRunProcess->kill(); // Qt-level cleanup in case process didn't die
}

//Tears the session down in a deliberately ordered, save-safe sequence:
//  1. Capture the session registry (PERSIST.REGISTRY) while RUNTIME is still mounted.
//  2. Non-lazily unmount every DURABLE-backed mount (persist binds + the union in PERSIST.ALL),
//     innermost-first, retrying briefly for a lingering wineserver. These expose
//     PackagePath/USERDATA through a mountpoint under TempPath.
//  3. Lazy-unmount the purely ephemeral VFS layers and remove the staging dirs.
//  4. ONLY IF every durable mount detached cleanly, wipe TempPath. If any is still live, the wipe
//     is ABORTED — remove_all could otherwise recurse through a live mount into USERDATA and
//     delete real saves. Leaving TEMP behind is the safe failure mode.
bool ContainerWrapper::Cleanup()
{
    std::error_code ec;

    //1. Registry + file capture must read RUNTIME/<...> before anything is unmounted.
    if (ContainerParams.PersistRegistry && !ContainerParams.PersistAll)
        CapturePersistRegistry(this->ContainerParams);
    if (!ContainerParams.PersistFiles.empty() && !ContainerParams.PersistAll)
        CapturePersistFiles(this->ContainerParams);
    if (!ContainerParams.PersistRegKeys.empty() && !ContainerParams.PersistAll)
        CapturePersistRegKeys(this->ContainerParams);

    //2. Durable-backed mounts: non-lazy + verified, innermost-first (reverse registration order).
    bool DurableUnmountOk = true;
    for (auto It = ContainerParams.CleanupPersistPaths.rbegin(); It != ContainerParams.CleanupPersistPaths.rend(); ++It)
    {
        bool Unmounted = false;
        for (int Attempt = 0; Attempt < 5 && !Unmounted; ++Attempt)
        {
            if (Attempt > 0) QThread::msleep(200); //give a lingering wineserver time to release
            if (ContainerWrapper::RunCommand("fusermount3", {"-u", *It}, SystemToolEnv()) == 0)
                Unmounted = true;
        }
        if (!Unmounted)
        {
            LogErr("ContainerWrapper::Cleanup", "DURABLE mount still busy after retries: " + It->string());
            //Lazy-detach so it eventually clears, but mark the wipe unsafe for this run.
            ContainerWrapper::RunCommand("fusermount3", {"-uz", *It}, SystemToolEnv());
            DurableUnmountOk = false;
        }
    }

    //3. Ephemeral VFS mounts: lazy unmount is fine (their RW/source is all under TempPath).
    for (const std::filesystem::path &UnmountPath : ContainerParams.CleanupUnmountPaths)
        ContainerWrapper::RunCommand("fusermount3", {"-uz", UnmountPath}, SystemToolEnv());

    //4. Save-safety gate: never wipe while a durable mount could still be traversed.
    if (!DurableUnmountOk)
    {
        LogErr("ContainerWrapper::Cleanup", "Aborting TEMP wipe to protect USERDATA — a durable-backed mount is still live. TEMP left in place.");
        return false;
    }

    //RUNTIME/WRITELAYER/DEFPREFIX all live inside TempPath, so this single removal wipes them all.
    std::filesystem::remove_all(this->ContainerParams.TempPath, ec);
    if (ec) LogWarn("ContainerWrapper::Cleanup", "Could not remove TEMP: " + ec.message() + " — may still be mounted.");
    return true;
}

//Returns every mountpoint at or under Prefix from /proc/self/mountinfo, deepest-first.
std::vector<std::string> ContainerWrapper::MountpointsUnder(const std::filesystem::path &Prefix)
{
    std::vector<std::string> Result;
    if (Prefix.empty()) return Result;

    std::ifstream Mounts("/proc/self/mountinfo");
    if (!Mounts.is_open()) return Result;

    //mountinfo escapes space/tab/newline/backslash as 3-digit octal (\040 etc.); decode them.
    auto Unescape = [](const std::string &In) -> std::string
    {
        std::string Out; Out.reserve(In.size());
        for (size_t i = 0; i < In.size(); ++i)
        {
            if (In[i] == '\\' && i + 3 < In.size()
                && In[i+1] >= '0' && In[i+1] <= '7'
                && In[i+2] >= '0' && In[i+2] <= '7'
                && In[i+3] >= '0' && In[i+3] <= '7')
            {
                Out.push_back(static_cast<char>((In[i+1]-'0')*64 + (In[i+2]-'0')*8 + (In[i+3]-'0')));
                i += 3;
            }
            else Out.push_back(In[i]);
        }
        return Out;
    };

    const std::string PrefixStr = Prefix.string();
    std::string Line;
    while (std::getline(Mounts, Line))
    {
        //Field 5 (0-indexed 4) of a mountinfo line is the mount point.
        std::istringstream Iss(Line);
        std::string Field, MountPoint;
        for (int Idx = 0; Iss >> Field; ++Idx) { if (Idx == 4) { MountPoint = Field; break; } }
        if (MountPoint.empty()) continue;
        MountPoint = Unescape(MountPoint);

        if (MountPoint == PrefixStr || MountPoint.rfind(PrefixStr + "/", 0) == 0)
            Result.push_back(MountPoint);
    }

    //Deepest-first (longest path first) so children unmount before their parents.
    std::sort(Result.begin(), Result.end(),
              [](const std::string &A, const std::string &B){ return A.size() > B.size(); });
    return Result;
}

//Clears a previous run's leftovers under TempPath before a new build. See header for the
//save-safety contract (never wipe while a mount under TempPath is still live).
void ContainerWrapper::CleanStaleRuntime(const std::filesystem::path &TempPath)
{
    if (TempPath.empty()) return;

    std::vector<std::string> StaleMounts = MountpointsUnder(TempPath);
    const bool Exists = std::filesystem::exists(TempPath);
    if (StaleMounts.empty() && !Exists) return; //nothing left over — clean slate

    if (!StaleMounts.empty())
        LogWarn("ContainerWrapper::CleanStaleRuntime",
                "Found " + std::to_string(StaleMounts.size()) + " stale mount(s) under " +
                TempPath.string() + " from a previous run; clearing before mount.");

    //Lazy-detach every stale mount (deepest-first). Lazy avoids the "target is busy" errors a
    //non-lazy unmount spews while a lingering wineserver still holds handles; the actual teardown
    //then finishes in the background, so we VERIFY it has fully cleared below before touching disk.
    for (const std::string &Mount : StaleMounts)
        ContainerWrapper::RunCommand("fusermount3", {"-uz", Mount}, SystemToolEnv());

    //Save-safety gate (mirrors Cleanup): poll until nothing under TempPath is mounted, so remove_all
    //can never traverse a still-live PERSIST bind into PackagePath/USERDATA. Wait, don't assume.
    bool Clear = StaleMounts.empty();
    for (int Attempt = 0; !Clear && Attempt < 30; ++Attempt) //~6s max for lazy unmounts to settle
    {
        QThread::msleep(200);
        Clear = MountpointsUnder(TempPath).empty();
    }
    if (!Clear)
    {
        LogErr("ContainerWrapper::CleanStaleRuntime",
               "Stale mounts under TEMP did not clear in time; leaving it in place to protect USERDATA.");
        return;
    }

    if (Exists)
    {
        std::error_code Ec;
        std::filesystem::remove_all(TempPath, Ec);
        if (Ec) LogWarn("ContainerWrapper::CleanStaleRuntime", "Could not remove stale TEMP: " + Ec.message());
        else if (!StaleMounts.empty() || Exists)
            LogSucc("ContainerWrapper::CleanStaleRuntime", "Stale runtime cleared.");
    }
}
