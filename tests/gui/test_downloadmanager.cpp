// DownloadManager, headless: the ONE non-interactive download core. What it must do before a byte of content is
// fetched: ADOPT a received package out of CATALOG into LIBRARY/<lib>/<pkg>, so the content lands in the library
// and the package is ours from then on (launchable, grafts offered, re-published). Teeth: drop the adopt block in
// beginDownload and the dir stays under CATALOG.
#include <QtTest>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <filesystem>
#include <fstream>
#include "appmodel.h"
#include "ipfsmodel.h"
#include "downloadmanager.h"
#include "packagecatalog.h"
#include "ipfswrapper.h"
#include "downloadqueue.h"
#include "nodefixture.h"
#include "cid.h"
#include <QElapsedTimer>
#include <QUuid>
#include <future>

using json = nlohmann::ordered_json;

static void writeJson(const QString & path, const json & j)
{
    NodeFixture::WriteNodes(path.toStdString(), j);
}

class DownloadManagerTest : public QObject
{
    Q_OBJECT
private slots:
    void install_adopts_the_received_package_before_fetching()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());

        // The seeder: one game whose content is a CID nobody serves (the fetch after the adopt may fail — the adopt
        // must have happened first).
        QTemporaryDir SeedRoot; QVERIFY(SeedRoot.isValid());
        const QString R = SeedRoot.path();
        QDir().mkpath(R + "/Games/[1] A");
        writeJson(R + "/Games/[1] A/tile.json", NodeFixture::Chain("a_tile", {NodeFixture::Tile("1", "A")}));
        writeJson(R + "/Games/[1] A/content.json",
                  NodeFixture::Chain("a_content", {NodeFixture::ContentCid("file", "data.bin",
                      "bafkreib52upmn2n6u65qll6mmj2dft4ddgnvrkcvyhiczcbjlrv2lu766e")}));
        writeJson(R + "/Games/[1] A/a.json",
                  NodeFixture::Chain("a_exec", {NodeFixture::Merge({NodeFixture::Exec("linux64", "a.sh"), NodeFixture::Variant("Play")})}, {"a_content", "a_tile"}));
        json seeder = json{{"Settings", {{"Paths", {{"LibraryRoot", R.toStdString()}}}}}};
        PackageCatalog::PublishLibrary(seeder, &Err);
        const json Items = seeder["Libraries"]["Games"];
        QCOMPARE((int)Items.size(), 1);

        // The receiver: the package received (its folder landed: every node file + the manifest), nothing installed.
        QTemporaryDir RxData; QVERIFY(RxData.isValid());
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", (RxData.path() + "/LIBRARY").toStdString()}}}}},
                        {"FriendLibraries", {{"12D3KooWSeederAlice", {{"Games", Items}}}}}};
        const std::string Nick = "derAlice";
        {
            std::vector<IpfsWrapper::FetchTarget> B;
            for (const auto & T : PackageCatalog::PlanReceivedFetches(cfg, Nick, json{{"Games", Items}}))
                B.push_back(IpfsWrapper::FetchTarget{ T.Cid, T.Dest, false, /*Dir=*/true, /*Verify=*/true });
            std::string WErr;
            QVERIFY2(IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch(B), 30000, &WErr), WErr.c_str());
        }
        QVERIFY2(PackageCatalog::LandReceivedPackages(cfg, &Err), Err.c_str());
        const std::filesystem::path Stub = std::filesystem::path(PackageCatalog::CatalogRootDir(cfg)) / (Nick + " - Games") / "[1] A";
        const std::filesystem::path Home = std::filesystem::path(RxData.path().toStdString()) / "LIBRARY" / "Games" / "[1] A";
        QVERIFY(std::filesystem::exists(Stub / ".package" / ".package.json"));

        QDir appDir(RxData.path());
        AppModel model(&cfg, &appDir);
        model.rebuildCatalog();
        const Node * A = model.catalogIndex().Find("a_exec");
        QVERIFY2(A && A->Received, "precondition: the game is a received stub");
        IpfsModel ipfs(model);
        DownloadManager dm(model, ipfs, nullptr);
        dm.beginDownload("1", {A->Key()}, {}, {});

        // The adopt runs on the worker BEFORE the content targets are collected: the dir moves, the stub is gone.
        QTRY_VERIFY_WITH_TIMEOUT(std::filesystem::exists(Home / "a.json") || std::filesystem::exists(Home / "tile.json")
                                 || std::filesystem::is_directory(Home), 60000);
        QVERIFY2(!std::filesystem::exists(Stub), "the received stub dir is gone from CATALOG");
        QVERIFY2(!std::filesystem::exists(Home / ".package.json"), "the manifest was a receive artifact");
        // From here on the package is ours: not Received, its bundle dir in the library.
        const NodeIndex Fresh = PackageCatalog::BuildCatalogIndex(cfg);
        const Node * A2 = Fresh.Find("a_exec");
        QVERIFY(A2 && !A2->Received);
        QCOMPARE(A2->BundleDir, Home);

        IpfsWrapper::DebugResetQueue();   // the content fetch (an unserved CID) must not keep the process alive
        IpfsWrapper::StopNode();
    }

    // Quitting while a download waits on a fetch no one can serve must not hang the exit. The worker fetches what it
    // learns of AFTER the download began — here the node a received game contains, which nobody has — and a retryable
    // fetch never fails on its own; the destructor cancelled only the CIDs it mapped up front, so join() waited for
    // ever (the window gone, the process still holding the lock and the repo). Teeth: go back to cancelling only
    // DownloadCidToUid in ~DownloadManager and the destruction below does not finish.
    void exit_does_not_wait_on_a_fetch_no_one_can_serve()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir RxData; QVERIFY(RxData.isValid());
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", (RxData.path() + "/LIBRARY").toStdString()}}}}},
                        {"FriendLibraries", {{"Alice", {{"Games", json::array()}}}}}};
        // A node no one can have: the CID of bytes made up now (a fixed test CID may exist somewhere — one did).
        const std::string Nobody = Cid::OfBytes("vidyagod unserved " + QUuid::createUuid().toString().toStdString());
        const std::string Dir = PackageCatalog::CatalogRootDir(cfg) + "/Alice - Games/[7] G/.package";
        std::filesystem::create_directories(Dir);
        const json Game = json{{"LABEL", "g_exec"}, {"VARIANT", "Play"}, {"LAYERS", json::array({
            json{{"NODE", Nobody}},
            json{{"EXEC", json::array({json{{"LABEL", "Play"}, {"HOST", "linux64"}, {"EXE", "g"}, {"TILE", {{"UID", "7"}, {"TITLE", "G"}}}}})}}})}};
        const std::string Bytes = Cid::Canonical(Game), GCid = Cid::OfBytes(Bytes);
        { std::ofstream O(Dir + "/" + GCid + ".json", std::ios::binary); O << Bytes; }
        { std::ofstream O(Dir + "/.package.json"); O << json{{"NODES", json::array({GCid})}, {"PKG", "[7] G"}}.dump(); }

        QDir appDir(RxData.path());
        AppModel model(&cfg, &appDir);
        model.rebuildCatalog();
        const Node * G = model.catalogIndex().Find("g_exec");
        QVERIFY2(G && PackageCatalog::NodeClosureIncomplete(model.catalogIndex(), "g_exec"), "precondition: its closure is incomplete");
        IpfsModel ipfs(model);
        auto dm = std::make_unique<DownloadManager>(model, ipfs, nullptr);
        dm->beginDownload("7", {G->Key()}, {}, {});
        QTRY_VERIFY_WITH_TIMEOUT(IpfsWrapper::DebugJobState(Nobody) >= 0, 30000);

        QElapsedTimer T; T.start();
        auto Gone = std::async(std::launch::async, [&]{ dm.reset(); });
        const bool Joined = Gone.wait_for(std::chrono::seconds(20)) == std::future_status::ready;
        if (!Joined) { IpfsWrapper::CloseQueue(); Gone.wait(); }             // let the test process end either way
        QVERIFY2(Joined, "destroying the manager waited on the unservable fetch");
        IpfsWrapper::DebugResetQueue();
        IpfsWrapper::StopNode();
    }

    // A finished download's worker thread is joined when the next download starts — a thread's stack is kept until
    // join, and the tray daemon grew one per download (53 for one replication). Teeth: drop reapWorkers() from
    // beginDownload and two workers are still held after the second start.
    void a_finished_download_worker_is_joined_by_the_next()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir Data; QVERIFY(Data.isValid());
        const QString R = Data.path() + "/LIBRARY";
        QDir().mkpath(R + "/Games/[9] N");
        writeJson(R + "/Games/[9] N/n.json", json{{"CID", "hN"}, {"LABEL", "n_exec"}, {"VARIANT", "Play"}, {"LAYERS", json::array({
            json{{"EXEC", json::array({json{{"LABEL", "Play"}, {"HOST", "linux64"}, {"EXE", "n"}, {"TILE", {{"UID", "9"}, {"TITLE", "N"}}}}})}}})}});
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", R.toStdString()}}}}}};
        QDir appDir(Data.path());
        AppModel model(&cfg, &appDir);
        model.rebuildCatalog();
        const Node * N = model.catalogIndex().Find("n_exec");
        QVERIFY(N);
        const std::string NKey = N->Key();                                    // a finished download rebuilds the index
        IpfsModel ipfs(model);
        DownloadManager dm(model, ipfs, nullptr);
        QSignalSpy Finished(&dm, &DownloadManager::downloadFinished);
        dm.beginDownload("9", {NKey}, {}, {});
        QTRY_COMPARE_WITH_TIMEOUT(Finished.count(), 1, 30000);                 // nothing to fetch: done at once
        dm.beginDownload("9", {NKey}, {}, {});
        QCOMPARE(dm.workerCount(), 1);                                        // the first was joined, not kept
        QTRY_COMPARE_WITH_TIMEOUT(Finished.count(), 2, 30000);
        IpfsWrapper::StopNode();
    }

    // One variant sits under two tiles (RoC and TFT), so two cards can download the same content. Each card tracks
    // all of it (its progress averages its own CIDs), and cancelling one card aborts only what no other in-flight
    // download still needs. Teeth: map a CID to its first card only (the old QHash<cid, key>) and the second card
    // tracks nothing; drop the shared-CID filter in cancellableCids and cancelling TFT aborts RoC's content.
    void two_cards_on_one_variant_each_track_its_content()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir Data; QVERIFY(Data.isValid());
        const QString R = Data.path() + "/LIBRARY";
        const std::string Shared = Cid::OfBytes("shared " + QUuid::createUuid().toString().toStdString());
        const std::string Own = Cid::OfBytes("own " + QUuid::createUuid().toString().toStdString());
        QDir().mkpath(R + "/Games/[802] W");
        writeJson(R + "/Games/[802] W/w.json", NodeFixture::Chain("w_exec", {NodeFixture::Merge({
            NodeFixture::ContentCid("zip", "w.zip", Shared), NodeFixture::Exec("linux64", "w")})}));
        QDir().mkpath(R + "/Games/[900] O");
        writeJson(R + "/Games/[900] O/o.json", NodeFixture::Chain("o_exec", {NodeFixture::Merge({
            NodeFixture::ContentCid("zip", "o.zip", Own), NodeFixture::Exec("linux64", "o")})}));
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", R.toStdString()}}}}}};
        QDir appDir(Data.path());
        AppModel model(&cfg, &appDir);
        model.rebuildCatalog();
        const Node * W = model.catalogIndex().Find("w_exec");
        const Node * O = model.catalogIndex().Find("o_exec");
        QVERIFY(W && O);
        const std::string WKey = W->Key(), OKey = O->Key();
        IpfsModel ipfs(model);
        DownloadManager dm(model, ipfs, nullptr);
        dm.beginDownload("802", {WKey}, {}, {});
        dm.beginDownload("803", {WKey}, {}, {});
        dm.beginDownload("900", {OKey}, {}, {});
        const QString QShared = QString::fromStdString(Shared), QOwn = QString::fromStdString(Own);
        QVERIFY2(dm.cidsOf("803").contains(QShared), "the second card tracks the content it shares with the first");
        QVERIFY(dm.cidsOf("802").contains(QShared));
        QVERIFY2(!dm.cancellableCids("803").contains(QShared), "cancelling one card leaves content another still needs");
        QVERIFY2(dm.cancellableCids("900").contains(QOwn), "a card's own content is its to cancel");
        // Progress on the shared CID moves BOTH cards' bars, and only theirs. Teeth: recompute only the first card a
        // CID maps to and 803's bar never moves.
        {
            QSignalSpy Prog(&dm, &DownloadManager::downloadProgress);
            emit ipfs.cidChanged(QShared);
            QSet<QString> Moved;
            for (const QList<QVariant> &A : Prog) Moved.insert(A.at(0).toString());
            QCOMPARE(Moved, (QSet<QString>{"802", "803"}));
        }

        // Cancelling purges the cache only for what the cancel aborted and did not finish: a shared CID (another card's
        // download) and a finished file are never purged — purging them wiped the other card's just-finished files.
        // Teeth: purge DownloadUidCids[Key] again (the old handler) and 803 purges the shared CID; drop purgeOnCancel's
        // own shared filter and 900 purges what 901 started needing after the cancel; test "no finished job" instead of
        // "an unfinished job" and 902 purges the file already in place.
        const QString LocalFile = Data.path() + "/local.bin";
        { std::ofstream Lf(LocalFile.toStdString(), std::ios::binary); Lf << std::string(5000, 'L'); }
        const std::string Local = IpfsWrapper::AddNoCopy(LocalFile.toStdString(), &Err);
        QVERIFY2(!Local.empty(), Err.c_str());
        const std::string Unserved = Cid::OfBytes("unserved " + QUuid::createUuid().toString().toStdString());
        const QString HereFile = Data.path() + "/here.bin";
        { std::ofstream Hf(HereFile.toStdString(), std::ios::binary); Hf << std::string(5000, 'H'); }
        const std::string Here = IpfsWrapper::AddNoCopy(HereFile.toStdString(), &Err);
        QVERIFY2(!Here.empty(), Err.c_str());
        QDir().mkpath(R + "/Games/[902] L");                         // local.bin: held, not in place → a job, done at once
        QVERIFY(QFile::copy(HereFile, R + "/Games/[902] L/here.bin"));  // here.bin: already in place → no job at all
        writeJson(R + "/Games/[902] L/l.json", NodeFixture::Chain("l_exec", {NodeFixture::Merge({
            NodeFixture::ContentCid("file", "local.bin", Local), NodeFixture::ContentCid("file", "here.bin", Here),
            NodeFixture::ContentCid("zip", "u.zip", Unserved),
            NodeFixture::Exec("linux64", "l")})}));
        model.rebuildCatalog();
        const std::string LKey = model.catalogIndex().Find("l_exec")->Key();
        dm.beginDownload("902", {LKey}, {}, {});
        QTRY_COMPARE_WITH_TIMEOUT(IpfsWrapper::DebugJobState(Local), 2, 30000);        // Done
        dm.cancelKey("803");
        dm.cancelKey("900");
        dm.cancelKey("902");
        QVERIFY2(!dm.purgeOnCancel("803").contains(QShared), "the shared CID is 802's too");
        QVERIFY(dm.purgeOnCancel("900").contains(QOwn));
        dm.beginDownload("901", {OKey}, {}, {});                            // another card needs it before the abort lands
        QVERIFY2(!dm.purgeOnCancel("900").contains(QOwn), "a CID another card started needing after the cancel is kept");
        QVERIFY2(!dm.purgeOnCancel("902").contains(QString::fromStdString(Local)), "a finished file is never purged");
        QVERIFY2(!dm.purgeOnCancel("902").contains(QString::fromStdString(Here)), "nor one this download never fetched");
        QVERIFY(dm.purgeOnCancel("902").contains(QString::fromStdString(Unserved)));
        IpfsWrapper::StopNode();
    }
};

QTEST_MAIN(DownloadManagerTest)
#include "test_downloadmanager.moc"
