#include "downloadqueue.h"
#include "ipfswrapper.h"
#include "commonutils.h"
#include "nodegraph.h"   // PathWithin

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace IpfsWrapper {

namespace {

namespace fs = std::filesystem;

// One queued/active/finished fetch, keyed by CID. Dests is every destination path that wants this content — the
// dispatcher fetches ONCE (to the first missing dest) and hard-links/copies the result into the rest.
using SteadyTP = std::chrono::steady_clock::time_point;

struct Job {
    std::string              Cid;
    std::vector<std::string> Dests;
    bool                     Optional = true;   // required if ANY requester is required (AND of requesters' Optional)
    bool                     Dir      = false;  // directory (meta) CID
    bool                     Verify   = false;  // any requester asked for refresh semantics (dest verified in Go)
    std::set<std::string>    Keep;              // dests never replaced (redirected into an installed package): filled if missing
    enum State : std::uint8_t { Queued, Active, Done, Failed } State = Queued;
    bool                     Wrote = false;      // Done by writing a dest (a fetch, a copy); not when found already here
    int                      Priority = 0;       // higher dispatches sooner; Prioritize bumps it above all queued
    long long                Seq = 0;            // insertion order — tiebreak within a priority (FIFO)
    int                      Attempts = 0;       // completed attempts — drives the retry backoff
    unsigned long long       SettledAt = 0;      // SettleEpoch when it last settled (a stall wait sees its own land)
    SteadyTP                 ReadyAt;            // not eligible to (re)dispatch until now >= ReadyAt (rotation backoff)
    std::string              Error;
    // Dests taken from this job while it was ACTIVE (dropped by ForgetDestsUnder / ForgetDest, or claimed by another
    // CID): {dest, the dir being dropped — the dest itself when only the file goes}. The worker holds its own copy of
    // Dests, so it may land into them after that; RunJob undoes it.
    std::vector<std::pair<std::string, std::string>> Forgotten;
};

// Exponential retry backoff after a rotated (stalled/exhausted) attempt: 2s,4s,8s,16s, capped at 30s.
std::chrono::milliseconds RetryBackoff(int attempts)
{
    const int shift = std::min(attempts > 0 ? attempts - 1 : 0, 4);
    return std::chrono::milliseconds(std::min(30000, 2000 << shift));
}

struct QState {
    std::mutex                        Mu;
    std::condition_variable           Cv;        // signalled on enqueue / completion / prioritize / cancel
    std::map<std::string, Job>        Jobs;      // CID → job (live + terminal; terminal jobs double as a session cache)
    std::set<std::string>             Preempting;    // CIDs whose active fetch we cancelled to free a slot (rotate, not fail)
    std::set<std::string>             UserCancelled;  // CIDs a user cancelled while ACTIVE (fail, even if also preempted)
    long long                         Seq = 0;
    int                               TopPriority = 0;
    bool                              DispatcherStarted = false;
    bool                              Closed = false;   // shutting down: nothing new runs, every waiter is released
    std::map<std::string, std::string> Moved;           // a moved folder (with '/') → its new place (with '/')
    //Every dest → the CID whose job lists it (a dest is ONE CID's at a time): claiming a dest, and finding who holds
    //one, looked through every job's dests — per target, under this lock (thousands of targets over thousands of jobs
    //when a received library is asked for after a start).
    std::unordered_map<std::string, std::string> DestOwner;
};

QState &Q()
{
    // Deliberately LEAKED: the detached dispatcher thread iterates Jobs for the process lifetime, and a function-local
    // static would be destroyed at exit WHILE that thread is inside AnyQueued() → _Rb_tree_increment on a freed map
    // (real SIGSEGV, caught via coredump from a headless probe exiting right after its fetches). The OS reclaims the
    // memory at process death anyway; leaking the singleton is the standard fix for statics shared with free threads.
    static QState * S = new QState();
    return *S;
}

QueueStateCallback g_QueueCb;   // UI sink (queued=true on new job, false on cancel-of-queued); set once by IpfsManager

// Fire the queue-state callback for a list of CIDs OUTSIDE the queue lock (the callback marshals to the GUI thread).
void notifyQueued(const std::vector<std::string> & Cids, bool Queued)
{
    if (!g_QueueCb) return;
    for (const std::string & C : Cids) g_QueueCb(C, Queued);
}

// The one form the queue keeps and compares a path in: normalised, '/'-separated. Callers hand it paths built every
// way — "<root>/CATALOG" joined with path::operator/ ("…/CATALOG\\nick - lib" on Windows), lexically_normal()'d
// (all '\\' there) — and every dest test is a text prefix: in mixed forms a moved package's redirect, or a forgotten
// stub, never matched on Windows, and a late download re-created the stub beside the installed package.
std::string DestKey(const std::string &P)
{
    return P.empty() ? P : fs::path(P).lexically_normal().generic_string();
}

bool PathExists(const std::string &P)
{
    std::error_code Ec;
    return fs::exists(P, Ec);
}

// A dest belongs to exactly ONE CID: a node path is reused across generations (the same label, a new CID; a
// collision-qualified name whose owner changes with the plan's order), and a job materialises EVERY dest it
// remembers from its primary. Without this, a re-run of the OLD cid's job rewrote a path the NEW cid now owns —
// three "Vanilla" files that all held the Age of Kings block, and no Conquerors.
static std::atomic<long long> ClaimScans{0};        // ClaimDest's scans of every job (a test: a re-enqueue makes none)
static std::atomic<long long> AlreadyHereJobs{0};   // Verify jobs over with nothing fetched (a test: a restart fetches none)
//Bumped (under Mu) whenever a job fails, or a settled one is asked for again (Done or Failed → Queued): a wait looks
//again at every item it holds — from the first — only when one of them may have.
static unsigned long long FailEpoch = 0;
//Bumped (under Mu) whenever a fetch job settles — done or failed: a wait on a stall (WaitBatchWhileMoving) gives up only
//after a while with none.
static unsigned long long SettleEpoch = 0;
static std::atomic<long long> WaitLooks{0};   // items a WaitBatch looked at (a test: a wake looks at what is left, not all)
static std::atomic<long long> Wakes{0};       // prioritize wake-ups (a test: a batch of them wakes the queue once)
static void ClaimDest(const std::string &Cid, const std::string &Dest)
{
    ++ClaimScans;
    const auto Owner = Q().DestOwner.find(Dest);
    if (Owner == Q().DestOwner.end() || Owner->second == Cid) return;
    if (const auto O = Q().Jobs.find(Owner->second); O != Q().Jobs.end())
    {
        Job &J = O->second;
        const auto It = std::remove(J.Dests.begin(), J.Dests.end(), Dest);
        if (It != J.Dests.end())
        {
            J.Dests.erase(It, J.Dests.end());
            if (J.State == Job::Active) J.Forgotten.emplace_back(Dest, Dest);   // its worker may still land it: undone after
        }
    }
    Q().DestOwner.erase(Owner);
}

// Add Dest to Job.Dests if not already present (keeps Dests a small deduped set); the dest is this CID's now.
void AddDest(Job &J, const std::string &Dest)
{
    if (std::find(J.Dests.begin(), J.Dests.end(), Dest) == J.Dests.end()) J.Dests.push_back(Dest);
    Q().DestOwner[Dest] = J.Cid;
}

// A dest a job lists no more: no longer its CID's (unless another job's already).
void DropOwner(const std::string &Dest, const std::string &Cid)
{
    if (const auto It = Q().DestOwner.find(Dest); It != Q().DestOwner.end() && It->second == Cid) Q().DestOwner.erase(It);
}

// The first destination already on disk that holds this job's node, or "" if none exist yet. Never a kept dest (an
// installed package's own file): it is there whatever CID the job asked for.
std::string FirstExisting(const Job &J)
{
    for (const std::string &D : J.Dests) if (!J.Keep.count(D) && PathExists(D)) return D;
    return std::string();
}

// Cross-dest materialize: put the fetched file From at To without re-fetching. Hard-link when possible (same inode →
// seeded once, survives either name being deleted), else copy across filesystems. A failure (unwritable dest,
// ENOSPC) is the JOB's failure: a dedup'd CID promised bytes at EVERY dest, and "logged but Done" made
// --download-all print "full mirror complete" over a hole.
[[nodiscard]] bool Materialize(const std::string &From, const std::string &To, std::string *Error = nullptr)
{
    std::error_code Ec;
    fs::create_directories(fs::path(To).parent_path(), Ec);
    Ec.clear();
    //A FOLDER job's result is a tree the fetch owns whole (a re-landed package folder replaces its dir): place the
    //tree, file by file, the same way — hard link, else copy.
    if (fs::is_directory(From, Ec))
    {
        fs::remove_all(To, Ec);
        std::error_code Wc;
        for (auto It = fs::recursive_directory_iterator(From, Wc); !Wc && It != fs::recursive_directory_iterator(); It.increment(Wc))
        {
            const fs::path Rel = fs::relative(It->path(), From, Wc);
            if (Wc) break;
            const fs::path Out = fs::path(To) / Rel;
            std::error_code Fc;
            if (It->is_directory(Fc)) { fs::create_directories(Out, Fc); continue; }
            fs::create_directories(Out.parent_path(), Fc);
            Fc.clear();
            fs::create_hard_link(It->path(), Out, Fc);
            if (Fc) { Fc.clear(); fs::copy_file(It->path(), Out, fs::copy_options::overwrite_existing, Fc); }
            if (Fc) { Wc = Fc; break; }
        }
        if (!Wc) return true;
        LogWarn("DownloadQueue::Materialize", "could not place " + To + " from " + From + " (" + Wc.message() + ")");
        if (Error) *Error = "could not place " + To + " (" + Wc.message() + ")";
        return false;
    }
    fs::create_hard_link(From, To, Ec);
    if (!Ec) return true;
    //Already the same file (another request for the same CID placed it here first — two packages sharing a zip):
    //placed. copy_file refuses to copy a file onto itself ("File exists"), and that failed the whole download.
    if (std::error_code Eq; fs::equivalent(From, To, Eq)) return true;
    std::error_code Ec2;
    fs::copy_file(From, To, fs::copy_options::overwrite_existing, Ec2);
    if (!Ec2) return true;
    LogWarn("DownloadQueue::Materialize", "could not place " + To + " from " + From + " (" + Ec2.message() + ")");
    if (Error) *Error = "could not place " + To + " (" + Ec2.message() + ")";
    return false;
}

// Highest-priority queued job that is READY (its rotation backoff has elapsed), FIFO within a priority. A queued job
// still backing off is skipped — that is stall-demotion: it waits its turn while healthy work runs. Caller holds Mu.
Job *PickReady()
{
    const SteadyTP Now = std::chrono::steady_clock::now();
    Job *Best = nullptr;
    for (auto &Kv : Q().Jobs) {
        Job &J = Kv.second;
        if (J.State != Job::Queued || J.ReadyAt > Now) continue;
        if (!Best || J.Priority > Best->Priority || (J.Priority == Best->Priority && J.Seq < Best->Seq)) Best = &J;
    }
    return Best;
}

bool AnyReady() { return PickReady() != nullptr; }

// The soonest a currently-queued (backing-off) job becomes eligible — so the dispatcher sleeps exactly until then
// instead of spinning when every queued job is still in backoff (the all-stalled case). Caller holds Mu.
bool EarliestReadyAt(SteadyTP &Out)
{
    bool Any = false;
    for (const auto &Kv : Q().Jobs) {
        if (Kv.second.State != Job::Queued) continue;
        if (!Any || Kv.second.ReadyAt < Out) { Out = Kv.second.ReadyAt; Any = true; }
    }
    return Any;
}

//Every dest there, no crashed finalize beside it, each file hashing to Cid, and the node holding Cid: a dest that
//merely exists proves nothing for a Verify job (a reusable node path can hold an older version).
bool AlreadyHere(const std::string &Cid, const std::vector<std::string> &Dests)
{
    if (Dests.empty()) return false;
    for (const std::string &D : Dests)
    {
        if (!PathExists(D) || PathExists(D + ".part")) return false;
        std::string CErr;
        if (ComputeCid(D, &CErr) != Cid) return false;
    }
    if (!HasLocal(Cid)) return false;
    ++AlreadyHereJobs;
    return true;
}

// Perform one job's fetch (off the dispatcher, holding a DownloadSlot). Fetches the CID to the first missing dest,
// then materializes into the others, and records the terminal state.
void RunJob(const std::string &Cid, std::vector<std::string> Dests, bool Dir, bool Verify, const std::set<std::string> &Keep)
{
    // Primary = a destination that still needs the node to run. Prefer one that is MISSING; but a dest that EXISTS
    // WITH a `<dest>.part` marker is a crashed finalize that must be repaired (referenced/pinned), so it also needs
    // the fetch — pick it over a plain-existing dest so the repair is not skipped. Else the first dest that is not
    // kept: a kept dest (an installed package's own file) that is there holds whatever node it holds, and every other
    // dest would be copied from it. Fall back to the first.
    std::string Primary;
    for (const std::string &D : Dests) if (!PathExists(D) || PathExists(D + ".part")) { Primary = D; break; }
    if (Primary.empty()) for (const std::string &D : Dests) if (!Keep.count(D)) { Primary = D; break; }
    if (Primary.empty() && !Dests.empty()) Primary = Dests.front();

    std::string Err;
    //A Verify job whose every dest already holds its CID, the node holding it too, is over with nothing fetched: every
    //received node file, on every start (the queue's record of what it landed lives only as long as the app), went to
    //the node one by one — read twice, hashed twice, a Started and a Finished each, through the download slots.
    const bool Here = Verify && !Dir && AlreadyHere(Cid, Dests);
    const int Rc = Here ? 0 : FetchOnce(Cid, Primary, Dir, &Err, Verify && !Keep.count(Primary));   // 0=Done 1=Retryable 2=Terminal — ONE attempt
    bool MatFail = false;
    if (Rc == 0 && !Here)
        for (const std::string &D : Dests)
            // Every dest, or fail. A Verify job's dests are REUSABLE paths (a node file: same path, new CID on
            // re-publish), so an EXISTING non-primary dest can hold a stale version — overwrite it from the
            // just-verified Primary (Materialize's copy fallback overwrites); presence proves nothing there.
            if (D != Primary && ((Verify && !Keep.count(D)) || !PathExists(D)))
            {
                //Already this node's (hashed once — what clearing it read anyway): left as it is, not copied over again.
                if (Verify && !Keep.count(D) && PathExists(D) && !PathExists(D + ".part"))
                {
                    std::string CErr;
                    const std::string Have = ComputeCid(D, &CErr);
                    if (Have == Cid) continue;
                    if (!Have.empty()) ClearStaleDest(D, Cid);          // another node's file there: it goes, reference and all
                }
                if (!Materialize(Primary, D, &Err)) MatFail = true;
            }

    {
        std::lock_guard<std::mutex> Lk(Q().Mu);
        const bool Preempted   = Q().Preempting.erase(Cid) > 0;
        const bool UserCancel   = Q().UserCancelled.erase(Cid) > 0;
        auto It = Q().Jobs.find(Cid);
        if (It != Q().Jobs.end()) {
            Job &J = It->second;
            // A dir dropped mid-fetch (an adopted or withdrawn package stub) must stay dropped: undo what this attempt
            // landed there — the dest, then every directory above it the fetch re-created, up to the dropped dir.
            // Under Mu, so a re-enqueue that claims the dest again (it is back in J.Dests) is never undone. A dest
            // another CID's job holds now is that job's: never removed here; a finished one runs again (this attempt
            // may have written over its file, and its fetch hash-checks the dest).
            for (const auto &[D, Root] : std::exchange(J.Forgotten, {}))
            {
                if (std::find(J.Dests.begin(), J.Dests.end(), D) != J.Dests.end()) continue;
                Job *Owner = nullptr;
                if (const auto Own = Q().DestOwner.find(D); Own != Q().DestOwner.end() && Own->second != Cid)
                    if (const auto O = Q().Jobs.find(Own->second); O != Q().Jobs.end()) Owner = &O->second;
                if (Owner)
                {
                    if (Owner->State == Job::Done) { Owner->State = Job::Queued; Owner->Seq = ++Q().Seq; Owner->ReadyAt = {}; ++FailEpoch; }
                    continue;
                }
                std::error_code Ec;
                std::filesystem::remove_all(D, Ec);
                const std::filesystem::path R = std::filesystem::path(Root).lexically_normal();
                for (std::filesystem::path P = std::filesystem::path(D).parent_path().lexically_normal();
                     !P.empty() && (P == R || NodeGraph::PathWithin(R, P)); P = P.parent_path())
                    if (!std::filesystem::remove(P, Ec)) break;   // only an empty dir goes; the first non-empty stops it
            }
            if (UserCancel) {
                // A user cancel of this ACTIVE job wins over everything (incl. a concurrent preemption): terminal.
                ClearCancel(Cid);
                J.State = Job::Failed; J.Error = "cancelled"; ++FailEpoch; J.SettledAt = ++SettleEpoch;
            } else if (Rc == 0 && !MatFail) {
                J.State = Job::Done; J.Error.clear(); J.Wrote = !Here; J.SettledAt = ++SettleEpoch;
            } else if (Preempted) {
                // We cancelled this active fetch to free a slot for a higher-priority item — NOT a failure. Clear the
                // cancel flag we set (else its next attempt would self-classify Terminal), then rotate it back with NO
                // penalty so it competes again the moment a slot frees; .part is preserved.
                ClearCancel(Cid);
                J.State = Job::Queued; J.Priority = 0; J.Seq = ++Q().Seq; J.ReadyAt = std::chrono::steady_clock::now();
            } else if (Rc == 2 || MatFail) {
                J.State = Job::Failed; J.Error = Err; ++FailEpoch; J.SettledAt = ++SettleEpoch;   // terminal: cancel / bad CID / disk / unwritable dest
            } else {
                // Retryable (stall / no providers / offline): ROTATE — strict back-of-queue + exponential backoff.
                // This IS the retry loop; the slot is freed (this worker returns) so healthy work runs meanwhile.
                J.Attempts++;
                J.State = Job::Queued; J.Priority = 0; J.Seq = ++Q().Seq;
                J.ReadyAt = std::chrono::steady_clock::now() + RetryBackoff(J.Attempts);
            }
        }
    }
    Q().Cv.notify_all();   // wake WaitBatch (result available) + the dispatcher (a slot just freed / a backoff set)
}

void DispatcherLoop()
{
    for (;;) {
        std::unique_lock<std::mutex> Lk(Q().Mu);
        // Wait until a READY job exists. If jobs are only backing off (all-stalled), sleep exactly until the earliest
        // becomes eligible instead of spinning; a wake (enqueue/completion/prioritize/cancel) rechecks immediately.
        while (!AnyReady()) {
            SteadyTP Next;
            if (EarliestReadyAt(Next)) Q().Cv.wait_until(Lk, Next);
            else Q().Cv.wait(Lk);
        }
        Lk.unlock();

        DownloadSlot Slot;   // blocks until a concurrency slot frees — MUST NOT hold Mu while waiting

        Lk.lock();
        Job *J = PickReady();           // a job may have been taken / cancelled / re-backed-off while we waited
        if (!J) { Lk.unlock(); continue; }   // nothing ready now — release the slot (RAII) and re-wait
        J->State = Job::Active;
        const std::string Cid = J->Cid;
        const bool Dir = J->Dir;
        const bool Verify = J->Verify;
        std::vector<std::string> Dests = J->Dests;
        std::set<std::string> Keep = J->Keep;
        Lk.unlock();

        // Hand the slot to a detached worker; loop back to fill the next slot (up to MaxConcurrentDownloads workers).
        std::thread([Cid, Dests = std::move(Dests), Dir, Verify, Keep = std::move(Keep), Slot = std::move(Slot)]() mutable {
            RunJob(Cid, Dests, Dir, Verify, Keep);
        }).detach();
    }
}

void EnsureDispatcher()   // caller holds Mu
{
    if (Q().DispatcherStarted) return;
    Q().DispatcherStarted = true;
    std::thread(DispatcherLoop).detach();
}

} // namespace

static std::string RedirectedLocked(const std::string &Path);   // below

static bool PrioritizeLocked(const std::string &Cid);

BatchHandle EnqueueBatch(const std::vector<FetchTarget> &Targets)
{
    BatchHandle Handle;
    bool Woke = false;
    std::vector<std::string> NewlyQueued;
    struct Pending { std::string Cid, Src, Dest; bool Verify = false; };
    std::vector<Pending> Late;   // Done-join materializes, performed OUTSIDE the lock (a copy can be a large file,
                                 // and EnqueueBatch runs on the GUI thread via the cover sweep)
    {
        std::lock_guard<std::mutex> Lk(Q().Mu);
        for (const FetchTarget &T0 : Targets) {
            FetchTarget T = T0;
            T.LocalPath = DestKey(T.LocalPath);
            // A file under a folder moved since the caller read it lands where the folder is now. Never a FOLDER target:
            // a folder fetch replaces its destination whole, and a received package folder redirected onto the
            // installed package deleted the game.
            //Redirected into an installed package: a node file there is the package's own, changed only by a version
            //switch. The fetch lands it when missing and never replaces it.
            bool Keep = false;
            if (!T.Dir)
            {
                const std::string Was = T.LocalPath;
                T.LocalPath = RedirectedLocked(T.LocalPath);
                Keep = T.LocalPath != Was;
                if (Keep) T.Verify = false;
            }
            Handle.Items.emplace_back(T.Cid, T.Optional);
            if (Q().Closed) {                                 // shutting down: the request fails at once, never waits
                Job &J = Q().Jobs[T.Cid];
                J.Cid = T.Cid;
                if (J.State != Job::Done) { J.State = Job::Failed; J.Error = "shutting down"; ++FailEpoch; }
                continue;
            }
            ClearCancel(T.Cid);   // a fresh request clears any prior user-cancel so a re-download is not pre-aborted
            //Whatever job held this path before, it is this CID's now. A dest already this CID's is no other job's (every
            //dest a job lists was claimed as it was added): claiming it again scanned every job — per target, under the
            //lock, on the GUI thread — and a resync of a received library is thousands of targets over thousands of jobs.
            if (const auto Own = Q().Jobs.find(T.Cid);
                Own == Q().Jobs.end() || std::find(Own->second.Dests.begin(), Own->second.Dests.end(), T.LocalPath) == Own->second.Dests.end())
                ClaimDest(T.Cid, T.LocalPath);
            auto It = Q().Jobs.find(T.Cid);
            if (It != Q().Jobs.end()) {
                Job &J = It->second;
                if (Keep) J.Keep.insert(T.LocalPath);
                J.Optional = J.Optional && T.Optional;   // required if any requester is required
                J.Verify   = J.Verify || T.Verify;
                switch (J.State) {
                    case Job::Queued:
                    case Job::Active:
                        AddDest(J, T.LocalPath);          // join the in-flight fetch (cross-dest)
                        Handle.Fetching.push_back(T.Cid);
                        break;
                    case Job::Done: {
                        // Trust exactly the dests THIS job wrote/verified (J.Dests) — a repeat of one of them is a
                        // no-op. A NEW dest materializes from one of them (same CID ⇒ same bytes). For a Verify
                        // target, mere PRESENCE of the new dest proves nothing (a reusable node path can hold an
                        // OLDER version), so it is materialized over; non-Verify keeps the presence shortcut.
                        if (PathExists(T.LocalPath)
                            && std::find(J.Dests.begin(), J.Dests.end(), T.LocalPath) != J.Dests.end()) break;
                        if (!T.Verify && PathExists(T.LocalPath)) { AddDest(J, T.LocalPath); break; }   // already there
                        const std::string Src = FirstExisting(J);
                        if (!Src.empty()) { AddDest(J, T.LocalPath); Late.push_back({T.Cid, Src, T.LocalPath, T.Verify && !Keep}); }
                        else { J.State = Job::Queued; J.Seq = ++Q().Seq; ++FailEpoch; AddDest(J, T.LocalPath); Woke = true; NewlyQueued.push_back(T.Cid); Handle.Fetching.push_back(T.Cid); }
                        break;
                    }
                    case Job::Failed:                      // retry a previously-failed CID — as a FRESH queue entry:
                        // priority + backoff reset (a re-request is a fresh start; the requester re-bumps if it still
                        // wants the front); the requester re-arms the retry the rolling scheduler stopped at Failed.
                        J.State = Job::Queued; J.Error.clear(); J.Priority = 0; J.Attempts = 0; J.ReadyAt = {}; ++FailEpoch;
                        J.Seq = ++Q().Seq; AddDest(J, T.LocalPath); Woke = true; NewlyQueued.push_back(T.Cid); Handle.Fetching.push_back(T.Cid);
                        break;
                }
                continue;
            }
            // New CID. "Already there" needs BOTH the dest on disk AND the node holding this CID — a dest can be
            // occupied by DIFFERENT content (a re-published node: same NODE_ID → same path, NEW cid; a modified
            // restore), and treating that as Done would silently pin the receiver to the stale version forever.
            // When the block isn't local the job queues and the Go fetch decides at the dest (hash-verify: adopt a
            // matching out-of-band file, discard + overwrite a stale one).
            Job NewJob;
            NewJob.Cid = T.Cid;
            NewJob.Dests = { T.LocalPath };
            Q().DestOwner[T.LocalPath] = T.Cid;
            NewJob.Optional = T.Optional;
            NewJob.Dir = T.Dir;
            NewJob.Verify = T.Verify;
            if (Keep) NewJob.Keep.insert(T.LocalPath);
            // A Verify target ALWAYS goes to the Go fetch (it hash-verifies the dest and overwrites a stale file):
            // for a reused dest neither presence nor a local block proves the file holds THIS cid.
            if (!T.Verify && PathExists(T.LocalPath) && IpfsWrapper::HasLocal(T.Cid)) { NewJob.State = Job::Done; }
            else { NewJob.State = Job::Queued; NewJob.Seq = ++Q().Seq; Woke = true; NewlyQueued.push_back(T.Cid); Handle.Fetching.push_back(T.Cid); }
            Q().Jobs.emplace(T.Cid, std::move(NewJob));
        }
        for (const FetchTarget &T : Targets)
            if (T.Front && PrioritizeLocked(T.Cid)) { ++Wakes; Woke = true; }
        EnsureDispatcher();
    }
    LogOut("DownloadQueue::EnqueueBatch", "targets=" + std::to_string(Targets.size())   // written with the lock let go
           + " newly-queued=" + std::to_string(NewlyQueued.size()));
    // Materialize the Done-joins now, lock-free; a failure marks the job Failed so batches SEE the hole.
    // (KNOWN, accepted: the per-CID Failed state is visible to EVERY batch waiting on that CID, so another batch's
    // unwritable dest can fail a batch whose own dest is fine — per-dest completion state is the real fix, tracked.)
    for (const Pending &P : Late) {
        std::string MErr;
        if (P.Verify) ClearStaleDest(P.Dest, P.Cid);                      // another node's file there: it goes first
        if (!Materialize(P.Src, P.Dest, &MErr)) {
            std::lock_guard<std::mutex> Lk(Q().Mu);
            auto It = Q().Jobs.find(P.Cid);
            if (It != Q().Jobs.end()) { It->second.State = Job::Failed; It->second.Error = MErr; ++FailEpoch; }
        }
        else ++Handle.Placed;
    }
    if (!Late.empty()) Q().Cv.notify_all();   // WaitBatch may be blocked on a job we just finalized/failed
    if (Woke) Q().Cv.notify_all();
    notifyQueued(NewlyQueued, true);   // outside the lock: surface the new queued rows in the UI
    return Handle;
}

// The (first) destination path a CID's queued/active job will write to — "" when the CID has no job. Lets the UI
// name a transfer from where it is going (the queue is the one place that knows), instead of "(unknown)".
std::string QueueDestForCid(const std::string & Cid)
{
    std::lock_guard<std::mutex> Lk(Q().Mu);
    auto It = Q().Jobs.find(Cid);
    return (It == Q().Jobs.end() || It->second.Dests.empty()) ? std::string() : It->second.Dests.front();
}

bool JobUnfinished(const std::string &Cid)
{
    std::lock_guard<std::mutex> Lk(Q().Mu);
    auto It = Q().Jobs.find(Cid);
    return It != Q().Jobs.end() && It->second.State != Job::Done;
}

bool JobSettled(const std::string &Cid, bool *Landed)
{
    std::lock_guard<std::mutex> Lk(Q().Mu);
    auto It = Q().Jobs.find(Cid);
    const bool Done = It != Q().Jobs.end() && It->second.State == Job::Done;
    if (Landed) *Landed = Done && It->second.Wrote;
    return It == Q().Jobs.end() || Done || It->second.State == Job::Failed;
}

long long DebugClaimScans() { return ClaimScans.load(); }
long long DebugAlreadyHere() { return AlreadyHereJobs.load(); }
long long DebugWaitLooks() { return WaitLooks.load(); }
long long DebugPrioritizeWakes() { return Wakes.load(); }

bool QueueBusy()
{
    std::lock_guard<std::mutex> Lk(Q().Mu);
    const SteadyTP Now = std::chrono::steady_clock::now();
    for (const auto &[Cid, J] : Q().Jobs)
        if (J.State == Job::Active || (J.State == Job::Queued && J.ReadyAt <= Now)) return true;
    return false;
}

int DebugJobState(const std::string & Cid)
{
    std::lock_guard<std::mutex> Lk(Q().Mu);
    auto It = Q().Jobs.find(Cid);
    return It == Q().Jobs.end() ? -1 : (int)It->second.State;
}

bool DebugJobBackingOff(const std::string & Cid)
{
    std::lock_guard<std::mutex> Lk(Q().Mu);
    auto It = Q().Jobs.find(Cid);
    return It != Q().Jobs.end() && It->second.State == Job::Queued
           && It->second.ReadyAt > std::chrono::steady_clock::now();
}

bool DebugIsPreempting(const std::string & Cid)
{
    std::lock_guard<std::mutex> Lk(Q().Mu);
    return Q().Preempting.count(Cid) > 0;
}

void CloseQueue()
{
    std::vector<std::string> Dropped;
    {
        std::lock_guard<std::mutex> Lk(Q().Mu);
        Q().Closed = true;
        for (auto &[Cid, J] : Q().Jobs)
            if (J.State == Job::Queued) { J.State = Job::Failed; J.Error = "shutting down"; ++FailEpoch; Dropped.push_back(Cid); }
            else if (J.State == Job::Active) { Q().UserCancelled.insert(Cid); RequestCancel(Cid); }
    }
    Q().Cv.notify_all();
    notifyQueued(Dropped, false);
}

void OpenQueue()
{
    std::lock_guard<std::mutex> Lk(Q().Mu);
    Q().Closed = false;
}

void DebugResetQueue()
{
    std::lock_guard<std::mutex> Lk(Q().Mu);
    Q().Jobs.clear();          // in-flight detached workers then find() nothing → no-op (they never re-create a job)
    Q().DestOwner.clear();
    Q().Preempting.clear();
    Q().UserCancelled.clear();
    Q().TopPriority = 0;
    Q().Closed = false;
    Q().Moved.clear();
}

bool DebugJobPrioritized(const std::string & Cid)
{
    std::lock_guard<std::mutex> Lk(Q().Mu);
    auto It = Q().Jobs.find(Cid);
    return It != Q().Jobs.end() && It->second.Priority > 0;
}

//Sliding: the deadline moves on while the batch moves — one of its items settled, or one waits its first turn.
static bool WaitBatchFor(const BatchHandle &Handle, int TimeoutMs, bool Sliding, std::string *Error)
{
    std::unique_lock<std::mutex> Lk(Q().Mu);
    const bool Bounded = TimeoutMs > 0;
    SteadyTP Deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(TimeoutMs);
    unsigned long long SeenSettle = SettleEpoch;
    size_t FirstAtSlide = 0;
    //Every event of the queue wakes every wait, and a wait looked at all of its items each time — a 5,000-file batch
    //looked 5,000 times at 5,000 items, under the lock every fetch takes. Now each wake goes on from the first item not
    //settled yet and stops at the next unsettled one; every item is looked at again, from the first, only when a job has
    //failed or a settled one was asked for again since (a required item failing ends the wait at once).
    size_t First = 0;
    unsigned long long SeenFail = ~0ull;
    const auto Settled = [&](const std::pair<std::string, bool> &Item, bool *RequiredFailed) {
        ++WaitLooks;
        auto It = Q().Jobs.find(Item.first);
        if (It == Q().Jobs.end()) return true;               // pruned/never-created → treat as satisfied
        const Job &J = It->second;
        if (J.State == Job::Failed && !Item.second) {        // a REQUIRED job (for THIS batch) failed → abort now
            *RequiredFailed = true;
            if (Error) *Error = "could not fetch CID " + Item.first + (J.Error.empty() ? "" : " (" + J.Error + ")");
        }
        return J.State == Job::Done || J.State == Job::Failed;
    };
    for (;;) {
        bool RequiredFailed = false;
        if (SeenFail != FailEpoch)
        {   //A job failed, or one settled is unsettled again (its file overwritten, its copies gone): from the first.
            SeenFail = FailEpoch;
            First = 0;
            for (size_t I = 0; I < Handle.Items.size(); ++I)
                if (Settled(Handle.Items[I], &RequiredFailed) && RequiredFailed) return false;
        }
        while (First < Handle.Items.size() && Settled(Handle.Items[First], &RequiredFailed))
        {
            if (RequiredFailed) return false;
            ++First;
        }
        if (First == Handle.Items.size()) return true;
        //The batch is moving: one of its own items settled since, or one has not had its first try yet (it waits its turn
        //behind the queue's other work, which is moving). An item that keeps failing to be had (its peer away) while
        //unrelated downloads land is not movement.
        if (Sliding && SeenSettle != SettleEpoch)
        {
            bool Moving = First != FirstAtSlide;
            for (size_t I = First; I < Handle.Items.size() && !Moving; ++I)
            {
                const auto It = Q().Jobs.find(Handle.Items[I].first);
                if (It == Q().Jobs.end()) continue;
                const Job &J = It->second;
                Moving = J.SettledAt > SeenSettle || (J.State != Job::Done && J.State != Job::Failed && J.Attempts == 0);
            }
            SeenSettle = SettleEpoch;
            FirstAtSlide = First;
            if (Moving) Deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(TimeoutMs);
        }
        if (Bounded) {
            if (Q().Cv.wait_until(Lk, Deadline) == std::cv_status::timeout && std::chrono::steady_clock::now() >= Deadline
                && !(Sliding && SeenSettle != SettleEpoch)) {
                if (Error) *Error = Sliding ? "none of it landed for " + std::to_string(TimeoutMs / 1000) + " s"
                                            : "timed out waiting for the download to finish";
                return false;   // NOT terminal — the jobs stay in the queue and keep rolling (launch semantic)
            }
        } else {
            Q().Cv.wait(Lk);
        }
    }
}

bool WaitBatch(const BatchHandle &Handle, int TimeoutMs, std::string *Error) { return WaitBatchFor(Handle, TimeoutMs, false, Error); }
bool WaitBatch(const BatchHandle &Handle, std::string *Error) { return WaitBatch(Handle, 0, Error); }
bool WaitBatchWhileMoving(const BatchHandle &Handle, int StallMs, std::string *Error)
{
    return WaitBatchFor(Handle, StallMs, true, Error);
}

std::string FailureOf(const std::string &Cid)
{
    std::lock_guard<std::mutex> Lk(Q().Mu);
    const auto It = Q().Jobs.find(Cid);
    if (It == Q().Jobs.end() || It->second.State != Job::Failed) return {};
    return It->second.Error.empty() ? std::string("failed") : It->second.Error;
}

// The path's place now: under the longest moved folder it lies in, rewritten to where that folder went. Q().Mu held.
static std::string RedirectedLocked(const std::string &Path)
{
    const std::pair<const std::string, std::string> *Best = nullptr;
    for (const auto &M : Q().Moved)
        if (Path.compare(0, M.first.size(), M.first) == 0 && (!Best || M.first.size() > Best->first.size())) Best = &M;
    return Best ? Best->second + Path.substr(Best->first.size()) : Path;
}

std::string Redirected(const std::string &Path)
{
    std::lock_guard<std::mutex> Lk(Q().Mu);
    return RedirectedLocked(DestKey(Path));
}

void RedirectDestsUnder(const std::string &From, const std::string &To)
{
    auto Slash = [](const std::string &D) { return D.empty() || D.back() == '/' ? D : D + "/"; };
    std::lock_guard<std::mutex> Lk(Q().Mu);
    Q().Moved[Slash(DestKey(From))] = Slash(DestKey(To));
}

void DropRedirectsUnder(const std::string &Dir)
{
    //Only a move whose destination is gone (the installed package deleted): the plan that calls this was read a moment
    //ago, and an install finishing in between registered a move that holds. One stat per move, on a rare call.
    const std::string D = DestKey(Dir);
    const std::string Prefix = D.empty() || D.back() == '/' ? D : D + "/";
    std::lock_guard<std::mutex> Lk(Q().Mu);
    for (auto It = Q().Moved.begin(); It != Q().Moved.end();)
    {
        //The folder first: a stat of every move, per call, made a library's resync (a call per package) a stat of
        //every move per package, under the queue's lock.
        if (It->first.compare(0, Prefix.size(), Prefix) != 0) { ++It; continue; }
        std::error_code Ec;
        const bool Gone = !std::filesystem::exists(It->second.substr(0, It->second.size() - 1), Ec);
        It = Gone ? Q().Moved.erase(It) : std::next(It);
    }
}

void ForgetDestsUnder(const std::string &DirIn)
{
    const std::string Dir = DestKey(DirIn);
    std::lock_guard<std::mutex> Lk(Q().Mu);
    const std::string Prefix = Dir.empty() || Dir.back() == '/' ? Dir : Dir + "/";
    for (auto &[C, J] : Q().Jobs)
        J.Dests.erase(std::remove_if(J.Dests.begin(), J.Dests.end(),
                                     [&](const std::string &D) {
                                         if (D.compare(0, Prefix.size(), Prefix) != 0) return false;
                                         if (J.State == Job::Active) J.Forgotten.emplace_back(D, Dir);   // RunJob undoes a late landing
                                         DropOwner(D, C);
                                         return true;
                                     }),
                      J.Dests.end());
}

void ForgetDestsUnder(const std::vector<std::string> &DirsIn)
{
    if (DirsIn.empty()) return;
    std::map<std::string, std::string> Under;        // "<dir>/" → the dir as RunJob records it
    for (const std::string &In : DirsIn)
    {
        const std::string Dir = DestKey(In);
        Under.emplace(Dir.empty() || Dir.back() == '/' ? Dir : Dir + "/", Dir);
    }
    //The folder a dest is under, if any: its own folders looked up, outermost first (the one whose drop undoes the most).
    const auto Holder = [&](const std::string &D) -> const std::string * {
        for (size_t At = D.find('/'); At != std::string::npos; At = D.find('/', At + 1))
            if (const auto It = Under.find(D.substr(0, At + 1)); It != Under.end()) return &It->second;
        return nullptr;
    };
    std::lock_guard<std::mutex> Lk(Q().Mu);
    for (auto &[C, J] : Q().Jobs)
        J.Dests.erase(std::remove_if(J.Dests.begin(), J.Dests.end(),
                                     [&](const std::string &D) {
                                         const std::string *Dir = Holder(D);
                                         if (!Dir) return false;
                                         if (J.State == Job::Active) J.Forgotten.emplace_back(D, *Dir);   // RunJob undoes a late landing
                                         DropOwner(D, C);
                                         return true;
                                     }),
                      J.Dests.end());
}

void ForgetDest(const std::string &Cid, const std::string &DestIn)
{
    const std::string Dest = DestKey(DestIn);
    std::lock_guard<std::mutex> Lk(Q().Mu);
    const auto It = Q().Jobs.find(Cid);
    if (It == Q().Jobs.end()) return;
    Job &J = It->second;
    const auto From = std::remove(J.Dests.begin(), J.Dests.end(), Dest);
    if (From == J.Dests.end()) return;
    J.Dests.erase(From, J.Dests.end());
    DropOwner(Dest, Cid);
    J.Keep.erase(Dest);
    if (J.State == Job::Active) J.Forgotten.emplace_back(Dest, Dest);   // RunJob undoes a late landing of the file
}

bool LandingUnder(const std::string &DirIn)
{
    const std::string Dir = DestKey(DirIn);
    const std::string Prefix = Dir.empty() || Dir.back() == '/' ? Dir : Dir + "/";
    std::lock_guard<std::mutex> Lk(Q().Mu);
    for (const auto &[C, J] : Q().Jobs)
    {
        if (J.State == Job::Done || J.State == Job::Failed) continue;
        for (const std::string &D : J.Dests) if (D.compare(0, Prefix.size(), Prefix) == 0) return true;
    }
    return false;
}

void CancelDownload(const std::string &Cid)
{
    bool WasQueued = false;
    {
        std::lock_guard<std::mutex> Lk(Q().Mu);
        auto It = Q().Jobs.find(Cid);
        if (It != Q().Jobs.end()) {
            if (It->second.State == Job::Queued) {
                It->second.State = Job::Failed;          // queued-but-not-started → drop it (waiters see a failure)
                It->second.Error = "cancelled";
                ++FailEpoch;
                WasQueued = true;
            } else if (It->second.State == Job::Active) {
                Q().UserCancelled.insert(Cid);           // active → RunJob must FAIL it (not rotate, even if preempted)
            }
        }
        RequestCancel(Cid);   // abort the active fetch at its next checkpoint (under Mu: no interleave with RunJob)
    }
    Q().Cv.notify_all();
    if (WasQueued) notifyQueued({ Cid }, false);   // it never started → drop its queued row in the UI
}

void SetQueueCallback(QueueStateCallback Cb) { g_QueueCb = std::move(Cb); }

//PrioritizeDownload's work, Mu held; false when nothing changed (nobody needs waking).
static bool PrioritizeLocked(const std::string &Cid)
{
    auto It = Q().Jobs.find(Cid);
    if (It == Q().Jobs.end() || It->second.State != Job::Queued) return false;   // only queued jobs can jump
    Job &J = It->second;
    // Suppress re-bumping ONLY for an OPTIONAL job that has already had a turn: the cover sweep re-requests every
    // lap, and re-bumping a rotated dead cover would annul its backoff and let dead art perpetually outrank real
    // downloads. A REQUIRED job (a launch layer) and every EXPLICIT user "Prioritize" must always take effect —
    // keying the suppression on history alone silently no-op'd the GUI action (the codebase's worst failure mode).
    if (J.Optional && J.Attempts != 0) return false;
    J.Priority = ++Q().TopPriority;   // above every currently-queued job
    J.ReadyAt = {};                   // eligible now

    // Preemption is for REQUIRED work only (a launch layer). An Optional job (a cover) never cancels an active
    // download — dead art must never evict a game mid-transfer. If all slots are busy and this required, fresh job
    // outranks the lowest-priority active one, cancel that one so its slot frees now; RunJob rotates the victim
    // (Preempting → not Failed), keeping its .part.
    if (J.Optional) return true;
    int Active = 0; Job *Lowest = nullptr;
    //A job already being preempted frees its slot for the one that asked first: each required job asks for a slot of its
    //own (several missing layers picked the same victim, and went through one slot one after another).
    for (auto &Kv : Q().Jobs) {
        if (Kv.second.State != Job::Active) continue;
        ++Active;
        if (Q().Preempting.count(Kv.second.Cid)) continue;
        if (!Lowest || Kv.second.Priority < Lowest->Priority) Lowest = &Kv.second;
    }
    if (Active >= MaxConcurrentDownloads() && Lowest && J.Priority > Lowest->Priority) {
        Q().Preempting.insert(Lowest->Cid);
        RequestCancel(Lowest->Cid);   // under Mu: RunJob's completion cannot interleave between insert and cancel
    }
    return true;
}

void PrioritizeDownload(const std::string &Cid)
{
    std::lock_guard<std::mutex> Lk(Q().Mu);
    if (PrioritizeLocked(Cid)) { ++Wakes; Q().Cv.notify_all(); }
}

void PrioritizeDownloads(const std::vector<std::string> &Cids)
{
    std::lock_guard<std::mutex> Lk(Q().Mu);
    bool Any = false;
    for (const std::string &Cid : Cids) Any = PrioritizeLocked(Cid) || Any;
    if (Any) { ++Wakes; Q().Cv.notify_all(); }
}

// FetchToPath / FetchDirToPath — the blocking convenience wrappers, now thin over the ONE queue: enqueue a single
// job and wait. TimeoutMs>0 bounds only THIS caller's WAIT (a launch's 120 s); on timeout the job STAYS in the queue
// and keeps rolling (it is ready when the caller next looks), so a slow layer turns "launch failed, retry" into
// "launch when ready". A dir sync fast-fails while offline (unchanged), else waits bounded so startup never hangs on
// a wedged source — the job keeps rolling and the next catalog sync picks it up.
constexpr int DirSyncWaitMs = 120000;

std::string FetchToPath(const std::string &Cid, const std::string &DestPathStr, std::string *Error, int TimeoutMs)
{
    if (Cid.empty())         { if (Error) *Error = "empty CID";               return std::string(); }
    if (DestPathStr.empty()) { if (Error) *Error = "empty destination path"; return std::string(); }
    const BatchHandle H = EnqueueBatch({ FetchTarget{ Cid, DestPathStr, /*Optional=*/false, /*Dir=*/false } });
    if (TimeoutMs > 0) PrioritizeDownload(Cid);   // a bounded (launch) caller must not starve behind a bulk batch
    if (!WaitBatch(H, TimeoutMs, Error)) return std::string();
    LogSucc("IpfsWrapper::FetchToPath", "Materialized CID " + Cid + " at " + DestPathStr);
    return DestPathStr;
}

std::string FetchDirToPath(const std::string &Cid, const std::string &DestDirStr, std::string *Error)
{
    if (Cid.empty())        { if (Error) *Error = "empty CID";              return std::string(); }
    if (DestDirStr.empty()) { if (Error) *Error = "empty destination dir"; return std::string(); }
    if (!DaemonRunning() && !FetchOnceHookActive()) { if (Error) *Error = "IPFS networking is offline"; return std::string(); }
    const BatchHandle H = EnqueueBatch({ FetchTarget{ Cid, DestDirStr, /*Optional=*/false, /*Dir=*/true } });
    if (!WaitBatch(H, DirSyncWaitMs, Error)) return std::string();
    LogSucc("IpfsWrapper::FetchDirToPath", "Materialized folder CID " + Cid + " at " + DestDirStr);
    return DestDirStr;
}

// The batch download entry point, now backed by the queue: enqueue every target (deduped by CID + already-seeded) and
// block until they finish. Multiple concurrent callers share the ONE queue, so a CID wanted by two packages (or a game
// and its runner) is fetched exactly once, and global concurrency stays bounded by the single dispatcher's slots.
bool FetchTargetsConcurrent(const std::vector<FetchTarget> &Targets, std::string *Error)
{
    // Fail LOUD when offline: an unbounded WaitBatch would otherwise block forever (every attempt rotates as
    // Retryable) with no signal — the old node-backed API returned "node not started". Callers here (CLI,
    // --download-all, the download manager) run with the node up; a genuine offline is a real error to surface.
    if (!DaemonRunning()) { if (Error) *Error = "IPFS networking is offline"; return false; }
    const ForegroundHold Hold;
    const BatchHandle Handle = EnqueueBatch(Targets);
    return WaitBatch(Handle, Error);
}

} // namespace IpfsWrapper
