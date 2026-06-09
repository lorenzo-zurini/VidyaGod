#ifndef CONTAINERWRAPPER_H
#define CONTAINERWRAPPER_H

#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <cstring>
#include <ctime>
#include <regex>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <algorithm>

#include <QDir>
#include <QVector>
#include <QMessageBox>
#include <QGuiApplication>
#include <QScreen>
#include <QProcess>
#include <QMutex>
#include <QMutexLocker>

//Determines which execution backend is used for a subgame.
//  Wine     — Windows executables via Wine/Proton (umu-run); needs a Wine prefix and VFS drive_c layout.
//  Emulator — ROM-based games; runner receives the ROM path as its argument.
//  Native   — Linux-native binaries; runner is the executable itself.
//  Custom   — Any other runner; DATAPATH is passed instead of EXEPATH/ROM.
enum class RunnerType { Wine, Emulator, Native, Custom };

//Describes one available variant for a given subgame.
//Selecting a variant = selecting which set of ENDPOINTS (terminal components) to build.
//Each endpoint walks its own PARENTCOMPONENT chain; the union of all chains forms the recipe.
struct VariantInfo {
    std::string VariantID;      // VARIANT_ID field on the variant
    std::string Name;           // optional NAME; falls back to VARIANT_ID for display
    bool        IsRecommended = false; // RECOMMENDED:true on the variant — shown with ⭐ in picker
    std::vector<std::string> Endpoints; // ENDPOINTS array (terminal component ids, load order)
};

//All resolved parameters needed to build and launch a single container session.
//Populated in two stages:
//  1. ContainerParams constructor — stores only the PASSED values (PackagePath, IDs).
//  2. ContainerWrapper::DeriveContainerParams — fills every other field from MANIFEST + GlobalConfig.
//Fields are grouped by concern below; the inline comments note how each is populated.
struct ContainerParams
{
public:
    ContainerParams(std::filesystem::path Passed_PackagePath, std::string Passed_subgame_id = "", std::string Passed_component_id = "");

    //Container params:
    //Package-specific:
    std::filesystem::path PackagePath;   //Root of the package directory on disk
    std::string PackageName;             //Human-readable name from MANIFEST["PACKAGENAME"]
    std::string PackageUID;              //Unique package identifier from MANIFEST["PACKAGEUID"]

    //Game specific:
    std::string GameName;                //Title of the selected game
    std::string UMUID;                   //UMU/Steam App ID used by umu-run for Proton compatibility; "0" if absent
    std::string Platform;                //HOST_PLATFORM of the package (e.g. "win32", "snes", "custom") — matched against runner GUEST_PLATFORM
    std::vector<std::string> Recipe;     //Ordered list of ComponentIDs to apply, from leaf to root (reversed after build)
    nlohmann::ordered_json SubComponentsArray; //Flat, ordered array of all SUBCOMPONENTS across the Recipe's components

    //Runner config (resolved from RUNNERS arrays — package's own + global registry — by GUEST_PLATFORM membership):
    std::string RunnerID;                //RUNNER_ID — selected/pinned runner id (PASSED by picker/CLI, or resolved)
    std::string RunnerName;              //Human-readable runner name (e.g. "umu-proton")
    std::string RunnerExecutable;        //Binary to exec (e.g. "umu-run", "snes9x")
    RunnerType RunnerTypeEnum = RunnerType::Wine; //Determines argument order and Wine-specific steps
    nlohmann::ordered_json RunnerEnv;    //Key/value env vars to set; values may contain %VARIABLE% tokens
    std::vector<std::string> RunnerRemoveEnv; //Env keys to remove before launch (e.g. LD_LIBRARY_PATH)
    std::vector<std::string> RunnerArgs; //Arguments prepended before the exe for emulator/custom runners
    std::vector<std::string> RunnerEndpoints; //RESOLVED — the selected runner's ENDPOINTS (its own components), mounted as recipe base
    nlohmann::ordered_json RunnerComponents = nlohmann::ordered_json::array(); //RESOLVED — COMPONENTS of the selected runner's registry package (empty for an embedded/PATH runner); folded into the component pool

