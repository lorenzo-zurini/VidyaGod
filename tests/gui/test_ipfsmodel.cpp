// Tests for IpfsModel — the download/IPFS runtime state extracted out of IpfsTab. Drives the model's slots and the
// transfer events (simulated by invoking IpfsManager's signals) and asserts the per-CID CidState transitions, the
// pct/size read API used by DownloadManager, and the cidChanged emissions the view renders from. The IPFS node is not
// started (Available()==false), so refresh() is inert and cancel/prioritize hit an empty queue (no-ops).

#include <QtTest>
#include <QTemporaryDir>
#include <QDir>
#include <QSignalSpy>

#include "appmodel.h"
#include "ipfsmodel.h"
#include "ipfswrapper.h"   // IpfsManager (its signals are invoked to simulate the node's transfer events)

using json = nlohmann::ordered_json;
using P    = IpfsModel::CidState;

namespace {
// Simulate a node transfer event by invoking the corresponding IpfsManager signal (same-thread → DirectConnection,
// so the model's connected handler runs synchronously).
void started (const QString & c)                 { QMetaObject::invokeMethod(IpfsManager::instance(), "transferStarted",   Qt::DirectConnection, Q_ARG(QString, c)); }
void progress(const QString & c, double p)       { QMetaObject::invokeMethod(IpfsManager::instance(), "transferProgress",  Qt::DirectConnection, Q_ARG(QString, c), Q_ARG(double, p)); }
void finished(const QString & c, bool ok, const QString & e = QString())
                                                 { QMetaObject::invokeMethod(IpfsManager::instance(), "transferFinished",  Qt::DirectConnection, Q_ARG(QString, c), Q_ARG(bool, ok), Q_ARG(QString, e)); }
void phaseEv (const QString & c, const QString & t){ QMetaObject::invokeMethod(IpfsManager::instance(), "transferPhase",   Qt::DirectConnection, Q_ARG(QString, c), Q_ARG(QString, t)); }
}

class IpfsModelTest : public QObject
{
    Q_OBJECT
    QTemporaryDir Dir;
    QDir          AppDir{Dir.path()};
    json          Cfg = json{{"Settings", json::object()}, {"LIBRARY", json::array()}};

private slots:
    // The transfer's NARRATION lands on its row verbatim (activity), and never outlives the transfer: Started
    // clears any stale line, Finished clears it again. Teeth (each caught): drop the transferPhase connect → the
    // set assert fails; drop the clear in Started or Finished → the matching clear assert fails.
    void narration_lands_on_the_row_and_never_outlives_the_transfer()
    {
        AppModel m(&Cfg, &AppDir); IpfsModel im(m);
        const QString cid = "QmNarrated";
        started(cid);
        phaseEv(cid, "attempt 2 — connecting to providers");
        QCOMPARE(im.state(cid).activity, QString("attempt 2 — connecting to providers"));
        started(cid);                                              // a NEW attempt lifecycle starts clean
        QVERIFY(im.state(cid).activity.isEmpty());
        phaseEv(cid, "downloading from gateway.pinata.cloud");
        QCOMPARE(im.state(cid).activity, QString("downloading from gateway.pinata.cloud"));
        finished(cid, true);
        QVERIFY(im.state(cid).activity.isEmpty());                 // Seeded rows carry no stale narration
        phaseEv("QmNeverSeen", "text for an unknown row");         // no row → ignored, no crash, none created
        QVERIFY(!im.has("QmNeverSeen"));
    }

    void mark_queued_sets_state_and_signals()
    {
        AppModel m(&Cfg, &AppDir); IpfsModel im(m);
        QSignalSpy spy(&im, &IpfsModel::cidChanged);
        const QString cid = "QmQueued";
        im.markQueued(cid);
        QVERIFY(im.has(cid));
        QCOMPARE(im.state(cid).phase, P::Queued);
        QVERIFY(spy.count() >= 1);
        QCOMPARE(spy.first().at(0).toString(), cid);
    }

    void transfer_lifecycle_queued_to_seeded()
    {
        AppModel m(&Cfg, &AppDir); IpfsModel im(m);
        const QString cid = "QmLife";
        started(cid);
        QCOMPARE(im.state(cid).phase, P::Downloading);
        progress(cid, 42.0);
        QCOMPARE(im.state(cid).phase, P::Downloading);
        QCOMPARE(im.pct(cid), 42.0);           // read API used by DownloadManager
        finished(cid, true);
        QCOMPARE(im.state(cid).phase, P::Seeded);
        QCOMPARE(im.pct(cid), 100.0);
    }

    void errored_transfer_keeps_reason()
    {
        AppModel m(&Cfg, &AppDir); IpfsModel im(m);
        const QString cid = "QmErr";
        started(cid);
        finished(cid, false, "boom\nmissing files");
        QCOMPARE(im.state(cid).phase, P::Errored);
        QVERIFY(im.state(cid).error.contains("missing files"));
    }

    // A bounded caller (launch/cover) that gives up on its deadline stamps Errored; if an unbounded background
    // download then takes over the same CID via leadership handoff, its progress ticks must HEAL the row back to
    // Downloading (and clear the error) rather than leave a red row on a healthy, progressing transfer. Teeth:
    // drop the `|| Errored` heal in IpfsModel's progress handler and the phase stays Errored → this fails.
    void progress_heals_a_handed_off_errored_row()
    {
        AppModel m(&Cfg, &AppDir); IpfsModel im(m);
        const QString cid = "QmHandoff";
        started(cid);
        finished(cid, false, "fetch of QmHandoff did not complete before its deadline"); // bounded leader gave up
        QCOMPARE(im.state(cid).phase, P::Errored);
        progress(cid, 55.0);                                    // unbounded successor took over → live again
        QCOMPARE(im.state(cid).phase, P::Downloading);
        QVERIFY(im.state(cid).error.isEmpty());
        QCOMPARE(im.pct(cid), 55.0);
    }

    void cancelled_before_local_drops_entry()
    {
        AppModel m(&Cfg, &AppDir); IpfsModel im(m);
        const QString cid = "QmCancel";
        started(cid);
        QVERIFY(im.has(cid));
        QSignalSpy removed(&im, &IpfsModel::cidRemoved);
        finished(cid, false, "cancelled");     // not on disk (node off) → the row is dropped
        QVERIFY(!im.has(cid));
        QCOMPARE(removed.count(), 1);
    }

    void read_api_defaults_and_node_status()
    {
        AppModel m(&Cfg, &AppDir); IpfsModel im(m);
        QCOMPARE(im.pct("QmUnknown"), -1.0);   // absent CID
        QCOMPARE(im.size("QmUnknown"), (qlonglong)-1);
        QVERIFY(!im.nodeStatus().available);   // node not started
    }

    void mutators_are_safe_with_empty_queue()
    {
        AppModel m(&Cfg, &AppDir); IpfsModel im(m);
        im.cancel("QmNope");         // empty queue → no-op, must not crash
        im.prioritize("QmNope");
        im.setActive(true);          // node off → refresh() inert
        im.setActive(false);
        QVERIFY(true);
    }
};

QTEST_MAIN(IpfsModelTest)
#include "test_ipfsmodel.moc"
