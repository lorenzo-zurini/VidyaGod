#include "cli/climodes.h"
#include "main.h"
#include "apppaths.h"
#include "instancestore.h"   // TouchLastRun on a real CLI launch
#include "platform/platform.h"
#include "commonutils.h"
#include "manifestmodel.h"
#include "packagecatalog.h"
#include "containerwrapper.h"
#include "ipfswrapper.h"
#include "jsonoperations.h"
#include "depcheck.h"       // MissingCore — a launch that never started names what the machine lacks
#include "sandboxlayer.h"

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

static nlohmann::ordered_json DumpResolution(struct ContainerParams &CP)   // non-const: GetVariablesMap() is not const
{
    nlohmann::ordered_json J;
    J["PackageUID"]        = CP.PackageUID;
    J["GameName"]          = CP.GameName;
    J["Platform"]          = CP.Platform;
    J["Stack"]             = nlohmann::ordered_json::array();   // what the launch stacks, lowest first
    for (const OpGroup &G : CP.Stack) J["Stack"].push_back(G.Name);
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
    J["KeepDirs"]          = CP.KeepDirs;    // [{path,target,cloud}] — file dir persists ("" path = whole runtime)
    J["KeepFiles"]         = CP.KeepFiles;   // [{path,target,cloud}] — file single-file persists
    J["KeepRegKeys"]       = CP.KeepRegKeys;
    J["CustomVariables"]   = CP.CustomVariables;
    J["RunnerShipsBuild"]  = CP.RunnerShipsBuild;
    J["UnifiedRuntime"]    = CP.UnifiedRuntime;
    J["SubComponentsArray"]= CP.SubComponentsArray;
    //The rest of what a launch is made of: the game's folded environment, the launched entry, and the runner build
    //(the boundary runner's closure, lowered) — so a dump is the complete resolved launch, not just the game side.
    J["LaunchEnv"]         = CP.LaunchEnv;
    J["LaunchRemoveEnv"]   = CP.LaunchRemoveEnv;
    J["ComposedExec"]      = CP.ComposedExec;
    J["RunnerOps"]         = CP.RunnerOps;
    J["RunnerLayers"]      = CP.RunnerLayers;
    J["RunnerPersistLayers"]= CP.RunnerPersistLayers;
    J["Variables"]         = CP.GetVariablesMap();   // every %KEY% the resolve substituted with (built-ins included)
    J["GuestRoots"]        = CP.GuestRoots;          // each anchor, in the boundary runner's guest
    J["Drives"]            = CP.Drives;              // each guest drive, in its layout
    return J;
}

#include "vfsmount.h"

CliModes::PreparedLaunch::~PreparedLaunch() = default;

