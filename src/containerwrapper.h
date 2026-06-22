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

#include "manifestmodel.h"   // ModuleInfo/VariantInfo + pure manifest queries (moved out of this class)
#include "packagecatalog.h"  // catalog/sharing service (moved out of this class)
#include "launchparams.h"    // ContainerParams — the shared resolved-session struct (moved out of this class)

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

    //(Per-package user settings + game/component/variant/module lookups moved out: PackageCatalog::Get/Set
    //PackageUserSetting and ManifestModel::Find*/variants/modules respectively — see those headers.)

    //Container initialization:
    //Reads the launch node's EXEC (CONTENTPATH/EXEARGS/WORKDIR) into ExePathRelative/ExePathComplete/WorkDir/
    //ExeArgs. Must be called AFTER BuildSubComponentsArray and BEFORE BuildContainerRuntime.
    static bool ResolveExecutableDefinition(const nlohmann::ordered_json &MANIFESTJSON, struct ContainerParams &ContainerParams);

    //Native node-graph entry (the ONLY initialization path): populates ContainerParams + the internal component
    //pool (this->MANIFESTJSON) DIRECTLY from the global NodeIndex (ContainerParams.NodeIdx) and launch NODE_ID —
    //runner picked from ROLE:"runner" nodes, exec from launch.EXEC, Recipe from ResolveNodeOrder. Invoked by
    //InitializeContainer.
    bool InitializeFromNode();
    //Derives the session paths (Temp/Runtime/WriteLayer/DefaultData/UserData/Program/DefPrefix + ContentRoot
    //resolution + PrefixRoot + screen geometry) from already-set ContainerParams fields. Shared by the node
    //path and DeriveContainerParams.
    static bool DerivePaths(struct ContainerParams &ContainerParams, const nlohmann::ordered_json &GlobalConfigJSON);
    //Picks the best ROLE:"runner" node for a launch node from the global index (GUEST_PLATFORM ∋ launch host
    //&& HOST_PLATFORM==MachinePlatform && executable available), honoring RunnerID pin / PREFERRED_RUNNER /
    //RECOMMENDED. Returns nullptr if none. (Native replacement for the GatherRunners block.)
    static const Node *PickRunnerNode(const NodeIndex &Idx, const Node &Launch, const struct ContainerParams &CP,
                                      const nlohmann::ordered_json &GlobalConfigJSON);
    //(Catalog / RegistryRunners / LibraryRootDir / Import / Publish / Mirror / Sync moved to PackageCatalog —
    //see packagecatalog.h. ImportRunner stays here: it needs the launch engine's mount + wineboot machinery.)
    //Returns unresolved dependency locators (missing VFS layer sources) for a built container — drives the
    //portable/standalone readiness warning. TODO(sharing): runner + RUNTIME + cross-package deps.
    static std::vector<std::string> VerifyDependencies(const struct ContainerParams &ContainerParams);
    //Pre-flight CHECK (never fetches; GUI-thread safe) that every dependency is satisfiable — runner build CIDs
    //cached, the wine DEFPREFIX artifact present, and every game VFS layer either local or backend-fetchable.
    //Returns false (blocking the launch) otherwise. Used by the play() gate and at the start of BuildContainerRuntime.
    static bool EnsureSources(struct ContainerParams &ContainerParams);
    //Worker-thread MATERIALIZER (may block on a download): fetches each game VFS layer whose local content is
    //missing from a backend (IPFS) straight to its expected local PATH (self-healing). Runner builds stay cached.
    //Run in BuildContainerRuntime after EnsureSources, before the mount. Returns false if a layer can't be fetched.
    static bool MaterializeLayers(struct ContainerParams &ContainerParams);
    //Scans all CustomVar subcomponents in the Recipe and resolves their values.
    //Priority: VariableOverrides (CLI) > GlobalConfigJSON USERSETTINGS > DEFAULT.
    //Must run BEFORE BuildSubComponentsArray so custom variables are available for substitution.
    static bool ResolveCustomVariables(const nlohmann::ordered_json &MANIFESTJSON, struct ContainerParams &ContainerParams, const nlohmann::ordered_json &GlobalConfigJSON);
    //Collects all SUBCOMPONENTS from components in the Recipe into SubComponentsArray.
    //Performs %VARIABLE% substitution on each subcomponent's JSON at collection time.
    //CustomVar and Persist* subcomponents are skipped here (handled by ResolveCustomVariables /
    //DerivePersistence respectively).
    static bool BuildSubComponentsArray(const nlohmann::ordered_json &MANIFESTJSON, struct ContainerParams &ContainerParams);
    //Walks the Recipe and derives persistence from PersistDir/PersistFile/RegPersist subcomponents
    //into PersistDirs/PersistFiles/PersistRegistry. When NONE are declared, sets PersistAll=true
    //(whole-runtime persist — the durable UserDataPath becomes the union's RW branch). PATH strings
    //are %VARIABLE%-substituted. Must run after ResolveCustomVariables and before BuildContainerRuntime.
    static bool DerivePersistence(const nlohmann::ordered_json &MANIFESTJSON, struct ContainerParams &ContainerParams);

