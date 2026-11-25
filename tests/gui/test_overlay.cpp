// The in-game overlay (GameOverlay), driven as InputPlumber drives it: tests/gui/mock_inputplumber on a private bus emits
// the intercepted presses (MockControl.Press), the controller service hands them over, the overlay acts. Never the
// real InputPlumber; the data root is a temporary folder (games' layout choices are written there).
#include <QtTest/QtTest>
#include <QApplication>
#include <QComboBox>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusVariant>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>

#include "apppaths.h"
#include "controllerlayout.h"
#include "controllerservice.h"
#include "gameoverlay.h"
#include "padnav.h"

namespace
{
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
}

class OverlayTest : public QObject
{
    Q_OBJECT
    ControllerService &S = ControllerService::instance();
    GameOverlay &O = GameOverlay::instance();
    QTemporaryDir Data;

    void Press(const QString &Address, const QString &Event)
    {
        QDBusConnection C = QDBusConnection::connectToBus(Address, QStringLiteral("test-mock-control"));
        QDBusInterface Ctl(QStringLiteral("org.shadowblip.InputPlumber"), QStringLiteral("/org/vidyagod/MockControl"),
                           QStringLiteral("org.vidyagod.MockControl"), C);
        Ctl.call(QStringLiteral("Press"), Event);
    }

private slots:
    void initTestCase()
    {
        AppPaths::SetDataRoot(Data.path().toStdString());
        PadRouter::instance().install();
        O.attach();
    }

    void cleanup()
    {
        O.clearGame();
        S.reset();
        S.setProgram({});
        QDBusConnection::disconnectFromBus(QStringLiteral("test-mock-control"));
    }

    // gamescope draws a window marked STEAM_OVERLAY above the game at its opacity, and gives the input to the one
    // with STEAM_INPUT_FOCUS: open, both; closed, still the overlay but neither shown nor taking the input.
    void the_overlay_marks_itself_as_gamescopes_overlay()
    {
        const auto Open = OverlayProperties(true), Closed = OverlayProperties(false);
        auto Get = [](const auto &V, const char *N) {
            for (const auto &[K, Val] : V) if (std::string(K) == N) return Val;
            return 12345u;
        };
        QCOMPARE(Get(Open, "STEAM_OVERLAY"), 1u);
        QCOMPARE(Get(Open, "STEAM_INPUT_FOCUS"), 1u);
        QCOMPARE(Get(Open, "_NET_WM_WINDOW_OPACITY"), 0xFFFFFFFFu);
        QCOMPARE(Get(Closed, "STEAM_OVERLAY"), 1u);
        QCOMPARE(Get(Closed, "STEAM_INPUT_FOCUS"), 0u);
        QCOMPARE(Get(Closed, "_NET_WM_WINDOW_OPACITY"), 0u);
    }

    // InputPlumber's intercepted events are the controller's buttons.
    void intercepted_events_are_the_controllers_buttons()
    {
        Pad::Button B;
        const std::pair<const char *, Pad::Button> Want[] = { { "ui_accept", Pad::Button::A }, { "ui_back", Pad::Button::B },
            { "ui_action", Pad::Button::X }, { "ui_context", Pad::Button::Y }, { "ui_up", Pad::Button::Up }, { "ui_right", Pad::Button::Right },
            { "ui_l1", Pad::Button::LB }, { "ui_r2", Pad::Button::RT }, { "ui_option", Pad::Button::Start }, { "ui_select", Pad::Button::Back } };
        for (const auto &[E, Btn] : Want)
        {
            QVERIFY2(PadButtonFromUiEvent(QString::fromLatin1(E), B), E);
            QVERIFY2(B == Btn, E);
        }
        QVERIFY(!PadButtonFromUiEvent(QStringLiteral("ui_touch"), B));
        QVERIFY(!PadButtonFromUiEvent(QStringLiteral("ui_volume_up"), B));
    }