    //Flags
    std::string subgame_id;                                         //PASSED
    std::string component_id;                                       //PASSED (direct/editor mode — single endpoint)
    std::vector<std::string> Endpoints;                             //RESOLVED — terminal component ids (the selected variant's ENDPOINTS, load order)
    bool ReadOnlyVFS = false;                                       //SET — if true, no writable top layer (spec readonly=true); whole runtime is read-only
    bool UsesVFS = false;                                           //AUTO-DETECTED from SubComponentsArray

    //Persistence (derived from PersistDir/PersistFile/RegPersist subcomponents — see DerivePersistence):
    bool PersistAll = false;                                        //DEFAULT — no persist subcomponents declared: RW union branch IS the durable UserDataPath (whole-runtime persist)
    bool PersistRegistry = false;                                   //a RegPersist subcomponent is present — persist user.reg/system.reg/userdef.reg
    std::vector<std::string> PersistDirs;                           //PersistDir subcomponents — runtime-root-relative leaf dirs bind-mounted from UserDataPath
    std::vector<std::string> PersistFiles;                          //PersistFile subcomponents — runtime-root-relative single files seeded/captured via UserDataPath
    std::vector<std::string> PersistRegKeys;                        //RegKeyPersist subcomponents — registry key paths (HKLM\.., HKCU\..) whose subtree is seeded/captured

    //Custom variables (from CustomVar subcomponents):
    std::map<std::string, std::string> CustomVariables;             //AUTO-RESOLVED: KEY → value; priority: CLI override > GlobalConfig > DEFAULT
    std::map<std::string, std::string> VariableOverrides;           //PASSED (CLI --var KEY=VALUE); highest-priority source for CustomVariables

    //Variant resolution:
    std::string VariantID;                                           //VARIANT_ID — resolved in DecideComponent (RECOMMENDED/first) or set by the caller

    //System Variables — queried from Qt at runtime
    std::string ScreenWidth;
    std::string ScreenHeight;

    //Paths (that need to be created before use)
    //Two storage tiers:
    //  EPHEMERAL — everything under TempPath = ~/.VidyaGod/TEMP/PackageUID; wiped by Cleanup().
    //              RuntimePath, WriteLayerPath and DefPrefixPath are all nested inside it.
    //  DURABLE   — UserDataPath = PackagePath/USERDATA; survives Cleanup(), travels with the package.
    //              Holds only the persisted state declared by the PERSIST manifest object.
    std::filesystem::path RuntimePath;       //TempPath/RUNTIME — where the final unionfs VFS is mounted
    std::filesystem::path WriteLayerPath;    //TempPath/WRITELAYER — ephemeral copy-on-write layer at the top of the VFS stack
    std::filesystem::path TempPath;          //~/.VidyaGod/TEMP/PackageUID — prefix, per-layer pre-mount dirs, reg patches
    std::filesystem::path UserDataPath;      //PackagePath/USERDATA — durable persist store (PERSIST.ALL / DIRS / REGISTRY)
    std::filesystem::path ProgramPath;       //Wine: RuntimePath/drive_c/PackageUID; others: RuntimePath
    std::filesystem::path DefPrefixPath;     //TempPath/DEFPREFIX — the Wine prefix directory
    std::filesystem::path ExePathRelative;   //Exe/ROM/data path relative to ProgramPath, from MANIFEST
    std::filesystem::path ExePathComplete;   //ProgramPath / ExePathRelative — absolute host path to the exe
    std::filesystem::path ExePathInPrefix;   //Wine only: C:\PackageUID\ExePathRelative — Windows-style path for Wine

    //Paths (that don't need to be created — derived or Windows-style strings)
    std::string WindowsProgramPath;                    //C:\PackageUID — used in registry patches and file edits
    std::string WindowsExePathComplete;                //C:\PackageUID\ExePathRelative — passed to Wine as the exe
    std::string WindowsProgramPathDoubleBackSlash;     //C:\\PackageUID — double-escaped for .reg file values
    std::filesystem::path WorkDirPathRelative;         //Working directory relative to ProgramPath, from MANIFEST WORKDIR
    std::filesystem::path WorkDirPathComplete;         //Absolute working directory; falls back to ProgramPath if unset

