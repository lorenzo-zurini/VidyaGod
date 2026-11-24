// Tests for the peers / multiplayer social layer wiring on the C++ side. Two angles:
//  1) The offline node's profile + contacts round-trip through the C ABI (VidyaGodIPFS social.go), including
//     persistence across a StopNode/StartNode cycle — with VIDYAGOD_IPFS_OFFLINE so no real network is touched.
//  2) PeersManager's event bridge: the node's inbound (kind, json) callback (IpfsNodePeerCb) is invoked directly
//     and we assert it marshals to the correct Qt signal with the contact fields parsed out.
// The live peer-to-peer handshake itself is covered by the Go two-host test (peer_test.go) and the headless
// --peer-add/--peer-serve CLI (cross-machine).

#include <QtTest>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QCoreApplication>
#include <QTableWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QPushButton>
#include <QLabel>
#include <QPointer>

#include <algorithm>
#include <QDir>

#include <fstream>

#include "ipfswrapper.h"
#include "appmodel.h"
#include "networktab.h"
#include "peersharing.h"

// The node→C++ event bridges (extern "C" in ipfswrapper.cpp). Calling them directly exercises the real kind-mapping +
// JSON parsing + queued signal emission that PeersManager / SessionManager install.
extern "C" void IpfsNodePeerCb(int kind, const char *json);
extern "C" void IpfsNodeSessionCb(int kind, const char *json);

class PeersTest : public QObject
{
    Q_OBJECT

private slots:
    // A test that stops half-way leaves no node and no nearby list behind for the next one.
    void cleanup() { NetworkTab::SetNearbySource({}); IpfsWrapper::StopNode(); }

    // A peer whose presence flaps (a Deck on flaky Wi-Fi) is reconciled once when it comes online, not at every
    // "online" — each re-planned and re-enqueued its whole library; a while later it is again. Each "online" still
    // asks it for its libraries (a request made while it was away failed). Teeth: drop the PresenceReconcileMs check
    // (three reconciles); reconcile on "offline" too; skip the request with the re-enqueue.
    void a_flapping_peer_is_reconciled_once_a_while()
    {
        QTemporaryDir Dir; QVERIFY(Dir.isValid());
        nlohmann::ordered_json Cfg = nlohmann::ordered_json{{"Settings", nlohmann::ordered_json::object()},
                                                            {"ReceiveFrom", {"12D3KooWFlappy"}}};
        QDir AppDir(Dir.path());
        AppModel M(&Cfg, &AppDir);
        PeersManager *FM = PeersManager::instance();
        const int Before = M.peers().presenceReconciles(), Asked = M.peers().libraryRequests();
        for (int I = 0; I < 3; ++I) { emit FM->peerPresence("12D3KooWFlappy", true); emit FM->peerPresence("12D3KooWFlappy", false); }
        QCOMPARE(M.peers().presenceReconciles() - Before, 1);
        QCOMPARE(M.peers().libraryRequests() - Asked, 3);
        M.peers().PresenceReconcileMs = 0;
        emit FM->peerPresence("12D3KooWFlappy", true);
        QCOMPARE(M.peers().presenceReconciles() - Before, 2);
    }

    // VIDYAGOD_IPFS_OFFLINE=1 is set by ctest (see CMakeLists) so the node stays purely local — no swarm/DHT — which
    // keeps this test hermetic and makes the offline-safety assertions below meaningful.
    void profile_and_persistence_roundtrip_offline()
    {
        QTemporaryDir Dir;
        QVERIFY(Dir.isValid());
        const std::string Repo = (Dir.path() + "/ipfs").toStdString();

        std::string Err;
        QVERIFY2(IpfsWrapper::StartNode(Repo, &Err), Err.c_str());

        // Profile set/get through the C ABI.
        QVERIFY(IpfsWrapper::SetProfile("alice", "QmAlicePic"));
        IpfsWrapper::Profile P = IpfsWrapper::GetProfile();
        QCOMPARE(QString::fromStdString(P.Nick), QStringLiteral("alice"));
        QCOMPARE(QString::fromStdString(P.PicCID), QStringLiteral("QmAlicePic"));

        // Offline safety: no host → peer code empty and outbound requests fail gracefully (not crash).
        QVERIFY(IpfsWrapper::PeerCode().empty());
        QVERIFY(IpfsWrapper::PeerList().empty());
        QVERIFY(!IpfsWrapper::PeerAdd("12D3KooWSomeoneElse"));   // networking offline → false

        // Persistence: the profile survives a node restart (rewritten to <repo>/social.json).
        IpfsWrapper::StopNode();
        QVERIFY2(IpfsWrapper::StartNode(Repo, &Err), Err.c_str());
        P = IpfsWrapper::GetProfile();
        QCOMPARE(QString::fromStdString(P.Nick), QStringLiteral("alice"));
        QCOMPARE(QString::fromStdString(P.PicCID), QStringLiteral("QmAlicePic"));
        IpfsWrapper::StopNode();
    }

