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

//Determines which execution backend is used for a subgame.
//  Wine     — Windows executables via Wine/Proton (umu-run); needs a Wine prefix and VFS drive_c layout.
//  Emulator — ROM-based games; runner receives the ROM path as its argument.
//  Native   — Linux-native binaries; runner is the executable itself.
//  Custom   — Any other runner; DATAPATH is passed instead of EXEPATH/ROM.
enum class RunnerType { Wine, Emulator, Native, Custom };

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

    //(Sub)game specific:
    std::string GameName;                //Title of the selected subgame
    std::string UMUID;                   //UMU/Steam App ID used by umu-run for Proton compatibility; "0" if absent
    std::string Platform;                //Platform string (e.g. "Microsoft Windows", "SNES") — drives runner selection
    std::vector<std::string> Recipe;     //Ordered list of ComponentIDs to apply, from leaf to root (reversed after build)
    nlohmann::ordered_json SubComponentsArray; //Flat, ordered array of all SUBCOMPONENTS across the Recipe's components

    //Runner config (resolved from GlobalConfig RUNNERS by platform):
    std::string RunnerName;              //Human-readable runner name (e.g. "umu-proton")
    std::string RunnerExecutable;        //Binary to exec (e.g. "umu-run", "snes9x")
    RunnerType RunnerTypeEnum = RunnerType::Wine; //Determines argument order and Wine-specific steps
    nlohmann::ordered_json RunnerEnv;    //Key/value env vars to set; values may contain %VARIABLE% tokens
    std::vector<std::string> RunnerRemoveEnv; //Env keys to remove before launch (e.g. LD_LIBRARY_PATH)
    std::vector<std::string> RunnerArgs; //Arguments prepended before the exe for emulator/custom runners

    //Flags
    std::string subgame_id;                                         //PASSED
    std::string component_id;                                       //PASSED
    bool ReadOnlyVFS = false;                                       //SET — if true, USERDATA is not prepended to VFSString
    bool UsesVFS = false;                                           //AUTO-DETECTED from SubComponentsArray

    //Custom variables (from CustomVar subcomponents):
    std::map<std::string, std::string> CustomVariables;             //AUTO-RESOLVED: KEY → value; priority: CLI override > GlobalConfig > DEFAULT
    std::map<std::string, std::string> VariableOverrides;           //PASSED (CLI --var KEY=VALUE); highest-priority source for CustomVariables

    //ExecutableDefinition resolution:
    std::string ExecutableID;                                        //PASSED from SUBGAMES["EXECUTABLE_ID"]
    std::string DefaultVariant;                                      //PASSED from SUBGAMES["DEFAULT_VARIANT"]
    std::string SelectedVariant;                                     //SET before ResolveExecutableDefinition (CLI --variant or user picker; falls back to DefaultVariant)

    //System Variables — queried from Qt at runtime
    std::string ScreenWidth;
    std::string ScreenHeight;

    //Paths (that need to be created before use)
    std::filesystem::path RuntimePath;       //Where the final unionfs VFS is mounted (PackagePath/RUNTIME)
    std::filesystem::path MetaDataPath;      //PackagePath/METADATA
    std::filesystem::path PackageFilesPath;  //PackagePath/PACKAGEFILES — source archives and directories
    std::filesystem::path UserDataPath;      //PackagePath/USERDATA — copy-on-write layer at the top of the VFS stack
    std::filesystem::path TempPath;          //PackagePath/TEMP — prefix, per-layer pre-mount dirs, reg patches
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

    //REGISTRYWRAPPER CLASS
    //Structured registry data built by CreateFlatRegPatchJSON:
    //  FlatRegPatch["32"|"64"][regPath][valueName] = value
    nlohmann::ordered_json FlatRegPatch;

    //VFSWRAPPER CLASS
    std::string VFSString;                                //Colon-separated unionfs branch string (built up incrementally)
    std::vector<std::filesystem::path> CleanupUnmountPaths; //All FUSE mount points that must be unmounted on Cleanup()
    //std::vector<std::filesystem::path> CleanupDeletePaths;

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

    //Unmounts all paths in CleanupUnmountPaths (fusermount -uz) and removes RUNTIME and TEMP.
    bool Cleanup();

    //ID lookup helpers:
    //Linear scan of MANIFEST["SUBGAMES"] for a matching SUBGAMEID. Returns index or -1.
    static int FindSubgameIndex(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID);
    //Linear scan of MANIFEST["COMPONENTS"] for a matching COMPONENTID. Returns index or -1.
    //Returns -1 immediately if ComponentID is empty.
    static int FindComponentIndex(const nlohmann::ordered_json &MANIFESTJSON, const std::string &ComponentID);

    //Container initialization:
    //Resolves which component_id to use given the provided subgame_id / component_id combination.
    //If only subgame_id is set, reads COMPONENT from that subgame to resolve component_id.
    static bool DecideComponent(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams);

    //Scans SubComponentsArray for all ExecutableDefinition entries matching ContainerParams.ExecutableID.
    //Returns the unique VARIANT values found — used to build the variant picker before launch.
    //Empty string means "no variant specified" for that definition.
    static std::vector<std::string> GetAvailableVariants(const struct ContainerParams &ContainerParams);

    //Picks the ExecutableDefinition matching (ExecutableID, SelectedVariant) from SubComponentsArray
    //and populates ExePathRelative, ExePathComplete, WorkDir, ExeArgs.
    //Last match in SubComponentsArray wins — later components in the chain override earlier ones.
    //Must be called AFTER BuildSubComponentsArray and BEFORE BuildContainerRuntime.
    static bool ResolveExecutableDefinition(struct ContainerParams &ContainerParams);
    //Walks the PARENTCOMPONENT chain from component_id up to the root component,
    //producing an ordered Recipe (ancestor-first).
    static bool CreateRecipe(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams);
    //Scans all CustomVar subcomponents in the Recipe and resolves their values.
    //Priority: VariableOverrides (CLI) > GlobalConfigJSON USERSETTINGS > DEFAULT.
    //Must run BEFORE BuildSubComponentsArray so custom variables are available for substitution.
    static bool ResolveCustomVariables(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams, nlohmann::ordered_json GlobalConfigJSON);
    //Collects all SUBCOMPONENTS from components in the Recipe into SubComponentsArray.
    //Performs %VARIABLE% substitution on each subcomponent's JSON at collection time.
    //CustomVar subcomponents are skipped here — they are resolved by ResolveCustomVariables.
    static bool BuildSubComponentsArray(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams);
    //Fills all derived ContainerParams fields from MANIFEST and GlobalConfigJSON.
    //Must run after DecideComponent so platform and subgame index are known.
    //Resolves runner via USERSETTINGS > RECOMMENDED_RUNNER > first available fallback.
    static bool DeriveContainerParams(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams, nlohmann::ordered_json GlobalConfigJSON);

    //Filesystem management:
    //Iterates SubComponentsArray and pre-mounts each VFSZipLayer/VFSDirLayer/VFSFileLayer
    //into a numbered TEMP/[i] directory, then adds each to VFSString.
    //WineMode=true wraps each layer inside drive_c/PackageUID to match Wine's prefix layout.
    static bool PreMountFilesystemComponents(struct ContainerParams &ContainerParams, bool WineMode = false);
    static bool BuildUnionFS(const nlohmann::ordered_json ContainerVariablesJSON);
    //Appends NewPath=RO to the front of VFSString (newer entries are higher priority in unionfs).
    static bool AddToVFSString(struct ContainerParams &ContainerParams, std::string NewPath);
    //Prepends USERDATA=RW to VFSString to form the final writable-top union.
    //Returns false if VFSString is empty (no layers were added).
    static bool FinalizeVFSString(struct ContainerParams &ContainerParams);
    //Mounts the finalized VFSString onto RuntimePath using unionfs with cow + uid=1000.
    static bool MountVFS(struct ContainerParams &ContainerParams);
    //Walks DirectoryPath recursively and warns (via QMessageBox) if any two paths
    //differ only in case — these cause unpredictable behavior under Wine.
    static bool CheckCaseConflicts(std::filesystem::path RuntimePath);

    //Registry handling:
    //Initializes the Wine prefix at DefPrefixPath by running `wineboot` via the runner,
    //then adds DefPrefixPath to VFSString as the base layer.
    //Must run before any VFS layers are added so the prefix sits at the bottom of the stack.
    static bool InitializeDefPrefix(struct ContainerParams &ContainerParams);
    //Flattens all RegEdit subcomponents into FlatRegPatch, keyed by architecture then registry path.
    static bool CreateFlatRegPatchJSON(struct ContainerParams &ContainerParams);
    //Writes RegPatch32.reg and RegPatch64.reg from FlatRegPatch into DefPrefixPath/drive_c.
    //Handles bool→dword, int→dword:hex, and string (with backslash escaping) value types.
    //Strings that already start with "dword:" or "hex:" are written verbatim.
    static bool CreateRegPatchFiles(struct ContainerParams &ContainerParams);
    //Imports RegPatch32.reg and RegPatch64.reg into the prefix using `reg import` via the runner.
    static bool MergeRegPatchFiles(struct ContainerParams &ContainerParams);

    //DLL overrides:
    //Collects DLLOVERRIDE values from all DllOverride subcomponents into DLLOverrides.
    //These are later joined and set as WINEDLLOVERRIDES in Execute().
    static bool ProcessDLLOverrides(struct ContainerParams &ContainerParams);

    //FileEdits:
    //Processes FileEdit subcomponents. Currently supports MODE="ConfigWrite" only.
    //MUST BE RUN AFTER VARIABLE SUBSTITUTION (already done in BuildSubComponentsArray).
    static bool ProcessFileEdits(struct ContainerParams &ContainerParams);
    //Reads FilePath line by line and replaces any line starting with Key with Key+Value.
    //Useful for patching INI-style config files that use prefix-based key matching.
    static bool ConfigWrite(std::string Key, std::string Value, std::filesystem::path FilePath);

    //Misc
    //Synchronously runs Program with Arguments in the given environment.
    //Waits indefinitely for completion. Returns the exit code, or -1 on crash/start failure.
    static int RunCommand(std::string Program, std::vector<std::string> Arguments, QProcessEnvironment ProcessEnvironment = QProcessEnvironment::systemEnvironment());
    //Replaces all %KEY% tokens in SourceString with values from VariablesMap.
    //Leaves unrecognised tokens unchanged and logs a warning. Returns true if any replacement was made.
    static bool StringVariableSubstitution(std::string &SourceString, const std::map<std::string, std::string> &VariablesMap);

private:
    //Runs DecideComponent → DeriveContainerParams → CreateRecipe → BuildSubComponentsArray
    //in the required order. Called from the constructor.
    bool InitializeContainer();
    bool BuildVirtualFilesystem(); //STUB — not yet implemented

    nlohmann::ordered_json GlobalConfigJSON;
    nlohmann::ordered_json MANIFESTJSON;
};
#endif // CONTAINERWRAPPER_H
