// DownloadManager, headless: the ONE non-interactive download core. What it must do before a byte of content is
// fetched: ADOPT a received package out of CATALOG into LIBRARY/<lib>/<pkg>, so the content lands in the library
// and the package is ours from then on (launchable, grafts offered, re-published). Teeth: drop the adopt block in
// beginDownload and the dir stays under CATALOG.
#include <QtTest>
#include <QTemporaryDir>
#include <QDir>
#include <filesystem>
#include <fstream>
#include "appmodel.h"
#include "ipfsmodel.h"
#include "downloadmanager.h"
#include "packagecatalog.h"
#include "ipfswrapper.h"
#include "downloadqueue.h"
#include "nodefixture.h"

using json = nlohmann::ordered_json;

static void writeJson(const QString & path, const json & j)
{
    std::ofstream f(path.toStdString());
    f << j.dump(2);
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

        // The receiver: the package received (manifest + every node block landed), nothing installed.
        QTemporaryDir RxData; QVERIFY(RxData.isValid());
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", (RxData.path() + "/LIBRARY").toStdString()}}}}},
                        {"FriendLibraries", {{"12D3KooWSeederAlice", {{"Games", Items}}}}}};
        const std::string Nick = "derAlice";
        {
            std::vector<IpfsWrapper::FetchTarget> B;
            for (const auto & T : PackageCatalog::PlanReceivedFetches(cfg, Nick, json{{"Games", Items}}))
                B.push_back(IpfsWrapper::FetchTarget{ T.Cid, T.Dest, false, false, /*Verify=*/true });
            std::string WErr;
            QVERIFY2(IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch(B), 30000, &WErr), WErr.c_str());
        }
        QVERIFY2(PackageCatalog::LandReceivedPackages(cfg, &Err), Err.c_str());
        const std::filesystem::path Stub = std::filesystem::path(PackageCatalog::CatalogRootDir(cfg)) / (Nick + " - Games") / "[1] A";
        const std::filesystem::path Home = std::filesystem::path(RxData.path().toStdString()) / "LIBRARY" / "Games" / "[1] A";
        QVERIFY(std::filesystem::exists(Stub / ".package.json"));

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
};

QTEST_MAIN(DownloadManagerTest)
#include "test_downloadmanager.moc"
