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
};

QTEST_MAIN(TrayTest)
#include "test_tray.moc"
