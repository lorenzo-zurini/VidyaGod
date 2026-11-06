#ifndef CONTAINERWRAPPER_H
#define CONTAINERWRAPPER_H

#include <nlohmann/json.hpp>
#include <filesystem>
#include <optional>
#include <string>

#include <QProcess>
#include <QMutex>
#include <QMutexLocker>

#include "manifestmodel.h"   // NodeIndex (ContainerParams.NodeIdx) + pure manifest queries
#include "launchparams.h"    // ContainerParams — the shared resolved-session struct (moved out of this class)
#include "varsubst.h"        // VarSubst — the %token% / custom-var encoding engine (moved out of this class)
#include "launchresolver.h"  // LaunchResolver — param/recipe/runner/exec/persistence resolution (moved out)
#include "fileedits.h"       // FileEdits — FileEdit/DllOverride application (moved out of this class)
#include "binarypatch.h"     // BinaryPatch — the executable-patch edit layer (Replace/Cave/Poke)
#include "registrylayer.h"   // RegistryLayer — Wine prefix/registry/DEFAULTDATA + reg-persistence (moved out)
#include "persistlayer.h"    // PersistLayer — PersistFile seed/capture (moved out of this class)
#include "vfsmount.h"        // VfsMount — vidyagodfs layer-spec build + mount/unmount (moved out)
#include "launchsources.h"   // LaunchSources — dependency pre-flight + materialize (moved out)

//Thin session orchestrator for a single game launch. It owns the live config/manifest/ContainerParams and drives
//the lifecycle, delegating every step to the extracted launch-engine units:
//  construction → InitializeContainer  → LaunchResolver::InitializeFromNode (resolve the whole container)
//  BuildContainerRuntime()             → LaunchSources (sources) · RegistryLayer (prefix/DEFAULTDATA) · PersistLayer
//                                         · VfsMount (mount) · FileEdits (edits)
//  Execute()                           → launches the runner with the resolved args/env (%tokens% via VarSubst)
//  Cleanup()                           → RegistryLayer/PersistLayer capture, then unmount + wipe RUNTIME/TEMP
//(Runner install is RunnerInstall; the pure manifest/catalog services are ManifestModel/PackageCatalog.)
class ContainerWrapper
{
public:
    //GlobalConfigJSON is read-only here — the wrapper copies it into its own member, never writing back to the
    //caller's config — so it's taken by const ref (lets read-only callers like the package editor pass a const config).
    ContainerWrapper(const nlohmann::ordered_json &Passed_GlobalConfigJSON, ContainerParams &Passed_ContainerParams);
    ~ContainerWrapper();
    ContainerWrapper(const ContainerWrapper &) = delete;
    ContainerWrapper &operator=(const ContainerWrapper &) = delete;
    struct ContainerParams ContainerParams;

    //The app is told to quit (QuitSignals): every session of this process is ended the way a user's Stop ends it — the
    //game is killed, and the launch carries on into Cleanup, which stores what the game kept and removes the runtime. A
    //launch still building stops at its next step instead of starting the game. For the rest of the process no game starts.
    static void StopAll();
    static bool Stopping();
    //Sessions of this process: a wrapper from claiming its session folder (BuildContainerRuntime) to the end of its
    //Cleanup — a resolve that builds nothing (a refused launch) is none, and neither is one marked NotAGame. WaitNoneLive
    //blocks until there are none.
    static size_t LiveCount();
    static void WaitNoneLive();
    //The package editor's authoring session: a runtime kept mounted while its window is open, which a quit does not wait
    //for (it would wait for the user to close the window). Its folder is cleared by the next start's sweep.
    void SetNotAGame() { NotAGame = true; }

    //Whether the constructor's resolve succeeded — false for a blocked, malformed or runnerless row. Nothing is
    //built, dumped or run from a wrapper that did not resolve.
    bool Resolved() const { return InitOk; }

    //Mounts VFS layers, applies registry patches and DLL overrides, checks for case conflicts.
    //Must be called before Execute(). Returns false if VFS setup fails.
    bool BuildContainerRuntime();

    //Launches the game via the resolved runner. If OverrideExe is non-empty it is used
    //instead of the manifest's EXEPATH/ROM, followed by OverrideArgs. Blocks until the process exits.
    bool Execute(const std::string &OverrideExe = "", const std::vector<std::string> &OverrideArgs = {});

    //Sends SIGKILL to the game process started by Execute() if it is still running.
    //Safe to call from any thread; no-op if no process is active.
    void KillGame();

    //Unmounts all paths in CleanupUnmountPaths (fusermount -uz) and removes RUNTIME and TEMP.
    bool Cleanup();

private:
    //Resolves the whole launch from the node graph (LaunchResolver::InitializeFromNode). Called from the constructor.
    bool InitializeContainer();

    //Result of the ctor's InitializeContainer() run. A failed resolve used to leave a half-populated
    //wrapper that BuildContainerRuntime would happily build on; now the first build step checks this.
    bool InitOk = false;

    nlohmann::ordered_json GlobalConfigJSON;

    //Held from BuildContainerRuntime to the end of Cleanup's unmounts (or the wrapper's end): a game is live.
    std::optional<VfsMount::RuntimeHold> Live;

    //The session folder's ownership lock (VfsMount::ClaimSession), held from the build to the end of Cleanup; -1 = none.
    int SessionLock = -1;
    //This wrapper claimed its session folder: only then is there anything of its own for Cleanup to store or remove.
    //A refused launch (the instance already running) must never tear down the folder another launch is using.
    bool Claimed = false;
    bool NotAGame = false;
    void LeaveLive();   // out of the live set (Cleanup's end, the destructor)

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
