#include "cli/climodes.h"
#include "main.h"
#include "apppaths.h"
#include "platform/platform.h"
#include "commonutils.h"
#include "manifestmodel.h"
#include "packagecatalog.h"
#include "containerwrapper.h"
#include "ipfswrapper.h"
#include "jsonoperations.h"

#include <QDir>
#include <QFile>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;

static nlohmann::ordered_json DumpResolution(const struct ContainerParams &CP)
{
    nlohmann::ordered_json J;
    J["PackageUID"]        = CP.PackageUID;
    J["GameName"]          = CP.GameName;
    J["Platform"]          = CP.Platform;
    J["Recipe"]            = CP.Recipe;
    J["RunnerName"]        = CP.RunnerName;
    //Runner daisy-chain (innermost→outermost): each link's node id, host, and exec — the shortest route resolved to
    //run this content on the machine, always ending in the native terminal.
    {
        nlohmann::ordered_json Chain = nlohmann::ordered_json::array();
        for (const RunnerLink &L : CP.RunnerChain)
            Chain.push_back({{"NodeId", L.NodeId}, {"Host", L.HostPlatform}, {"Guests", L.GuestPlatform},
                             {"Executable", L.Executable}, {"Native", L.NativeNamespace()}});
        J["RunnerChain"] = Chain;
    }
    J["RunnerExecutable"]  = CP.RunnerExecutable;
    J["RunnerArgs"]        = CP.RunnerArgs;
    J["RunnerEnv"]         = CP.RunnerEnv;
    J["RunnerRemoveEnv"]   = CP.RunnerRemoveEnv;
    J["ContentRoot"]       = CP.ContentRoot;
    J["PrefixRoot"]        = CP.PrefixRoot;
    J["PrefixGenerate"]    = CP.PrefixGenerate;
    J["ContentPath"]       = CP.ExePathRelative.string();
    J["Content"]           = CP.ExePathComplete.string();
    J["ExeArgs"]           = CP.ExeArgs;
    J["DLLOverrides"]      = CP.DLLOverrides;
    J["PersistAll"]        = CP.PersistAll;
    J["KeepDirs"]          = CP.KeepDirs;
    J["KeepFiles"]         = CP.KeepFiles;
    J["KeepRegHives"]      = CP.KeepRegHives;
    J["KeepRegKeys"]       = CP.KeepRegKeys;
    J["DropPaths"]         = CP.DropPaths;
    J["CustomVariables"]   = CP.CustomVariables;
    J["RunnerShipsBuild"]  = CP.RunnerShipsBuild;
    J["UnifiedRuntime"]    = CP.UnifiedRuntime;
    J["SubComponentsArray"]= CP.SubComponentsArray;
    return J;
}

#include "vfsmount.h"

