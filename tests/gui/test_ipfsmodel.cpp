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
}

class IpfsModelTest : public QObject
{
    Q_OBJECT
    QTemporaryDir Dir;
    QDir          AppDir{Dir.path()};
    json          Cfg = json{{"Settings", json::object()}, {"LIBRARY", json::array()}};

private slots:
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
