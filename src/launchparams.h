#ifndef LAUNCHPARAMS_H
#define LAUNCHPARAMS_H

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "manifestmodel.h"   // NodeIndex (native node-graph launch source)

//True if a raw (pre-substitution) runner ARG references the launch target via a content token. Used to drop the
//content-bearing args when swapping the target — an OverrideExe (tooling) or the wineboot prefix-init. Shared by
//the registry layer (RegistryLayer), runner install (RunnerInstall), and the orchestrator (Execute).
inline bool ArgReferencesContent(const std::string &RawArg)
{
    return RawArg.find("%Content%") != std::string::npos
        || RawArg.find("%ContentPath%") != std::string::npos;
}

//(The RunnerType enum is gone — a runner declares its invocation as data: EXECUTABLE + ARGS with %Content%/
//%ContentPath% tokens, CONTENT_ROOT for host mount placement, and PREFIX_GENERATE for a one-time wine prefix.
//The runner's TYPE no longer drives any C++ branch.)

//(ModuleInfo / VariantInfo and the pure manifest-query helpers now live in manifestmodel.h / ManifestModel.)

//One resolved runner in the launch CHAIN (runner daisy-chaining). A runner is a directed edge GUEST→HOST in the
//platform graph; the chain nests them innermost→outermost: RunnerChain[0] runs the content, RunnerChain[i+1] runs
//RunnerChain[i]'s command, and the last link is the native terminal (HOST==GUEST==machine) that VidyaGod execve's
//directly. A length-1 chain ([content-runner, native]) reproduces the classic single-runner case. Resolved by
//LaunchResolver::PickRunnerChain; consumed by BuildNestedInvocation (execution) + vfsmount (cross-namespace mounts).
struct RunnerLink
{
    std::string NodeId;                          // runner NODE_ID
    std::string Name;                            // human label (= NodeId today)
    std::filesystem::path PackagePath;           // runner bundle dir (build layers + DEFPREFIX live here)
    std::string Executable;                      // EXEC.EXECUTABLE ("" = native passthrough: forward the inner command)
    std::vector<std::string> Args;               // EXEC.ARGS (raw, pre-substitution; %Content%/%ContentPath% = the inner target)
    nlohmann::ordered_json Env = nlohmann::ordered_json::object();   // EXEC.ENV
    std::vector<std::string> RemoveEnv;          // EXEC.REMOVE_ENV
    std::string ContentRoot;                     // EXEC.CONTENT_ROOT (non-empty ⇒ namespace boundary, e.g. wine drive_c)
    bool PrefixGenerate = false;                 // EXEC.PREFIX_GENERATE — this link needs a wine prefix
    bool UnifiedRuntime = false;                 // EXEC.UNIFIED_RUNTIME
    std::string GuestPathTemplate;               // EXEC.GUEST_PATH (Phase C: render an inner path into this runner's namespace; "" = identity)
    std::string HostPlatform;                    // PLATFORM.HOST — the platform this runner runs ON
    std::vector<std::string> GuestPlatform;      // PLATFORM.GUEST — the platforms this runner can run
    std::vector<nlohmann::ordered_json> Layers;  // build VFS layers (this runner's content closure)
    bool ShipsBuild = false;                     // has any build VFS layer

    //True when this link runs in the host namespace (no guest fs, host paths verbatim) — the native terminal and
    //native wrappers. Cross-namespace links (wine/proton via CONTENT_ROOT or PREFIX_GENERATE) need path translation.
    bool NativeNamespace() const { return ContentRoot.empty() && !PrefixGenerate; }
    //True for a pure passthrough terminal: forwards the inner command unchanged, only contributing env.
    bool Passthrough() const { return Executable.empty(); }
};

