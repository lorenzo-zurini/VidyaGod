#include "launchsession.h"
#include "apppaths.h"
#include "commonutils.h"     // QuietLog, Log*
#include "containerwrapper.h"
#include "depcheck.h"        // MissingCore — what keeps a game from running here
#include "instancestore.h"   // the instance's config the values read back go to
#include "ipfswrapper.h"     // LanLaunchVars / GetProfile — the session facts a launch is given
#include "launchresolver.h"
#include "optionreadback.h"
#include "sandboxlayer.h"
#include "sessions.h"        // SessionOwned — no read-back while the instance runs

#include <set>

bool LaunchSession::ResolveTakingBack(ContainerParams &CP, const nlohmann::ordered_json &Config, bool Reresolve)
{
    if (!AppPaths::UserDataPathOverride().empty()) return LaunchResolver::InitializeFromNode(CP, Config);
    const ContainerParams Input = CP;
    bool Looked = false;
    {
        QuietLog Quiet;
        Looked = LaunchResolver::InitializeFromNode(CP, Config);
    }
    if (Looked && !Sessions::SessionOwned(CP.TempPath))
    {
        const auto Found = OptionReadBack::Read(CP);
        nlohmann::ordered_json Instance = Found.empty() ? nlohmann::ordered_json()
                                        : InstanceStore::ReadConfig(Config, CP.PackageUID, CP.InstanceName);
        bool Touched = false;
        const std::vector<std::string> Changed = Found.empty() ? std::vector<std::string>()
                                               : OptionReadBack::Adopt(Instance, Found, OptionReadBack::Current(CP), Touched);
        std::string Err;
        if (Touched && !InstanceStore::WriteConfig(Config, CP.PackageUID, CP.InstanceName, Instance, &Err))
            LogErr("LaunchSession", "could not store the options read back from the game's files (" + Err + ")");
        else if (!Changed.empty())
        {
            std::string List;
            for (const std::string &K : Changed) List += (List.empty() ? "" : ", ") + K + " = " + Instance["VARIABLES"][K].dump();
            LogOut("LaunchSession", "Changed in the game since the last launch, read back from its files: " + List);
        }
    }
    if (Looked && !Reresolve) return true;
    CP = Input;
    return LaunchResolver::InitializeFromNode(CP, Config);
}

