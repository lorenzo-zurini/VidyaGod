// test_covercache.cpp — covers ride the ONE DownloadQueue, and PAINT NEVER FETCHES.
//
// The old design (detached thread per cover + 30 s deadline + negative cache) is gone; these tests pin the
// replacement's contracts:
//   * resolve() is a pure disk check — a miss paints blank and only RECORDS (cid,dest); nothing is enqueued
//     from the paint path (teeth: enqueue inside resolve() → the no-enqueue spy sees a queue event early);
//   * the SWEEP batches every recorded miss into the DownloadQueue when online, and skips entirely while
//     offline (teeth: drop the OnlineProbe gate → case B sees an enqueue while offline);
//   * a finished cover fires coverReady(cid) so surfaces repaint; a FAILED cover stays recorded and rides the
//     next sweep — retry forever, batched, no negative cache (teeth: drop the miss on failure → case D's
//     re-sweep enqueues nothing).

#include "covercache.h"
#include "downloadqueue.h"
#include "ipfswrapper.h"

#include <QtTest>
#include <QTimer>

extern "C" void IpfsNodeTransferCb(const char *cid, int kind, double percent, int ok, const char *err);
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QDir>

#include <fstream>
#include <mutex>
#include <vector>

namespace {
// Capture the queue's "new job queued" callback (global — install per test, restore after).
struct QueueSpy {
    std::mutex Mu;
    std::vector<std::string> Queued;
    QueueSpy()  { IpfsWrapper::SetQueueCallback([this](const std::string &Cid, bool Q){ if (Q) { std::lock_guard<std::mutex> Lk(Mu); Queued.push_back(Cid); } }); }
    ~QueueSpy() { IpfsWrapper::SetQueueCallback({}); }
    bool Saw(const std::string &Cid) { std::lock_guard<std::mutex> Lk(Mu); for (const auto &C : Queued) if (C == Cid) return true; return false; }
    int  Count() { std::lock_guard<std::mutex> Lk(Mu); return (int)Queued.size(); }
};

nlohmann::ordered_json CoverJson(const std::string &Cid)
{
    return { {"PATH", "cover.png"}, {"SOURCE", {{"TYPE", "ipfs"}, {"CID", Cid}}} };
}
} // namespace

class TestCoverCache : public QObject
{
    Q_OBJECT
private slots:
    // A cover already on disk resolves with ZERO side effects; a missing one paints blank and records the miss —
    // and neither touches the queue from the paint path.
    void resolve_is_a_pure_disk_check()
    {
        auto * cc = CoverCache::instance();
        cc->setOnlineProbe([]{ return true; });
        QueueSpy Spy;

        QTemporaryDir pkg; QVERIFY(pkg.isValid());
        { std::ofstream((pkg.path() + "/cover.png").toStdString()) << "png"; }
        const int Before = cc->missCount();
        QVERIFY(!cc->resolve(CoverJson("CID_COVER_PRESENT"), pkg.path()).isEmpty());
        QCOMPARE(cc->missCount(), Before);                     // a hit records nothing

        QTemporaryDir miss; QVERIFY(miss.isValid());
        QVERIFY(cc->resolve(CoverJson("CID_COVER_MISS_A"), miss.path()).isEmpty());   // blank tile
        QCOMPARE(cc->missCount(), Before + 1);                 // …but the miss is remembered for the sweep
        QCOMPARE(Spy.Count(), 0);                              // and PAINT ENQUEUED NOTHING
    }

    // The sweep is the only enqueue path: offline it does nothing at all; online it batches the recorded misses.
    void sweep_batches_misses_online_and_skips_offline()
    {
        auto * cc = CoverCache::instance();
        QTemporaryDir miss; QVERIFY(miss.isValid());

        cc->setOnlineProbe([]{ return false; });
        QVERIFY(cc->resolve(CoverJson("CID_COVER_SWEEP"), miss.path()).isEmpty());
        {
            QueueSpy Spy;
            cc->sweepNow();
            QCOMPARE(Spy.Count(), 0);                          // OFFLINE: no enqueue (it would fail terminally)
        }
        cc->setOnlineProbe([]{ return true; });
        {
            QueueSpy Spy;
            cc->sweepNow();
            QVERIFY2(Spy.Saw("CID_COVER_SWEEP"), "the online sweep must enqueue the recorded miss");
            // Covers are ORDINARY items, bumped to the front on add (small, on-screen). The rolling scheduler — not a
            // cover-specific bound — frees the slot when a dead cover stalls. Teeth: drop the PrioritizeDownload → false.
            QVERIFY2(IpfsWrapper::DebugJobPrioritized("CID_COVER_SWEEP"), "a swept cover must be bumped to the front");
        }
    }

    // Completion: a finished cover fires coverReady so surfaces re-resolve; foreign CIDs are ignored.
    void a_finished_cover_fires_coverReady()
    {
        auto * cc = CoverCache::instance();
        cc->setOnlineProbe([]{ return true; });
        QTemporaryDir miss; QVERIFY(miss.isValid());
        QVERIFY(cc->resolve(CoverJson("CID_COVER_DONE"), miss.path()).isEmpty());

        QSignalSpy Ready(cc, &CoverCache::coverReady);
        cc->onTransferFinished("CID_NOT_A_COVER", true, {});
        QCOMPARE(Ready.count(), 0);                            // not ours → no repaint churn
        cc->onTransferFinished("CID_COVER_DONE", true, {});
        QCOMPARE(Ready.count(), 1);
        QCOMPARE(Ready.at(0).at(0).toString(), QString("CID_COVER_DONE"));
    }