    //Wine / Proton specific:
    std::vector<std::string> ExeArgs;                               //DERIVED FROM MANIFESTJSON — split from EXEARGS string
    std::vector<std::string> DLLOverrides;                          //DERIVED FROM MANIFESTJSON — fed into WINEDLLOVERRIDES

    //VFSWRAPPER CLASS — the runtime is one vidyagodfs FUSE mount at RuntimePath (see BuildLayerSpec/MountVFS).
    std::vector<std::filesystem::path> CleanupUnmountPaths; //Purely-ephemeral FUSE mount(s) — lazy-unmounted on Cleanup()
    //Durable-backed mount: the vidyagodfs RUNTIME mount whenever durable data is reachable through it
    //(PersistAll writelayer or any RW passthrough persist dir). It exposes PackagePath/USERDATA, so it
    //MUST be non-lazily unmounted and verified before Cleanup() wipes TempPath — else remove_all could
    //recurse into real saves.
    std::vector<std::filesystem::path> CleanupPersistPaths;

    //Returns a map of all ContainerParams fields keyed by their %VARIABLE% token names.
    //Used by StringVariableSubstitution to expand tokens in runner ENV, args, and subcomponent paths.
    std::map<std::string, std::string> GetVariablesMap();
};

//Orchestrates the full lifecycle of a single game session:
//  construction → InitializeContainer (DecideComponent, DeriveContainerParams, CreateRecipe, BuildSubComponentsArray)
//  BuildContainerRuntime() → registry, VFS mounts
//  Execute()               → launches the runner with the correct arguments and environment
//  Cleanup()               → unmounts all FUSE filesystems, removes RUNTIME and TEMP
//
//Static helpers are grouped into logical subsystems (Registry, VFS, Runner) and can
//also be called standalone where the full wrapper is not needed.
class ContainerWrapper
{
public:
    ContainerWrapper(nlohmann::ordered_json &Passed_GlobalConfigJSON, nlohmann::ordered_json &Passed_MANIFESTJSON, ContainerParams &Passed_ContainerParams);
    struct ContainerParams ContainerParams;

    //Mounts VFS layers, applies registry patches and DLL overrides, checks for case conflicts.
    //Must be called before Execute(). Returns false if VFS setup fails.
    bool BuildContainerRuntime();

    //Launches the game via the resolved runner. If OverrideExe is non-empty it is used
    //instead of the manifest's EXEPATH/ROM. Blocks until the process exits.
    bool Execute(std::string OverrideExe = "");

    //Sends SIGKILL to the game process started by Execute() if it is still running.
    //Safe to call from any thread; no-op if no process is active.
    void KillGame();

    //Unmounts all paths in CleanupUnmountPaths (fusermount -uz) and removes RUNTIME and TEMP.
    bool Cleanup();

    //Pre-launch hygiene: clears any runtime left under TempPath by a previously crashed/incomplete
    //run. A fresh ContainerWrapper has empty Cleanup*Paths and cannot know about a prior process's
    //mounts, so this discovers them from /proc/self/mountinfo, unmounts deepest-first, and only then
    //removes TempPath. Mirrors Cleanup()'s save-safety gate: if any mount under TempPath refuses to
    //detach, TempPath is LEFT IN PLACE (a lingering PERSIST bind could otherwise let remove_all
    //recurse into USERDATA). Run at the start of BuildContainerRuntime, before any mounting.
    static void CleanStaleRuntime(const std::filesystem::path &TempPath);

    //ID lookup helpers:
    //Linear scan of MANIFEST["GAMES"] for a matching SUBGAMEID. Returns index or -1.
    static int FindGameIndex(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID);

