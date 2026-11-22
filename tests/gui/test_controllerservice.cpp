// The controller service (ControllerService): VidyaGod's own InputPlumber, run as its child while controllers are in
// use. Driven against mock_inputplumber on a private bus — never the real InputPlumber, which would take the
// controllers plugged into the machine running the tests.
#include <QtTest/QtTest>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusVariant>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>

#include "controllerservice.h"
#include "launchparams.h"   // ContainerParams
#include "padinput.h"
#include "sandboxlayer.h"

#include <unistd.h>   // link

namespace
{
const QString Root = QStringLiteral("/org/shadowblip/InputPlumber");

// A private message bus for one test: a dbus-daemon of its own, gone with the test.
struct PrivateBus
{
    QProcess Daemon;
    QString Address;
    PrivateBus()
    {
        Daemon.start(QStringLiteral("dbus-daemon"), { QStringLiteral("--session"), QStringLiteral("--nofork"), QStringLiteral("--nopidfile"),
                                                      QStringLiteral("--print-address=1") });
        if (Daemon.waitForStarted(5000) && Daemon.waitForReadyRead(5000)) Address = QString::fromUtf8(Daemon.readLine()).trimmed();
    }
    ~PrivateBus() { Daemon.terminate(); Daemon.waitForFinished(3000); }
};

QVariant Prop(const QString &Address, const QString &Path, const QString &Iface, const QString &Name)
{
    QDBusConnection C = QDBusConnection::connectToBus(Address, QStringLiteral("test-reader"));
    QDBusInterface P(QStringLiteral("org.shadowblip.InputPlumber"), Path, QStringLiteral("org.freedesktop.DBus.Properties"), C);
    const QDBusReply<QDBusVariant> R = P.call(QStringLiteral("Get"), Iface, Name);
    return R.isValid() ? R.value().variant() : QVariant();
}

const char *Layout = "version: 1\nkind: DeviceProfile\nname: Trackpad Mouse\nmapping: []\n";
}

class ControllerServiceTest : public QObject
{
    Q_OBJECT
    ControllerService &S = ControllerService::instance();

    // Points the service at the mock on Bus, with these source nodes.
    void UseMock(const PrivateBus &Bus, const QString &Sources, int HotplugMs = 0, const QString &Report = {})
    {
        qputenv("MOCK_IP_SOURCES", Sources.toUtf8());
        qputenv("MOCK_IP_HOTPLUG_MS", QByteArray::number(HotplugMs));
        qputenv("MOCK_IP_REPORT", Report.toUtf8());
        S.setBus(Bus.Address);
        S.setProgram(QStringLiteral(MOCK_INPUTPLUMBER));
    }

private slots:
    void cleanup()
    {
        S.reset();
        S.setProgram({});
        QDBusConnection::disconnectFromBus(QStringLiteral("test-reader"));
    }

    // Only the app names the program (MainWindow): with none, nothing runs whatever is wanted — a test that opens
    // controller mode never starts the real InputPlumber. Teeth: a default program, or want() starting without one.
    void it_never_runs_unless_the_app_names_its_program()
    {
        QVERIFY(S.program().isEmpty());
        S.want(ControllerService::Need::ControllerMode, true);
        QVERIFY(!S.running());
        QCOMPARE(S.unavailable(), QStringLiteral("no InputPlumber program"));
        S.setProgram(QStringLiteral("/nonexistent/vidyagod-inputplumber"));
        QVERIFY(S.unavailable().startsWith(QStringLiteral("InputPlumber not found")));
        QVERIFY(!S.running());
    }

