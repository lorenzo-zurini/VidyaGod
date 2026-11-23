// Tests for the friends / multiplayer social layer wiring on the C++ side. Two angles:
//  1) The offline node's profile + contacts round-trip through the C ABI (VidyaGodIPFS social.go), including
//     persistence across a StopNode/StartNode cycle — with VIDYAGOD_IPFS_OFFLINE so no real network is touched.
//  2) FriendsManager's event bridge: the node's inbound (kind, json) callback (IpfsNodeFriendCb) is invoked directly
//     and we assert it marshals to the correct Qt signal with the contact fields parsed out.
// The live peer-to-peer handshake itself is covered by the Go two-host test (friend_test.go) and the headless
// --friend-add/--friend-serve CLI (cross-machine).

#include <QtTest>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QCoreApplication>
#include <QTableWidget>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QDir>

#include <fstream>

#include "ipfswrapper.h"
#include "appmodel.h"
#include "networktab.h"
#include "friendsharing.h"

// The node→C++ event bridges (extern "C" in ipfswrapper.cpp). Calling them directly exercises the real kind-mapping +
// JSON parsing + queued signal emission that FriendsManager / SessionManager install.
extern "C" void IpfsNodeFriendCb(int kind, const char *json);
extern "C" void IpfsNodeSessionCb(int kind, const char *json);

class FriendsTest : public QObject
{
    Q_OBJECT

private slots:
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

        // Offline safety: no host → friend code empty and outbound requests fail gracefully (not crash).
        QVERIFY(IpfsWrapper::FriendCode().empty());
        QVERIFY(IpfsWrapper::FriendList().empty());
        QVERIFY(!IpfsWrapper::FriendAdd("12D3KooWSomeoneElse"));   // networking offline → false

