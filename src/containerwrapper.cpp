#include "containerwrapper.h"
#include "commonutils.h"
#include "processenv.h"      // RunCommand / SystemToolEnv (fusermount on cleanup + the game launch)
#include "sandboxlayer.h"    // optional bubblewrap wrap of the launched game (opt-in via VIDYAGOD_SANDBOX)
#include "ipfswrapper.h"     // IpfsWrapper::OverlayServe/OverlayStop (nested-sandbox overlay TUN handoff)

#include <numeric>           // std::accumulate (WINEDLLOVERRIDES join)
#include <algorithm>         // std::min (session-id truncation for the TUN name)
#include <cstdlib>           // ::getenv (HOME for the writable bind)

// The launcher-wide "sandbox on by default" setting (Settings.SandboxByDefault, default ON). An explicit
// VIDYAGOD_SANDBOX custom variable on a launch overrides it; an un-sandboxable machine (SandboxLayer::Available()
// probe) falls back to a normal unsandboxed launch regardless.
static bool SandboxDefaultOn(const nlohmann::ordered_json &GC)
{
    if (GC.contains("Settings") && GC["Settings"].is_object())
        return GC["Settings"].value("SandboxByDefault", true);
    return true;
}

// The friend-LAN opt-in (Settings.LanEnabled, default OFF). When on, a launch joins the virtual LAN of friends: the
// game runs in an ISOLATED netns (no host network) with a TUN carrying only the overlay. Off by default because
// isolated networking cuts the game off from the internet — you turn it on to play LAN together.
static bool LanEnabled(const nlohmann::ordered_json &GC)
{
    if (GC.contains("Settings") && GC["Settings"].is_object())
        return GC["Settings"].value("LanEnabled", false);
    return false;
}