    // Controller mode wants it: it starts, takes every controller it has a configuration for (ManageAllDevices),
    // and VidyaGod then reads only virtual controllers; let go, it stops and VidyaGod reads the real ones again.
    // Teeth: ManageAllDevices never set, PadInput still reading real controllers (or reading the Deck directly) while
    // it owns them, or the process left running.
    void it_runs_while_wanted_and_takes_the_controllers()
    {
        PrivateBus Bus;
        QVERIFY(!Bus.Address.isEmpty());
        UseMock(Bus, QStringLiteral("/dev/hidraw-test0,/dev/input/event-test0"));
        S.want(ControllerService::Need::ControllerMode, true);
        QTRY_VERIFY_WITH_TIMEOUT(S.running(), 10000);
        QCOMPARE(Prop(Bus.Address, Root + QStringLiteral("/Manager"), QStringLiteral("org.shadowblip.InputManager"), QStringLiteral("ManageAllDevices")).toBool(), true);
        const QList<ControllerService::Device> D = S.devices();
        QCOMPARE(D.size(), 1);
        QCOMPARE(D[0].Name, QStringLiteral("Mock Deck"));
        QCOMPARE(D[0].Sources, (QStringList{ QStringLiteral("/dev/hidraw-test0"), QStringLiteral("/dev/input/event-test0") }));
        QCOMPARE(S.hiddenNodes(), (std::vector<std::string>{ "/dev/hidraw-test0", "/dev/input/event-test0" }));
        QVERIFY(PadInput::instance().onlyVirtual());
        QVERIFY(!PadInput::instance().deckDirect());
        S.want(ControllerService::Need::ControllerMode, false);
        QVERIFY(!S.running());
        QVERIFY(S.hiddenNodes().empty());
        QVERIFY(!PadInput::instance().onlyVirtual());
        QTRY_VERIFY_WITH_TIMEOUT(!QDBusConnection::connectToBus(Bus.Address, QStringLiteral("test-reader")).interface()
                                     ->isServiceRegistered(QStringLiteral("org.shadowblip.InputPlumber")), 5000);
    }

    // It runs as VidyaGod's own child: polkit off (only VidyaGod reaches it), its device configurations first in
    // XDG_DATA_DIRS, and a working folder that holds no ./rootfs of anyone's. Teeth: any of the three dropped.
    void it_starts_with_its_own_configurations_and_no_polkit()
    {
        PrivateBus Bus;
        QTemporaryDir Tmp;
        const QString Report = Tmp.filePath(QStringLiteral("report"));
        UseMock(Bus, {}, 0, Report);
        S.setDataDir(QStringLiteral("/opt/vg/share"));
        S.want(ControllerService::Need::ControllerMode, true);
        QTRY_VERIFY_WITH_TIMEOUT(S.running(), 10000);
        QFile F(Report);
        QVERIFY(F.open(QIODevice::ReadOnly));
        const QString R = QString::fromUtf8(F.readAll());
        QVERIFY2(R.contains(QLatin1String("INSECURE_DISABLE_POLKIT=1\n")), qPrintable(R));
        QVERIFY2(R.contains(QLatin1String("XDG_DATA_DIRS=/opt/vg/share:")), qPrintable(R));
        QVERIFY2(R.contains(QLatin1String("PWD=/\n")), qPrintable(R));
        S.setDataDir({});
    }

    // A game keeps it running past controller mode, but a game alone never starts it (it would appear mid-launch,
    // after the game's sandbox was built). Teeth: Game counted as a reason to start, or not as one to keep running.
    void a_game_keeps_it_running_but_never_starts_it()
    {
        PrivateBus Bus;
        UseMock(Bus, QStringLiteral("/dev/hidraw-test1"));
        S.want(ControllerService::Need::Game, true);
        QTest::qWait(300);
        QVERIFY(!S.running());
        S.want(ControllerService::Need::ControllerMode, true);
        QTRY_VERIFY_WITH_TIMEOUT(S.running(), 10000);
        S.want(ControllerService::Need::ControllerMode, false);
        QVERIFY(S.running());
        S.want(ControllerService::Need::Game, false);
        QVERIFY(!S.running());
    }

