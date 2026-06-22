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
#include "varsubst.h"        // VarSubst — the %token% / custom-var encoding engine (moved out of this class)
#include "launchresolver.h"  // LaunchResolver — param/recipe/runner/exec/persistence resolution (moved out)
#include "fileedits.h"       // FileEdits — FileEdit/DllOverride application (moved out of this class)
#include "registrylayer.h"   // RegistryLayer — Wine prefix/registry/DEFAULTDATA + reg-persistence (moved out)
#include "persistlayer.h"    // PersistLayer — PersistFile seed/capture (moved out of this class)
#include "vfsmount.h"        // VfsMount — vidyagodfs layer-spec build + mount/unmount (moved out)
#include "launchsources.h"   // LaunchSources — dependency pre-flight + materialize (moved out)
#include "runnerinstall.h"   // RunnerInstall — runner build install + DEFPREFIX generation (moved out)

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

    //(Per-package user settings + game/component/variant/module lookups moved out: PackageCatalog::Get/Set
    //PackageUserSetting and ManifestModel::Find*/variants/modules respectively — see those headers.)

    //(Container parameter/recipe/runner/exec/persistence resolution moved to LaunchResolver — see
    //launchresolver.h. The %token% engine moved to VarSubst. The vidyagodfs mount subsystem (BuildLayerSpec/
    //MountVFS/SpawnVidyagodfs/MountRunnerBuild/CheckCaseConflicts/MountpointsUnder/CleanStaleRuntime) moved to
    //VfsMount; dependency pre-flight + materialize (VerifyDependencies/EnsureSources/MaterializeLayers) to
    //LaunchSources. Catalog/Sync/Publish to PackageCatalog. ImportRunner stays here — it needs the mount +
    //wineboot machinery.)

private:
    //(Runner install — ImportRunner/ImportRunnerNode/RunnerNodeImported — moved to RunnerInstall; see runnerinstall.h.)
    //(Wine prefix/registry/DEFAULTDATA + registry-persistence moved to RegistryLayer — see registrylayer.h:
    //InitializeDefPrefix/BuildDefaultData/ApplyOverrideRegEdits/Seed+CapturePersistRegistry/CapturePersistRegKeys.
    //File-persistence (Seed/CapturePersistFiles) moved to PersistLayer; FileEdit/DllOverride to FileEdits.)

    //(RunCommand moved to processenv.{h,cpp} (free function); TranslateCustomVarValue / StringVariableSubstitution
    //— the pure %token% engine — moved to VarSubst; see varsubst.h.)

private:
    //Runs DecideComponent → DeriveContainerParams → CreateRecipe → BuildSubComponentsArray
    //in the required order. Called from the constructor.
    bool InitializeContainer();
    bool BuildVirtualFilesystem(); //STUB — not yet implemented

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
