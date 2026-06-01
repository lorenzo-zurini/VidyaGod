#include "containerwrapper.h"
#include "commonutils.h"
#include <random>

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
//Returns the array index into MANIFEST["SUBGAMES"], or -1 if not found.
int ContainerWrapper::FindSubgameIndex(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID)
{
    for (int i = 0; i < (int)MANIFESTJSON["SUBGAMES"].size(); i++)
    {
        if (!MANIFESTJSON["SUBGAMES"][i]["SUBGAMEID"].is_null() && MANIFESTJSON["SUBGAMES"][i]["SUBGAMEID"] == SubgameID)
        {
            return i;
        }
    }
    LogErr("ContainerWrapper::FindSubgameIndex", "Subgame ID not found: " + SubgameID);
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
    //the lowest-priority VFS layer — and USERDATA can shadow them if the user has their own values.
    if (WineMode)
    {
        this->InitializeDefPrefix(this->ContainerParams);
        this->CreateFlatRegPatchJSON(this->ContainerParams);
        this->CreateRegPatchFiles(this->ContainerParams);
        this->MergeRegPatchFiles(this->ContainerParams);
        this->ProcessFileEdits(this->ContainerParams, false);
    }

    //Mount all filesystem subcomponents into TEMP, then assemble and mount the final union.
    this->PreMountFilesystemComponents(this->ContainerParams, WineMode);
    if (!this->FinalizeVFSString(this->ContainerParams)) return false;
    this->MountVFS(this->ContainerParams);
    this->CheckCaseConflicts(ContainerParams.RuntimePath);

    //Wine-specific post-mount steps: DLL overrides, OVERRIDE FileEdits, and OVERRIDE reg patches
    //operate on the mounted runtime (COW directly to USERDATA) — they win unconditionally.
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
    LogOut("ContainerWrapper::DecideComponent", "Deciding component: subgame_id: " + ContainerParams.subgame_id + " component_id: " + ContainerParams.component_id);

    if (ContainerParams.subgame_id.empty() && ContainerParams.component_id.empty())
    {
        LogOut("ContainerWrapper::DecideComponent", "Neither subgame nor component specified, mounting defprefix.");
        return true;
    }
    else if (!ContainerParams.subgame_id.empty() && ContainerParams.component_id.empty())
    {
        int SubgameIdx = FindSubgameIndex(MANIFESTJSON, ContainerParams.subgame_id);
        if (SubgameIdx == -1) return false;

        //Find the RECOMMENDED entrypoint; fall back to the first one.
        std::string ResolvedComponentID;
        std::vector<EntrypointInfo> Entrypoints = GetAvailableEntrypoints(MANIFESTJSON, ContainerParams.subgame_id);
        for (auto &EP : Entrypoints)
            if (EP.IsRecommended) { ResolvedComponentID = EP.ComponentID; break; }
        if (ResolvedComponentID.empty() && !Entrypoints.empty())
        {
            ResolvedComponentID = Entrypoints.front().ComponentID;
            LogWarn("ContainerWrapper::DecideComponent", "No RECOMMENDED entrypoint found, falling back to first: '" + ResolvedComponentID + "'");
        }

        if (!ResolvedComponentID.empty())
        {
            ContainerParams.component_id = ResolvedComponentID;
            LogOut("ContainerWrapper::DecideComponent", "Only subgame specified, resolved component_id: " + ContainerParams.component_id);
        }
        LogOut("ContainerWrapper::DecideComponent", "Running subgame " + std::string(MANIFESTJSON["SUBGAMES"][SubgameIdx]["TITLE"]));
        return true;
    }
    else if (ContainerParams.subgame_id.empty() && !ContainerParams.component_id.empty())
    {
        int ComponentIdx = FindComponentIndex(MANIFESTJSON, ContainerParams.component_id);
        if (ComponentIdx == -1) return false;
        LogOut("ContainerWrapper::DecideComponent", "Running component " + std::string(MANIFESTJSON["COMPONENTS"][ComponentIdx]["NAME"]));
        return true;
    }
    else
    {
        LogOut("ContainerWrapper::DecideComponent", "Running subgame " + ContainerParams.subgame_id + " component " + ContainerParams.component_id);
        return true;
    }
    return false;
}

//Builds the ordered Recipe by walking the PARENTCOMPONENT chain upward from component_id.
//The chain is collected leaf-first then reversed, so Recipe[0] is the root ancestor
//and the last entry is component_id itself. This ensures layers are applied base-first.
bool ContainerWrapper::CreateRecipe(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams)
{
    //CREATE RECIPE BY RESOLVING COMPONENT DEPENDENCY CHAIN
    ContainerParams.Recipe.clear();
    std::string CurrentID = ContainerParams.component_id;
    while (!CurrentID.empty())
    {
        int Idx = FindComponentIndex(MANIFESTJSON, CurrentID);
        if (Idx == -1) break;

        ContainerParams.Recipe.push_back(CurrentID);

        auto &ParentField = MANIFESTJSON["COMPONENTS"][Idx]["PARENTCOMPONENT"];
        if (ParentField.is_null() || ParentField == "")
        {
            LogOut("ContainerWrapper::CreateRecipe", "No parent, ending recipe.");
            break;
        }
        CurrentID = ParentField;
    }
    //Reverse so the root component's subcomponents are mounted first (lowest VFS priority).
    std::reverse(ContainerParams.Recipe.begin(), ContainerParams.Recipe.end());
    LogOut("ContainerWrapper::CreateRecipe", "Recipe: " + [&](const std::vector<std::string>& v){ std::string r; for (const auto &x : v) r += x + " "; return r;}(ContainerParams.Recipe));
    return true;
}

