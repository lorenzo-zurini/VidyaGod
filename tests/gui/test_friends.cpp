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
#include <QDir>

#include <fstream>

#include "ipfswrapper.h"
#include "appmodel.h"
#include "friendstab.h"

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

    void session_offline_safety()
    {
        // Offline node (VIDYAGOD_IPFS_OFFLINE): no session service, so create fails gracefully and the list is empty.
        QTemporaryDir Dir;
        std::string Err;
        QVERIFY(IpfsWrapper::StartNode((Dir.path() + "/ipfs").toStdString(), &Err));
        QVERIFY(IpfsWrapper::SessionCreate("QmGame", &Err).Id.empty());   // offline → empty id
        QVERIFY(IpfsWrapper::SessionList().empty());
        IpfsWrapper::StopNode();
    }

    void sessionmanager_marshals_roster_and_ended()
    {
        SessionManager *SM = SessionManager::instance();
        QVERIFY(SM);
        QSignalSpy Roster(SM, &SessionManager::sessionRoster);
        QSignalSpy Invite(SM, &SessionManager::sessionInvite);
        QSignalSpy Ended(SM, &SessionManager::sessionEnded);

        IpfsNodeSessionCb(0, R"({"id":"sid1","game":"QmG","host":"12D3KooWHost"})");            // evSessionInvite
        QVERIFY(Invite.wait(1000));
        QCOMPARE(Invite.first().at(0).toString(), QStringLiteral("sid1"));
        QCOMPARE(Invite.first().at(2).toString(), QStringLiteral("12D3KooWHost"));

        IpfsNodeSessionCb(1, R"({"id":"sid1","host":"12D3KooWHost","subnet":"10.66.7.0/24",
                                 "members":[{"peer":"12D3KooWHost","vip":"10.66.7.1","ready":true}]})"); // evSessionRoster
        QVERIFY(Roster.wait(1000));
        QCOMPARE(Roster.first().at(0).toString(), QStringLiteral("sid1"));

        IpfsNodeSessionCb(2, R"({"id":"sid1"})");                                                // evSessionEnded
        QVERIFY(Ended.wait(1000));
        QCOMPARE(Ended.first().at(0).toString(), QStringLiteral("sid1"));
    }

    void friendstab_renders_saved_contacts()
    {
        // Seed a social.json with one accepted contact, start the (offline) node so it loads, and confirm the
        // FriendsTab renders that contact — the read/render path from the address book to the table.
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
        FriendsTab Tab(M);
        Tab.setActive(true);

        auto * Table = Tab.findChild<QTableWidget *>();
        QVERIFY(Table);
        QCOMPARE(Table->rowCount(), 1);
        QCOMPARE(Table->item(0, 0)->text(), QStringLiteral("savedpal"));
        QCOMPARE(Table->item(0, 2)->text(), QStringLiteral("accepted"));

        IpfsWrapper::StopNode();
    }
};

QTEST_MAIN(FriendsTest)
#include "test_friends.moc"