private:
    //Filesystem management (single vidyagodfs FUSE mount — replaces unionfs/fuse-zip/bindfs):
    //Builds the JSON layer-spec from the resolved container: DEFPREFIX base (Wine), each
    //VFSZipLayer/VFSDirLayer/VFSFileLayer rooted at its TARGET (logically, no staging dirs), PERSIST
    //dirs as RW passthrough layers, and the writable top branch (WriteLayerPath or UserDataPath).
    static nlohmann::ordered_json BuildLayerSpec(struct ContainerParams &ContainerParams);
    //Writes the layer-spec and spawns vidyagodfs onto RuntimePath, then polls mountinfo for readiness.
    //Registers RuntimePath for non-lazy save-safe unmount when durable data is reachable through it.
    static bool MountVFS(struct ContainerParams &ContainerParams);
    //Writes a layer spec to SpecPath, spawns vidyagodfs onto Mountpoint, polls until the mount is live.
    //The low-level mount primitive shared by MountVFS, the runner mount, and runner install.
    static bool SpawnVidyagodfs(const nlohmann::ordered_json &Spec, const std::filesystem::path &Mountpoint,
                                const std::filesystem::path &SpecPath);
    //Mounts the selected runner's build (RunnerLayers) read-only at RunnerMountPath (the separate-mount,
    //installed-runner model). Registers it for cleanup. No-op when the runner ships no build.
    bool MountRunnerBuild(struct ContainerParams &ContainerParams);
public:
    //Installs one runner VARIANT: hydrates its build layers IN PLACE into the runner's LIBRARY dir (PackageDir),
    //and for a wine variant generates the one-time DEFPREFIX at PackageDir/__DEFPREFIX__/<variant>. VariantId "" =
    //default variant. (Stays public: it's the runner-install entry point called by the UI / headless --import-runner.)
    static bool ImportRunner(nlohmann::ordered_json &GlobalConfigJSON, const nlohmann::ordered_json &RunnerPkg,
                              const std::string &PackageDir, const std::string &VariantId = std::string(), std::string *Error = nullptr);
    //Node-native runner install: synthesizes a minimal runner package from a ROLE:"runner" node + its content
    //closure (the build) and runs ImportRunner — hydrating the build into the runner's bundle and generating the
    //one-time DEFPREFIX at <bundle>/__DEFPREFIX__/default (where DerivePaths reads it). The runner-install entry
    //point for the node world (Settings page / play-gate).
    static bool ImportRunnerNode(nlohmann::ordered_json &GlobalConfigJSON, const NodeIndex &Idx,
                                 const std::string &RunnerNodeId, std::string *Error = nullptr);
    //True when a runner node is installed: every build VFS layer hydrated locally AND (PREFIX_GENERATE) its
    //DEFPREFIX artifact exists. A runner that ships no build is always "installed".
    static bool RunnerNodeImported(const NodeIndex &Idx, const std::string &RunnerNodeId);
private:
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
    //(The seed side of RegKeyPersist — merging persisted subtrees into the base hives — is done by
    //BuildDefaultData, which writes them into the DEFAULTDATA hives rather than mutating DEFPREFIX.)
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
    //Builds the DEFAULTDATA layer (TempPath/DEFAULTDATA): all package-encoded BASE (non-OVERRIDE) edits —
    //FileEdits as files, RegEdits as full hives copied-from-and-shadowing DEFPREFIX, plus merged persisted
    //RegKeyPersist subtrees. Mounts between the component layers and the WRITELAYER, so it overrides package
    //content but the user's persisted writes shadow it. DEFPREFIX is never mutated. Wine-only, pre-VFS.
    static bool BuildDefaultData(struct ContainerParams &ContainerParams);
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
    //OverridePass selects WHICH edits run (the OVERRIDE flag must match it).
    //BaseDir, when non-empty, is the directory the matched edits are written under; otherwise it
    //defaults to RuntimePath for the override pass and DefPrefixPath for the base pass.
    //  base pass  (OverridePass=false): written into BaseDir — normally the DEFAULTDATA layer, so the
    //                                   WRITELAYER (user) can shadow them.
    //  override pass (OverridePass=true): written into RuntimePath (post-VFS, COW to WRITELAYER — wins).
    static bool ProcessFileEdits(struct ContainerParams &ContainerParams, bool OverridePass = false,
                                 const std::filesystem::path &BaseDir = {});
    //Reads FilePath line by line and replaces any line starting with Key with Key+Value.
    //Useful for patching INI-style config files that use prefix-based key matching.
    static bool ConfigWrite(std::string Key, std::string Value, std::filesystem::path FilePath);
    //Writes Value as the complete content of FilePath, creating parent dirs if needed.
    static bool FileOverwrite(const std::string &Value, const std::filesystem::path &FilePath);

public:
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

public:
    //Run outcome, set by Execute() and readable by the caller (LaunchThread) to surface failures: the child's
    //exit code, whether it crashed, and whether the user asked to kill it (KillGame) — a kill isn't a failure.
    int  LastExitCode = 0;
    bool LastCrashed  = false;
    bool UserKilled   = false;
};
#endif // CONTAINERWRAPPER_H