        // Persistence: the profile survives a node restart (rewritten to <repo>/social.json).
        IpfsWrapper::StopNode();
        QVERIFY2(IpfsWrapper::StartNode(Repo, &Err), Err.c_str());
        P = IpfsWrapper::GetProfile();
        QCOMPARE(QString::fromStdString(P.Nick), QStringLiteral("alice"));
        QCOMPARE(QString::fromStdString(P.PicCID), QStringLiteral("QmAlicePic"));
        IpfsWrapper::StopNode();
    }

    void friendsmanager_marshals_request_event()
    {
        FriendsManager *FM = FriendsManager::instance();   // installs the callback + node bridge
        QVERIFY(FM);
        QSignalSpy Spy(FM, &FriendsManager::friendRequest);

        IpfsNodeFriendCb(0, R"({"peer":"12D3KooWBob","nick":"bob","pic":"QmBobPic","state":"incoming"})");
        QVERIFY(Spy.wait(1000));   // queued connection → pump the event loop
        QCOMPARE(Spy.count(), 1);
        QCOMPARE(Spy.first().at(0).toString(), QStringLiteral("12D3KooWBob"));
        QCOMPARE(Spy.first().at(1).toString(), QStringLiteral("bob"));
        QCOMPARE(Spy.first().at(2).toString(), QStringLiteral("QmBobPic"));
    }

    // Our own outgoing request (evFriendSent, kind 7) reaches the app as friendRequestSent — never as friendRequest,
    // the signal the auto-accept path listens to. Taken for an incoming request, it was auto-accepted on the spot:
    // the contact flipped to accepted, the node's retry loop (pending only) stopped, and a request whose first send
    // missed never reached the peer (laptop replication: the seeder "accepted" a friend who never heard of it).
    // The Go side proves addFriend emits kind 7 (TestOurOwnRequestIsNotAnIncomingOne). Teeth: map kind 7 to Request.
    void friendsmanager_marshals_our_own_request_as_sent()
    {
        FriendsManager *FM = FriendsManager::instance();
        QSignalSpy Sent(FM, &FriendsManager::friendRequestSent);
        QSignalSpy Req(FM, &FriendsManager::friendRequest);
        IpfsNodeFriendCb(7, R"({"peer":"12D3KooWOurRequest","state":"pending"})");   // evFriendSent
        QVERIFY(Sent.wait(1000));
        QCOMPARE(Sent.first().at(0).toString(), QStringLiteral("12D3KooWOurRequest"));
        QCoreApplication::processEvents();
        QCOMPARE(Req.count(), 0);
    }

    void friendsmanager_marshals_presence_and_removed()
    {
        FriendsManager *FM = FriendsManager::instance();
        QSignalSpy Presence(FM, &FriendsManager::friendPresence);
        QSignalSpy Removed(FM, &FriendsManager::friendRemoved);

        IpfsNodeFriendCb(3, R"({"peer":"12D3KooWPal","online":true})");   // evFriendPresence
        QVERIFY(Presence.wait(1000));
        QCOMPARE(Presence.first().at(0).toString(), QStringLiteral("12D3KooWPal"));
        QCOMPARE(Presence.first().at(1).toBool(), true);

        IpfsNodeFriendCb(5, R"({"peer":"12D3KooWPal"})");                 // evFriendRemoved
        QVERIFY(Removed.wait(1000));
        QCOMPARE(Removed.first().at(0).toString(), QStringLiteral("12D3KooWPal"));
    }

    void lan_launchvars_offline_safety()
    {
        // Offline node (no friends / no social routing): the host-less friend LAN yields no launch vars — the launch
        // path must degrade gracefully (empty map), not crash. The vIP/config logic itself is covered in Go
        // (friendlan_test.go); here we only assert the C ABI boundary is safe when there's nothing to join.
        QTemporaryDir Dir;
        std::string Err;
        QVERIFY(IpfsWrapper::StartNode((Dir.path() + "/ipfs").toStdString(), &Err));
        const auto Vars = IpfsWrapper::LanLaunchVars();
        QVERIFY(Vars.find("VIDYAGOD_PEER_VIPS") == Vars.end() || Vars.at("VIDYAGOD_PEER_VIPS").empty());
        IpfsWrapper::StopNode();
    }

    void networktab_renders_saved_contacts()
    {
        // Seed a social.json with one accepted contact, start the (offline) node so it loads, and confirm the
        // NetworkTab renders that contact — the read/render path from the address book to the table.
        QTemporaryDir Dir;
        const QString Repo = Dir.path() + "/ipfs";
        QDir().mkpath(Repo);
        {
            std::ofstream OF((Repo + "/social.json").toStdString());
            OF << R"({"profile":{"nick":"me"},"contacts":[{"peer":"12D3KooWSavedFriend","nick":"savedpal","state":"accepted","added":1}]})";
        }
        std::string Err;
        QVERIFY2(IpfsWrapper::StartNode(Repo.toStdString(), &Err), Err.c_str());

        nlohmann::ordered_json Cfg = nlohmann::ordered_json{{"Settings", nlohmann::ordered_json::object()},
                                                            {"LIBRARY", nlohmann::ordered_json::array()}};
        QDir AppDir(Dir.path());
        AppModel M(&Cfg, &AppDir);
        NetworkTab Tab(M);
        Tab.setActive(true);

        auto * Table = Tab.findChild<QTableWidget *>();
        QVERIFY(Table);
        // Row 0 is the "New peers" defaults row; the saved contact follows it.
        QVERIFY(Table->rowCount() >= 2);
        bool Found = false;
        for (int R = 0; R < Table->rowCount(); ++R)
            if (Table->item(R, 0) && Table->item(R, 0)->text().contains("savedpal")) Found = true;
        QVERIFY2(Found, "the saved contact must render in the Network matrix");

        IpfsWrapper::StopNode();
    }

    // "On your network": a request waiting on us is one press from accepted, a nearby device one press from asked, each
    // with the two ticks; one already asked says so; a friend is in the table, not here, and so is a request from off
    // this network (its name is anyone's to choose); each row shows the end of its code. The rows are rebuilt only
    // when what they show changes (a controller's focus stays on its row) and keep their ticks when they are. A Pair
    // that fails (networking off) shares nothing. Teeth: a row missing or wrong, a friend or a far request offered, no code, the rows
    // rebuilt every poll, ticks lost, ticks applied when the request was not sent.
    void nearby_devices_and_requests_are_one_press_from_paired()
    {
        QTemporaryDir Dir;
        const QString Repo = Dir.path() + "/ipfs";
        QDir().mkpath(Repo);
        {
            std::ofstream OF((Repo + "/social.json").toStdString());
            OF << R"({"profile":{"nick":"me"},"contacts":[{"peer":"12D3KooWAsking","nick":"LGZ-MacBook","state":"incoming","added":1},)"
                  R"({"peer":"12D3KooWFarAway","nick":"LGZ-SteamDeck","state":"incoming","added":2}]})";
        }
        std::string Err;
        QVERIFY2(IpfsWrapper::StartNode(Repo.toStdString(), &Err), Err.c_str());
        std::vector<IpfsWrapper::NearbyPeer> Near = {
            { "12D3KooWAsking", "LGZ-MacBook", "incoming" },
            { "12D3KooWDeckDeviceA1b2c3", "LGZ-SteamDeck", "" },
            { "12D3KooWAsked", "Laptop", "pending" },
            { "12D3KooWFriend", "Old friend", "accepted" } };
        NetworkTab::SetNearbySource([&Near] { return Near; });

        nlohmann::ordered_json Cfg = nlohmann::ordered_json{{"Settings", nlohmann::ordered_json::object()},
            {"LIBRARY", nlohmann::ordered_json::array()}, {"ShareSheet", nlohmann::ordered_json{{"bafkreia", "Games/[1] A/a.json"}}}};
        QDir AppDir(Dir.path());
        AppModel M(&Cfg, &AppDir);
        NetworkTab Tab(M);
        Tab.setActive(true);

        QVERIFY(Tab.findChild<QPushButton *>("accept:12D3KooWAsking"));
        QVERIFY(Tab.findChild<QPushButton *>("decline:12D3KooWAsking"));
        QVERIFY(Tab.findChild<QPushButton *>("pair:12D3KooWDeckDeviceA1b2c3"));
        QVERIFY(Tab.findChild<QCheckBox *>("shareMine:12D3KooWDeckDeviceA1b2c3"));
        QVERIFY(Tab.findChild<QCheckBox *>("receiveTheirs:12D3KooWDeckDeviceA1b2c3"));
        QVERIFY(Tab.findChild<QWidget *>("nearby:12D3KooWAsked"));
        QVERIFY(!Tab.findChild<QPushButton *>("pair:12D3KooWAsked"));     // asked already: nothing to press
        QVERIFY(!Tab.findChild<QWidget *>("nearby:12D3KooWFriend"));      // a friend: in the table
        QVERIFY(!Tab.findChild<QWidget *>("nearby:12D3KooWFarAway"));     // a request from off this network: in the table only
        QVERIFY(Tab.findChild<QWidget *>("nearby:12D3KooWDeckDeviceA1b2c3")->findChild<QLabel *>()->text().contains("…viceA1b2c3"));   // the end of its code

        QWidget * DeckRow = Tab.findChild<QWidget *>("nearby:12D3KooWDeckDeviceA1b2c3");
        Tab.findChild<QCheckBox *>("shareMine:12D3KooWDeckDeviceA1b2c3")->setChecked(true);
        QTest::qWait(2300);   // a poll with nothing new
        QCOMPARE(Tab.findChild<QWidget *>("nearby:12D3KooWDeckDeviceA1b2c3"), DeckRow);
        Near.push_back({ "12D3KooWNew", "New one", "" });
        QTRY_VERIFY_WITH_TIMEOUT(Tab.findChild<QPushButton *>("pair:12D3KooWNew"), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(Tab.findChildren<QWidget *>("nearby:12D3KooWDeckDeviceA1b2c3").size() == 1, 3000);
        QVERIFY(Tab.findChild<QCheckBox *>("shareMine:12D3KooWDeckDeviceA1b2c3")->isChecked());   // kept across the rebuild

        Tab.findChild<QPushButton *>("pair:12D3KooWDeckDeviceA1b2c3")->click();       // offline: no friend service to ask with
        QVERIFY(Tab.findChild<QLabel *>("nearbyStatus")->text().startsWith("Could not ask LGZ-SteamDeck"));
        QCOMPARE(Tab.findChild<QLabel *>("nearbyStatus")->textFormat(), Qt::PlainText);   // it carries a name anyone chose
        QVERIFY(!M.friends().isSharingWithFriend("12D3KooWDeckDeviceA1b2c3", "Games"));

        NetworkTab::SetNearbySource({});
        IpfsWrapper::StopNode();
    }
};

QTEST_MAIN(FriendsTest)
#include "test_friends.moc"
