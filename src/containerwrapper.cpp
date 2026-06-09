#include "containerwrapper.h"
#include "commonutils.h"
#include "jsonoperations.h"
#include "registrywrapper.h"
#include <random>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <QThread>
#include <QMetaObject>

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

//TO-DO:
//CREATE SEPARATE CLASS FOR VFS
//CREATE WRAPPER CLASS FOR REGISTRY

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
    if (this->ContainerParams.RunnerComponents.is_array() && !this->ContainerParams.RunnerComponents.empty())
    {
        if (!this->MANIFESTJSON.contains("COMPONENTS") || !this->MANIFESTJSON["COMPONENTS"].is_array())
            this->MANIFESTJSON["COMPONENTS"] = nlohmann::ordered_json::array();
        for (auto &C : this->ContainerParams.RunnerComponents)
            this->MANIFESTJSON["COMPONENTS"].push_back(C);
        LogOut("ContainerWrapper::InitializeContainer", "Folded " + std::to_string(this->ContainerParams.RunnerComponents.size()) + " runner component(s) into the pool.");
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

    //Wine-specific pre-VFS steps: the prefix must exist before layers are stacked on top.
    //Registry patches and pre-VFS FileEdits are applied here so they land in DefPrefixPath —
    //the lowest-priority VFS layer — and WRITELAYER can shadow them if the user has their own values.
    if (WineMode)
    {
        this->InitializeDefPrefix(this->ContainerParams);
        this->ApplyBaseRegEdits(this->ContainerParams);
        //Seed persisted registry-key subtrees into DEFPREFIX AFTER base RegEdits, so the user's saved
        //state wins over the package defaults. No-op unless RegKeyPersist subcomponents are declared.
        this->SeedPersistRegKeys(this->ContainerParams);
        this->ProcessFileEdits(this->ContainerParams, false);
    }

    //Seed any persisted registry into the ephemeral WRITELAYER so it shadows DEFPREFIX.
    //No-op under PersistAll (durable RW branch already holds the reg files).
    if (ContainerParams.PersistRegistry)
        this->SeedPersistRegistry(this->ContainerParams);

    //Seed any persisted PersistFiles into the WRITELAYER so they shadow their lower layers.
    //No-op under PersistAll or when none are declared.
    if (!ContainerParams.PersistFiles.empty())
        this->SeedPersistFiles(this->ContainerParams);

    //Build the layer-spec (DEFPREFIX + VFS subcomponents target-rooted + PERSIST dirs as RW
    //passthrough) and mount it as a single vidyagodfs filesystem at RuntimePath.
    if (!this->MountVFS(this->ContainerParams)) return false;
    this->CheckCaseConflicts(ContainerParams.RuntimePath);

    //Wine-specific post-mount steps: DLL overrides, OVERRIDE FileEdits, and OVERRIDE reg patches
    //operate on the mounted runtime (COW directly to WRITELAYER) — they win unconditionally.
    if (WineMode)
    {
        this->ProcessDLLOverrides(this->ContainerParams);
        this->ProcessFileEdits(this->ContainerParams, true);
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

        //Resolve the chosen variant's ENDPOINTS into the build target list.
        ContainerParams.Endpoints = FindEndpointsForVariant(MANIFESTJSON, ContainerParams.subgame_id, ContainerParams.VariantID);
        LogOut("ContainerWrapper::DecideComponent", "Variant '" + ContainerParams.VariantID + "' resolved to " + std::to_string(ContainerParams.Endpoints.size()) + " endpoint(s).");
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
//ContainerParams.CustomVariables, keyed by the namespaced token "COMPONENTID.KEY". Components are
//walked in Recipe order, so a later component's var overrides an earlier identical namespaced token.
//
//Resolution priority for each variable (highest to lowest):
//  1. ContainerParams.VariableOverrides — set from --var COMP.KEY=VALUE CLI flags or UI picker
//  2. GlobalConfigJSON["USERSETTINGS"][PackageUID]["VARIABLES"] — persisted user choices
//  3. DEFAULT from the CustomVar definition
bool ContainerWrapper::ResolveCustomVariables(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams, nlohmann::ordered_json GlobalConfigJSON)
{
    //Helper: resolve a single namespaced KEY/DEFAULT pair through the priority chain.
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

    //Resolve a single CustomVar subcomponent under namespace NS = "COMPONENTID." — two-layer pipeline:
    //  Layer 1: StringVariableSubstitution expands %ScreenWidth%, %PackagePath%, and any
    //           already-resolved CustomVar (GetVariablesMap() is rebuilt per-var).
    //  Layer 2: TranslateCustomVarValue converts the display value to raw storage (e.g. dword).
    //DISPLAY is a UI-only flag; all vars resolve here regardless.
    auto ResolveCustomVar = [&](const nlohmann::ordered_json &CV, const std::string &NS)
    {
        std::string Bare = CV.value("KEY", std::string());
        if (Bare.empty()) return;
        std::string Key     = NS + Bare;                            // namespaced token name
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
    //components win (they re-assign the same namespaced key).
    LogOut("ContainerWrapper::ResolveCustomVariables", "Resolving CustomVar subcomponents...");
    for (const std::string &CompID : ContainerParams.Recipe)
    {
        int Idx = FindComponentIndex(MANIFESTJSON, CompID);
        if (Idx == -1) continue;
        const auto &Comp = MANIFESTJSON["COMPONENTS"][Idx];
        if (!Comp.contains("SUBCOMPONENTS") || !Comp["SUBCOMPONENTS"].is_array()) continue;
        const std::string NS = CompID + ".";
        for (const auto &S : Comp["SUBCOMPONENTS"])
            if (S.is_object() && S.value("TYPE", std::string()) == "CustomVar")
                ResolveCustomVar(S, NS);
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

//Returns all variants listed under SUBGAMES[SubgameID].VARIANTS.
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
        if (V.contains("ENDPOINTS") && V["ENDPOINTS"].is_array())
            for (auto &E : V["ENDPOINTS"])
                if (E.is_string()) Info.Endpoints.push_back(std::string(E));
        Variants.push_back(Info);
    }
    return Variants;
}

//Returns the ENDPOINTS array of the variant matching { SubgameID, VariantID }.
//Returns an empty vector if not found.
std::vector<std::string> ContainerWrapper::FindEndpointsForVariant(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID, const std::string &VariantID)
{
    std::vector<std::string> Endpoints;
    int SubgameIdx = FindGameIndex(MANIFESTJSON, SubgameID);
    if (SubgameIdx == -1) return Endpoints;
    auto &Subgame = MANIFESTJSON["GAMES"][SubgameIdx];
    if (!Subgame.contains("VARIANTS") || !Subgame["VARIANTS"].is_array()) return Endpoints;
    for (auto &V : Subgame["VARIANTS"])
    {
        if (V.value("VARIANT_ID", std::string()) != VariantID) continue;
        if (V.contains("ENDPOINTS") && V["ENDPOINTS"].is_array())
            for (auto &E : V["ENDPOINTS"])
                if (E.is_string()) Endpoints.push_back(std::string(E));
        break;
    }
    return Endpoints;
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

//Returns the configured GLOBAL_RUNNERS registry directories (Settings.GlobalRunners), with a leading
//"~" expanded to $HOME. Defaults to ~/.VidyaGod/GLOBAL_RUNNERS when unset.
static std::vector<std::string> GatherRegistryDirs(const nlohmann::ordered_json &GlobalConfigJSON)
{
    auto Expand = [](std::string P) -> std::string {
        if (!P.empty() && P[0] == '~') P = QDir::homePath().toStdString() + P.substr(1);
        return P;
    };
    std::vector<std::string> Dirs;
    if (GlobalConfigJSON.contains("Settings") && GlobalConfigJSON["Settings"].is_object()
        && GlobalConfigJSON["Settings"].contains("GlobalRunners") && GlobalConfigJSON["Settings"]["GlobalRunners"].is_array())
        for (const auto &D : GlobalConfigJSON["Settings"]["GlobalRunners"])
            if (D.is_string()) Dirs.push_back(Expand(std::string(D)));
    if (Dirs.empty())
        Dirs.push_back(QDir::cleanPath(QDir::homePath() + "/.VidyaGod/GLOBAL_RUNNERS").toStdString());
    return Dirs;
}

//Gathers every runner object from every package in every configured registry directory.
std::vector<nlohmann::ordered_json> ContainerWrapper::RegistryRunners(const nlohmann::ordered_json &GlobalConfigJSON)
{
    std::vector<nlohmann::ordered_json> Runners;
    for (const auto &RegDir : GatherRegistryDirs(GlobalConfigJSON))
    {
        QDir Dir(QString::fromStdString(RegDir));
        if (!Dir.exists()) continue;
        for (const QString &Sub : Dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        {
            nlohmann::ordered_json Pkg; std::vector<std::string> Warn;
            if (!JSONOps::AssembleManifest(Dir.filePath(Sub), Pkg, Warn)) continue;
            if (!JSONOps::HasRunners(Pkg)) continue;
            for (const auto &R : Pkg["RUNNERS"]) Runners.push_back(R);
        }
    }
    return Runners;
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

    //HOST_PLATFORM is package identity — the platform this package runs AS, matched against each
    //runner's GUEST_PLATFORM. (Free-form: "win32", "snes", "custom", …)
    ContainerParams.Platform = MANIFESTJSON.value("HOST_PLATFORM", std::string());

    //Game specific — only populated when a game was specified.
    int SubgameIdx = FindGameIndex(MANIFESTJSON, ContainerParams.subgame_id);
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

    //A candidate carries its runner json plus its owning package's COMPONENTS (so a registry runner's
    //ENDPOINTS / CustomVar subcomponents resolve). The launching package's own runners carry no extra
    //components — those already live in MANIFESTJSON.
    struct RunnerCandidate { nlohmann::ordered_json Runner; nlohmann::ordered_json Components; };
    std::vector<RunnerCandidate> Candidates;
    std::unordered_set<std::string> SeenRunnerIds;
    auto GatherRunners = [&](const nlohmann::ordered_json &Source, const nlohmann::ordered_json &Components)
    {
        if (!Source.contains("RUNNERS") || !Source["RUNNERS"].is_array()) return;
        for (const auto &R : Source["RUNNERS"])
        {
            bool Match = false;
            if (R.contains("GUEST_PLATFORM") && R["GUEST_PLATFORM"].is_array())
                for (const auto &P : R["GUEST_PLATFORM"])
                    if (P.is_string() && std::string(P) == ContainerParams.Platform) { Match = true; break; }
            if (!Match) continue;
            std::string RID = R.value("RUNNER_ID", std::string());
            if (!RID.empty() && SeenRunnerIds.count(RID)) continue;
            SeenRunnerIds.insert(RID);
            Candidates.push_back({ R, Components });
        }
    };
    //1. The launching package's own runners first (a bundle shadows a registry runner of the same id).
    GatherRunners(MANIFESTJSON, nlohmann::ordered_json::array());
    //2. Every runner package in each configured registry directory.
    for (const auto &RegDir : GatherRegistryDirs(GlobalConfigJSON))
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
            GatherRunners(Pkg, Comps);
        }
    }

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
        const nlohmann::ordered_json &SelectedRunner = Selected->Runner;
        ContainerParams.RunnerComponents = Selected->Components; //folded into the pool by InitializeContainer
        //Use explicit null guards — direct JSON-to-string conversion throws on null values.
        ContainerParams.RunnerID         = SelectedRunner.value("RUNNER_ID", std::string());
        ContainerParams.RunnerName       = (SelectedRunner.contains("NAME")       && SelectedRunner["NAME"].is_string())       ? std::string(SelectedRunner["NAME"])       : "";
        ContainerParams.RunnerExecutable = (SelectedRunner.contains("EXECUTABLE") && SelectedRunner["EXECUTABLE"].is_string()) ? std::string(SelectedRunner["EXECUTABLE"]) : "";
        std::string RunnerTypeStr        = (SelectedRunner.contains("TYPE")       && SelectedRunner["TYPE"].is_string())       ? std::string(SelectedRunner["TYPE"])       : "custom";
        //Map the string type to the enum so the rest of the code can branch without string comparisons.
        if (RunnerTypeStr == "wine")           ContainerParams.RunnerTypeEnum = RunnerType::Wine;
        else if (RunnerTypeStr == "emulator")  ContainerParams.RunnerTypeEnum = RunnerType::Emulator;
        else if (RunnerTypeStr == "custom")    ContainerParams.RunnerTypeEnum = RunnerType::Custom;
        else                                   ContainerParams.RunnerTypeEnum = RunnerType::Native;
        if (SelectedRunner.contains("ENV"))        ContainerParams.RunnerEnv = SelectedRunner["ENV"];
        if (SelectedRunner.contains("REMOVE_ENV")) for (auto &E : SelectedRunner["REMOVE_ENV"]) ContainerParams.RunnerRemoveEnv.push_back(std::string(E));
        if (SelectedRunner.contains("ARGS"))       for (auto &A : SelectedRunner["ARGS"])       ContainerParams.RunnerArgs.push_back(std::string(A));
        //The runner's own components join the recipe (mounted as base) — see CreateRecipe.
        if (SelectedRunner.contains("ENDPOINTS") && SelectedRunner["ENDPOINTS"].is_array())
            for (auto &E : SelectedRunner["ENDPOINTS"]) if (E.is_string()) ContainerParams.RunnerEndpoints.push_back(std::string(E));
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

    ContainerParams.RuntimePath                         = ContainerParams.TempPath / "RUNTIME";
    LogOut("ContainerWrapper::DeriveContainerParams", "RuntimePath: " + ContainerParams.RuntimePath.string());

    ContainerParams.WriteLayerPath                      = ContainerParams.TempPath / "WRITELAYER";
    LogOut("ContainerWrapper::DeriveContainerParams", "WriteLayerPath: " + ContainerParams.WriteLayerPath.string());

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
        ContainerParams.DefPrefixPath                   = ContainerParams.TempPath / "DEFPREFIX";
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
    VariablesMap["WriteLayerPath"] = this->WriteLayerPath;
    VariablesMap["UserDataPath"] = this->UserDataPath;
    VariablesMap["TempPath"] = this->TempPath;
    VariablesMap["ProgramPath"] = this->ProgramPath;
    VariablesMap["DefPrefixPath"] = this->DefPrefixPath;
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
int ContainerWrapper::RunCommand(std::string Program, std::vector<std::string> Arguments, QProcessEnvironment ProcessEnvironment)
{
    auto toQStringList = [](const std::vector<std::string>& v)
    {
        QStringList l; for (auto& s : v) l << QString::fromStdString(s); return l;
    };

    QStringList ArgumentsQList = toQStringList(Arguments);
    LogOut("ContainerWrapper::RunCommand", "Running program " + Program + " with arguments: " + ArgumentsQList.join(" ").toStdString());

    QProcess Process;
    Process.setProcessEnvironment(ProcessEnvironment);
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

//Applies all non-OVERRIDE RegEdit subcomponents directly into the DEFPREFIX hive files. Replaces the
//former CreateFlatRegPatchJSON → CreateRegPatchFiles → `reg import` pipeline: no .reg generation, no
//runner shell-out. Runs pre-VFS so DEFPREFIX remains the lowest union layer.
bool ContainerWrapper::ApplyBaseRegEdits(struct ContainerParams &ContainerParams)
{
    if (!HasRegEdits(ContainerParams, /*WantOverride=*/false)) return true; // nothing to apply

    LogOut("ContainerWrapper::ApplyBaseRegEdits", "Applying base RegEdits directly to DEFPREFIX hives.");

    //No wineserver quiesce is needed: InitializeDefPrefix runs `umu-run wineboot` with
    //PROTON_VERB=waitforexitandrun, so Proton tears the prefix's wineserver down as part of session
    //cleanup before that call returns. The prefix is therefore quiescent here and our direct edits to
    //the hive files survive (verified: a known RegEdit value persists in DEFPREFIX/system.reg after a
    //real launch). (Proton also doesn't expose `wineserver` as an umu-run target anyway.)
    RegistryWrapper RW;
    RW.LoadPrefix(ContainerParams.DefPrefixPath);
    RW.ApplyRegEdits(ContainerParams.SubComponentsArray, /*WantOverride=*/false);
    if (!RW.SavePrefix(ContainerParams.DefPrefixPath))
    {
        LogErr("ContainerWrapper::ApplyBaseRegEdits", "Failed to write DEFPREFIX hives.");
        return false;
    }
    LogSucc("ContainerWrapper::ApplyBaseRegEdits", "Base RegEdits applied to DEFPREFIX.");
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
    //Initialising UMU prefix.
    LogOut("ContainerWrapper::InitializeDefPrefix", "Initialising DefPrefix, path: " + ContainerParams.DefPrefixPath.string());
    std::filesystem::create_directories(ContainerParams.DefPrefixPath);

    QProcessEnvironment RunProcessEnvironment = QProcessEnvironment::systemEnvironment();
    QProcess * RunProcess = new QProcess;
    RunProcess->setProgram(QString::fromStdString(ContainerParams.RunnerExecutable));
    RunProcess->setArguments({"wineboot"});

    //RunProcessEnvironment.insert("PROTONPATH", this->Paths["ProtonPath"]);
    RunProcessEnvironment.insert("WINEPREFIX", QString::fromStdString(ContainerParams.DefPrefixPath));
    RunProcessEnvironment.insert("GAMEID", "0");
    //PROTON_VERB=waitforexitandrun ensures wineboot completes before we proceed.
    RunProcessEnvironment.insert("PROTON_VERB", "waitforexitandrun");
    //Remove LD_LIBRARY_PATH to prevent host libraries conflicting with Proton's bundled ones.
    RunProcessEnvironment.remove("LD_LIBRARY_PATH");

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

        std::filesystem::path Source = ContainerParams.PackagePath / Sub.value("PATH", std::string());
        std::string Target = WineRoot;
        if (Sub.contains("TARGET") && Sub["TARGET"].is_string() && !std::string(Sub["TARGET"]).empty())
        {
            std::string T = Sub["TARGET"];
            Target = Target.empty() ? T : (Target + "/" + T);
        }
        Layers.push_back({{"type", LType}, {"source", Source.string()}, {"target", Target}, {"rw", false}});
    }

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

    std::filesystem::create_directories(ContainerParams.RuntimePath);
    std::filesystem::path SpecPath = ContainerParams.TempPath / "vidyagodfs.spec.json";
    {
        std::ofstream Out(SpecPath);
        if (!Out) { LogErr("ContainerWrapper::MountVFS", "Cannot write spec " + SpecPath.string()); return false; }
        Out << Spec.dump(2);
    }

    const std::string Helper = VidyagodfsPath();
    LogOut("ContainerWrapper::MountVFS", "Mounting via " + Helper + " (spec " + SpecPath.string() + ")");
    int result = ContainerWrapper::RunCommand(Helper, {SpecPath, ContainerParams.RuntimePath, "-o", "auto_cache"});
    LogOut("ContainerWrapper::MountVFS", "vidyagodfs spawn exit: " + std::to_string(result));

    //The helper forks after the mount is established; poll until RuntimePath shows as a mount.
    bool Mounted = false;
    for (int i = 0; i < 50; ++i)
    {
        if (!ContainerWrapper::MountpointsUnder(ContainerParams.RuntimePath).empty()) { Mounted = true; break; }
        QThread::msleep(100);
    }
    if (!Mounted)
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

//Merges each previously-persisted RegKeyPersist subtree from the durable store
//(UserDataPath/__REGKEYS__) directly into the DEFPREFIX hives, so the durable key shadows the
//default the game would otherwise see. Done in place on DEFPREFIX (the lowest, regenerated-each-launch
//union layer) rather than copying a whole hive into WRITELAYER — only the one key is touched.
//Wine-only, pre-VFS. No-op under PersistAll, with no declared keys, or before anything is persisted.
bool ContainerWrapper::SeedPersistRegKeys(struct ContainerParams &ContainerParams)
{
    if (ContainerParams.PersistAll || ContainerParams.PersistRegKeys.empty()) return true;
    const std::filesystem::path Store = ContainerParams.UserDataPath / "__REGKEYS__";
    if (!std::filesystem::exists(Store)) return true; // nothing persisted yet

    RegistryWrapper Durable;
    Durable.LoadPrefix(Store);

    RegistryWrapper Prefix;
    Prefix.LoadPrefix(ContainerParams.DefPrefixPath);

    int Seeded = 0;
    for (const std::string &RegPath : ContainerParams.PersistRegKeys)
        if (Prefix.MergeKeyFrom(Durable, RegPath)) { LogOut("ContainerWrapper::SeedPersistRegKeys", "Seeded " + RegPath); ++Seeded; }

    if (Seeded > 0 && !Prefix.SavePrefix(ContainerParams.DefPrefixPath))
        LogWarn("ContainerWrapper::SeedPersistRegKeys", "Failed to write seeded DEFPREFIX hives.");
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
    for (const auto& FilePath : std::filesystem::recursive_directory_iterator(DirectoryPath))
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

    if(!NoConflict)
    {
        std::ostringstream oss;
        std::for_each(CaseConflictList.begin(), CaseConflictList.end(),[&oss](const std::string& s){ oss << s << '\n'; });
        std::cerr << "CASE CONFLICTS:\n" << oss.str() << std::endl;
        QMessageBox::warning(nullptr, "CASE CONFLICTS!", QString::fromStdString(oss.str()));
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

//Processes FileEdit subcomponents in two passes, mirroring the RegEdit pre/post-VFS split.
//  OverridePass=false (pre-VFS): writes to DefPrefixPath. DefPrefixPath is the lowest-priority
//    VFS layer, so WRITELAYER (the RW top layer) naturally shadows it if the user already has the
//    file — their value sticks without any explicit logic.
//  OverridePass=true  (post-VFS): writes to RuntimePath via the union, COW-ing directly into
//    WRITELAYER — wins unconditionally over any previous state.
//MUST BE RUN AFTER VARIABLE SUBSTITUTION (already done in BuildSubComponentsArray).
bool ContainerWrapper::ProcessFileEdits(struct ContainerParams &ContainerParams, bool OverridePass)
{
    std::filesystem::path BasePath = OverridePass ? ContainerParams.RuntimePath : ContainerParams.DefPrefixPath;
    LogOut("ContainerWrapper::ProcessFileEdits",
           std::string(OverridePass ? "Post-VFS" : "Pre-VFS") + " FileEdit pass. Base: " + BasePath.string());

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

    QProcessEnvironment RunProcessEnvironment = QProcessEnvironment::systemEnvironment();

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

    // For emulator/native runners: prepend RunnerArgs (template-expanded), then exe
    // For wine runners: exe first, then ExeArgs
    QStringList Arguments;
    if (ContainerParams.RunnerTypeEnum != RunnerType::Wine)
    {
        //Emulator/Custom/Native: runner args first, then the exe/ROM path (if any), then ExeArgs.
        for (std::string Arg : ContainerParams.RunnerArgs)
        {
            ContainerWrapper::StringVariableSubstitution(Arg, ContainerParams.GetVariablesMap());
            Arguments.append(QString::fromStdString(Arg));
        }
        if (!FinalExe.empty())
            Arguments.append(QString::fromStdString(FinalExe));
        //ExeArgs from the VariantDefinition (e.g. -c config.cfg for GemRB, or extra flags).
        if (OverrideExe.empty())
            for (std::string Arg : ContainerParams.ExeArgs)
                Arguments.append(QString::fromStdString(Arg));
    }
    else
    {
        //Wine: exe path is the first argument; game's own args follow.
        //ExeArgs are skipped when OverrideExe is set (tooling scenario, not a game launch).
        Arguments.append(QString::fromStdString(FinalExe));
        if (OverrideExe.empty() && !ContainerParams.ExeArgs.empty())
        {
            for (std::string Arg : ContainerParams.ExeArgs)
            {
                Arguments.append(QString::fromStdString(Arg));
            }
        }
    }
    RunProcess.setArguments(Arguments);
    RunProcess.setProcessEnvironment(RunProcessEnvironment);

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

    std::cout << RunProcess.readAllStandardError().toStdString() << std::endl;
    std::cout << RunProcess.readAllStandardOutput().toStdString() << std::endl;

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
        //SIGKILL the group first, then the direct process as a fallback.
        QProcess::execute("kill", {"-9", "--", "-" + QString::number(Pid)});

        //Also walk /proc to kill any stragglers that escaped the group.
        QProcess::execute("bash", {"-c",
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
            if (ContainerWrapper::RunCommand("fusermount3", {"-u", *It}) == 0)
                Unmounted = true;
        }
        if (!Unmounted)
        {
            LogErr("ContainerWrapper::Cleanup", "DURABLE mount still busy after retries: " + It->string());
            //Lazy-detach so it eventually clears, but mark the wipe unsafe for this run.
            ContainerWrapper::RunCommand("fusermount3", {"-uz", *It});
            DurableUnmountOk = false;
        }
    }

    //3. Ephemeral VFS mounts: lazy unmount is fine (their RW/source is all under TempPath).
    for (const std::filesystem::path &UnmountPath : ContainerParams.CleanupUnmountPaths)
        ContainerWrapper::RunCommand("fusermount3", {"-uz", UnmountPath});

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
        ContainerWrapper::RunCommand("fusermount3", {"-uz", Mount});

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
