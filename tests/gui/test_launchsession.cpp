// test_launchsession.cpp — one launch's lifecycle (engine/launchsession.h): what a plan, a refused build and an early
// stop must leave alone. Each of these was a regression a review reproduced: a dry run whose build was refused
// deleted the running session's writable layer; a plan saved a pool secret and so made the instance a typo named.

#include <QtTest/QtTest>
#include <QTemporaryDir>

#include "apppaths.h"
#include "containerwrapper.h"
#include "instancestore.h"
#include "launchsession.h"
#include "manifestmodel.h"
#include "sessions.h"

#include <fcntl.h>
#include <fstream>
#include <sys/file.h>
#include <unistd.h>

using json = nlohmann::ordered_json;

class LaunchSessionTest : public QObject
{
    Q_OBJECT
    QTemporaryDir Data;
    json Cfg;

    //One game: a folder and a Play entry on this machine — and, with Secret, a key drawn from a pool.
    std::shared_ptr<NodeIndex> Game(bool Secret = false, bool Keeps = false)
    {
        //The program: a script that writes the file a KEEP layer keeps.
        const std::filesystem::path Dir = (Data.path() + "/bundle/game").toStdString();
        std::filesystem::create_directories(Dir);
        { std::ofstream G(Dir / "game"); G << "#!/bin/sh\necho kept > \"$(dirname \"$0\")/save.dat\"\nexit 0\n"; }
        std::filesystem::permissions(Dir / "game", std::filesystem::perms::owner_all);
        auto Idx = std::make_shared<NodeIndex>();
        const json E = { {"LABEL", "Play"}, {"HOST", ManifestModel::MachinePlatform()}, {"EXE", "game"},
                         {"TILE", {{"UID", "lsgame"}, {"TITLE", "lsgame"}}} };
        json Layers = json::array({ json{{"DIR", "game"}}, json{{"EXEC", json::array({ E })}} });
        if (Keeps) Layers.push_back(json{{"KEEP", {{"FILES/save.dat", true}}}});
        if (Secret)
            Layers.insert(Layers.begin(), json{{"VARS", {{"cdkey", {{"DEFAULT", ""},
                {"UI", {{"CONTROL", "secret"}, {"LABEL", "CD key"}, {"POOL", json::array({"AAAA-1111", "BBBB-2222"})}}}}}}}});
        Node N;
        ManifestModel::ParseNode(json{ {"CID", "lsgame"}, {"LABEL", "lsgame"}, {"VARIANT", "v"}, {"LAYERS", Layers} },
                                 "lsgame.json", (Data.path() + "/bundle").toStdString(), N);
        Idx->Nodes["lsgame"] = N;
        ManifestModel::DeriveFacts(*Idx);
        return Idx;
    }
    LaunchSession::Request Req(bool Secret = false, bool Keeps = false)
    {
        LaunchSession::Request R;
        R.Index = Game(Secret, Keeps);
        R.NodeId = "lsgame";
        R.ReadBack = false;
        return R;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(Data.isValid());
        AppPaths::SetDataRoot(Data.path().toStdString());   // never the real data dir
        qputenv("TMPDIR", (Data.path() + "/tmp").toUtf8());     // nor the app's real session root
        QDir().mkpath(Data.path() + "/tmp");
        Cfg = json{{"Settings", {{"Paths", {{"UserDataRoot", (Data.path() + "/USERDATA").toStdString()}}}}}};
    }

    // A dry run whose build is refused (the instance is already running: another launch holds its session) leaves
    // that session's writable layer alone — the running game's kept files and registry hives are captured from it.
    // Teeth: remove a dry run's layer whether or not this launch built it.
    void a_refused_dry_run_leaves_the_running_sessions_layer_alone()
    {
        std::filesystem::path Temp;
        {
            LaunchSession::Session A(Cfg, Req());
            std::string Why;
            QVERIFY2(A.Resolve(&Why), Why.c_str());
            Temp = A.Wrapper()->ContainerParams.TempPath;
        }
        std::filesystem::create_directories(Temp / "WRITELAYER");
        { std::ofstream(Temp / "WRITELAYER" / "save.dat") << "precious"; }
        const int Fd = ::open(Sessions::LockPath(Temp).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);   // the live launch
        QVERIFY(Fd >= 0);
        QVERIFY(::flock(Fd, LOCK_EX | LOCK_NB) == 0);
        LaunchSession::Request D = Req();
        D.DryRun = true;
        {
            LaunchSession::Session B(Cfg, D);
            std::string Why;
            QVERIFY2(B.Resolve(&Why), Why.c_str());
            QVERIFY(!B.Build(&Why));
        }
        const bool Survived = std::filesystem::exists(Temp / "WRITELAYER" / "save.dat");
        ::close(Fd);
        QVERIFY(Survived);
    }

