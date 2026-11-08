#ifndef LAUNCHSESSION_H
#define LAUNCHSESSION_H

#include "commonutils.h"   // Diagnostics::Report, LogRecorder
#include "launchparams.h"
#include "manifestmodel.h" // NodeIndex

#include <nlohmann/json.hpp>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class ContainerWrapper;

// LaunchSession — one launch, whoever starts it (the launch window, the command line, `launch build`): what to run and
// with what, resolved; its runtime built; the game run; what it keeps captured and the runtime taken down; and the
// verdict — every warning and error the launch logged, counted on the thread that runs it, so two launches at once
// each get their own. Made, used and ended on that one thread.
namespace LaunchSession
{

//What to launch: a row of an index, as the launch window or a command names it (a CID; finding it by tile or label is
//the caller's).
struct Request
{
    std::shared_ptr<const NodeIndex> Index;
    std::string NodeId;                                   // the row (a variant, or a graft that is a version)
    std::string GraftBase;                                // a graft row: the version it runs on ("" = its default)
    std::string Entrypoint;                               // which entry (LABEL; "" = the launched tile's)
    std::string Face;                                     // the launched tile (its UID; "" = the node's first) — %UID%
    std::string InstanceName;                             // "" = the active one
    std::optional<std::vector<std::string>> Grafts;       // the instance's graft list (unset: the row's pre-ticked)
    std::map<std::string, std::string> VariableOverrides; // the launch window's values, a command's --var
    std::vector<std::string> RunnerChain;                 // pinned (innermost first); empty = the instance's / default
    std::string ScreenWidth, ScreenHeight;                // asked of the GUI on its thread, before the launch
    bool ReadBack = true;                                 // a launch: what the game changed is read back first
    bool DryRun = false;                                  // the writable layer goes at the end, whatever it holds
};

//How a run ended.
struct Outcome
{
    bool Started = false;   // the game ran (false: refused before it started — no sandbox, a stop meanwhile)
    bool Crashed = false;
    bool Stopped = false;   // the user stopped it (not a failure)
    int ExitCode = 0;
};

class Session
{
public:
    Session(const nlohmann::ordered_json &Config, Request R);
    ~Session();             // takes down whatever is still up
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    //The plan: the row resolved (what the game changed in its files read back first, unless !ReadBack) and its entry.
    //Nothing is written: a plan is pure. False: *Why says why.
    bool Resolve(std::string *Why = nullptr);
    //The runtime: mounted, prefix made, edits applied. Played (a launch, not `launch build`'s look inside) and not a
    //dry run: a secret drawn from its pool is kept in the instance so the next launch draws the same, and the instance
    //is marked last played (not with a path override). False: taken down again, *Why naming what core tool is missing
    //when that is the reason; a stop asked meanwhile ends it here.
    bool Build(std::string *Why = nullptr, bool Played = true);
    Outcome Run();
    //What the package keeps, captured; the runtime taken down (a dry run's writable layer too, once this launch built
    //it — never one a running session of the instance holds). Once; again is a no-op. False: the capture failed.
    bool Cleanup();
    //Every warning and error the launch logged, printed as the verdict for What ("Launch of 'x'") and returned. Taken
    //after Cleanup, so a capture that failed is in it.
    Diagnostics::Report Verdict(const std::string &What);

    ContainerWrapper *Wrapper() const { return W.get(); }   // null before Resolve
    //From any thread: the game is told to stop — before it starts too (the build ends, the game is not started).
    void Stop();
    bool Stopped() const { return StopAsked; }              // a stop was asked: a launch it ended is not a failure

    //What a missing core tool (the mount helper, FUSE/WinFsp, the sandbox) keeps from running games here, "" for none.
    static std::string MissingCore();

private:
    nlohmann::ordered_json Config;
    Request Req;
    std::unique_ptr<LogRecorder> Said;   // this launch's warnings and errors, on its thread
    std::unique_ptr<ContainerWrapper> W;   // set once on the launch's thread, under Live (Stop reads it from another)
    std::mutex Live;
    std::atomic<bool> StopAsked{false};
    bool Built = false, Down = false;
};

//A launch's resolve that first takes back what the game changed in its own files (OptionReadBack): a quiet look-ahead
//resolve says where the options are written and where the user's saved copies are, the values the files hold become
//the instance's (Adopt), and the launch is then resolved for real from its input, on them (a value can gate a layer).
//The look-ahead logs nothing into a running launch's console or verdict. Nothing is read while the instance is running
//(its saved copies are not yet what the session has written), nor with a path override (--userdata-dir: a plain
//resolve). Reresolve false: the look-ahead only — the launch window updating the instance before it shows the values.
[[nodiscard]] bool ResolveTakingBack(ContainerParams &CP, const nlohmann::ordered_json &Config, bool Reresolve = true);

} // namespace LaunchSession

#endif // LAUNCHSESSION_H
