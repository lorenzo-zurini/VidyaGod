#include "containerwrapper.h"
#include "commonutils.h"
#include "jsonoperations.h"
#include "processenv.h"
#include "registrywrapper.h"
#include "ipfswrapper.h"
#include "runnerwrapper.h"
#include "packagecatalog.h"
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

//Pure manifest queries + the VFS-layer helpers (IsVfsLayer/LayerType/ForEachVfsLayer/LayerLocator/
//ResolveLayerSource/MachinePlatform/Find*/...) now live in ManifestModel; the catalog/sharing service
//(Catalog/Sync/Import/Publish/RepositoryDirs/user-settings) lives in PackageCatalog. Bring both in unqualified
//so the launch-engine code below (which calls RepositoryDirs / GetPackageUserSettings) reads naturally.
using namespace ManifestModel;
using namespace PackageCatalog;

//True if any subcomponent is a matching-scope RegEdit (defined below; forward-declared for the post-mount
//installed-runner base-reg-edit pass in BuildContainerRuntime).
static bool HasRegEdits(const struct ContainerParams &ContainerParams, bool WantOverride);

//True if a raw (pre-substitution) runner ARG references the launch target via a content token. Used to drop
//the content-bearing args when swapping the target — an OverrideExe (tooling) or the wineboot prefix-init.
static bool ArgReferencesContent(const std::string &RawArg)
{
    return RawArg.find("%Content%") != std::string::npos
        || RawArg.find("%ContentPath%") != std::string::npos;
}

//The registry-hive directory under a layer/runtime root: the prefix root (CONTENT_ROOT up to /drive_c) is
//where system.reg/user.reg/userdef.reg live. "" for plain wine (hives at the root), "pfx" for proton.
static std::filesystem::path HiveDir(const struct ContainerParams &CP, const std::filesystem::path &Base)
{
    return CP.PrefixRoot.empty() ? Base : Base / CP.PrefixRoot;
}

//Stores the JSON references and immediately runs the full initialization pipeline.
//After construction, ContainerParams is fully populated and ready for BuildContainerRuntime().
ContainerWrapper::ContainerWrapper(nlohmann::ordered_json &Passed_GlobalConfigJSON, nlohmann::ordered_json &Passed_MANIFESTJSON, struct ContainerParams &Passed_ContainerParams)
    : ContainerParams(Passed_ContainerParams), GlobalConfigJSON(Passed_GlobalConfigJSON), MANIFESTJSON(Passed_MANIFESTJSON)   // match member declaration order (ContainerParams is declared first)
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
    //Everything is a node: resolve the whole container from the global node index (InitializeFromNode populates
    //the internal COMPONENTS pool the shared iterators consume). There is no MANIFESTJSON path any more.
    if (this->ContainerParams.NodeIdx && !this->ContainerParams.LaunchNodeId.empty())
        return this->InitializeFromNode();

    LogErr("ContainerWrapper::InitializeContainer", "No node graph supplied (NodeIdx/LaunchNodeId unset) — aborting.");
    return false;
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
        if (IsVfsLayer(Type))
        {
            ContainerParams.UsesVFS = true;
            break;
        }
    }

    const bool PrefixGen = ContainerParams.PrefixGenerate;

    //A runner that neither generates a prefix nor mounts VFS layers has no content to run — malformed.
    if (!PrefixGen && !ContainerParams.UsesVFS)
    {
        LogWarn("ContainerWrapper::BuildContainerRuntime", "No VFS layers found. Package may be malformed.");
        return false;
    }

    //Provision the Wine prefix (DEFPREFIX) — only when the runner asks for one. Left pristine; base edits
    //go to DEFAULTDATA below. One code path, no installed-vs-not branching for edits.
    if (PrefixGen)
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
    if (PrefixGen)
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