    //Per-package user settings helpers (settings live inside LIBRARY[i]["USERSETTINGS"]):
    //Returns a COPY of the USERSETTINGS object for the given PackageUID, or an empty object if not found.
    static nlohmann::ordered_json GetPackageUserSettings(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &PackageUID);
    //Writes a key/value into LIBRARY[i]["USERSETTINGS"] for the given PackageUID.
    //Creates the USERSETTINGS object if absent. No-op if the package is not in LIBRARY.
    static void SetPackageUserSetting(nlohmann::ordered_json &GlobalConfigJSON, const std::string &PackageUID, const std::string &Key, const nlohmann::ordered_json &Value);
    //Linear scan of MANIFEST["COMPONENTS"] for a matching COMPONENTID. Returns index or -1.
    //Returns -1 immediately if ComponentID is empty.
    static int FindComponentIndex(const nlohmann::ordered_json &MANIFESTJSON, const std::string &ComponentID);

    //Container initialization:
    //Resolves which variant + endpoints to build given the subgame_id / VariantID / component_id combo.
    //If only subgame_id is set, picks the RECOMMENDED variant (else first) and fills VariantID + Endpoints.
    static bool DecideComponent(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams);

    //Returns all variants defined under SUBGAMES[SubgameID].VARIANTS.
    static std::vector<VariantInfo> GetAvailableVariants(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID);

    //Returns the ENDPOINTS array of the variant matching { SubgameID, VariantID }.
    //Returns an empty vector if not found.
    static std::vector<std::string> FindEndpointsForVariant(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID, const std::string &VariantID);

    //Finds the variant in MANIFESTJSON matching ContainerParams.VariantID under the subgame
    //identified by ContainerParams.subgame_id, then populates ExePathRelative, ExePathComplete,
    //WorkDir, ExeArgs from it. Must be called AFTER BuildSubComponentsArray and BEFORE BuildContainerRuntime.
    static bool ResolveExecutableDefinition(const nlohmann::ordered_json &MANIFESTJSON, struct ContainerParams &ContainerParams);
    //Builds the ordered Recipe as the UNION of every endpoint's PARENTCOMPONENT chain.
    //Each endpoint chain is appended root-first; later endpoints (array order) stack higher
    //in the union, so they override earlier ones. Shared ancestors appear once. Falls back to
    //{component_id} when Endpoints is empty (direct/editor mode).
    static bool CreateRecipe(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams);
    //Returns the platform token the HOST runs as (e.g. "linux64"). Phase-1 stub — later real
    //detection. Used to reason about which guest platforms run natively vs. need a translation runner.
    static std::string HostPlatform();
    //Every package manifest known across all configured Repositories (Settings.Repositories) — the
    //catalog, kind-agnostic and globally cross-referenceable. TODO(sharing): + LIBRARY/DOWNLOADS, persisted.
    static std::vector<nlohmann::ordered_json> CatalogPackages(const nlohmann::ordered_json &GlobalConfigJSON);
    //Returns every runner object across the catalog (the HasRunners packages). Used by the UI (runner
    //picker, settings list). The launch resolver scans repositories itself (it also needs each runner's
    //owning components).
    static std::vector<nlohmann::ordered_json> RegistryRunners(const nlohmann::ordered_json &GlobalConfigJSON);
    //Indexes the configured Repositories at startup. TODO(sharing): git clone/pull + persisted catalog.
    static void SyncRepositories(const nlohmann::ordered_json &GlobalConfigJSON);
    //Returns unresolved dependency locators (missing VFS layer sources) for a built container — drives the
    //portable/standalone readiness warning. TODO(sharing): runner + RUNTIME + cross-package deps.
    static std::vector<std::string> VerifyDependencies(const struct ContainerParams &ContainerParams);
    //Scans all CustomVar subcomponents in the Recipe and resolves their values.
    //Priority: VariableOverrides (CLI) > GlobalConfigJSON USERSETTINGS > DEFAULT.
    //Must run BEFORE BuildSubComponentsArray so custom variables are available for substitution.
    static bool ResolveCustomVariables(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams, nlohmann::ordered_json GlobalConfigJSON);
    //Collects all SUBCOMPONENTS from components in the Recipe into SubComponentsArray.
    //Performs %VARIABLE% substitution on each subcomponent's JSON at collection time.
    //CustomVar and Persist* subcomponents are skipped here (handled by ResolveCustomVariables /
    //DerivePersistence respectively).
    static bool BuildSubComponentsArray(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams);
    //Walks the Recipe and derives persistence from PersistDir/PersistFile/RegPersist subcomponents
    //into PersistDirs/PersistFiles/PersistRegistry. When NONE are declared, sets PersistAll=true
    //(whole-runtime persist — the durable UserDataPath becomes the union's RW branch). PATH strings
    //are %VARIABLE%-substituted. Must run after ResolveCustomVariables and before BuildContainerRuntime.
    static bool DerivePersistence(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams);
    //Fills all derived ContainerParams fields from MANIFEST and GlobalConfigJSON.
    //Must run after DecideComponent so platform and subgame index are known.
    //Resolves runner via USERSETTINGS > RECOMMENDED_RUNNER > first available fallback.
    static bool DeriveContainerParams(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams, nlohmann::ordered_json GlobalConfigJSON);