namespace LaunchSession
{

Session::Session(const nlohmann::ordered_json &C, Request R)
    : Config(C), Req(std::move(R)), Said(std::make_unique<LogRecorder>(std::set<std::string>{})) {}

Session::~Session()
{
    Cleanup();
}

std::string Session::MissingCore()
{
    std::string List;
    for (const std::string &M : DepCheck::MissingCore(DepCheck::ProbeSystem(SandboxLayer::Available())))
        List += (List.empty() ? "" : ", ") + M;
    return List;
}

bool Session::Resolve(std::string *Why)
{
    const auto No = [&](const std::string &M) { if (Why) *Why = M; LogErr("LaunchSession", M); return false; };
    if (!Req.Index || Req.NodeId.empty()) return No("nothing to launch: no node graph, or no node");
    ContainerParams P;
    P.NodeIdx           = Req.Index.get();
    P.NodeIdxOwned      = Req.Index;   // the wrapper's copy co-owns the index
    P.LaunchNodeId      = Req.NodeId;
    P.GraftBase         = Req.GraftBase;
    P.Entrypoint        = Req.Entrypoint;
    P.LaunchFace        = Req.Face;
    P.InstanceName      = Req.InstanceName;
    P.Grafts            = Req.Grafts;
    P.VariableOverrides = Req.VariableOverrides;
    P.RunnerChainIds    = Req.RunnerChain;
    P.ScreenWidth       = Req.ScreenWidth;
    P.ScreenHeight      = Req.ScreenHeight;
    P.ReadBackOptions   = Req.ReadBack;
    //The session facts every launch gets (lowest priority; see ContainerParams::SessionVars): the virtual LAN's
    //addresses and the player's own name — empty when the node is down, so the name has a fallback: a package writes
    //it into a game's config verbatim and must never get an empty one.
    P.SessionVars = IpfsWrapper::LanLaunchVars();
    if (P.SessionVars["VIDYAGOD_SELF_NAME"].empty())
    {
        const std::string Nick = IpfsWrapper::GetProfile().Nick;
        P.SessionVars["VIDYAGOD_SELF_NAME"] = Nick.empty() ? std::string("Player") : Nick;
    }
    auto Made = std::make_unique<ContainerWrapper>(Config, P);
    { std::lock_guard<std::mutex> G(Live); W = std::move(Made); }
    if (!W->Resolved()) return No("resolving '" + Req.NodeId + "' failed (see the log)");
    if (!LaunchResolver::ResolveExecutableDefinition(W->ContainerParams)) return No("its entry did not resolve (see the log)");
    return true;
}

bool Session::Build(std::string *Why, bool Played)
{
    if (!W) { if (Why) *Why = "nothing resolved to build"; return false; }
    if (StopAsked) { if (Why) *Why = "stopped before the runtime was built"; LogOut("LaunchSession", "Stopped: the runtime is not built."); return false; }
    LogRecorder BuildSaid({});   // what the build says (and still the launch's: it records on through to Said)
    if (!W->BuildContainerRuntime())
    {
        Cleanup();
        const std::string Missing = MissingCore();
        std::string First;
        for (const auto &E : BuildSaid.Take()) if (E.Level == LogLevel::ERR) { First = E.Message; break; }
        if (Why) *Why = !Missing.empty() ? "this machine is missing " + Missing
                      : !First.empty()   ? First
                                         : "the runtime did not build (see the log)";
        return false;
    }
    Built = true;
    if (!Played || Req.DryRun || StopAsked) return true;   // stopped meanwhile: the game will not start
    //A secret drawn from its POOL this launch is kept, so the value is stable from here on: whatever the game wrote
    //into its prefix with it still matches next time. After the build, as "active" below: a failed build keeps nothing.
    const ContainerParams &CP = W->ContainerParams;
    if (!CP.PickedSecrets.empty() && !CP.PackageUID.empty())
    {
        InstanceStore::MergeVariables(Config, CP.PackageUID, CP.PickedSecrets, CP.InstanceName);
        LogOut("LaunchSession", "Kept " + std::to_string(CP.PickedSecrets.size()) + " pool-seeded secret(s) for " + CP.PackageUID + ".");
    }
    //The instance becomes the active, last-played one. Not with a path override (the instance store is bypassed there).
    if (AppPaths::UserDataPathOverride().empty() && !CP.PackageUID.empty())
        InstanceStore::TouchLastRun(Config, CP.PackageUID, CP.InstanceName);
    return true;
}

Outcome Session::Run()
{
    Outcome O;
    if (!W || !Built) return O;
    const bool Ran = W->Execute();
    O.Crashed  = W->LastCrashed;
    O.Stopped  = W->UserKilled;
    O.ExitCode = W->LastExitCode;
    O.Started  = Ran || O.Crashed;   // false without a crash: refused before it started (the mandatory sandbox)
    if (!O.Started && !O.Stopped)
    {
        LogErr("LaunchSession", "The game was not started (see the log above).");
        for (const std::string &M : DepCheck::MissingCore(DepCheck::ProbeSystem(SandboxLayer::Available())))
            LogErr("LaunchSession", "  missing: " + M + " (Settings → Dependencies shows what to install)");
    }
    else if (O.Crashed && !O.Stopped && !ContainerWrapper::Stopping())   // a non-zero exit is Execute's warning
        LogWarn("LaunchSession", "Game did not exit cleanly (code " + std::to_string(O.ExitCode) + ").");
    return O;
}

void Session::Stop()
{
    StopAsked = true;   // read before the build and before the game starts
    std::lock_guard<std::mutex> G(Live);
    if (W) W->KillGame();
}

bool Session::Cleanup()
{
    if (!W || Down) return true;
    Down = true;
    const std::filesystem::path WriteLayer = W->ContainerParams.WriteLayerPath;
    W->Cleanup();   // what the package keeps is captured first, then the runtime comes down
    const bool Kept = W->CapturedAll;
    //Only a layer this launch built: a refused build (the instance already running) left the running session's alone.
    if (Req.DryRun && Built && !WriteLayer.empty())
    {
        //Normally gone with the runtime already; this covers a runtime kept for inspection.
        std::error_code Ec;
        std::filesystem::remove_all(WriteLayer, Ec);
        if (Ec) LogWarn("LaunchSession", "Dry run: could not remove WRITELAYER: " + Ec.message());
        else    LogSucc("LaunchSession", "Dry run: WRITELAYER deleted.");
    }
    return Kept;
}

Diagnostics::Report Session::Verdict(const std::string &What)
{
    const std::vector<Diagnostics::Entry> Lines = Said ? Said->Take() : std::vector<Diagnostics::Entry>{};
    Said.reset();   // the verdict's own lines are not the launch's
    return Diagnostics::PrintVerdict(What, Diagnostics::Summarize(Lines));
}

} // namespace LaunchSession