    void peersmanager_marshals_request_event()
    {
        PeersManager *FM = PeersManager::instance();   // installs the callback + node bridge
        QVERIFY(FM);
        QSignalSpy Spy(FM, &PeersManager::peerRequest);

        IpfsNodePeerCb(0, R"({"peer":"12D3KooWBob","nick":"bob","pic":"QmBobPic","state":"incoming"})");
        QVERIFY(Spy.wait(1000));   // queued connection → pump the event loop
        QCOMPARE(Spy.count(), 1);
        QCOMPARE(Spy.first().at(0).toString(), QStringLiteral("12D3KooWBob"));
        QCOMPARE(Spy.first().at(1).toString(), QStringLiteral("bob"));
        QCOMPARE(Spy.first().at(2).toString(), QStringLiteral("QmBobPic"));
    }

    // Our own outgoing request (evPeerSent, kind 7) reaches the app as peerRequestSent — never as peerRequest,
    // the signal the auto-accept path listens to. Taken for an incoming request, it was auto-accepted on the spot:
    // the contact flipped to accepted, the node's retry loop (pending only) stopped, and a request whose first send
    // missed never reached the peer (laptop replication: the seeder "accepted" a peer who never heard of it).
    // The Go side proves addPeer emits kind 7 (TestOurOwnRequestIsNotAnIncomingOne). Teeth: map kind 7 to Request.
    void peersmanager_marshals_our_own_request_as_sent()
    {
        PeersManager *FM = PeersManager::instance();
        QSignalSpy Sent(FM, &PeersManager::peerRequestSent);
        QSignalSpy Req(FM, &PeersManager::peerRequest);
        IpfsNodePeerCb(7, R"({"peer":"12D3KooWOurRequest","state":"pending"})");   // evPeerSent
        QVERIFY(Sent.wait(1000));
        QCOMPARE(Sent.first().at(0).toString(), QStringLiteral("12D3KooWOurRequest"));
        QCoreApplication::processEvents();
        QCOMPARE(Req.count(), 0);
    }

    void peersmanager_marshals_presence_and_removed()
    {
        PeersManager *FM = PeersManager::instance();
        QSignalSpy Presence(FM, &PeersManager::peerPresence);
        QSignalSpy Removed(FM, &PeersManager::peerRemoved);

        IpfsNodePeerCb(3, R"({"peer":"12D3KooWPal","online":true})");   // evPeerPresence
        QVERIFY(Presence.wait(1000));
        QCOMPARE(Presence.first().at(0).toString(), QStringLiteral("12D3KooWPal"));
        QCOMPARE(Presence.first().at(1).toBool(), true);

        IpfsNodePeerCb(5, R"({"peer":"12D3KooWPal"})");                 // evPeerRemoved
        QVERIFY(Removed.wait(1000));
        QCOMPARE(Removed.first().at(0).toString(), QStringLiteral("12D3KooWPal"));
    }

    void lan_launchvars_offline_safety()
    {
        // Offline node (no peers / no social routing): the host-less peer LAN yields no launch vars — the launch
        // path must degrade gracefully (empty map), not crash. The vIP/config logic itself is covered in Go
        // (peerlan_test.go); here we only assert the C ABI boundary is safe when there's nothing to join.
        QTemporaryDir Dir;
        std::string Err;
        QVERIFY(IpfsWrapper::StartNode((Dir.path() + "/ipfs").toStdString(), &Err));
        const auto Vars = IpfsWrapper::LanLaunchVars();
        QVERIFY(Vars.find("VIDYAGOD_PEER_VIPS") == Vars.end() || Vars.at("VIDYAGOD_PEER_VIPS").empty());
        IpfsWrapper::StopNode();
    }

