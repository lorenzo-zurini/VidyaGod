// Tests for PackageCatalog: hydration predicates (NodeHasContent / NodeHydrated — the #2 content-guard), the
// remove-flow DehydrateNode (#8/#9), per-package user settings, and runner predicates. Builds real temp bundles on
// disk so the layer-presence checks have something to resolve. IPFS node is not started, so DehydrateNode's
// unpin/drop-ref calls are graceful no-ops (we assert the filesystem side).

#include <QtTest>
#include <QTemporaryDir>

#include "packagecatalog.h"
#include "manifestmodel.h"
#include "runnerinstall.h"
#include "ipfswrapper.h"

#include <fstream>
#include <string>
#include <vector>

using json = nlohmann::ordered_json;

namespace {
void writeJson(const QString & path, const json & j)
{
    std::ofstream f(path.toStdString());
    f << j.dump(2);
}
void writeFile(const QString & path, const std::string & content = "x")
{
    std::ofstream f(path.toStdString());
    f << content;
}
}

class PackageCatalogTest : public QObject
{
    Q_OBJECT
private slots:
    // #2: a content-less node is vacuously "hydrated" but has no content — NodeHasContent must distinguish it.
    void node_has_content_vs_contentless()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        writeJson(dir.path() + "/game.json", json{{"NODE_ID", "game"}, {"ROLE", "launchable"},
            {"LAYERS", json::array({ json{{"TYPE", "VFSDirLayer"}, {"PATH", "data"}} })}});
        writeJson(dir.path() + "/empty.json", json{{"NODE_ID", "empty"}, {"ROLE", "launchable"}, {"LAYERS", json::array()}});
        QDir(dir.path() + "/data").mkpath(".");                 // make 'game' hydrated

