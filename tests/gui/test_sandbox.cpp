// Tests for SandboxLayer — the opt-in bubblewrap wrapper for the launched game. Wrap() is pure argv construction
// (no bwrap needed), so we assert: the sandbox is off unless VIDYAGOD_SANDBOX is truthy; the network mode reads from
// VIDYAGOD_SANDBOX_NET; and the produced argv mounts the host root read-only, unshares the net only when isolated,
// and ends with `-- <origProgram> <origArgs...>` so the real command is preserved verbatim.

#include <QtTest>

#include "sandboxlayer.h"
#include "launchparams.h"

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

private slots:
    void off_by_default()
    {
        QVERIFY(!SandboxLayer::Requested(params({})));
        QVERIFY(!SandboxLayer::Requested(params({{"VIDYAGOD_SANDBOX", "off"}})));
        QVERIFY(SandboxLayer::Requested(params({{"VIDYAGOD_SANDBOX", "on"}})));
        QVERIFY(SandboxLayer::Requested(params({{"VIDYAGOD_SANDBOX", "true"}})));
        QVERIFY(SandboxLayer::Requested(params({{"VIDYAGOD_SANDBOX", "1"}})));
    }

    void net_mode_from_var()
    {
        QCOMPARE(SandboxLayer::FromParams(params({{"VIDYAGOD_SANDBOX", "on"}})).Net == SandboxLayer::NetMode::Host, true);
        QCOMPARE(SandboxLayer::FromParams(params({{"VIDYAGOD_SANDBOX", "on"}, {"VIDYAGOD_SANDBOX_NET", "isolated"}})).Net
                     == SandboxLayer::NetMode::Isolated, true);
    }

    void wrap_preserves_command_and_readonly_root()
    {
        SandboxLayer::Options O = SandboxLayer::FromParams(params({{"VIDYAGOD_SANDBOX", "on"}}));
        std::string Program = "/opt/game/run.sh";
        QStringList Args = {"--fullscreen", "level1"};
        SandboxLayer::Wrap(O, Program, Args);

        QCOMPARE(QString::fromStdString(Program), QStringLiteral("bwrap"));
        // Host root mounted read-only.
        const int Ro = Args.indexOf("--ro-bind");
        QVERIFY(Ro >= 0);
        QCOMPARE(Args.at(Ro + 1), QStringLiteral("/"));
        QCOMPARE(Args.at(Ro + 2), QStringLiteral("/"));
        // Host net mode → NO --unshare-net.
        QVERIFY(!Args.contains("--unshare-net"));
        // The real command is preserved verbatim after "--".
        const int Sep = Args.indexOf("--");
        QVERIFY(Sep >= 0);
        QCOMPARE(Args.at(Sep + 1), QStringLiteral("/opt/game/run.sh"));
        QCOMPARE(Args.at(Sep + 2), QStringLiteral("--fullscreen"));
        QCOMPARE(Args.at(Sep + 3), QStringLiteral("level1"));
    }

    void isolated_unshares_net()
    {
        SandboxLayer::Options O = SandboxLayer::FromParams(
            params({{"VIDYAGOD_SANDBOX", "on"}, {"VIDYAGOD_SANDBOX_NET", "isolated"}}));
        std::string Program = "/bin/true";
        QStringList Args;
        SandboxLayer::Wrap(O, Program, Args);
        QVERIFY(Args.contains("--unshare-net"));
        // --unshare-net must come before the "--" command separator.
        QVERIFY(Args.indexOf("--unshare-net") < Args.indexOf("--"));
    }
};

QTEST_MAIN(SandboxTest)
#include "test_sandbox.moc"
