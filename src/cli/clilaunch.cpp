#include "cli/climodes.h"
#include "shelf.h"
#include "main.h"
#include "apppaths.h"
#include "instancestore.h"   // TouchLastRun on a real CLI launch
#include "platform/platform.h"
#include "commonutils.h"
#include "manifestmodel.h"
#include "packagecatalog.h"
#include "containerwrapper.h"
#include "launchsession.h"
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
                             {"Executable", L.Executable}, {"WorkDir", L.WorkDir}, {"Native", L.NativeNamespace()}});
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
    J["WorkDir"]           = CP.WorkDirPathComplete.string();   // where it starts: the game's WORKDIR, its runner's, or its EXE's folder
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
                                                                std::string &Err, std::shared_ptr<const NodeIndex> Loaded)
{
    LaunchRequest LaunchParameters = Req;   // the node is filled in from the tile below
    auto Out = std::make_unique<PreparedLaunch>();
    {
        Out->Index = Loaded ? std::move(Loaded) : PackageCatalog::CurrentIndex(GlobalConfigJSON);   // the app's own while no node file changed
        const auto &Index = Out->Index;   // repos + locally-added packages
        //--tile [--variant]: the row the shelf shows, named the way the shelf names it.
        if (LaunchParameters.LaunchNodeId.empty())
        {
            std::string Why;
            LaunchParameters.LaunchNodeId = Shelf::RowUnderTile(*Index, LaunchParameters.LaunchTile, LaunchParameters.LaunchVariant, &Why);
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
        LaunchSession::Request R;
        R.Index = Index;
        R.NodeId = LookupCid;
        R.Entrypoint = LaunchParameters.Entrypoint;
        R.Face = LaunchParameters.LaunchTile;   // --tile: the row runs as that tile
        R.InstanceName = LaunchParameters.InstanceName;
        R.Grafts = LaunchParameters.Grafts;
        R.VariableOverrides = LaunchParameters.VariableOverrides;
        R.RunnerChain = LaunchParameters.RunnerChain;   // --runner, innermost first (empty: the instance's / default)
        R.ReadBack = !LaunchParameters.ResolveOnly;   // a launch reads back what the game changed in its files
        Out->S = std::make_unique<LaunchSession::Session>(GlobalConfigJSON, std::move(R));
        //A row that does not resolve (blocked, malformed, no runner) has nothing to dump or run.
        if (!Out->S->Resolve(&Err)) { LogErr("main.cpp", "'" + LaunchParameters.LaunchNodeId + "': " + Err + " — aborting."); return nullptr; }
        const ContainerParams &CP = Out->S->Wrapper()->ContainerParams;
        if (!LaunchParameters.InstanceName.empty())
        {
            const auto Have = InstanceStore::List(GlobalConfigJSON, CP.PackageUID);
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
        Out->Name = LaunchParameters.LaunchNodeId;
        return Out;
    }
}

nlohmann::ordered_json CliModes::ResolutionOf(PreparedLaunch &P) { return DumpResolution(P.S->Wrapper()->ContainerParams); }

int CliModes::Launch(const LaunchRequest &Req, nlohmann::ordered_json &GlobalConfigJSON, QDir &AppDataDir,
                     nlohmann::ordered_json *Resolution, Diagnostics::Report *VerdictOut)
{
    (void)AppDataDir;
    const std::string Name = Req.LaunchNodeId + Req.LaunchTile;
    std::string Err;
    std::unique_ptr<PreparedLaunch> P;
    {
        //A failed launch is exactly when the verdict matters most; returning early without one means the runs that
        //went wrong are the runs that say nothing. What the lookup and the resolve said is it (the session went with
        //the failure).
        //The catalog loads first, outside it: another package's malformed node is not this launch's verdict.
        std::shared_ptr<const NodeIndex> Index = PackageCatalog::CurrentIndex(GlobalConfigJSON);
        LogRecorder Said({});
        P = PrepareLaunch(Req, GlobalConfigJSON, Err, std::move(Index));
        if (!P)
        {
            const Diagnostics::Report R = Diagnostics::PrintVerdict((Req.ResolveOnly ? "Resolve of '" : "Launch of '") + Name + "'",
                                                                    Diagnostics::Summarize(Said.Take()));
            if (VerdictOut) *VerdictOut = R;
            return 1;
        }
    }
    LaunchSession::Session &S = *P->S;
    //Every exit reports — a resolve too: it is the cheap way to ask "is this package healthy?", so it is precisely
    //where a quiet resolve warning should be surfaced rather than scrolled past.
    const auto Done = [&](const std::string &What, int Rc) {
        S.Cleanup();
        const Diagnostics::Report R = S.Verdict(What + " of '" + Name + "'");
        if (VerdictOut) *VerdictOut = R;
        return Rc;
    };
    if (Resolution) *Resolution = ResolutionOf(*P);
    if (Req.ResolveOnly) return Done("Resolve", 0);
    if (!S.Build(&Err))
    {
        if (S.Stopped()) return Done("Launch", 1);   // told to stop first: not started, as a stop before Run (no error)
        LogErr("main.cpp", "'" + Name + "': " + Err + " — aborting launch.");
        return Done("Launch", 1);
    }
    const LaunchSession::Outcome O = S.Run();
    if (!O.Started) return Done("Launch", 1);   // refused before it started, or told to quit first
    return Done("Launch", O.Crashed ? 1 : O.ExitCode);
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
