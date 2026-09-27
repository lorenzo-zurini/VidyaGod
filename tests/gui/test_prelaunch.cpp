// The pre-launch window's graft list, driven through the real widgets: the list is the instance's ordered GRAFTS
// (the order they apply in), Move up / Move down reorder it, and a move that would leave a graft unapplied (a graft
// on a graft moved above the graft it needs) is refused on screen and changes nothing saved.

#include <QtTest>
#include <QCheckBox>
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
            if (US.contains("GRAFTS") && US["GRAFTS"].contains("7")) for (const auto &X : US["GRAFTS"]["7"]) G.push_back(X.get<std::string>());
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

    // A move is judged as the launch would judge it: with the values the pickers hold now, saved or not. Here ona
    // needs a only while "strict" is on; the user unticks it (not saved yet) and ona may move above a. Teeth: judge
    // with the saved instance only and the move is refused.
    void aGraftMoveIsJudgedWithThePickersCurrentValues()
    {
        NodeIndex idx;
        idx.Nodes["base"] = parse(json{ {"CID", "base"}, {"LABEL", "base"}, {"LAYERS", json::array({ json{{"DIR", "base"}} })} });
        idx.Nodes["game"] = parse(json{ {"CID", "game"}, {"LABEL", "game"}, {"VARIANT", "v1"},
            {"LAYERS", json::array({ json{{"NODE", "base"}},
                json{{"VARS", {{"strict", {{"DEFAULT", "1"}, {"UI", {{"CONTROL", "bool"}}}}}}}},
                json{{"EXEC", json::array({ json{{"LABEL", "Play"}, {"HOST", ManifestModel::MachinePlatform()}, {"EXE", "game.exe"},
                    {"TILE", {{"UID", "9"}, {"TITLE", "Strict"}}}} })}} })} });
        idx.Nodes["a"] = parse(json{ {"CID", "a"}, {"LABEL", "a"}, {"RECOMMENDED", json::array({"9"})},
            {"LAYERS", json::array({ json{{"ANY", json::array({"game"})}}, json{{"DIR", "a"}} })} });
        idx.Nodes["ona"] = parse(json{ {"CID", "ona"}, {"LABEL", "ona"}, {"RECOMMENDED", json::array({"9"})},
            {"LAYERS", json::array({ json{{"ANY", json::array({"game"})}},
                                     json{{"ANY", json::array({"a"})}, {"WHEN", "%strict%==1"}}, json{{"DIR", "ona"}} })} });
        ManifestModel::DeriveFacts(idx);

        json Cfg = json{{"Settings", json::object()}};
        PreLaunchWindow W(&Cfg, &idx, {"game"});
        W.show();
        QCoreApplication::processEvents();
        auto *List = W.findChild<QTreeWidget *>("graftList");
        auto *Up = W.findChild<QPushButton *>("graftUp");
        QVERIFY(List && Up);
        const auto rows = [&] {
            std::vector<std::string> R;
            for (int i = 0; i < List->topLevelItemCount(); ++i) R.push_back(List->topLevelItem(i)->text(0).toStdString());
            return R;
        };
        QCOMPARE(rows(), (std::vector<std::string>{"a", "ona"}));
        QCheckBox *Strict = nullptr;
        for (QCheckBox *C : W.findChildren<QCheckBox *>()) if (C->property("CVKey").toString() == "strict") Strict = C;
        QVERIFY2(Strict && Strict->isChecked(), "the strict picker, on by default");
        Strict->setChecked(false);                                        // not saved: the window is still open
        List->setCurrentItem(List->topLevelItem(1));
        QVERIFY(Up->isEnabled());
        QTest::mouseClick(Up, Qt::LeftButton);
        QCOMPARE(rows(), (std::vector<std::string>{"ona", "a"}));
    }

    // Instance settings are the family's (RoC and TFT share one), but the graft list is the TILE's: saving under one
    // tile must neither replace the sibling's recommended pre-ticks nor drop the grafts only the sibling offers.
    // Teeth: key GRAFTS by family again (one list) and tile 8 opens with tile 7's list instead of its own pre-tick.
    void eachTileKeepsItsOwnGraftList()
    {
        NodeIndex idx;
        const std::string Host = ManifestModel::MachinePlatform();
        idx.Nodes["fam"] = parse(json{ {"CID", "fam"}, {"LABEL", "fam"}, {"VARIANT", "v1"},
            {"LAYERS", json::array({ json{{"DIR", "fam"}}, json{{"EXEC", json::array({
                json{{"LABEL", "RoC"}, {"HOST", Host}, {"EXE", "roc.exe"}, {"TILE", {{"UID", "70"}, {"TITLE", "RoC"}}}},
                json{{"LABEL", "TFT"}, {"HOST", Host}, {"EXE", "tft.exe"}, {"TILE", {{"UID", "80"}, {"TITLE", "TFT"}}}} })}} })} });
        const auto graft = [](const char *Id, const char *Tile) {
            return parse(json{ {"CID", Id}, {"LABEL", Id}, {"RECOMMENDED", json::array({Tile})},
                {"LAYERS", json::array({ json{{"ANY", json::array({"fam"})}}, json{{"DIR", Id}} })} });
        };
        idx.Nodes["a"] = graft("a", "70");
        idx.Nodes["b"] = graft("b", "80");
        ManifestModel::DeriveFacts(idx);
        json Cfg = json{{"Settings", json::object()}};
        const auto ticked = [](PreLaunchWindow &W) {
            std::vector<std::string> R;
            auto *List = W.findChild<QTreeWidget *>("graftList");
            for (int i = 0; List && i < List->topLevelItemCount(); ++i)
                if (List->topLevelItem(i)->checkState(0) == Qt::Checked) R.push_back(List->topLevelItem(i)->text(0).toStdString());
            return R;
        };
        {
            PreLaunchWindow W7(&Cfg, &idx, {"fam"}, "70");
            W7.show(); QCoreApplication::processEvents();
            QCOMPARE(ticked(W7), (std::vector<std::string>{"a"}));               // its own recommendation
            auto *List = W7.findChild<QTreeWidget *>("graftList");
            for (int i = 0; i < List->topLevelItemCount(); ++i)                  // the user unticks a: a saved list
                if (List->topLevelItem(i)->text(0) == "a") { List->topLevelItem(i)->setCheckState(0, Qt::Unchecked); break; }   // (the list rebuilds)
            QCoreApplication::processEvents();
            QCOMPARE(ticked(W7), (std::vector<std::string>{}));
        }
        {
            PreLaunchWindow W8(&Cfg, &idx, {"fam"}, "80");
            W8.show(); QCoreApplication::processEvents();
            QCOMPARE(ticked(W8), (std::vector<std::string>{"b"}));               // not tile 7's (empty) list
        }
        {
            PreLaunchWindow W7(&Cfg, &idx, {"fam"}, "70");
            W7.show(); QCoreApplication::processEvents();
            QCOMPARE(ticked(W7), (std::vector<std::string>{}));                  // 7 keeps what was saved for 7
        }
    }

private:
    QTemporaryDir *SuiteDataRoot = nullptr;
};

QTEST_MAIN(PreLaunchTest)
#include "test_prelaunch.moc"
