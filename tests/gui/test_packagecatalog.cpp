// Tests for PackageCatalog: hydration predicates (NodeHasContent / NodeHydrated — the #2 content-guard), the
// remove-flow DehydrateNode (#8/#9), per-package user settings, and runner predicates. Builds real temp bundles on
// disk so the layer-presence checks have something to resolve. IPFS node is not started, so DehydrateNode's
// unpin/drop-ref calls are graceful no-ops (we assert the filesystem side).

#include <QtTest>
#include <QTemporaryDir>

#include "packagecatalog.h"
#include "pkggraph.h"
#include "packageeditormodel.h"
#include "manifestmodel.h"
#include "nodefixture.h"
#include "runnerinstall.h"
#include "ipfswrapper.h"
#include "apppaths.h"
#include "commonutils.h"

#include <fstream>
#include <set>
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
    //The data root is PROCESS-GLOBAL and sticky, and PackageEditorModel::SaveLayout flushes GlobalConfig.JSON
    //to it — so a suite that does not claim it writes to AppPaths' fallback, which is the developer's REAL
    //~/.VidyaGod/GlobalConfig.JSON. Running a single slot by name replaced a 50 KB config (sources, CIDs,
    //friends, per-package variables) with a two-key stub, and reported PASS. A full-suite run only escaped
    //because an earlier slot happened to leave the root pointing somewhere else — order-dependent luck that
    //any reordering removes. Claim it once, for the whole binary.
    void initTestCase()
    {
        SuiteRoot = new QTemporaryDir();
        QVERIFY(SuiteRoot->isValid());
        AppPaths::SetDataRoot(SuiteRoot->path().toStdString());
    }
    void cleanupTestCase() { delete SuiteRoot; SuiteRoot = nullptr; }

    // #2: a content-less node is vacuously "hydrated" but has no content — NodeHasContent must distinguish it.
    void node_has_content_vs_contentless()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        writeJson(dir.path() + "/game.json",
                  NodeFixture::Chain("game", {NodeFixture::Content("dir", "data"), NodeFixture::Exec("win32", "g.exe")}));
        writeJson(dir.path() + "/empty.json", NodeFixture::Chain("empty", {NodeFixture::Exec("win32", "g.exe")}));
        QDir(dir.path() + "/data").mkpath(".");                 // make 'game' hydrated

        NodeIndex idx; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx);
        QVERIFY(PackageCatalog::NodeHasContent(idx, "game"));
        QVERIFY(!PackageCatalog::NodeHasContent(idx, "empty"));  // content-less → hidden from Library/Installed
        QVERIFY(PackageCatalog::NodeHydrated(idx, "game"));      // data dir present
    }

    // NodeHydrated = "everything FETCHABLE has been fetched", NOT "every backing file exists". A missing content layer
    // is un-hydrated ONLY when it carries a CID (a remote source to fetch from). A missing LOCAL-ONLY layer (no CID) is
    // not un-hydrated — there is nothing to download; a broken package is --validate-nodes' concern, not the tile's.
    void node_hydrated_tracks_fetchable_content()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        // Missing file + a CID → FETCHABLE-MISSING → un-hydrated (the download button has work to do).
        writeJson(dir.path() + "/remote.json",
                  NodeFixture::Chain("remote", {NodeFixture::ContentCid("file", "blob.bin", "Qmdeadbeef"), NodeFixture::Exec("win32", "g.exe")}));
        // Missing file + NO CID → local-only, nothing to fetch → hydrated (broken-ness surfaces via validation).
        writeJson(dir.path() + "/localonly.json",
                  NodeFixture::Chain("localonly", {NodeFixture::Content("file", "solo.bin"), NodeFixture::Exec("win32", "g.exe")}));

        NodeIndex idx0; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx0);
        QVERIFY(!PackageCatalog::NodeHydrated(idx0, "remote"));     // CID-backed blob.bin not fetched yet
        QVERIFY(PackageCatalog::NodeHydrated(idx0, "localonly"));   // no CID → nothing fetchable is missing

        writeFile(dir.path() + "/blob.bin");                        // the fetch lands the file locally
        NodeIndex idx1; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx1);
        QVERIFY(PackageCatalog::NodeHydrated(idx1, "remote"));      // now fully fetched
    }

    // #8/#9: DehydrateNode deletes the content-layer files (keeping the manifest) and reports what it removed.
    void dehydrate_node_removes_content_keeps_manifest()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        writeJson(dir.path() + "/game.json",
                  NodeFixture::Chain("game", {NodeFixture::Content("file", "blob.bin"), NodeFixture::Exec("win32", "g.exe")}));
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
        writeJson(dir.path() + "/game.json",
                  NodeFixture::Chain("game", {NodeFixture::Tile("g"), NodeFixture::Exec("win32", "g.exe")}, {"content"}));
        writeJson(dir.path() + "/content.json",
                  NodeFixture::Chain("content", {NodeFixture::ContentCid("zip", "game.zip", "CID_GAME")}));
        // Runner with a dehydrated build (its build is a PARENT content node, per the runner closure).
        writeJson(dir.path() + "/wine.json",
                  NodeFixture::Chain("wine", {NodeFixture::Runner(ManifestModel::MachinePlatform(), {"win32"}, "x")},
                                     {"winebuild"}));
        writeJson(dir.path() + "/winebuild.json",
                  NodeFixture::Chain("winebuild", {NodeFixture::ContentCid("zip", "wine.zip", "CID_WINE")}));

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

    // Hydration asks the LAYER whether it is runtime-sourced, never the resolved absolute path. Two ways the
    // path-derived version got it wrong, both of which report an un-fetched layer as HYDRATED — so the download
    // button never appears and the game launches against a hole:
    //   * a URL-escaped filename, which the predicate deliberately treats as real content; and
    //   * a library root that itself contains a '%' segment, which poisoned every layer beneath it.
    void hydrationAsksTheLayerNotTheResolvedPath()
    {
        auto missingWithCid = [&](const QString &dir, const char *name) {
            writeJson(dir + "/game.json",
                      NodeFixture::Chain("game", {NodeFixture::Tile("g"), NodeFixture::Exec("win32", "g.exe")},
                                         {"content"}));
            writeJson(dir + "/content.json",
                      NodeFixture::Chain("content", {NodeFixture::ContentCid("zip", name, "CID_GAME")}));
            NodeIndex idx; ManifestModel::ScanBundleNodes(dir.toStdString(), idx);
            return PackageCatalog::NodeHydrated(idx, "game");      // file ABSENT + a CID => NOT hydrated
        };

        QTemporaryDir plain; QVERIFY(plain.isValid());
        QVERIFY(!missingWithCid(plain.path(), "game.zip"));                  // the control

        QTemporaryDir escaped; QVERIFY(escaped.isValid());
        QVERIFY(!missingWithCid(escaped.path(), "100%25%20done.zip"));       // an escaped NAME is content

        // A bundle whose own directory carries a '%' segment: nothing about the LAYER changed.
        QTemporaryDir root; QVERIFY(root.isValid());
        const QString odd = root.path() + "/My %Games%";
        QVERIFY(QDir().mkpath(odd));
        QVERIFY(!missingWithCid(odd, "game.zip"));

        // ...and a genuine runtime mount is still Runtime: no file, no CID, nothing to fetch, still "hydrated".
        QTemporaryDir rt; QVERIFY(rt.isValid());
        writeJson(rt.path() + "/wine.json",
                  NodeFixture::Chain("wine", {NodeFixture::Runner(ManifestModel::MachinePlatform(), {"win32"}, "x")},
                                     {"pfx"}));
        writeJson(rt.path() + "/pfx.json",
                  NodeFixture::Chain("pfx", {NodeFixture::Content("dir", "%DefaultPfxDir%")}));
        writeJson(rt.path() + "/game.json",
                  NodeFixture::Chain("game", {NodeFixture::Tile("g"), NodeFixture::Exec("win32", "g.exe")}, {"pfx"}));
        NodeIndex ridx; ManifestModel::ScanBundleNodes(rt.path().toStdString(), ridx);
        QVERIFY(PackageCatalog::NodeHydrated(ridx, "game"));
    }

    // THE install gate, against the shape the flat schema actually produces. A prefix-generating runner's
    // assembly mounts ("%DefaultPfxDir%") used to sit on the runner node itself, where "skip the root" hid them;
    // they are now ordinary Content nodes INSIDE the closure, and they resolve to <bundle>/%DefaultPfxDir% —
    // a path that never exists on disk. Any site that counts them as build content declares the runner
    // not-installed, which greys out Play on every game that needs it with no diagnostic.
    //
    // This asserts the CALL SITES, not the predicate: the previous test pinned IsRuntimeSourcedLayer and passed
    // while two of the five callers had never been taught to use it.
    void prefix_assembly_content_is_not_the_runners_build()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        writeJson(dir.path() + "/wine.json",
                  NodeFixture::Chain("wine", {NodeFixture::Runner(ManifestModel::MachinePlatform(), {"win32"}, "x")},
                                     {"winebuild"}));
        // The real build: present on disk.
        writeJson(dir.path() + "/winebuild.json",
                  NodeFixture::Chain("winebuild", {NodeFixture::Content("zip", "wine.zip")}, {"wine_defaultpfx"}));
        writeFile(dir.path() + "/wine.zip");
        // The prefix-assembly mount, as its own Content node — nothing on disk, by design.
        writeJson(dir.path() + "/wine_defaultpfx.json",
                  NodeFixture::Chain("wine_defaultpfx", {NodeFixture::Content("dir", "%DefaultPfxDir%")}));

        NodeIndex idx; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx);

        QVERIFY(RunnerInstall::RunnerBuildPresent(idx, "wine"));      // the build IS here
        QVERIFY(PackageCatalog::RunnerInstalled(idx, "wine"));        // ...so the runner is installed

        // ...and it is not something to download, either: a fetch target for a %variable% path would have the
        // download manager forever re-offering an already-hydrated runner.
        std::vector<IpfsWrapper::FetchTarget> targets; std::string err;
        QVERIFY(RunnerInstall::CollectRunnerNodeTargets(idx, "wine", targets, &err));
        for (const auto & t : targets)
            QVERIFY2(t.LocalPath.find('%') == std::string::npos, t.LocalPath.c_str());

        // The control: remove the REAL build and the runner must go back to not-installed, or this test would
        // pass just as well against a gate that checks nothing at all.
        QVERIFY(QFile::remove(dir.path() + "/wine.zip"));
        NodeIndex idx2; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx2);
        QVERIFY(!RunnerInstall::RunnerBuildPresent(idx2, "wine"));
        QVERIFY(!PackageCatalog::RunnerInstalled(idx2, "wine"));
    }

    // Full-closure hydrate: CollectRunnerChainTargets auto-resolves the game's runner via the PLATFORM GRAPH (no
    // manually-named runner) and pools its build — so a downloaded game is immediately playable. Same graph as above:
    // the game (PLATFORM win32) resolves to the wine runner (GUEST [win32]) whose build (CID_WINE) must be fetched.
    void hydrate_pools_resolved_runner_chain()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        writeJson(dir.path() + "/game.json",
                  NodeFixture::Chain("game", {NodeFixture::Tile("g"), NodeFixture::Exec("win32", "g.exe")}, {"content"}));
        writeJson(dir.path() + "/content.json",
                  NodeFixture::Chain("content", {NodeFixture::ContentCid("zip", "game.zip", "CID_GAME")}));
        writeJson(dir.path() + "/wine.json",
                  NodeFixture::Chain("wine", {NodeFixture::Runner(ManifestModel::MachinePlatform(), {"win32"}, "x")},
                                     {"winebuild"}));
        writeJson(dir.path() + "/winebuild.json",
                  NodeFixture::Chain("winebuild", {NodeFixture::ContentCid("zip", "wine.zip", "CID_WINE")}));

        NodeIndex idx; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx);

        json cfg = json::object();                                 // no runner pin → default platform-graph resolve
        std::vector<IpfsWrapper::FetchTarget> targets; std::string err;
        QVERIFY(PackageCatalog::CollectRunnerChainTargets(idx, "game", cfg, targets, &err));
        bool hasWine = false;
        for (const auto & t : targets) { if (t.Cid == "CID_WINE") hasWine = true; }
        QVERIFY(hasWine);                       // runner build auto-pooled purely from the game's resolved chain
    }

    // A locally-added bundle (a LIBRARY entry whose PATH is OUTSIDE any repo dir) must be indexed by the catalog —
    // BuildCatalogIndex scans LocalPackageDirs alongside the repo roots. Regression: external bundles silently never
    // showed because only repo dirs were scanned.
    void local_package_is_indexed()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());                 // an external bundle, not under any repo
        writeJson(dir.path() + "/tile.json", NodeFixture::Chain("tile", {NodeFixture::Tile("777", "My Local Game")}));
        writeJson(dir.path() + "/variant.json",
                  NodeFixture::Chain("variant", {NodeFixture::Exec("win32", "g.exe")}, {"tile"}));

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

    // A CID package source (its already-fetched dir under <DataRoot>/LIBRARY/) is indexed by BuildCatalogIndex as a
    // root, and its packages are treated as MANAGED — not badged/pruned as "local". (Simulates a fetched source; the
    // network fetch itself is covered by the Go node's TestFetchDirToPath.)
    void cid_package_source_is_managed_and_indexed()
    {
        QTemporaryDir data; QVERIFY(data.isValid());
        AppPaths::SetDataRoot(data.path().toStdString());

        const QString bundle = data.path() + "/LIBRARY/mysource/game";
        QDir().mkpath(bundle);
        writeJson(bundle + "/tile.json", NodeFixture::Chain("cidtile", {NodeFixture::Tile("9090", "CID Game")}));
        writeJson(bundle + "/variant.json",
                  NodeFixture::Chain("cidvariant", {NodeFixture::Exec("win32", "g.exe")}, {"cidtile"}));

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
        const QString dir = data.path() + "/LIBRARY/solopkg";
        QDir().mkpath(dir);
        writeJson(dir + "/tile.json", NodeFixture::Chain("solotile", {NodeFixture::Tile("7777", "Solo Game")}));
        writeJson(dir + "/variant.json",
                  NodeFixture::Chain("solovariant", {NodeFixture::Exec("win32", "g.exe")}, {"solotile"}));

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
        const QString dir = data.path() + "/LIBRARY/src1";
        QDir().mkpath(dir);
        cfg["LIBRARY"] = json::array({ json{{"PACKAGEUID", "z"}, {"PATH", (dir + "/pkg").toStdString()}, {"CIDSOURCE", "QmABC"}} });

        PackageCatalog::RemovePackageSource(cfg, 0);
        QCOMPARE((int)cfg["Settings"]["PackageSources"].size(), 0);         // source gone
        QVERIFY(cfg["LIBRARY"].empty());                                    // its LIBRARY entry gone
        QVERIFY(!QDir(dir).exists());                                       // its dir deleted
    }

    // Catalog sectioning: PackageSourceNameForPath returns the owning source's NAME for a bundle under it, or "" for a
    // path outside every source (which the catalog groups under "Local"). Each source is thus its own named section.
    void package_source_name_for_path_sections_by_source()
    {
        QTemporaryDir data; QVERIFY(data.isValid());
        AppPaths::SetDataRoot(data.path().toStdString());

        const QString aBundle = data.path() + "/LIBRARY/Alpha/game";
        const QString bBundle = data.path() + "/LIBRARY/Beta/game";
        QDir().mkpath(aBundle); QDir().mkpath(bBundle);

        json cfg = json{{"Settings", {{"PackageSources", json::array({
                            json{{"CID", "QmA"}, {"NAME", "Alpha"}}, json{{"CID", "QmB"}, {"NAME", "Beta"}} })}}}};

        QCOMPARE(PackageCatalog::PackageSourceNameForPath(cfg, aBundle.toStdString()), std::string("Alpha"));
        QCOMPARE(PackageCatalog::PackageSourceNameForPath(cfg, bBundle.toStdString()), std::string("Beta"));
        QCOMPARE(PackageCatalog::PackageSourceNameForPath(cfg, (data.path() + "/Elsewhere/pkg").toStdString()), std::string());
    }

    // Regression: a node-native cover lives on the DeclareLibraryItem LAYER's COVER field (post-Declare* refactor), NOT
    // a top-level META.COVER. SeedTargets must collect it (else covers were silently skipped by --seed/re-publish).
    void seed_targets_finds_declarelibraryitem_cover_and_layers()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        const QString bundle = d.path() + "/game";
        QDir().mkpath(bundle);
        // The content file + cover file must exist on disk (SeedTargets only returns present files).
        { std::ofstream f((bundle + "/game.zip").toStdString()); f << "content"; }
        { std::ofstream f((bundle + "/cover.png").toStdString()); f << "img"; }

        json tile = NodeFixture::Tile("1", "G");
        tile["COVER"] = json{{"PATH", "cover.png"}, {"SOURCE", {{"TYPE", "ipfs"}, {"CID", "QmCoverCID"}}}};
        writeJson(bundle + "/node.json",
                  NodeFixture::Chain("g", {NodeFixture::ContentCid("zip", "game.zip", "QmLayerCID"), tile}));

        const auto all = PackageCatalog::SeedTargets(d.path().toStdString(), /*CoversOnly=*/false);
        QCOMPARE(all.at((bundle + "/game.zip").toStdString()), std::string("QmLayerCID"));
        QCOMPARE(all.at((bundle + "/cover.png").toStdString()), std::string("QmCoverCID"));   // the bug: cover was missed
        QCOMPARE((int)all.size(), 2);

        const auto covers = PackageCatalog::SeedTargets(d.path().toStdString(), /*CoversOnly=*/true);
        QCOMPARE((int)covers.size(), 1);                                                       // covers-only skips layers
        QCOMPARE(covers.at((bundle + "/cover.png").toStdString()), std::string("QmCoverCID"));
    }

    // ---- PublishPackage: the one operation whose mistakes travel ----
    //
    // A publish that skips something still returns true and still mints a CID. Every peer then fetches a package
    // that looks healthy and is not, and the gap surfaces days later as "this game is missing files" on a machine
    // that is not the author's. Both quiet skips below are now reported; these pin that they are.

    void publish_reports_an_unparseable_fragment_instead_of_dropping_it()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        const QString bundle = d.path() + "/game";
        QDir().mkpath(bundle);
        writeJson(bundle + "/good.json", NodeFixture::Chain("g", {}));   // pure composition: TYPE "Group"
        writeFile(bundle + "/broken.json", "{ \"NODE_ID\": \"b\", oops not json");

        std::vector<std::string> Errors;
        SetLogCallback([&](LogLevel L, const std::string &, const std::string &M){
            if (L == LogLevel::ERR) Errors.push_back(M); });
        (void)PackageCatalog::PublishPackage(bundle.toStdString(), std::string(), nullptr);
        ClearLogCallback();

        bool Named = false, Summarised = false;
        for (const auto &E : Errors)
        {
            if (E.find("broken.json") != std::string::npos) Named = true;
            if (E.find("PUBLISHED WITH GAPS") != std::string::npos) Summarised = true;
        }
        QVERIFY2(Named, "the fragment that would not parse must be named — it is absent from the published package");
        QVERIFY2(Summarised, "and the publish must end saying the CID will look healthy while content is missing");
    }

    // A layer with neither a CID nor a local file is published as a reference to bytes that exist nowhere: the
    // download reports nothing to fetch and the game is simply missing files on every machine but this one.
    void publish_reports_a_layer_that_is_neither_addressed_nor_present()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        const QString bundle = d.path() + "/game";
        QDir().mkpath(bundle);
        writeJson(bundle + "/node.json", NodeFixture::Chain("g", {NodeFixture::Content("zip", "never_existed.zip")}));

        std::vector<std::string> Errors;
        SetLogCallback([&](LogLevel L, const std::string &, const std::string &M){
            if (L == LogLevel::ERR) Errors.push_back(M); });
        (void)PackageCatalog::PublishPackage(bundle.toStdString(), std::string(), nullptr);
        ClearLogCallback();

        bool Found = false;
        for (const auto &E : Errors)
            if (E.find("UNFETCHABLE") != std::string::npos && E.find("never_existed.zip") != std::string::npos)
                Found = true;
        QVERIFY2(Found, "an unaddressed, absent layer must be named before it is published");
    }

    // The counterpart that keeps the noise meaningful: a package with nothing wrong must publish silently.
    void a_healthy_publish_reports_no_gaps()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        const QString bundle = d.path() + "/game";
        QDir().mkpath(bundle);
        writeJson(bundle + "/node.json",
                  NodeFixture::Chain("g", {NodeFixture::ContentCid("zip", "already.zip", "QmAlreadyAddressed")}));

        std::vector<std::string> Errors;
        SetLogCallback([&](LogLevel L, const std::string &, const std::string &M){
            if (L == LogLevel::ERR) Errors.push_back(M); });
        (void)PackageCatalog::PublishPackage(bundle.toStdString(), std::string(), nullptr);
        ClearLogCallback();

        for (const auto &E : Errors)
            QVERIFY2(E.find("PUBLISHED WITH GAPS") == std::string::npos,
                     "an already-addressed layer is the idempotent case and must not be reported as a gap");
    }

    // ---- StampNodePositions: the layout the author sees is what a peer receives -------------------------
    // Positions are the one thing allowed to enter the package's bytes only at publish time. These pin the
    // three properties that makes that safe: it writes POS for everything, it bakes the CURRENT layout (a
    // local drag beats the node's old POS), and it is a no-op on an unchanged bundle — because a stamp that
    // rewrote bytes every run would mint a new CID, and re-download the package for every peer, for nothing.

    void stamp_writes_POS_for_every_node()
    {
        QTemporaryDir Dir;
        QVERIFY(Dir.isValid());
        for (int I = 0; I < 5; ++I)
        {
            json N;
            N["NODE_ID"] = "n" + std::to_string(I);
            N["TYPE"]    = "Group";
            if (I > 0) N["PARENTS"] = json::array({"n" + std::to_string(I - 1)});
            writeJson(Dir.filePath(QString("n%1.json").arg(I)), N);
        }
        std::string Err;
        QVERIFY2(PackageCatalog::StampNodePositions(Dir.path().toStdString(), nullptr, &Err), Err.c_str());

        std::set<std::pair<double, double>> Seen;
        for (int I = 0; I < 5; ++I)
        {
            std::ifstream In(Dir.filePath(QString("n%1.json").arg(I)).toStdString());
            json J; In >> J;
            QVERIFY(J.contains("POS"));
            QVERIFY(J["POS"].is_array() && J["POS"].size() == 2);
            Seen.insert({J["POS"][0].get<double>(), J["POS"][1].get<double>()});
        }
        QCOMPARE(Seen.size(), (size_t)5);          // distinct: nothing stacked on the origin
    }

    void stamp_is_a_no_op_the_second_time()
    {
        QTemporaryDir Dir;
        QVERIFY(Dir.isValid());
        json A; A["NODE_ID"] = "a"; A["TYPE"] = "Group";
        json B; B["NODE_ID"] = "b"; B["TYPE"] = "Group"; B["PARENTS"] = json::array({"a"});
        writeJson(Dir.filePath("a.json"), A);
        writeJson(Dir.filePath("b.json"), B);

        std::string Err;
        QVERIFY(PackageCatalog::StampNodePositions(Dir.path().toStdString(), nullptr, &Err));
        auto Read = [&](const char *F) {
            std::ifstream In(Dir.filePath(F).toStdString());
            return std::string((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
        };
        const std::string A1 = Read("a.json"), B1 = Read("b.json");
        //Byte equality alone proves nothing here — rewriting the same content produces the same bytes, so it
        //holds even if the skip is gone. The property worth pinning is that an unchanged node is NOT WRITTEN
        //at all, so assert the mtime too, after letting the clock move far enough to tell.
        const QDateTime AT = QFileInfo(Dir.filePath("a.json")).lastModified();
        QTest::qSleep(1100);
        QVERIFY(PackageCatalog::StampNodePositions(Dir.path().toStdString(), nullptr, &Err));
        QCOMPARE(Read("a.json"), A1);              // byte-identical ⇒ same Meta-CID on republish
        QCOMPARE(Read("b.json"), B1);
        QCOMPARE(QFileInfo(Dir.filePath("a.json")).lastModified(), AT);   // and untouched on disk
    }

    void stamp_bakes_a_local_drag_over_the_existing_POS()
    {
        QTemporaryDir Dir;
        QVERIFY(Dir.isValid());
        json A; A["NODE_ID"] = "a"; A["TYPE"] = "Group"; A["POS"] = json::array({10.0, 20.0});
        writeJson(Dir.filePath("a.json"), A);

        json Local = json::object();
        Local["a"] = json::array({777.0, 888.0});   // the author dragged it; that is the current layout
        std::string Err;
        QVERIFY(PackageCatalog::StampNodePositions(Dir.path().toStdString(), &Local, &Err));

        std::ifstream In(Dir.filePath("a.json").toStdString());
        json J; In >> J;
        QCOMPARE(J["POS"][0].get<double>(), 777.0);
        QCOMPARE(J["POS"][1].get<double>(), 888.0);
    }

    void stamp_handles_a_file_holding_an_array_of_nodes()
    {
        //Grouping nodes into one file is presentation the editor preserves, so the stamp has to reach into an
        //array rather than skipping it — a skipped file is a package that publishes half a layout.
        QTemporaryDir Dir;
        QVERIFY(Dir.isValid());
        json Arr = json::array();
        for (int I = 0; I < 3; ++I)
        { json N; N["NODE_ID"] = "m" + std::to_string(I); N["TYPE"] = "Group"; Arr.push_back(N); }
        writeJson(Dir.filePath("many.json"), Arr);

        std::string Err;
        QVERIFY(PackageCatalog::StampNodePositions(Dir.path().toStdString(), nullptr, &Err));
        std::ifstream In(Dir.filePath("many.json").toStdString());
        json J; In >> J;
        QCOMPARE(J.size(), (size_t)3);
        for (const auto &N : J) QVERIFY(N.contains("POS"));
    }

    // The WIRING, not the function. Every other stamp test calls StampNodePositions with a hand-built override,
    // so when the editor's key and publishing's lookup drifted apart — which they did — nothing failed: the
    // lookup simply missed and the author's whole arrangement was replaced by the algorithm's default at the
    // one moment it was supposed to be preserved. This asserts the two agree on the same bundle.
    void theEditorsLayoutKeyIsTheOnePublishingLooksUp()
    {
        QTemporaryDir Dir;
        QVERIFY(Dir.isValid());
        const QString Bundle = Dir.filePath("[999][v1.0] Keyed");
        QVERIFY(QDir().mkpath(Bundle));
        json A; A["NODE_ID"] = "a"; A["TYPE"] = "Group";
        writeJson(Bundle + "/a.json", A);

        // What the editor writes, through the editor's own path.
        json Cfg = json::object();
        PackageEditorModel Model(&Cfg, nullptr);
        Model.initPackage(Bundle, nullptr);
        PkgGraph::SetPos(Model.layout(), "a", 4242.0, 2424.0);
        Model.SaveLayout();
        QVERIFY2(Cfg.contains("EDITORLAYOUT"), "the editor wrote no layout at all");

        // What publishing reads, through publishing's own path.
        // Through the SAME function the publisher calls, and — critically — with the path SHAPES the publisher
        // actually produces. An already-absolute, already-normal path makes both sides equal by construction,
        // so the test would pass with EditorLayoutKey implemented as `return BundleDir.string();` and could not
        // see a relative root (--remint-library LIBRARY), a trailing slash, or a "/./" segment.
        for (const std::string &Shape : { Bundle.toStdString(),
                                          Bundle.toStdString() + "/",
                                          Bundle.toStdString() + "/./",
                                          std::filesystem::relative(Bundle.toStdString()).string() })
        {
            if (Shape.empty()) continue;   // relative() yields "" across filesystems; not a case we can force
            QVERIFY2(PackageCatalog::EditorLayoutFor(Cfg, std::filesystem::path(Shape)) != nullptr,
                     qPrintable(QString("publishing finds nothing for bundle path shape '%1'")
                                .arg(QString::fromStdString(Shape))));
        }
        const json *Local = PackageCatalog::EditorLayoutFor(Cfg, std::filesystem::path(Bundle.toStdString()));
        QVERIFY2(Local != nullptr,
                 "publishing's own lookup finds nothing for the bundle the editor just wrote");
        QCOMPARE((*Local)["a"][0].get<double>(), 4242.0);

        // ...and end to end: the drag must reach POS.
        std::string Err;
        QVERIFY2(PackageCatalog::StampNodePositions(Bundle.toStdString(), Local, &Err), Err.c_str());
        std::ifstream In((Bundle + "/a.json").toStdString());
        json J; In >> J;
        QCOMPARE(J["POS"][0].get<double>(), 4242.0);
        QCOMPARE(J["POS"][1].get<double>(), 2424.0);
    }

    // The two properties the publish split exists for. Both were unpinned: re-inserting the stamp into
    // PublishPackage, or dropping the has-a-layout gate, left the whole suite green — and four consecutive
    // fix-ups in this changeset each introduced a regression, so the fixes themselves get the tests.

    void publishingDoesNotWritePositionsIntoNodeFiles()
    {
        QTemporaryDir Dir;
        QVERIFY(Dir.isValid());
        json A; A["NODE_ID"] = "a"; A["TYPE"] = "Group";
        writeJson(Dir.filePath("a.json"), A);

        std::string Err;
        // Dehydrated destination empty = publish in place, which is what the IPFS tab and --publish do.
        (void)PackageCatalog::PublishPackage(Dir.path().toStdString(), std::string(), &Err);

        std::ifstream In(Dir.filePath("a.json").toStdString());
        json J; In >> J;
        QVERIFY2(!J.contains("POS"),
                 "PublishPackage stamped a layout — it is reached for bundles fetched from other people, "
                 "whose bytes must keep matching the CID that serves them");
    }

    // Pins the LOOKUP CONTRACT the two authoring gates are built on: nullptr for a bundle nobody arranged,
    // non-null once one exists. It does NOT pin the gates themselves — removing `if (Local)` from
    // packageeditor.cpp or packagecatalog_publish.cpp leaves this green, because reaching either needs a live
    // PackageEditor or a RemintLibrary run (IPFS). The PublishPackage half IS pinned, by
    // publishingDoesNotWritePositionsIntoNodeFiles; the editor-side gate is currently unguarded.
    void theLayoutLookupIsNullUntilSomethingIsArranged()
    {
        QTemporaryDir Dir;
        QVERIFY(Dir.isValid());
        json Cfg = json::object();
        QCOMPARE(PackageCatalog::EditorLayoutFor(Cfg, std::filesystem::path(Dir.path().toStdString())),
                 (const json *)nullptr);

        Cfg["EDITORLAYOUT"] = json::object();
        Cfg["EDITORLAYOUT"][PackageCatalog::EditorLayoutKey(
            std::filesystem::path(Dir.path().toStdString()))] = json{{"a", json::array({1.0, 2.0})}};
        QVERIFY(PackageCatalog::EditorLayoutFor(Cfg, std::filesystem::path(Dir.path().toStdString())) != nullptr);
    }

private:
    QTemporaryDir *SuiteRoot = nullptr;
};

QTEST_MAIN(PackageCatalogTest)
#include "test_packagecatalog.moc"
