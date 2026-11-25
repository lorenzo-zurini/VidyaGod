// Tests for AppModel — the GUI state/signal hub: card-size persistence, repository add validation (#1 dup/empty),
// and removePackage for a local/portable package (#E28: drop the reference, never touch the user's files).
// Uses a temp data dir; the IPFS node is not started (the dehydrate path's unpin/drop-ref are no-ops).

#include <QtTest>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>

#include "appmodel.h"
#include "jsonoperations.h"
#include "cid.h"
#include "peersharing.h"
#include "packagecatalog.h"
#include "platform/platform.h"
#include <filesystem>
#include "apppaths.h"
#include "nodefixture.h"
#include "ipfswrapper.h"
#include "downloadqueue.h"

#include <fstream>
#include <thread>
#include <atomic>
#include <QSignalSpy>

using json = nlohmann::ordered_json;

class AppModelTest : public QObject
{
    Q_OBJECT
private slots:
    // The package editor's Publish publishes its package alone; one asked for while a publish runs goes in the next
    // pass (never lost, never two at once), and each pass puts only its own package's entries in the share sheet.
    // Republish-all still replaces the sheet whole (a deleted package's entry goes). Teeth: lose what was asked
    // meanwhile (one signal, B missing); run every pass over the whole library (the first pass reports 2 packages);
    // merge on republish-all (the gone entry stays).
    void a_package_publish_merges_and_one_asked_meanwhile_runs_after()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        std::string Err;
        QVERIFY2(IpfsWrapper::StartNode((d.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        const std::string Lib = d.path().toStdString() + "/LIB";
        for (const char *P : {"[1] A", "[2] B"})
        {
            std::filesystem::create_directories(Lib + "/VidyaGod/" + P);
            NodeFixture::WriteNodes(Lib + "/VidyaGod/" + P + "/n.json",
                                    NodeFixture::Chain(std::string("h") + P[1], {NodeFixture::Exec("win32", std::string(P) + ".exe")}));
        }
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", Lib}}}}}};
        {
            AppModel m(&cfg, &appDir);
            QSignalSpy Done(&m, &AppModel::libraryPublished), Failed(&m, &AppModel::libraryPublishFailed);
            m.publishPackage(QString::fromStdString(Lib + "/VidyaGod/[1] A"));
            QVERIFY(m.publishing());
            m.publishPackage(QString::fromStdString(Lib + "/VidyaGod/[2] B"));   // asked meanwhile
            QTRY_COMPARE_WITH_TIMEOUT(Done.count() + Failed.count(), 2, 120000);
            QCOMPARE(Failed.count(), 0);
            QVERIFY(!m.publishing());
            QCOMPARE(Done.at(0).at(1).toString(), QString("1 package(s)"));
            QCOMPARE((int)cfg["ShareSheet"].size(), 2);
            QCOMPARE((int)cfg["Pins"].size(), 2);
            cfg["ShareSheet"]["bafkreigone"] = "VidyaGod/[9] Gone/gone.json";
            m.publishLibraries();
            QTRY_COMPARE_WITH_TIMEOUT(Done.count() + Failed.count(), 3, 120000);
            QCOMPARE(Failed.count(), 0);
            QCOMPARE((int)cfg["ShareSheet"].size(), 2);
        }
        IpfsWrapper::StopNode();
    }

    void set_card_pixel_width_persists_and_signals()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}};
        AppModel m(&cfg, &appDir);

        QSignalSpy spy(&m, &AppModel::cardSizeChanged);
        m.setCardPixelWidth(250);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(cfg["Settings"].value("CardPixelWidth", 0), 250);

