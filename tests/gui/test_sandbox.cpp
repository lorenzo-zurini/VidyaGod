// Tests for SandboxLayer — the MANDATORY bubblewrap wrapper for the launched game. Wrap() is pure argv construction
// (no bwrap needed), so we assert: the network mode reads from VIDYAGOD_SANDBOX_NET; and the produced argv mounts the
// host root read-only, unshares the net only when isolated, and ends with `-- <origProgram> <origArgs...>` so the real command is preserved verbatim.

#include <QtTest>

#include "sandboxlayer.h"
#include "launchparams.h"

#include <QTemporaryDir>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

class SandboxTest : public QObject
{
    Q_OBJECT

    static ContainerParams params(const std::map<std::string, std::string> &Vars)
    {
        ContainerParams CP(std::filesystem::path("/tmp"), std::string("node"), std::string());
        CP.PackagePath = "/tmp";
        CP.UsesVFS = false;
        CP.CustomVariables = Vars;
        return CP;
    }

    static QProcessEnvironment env(const std::map<QString, QString> &Vars)
    {
        QProcessEnvironment E;
        for (const auto &[K, V] : Vars) E.insert(K, V);
        return E;
    }

private slots:
    // A runner's LD_LIBRARY_PATH (its bundled libraries) must not reach VidyaGod's own --sandbox-init: the bundled
    // GLib of an Ubuntu 22.04 build broke our Qt before main() ("undefined symbol", exit 127) on Raze and ECWolf.
    void the_games_loader_path_is_taken_out_of_the_inits_environment()
    {
        QProcessEnvironment Run = env({{"LD_LIBRARY_PATH", "/runner/lib"}, {"HOME", "/home/u"}});
        const auto Game = SandboxLayer::TakeLoaderEnv(Run, env({{"HOME", "/home/u"}}));
        QCOMPARE(Game.size(), size_t(1));
        QCOMPARE(QString::fromStdString(Game[0].Name), QStringLiteral("LD_LIBRARY_PATH"));
        QVERIFY(Game[0].Set);
        QCOMPARE(QString::fromStdString(Game[0].Value), QStringLiteral("/runner/lib"));
        QVERIFY(!Run.contains("LD_LIBRARY_PATH"));
        QCOMPARE(Run.value("HOME"), QStringLiteral("/home/u"));   // only loader variables move
    }

    void the_init_keeps_the_launchers_own_loader_path()
    {
        QProcessEnvironment Run = env({{"LD_LIBRARY_PATH", "/runner/lib"}});
        const auto Game = SandboxLayer::TakeLoaderEnv(Run, env({{"LD_LIBRARY_PATH", "/app/lib"}}));
        QCOMPARE(Game.size(), size_t(1));
        QCOMPARE(QString::fromStdString(Game[0].Value), QStringLiteral("/runner/lib"));
        QCOMPARE(Run.value("LD_LIBRARY_PATH"), QStringLiteral("/app/lib"));
    }

    void a_loader_variable_the_game_removes_is_removed_for_the_game_only()
    {
        QProcessEnvironment Run;
        const auto Game = SandboxLayer::TakeLoaderEnv(Run, env({{"LD_PRELOAD", "/host/hook.so"}}));
        QCOMPARE(Game.size(), size_t(1));
        QCOMPARE(QString::fromStdString(Game[0].Name), QStringLiteral("LD_PRELOAD"));
        QVERIFY(!Game[0].Set);
        QCOMPARE(Run.value("LD_PRELOAD"), QStringLiteral("/host/hook.so"));
    }

    void loader_variables_the_game_leaves_alone_are_not_passed()
    {
        QProcessEnvironment Run = env({{"LD_LIBRARY_PATH", "/app/lib"}});
        QVERIFY(SandboxLayer::TakeLoaderEnv(Run, env({{"LD_LIBRARY_PATH", "/app/lib"}})).empty());
        QCOMPARE(Run.value("LD_LIBRARY_PATH"), QStringLiteral("/app/lib"));
    }

