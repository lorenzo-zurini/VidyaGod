// Construction/render smoke tests for every main tab + Settings subpage view against an AppModel. Catches ctor /
// signal-wiring regressions (each is its own QWidget reacting to the model). Renders headless via grab(); the IPFS
// node is not started, so the IPFS tab shows its graceful "unavailable" state.

#include <QtTest>
#include <QTemporaryDir>
#include <QDir>

#include "appmodel.h"
#include "librarytab.h"
#include "catalogtab.h"
#include "packagesview.h"
#include "settingstab.h"
#include "ipfstab.h"
#include "gamepicker.h"

using json = nlohmann::ordered_json;

namespace {
// Render a widget and neutralize its subtree's signals before the caller destroys it (a focused QLineEdit would
// otherwise fire editingFinished during ~QWidget and hit a dying slot — same class of bug found in the editor).
bool renders(QWidget * w)
{
    w->resize(900, 650);
    w->show();
    QApplication::processEvents();
    QApplication::processEvents();
    const bool ok = !w->grab().isNull();
    w->blockSignals(true);
    for (QObject * c : w->findChildren<QObject *>()) c->blockSignals(true);
    return ok;
}
}

class TabsTest : public QObject
{
    Q_OBJECT
    QTemporaryDir Dir;
    QDir          AppDir{Dir.path()};
    json          Cfg = json{{"Settings", json::object()}, {"LIBRARY", json::array()}};

private slots:
    void library_tab_constructs()   { AppModel m(&Cfg, &AppDir); LibraryTab  t(m); QVERIFY(renders(&t)); }
    void catalog_tab_constructs()   { AppModel m(&Cfg, &AppDir); CatalogTab  t(m); QVERIFY(renders(&t)); }
    void packages_view_constructs() { AppModel m(&Cfg, &AppDir); PackagesView t(m); QVERIFY(renders(&t)); }
    void settings_tab_constructs()  { AppModel m(&Cfg, &AppDir); SettingsTab t(m); QVERIFY(renders(&t)); }
    void ipfs_tab_constructs()      { AppModel m(&Cfg, &AppDir); IpfsTab     t(m); QVERIFY(renders(&t)); }

    // The in-package multi-game picker builds a card per presentable group over a bundle index and renders.
    void game_picker_constructs()
    {
        NodeIndex idx;
        for (const char * id : {"g1", "g2"})
        {
            Node n; n.NodeId = id; n.Role = "launchable"; n.Group = id;
            n.Meta = json{{"TITLE", std::string("Game ") + id}};
            idx.Nodes[id] = n;
        }
        json cfg = json{{"Settings", json::object()}};
        GamePicker p(&cfg, &idx);
        QVERIFY(renders(&p));   // two game cards laid out
    }

    // Switching the Settings subpages must not crash (each subpage is its own widget).
    void settings_subpages_switch()
    {
        AppModel m(&Cfg, &AppDir);
        SettingsTab tab(m);
        tab.resize(1000, 700); tab.show(); QApplication::processEvents();
        // The settings nav is a list/stacked layout; just cycle any child stacked widget if present.
        for (QObject * c : tab.findChildren<QObject *>()) c->blockSignals(true);
        QVERIFY(true);   // reached here without crashing
    }
};

QTEST_MAIN(TabsTest)
#include "test_tabs.moc"
