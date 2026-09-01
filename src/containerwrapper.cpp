#include "containerwrapper.h"
#include "commonutils.h"
#include "procenv.h"      // RunCommand / SystemToolEnv (fusermount on cleanup + the game launch)
#include "sandboxlayer.h"    // optional bubblewrap wrap of the launched game (opt-in via VIDYAGOD_SANDBOX)
#include "ipfswrapper.h"     // IpfsWrapper::OverlayServe/OverlayStop (nested-sandbox overlay TUN handoff)

#include <numeric>           // std::accumulate (WINEDLLOVERRIDES join)
#include <algorithm>         // std::min (session-id truncation for the TUN name)
#include <cstdlib>           // ::getenv (HOME for the writable bind)
#include <sys/stat.h>        // ::stat (probe the mounted prefix's system.reg mtime for config_info)

// The launcher-wide "sandbox on by default" setting (Settings.SandboxByDefault, default ON). An explicit
// VIDYAGOD_SANDBOX custom variable on a launch overrides it; an un-sandboxable machine (SandboxLayer::Available()
// probe) falls back to a normal unsandboxed launch regardless.
static bool SandboxDefaultOn(const nlohmann::ordered_json &GC)
{
    if (GC.contains("Settings") && GC["Settings"].is_object())
        return GC["Settings"].value("SandboxByDefault", true);
    return true;
}

// The friend LAN is ALWAYS ON (no setting): whenever the node is up and the friend LAN resolves, a launch joins the
// TRI-PLANE virtual LAN — the game runs in an isolated netns whose only interface is the overlay TUN, and the node
// itself is the gateway (no external helper): overlay unicast/broadcast to friends, an in-node gVisor NAT for
// internet + real-LAN unicast (VIDYAGOD_LAN_BRIDGE), and a userspace reflector for real-LAN broadcasts both ways
// (VIDYAGOD_LAN_HOSTRELAY). Discovery broadcasts stay on the overlay via the 255.255.255.255/32 route pinned by the
// sandbox-init. Escape hatches: VIDYAGOD_SANDBOX_NET=host skips the LAN; VIDYAGOD_LAN_BRIDGE=off = overlay-only;
// VIDYAGOD_LAN_HOSTRELAY=off = no real-LAN broadcast bridging.

#include <QThread>

#include <QMetaObject>

#include <chrono>

namespace {
//One launch phase, announced on entry (LogStep — drives the prelaunch progress bar) and timed on exit
//("done in N ms" lands in the console), so every step of a launch is visible AND measurable live.
//kLaunchSteps is the shared denominator: step 1 (catalog index) is emitted by the caller
//(LaunchThread / the CLI), 2..12 by this file.
constexpr int kLaunchSteps = 12;
struct StepScope {
    int K; std::string Label; std::chrono::steady_clock::time_point T0;
    StepScope(int k, std::string l) : K(k), Label(std::move(l)), T0(std::chrono::steady_clock::now())
    { LogStep(K, kLaunchSteps, Label); }
    ~StepScope()
    {
        auto Ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - T0).count();
        LogOut("LaunchStep", std::to_string(K) + "/" + std::to_string(kLaunchSteps) + " " + Label
               + " — done in " + std::to_string(Ms) + " ms");
    }
};
} // namespace

//The launch-build subsystems were lifted out into their own units (LaunchResolver / VarSubst / FileEdits /
//RegistryLayer / PersistLayer / VfsMount / LaunchSources / RunnerInstall — see their headers, all pulled in via
//containerwrapper.h). What remains here is the thin session lifecycle that orchestrates them. The only pure helper
//still referenced unqualified is IsVfsLayer, so bring ManifestModel in.
using namespace ManifestModel;

//Stores the JSON references and immediately runs the full initialization pipeline.
//After construction, ContainerParams is fully populated and ready for BuildContainerRuntime().
ContainerWrapper::ContainerWrapper(const nlohmann::ordered_json &Passed_GlobalConfigJSON, nlohmann::ordered_json &Passed_MANIFESTJSON, struct ContainerParams &Passed_ContainerParams)
    : ContainerParams(Passed_ContainerParams), GlobalConfigJSON(Passed_GlobalConfigJSON), MANIFESTJSON(Passed_MANIFESTJSON)   // match member declaration order (ContainerParams is declared first)
{
    InitOk = this->InitializeContainer();
}

//ContainerWrapper is now a thin SESSION ORCHESTRATOR: it owns the live config/manifest/ContainerParams and a single
//game session's lifecycle (construct → InitializeContainer → BuildContainerRuntime → Execute → Cleanup), delegating
//every step to the extracted subsystems. The registry is RegistryWrapper, the filesystem is the vidyagodfs binary,
//and the launch-engine logic lives in LaunchResolver/VfsMount/RegistryLayer/PersistLayer/FileEdits/LaunchSources/
//RunnerInstall (with the pure %token% engine in VarSubst).