    void a_runners_LD_AUDIT_and_tunables_are_taken_too()
    {
        QProcessEnvironment Run = env({{"LD_AUDIT", "/runner/audit.so"}, {"GLIBC_TUNABLES", "glibc.rtld.x=1"}, {"QT_X", "1"}});
        const auto Game = SandboxLayer::TakeLoaderEnv(Run, QProcessEnvironment());
        QCOMPARE(Game.size(), size_t(2));
        QVERIFY(!Run.contains("LD_AUDIT") && !Run.contains("GLIBC_TUNABLES"));
        QVERIFY(Run.contains("QT_X"));
    }

    // The launch's entry point: the run environment the init gets loses the game's loader path, the init's
    // arguments carry it.
    void a_launch_wraps_with_the_loader_path_moved_to_the_init()
    {
#ifdef _WIN32
        QSKIP("the re-invoked init is the Linux bwrap backend");
#else
        QProcessEnvironment Run = env({{"LD_LIBRARY_PATH", "/runner/lib"}, {"HOME", "/home/u"}});
        std::string Program = "/opt/game/run.sh"; QStringList Args;
        SandboxLayer::WrapGame(SandboxLayer::Options{}, Program, Args, Run, env({{"HOME", "/home/u"}}));
        QVERIFY(!Run.contains("LD_LIBRARY_PATH"));
        const int E = Args.indexOf("--env");
        QVERIFY(E >= 0 && E < Args.lastIndexOf("--"));
        QCOMPARE(Args.at(E + 1), QStringLiteral("LD_LIBRARY_PATH=/runner/lib"));
#endif
    }

    void wrap_hands_the_games_loader_variables_to_the_init()
    {
#ifdef _WIN32
        QSKIP("bwrap argv is the Linux backend; Windows sandboxing is the separate Sandboxie backend");
#endif
        SandboxLayer::Options O;
        O.GameEnv = {{"LD_LIBRARY_PATH", true, "/runner/lib"}, {"LD_PRELOAD", false, ""}};
        std::string Program = "/opt/game/run.sh"; QStringList Args{"x"};
        SandboxLayer::Wrap(O, Program, Args);
        const int Init = Args.indexOf("--sandbox-init"), Game = Args.lastIndexOf("--");
        const int E = Args.indexOf("--env"), U = Args.indexOf("--unenv");
        QVERIFY(Init >= 0 && E > Init && U > Init && E < Game && U < Game);
        QCOMPARE(Args.at(E + 1), QStringLiteral("LD_LIBRARY_PATH=/runner/lib"));
        QCOMPARE(Args.at(U + 1), QStringLiteral("LD_PRELOAD"));
        QCOMPARE(Args.at(Game + 1), QStringLiteral("/opt/game/run.sh"));
    }