//(GetPackageUserSettings / SetPackageUserSetting moved to PackageCatalog — see packagecatalog.h.)


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
bool ContainerWrapper::ResolveCustomVariables(const nlohmann::ordered_json &MANIFESTJSON, struct ContainerParams &ContainerParams, const nlohmann::ordered_json &GlobalConfigJSON)
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
bool ContainerWrapper::DerivePersistence(const nlohmann::ordered_json &MANIFESTJSON, struct ContainerParams &ContainerParams)
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
            LogErr("ContainerWrapper::ResolveExecutableDefinition", "No variant found for VARIANT_ID='" + ContainerParams.VariantID + "' in subgame '" + ContainerParams.subgame_id + "'");
            return false;
        }
    }
    LogSucc("ContainerWrapper::ResolveExecutableDefinition", "Resolved exec for: " + ContainerParams.subgame_id);
    // The variant declares ONE universal target path — CONTENTPATH: the path (relative to the program mount) to
    // whatever the runner runs or loads (an exe, a ROM, a data root, or nothing for a self-contained runner).
    // The runner composes how it's used from %ContentPath% (relative) / %Content% (absolute host) in its ARGS.
    {
        std::string ExeStr = Resolved.value("CONTENTPATH", std::string());
        ContainerWrapper::StringVariableSubstitution(ExeStr, ContainerParams.GetVariablesMap());
        ContainerParams.ExePathRelative = std::filesystem::path(ExeStr);
    }
    ContainerParams.ExePathComplete = ContainerParams.ProgramPath / ContainerParams.ExePathRelative;
    LogOut("ContainerWrapper::ResolveExecutableDefinition", "ContentPath: " + ContainerParams.ExePathRelative.string());
    LogOut("ContainerWrapper::ResolveExecutableDefinition", "Content: " + ContainerParams.ExePathComplete.string());
    auto &WorkDirVal = Resolved["WORKDIR"];
    if (!WorkDirVal.is_null() && WorkDirVal.is_string() && !std::string(WorkDirVal).empty())
    {
        std::string WorkDirStr = std::string(WorkDirVal);
        ContainerWrapper::StringVariableSubstitution(WorkDirStr, ContainerParams.GetVariablesMap());
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
bool ContainerWrapper::BuildSubComponentsArray(const nlohmann::ordered_json &MANIFESTJSON, struct ContainerParams &ContainerParams)
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

//(Library/repo path helpers + the catalog/sync/import/publish service moved to PackageCatalog — see
//packagecatalog.h. The launch-engine code below calls PackageCatalog::RepositoryDirs / GetPackageUserSettings
//unqualified via the using-directive above.)

//(Catalog/RegistryRunners + git plumbing + Sync/Upsert/Reconcile moved to PackageCatalog — see packagecatalog.h.)

//=====================================================================================================================
//                                          NATIVE NODE-GRAPH LAUNCH
//=====================================================================================================================

//Derives the session paths from already-set ContainerParams fields (PackageUID/Platform/ContentRoot/
//PrefixGenerate/RunnerShipsBuild/UnifiedRuntime/RunnerVariantID/RunnerPackagePath). Pure of MANIFESTJSON.
bool ContainerWrapper::DerivePaths(struct ContainerParams &ContainerParams, const nlohmann::ordered_json &GlobalConfigJSON)
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
    StringVariableSubstitution(ContainerParams.ContentRoot, ContainerParams.GetVariablesMap());
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
    LogOut("ContainerWrapper::DerivePaths", "RuntimePath: " + ContainerParams.RuntimePath.string()
           + " | ContentRoot: " + ContainerParams.ContentRoot + " (PrefixRoot: '" + ContainerParams.PrefixRoot + "')");
    return true;
}

