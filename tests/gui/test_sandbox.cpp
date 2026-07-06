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
    void on_by_default_with_overrides()
    {
        // Default ON: an unremarkable launch is sandboxed.
        QVERIFY(SandboxLayer::Requested(params({})));
        // The global default can be turned off (Settings.SandboxByDefault=false).
        QVERIFY(!SandboxLayer::Requested(params({}), /*DefaultOn=*/false));
        // An explicit per-launch/package/session VIDYAGOD_SANDBOX override always wins over the default.
        QVERIFY(!SandboxLayer::Requested(params({{"VIDYAGOD_SANDBOX", "off"}})));
        QVERIFY(!SandboxLayer::Requested(params({{"VIDYAGOD_SANDBOX", "false"}})));
        QVERIFY(SandboxLayer::Requested(params({{"VIDYAGOD_SANDBOX", "on"}}), /*DefaultOn=*/false));
        QVERIFY(SandboxLayer::Requested(params({{"VIDYAGOD_SANDBOX", "1"}}), /*DefaultOn=*/false));
    }

    void net_mode_from_var()
    {
        QCOMPARE(SandboxLayer::FromParams(params({{"VIDYAGOD_SANDBOX", "on"}})).Net == SandboxLayer::NetMode::Host, true);
        QCOMPARE(SandboxLayer::FromParams(params({{"VIDYAGOD_SANDBOX", "on"}, {"VIDYAGOD_SANDBOX_NET", "isolated"}})).Net
                     == SandboxLayer::NetMode::Isolated, true);
    }

    void wrap_nests_userns_readonly_root_and_sandbox_init()
    {
        SandboxLayer::Options O = SandboxLayer::FromParams(params({{"VIDYAGOD_SANDBOX", "on"}}));
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
        SandboxLayer::Options O = SandboxLayer::FromParams(
            params({{"VIDYAGOD_SANDBOX", "on"}, {"VIDYAGOD_SANDBOX_NET", "isolated"}}));
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