#include <QThread>
#include <QMetaObject>

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
    this->InitializeContainer();
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
    //Friend LAN (opt-in, Settings.LanEnabled): merge the LAN launch vars into CustomVariables up front so the sandbox
    //evaluation below sees SANDBOX=on + SANDBOX_NET=isolated (records the mounts, forces the isolated netns) and Execute
    //picks up SELF_VIP/SUBNET/PEER_* to bring the overlay TUN up inside the game's own netns. The LAN is sandbox-only.
    if (LanEnabled(GlobalConfigJSON))
        for (const auto &[K, V] : IpfsWrapper::LanLaunchVars())
            ContainerParams.CustomVariables[K] = V;

    //Clear any runtime left under TempPath by a previously crashed/incomplete run BEFORE mounting.
    //Stale union/zip/bind mounts and leftover RUNTIME/DEFPREFIX/staging dirs would otherwise corrupt
    //this build (or cause spurious "already mounted"/EEXIST failures).
    VfsMount::CleanStaleRuntime(ContainerParams.TempPath);

    //Verify every dependency is satisfiable (runner build cached, DEFPREFIX present, game layers local-or-fetchable),
    //then materialize any missing game-layer content from a backend (IPFS) to its expected local path. Both run
    //before InitializeDefPrefix/mount — the content must exist on disk by then. Abort if anything can't be provided.
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

    //Provision the Wine prefix (DEFPREFIX) — only when the runner asks for one. Left pristine; base edits
    //go to DEFAULTDATA below. One code path, no installed-vs-not branching for edits.
    if (PrefixGen)
    {
        if (ContainerParams.RunnerShipsBuild)
            //Installed runner: build is pre-provisioned. Mount it read-only at RunnerMountPath; DEFPREFIX is
            //the one-time read-only artifact (no wineboot). We only READ its hives when building DEFAULTDATA.
            { if (!VfsMount::MountRunnerBuild(this->ContainerParams)) return false; }
        else
            RegistryLayer::InitializeDefPrefix(this->ContainerParams);
    }

    //Materialise every package-encoded BASE edit into the DEFAULTDATA layer (between the component layers
    //and the WRITELAYER). FileEdits apply to any runner; base RegEdits are Wine-only (handled inside).
    RegistryLayer::BuildDefaultData(this->ContainerParams);

    //Seed any persisted KEEP hives into the ephemeral WRITELAYER so they shadow DEFPREFIX.
    //No-op when PersistAll (durable RW branch already holds the reg files).
    if (!ContainerParams.KeepRegHives.empty())
        RegistryLayer::SeedPersistRegistry(this->ContainerParams);

    //Seed any persisted KEEP files into the WRITELAYER so they shadow their lower layers.
    //No-op when PersistAll or when none are declared.
    if (!ContainerParams.KeepFiles.empty())
        PersistLayer::SeedPersistFiles(this->ContainerParams);

    //Verify every layer's locator resolves before mounting — a missing source (e.g. a moved local file,
    //or a remote SOURCE not yet fetched) is surfaced now rather than silently mounting nothing.
    //TODO(sharing): block / offer to fetch when a remote SOURCE is missing; check runner + cross-package deps.
    if (std::vector<std::string> Missing = LaunchSources::VerifyDependencies(this->ContainerParams); !Missing.empty())
        LogWarn("ContainerWrapper::BuildContainerRuntime",
                std::to_string(Missing.size()) + " layer source(s) missing — the runtime may be incomplete.");

    //Build the layer-spec (DEFPREFIX + VFS subcomponents target-rooted + PERSIST dirs as RW
    //passthrough) and mount it as a single vidyagodfs filesystem at RuntimePath.
    if (!VfsMount::MountVFS(this->ContainerParams)) return false;
    VfsMount::CheckCaseConflicts(ContainerParams.RuntimePath);

    //Post-mount: OVERRIDE edits operate on the mounted runtime (COW directly to WRITELAYER) — they win
    //unconditionally, over both the package content AND the user's persisted state. OVERRIDE FileEdits apply
    //to any runner; DLL overrides and OVERRIDE RegEdits are Wine-only. Base edits already live in DEFAULTDATA.
    FileEdits::ProcessFileEdits(this->ContainerParams, /*OverridePass=*/true);
    if (PrefixGen)
    {
        FileEdits::ProcessDLLOverrides(this->ContainerParams);
        RegistryLayer::ApplyOverrideRegEdits(this->ContainerParams);
    }

    //----- SANDBOX prep: record the mounted spec(s) so the sandbox can RE-mount them inside its own namespace, ON TOP
    //of the host mount (which stays put — the game sees the stacked in-namespace mount; the host mount serves any
    //unsandboxed path and cleanup). The OVERRIDE edits above already landed in the writelayer, which both mounts
    //share, so the in-sandbox mount is identical. Gated on the real probe so we only prepare this when the machine can
    //actually sandbox; tooling/authoring/override runs (which never sandbox) simply use the host mount as today. -----
    if (SandboxLayer::Requested(ContainerParams, SandboxDefaultOn(GlobalConfigJSON)) && SandboxLayer::Available())
    {
        auto Record = [&](const std::filesystem::path &SpecPath, const std::filesystem::path &Mnt){
            if (std::filesystem::exists(SpecPath))
                ContainerParams.SandboxMounts.emplace_back(SpecPath.string(), Mnt.string());
        };
        Record(ContainerParams.TempPath / "vidyagodfs.spec.json", ContainerParams.RuntimePath);
        if (ContainerParams.RunnerShipsBuild && !ContainerParams.UnifiedRuntime && !ContainerParams.RunnerLayers.empty())
            Record(ContainerParams.TempPath / "vidyagodfs.runner.spec.json", ContainerParams.RunnerMountPath);
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



//(RunCommand — the generic synchronous QProcess runner — moved to processenv.{h,cpp} as a free function.)

//=====================================================================================================================================================================
//                                                                    REGISTRYWRAPPER CLASS
//=====================================================================================================================================================================




//=====================================================================================================================================================================
//                                                                          VFSWRAPPER CLASS
//=====================================================================================================================================================================




//(LayerLocator / ResolveLayerSource moved to ManifestModel — see manifestmodel.h.)











//(PackageIpfsCids / PackageCoverCids moved to ManifestModel — see manifestmodel.h.)

//(ImportPackage / PublishPackage / MirrorDehydrated / IsPackageImported moved to PackageCatalog — see packagecatalog.h.)














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

    // WINEDLLOVERRIDES — only meaningful for runners that have a wine prefix.
    if (ContainerParams.PrefixGenerate && !ContainerParams.DLLOverrides.empty())
    {
        RunProcessEnvironment.insert("WINEDLLOVERRIDES", QString::fromStdString(std::accumulate(ContainerParams.DLLOverrides.begin(), ContainerParams.DLLOverrides.end(), std::string{}, [](const std::string &a, const std::string &b){ return a.empty() ? b : a + ";" + b; })));
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
        SandboxLayer::Options SbOpts = SandboxLayer::FromParams(ContainerParams, SandboxDefaultOn(GlobalConfigJSON));
        if (SbOpts.Enabled && !SandboxLayer::Available())
            LogOut("ContainerWrapper::Execute", "Sandbox unavailable on this machine (no bwrap / unprivileged user namespaces disabled) — launching normally.");
        else if (SbOpts.Enabled && ContainerParams.SandboxMounts.empty())
            LogWarn("ContainerWrapper::Execute", "Sandbox requested but no runtime mount was prepared — launching normally.");
        else if (SbOpts.Enabled)
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
                std::string OErr;
                if (IpfsWrapper::OverlayServe(Sock, &OErr))
                {
                    std::string Mask = "16";
                    if (auto S = Subnet.rfind('/'); S != std::string::npos) Mask = Subnet.substr(S + 1);
                    SbOpts.TunName = "vg-lan";
                    SbOpts.TunCidr = SelfVip + "/" + Mask;
                    SbOpts.TunSock = Sock;
                    OverlayServing = true;
                }
                else
                    LogWarn("ContainerWrapper::Execute", "overlay serve failed (launching without the virtual LAN): " + OErr);
            }

            SandboxLayer::Wrap(SbOpts, Program, Arguments);
            LogOut("ContainerWrapper::Execute", std::string("Sandbox: nested bubblewrap (net ")
                   + (SbOpts.Net == SandboxLayer::NetMode::Isolated ? "isolated" : "host") + ", "
                   + std::to_string(SbOpts.Mounts.size()) + " mount(s)" + (OverlayServing ? ", overlay" : "") + ")");
        }
    }

    QProcess RunProcess;
    RunProcess.setProgram(QString::fromStdString(Program));
    RunProcess.setWorkingDirectory(QString::fromStdString(FinalWorkDir));
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

    //1. Registry + file capture must read RUNTIME/<...> before anything is unmounted. No-op when PersistAll.
    if (!ContainerParams.KeepRegHives.empty() && !ContainerParams.PersistAll)
        RegistryLayer::CapturePersistRegistry(this->ContainerParams);
    if (!ContainerParams.KeepFiles.empty() && !ContainerParams.PersistAll)
        PersistLayer::CapturePersistFiles(this->ContainerParams);
    if (!ContainerParams.KeepRegKeys.empty() && !ContainerParams.PersistAll)
        RegistryLayer::CapturePersistRegKeys(this->ContainerParams);

    //2. Durable-backed mounts: non-lazy + verified, innermost-first (reverse registration order).
    bool DurableUnmountOk = true;
    for (auto It = ContainerParams.CleanupPersistPaths.rbegin(); It != ContainerParams.CleanupPersistPaths.rend(); ++It)
    {
        bool Unmounted = false;
        for (int Attempt = 0; Attempt < 5 && !Unmounted; ++Attempt)
        {
            if (Attempt > 0) QThread::msleep(200); //give a lingering wineserver time to release
            if (RunCommand("fusermount3", {"-u", *It}, SystemToolEnv()) == 0)
                Unmounted = true;
        }
        if (!Unmounted)
        {
            LogErr("ContainerWrapper::Cleanup", "DURABLE mount still busy after retries: " + It->string());
            //Lazy-detach so it eventually clears, but mark the wipe unsafe for this run.
            RunCommand("fusermount3", {"-uz", *It}, SystemToolEnv());
            DurableUnmountOk = false;
        }
    }

    //3. Ephemeral VFS mounts: lazy unmount is fine (their RW/source is all under TempPath).
    for (const std::filesystem::path &UnmountPath : ContainerParams.CleanupUnmountPaths)
        RunCommand("fusermount3", {"-uz", UnmountPath}, SystemToolEnv());

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