//Resolves all configurable variables into ContainerParams.CustomVariables.
//Two sources are merged (runner SETTINGS first, then CustomVar subcomponents —
//CustomVar wins on key collision since it is more package-specific):
//  1. SETTINGS array on the selected runner definition (runner-level options)
//  2. MANIFEST["CUSTOMVARS"] top-level array (game-level options)
//
//Resolution priority for each variable (highest to lowest):
//  1. ContainerParams.VariableOverrides — set from --var KEY=VALUE CLI flags or UI picker
//  2. GlobalConfigJSON["USERSETTINGS"][PackageUID]["VARIABLES"] — persisted user choices
//  3. DEFAULT from the SETTINGS / CustomVar definition
bool ContainerWrapper::ResolveCustomVariables(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams, nlohmann::ordered_json GlobalConfigJSON)
{
    //Helper: resolve a single KEY/DEFAULT pair through the priority chain.
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

    //--- Runner SETTINGS (runner-level configurable options) ---
    //Find the selected runner in MANIFEST RUNNERS first, then GlobalConfig, mirroring
    //the same lookup order used in DeriveContainerParams.
    LogOut("ContainerWrapper::ResolveCustomVariables", "Resolving runner SETTINGS...");
    auto ProcessRunnerSettings = [&](nlohmann::ordered_json &Source)
    {
        if (!Source.contains("RUNNERS") || !Source["RUNNERS"].contains(ContainerParams.Platform)) return;
        for (auto &Runner : Source["RUNNERS"][ContainerParams.Platform])
        {
            if (Runner.value("NAME", std::string()) != ContainerParams.RunnerName) continue;
            if (!Runner.contains("SETTINGS")) return;
            for (auto &Setting : Runner["SETTINGS"])
            {
                std::string Key = Setting.value("KEY", std::string());
                if (Key.empty()) continue;
                ContainerParams.CustomVariables[Key] = ResolveOne(Key, Setting.value("DEFAULT", std::string()));
            }
            return;
        }
    };
    ProcessRunnerSettings(MANIFESTJSON);
    if (!GlobalConfigJSON.is_null()) ProcessRunnerSettings(GlobalConfigJSON);

    //--- MANIFEST["CUSTOMVARS"] — two-layer pipeline (per-variable, in declaration order) ---
    //Layer 1: StringVariableSubstitution — expands %ScreenWidth%, %PackagePath%, and any
    //          previously-resolved CustomVar (GetVariablesMap() is called per-iteration so
    //          each resolved var is immediately available to subsequent ones).
    //Layer 2: TranslateCustomVarValue — converts the display value to raw storage format
    //          (e.g. "1920" + dword → "dword:00000780").
    //DISPLAY is a UI-only flag; all vars are resolved here regardless.
    LogOut("ContainerWrapper::ResolveCustomVariables", "Resolving MANIFEST CUSTOMVARS...");
    if (MANIFESTJSON.contains("CUSTOMVARS") && MANIFESTJSON["CUSTOMVARS"].is_array())
    {
        for (auto &CV : MANIFESTJSON["CUSTOMVARS"])
        {
            std::string Key     = CV.value("KEY",     std::string());
            std::string VarType = CV.value("VARTYPE",  std::string("string"));
            if (Key.empty()) continue;

            // --- random type: pick one value from OPTIONS on every launch ---
            // Skips USERSETTINGS so the selection is fresh each time.
            // CLI VariableOverrides still take highest priority (useful for debugging).
            if (VarType == "random")
            {
                if (ContainerParams.VariableOverrides.count(Key))
                {
                    ContainerParams.CustomVariables[Key] = ContainerParams.VariableOverrides.at(Key);
                    LogOut("ContainerWrapper::ResolveCustomVariables", "  " + Key + " = [random:override] " + ContainerParams.CustomVariables[Key]);
                }
                else if (CV.contains("OPTIONS") && CV["OPTIONS"].is_array() && !CV["OPTIONS"].empty())
                {
                    auto &Opts = CV["OPTIONS"];
                    static std::mt19937 Rng(std::random_device{}());
                    std::uniform_int_distribution<size_t> Dist(0, Opts.size() - 1);
                    std::string Picked = Opts[Dist(Rng)].value("VALUE", std::string());
                    ContainerWrapper::StringVariableSubstitution(Picked, ContainerParams.GetVariablesMap());
                    ContainerParams.CustomVariables[Key] = Picked;
                    LogOut("ContainerWrapper::ResolveCustomVariables", "  " + Key + " = [random] " + ContainerParams.CustomVariables[Key]);
                }
                else
                {
                    LogWarn("ContainerWrapper::ResolveCustomVariables", "  " + Key + ": random type has no OPTIONS, leaving empty.");
                    ContainerParams.CustomVariables[Key] = "";
                }
                continue;
            }

            // Resolve raw display value through priority chain.
            std::string Raw = ResolveOne(Key, CV.value("DEFAULT", std::string()));

            // Layer 1 — token substitution (called per-var so prior CustomVars are available).
            ContainerWrapper::StringVariableSubstitution(Raw, ContainerParams.GetVariablesMap());

            // Layer 2 — type translation.
            ContainerParams.CustomVariables[Key] = TranslateCustomVarValue(Raw, VarType);
            LogOut("ContainerWrapper::ResolveCustomVariables", "  " + Key + " = " + ContainerParams.CustomVariables[Key]);
        }
    }
    LogSucc("ContainerWrapper::ResolveCustomVariables", "Resolved " + std::to_string(ContainerParams.CustomVariables.size()) + " custom variable(s).");
    return true;
}

