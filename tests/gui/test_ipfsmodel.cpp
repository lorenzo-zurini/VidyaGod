// Tests for IpfsModel — the download/IPFS runtime state extracted out of IpfsTab. Drives the model's slots and the
// transfer events (simulated by invoking IpfsManager's signals) and asserts the per-CID CidState transitions, the
// pct/size read API used by DownloadManager, and the cidChanged emissions the view renders from. The IPFS node is not
// started (Available()==false), so refresh() is inert and cancel/prioritize hit an empty queue (no-ops).

#include <QtTest>
#include <QTemporaryDir>
#include <QDir>
#include <QSignalSpy>
#include <QTreeWidget>

#include "appmodel.h"
#include "ipfsmodel.h"
#include "ipfstab.h"
#include "ipfswrapper.h"   // IpfsManager (its signals are invoked to simulate the node's transfer events)
#include "downloadqueue.h" // EnqueueBatch/DebugResetQueue (the queue-destination naming under test)
#include "packagecatalog.h"
#include "nodefixture.h"

#include <fstream>
#include <filesystem>

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

    // A queued transfer the catalog can't name yet is named by WHERE IT IS GOING — the queue's destination path:
    // file name, "[uid] Title" package dir, LIBRARY collection. This is what keeps received-share node blocks (queued
    // before their block exists anywhere) out of "Unknown / not in your library". Teeth: drop LabelFromQueueDest, the
    // QueueDestForCid accessor, or the "[uid] " strip and the asserts fail.
    void queued_transfer_is_named_by_its_destination()
    {
        AppModel m(&Cfg, &AppDir); IpfsModel im(m);
        const QString cid = "bafyreidestnamedblockaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        const std::string Dest = Dir.path().toStdString() + "/LIBRARY/Alice - Games/[7] Multi/m_v1.json";
        IpfsWrapper::EnqueueBatch({ IpfsWrapper::FetchTarget{ cid.toStdString(), Dest, /*Optional=*/true } });
        im.markQueued(cid);
        QCOMPARE(im.state(cid).label,    QString("m_v1.json"));
        QCOMPARE(im.state(cid).package,  QString("Multi"));
        QCOMPARE(im.state(cid).source,   QString("Alice - Games"));
        IpfsWrapper::DebugResetQueue();

        // No queue job and no catalog entry → the honest fallback remains.
        const QString stray = "QmStrayNoJob";
        im.markQueued(stray);
        QCOMPARE(im.state(stray).package, QString("Unknown / not in your library"));
        IpfsWrapper::DebugResetQueue();
    }

    // A catalog node's OWN block CID is a first-class row: labeled "<NODE_ID> (node)" under its package and LIBRARY
    // collection, category Meta — a received share that has LANDED is exactly such a node, and must never read as
    // "Unknown". Teeth: drop the node-block pass in BuildCidLabels and every assert fails.
    void catalog_node_blocks_are_labeled()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir Root; QVERIFY(Root.isValid());
        const QString R = Root.path() + "/LIBRARY";                 // a real LIBRARY component → SourceOfBundle works
        QDir().mkpath(R + "/VidyaGod/[1] Game");
        auto writeJson = [](const QString & path, const json & j) { NodeFixture::WriteNodes(path.toStdString(), j); };
        writeJson(R + "/VidyaGod/[1] Game/tile.json", NodeFixture::Chain("g_tile", {NodeFixture::Tile("1", "Game")}));
        writeJson(R + "/VidyaGod/[1] Game/game.json",
                  NodeFixture::Chain("g_exec", {NodeFixture::Merge({NodeFixture::Exec("win32", "g.exe"), NodeFixture::TakeTile("g_tile")})}, {"g_tile"}));

        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", R.toStdString()}}}}}, {"LIBRARY", json::array()}};
        AppModel m(&cfg, &AppDir);
        m.rebuildCatalog();
        QString NodeCid;
        for (const auto & [C, N] : m.catalogIndex().Nodes) if (N.NodeId == "g_exec") NodeCid = QString::fromStdString(C);
        QVERIFY2(!NodeCid.isEmpty(), "the exec node indexed with a CID key");

        IpfsModel im(m);
        started(NodeCid);                                           // any transfer/pin row for a node block
        QCOMPARE(im.state(NodeCid).label,    QString("g_exec (node)"));
        QCOMPARE(im.state(NodeCid).package,  QString("Game"));
        QCOMPARE(im.state(NodeCid).category, QString("Content"));   // node rows live INSIDE their package's tree
        QCOMPARE(im.state(NodeCid).source,   QString("VidyaGod"));
        IpfsWrapper::StopNode();
    }

    // Our package's pin folder (what a pinning service is given) is named from the published pins and belongs ON its
    // package row: in the tab it is the "Game" group's CID, never a leaf and never under "Unknown / not in your
    // library" — nothing inside a node names it. Teeth: drop the pin pass in BuildCidLabels (every assert fails), or
    // the packageLevel branch in IpfsTab::renderLeaf (the tab asserts fail).
    void package_pin_folder_is_the_package_row()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir Root; QVERIFY(Root.isValid());
        const QString R = Root.path() + "/LIBRARY";
        QDir().mkpath(R + "/VidyaGod/[1] Game");
        NodeFixture::WriteNodes((R + "/VidyaGod/[1] Game/game.json").toStdString(),
                                NodeFixture::Chain("g_exec", {NodeFixture::Exec("win32", "g.exe")}));
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", R.toStdString()}}}}}, {"LIBRARY", json::array()},
                        {"Pins", {{"VidyaGod/[1] Game", "QmGamePin"}}}};
        AppModel m(&cfg, &AppDir);
        m.rebuildCatalog();
        IpfsModel im(m);
        IpfsTab tab(im);
        started("QmGamePin");
        QCOMPARE(im.state("QmGamePin").label,   QString("Game — pin folder"));
        QCOMPARE(im.state("QmGamePin").package, QString("Game"));
        QCOMPARE(im.state("QmGamePin").source,  QString("VidyaGod"));
        QVERIFY2(im.state("QmGamePin").packageLevel, "the pin folder is the package itself");
        QCOMPARE(im.packagePin("Game"), QString("QmGamePin"));

        QTreeWidget * tree = tab.findChild<QTreeWidget *>();
        QVERIFY(tree);
        QList<QTreeWidgetItem *> rows = tree->findItems("QmGamePin", Qt::MatchExactly | Qt::MatchRecursive, 6);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0]->text(0), QString("Game"));                                   // the package row itself
        QVERIFY2(tree->findItems("Unknown / not in your library", Qt::MatchExactly | Qt::MatchRecursive, 0).isEmpty(),
                 "nothing of this package lands in Unknown");
        IpfsWrapper::StopNode();
    }

    // The IPFS tab adds a source through the model's one path: a duplicate is refused (and said), a new one is kept and
    // the Sources page is told. Teeth: the tab's own copy — its own dedup and write, and no packageSourcesChanged.
    void a_source_added_here_is_the_models()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir Dir(d.path());
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", (d.path() + "/LIBRARY").toStdString()}}},
                                      {"PackageSources", json::array({ json{{"CID", "bafybeigdyrzt5sfp7udm7hu76uh7y26nf3efuylqabf3oclgtqy55fbzdi"}, {"NAME", "x"}} })}}},
                        {"LIBRARY", json::array()}};
        AppModel m(&cfg, &Dir); IpfsModel im(m);
        QSignalSpy Told(&m, &AppModel::packageSourcesChanged);
        QVERIFY(!im.addSource("bafybeigdyrzt5sfp7udm7hu76uh7y26nf3efuylqabf3oclgtqy55fbzdi", "again"));
        QVERIFY(im.addSource("bafybeihdwdcefgh4dqkjv67uzcmw7ojee6xedzdetojuzjevtenxquvyku", "new"));
        QCOMPARE((int)cfg["Settings"]["PackageSources"].size(), 2);
        QVERIFY(Told.count() >= 1);
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

    // A transfer's end refreshes the model while the IPFS tab shows, and only then: each refresh lists every pin and
    // asks the node about each one, and a peer's library landing ended thousands of transfers, the tab hidden or not.
    // Shown again, it refreshes. Teeth: refresh on every transfer's end; forget to refresh when the tab shows.
    // The sizes of many queued CIDs (a received library queues thousands) are looked up together, a look at a time —
    // a thread each entered the node from thousands of threads. Every row still gets its size. Teeth: a look per CID
    // (as many looks as CIDs); drop the look after (CIDs asked during the first look never get one).
    void queued_sizes_are_looked_up_together()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        {
            AppModel m(&Cfg, &AppDir); IpfsModel im(m);
            QTemporaryDir D; QVERIFY(D.isValid());
            QStringList Held;
            for (int I = 0; I < 40; ++I)
            {
                const std::string F = D.path().toStdString() + "/f" + std::to_string(I);
                std::ofstream(F) << "file " << I;
                Held << QString::fromStdString(IpfsWrapper::AddNoCopy(F, &Err));
            }
            const int Before = im.sizeLooks();
            for (const QString &C : Held) im.markQueued(C);
            QTRY_VERIFY_WITH_TIMEOUT([&]{ for (const QString &C : Held) if (im.size(C) < 0) return false; return true; }(), 10000);
            QVERIFY2(im.sizeLooks() - Before <= 3, std::to_string(im.sizeLooks() - Before).c_str());
        }
        IpfsWrapper::StopNode();
    }

    // The tab's check of every pin, a pass for many pins at a time: a pin whose file is gone turns red with why, its
    // row says a file is missing, and the pins beside it stay as they are — the same verdicts the pin-by-pin walks gave.
    // Teeth: take the verdicts in another order (the wrong row turns red); read "gone" from the wrong field (the row
    // never says a file is missing).
    void a_pin_whose_file_is_gone_turns_red_in_the_batched_check()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        {
            QTemporaryDir D; QVERIFY(D.isValid());
            QStringList Held;
            for (int I = 0; I < 6; ++I)
            {
                const std::string F = D.path().toStdString() + "/pin" + std::to_string(I);
                std::ofstream(F) << "pinned file " << I;
                Held << QString::fromStdString(IpfsWrapper::AddNoCopy(F, &Err));
            }
            std::filesystem::remove(D.path().toStdString() + "/pin3");
            AppModel m(&Cfg, &AppDir); IpfsModel im(m);
            im.SweepChunk = 4;                                       // two passes over the six
            im.setActive(true);
            using P = IpfsModel::CidState::Phase;
            QTRY_VERIFY_WITH_TIMEOUT(im.state(Held[3]).phase == P::Errored, 10000);
            QVERIFY(im.state(Held[3]).error.contains("gone"));
            QTRY_COMPARE_WITH_TIMEOUT(im.state(Held[3]).missing, 1, 10000);
            for (int I : {0, 1, 2, 4, 5})
            {
                QVERIFY2(im.state(Held[I]).phase != P::Errored, qPrintable(Held[I]));
                QTRY_COMPARE_WITH_TIMEOUT(im.state(Held[I]).missing, 0, 10000);
            }
            im.setActive(false);
        }
        IpfsWrapper::StopNode();
    }

    void a_transfers_end_refreshes_only_while_the_tab_shows()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        {
            AppModel m(&Cfg, &AppDir); IpfsModel im(m);
            QSignalSpy Status(&im, &IpfsModel::nodeStatusChanged);
            for (int I = 0; I < 5; ++I) finished(QString("QmEnded%1").arg(I), true);
            QTest::qWait(800);
            QCOMPARE(Status.count(), 0);                   // hidden: no refresh
            QVERIFY(!im.nodeStatus().available);
            im.setActive(true);                            // shown: it catches up
            QTRY_VERIFY_WITH_TIMEOUT(im.nodeStatus().available, 10000);
            im.setActive(false);
        }
        IpfsWrapper::StopNode();
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