//Picks the best ROLE:"runner" node for a launch node (GUEST ∋ launch host, HOST==machine, executable available),
//priority: explicit RunnerID pin > USERSETTINGS PREFERRED_RUNNER > first qualifying (sorted node-id order).
const Node *ContainerWrapper::PickRunnerNode(const NodeIndex &Idx, const Node &Launch, const struct ContainerParams &CP,
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
bool ContainerWrapper::InitializeFromNode()
{
    auto &CP = this->ContainerParams;
    const NodeIndex &Idx = *CP.NodeIdx;
    const std::string LaunchId = CP.LaunchNodeId;
    const Node *Launch = Idx.Find(LaunchId);
    if (!Launch) { LogErr("ContainerWrapper::InitializeFromNode", "Launch node not found: " + LaunchId); return false; }
    if (!Launch->IsLaunchable()) LogWarn("ContainerWrapper::InitializeFromNode", "Node '" + LaunchId + "' is not ROLE:launchable.");

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
    const Node *RunnerNode = PickRunnerNode(Idx, *Launch, CP, this->GlobalConfigJSON);
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
    else LogErr("ContainerWrapper::InitializeFromNode", "No runner found for platform '" + CP.Platform + "'.");

    //Game content nodes (resolved order; runners + the launch node excluded), then the launch node's own layers.
    std::vector<std::string> Missing;
    for (const std::string &Id : ManifestModel::ResolveNodeOrder(Idx, LaunchId, CP.ModuleStates, &Missing))
    {
        if (Id == LaunchId) continue;
        const Node *N = Idx.Find(Id);
        if (!N || N->IsRunner()) continue;
        AddComponent(N);
    }
    for (const auto &M : Missing) LogWarn("ContainerWrapper::InitializeFromNode", "Unresolved parent: " + M);
    if (Launch->Layers.is_array() && !Launch->Layers.empty())
    { Components.push_back({{"COMPONENTID", LaunchId + "__self"}, {"SUBCOMPONENTS", AbsLayers(Launch)}}); CP.Recipe.push_back(LaunchId + "__self"); }

    //The internal component pool the generic iterators (BuildSubComponentsArray/ResolveCustomVariables/
    //DerivePersistence/BuildDefaultData) consume — built from nodes, never authored or read from disk.
    this->MANIFESTJSON = nlohmann::ordered_json::object();
    this->MANIFESTJSON["PACKAGEUID"]  = CP.PackageUID;
    this->MANIFESTJSON["PACKAGENAME"] = CP.PackageName;
    this->MANIFESTJSON["COMPONENTS"]  = Components;

    DerivePaths(CP, this->GlobalConfigJSON);
    this->ResolveCustomVariables(this->MANIFESTJSON, CP, this->GlobalConfigJSON);
    this->BuildSubComponentsArray(this->MANIFESTJSON, CP);
    this->DerivePersistence(this->MANIFESTJSON, CP);
    LogSucc("ContainerWrapper::InitializeFromNode", "Resolved node '" + LaunchId + "' (runner " + CP.RunnerName + ", "
            + std::to_string(CP.Recipe.size()) + " component(s)).");
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
    //The single mount root — where the VFS mounts; STEAM_COMPAT_DATA_PATH / WINEPREFIX point here.
    VariablesMap["RuntimePath"] = this->RuntimePath;
    VariablesMap["RunnerRuntimePath"] = this->RunnerRuntimePath;
    //The runner build's mount (installed-runner model); unified runners run from inside the game RUNTIME.
    VariablesMap["RunnerMount"] = this->UnifiedRuntime ? this->RuntimePath.string() : this->RunnerMountPath.string();
    VariablesMap["WriteLayerPath"] = this->WriteLayerPath;
    VariablesMap["UserDataPath"] = this->UserDataPath;
    VariablesMap["TempPath"] = this->TempPath;
    VariablesMap["ProgramPath"] = this->ProgramPath;
    VariablesMap["DefPrefixPath"] = this->DefPrefixPath;
    VariablesMap["DefaultData"] = this->DefaultDataPath;
    //Content primitives: %ContentPath% (relative to the program mount) and %Content% (absolute host path).
    //Authors compose guest paths from these, e.g. ARGS: "C:\\%PackageUID%\\%ContentPath%".
    VariablesMap["ContentPath"] = this->ExePathRelative;
    VariablesMap["Content"] = this->ExePathComplete;
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
    const bool HavePrefix = ContainerParams.PrefixGenerate;

    bool HaveBaseFileEdits = false;
    for (const auto &S : ContainerParams.SubComponentsArray)
        if (S.value("TYPE", std::string()) == "FileEdit" && S.value("OVERRIDE", false) == false)
            { HaveBaseFileEdits = true; break; }

    //Registry only exists when the runner generates a wine prefix; ROM/native runners have no hives.
    const bool HaveBaseReg = HavePrefix && HasRegEdits(ContainerParams, /*WantOverride=*/false);
    const std::filesystem::path RegKeyStore = ContainerParams.UserDataPath / "__REGKEYS__";
    const bool HavePersistKeys = HavePrefix && !ContainerParams.PersistAll && !ContainerParams.PersistRegKeys.empty()
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
        RW.LoadPrefix(HiveDir(ContainerParams, ContainerParams.DefPrefixPath));    // hives at <artifact>/<PrefixRoot>
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
        const std::filesystem::path HiveOut = HiveDir(ContainerParams, ContainerParams.DefaultDataPath);
        std::filesystem::create_directories(HiveOut, Ec);
        if (!RW.SavePrefix(HiveOut))                                                // mounts at root → lands at /<PrefixRoot>
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

    const std::filesystem::path Hives = HiveDir(ContainerParams, ContainerParams.RuntimePath);
    RegistryWrapper RW;
    RW.LoadPrefix(Hives);
    RW.ApplyRegEdits(ContainerParams.SubComponentsArray, /*WantOverride=*/true);
    if (!RW.SavePrefix(Hives))
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
    //proton difference is just the runner's EXECUTABLE/ARGS/ENV plus the prefix layout. The runner ENV points
    //its prefix var (WINEPREFIX / STEAM_COMPAT_DATA_PATH) at %RuntimePath%; during init we bind %RuntimePath%
    //to the DEF-prefix dir we're building, so the wineboot lands in DefPrefixPath (mounted whole as base later).
    LogOut("ContainerWrapper::InitializeDefPrefix", "Initialising DefPrefix, path: " + ContainerParams.DefPrefixPath.string());
    std::filesystem::create_directories(ContainerParams.DefPrefixPath);

    std::map<std::string, std::string> Vars = ContainerParams.GetVariablesMap();
    Vars["RuntimePath"] = ContainerParams.DefPrefixPath.string();

    //EXECUTABLE may reference %RunnerRuntimePath% (e.g. the Proton build's proton script).
    std::string Program = ContainerParams.RunnerExecutable;
    ContainerWrapper::StringVariableSubstitution(Program, Vars);

    QProcessEnvironment RunProcessEnvironment = SystemToolEnv();                 // system runner, not AppImage libs
    QProcess * RunProcess = new QProcess;
    RunProcess->setProgram(QString::fromStdString(Program));

    //Args: the runner's ARGS with the content target swapped for "wineboot" — the launcher verb (e.g. proton's
    //"waitforexitandrun"; empty for umu/wine) is kept, the content-bearing arg dropped, then wineboot appended.
    QStringList Arguments;
    for (const std::string &Raw : ContainerParams.RunnerArgs)
    {
        if (ArgReferencesContent(Raw)) continue;
        std::string Arg = Raw;
        ContainerWrapper::StringVariableSubstitution(Arg, Vars);
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
        ContainerWrapper::StringVariableSubstitution(ExpandedValue, Vars);
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

//(LayerLocator / ResolveLayerSource moved to ManifestModel — see manifestmodel.h.)

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
        if (!IsVfsLayer(Type)) continue;
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
        if (!IsVfsLayer(Type)) continue;
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
        if (!IsVfsLayer(Type)) continue;
        std::filesystem::path Local; std::string Cid;
        LayerLocator(Sub, ContainerParams.PackagePath, Local, Cid);
        std::error_code Ec;
        if (std::filesystem::exists(Local, Ec) || !Cid.empty()) continue;        // present, or a backend can provide it
        Ok = false;
        LogErr("ContainerWrapper::EnsureSources", "Missing layer content (no local file, no source): " + Local.string());
    }

    //An installed prefix-generating runner must have its one-time DEFPREFIX artifact (generated at import).
    if (ContainerParams.RunnerShipsBuild && ContainerParams.PrefixGenerate)
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
        if (!IsVfsLayer(Type)) continue;
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

//Builds the vidyagodfs JSON layer-spec from the resolved container: the DEFPREFIX base (when the runner
//generates a prefix), every VFS subcomponent rooted at CONTENT_ROOT + its TARGET (logically — no staging
//dirs), the PERSIST dirs as RW passthrough layers, and the writable top branch. Array order is union
//priority (lowest first).
nlohmann::ordered_json ContainerWrapper::BuildLayerSpec(struct ContainerParams &ContainerParams)
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

    //DEFPREFIX base — lowest priority, at the runtime root. Mounted whole when the runner generates a prefix
    //(its drive_c / pfx structure and compat-data bookkeeping show at the root the launcher's env points at).
    if (ContainerParams.PrefixGenerate && !ContainerParams.DefPrefixPath.empty())
        Layers.push_back({{"type", "dir"}, {"source", ContainerParams.DefPrefixPath.string()}, {"target", ""}, {"rw", false}});

    //Host content placement is a RUNNER property (CONTENT_ROOT, resolved): "" = root; "drive_c/<UID>" maps
    //the game under Wine's C:; "pfx/drive_c/<UID>" places it inside Proton's pfx. Same game, different runner.
    const std::string &ContentRoot = ContainerParams.ContentRoot;

    for (auto &Sub : ContainerParams.SubComponentsArray)
    {
        std::string Type = Sub.value("TYPE", std::string());
        std::string LType;
        if      (Type == "VFSZipLayer")  LType = "zip";
        else if (Type == "VFSDirLayer")  LType = "dir";
        else if (Type == "VFSFileLayer") LType = "file";
        else continue;

        std::filesystem::path Source = ResolveLayerSource(Sub, ContainerParams.PackagePath);
        std::string Target = ContentRoot;
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
    nlohmann::ordered_json Spec = BuildLayerSpec(ContainerParams);

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
            if (!IsVfsLayer(T)) continue;
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
    if (!RunnerWrapper::GeneratesPrefix(RunnerPkg, Vid)) return true;    // no PREFIX_GENERATE: no prefix to build

    //2. Generate the one-time DEFPREFIX in the runner's library dir (idempotent): PackageDir/__DEFPREFIX__/<vid>.
    //The whole artifact is mounted at the runtime root later, so the prefix hives live at <artifact>/<PrefixRoot>,
    //PrefixRoot = CONTENT_ROOT up to /drive_c ("" for wine, "pfx" for proton) — derived, no PREFIX_SUBPATH.
    const std::filesystem::path DefArtifact = RunnerWrapper::DefPrefixArtifact(PackageDir, Vid);
    const std::string ContentRoot = R.value("CONTENT_ROOT", std::string());
    std::string PrefixRoot;
    { const auto Pos = ContentRoot.find("drive_c");
      if (Pos != std::string::npos && Pos != 0) PrefixRoot = ContentRoot.substr(0, Pos - 1); }
    const std::filesystem::path PrefixDir = PrefixRoot.empty() ? DefArtifact : (DefArtifact / PrefixRoot);
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

    //Build the runner ENV/ARGS/EXECUTABLE with the install-time variable bindings. The runner ENV points its
    //prefix var (WINEPREFIX / STEAM_COMPAT_DATA_PATH) at %RuntimePath%; bind it to the artifact root we're
    //building, so proton writes <artifact>/pfx and wine writes <artifact>/drive_c (mounted whole at launch).
    std::map<std::string, std::string> Vars;
    Vars["RunnerMount"]  = MountDir.string();
    Vars["RuntimePath"]  = DefArtifact.string();
    Vars["TempPath"]     = DefArtifact.string();

    std::string Program = R.value("EXECUTABLE", std::string("%RunnerMount%/proton"));
    ContainerWrapper::StringVariableSubstitution(Program, Vars);
    //Args with the content target swapped for wineboot — keep the launcher verb, drop the content-bearing arg.
    QStringList Args;
    for (const auto &A : R.value("ARGS", nlohmann::ordered_json::array()))
    { std::string a = std::string(A); if (ArgReferencesContent(a)) continue;
      ContainerWrapper::StringVariableSubstitution(a, Vars); Args << QString::fromStdString(a); }
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

//Synthesizes a minimal RUNNERS[]-shaped package from a runner node + its content closure so the tested
//ImportRunner machinery installs it unchanged. The build content nodes (the runner's PARENTS) become COMPONENTS;
//the runner node's EXEC becomes the single "default" variant — matching DerivePaths (RunnerVariantID="default").
bool ContainerWrapper::ImportRunnerNode(nlohmann::ordered_json &GlobalConfigJSON, const NodeIndex &Idx,
                                        const std::string &RunnerNodeId, std::string *Error)
{
    auto Fail = [&](const std::string &M) -> bool { if (Error) *Error = M; LogErr("ContainerWrapper::ImportRunnerNode", M); return false; };
    const Node *R = Idx.Find(RunnerNodeId);
    if (!R || !R->IsRunner()) return Fail("not a runner node: " + RunnerNodeId);

    nlohmann::ordered_json Components = nlohmann::ordered_json::array();
    nlohmann::ordered_json Modules    = nlohmann::ordered_json::array();
    for (const std::string &Id : ManifestModel::ResolveNodeOrder(Idx, RunnerNodeId, {}))
    {
        if (Id == RunnerNodeId) continue;
        const Node *C = Idx.Find(Id);
        if (!C || !C->Layers.is_array() || C->Layers.empty()) continue;
        Components.push_back(nlohmann::ordered_json{{"COMPONENTID", Id}, {"SUBCOMPONENTS", C->Layers}});
        Modules.push_back(nlohmann::ordered_json{{"COMPONENT", Id}});
    }

    nlohmann::ordered_json Variant = R->Exec.is_object() ? R->Exec : nlohmann::ordered_json::object();
    Variant["VARIANT_ID"]    = "default";
    Variant["HOST_PLATFORM"] = R->HostPlatform;
    Variant["GUEST_PLATFORM"] = R->GuestPlatform;
    Variant["MODULES"]       = Modules;
    Variant["RECOMMENDED"]   = true;

    nlohmann::ordered_json Pkg = nlohmann::ordered_json::object();
    Pkg["PACKAGEUID"] = R->Uid.empty() ? R->NodeId : R->Uid;
    Pkg["RUNNERS"]    = nlohmann::ordered_json::array({ nlohmann::ordered_json{
        {"RUNNER_ID", R->NodeId}, {"NAME", R->NodeId}, {"VARIANTS", nlohmann::ordered_json::array({Variant})} } });
    Pkg["COMPONENTS"] = Components;

    return ImportRunner(GlobalConfigJSON, Pkg, R->BundleDir.string(), "default", Error);
}

bool ContainerWrapper::RunnerNodeImported(const NodeIndex &Idx, const std::string &RunnerNodeId)
{
    const Node *R = Idx.Find(RunnerNodeId);
    if (!R || !R->IsRunner()) return false;

    //Every build VFS layer hydrated locally (the runner's content closure).
    bool AllPresent = true; bool AnyBuild = false;
    for (const std::string &Id : ManifestModel::ResolveNodeOrder(Idx, RunnerNodeId, {}))
    {
        if (Id == RunnerNodeId) continue;
        const Node *C = Idx.Find(Id);
        if (!C || !C->Layers.is_array()) continue;
        for (const auto &L : C->Layers)
        {
            if (!IsVfsLayer(LayerType(L))) continue;
            std::filesystem::path Local; std::string Cid;
            LayerLocator(L, C->BundleDir, Local, Cid);
            if (Local == C->BundleDir) continue;                                  // no PATH
            AnyBuild = true;
            std::error_code Ec;
            if (!std::filesystem::exists(Local, Ec)) AllPresent = false;
        }
    }
    if (!AnyBuild) return true;                                                   // ships no build → always installed
    if (!AllPresent) return false;

    //PREFIX_GENERATE runners also need their DEFPREFIX artifact present (<bundle>/__DEFPREFIX__/default/<prefixRoot>).
    const bool PrefixGen = R->Exec.is_object() && R->Exec.value("PREFIX_GENERATE", false);
    if (!PrefixGen) return true;
    const std::string CR = R->Exec.is_object() ? R->Exec.value("CONTENT_ROOT", std::string()) : std::string();
    std::string PrefixRoot;
    { const auto Pos = CR.find("drive_c"); if (Pos != std::string::npos && Pos != 0) PrefixRoot = CR.substr(0, Pos - 1); }
    std::filesystem::path Artifact = std::filesystem::path(RunnerWrapper::DefPrefixArtifact(R->BundleDir.string(), "default"));
    std::filesystem::path PrefixDir = PrefixRoot.empty() ? Artifact : (Artifact / PrefixRoot);
    std::error_code Ec;
    return std::filesystem::exists(PrefixDir, Ec) && !std::filesystem::is_empty(PrefixDir, Ec);
}

//(PackageIpfsCids / PackageCoverCids moved to ManifestModel — see manifestmodel.h.)

//(ImportPackage / PublishPackage / MirrorDehydrated / IsPackageImported moved to PackageCatalog — see packagecatalog.h.)

//The 3 Wine registry files that hold per-prefix state.
static const char *const kRegFiles[] = { "system.reg", "user.reg", "userdef.reg" };

//Seeds previously-persisted reg files from UserDataPath/__REGISTRY__/ into the ephemeral
//WRITELAYER before the union mounts, so they shadow the DEFPREFIX base. Wine always writes the
//complete file, so a whole persisted reg file is correct (no stripped-delta shadowing).
bool ContainerWrapper::SeedPersistRegistry(struct ContainerParams &ContainerParams)
{
    if (ContainerParams.PersistAll) return true; //durable RW branch already holds the reg files
    const std::filesystem::path RegStore = ContainerParams.UserDataPath / "__REGISTRY__";
    //Shadow at the prefix root in the union (/<PrefixRoot>/*.reg) — "" for wine, "pfx" for proton.
    const std::filesystem::path WriteHives = HiveDir(ContainerParams, ContainerParams.WriteLayerPath);
    std::error_code ec;
    std::filesystem::create_directories(WriteHives, ec);
    for (const char *const Name : kRegFiles)
    {
        const std::filesystem::path SrcReg = RegStore / Name;
        if (!std::filesystem::exists(SrcReg)) continue;
        const std::filesystem::path DstReg = WriteHives / Name;
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
    const std::filesystem::path RunHives = HiveDir(ContainerParams, ContainerParams.RuntimePath);
    for (const char *const Name : kRegFiles)
    {
        const std::filesystem::path SrcReg = RunHives / Name;
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
    Session.LoadPrefix(HiveDir(ContainerParams, ContainerParams.RuntimePath));

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
    for (size_t i = 0; i < ContainerParams.SubComponentsArray.size(); i++)
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
//Fully data-driven: the runner's EXECUTABLE + ARGS ARE the command line. The author composes the launch
//target into ARGS (a guest path "C:\%PackageUID%\%ContentPath%" for wine/proton, the absolute "%Content%"
//for ROM/native runners, or nothing for a self-contained runner). The engine no longer auto-appends content.
//
//OverrideExe (tooling / install steps) swaps just the target: content-bearing ARGS are dropped and the
//override exe is appended after the launcher verb, leaving the rest of the runner's command line intact.
//
//Runner ENV values undergo %VARIABLE% substitution before being inserted into the
//process environment, so tokens like %WINEPREFIX% are expanded at launch time.
//
//WorkDir falls back to RuntimePath (VFS) or PackagePath if the configured path
//does not exist in the mounted runtime.
bool ContainerWrapper::Execute(std::string OverrideExe)
{
    //Substitute variables in the runner executable path — manifest runners reference
    //%ProgramPath% to point to a bundled binary mounted into RUNTIME via a VFS layer.
    ContainerWrapper::StringVariableSubstitution(ContainerParams.RunnerExecutable, ContainerParams.GetVariablesMap());
    LogOut("ContainerWrapper::Execute", "Runner: " + ContainerParams.RunnerExecutable);
    if (!OverrideExe.empty()) LogOut("ContainerWrapper::Execute", "OverrideExe: " + OverrideExe);
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

    // WINEDLLOVERRIDES — only meaningful for runners that have a wine prefix.
    //Joins all DLL override strings; umu-run/Wine interprets the combined value.
    if (ContainerParams.PrefixGenerate && !ContainerParams.DLLOverrides.empty())
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

    //The runner's ARGS ARE the command line (template-expanded), with the launch target already composed in
    //by the author (e.g. proton's ["waitforexitandrun", "C:\\%PackageUID%\\%ContentPath%"]; snes9x's
    //["-fullscreen", "%Content%"]; gemrb's ["-f"]). Then the game variant's own EXEARGS.
    //For an OverrideExe (tooling/install), the content-bearing ARGS are dropped and the override is appended
    //after the launcher verb, so e.g. proton runs [waitforexitandrun, <override>] instead of the game.
    const bool Override = !OverrideExe.empty();
    QStringList Arguments;
    for (const std::string &Raw : ContainerParams.RunnerArgs)
    {
        if (Override && ArgReferencesContent(Raw)) continue;
        std::string Arg = Raw;
        ContainerWrapper::StringVariableSubstitution(Arg, ContainerParams.GetVariablesMap());
        Arguments.append(QString::fromStdString(Arg));
    }
    if (Override)
        Arguments.append(QString::fromStdString(OverrideExe));
    else
        //Game variant's own EXEARGS (e.g. -c config.cfg). Skipped under OverrideExe (tooling).
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
        this->LastCrashed = true;
        this->LastExitCode = -1;
        LogErr("ContainerWrapper::Execute", "Process crashed.");
        return false;
    }
    this->LastCrashed = false;
    this->LastExitCode = RunProcess.exitCode();
    LogOut("ContainerWrapper::Execute", "Process exited normally with code " + std::to_string(RunProcess.exitCode()));
    return true;
}

//Sends SIGKILL to the active game process if one is running.
//Called from the UI thread while Execute() blocks on the worker thread.
void ContainerWrapper::KillGame()
{
    QMutexLocker Locker(&ActiveRunMutex);
    if (!ActiveRunProcess) return;
    this->UserKilled = true;                 // a deliberate kill — not a launch failure to report
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