//Returns all entrypoints listed under SUBGAMES[SubgameID].ENTRYPOINTS.
//ComponentName is derived by looking up LASTCOMPONENT in COMPONENTS.
std::vector<EntrypointInfo> ContainerWrapper::GetAvailableEntrypoints(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID)
{
    std::vector<EntrypointInfo> Entrypoints;
    int SubgameIdx = FindSubgameIndex(MANIFESTJSON, SubgameID);
    if (SubgameIdx == -1) return Entrypoints;
    auto &Subgame = MANIFESTJSON["SUBGAMES"][SubgameIdx];
    if (!Subgame.contains("ENTRYPOINTS") || !Subgame["ENTRYPOINTS"].is_array()) return Entrypoints;
    for (auto &EP : Subgame["ENTRYPOINTS"])
    {
        std::string LastComp = EP.value("LASTCOMPONENT", std::string());
        // Derive ComponentName from the LASTCOMPONENT lookup.
        std::string CompName;
        int CompIdx = FindComponentIndex(MANIFESTJSON, LastComp);
        if (CompIdx != -1)
            CompName = MANIFESTJSON["COMPONENTS"][CompIdx].value("NAME", LastComp);
        else
            CompName = LastComp;
        EntrypointInfo Info;
        Info.EntrypointID   = EP.value("ENTRYPOINT_ID", std::string());
        Info.ComponentName  = CompName;
        Info.ComponentID    = LastComp;
        Info.IsRecommended  = EP.value("RECOMMENDED", false);
        Entrypoints.push_back(Info);
    }
    return Entrypoints;
}

//Returns the LASTCOMPONENT value of the entrypoint matching { SubgameID, EntrypointID }.
//Returns empty string if not found.
std::string ContainerWrapper::FindComponentForEntrypoint(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID, const std::string &EntrypointID)
{
    int SubgameIdx = FindSubgameIndex(MANIFESTJSON, SubgameID);
    if (SubgameIdx == -1) return "";
    auto &Subgame = MANIFESTJSON["SUBGAMES"][SubgameIdx];
    if (!Subgame.contains("ENTRYPOINTS") || !Subgame["ENTRYPOINTS"].is_array()) return "";
    for (auto &EP : Subgame["ENTRYPOINTS"])
    {
        if (EP.value("ENTRYPOINT_ID", std::string()) != EntrypointID) continue;
        return EP.value("LASTCOMPONENT", std::string());
    }
    return "";
}