    // The Peers page, as it reads the address book and the network. Every peer is the same:
    //  - the table: a row per peer (a request waiting on us is above it, not in it), its permissions in columns — Accepted,
    //    Share: <library> each, Receive, vLAN, Presence —, what of theirs is here, under the "New peers" row;
    //  - a request waiting on us, and a device nearby we have nothing to do with yet, ask for those same permissions where
    //    the press is, ticks starting as the "New peers" row; a request from off this network says to check its code;
    //  - a tick in the table reaches the model; a Pair that fails (networking off) gives nothing;
    //  - a code gets the same ticks; polling changes nothing on screen when nothing changed; a row keeps its ticks.
    // Teeth: a column or a row missing, a request in the table, ticks not starting as the row, the far warning missing or
    // on a near one, a table tick not reaching the model, permissions given on a failed Pair, rows rebuilt each poll.
    void the_peers_page_is_a_table_of_peers_and_pairing_asks_its_permissions()
    {
        QTemporaryDir Dir;
        const QString Repo = Dir.path() + "/ipfs";
        QDir().mkpath(Repo);
        {
            std::ofstream OF((Repo + "/social.json").toStdString());
            OF << R"({"profile":{"nick":"me"},"contacts":[)"
                  R"({"peer":"12D3KooWSavedPal","nick":"savedpal","state":"accepted","added":1},)"
                  R"({"peer":"12D3KooWAsking","nick":"LGZ-MacBook","state":"incoming","added":2},)"
                  R"({"peer":"12D3KooWFarAway","nick":"LGZ-SteamDeck","state":"incoming","added":3},)"
                  R"({"peer":"12D3KooWAskedThem","nick":"Laptop","state":"pending","added":4}]})";
        }
        std::string Err;
        QVERIFY2(IpfsWrapper::StartNode(Repo.toStdString(), &Err), Err.c_str());
        std::vector<IpfsWrapper::NearbyPeer> Near = {
            { "12D3KooWAsking", "LGZ-MacBook", "incoming" },
            { "12D3KooWDeckDeviceA1b2c3", "LGZ-SteamDeck", "" },
            { "12D3KooWSavedPal", "savedpal", "accepted" } };
        NetworkTab::SetNearbySource([&Near] { return Near; });

        nlohmann::ordered_json Cfg = nlohmann::ordered_json{{"Settings", nlohmann::ordered_json::object()},
            {"LIBRARY", nlohmann::ordered_json::array()}, {"ShareSheet", nlohmann::ordered_json{{"bafkreia", "Games/[1] A/a.json"}}},
            {"Sharing", {{"12D3KooWSavedPal", {"Games"}}}}, {"NewPeerDefaults", {{"receive", true}, {"vlan", false}, {"presence", true}}}};
        QDir AppDir(Dir.path());
        AppModel M(&Cfg, &AppDir);
        NetworkTab Tab(M);
        Tab.show();
        Tab.setActive(true);
        QApplication::processEvents();
        const auto Has = [&](const QString &Name) { return Tab.findChild<QWidget *>(Name) != nullptr; };
        const auto Box = [&](const QString &Name) { return Tab.findChild<QCheckBox *>(Name); };

        auto *Table = Tab.findChild<QTableWidget *>("peers");
        QVERIFY(Table);
        QStringList Headers;
        for (int C = 0; C < Table->columnCount(); ++C) Headers << (Table->horizontalHeaderItem(C) ? Table->horizontalHeaderItem(C)->text() : QString());
        QCOMPARE(Headers.mid(0, 7), (QStringList{"Peer", "Accepted", "Share: Games", "Receive", "vLAN", "Presence", "Their games"}));
        QCOMPARE(Table->rowCount(), 3);                                     // the row of defaults, the accepted peer, the one asked
        QVERIFY(Table->item(0, 0)->text().startsWith("New peers"));
        QVERIFY(Box("share:Games:12D3KooWSavedPal")->isChecked());
        QVERIFY(!Has("receive:12D3KooWAsking") || !Table->findChild<QCheckBox *>("receive:12D3KooWAsking"));   // a request is not in the table
        QVERIFY(Table->item(2, 0)->text().contains("asked"));

        for (const char *P : {"12D3KooWAsking", "12D3KooWFarAway"})
        {
            const QString Q = P;
            QVERIFY(Has("accept:" + Q) && Has("decline:" + Q));
            QVERIFY(Box("share:Games:" + Q) && !Box("share:Games:" + Q)->isChecked());   // the row: Games not shared
            QVERIFY(Box("receive:" + Q)->isChecked() && !Box("vlan:" + Q)->isChecked() && Box("presence:" + Q)->isChecked());
        }
        const auto Warned = [&](const char *P) {
            for (QLabel *L : Tab.findChild<QWidget *>(QString("waiting:") + P)->findChildren<QLabel *>())
                if (L->text().startsWith("Not on your network")) return true;
            return false; };
        QVERIFY(Warned("12D3KooWFarAway"));
        QVERIFY(!Warned("12D3KooWAsking"));
        QVERIFY(Has("pair:12D3KooWDeckDeviceA1b2c3") && Box("receive:12D3KooWDeckDeviceA1b2c3"));
        QVERIFY(!Has("nearby:12D3KooWSavedPal") && !Has("nearby:12D3KooWAsking"));   // in the book: not offered again
        QVERIFY(Has("sendRequest") && Box("share:Games:code") && Box("receive:code"));  // a code: the same ticks

        Box("receive:12D3KooWSavedPal")->setChecked(true);                    // a table tick reaches the model
        QVERIFY(M.peers().isReceivingFrom("12D3KooWSavedPal"));
        QTRY_VERIFY_WITH_TIMEOUT(Box("receive:12D3KooWSavedPal") && Box("receive:12D3KooWSavedPal")->isChecked(), 3000);

        QWidget *DeckRow = Tab.findChild<QWidget *>("nearby:12D3KooWDeckDeviceA1b2c3");
        Box("share:Games:12D3KooWDeckDeviceA1b2c3")->setChecked(true);
        QTest::qWait(2300);   // a poll with nothing new
        QCOMPARE(Tab.findChild<QWidget *>("nearby:12D3KooWDeckDeviceA1b2c3"), DeckRow);
        Near.push_back({ "12D3KooWNew", "New one", "" });
        QTRY_VERIFY_WITH_TIMEOUT(Has("pair:12D3KooWNew"), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(Tab.findChildren<QWidget *>("nearby:12D3KooWDeckDeviceA1b2c3").size() == 1, 3000);
        QVERIFY(Box("share:Games:12D3KooWDeckDeviceA1b2c3")->isChecked());   // kept across the rebuild

        Tab.findChild<QPushButton *>("pair:12D3KooWDeckDeviceA1b2c3")->click();   // offline: no service to ask with
        QVERIFY(Tab.findChild<QLabel *>("peersStatus")->text().startsWith("Could not ask LGZ-SteamDeck"));
        QCOMPARE(Tab.findChild<QLabel *>("peersStatus")->textFormat(), Qt::PlainText);
        QVERIFY(!M.peers().isSharingWithPeer("12D3KooWDeckDeviceA1b2c3", "Games") && !M.peers().hasChosen("12D3KooWDeckDeviceA1b2c3"));
    }

    // With nothing waiting and nobody yet, no request section shows; the table is its row of defaults, the hint
    // under it says how to add someone.
    void an_empty_peers_page_shows_only_how_to_add_someone()
    {
        QTemporaryDir Dir;
        std::string Err;
        QVERIFY2(IpfsWrapper::StartNode((Dir.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        NetworkTab::SetNearbySource([] { return std::vector<IpfsWrapper::NearbyPeer>{}; });
        nlohmann::ordered_json Cfg = nlohmann::ordered_json{{"Settings", nlohmann::ordered_json::object()}};
        QDir AppDir(Dir.path());
        AppModel M(&Cfg, &AppDir);
        NetworkTab Tab(M);
        Tab.show();
        Tab.setActive(true);
        QApplication::processEvents();
        int Shown = 0;
        for (QGroupBox *G : Tab.findChildren<QGroupBox *>()) if (G->isVisible()) { ++Shown; QCOMPARE(G->title(), QString("Add someone")); }
        QCOMPARE(Shown, 1);
        QVERIFY(Tab.findChild<QLabel *>("emptyHint")->isVisible());
        QCOMPARE(Tab.findChild<QTableWidget *>("peers")->rowCount(), 1);   // the row of defaults alone
        // Hidden, a change does not rebuild the page (every catalog change and presence ping rebuilt it, and stat'd a
        // receiving peer's every file); shown again, it is rebuilt once. Teeth: drop the Showing gate (three refreshes
        // while hidden); drop the refresh on show (the page stays as it was).
        Tab.setActive(false);
        const int Before = Tab.refreshes();
        for (int I = 0; I < 3; ++I) { emit M.catalogChanged(); QApplication::processEvents(); }
        QCOMPARE(Tab.refreshes(), Before);
        Tab.setActive(true);
        QCOMPARE(Tab.refreshes(), Before + 1);
        NetworkTab::SetNearbySource({});
        IpfsWrapper::StopNode();
    }
};

QTEST_MAIN(PeersTest)
#include "test_peers.moc"
