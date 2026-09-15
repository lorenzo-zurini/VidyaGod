// test_covercache.cpp — the cover negative-cache must not be POISONED by a pre-online / offline fetch failure
// (pass-4 HIGH). A give-up is remembered ONLY when it happened while the node was ONLINE; the online transition
// clears the cache so tiles that could not be fetched while the network was down get a fresh attempt.

#include "covercache.h"
#include "ipfswrapper.h"

#include <QtTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <atomic>

class TestCoverCache : public QObject
{
    Q_OBJECT
private slots:
    // The decision function is where the bug lived: the old code cached on `!Ok` alone, so an offline failure
    // (which fails instantly, before goOnline) blacklisted the cover permanently → blank tile until restart.
    void decision_caches_only_online_giveups()
    {
        QVERIFY2(!CoverCache::ShouldNegativeCache(true, true),   "a SUCCESS must never be negative-cached");
        QVERIFY2(!CoverCache::ShouldNegativeCache(true, false),  "a success while offline is still a success");
        QVERIFY2(!CoverCache::ShouldNegativeCache(false, false), "an OFFLINE give-up must NOT poison the cache");
        QVERIFY2(CoverCache::ShouldNegativeCache(false, true),   "an ONLINE give-up SHOULD be cached (stop churn)");
    }

    // recordFetchResult must honour that decision, and onNetworkOnline must clear the cache — so a cover that was
    // (correctly) given up on while online is retried after a reconnect, and one that failed offline never stuck.
    void record_and_clear_conserve_the_invariant()
    {
        auto * cc = CoverCache::instance();
        cc->onNetworkOnline();                                  // start from a known-empty cache
        QCOMPARE(cc->negativeCacheCount(), 0);

        cc->recordFetchResult("/cache/offline_giveup.png", /*FetchOk=*/false, /*NodeOnline=*/false);
        QCOMPARE(cc->negativeCacheCount(), 0);                  // offline give-up left NO poison

        cc->recordFetchResult("/cache/online_success.png", /*FetchOk=*/true, /*NodeOnline=*/true);
        QCOMPARE(cc->negativeCacheCount(), 0);                  // a success is never cached

        cc->recordFetchResult("/cache/online_giveup.png", /*FetchOk=*/false, /*NodeOnline=*/true);
        QCOMPARE(cc->negativeCacheCount(), 1);                  // an online give-up IS cached

        // The online transition must clear the cache AND nudge surfaces to re-resolve (emits coverReady once).
        QSignalSpy spy(cc, &CoverCache::coverReady);
        cc->onNetworkOnline();
        QCOMPARE(cc->negativeCacheCount(), 0);                 // cache cleared → the tile gets a fresh chance
        QCOMPARE(spy.count(), 1);                              // re-resolve nudge fired
    }

    // Drive the REAL request() call site (not just the helpers) through the offline→online sequence via the
    // injected online probe. No live node: FetchToPath returns "node not started" fast, so this is deterministic.
    // Catches the two call-site survivors the helper-only test missed: (1) deleting request()'s offline guard →
    // case A fetches while offline and fires coverReady; (2) hardcoding the record's online arg true → case B
    // caches a give-up that happened after the node dropped offline.
    void request_respects_the_online_probe_at_start_and_at_record()
    {
        auto * cc = CoverCache::instance();
        cc->onNetworkOnline();
        QCOMPARE(cc->negativeCacheCount(), 0);

        QTemporaryDir pkg;                                     // empty → the cover file is absent → request() runs
        QVERIFY(pkg.isValid());
        const nlohmann::ordered_json cover = {
            {"PATH", "cover.png"},
            {"SOURCE", {{"TYPE", "ipfs"}, {"CID", "bafkreiaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}}}};

        // Case A — OFFLINE at start: request() must not fetch at all (no coverReady, no negative cache).
        cc->setOnlineProbe([] { return false; });
        QSignalSpy offline(cc, &CoverCache::coverReady);
        QVERIFY(cc->resolve(cover, pkg.path()).isEmpty());
        QVERIFY2(!offline.wait(600), "request() fetched while offline (the offline guard is gone)");
        QCOMPARE(cc->negativeCacheCount(), 0);

        // Case B — ONLINE at start, OFFLINE by completion: a failed fetch must NOT be cached (network dropped).
        std::atomic<int> probeCalls{0};
        cc->setOnlineProbe([&probeCalls] { return probeCalls.fetch_add(1) == 0; }); // true once (start), then false
        QSignalSpy dropped(cc, &CoverCache::coverReady);
        QVERIFY(cc->resolve(cover, pkg.path()).isEmpty());
        QVERIFY2(dropped.wait(4000), "the fetch never completed");
        QCOMPARE(cc->negativeCacheCount(), 0);                 // offline at record time → not poisoned

        // Case C — ONLINE throughout: a failed fetch IS cached (stop churning on unseeded art).
        cc->setOnlineProbe([] { return true; });
        QSignalSpy online(cc, &CoverCache::coverReady);
        QVERIFY(cc->resolve(cover, pkg.path()).isEmpty());
        QVERIFY2(online.wait(4000), "the fetch never completed");
        QCOMPARE(cc->negativeCacheCount(), 1);                 // online give-up → cached

        cc->setOnlineProbe([] { return IpfsWrapper::DaemonRunning(); }); // restore default for any later test
        cc->onNetworkOnline();
    }
};

QTEST_MAIN(TestCoverCache)
#include "test_covercache.moc"