//Finds the entrypoint in MANIFESTJSON matching ContainerParams.ExecutableID (= EntrypointID)
//under SUBGAMES[ContainerParams.subgame_id].ENTRYPOINTS and populates exe/work-dir/args fields.
//Must be called AFTER BuildSubComponentsArray and BEFORE BuildContainerRuntime.
bool ContainerWrapper::ResolveExecutableDefinition(const nlohmann::ordered_json &MANIFESTJSON, struct ContainerParams &ContainerParams)
{
    if (ContainerParams.ExecutableID.empty())
    {
        LogWarn("ContainerWrapper::ResolveExecutableDefinition", "No EntrypointID set — skipping.");
        return true;
    }
    // Find the entrypoint with matching ENTRYPOINT_ID in the subgame.
    int SubgameIdx = FindSubgameIndex(MANIFESTJSON, ContainerParams.subgame_id);
    nlohmann::ordered_json Resolved;
    if (SubgameIdx != -1 && MANIFESTJSON["SUBGAMES"][SubgameIdx].contains("ENTRYPOINTS"))
    {
        for (auto &EP : MANIFESTJSON["SUBGAMES"][SubgameIdx]["ENTRYPOINTS"])
        {
            if (EP.value("ENTRYPOINT_ID", std::string()) == ContainerParams.ExecutableID)
            { Resolved = EP; break; }
        }
    }
    if (Resolved.is_null() || Resolved.empty())
    {
        LogErr("ContainerWrapper::ResolveExecutableDefinition", "No entrypoint found for ENTRYPOINT_ID='" + ContainerParams.ExecutableID + "' in subgame '" + ContainerParams.subgame_id + "'");
        return false;
    }
    LogSucc("ContainerWrapper::ResolveExecutableDefinition", "Resolved entrypoint: " + ContainerParams.ExecutableID);
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
//CustomVar subcomponents are skipped — they are handled by ResolveCustomVariables.
//MANIFEST order is preserved, which determines VFS layer stacking order.
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

//Fills all derived fields in ContainerParams from MANIFEST and GlobalConfigJSON.
//Must be called after DecideComponent so ContainerParams.subgame_id and platform are set.
//
//Runner resolution order:
//  1. USERSETTINGS[PackageUID]["PREFERRED_RUNNER"]  (per-package user override)
//  2. SUBGAMES[SubgameIdx]["RECOMMENDED_RUNNER"]   (manifest recommendation)
//  2. First runner in RUNNERS[Platform]             (any available runner)
//  3. Hardcoded fallback: umu-run / Wine            (backwards compatibility)
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

    //(Sub)game specific — only populated when a subgame_id was specified.
    int SubgameIdx = FindSubgameIndex(MANIFESTJSON, ContainerParams.subgame_id);
    if (!ContainerParams.subgame_id.empty() && SubgameIdx != -1)
    {
        ContainerParams.GameName                        = MANIFESTJSON["SUBGAMES"][SubgameIdx]["TITLE"];
        //UMUID lives under METADATA since v2 of the manifest schema.
        auto &Meta      = MANIFESTJSON["SUBGAMES"][SubgameIdx]["METADATA"];
        auto &UMUIDField = (Meta.is_object() && Meta.contains("UMUID")) ? Meta["UMUID"] : MANIFESTJSON["SUBGAMES"][SubgameIdx]["UMUID"];
        //UMUID "0" means no Steam AppID — umu-run accepts this as a generic Wine launch.
        ContainerParams.UMUID                           = (!UMUIDField.is_null() && UMUIDField.is_string()) ? std::string(UMUIDField) : "0";
        ContainerParams.Platform                        = MANIFESTJSON["SUBGAMES"][SubgameIdx]["PLATFORM"];
    }
    else
    {
        //No subgame specified — default to Windows platform so a Wine prefix can still be initialised.
        ContainerParams.UMUID                           = "0";
        ContainerParams.Platform                        = "Microsoft Windows";
    }
    LogOut("ContainerWrapper::DeriveContainerParams", "GameName: " + ContainerParams.GameName);
    LogOut("ContainerWrapper::DeriveContainerParams", "UMUID: " + ContainerParams.UMUID);
    LogOut("ContainerWrapper::DeriveContainerParams", "Platform: " + ContainerParams.Platform);

    // Runner resolution: USERSETTINGS > RECOMMENDED_RUNNER > first available
    std::string PreferredRunner;
    {
        auto US = GetPackageUserSettings(GlobalConfigJSON, ContainerParams.PackageUID);
        if (US.contains("PREFERRED_RUNNER") && US["PREFERRED_RUNNER"].is_string())
            PreferredRunner = std::string(US["PREFERRED_RUNNER"]);
    }
    //Search for the preferred runner by name, checking MANIFEST RUNNERS first so packages
    //can define their own runners (e.g. bundled interpreters) that shadow GlobalConfig ones.
    //Falls back to the first runner in the list for the platform if no name match is found.
    nlohmann::ordered_json SelectedRunner;
    auto TrySelectFromSource = [&](nlohmann::ordered_json &Source)
    {
        if (!Source.contains("RUNNERS") || !Source["RUNNERS"].contains(ContainerParams.Platform)) return;
        auto &Runners = Source["RUNNERS"][ContainerParams.Platform];
        for (auto &Runner : Runners)
        {
            std::string RunnerName = (Runner.contains("NAME") && Runner["NAME"].is_string()) ? std::string(Runner["NAME"]) : "";
            if (!PreferredRunner.empty() && RunnerName == PreferredRunner)
            {
                SelectedRunner = Runner;
                return;
            }
        }
        if (SelectedRunner.is_null() && !Runners.empty())
            SelectedRunner = Runners[0];
    };

    //MANIFEST RUNNERS take priority — allows packages to define self-contained runners.
    TrySelectFromSource(MANIFESTJSON);
    //Fall back to GlobalConfig (system-wide runners, fetched from the online registry).
    if (SelectedRunner.is_null()) TrySelectFromSource(GlobalConfigJSON);

    if (!SelectedRunner.is_null())
    {
        //Use explicit null guards — direct JSON-to-string conversion throws on null values.
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
    }
    else
    {
        // No runner found — fall back to umu-run/Wine for backwards compatibility
        ContainerParams.RunnerExecutable = "umu-run";
        ContainerParams.RunnerTypeEnum   = RunnerType::Wine;
        LogWarn("ContainerWrapper::DeriveContainerParams", "No runner found for platform '" + ContainerParams.Platform + "', falling back to umu-run.");
    }
    LogOut("ContainerWrapper::DeriveContainerParams", "Runner: " + ContainerParams.RunnerName + " (" + ContainerParams.RunnerExecutable + ")");

    //System Variables — queried live from Qt so they reflect the actual display at launch time.
    ContainerParams.ScreenWidth                         = std::to_string(QGuiApplication::primaryScreen()->geometry().width());
    LogOut("ContainerWrapper::DeriveContainerParams", "ScreenWidth: " + ContainerParams.ScreenWidth);

    ContainerParams.ScreenHeight                        = std::to_string(QGuiApplication::primaryScreen()->geometry().height());
    LogOut("ContainerWrapper::DeriveContainerParams", "ScreenHeight: " + ContainerParams.ScreenHeight);

    //All paths are derived from PackagePath so packages are fully self-contained and relocatable.
    ContainerParams.RuntimePath                         = ContainerParams.PackagePath / "RUNTIME";
    LogOut("ContainerWrapper::DeriveContainerParams", "RuntimePath: " + ContainerParams.RuntimePath.string());

    ContainerParams.MetaDataPath                        = ContainerParams.PackagePath / "METADATA";
    LogOut("ContainerWrapper::DeriveContainerParams", "MetaDataPath: " + ContainerParams.MetaDataPath.string());

    ContainerParams.PackageFilesPath                    = ContainerParams.PackagePath / "PACKAGEFILES";
    LogOut("ContainerWrapper::DeriveContainerParams", "PackageFilesPath: " + ContainerParams.PackageFilesPath.string());

    ContainerParams.UserDataPath                        = ContainerParams.PackagePath / "USERDATA";
    LogOut("ContainerWrapper::DeriveContainerParams", "UserDataPath: " + ContainerParams.UserDataPath.string());

    ContainerParams.TempPath                            = ContainerParams.PackagePath / "TEMP";
    LogOut("ContainerWrapper::DeriveContainerParams", "TempPath: " + ContainerParams.TempPath.string());

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

    //Resolve the default EntrypointID from the RECOMMENDED entrypoint; fall back to first.
    //Exe/args/workdir resolution is deferred to ResolveExecutableDefinition().
    if (!ContainerParams.subgame_id.empty() && SubgameIdx != -1)
    {
        auto Entrypoints = GetAvailableEntrypoints(MANIFESTJSON, ContainerParams.subgame_id);
        for (auto &EP : Entrypoints)
            if (EP.IsRecommended) { ContainerParams.ExecutableID = EP.EntrypointID; break; }
        if (ContainerParams.ExecutableID.empty() && !Entrypoints.empty())
            ContainerParams.ExecutableID = Entrypoints.front().EntrypointID;
        LogOut("ContainerWrapper::DeriveContainerParams", "EntrypointID (recommended): " + ContainerParams.ExecutableID);
    }
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
    VariablesMap["MetaDataPath"] = this->MetaDataPath;
    VariablesMap["PackageFilesPath"] = this->PackageFilesPath;
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

//Flattens all RegEdit subcomponents from SubComponentsArray into FlatRegPatch.
//Structure: FlatRegPatch[architecture]["32"|"64"][regPath][valueName] = value
//Null values are stored as nullptr so CreateRegPatchFiles can emit an empty string entry.
//If a subcomponent has no KEYVALUES, the registry key is created with no values (key-only).
bool ContainerWrapper::CreateFlatRegPatchJSON(struct ContainerParams &ContainerParams)
{
    ContainerParams.FlatRegPatch.clear();
    LogOut("ContainerWrapper::CreateFlatRegPatchJSON", "Creating registry patch.");
    for (int i = 0; i < ContainerParams.SubComponentsArray.size(); i++)
    {
        nlohmann::ordered_json SubComponentJSON = ContainerParams.SubComponentsArray[i];
        if (SubComponentJSON["TYPE"] == "RegEdit" && !SubComponentJSON.value("OVERRIDE", false))
        {
            if (SubComponentJSON.contains("KEYVALUES"))
            {
                for (auto Item : SubComponentJSON["KEYVALUES"].items())
                {
                    if (!SubComponentJSON["KEYVALUES"][Item.key()].is_null())
                    {
                        ContainerParams.FlatRegPatch[std::string(SubComponentJSON["ARCHITECTURE"])][std::string(SubComponentJSON["REGPATH"])][Item.key()] = Item.value();
                    }
                    else
                    {
                        //Explicit null means "write an empty string" in the .reg file.
                        ContainerParams.FlatRegPatch[std::string(SubComponentJSON["ARCHITECTURE"])][std::string(SubComponentJSON["REGPATH"])][Item.key()] = nullptr;
                    }
                }
            }
            else
            {
                //No KEYVALUES — store nullptr so the key header is written but no values follow.
                ContainerParams.FlatRegPatch[std::string(SubComponentJSON["ARCHITECTURE"])][std::string(SubComponentJSON["REGPATH"])] = nullptr;
            }
        }
    }
    LogSucc("ContainerWrapper::CreateFlatRegPatchJSON", "Successfully created FlatRegPatch.");
    //std::cout << ContainerParams.FlatRegPatch.dump(4) << std::endl;
    return true;
}

//Writes RegPatch32.reg and RegPatch64.reg to DefPrefixPath/drive_c/ from FlatRegPatch.
//
//Value type mapping:
//  null    → ""              (empty string, deletes value if already present in Wine)
//  bool    → dword:00000001 / dword:00000000
//  int     → dword:XXXXXXXX  (8-digit hex)
//  string starting with "dword:" or "hex:" → written verbatim (pre-formatted by caller)
//  other string → double-quoted with backslashes escaped as \\
//
//Root key abbreviations (HKLM, HKCU) are expanded to full names for .reg file compatibility.
bool ContainerWrapper::CreateRegPatchFiles(struct ContainerParams &ContainerParams)
{
    //Expands HKLM/HKCU short forms to the full registry root key names required by regedit.
    auto normalizeRootKey = [](std::string path) {
        if (path.rfind("HKLM", 0) == 0) path.replace(0, 4, "HKEY_LOCAL_MACHINE");
        else if (path.rfind("HKCU", 0) == 0) path.replace(0, 4, "HKEY_CURRENT_USER");
        return path;
    };

    auto writeArch = [&](const std::string& arch) -> bool
    {
        const std::filesystem::path filePath = ContainerParams.TempPath / "DEFPREFIX" / "drive_c" / ("RegPatch" + arch + ".reg");
        std::filesystem::create_directories(filePath.parent_path());

        std::ofstream out(filePath, std::ios::out | std::ios::trunc);
        if (!out) return false;

        // Header
        out << "Windows Registry Editor Version 5.00\n\n";

        // Iterate all registry paths
        for (const auto& [rawPath, keySet] :
             ContainerParams.FlatRegPatch[arch].items())
        {
            std::string regPath = normalizeRootKey(rawPath);
            out << "[" << regPath << "]\n";

            // If keySet is null, just create the key (no values)
            if (keySet.is_object())
            {
                for (const auto& [key, value] : keySet.items())
                {
                    out << "\"" << key << "\"=";

                    if (value.is_null())
                    {
                        out << "\"\""; // empty string for null
                    }
                    else if (value.is_boolean())
                    {
                        out << "dword:" << (value.get<bool>() ? "00000001" : "00000000");
                    }
                    else if (value.is_number_integer() || value.is_number_unsigned())
                    {
                        out << "dword:"
                            << std::hex << std::setw(8) << std::setfill('0')
                            << value.get<uint32_t>()
                            << std::dec;
                    }
                    else if (value.is_string())
                    {
                        std::string s = value.get<std::string>();

                        // Already a literal (dword: or hex:) → write as-is
                        if (s.rfind("dword:", 0) == 0 || s.rfind("hex:", 0) == 0)
                        {
                            out << s;
                        }
                        else
                        {
                            //Escape backslashes so the .reg parser sees single backslashes.
                            std::string escaped;
                            for (char c : s)
                                escaped += (c == '\\') ? "\\\\" : std::string(1, c);
                            out << "\"" << escaped << "\"";
                        }
                    }

                    out << "\n";
                }
            }

            out << "\n"; // blank line between keys
        }

        return true;
    };

    // Write both 32-bit and 64-bit patches
    return writeArch("32") && writeArch("64");
}

//Imports the two generated .reg files into the Wine prefix using `reg import`.
//LD_LIBRARY_PATH is removed to prevent system libraries from shadowing Wine's own;
//GAMEID=0 tells umu-run to use a generic (non-Steam) Wine session for the import.
bool ContainerWrapper::MergeRegPatchFiles(struct ContainerParams &ContainerParams)
{
    LogOut("ContainerWrapper::MergeRegPatchFiles", "Merging RegPatchFiles.");

    // Prepare environment
    QProcessEnvironment ProcessEnvironment = QProcessEnvironment::systemEnvironment();
    ProcessEnvironment.insert("WINEPREFIX", QString::fromStdString(ContainerParams.DefPrefixPath));
    ProcessEnvironment.insert("GAMEID", "0");
    ProcessEnvironment.remove("LD_LIBRARY_PATH");

    int Merge32Complete = RunCommand(ContainerParams.RunnerExecutable, {"reg", "import", ContainerParams.DefPrefixPath / "drive_c" / "RegPatch32.reg"}, ProcessEnvironment);
    LogOut("ContainerWrapper::MergeRegPatchFiles", "Merged RegPatch32.reg. EXIT CODE: " + std::to_string(Merge32Complete));
    int Merge64Complete = RunCommand(ContainerParams.RunnerExecutable, {"reg", "import", ContainerParams.DefPrefixPath / "drive_c" / "RegPatch64.reg"}, ProcessEnvironment);
    LogOut("ContainerWrapper::MergeRegPatchFiles", "Merged RegPatch64.reg. EXIT CODE: " + std::to_string(Merge64Complete));
    return true;
}

//Applies OVERRIDE:true RegEdit subcomponents directly into the mounted runtime.
//Called post-MountVFS so the values land in the RW USERDATA layer, taking precedence over
//both DEFPREFIX and any previously COW'd registry state from prior game sessions.
bool ContainerWrapper::ApplyOverrideRegEdits(struct ContainerParams &ContainerParams)
{
    //Collect OVERRIDE:true RegEdit entries into a temporary flat patch.
    nlohmann::ordered_json OverridePatch = nlohmann::ordered_json::object();
    bool AnyOverride = false;

    auto normalizeRootKey = [](std::string path) {
        if (path.rfind("HKLM", 0) == 0) path.replace(0, 4, "HKEY_LOCAL_MACHINE");
        else if (path.rfind("HKCU", 0) == 0) path.replace(0, 4, "HKEY_CURRENT_USER");
        return path;
    };

    for (auto &Sub : ContainerParams.SubComponentsArray)
    {
        if (Sub.value("TYPE", std::string()) != "RegEdit") continue;
        if (!Sub.value("OVERRIDE", false)) continue;
        AnyOverride = true;
        std::string Arch    = Sub.value("ARCHITECTURE", std::string("32"));
        std::string RegPath = Sub.value("REGPATH", std::string());
        if (RegPath.empty()) continue;
        if (Sub.contains("KEYVALUES") && Sub["KEYVALUES"].is_object())
            for (auto &[K, V] : Sub["KEYVALUES"].items())
                OverridePatch[Arch][RegPath][K] = V;
        else
            OverridePatch[Arch][RegPath] = nullptr;
    }

    if (!AnyOverride) return true;
    LogOut("ContainerWrapper::ApplyOverrideRegEdits", "Applying OVERRIDE RegEdits to runtime...");

    //Write and import one .reg file per architecture.
    for (const std::string &Arch : {"32", "64"})
    {
        if (!OverridePatch.contains(Arch)) continue;
        std::filesystem::path RegFile = ContainerParams.RuntimePath / "drive_c" / ("OverridePatch" + Arch + ".reg");
        {
            std::ofstream Out(RegFile, std::ios::out | std::ios::trunc);
            if (!Out) { LogErr("ContainerWrapper::ApplyOverrideRegEdits", "Could not write " + RegFile.string()); continue; }
            Out << "Windows Registry Editor Version 5.00\n\n";
            for (auto &[RawPath, KeySet] : OverridePatch[Arch].items())
            {
                Out << "[" << normalizeRootKey(RawPath) << "]\n";
                if (KeySet.is_object())
                    for (auto &[Key, Value] : KeySet.items())
                    {
                        Out << "\"" << Key << "\"=";
                        if      (Value.is_null())    Out << "\"\"";
                        else if (Value.is_boolean()) Out << "dword:" << (Value.get<bool>() ? "00000001" : "00000000");
                        else if (Value.is_number_integer() || Value.is_number_unsigned())
                            Out << "dword:" << std::hex << std::setw(8) << std::setfill('0') << Value.get<uint32_t>() << std::dec;
                        else if (Value.is_string())
                        {
                            std::string S = Value.get<std::string>();
                            if (S.rfind("dword:", 0) == 0 || S.rfind("hex:", 0) == 0) Out << S;
                            else { std::string Esc; for (char c : S) Esc += (c == '\\') ? "\\\\" : std::string(1,c); Out << "\"" << Esc << "\""; }
                        }
                        Out << "\n";
                    }
                Out << "\n";
            }
        }

        QProcessEnvironment Env = QProcessEnvironment::systemEnvironment();
        Env.insert("WINEPREFIX", QString::fromStdString(ContainerParams.RuntimePath));
        Env.insert("GAMEID", "0");
        Env.remove("LD_LIBRARY_PATH");
        int ExitCode = RunCommand(ContainerParams.RunnerExecutable, {"reg", "import", RegFile}, Env);
        LogOut("ContainerWrapper::ApplyOverrideRegEdits", "Imported OverridePatch" + Arch + ".reg, exit=" + std::to_string(ExitCode));
        std::filesystem::remove(RegFile);
    }

    return true;
}

//=====================================================================================================================================================================
//                                                                          VFSWRAPPER CLASS
//=====================================================================================================================================================================

//Initializes the Wine prefix at DefPrefixPath by running `wineboot` via the runner.
//Exclusive for wine / proton runners.
//Must be made to work with any runner, not just UMU...
//
//After a successful wineboot, DefPrefixPath is added to VFSString as the base (lowest-priority)
//read-only layer so the prefix's drive_c structure is visible in the final union mount.
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
        //Add the freshly created prefix as the lowest-priority VFS layer (base of the stack).
        if (!ContainerWrapper::AddToVFSString(ContainerParams, ContainerParams.DefPrefixPath))
        {
            return false;
        }
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

//Pre-mounts each VFS subcomponent into an isolated TEMP/[i] staging directory,
//then adds that directory to VFSString.
//
//Staging in numbered directories keeps layers independent — each gets its own
//mount point so fuse-zip/bindfs don't share or conflict.
//
//WineMode=true wraps each layer's target inside drive_c/PackageUID so the game
//files appear at the correct Windows path when Wine accesses drive_c.
//
//Layer type behavior:
//  VFSZipLayer  — fuse-zip mounts the zip read-only; the mount point goes into CleanupUnmountPaths.
//  VFSDirLayer  — bindfs bind-mounts the directory read-only; same cleanup registration.
//  VFSFileLayer — hard-linked into the staging dir (no FUSE mount); falls back to copy if
//                 cross-device. The staging dir is registered in CleanupDeletePaths.
bool ContainerWrapper::PreMountFilesystemComponents(struct ContainerParams &ContainerParams, bool WineMode)
{
    LogOut("ContainerWrapper::PreMountFilesystemComponents", "Processing filesystem Subcomponents.");
    for (int i = 0; i < (int)ContainerParams.SubComponentsArray.size(); i++)
    {
        nlohmann::ordered_json SubComponentJSON = ContainerParams.SubComponentsArray[i];
        std::string Type = SubComponentJSON.value("TYPE", std::string());

        if (Type != "VFSZipLayer" && Type != "VFSDirLayer" && Type != "VFSFileLayer")
            continue;

        //Each layer gets its own numbered staging directory under TEMP.
        std::filesystem::path PreMountPath = ContainerParams.TempPath / ("[" + std::to_string(i) + "]");
        std::filesystem::create_directories(PreMountPath);

        //In Wine mode the game content must land under drive_c/PackageUID so that
        //Wine's C: drive maps to the correct location inside the union mount.
        std::filesystem::path TargetPath = WineMode
            ? PreMountPath / "drive_c" / ContainerParams.PackageUID
            : PreMountPath;

        //Optional TARGET subpath lets a layer be mounted into a subdirectory of the program dir.
        if (SubComponentJSON.contains("TARGET") && !SubComponentJSON["TARGET"].is_null() && !SubComponentJSON["TARGET"].empty())
            TargetPath = TargetPath / std::string(SubComponentJSON["TARGET"]);

        std::filesystem::path SourcePath = ContainerParams.PackageFilesPath / std::string(SubComponentJSON["PATH"]);

        if (Type == "VFSZipLayer")
        {
            std::filesystem::create_directories(TargetPath);
            LogOut("ContainerWrapper::PreMountFilesystemComponents", "Mounting VFSZipLayer " + SourcePath.string() + " at " + TargetPath.string());
            //fuse-zip exit code 0 = success; non-zero = mount failed.
            if (!ContainerWrapper::RunCommand("fuse-zip", {"-r", SourcePath, TargetPath}))
            {
                ContainerParams.CleanupUnmountPaths.push_back(TargetPath);
                if (!ContainerWrapper::AddToVFSString(ContainerParams, PreMountPath))
                    return false;
            }
            else
            {
                LogErr("ContainerWrapper::PreMountFilesystemComponents", "FAILED TO MOUNT VFSZipLayer " + SourcePath.string());
            }
        }
        else if (Type == "VFSDirLayer")
        {
            std::filesystem::create_directories(TargetPath);
            LogOut("ContainerWrapper::PreMountFilesystemComponents", "Mounting VFSDirLayer " + SourcePath.string() + " at " + TargetPath.string());
            if (!ContainerWrapper::RunCommand("bindfs", {"-r", SourcePath, TargetPath}))
            {
                ContainerParams.CleanupUnmountPaths.push_back(TargetPath);
                if (!ContainerWrapper::AddToVFSString(ContainerParams, PreMountPath))
                    return false;
            }
            else
            {
                LogErr("ContainerWrapper::PreMountFilesystemComponents", "FAILED TO MOUNT VFSDirLayer " + SourcePath.string());
            }
        }
        else if (Type == "VFSFileLayer")
        {
            std::filesystem::create_directories(TargetPath);
            std::filesystem::path DestPath = TargetPath / SourcePath.filename();
            std::error_code ec;
            std::filesystem::create_hard_link(SourcePath, DestPath, ec);
            if (ec)
            {
                if (ec.value() == EXDEV)
                {
                    // Cross-device — fall back to a full copy.
                    LogWarn("ContainerWrapper::PreMountFilesystemComponents",
                            "Hardlink failed (cross-device), copying VFSFileLayer: " + SourcePath.filename().string());
                    std::filesystem::copy_file(SourcePath, DestPath, std::filesystem::copy_options::overwrite_existing, ec);
                }
                if (ec)
                {
                    LogErr("ContainerWrapper::PreMountFilesystemComponents",
                           "VFSFileLayer failed: " + ec.message() + " — " + SourcePath.string());
                    continue;
                }
            }
            else
            {
                LogOut("ContainerWrapper::PreMountFilesystemComponents",
                       "Hardlinked VFSFileLayer: " + SourcePath.filename().string());
            }
            // Register the staging dir for cleanup regardless of hard-link vs copy.
            ContainerParams.CleanupDeletePaths.push_back(PreMountPath);
            if (!ContainerWrapper::AddToVFSString(ContainerParams, PreMountPath))
                return false;
        }
    }
    LogOut("ContainerWrapper::PreMountFilesystemComponents", "Filesystem subcomponent pre-mount complete.");
    return true;
}

//Adds NewPath=RO to the front of VFSString so it has higher union priority than existing layers.
//unionfs resolves conflicts by reading from the leftmost (first) branch, so layers added
//later via this function take precedence over earlier ones.
bool ContainerWrapper::AddToVFSString(struct ContainerParams &ContainerParams, std::string NewPath)
{
    if (ContainerParams.VFSString.empty())
    {
        ContainerParams.VFSString = NewPath + "=RO";
        LogOut("ContainerWrapper::AddToVFSString", "Initialized VFSString with " + NewPath);
        return true;
    }
    else
    {
        //Prepend so the newest layer sits at the top of the union stack.
        ContainerParams.VFSString = NewPath + "=RO:" + ContainerParams.VFSString;
        LogOut("ContainerWrapper::AddToVFSString", "Added " + NewPath + " to VFSString");
        return true;
    }
}

//Prepends USERDATA=RW as the writable top layer of the union stack.
//This gives the copy-on-write semantics: reads fall through to read-only layers,
//writes go to USERDATA and persist across sessions.
//Returns false if VFSString is empty (no read-only layers were added first).
bool ContainerWrapper::FinalizeVFSString(struct ContainerParams &ContainerParams)
{
    if (ContainerParams.ReadOnlyVFS)
    {
        //ReadOnly mode: no writable layer is prepended; the entire runtime is read-only.
        LogOut("ContainerWrapper::FinalizeVFSString", "UnionFSString: RUNTIME IS READONLY!");
    }
    else
    {
        if (ContainerParams.VFSString.empty())
        {
            LogErr("ContainerWrapper::FinalizeVFSString", "VFSString is null or empty...");
            return false;
        }
        else
        {
            //Ensure USERDATA exists before it is used as a unionfs branch.
            std::filesystem::create_directories(ContainerParams.UserDataPath);
            ContainerParams.VFSString = ContainerParams.UserDataPath.string() + "=RW:" + ContainerParams.VFSString;
            LogOut("ContainerWrapper::FinalizeVFSString", "Finalized VFSString. Final VFSSTRING:");
            std::cout << ContainerParams.VFSString << std::endl;
            return true;
        }
    }
    return true;
}


//Mounts the finalized VFSString onto RuntimePath using unionfs.
//Options: cow (copy-on-write) and uid=1000 (ensures the mounted files are accessible
//to the default non-root user even if the source archives are owned differently).
//RuntimePath is added to CleanupUnmountPaths so Cleanup() will unmount it.
bool ContainerWrapper::MountVFS(struct ContainerParams &ContainerParams)
{
    if (ContainerParams.VFSString.empty())
    {
        LogErr("ContainerWrapper::MountVFS", "VFSString is null or empty...");
        return false;
    }
    LogOut("ContainerWrapper::MountVFS", "Proceeding with VFS mount.....");
    std::filesystem::create_directories(ContainerParams.RuntimePath);
    int result = ContainerWrapper::RunCommand("unionfs", {"-o", "cow", "-o", "uid=1000", ContainerParams.VFSString, ContainerParams.RuntimePath});
    LogOut("ContainerWrapper::MountVFS", "EXIT CODE: " + std::to_string(result));

    if (!result)
    {
        //Success
        ContainerParams.CleanupUnmountPaths.push_back(ContainerParams.RuntimePath);
        LogSucc("ContainerWrapper::MountVFS", "Successfully mounted VFS.");
        return true;
    }
    else
    {
        //Failiure
        LogErr("ContainerWrapper::MountVFS", "Failed to mount VFS.");
        return false;
    }
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
//    VFS layer, so USERDATA (the RW top layer) naturally shadows it if the user already has the
//    file — their value sticks without any explicit logic.
//  OverridePass=true  (post-VFS): writes to RuntimePath via the union, COW-ing directly into
//    USERDATA — wins unconditionally over any previous state.
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
//WorkDir falls back to RuntimePath (VFS) or PackageFilesPath if the configured path
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
    //gracefully: prefer the RuntimePath (VFS root) if VFS was used, otherwise PackageFilesPath.
    std::filesystem::path FinalWorkDir = ContainerParams.WorkDirPathComplete;
    if (!std::filesystem::exists(FinalWorkDir))
    {
        FinalWorkDir = ContainerParams.UsesVFS ? ContainerParams.RuntimePath : ContainerParams.PackageFilesPath;
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

//Unmounts all FUSE filesystems registered in CleanupUnmountPaths (in registration order),
//then removes the RUNTIME and TEMP directory trees.
//fusermount -uz: lazy unmount (-z) so the call succeeds even if the mount is still busy.
bool ContainerWrapper::Cleanup()
{
    for (std::filesystem::path UnmountPath : ContainerParams.CleanupUnmountPaths)
    {
        ContainerWrapper::RunCommand("fusermount", {"-uz", UnmountPath});
    }
    //Explicitly remove VFSFileLayer staging directories (hard-link or copy; no FUSE involved).
    std::error_code ec;
    for (const std::filesystem::path &DeletePath : ContainerParams.CleanupDeletePaths)
    {
        std::filesystem::remove_all(DeletePath, ec);
        if (ec) LogWarn("ContainerWrapper::Cleanup", "Could not remove staging dir: " + DeletePath.string() + " — " + ec.message());
    }
    //remove_all can throw filesystem_error if a path is still a FUSE mountpoint (device busy).
    //Catch and log rather than crash — stale mounts should not bring down the launcher.
    std::filesystem::remove_all(this->ContainerParams.RuntimePath, ec);
    if (ec) LogWarn("ContainerWrapper::Cleanup", "Could not remove RUNTIME: " + ec.message() + " — may still be mounted.");
    std::filesystem::remove_all(this->ContainerParams.TempPath, ec);
    if (ec) LogWarn("ContainerWrapper::Cleanup", "Could not remove TEMP: " + ec.message());
    return true;
}
