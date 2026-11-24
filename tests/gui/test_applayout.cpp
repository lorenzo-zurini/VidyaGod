// VidyaGod's own controller layout at the app's start and in Settings → Controller: the window loads it into the
// controller service as it comes up (the session had none loaded: InputPlumber's default, without the volume keys), and
// the page's chooser makes another one VidyaGod's and loads it. Teeth: the start not loading it; the chooser writing it
// without loading it.

#include <QtTest>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "apppaths.h"
#include "controllerlayout.h"
#include "controllerservice.h"
#include "mainwindow.h"
#include "settingstab.h"

using json = nlohmann::ordered_json;

class AppLayoutTest : public QObject
{
    Q_OBJECT
private slots:
    void the_window_loads_vidyagods_layout_and_settings_chooses_another()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        AppPaths::SetDataRoot(d.path().toStdString());
        const std::string Layouts = (AppPaths::DataRoot() / "ControllerLayouts").string();
        QVERIFY(ControllerLayout::SetForApp(Layouts, "Desktop"));
        ControllerService &S = ControllerService::instance();
        auto Loaded = [&] { return json::parse(S.layout().toStdString(), nullptr, false).value("name", std::string()); };
        json cfg = json{{"Settings", json::object()}};
        MainWindow mw(&cfg, &appDir);
        QCOMPARE(S.appLayoutDir().toStdString(), Layouts);
        QCOMPARE(Loaded(), std::string("Desktop"));
        SettingsTab * settings = mw.findChild<SettingsTab *>();
        QVERIFY(settings && settings->openPage("Controller"));
        QApplication::processEvents();
        auto * Combo = mw.findChild<QComboBox *>("appLayout");
        QVERIFY(Combo);
        QCOMPARE(Combo->currentText(), QStringLiteral("Desktop"));
        Combo->setCurrentIndex(Combo->findText(QStringLiteral("Keyboard and mouse (WASD)")));
        QCOMPARE(ControllerLayout::ForApp(Layouts), std::string("Keyboard and mouse (WASD)"));
        QCOMPARE(Loaded(), std::string("Keyboard and mouse (WASD)"));
        S.loadLayout({});
    }
};

QTEST_MAIN(AppLayoutTest)
#include "test_applayout.moc"