    // Over a game: the game's start has InputPlumber intercept the Guide button only; Guide opens the overlay (and
    // InputPlumber now intercepts everything); the controller drives it — down to the layout, right switches it live
    // (the game's choice, on the controller now) —; B closes it and only Guide is intercepted again; Quit asks twice
    // and ends the game; the game's end lets the intercept go. Teeth: no intercept at the start, Guide not opening it,
    // presses not reaching it, the layout not switched live, the intercept not restored, Quit at the first press, the
    // intercept kept after the game.
    void guide_opens_it_over_a_game_and_the_controller_drives_it()
    {
        PrivateBus Bus;
        qputenv("MOCK_IP_SOURCES", "/dev/hidraw-test9");
        qputenv("MOCK_IP_HOTPLUG_MS", "0");
        S.setBus(Bus.Address);
        S.setProgram(QStringLiteral(MOCK_INPUTPLUMBER));
        S.want(ControllerService::Need::ControllerMode, true);
        QTRY_VERIFY_WITH_TIMEOUT(S.running(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!S.devices().isEmpty(), 5000);   // its controllers come after its Manager, which comes after its name

        int Quits = 0;
        O.setGame(QStringLiteral("Some Game"), QStringLiteral("g1"), [&] { ++Quits; });
        S.want(ControllerService::Need::Game, true);
        QTRY_COMPARE_WITH_TIMEOUT(S.intercept(), 1u, 3000);
        QVERIFY(!O.isOpen());

        Press(Bus.Address, QStringLiteral("ui_guide"));
        QTRY_VERIFY_WITH_TIMEOUT(O.isOpen(), 3000);
        QCOMPARE(S.intercept(), 2u);
        QCOMPARE(O.title(), QStringLiteral("Some Game"));
        QCOMPARE(O.layoutName(), QStringLiteral("Gamepad"));
        QTRY_VERIFY_WITH_TIMEOUT(QApplication::focusWidget() && QApplication::focusWidget()->objectName() == QLatin1String("resume"), 3000);

        //VidyaGod's own layout (its menus') is Desktop: a game put back to the controller as it is gets Gamepad, not it.
        const std::string Dir = (AppPaths::DataRoot() / "ControllerLayouts").string();
        QVERIFY(ControllerLayout::SetForApp(Dir, "Desktop"));
        QVERIFY(S.loadAppLayout(QString::fromStdString(Dir)));
        const auto Restore = qScopeGuard([&] { ControllerLayout::SetForApp(Dir, ""); S.loadLayout({}); });
        Press(Bus.Address, QStringLiteral("ui_down"));
        QTRY_VERIFY_WITH_TIMEOUT(QApplication::focusWidget() && QApplication::focusWidget()->objectName() == QLatin1String("overlayLayouts"), 3000);
        Press(Bus.Address, QStringLiteral("ui_right"));
        QTRY_COMPARE_WITH_TIMEOUT(O.layoutName(), QStringLiteral("Keyboard and mouse (WASD)"), 3000);
        QCOMPARE(ControllerLayout::ForGame(Dir, "g1"), std::string("Keyboard and mouse (WASD)"));
        QVERIFY(S.gameLayout().contains(QLatin1String("Keyboard and mouse (WASD)")));
        QTRY_COMPARE_WITH_TIMEOUT(S.devices().value(0).Profile, QStringLiteral("Keyboard and mouse (WASD)"), 3000);
        Press(Bus.Address, QStringLiteral("ui_left"));
        QTRY_COMPARE_WITH_TIMEOUT(O.layoutName(), QStringLiteral("Gamepad"), 3000);
        QCOMPARE(ControllerLayout::ForGame(Dir, "g1"), std::string());
        QTRY_COMPARE_WITH_TIMEOUT(S.devices().value(0).Profile, QStringLiteral("Gamepad"), 3000);   // not Desktop
        Press(Bus.Address, QStringLiteral("ui_right"));
        QTRY_COMPARE_WITH_TIMEOUT(O.layoutName(), QStringLiteral("Keyboard and mouse (WASD)"), 3000);

        Press(Bus.Address, QStringLiteral("ui_back"));
        QTRY_VERIFY_WITH_TIMEOUT(!O.isOpen(), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(S.intercept(), 1u, 3000);

        Press(Bus.Address, QStringLiteral("ui_guide"));
        QTRY_VERIFY_WITH_TIMEOUT(O.isOpen(), 3000);
        auto *Quit = O.findChild<QPushButton *>(QStringLiteral("quitGame"));
        Quit->setFocus();
        Press(Bus.Address, QStringLiteral("ui_accept"));
        QTRY_VERIFY_WITH_TIMEOUT(O.quitArmed(), 3000);
        QCOMPARE(Quits, 0);
        //Moving off it disarms it: a press there much later must not quit. (Teeth: armed until the overlay closes.)
        Press(Bus.Address, QStringLiteral("ui_up"));
        QTRY_VERIFY_WITH_TIMEOUT(!O.quitArmed(), 3000);
        Quit->setFocus();
        Press(Bus.Address, QStringLiteral("ui_accept"));
        QTRY_VERIFY_WITH_TIMEOUT(O.quitArmed(), 3000);
        QCOMPARE(Quits, 0);
        Press(Bus.Address, QStringLiteral("ui_accept"));
        QTRY_COMPARE_WITH_TIMEOUT(Quits, 1, 3000);
        QVERIFY(!O.isOpen());

        S.want(ControllerService::Need::Game, false);
        QTRY_COMPARE_WITH_TIMEOUT(S.intercept(), 0u, 3000);
    }

    // Typing into the game from the overlay: back to the game, the text and Enter on its keyboard, the field emptied;
    // without a game, nothing is typed. Teeth: Enter not added, the overlay left open over the game.
    void the_overlay_types_into_the_game()
    {
        PrivateBus Bus;
        qputenv("MOCK_IP_SOURCES", "/dev/hidraw-test8");
        qputenv("MOCK_IP_HOTPLUG_MS", "0");
        S.setBus(Bus.Address);
        S.setProgram(QStringLiteral(MOCK_INPUTPLUMBER));
        S.want(ControllerService::Need::ControllerMode, true);
        QTRY_VERIFY_WITH_TIMEOUT(S.running(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!S.devices().isEmpty(), 5000);   // its controllers come after its Manager, which comes after its name
        QString Err;
        QVERIFY(!O.typeIntoGame(QStringLiteral("ok"), &Err));   // no game
        O.setGame(QStringLiteral("Chatty"), QStringLiteral("g3"), [] {});
        O.openOverlay();
        auto *Field = O.findChild<QLineEdit *>(QStringLiteral("overlayTypeText"));
        Field->setText(QStringLiteral("gg"));
        O.findChild<QPushButton *>(QStringLiteral("overlayType"))->click();
        QVERIFY(!O.isOpen());
        QVERIFY(Field->text().isEmpty());
        QDBusConnection C = QDBusConnection::connectToBus(Bus.Address, QStringLiteral("test-mock-control"));
        QDBusInterface P(QStringLiteral("org.shadowblip.InputPlumber"), QStringLiteral("/org/shadowblip/InputPlumber/CompositeDevice0"),
                         QStringLiteral("org.freedesktop.DBus.Properties"), C);
        const QDBusReply<QDBusVariant> R = P.call(QStringLiteral("Get"), QStringLiteral("org.shadowblip.Input.CompositeDevice"), QStringLiteral("Sent"));
        QCOMPARE(R.value().variant().toStringList(), (QStringList{ "Keyboard:KeyG+", "Keyboard:KeyG-", "Keyboard:KeyG+", "Keyboard:KeyG-",
                                                                   "Keyboard:KeyEnter+", "Keyboard:KeyEnter-" }));
    }

    // Without a game (no intercept, so no Guide reaches it from InputPlumber; opened by hand, as `controller overlay
    // --open` does) it says so and offers nothing for one; the game's end closes an open overlay. Teeth: Quit or a
    // layout offered with no game; an overlay left open after its game.
    void without_a_game_it_offers_nothing_and_the_games_end_closes_it()
    {
        O.onUiEvent(QStringLiteral("ui_guide"), 1.0);
        QVERIFY(O.isOpen());   // opened by hand it shows, but says no game runs and offers nothing for one
        QCOMPARE(O.title(), QStringLiteral("No game is running"));
        QVERIFY(!O.findChild<QPushButton *>(QStringLiteral("quitGame"))->isEnabled());
        O.closeOverlay();
        O.setGame(QStringLiteral("G"), QStringLiteral("g2"), [] {});
        O.openOverlay();
        QVERIFY(O.isOpen());
        O.clearGame();
        QVERIFY(!O.isOpen());
    }
};

QTEST_MAIN(OverlayTest)
#include "test_overlay.moc"
