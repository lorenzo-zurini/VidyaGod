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
//the registry layer (InitializeDefPrefix), runner install (ImportRunner), and the orchestrator (Execute).
inline bool ArgReferencesContent(const std::string &RawArg)
{
    return RawArg.find("%Content%") != std::string::npos
        || RawArg.find("%ContentPath%") != std::string::npos;
}

//(The RunnerType enum is gone — a runner declares its invocation as data: EXECUTABLE + ARGS with %Content%/
//%ContentPath% tokens, CONTENT_ROOT for host mount placement, and PREFIX_GENERATE for a one-time wine prefix.
//The runner's TYPE no longer drives any C++ branch.)

//(ModuleInfo / VariantInfo and the pure manifest-query helpers now live in manifestmodel.h / ManifestModel.)

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

    //Runner config (resolved from RUNNERS arrays — package's own + global registry — by GUEST_PLATFORM membership):
    std::string RunnerID;                //RUNNER_ID — selected/pinned runner id (PASSED by picker/CLI, or resolved)
    std::string RunnerName;              //Human-readable runner name (e.g. "umu-proton")
    std::string RunnerVariantID;         //VARIANT_ID of the chosen runner variant (install artifact + exec params come from it)
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

    //Persistence (derived from PersistDir/PersistFile/RegPersist subcomponents — see DerivePersistence):
    bool PersistAll = false;                                        //DEFAULT — no persist subcomponents declared: RW union branch IS the durable UserDataPath (whole-runtime persist)
    bool PersistRegistry = false;                                   //a RegPersist subcomponent is present — persist user.reg/system.reg/userdef.reg
    std::vector<std::string> PersistDirs;                           //PersistDir subcomponents — runtime-root-relative leaf dirs bind-mounted from UserDataPath
    std::vector<std::string> PersistFiles;                          //PersistFile subcomponents — runtime-root-relative single files seeded/captured via UserDataPath
    std::vector<std::string> PersistRegKeys;                        //RegKeyPersist subcomponents — registry key paths (HKLM\.., HKCU\..) whose subtree is seeded/captured

    //Custom variables (from CustomVar subcomponents):
    std::map<std::string, std::string> CustomVariables;             //AUTO-RESOLVED: KEY → value; priority: CLI override > GlobalConfig > DEFAULT
    std::map<std::string, std::string> VariableOverrides;           //PASSED (CLI --var KEY=VALUE); highest-priority source for CustomVariables

    //Module toggles (optional modules only; REQUIRED modules ignore this):
    std::map<std::string, bool> ModuleStates;                       //PASSED (UI tree / --module COMP=on|off); component → enabled. Absent → REQUIRED||DEFAULT

    //Variant resolution:
    std::string VariantID;                                           //VARIANT_ID — resolved in DecideComponent (RECOMMENDED/first) or set by the caller

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
    std::filesystem::path DefPrefixPath;     //the wine-prefix artifact dir (TempPath/DEFPREFIX per-launch, or the installed __DEFPREFIX__/<variant>); mounted whole as the base layer when PrefixGenerate
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
    //Durable-backed mount: the vidyagodfs RUNTIME mount whenever durable data is reachable through it
    //(PersistAll writelayer or any RW passthrough persist dir). It exposes PackagePath/USERDATA, so it
    //MUST be non-lazily unmounted and verified before Cleanup() wipes TempPath — else remove_all could
    //recurse into real saves.
    std::vector<std::filesystem::path> CleanupPersistPaths;

    //Returns a map of all ContainerParams fields keyed by their %VARIABLE% token names.
    //Used by StringVariableSubstitution to expand tokens in runner ENV, args, and subcomponent paths.
    std::map<std::string, std::string> GetVariablesMap();
};

#endif // LAUNCHPARAMS_H