//Runs the initialization steps in the required order:
//  DecideComponent → DeriveContainerParams → CreateRecipe → ResolveCustomVariables → BuildSubComponentsArray
//ResolveCustomVariables must precede BuildSubComponentsArray so that custom %KEY% tokens
//are already in GetVariablesMap() when subcomponent strings are substituted.
bool ContainerWrapper::InitializeContainer()
{
    StepScope Step(2, "Resolving the launch (closure, runner chain, variables, persistence)");
    LogOut("ContainerWrapper::InitializeContainer", "Initializing container...");
    //Everything is a node: resolve the whole container from the global node index (InitializeFromNode populates
    //the internal COMPONENTS pool the shared iterators consume). There is no MANIFESTJSON path any more.
    if (this->ContainerParams.NodeIdx && !this->ContainerParams.LaunchNodeId.empty())
        return LaunchResolver::InitializeFromNode(this->ContainerParams, this->MANIFESTJSON, this->GlobalConfigJSON);

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
    //A failed InitializeContainer left ContainerParams half-populated — building a runtime on top of it
    //produces confusing downstream failures (empty exec, no layers); fail loudly at the boundary instead.
    if (!InitOk)
    {
        LogErr("ContainerWrapper::BuildContainerRuntime", "Container initialization failed — refusing to build the runtime.");
        return false;
    }

    //Friend LAN (ALWAYS ON — see the header comment above): merge the LAN launch vars into CustomVariables up front so
    //the sandbox evaluation below sees SANDBOX=on + SANDBOX_NET=isolated (records the mounts, isolates the netns) and
    //Execute picks up SELF_VIP/SUBNET/PEER_* to bring the overlay TUN up inside the game's own netns. LanLaunchVars is
    //empty when the LAN can't resolve (node down) — the launch then proceeds with plain host networking. A package/user
    //var wins over the LAN's values (Execute merges CustomVariables AFTER this), so VIDYAGOD_SANDBOX_NET=host opts out.
    for (const auto &[K, V] : IpfsWrapper::LanLaunchVars())
        if (!ContainerParams.CustomVariables.count(K))
            ContainerParams.CustomVariables[K] = V;

    //Clear any runtime left under TempPath by a previously crashed/incomplete run BEFORE mounting.
    //Stale union/zip/bind mounts and leftover RUNTIME/DEFPREFIX/staging dirs would otherwise corrupt
    //this build (or cause spurious "already mounted"/EEXIST failures).
    {
        StepScope Step(3, "Sweeping stale runtime state");
        VfsMount::CleanStaleRuntime(ContainerParams.TempPath);
    }

    //Verify every dependency is satisfiable (runner build cached, DEFPREFIX present, game layers local-or-fetchable),
    //then materialize any missing game-layer content from a backend (IPFS) to its expected local path. Both run
    //before the prefix layers mount — the content must exist on disk by then. Abort if anything can't be provided.
    {
        StepScope Step(4, "Checking and materializing content sources");
        if (!LaunchSources::EnsureSources(this->ContainerParams))
        {
            LogErr("ContainerWrapper::BuildContainerRuntime", "Required sources unavailable — aborting.");
            return false;
        }
        if (!LaunchSources::MaterializeLayers(this->ContainerParams))
        {
            LogErr("ContainerWrapper::BuildContainerRuntime", "Could not materialize required layer content — aborting.");
            return false;
        }
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

    //A runner that neither generates a prefix nor mounts VFS layers has no content to run — malformed. EXCEPT in the
    //authoring bare runtime, where an empty writable overlay (no content yet) is exactly the point: a capture workbench.
    if (!PrefixGen && !ContainerParams.UsesVFS && !ContainerParams.AuthoringBare)
    {
        LogWarn("ContainerWrapper::BuildContainerRuntime", "No VFS layers found. Package may be malformed.");
        return false;
    }

    //Mount the runner's shipped build READ-ONLY at RunnerMountPath — proton's wine prefix OR a NATIVE runner's runtime
    //(e.g. a java runner's JRE, shipped via its jre_N PARENT). This must run whenever the runner ships a build; it is
    //NOT gated on PrefixGenerate — a native runner ships a build without generating a wine prefix, and gating on
    //PrefixGen was why a java runner's %RunnerMount%/__jre never got mounted (execvp → 127). MountRunnerBuild self-guards
    //on RunnerShipsBuild/UnifiedRuntime, so this is a no-op for a runnerless / unified / build-less launch.
    {
        StepScope Step(5, "Mounting the runner build");
        if (!VfsMount::MountRunnerBuild(this->ContainerParams)) return false;
    }

    //Provision the Wine prefix LAYOUT probe — ONLY when the runner GENERATES a prefix (proton). Probes the mounted
    //runner's layout (new files/lib vs old dist/lib64) into launch vars so a runner node's prefix-assembly layers
    //(default_pfx/DLL VFSDirLayers + config_info FileEdit) are IDENTICAL across all ~150 versions — the layout paths +
    //system.reg mtime resolve from the live mount, mirroring proton's fonts_dir/lib_dir/default_pfx_dir + system.reg
    //mtime so its config_info fast-path fires (zero-copy). Irrelevant to native runners (no wine prefix).
    if (PrefixGen && ContainerParams.RunnerShipsBuild && !ContainerParams.RunnerMountPath.empty())
    {
        StepScope Step(6, "Probing the wine prefix layout");
        const std::filesystem::path RM = ContainerParams.RunnerMountPath;
        std::error_code Pec;
        const std::string Root = std::filesystem::exists(RM / "files" / "lib" / "wine", Pec) ? "files" : "dist";
        const std::string Lib  = (Root == "files") ? "lib" : "lib64";
        //The two PE dirs are probed INDEPENDENTLY: newer protons unify both arches under lib/wine, but
        //GE-Proton7-era builds split them (lib/wine/i386-windows + lib64/wine/x86_64-windows). Deriving both from
        //one Lib pointed WineSys32Dir at a nonexistent dir on GE-7 — the system32 assembly layer mounted EMPTY
        //(caught by the mount plan's missing-source check during the 2026-09-01 overnight run; harmless for a
        //32-bit game, fatal for any 64-bit one on that runner).
        auto PeDir = [&](const char *Arch) -> std::string {
            for (const char *L : {"lib", "lib64"})
            {
                std::error_code E;
                const std::filesystem::path P = RM / Root / L / "wine" / Arch;
                if (std::filesystem::exists(P, E)) return P.string();
            }
            return (RM / Root / Lib / "wine" / Arch).string();   // old behavior as the last resort
        };
        auto &V = ContainerParams.CustomVariables;
        V["DefaultPfxDir"]   = (RM / Root / "share" / "default_pfx").string() + "/";
        V["WineFontsDir"]    = (RM / Root / "share" / "fonts").string() + "/";
        V["WineLibDir"]      = (RM / Root / Lib).string() + "/";
        V["WineSys32Dir"]    = PeDir("x86_64-windows");
        V["WineSysWow64Dir"] = PeDir("i386-windows");
        struct stat St{};
        const std::string SysReg = (RM / Root / "share" / "default_pfx" / "system.reg").string();
        V["SysRegMtime"] = (::stat(SysReg.c_str(), &St) == 0) ? (std::to_string((long long)St.st_mtime) + ".0") : "0";
        LogOut("ContainerWrapper::BuildContainerRuntime", "Probed prefix layout: " + Root + " (sysreg mtime " + V["SysRegMtime"] + ")");
    }

    //Materialise every package-encoded BASE edit into the DEFAULTDATA layer (between the component layers
    //and the WRITELAYER). FileEdits apply to any runner; base RegEdits are Wine-only (handled inside).
    //OBSERVABILITY STAGE (see the overhaul plan): failures are surfaced loudly but not yet fatal — after
    //a few days of daily-use logs prove which ones occur in practice, the fatal/warn split gets decided.
    {
        StepScope Step(7, "Building DEFAULTDATA (base edits + composed registry)");
        if (!RegistryLayer::BuildDefaultData(this->ContainerParams))
            LogWarn("ContainerWrapper::BuildContainerRuntime", "BuildDefaultData reported failures — DEFAULTDATA may be incomplete.");
    }

    {
        StepScope Step(8, "Seeding persisted saves (registry + files)");
        //Seed any persisted KEEP hives into the ephemeral WRITELAYER so they shadow DEFPREFIX.
        //No-op when PersistAll (durable RW branch already holds the reg files).
        if (!ContainerParams.KeepRegHives.empty() && !RegistryLayer::SeedPersistRegistry(this->ContainerParams))
            LogWarn("ContainerWrapper::BuildContainerRuntime", "Persisted registry seed reported failures — saved settings may be missing this session.");

        //Seed any persisted KEEP files into the WRITELAYER so they shadow their lower layers.
        //No-op when PersistAll or when none are declared.
        if (!ContainerParams.KeepFiles.empty() && !PersistLayer::SeedPersistFiles(this->ContainerParams))
            LogWarn("ContainerWrapper::BuildContainerRuntime", "Persisted file seed reported failures — saved files may be missing this session.");
    }

    //Verify every layer's locator resolves before mounting — a missing source (e.g. a moved local file,
    //or a remote SOURCE not yet fetched) is surfaced now rather than silently mounting nothing.
    //TODO(sharing): block / offer to fetch when a remote SOURCE is missing; check runner + cross-package deps.
    if (std::vector<std::string> Missing = LaunchSources::VerifyDependencies(this->ContainerParams); !Missing.empty())
        LogWarn("ContainerWrapper::BuildContainerRuntime",
                std::to_string(Missing.size()) + " layer source(s) missing — the runtime may be incomplete.");

    //Build the layer-spec (DEFPREFIX + VFS subcomponents target-rooted + PERSIST dirs as RW
    //passthrough) and mount it as a single vidyagodfs filesystem at RuntimePath.
    {
        StepScope Step(9, "Assembling and mounting the runtime filesystem");
        if (!VfsMount::MountVFS(this->ContainerParams)) return false;
    }
    //Case-conflict scan: a FULL walk of the freshly-mounted FUSE tree (every file, through the FS) whose
    //result was advisory-only — pure launch latency on big games. --validate-nodes already case-checks
    //package data at rest, so the runtime walk is DEBUG-opt-in now (VG_CASE_CHECK=1).
    //Advisory by design (it warns via its own dialog/log); the launch continues either way.
    if (const char *CC = std::getenv("VG_CASE_CHECK"); CC && *CC == '1')
        (void)VfsMount::CheckCaseConflicts(ContainerParams.RuntimePath);

    //Post-mount: OVERRIDE edits operate on the mounted runtime (COW directly to WRITELAYER) — they win
    //unconditionally, over both the package content AND the user's persisted state. OVERRIDE FileEdits apply
    //to any runner; DLL overrides and OVERRIDE RegEdits are Wine-only. Base edits already live in DEFAULTDATA.
    {
        StepScope Step(10, "Applying override edits (files, DLL overrides, registry)");
        //These are ERRORS, not warnings. A failed edit means a package setting the author declared was NOT applied,
        //so the game runs misconfigured while everything looks fine — precisely the failure that hid for months.
        //They are non-fatal (the game may still be playable) but they must be loud, and they feed the launch verdict.
        if (!FileEdits::ProcessFileEdits(this->ContainerParams, /*OverridePass=*/true))
            LogErr("ContainerWrapper::BuildContainerRuntime",
                   "OVERRIDE FileEdits FAILED — package settings above were not applied; the game is misconfigured.");
        if (PrefixGen)
        {
            if (!FileEdits::ProcessDLLOverrides(this->ContainerParams))
                LogErr("ContainerWrapper::BuildContainerRuntime",
                       "DllOverride collection contained malformed entries — a DLL override was NOT applied.");
            if (!RegistryLayer::ApplyOverrideRegEdits(this->ContainerParams))
                LogErr("ContainerWrapper::BuildContainerRuntime",
                       "OVERRIDE RegEdits FAILED to write the runtime hives — registry settings were not applied.");
        }
    }

    //----- SANDBOX prep: record the mounted spec(s) so the sandbox can RE-mount them inside its own namespace, ON TOP
    //of the host mount (which stays put — the game sees the stacked in-namespace mount; the host mount serves any
    //unsandboxed path and cleanup). The OVERRIDE edits above already landed in the writelayer, which both mounts
    //share, so the in-sandbox mount is identical. Gated on the real probe so we only prepare this when the machine can
    //actually sandbox; tooling/authoring/override runs (which never sandbox) simply use the host mount as today. -----
    if (SandboxLayer::Requested(ContainerParams, SandboxDefaultOn(GlobalConfigJSON)) && SandboxLayer::Available())
    {
        StepScope Step(11, "Preparing the sandbox");
        auto Record = [&](const std::filesystem::path &SpecPath, const std::filesystem::path &Mnt){
            if (std::filesystem::exists(SpecPath))
                ContainerParams.SandboxMounts.emplace_back(SpecPath.string(), Mnt.string());
        };
        Record(ContainerParams.TempPath / "vidyagodfs.spec.json", ContainerParams.RuntimePath);
        //The RUNNER build mount (e.g. a native runner's JRE) must be bound INSIDE the sandbox too, else its executable
        //(%RunnerMount%/...) is unreachable in the namespace and execvp fails with 127. RunnerShipsBuild already proves a
        //build mount was created (its content may live entirely in the runner's PARENTS, so the runner NODE's own LAYERS
        //can be empty — do NOT gate on that); Record() no-ops if the runner spec wasn't written.
        if (ContainerParams.RunnerShipsBuild && !ContainerParams.UnifiedRuntime)
            Record(ContainerParams.TempPath / "vidyagodfs.runner.spec.json", ContainerParams.RunnerMountPath);
    }

    LogSucc("ContainerWrapper::BuildContainerRuntime", "Runtime ready.");
    return true;
}

//(GetPackageUserSettings / SetPackageUserSetting moved to PackageCatalog — see packagecatalog.h.)



//(RunCommand — the generic synchronous QProcess runner — moved to procenv.{h,cpp} as a free function.)







//(LayerLocator / ResolveLayerSource moved to ManifestModel — see manifestmodel.h.)











//(PackageIpfsCids / PackageCoverCids moved to ManifestModel — see manifestmodel.h.)

//(ImportPackage / PublishPackage / MirrorDehydrated / IsPackageImported moved to PackageCatalog — see packagecatalog.h.)














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
bool ContainerWrapper::Execute(const std::string &OverrideExe)
{
    LogStep(12, kLaunchSteps, "Starting the game process (wine boot + game init follow in the console)");
    const bool Override = !OverrideExe.empty();
    auto Subst = [&](std::string S){ VarSubst::StringVariableSubstitution(S, ContainerParams.GetVariablesMap()); return S; };

    //----- CROSS-NAMESPACE NESTING: when an inner runner runs inside the boundary's guest fs (e.g. a win32 emulator
    //under proton), redirect the boundary's content target at the inner runners' guest command. ComposeGuestTarget
    //returns {CrossNamespace:false} for every classic chain, so this whole block is a no-op there. The inner runners'
    //own args (the ROM, etc.) are appended after the boundary's ARGS. Not applied under an OverrideExe (tooling). -----
    std::vector<std::string> CrossTrailingArgs;
    if (!Override)
    {
        LaunchResolver::GuestTarget GT = LaunchResolver::ComposeGuestTarget(ContainerParams);
        if (GT.CrossNamespace)
        {
            //Point the boundary's %ContentPath%/%Content% at the inner runner's guest exe (its ARGS template composes
            //the guest path from these), and carry the inner args to append after the boundary's ARGS.
            ContainerParams.ExePathRelative = std::filesystem::path(GT.ContentRel);
            ContainerParams.ExePathComplete = ContainerParams.ProgramPath / GT.ContentRel;
            CrossTrailingArgs = GT.TrailingArgs;
            LogOut("ContainerWrapper::Execute", "Cross-namespace target: " + GT.ContentRel
                   + " (+" + std::to_string(CrossTrailingArgs.size()) + " inner arg(s))");
        }
    }

    //----- BASE command: the BOUNDARY runner runs the content (runner daisy-chaining) -----
    //The legacy single-runner fields are the boundary runner's view (the link that owns the FUSE mount / prefix —
    //proton for a win32 game, or the native terminal for native content). An EMPTY RunnerExecutable = native
    //passthrough: run the content's own executable (%Content%) directly. Otherwise the runner's EXECUTABLE + ARGS ARE
    //the command (author composes the guest path into ARGS, e.g. proton's ["waitforexitandrun","C:\\%PackageUID%\\..."]).
    VarSubst::StringVariableSubstitution(ContainerParams.RunnerExecutable, ContainerParams.GetVariablesMap());
    const bool NativeBoundary = ContainerParams.RunnerExecutable.empty();
    std::string Program;
    QStringList Arguments;
    if (NativeBoundary)
    {
        //Native passthrough boundary: exec the content directly (or the OverrideExe for tooling).
        Program = Override ? OverrideExe : ContainerParams.ExePathComplete.string();
        if (!Override) for (const std::string &A : ContainerParams.ExeArgs) Arguments.append(QString::fromStdString(A));
    }
    else
    {
        Program = ContainerParams.RunnerExecutable;
        //For an OverrideExe (tooling/install), drop the content-bearing ARGS and append the override after the verb.
        for (const std::string &Raw : ContainerParams.RunnerArgs)
        {
            if (Override && ArgReferencesContent(Raw)) continue;
            Arguments.append(QString::fromStdString(Subst(Raw)));
        }
        //Cross-namespace: after the boundary's ARGS (which now reference the inner runner's guest exe via the
        //redirected %ContentPath%), append the inner runners' own guest args (e.g. the ROM path).
        for (const std::string &A : CrossTrailingArgs) Arguments.append(QString::fromStdString(A));
        if (Override) Arguments.append(QString::fromStdString(OverrideExe));
        else for (const std::string &A : ContainerParams.ExeArgs) Arguments.append(QString::fromStdString(A));
    }

    //----- NESTING: wrap the base command with each OUTER same-namespace link (native wrappers + the native terminal) -----
    //chain = innermost(content)→outermost(machine). The boundary is the last cross-namespace link (or the first link for
    //a native chain); links AFTER it run IN THE HOST NAMESPACE and simply wrap the inner argv (`wrapper <args> <innercmd>`).
    //An empty-EXECUTABLE link is a passthrough terminal (forwards unchanged). Cross-namespace nesting (an inner emulator
    //inside this runner's guest fs) is Phase C — skipped here with a warning. Wrappers are not applied under OverrideExe.
    int BoundaryIdx = 0;
    for (int i = 0; i < (int)ContainerParams.RunnerChain.size(); ++i)
        if (!ContainerParams.RunnerChain[i].NativeNamespace()) BoundaryIdx = i;
    std::vector<const RunnerLink*> OuterWrappers;
    if (!Override)
        for (int i = BoundaryIdx + 1; i < (int)ContainerParams.RunnerChain.size(); ++i)
        {
            const RunnerLink &L = ContainerParams.RunnerChain[i];
            if (!L.NativeNamespace())
            { LogWarn("ContainerWrapper::Execute", "Cross-namespace wrapper '" + L.NodeId + "' not yet supported (Phase C) — skipped."); continue; }
            //A native terminal whose EXECUTABLE is empty or runs %Content% is a passthrough — as an OUTER wrapper it
            //forwards the inner argv unchanged (the inner command IS its content). Only a real wrapper TOOL (e.g.
            //gamescope/mangohud, EXECUTABLE=a program) actually wraps. This keeps the appended native terminal a no-op
            //for the common [proton, native] chain (byte-identical to a direct proton launch).
            if (L.Executable.empty() || ArgReferencesContent(L.Executable)) continue;
            QStringList Wrapped;
            for (const std::string &Raw : L.Args) Wrapped.append(QString::fromStdString(Subst(Raw)));
            Wrapped.append(QString::fromStdString(Program));                     // inner program ...
            Wrapped.append(Arguments);                                          // ... then its args
            Program   = Subst(L.Executable);
            Arguments = Wrapped;
            OuterWrappers.push_back(&L);
            LogOut("ContainerWrapper::Execute", "Chain wrap: " + L.NodeId + " (" + Program + ")");
        }

    LogOut("ContainerWrapper::Execute", "Command: " + Program);
    if (!OverrideExe.empty()) LogOut("ContainerWrapper::Execute", "OverrideExe: " + OverrideExe);
    LogOut("ContainerWrapper::Execute", "WorkDirPath: " + ContainerParams.WorkDirPathComplete.string());

    //----- ENV: one process tree, so every link's ENV/REMOVE_ENV applies. Boundary (legacy fields) first, then the
    //outer same-namespace wrappers — boundary (innermost, content-proximate) WINS on key conflicts. -----
    QProcessEnvironment RunProcessEnvironment = SystemToolEnv();                 // system runner, not AppImage libs
    for (const std::string &Key : ContainerParams.RunnerRemoveEnv)
        RunProcessEnvironment.remove(QString::fromStdString(Key));
    for (auto &[Key, Value] : ContainerParams.RunnerEnv.items())
        RunProcessEnvironment.insert(QString::fromStdString(Key), QString::fromStdString(Subst(Value.get<std::string>())));
    for (const RunnerLink *L : OuterWrappers)
    {
        for (const std::string &Key : L->RemoveEnv) RunProcessEnvironment.remove(QString::fromStdString(Key));
        for (auto &[Key, Value] : L->Env.items())
            if (!RunProcessEnvironment.contains(QString::fromStdString(Key)))    // innermost/boundary wins
                RunProcessEnvironment.insert(QString::fromStdString(Key), QString::fromStdString(Subst(Value.get<std::string>())));
    }

    // WINEDLLOVERRIDES — only meaningful for runners that have a wine prefix. MERGED with any value already in
    // the environment rather than replacing it: this line silently clobbered an operator's debugging override
    // (WINEDLLOVERRIDES="d3d11,dxgi=b" to force wined3d on a GPU whose Vulkan is too old for DXVK) and cost an
    // hour of "why does my env var do nothing" during the 2026-09-01 laptop triage. External entries go LAST —
    // measured empirically in the same triage: with a dll named in both lists, wine honored the LATER entry
    // (external-first left the package's d3d11=n winning while dxgi flipped) — so last place is what makes a
    // human's override beat the package's.
    if (ContainerParams.PrefixGenerate && !ContainerParams.DLLOverrides.empty())
    {
        QString Composed = QString::fromStdString(std::accumulate(ContainerParams.DLLOverrides.begin(), ContainerParams.DLLOverrides.end(), std::string{}, [](const std::string &a, const std::string &b){ return a.empty() ? b : a + ";" + b; }));
        const QString External = qEnvironmentVariable("WINEDLLOVERRIDES");
        if (!External.isEmpty())
        {
            LogOut("ContainerWrapper::Execute", "WINEDLLOVERRIDES from the environment merged ahead of the package's: " + External.toStdString());
            Composed = (Composed.isEmpty() ? QString() : Composed + ";") + External;
        }
        RunProcessEnvironment.insert("WINEDLLOVERRIDES", Composed);
    }

    //If the configured working directory doesn't exist in the mounted runtime, fall back
    //gracefully: prefer the RuntimePath (VFS root) if VFS was used, otherwise PackagePath.
    std::filesystem::path FinalWorkDir = ContainerParams.WorkDirPathComplete;
    if (!std::filesystem::exists(FinalWorkDir))
    {
        FinalWorkDir = ContainerParams.UsesVFS ? ContainerParams.RuntimePath : ContainerParams.PackagePath;
        LogOut("ContainerWrapper::Execute", "WorkDirPath does not exist, falling back to " + FinalWorkDir.string());
    }

    //----- SANDBOX (opt-in VIDYAGOD_SANDBOX): re-invoke ourselves as the nested `--sandbox-init` inside one hermetic
    //bwrap namespace, which re-mounts the vidyagodfs spec(s) (deferred + host-unmounted in BuildContainerRuntime),
    //optionally brings up the overlay TUN, then execs the game. RO host root + writable runtime/home; the composed FS
    //and the virtual LAN live entirely inside the sandbox. Not applied under an OverrideExe. Default OFF → byte-
    //identical launch. -----
    bool OverlayServing = false;
    if (!Override)
    {
        SandboxLayer::Options SbOpts = SandboxLayer::FromParams(ContainerParams, true);   // Enabled is always true (MANDATORY)
        if (!SandboxLayer::Available())
        {
#ifdef _WIN32
            LogErr("ContainerWrapper::Execute", "Sandbox is MANDATORY but unavailable (Sandboxie not installed / SbieSvc not running) — refusing to launch unsandboxed.");
#else
            LogErr("ContainerWrapper::Execute", "Sandbox is MANDATORY but unavailable (no bwrap / unprivileged user namespaces disabled) — refusing to launch unsandboxed.");
#endif
            return false;
        }
        {
            for (const auto &[Spec, Mnt] : ContainerParams.SandboxMounts)
                SbOpts.Mounts.push_back({Spec, Mnt});
            SbOpts.WritableBinds = { ContainerParams.TempPath.string(), ContainerParams.UserDataPath.string() };
            if (const char *H = ::getenv("HOME")) SbOpts.WritableBinds.emplace_back(H);
            SbOpts.WorkDir = ContainerParams.WorkDirPathComplete.string();

            //Friend LAN: the LAN vars (SELF_VIP/SUBNET/PEER_*/SANDBOX_NET=isolated) were merged into CustomVariables in
            //BuildContainerRuntime when Settings.LanEnabled. Bring up the overlay TUN INSIDE the sandbox's own netns
            //(the host stack is never touched): listen on a bound unix socket for the TUN fd the sandbox-init hands
            //back, and pass the vIP + socket to the init.
            auto Var = [&](const char *K){ auto It = ContainerParams.CustomVariables.find(K);
                                           return It == ContainerParams.CustomVariables.end() ? std::string() : It->second; };
            const std::string SelfVip = Var("VIDYAGOD_SELF_VIP"), Subnet = Var("VIDYAGOD_SUBNET");
            if (SbOpts.Net == SandboxLayer::NetMode::Isolated && !SelfVip.empty())
            {
                const std::string Sock = (ContainerParams.TempPath / "overlay.sock").string();
                auto GateOn = [&](const char *K){ auto It = ContainerParams.CustomVariables.find(K);
                                                  return It == ContainerParams.CustomVariables.end() || It->second != "off"; };
                const bool Bridge    = GateOn("VIDYAGOD_LAN_BRIDGE");     // in-node NAT: internet + real-LAN unicast
                const bool HostRelay = GateOn("VIDYAGOD_LAN_HOSTRELAY");  // real-LAN broadcast reflector
                std::string OErr;
                if (IpfsWrapper::OverlayServe(Sock, Bridge, HostRelay, &OErr))
                {
                    std::string Mask = "16";
                    if (auto S = Subnet.rfind('/'); S != std::string::npos) Mask = Subnet.substr(S + 1);
                    SbOpts.TunName   = "vg-lan";
                    SbOpts.TunCidr   = SelfVip + "/" + Mask;
                    SbOpts.TunSock   = Sock;
                    SbOpts.TunBridge = Bridge;   // sandbox-init adds the default route only when the NAT is up
                    OverlayServing = true;
                }
                else
                    LogWarn("ContainerWrapper::Execute", "overlay serve failed (launching without the virtual LAN): " + OErr);
            }

            //Window cloak (X-side): a package that knows its game shows an ugly, unkillable-from-win32 startup
            //window declares VIDYAGOD_CLOAK = "<caption>|<maxwidth>"; sandbox-init forks an xcb watcher that
            //holds any matching small window unmapped until the real (wider) game window appears.
            if (auto It = ContainerParams.CustomVariables.find("VIDYAGOD_CLOAK");
                It != ContainerParams.CustomVariables.end() && !It->second.empty())
                SbOpts.Cloak = It->second;

            SandboxLayer::Wrap(SbOpts, Program, Arguments);
#ifdef _WIN32
            LogOut("ContainerWrapper::Execute", "Sandbox: Sandboxie box (host FS + registry virtualization; VidyaGod writelayer/saves granted pass-through)");
#else
            LogOut("ContainerWrapper::Execute", std::string("Sandbox: nested bubblewrap (net ")
                   + (SbOpts.Net == SandboxLayer::NetMode::Isolated ? "isolated" : "host") + ", "
                   + std::to_string(SbOpts.Mounts.size()) + " mount(s)" + (OverlayServing ? ", overlay" : "") + ")");
#endif
        }
    }

    QProcess RunProcess;
    RunProcess.setProgram(QString::fromStdString(Program));
    RunProcess.setWorkingDirectory(QString::fromStdString(FinalWorkDir.string()));
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

    //Tear the overlay forwarder + its handoff socket down now the sandboxed game (and its in-sandbox TUN) are gone.
    if (OverlayServing) IpfsWrapper::OverlayStop();

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
#ifdef _WIN32
        //Kill the launched process tree — taskkill /T reaps children (Start.exe + the game, or the game
        //directly). Then terminate the Sandboxie box so no boxed helper lingers (no-op if unsandboxed).
        QProcess Pr; Pr.start("taskkill", {"/PID", QString::number(Pid), "/T", "/F"}); Pr.waitForFinished(-1);
        SandboxLayer::TerminateBox(this->ContainerParams.WorkDirPathComplete.string());
#else
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
#endif
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

    //1. Registry + file capture must read RUNTIME/<...> before anything is unmounted. No-op when PersistAll.
    //A capture failure means the user's session state was NOT saved — say so loudly (still proceed with
    //cleanup: the runtime must come down regardless).
    if (!ContainerParams.KeepRegHives.empty() && !ContainerParams.PersistAll
        && !RegistryLayer::CapturePersistRegistry(this->ContainerParams))
        LogErr("ContainerWrapper::Cleanup", "Registry persistence capture FAILED — this session's registry changes were not saved.");
    if (!ContainerParams.KeepFiles.empty() && !ContainerParams.PersistAll
        && !PersistLayer::CapturePersistFiles(this->ContainerParams))
        LogErr("ContainerWrapper::Cleanup", "File persistence capture FAILED — this session's saved files were not stored.");
    if (!ContainerParams.KeepRegKeys.empty() && !ContainerParams.PersistAll
        && !RegistryLayer::CapturePersistRegKeys(this->ContainerParams))
        LogErr("ContainerWrapper::Cleanup", "Registry-key persistence capture FAILED — this session's key state was not stored.");

    //2. Durable-backed mounts: non-lazy + verified, innermost-first (reverse registration order).
    bool DurableUnmountOk = true;
#ifdef _WIN32
    // WinFsp has no fusermount3: it unmounts when its serving process exits, and the FSD tears the mount
    // down even on a forced kill (the mount is owned by the process handle). So terminate every spawned
    // vidyagodfs helper (/F is synchronous on termination, /T also reaps any children), then confirm the
    // durable mountpoints have vanished before we allow the TEMP wipe.
    for (long long Pid : ContainerParams.VfsHelperPids)
        RunCommand("taskkill", {"/PID", std::to_string(Pid), "/T", "/F"}, SystemToolEnv());
    for (auto It = ContainerParams.CleanupPersistPaths.rbegin(); It != ContainerParams.CleanupPersistPaths.rend(); ++It)
    {
        bool Gone = false;
        for (int Attempt = 0; Attempt < 25 && !Gone; ++Attempt)   // ~5s for the kernel to drop the mount
        {
            if (!std::filesystem::exists(*It)) Gone = true;
            else QThread::msleep(200);
        }
        if (!Gone)
        {
            LogErr("ContainerWrapper::Cleanup", "DURABLE WinFsp mount still present after helper kill: " + It->string());
            DurableUnmountOk = false;
        }
    }
    // Ephemeral mounts: best-effort settle, never gate the wipe.
    for (const std::filesystem::path &UnmountPath : ContainerParams.CleanupUnmountPaths)
        for (int Attempt = 0; Attempt < 15 && std::filesystem::exists(UnmountPath); ++Attempt) QThread::msleep(200);
#else
    for (auto It = ContainerParams.CleanupPersistPaths.rbegin(); It != ContainerParams.CleanupPersistPaths.rend(); ++It)
    {
        if (!VfsMount::UnmountDurable(It->string()))
        {
            LogErr("ContainerWrapper::Cleanup", "DURABLE mount still busy after retries (lazy-detached): " + It->string());
            DurableUnmountOk = false;
        }
    }

    //3. Ephemeral VFS mounts: lazy unmount is fine (their RW/source is all under TempPath).
    for (const std::filesystem::path &UnmountPath : ContainerParams.CleanupUnmountPaths)
        VfsMount::UnmountLazy(UnmountPath.string());
#endif

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