std::unique_ptr<CliModes::PreparedLaunch> CliModes::PrepareLaunch(const LaunchRequest &Req, nlohmann::ordered_json &GlobalConfigJSON,
                                                                std::string &Err)
{
    LaunchRequest LaunchParameters = Req;   // the node is filled in from the tile below
    auto Out = std::make_unique<PreparedLaunch>();
    {
        Out->Index = PackageCatalog::CurrentIndex(GlobalConfigJSON);   // the app's own while no node file changed
        const auto &Index = Out->Index;   // repos + locally-added packages
        //--tile [--variant]: the row the shelf shows, named the way the shelf names it.
        if (LaunchParameters.LaunchNodeId.empty())
        {
            std::string Why;
            LaunchParameters.LaunchNodeId = PackageCatalog::RowUnderTile(*Index, LaunchParameters.LaunchTile, LaunchParameters.LaunchVariant, &Why);
            if (LaunchParameters.LaunchNodeId.empty()) { Err = Why; LogErr("main.cpp", Why + " — aborting."); return nullptr; }
        }
        else if (!LaunchParameters.LaunchVariant.empty())
        { Err = "--variant picks a row under --tile; with --node the node is the row"; LogErr("main.cpp", Err + " — aborting."); return nullptr; }
        LogOut("main.cpp", "Launching node '" + LaunchParameters.LaunchNodeId + "' from the global node graph.");
        // Identity is the CID, so Find() keys on it. As a CLI CONVENIENCE, accept a cosmetic LABEL too: if the arg is
        // not a CID key, resolve it to the CID of the (first) node whose LABEL matches. Lets a person type
        // `--node "Age of Empires II"` instead of a hash. Ambiguous only across unrelated games (cosmetic labels may
        // repeat); the first match wins — a person who needs a specific variant passes its CID. The ARG is kept for
        // the log line and the resolve-dump filename; the resolved CID is what the engine looks the node up by.
        //By CID, else by working-tree handle (the stored "CID" of the last publish — stable while a node is being
        //edited, so a before/after comparison can name the same node), else by LABEL.
        std::string LookupCid = LaunchParameters.LaunchNodeId;
        if (!Index->Find(LookupCid))
        {
            LookupCid.clear();
            for (const auto &[Cid, N] : Index->Nodes)
                if (N.Handle == LaunchParameters.LaunchNodeId) { LookupCid = Cid; break; }
            if (LookupCid.empty())
                for (const auto &[Cid, N] : Index->Nodes)
                    if (N.NodeId == LaunchParameters.LaunchNodeId) { LookupCid = Cid; break; }
            if (LookupCid.empty())
            { Err = "node '" + LaunchParameters.LaunchNodeId + "' not found in the catalog (by CID, handle or name)"; LogErr("main.cpp", Err + ", aborting."); return nullptr; }
        }

        struct ContainerParams NewContainerParams;
        NewContainerParams.NodeIdx           = Index.get();
        NewContainerParams.NodeIdxOwned      = Index;         // the wrapper's copy co-owns the index
        NewContainerParams.LaunchNodeId      = LookupCid;
        NewContainerParams.VariableOverrides = LaunchParameters.VariableOverrides;
        NewContainerParams.Grafts            = LaunchParameters.Grafts;
        NewContainerParams.Entrypoint        = LaunchParameters.Entrypoint;
        NewContainerParams.InstanceName      = LaunchParameters.InstanceName;   // "" ⇒ the active (last played) one
        NewContainerParams.ReadBackOptions   = !LaunchParameters.ResolveOnly;  // what the game changed in its files is read back
        NewContainerParams.LaunchFace        = LaunchParameters.LaunchTile;   // --tile: the row runs as that tile
        //Same engine-injected session facts as the GUI path (see LaunchThread::run): the virtual-LAN vIPs + the
        //player's own display name, so `--node` behaves identically to a GUI launch.
        NewContainerParams.SessionVars = IpfsWrapper::LanLaunchVars();
        if (NewContainerParams.SessionVars["VIDYAGOD_SELF_NAME"].empty())
        {
            const std::string Nick = IpfsWrapper::GetProfile().Nick;
            NewContainerParams.SessionVars["VIDYAGOD_SELF_NAME"] = Nick.empty() ? std::string("Player") : Nick;
        }
        NewContainerParams.RunnerID          = LaunchParameters.RunnerID;
        //Runner daisy-chain: the explicit --runner chain wins; else the single RunnerID as a 1-step pin (else auto).
        if (!LaunchParameters.RunnerChain.empty())   NewContainerParams.RunnerChainIds = LaunchParameters.RunnerChain;
        else if (!LaunchParameters.RunnerID.empty()) NewContainerParams.RunnerChainIds = { LaunchParameters.RunnerID };
        Out->W = std::make_unique<ContainerWrapper>(GlobalConfigJSON, NewContainerParams);
        ContainerWrapper &NewContainerWrapper = *Out->W;
        //A row that does not resolve (blocked, malformed, no runner) has nothing to dump or run.
        if (!NewContainerWrapper.Resolved())
        { Err = "resolving '" + LaunchParameters.LaunchNodeId + "' failed (see the log)"; LogErr("main.cpp", Err + " — aborting."); return nullptr; }
        //--instance names an EXISTING instance. A typo would otherwise run a fresh, empty one (no settings, no saves)
        //and create it on the first save; instances are made in the launch window's menu. A game never played has
        //none yet, so its DefaultInstance is accepted (it is what a launch without the flag would use).
        if (!LaunchParameters.InstanceName.empty() && !AppPaths::UserDataPathOverride().empty())
        {
            Err = "--instance can't be combined with --userdata-dir (or an in-package run): that folder IS the "
                  "game's userdata, so there are no instances to pick from";
            LogErr("main.cpp", Err);
            return nullptr;
        }
        if (!LaunchParameters.InstanceName.empty())
        {
            const auto Have = InstanceStore::List(GlobalConfigJSON, NewContainerWrapper.ContainerParams.PackageUID);
            const bool Known = std::find(Have.begin(), Have.end(), LaunchParameters.InstanceName) != Have.end()
                            || (Have.empty() && LaunchParameters.InstanceName == InstanceStore::DefaultInstance);
            if (!Known)
            {
                std::string List;
                for (const std::string &I : Have) List += (List.empty() ? "" : ", ") + I;
                Err = "no instance '" + LaunchParameters.InstanceName + "' for this game. "
                      + (Have.empty() ? std::string("It has none yet; its first launch creates ") + InstanceStore::DefaultInstance + "."
                                      : "Its instances: " + List + ".");
                LogErr("main.cpp", Err);
                return nullptr;
            }
        }
        if (!LaunchResolver::ResolveExecutableDefinition(NewContainerWrapper.ContainerParams))
        { Err = "its entry did not resolve (see the log)"; LogErr("main.cpp", "ResolveExecutableDefinition failed, aborting."); return nullptr; }
        Out->Name = LaunchParameters.LaunchNodeId;
        return Out;
    }
}

