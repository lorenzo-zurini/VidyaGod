#include <QtTest/QtTest>
#include <QTemporaryDir>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "downloadqueue.h"
#include "launchsources.h"
#include "launchparams.h"
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

    // Two packages sharing a zip: another request already placed the file at this dest (a hard link of the fetched one).
    // Placing it again — a Verify join re-places even over an existing file — is success: copy_file refuses to copy
    // a file onto itself, and that "File exists" failed a whole download on the replication. Teeth: drop the
    // equivalent() check in Materialize and this batch fails.
    void placingAFileWhereTheSameFileAlreadyIsSucceeds()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const std::string A = (dir.path() + "/a.zip").toStdString();
        const std::string B = (dir.path() + "/other/a.zip").toStdString();
        { std::ofstream(A) << "a zip two packages share"; }
        QVERIFY(IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch({{"CID_DQ_SAME", A, false}})));
        std::filesystem::create_directories(std::filesystem::path(B).parent_path());
        std::filesystem::create_hard_link(A, B);                           // placed already, by the other package
        std::string Err;
        QVERIFY2(IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch({IpfsWrapper::FetchTarget{"CID_DQ_SAME", B, false, false, /*Verify=*/true}}), &Err),
                 Err.c_str());
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

    // A package installed while another download still reads the old index: what that download enqueues for the old
    // CATALOG folder lands in the installed package instead — never re-creating the old folder as a stub (which then
    // failed every later install with "already exists"). A sibling folder that merely starts the same is untouched,
    // and the landed folder's own redirect (its files moved up) wins over the package's. Teeth: drop the redirect in
    // EnqueueBatch (the dest stays under CATALOG), or the longest-prefix choice (.package lands under .package).
    // A file under a moved folder lands where the folder is now; a FOLDER target is never redirected (a folder fetch
    // replaces its destination whole: a received package folder sent onto the installed package deleted the game);
    // the move ends when the package is landed as a stub again. Teeth: redirect Dir targets too and the package folder
    // lands on the installed package; drop DropRedirectsUnder's erase and the re-received stub's file is redirected.
    void aRequestForAMovedFolderLandsWhereItIsNow()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const std::string Cat = (dir.path() + "/CATALOG/Alice - Games/[1] A").toStdString();
        const std::string Lib = (dir.path() + "/LIBRARY/Games/[1] A").toStdString();
        std::filesystem::create_directories(Lib);                             // installed: the stub is gone, the package here
        IpfsWrapper::RedirectDestsUnder(Cat + "/.package", Lib);
        IpfsWrapper::RedirectDestsUnder(Cat, Lib);
        IpfsWrapper::EnqueueBatch({{"CID_DQ_MOVED", Cat + "/n.json", /*Optional=*/true},
                                   {"CID_DQ_LANDED", Cat + "/.package/m.json", /*Optional=*/true},
                                   {"CID_DQ_SIBLING", (dir.path() + "/CATALOG/Alice - Games/[1] AB/s.json").toStdString(), /*Optional=*/true}});
        QCOMPARE(IpfsWrapper::QueueDestForCid("CID_DQ_MOVED"), Lib + "/n.json");
        QCOMPARE(IpfsWrapper::QueueDestForCid("CID_DQ_LANDED"), Lib + "/m.json");
        QCOMPARE(IpfsWrapper::QueueDestForCid("CID_DQ_SIBLING"), (dir.path() + "/CATALOG/Alice - Games/[1] AB/s.json").toStdString());
        IpfsWrapper::EnqueueBatch({IpfsWrapper::FetchTarget{"CID_DQ_FOLDER", Cat + "/.package", /*Optional=*/true, /*Dir=*/true}});
        QCOMPARE(IpfsWrapper::QueueDestForCid("CID_DQ_FOLDER"), Cat + "/.package");      // a folder target: never redirected
        IpfsWrapper::EnqueueBatch({IpfsWrapper::FetchTarget{"CID_DQ_UNDER", Cat + "/.package/sub", /*Optional=*/true, /*Dir=*/true}});
        QCOMPARE(IpfsWrapper::QueueDestForCid("CID_DQ_UNDER"), Cat + "/.package/sub");
        // The installed package deleted, the package landed as a stub again: its moves are over.
        std::filesystem::remove_all(Lib);
        IpfsWrapper::DropRedirectsUnder(Cat);
        IpfsWrapper::EnqueueBatch({{"CID_DQ_AGAIN", Cat + "/.package/n.json", /*Optional=*/true}});
        QCOMPARE(IpfsWrapper::QueueDestForCid("CID_DQ_AGAIN"), Cat + "/.package/n.json");
        IpfsWrapper::DebugResetQueue();
    }

    // The queue compares paths in ONE form, however a caller spelled them: an install that names the moved folder
    // lexically_normal()'d, and a download that built the old path by joining, must meet. On Windows the two differ by
    // separators ("…/CATALOG\\nick" vs "…\\CATALOG\\nick") and the redirect, never matching, let a late download re-create
    // the stub; here the same mismatch is "./" and "//". Teeth: store and compare the paths as given (DestKey = identity)
    // and the dests below keep their CATALOG spelling.
    void aMovedFolderMatchesHoweverThePathIsSpelled()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const std::string Cat = (dir.path() + "/CATALOG/Alice - Games/[1] A").toStdString();
        const std::string Lib = (dir.path() + "/LIBRARY/Games/[1] A").toStdString();
        std::filesystem::create_directories(Lib);
        IpfsWrapper::RedirectDestsUnder((dir.path() + "/CATALOG/./Alice - Games//[1] A").toStdString(), Lib);
        IpfsWrapper::EnqueueBatch({{"CID_DQ_SPELT", Cat + "/n.json", /*Optional=*/true}});
        QCOMPARE(IpfsWrapper::QueueDestForCid("CID_DQ_SPELT"), Lib + "/n.json");
        QCOMPARE(IpfsWrapper::Redirected(Cat + "//m.json"), Lib + "/m.json");
        IpfsWrapper::DebugResetQueue();                                      // no moves: the stub's file stays in CATALOG
        IpfsWrapper::EnqueueBatch({{"CID_DQ_STUB", Cat + "/s.json", /*Optional=*/true}});
        IpfsWrapper::ForgetDestsUnder((dir.path() + "/CATALOG/Alice - Games/./[1] A/").toStdString());
        QCOMPARE(IpfsWrapper::QueueDestForCid("CID_DQ_STUB"), std::string());          // forgotten, spelt otherwise
        IpfsWrapper::DebugResetQueue();
    }

    // A job that fetches in waves holds the network queue in foreground mode across them (background work: one
    // slot), and lets go when it ends; holds nest. Teeth: construct ForegroundHold without VgNetHold and the queue
    // still reads idle inside the hold.
    void aForegroundHoldKeepsTheQueueBusyUntilReleased()
    {
        QVERIFY2(IpfsWrapper::DebugNetForegroundIdle(), "precondition: nothing downloading");
        {
            const IpfsWrapper::ForegroundHold Outer;
            QVERIFY(!IpfsWrapper::DebugNetForegroundIdle());
            { const IpfsWrapper::ForegroundHold Inner; }
            QVERIFY2(!IpfsWrapper::DebugNetForegroundIdle(), "the outer hold still stands");
        }
        QVERIFY2(IpfsWrapper::DebugNetForegroundIdle(), "released");
    }

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
            if (e) *e = "no providers";
            return 1; /* Retryable */ });
        const std::string C = "CID_ROLL_STALL";
        IpfsWrapper::EnqueueBatch({{C, "/nonexistent/roll_stall.bin", true}});
        QTRY_VERIFY_WITH_TIMEOUT(IpfsWrapper::DebugJobBackingOff(C), 5000);   // rotated to Queued + backing off
        QVERIFY2(IpfsWrapper::DebugJobState(C) != 3, "a stalled attempt must never be Failed");
        IpfsWrapper::CancelDownload(C);                                        // stop it before the next test
    }

    // Paused (networking off), a queued job is not attempted — no retry every 30 s against a stopped node; resumed, it
    // runs at once. Teeth: PickReady ignoring the pause (the hook is called while paused).
    void aPausedQueueAttemptsNothingAndResumesAtOnce()
    {
        std::atomic<int> Calls{0};
        IpfsWrapper::SetFetchOnceHook([&Calls](const std::string &, const std::string &, bool, std::string *e) {
            ++Calls;
            if (e) *e = "no providers";
            return 1; /* Retryable */ });
        IpfsWrapper::SetQueuePaused(true);
        const std::string C = "CID_PAUSED";
        IpfsWrapper::EnqueueBatch({{C, "/nonexistent/paused.bin", true}});
        QTest::qWait(600);
        const int WhilePaused = Calls.load();
        IpfsWrapper::SetQueuePaused(false);
        QTRY_VERIFY_WITH_TIMEOUT(Calls.load() > 0, 3000);
        IpfsWrapper::CancelDownload(C);
        IpfsWrapper::SetFetchOnceHook({});
        QCOMPARE(WhilePaused, 0);
    }

    // A TERMINAL attempt (cancel / bad CID / disk) fails the job — no endless rotation. Teeth: RunJob rotating a
    // terminal rc → never reaches Failed.
    void aTerminalAttemptFailsTheJob()
    {
        IpfsWrapper::SetFetchOnceHook([](const std::string &, const std::string &, bool, std::string *e) {
            if (e) *e = "malformed CID";
            return 2; /* Terminal */ });
        const std::string C = "CID_ROLL_TERMINAL";
        IpfsWrapper::EnqueueBatch({{C, "/nonexistent/roll_term.bin", true}});
        QTRY_COMPARE_WITH_TIMEOUT(IpfsWrapper::DebugJobState(C), 3 /* Failed */, 5000);
    }

    // A folder fetch still in flight when its stub dir is dropped (the package was adopted, or the peer withdrew it)
    // must not re-create the stub when it lands: the worker holds its own copy of the dests. Teeth: stop
    // ForgetDestsUnder recording active jobs' dests (or RunJob undoing them) and the dropped dir comes back.
    void aFolderLandingAfterItsDirWasDroppedIsUndone()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const std::string Stub = (dir.path() + "/CATALOG/Alice - Games/[1] Game").toStdString();
        const std::string Dest = Stub + "/.package";
        std::filesystem::create_directories(Stub);
        std::atomic<bool> Started{false}, Release{false};
        IpfsWrapper::SetFetchOnceHook([&](const std::string &, const std::string &dest, bool, std::string *) {
            Started = true;
            while (!Release && !g_stopHook) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            std::filesystem::create_directories(dest);                 // like Go: MkdirAll + land the tree
            std::ofstream(dest + "/n.json") << "{}";
            return 0; });
        g_stopHook = false;
        auto H = IpfsWrapper::EnqueueBatch({{"CID_DQ_LATE_DIR", Dest, false, /*Dir=*/true}});
        QTRY_VERIFY_WITH_TIMEOUT(Started.load(), 5000);
        std::filesystem::remove_all(dir.path().toStdString() + "/CATALOG/Alice - Games");   // the caller drops the stub
        IpfsWrapper::ForgetDestsUnder(dir.path().toStdString() + "/CATALOG/Alice - Games");
        Release = true;
        IpfsWrapper::WaitBatch(H, 5000);
        std::error_code Ec;
        QVERIFY2(!std::filesystem::exists(dir.path().toStdString() + "/CATALOG/Alice - Games", Ec), "the dropped dir stays dropped");
        QVERIFY2(std::filesystem::exists(dir.path().toStdString() + "/CATALOG", Ec), "nothing above the dropped dir is touched");
    }

    // A dest belongs to ONE CID. A node path is reused across generations (same label, new CID), and a job
    // materialises every dest it remembers — so when a NEW cid claims a path, the OLD cid's job must forget it, or
    // its next run rewrites the path with the old block (three "Vanilla" files, all the Age of Kings block).
    // Enqueueing again what the queue already holds (a resync of a received library: thousands of node files, each
    // already this CID's at this path) is quick: claiming a path that is already the CID's scanned every job, per
    // target, under the queue's lock, on the GUI thread — seconds on a handheld every 10 minutes. Teeth: claim it
    // again (a scan of every job per target).
    void enqueueingWhatIsHereAgainIsQuick()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        IpfsWrapper::DebugResetQueue();
        IpfsWrapper::SetFetchOnceHook([](const std::string &cid, const std::string &dest, bool, std::string *) {
            std::ofstream(dest) << cid; return 0; });
        std::vector<IpfsWrapper::FetchTarget> T;
        for (int I = 0; I < 6000; ++I)
            T.push_back({ "CID_RESYNC_" + std::to_string(I), (dir.path() + "/n" + QString::number(I) + ".json").toStdString(), true, false, true });
        QVERIFY(IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch(T), 60000));
        const long long Scans = IpfsWrapper::DebugClaimScans();
        const auto H = IpfsWrapper::EnqueueBatch(T);
        QVERIFY(IpfsWrapper::WaitBatch(H, 60000));
        QCOMPARE(IpfsWrapper::DebugClaimScans() - Scans, 0LL);                    // no scan of every job, per target
        IpfsWrapper::SetFetchOnceHook(nullptr);
        IpfsWrapper::DebugResetQueue();
    }

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

    // A wait goes on from where it was at each wake of the queue, not over its whole batch again: 200 files landing
    // one by one cost a wait 200 looks at 200 items (each wake, under the lock every fetch takes). A required item
    // failing still ends the wait at once, wherever it is in the batch. Teeth: start each wake from the first item
    // (the looks grow with the square); drop the look again on a failure (the failure is seen only when the wait
    // reaches it — never, behind an item that does not settle).
    void a_wait_looks_at_what_is_left_and_still_sees_a_failure()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        IpfsWrapper::SetFetchOnceHook([](const std::string &cid, const std::string &dest, bool, std::string *E) {
            if (cid == "CID_WL_BAD") { if (E) *E = "refused"; return 2; }
            if (cid == "CID_WL_SLOW") { std::this_thread::sleep_for(std::chrono::milliseconds(800)); }
            std::ofstream(dest) << cid; return 0; });
        std::vector<IpfsWrapper::FetchTarget> T;
        for (int I = 0; I < 200; ++I) T.push_back({"CID_WL_" + std::to_string(I), dir.path().toStdString() + "/f" + std::to_string(I), false});
        const long long Before = IpfsWrapper::DebugWaitLooks();
        QVERIFY(IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch(T), 20000));
        const long long Looks = IpfsWrapper::DebugWaitLooks() - Before;
        QVERIFY2(Looks < 200 * 20, std::to_string(Looks).c_str());
        // A slow first item, a required one failing behind it: the wait ends with the failure, not after the slow one.
        IpfsWrapper::DebugResetQueue();
        std::string Err;
        const auto T0 = std::chrono::steady_clock::now();
        const bool Ok = IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch({{"CID_WL_SLOW", (dir.path() + "/slow").toStdString(), false},
                                                                         {"CID_WL_BAD", (dir.path() + "/bad").toStdString(), false}}), 20000, &Err);
        QVERIFY(!Ok);
        QVERIFY2(Err.find("CID_WL_BAD") != std::string::npos, Err.c_str());
        QVERIFY2(std::chrono::steady_clock::now() - T0 < std::chrono::milliseconds(700), "the failure ended the wait before the slow item settled");
        IpfsWrapper::SetFetchOnceHook({});
        IpfsWrapper::DebugResetQueue();
    }

    // An item a wait already passed as done that is asked for again (its file gone, a new copy wanted) is fetching again:
    // the wait does not end until it lands (a wait that never looked back said a batch was in place over a file still
    // on its way). Teeth: re-check only from where the wait was (it ends at once, the item still fetching).
    void a_wait_sees_an_item_it_passed_fetching_again()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const std::string A = dir.path().toStdString() + "/a.bin", A2 = dir.path().toStdString() + "/a2.bin", B = dir.path().toStdString() + "/b.bin";
        std::atomic<bool> HoldA{false}, HoldB{true};
        g_stopHook = false;
        IpfsWrapper::SetFetchOnceHook([&](const std::string &cid, const std::string &dest, bool, std::string *) {
            while (((cid == "CID_RQ_A" && HoldA) || (cid == "CID_RQ_B" && HoldB)) && !g_stopHook) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            std::ofstream(dest) << cid; return 0; });
        const auto H = IpfsWrapper::EnqueueBatch({{"CID_RQ_A", A, false}, {"CID_RQ_B", B, false}});
        QTRY_COMPARE_WITH_TIMEOUT(IpfsWrapper::DebugJobState("CID_RQ_A"), 2, 5000);     // A done; B held
        std::atomic<int> Result{-1};
        std::thread W([&] { Result = IpfsWrapper::WaitBatch(H, 10000) ? 1 : 0; });
        QTest::qWait(200);                                                               // the wait has passed A
        HoldA = true;
        std::filesystem::remove(A);
        IpfsWrapper::EnqueueBatch({{"CID_RQ_A", A2, false}});                            // no copy left: fetched again
        QTRY_VERIFY_WITH_TIMEOUT(IpfsWrapper::DebugJobState("CID_RQ_A") != 2, 5000);
        HoldB = false;                                                                   // B lands
        QTest::qWait(500);
        QCOMPARE(Result.load(), -1);                                                     // still waiting for A
        HoldA = false;
        W.join();
        QCOMPARE(Result.load(), 1);
        g_stopHook = true;
        IpfsWrapper::SetFetchOnceHook({});
        IpfsWrapper::DebugResetQueue();
    }

    // A wait on a stall (landing every received package's files at once) goes on while files keep landing, however
    // long it all takes, and gives up once none has for the stall time; an optional file that failed says why.
    // Teeth: a fixed deadline (the steady landing is cut off); slide it at any wake, not a settle (the held file waits
    // for good); FailureOf saying nothing for a failed job.
    void a_stall_wait_goes_on_while_files_land()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        g_stopHook = false;
        IpfsWrapper::SetFetchOnceHook([](const std::string &cid, const std::string &dest, bool, std::string *E) {
            if (cid == "CID_SW_BAD") { if (E) *E = "refused by the test"; return 2; }
            if (cid == "CID_SW_HELD") { while (!g_stopHook) std::this_thread::sleep_for(std::chrono::milliseconds(5)); return 1; }
            if (cid.rfind("CID_SW_NOISE", 0) == 0) return 1;                      // a stall: rotates, never settles
            if (cid == "CID_SW_AWAY") return 1;                                    // its peer away: tried, failing, retried
            if (cid.rfind("CID_SW_LAND", 0) == 0) { std::ofstream(dest) << cid; return 0; }   // someone else's install
            const int K = cid.back() - '0';
            std::this_thread::sleep_for(std::chrono::milliseconds(250 * K));
            std::ofstream(dest) << cid; return 0; });
        std::vector<IpfsWrapper::FetchTarget> T;
        for (int K = 1; K <= 6; ++K) T.push_back({"CID_SW_" + std::to_string(K), dir.path().toStdString() + "/f" + std::to_string(K), true});
        T.push_back({"CID_SW_BAD", dir.path().toStdString() + "/bad", true});
        std::string Err;
        const auto T0 = std::chrono::steady_clock::now();
        QVERIFY2(IpfsWrapper::WaitBatchWhileMoving(IpfsWrapper::EnqueueBatch(T), 700, &Err), Err.c_str());
        QVERIFY2(std::chrono::steady_clock::now() - T0 > std::chrono::milliseconds(1200), "the landing took longer than one stall");
        QCOMPARE(IpfsWrapper::FailureOf("CID_SW_BAD"), std::string("refused by the test"));
        QCOMPARE(IpfsWrapper::FailureOf("CID_SW_1"), std::string());
        // Nothing lands, the queue busy meanwhile (fetches that stall and rotate wake every wait): given up after the
        // stall, the job left queued.
        const auto T1 = std::chrono::steady_clock::now();
        const auto Held = IpfsWrapper::EnqueueBatch({{"CID_SW_HELD", dir.path().toStdString() + "/held", true}});
        std::atomic<int> Result{-1};
        std::thread W([&] { Result = IpfsWrapper::WaitBatchWhileMoving(Held, 700, &Err) ? 1 : 0; });
        for (int I = 0; Result == -1 && I < 40; ++I)
        {
            IpfsWrapper::EnqueueBatch({{"CID_SW_NOISE" + std::to_string(I), dir.path().toStdString() + "/n" + std::to_string(I), true}});
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        g_stopHook = true;
        W.join();
        QCOMPARE(Result.load(), 0);
        QVERIFY2(Err.find("none of it landed") != std::string::npos, Err.c_str());
        QVERIFY2(std::chrono::steady_clock::now() - T1 < std::chrono::milliseconds(3000), "a stalled wait ends");
        // Its last file keeps failing while another batch lands file after file: the wait ends after the stall all the
        // same (the other batch's files are not this one moving).
        g_stopHook = false;
        const auto T2 = std::chrono::steady_clock::now();
        const auto Away = IpfsWrapper::EnqueueBatch({{"CID_SW_AWAY", dir.path().toStdString() + "/away", true}});
        std::atomic<int> Result2{-1};
        std::string Err2;
        std::thread W2([&] { Result2 = IpfsWrapper::WaitBatchWhileMoving(Away, 700, &Err2) ? 1 : 0; });
        for (int I = 0; Result2 == -1 && I < 40; ++I)
        {
            IpfsWrapper::EnqueueBatch({{"CID_SW_LAND" + std::to_string(I), dir.path().toStdString() + "/l" + std::to_string(I), true}});
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        W2.join();
        QCOMPARE(Result2.load(), 0);
        QVERIFY2(std::chrono::steady_clock::now() - T2 < std::chrono::milliseconds(3000), "the other batch landing does not keep it waiting");
        g_stopHook = true;
        IpfsWrapper::SetFetchOnceHook({});
        IpfsWrapper::DebugResetQueue();
    }

    // A stall wait counts its own batch moving the two ways the queue's other work does not: one of its files landing on
    // a retry (tried before, then had), and one still waiting its first turn behind the other work. Teeth: count only
    // the wait's first unsettled item reached (the retried files land after it gives up); count no file waiting its
    // turn (it gives up before its turn).
    void a_stall_wait_counts_its_own_files_landing_late_and_waiting_their_turn()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        IpfsWrapper::DebugResetQueue();
        std::mutex Mu; std::map<std::string, int> Tries;
        IpfsWrapper::SetFetchOnceHook([&](const std::string &cid, const std::string &dest, bool, std::string *) {
            int T; { std::lock_guard<std::mutex> g(Mu); T = ++Tries[cid]; }
            if (cid == "CID_SL_AWAY") return 1;                                   // never had
            if (cid.rfind("CID_SL_LATE", 0) == 0) {                               // had at the second try, one after another
                if (T == 1) return 1;
                std::this_thread::sleep_for(std::chrono::milliseconds(400 * (cid.back() - '0')));
            }
            if (cid.rfind("CID_SL_OTHER", 0) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(300));
            std::ofstream(dest) << cid; return 0; });
        // Its first file never had; the others had at their retry, 2 s on and ~0.4 s apart — later than one stall.
        std::vector<IpfsWrapper::FetchTarget> T{{"CID_SL_AWAY", dir.path().toStdString() + "/away", true}};
        for (int K = 1; K <= 6; ++K) T.push_back({"CID_SL_LATE" + std::to_string(K), dir.path().toStdString() + "/late" + std::to_string(K), true});
        QVERIFY(!IpfsWrapper::WaitBatchWhileMoving(IpfsWrapper::EnqueueBatch(T), 3000));
        for (int K = 1; K <= 6; ++K)
            QVERIFY2(std::filesystem::exists(dir.path().toStdString() + "/late" + std::to_string(K)), std::to_string(K).c_str());
        IpfsWrapper::DebugResetQueue();
        // One slot, another batch's eight files in it one after another; this batch's file waits its turn behind them.
        const int SavedMax = IpfsWrapper::MaxConcurrentDownloads();
        IpfsWrapper::SetMaxConcurrentDownloads(1);
        std::vector<IpfsWrapper::FetchTarget> Other;
        for (int I = 0; I < 8; ++I) Other.push_back({"CID_SL_OTHER" + std::to_string(I), dir.path().toStdString() + "/o" + std::to_string(I), true});
        IpfsWrapper::EnqueueBatch(Other);
        std::string Err;
        const bool Ok = IpfsWrapper::WaitBatchWhileMoving(IpfsWrapper::EnqueueBatch({{"CID_SL_MINE", dir.path().toStdString() + "/mine", true}}), 700, &Err);
        IpfsWrapper::SetMaxConcurrentDownloads(SavedMax);
        QVERIFY2(Ok, Err.c_str());
        IpfsWrapper::SetFetchOnceHook({});
        IpfsWrapper::DebugResetQueue();
    }

    // Many covers prioritized together wake the queue once, not once each (each wake was every wait's look at its whole
    // batch, and the dispatcher's at every job). Teeth: prioritize one by one in PrioritizeDownloads.
    void many_prioritized_together_wake_the_queue_once()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        g_stopHook = false;
        IpfsWrapper::SetFetchOnceHook([&](const std::string &, const std::string &, bool, std::string *) {
            while (!g_stopHook) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return 1; });
        std::vector<IpfsWrapper::FetchTarget> T;
        std::vector<std::string> Cids;
        for (int I = 0; I < 50; ++I) { Cids.push_back("CID_PW_" + std::to_string(I)); T.push_back({Cids.back(), dir.path().toStdString() + "/c" + std::to_string(I), true}); }
        IpfsWrapper::EnqueueBatch(T);
        const long long Before = IpfsWrapper::DebugPrioritizeWakes();
        IpfsWrapper::PrioritizeDownloads(Cids);
        QCOMPARE(IpfsWrapper::DebugPrioritizeWakes() - Before, 1LL);
        QVERIFY(IpfsWrapper::DebugJobPrioritized(Cids.back()));
        g_stopHook = true;
        IpfsWrapper::SetFetchOnceHook({});
        IpfsWrapper::DebugResetQueue();
    }

    // A launch missing several of its layers fetches them together, put at the front together: one at a time, each
    // waited for in turn, its wait was every layer's fetch time added up. One that cannot be had fails the launch, and
    // the others still land. Teeth: fetch them one by one (the second only starts when the first has landed); make
    // them required (the failed one ends the wait before the other lands).
    void a_launch_fetches_its_missing_layers_together()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const std::string Pkg = dir.path().toStdString();
        std::atomic<bool> BLanded{false}, ASawB{false}, FailA{false};
        IpfsWrapper::SetFetchOnceHook([&](const std::string &cid, const std::string &dest, bool, std::string *E) {
            if (cid == "CID_ML_A")
            {
                if (FailA) { if (E) *E = "nobody has it"; return 2; }
                for (int I = 0; I < 300 && !BLanded; ++I) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                ASawB = BLanded.load();
            }
            else std::this_thread::sleep_for(std::chrono::milliseconds(200));
            std::filesystem::create_directories(std::filesystem::path(dest).parent_path());
            std::ofstream(dest) << cid;
            if (cid == "CID_ML_B") BLanded = true;
            return 0; });
        ContainerParams P(Pkg);
        const auto Layer = [](const std::string &Path, const std::string &Cid) {
            return nlohmann::ordered_json{{"TYPE", "VFSZipLayer"}, {"PATH", Path}, {"SOURCE", {{"TYPE", "ipfs"}, {"CID", Cid}}}};
        };
        P.SubComponentsArray = nlohmann::ordered_json::array({ Layer("a.zip", "CID_ML_A"), Layer("b.zip", "CID_ML_B") });
        QVERIFY(LaunchSources::MaterializeLayers(P));
        QVERIFY2(ASawB, "the second layer landed while the first was on its way");
        QVERIFY(std::filesystem::exists(Pkg + "/a.zip") && std::filesystem::exists(Pkg + "/b.zip"));
        // The first cannot be had: the launch fails, the other lands.
        std::filesystem::remove(Pkg + "/a.zip"); std::filesystem::remove(Pkg + "/b.zip");
        IpfsWrapper::DebugResetQueue();
        FailA = true;
        QVERIFY(!LaunchSources::MaterializeLayers(P));
        QVERIFY2(std::filesystem::exists(Pkg + "/b.zip"), "the other layer landed");
        IpfsWrapper::SetFetchOnceHook({});
        IpfsWrapper::DebugResetQueue();
    }

    // A launch missing a layer while a bulk download holds every slot preempts it: the layer is required in the queue
    // (an optional job never preempts), though one that cannot be had does not end the wait for the others. Teeth:
    // enqueue the layers optional (the layer waits behind the bulk download; the 120 s wait would run out).
    void a_launchs_missing_layer_preempts_a_bulk_download()
    {
        g_stopHook = false;
        const int SavedMax = IpfsWrapper::MaxConcurrentDownloads();
        IpfsWrapper::SetMaxConcurrentDownloads(1);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        IpfsWrapper::SetFetchOnceHook([](const std::string &cid, const std::string &dest, bool, std::string *) {
            if (cid == "CID_BULK") {
                while (!IpfsWrapper::DebugIsPreempting("CID_BULK") && !g_stopHook) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                return 1;
            }
            std::filesystem::create_directories(std::filesystem::path(dest).parent_path());
            std::ofstream(dest) << cid; return 0; });
        IpfsWrapper::EnqueueBatch({{"CID_BULK", dir.path().toStdString() + "/bulk.bin", true}});
        QTRY_COMPARE_WITH_TIMEOUT(IpfsWrapper::DebugJobState("CID_BULK"), 1, 5000);
        ContainerParams P(dir.path().toStdString());
        P.SubComponentsArray = nlohmann::ordered_json::array({ nlohmann::ordered_json{{"TYPE", "VFSZipLayer"}, {"PATH", "layer.zip"},
                                                               {"SOURCE", {{"TYPE", "ipfs"}, {"CID", "CID_LAYER"}}}} });
        std::atomic<bool> Done{false}, Ok{false};
        std::thread T([&] { Ok = LaunchSources::MaterializeLayers(P); Done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(Done.load(), 10000);
        T.join();
        QVERIFY(Ok.load());
        QVERIFY(std::filesystem::exists(dir.path().toStdString() + "/layer.zip"));
        g_stopHook = true;
        IpfsWrapper::CancelDownload("CID_BULK");
        IpfsWrapper::SetFetchOnceHook({});
        IpfsWrapper::SetMaxConcurrentDownloads(SavedMax);
        IpfsWrapper::DebugResetQueue();
    }

    // Several missing layers prioritized together each take a slot of their own: every bulk download in a slot is
    // preempted, not the same one again for each. Teeth: pick a victim already being preempted.
    void several_required_layers_preempt_a_download_each()
    {
        g_stopHook = false;
        const int SavedMax = IpfsWrapper::MaxConcurrentDownloads();
        IpfsWrapper::SetMaxConcurrentDownloads(2);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        IpfsWrapper::SetFetchOnceHook([](const std::string &cid, const std::string &dest, bool, std::string *) {
            if (cid.rfind("CID_PB", 0) == 0) {
                while (!IpfsWrapper::DebugIsPreempting(cid) && !g_stopHook) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                return 1;
            }
            while (!g_stopHook) std::this_thread::sleep_for(std::chrono::milliseconds(10));   // the layers: held in their slots
            std::ofstream(dest) << cid; return 0; });
        IpfsWrapper::EnqueueBatch({{"CID_PB1", dir.path().toStdString() + "/b1", true}, {"CID_PB2", dir.path().toStdString() + "/b2", true}});
        QTRY_VERIFY_WITH_TIMEOUT(IpfsWrapper::DebugJobState("CID_PB1") == 1 && IpfsWrapper::DebugJobState("CID_PB2") == 1, 5000);
        IpfsWrapper::EnqueueBatch({{"CID_PL1", dir.path().toStdString() + "/l1", false}, {"CID_PL2", dir.path().toStdString() + "/l2", false}});
        IpfsWrapper::PrioritizeDownloads({"CID_PL1", "CID_PL2"});
        QTRY_VERIFY_WITH_TIMEOUT(IpfsWrapper::DebugJobState("CID_PL1") == 1 && IpfsWrapper::DebugJobState("CID_PL2") == 1, 5000);
        g_stopHook = true;
        IpfsWrapper::SetFetchOnceHook({});
        IpfsWrapper::SetMaxConcurrentDownloads(SavedMax);
        IpfsWrapper::DebugResetQueue();
    }

    // Many folders forgotten at once (a peer's library dropped: hundreds of package folders) forget every dest under
    // any of them, in one pass over the jobs, and nothing under a sibling whose name merely starts the same. Teeth:
    // match the folder without its "/" (the sibling's dest goes too); forget only the first folder's.
    void dests_under_many_folders_are_forgotten_at_once()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const std::string Root = dir.path().toStdString() + "/CATALOG/Alice - Games";
        g_stopHook = false;
        IpfsWrapper::SetFetchOnceHook([&](const std::string &, const std::string &, bool, std::string *) {
            while (!g_stopHook) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return 1; });
        IpfsWrapper::EnqueueBatch({{"CID_FM_A", Root + "/[1] A/a.json", true}, {"CID_FM_AB", Root + "/[1] AB/b.json", true},
                                   {"CID_FM_C", Root + "/[3] C/sub/c.json", true}});
        IpfsWrapper::ForgetDestsUnder(std::vector<std::string>{ Root + "/[1] A", Root + "/[3] C" });
        QCOMPARE(IpfsWrapper::QueueDestForCid("CID_FM_A"), std::string());
        QCOMPARE(IpfsWrapper::QueueDestForCid("CID_FM_AB"), Root + "/[1] AB/b.json");
        QCOMPARE(IpfsWrapper::QueueDestForCid("CID_FM_C"), std::string());
        g_stopHook = true;
        IpfsWrapper::SetFetchOnceHook({});
        IpfsWrapper::DebugResetQueue();
    }

    // A path claimed by a new CID while the old CID's fetch is in flight: the old worker still lands it (it holds its
    // own copy of the dests), over the new node's file. The path is the new node's: the late landing is undone by
    // running the new node's finished job again. Teeth: stop ClaimDest recording an active job's dest (the old bytes
    // stay); remove the late file instead of re-running the owner (the path ends empty); skip the re-run (old bytes).
    void aPathClaimedWhileTheOldFetchRunsEndsWithTheNewNode()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const std::string X = (dir.path() + "/Vanilla.json").toStdString();
        std::atomic<bool> Started{false}, Release{false};
        g_stopHook = false;
        IpfsWrapper::SetFetchOnceHook([&](const std::string &cid, const std::string &dest, bool, std::string *) {
            if (cid == "CID_CLAIM_OLD") { Started = true; while (!Release && !g_stopHook) std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
            std::ofstream(dest, std::ios::trunc) << cid; return 0; });
        auto Read = [](const std::string &P) { std::ifstream I(P); std::string S; std::getline(I, S); return S; };
        auto Old = IpfsWrapper::EnqueueBatch({{"CID_CLAIM_OLD", X, false, false, true}});
        QTRY_VERIFY_WITH_TIMEOUT(Started.load(), 5000);
        QVERIFY(IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch({{"CID_CLAIM_NEW", X, false, false, true}}), 5000));
        QCOMPARE(Read(X), std::string("CID_CLAIM_NEW"));
        Release = true;
        IpfsWrapper::WaitBatch(Old, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(Read(X), std::string("CID_CLAIM_NEW"), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(IpfsWrapper::DebugJobState("CID_CLAIM_NEW"), 2 /* Done */, 5000);
        QCOMPARE(Read(X), std::string("CID_CLAIM_NEW"));
    }

    // One file forgotten from a fetch in flight (a peer's package no longer names it there): the worker's late
    // landing of it is undone, and nothing else — its folder and the fetch's other dest stay. Teeth: stop ForgetDest
    // recording an active job's dest (the file stays).
    void aFileForgottenWhileItsFetchRunsIsUndone()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const std::string Pkg = (dir.path() + "/Game").toStdString();
        std::filesystem::create_directories(Pkg);
        const std::string A = Pkg + "/a.json", B = Pkg + "/b.json";
        std::atomic<bool> Started{false}, Release{false};
        g_stopHook = false;
        IpfsWrapper::SetFetchOnceHook([&](const std::string &cid, const std::string &dest, bool, std::string *) {
            Started = true;
            while (!Release && !g_stopHook) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            std::ofstream(dest, std::ios::trunc) << cid; return 0; });
        auto H = IpfsWrapper::EnqueueBatch({{"CID_FORGET_ONE", A, false}, {"CID_FORGET_ONE", B, false}});
        QTRY_VERIFY_WITH_TIMEOUT(Started.load(), 5000);
        IpfsWrapper::ForgetDest("CID_FORGET_ONE", B);
        Release = true;
        IpfsWrapper::WaitBatch(H, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(IpfsWrapper::DebugJobState("CID_FORGET_ONE"), 2 /* Done */, 5000);
        std::error_code Ec;
        QVERIFY(std::filesystem::exists(A, Ec));
        QVERIFY2(!std::filesystem::exists(B, Ec), "a forgotten file landed late and stayed");
        QVERIFY(std::filesystem::is_directory(Pkg, Ec));
    }

    // A dest that cannot be hashed (unreadable) is not known to hold another node: it is never removed as stale. (The
    // node going down made the hash empty, and a correct node file was deleted.) Teeth: treat an empty hash as stale.
    void anUnhashableDestIsNeverRemovedAsStale()
    {
#ifndef _WIN32
        if (::geteuid() == 0) QSKIP("root reads every file");
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const std::string F = (dir.path() + "/a.json").toStdString();
        { std::ofstream(F) << "{}"; }
        std::filesystem::permissions(F, std::filesystem::perms::none);
        IpfsWrapper::ClearStaleDest(F, "bafkreihdwdcefgh4dqkjv67uzcmw7ojee6xedzdetojuzjevtenxquvyku");
        std::error_code Ec;
        const bool Kept = std::filesystem::exists(F, Ec);
        std::filesystem::permissions(F, std::filesystem::perms::owner_all, Ec);
        QVERIFY2(Kept, "an unreadable dest was removed as stale");
#endif
    }

    // Forgetting a directory's dests: after a peer's stubs were removed on purpose, a later run of a job that
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
            if (e) *e = "no providers";
            return 1; /* forever Retryable */ });
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
            if (e) *e = "cancelled";
            return 1;   // returns Retryable; RunJob must override to Failed on user-cancel
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