    // The init itself: it starts with its own environment and the game it execs gets the passed values.
    void the_init_applies_the_loader_variables_to_the_game_it_execs()
    {
#ifdef _WIN32
        QSKIP("the sandbox init is Linux-only");
#else
        QTemporaryDir Dir;
        const std::string Out = (Dir.path() + "/env.txt").toStdString();
        const std::string Script = "printf '%s|%s' \"$LD_LIBRARY_PATH\" \"${LD_PRELOAD-unset}\" > '" + Out + "'";
        const pid_t Pid = fork();
        if (Pid == 0)
        {
            setenv("LD_PRELOAD", "", 1);   // present in the init's own environment, removed for the game
            std::vector<std::string> A{"VidyaGod", "--sandbox-init", "--env", "LD_LIBRARY_PATH=/runner/lib",
                                       "--unenv", "LD_PRELOAD", "--", "/bin/sh", "-c", Script};
            std::vector<char *> V; for (auto &S : A) V.push_back(S.data()); V.push_back(nullptr);
            _exit(SandboxLayer::RunSandboxInit(int(A.size()), V.data()));
        }
        int Status = 0; waitpid(Pid, &Status, 0);
        QVERIFY(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
        QFile F(QString::fromStdString(Out)); QVERIFY(F.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(F.readAll()), QStringLiteral("/runner/lib|unset"));
#endif
    }

    void net_mode_from_var()
    {
        QCOMPARE(SandboxLayer::FromParams(params({})).Net == SandboxLayer::NetMode::Host, true);
        QCOMPARE(SandboxLayer::FromParams(params({{"VIDYAGOD_SANDBOX_NET", "isolated"}})).Net
                     == SandboxLayer::NetMode::Isolated, true);
    }

    void wrap_nests_userns_readonly_root_and_sandbox_init()
    {
#ifdef _WIN32
        QSKIP("bwrap argv is the Linux backend; Windows sandboxing is the separate Sandboxie backend (sandboxlayer_win.cpp)");
#endif
        SandboxLayer::Options O = SandboxLayer::FromParams(params({}));
        O.Mounts.push_back({"/tmp/spec.json", "/tmp/mnt"});
        O.WorkDir = "/tmp/mnt/game";
        std::string Program = "/opt/game/run.sh";
        QStringList Args = {"--fullscreen", "level1"};
        SandboxLayer::Wrap(O, Program, Args);

        QCOMPARE(QString::fromStdString(Program), QStringLiteral("bwrap"));
        // One hermetic userns with CAP_SYS_ADMIN retained (so the init can FUSE-mount), host root read-only.
        QVERIFY(Args.contains("--unshare-user"));
        QVERIFY(Args.contains("--cap-add") && Args.contains("CAP_SYS_ADMIN"));
        const int Ro = Args.indexOf("--ro-bind");
        QVERIFY(Ro >= 0 && Args.at(Ro + 1) == "/" && Args.at(Ro + 2) == "/");
        // Host net mode → NO --unshare-net.
        QVERIFY(!Args.contains("--unshare-net"));
        // The payload re-invokes the sandbox init, passing the deferred mount + the chdir.
        QVERIFY(Args.contains("--sandbox-init"));
        const int Mnt = Args.indexOf("--mount");
        QVERIFY(Mnt >= 0 && Args.at(Mnt + 1) == "/tmp/spec.json" && Args.at(Mnt + 2) == "/tmp/mnt");
        const int Cd = Args.indexOf("--chdir");
        QVERIFY(Cd >= 0 && Args.at(Cd + 1) == "/tmp/mnt/game");
        // The real game command is preserved verbatim after the LAST "--".
        const int Sep = Args.lastIndexOf("--");
        QVERIFY(Sep >= 0);
        QCOMPARE(Args.at(Sep + 1), QStringLiteral("/opt/game/run.sh"));
        QCOMPARE(Args.at(Sep + 2), QStringLiteral("--fullscreen"));
        QCOMPARE(Args.at(Sep + 3), QStringLiteral("level1"));
    }

    void isolated_unshares_net_and_keeps_net_admin()
    {
#ifdef _WIN32
        QSKIP("bwrap --unshare-net is the Linux backend; Windows sandboxing is the separate Sandboxie backend");
#endif
        SandboxLayer::Options O = SandboxLayer::FromParams(
            params({{"VIDYAGOD_SANDBOX_NET", "isolated"}}));
        O.TunName = "vg-abcdef"; O.TunCidr = "10.66.7.2/24"; O.TunSock = "/tmp/t.sock";
        std::string Program = "/bin/true";
        QStringList Args;
        SandboxLayer::Wrap(O, Program, Args);
        QVERIFY(Args.contains("--unshare-net"));
        QVERIFY(Args.contains("CAP_NET_ADMIN"));
        // --unshare-net must come before the payload separator.
        QVERIFY(Args.indexOf("--unshare-net") < Args.indexOf("--sandbox-init"));
        // The TUN handoff is passed to the init.
        const int Tun = Args.indexOf("--tun");
        QVERIFY(Tun >= 0 && Args.at(Tun + 1) == "vg-abcdef" && Args.at(Tun + 2) == "10.66.7.2/24" && Args.at(Tun + 3) == "/tmp/t.sock");
    }
};

QTEST_MAIN(SandboxTest)
#include "test_sandbox.moc"