nlohmann::ordered_json CliModes::ResolutionOf(PreparedLaunch &P) { return DumpResolution(P.W->ContainerParams); }

int CliModes::Launch(const LaunchRequest &Req, nlohmann::ordered_json &GlobalConfigJSON, QDir &AppDataDir,
                     nlohmann::ordered_json *Resolution, Diagnostics::Report *VerdictOut)
{
    (void)AppDataDir;
    const LaunchRequest &LaunchParameters = Req;
    Diagnostics::Begin();   // count every WARN/ERR this launch produces; verdict printed at the end
    const auto Verdict = [VerdictOut](const std::string &What) {   // printed, and handed to the caller that asked
        const Diagnostics::Report R = Diagnostics::ReportVerdict(What);
        if (VerdictOut) *VerdictOut = R;
    };
    std::string Err;
    const std::unique_ptr<PreparedLaunch> P = PrepareLaunch(Req, GlobalConfigJSON, Err);
    //A failed launch is exactly when the verdict matters most; returning early without one means the runs that went
    //wrong are the runs that say nothing.
    if (!P) { Verdict("Launch of '" + Req.LaunchNodeId + Req.LaunchTile + "'"); return 1; }
    ContainerWrapper &NewContainerWrapper = *P->W;
    {
        if (Resolution) *Resolution = ResolutionOf(*P);
        if (LaunchParameters.ResolveOnly)
        {
            //Every exit reports. A resolve is the cheap way to ask "is this package healthy?", so it is precisely
            //where a quiet resolve warning should be surfaced rather than scrolled past.
            Verdict("Resolve of '" + LaunchParameters.LaunchNodeId + "'");
            return 0;
        }
        //Persist any secret seeded from its POOL this launch, so the value is stable from here on (the GUI does the
        //same through the prelaunch picker). Without this a CLI launch would redraw a different key every time and
        //whatever the game wrote into its prefix from the previous one would no longer match.
        if (!NewContainerWrapper.ContainerParams.PickedSecrets.empty()
            && !NewContainerWrapper.ContainerParams.PackageUID.empty())
        {
            const std::string &Uid = NewContainerWrapper.ContainerParams.PackageUID;
            // MergePackageVariables now persists to the launched INSTANCE's instance.json (not GlobalConfig) — no
            // GlobalConfig SaveJSON needed (that was a dead write, and its success gated a lying "persisted" log).
            PackageCatalog::MergePackageVariables(GlobalConfigJSON, Uid, NewContainerWrapper.ContainerParams.PickedSecrets,
                                                  NewContainerWrapper.ContainerParams.InstanceName);
            LogOut("main.cpp", "Persisted " + std::to_string(NewContainerWrapper.ContainerParams.PickedSecrets.size())
                               + " pool-seeded secret(s) for package " + Uid + ".");
        }

        //Bail if the runtime couldn't be built (no compatible runner, unmountable/compressed layers, missing
        //dependencies, …). Without this the engine would proceed to Execute() an empty/garbage command and report
        //a misleading clean exit (code 0) for a launch that never actually ran.
        if (!NewContainerWrapper.BuildContainerRuntime())
        { LogErr("main.cpp", "Failed to build the container runtime for '" + LaunchParameters.LaunchNodeId
                 + "' — aborting launch (check the log above)."); NewContainerWrapper.Cleanup();
          Verdict("Launch of '" + LaunchParameters.LaunchNodeId + "'"); return 1; }
        // Real launch (runtime built) → mark the instance last-played. Skipped in the --userdata-dir/in-package
        // override mode (instances are bypassed there — don't litter <root>/USERDATA with a DefaultInstance).
        if (AppPaths::UserDataPathOverride().empty() && !NewContainerWrapper.ContainerParams.PackageUID.empty())
            InstanceStore::TouchLastRun(GlobalConfigJSON, NewContainerWrapper.ContainerParams.PackageUID,
                                        NewContainerWrapper.ContainerParams.InstanceName);
        //False without a crash: the game was never started (the mandatory sandbox is unavailable). That is a failed
        //launch; it used to exit 0, and the launch matrix read "ran, printed nothing" on a machine without bwrap.
        const bool Started = NewContainerWrapper.Execute() || NewContainerWrapper.LastCrashed;
        if (!Started && NewContainerWrapper.UserKilled)                 // told to quit before the game started
        {
            NewContainerWrapper.Cleanup();
            Verdict("Launch of '" + LaunchParameters.LaunchNodeId + "'");
            return 1;
        }
        if (!Started)
        {
            LogErr("main.cpp", "The game was not started (see the log above).");
            for (const std::string &M : DepCheck::MissingCore(DepCheck::ProbeSystem(SandboxLayer::Available())))
                LogErr("main.cpp", "  missing: " + M + " (Settings → Dependencies shows what to install)");
            //The runtime WAS built (mounted): take it down, as the GUI's LaunchThread does on every path. Returning
            //without this left the game's vidyagodfs mount up after a refused launch.
            NewContainerWrapper.Cleanup();
            Verdict("Launch of '" + LaunchParameters.LaunchNodeId + "'");
            return 1;
        }
        if (!NewContainerWrapper.UserKilled && !ContainerWrapper::Stopping() && NewContainerWrapper.LastCrashed)   // a non-zero exit is Execute's warning; a stop is no crash
            LogWarn("main.cpp", "Game did not exit cleanly (code " + std::to_string(NewContainerWrapper.LastExitCode) + ").");
        //After the run, as after a refused one: what the package KEEPs is captured, then the runtime comes down.
        NewContainerWrapper.Cleanup();
        //Same verdict the GUI prints. The CLI is what tooling and debugging actually use, so leaving it untallied
        //would have meant the one path we test with was the one path that could still hide a quiet error.
        Verdict("Launch of '" + LaunchParameters.LaunchNodeId + "'");
        return NewContainerWrapper.LastCrashed ? 1 : NewContainerWrapper.LastExitCode;
    }
}