    //Filesystem management (single vidyagodfs FUSE mount — replaces unionfs/fuse-zip/bindfs):
    //Builds the JSON layer-spec from the resolved container: DEFPREFIX base (Wine), each
    //VFSZipLayer/VFSDirLayer/VFSFileLayer rooted at its TARGET (logically, no staging dirs), PERSIST
    //dirs as RW passthrough layers, and the writable top branch (WriteLayerPath or UserDataPath).
    static nlohmann::ordered_json BuildLayerSpec(struct ContainerParams &ContainerParams, bool WineMode);
    //Writes the layer-spec and spawns vidyagodfs onto RuntimePath, then polls mountinfo for readiness.
    //Registers RuntimePath for non-lazy save-safe unmount when durable data is reachable through it.
    static bool MountVFS(struct ContainerParams &ContainerParams);
    //Seeds previously-persisted reg files (UserDataPath/__REGISTRY__/*.reg) into WriteLayerPath
    //before MountVFS so they shadow DEFPREFIX. No-op when PersistAll or no persisted regs exist.
    static bool SeedPersistRegistry(struct ContainerParams &ContainerParams);
    //Copies RuntimePath/{system,user,userdef}.reg into UserDataPath/__REGISTRY__/ on Cleanup,
    //capturing the session's registry. Must run BEFORE the runtime is unmounted/wiped.
    static bool CapturePersistRegistry(struct ContainerParams &ContainerParams);
    //Seeds each previously-persisted PersistFile (UserDataPath/<rel>) into WriteLayerPath/<rel>
    //before MountVFS so it shadows the lower layers. No-op when PersistAll or none persisted yet.
    static bool SeedPersistFiles(struct ContainerParams &ContainerParams);
    //Copies each PersistFile from RuntimePath/<rel> into UserDataPath/<rel> on Cleanup, capturing
    //the session's writes. Must run BEFORE the runtime is unmounted/wiped. No-op when PersistAll.
    static bool CapturePersistFiles(struct ContainerParams &ContainerParams);
    //Merges each previously-persisted RegKeyPersist subtree (from UserDataPath/__REGKEYS__/*.reg)
    //directly into the DEFPREFIX hives before the union mounts, so the durable key shadows the
    //default. Wine-only, pre-VFS. No-op when PersistAll or nothing persisted yet.
    static bool SeedPersistRegKeys(struct ContainerParams &ContainerParams);
    //Extracts each RegKeyPersist subtree from the mounted RuntimePath hives and merges it into the
    //durable store UserDataPath/__REGKEYS__/*.reg on Cleanup. Must run BEFORE unmount. No-op PersistAll.
    static bool CapturePersistRegKeys(struct ContainerParams &ContainerParams);
    //Walks DirectoryPath recursively and warns (via QMessageBox) if any two paths
    //differ only in case — these cause unpredictable behavior under Wine.
    static bool CheckCaseConflicts(std::filesystem::path RuntimePath);