    // A plan writes nothing: a secret drawn from its pool is kept by a launch's build, not by the resolve — so a plan
    // makes no instance (and `--instance Tpyo` still finds none). Teeth: keep the secret at resolve.
    void a_plan_writes_nothing()
    {
        LaunchSession::Session S(Cfg, Req(true));
        std::string Why;
        QVERIFY2(S.Resolve(&Why), Why.c_str());
        QVERIFY(!S.Wrapper()->ContainerParams.PickedSecrets.empty());   // one was drawn
        QVERIFY(InstanceStore::List(Cfg, S.Wrapper()->ContainerParams.PackageUID).empty());
    }

    // A stop asked before the build ends the launch there: nothing is built, nothing started. Teeth: let the stop
    // reach only a running game.
    void a_stop_before_the_build_starts_nothing()
    {
        LaunchSession::Session S(Cfg, Req());
        std::string Why;
        QVERIFY2(S.Resolve(&Why), Why.c_str());
        S.Stop();
        QVERIFY(!S.Build(&Why));
        QVERIFY(Why.find("stopped") != std::string::npos);
        const LaunchSession::Outcome O = S.Run();
        QVERIFY(!O.Started);
    }

    // A launch keeps the secret it drew from the pool (the next one draws the same: what the game wrote with it still
    // matches); `launch build`'s look inside does not, nor makes the instance. Teeth: keep it in neither; in both.
    void a_launch_keeps_its_pool_secret_and_a_look_inside_does_not()
    {
        std::string Uid;
        {
            LaunchSession::Session S(Cfg, Req(true));
            std::string Why;
            QVERIFY2(S.Resolve(&Why), Why.c_str());
            QVERIFY2(S.Build(&Why, false), Why.c_str());
            Uid = S.Wrapper()->ContainerParams.PackageUID;
            QVERIFY(S.Cleanup());
        }
        QVERIFY(InstanceStore::List(Cfg, Uid).empty());
        std::string Drawn, Inst;
        {
            LaunchSession::Session S(Cfg, Req(true));
            std::string Why;
            QVERIFY2(S.Resolve(&Why), Why.c_str());
            QVERIFY2(S.Build(&Why), Why.c_str());
            Drawn = S.Wrapper()->ContainerParams.PickedSecrets.begin()->second;
            Inst = S.Wrapper()->ContainerParams.InstanceName;
            S.Cleanup();
        }
        const json I = InstanceStore::ReadConfig(Cfg, Uid, Inst);
        QVERIFY(I.contains("VARIABLES") && I["VARIABLES"].contains("cdkey"));
        QCOMPARE(I["VARIABLES"]["cdkey"].get<std::string>(), Drawn);
    }

    // A stop that lands between the build and the start: the game is not started, and it is a stop, not a failure —
    // asked of the session, or of the wrapper itself (the race KillGame used to lose: no process yet, nothing killed).
    // Teeth: drop the session's check before Run; drop the wrapper's latch in Execute.
    void a_stop_between_the_build_and_the_start_starts_nothing()
    {
        for (const bool OnWrapper : {false, true})
        {
            LaunchSession::Session S(Cfg, Req());
            std::string Why;
            QVERIFY2(S.Resolve(&Why), Why.c_str());
            QVERIFY2(S.Build(&Why, false), Why.c_str());
            if (OnWrapper) S.Wrapper()->KillGame(); else S.Stop();
            const LaunchSession::Outcome O = S.Run();
            QVERIFY(!O.Started);
            QVERIFY(O.Stopped);
            QVERIFY(!std::filesystem::exists(S.Wrapper()->ContainerParams.TempPath / "WRITELAYER" / "save.dat"));
        }
    }

    // A capture that fails fails the cleanup (and so `launch teardown`): the game wrote a kept file, and the instance's
    // folder cannot be made. Teeth: report the wrapper's own Cleanup (true while no mount is stuck).
    void a_failed_capture_fails_the_cleanup()
    {
        const QString Ro = Data.path() + "/ro";
        QDir().mkpath(Ro);
        const json Locked = json{{"Settings", {{"Paths", {{"UserDataRoot", (Ro + "/USERDATA").toStdString()}}}}}};
        LaunchSession::Session S(Locked, Req(false, true));
        std::string Why;
        QVERIFY2(S.Resolve(&Why), Why.c_str());
        QVERIFY2(S.Build(&Why, false), Why.c_str());
        { std::ofstream(S.Wrapper()->ContainerParams.RuntimePath / "save.dat") << "kept"; }   // what the game would write
        QFile::setPermissions(Ro, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
        const bool Kept = S.Cleanup();
        QFile::setPermissions(Ro, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        QVERIFY(!Kept);
    }
};

QTEST_MAIN(LaunchSessionTest)
#include "test_launchsession.moc"
