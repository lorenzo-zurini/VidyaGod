// Tests for AppModel — the GUI state/signal hub: card-size persistence, repository add validation (#1 dup/empty),
// and removePackage for a local/portable package (#E28: drop the reference, never touch the user's files).
// Uses a temp data dir; the IPFS node is not started (the dehydrate path's unpin/drop-ref are no-ops).

#include <QtTest>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>

#include "appmodel.h"
#include "apppaths.h"
#include "nodefixture.h"

#include <fstream>

using json = nlohmann::ordered_json;

class AppModelTest : public QObject
{
    Q_OBJECT
private slots:
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

    void remove_unknown_package_is_noop()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}, {"LIBRARY", json::array()}};
        AppModel m(&cfg, &appDir);
        m.removePackage("does-not-exist");                         // must not crash / throw
        QVERIFY(true);
    }

    // Registering a package whose launchable declares DeclareExec.RUNNER seeds the package's PREFERRED_RUNNER
    // (a soft, package-side runner recommendation the user later overrides). A fresh entry only — never clobbers a
    // user's existing choice.
    // Per-package config (incl. PREFERRED_RUNNER) now lives in the INSTANCE file, not the LIBRARY entry — register
    // must leave the entry PACKAGE-FREE. The recommended runner is still applied at launch by PickRunnerNode's
    // RECOMMENDED-first default (see test_launchresolver::pick_runner_prefers_recommended), not by a seed here.
    void register_leaves_library_entry_package_free()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        QTemporaryDir pkg; QVERIFY(pkg.isValid());
        json exec = NodeFixture::Exec("win32", "g.exe");
        exec["RUNNER"] = "geproton_10_20_runner";
        // The tile is the launchable's PARENT (pure Meta upstream, exec terminal).
        json node = NodeFixture::Chain("tg", {NodeFixture::Tile("999", "Test Game"), exec});
        { std::ofstream f((pkg.path() + "/tg.json").toStdString()); f << node.dump(); }

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
        QVERIFY(QFile::exists(path));

        std::ifstream f(path.toStdString());
        json onDisk; f >> onDisk;
        QCOMPARE(onDisk["Settings"].value("CardPixelWidth", 0), 199);
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

    // markNodeReady emits nodeReady — the signal the Network tab needs so its friend code + Verify&Publish enable
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

    // A friend's library snapshot REPLACES that peer's record wholesale — the core of the bilateral protocol's
    // "receiver". This is what makes a withdrawn library un-loseable: a snapshot that no longer lists a library drops
    // it, even if the specific "unshare" push was never received. An empty snapshot drops the peer entirely; an
    // identical snapshot is a no-op (no disk write, no UI refresh).
    void friend_snapshot_replaces_wholesale_and_dedups()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}};
        AppModel m(&cfg, &appDir);
        const QString peer = "12D3KooWSeeder";
        QSignalSpy chg(&m, &AppModel::friendCatalogChanged);

        // First snapshot: two libraries recorded under the peer.
        m.applyFriendLibrarySnapshot(peer, R"({"Games":[{"cid":"cidX"},{"cid":"cidY"}],"Retro":[{"cid":"cidZ"}]})");
        QCOMPARE(chg.count(), 1);
        QVERIFY(cfg.contains("FriendLibraries"));
        QCOMPARE((int)cfg["FriendLibraries"][peer.toStdString()].size(), 2);
        QCOMPARE(cfg["FriendLibraries"][peer.toStdString()]["Games"].size(), (size_t)2);

        // Withdraw "Games" by sending a snapshot that omits it — wholesale replace, so "Games" is gone even though no
        // explicit unshare was delivered. THE regression this whole redesign exists to prevent.
        m.applyFriendLibrarySnapshot(peer, R"({"Retro":[{"cid":"cidZ"}]})");
        QCOMPARE(chg.count(), 2);
        QVERIFY(!cfg["FriendLibraries"][peer.toStdString()].contains("Games"));
        QVERIFY(cfg["FriendLibraries"][peer.toStdString()].contains("Retro"));

        // Identical snapshot again → no change, no signal, no churn.
        m.applyFriendLibrarySnapshot(peer, R"({"Retro":[{"cid":"cidZ"}]})");
        QCOMPARE(chg.count(), 2);

        // Empty snapshot → the peer is dropped entirely.
        m.applyFriendLibrarySnapshot(peer, R"({})");
        QCOMPARE(chg.count(), 3);
        QVERIFY(!cfg["FriendLibraries"].contains(peer.toStdString()));

        // Empty again with nothing to drop → still a no-op.
        m.applyFriendLibrarySnapshot(peer, R"({})");
        QCOMPARE(chg.count(), 3);
    }

    // The seq authority (last-writer-wins): snapshots ride independent, concurrently-handled streams and can be applied
    // out of order. A stamp lower than or equal to the highest already seen for a peer is a reordered straggler and MUST
    // be dropped, so a stale snapshot delivered late can't resurrect a withdrawn library. seq==0 (unstamped) always
    // applies. This is the receiver-side fix for the emit-off-lock race the Go side structurally can't close.
    void friend_snapshot_last_writer_wins_by_seq()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}};
        AppModel m(&cfg, &appDir);
        const QString peer = "12D3KooWSeeder";
        const std::string P = peer.toStdString();
        QSignalSpy chg(&m, &AppModel::friendCatalogChanged);

        // seq 10: the fresh state (Retro only).
        m.applyFriendLibrarySnapshot(peer, R"({"Retro":[{"cid":"cidZ"}]})", 10);
        QCOMPARE(chg.count(), 1);
        QVERIFY(cfg["FriendLibraries"][P].contains("Retro"));

        // seq 5 arrives LATE (a big stale snapshot that still listed Games) → must be dropped, Games stays gone.
        m.applyFriendLibrarySnapshot(peer, R"({"Games":[{"cid":"cidX"}],"Retro":[{"cid":"cidZ"}]})", 5);
        QCOMPARE(chg.count(), 1);                                   // no change emitted
        QVERIFY(!cfg["FriendLibraries"][P].contains("Games"));      // the stale withdraw-loser did NOT resurrect Games

        // An equal stamp (duplicate) is also dropped.
        m.applyFriendLibrarySnapshot(peer, R"({"Games":[{"cid":"cidX"}]})", 10);
        QCOMPARE(chg.count(), 1);
        QVERIFY(!cfg["FriendLibraries"][P].contains("Games"));

        // A higher stamp is accepted.
        m.applyFriendLibrarySnapshot(peer, R"({"Games":[{"cid":"cidX"}]})", 11);
        QCOMPARE(chg.count(), 2);
        QVERIFY(cfg["FriendLibraries"][P].contains("Games"));

        // A malformed message must NOT advance the high-water mark: after it, a valid seq-12 still applies.
        m.applyFriendLibrarySnapshot(peer, "garbage", 99);          // parse error — ignored, mark stays at 11
        m.applyFriendLibrarySnapshot(peer, R"({"Retro":[{"cid":"cidZ"}]})", 12);
        QCOMPARE(chg.count(), 3);
        QVERIFY(cfg["FriendLibraries"][P].contains("Retro"));
    }

    // Malformed snapshots must never corrupt the store: bad JSON / a non-object is ignored (record untouched), and
    // within a valid snapshot non-array libraries and non-string CIDs are dropped, empty libraries elided.
    void friend_snapshot_sanitizes_malformed_input()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}};
        AppModel m(&cfg, &appDir);
        const QString peer = "12D3KooWSeeder";
        const std::string P = peer.toStdString();
        QSignalSpy chg(&m, &AppModel::friendCatalogChanged);

        m.applyFriendLibrarySnapshot(peer, "not json at all");   // parse error → ignored
        QCOMPARE(chg.count(), 0);
        m.applyFriendLibrarySnapshot(peer, R"(["array","not","object"])");  // wrong shape → ignored
        QCOMPARE(chg.count(), 0);
        QVERIFY(!cfg.contains("FriendLibraries") || !cfg["FriendLibraries"].contains(P));

        // Mixed valid/invalid: "Good" keeps only its cid-bearing objects; "Bad" (non-array) and "Empty" (no valid
        // entries — a bare string, a number, an object with no cid) are elided.
        m.applyFriendLibrarySnapshot(peer, R"({"Good":[{"cid":"ok"},42,{"cid":"ok2","node":"n"}],"Bad":"nope","Empty":[7,"cid",{"node":"x"}]})");
        QCOMPARE(chg.count(), 1);
        QCOMPARE((int)cfg["FriendLibraries"][P].size(), 1);
        QCOMPARE(cfg["FriendLibraries"][P]["Good"].size(), (size_t)2);
        QVERIFY(!cfg["FriendLibraries"][P].contains("Bad"));
        QVERIFY(!cfg["FriendLibraries"][P].contains("Empty"));
    }

    // Ending a friendship forgets BOTH directions of sharing state: what we serve them (config["Sharing"]) and what
    // they shared with us (config["FriendLibraries"]) — and only signals when something was actually forgotten.
    void forget_friend_clears_both_directions()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        const std::string P = "12D3KooWFriend", Q = "12D3KooWOther";
        json cfg = json{{"Settings", json::object()},
                        {"Sharing", json{{P, json::array({"Games"})}, {Q, json::array({"Retro"})}}},
                        {"FriendLibraries", json{{P, json{{"Games", json::array({json{{"cid","cidX"}}})}}}}}};
        AppModel m(&cfg, &appDir);
        QSignalSpy chg(&m, &AppModel::friendCatalogChanged);

        m.forgetFriend(QString::fromStdString(P));
        QCOMPARE(chg.count(), 1);
        QVERIFY(!cfg["Sharing"].contains(P));            // our side dropped
        QVERIFY(!cfg["FriendLibraries"].contains(P));    // their side dropped
        QVERIFY(cfg["Sharing"].contains(Q));             // the OTHER friend is untouched

        m.forgetFriend(QString::fromStdString(P));       // already gone → no-op, no signal
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

        QVERIFY(!m.isReceivingFrom(peer));                 // receive: off by default
        m.setReceivingFrom(peer, true);  QVERIFY(m.isReceivingFrom(peer));
        m.setReceivingFrom(peer, false); QVERIFY(!m.isReceivingFrom(peer));

        QVERIFY(m.isPresenceSharedWith(peer));             // presence: shared by default
        m.setPresenceSharedWith(peer, false);
        QVERIFY(!m.isPresenceSharedWith(peer));            // hidden → in PresenceDeny
        m.setPresenceSharedWith(peer, true);
        QVERIFY(m.isPresenceSharedWith(peer));

        QVERIFY(m.isInVlan(peer));                         // vLAN: in by default
        m.setInVlan(peer, false);
        QVERIFY(!m.isInVlan(peer));                        // left → in LanExcludedPeers
        m.setInVlan(peer, true);
        QVERIFY(m.isInVlan(peer));

        QVERIFY(!m.autoAcceptEnabled());                   // auto-accept: off by default, persists
        m.setAutoAcceptEnabled(true);
        QVERIFY(m.autoAcceptEnabled());
        QVERIFY(cfg.value("AutoAcceptPeers", false));
    }

    // New-Peer defaults: presence ON, receive/vLAN OFF out of the box; applyNewPeerDefaults stamps a peer; forgetFriend
    // clears the per-peer records.
    void new_peer_defaults_and_apply()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}};
        AppModel m(&cfg, &appDir);

        QCOMPARE(m.newPeerDefault("presence"), true);
        QCOMPARE(m.newPeerDefault("receive"), false);
        QCOMPARE(m.newPeerDefault("vlan"), false);
        m.setNewPeerDefault("receive", true);
        m.setNewPeerShareDefault("Games", true);
        QCOMPARE(m.newPeerDefault("receive"), true);
        QVERIFY(m.newPeerShareDefaults().contains("Games"));
        m.setNewPeerShareDefault("Games", false);
        QVERIFY(!m.newPeerShareDefaults().contains("Games"));   // toggle back off (exercises the fixed else-if branch)

        const QString peer = "12D3KooWNewBBB";
        m.applyNewPeerDefaults(peer);
        QVERIFY(m.isReceivingFrom(peer));                       // receive default applied
        QVERIFY(m.isPresenceSharedWith(peer));                 // presence default (on) applied

        m.forgetFriend(peer);
        QVERIFY(!m.isReceivingFrom(peer));                     // forgetFriend clears the per-peer records
    }
};

QTEST_MAIN(AppModelTest)
#include "test_appmodel.moc"