int CliModes::RunNodeLaunch(LaunchParameters &LP, nlohmann::ordered_json &GlobalConfigJSON, QDir &AppDataDir)
{
    if (LP.LaunchNodeId.empty() && LP.LaunchTile.empty()) return -1;   // no mode of this family requested
    LaunchRequest R;
    R.LaunchNodeId = LP.LaunchNodeId;
    R.LaunchTile = LP.LaunchTile;
    R.LaunchVariant = LP.LaunchVariant;
    R.Entrypoint = LP.Entrypoint;
    R.InstanceName = LP.InstanceName;
    R.RunnerID = LP.RunnerID;
    R.RunnerChain = LP.RunnerChain;
    R.Grafts = LP.Grafts;
    R.VariableOverrides = LP.VariableOverrides;
    R.ResolveOnly = LP.ResolveOnly;
    nlohmann::ordered_json Dump;
    const int Rc = Launch(R, GlobalConfigJSON, AppDataDir, &Dump);
    if (LP.ResolveOnly && Rc == 0)
    {
        //Dump the resolved params (no mount, no game) for golden-compare / hang-free verification. Under the app data
        //dir so ALL VidyaGod-produced data lives in ~/.VidyaGod (a single delete = clean slate).
        const fs::path Out = fs::path(AppDataDir.absolutePath().toStdString()) / ("vg_resolve_" + LP.LaunchNodeId + ".json");
        std::ofstream OF(Out);
        OF << Dump.dump(2) << std::endl;
        LogSucc("main.cpp", "Resolved '" + LP.LaunchNodeId + "' -> " + Out.string());
    }
    return Rc;
}