int CliModes::RunNodeLaunch(LaunchParameters &LaunchParameters, nlohmann::ordered_json &GlobalConfigJSON, QDir &AppDataDir)
{
    (void)GlobalConfigJSON; (void)AppDataDir;
    if (!LaunchParameters.LaunchNodeId.empty())
    {
        LogOut("main.cpp", "Launching node '" + LaunchParameters.LaunchNodeId + "' from the global node graph.");
        NodeIndex Index = PackageCatalog::BuildCatalogIndex(GlobalConfigJSON);   // repos + locally-added packages
        if (!Index.Find(LaunchParameters.LaunchNodeId))
        { LogErr("main.cpp", "Node '" + LaunchParameters.LaunchNodeId + "' not found in the catalog, aborting."); return 1; }

        nlohmann::ordered_json MANIFESTJSON = nlohmann::ordered_json::object();   // engine fills this from the node graph
        struct ContainerParams NewContainerParams = ContainerParams(std::filesystem::path(), LaunchParameters.LaunchNodeId, std::string());
        NewContainerParams.NodeIdx           = &Index;
        NewContainerParams.LaunchNodeId      = LaunchParameters.LaunchNodeId;
        NewContainerParams.VariableOverrides = LaunchParameters.VariableOverrides;
        NewContainerParams.ModuleStates      = LaunchParameters.ModuleStates;
        NewContainerParams.VariantID         = "default";
        NewContainerParams.RunnerID          = LaunchParameters.RunnerID;
        //Runner daisy-chain: the explicit --runner chain wins; else the single RunnerID as a 1-step pin (else auto).
        if (!LaunchParameters.RunnerChain.empty())   NewContainerParams.RunnerChainIds = LaunchParameters.RunnerChain;
        else if (!LaunchParameters.RunnerID.empty()) NewContainerParams.RunnerChainIds = { LaunchParameters.RunnerID };
        class ContainerWrapper NewContainerWrapper = ContainerWrapper(GlobalConfigJSON, MANIFESTJSON, NewContainerParams);
        if (!LaunchResolver::ResolveExecutableDefinition(MANIFESTJSON, NewContainerWrapper.ContainerParams))
        { LogErr("main.cpp", "ResolveExecutableDefinition failed, aborting."); return 1; }
        if (LaunchParameters.ResolveOnly)
        {
            //Dump the resolved params (no mount, no game) for golden-compare / hang-free verification.
            //Under the app data dir so ALL VidyaGod-produced data lives in ~/.VidyaGod (a single delete = clean slate).
            const std::filesystem::path Out = std::filesystem::path(AppDataDir.absolutePath().toStdString())
                                              / ("vg_resolve_" + LaunchParameters.LaunchNodeId + ".json");
            std::ofstream OF(Out);
            OF << DumpResolution(NewContainerWrapper.ContainerParams).dump(2) << std::endl;
            LogSucc("main.cpp", "Resolved '" + LaunchParameters.LaunchNodeId + "' -> " + Out.string());
            return 0;
        }
        //Persist any secret seeded from its POOL this launch, so the value is stable from here on (the GUI does the
        //same through the prelaunch picker). Without this a CLI launch would redraw a different key every time and
        //whatever the game wrote into its prefix from the previous one would no longer match.
        if (!NewContainerWrapper.ContainerParams.PickedSecrets.empty()
            && !NewContainerWrapper.ContainerParams.PackageUID.empty())
        {
            const std::string &Uid = NewContainerWrapper.ContainerParams.PackageUID;
            PackageCatalog::MergePackageVariables(GlobalConfigJSON, Uid, NewContainerWrapper.ContainerParams.PickedSecrets);
            QFile CfgFile(AppDataDir.filePath("GlobalConfig.JSON"));
            if (JSONOps::SaveJSON(&GlobalConfigJSON, &CfgFile))
                LogOut("main.cpp", "Persisted " + std::to_string(NewContainerWrapper.ContainerParams.PickedSecrets.size())
                                   + " pool-seeded secret(s) for package " + Uid + ".");
        }

        //Bail if the runtime couldn't be built (no compatible runner, unmountable/compressed layers, missing
        //dependencies, …). Without this the engine would proceed to Execute() an empty/garbage command and report
        //a misleading clean exit (code 0) for a launch that never actually ran.
        if (!NewContainerWrapper.BuildContainerRuntime())
        { LogErr("main.cpp", "Failed to build the container runtime for '" + LaunchParameters.LaunchNodeId
                 + "' — aborting launch (check the log above)."); NewContainerWrapper.Cleanup(); return 1; }
        NewContainerWrapper.Execute();
        if (NewContainerWrapper.LastCrashed || NewContainerWrapper.LastExitCode != 0)
            LogWarn("main.cpp", "Game did not exit cleanly (code " + std::to_string(NewContainerWrapper.LastExitCode) + ").");
        return NewContainerWrapper.LastCrashed ? 1 : NewContainerWrapper.LastExitCode;
    }

    return -1;   // no mode of this family requested
}
