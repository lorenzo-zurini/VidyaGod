#include <QtTest/QtTest>
#include <QTemporaryDir>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include "downloadqueue.h"
#include "ipfswrapper.h"

static std::atomic<bool> g_stopHook{false};

// DownloadQueue's ENQUEUE-side contracts, exercised with no node: a dest already on disk satisfies a new CID
// without a fetch, and joining a Done job at a NEW dest materializes (hard-link/copy) from an existing one.
// The queue is a process-global keyed by CID, so every case uses its own CID.
//
// The review finding these teeth pin: Materialize() failures were LOGGED but the job stayed Done, so a batch —
// and --download-all on top of it — reported "full mirror complete" over a dest that was never written.
class DownloadQueueTest : public QObject
{
    Q_OBJECT

private slots:
    void aDestAlreadyOnDiskSatisfiesTheBatchWithoutANode()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const std::string A = (dir.path() + "/a.bin").toStdString();
        { std::ofstream(A) << "bytes"; }
        auto H = IpfsWrapper::EnqueueBatch({{"CID_DQ_SAT", A, /*Optional=*/false}});
        std::string Err;
        QVERIFY(IpfsWrapper::WaitBatch(H, &Err));
        QVERIFY(Err.empty());
    }

    void joiningADoneJobAtANewDestMaterializesFromTheExistingOne()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const std::string A = (dir.path() + "/a.bin").toStdString();
        const std::string B = (dir.path() + "/sub/b.bin").toStdString();   // parent created by Materialize
        { std::ofstream(A) << "the same bytes at every dest"; }
        QVERIFY(IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch({{"CID_DQ_MAT", A, false}})));
        std::string Err;
        QVERIFY(IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch({{"CID_DQ_MAT", B, false}}), &Err));
        std::error_code Ec;
        QVERIFY(std::filesystem::exists(B, Ec));
        QCOMPARE(std::filesystem::file_size(B, Ec), std::filesystem::file_size(A, Ec));
    }

    // THE review finding. Teeth (each caught): drop the `J.State = Job::Failed` on the EnqueueBatch materialize
    // → WaitBatch returns true over the hole; make Materialize void again → same.
    void aMaterializeFailureFailsTheBatchInsteadOfReportingAMirrorOverAHole()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const std::string A = (dir.path() + "/a.bin").toStdString();
        { std::ofstream(A) << "bytes"; }
        // The unwritable parent is a regular FILE — ENOTDIR on every platform and for every euid (a read-only
        // DIRECTORY is ignored by Windows and by root, which made the first version of this test Linux-non-root-only).
        const std::string RO = (dir.path() + "/ro").toStdString();
        { std::ofstream(RO) << "a file where a directory would be needed"; }
        const std::string B = RO + "/sub/b.bin";                           // create_directories under a FILE must fail
        QVERIFY(IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch({{"CID_DQ_HOLE", A, false}})));
        std::string Err;
        const bool Ok = IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch({{"CID_DQ_HOLE", B, false}}), &Err);
        QVERIFY2(!Ok, "a dest that could not be written must FAIL the batch, not vanish into a log line");
        QVERIFY(!Err.empty());
        std::error_code Ec;
        QVERIFY(!std::filesystem::exists(B, Ec));
    }

    // An OPTIONAL requester of the same holed CID must still tolerate the failure — the per-batch view.
    void anOptionalRequesterToleratesTheSameHole()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const std::string A = (dir.path() + "/a.bin").toStdString();
        { std::ofstream(A) << "bytes"; }
        const std::string RO = (dir.path() + "/ro").toStdString();
        { std::ofstream(RO) << "a file where a directory would be needed"; }
        QVERIFY(IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch({{"CID_DQ_OPT", A, false}})));
        std::string Err;
        const bool Ok = IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch({{"CID_DQ_OPT", RO + "/sub/b.bin", /*Optional=*/true}}), &Err);
        QVERIFY2(Ok, "an optional job's failure must not abort the batch");
    }

    // ---- rolling scheduler (stall-demotion), driven by the FetchOnce test hook ----

    void cleanup()
    {
        g_stopHook = true;                       // unblock any hook still holding a slot
        IpfsWrapper::SetFetchOnceHook({});
        QTest::qWait(150);                       // let blocked/rotating workers return + release their slots
        IpfsWrapper::DebugResetQueue();          // drop this test's jobs so none bleed into the next (shared global queue)
        IpfsWrapper::SetMaxConcurrentDownloads(3);
    }

    // THE core of the whole change: a STALLED attempt (Retryable) must ROTATE — back to Queued, backing off — and
    // NEVER hold its DownloadSlot or turn into Failed. Teeth: RunJob marking Retryable as Failed → state Failed;
    // dropping the backoff → not BackingOff.
    void aStalledAttemptRotatesAndBacksOffInsteadOfFailingOrHoldingASlot()
    {
        IpfsWrapper::SetFetchOnceHook([](const std::string &, const std::string &, bool, std::string *e) {
            if (e) *e = "no providers"; return 1; /* Retryable */ });
        const std::string C = "CID_ROLL_STALL";
        IpfsWrapper::EnqueueBatch({{C, "/nonexistent/roll_stall.bin", true}});
        QTRY_VERIFY_WITH_TIMEOUT(IpfsWrapper::DebugJobBackingOff(C), 5000);   // rotated to Queued + backing off
        QVERIFY2(IpfsWrapper::DebugJobState(C) != 3, "a stalled attempt must never be Failed");
        IpfsWrapper::CancelDownload(C);                                        // stop it before the next test
    }

    // A TERMINAL attempt (cancel / bad CID / disk) fails the job — no endless rotation. Teeth: RunJob rotating a
    // terminal rc → never reaches Failed.
    void aTerminalAttemptFailsTheJob()
    {
        IpfsWrapper::SetFetchOnceHook([](const std::string &, const std::string &, bool, std::string *e) {
            if (e) *e = "malformed CID"; return 2; /* Terminal */ });
        const std::string C = "CID_ROLL_TERMINAL";
        IpfsWrapper::EnqueueBatch({{C, "/nonexistent/roll_term.bin", true}});
        QTRY_COMPARE_WITH_TIMEOUT(IpfsWrapper::DebugJobState(C), 3 /* Failed */, 5000);
    }

    // A dest belongs to ONE CID. A node path is reused across generations (same label, new CID), and a job
    // materialises every dest it remembers — so when a NEW cid claims a path, the OLD cid's job must forget it, or
    // its next run rewrites the path with the old block (three "Vanilla" files, all the Age of Kings block).
    void aDestClaimedByANewCidIsForgottenByTheOldOne()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const std::string X = (dir.path() + "/Vanilla.json").toStdString();
        const std::string Y = (dir.path() + "/Vanilla (aaa).json").toStdString();
        IpfsWrapper::SetFetchOnceHook([](const std::string &cid, const std::string &dest, bool, std::string *) {
            std::ofstream(dest) << cid; return 0; });
        auto Read = [](const std::string &P) { std::ifstream I(P); std::string S; std::getline(I, S); return S; };
        QVERIFY(IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch({{"CID_OLD_GEN", X, false, false, true}}), 5000));
        QCOMPARE(Read(X), std::string("CID_OLD_GEN"));
        // the next generation: another cid claims X; the old cid moves to Y
        QVERIFY(IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch({{"CID_NEW_GEN", X, false, false, true},
                                                                 {"CID_OLD_GEN", Y, false, false, true}}), 5000));
        QCOMPARE(Read(Y), std::string("CID_OLD_GEN"));
        QCOMPARE(Read(X), std::string("CID_NEW_GEN"));          // the old cid's job did NOT rewrite X
    }

    // Forgetting a directory's dests: after a friend's stubs were removed on purpose, a later run of a job that
    // remembered a path under that directory must not recreate it.
    void forgottenDestsAreNeverMaterialisedAgain()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const std::string Old = (dir.path() + "/gen1/node.json").toStdString();
        const std::string New = (dir.path() + "/gen2/node.json").toStdString();
        std::filesystem::create_directories(dir.path().toStdString() + "/gen1");
        std::filesystem::create_directories(dir.path().toStdString() + "/gen2");   // the hook writes flat (no parent creation)
        IpfsWrapper::SetFetchOnceHook([](const std::string &cid, const std::string &dest, bool, std::string *) {
            std::ofstream(dest) << cid; return 0; });
        QVERIFY(IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch({{"CID_FORGET", Old, false, false, true}}), 5000));
        std::filesystem::remove_all(dir.path().toStdString() + "/gen1");
        IpfsWrapper::ForgetDestsUnder(dir.path().toStdString() + "/gen1");
        QVERIFY(IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch({{"CID_FORGET", New, false, false, true}}), 5000));
        std::error_code Ec;
        QVERIFY(std::filesystem::exists(New, Ec));
        QVERIFY2(!std::filesystem::exists(Old, Ec), "a forgotten path was materialised again");
    }

    // A DONE attempt completes and materializes EVERY dest from the fetched primary.
    void aDoneAttemptCompletesAndMaterializesEveryDest()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const std::string P = (dir.path() + "/prim.bin").toStdString();
        const std::string Q = (dir.path() + "/sub/copy.bin").toStdString();
        IpfsWrapper::SetFetchOnceHook([](const std::string &, const std::string &dest, bool, std::string *) {
            std::ofstream(dest) << "fetched-bytes"; return 0; /* Done — wrote the primary */ });
        auto H = IpfsWrapper::EnqueueBatch({{"CID_ROLL_DONE", P, false}, {"CID_ROLL_DONE", Q, false}});
        std::string Err;
        QVERIFY2(IpfsWrapper::WaitBatch(H, 5000, &Err), Err.c_str());
        std::error_code Ec;
        QVERIFY(std::filesystem::exists(P, Ec));
        QVERIFY(std::filesystem::exists(Q, Ec));   // materialized into the second dest
    }

    // A BOUNDED wait (the launch semantic) returns "not ready" on timeout and LEAVES the job rolling in the queue —
    // it is NOT marked Failed. Teeth: WaitBatch treating a timeout as a terminal failure → state Failed.
    void aBoundedWaitLeavesTheJobRollingInsteadOfFailingIt()
    {
        IpfsWrapper::SetFetchOnceHook([](const std::string &, const std::string &, bool, std::string *e) {
            if (e) *e = "no providers"; return 1; /* forever Retryable */ });
        const std::string C = "CID_ROLL_BOUNDED";
        auto H = IpfsWrapper::EnqueueBatch({{C, "/nonexistent/roll_bounded.bin", false}});
        std::string Err;
        QVERIFY2(!IpfsWrapper::WaitBatch(H, 300, &Err), "a still-rolling job must time out, not report success");
        QVERIFY2(IpfsWrapper::DebugJobState(C) != 3, "a bounded-wait timeout must not FAIL the job — it keeps rolling");
        IpfsWrapper::CancelDownload(C);
    }

    // All-stalled must NOT hot-loop the dispatcher: with a job that only ever stalls, the attempt count over a window
    // is bounded by the backoff (a few), not hundreds. Teeth: dropping the backoff / the wait_until → the counter
    // explodes.
    void aStalledJobDoesNotHotLoopTheDispatcher()
    {
        std::atomic<int> calls{0};
        IpfsWrapper::SetFetchOnceHook([&calls](const std::string &, const std::string &, bool, std::string *) {
            calls.fetch_add(1); return 1; /* Retryable, returns immediately */ });
        const std::string C = "CID_ROLL_NOSPIN";
        IpfsWrapper::EnqueueBatch({{C, "/nonexistent/roll_nospin.bin", true}});
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));   // first attempt + one ~2s backoff not yet elapsed
        IpfsWrapper::CancelDownload(C);
        QVERIFY2(calls.load() <= 3, qPrintable(QString("dispatcher hot-looped: %1 attempts in 1.5s").arg(calls.load())));
    }

    // PREEMPTION: forcing a queued item when all slots are busy cancels the lowest-priority ACTIVE job to free a slot
    // NOW; the preempted job ROTATES (keeps its partial), it is not Failed. Teeth: remove the preemption block in
    // PrioritizeDownload → HIGH never runs while LOW hogs the only slot (the QTRY times out).
    void forcingAQueuedItemPreemptsTheLowestActiveJob()
    {
        g_stopHook = false;
        const int SavedMax = IpfsWrapper::MaxConcurrentDownloads();
        IpfsWrapper::SetMaxConcurrentDownloads(1);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const std::string HighDest = (dir.path() + "/high.bin").toStdString();
        IpfsWrapper::SetFetchOnceHook([HighDest](const std::string &cid, const std::string &dest, bool, std::string *) {
            if (cid == "CID_PREEMPT_LOW") {
                while (!IpfsWrapper::DebugIsPreempting("CID_PREEMPT_LOW") && !g_stopHook) // hold the slot until preempted
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                return 1;                                                  // then rotate (Retryable)
            }
            std::ofstream(dest) << "high"; return 0;                       // HIGH completes once it gets the slot
        });
        IpfsWrapper::EnqueueBatch({{"CID_PREEMPT_LOW", (dir.path() + "/low.bin").toStdString(), true}});
        QTRY_COMPARE_WITH_TIMEOUT(IpfsWrapper::DebugJobState("CID_PREEMPT_LOW"), 1 /* Active — holds the slot */, 5000);
        IpfsWrapper::EnqueueBatch({{"CID_PREEMPT_HIGH", HighDest, false}});
        IpfsWrapper::PrioritizeDownload("CID_PREEMPT_HIGH");               // must preempt LOW to run
        QTRY_COMPARE_WITH_TIMEOUT(IpfsWrapper::DebugJobState("CID_PREEMPT_HIGH"), 2 /* Done */, 5000);
        QVERIFY2(IpfsWrapper::DebugJobState("CID_PREEMPT_LOW") != 3, "a preempted job must rotate, not Fail");
        IpfsWrapper::CancelDownload("CID_PREEMPT_LOW");
        IpfsWrapper::SetMaxConcurrentDownloads(SavedMax);
    }

    // CRITICAL (review): an OPTIONAL job (a cover) must NEVER preempt an active download — dead art evicting a game
    // mid-transfer was the reintroduced bug. Teeth: drop the `if (J.Optional) return` guard → the active job is
    // cancelled and this asserts it stayed Active.
    void anOptionalJobNeverPreemptsAnActiveDownload()
    {
        g_stopHook = false;
        const int SavedMax = IpfsWrapper::MaxConcurrentDownloads();
        IpfsWrapper::SetMaxConcurrentDownloads(1);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        // The active game hook blocks until the test stops — so it holds the one slot and, crucially, never returns
        // to erase a preemption flag. A cover forcing itself must therefore leave DebugIsPreempting(game) FALSE.
        IpfsWrapper::SetFetchOnceHook([](const std::string &, const std::string &, bool, std::string *) {
            while (!g_stopHook) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return 1;
        });
        IpfsWrapper::EnqueueBatch({{"CID_OPT_ACTIVE", (dir.path()+"/a.bin").toStdString(), /*Optional=*/false}});
        QTRY_COMPARE_WITH_TIMEOUT(IpfsWrapper::DebugJobState("CID_OPT_ACTIVE"), 1 /* Active */, 5000);
        IpfsWrapper::EnqueueBatch({{"CID_OPT_COVER", (dir.path()+"/c.bin").toStdString(), /*Optional=*/true}});
        IpfsWrapper::PrioritizeDownload("CID_OPT_COVER");           // a cover forcing itself
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        QVERIFY2(!IpfsWrapper::DebugIsPreempting("CID_OPT_ACTIVE"),
                 "an Optional (cover) job must never mark an active game for preemption");
        QCOMPARE(IpfsWrapper::DebugJobState("CID_OPT_ACTIVE"), 1); // and the game is still Active
        g_stopHook = true;
        IpfsWrapper::CancelDownload("CID_OPT_ACTIVE"); IpfsWrapper::CancelDownload("CID_OPT_COVER");
        IpfsWrapper::SetMaxConcurrentDownloads(SavedMax);
    }

    // CRITICAL (review): a re-swept dead COVER (Optional, Attempts>0) must NOT be re-prioritized — else its backoff
    // is annulled and dead art perpetually outranks real work. But a REQUIRED job (a launch layer) and every explicit
    // user Prioritize MUST still take effect even after a rotation — suppressing by history alone silently no-op'd the
    // GUI action. Teeth: `if (J.Attempts != 0) return` (unscoped) → the required case fails; dropping the guard
    // entirely → the optional case fails.
    void reprioritizeSuppressedForRotatedCoversOnlyNotRequiredJobs()
    {
        IpfsWrapper::SetFetchOnceHook([](const std::string &, const std::string &, bool, std::string *) { return 1; /* rotate */ });
        const std::string Cov = "CID_REBUMP_COVER", Req = "CID_REBUMP_REQ";
        IpfsWrapper::EnqueueBatch({{Cov, "/nonexistent/cover.bin", /*Optional=*/true}});
        IpfsWrapper::EnqueueBatch({{Req, "/nonexistent/req.bin",   /*Optional=*/false}});
        QTRY_VERIFY_WITH_TIMEOUT(IpfsWrapper::DebugJobBackingOff(Cov), 5000);   // both rotated once (Attempts>0)
        QTRY_VERIFY_WITH_TIMEOUT(IpfsWrapper::DebugJobBackingOff(Req), 5000);
        IpfsWrapper::PrioritizeDownload(Cov);
        IpfsWrapper::PrioritizeDownload(Req);
        QVERIFY2(!IpfsWrapper::DebugJobPrioritized(Cov), "a rotated cover must not jump the queue again");
        QVERIFY2(IpfsWrapper::DebugJobPrioritized(Req),  "a required job / explicit Prioritize must always take effect");
        IpfsWrapper::CancelDownload(Cov); IpfsWrapper::CancelDownload(Req);
    }

    // MEDIUM (review): a USER cancel of an ACTIVE job must FAIL it, even if a preemption is racing — the cancel is not
    // swallowed by the preempt-rotate. Teeth: drop the UserCancel branch in RunJob → the job rotates (not Failed).
    void aUserCancelOfAnActiveJobFailsItNotRotates()
    {
        g_stopHook = false;
        std::atomic<bool> release{false};
        IpfsWrapper::SetFetchOnceHook([&release](const std::string &, const std::string &, bool, std::string *e) {
            while (!release && !g_stopHook) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (e) *e = "cancelled"; return 1;   // returns Retryable; RunJob must override to Failed on user-cancel
        });
        const std::string C = "CID_USER_CANCEL";
        IpfsWrapper::EnqueueBatch({{C, "/nonexistent/usercancel.bin", false}});
        QTRY_COMPARE_WITH_TIMEOUT(IpfsWrapper::DebugJobState(C), 1 /* Active */, 5000);
        IpfsWrapper::CancelDownload(C);   // active → records UserCancelled + RequestCancel
        release = true;                   // let the attempt return
        QTRY_COMPARE_WITH_TIMEOUT(IpfsWrapper::DebugJobState(C), 3 /* Failed */, 5000);
    }

    // MEDIUM (review): the batch entry point fails LOUD when the node is offline instead of blocking forever on an
    // unbounded wait (no node runs in these tests, so DaemonRunning() is false). Teeth: drop the DaemonRunning gate
    // → this call blocks and the test times out.
    void aBatchFetchFailsLoudlyWhenOffline()
    {
        std::string Err;
        const bool Ok = IpfsWrapper::FetchTargetsConcurrent({{"CID_OFFLINE", "/nonexistent/off.bin", false}}, &Err);
        QVERIFY2(!Ok, "an offline batch fetch must fail, not block forever");
        QVERIFY(!Err.empty());
    }
};

QTEST_MAIN(DownloadQueueTest)
#include "test_downloadqueue.moc"
