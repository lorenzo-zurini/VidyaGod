#include "downloadqueue.h"
#include "ipfswrapper.h"
#include "commonutils.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace IpfsWrapper {

namespace {

namespace fs = std::filesystem;

// One queued/active/finished fetch, keyed by CID. Dests is every destination path that wants this content — the
// dispatcher fetches ONCE (to the first missing dest) and hard-links/copies the result into the rest.
struct Job {
    std::string              Cid;
    std::vector<std::string> Dests;
    bool                     Optional = true;   // required if ANY requester is required (AND of requesters' Optional)
    enum State : std::uint8_t { Queued, Active, Done, Failed } State = Queued;
    int                      Priority = 0;       // higher dispatches sooner; Prioritize bumps it above all queued
    long long                Seq = 0;            // insertion order — tiebreak within a priority (FIFO)
    std::string              Error;
};

struct QState {
    std::mutex                        Mu;
    std::condition_variable           Cv;        // signalled on enqueue / completion / prioritize / cancel
    std::map<std::string, Job>        Jobs;      // CID → job (live + terminal; terminal jobs double as a session cache)
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
// seeded once, survives either name being deleted), else copy across filesystems. Best-effort; logged on failure.
void Materialize(const std::string &From, const std::string &To)
{
    std::error_code Ec;
    fs::create_directories(fs::path(To).parent_path(), Ec);
    Ec.clear();
    fs::create_hard_link(From, To, Ec);
    if (!Ec) return;
    std::error_code Ec2;
    fs::copy_file(From, To, fs::copy_options::overwrite_existing, Ec2);
    if (Ec2) LogWarn("DownloadQueue::Materialize", "could not place " + To + " from " + From + " (" + Ec2.message() + ")");
}

// Pick the highest-priority queued job (FIFO within a priority). Returns nullptr if none is queued. Caller holds Mu.
Job *PickQueued()
{
    Job *Best = nullptr;
    for (auto &Kv : Q().Jobs) {
        Job &J = Kv.second;
        if (J.State != Job::Queued) continue;
        if (!Best || J.Priority > Best->Priority || (J.Priority == Best->Priority && J.Seq < Best->Seq)) Best = &J;
    }
    return Best;
}

bool AnyQueued()
{
    for (const auto &Kv : Q().Jobs) if (Kv.second.State == Job::Queued) return true;
    return false;
}

// Perform one job's fetch (off the dispatcher, holding a DownloadSlot). Fetches the CID to the first missing dest,
// then materializes into the others, and records the terminal state.
void RunJob(const std::string &Cid, std::vector<std::string> Dests)
{
    // Primary = the first destination not already on disk (so a partial/missing one drives the fetch); else the first.
    std::string Primary = Dests.empty() ? std::string() : Dests.front();
    for (const std::string &D : Dests) if (!PathExists(D)) { Primary = D; break; }

    std::string Err;
    const bool Ok = !FetchToPath(Cid, Primary, &Err).empty();
    if (Ok)
        for (const std::string &D : Dests)
            if (D != Primary && !PathExists(D)) Materialize(Primary, D);

    {
        std::lock_guard<std::mutex> Lk(Q().Mu);
        auto It = Q().Jobs.find(Cid);
        if (It != Q().Jobs.end()) {
            It->second.State = Ok ? Job::Done : Job::Failed;
            It->second.Error = Ok ? std::string() : Err;
        }
    }
    Q().Cv.notify_all();   // wake WaitBatch (result available) + the dispatcher (a slot just freed)
}

void DispatcherLoop()
{
    for (;;) {
        std::unique_lock<std::mutex> Lk(Q().Mu);
        Q().Cv.wait(Lk, [] { return AnyQueued(); });   // sleep until there's something to fetch
        Lk.unlock();

        DownloadSlot Slot;   // blocks until a concurrency slot frees — MUST NOT hold Mu while waiting

        Lk.lock();
        Job *J = PickQueued();          // a queued job may have been cancelled/taken while we waited for the slot
        if (!J) { Lk.unlock(); continue; }   // nothing to do now — release the slot (RAII) and re-wait
        J->State = Job::Active;
        const std::string Cid = J->Cid;
        std::vector<std::string> Dests = J->Dests;
        Lk.unlock();

        // Hand the slot to a detached worker; loop back to fill the next slot (up to MaxConcurrentDownloads workers).
        std::thread([Cid, Dests = std::move(Dests), Slot = std::move(Slot)]() mutable {
            RunJob(Cid, Dests);
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
    {
        std::lock_guard<std::mutex> Lk(Q().Mu);
        for (const FetchTarget &T : Targets) {
            Handle.Items.emplace_back(T.Cid, T.Optional);
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
                        if (!Src.empty()) { Materialize(Src, T.LocalPath); AddDest(J, T.LocalPath); }
                        else { J.State = Job::Queued; J.Seq = ++Q().Seq; AddDest(J, T.LocalPath); Woke = true; NewlyQueued.push_back(T.Cid); }
                        break;
                    }
                    case Job::Failed:                      // retry a previously-failed CID
                        J.State = Job::Queued; J.Error.clear(); J.Seq = ++Q().Seq; AddDest(J, T.LocalPath); Woke = true; NewlyQueued.push_back(T.Cid);
                        break;
                }
                continue;
            }
            // New CID. Already-on-disk → satisfied (no fetch); else queue it.
            Job NewJob;
            NewJob.Cid = T.Cid;
            NewJob.Dests = { T.LocalPath };
            NewJob.Optional = T.Optional;
            if (PathExists(T.LocalPath)) { NewJob.State = Job::Done; }
            else { NewJob.State = Job::Queued; NewJob.Seq = ++Q().Seq; Woke = true; NewlyQueued.push_back(T.Cid); }
            Q().Jobs.emplace(T.Cid, std::move(NewJob));
        }
        EnsureDispatcher();
    }
    if (Woke) Q().Cv.notify_all();
    notifyQueued(NewlyQueued, true);   // outside the lock: surface the new queued rows in the UI
    return Handle;
}

bool WaitBatch(const BatchHandle &Handle, std::string *Error)
{
    std::unique_lock<std::mutex> Lk(Q().Mu);
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
        Q().Cv.wait(Lk);
    }
}

void CancelDownload(const std::string &Cid)
{
    bool WasQueued = false;
    {
        std::lock_guard<std::mutex> Lk(Q().Mu);
        auto It = Q().Jobs.find(Cid);
        if (It != Q().Jobs.end() && It->second.State == Job::Queued) {
            It->second.State = Job::Failed;              // queued-but-not-started → drop it (waiters see a failure)
            It->second.Error = "cancelled";
            WasQueued = true;
        }
    }
    RequestCancel(Cid);       // active fetch → abort at its next checkpoint (harmless if already gone)
    Q().Cv.notify_all();
    if (WasQueued) notifyQueued({ Cid }, false);   // it never started → drop its queued row in the UI
}

void SetQueueCallback(QueueStateCallback Cb) { g_QueueCb = std::move(Cb); }

void PrioritizeDownload(const std::string &Cid)
{
    {
        std::lock_guard<std::mutex> Lk(Q().Mu);
        auto It = Q().Jobs.find(Cid);
        if (It == Q().Jobs.end() || It->second.State != Job::Queued) return;   // only queued jobs can jump
        It->second.Priority = ++Q().TopPriority;         // above every currently-queued job
    }
    Q().Cv.notify_all();
}

// The batch download entry point, now backed by the queue: enqueue every target (deduped by CID + already-seeded) and
// block until they finish. Multiple concurrent callers share the ONE queue, so a CID wanted by two packages (or a game
// and its runner) is fetched exactly once, and global concurrency stays bounded by the single dispatcher's slots.
bool FetchTargetsConcurrent(const std::vector<FetchTarget> &Targets, std::string *Error)
{
    const BatchHandle Handle = EnqueueBatch(Targets);
    return WaitBatch(Handle, Error);
}

} // namespace IpfsWrapper
