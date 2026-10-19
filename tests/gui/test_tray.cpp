// Tests for the tray / daemon-hybrid seam: TrayController reads Settings.Tray (with the on-by-default fallbacks),
// persists the remembered hidden state, and the GeneralPage toggles write through to the config. Runs headless
// under offscreen QPA — no system tray exists there, so TrayController::available() is false and the behavior
// degrades to normal windows; we test the settings plumbing (the getters read config regardless of a live tray).

#include <QtTest>
#include <QTemporaryDir>
#include <QDir>
#include <QCheckBox>

#include "appmodel.h"
#include "traycontroller.h"
#include "generalpage.h"
#include "apppaths.h"
#include "autostart.h"

#include <QStandardPaths>
#include <QSettings>
#include <QScopeGuard>
#include <QCoreApplication>

using json = nlohmann::ordered_json;

class TrayTest : public QObject
{
    Q_OBJECT
private slots:
    // On-by-default: missing Settings.Tray → close/minimize to tray ON, start-in-tray OFF.
    void settings_default_on_when_absent()
    {
        QTemporaryDir d; QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}};
        AppModel m(&cfg, &appDir);
        TrayController t(m, nullptr);
        QVERIFY(t.closeToTray());
        QVERIFY(t.minimizeToTray());
        QVERIFY(!t.startInTray());
    }

    // Explicit Settings.Tray values override the defaults.
    void settings_read_from_config()
    {
        QTemporaryDir d; QDir appDir(d.path());
        json cfg = json{{"Settings", {{"Tray", {{"CloseToTray", false}, {"MinimizeToTray", false},
                                                 {"StartInTray", true}}}}}};
        AppModel m(&cfg, &appDir);
        TrayController t(m, nullptr);
        QVERIFY(!t.closeToTray());
        QVERIFY(!t.minimizeToTray());
        QVERIFY(t.startInTray());
    }

    // rememberHidden persists Settings.Tray.LaunchHidden (the "reopen the way you left it" state).
    void remember_hidden_persists()
    {
        QTemporaryDir d; QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}};
        AppModel m(&cfg, &appDir);
        TrayController t(m, nullptr);
        t.rememberHidden(true);
        QCOMPARE(cfg["Settings"]["Tray"].value("LaunchHidden", false), true);
        t.rememberHidden(false);
        QCOMPARE(cfg["Settings"]["Tray"].value("LaunchHidden", true), false);
    }

    // shouldStartHidden is false without a real tray (degrades to a normal visible window), regardless of settings.
    void start_hidden_needs_a_tray()
    {
#ifdef _WIN32
        QSKIP("premise (offscreen QPA => no system tray) is Linux-specific; Windows reports a tray regardless of QPA");
#endif
        QTemporaryDir d; QDir appDir(d.path());
        json cfg = json{{"Settings", {{"Tray", {{"StartInTray", true}, {"LaunchHidden", true}}}}}};
        AppModel m(&cfg, &appDir);
        TrayController t(m, nullptr);
        QVERIFY(!t.available());          // offscreen QPA has no system tray
        QVERIFY(!t.shouldStartHidden());  // so it never starts hidden (can't restore without a tray)
    }

    // The GeneralPage toggles write through to Settings.Tray.
    void general_page_toggle_writes_config()
    {
        QTemporaryDir d; QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}};
        AppModel m(&cfg, &appDir);
        GeneralPage page(m);

        QCheckBox * closeCb = nullptr;
        for (QCheckBox * cb : page.findChildren<QCheckBox *>())
            if (cb->text().contains("Close to tray")) closeCb = cb;
        QVERIFY(closeCb != nullptr);

        closeCb->setChecked(false);   // emits toggled → writes config
        QCOMPARE(cfg["Settings"]["Tray"].value("CloseToTray", true), false);
        closeCb->setChecked(true);
        QCOMPARE(cfg["Settings"]["Tray"].value("CloseToTray", false), true);
    }

    // Run-mode gating: daemon allowed except In-package; autostart only in Normal.
    void mode_gates_daemon_and_autostart()
    {
        AppPaths::SetMode(AppPaths::Mode::Normal);
        QVERIFY(AppPaths::DaemonSupported());  QVERIFY(AppPaths::AutostartSupported());
        AppPaths::SetMode(AppPaths::Mode::Portable);
        QVERIFY(AppPaths::DaemonSupported());  QVERIFY(!AppPaths::AutostartSupported());
        AppPaths::SetMode(AppPaths::Mode::CliPaths);
        QVERIFY(AppPaths::DaemonSupported());  QVERIFY(!AppPaths::AutostartSupported());
        AppPaths::SetMode(AppPaths::Mode::InPackage);
        QVERIFY(!AppPaths::DaemonSupported()); QVERIFY(!AppPaths::AutostartSupported());
        AppPaths::SetMode(AppPaths::Mode::Normal);   // restore for other tests
    }

    // The login entry writes + removes, and the launcher carries the quiet --tray flag. Windows: the real HKCU Run key
    // (no redirect exists for it), so whatever entry the machine had is put back afterwards.
    void autostart_enable_disable_roundtrip()
    {
#ifdef _WIN32
        const QString runKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
        const QVariant before = QSettings(runKey, QSettings::NativeFormat).value("VidyaGod");
        //Put the machine's own entry back however this test ends — a failed QVERIFY returns early, and leaving the
        //developer's login entry deleted, or pointing at test_tray.exe, is not an acceptable side effect.
        const auto restore = qScopeGuard([&]{
            QSettings s(runKey, QSettings::NativeFormat);
            if (before.isValid()) s.setValue("VidyaGod", before); else s.remove("VidyaGod");
        });
        QString err;
        QVERIFY2(AutoStart::SetEnabled(true, &err), qPrintable(err));
        QVERIFY(AutoStart::IsEnabled());
        const QString cmd = QSettings(runKey, QSettings::NativeFormat).value("VidyaGod").toString();
        QVERIFY2(cmd.startsWith('"') && cmd.endsWith("\" --tray"), qPrintable(cmd));   // quoted exe, quiet to the tray
        QVERIFY(cmd.contains(QDir::toNativeSeparators(QCoreApplication::applicationFilePath())));
        QVERIFY(AutoStart::SetEnabled(false, &err));
        QVERIFY(!AutoStart::IsEnabled());
        return;
#endif
        QTemporaryDir cfgHome; QVERIFY(cfgHome.isValid());
        qputenv("XDG_CONFIG_HOME", cfgHome.path().toUtf8());

        QVERIFY(!AutoStart::IsEnabled());
        QString err;
        QVERIFY2(AutoStart::SetEnabled(true, &err), qPrintable(err));
        QVERIFY(AutoStart::IsEnabled());

        const QString path = cfgHome.path() + "/autostart/org.vidyagod.VidyaGod.desktop";
        QVERIFY(QFile::exists(path));
        QFile f(path); QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString body = f.readAll(); f.close();
        QVERIFY(body.contains("[Desktop Entry]"));
        QVERIFY(body.contains("Exec="));
        QVERIFY(body.contains("--tray"));               // login start goes quiet to the tray

        QVERIFY(AutoStart::SetEnabled(false, &err));
        QVERIFY(!AutoStart::IsEnabled());
        QVERIFY(!QFile::exists(path));

        qunsetenv("XDG_CONFIG_HOME");
    }

    // The GeneralPage greys out "Launch at login" outside Normal mode (e.g. portable).
    void general_page_disables_login_in_portable()
    {
        AppPaths::SetMode(AppPaths::Mode::Portable);
        QTemporaryDir d; QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}};
        AppModel m(&cfg, &appDir);
        GeneralPage page(m);

        QCheckBox * loginCb = nullptr;
        for (QCheckBox * cb : page.findChildren<QCheckBox *>())
            if (cb->text().contains("Launch at login")) loginCb = cb;
        QVERIFY(loginCb != nullptr);
        QVERIFY(!loginCb->isEnabled());   // greyed out in portable mode
        AppPaths::SetMode(AppPaths::Mode::Normal);
    }
};

QTEST_MAIN(TrayTest)
#include "test_tray.moc"
