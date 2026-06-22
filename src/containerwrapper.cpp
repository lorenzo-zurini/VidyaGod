#include "containerwrapper.h"
#include "commonutils.h"
#include "processenv.h"      // RunCommand / SystemToolEnv (fusermount on cleanup + the game launch)
#include <QThread>
#include <QMetaObject>

//The launch-build subsystems were lifted out into their own units (LaunchResolver / VarSubst / FileEdits /
//RegistryLayer / PersistLayer / VfsMount / LaunchSources / RunnerInstall — see their headers, all pulled in via
//containerwrapper.h). What remains here is the thin session lifecycle that orchestrates them. The only pure helper
//still referenced unqualified is IsVfsLayer, so bring ManifestModel in.
using namespace ManifestModel;

//Stores the JSON references and immediately runs the full initialization pipeline.
//After construction, ContainerParams is fully populated and ready for BuildContainerRuntime().
ContainerWrapper::ContainerWrapper(nlohmann::ordered_json &Passed_GlobalConfigJSON, nlohmann::ordered_json &Passed_MANIFESTJSON, struct ContainerParams &Passed_ContainerParams)
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
            { if (!VfsMount::MountRunnerBuild(this->ContainerParams)) return false; }
        else
            RegistryLayer::InitializeDefPrefix(this->ContainerParams);
    }

    //Materialise every package-encoded BASE edit into the DEFAULTDATA layer (between the component layers
    //and the WRITELAYER). FileEdits apply to any runner; base RegEdits are Wine-only (handled inside).
    RegistryLayer::BuildDefaultData(this->ContainerParams);

    //Seed any persisted registry into the ephemeral WRITELAYER so it shadows DEFPREFIX.
    //No-op under PersistAll (durable RW branch already holds the reg files).
    if (ContainerParams.PersistRegistry)
        RegistryLayer::SeedPersistRegistry(this->ContainerParams);

    //Seed any persisted PersistFiles into the WRITELAYER so they shadow their lower layers.
    //No-op under PersistAll or when none are declared.
    if (!ContainerParams.PersistFiles.empty())
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
    //Substitute variables in the runner executable path — manifest runners reference
    //%ProgramPath% to point to a bundled binary mounted into RUNTIME via a VFS layer.
    VarSubst::StringVariableSubstitution(ContainerParams.RunnerExecutable, ContainerParams.GetVariablesMap());
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
        VarSubst::StringVariableSubstitution(ExpandedValue, ContainerParams.GetVariablesMap());
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
        VarSubst::StringVariableSubstitution(Arg, ContainerParams.GetVariablesMap());
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
        RegistryLayer::CapturePersistRegistry(this->ContainerParams);
    if (!ContainerParams.PersistFiles.empty() && !ContainerParams.PersistAll)
        PersistLayer::CapturePersistFiles(this->ContainerParams);
    if (!ContainerParams.PersistRegKeys.empty() && !ContainerParams.PersistAll)
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