        NodeIndex idx; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx);
        QVERIFY(PackageCatalog::NodeHasContent(idx, "game"));
        QVERIFY(!PackageCatalog::NodeHasContent(idx, "empty"));  // content-less → hidden from Library/Installed
        QVERIFY(PackageCatalog::NodeHydrated(idx, "game"));      // data dir present
    }

    // NodeHydrated flips to false once the content is gone.
    void node_hydrated_tracks_content_presence()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        writeJson(dir.path() + "/game.json", json{{"NODE_ID", "game"}, {"ROLE", "launchable"},
            {"LAYERS", json::array({ json{{"TYPE", "VFSFileLayer"}, {"PATH", "blob.bin"}} })}});

        NodeIndex idx0; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx0);
        QVERIFY(!PackageCatalog::NodeHydrated(idx0, "game"));    // blob.bin missing

        writeFile(dir.path() + "/blob.bin");
        NodeIndex idx1; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx1);
        QVERIFY(PackageCatalog::NodeHydrated(idx1, "game"));
    }

    // #8/#9: DehydrateNode deletes the content-layer files (keeping the manifest) and reports what it removed.
    void dehydrate_node_removes_content_keeps_manifest()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        writeJson(dir.path() + "/game.json", json{{"NODE_ID", "game"}, {"ROLE", "launchable"},
            {"LAYERS", json::array({ json{{"TYPE", "VFSFileLayer"}, {"PATH", "blob.bin"}} })}});
        writeFile(dir.path() + "/blob.bin");

        NodeIndex idx; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx);
        QVERIFY(PackageCatalog::NodeHydrated(idx, "game"));

        const int removed = PackageCatalog::DehydrateNode(idx, "game");
        QVERIFY(removed >= 1);
        QVERIFY(!QFile::exists(dir.path() + "/blob.bin"));       // content gone
        QVERIFY(QFile::exists(dir.path() + "/game.json"));       // manifest kept → returns to Catalog
    }

    // Per-package user settings persist into the GlobalConfig and read back.
    void package_user_settings_roundtrip()
    {
        // User settings live inside the package's LIBRARY entry (USERSETTINGS), so the entry must exist first.
        json cfg = json{{"LIBRARY", json::array({ json{{"PACKAGEUID", "pkg1"}} })}};
        PackageCatalog::SetPackageUserSetting(cfg, "pkg1", "PREFERRED_RUNNER", "wine-ge");
        json us = PackageCatalog::GetPackageUserSettings(cfg, "pkg1");
        QCOMPARE(us.value("PREFERRED_RUNNER", std::string()), std::string("wine-ge"));
        QVERIFY(PackageCatalog::GetPackageUserSettings(cfg, "other").value("PREFERRED_RUNNER", std::string()).empty());
    }

    // CompatibleRunners returns runners whose GUEST set covers the launchable's host platform.
    void compatible_runners_by_platform()
    {
        NodeIndex idx;
        Node launch; launch.NodeId = "game"; launch.Role = "launchable"; launch.HostPlatform = "win32";
        Node wine;  wine.NodeId = "wine"; wine.Role = "runner"; wine.GuestPlatform = {"win32", "win64"};
        wine.HostPlatform = ManifestModel::MachinePlatform();
        wine.Exec = json{{"EXECUTABLE", "%RunnerMount%/proton"}};
        Node snes;  snes.NodeId = "snes9x"; snes.Role = "runner"; snes.GuestPlatform = {"snes"};
        snes.HostPlatform = ManifestModel::MachinePlatform();
        snes.Exec = json{{"EXECUTABLE", "%RunnerMount%/snes9x"}};
        idx.Nodes["game"] = launch; idx.Nodes["wine"] = wine; idx.Nodes["snes9x"] = snes;

        auto runners = PackageCatalog::CompatibleRunners(idx, idx.Nodes["game"]);
        bool hasWine = false, hasSnes = false;
        for (const Node * r : runners) { if (r->NodeId == "wine") hasWine = true; if (r->NodeId == "snes9x") hasSnes = true; }
        QVERIFY(hasWine);
        QVERIFY(!hasSnes);   // snes runner doesn't serve win32
    }

    // A package download pools the game's content layers AND its runner's build layers into ONE fetch batch (so they
    // download concurrently). CollectContentTargets + CollectRunnerNodeTargets append to the same Targets vector.
    void download_pools_content_and_runner_into_one_batch()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        // Dehydrated game: content zip has an IPFS CID but the file is absent → a fetch target.
        writeJson(dir.path() + "/game.json", json{{"NODE_ID", "game"}, {"ROLE", "launchable"}, {"UID", "g"},
            {"PLATFORM", {{"HOST", "win32"}}}, {"EXEC", {{"CONTENTPATH", "g.exe"}}}, {"PARENTS", json::array({"content"})}});
        writeJson(dir.path() + "/content.json", json{{"NODE_ID", "content"}, {"ROLE", "content"}, {"LAYERS", json::array({
            json{{"TYPE", "VFSZipLayer"}, {"PATH", "game.zip"}, {"SOURCE", {{"TYPE", "ipfs"}, {"CID", "CID_GAME"}}}} })}});
        // Runner with a dehydrated build (its build is a PARENT content node, per the runner closure).
        writeJson(dir.path() + "/wine.json", json{{"NODE_ID", "wine"}, {"ROLE", "runner"},
            {"PLATFORM", {{"HOST", ManifestModel::MachinePlatform()}, {"GUEST", json::array({"win32"})}}},
            {"EXEC", {{"EXECUTABLE", "x"}}}, {"PARENTS", json::array({"winebuild"})}});
        writeJson(dir.path() + "/winebuild.json", json{{"NODE_ID", "winebuild"}, {"ROLE", "content"}, {"LAYERS", json::array({
            json{{"TYPE", "VFSZipLayer"}, {"PATH", "wine.zip"}, {"SOURCE", {{"TYPE", "ipfs"}, {"CID", "CID_WINE"}}}} })}});

        NodeIndex idx; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx);

        std::vector<IpfsWrapper::FetchTarget> targets; std::string err;
        QVERIFY(PackageCatalog::CollectContentTargets(idx, "game", {}, targets, &err));
        QVERIFY(RunnerInstall::CollectRunnerNodeTargets(idx, "wine", targets, &err));

        bool hasGame = false, hasWine = false;
        for (const auto & t : targets) { if (t.Cid == "CID_GAME") hasGame = true; if (t.Cid == "CID_WINE") hasWine = true; }
        QVERIFY(hasGame);                       // game content target
        QVERIFY(hasWine);                       // runner build target — in the SAME batch
        QVERIFY(targets.size() >= 2);
    }
};

QTEST_MAIN(PackageCatalogTest)
#include "test_packagecatalog.moc"