        m.setCardPixelWidth(250);          // unchanged → no signal
        QCOMPARE(spy.count(), 1);
    }

    // What a peer receives is closed over what the shared packages contain: share "Games" and the library package a
    // game contains comes too, as its own package under its own library (never copied into the game); a package nothing
    // shared contains stays home; a shared library with nothing published still goes out, empty. Teeth: push only the
    // explicit libraries (drop the SharedWith closure) and DxWnd is missing; drop the empty entry and "Later" is.
    void a_peer_receives_the_packages_their_share_contains()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        const std::string Lib = d.path().toStdString() + "/LIB";
        auto put = [&](const std::string &Pkg, const json &N) {               // a published node, as it lies on disk
            std::filesystem::create_directories(Lib + "/" + Pkg);
            std::ofstream(Lib + "/" + Pkg + "/" + N.value("LABEL", std::string()) + ".json", std::ios::binary) << Cid::Canonical(N);
            return Cid::OfNode(N);
        };
        const json Dx = json{{"LABEL", "dxwnd"}, {"LAYERS", json::array({ json{{"DIR", "d"}} })}};
        const json Unused = json{{"LABEL", "unused"}, {"LAYERS", json::array({ json{{"DIR", "u"}} })}};
        const std::string DxC = put("Libs/DxWnd", Dx), UC = put("Libs/Unused", Unused);
        const json Game = json{{"LABEL", "wipeout"}, {"LAYERS", json::array({ json{{"NODE", DxC}}, json{{"DIR", "w"}} })}};
        const std::string GC = put("Games/Wipeout", Game);
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", Lib}}}}},
            {"ShareSheet", {{GC, "Games/Wipeout/wipeout.json"}, {DxC, "Libs/DxWnd/dxwnd.json"}, {UC, "Libs/Unused/unused.json"}}},
            {"Sharing", {{"PeerA", json::array({"Games", "Later"})}}}};
        AppModel m(&cfg, &appDir);
        m.rebuildCatalog();
        const json S = m.peers().sharedWith("PeerA");
        QCOMPARE((int)S["Games"].size(), 1);
        QVERIFY2(S.contains("Libs") && S["Libs"].size() == 1 && S["Libs"][0]["pkg"] == "DxWnd", S.dump().c_str());
        QCOMPARE(S["Libs"][0].value("node", std::string()), std::string("dxwnd.json"));
        QVERIFY2(S.contains("Later") && S["Later"].empty(), "an explicitly shared library with nothing published goes out empty");
        QVERIFY2(m.peers().sharedWith("PeerB").empty(), "nothing is shared with a peer nobody shared with");
        // The game edited since it was published (saved, not published yet): the index has moved on, and the sheet still
        // names what was published. What it contains is read from its published block, held here. Teeth: read the
        // index alone (DxWnd drops out of the share).
        std::string Err;
        QVERIFY2(IpfsWrapper::StartNode((d.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QCOMPARE(IpfsWrapper::BlockPut(Cid::Canonical(Game), &Err), GC);         // what publishing stored
        json Edited = Game;
        Edited["COMMENT"] = "edited, not published";
        std::filesystem::remove(Lib + "/Games/Wipeout/wipeout.json");
        put("Games/Wipeout", Edited);
        m.rebuildCatalog();
        QVERIFY2(!m.catalogIndex().Find(GC) || m.catalogIndex().Find(GC)->Key() != GC, "precondition: the index has moved on");
        const json S2 = m.peers().sharedWith("PeerA");
        QVERIFY2(S2.contains("Libs") && S2["Libs"].size() == 1 && S2["Libs"][0]["pkg"] == "DxWnd", S2.dump().c_str());
        IpfsWrapper::StopNode();
    }

    // Upgrading an unknown source must fail through packageSourceFailed and leave no pending plan — so a later
    // applySourceUpgrade cannot act on a stale/rejected plan. Also pins the two-phase contract: planning alone
    // NEVER mutates the config (the CID must still be the old one after a plan attempt).
    void plan_source_upgrade_rejects_unknown_source_and_leaves_config_alone()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json{{"PackageSources", json::array({ json{{"NAME","Real"},{"CID","QmOld"}} })}}}};
        AppModel m(&cfg, &appDir);

        QSignalSpy failed(&m, &AppModel::packageSourceFailed);
        QSignalSpy planned(&m, &AppModel::sourceUpgradePlanned);
        m.planSourceUpgrade("Nope", "QmWhatever");
        QVERIFY(failed.wait(5000));
        QCOMPARE(planned.count(), 0);
        QCOMPARE(cfg["Settings"]["PackageSources"][0].value("CID", std::string()), std::string("QmOld"));

        // No plan was stored, so applying must refuse rather than fall through to a half-configured upgrade.
        QSignalSpy failed2(&m, &AppModel::packageSourceFailed);
        m.applySourceUpgrade(false);
        QCOMPARE(failed2.count(), 1);
        QCOMPARE(cfg["Settings"]["PackageSources"][0].value("CID", std::string()), std::string("QmOld"));
    }

    // A source sync works on a copy of the config; what it changes comes back as a change, never as its copy: an
    // import made while it ran is still there after, and the package the sync itself added is too. Teeth: assign the
    // copy's LIBRARY back (as the completions did) and the import made meanwhile is gone.
    void a_change_made_while_a_source_syncs_is_kept()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        const std::string Old = d.filePath("old").toStdString(), Imported = d.filePath("imported").toStdString();
        std::filesystem::create_directories(Old);
        std::filesystem::create_directories(Imported);
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", d.filePath("LIBRARY").toStdString()}}}}},
                        {"LIBRARY", json::array({ json{{"PATH", Old}, {"PACKAGEUID", "1"}} })}};
        //The source is already on disk (as after its first fetch): the sync reads it and adds its package to LIBRARY.
        const json Src = json{{"CID", "QmYwAPJzv5CZsnA625s3Xf2nemtYgPpHdWEz79ojWnPbdG"}, {"NAME", "Local"}};
        const std::string SrcDir = PackageCatalog::PackageSourceDir(cfg, Src);
        std::filesystem::create_directories(SrcDir);
        std::ofstream(SrcDir + "/game.json") << R"({"LABEL":"Synced","LAYERS":[{"ENV":{"A":"1"}}]})";
        std::ofstream(SrcDir + "/.vgsource") << "QmYwAPJzv5CZsnA625s3Xf2nemtYgPpHdWEz79ojWnPbdG\n.\n";                                 // its own fetch's marker: one package
        AppModel m(&cfg, &appDir);
        QSignalSpy changed(&m, &AppModel::packageSourcesChanged);
        QVERIFY(m.addPackageSource("QmYwAPJzv5CZsnA625s3Xf2nemtYgPpHdWEz79ojWnPbdG", "Local"));
        cfg["LIBRARY"].push_back(json{{"PATH", Imported}, {"PACKAGEUID", "2"}});          // an import, while the sync runs
        QTRY_VERIFY_WITH_TIMEOUT(changed.count() >= 2, 20000);                                // the sync's completion
        bool HasOld = false, HasImported = false, HasSynced = false;
        for (const auto &E : cfg["LIBRARY"])
        { HasOld |= E["PATH"] == Old; HasImported |= E["PATH"] == Imported; HasSynced |= E["PATH"] == SrcDir; }
        QVERIFY2(HasOld && HasImported && HasSynced, cfg["LIBRARY"].dump().c_str());
    }

    // The merge itself: what the worker removed goes, what it added comes, what it changed is replaced in place, and
    // what the live list gained or lost meanwhile stays. Teeth: match by key alone (the first version) and the second
    // "Lib" overwrites the first; push a changed entry that is no longer live and the removed G comes back.
    void a_worker_change_merges_into_the_live_list()
    {
        const json Before = json::array({ json{{"PATH", "a"}, {"V", 1}}, json{{"PATH", "b"}, {"V", 1}}, json{{"PATH", "c"}, {"V", 1}} });
        const json After  = json::array({ json{{"PATH", "a"}, {"V", 2}}, json{{"PATH", "c"}, {"V", 1}}, json{{"PATH", "d"}, {"V", 1}} });
        json Live = json::array({ json{{"PATH", "a"}, {"V", 1}}, json{{"PATH", "b"}, {"V", 1}}, json{{"PATH", "c"}, {"V", 1}},
                                  json{{"PATH", "mine"}, {"V", 1}} });
        PackageCatalog::MergeListChange(Live, Before, After, PackageCatalog::LibraryEntryKey);
        const json Want = json::array({ json{{"PATH", "a"}, {"V", 2}}, json{{"PATH", "c"}, {"V", 1}}, json{{"PATH", "mine"}, {"V", 1}},
                                        json{{"PATH", "d"}, {"V", 1}} });
        QCOMPARE(QString::fromStdString(Live.dump()), QString::fromStdString(Want.dump()));

        //Two sources of one name are two: upgrading "Games" touches only Games (keyed by name, matched by value).
        const json SBefore = json::array({ json{{"NAME", "Games"}, {"CID", "old"}}, json{{"NAME", "Lib"}, {"CID", "/ipns/A"}},
                                           json{{"NAME", "Lib"}, {"CID", "/ipns/B"}} });
        json SAfter = SBefore;
        SAfter[0]["CID"] = "new";
        json SLive = SBefore;
        PackageCatalog::MergeListChange(SLive, SBefore, SAfter, PackageCatalog::SourceKey);
        QCOMPARE(QString::fromStdString(SLive.dump()), QString::fromStdString(SAfter.dump()));

        //An entry removed while the worker changed it stays removed: the live removal wins.
        const json GBefore = json::array({ json{{"NAME", "G"}, {"CID", "a"}}, json{{"NAME", "H"}, {"CID", "h"}} });
        const json GAfter  = json::array({ json{{"NAME", "G"}, {"CID", "new"}}, json{{"NAME", "H"}, {"CID", "h"}} });
        json GLive = json::array({ json{{"NAME", "H"}, {"CID", "h"}} });
        PackageCatalog::MergeListChange(GLive, GBefore, GAfter, PackageCatalog::SourceKey);
        QCOMPARE(QString::fromStdString(GLive.dump()), QString::fromStdString(json::array({ json{{"NAME", "H"}, {"CID", "h"}} }).dump()));

        //Two workers that each add the same package (two syncs at once): the second finds it there, and adds no copy.
        const json Was = json::array({ json{{"PATH", "/old"}} });
        const json One = json::array({ json{{"PATH", "/old"}}, json{{"PATH", "S1/game"}} });
        const json Two = json::array({ json{{"PATH", "/old"}}, json{{"PATH", "S1/game"}}, json{{"PATH", "S2/g2"}} });
        json Lib = Was;
        PackageCatalog::MergeListChange(Lib, Was, One, PackageCatalog::LibraryEntryKey);
        PackageCatalog::MergeListChange(Lib, Was, Two, PackageCatalog::LibraryEntryKey);
        QCOMPARE(QString::fromStdString(Lib.dump()), QString::fromStdString(Two.dump()));
    }

    void remove_local_package_drops_reference_keeps_user_files()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        QTemporaryDir ext; QVERIFY(ext.isValid());                 // a package OUTSIDE the library root
        { std::ofstream f((ext.path() + "/savegame.dat").toStdString()); f << "precious"; }

        json cfg = json{{"Settings", json::object()},
                        {"LIBRARY", json::array({ json{{"PACKAGEUID", "loc"}, {"PACKAGENAME", "Local Game"},
                                                       {"PATH", ext.path().toStdString()}} })}};
        AppModel m(&cfg, &appDir);

        QSignalSpy spy(&m, &AppModel::catalogChanged);
        m.removePackage("loc");

        QCOMPARE((int)cfg["LIBRARY"].size(), 0);                    // reference dropped
        QVERIFY(QFile::exists(ext.path() + "/savegame.dat"));       // user's own files untouched (TESTPLAN E28)
        QVERIFY(spy.count() >= 1);                                  // catalog rebuilt
    }

    // What of the catalog is on disk is worked out once per rebuild and shared by the views — and a rebuild works it
    // out again: content that landed shows as installed after the rebuild that follows. Teeth: keep the map across
    // rebuilds.
    void hydration_is_the_rebuilds()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        const QString Pkg = d.path() + "/LIBRARY/VidyaGod/[1] Game";
        QDir().mkpath(Pkg);
        NodeFixture::WriteNodes((Pkg + "/game.json").toStdString(), NodeFixture::Chain("game", {NodeFixture::Tile("1", "Game"),
            NodeFixture::Exec("win32", "g.exe"), NodeFixture::ContentCid("zip", "game.zip", "bafkreigamezip")}, {}, {{"VARIANT", "Edition 1"}}));
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", (d.path() + "/LIBRARY").toStdString()}}}}}, {"LIBRARY", json::array()}};
        AppModel m(&cfg, &appDir);
        m.rebuildCatalog();
        QVERIFY(m.catalogIndex().Find("game"));
        const std::string Key = m.catalogIndex().Find("game")->Key();
        QVERIFY(m.hydration().count(Key));
        QVERIFY(!m.hydration().at(Key).Hydrated);                 // its zip is not here yet
        { std::ofstream f((Pkg + "/game.zip").toStdString()); f << "content"; }
        m.rebuildCatalog();                                       // a download's end
        QVERIFY(m.hydration().at(m.catalogIndex().Find("game")->Key()).Hydrated);
    }

    // A package of LIBRARY that the config's LIBRARY list does not name (every package since the node cutover) is not
    // removed: its dehydrate would walk into other packages' shared content and delete files no source can bring back.
    // Teeth: drop the refusal and the package's content goes (the dehydrate deletes game.zip).
    void remove_refuses_a_library_package_the_config_does_not_name()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        const QString Pkg = d.path() + "/LIBRARY/VidyaGod/[1] Game";
        QDir().mkpath(Pkg);
        { std::ofstream f((Pkg + "/game.zip").toStdString()); f << "content"; }
        NodeFixture::WriteNodes((Pkg + "/game.json").toStdString(), NodeFixture::Chain("game", {NodeFixture::Tile("1", "Game"),
            NodeFixture::Exec("win32", "g.exe"), NodeFixture::Content("zip", "game.zip")}, {}, {{"VARIANT", "Edition 1"}}));
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", (d.path() + "/LIBRARY").toStdString()}}}}}, {"LIBRARY", json::array()}};
        AppModel m(&cfg, &appDir);
        QSignalSpy Built(&m, &AppModel::catalogChanged);
        m.rebuildCatalog();
        if (Built.isEmpty()) QVERIFY(Built.wait(20000));
        QVERIFY2(m.catalogIndex().Find("game"), "the package is in the index (so a dehydrate would find it)");
        bool Async = true;
        const QString Why = m.removePackage(Pkg, &Async);
        QVERIFY2(!Why.isEmpty(), "refused, saying why");
        QVERIFY(!Async);
        QTest::qWait(1500);   // a dehydrate would run on a worker
        QVERIFY2(QFile::exists(Pkg + "/game.zip"), "its content is untouched");
        QVERIFY(!m.removePackage("1").isEmpty());   // by its UID too
        QTest::qWait(300);
        QVERIFY(QFile::exists(Pkg + "/game.zip"));
    }

    void remove_unknown_package_is_noop()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}, {"LIBRARY", json::array()}};
        AppModel m(&cfg, &appDir);
        m.removePackage("does-not-exist");                         // must not crash / throw
        QVERIFY(true);
    }

    // Per-package config lives in the INSTANCE file, not the LIBRARY entry — register must leave the entry
    // PACKAGE-FREE. (A package never names a runner; runners say which tiles they are recommended for.)
    void register_leaves_library_entry_package_free()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        QTemporaryDir pkg; QVERIFY(pkg.isValid());
        // On the shelf (a VARIANT whose Play entry carries a tile): what an import registers.
        json node = NodeFixture::Chain("tg", {NodeFixture::Merge({NodeFixture::Tile("999", "Test Game"),
                                                                  NodeFixture::Exec("win32", "g.exe"), NodeFixture::Variant("Play")})});
        { std::ofstream f((pkg.path() + "/tg.json").toStdString()); f << node[0].dump(); }

        json cfg = json{{"Settings", json::object()}, {"LIBRARY", json::array()}};
        AppModel m(&cfg, &appDir);
        auto [added, skipped] = m.importPackagesFromDir(pkg.path());
        QCOMPARE(added, 1);
        QCOMPARE((int)cfg["LIBRARY"].size(), 1);
        QVERIFY2(!cfg["LIBRARY"][0].contains("USERSETTINGS"), "the LIBRARY entry must carry no per-package config");
    }

    // The ctor applies the persisted CardPixelWidth (so a restart restores the user's zoom).
    void ctor_restores_persisted_card_width()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", {{"CardPixelWidth", 222}}}};
        AppModel m(&cfg, &appDir);
        QCOMPARE(m.cardPixelWidth(), 222);

        json def = json{{"Settings", json::object()}};            // absent → built-in default (185)
        AppModel m2(&def, &appDir);
        QCOMPARE(m2.cardPixelWidth(), 185);
    }

    // save() writes GlobalConfig.JSON into the app data dir and it reads back identically.
    void save_writes_globalconfig_to_disk()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", {{"CardPixelWidth", 199}}}, {"LIBRARY", json::array()}};
        AppModel m(&cfg, &appDir);

        QVERIFY(m.save());
        const QString path = appDir.filePath("GlobalConfig.JSON");
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(path), 5000);
        JSONOps::FlushConfigWrites();

        std::ifstream f(path.toStdString());
        json onDisk; f >> onDisk;
        QCOMPARE(onDisk["Settings"].value("CardPixelWidth", 0), 199);
    }

    // Saves asked in a burst (one per received library, per download, per pairing step) write the config once, as it
    // is after the last, and off the GUI thread. Teeth: write at every ask (as many writes as asks); drop the
    // SaveDirty check (a write after saveNow wrote it already).
    void saves_asked_together_write_the_config_once()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", {{"CardPixelWidth", 1}}}};
        AppModel m(&cfg, &appDir);
        JSONOps::FlushConfigWrites();
        const long long Before = JSONOps::DebugConfigWrites();
        for (int I = 2; I <= 40; ++I) { cfg["Settings"]["CardPixelWidth"] = I; QVERIFY(m.save()); }
        QTRY_COMPARE_WITH_TIMEOUT(JSONOps::DebugConfigWrites() - Before, 1LL, 5000);
        QTest::qWait(m.SaveSoonMs * 2);
        JSONOps::FlushConfigWrites();
        QCOMPARE(JSONOps::DebugConfigWrites() - Before, 1LL);
        std::ifstream f(appDir.filePath("GlobalConfig.JSON").toStdString());
        json onDisk; f >> onDisk;
        QCOMPARE(onDisk["Settings"].value("CardPixelWidth", 0), 40);
        // Written now (a command's save): the save still waiting has nothing left to write.
        cfg["Settings"]["CardPixelWidth"] = 41;
        QVERIFY(m.save());
        QVERIFY(m.saveNow());
        QTest::qWait(m.SaveSoonMs * 2);
        JSONOps::FlushConfigWrites();
        QCOMPARE(JSONOps::DebugConfigWrites() - Before, 2LL);
    }

    // A config written now (the editor's, a command's) is never overwritten by an older text still waiting for the
    // worker: the newer config would be lost until the next save. Teeth: drop the N <= Committed check.
    void an_older_config_text_never_overwrites_a_newer_one()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        const std::filesystem::path Dir = d.path().toStdString();
        JSONOps::DebugHoldConfigWrites(true);
        QVERIFY(JSONOps::SaveConfigAsync(json{{"V", "older"}}, Dir));
        QVERIFY(JSONOps::SaveConfig(json{{"V", "newer"}}, Dir));
        JSONOps::DebugHoldConfigWrites(false);
        JSONOps::FlushConfigWrites();
        std::ifstream f(Dir / "GlobalConfig.JSON");
        json onDisk; f >> onDisk;
        QCOMPARE(onDisk.value("V", std::string()), std::string("newer"));
    }

    // Each data folder's config is ordered on its own: a newer text of one never stops an older pending one of another
    // from being written (two models, a test's data folder beside the app's), nor replaces it while both wait. Teeth:
    // one newest-written mark for every file; one pending slot for every file.
    void a_config_write_waits_only_on_its_own_files_newer_text()
    {
        QTemporaryDir a, b; QVERIFY(a.isValid() && b.isValid());
        const std::filesystem::path A = a.path().toStdString(), B = b.path().toStdString();
        auto Read = [](const std::filesystem::path & D) { std::ifstream f(D / "GlobalConfig.JSON"); json J = json::parse(f, nullptr, false); return J.is_object() ? J.value("V", std::string()) : std::string("(none)"); };
        JSONOps::DebugHoldConfigWrites(true);
        QVERIFY(JSONOps::SaveConfigAsync(json{{"V", "a1"}}, A));
        QVERIFY(JSONOps::SaveConfig(json{{"V", "b1"}}, B));                       // newer, another file
        QVERIFY(JSONOps::SaveConfigAsync(json{{"V", "b2"}}, B));                  // both wait together
        JSONOps::DebugHoldConfigWrites(false);
        JSONOps::FlushConfigWrites();
        QCOMPARE(Read(A), std::string("a1"));
        QCOMPARE(Read(B), std::string("b2"));
    }

    // A model that goes away (the app's exit) writes what it was asked to save and had not yet. Teeth: drop the write
    // in ~AppModel (the change is lost).
    void a_model_going_away_writes_what_it_still_holds()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", {{"CardPixelWidth", 1}}}};
        {
            AppModel m(&cfg, &appDir);
            m.SaveSoonMs = 60000;
            cfg["Settings"]["CardPixelWidth"] = 77;
            QVERIFY(m.save());
        }
        std::ifstream f(appDir.filePath("GlobalConfig.JSON").toStdString());
        json onDisk; f >> onDisk;
        QCOMPARE(onDisk["Settings"].value("CardPixelWidth", 0), 77);
    }

    // removePackageSource drops the source entry, persists, rebuilds the catalog, and signals the change.
    void remove_package_source_drops_entry_and_signals()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        AppPaths::SetDataRoot(d.path().toStdString());   // keep LIBRARY deletion inside the temp dir
        QDir appDir(d.path());
        json cfg = json{{"Settings", {{"PackageSources", json::array({
                            json{{"CID", "QmA"}, {"NAME", "a"}}, json{{"CID", "QmB"}, {"NAME", "b"}} })}}},
                        {"LIBRARY", json::array()}};
        AppModel m(&cfg, &appDir);

        QSignalSpy srcs(&m, &AppModel::packageSourcesChanged);
        QSignalSpy cat(&m, &AppModel::catalogChanged);
        m.removePackageSource(0);

        QCOMPARE((int)cfg["Settings"]["PackageSources"].size(), 1);
        QCOMPARE(cfg["Settings"]["PackageSources"][0].value("CID", std::string()), std::string("QmB"));
        QVERIFY(srcs.count() >= 1);
        QVERIFY(cat.count() >= 1);

        m.removePackageSource(99);   // out-of-range → no erase, no crash
        QCOMPARE((int)cfg["Settings"]["PackageSources"].size(), 1);
    }

    // Networking is OFF by default and toggles persist Settings.IPFS.Enabled + emit networkingChanged once.
    void networking_off_by_default_and_toggles()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}};
        AppModel m(&cfg, &appDir);
        QVERIFY(!m.networkingEnabled());                  // off by default

        QSignalSpy spy(&m, &AppModel::networkingChanged);
        m.setNetworkingEnabled(true);
        QCOMPARE(spy.count(), 1);
        QVERIFY(m.networkingEnabled());
        QCOMPARE(cfg["Settings"]["IPFS"].value("Enabled", false), true);

        m.setNetworkingEnabled(true);                     // unchanged → no signal
        QCOMPARE(spy.count(), 1);
        m.setNetworkingEnabled(false);
        QCOMPARE(spy.count(), 2);
        QVERIFY(!m.networkingEnabled());
    }

    // markNodeReady emits nodeReady — the signal the Network tab needs so its peer code + Verify&Publish enable
    // refresh once the ASYNC node start finishes (networkingChanged fires at toggle time, before the node is up, so
    // without this the tab stays stuck "networking off" with Publish disabled after startup).
    void mark_node_ready_emits_node_ready()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}};
        AppModel m(&cfg, &appDir);
        QSignalSpy spy(&m, &AppModel::nodeReady);
        m.markNodeReady();
        QCOMPARE(spy.count(), 1);
    }

    // rebuildCatalog emits catalogChanged (empty config → empty index, but the signal still fires).
    void rebuild_catalog_signals()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}, {"LIBRARY", json::array()}};
        AppModel m(&cfg, &appDir);
        QSignalSpy cat(&m, &AppModel::catalogChanged);
        m.rebuildCatalog();
        QCOMPARE(cat.count(), 1);
    }

    // A peer's library snapshot REPLACES that peer's record wholesale — the core of the bilateral protocol's
    // "receiver". This is what makes a withdrawn library un-loseable: a snapshot that no longer lists a library drops
    // it, even if the specific "unshare" push was never received. An empty snapshot drops the peer entirely; an
    // identical snapshot is a no-op (no disk write, no UI refresh).
    // A peer's received stubs (CATALOG/<Nick> - <Lib>/…) are the materialisation of ONE snapshot. A changed
    // snapshot must replace them, not join them: with three publishes' worth of "v1.30.4" side by side, the index
    // held three variants of three generations and the library showed every one. An identical snapshot leaves them.
    void a_changed_snapshot_replaces_the_received_stubs()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}, {"ReceiveFrom", json::array({"12D3KooWSeeder"})}};   // we receive from them
        AppModel m(&cfg, &appDir);
        const QString peer = "12D3KooWSeeder";
        const std::string P = peer.toStdString();
        //Real nodes: a fetch the snapshot started may be at the stub's file meanwhile, and it keeps the node it asks for.
        const json Old = json{{"LABEL", "old"}, {"LAYERS", json::array({ json{{"DIR", "o"}} })}};
        const json New = json{{"LABEL", "old"}, {"LAYERS", json::array({ json{{"DIR", "n"}} })}};
        const std::string OB = Cid::Canonical(Old), NB = Cid::Canonical(New);
        const auto snap = [](const std::string &C, const std::string &Pkg = "Old Game") {
            return QString::fromStdString(json{{"Games", json::array({ json{{"cid", C}, {"pkg", Pkg}, {"node", "old.json"}} })}}.dump()); };
        m.peers().applyPeerLibrarySnapshot(peer, snap(Cid::OfBytes(OB)));
        const auto Plan = PackageCatalog::PlanReceivedFetches(cfg, P.substr(P.size() - 8), cfg["PeerLibraries"][P]);
        QVERIFY(!Plan.empty());
        const std::filesystem::path LibDir = std::filesystem::path(Plan.front().Dest).parent_path().parent_path();
        const auto land = [&](const std::string &Bytes) {                         // a landed node file: the package holds node files only
            std::filesystem::create_directories(std::filesystem::path(Plan.front().Dest).parent_path());
            std::ofstream S(Plan.front().Dest, std::ios::binary); S << Bytes; };
        land(OB);
        QVERIFY(std::filesystem::exists(Plan.front().Dest));
        // identical snapshot → stubs stay
        m.peers().applyPeerLibrarySnapshot(peer, snap(Cid::OfBytes(OB)));
        QVERIFY(std::filesystem::exists(Plan.front().Dest));
        // an INSTALLED received package lives in the same tree (its content was fetched beside its stubs): it stays,
        // whatever the snapshot says — only pure-stub package dirs go
        const std::filesystem::path Installed = LibDir / "[9] Installed Game";
        std::filesystem::create_directories(Installed);
        { std::ofstream S(Installed / "game.json"); S << "{}"; }
        { std::ofstream S(Installed / "game.zip"); S << "bytes"; }
        // a re-publish that changes the package's node: the package is still shared, so its folder stays (only what
        // changed lands again; the prune drops what its entries no longer name). Teeth: drop every stub on a change.
        m.peers().applyPeerLibrarySnapshot(peer, snap(Cid::OfBytes(NB)));
        QVERIFY(std::filesystem::is_directory(std::filesystem::path(Plan.front().Dest).parent_path()));
        // a snapshot that no longer names the package: its stubs are gone
        m.peers().applyPeerLibrarySnapshot(peer, snap(Cid::OfBytes(NB), "Other Game"));
        QVERIFY(!std::filesystem::exists(Plan.front().Dest));
        QVERIFY(!std::filesystem::exists(std::filesystem::path(Plan.front().Dest).parent_path()));
        QVERIFY(std::filesystem::exists(Installed / "game.zip"));
        QVERIFY(cfg["PeerLibraries"][P].contains("Games"));
        // an EMPTY snapshot is what a restarting seeder pushes before its library loads: a transient, nothing is dropped
        m.peers().applyPeerLibrarySnapshot(peer, snap(Cid::OfBytes(NB)));
        land(NB);
        m.peers().applyPeerLibrarySnapshot(peer, R"({})");
        QVERIFY(std::filesystem::exists(Plan.front().Dest));
        QVERIFY(std::filesystem::exists(Installed / "game.zip"));
    }

    // A changed snapshot keeps the package folders it still names, and with them the fetches already queued there —
    // but not for a file it no longer names: that path is no node's now, and a fetch still holding it would land it
    // after the prune. Here the node moved to another package (so its fetch is still wanted, elsewhere). Teeth: drop
    // the ForgetDest of a kept folder's stale file in dropReceivedStubs (the old path stays the fetch's first dest);
    // forget every file of a kept folder (b's fetch loses its path).
    void a_changed_snapshot_forgets_the_files_it_no_longer_names()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}, {"ReceiveFrom", json::array({"12D3KooWSeeder"})}};
        AppModel m(&cfg, &appDir);
        const QString peer = "12D3KooWSeeder";
        const std::string P = peer.toStdString();
        const json A = json{{"LABEL", "a"}, {"LAYERS", json::array({ json{{"DIR", "a"}} })}};
        const json B = json{{"LABEL", "b"}, {"LAYERS", json::array({ json{{"DIR", "b"}} })}};
        const std::string CA = Cid::OfNode(A), CB = Cid::OfNode(B);
        IpfsWrapper::DebugResetQueue();
        IpfsWrapper::SetFetchOnceHook([](const std::string &, const std::string &, bool, std::string *E) {
            if (E) { *E = "no providers"; } return 1; });                           // every fetch waits: retryable
        const auto item = [](const std::string &C, const std::string &Pkg, const std::string &File) {
            return json{{"cid", C}, {"pkg", Pkg}, {"node", File}}; };
        m.peers().applyPeerLibrarySnapshot(peer, QString::fromStdString(json{{"Games", json::array({
            item(CA, "Game", "a.json"), item(CB, "Game", "b.json") })}}.dump()));
        const auto Plan = PackageCatalog::PlanReceivedFetches(cfg, P.substr(P.size() - 8), cfg["PeerLibraries"][P]);
        std::string OldA, OldB;
        for (const auto &T : Plan) (T.Cid == CA ? OldA : OldB) = T.Dest;
        QVERIFY(!OldA.empty() && !OldB.empty());
        IpfsWrapper::EnqueueBatch({ IpfsWrapper::FetchTarget{ CA, OldA, false, false, /*Verify=*/true },
                                    IpfsWrapper::FetchTarget{ CB, OldB, false, false, /*Verify=*/true } });
        QCOMPARE(IpfsWrapper::QueueDestForCid(CA), OldA);
        //Node a moves to another package; Game is still shared (b), so its folder stays.
        m.peers().applyPeerLibrarySnapshot(peer, QString::fromStdString(json{{"Games", json::array({
            item(CA, "Other", "a.json"), item(CB, "Game", "b.json") })}}.dump()));
        QVERIFY(IpfsWrapper::QueueDestForCid(CA) != OldA);
        QCOMPARE(IpfsWrapper::QueueDestForCid(CB), OldB);                     // b is still planned there: its fetch stands
        IpfsWrapper::SetFetchOnceHook(nullptr);
        IpfsWrapper::DebugResetQueue();
    }

    // The 90 s orphan heal waits while the network queue fetches: every landed block writes a reference, and the check
    // read the whole reference index again each time. Idle, it checks — and a fetch retried for ever (a peer gone) leaves
    // it idle between tries. Teeth: heal while the queue fetches; count a retry's backoff as fetching (no heal all
    // session).
    void the_orphan_heal_waits_while_the_queue_fetches()
    {
        std::string Err;
        QTemporaryDir d; QVERIFY(d.isValid());
        QVERIFY2(IpfsWrapper::StartNode((d.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}};
        AppModel m(&cfg, &appDir);
        IpfsWrapper::DebugResetQueue();
        std::atomic<bool> Release{false};
        IpfsWrapper::SetFetchOnceHook([&](const std::string &, const std::string &, bool, std::string *E) {
            while (!Release) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (E) *E = "gone";
            return 2; });
        const auto H = IpfsWrapper::EnqueueBatch({ IpfsWrapper::FetchTarget{ "CID_HEAL_WAITS", (d.path() + "/x").toStdString(), true } });
        QTRY_VERIFY_WITH_TIMEOUT(IpfsWrapper::QueueBusy(), 5000);
        const long long Scans = IpfsWrapper::DebugRefScans();
        m.healOrphansIfAny();
        QTest::qWait(1000);
        QCOMPARE(IpfsWrapper::DebugRefScans(), Scans);                            // fetching: no check
        Release = true;
        IpfsWrapper::WaitBatch(H, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!IpfsWrapper::QueueBusy(), 10000);
        m.healOrphansIfAny();
        QTRY_VERIFY_WITH_TIMEOUT(IpfsWrapper::DebugRefScans() > Scans, 10000);    // idle: it checks
        // a fetch that keeps failing (its peer gone) waits out a backoff between tries: the queue is idle meanwhile
        IpfsWrapper::SetFetchOnceHook([](const std::string &, const std::string &, bool, std::string *E) {
            if (E) *E = "no providers"; return 1; });
        const auto Stuck = IpfsWrapper::EnqueueBatch({ IpfsWrapper::FetchTarget{ "CID_HEAL_BACKOFF", (d.path() + "/y").toStdString(), true } });
        QTRY_VERIFY_WITH_TIMEOUT(!IpfsWrapper::QueueBusy(), 10000);
        { std::ofstream(d.path().toStdString() + "/z.bin") << "z"; }                // a reference dropped: the check has a
        const std::string Z = IpfsWrapper::AddNoCopy(d.path().toStdString() + "/z.bin", &Err);   // new index to read
        QVERIFY(!Z.empty());                                                       // (a written one only adds its file)
        QVERIFY(IpfsWrapper::DropRef(Z));
        //(another model's first heal: this one's is still in its once-a-run seed pass, which waits for a quiet network)
        AppModel m2(&cfg, &appDir);
        const long long Before = IpfsWrapper::DebugRefScans();
        m2.healOrphansIfAny();
        QTRY_VERIFY_WITH_TIMEOUT(IpfsWrapper::DebugRefScans() > Before, 10000);   // a retry's backoff holds nothing up
        (void)Stuck;
        IpfsWrapper::SetFetchOnceHook(nullptr);
        IpfsWrapper::DebugResetQueue();
        IpfsWrapper::StopNode();
    }

    // Asked for a rebuild many times at once (downloads ending together), the catalog is rebuilt once; rebuilt at once
    // meanwhile (the last download's own), the ask is covered — and an ask after that rebuild is not. Teeth: rebuild at
    // each ask; rebuild again after one; judge "rebuilt meanwhile" from the first ask (a later one skipped).
    void rebuilds_asked_together_are_one()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}};
        AppModel m(&cfg, &appDir);
        QSignalSpy cat(&m, &AppModel::catalogChanged);
        for (int I = 0; I < 10; ++I) m.rebuildCatalogSoon();
        QTRY_COMPARE_WITH_TIMEOUT(cat.count(), 1, 5000);
        QTest::qWait(600);
        QCOMPARE(cat.count(), 1);
        m.rebuildCatalogSoon();                                                    // a later ask: another rebuild
        QTRY_COMPARE_WITH_TIMEOUT(cat.count(), 2, 5000);
        m.rebuildCatalogSoon();                                                    // asked, then rebuilt at once: that one
        m.rebuildCatalog();                                                        // covers the ask
        QTest::qWait(600);
        QCOMPARE(cat.count(), 3);
        m.rebuildCatalogSoon();                                                    // asked, rebuilt, asked again before the
        m.rebuildCatalog();                                                        // first ask's time: the second ask is
        m.rebuildCatalogSoon();                                                    // its own
        QTRY_COMPARE_WITH_TIMEOUT(cat.count(), 5, 5000);
        QTest::qWait(600);
        QCOMPARE(cat.count(), 5);
    }

    // What peers share lands as one batch and the catalog is rebuilt ONCE, when it has — however many files land, and
    // however many peers' batches land together. A rebuild per landed file (every 2 s while thousands landed) held a
    // receiving Steam Deck's GUI thread for seconds at a time, for minutes. Teeth: rebuild per landed file or per
    // batch (two batches, two rebuilds); never rebuild when a batch has landed (nothing shown); rebuild when what was
    // asked for again was already in place (a resync of a landed library: a stall every 10 minutes).
    void a_received_library_lands_with_one_catalog_rebuild()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}, {"ReceiveFrom", json::array({"12D3KooWSeederA", "12D3KooWSeederB"})}};
        AppModel m(&cfg, &appDir);
        std::map<std::string, std::string> Bytes;                                  // cid → its node's canonical bytes
        const auto snap = [&](const std::string &Who, int N) {
            json Items = json::array();
            for (int I = 0; I < N; ++I)
            {
                const json Node = json{{"LABEL", Who + " " + std::to_string(I)}, {"LAYERS", json::array({ json{{"DIR", "x"}} })}};
                Bytes[Cid::OfNode(Node)] = Cid::Canonical(Node);
                Items.push_back(json{{"cid", Cid::OfNode(Node)}, {"pkg", Who + " game " + std::to_string(I % 4)}, {"node", std::to_string(I) + ".json"}});
            }
            return QString::fromStdString(json{{"Games", Items}}.dump()); };
        const QString A = snap("a", 24), B = snap("b", 12);
        std::atomic<bool> Release{false};
        IpfsWrapper::DebugResetQueue();
        IpfsWrapper::SetFetchOnceHook([&](const std::string &C, const std::string &Dest, bool, std::string *E) {
            while (!Release) std::this_thread::sleep_for(std::chrono::milliseconds(5));   // both batches queued first
            const auto It = Bytes.find(C);
            if (It == Bytes.end()) { if (E) *E = "unknown"; return 1; }
            std::filesystem::create_directories(std::filesystem::path(Dest).parent_path());
            std::ofstream(Dest, std::ios::binary) << It->second;
            return 0; });
        QSignalSpy Rebuilt(&m, &AppModel::catalogChanged);
        m.peers().applyPeerLibrarySnapshot("12D3KooWSeederA", A);
        m.peers().reconcileReceivedLibraries();                                   // the same files asked for again
        m.peers().applyPeerLibrarySnapshot("12D3KooWSeederB", B);
        Release = true;
        QTRY_COMPARE_WITH_TIMEOUT(Rebuilt.count(), 1, 20000);
        QTest::qWait(1500);                                                        // and no more after it
        QCOMPARE(Rebuilt.count(), 1);
        m.peers().reconcileReceivedLibraries();                                   // the 10-min resync: all in place
        QTest::qWait(1500);
        QCOMPARE(Rebuilt.count(), 1);                                              // nothing landed: nothing rebuilt
        size_t Landed = 0;                                                         // every file, at its place, whole
        for (const std::string P : { "12D3KooWSeederA", "12D3KooWSeederB" })
            for (const auto &T : PackageCatalog::PlanReceivedFetches(cfg, P.substr(P.size() - 8), cfg["PeerLibraries"][P]))
            {
                std::ifstream I(T.Dest, std::ios::binary);
                if (I && std::string(std::istreambuf_iterator<char>(I), {}) == Bytes.at(T.Cid)) ++Landed;
            }
        QCOMPARE(Landed, Bytes.size());
        IpfsWrapper::SetFetchOnceHook(nullptr);
        IpfsWrapper::DebugResetQueue();
    }

    // A node a second peer shares is already here (the first peer's copy): the queue puts it in place at once, from that
    // copy, and that is shown too — never awaited, so never shown, it was. Teeth: count only what is fetched.
    void a_node_already_here_shared_by_another_peer_is_shown()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}, {"ReceiveFrom", json::array({"12D3KooWSeederA", "12D3KooWSeederB"})}};
        AppModel m(&cfg, &appDir);
        const json N = json{{"LABEL", "shared"}, {"LAYERS", json::array({ json{{"DIR", "s"}} })}};
        IpfsWrapper::DebugResetQueue();
        IpfsWrapper::SetFetchOnceHook([&](const std::string &, const std::string &Dest, bool, std::string *) {
            std::filesystem::create_directories(std::filesystem::path(Dest).parent_path());
            std::ofstream(Dest, std::ios::binary) << Cid::Canonical(N);
            return 0; });
        const auto snap = [&](const std::string &Pkg) {
            return QString::fromStdString(json{{"Games", json::array({ json{{"cid", Cid::OfNode(N)}, {"pkg", Pkg}, {"node", "shared.json"}} })}}.dump()); };
        QSignalSpy Rebuilt(&m, &AppModel::catalogChanged);
        m.peers().applyPeerLibrarySnapshot("12D3KooWSeederA", snap("A's"));
        QTRY_COMPARE_WITH_TIMEOUT(Rebuilt.count(), 1, 10000);
        m.peers().applyPeerLibrarySnapshot("12D3KooWSeederB", snap("B's"));        // the same node, elsewhere
        QTRY_COMPARE_WITH_TIMEOUT(Rebuilt.count(), 2, 10000);
        IpfsWrapper::SetFetchOnceHook(nullptr);
        IpfsWrapper::DebugResetQueue();
    }

    // A batch that does not finish — a peer gone mid-batch leaves its fetches retrying — still shows what has landed,
    // after ReceiveProgressMs, and a batch that finished while it waits is shown at its next look; the last file
    // landing rebuilds once more — the wait's own rebuild (nothing else rebuilds while a file is still awaited). Asking
    // again for the stuck file adds no wait (one wait covers everything). Teeth: show nothing until every batch lands
    // (the stuck one hides all); drop the look at batches that landed meanwhile (the second peer's stays hidden); a
    // wait per enqueue (each looks and rebuilds); no rebuild when the last file lands; no closure pass until nothing is
    // awaited (a stuck file held up every package's contents).
    void a_stuck_received_batch_shows_what_has_landed()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}, {"ReceiveFrom", json::array({"12D3KooWSeederA", "12D3KooWSeederB"})}};
        AppModel m(&cfg, &appDir);
        m.peers().ReceiveProgressMs = 300;
        const json N1 = json{{"LABEL", "landed"}, {"LAYERS", json::array({ json{{"DIR", "1"}} })}};
        const json N2 = json{{"LABEL", "stuck"}, {"LAYERS", json::array({ json{{"DIR", "2"}} })}};
        const json N3 = json{{"LABEL", "other peer"}, {"LAYERS", json::array({ json{{"DIR", "3"}} })}};
        const std::string Stuck = Cid::OfNode(N2);
        std::map<std::string, std::string> Bytes{ {Cid::OfNode(N1), Cid::Canonical(N1)}, {Stuck, Cid::Canonical(N2)}, {Cid::OfNode(N3), Cid::Canonical(N3)} };
        std::atomic<bool> Release{false};
        IpfsWrapper::DebugResetQueue();
        IpfsWrapper::SetFetchOnceHook([&](const std::string &C, const std::string &Dest, bool, std::string *E) {
            if (C == Stuck && !Release) { if (E) *E = "no providers"; return 1; }   // retryable: it waits on
            std::filesystem::create_directories(std::filesystem::path(Dest).parent_path());
            std::ofstream(Dest, std::ios::binary) << Bytes.at(C);
            return 0; });
        const auto item = [](const json &N, const std::string &Pkg, const std::string &File) {
            return json{{"cid", Cid::OfNode(N)}, {"pkg", Pkg}, {"node", File}}; };
        QSignalSpy Rebuilt(&m, &AppModel::catalogChanged);
        m.peers().applyPeerLibrarySnapshot("12D3KooWSeederA", QString::fromStdString(json{{"Games", json::array({
            item(N1, "Game", "landed.json"), item(N2, "Game", "stuck.json") })}}.dump()));
        QTRY_VERIFY_WITH_TIMEOUT(Rebuilt.count() >= 1, 10000);                      // what landed shows, the stuck file waits
        QVERIFY2(m.peers().closurePasses() >= 1, "what landed is landed whole (its closure) though a file is stuck");
        QTest::qWait(1000);
        const int Shown = Rebuilt.count();                                         // nothing new: no more rebuilds,
        for (int I = 0; I < 3; ++I) m.peers().reconcileReceivedLibraries();       // however often it is asked for again
        QTest::qWait(1500);
        QCOMPARE(Rebuilt.count(), Shown);
        m.peers().applyPeerLibrarySnapshot("12D3KooWSeederB", QString::fromStdString(json{{"Games", json::array({
            item(N3, "Other", "other.json") })}}.dump()));
        QTRY_VERIFY_WITH_TIMEOUT(Rebuilt.count() > Shown, 10000);                   // the other peer's landed batch shows
        const int Before = Rebuilt.count();
        Release = true;
        QTRY_VERIFY_WITH_TIMEOUT(Rebuilt.count() > Before, 20000);                  // the stuck file landed: shown too
        IpfsWrapper::SetFetchOnceHook(nullptr);
        IpfsWrapper::DebugResetQueue();
    }

    // Receive-off erases the peer's snapshot record, but a closure pass in flight can still land one late block into
    // the dir it just swept. The dirs a drop swept are remembered per peer, so a second drop after the record is
    // gone sweeps them again instead of no-oping on the missing record and leaving a ghost stub dir.
    void a_late_stub_after_receive_off_is_swept_again()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}, {"ReceiveFrom", json::array({"12D3KooWSeeder"})}};   // we receive from them
        AppModel m(&cfg, &appDir);
        const QString peer = "12D3KooWSeeder";
        const std::string P = peer.toStdString();
        m.peers().applyPeerLibrarySnapshot(peer, R"({"Games":[{"cid":"bafyold","pkg":"Old Game","node":"old.json"}]})");
        const auto Plan = PackageCatalog::PlanReceivedFetches(cfg, P.substr(P.size() - 8), cfg["PeerLibraries"][P]);
        QVERIFY(!Plan.empty());
        const std::filesystem::path Pkg = std::filesystem::path(Plan.front().Dest).parent_path();
        std::filesystem::create_directories(Pkg);
        { std::ofstream S(Plan.front().Dest); S << "{}"; }
        m.peers().stopReceivingFromPeer(peer);                                     // record erased, stubs swept
        QVERIFY(!std::filesystem::exists(Pkg));
        QVERIFY(!cfg["PeerLibraries"].contains(P));
        std::filesystem::create_directories(Pkg);                            // a late block from the in-flight pass
        { std::ofstream S(Plan.front().Dest); S << "{}"; }
        m.peers().stopReceivingFromPeer(peer);                                     // the re-drop path with NO record
        QVERIFY(!std::filesystem::exists(Pkg));
    }

    // A peer pushes its snapshot whenever its shares change, asked or not. Until you choose to receive from them,
    // nothing of it is kept: no record (which would list their packages, offer their builds as updates, plan fetches)
    // and no sequence stamp (which would make the fresh snapshot Receive-on asks for look stale). Teeth: drop the
    // receive gate in applyPeerLibrarySnapshot.
    // Every peer is the same: pairing (or accepting) gives exactly the permissions ticked — the table's columns, one
    // library at a time, receive, vLAN, presence — and they start as the "New peers" row. A choice stands: an acceptance
    // landing later never applies the defaults over it, and a peer nobody chose for gets nothing of ours nor we of its.
    // Server mode applies the row. Removing a peer forgets all of it. Teeth: a permission not applied, a library
    // shared that was not ticked, defaults over a choice, the row never offered as the start, a forgotten choice.
    void pairing_gives_exactly_the_permissions_ticked()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        const std::string Lib = d.path().toStdString() + "/LIB";
        for (const char *P : {"VidyaGod/[1] A", "VidyaGodRunners/wine"})
        {
            std::filesystem::create_directories(Lib + "/" + P);
            NodeFixture::WriteNodes(Lib + "/" + P + "/n.json", NodeFixture::Chain(std::string("h") + P[0] + P[9], {NodeFixture::Exec("win32", "x.exe")}));
        }
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", Lib}}}}},
                        {"NewPeerDefaults", {{"receive", true}, {"vlan", true}, {"presence", false}, {"share", {"VidyaGodRunners"}}}}};
        AppModel m(&cfg, &appDir);
        QSignalSpy Rebuilt(&m, &AppModel::catalogChanged);
        m.rebuildCatalog();
        QTRY_VERIFY_WITH_TIMEOUT(Rebuilt.count() > 0 && m.catalogSnapshot(), 30000);
        PeerSharing &F = m.peers();
        QCOMPARE(F.myLibraryNames(), (QStringList{"VidyaGod", "VidyaGodRunners"}));

        const PeerSharing::Permissions D = F.defaultPermissions();
        QCOMPARE(D.Libraries, QStringList{"VidyaGodRunners"});
        QVERIFY(D.Receive && D.Vlan && !D.Presence);

        PeerSharing::Permissions Given;
        Given.Libraries = {"VidyaGod"};
        Given.Receive = false;
        Given.Vlan = true;
        Given.Presence = true;
        F.applyPairing("Pal", Given);
        QVERIFY(F.isSharingWithPeer("Pal", "VidyaGod") && !F.isSharingWithPeer("Pal", "VidyaGodRunners"));
        QVERIFY(!F.isReceivingFrom("Pal") && F.isInVlan("Pal") && F.isPresenceSharedWith("Pal"));
        emit PeersManager::instance()->peerAccepted("Pal", "pal", "");   // their acceptance lands later
        QVERIFY(!F.isReceivingFrom("Pal") && !F.isSharingWithPeer("Pal", "VidyaGodRunners"));

        QVERIFY(!F.hasChosen("Code"));
        emit PeersManager::instance()->peerAccepted("Code", "code", "");   // paired with nothing chosen
        QVERIFY(!F.isReceivingFrom("Code") && !F.isSharingWithPeer("Code", "VidyaGod") && !F.isSharingWithPeer("Code", "VidyaGodRunners"));
        F.applyNewPeerDefaults("Srv");                                          // accepted by itself (server mode): the row
        QVERIFY(F.isReceivingFrom("Srv") && F.isSharingWithPeer("Srv", "VidyaGodRunners") && !F.isSharingWithPeer("Srv", "VidyaGod"));

        F.forgetPeer("Pal");
        QVERIFY(!F.hasChosen("Pal") && !F.isSharingWithPeer("Pal", "VidyaGod"));

        //A peer that ends the pairing from its side: what was chosen for it ends with it (it stayed: a pairing made again
        //served the old shares at once). Teeth: only refresh on their decline.
        F.applyPairing("Gone", Given);
        QVERIFY(F.hasChosen("Gone") && F.isSharingWithPeer("Gone", "VidyaGod"));
        emit PeersManager::instance()->peerDeclined("Gone");
        QVERIFY(!F.hasChosen("Gone") && !F.isSharingWithPeer("Gone", "VidyaGod"));
    }

    // What is shared stays current by itself: a library never prepared says so (and nothing publishes it unasked);
    // prepared once, it is up to date; edited packages are published by themselves, together, and then are; a package that cannot be
    // shared as it is (a node too large to be one) is named, and nothing is published for it. Teeth: an unpublished
    // library published unasked, an edit missed, a publish not started, a package that cannot be shared left unsaid.
    void what_is_shared_is_kept_current()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        std::string Err;
        QVERIFY2(IpfsWrapper::StartNode((d.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        const std::string Lib = d.path().toStdString() + "/LIB";
        for (const char *P : {"[1] A", "[2] B"})
        {
            std::filesystem::create_directories(Lib + "/VidyaGod/" + P);
            NodeFixture::WriteNodes(Lib + "/VidyaGod/" + P + "/n.json",
                                    NodeFixture::Chain(std::string("h") + P[1], {NodeFixture::Exec("win32", std::string(P) + ".exe")}));
        }
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", Lib}}}}}};
        {
            AppModel m(&cfg, &appDir);
            using S = PeerSharing::ShareState;
            PeerSharing &F = m.peers();
            QSignalSpy Rebuilt(&m, &AppModel::catalogChanged), Done(&m, &AppModel::libraryPublished), Failed(&m, &AppModel::libraryPublishFailed);
            const auto Rebuild = [&] { const int N = Rebuilt.count(); m.rebuildCatalog(); QTRY_VERIFY_WITH_TIMEOUT(Rebuilt.count() > N, 30000); };
            Rebuild();
            F.checkShares();
            QCOMPARE(F.shareState(), S::NothingShared);
            F.shareLibraryWithPeer("Dev", "VidyaGod");
            F.checkShares();
            QCOMPARE(F.shareState(), S::Unpublished);
            QVERIFY(!m.publishing());

            F.prepareSharing();
            QTRY_COMPARE_WITH_TIMEOUT(Done.count() + Failed.count(), 1, 120000);
            Rebuild();
            F.checkShares();
            QCOMPARE(F.shareState(), S::UpToDate);

            const std::filesystem::path ADir = Lib + "/VidyaGod/[1] A";
            std::filesystem::path AFile;
            for (const auto &E : std::filesystem::directory_iterator(ADir)) if (E.path().extension() == ".json") AFile = E.path();
            json J;
            { std::ifstream In(AFile); J = json::parse(In); }
            J["COMMENT"] = "v2";
            NodeFixture::WriteNodes(AFile.string(), J);
            //Two edited packages go out in one pass (one read of the library), not one pass each.
            std::filesystem::path BFile;
            for (const auto &E : std::filesystem::directory_iterator(Lib + "/VidyaGod/[2] B")) if (E.path().extension() == ".json") BFile = E.path();
            json JB;
            { std::ifstream In(BFile); JB = json::parse(In); }
            JB["COMMENT"] = "v2";
            NodeFixture::WriteNodes(BFile.string(), JB);
            Rebuild();
            F.checkShares();
            QCOMPARE(F.shareState(), S::Updating);
            QVERIFY(m.publishing());
            QTRY_COMPARE_WITH_TIMEOUT(Done.count() + Failed.count(), 2, 120000);
            QCOMPARE(Done.last().at(1).toString(), QString("2 package(s)"));
            Rebuild();
            F.checkShares();
            QCOMPARE(F.shareState(), S::UpToDate);

            //A package whose node is too large to be a node: the index refuses it, so it is never shared as it is —
            //and the line says so by name, starting no publish.
            std::filesystem::create_directories(Lib + "/VidyaGod/[3] Huge");
            NodeFixture::WriteNodes(Lib + "/VidyaGod/[3] Huge/n.json",
                                    NodeFixture::Chain("hhuge", {NodeFixture::Exec("win32", "h.exe")}, {}, json{{"COMMENT", std::string(300 * 1024, 'x')}}));
            Rebuild();
            F.checkShares();
            QCOMPARE(F.shareState(), S::Failed);
            QVERIFY2(F.shareStateText().contains("[3] Huge"), qPrintable(F.shareStateText()));
            QVERIFY(!m.publishing());
            QTest::qWait(2500);                                              // the debounced checks start nothing either
            QCOMPARE(Done.count() + Failed.count(), 2);
            std::filesystem::remove_all(Lib + "/VidyaGod/[3] Huge");

            //A library added once the first is prepared: never published unasked; prepared, only its packages are (a
            //marker pin on another package survives: no whole publish). A package deleted leaves the sheet with no
            //publish at all. Teeth: an added library published unasked, preparing it publishing everything, a deletion
            //publishing.
            std::filesystem::create_directories(Lib + "/Mods/[9] Mod");
            NodeFixture::WriteNodes(Lib + "/Mods/[9] Mod/n.json", NodeFixture::Chain("hmod", {NodeFixture::Exec("win32", "m.exe")}));
            Rebuild();
            F.shareLibraryWithPeer("Dev", "Mods");
            F.checkShares();
            QCOMPARE(F.shareState(), S::Unpublished);
            QVERIFY2(F.shareStateText().contains("Mods"), qPrintable(F.shareStateText()));
            QVERIFY(!m.publishing());
            cfg["Pins"]["VidyaGod/[2] B"] = "kept";
            const int Before = Done.count() + Failed.count();
            F.prepareSharing();
            QTRY_COMPARE_WITH_TIMEOUT(Done.count() + Failed.count(), Before + 1, 120000);
            QCOMPARE(cfg["Pins"].value("VidyaGod/[2] B", std::string()), std::string("kept"));
            bool ModShared = false;
            for (const auto &[C, Path] : cfg["ShareSheet"].items()) ModShared = ModShared || Path.get<std::string>().rfind("Mods/", 0) == 0;
            QVERIFY(ModShared);
            Rebuild();
            F.checkShares();
            QCOMPARE(F.shareState(), S::UpToDate);
            std::filesystem::remove_all(Lib + "/VidyaGod/[2] B");
            Rebuild();
            F.checkShares();
            QVERIFY(!m.publishing());
            for (const auto &[C, Path] : cfg["ShareSheet"].items()) QVERIFY(Path.get<std::string>().rfind("VidyaGod/[2] B/", 0) != 0);
            QCOMPARE(F.shareState(), S::UpToDate);
            QCOMPARE(Done.count() + Failed.count(), Before + 1);

            //Another tool writing the library (the packaging scripts' flock): an edit waits, says so, and is published
            //by itself once the lock is free — busy is no failure. Teeth: busy taken for a failure (never retried).
            F.BusyRetryMs = 300;
            { std::ifstream In(AFile); J = json::parse(In); }
            J["COMMENT"] = "v3";
            NodeFixture::WriteNodes(AFile.string(), J);
            {
                const Platform::ScopedFileLock Theirs(PackageCatalog::LibraryWriteLock(cfg));
                QVERIFY(Theirs.held());
                Rebuild();
                F.checkShares();
                QTRY_COMPARE_WITH_TIMEOUT(Failed.count(), 1, 120000);
                QTRY_VERIFY_WITH_TIMEOUT(F.shareStateText().startsWith("Waiting for another tool"), 5000);
            }
            QTRY_COMPARE_WITH_TIMEOUT(Done.count(), 4, 120000);
            Rebuild();
            F.checkShares();
            QCOMPARE(F.shareState(), S::UpToDate);
        }
        IpfsWrapper::StopNode();
    }

    void a_snapshot_from_a_peer_you_do_not_receive_from_is_not_kept()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}};
        AppModel m(&cfg, &appDir);
        const QString peer = "12D3KooWSeeder";
        const std::string P = peer.toStdString();
        QSignalSpy changed(&m.peers(), &PeerSharing::peerCatalogChanged);
        m.peers().applyPeerLibrarySnapshot(peer, R"({"Games":[{"cid":"bafyx","pkg":"Game","node":"game.json"}]})", 10);
        QVERIFY(!cfg.contains("PeerLibraries") || !cfg["PeerLibraries"].contains(P));
        QCOMPARE(changed.count(), 0);
        m.peers().setReceivingFrom(peer, true);
        m.peers().applyPeerLibrarySnapshot(peer, R"({"Games":[{"cid":"bafyx","pkg":"Game","node":"game.json"}]})", 5);   // the reply to Receive-on
        QVERIFY(cfg["PeerLibraries"].contains(P));
    }

    void peer_snapshot_replaces_wholesale_and_dedups()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}, {"ReceiveFrom", json::array({"12D3KooWSeeder"})}};   // we receive from them
        AppModel m(&cfg, &appDir);
        const QString peer = "12D3KooWSeeder";
        QSignalSpy chg(&m.peers(), &PeerSharing::peerCatalogChanged);

        // First snapshot: two libraries recorded under the peer.
        m.peers().applyPeerLibrarySnapshot(peer, R"({"Games":[{"cid":"cidX"},{"cid":"cidY"}],"Retro":[{"cid":"cidZ"}]})");
        QCOMPARE(chg.count(), 1);
        QVERIFY(cfg.contains("PeerLibraries"));
        QCOMPARE((int)cfg["PeerLibraries"][peer.toStdString()].size(), 2);
        QCOMPARE(cfg["PeerLibraries"][peer.toStdString()]["Games"].size(), (size_t)2);

        // Withdraw "Games" by sending a snapshot that omits it — wholesale replace, so "Games" is gone even though no
        // explicit unshare was delivered. THE regression this whole redesign exists to prevent.
        m.peers().applyPeerLibrarySnapshot(peer, R"({"Retro":[{"cid":"cidZ"}]})");
        QCOMPARE(chg.count(), 2);
        QVERIFY(!cfg["PeerLibraries"][peer.toStdString()].contains("Games"));
        QVERIFY(cfg["PeerLibraries"][peer.toStdString()].contains("Retro"));

        // Identical snapshot again → no change, no signal, no churn.
        m.peers().applyPeerLibrarySnapshot(peer, R"({"Retro":[{"cid":"cidZ"}]})");
        QCOMPARE(chg.count(), 2);

        // Empty snapshot → the peer is dropped entirely.
        m.peers().applyPeerLibrarySnapshot(peer, R"({})");
        QCOMPARE(chg.count(), 3);
        QVERIFY(!cfg["PeerLibraries"].contains(peer.toStdString()));

        // Empty again with nothing to drop → still a no-op.
        m.peers().applyPeerLibrarySnapshot(peer, R"({})");
        QCOMPARE(chg.count(), 3);
    }

    // The seq authority (last-writer-wins): snapshots ride independent, concurrently-handled streams and can be applied
    // out of order. A stamp lower than or equal to the highest already seen for a peer is a reordered straggler and MUST
    // be dropped, so a stale snapshot delivered late can't resurrect a withdrawn library. seq==0 (unstamped) always
    // applies. This is the receiver-side fix for the emit-off-lock race the Go side structurally can't close.
    void peer_snapshot_last_writer_wins_by_seq()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}, {"ReceiveFrom", json::array({"12D3KooWSeeder"})}};   // we receive from them
        AppModel m(&cfg, &appDir);
        const QString peer = "12D3KooWSeeder";
        const std::string P = peer.toStdString();
        QSignalSpy chg(&m.peers(), &PeerSharing::peerCatalogChanged);

        // seq 10: the fresh state (Retro only).
        m.peers().applyPeerLibrarySnapshot(peer, R"({"Retro":[{"cid":"cidZ"}]})", 10);
        QCOMPARE(chg.count(), 1);
        QVERIFY(cfg["PeerLibraries"][P].contains("Retro"));

        // seq 5 arrives LATE (a big stale snapshot that still listed Games) → must be dropped, Games stays gone.
        m.peers().applyPeerLibrarySnapshot(peer, R"({"Games":[{"cid":"cidX"}],"Retro":[{"cid":"cidZ"}]})", 5);
        QCOMPARE(chg.count(), 1);                                   // no change emitted
        QVERIFY(!cfg["PeerLibraries"][P].contains("Games"));      // the stale withdraw-loser did NOT resurrect Games

        // An equal stamp (duplicate) is also dropped.
        m.peers().applyPeerLibrarySnapshot(peer, R"({"Games":[{"cid":"cidX"}]})", 10);
        QCOMPARE(chg.count(), 1);
        QVERIFY(!cfg["PeerLibraries"][P].contains("Games"));

        // A higher stamp is accepted.
        m.peers().applyPeerLibrarySnapshot(peer, R"({"Games":[{"cid":"cidX"}]})", 11);
        QCOMPARE(chg.count(), 2);
        QVERIFY(cfg["PeerLibraries"][P].contains("Games"));

        // A malformed message must NOT advance the high-water mark: after it, a valid seq-12 still applies.
        m.peers().applyPeerLibrarySnapshot(peer, "garbage", 99);          // parse error — ignored, mark stays at 11
        m.peers().applyPeerLibrarySnapshot(peer, R"({"Retro":[{"cid":"cidZ"}]})", 12);
        QCOMPARE(chg.count(), 3);
        QVERIFY(cfg["PeerLibraries"][P].contains("Retro"));
    }

    // Malformed snapshots must never corrupt the store: bad JSON / a non-object is ignored (record untouched), and
    // within a valid snapshot non-array libraries and non-string CIDs are dropped, empty libraries elided.
    void peer_snapshot_sanitizes_malformed_input()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}, {"ReceiveFrom", json::array({"12D3KooWSeeder"})}};   // we receive from them
        AppModel m(&cfg, &appDir);
        const QString peer = "12D3KooWSeeder";
        const std::string P = peer.toStdString();
        QSignalSpy chg(&m.peers(), &PeerSharing::peerCatalogChanged);

        m.peers().applyPeerLibrarySnapshot(peer, "not json at all");   // parse error → ignored
        QCOMPARE(chg.count(), 0);
        m.peers().applyPeerLibrarySnapshot(peer, R"(["array","not","object"])");  // wrong shape → ignored
        QCOMPARE(chg.count(), 0);
        QVERIFY(!cfg.contains("PeerLibraries") || !cfg["PeerLibraries"].contains(P));

        // Mixed valid/invalid: "Good" keeps only its cid-bearing objects; "Bad" (non-array) and "Empty" (no valid
        // entries — a bare string, a number, an object with no cid) are elided.
        m.peers().applyPeerLibrarySnapshot(peer, R"({"Good":[{"cid":"ok"},42,{"cid":"ok2","node":"n"}],"Bad":"nope","Empty":[7,"cid",{"node":"x"}]})");
        QCOMPARE(chg.count(), 1);
        QCOMPARE((int)cfg["PeerLibraries"][P].size(), 1);
        QCOMPARE(cfg["PeerLibraries"][P]["Good"].size(), (size_t)2);
        QVERIFY(!cfg["PeerLibraries"][P].contains("Bad"));
        QVERIFY(!cfg["PeerLibraries"][P].contains("Empty"));
    }

    // Ending a pairing forgets BOTH directions of sharing state: what we serve them (config["Sharing"]) and what
    // they shared with us (config["PeerLibraries"]) — and only signals when something was actually forgotten.
    void forget_peer_clears_both_directions()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        const std::string P = "12D3KooWPeer", Q = "12D3KooWOther";
        json cfg = json{{"Settings", json::object()},
                        {"Sharing", json{{P, json::array({"Games"})}, {Q, json::array({"Retro"})}}},
                        {"PeerLibraries", json{{P, json{{"Games", json::array({json{{"cid","cidX"}}})}}}}}};
        AppModel m(&cfg, &appDir);
        QSignalSpy chg(&m.peers(), &PeerSharing::peerCatalogChanged);

        m.peers().forgetPeer(QString::fromStdString(P));
        QCOMPARE(chg.count(), 1);
        QVERIFY(!cfg["Sharing"].contains(P));            // our side dropped
        QVERIFY(!cfg["PeerLibraries"].contains(P));    // their side dropped
        QVERIFY(cfg["Sharing"].contains(Q));             // the OTHER peer is untouched

        m.peers().forgetPeer(QString::fromStdString(P));       // already gone → no-op, no signal
        QCOMPARE(chg.count(), 1);
    }
    // Network-tab per-peer toggles are config-backed with the correct inversions (Receive default off; Presence
    // SHARED by default = not in PresenceDeny; vLAN IN by default = not in LanExcludedPeers), and auto-accept persists.
    // (Go pushes are safe no-ops offline; we assert the config state, which is the durable source of truth.)
    void network_per_peer_toggles_roundtrip()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}};
        AppModel m(&cfg, &appDir);
        const QString peer = "12D3KooWPeerAAA";

        QVERIFY(!m.peers().isReceivingFrom(peer));                 // receive: off by default
        m.peers().setReceivingFrom(peer, true);  QVERIFY(m.peers().isReceivingFrom(peer));
        m.peers().setReceivingFrom(peer, false); QVERIFY(!m.peers().isReceivingFrom(peer));

        QVERIFY(m.peers().isPresenceSharedWith(peer));             // presence: shared by default
        m.peers().setPresenceSharedWith(peer, false);
        QVERIFY(!m.peers().isPresenceSharedWith(peer));            // hidden → in PresenceDeny
        m.peers().setPresenceSharedWith(peer, true);
        QVERIFY(m.peers().isPresenceSharedWith(peer));

        QVERIFY(m.peers().isInVlan(peer));                         // vLAN: in by default
        m.peers().setInVlan(peer, false);
        QVERIFY(!m.peers().isInVlan(peer));                        // left → in LanExcludedPeers
        m.peers().setInVlan(peer, true);
        QVERIFY(m.peers().isInVlan(peer));

        QVERIFY(!m.peers().autoAcceptEnabled());                   // auto-accept: off by default, persists
        m.peers().setAutoAcceptEnabled(true);
        QVERIFY(m.peers().autoAcceptEnabled());
        QVERIFY(cfg.value("AutoAcceptPeers", false));
    }

    // New-Peer defaults: presence ON, receive/vLAN OFF out of the box; applyNewPeerDefaults stamps a peer; forgetPeer
    // clears the per-peer records.
    void new_peer_defaults_and_apply()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}};
        AppModel m(&cfg, &appDir);

        QCOMPARE(m.peers().newPeerDefault("presence"), true);
        QCOMPARE(m.peers().newPeerDefault("receive"), false);
        QCOMPARE(m.peers().newPeerDefault("vlan"), false);
        m.peers().setNewPeerDefault("receive", true);
        m.peers().setNewPeerShareDefault("Games", true);
        QCOMPARE(m.peers().newPeerDefault("receive"), true);
        QVERIFY(m.peers().newPeerShareDefaults().contains("Games"));
        m.peers().setNewPeerShareDefault("Games", false);
        QVERIFY(!m.peers().newPeerShareDefaults().contains("Games"));   // toggle back off (exercises the fixed else-if branch)

        const QString peer = "12D3KooWNewBBB";
        m.peers().applyNewPeerDefaults(peer);
        QVERIFY(m.peers().isReceivingFrom(peer));                       // receive default applied
        QVERIFY(m.peers().isPresenceSharedWith(peer));                 // presence default (on) applied

        m.peers().forgetPeer(peer);
        QVERIFY(!m.peers().isReceivingFrom(peer));                     // forgetPeer clears the per-peer records
    }
};

QTEST_MAIN(AppModelTest)
#include "test_appmodel.moc"
