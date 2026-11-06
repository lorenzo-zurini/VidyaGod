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
    std::shared_ptr<NodeIndex> Game(bool Secret = false)
    {
        auto Idx = std::make_shared<NodeIndex>();
        const json E = { {"LABEL", "Play"}, {"HOST", ManifestModel::MachinePlatform()}, {"EXE", "game"},
                         {"TILE", {{"UID", "lsgame"}, {"TITLE", "lsgame"}}} };
        json Layers = json::array({ json{{"DIR", "game"}}, json{{"EXEC", json::array({ E })}} });
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
    LaunchSession::Request Req(bool Secret = false)
    {
        LaunchSession::Request R;
        R.Index = Game(Secret);
        R.NodeId = "lsgame";
        R.ReadBack = false;
        return R;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(Data.isValid());
        AppPaths::SetDataRoot(Data.path().toStdString());   // never the real data dir
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
};

QTEST_MAIN(LaunchSessionTest)
#include "test_launchsession.moc"