    //Registry handling:
    //Initializes the Wine prefix at DefPrefixPath by running `wineboot` via the runner.
    //DefPrefixPath becomes the base (root) layer of the spec built by BuildLayerSpec.
    static bool InitializeDefPrefix(struct ContainerParams &ContainerParams);
    //Applies all non-OVERRIDE RegEdit subcomponents directly into the DEFPREFIX hive files via
    //RegistryWrapper (no .reg generation, no `reg import`). Quiesces any wineserver left by wineboot
    //first so it cannot flush and clobber the edits. Pre-VFS, so DEFPREFIX stays the lowest layer.
    static bool ApplyBaseRegEdits(struct ContainerParams &ContainerParams);
    //Applies OVERRIDE:true RegEdit subcomponents to the mounted runtime hives via RegistryWrapper,
    //after VFS is up. Saving RuntimePath/*.reg COWs the whole file into the RW WRITELAYER, so the
    //values win over DEFPREFIX and prior COW state. No wine runs on RuntimePath yet, so no quiesce.
    static bool ApplyOverrideRegEdits(struct ContainerParams &ContainerParams);

    //DLL overrides:
    //Collects DLLOVERRIDE values from all DllOverride subcomponents into DLLOverrides.
    //These are later joined and set as WINEDLLOVERRIDES in Execute().
    static bool ProcessDLLOverrides(struct ContainerParams &ContainerParams);

    //FileEdits:
    //Processes FileEdit subcomponents. MUST BE RUN AFTER VARIABLE SUBSTITUTION.
    //OverridePass=false: writes to DefPrefixPath (pre-VFS, WRITELAYER can shadow it).
    //OverridePass=true:  writes to RuntimePath  (post-VFS, goes directly to WRITELAYER — wins unconditionally).
    static bool ProcessFileEdits(struct ContainerParams &ContainerParams, bool OverridePass = false);
    //Reads FilePath line by line and replaces any line starting with Key with Key+Value.
    //Useful for patching INI-style config files that use prefix-based key matching.
    static bool ConfigWrite(std::string Key, std::string Value, std::filesystem::path FilePath);
    //Writes Value as the complete content of FilePath, creating parent dirs if needed.
    static bool FileOverwrite(const std::string &Value, const std::filesystem::path &FilePath);

    //Misc
    //Synchronously runs Program with Arguments in the given environment.
    //Waits indefinitely for completion. Returns the exit code, or -1 on crash/start failure.
    static int RunCommand(std::string Program, std::vector<std::string> Arguments, QProcessEnvironment ProcessEnvironment = QProcessEnvironment::systemEnvironment(), const std::string &WorkingDirectory = "");
    //Translates a display-layer value to its raw storage format based on VARTYPE (dword/qword/bool).
    static std::string TranslateCustomVarValue(const std::string &Value, const std::string &VarType);
    //Replaces all %KEY% tokens in SourceString with values from VariablesMap.
    //Leaves unrecognised tokens unchanged and logs a warning. Returns true if any replacement was made.
    static bool StringVariableSubstitution(std::string &SourceString, const std::map<std::string, std::string> &VariablesMap);

private:
    //Runs DecideComponent → DeriveContainerParams → CreateRecipe → BuildSubComponentsArray
    //in the required order. Called from the constructor.
    bool InitializeContainer();
    bool BuildVirtualFilesystem(); //STUB — not yet implemented

    //Returns every current mountpoint at or beneath Prefix (from /proc/self/mountinfo), deepest-first
    //so children unmount before parents. Mountinfo octal escapes (\040 etc.) are decoded.
    static std::vector<std::string> MountpointsUnder(const std::filesystem::path &Prefix);

    nlohmann::ordered_json GlobalConfigJSON;
    nlohmann::ordered_json MANIFESTJSON;

    //Set to the running QProcess during Execute() so KillGame() can signal it.
    //Null at all other times. Read/written only under ActiveRunMutex.
    QProcess* ActiveRunProcess = nullptr;
    QMutex    ActiveRunMutex;
};
#endif // CONTAINERWRAPPER_H
