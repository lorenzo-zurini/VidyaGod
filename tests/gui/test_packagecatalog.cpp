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
#include "apppaths.h"

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
        writeJson(dir.path() + "/game.json", json{{"NODE_ID", "game"},
            {"LAYERS", json::array({ json{{"TYPE", "DeclareExec"}, {"PLATFORM", "win32"}, {"CONTENTPATH", "g.exe"}},
                                     json{{"TYPE", "VFSDirLayer"}, {"PATH", "data"}} })}});
        writeJson(dir.path() + "/empty.json", json{{"NODE_ID", "empty"},
            {"LAYERS", json::array({ json{{"TYPE", "DeclareExec"}, {"PLATFORM", "win32"}, {"CONTENTPATH", "g.exe"}} })}});
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
        writeJson(dir.path() + "/game.json", json{{"NODE_ID", "game"},
            {"LAYERS", json::array({ json{{"TYPE", "DeclareExec"}, {"PLATFORM", "win32"}, {"CONTENTPATH", "g.exe"}},
                                     json{{"TYPE", "VFSFileLayer"}, {"PATH", "blob.bin"}} })}});

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
        writeJson(dir.path() + "/game.json", json{{"NODE_ID", "game"},
            {"LAYERS", json::array({ json{{"TYPE", "DeclareExec"}, {"PLATFORM", "win32"}, {"CONTENTPATH", "g.exe"}},
                                     json{{"TYPE", "VFSFileLayer"}, {"PATH", "blob.bin"}} })}});
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
        Node launch; launch.NodeId = "game"; launch.HasExec = true; launch.HostPlatform = "win32";
        Node wine;  wine.NodeId = "wine"; wine.HasRunner = true; wine.GuestPlatform = {"win32", "win64"};
        wine.HostPlatform = ManifestModel::MachinePlatform();
        wine.Exec = json{{"EXECUTABLE", "%RunnerMount%/proton"}};
        Node snes;  snes.NodeId = "snes9x"; snes.HasRunner = true; snes.GuestPlatform = {"snes"};
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
        writeJson(dir.path() + "/game.json", json{{"NODE_ID", "game"},
            {"LAYERS", json::array({ json{{"TYPE", "DeclareLibraryItem"}, {"UID", "g"}},
                                     json{{"TYPE", "DeclareExec"}, {"PLATFORM", "win32"}, {"CONTENTPATH", "g.exe"}} })},
            {"PARENTS", json::array({"content"})}});
        writeJson(dir.path() + "/content.json", json{{"NODE_ID", "content"}, {"LAYERS", json::array({
            json{{"TYPE", "VFSZipLayer"}, {"PATH", "game.zip"}, {"SOURCE", {{"TYPE", "ipfs"}, {"CID", "CID_GAME"}}}} })}});
        // Runner with a dehydrated build (its build is a PARENT content node, per the runner closure).
        writeJson(dir.path() + "/wine.json", json{{"NODE_ID", "wine"},
            {"LAYERS", json::array({ json{{"TYPE", "DeclareRunner"}, {"HOST", ManifestModel::MachinePlatform()},
                                          {"GUEST", json::array({"win32"})}, {"EXECUTABLE", "x"}} })},
            {"PARENTS", json::array({"winebuild"})}});
        writeJson(dir.path() + "/winebuild.json", json{{"NODE_ID", "winebuild"}, {"LAYERS", json::array({
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

    // A locally-added bundle (a LIBRARY entry whose PATH is OUTSIDE any repo dir) must be indexed by the catalog —
    // BuildCatalogIndex scans LocalPackageDirs alongside the repo roots. Regression: external bundles silently never
    // showed because only repo dirs were scanned.
    void local_package_is_indexed()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());                 // an external bundle, not under any repo
        writeJson(dir.path() + "/tile.json", json{{"NODE_ID", "tile"},
            {"LAYERS", json::array({ json{{"TYPE", "DeclareLibraryItem"}, {"UID", "777"}, {"TITLE", "My Local Game"}} })}});
        writeJson(dir.path() + "/variant.json", json{{"NODE_ID", "variant"}, {"PARENTS", json::array({"tile"})},
            {"LAYERS", json::array({ json{{"TYPE", "DeclareExec"}, {"PLATFORM", "win32"}, {"CONTENTPATH", "g.exe"}} })}});

        json cfg = json{{"Settings", {{"Repositories", json::array()}}},
                        {"LIBRARY", json::array({ json{{"PACKAGEUID", "777"}, {"PATH", dir.path().toStdString()}} })}};

        QVERIFY(!PackageCatalog::LocalPackageDirs(cfg).empty());   // the external bundle dir is collected
        NodeIndex idx = PackageCatalog::BuildCatalogIndex(cfg);
        const Node * v = idx.Find("variant");
        QVERIFY(v != nullptr);                                     // indexed despite living outside any repo
        QVERIFY(v->Presentable());                                 // linked to its tile → shows in the library
        QCOMPARE(v->GameKey(), std::string("tile"));
    }

    // Startup prune drops a LIBRARY entry for a local bundle whose PATH no longer exists (moved/deleted), keeps the
    // present one.
    void prune_moved_local_package()
    {
        QTemporaryDir present; QVERIFY(present.isValid());
        json cfg = json{{"Settings", {{"Repositories", json::array()}}},
                        {"LIBRARY", json::array({
                            json{{"PACKAGEUID", "a"}, {"PATH", present.path().toStdString()}},
                            json{{"PACKAGEUID", "b"}, {"PATH", "/no/such/bundle/dir/xyz"}} })}};

        const int removed = PackageCatalog::PruneMovedLocalPackages(cfg);
        QCOMPARE(removed, 1);
        QCOMPARE((int)cfg["LIBRARY"].size(), 1);
        QCOMPARE(cfg["LIBRARY"][0].value("PACKAGEUID", std::string()), std::string("a"));   // present one kept
    }

    // A CID package source (its already-fetched dir under <DataRoot>/CIDPACKAGES/) is indexed by BuildCatalogIndex as a
    // root, and its packages are treated as MANAGED — not badged/pruned as "local". (Simulates a fetched source; the
    // network fetch itself is covered by the Go node's TestFetchDirToPath.)
    void cid_package_source_is_managed_and_indexed()
    {
        QTemporaryDir data; QVERIFY(data.isValid());
        AppPaths::SetDataRoot(data.path().toStdString());

        const QString bundle = data.path() + "/CIDPACKAGES/mysource/game";
        QDir().mkpath(bundle);
        writeJson(bundle + "/tile.json", json{{"NODE_ID", "cidtile"},
            {"LAYERS", json::array({ json{{"TYPE", "DeclareLibraryItem"}, {"UID", "9090"}, {"TITLE", "CID Game"}} })}});
        writeJson(bundle + "/variant.json", json{{"NODE_ID", "cidvariant"}, {"PARENTS", json::array({"cidtile"})},
            {"LAYERS", json::array({ json{{"TYPE", "DeclareExec"}, {"PLATFORM", "win32"}, {"CONTENTPATH", "g.exe"}} })}});

        json cfg = json{{"Settings", {{"Repositories", json::array()},
                                      {"PackageSources", json::array({ json{{"CID", "QmSourceFolderCID"}, {"NAME", "mysource"}} })}}}};

        const auto srcDirs = PackageCatalog::PackageSourceDirs(cfg);
        QCOMPARE((int)srcDirs.size(), 1);                                   // the existing source dir is a scan root
        QVERIFY(PackageCatalog::IsPackageSourcePath(cfg, bundle.toStdString()));
        QVERIFY(!PackageCatalog::IsLocalPackagePath(cfg, bundle.toStdString()));   // managed, NOT local (no LOCAL badge)

        NodeIndex idx = PackageCatalog::BuildCatalogIndex(cfg);
        const Node * v = idx.Find("cidvariant");
        QVERIFY(v != nullptr);                                             // indexed via the CID-source root
        QVERIFY(v->Presentable());                                         // grouped under its tile
        QCOMPARE(v->GameKey(), std::string("cidtile"));
    }

    // A PER-PACKAGE CID: the fetched source dir is ITSELF a bundle (node JSON at its top level, no package subdirs).
    // SyncPackageSources must index it as one package whose PATH is the source dir itself (not a subdir) — this is what
    // makes each package individually addable by its own folder CID.
    void single_package_cid_source_indexed_as_one_package()
    {
        QTemporaryDir data; QVERIFY(data.isValid());
        AppPaths::SetDataRoot(data.path().toStdString());

        // The already-fetched source dir holds the bundle files directly (no wrapping subdir).
        const QString dir = data.path() + "/CIDPACKAGES/solopkg";
        QDir().mkpath(dir);
        writeJson(dir + "/tile.json", json{{"NODE_ID", "solotile"},
            {"LAYERS", json::array({ json{{"TYPE", "DeclareLibraryItem"}, {"UID", "7777"}, {"TITLE", "Solo Game"}} })}});
        writeJson(dir + "/variant.json", json{{"NODE_ID", "solovariant"}, {"PARENTS", json::array({"solotile"})},
            {"LAYERS", json::array({ json{{"TYPE", "DeclareExec"}, {"PLATFORM", "win32"}, {"CONTENTPATH", "g.exe"}} })}});

        json cfg = json{{"Settings", {{"Repositories", json::array()},
                                      {"PackageSources", json::array({ json{{"CID", "QmSoloPackageCID"}, {"NAME", "solopkg"}} })}}}};

        const int indexed = PackageCatalog::SyncPackageSources(cfg);
        QCOMPARE(indexed, 1);                                              // one launchable package
        QCOMPARE((int)cfg["LIBRARY"].size(), 1);
        QVERIFY(!cfg["LIBRARY"][0].value("PACKAGEUID", std::string()).empty());
        QCOMPARE(cfg["LIBRARY"][0].value("PATH", std::string()), dir.toStdString());   // PATH is the source dir itself
        QCOMPARE(cfg["LIBRARY"][0].value("CIDSOURCE", std::string()), std::string("QmSoloPackageCID"));

        NodeIndex idx = PackageCatalog::BuildCatalogIndex(cfg);
        QVERIFY(idx.Find("solovariant") != nullptr);                       // launchable via the per-package CID root
    }

    // Add/remove a package source: add dedups on CID; remove drops the config entry, deletes the dir, and drops LIBRARY
    // entries under it.
    void add_remove_package_source()
    {
        QTemporaryDir data; QVERIFY(data.isValid());
        AppPaths::SetDataRoot(data.path().toStdString());
        json cfg = json{{"Settings", json::object()}};

        QVERIFY(PackageCatalog::AddPackageSource(cfg, "QmABC", "src1"));
        QVERIFY(!PackageCatalog::AddPackageSource(cfg, "QmABC", "again"));   // dedup on CID
        QVERIFY(!PackageCatalog::AddPackageSource(cfg, "", "empty"));        // empty CID rejected
        QCOMPARE((int)cfg["Settings"]["PackageSources"].size(), 1);

        // Simulate a fetched dir + a LIBRARY entry under it, then remove.
        const QString dir = data.path() + "/CIDPACKAGES/src1";
        QDir().mkpath(dir);
        cfg["LIBRARY"] = json::array({ json{{"PACKAGEUID", "z"}, {"PATH", (dir + "/pkg").toStdString()}, {"CIDSOURCE", "QmABC"}} });

        PackageCatalog::RemovePackageSource(cfg, 0);
        QCOMPARE((int)cfg["Settings"]["PackageSources"].size(), 0);         // source gone
        QVERIFY(cfg["LIBRARY"].empty());                                    // its LIBRARY entry gone
        QVERIFY(!QDir(dir).exists());                                       // its dir deleted
    }
};

QTEST_MAIN(PackageCatalogTest)
#include "test_packagecatalog.moc"