//All resolved parameters needed to build and launch a single container session.
//Populated in two stages:
//  1. ContainerParams constructor — stores only the PASSED values (PackagePath, IDs).
//  2. The LaunchResolver pipeline (InitializeFromNode/DerivePaths/…) — fills every other field from the node
//     graph + GlobalConfig.
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

    //Runner CHAIN (runner daisy-chaining): innermost→outermost resolved runner links. RunnerChain[0] runs the
    //content; the last link is the native terminal VidyaGod execve's. Resolved by PickRunnerChain. A length-1
    //chain is the classic single runner. The legacy single-runner fields below are the resolved view of the
    //chain's runtime BOUNDARY runner (the one that owns the FUSE mount / prefix) for back-compat.
    std::vector<RunnerLink> RunnerChain;
    std::vector<std::string> RunnerChainIds;     //PASSED (picker/CLI) — pinned chain (innermost→outermost runner ids); empty = auto-resolve

    //Runner config (resolved from RUNNERS arrays — package's own + global registry — by GUEST_PLATFORM membership):
    std::string RunnerID;                //RUNNER_ID — selected/pinned runner id (PASSED by picker/CLI, or resolved)
    std::string RunnerName;              //Human-readable runner name (e.g. "umu-proton")
    std::string RunnerExecutable;        //EXECUTABLE — binary to exec (e.g. "umu-run", "%RunnerMount%/proton"); %vars% expanded
    std::string ContentRoot;             //CONTENT_ROOT (resolved) — where game content mounts under RuntimePath ("" = root; "pfx/drive_c/<UID>" = proton)
    std::string PrefixRoot;              //DERIVED from ContentRoot (the part before /drive_c) — where a wine prefix's hives live ("" / "pfx")
    bool PrefixGenerate = false;         //PREFIX_GENERATE — runner needs a one-time generated wine prefix (DEFPREFIX), mounted as base
    nlohmann::ordered_json RunnerEnv;    //ENV — key/value env vars to set; values may contain %VARIABLE% tokens
    std::vector<std::string> RunnerRemoveEnv; //REMOVE_ENV — env keys to remove before launch (e.g. LD_LIBRARY_PATH)
    std::vector<std::string> RunnerArgs; //ARGS — the full argument vector (author composes %Content%/guest paths explicitly; no auto-append)
    std::vector<std::string> RunnerEndpoints; //RESOLVED — the selected runner's ENDPOINTS (its own components), mounted as recipe base
    nlohmann::ordered_json RunnerComponents = nlohmann::ordered_json::array(); //RESOLVED — COMPONENTS of the selected runner's registry package (empty for an embedded/PATH runner); folded into the component pool
    std::filesystem::path RunnerPackagePath; //RESOLVED — the selected runner package's LIBRARY dir; its build layers + DEFPREFIX hydrate/resolve here
    std::vector<std::string> RunnerRecipe; //RESOLVED — the selected runner variant's enabled component ids (its MODULES recipe); scopes runner CustomVar resolution to the active variant only

    //Flags
    std::string subgame_id;                                         //PASSED
    std::string component_id;                                       //PASSED (direct/editor mode — single endpoint)
    std::vector<std::string> Endpoints;                             //RESOLVED — terminal component ids (the selected variant's ENDPOINTS, load order)
    bool ReadOnlyVFS = false;                                       //SET — if true, no writable top layer (spec readonly=true); whole runtime is read-only
    bool UsesVFS = false;                                           //AUTO-DETECTED from SubComponentsArray
    bool AuthoringBare = false;                                     //AUTHORING — mount the node's content overlay + writable upper with NO runner/prefix and CONTENT_ROOT="" (a platform-agnostic capture workbench); an empty runtime is valid. InitializeFromNode skips runner resolution; BuildContainerRuntime skips the no-content guard.

    //Persistence (derived from the unified Persist primitive {KEEP/DROP} — see DerivePersistence). The four old types
    //(PersistDir/PersistFile/RegPersist/RegKeyPersist) collapsed into one self-describing layer; KEEP targets are
    //classified by shape into the buckets below. Purely additive (KEEP adds, DROP removes) — no mode; DEFAULT is
    //pristine (only KEEPs persist), and runner keep-sets capture the standard saves.
    bool PersistAll = false;                                        //a KEEP named the runtime root (`%RuntimePath%`) — RW union branch IS the durable UserDataPath (whole-runtime persist). Default false (pristine).
    std::vector<std::string> KeepDirs;                              //KEEP dir targets — runtime-root-relative dirs unioned as durable RW passthroughs from UserDataPath/<rel> (live)
    std::vector<std::string> KeepFiles;                             //KEEP file targets — runtime-root-relative single files seeded/captured by copy via UserDataPath/<rel>
    std::vector<std::string> KeepRegKeys;                           //KEEP registry-subtree targets (HKCU\Software\..) — partial-hive merge seed/capture (Wine-only)
    std::vector<std::string> KeepRegHives;                          //KEEP registry-hive targets — whole .reg filenames to persist (user/system/userdef.reg), copied whole (Wine-only)
    std::vector<std::string> DropPaths;                             //DROP path targets — runtime-root-relative paths shadowed by an ephemeral RW layer (writes discarded)
    nlohmann::ordered_json RunnerPersistLayers = nlohmann::ordered_json::array(); //RESOLVED — the boundary runner node's own LAYERS (its platform keep-set: where user-state lives), folded into DerivePersistence before the game's

    //Custom variables (from CustomVar subcomponents):
    std::map<std::string, std::string> CustomVariables;             //AUTO-RESOLVED: KEY → value; priority: CLI override > GlobalConfig > DEFAULT
    std::map<std::string, std::string> VariableOverrides;           //PASSED (CLI --var KEY=VALUE); highest-priority source for CustomVariables
    //AUTO-RESOLVED (out): secret+POOL keys seeded from their pool THIS launch because nothing was persisted yet.
    //A pool is a one-time seed, not a per-launch rotation, so the caller (which owns the mutable config) must
    //persist these into USERSETTINGS.VARIABLES — the game then keeps the same key forever (and the user can
    //still overwrite it in the prelaunch window). Empty once a value is persisted.
    std::map<std::string, std::string> PickedSecrets;

    //Module toggles (optional modules only; REQUIRED modules ignore this):
    std::map<std::string, bool> ModuleStates;                       //PASSED (UI tree / --module COMP=on|off); component → enabled. Absent → REQUIRED||DEFAULT

    //Variant resolution:
    std::string VariantID;                                           //VARIANT_ID — resolved in DecideComponent (RECOMMENDED/first) or set by the caller

    //The launch EXEC composed across the launchable's closure (field-level last-wins, launch node highest-priority) —
    //so a parent/base can supply CONTENTPATH/WORKDIR and the variant overrides EXEARGS, etc. Set by InitializeFromNode
    //from the DeclareExec layers; consumed by ResolveExecutableDefinition. Empty ⇒ fall back to the launch node's Exec.
    nlohmann::ordered_json ComposedExec;

    //Native node-graph launch (everything-is-a-node): when NodeIdx+LaunchNodeId are set, the engine resolves
    //EVERYTHING from the global node graph (InitializeFromNode) instead of from a MANIFESTJSON.
    const NodeIndex *NodeIdx = nullptr;                             //PASSED — the global cross-bundle node graph
    std::string LaunchNodeId;                                       //PASSED — the ROLE:"launchable" node to run

    //System Variables — queried from Qt at runtime
    std::string ScreenWidth;
    std::string ScreenHeight;

    //Paths (that need to be created before use)
    //Two storage tiers:
    //  EPHEMERAL — everything under TempPath = ~/.VidyaGod/TEMP/PackageUID; wiped by Cleanup().
    //              RuntimePath, WriteLayerPath and DefPrefixPath are all nested inside it.
    //  DURABLE   — UserDataPath = PackagePath/USERDATA; survives Cleanup(), travels with the package.
    //              Holds only the persisted state declared by the PERSIST manifest object.
    std::filesystem::path RuntimePath;       //TempPath/RUNTIME — the single mount root (= %RuntimePath%; STEAM_COMPAT_DATA_PATH / WINEPREFIX point here)
    std::filesystem::path WriteLayerPath;    //TempPath/WRITELAYER — ephemeral copy-on-write layer at the top of the VFS stack
    std::filesystem::path TempPath;          //~/.VidyaGod/TEMP/PackageUID — prefix, per-layer pre-mount dirs, reg patches
    std::filesystem::path UserDataPath;      //PackagePath/USERDATA — durable persist store (PERSIST.ALL / DIRS / REGISTRY)
    std::filesystem::path ProgramPath;       //RuntimePath / ContentRoot — where the game content sits (WORKDIR default)
    std::filesystem::path DefPrefixPath;     //the wine-prefix artifact dir (TempPath/DEFPREFIX per-launch, or the installed DEFPREFIX/<variant>); mounted whole as the base layer when PrefixGenerate
    std::filesystem::path DefaultDataPath;   //TempPath/DEFAULTDATA — RO layer holding all package-encoded base edits (Reg/File), between the component layers and the WRITELAYER; regenerated each launch
    std::filesystem::path RunnerRuntimePath; //Resolved runner SOURCE locator (e.g. the Proton build dir) — exposed as %RunnerRuntimePath%
    nlohmann::ordered_json RunnerSource;     //The selected runner's SOURCE block (if any) — fetched by EnsureSources before launch

    //Installed-runner model: the runner ships its own build as VFSZipLayer(s) (e.g. a Proton zip), mounted
    //read-only at its own mount; its DEFPREFIX is a one-time read-only per-runner artifact (no launch wineboot).
    bool                                 RunnerShipsBuild = false; //selected runner has a mounted build
    bool                                 UnifiedRuntime   = false; //mount the runner build INTO the game RUNTIME (rare; UNIFIED_RUNTIME)
    std::filesystem::path                RunnerMountPath;          //where the runner build is mounted — exposed as %RunnerMount%
    std::vector<nlohmann::ordered_json>  RunnerLayers;             //the runner's VFSZipLayer subcomponents to mount at RunnerMountPath
    std::filesystem::path ExePathRelative;   //CONTENTPATH — path relative to ProgramPath → %ContentPath% (compose guest paths from this)
    std::filesystem::path ExePathComplete;   //ProgramPath / ExePathRelative — absolute host path → %Content%
    std::filesystem::path WorkDirPathRelative;         //Working directory relative to ProgramPath, from MANIFEST WORKDIR
    std::filesystem::path WorkDirPathComplete;         //Absolute working directory; falls back to ProgramPath if unset

    //Wine / Proton specific:
    std::vector<std::string> ExeArgs;                               //DERIVED FROM MANIFESTJSON — split from EXEARGS string
    std::vector<std::string> DLLOverrides;                          //DERIVED FROM MANIFESTJSON — fed into WINEDLLOVERRIDES

    //VFSWRAPPER CLASS — the runtime is one vidyagodfs FUSE mount at RuntimePath (see BuildLayerSpec/MountVFS).
    std::vector<std::filesystem::path> CleanupUnmountPaths; //Purely-ephemeral FUSE mount(s) — lazy-unmounted on Cleanup()
    //Windows/WinFsp only: PIDs of the spawned vidyagodfs helpers. WinFsp unmounts when its serving process
    //exits, so Cleanup() terminates these (there is no fusermount3). Unused on Linux (fusermount3 by path).
    std::vector<long long> VfsHelperPids;
    //Durable-backed mount: the vidyagodfs RUNTIME mount whenever durable data is reachable through it
    //(whole-runtime keep's writelayer or any KEEP-dir RW passthrough). It exposes PackagePath/USERDATA, so it
    //MUST be non-lazily unmounted and verified before Cleanup() wipes TempPath — else remove_all could
    //recurse into real saves.
    std::vector<std::filesystem::path> CleanupPersistPaths;

    //SANDBOX (opt-in bubblewrap): the vidyagodfs (spec, mountpoint) pairs to RE-mount inside the sandbox namespace.
    //When sandboxed, BuildContainerRuntime still mounts on the host (to apply OVERRIDE edits into the writelayer),
    //then unmounts and records the specs here; Execute's bwrap runs `--sandbox-init` which mounts them inside the
    //game's own mount namespace (so the composed FS is scoped to the game, invisible to the host, self-cleaning).
    std::vector<std::pair<std::string, std::string>> SandboxMounts;

    //Returns a map of all ContainerParams fields keyed by their %VARIABLE% token names.
    //Used by StringVariableSubstitution to expand tokens in runner ENV, args, and subcomponent paths.
    std::map<std::string, std::string> GetVariablesMap();
};

#endif // LAUNCHPARAMS_H
