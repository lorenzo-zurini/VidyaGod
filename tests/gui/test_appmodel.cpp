// Tests for AppModel — the GUI state/signal hub: card-size persistence, repository add validation (#1 dup/empty),
// and removePackage for a local/portable package (#E28: drop the reference, never touch the user's files).
// Uses a temp data dir; the IPFS node is not started (the dehydrate path's unpin/drop-ref are no-ops).

#include <QtTest>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>

#include "appmodel.h"
#include "apppaths.h"

#include <fstream>

using json = nlohmann::ordered_json;

class AppModelTest : public QObject
{
    Q_OBJECT
private slots:
    void set_card_pixel_width_persists_and_signals()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}};
        AppModel m(&cfg, &appDir);

        QSignalSpy spy(&m, &AppModel::cardSizeChanged);
        m.setCardPixelWidth(250);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(cfg["Settings"].value("CardPixelWidth", 0), 250);

        m.setCardPixelWidth(250);          // unchanged → no signal
        QCOMPARE(spy.count(), 1);
    }

    void remove_local_package_drops_reference_keeps_user_files()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        QTemporaryDir ext; QVERIFY(ext.isValid());                 // a package OUTSIDE the library root
        { std::ofstream f((ext.path() + "/savegame.dat").toStdString()); f << "precious"; }

        json cfg = json{{"Settings", json::object()},
                        {"LIBRARY", json::array({ json{{"PACKAGEUID", "loc"}, {"PACKAGENAME", "Local Game"},
                                                       {"PATH", ext.path().toStdString()}} })}};
        AppModel m(&cfg, &appDir);

        QSignalSpy spy(&m, &AppModel::catalogChanged);
        m.removePackage("loc");

        QCOMPARE((int)cfg["LIBRARY"].size(), 0);                    // reference dropped
        QVERIFY(QFile::exists(ext.path() + "/savegame.dat"));       // user's own files untouched (TESTPLAN E28)
        QVERIFY(spy.count() >= 1);                                  // catalog rebuilt
    }

    void remove_unknown_package_is_noop()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}, {"LIBRARY", json::array()}};
        AppModel m(&cfg, &appDir);
        m.removePackage("does-not-exist");                         // must not crash / throw
        QVERIFY(true);
    }

    // The ctor applies the persisted CardPixelWidth (so a restart restores the user's zoom).
    void ctor_restores_persisted_card_width()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", {{"CardPixelWidth", 222}}}};
        AppModel m(&cfg, &appDir);
        QCOMPARE(m.cardPixelWidth(), 222);

        json def = json{{"Settings", json::object()}};            // absent → built-in default (185)
        AppModel m2(&def, &appDir);
        QCOMPARE(m2.cardPixelWidth(), 185);
    }

    // save() writes GlobalConfig.JSON into the app data dir and it reads back identically.
    void save_writes_globalconfig_to_disk()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", {{"CardPixelWidth", 199}}}, {"LIBRARY", json::array()}};
        AppModel m(&cfg, &appDir);

        QVERIFY(m.save());
        const QString path = appDir.filePath("GlobalConfig.JSON");
        QVERIFY(QFile::exists(path));

        std::ifstream f(path.toStdString());
        json onDisk; f >> onDisk;
        QCOMPARE(onDisk["Settings"].value("CardPixelWidth", 0), 199);
    }

    // removePackageSource drops the source entry, persists, rebuilds the catalog, and signals the change.
    void remove_package_source_drops_entry_and_signals()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        AppPaths::SetDataRoot(d.path().toStdString());   // keep LIBRARY deletion inside the temp dir
        QDir appDir(d.path());
        json cfg = json{{"Settings", {{"PackageSources", json::array({
                            json{{"CID", "QmA"}, {"NAME", "a"}}, json{{"CID", "QmB"}, {"NAME", "b"}} })}}},
                        {"LIBRARY", json::array()}};
        AppModel m(&cfg, &appDir);

        QSignalSpy srcs(&m, &AppModel::packageSourcesChanged);
        QSignalSpy cat(&m, &AppModel::catalogChanged);
        m.removePackageSource(0);

        QCOMPARE((int)cfg["Settings"]["PackageSources"].size(), 1);
        QCOMPARE(cfg["Settings"]["PackageSources"][0].value("CID", std::string()), std::string("QmB"));
        QVERIFY(srcs.count() >= 1);
        QVERIFY(cat.count() >= 1);

        m.removePackageSource(99);   // out-of-range → no erase, no crash
        QCOMPARE((int)cfg["Settings"]["PackageSources"].size(), 1);
    }

    // Networking is OFF by default and toggles persist Settings.IPFS.Enabled + emit networkingChanged once.
    void networking_off_by_default_and_toggles()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}};
        AppModel m(&cfg, &appDir);
        QVERIFY(!m.networkingEnabled());                  // off by default

        QSignalSpy spy(&m, &AppModel::networkingChanged);
        m.setNetworkingEnabled(true);
        QCOMPARE(spy.count(), 1);
        QVERIFY(m.networkingEnabled());
        QCOMPARE(cfg["Settings"]["IPFS"].value("Enabled", false), true);

        m.setNetworkingEnabled(true);                     // unchanged → no signal
        QCOMPARE(spy.count(), 1);
        m.setNetworkingEnabled(false);
        QCOMPARE(spy.count(), 2);
        QVERIFY(!m.networkingEnabled());
    }

    // rebuildCatalog emits catalogChanged (empty config → empty index, but the signal still fires).
    void rebuild_catalog_signals()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        json cfg = json{{"Settings", json::object()}, {"LIBRARY", json::array()}};
        AppModel m(&cfg, &appDir);
        QSignalSpy cat(&m, &AppModel::catalogChanged);
        m.rebuildCatalog();
        QCOMPARE(cat.count(), 1);
    }
};

QTEST_MAIN(AppModelTest)
#include "test_appmodel.moc"