    // A layout reaches every controller: those there when it comes up, and one plugged in later; a layout a
    // controller refuses is reported. Teeth: the layout not loaded at start, not reaching the hotplugged one, or a
    // refusal swallowed.
    void a_layout_reaches_every_controller_now_and_later()
    {
        PrivateBus Bus;
        UseMock(Bus, QStringLiteral("/dev/hidraw-test2"), 4000);
        QVERIFY(S.loadLayout(QString::fromLatin1(Layout)));
        S.want(ControllerService::Need::ControllerMode, true);
        QTRY_VERIFY_WITH_TIMEOUT(S.running(), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(S.devices().value(0).Profile, QStringLiteral("Trackpad Mouse"), 2000);
        QCOMPARE(S.devices().size(), 1);   // as it came up: not by the later controller's arrival
        QTRY_COMPARE_WITH_TIMEOUT(S.devices().size(), 2, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(S.devices().value(1).Profile, QStringLiteral("Trackpad Mouse"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(S.hiddenNodes().size() == 2, 5000);
        QString Error;
        QVERIFY(!S.loadLayout(QStringLiteral("name: not a profile\n"), &Error));
        QVERIFY2(Error.contains(QLatin1String("not a DeviceProfile")), qPrintable(Error));
    }

    // A game's layout goes over VidyaGod's while it is set; VidyaGod's waits under it (a change to it shows when the
    // game's comes off); "" is InputPlumber's own default profile, from its device configurations. Teeth: the game's
    // not loaded, VidyaGod's loaded over the game's, not restored, or the default never loaded.
    void a_games_layout_goes_over_and_comes_off()
    {
        PrivateBus Bus;
        QTemporaryDir Data;
        QVERIFY(QDir().mkpath(Data.filePath(QStringLiteral("inputplumber/profiles"))));
        QFile D(Data.filePath(QStringLiteral("inputplumber/profiles/default.yaml")));
        QVERIFY(D.open(QIODevice::WriteOnly));
        D.write("version: 1\nkind: DeviceProfile\nname: Default\nmapping: []\n");
        D.close();
        UseMock(Bus, QStringLiteral("/dev/hidraw-test3"));
        S.setDataDir(Data.path());
        QVERIFY(S.loadLayout(QString::fromLatin1(Layout)));
        S.want(ControllerService::Need::ControllerMode, true);
        QTRY_VERIFY_WITH_TIMEOUT(S.running(), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(S.devices().value(0).Profile, QStringLiteral("Trackpad Mouse"), 5000);
        QVERIFY(S.setGameLayout(QStringLiteral("{\"version\":1,\"kind\":\"DeviceProfile\",\"name\":\"Game Keys\",\"mapping\":[]}")));
        QCOMPARE(S.devices().value(0).Profile, QStringLiteral("Game Keys"));
        QVERIFY(S.loadLayout(QStringLiteral("version: 1\nkind: DeviceProfile\nname: Menu Pad\nmapping: []\n")));
        QCOMPARE(S.devices().value(0).Profile, QStringLiteral("Game Keys"));   // the game's stays on
        QVERIFY(S.setGameLayout({}));
        QCOMPARE(S.devices().value(0).Profile, QStringLiteral("Menu Pad"));
        QVERIFY(S.loadLayout({}));
        QCOMPARE(S.devices().value(0).Profile, QStringLiteral("Default from default.yaml"));
        S.setDataDir({});
    }

    // A harness running the real app (these tests, cli_sweep, a scripted drive) sets VIDYAGOD_NO_CONTROLLER_SERVICE=1
    // and the app names no InputPlumber: with every device managed it takes ANY gamepad, the machine's own included (it
    // once took this PC's own pad through cli_sweep). Run safely: the app is a hard link in a folder of its own, beside
    // the MOCK named `vidyagod-inputplumber`, on a private bus — a broken guard starts the mock, never the real one.
    // Teeth: the guard gone (the app reports the service available, then running).
    void a_harness_running_the_app_never_starts_the_service()
    {
        const QFileInfo App(QStringLiteral(VIDYAGOD_BIN));
        QTemporaryDir Dir(App.absolutePath() + QStringLiteral("/guard-XXXXXX"));   // the build's filesystem: hard links
        QVERIFY(Dir.isValid());
        for (const QString &N : { QStringLiteral("VidyaGod"), QStringLiteral("libvgipfs.so") })
            QVERIFY(::link(QFile::encodeName(App.absolutePath() + QLatin1Char('/') + N).constData(), QFile::encodeName(Dir.filePath(N)).constData()) == 0);
        QVERIFY(QFile::copy(QStringLiteral(MOCK_INPUTPLUMBER), Dir.filePath(QStringLiteral("vidyagod-inputplumber"))));
        PrivateBus Bus;
        QProcessEnvironment Env = QProcessEnvironment::systemEnvironment();
        Env.insert(QStringLiteral("VIDYAGOD_NO_CONTROLLER_SERVICE"), QStringLiteral("1"));
        Env.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"), Bus.Address);
        Env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        Env.insert(QStringLiteral("HOME"), Dir.filePath(QStringLiteral("home")));
        QProcess P;
        P.setProcessEnvironment(Env);
        P.start(Dir.filePath(QStringLiteral("VidyaGod")), { QStringLiteral("--data-dir"), Dir.filePath(QStringLiteral("data")), QStringLiteral("--json"),
                                                              QStringLiteral("controller"), QStringLiteral("service"), QStringLiteral("--want"), QStringLiteral("controller") });
        QVERIFY(P.waitForFinished(60000));
        const QByteArray Out = P.readAllStandardOutput();
        const QJsonObject J = QJsonDocument::fromJson(Out.mid(Out.indexOf('{'))).object();
        QVERIFY2(!J.isEmpty(), Out.constData());
        QCOMPARE(J.value(QStringLiteral("unavailable")).toString(), QStringLiteral("no InputPlumber program"));
        QCOMPARE(J.value(QStringLiteral("running")).toBool(), false);
    }

    // Which nodes are virtual, read from sysfs: a uinput device lives under /devices/virtual; a real one does not.
    // And an event node's joystick siblings (the same device's jsN), which a game's sandbox covers too.
    void virtual_nodes_and_joystick_siblings_come_from_sysfs()
    {
        QTemporaryDir Sys;
        const QString R = Sys.path();
        QVERIFY(QDir().mkpath(R + QStringLiteral("/devices/virtual/input/input90/event20")));
        QVERIFY(QDir().mkpath(R + QStringLiteral("/devices/pci0000:00/usb1/input/input7/event5")));
        QVERIFY(QDir().mkpath(R + QStringLiteral("/devices/pci0000:00/usb1/input/input7/js0")));
        QVERIFY(QDir().mkpath(R + QStringLiteral("/class/input")));
        QVERIFY(QFile::link(R + QStringLiteral("/devices/virtual/input/input90/event20"), R + QStringLiteral("/class/input/event20")));
        QVERIFY(QFile::link(R + QStringLiteral("/devices/pci0000:00/usb1/input/input7/event5"), R + QStringLiteral("/class/input/event5")));
        QVERIFY(IsVirtualInputNode(QStringLiteral("/dev/input/event20"), R));
        QVERIFY(!IsVirtualInputNode(QStringLiteral("/dev/input/event5"), R));
        QVERIFY(!IsVirtualInputNode(QStringLiteral("/dev/input/event99"), R));   // no such device
        QCOMPARE(JoystickSiblings(QStringLiteral("/dev/input/event5"), R), QStringList{ QStringLiteral("/dev/input/js0") });
        QVERIFY(JoystickSiblings(QStringLiteral("/dev/input/event20"), R).isEmpty());
    }

    // A game's sandbox covers every real controller node the service owns with /dev/null, after /dev is bound (a
    // later bind of /dev would uncover them); a node that does not exist is skipped (bwrap refuses a missing target).
    // Teeth: the hook not consulted, the cover missing, or placed before /dev.
    void a_games_sandbox_covers_the_real_controllers()
    {
        QTemporaryDir Tmp;
        const QString Real = Tmp.filePath(QStringLiteral("hidraw-real"));
        QFile F(Real);
        QVERIFY(F.open(QIODevice::WriteOnly));
        F.close();
        SandboxLayer::SetHiddenDevicesSource([&] { return std::vector<std::string>{ Real.toStdString(), "/dev/no-such-node" }; });
        ContainerParams CP;
        const SandboxLayer::Options O = SandboxLayer::FromParams(CP);
        QCOMPARE(O.HiddenDevices.size(), size_t(2));
        std::string Program = "game";
        QStringList Args;
        SandboxLayer::Wrap(O, Program, Args);
        const int Dev = Args.indexOf(QStringLiteral("--dev-bind"));
        const int Cover = Args.indexOf(Real);
        QVERIFY(Dev >= 0 && Cover > Dev);
        QCOMPARE(Args.at(Cover - 1), QStringLiteral("/dev/null"));
        QCOMPARE(Args.at(Cover - 2), QStringLiteral("--ro-bind"));
        QVERIFY(!Args.contains(QStringLiteral("/dev/no-such-node")));
        SandboxLayer::SetHiddenDevicesSource([] { return ControllerService::instance().hiddenNodes(); });
    }
};

QTEST_MAIN(ControllerServiceTest)
#include "test_controllerservice.moc"
