#include "downloadqueue.h"
#include "ipfswrapper.h"
#include "commonutils.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
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
    enum State : std::uint8_t { Queued, Active, Done, Failed } State = Queued;
    int                      Priority = 0;       // higher dispatches sooner; Prioritize bumps it above all queued
    long long                Seq = 0;            // insertion order — tiebreak within a priority (FIFO)
    int                      Attempts = 0;       // completed attempts — drives the retry backoff
    SteadyTP                 ReadyAt;            // not eligible to (re)dispatch until now >= ReadyAt (rotation backoff)
    std::string              Error;
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

bool PathExists(const std::string &P)
{
    std::error_code Ec;
    return fs::exists(P, Ec);
}

// Add Dest to Job.Dests if not already present (keeps Dests a small deduped set).
void AddDest(Job &J, const std::string &Dest)
{
    if (std::find(J.Dests.begin(), J.Dests.end(), Dest) == J.Dests.end()) J.Dests.push_back(Dest);
}

// The first destination already on disk (a fetched job always has one), or "" if none exist yet.
std::string FirstExisting(const Job &J)
{
    for (const std::string &D : J.Dests) if (PathExists(D)) return D;
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
    fs::create_hard_link(From, To, Ec);
    if (!Ec) return true;
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

// Perform one job's fetch (off the dispatcher, holding a DownloadSlot). Fetches the CID to the first missing dest,
// then materializes into the others, and records the terminal state.
void RunJob(const std::string &Cid, std::vector<std::string> Dests, bool Dir)
{
    // Primary = a destination that still needs the node to run. Prefer one that is MISSING; but a dest that EXISTS
    // WITH a `<dest>.part` marker is a crashed finalize that must be repaired (referenced/pinned), so it also needs
    // the fetch — pick it over a plain-existing dest so the repair is not skipped. Fall back to the first.
    std::string Primary = Dests.empty() ? std::string() : Dests.front();
    for (const std::string &D : Dests) if (!PathExists(D) || PathExists(D + ".part")) { Primary = D; break; }

    std::string Err;
    const int Rc = FetchOnce(Cid, Primary, Dir, &Err);   // 0=Done 1=Retryable 2=Terminal — ONE attempt
    bool MatFail = false;
    if (Rc == 0)
        for (const std::string &D : Dests)
            if (D != Primary && !PathExists(D) && !Materialize(Primary, D, &Err)) MatFail = true;   // every dest, or fail

    {
        std::lock_guard<std::mutex> Lk(Q().Mu);
        const bool Preempted   = Q().Preempting.erase(Cid) > 0;
        const bool UserCancel   = Q().UserCancelled.erase(Cid) > 0;
        auto It = Q().Jobs.find(Cid);
        if (It != Q().Jobs.end()) {
            Job &J = It->second;
            if (UserCancel) {
                // A user cancel of this ACTIVE job wins over everything (incl. a concurrent preemption): terminal.
                ClearCancel(Cid);
                J.State = Job::Failed; J.Error = "cancelled";
            } else if (Rc == 0 && !MatFail) {
                J.State = Job::Done; J.Error.clear();
            } else if (Preempted) {
                // We cancelled this active fetch to free a slot for a higher-priority item — NOT a failure. Clear the
                // cancel flag we set (else its next attempt would self-classify Terminal), then rotate it back with NO
                // penalty so it competes again the moment a slot frees; .part is preserved.
                ClearCancel(Cid);
                J.State = Job::Queued; J.Priority = 0; J.Seq = ++Q().Seq; J.ReadyAt = std::chrono::steady_clock::now();
            } else if (Rc == 2 || MatFail) {
                J.State = Job::Failed; J.Error = Err;   // terminal: cancel / bad CID / disk / unwritable dest
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
        std::vector<std::string> Dests = J->Dests;
        Lk.unlock();

        // Hand the slot to a detached worker; loop back to fill the next slot (up to MaxConcurrentDownloads workers).
        std::thread([Cid, Dests = std::move(Dests), Dir, Slot = std::move(Slot)]() mutable {
            RunJob(Cid, Dests, Dir);
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

BatchHandle EnqueueBatch(const std::vector<FetchTarget> &Targets)
{
    BatchHandle Handle;
    bool Woke = false;
    std::vector<std::string> NewlyQueued;
    struct Pending { std::string Cid, Src, Dest; };
    std::vector<Pending> Late;   // Done-join materializes, performed OUTSIDE the lock (a copy can be a large file,
                                 // and EnqueueBatch runs on the GUI thread via the cover sweep)
    {
        std::lock_guard<std::mutex> Lk(Q().Mu);
        for (const FetchTarget &T : Targets) {
            Handle.Items.emplace_back(T.Cid, T.Optional);
            ClearCancel(T.Cid);   // a fresh request clears any prior user-cancel so a re-download is not pre-aborted
            auto It = Q().Jobs.find(T.Cid);
            if (It != Q().Jobs.end()) {
                Job &J = It->second;
                J.Optional = J.Optional && T.Optional;   // required if any requester is required
                switch (J.State) {
                    case Job::Queued:
                    case Job::Active:
                        AddDest(J, T.LocalPath);          // join the in-flight fetch (cross-dest)
                        break;
                    case Job::Done: {
                        if (PathExists(T.LocalPath)) { AddDest(J, T.LocalPath); break; }   // already there
                        const std::string Src = FirstExisting(J);
                        if (!Src.empty()) { AddDest(J, T.LocalPath); Late.push_back({T.Cid, Src, T.LocalPath}); }
                        else { J.State = Job::Queued; J.Seq = ++Q().Seq; AddDest(J, T.LocalPath); Woke = true; NewlyQueued.push_back(T.Cid); }
                        break;
                    }
                    case Job::Failed:                      // retry a previously-failed CID — as a FRESH queue entry:
                        // priority + backoff reset (a re-request is a fresh start; the requester re-bumps if it still
                        // wants the front); the requester re-arms the retry the rolling scheduler stopped at Failed.
                        J.State = Job::Queued; J.Error.clear(); J.Priority = 0; J.Attempts = 0; J.ReadyAt = {};
                        J.Seq = ++Q().Seq; AddDest(J, T.LocalPath); Woke = true; NewlyQueued.push_back(T.Cid);
                        break;
                }
                continue;
            }
            // New CID. Already-on-disk → satisfied (no fetch); else queue it.
            Job NewJob;
            NewJob.Cid = T.Cid;
            NewJob.Dests = { T.LocalPath };
            NewJob.Optional = T.Optional;
            NewJob.Dir = T.Dir;
            if (PathExists(T.LocalPath)) { NewJob.State = Job::Done; }
            else { NewJob.State = Job::Queued; NewJob.Seq = ++Q().Seq; Woke = true; NewlyQueued.push_back(T.Cid); }
            Q().Jobs.emplace(T.Cid, std::move(NewJob));
        }
        LogOut("DownloadQueue::EnqueueBatch", "targets=" + std::to_string(Targets.size())
               + " newly-queued=" + std::to_string(NewlyQueued.size()));
        EnsureDispatcher();
    }
    // Materialize the Done-joins now, lock-free; a failure marks the job Failed so batches SEE the hole.
    // (KNOWN, accepted: the per-CID Failed state is visible to EVERY batch waiting on that CID, so another batch's
    // unwritable dest can fail a batch whose own dest is fine — per-dest completion state is the real fix, tracked.)
    for (const Pending &P : Late) {
        std::string MErr;
        if (!Materialize(P.Src, P.Dest, &MErr)) {
            std::lock_guard<std::mutex> Lk(Q().Mu);
            auto It = Q().Jobs.find(P.Cid);
            if (It != Q().Jobs.end()) { It->second.State = Job::Failed; It->second.Error = MErr; }
        }
    }
    if (!Late.empty()) Q().Cv.notify_all();   // WaitBatch may be blocked on a job we just finalized/failed
    if (Woke) Q().Cv.notify_all();
    notifyQueued(NewlyQueued, true);   // outside the lock: surface the new queued rows in the UI
    return Handle;
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

void DebugResetQueue()
{
    std::lock_guard<std::mutex> Lk(Q().Mu);
    Q().Jobs.clear();          // in-flight detached workers then find() nothing → no-op (they never re-create a job)
    Q().Preempting.clear();
    Q().UserCancelled.clear();
    Q().TopPriority = 0;
}

bool DebugJobPrioritized(const std::string & Cid)
{
    std::lock_guard<std::mutex> Lk(Q().Mu);
    auto It = Q().Jobs.find(Cid);
    return It != Q().Jobs.end() && It->second.Priority > 0;
}

bool WaitBatch(const BatchHandle &Handle, int TimeoutMs, std::string *Error)
{
    std::unique_lock<std::mutex> Lk(Q().Mu);
    const bool Bounded = TimeoutMs > 0;
    const SteadyTP Deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(TimeoutMs);
    for (;;) {
        bool AllTerminal = true;
        for (const auto &[Cid, Optional] : Handle.Items) {
            auto It = Q().Jobs.find(Cid);
            if (It == Q().Jobs.end()) continue;   // pruned/never-created → treat as satisfied
            const Job &J = It->second;
            if (J.State == Job::Failed && !Optional) {   // a REQUIRED job (for THIS batch) failed → abort now
                if (Error) *Error = "could not fetch CID " + Cid + (J.Error.empty() ? "" : " (" + J.Error + ")");
                return false;
            }
            if (J.State != Job::Done && J.State != Job::Failed) AllTerminal = false;
        }
        if (AllTerminal) return true;
        if (Bounded) {
            if (Q().Cv.wait_until(Lk, Deadline) == std::cv_status::timeout) {
                if (Error) *Error = "timed out waiting for the download to finish";
                return false;   // NOT terminal — the jobs stay in the queue and keep rolling (launch semantic)
            }
        } else {
            Q().Cv.wait(Lk);
        }
    }
}

bool WaitBatch(const BatchHandle &Handle, std::string *Error) { return WaitBatch(Handle, 0, Error); }

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

void PrioritizeDownload(const std::string &Cid)
{
    std::lock_guard<std::mutex> Lk(Q().Mu);
    auto It = Q().Jobs.find(Cid);
    if (It == Q().Jobs.end() || It->second.State != Job::Queued) return;   // only queued jobs can jump
    Job &J = It->second;
    // Suppress re-bumping ONLY for an OPTIONAL job that has already had a turn: the cover sweep re-requests every
    // lap, and re-bumping a rotated dead cover would annul its backoff and let dead art perpetually outrank real
    // downloads. A REQUIRED job (a launch layer) and every EXPLICIT user "Prioritize" must always take effect —
    // keying the suppression on history alone silently no-op'd the GUI action (the codebase's worst failure mode).
    if (J.Optional && J.Attempts != 0) return;
    J.Priority = ++Q().TopPriority;   // above every currently-queued job
    J.ReadyAt = {};                   // eligible now

    // Preemption is for REQUIRED work only (a launch layer). An Optional job (a cover) never cancels an active
    // download — dead art must never evict a game mid-transfer. If all slots are busy and this required, fresh job
    // outranks the lowest-priority active one, cancel that one so its slot frees now; RunJob rotates the victim
    // (Preempting → not Failed), keeping its .part.
    if (J.Optional) { Q().Cv.notify_all(); return; }
    int Active = 0; Job *Lowest = nullptr;
    for (auto &Kv : Q().Jobs) {
        if (Kv.second.State != Job::Active) continue;
        ++Active;
        if (!Lowest || Kv.second.Priority < Lowest->Priority) Lowest = &Kv.second;
    }
    if (Active >= MaxConcurrentDownloads() && Lowest && J.Priority > Lowest->Priority) {
        Q().Preempting.insert(Lowest->Cid);
        RequestCancel(Lowest->Cid);   // under Mu: RunJob's completion cannot interleave between insert and cancel
    }
    Q().Cv.notify_all();
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
    const BatchHandle Handle = EnqueueBatch(Targets);
    return WaitBatch(Handle, Error);
}

} // namespace IpfsWrapper