    // Failure is NOT terminal and NOT cached: the miss survives and the next sweep re-enqueues it (the queue
    // requeues a Failed CID). This is the retry-forever the negative cache used to prevent.
    void a_failed_cover_stays_recorded_and_rides_the_next_sweep()
    {
        auto * cc = CoverCache::instance();
        cc->setOnlineProbe([]{ return true; });
        QTemporaryDir miss; QVERIFY(miss.isValid());
        QVERIFY(cc->resolve(CoverJson("CID_COVER_RETRY"), miss.path()).isEmpty());
        const int Have = cc->missCount();

        QSignalSpy Ready(cc, &CoverCache::coverReady);
        cc->onTransferFinished("CID_COVER_RETRY", /*Ok=*/false, "no providers");
        QCOMPARE(Ready.count(), 0);                            // no art landed → no repaint
        QCOMPARE(cc->missCount(), Have);                       // the miss SURVIVES the failure

        QueueSpy Spy;
        cc->sweepNow();
        QVERIFY2(Spy.Saw("CID_COVER_RETRY"), "the next sweep must retry the failed cover");
    }

    // The wiring the hand-driven slots can't see: the 60 s timer exists and fires sweepNow; the first new miss
    // schedules a ~2 s debounced sweep; the completion CONNECT is real (the IpfsManager signal, not a direct
    // call, reaches onTransferFinished). Teeth (each caught): delete the ctor's QTimer → timer assert; delete the
    // debounce single-shot → the qWait window sees no enqueue; delete the connect → no coverReady.
    void the_timer_the_debounce_and_the_connect_are_real()
    {
        auto * cc = CoverCache::instance();
        cc->setOnlineProbe([]{ return true; });

        bool Timed = false;
        for (QTimer * T : cc->findChildren<QTimer*>())
            if (T->isActive() && T->interval() == 60'000) Timed = true;
        QVERIFY2(Timed, "the periodic sweep timer must exist and be running");

        QTemporaryDir miss; QVERIFY(miss.isValid());
        QueueSpy Spy;
        QVERIFY(cc->resolve(CoverJson("CID_COVER_DEBOUNCE"), miss.path()).isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(Spy.Saw("CID_COVER_DEBOUNCE"), 5000);   // the DEBOUNCED sweep enqueued it — no manual sweepNow

        QSignalSpy Ready(cc, &CoverCache::coverReady);
        QMetaObject::invokeMethod(IpfsManager::instance(), "transferFinished", Qt::DirectConnection,
                                  Q_ARG(QString, "CID_COVER_DEBOUNCE"), Q_ARG(bool, true), Q_ARG(QString, QString()));
        QCOMPARE(Ready.count(), 1);                                      // through the CONNECT, not a direct call
    }

    // The C bridge itself: a kind-4 event with text reaches transferPhase exactly once per distinct line, and a
    // new lifecycle (Started) resets the dedup so a retry's repeated first line is NOT swallowed. Teeth: delete
    // the kind-4 case in IpfsNodeTransferCb → no signal; drop phaseForget on Started → the third assert fails.
    void the_phase_bridge_dedups_within_a_lifecycle_and_resets_on_started()
    {
        QSignalSpy Phase(IpfsManager::instance(), &IpfsManager::transferPhase);
        IpfsNodeTransferCb("CID_BRIDGE", 4, -1, 0, "attempt 1 — connecting to providers");
        IpfsNodeTransferCb("CID_BRIDGE", 4, -1, 0, "attempt 1 — connecting to providers");   // consecutive dup → dropped
        QCoreApplication::processEvents();
        QCOMPARE(Phase.count(), 1);
        IpfsNodeTransferCb("CID_BRIDGE", 0, -1, 0, nullptr);                                  // Started: new lifecycle
        IpfsNodeTransferCb("CID_BRIDGE", 4, -1, 0, "attempt 1 — connecting to providers");   // same words, must PASS
        QCoreApplication::processEvents();
        QCOMPARE(Phase.count(), 2);
        QCOMPARE(Phase.at(1).at(1).toString(), QString("attempt 1 — connecting to providers"));
    }

    // onNetworkOnline = sweep the offline backlog + one re-resolve nudge.
    void the_online_transition_sweeps_and_nudges()
    {
        auto * cc = CoverCache::instance();
        QTemporaryDir miss; QVERIFY(miss.isValid());
        cc->setOnlineProbe([]{ return false; });
        QVERIFY(cc->resolve(CoverJson("CID_COVER_ONLINE"), miss.path()).isEmpty());

        cc->setOnlineProbe([]{ return true; });
        QueueSpy Spy;
        QSignalSpy Ready(cc, &CoverCache::coverReady);
        cc->onNetworkOnline();
        QVERIFY2(Spy.Saw("CID_COVER_ONLINE"), "the offline backlog must be swept on the online transition");
        QCOMPARE(Ready.count(), 1);                            // the re-resolve nudge

        cc->setOnlineProbe([]{ return IpfsWrapper::DaemonRunning(); });   // restore the default for later tests
    }
};

QTEST_MAIN(TestCoverCache)
#include "test_covercache.moc"
