// The pre-launch window's graft list, driven through the real widgets: the list is the instance's ordered GRAFTS
// (the order they apply in), Move up / Move down reorder it, and a move that would leave a graft unapplied (a graft
// on a graft moved above the graft it needs) is refused on screen and changes nothing saved.

#include <QtTest>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>

#include "apppaths.h"
#include "manifestmodel.h"
#include "packagecatalog.h"
#include "prelaunchwindow.h"

using json = nlohmann::ordered_json;

namespace {
Node parse(const json & j)
{
    Node n;
    ManifestModel::ParseNode(j, "f.json", "/tmp/vg_prelaunch_bundle", n);
    return n;
}
}

class PreLaunchTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        SuiteDataRoot = new QTemporaryDir();
        QVERIFY(SuiteDataRoot->isValid());
        AppPaths::SetDataRoot(SuiteDataRoot->path().toStdString());   // never the developer's GlobalConfig
    }
    void cleanupTestCase() { delete SuiteDataRoot; }

    void theGraftListReordersAndRefusesAMoveThatWouldDropAGraft()
    {
        NodeIndex idx;
        idx.Nodes["base"] = parse(json{ {"CID", "base"}, {"LABEL", "base"}, {"LAYERS", json::array({ json{{"DIR", "base"}} })} });
        idx.Nodes["game"] = parse(json{ {"CID", "game"}, {"LABEL", "game"}, {"VARIANT", "v1"},
            {"LAYERS", json::array({ json{{"NODE", "base"}}, json{{"EXEC", json::array({ json{{"LABEL", "Play"},
                {"HOST", ManifestModel::MachinePlatform()}, {"EXE", "game.exe"}, {"TILE", {{"UID", "7"}, {"TITLE", "Game"}}}} })}} })} });
        const auto graft = [](const char *Id, const char *On) {   // RECOMMENDED under tile 7: a fresh instance ticks it
            return parse(json{ {"CID", Id}, {"LABEL", Id}, {"RECOMMENDED", json::array({"7"})},
                {"LAYERS", json::array({ json{{"ANY", json::array({On})}}, json{{"DIR", Id}} })} });
        };
        idx.Nodes["a"] = graft("a", "game");
        idx.Nodes["b"] = graft("b", "game");
        idx.Nodes["ona"] = graft("ona", "a");                    // a graft on the graft a
        ManifestModel::DeriveFacts(idx);

        json Cfg = json{{"Settings", json::object()}};
        PreLaunchWindow W(&Cfg, &idx, {"game"});
        W.show();
        QCoreApplication::processEvents();
        auto *List = W.findChild<QTreeWidget *>("graftList");
        auto *Up = W.findChild<QPushButton *>("graftUp");
        auto *Down = W.findChild<QPushButton *>("graftDown");
        auto *Note = W.findChild<QLabel *>("graftNote");
        QVERIFY(List && Up && Down && Note);
        const auto rows = [&] {
            std::vector<std::string> R;
            for (int i = 0; i < List->topLevelItemCount(); ++i) R.push_back(List->topLevelItem(i)->text(0).toStdString());
            return R;
        };
        const auto select = [&](const char *Label) {
            for (int i = 0; i < List->topLevelItemCount(); ++i)
                if (List->topLevelItem(i)->text(0) == Label) List->setCurrentItem(List->topLevelItem(i));
        };
        const auto saved = [&] {
            const json US = PackageCatalog::GetPackageUserSettings(Cfg, idx.Find("game")->PackageUid);
            std::vector<std::string> G;
            if (US.contains("GRAFTS")) for (const auto &X : US["GRAFTS"]) G.push_back(X.get<std::string>());
            return G;
        };
        QCOMPARE(rows(), (std::vector<std::string>{"a", "b", "ona"}));   // all three recommended, a before what needs it

        select("b");
        QVERIFY(Down->isEnabled());
        QTest::mouseClick(Down, Qt::LeftButton);                         // b after ona: nothing depends on b
        QCOMPARE(rows(), (std::vector<std::string>{"a", "ona", "b"}));
        QCOMPARE(saved(), (std::vector<std::string>{"a", "ona", "b"}));   // the order is instance data
        QVERIFY(!Note->isVisible());

        select("ona");
        QVERIFY(Up->isEnabled());
        QTest::mouseClick(Up, Qt::LeftButton);                           // ona above a: it would no longer apply
        QCOMPARE(rows(), (std::vector<std::string>{"a", "ona", "b"}));
        QCOMPARE(saved(), (std::vector<std::string>{"a", "ona", "b"}));
        QVERIFY(Note->isVisible());
        QVERIFY2(Note->text().contains("ona"), qPrintable(Note->text()));

        select("a");
        QVERIFY(!Up->isEnabled());                                        // nothing above the first
    }

private:
    QTemporaryDir *SuiteDataRoot = nullptr;
};

QTEST_MAIN(PreLaunchTest)
#include "test_prelaunch.moc"
