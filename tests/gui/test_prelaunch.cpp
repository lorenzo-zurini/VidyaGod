// The pre-launch window's graft list, driven through the real widgets: the list is the instance's ordered GRAFTS
// (the order they apply in), Move up / Move down reorder it, and a move that would leave a graft unapplied (a graft
// on a graft moved above the graft it needs) is refused on screen and changes nothing saved.

#include <QtTest>
#include <QCheckBox>
#include <QLineEdit>
#include <QComboBox>
#include <QGroupBox>
#include <QTabWidget>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <set>

#include "apppaths.h"
#include "instancestore.h"
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
        //The add-ons live on their own tab: open it, as the person reordering them has.
        if (auto *Tabs = W.findChild<QTabWidget *>("prelaunchTabs"))
            for (int i = 0; i < Tabs->count(); ++i) if (Tabs->widget(i)->isAncestorOf(List)) Tabs->setCurrentIndex(i);
        QCoreApplication::processEvents();
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

    // Grafts that exclude each other are a choice of one on screen: ticking one unticks the others, whichever side
    // carries the NOT (AoE2's soundtracks: tc NOT aok, both NOT aok + NOT tc), and every choice stays listed so the
    // user can switch with one click, and no row moves when one is ticked. Teeth: tick without TickGraft (all stay
    // ticked); drop a graft whose NOT is hit from the list (tc vanishes while aok is ticked); list ticked rows first
    // (the ticked row jumps to the top).
    void ticking_one_of_exclusive_grafts_unticks_the_others()
    {
        NodeIndex idx;
        idx.Nodes["base"] = parse(json{ {"CID", "base"}, {"LABEL", "base"}, {"LAYERS", json::array({ json{{"DIR", "base"}} })} });
        idx.Nodes["game"] = parse(json{ {"CID", "game"}, {"LABEL", "game"}, {"VARIANT", "v1"},
            {"LAYERS", json::array({ json{{"NODE", "base"}}, json{{"EXEC", json::array({ json{{"LABEL", "Play"},
                {"HOST", ManifestModel::MachinePlatform()}, {"EXE", "game.exe"}, {"TILE", {{"UID", "17"}, {"TITLE", "Game"}}}} })}} })} });
        const auto graft = [](const char *Id, const json &Nots, bool Rec) {
            json L = json::array({ json{{"ANY", json::array({"game"})}} });
            for (const auto &N : Nots) L.push_back(json{{"NOT", N}});
            L.push_back(json{{"DIR", Id}});
            json G{ {"CID", Id}, {"LABEL", Id}, {"LAYERS", L} };
            if (Rec) G["RECOMMENDED"] = json::array({"17"});   // its own tile: the suite shares one data root
            return parse(G);
        };
        idx.Nodes["aok"] = graft("aok", json::array(), false);
        idx.Nodes["tc"] = graft("tc", json::array({"aok"}), false);
        idx.Nodes["both"] = graft("both", json::array({"aok", "tc"}), true);
        ManifestModel::DeriveFacts(idx);

        json Cfg = json{{"Settings", json::object()}};
        PreLaunchWindow W(&Cfg, &idx, {"game"});
        W.show();
        QCoreApplication::processEvents();
        auto *List = W.findChild<QTreeWidget *>("graftList");
        QVERIFY(List);
        const auto ticked = [&] {
            std::vector<std::string> R;
            for (int i = 0; i < List->topLevelItemCount(); ++i)
                if (List->topLevelItem(i)->checkState(0) == Qt::Checked) R.push_back(List->topLevelItem(i)->text(0).toStdString());
            return R;
        };
        const auto listed = [&] {
            std::set<std::string> R;
            for (int i = 0; i < List->topLevelItemCount(); ++i) R.insert(List->topLevelItem(i)->text(0).toStdString());
            return R;
        };
        const auto tick = [&](const char *Label) {
            for (int i = 0; i < List->topLevelItemCount(); ++i)
                if (List->topLevelItem(i)->text(0) == Label) { List->topLevelItem(i)->setCheckState(0, Qt::Checked); break; }
            QCoreApplication::processEvents();   // the toggle is handled queued (the list rebuilds)
        };
        const auto rows = [&] {
            std::vector<std::string> R;
            for (int i = 0; i < List->topLevelItemCount(); ++i) R.push_back(List->topLevelItem(i)->text(0).toStdString());
            return R;
        };
        const std::set<std::string> All{"aok", "tc", "both"};
        const std::vector<std::string> Order{"aok", "tc", "both"};   // the default order (tc and both follow what they name) — ticking never moves a row
        QCOMPARE(ticked(), (std::vector<std::string>{"both"}));   // the recommended one
        QCOMPARE(listed(), All);
        tick("aok");
        QCOMPARE(ticked(), (std::vector<std::string>{"aok"}));    // both's NOT names aok
        QCOMPARE(rows(), Order);
        tick("tc");
        QCOMPARE(ticked(), (std::vector<std::string>{"tc"}));     // tc's own NOT names aok
        QCOMPARE(rows(), Order);
        tick("both");
        QCOMPARE(ticked(), (std::vector<std::string>{"both"}));
        QCOMPARE(rows(), Order);
        QCOMPARE(PackageCatalog::AppliedGrafts(idx, "game", std::vector<std::string>{"both"}), (std::vector<std::string>{"both"}));

        // A list saved before the exclusion was enforced holds all three: the window ticks what the launch applies.
        // Teeth: tick the saved list as saved (all three show ticked).
        PackageCatalog::SetPackageUserSetting(Cfg, idx.Find("game")->PackageUid, "GRAFTS", json{{"17", json::array({"tc", "aok", "both"})}});
        PreLaunchWindow W2(&Cfg, &idx, {"game"});
        W2.show();
        QCoreApplication::processEvents();
        auto *List2 = W2.findChild<QTreeWidget *>("graftList");
        std::vector<std::string> T2;
        for (int i = 0; i < List2->topLevelItemCount(); ++i)
            if (List2->topLevelItem(i)->checkState(0) == Qt::Checked) T2.push_back(List2->topLevelItem(i)->text(0).toStdString());
        QCOMPARE(T2, (std::vector<std::string>{"tc"}));           // tc applies first; aok and both are then excluded
    }

    // A saved list naming a graft the library no longer has was saved against an older version of the package (an
    // edit re-mints its grafts): the tile opens with its defaults, not with nothing ticked. Teeth: honour the stale
    // list (the recommended graft shows unticked).
    void aStaleSavedGraftListFallsBackToTheDefaults()
    {
        NodeIndex idx;
        idx.Nodes["game"] = parse(json{ {"CID", "game"}, {"LABEL", "game"}, {"VARIANT", "v1"},
            {"LAYERS", json::array({ json{{"DIR", "game"}}, json{{"EXEC", json::array({ json{{"LABEL", "Play"},
                {"HOST", ManifestModel::MachinePlatform()}, {"EXE", "game.exe"}, {"TILE", {{"UID", "47"}, {"TITLE", "Game"}}}} })}} })} });
        idx.Nodes["ost"] = parse(json{ {"CID", "ost"}, {"LABEL", "ost"}, {"RECOMMENDED", json::array({"47"})},
            {"LAYERS", json::array({ json{{"ANY", json::array({"game"})}}, json{{"DIR", "ost"}} })} });
        ManifestModel::DeriveFacts(idx);
        json Cfg = json{{"Settings", json::object()}};
        PackageCatalog::SetPackageUserSetting(Cfg, idx.Find("game")->PackageUid, "GRAFTS", json{{"47", json::array({"ost-before-its-remint"})}});
        PreLaunchWindow W(&Cfg, &idx, {"game"});
        W.show();
        QCoreApplication::processEvents();
        auto *List = W.findChild<QTreeWidget *>("graftList");
        QVERIFY(List && List->topLevelItemCount() == 1);
        QCOMPARE(List->topLevelItem(0)->checkState(0), Qt::Checked);
        QVERIFY((PackageCatalog::CurrentGraftChoice(idx, std::vector<std::string>{"ost"}) == std::vector<std::string>{"ost"}));
        QVERIFY(!PackageCatalog::CurrentGraftChoice(idx, std::vector<std::string>{"ost", "gone"}));
    }

    // A graft ticked beside others keeps its row: it applies after the ticked rows above it, so the rows' places —
    // filled in apply order — hold it where it was. Teeth: append it to the apply order (x jumps below y and z).
    void aTickedGraftKeepsItsRow()
    {
        NodeIndex idx;
        idx.Nodes["game"] = parse(json{ {"CID", "game"}, {"LABEL", "game"}, {"VARIANT", "v1"},
            {"LAYERS", json::array({ json{{"DIR", "game"}}, json{{"EXEC", json::array({ json{{"LABEL", "Play"},
                {"HOST", ManifestModel::MachinePlatform()}, {"EXE", "game.exe"}, {"TILE", {{"UID", "27"}, {"TITLE", "Game"}}}} })}} })} });
        const auto graft = [](const char *Id, bool Rec) {
            json G{ {"CID", Id}, {"LABEL", Id}, {"LAYERS", json::array({ json{{"ANY", json::array({"game"})}}, json{{"DIR", Id}} })} };
            if (Rec) G["RECOMMENDED"] = json::array({"27"});
            return parse(G);
        };
        idx.Nodes["x"] = graft("x", false);
        idx.Nodes["y"] = graft("y", true);
        idx.Nodes["z"] = graft("z", true);
        ManifestModel::DeriveFacts(idx);
        json Cfg = json{{"Settings", json::object()}};
        PreLaunchWindow W(&Cfg, &idx, {"game"});
        W.show();
        QCoreApplication::processEvents();
        auto *List = W.findChild<QTreeWidget *>("graftList");
        QVERIFY(List);
        const auto rows = [&] {
            std::vector<std::string> R;
            for (int i = 0; i < List->topLevelItemCount(); ++i)
                R.push_back(List->topLevelItem(i)->text(0).toStdString() + (List->topLevelItem(i)->checkState(0) == Qt::Checked ? "+" : ""));
            return R;
        };
        QCOMPARE(rows(), (std::vector<std::string>{"x", "y+", "z+"}));
        List->topLevelItem(0)->setCheckState(0, Qt::Checked);
        QCoreApplication::processEvents();
        QCOMPARE(rows(), (std::vector<std::string>{"x+", "y+", "z+"}));
    }

    // The instance picker: every instance of the game, the last played first; picking one shows ITS option values,
    // and an add-on ticked then is saved to IT, not to the other. Teeth: read and write the active instance whatever
    // is picked (the other instance's value shows; the tick lands in the wrong instance.json).
    void theInstancePickerShowsAndSavesThePickedInstance()
    {
        NodeIndex idx;
        idx.Nodes["game"] = parse(json{ {"CID", "game"}, {"LABEL", "game"}, {"VARIANT", "v1"},
            {"LAYERS", json::array({ json{{"DIR", "game"}},
                json{{"VARS", {{"opt", {{"DEFAULT", "d"}, {"UI", {{"CONTROL", "text"}, {"LABEL", "Option"}}}}}}}},
                json{{"EXEC", json::array({ json{{"LABEL", "Play"}, {"HOST", ManifestModel::MachinePlatform()}, {"EXE", "game.exe"},
                    {"TILE", {{"UID", "41"}, {"TITLE", "Game"}}}} })}} })} });
        idx.Nodes["g"] = parse(json{ {"CID", "g"}, {"LABEL", "g"}, {"LAYERS", json::array({ json{{"ANY", json::array({"game"})}}, json{{"DIR", "g"}} })} });
        ManifestModel::DeriveFacts(idx);
        json Cfg = json{{"Settings", json::object()}};
        const std::string Uid = idx.Find("game")->GameKey();
        QVERIFY(InstanceStore::WriteConfig(Cfg, Uid, "Alpha", json{{"VARIABLES", {{"opt", "alpha"}}}, {"LASTRUN", "2026-09-30T10:00:00Z"}}));
        QVERIFY(InstanceStore::WriteConfig(Cfg, Uid, "Beta",  json{{"VARIABLES", {{"opt", "beta"}}},  {"LASTRUN", "2026-09-01T10:00:00Z"}}));
        PreLaunchWindow W(&Cfg, &idx, {"game"});
        W.show();
        QCoreApplication::processEvents();
        auto *Pick = W.findChild<QComboBox *>("instanceCombo");
        QVERIFY(Pick);
        QCOMPARE(Pick->count(), 2);
        QCOMPARE(Pick->currentData().toString(), QString("Alpha"));          // the last played
        const auto value = [&] {
            for (QLineEdit *E : W.findChildren<QLineEdit *>()) if (E->property("CVKey").toString() == "opt") return E->text();
            return QString("<none>");
        };
        QCOMPARE(value(), QString("alpha"));
        Pick->setCurrentIndex(Pick->findData("Beta"));
        QCoreApplication::processEvents();
        QCOMPARE(value(), QString("beta"));                                  // Beta's own value
        auto *List = W.findChild<QTreeWidget *>("graftList");
        QVERIFY(List);
        QTreeWidgetItem *G = nullptr;
        for (QTreeWidgetItemIterator It(List); *It; ++It) if ((*It)->text(0) == "g") G = *It;
        QVERIFY(G);
        G->setCheckState(0, Qt::Checked);
        QCoreApplication::processEvents();
        const json Beta = InstanceStore::ReadConfig(Cfg, Uid, "Beta"), Alpha = InstanceStore::ReadConfig(Cfg, Uid, "Alpha");
        QVERIFY2(Beta.contains("GRAFTS") && Beta["GRAFTS"].contains("41") && Beta["GRAFTS"]["41"] == json::array({"g"}), Beta.dump().c_str());
        QVERIFY2(!Alpha.contains("GRAFTS"), Alpha.dump().c_str());          // the other instance is untouched
    }

    // A version with one way to run is called by its name alone ("1.0", not "1.0 - Play"); one with several names each
    // entry. Teeth: append the entry's label to a single-entry version.
    void aVersionIsCalledByItsName()
    {
        NodeIndex idx;
        const auto Exec = [](std::vector<std::string> Labels) {
            json E = json::array();
            for (const std::string &L : Labels)
                E.push_back(json{{"LABEL", L}, {"HOST", ManifestModel::MachinePlatform()}, {"EXE", L + ".exe"}, {"TILE", {{"UID", "47"}, {"TITLE", "Game"}}}});
            return E;
        };
        idx.Nodes["one"] = parse(json{ {"CID", "one"}, {"LABEL", "one"}, {"VARIANT", "1.0"},
            {"LAYERS", json::array({ json{{"DIR", "one"}}, json{{"EXEC", Exec({"Play"})}} })} });
        idx.Nodes["two"] = parse(json{ {"CID", "two"}, {"LABEL", "two"}, {"VARIANT", "2.0"},
            {"LAYERS", json::array({ json{{"DIR", "two"}}, json{{"EXEC", Exec({"Play", "Editor"})}} })} });
        ManifestModel::DeriveFacts(idx);
        json Cfg = json{{"Settings", json::object()}};
        PreLaunchWindow W(&Cfg, &idx, {"one", "two"}, "47");
        QComboBox *Versions = nullptr;
        for (QComboBox *C : W.findChildren<QComboBox *>()) if (C->count() >= 3) Versions = C;
        QVERIFY(Versions);
        QStringList Shown;
        for (int i = 0; i < Versions->count(); ++i) Shown << Versions->itemText(i);
        QVERIFY2(Shown.contains("1.0"), qPrintable(Shown.join(" | ")));
        QVERIFY2(Shown.contains("2.0 - Play") && Shown.contains("2.0 - Editor"), qPrintable(Shown.join(" | ")));
    }

    // On the card a graft presents (a mod with its own card), the graft is the game: applied (its options show) and not
    // an add-on to tick; on the base game's card it is not offered either — it is played from its own card. Teeth: leave
    // the card's graft out of the launch; offer a graft that has its own card; offer what the card's graft contains.
    void aGraftsCardAppliesItAndOffersItNowhere()
    {
        NodeIndex idx;
        const auto entry = [](const char *Exe, const char *Uid) {
            return json{ {"LABEL", "Play"}, {"HOST", ManifestModel::MachinePlatform()}, {"EXE", Exe}, {"TILE", {{"UID", Uid}, {"TITLE", Uid}}} };
        };
        idx.Nodes["tc"] = parse(json{ {"CID", "tc"}, {"LABEL", "tc"}, {"VARIANT", "1.0e"},
            {"LAYERS", json::array({ json{{"DIR", "tc"}}, json{{"EXEC", json::array({ entry("tc.exe", "53") })}} })} });
        idx.Nodes["up"] = parse(json{ {"CID", "up"}, {"LABEL", "up"},                     // a patch FE needs, and contains
            {"LAYERS", json::array({ json{{"ANY", json::array({"tc"})}}, json{{"DIR", "up"}} })} });
        idx.Nodes["fe"] = parse(json{ {"CID", "fe"}, {"LABEL", "fe"},
            {"LAYERS", json::array({ json{{"ANY", json::array({"tc"})}}, json{{"NODE", "up"}}, json{{"DIR", "fe"}},
                json{{"VARS", {{"fe_opt", {{"DEFAULT", "1"}, {"UI", {{"CONTROL", "bool"}, {"LABEL", "FE option"}}}}}}}},
                json{{"EXEC", json::array({ entry("fe.exe", "53001") })}} })} });
        ManifestModel::DeriveFacts(idx);
        json Cfg = json{{"Settings", json::object()}};
        const auto look = [&](const char *Face, bool &HasOption, bool &Offered, bool &UpOffered) {
            PreLaunchWindow W(&Cfg, &idx, {"tc"}, Face);
            QCoreApplication::processEvents();
            HasOption = Offered = UpOffered = false;
            for (QWidget *X : W.findChildren<QWidget *>()) if (X->property("CVKey").toString() == "fe_opt") HasOption = true;
            if (auto *List = W.findChild<QTreeWidget *>("graftList"))
                for (QTreeWidgetItemIterator It(List); *It; ++It)
                {
                    if ((*It)->text(0) == "fe") Offered = true;
                    if ((*It)->text(0) == "up") UpOffered = true;
                }
        };
        bool Opt = false, Off = false, Up = false;
        look("53001", Opt, Off, Up);
        QVERIFY2(Opt, "on its card the graft is not applied");
        QVERIFY(!Off);
        QVERIFY2(!Up, "what the card's graft contains is offered as a choice");
        look("53", Opt, Off, Up);
        QVERIFY(!Opt && !Off);                                                // the base game: neither applied nor offered
        QVERIFY(Up);                                                          // but the patch is its add-on there
    }

    // Grafts that are versions are rows: the card's version picker names them, the picked one is applied (its option
    // shows), and none is offered as an add-on. Teeth: rows from a graft version's own resolve (no base: no entry, no
    // row); offer graft versions as add-ons.
    void graftVersionsAreRows()
    {
        NodeIndex idx;
        const auto entry = [](const char *Exe, const char *Uid) {
            return json{ {"LABEL", "Play"}, {"HOST", ManifestModel::MachinePlatform()}, {"EXE", Exe}, {"TILE", {{"UID", Uid}, {"TITLE", Uid}}} };
        };
        idx.Nodes["tc"] = parse(json{ {"CID", "tc"}, {"LABEL", "tc"}, {"VARIANT", "1.0e"},
            {"LAYERS", json::array({ json{{"DIR", "tc"}}, json{{"EXEC", json::array({ entry("tc.exe", "61") })}} })} });
        const auto fe = [&](const char *Id, const char *Ver) {
            return parse(json{ {"CID", Id}, {"LABEL", Id}, {"VARIANT", Ver},
                {"LAYERS", json::array({ json{{"ANY", json::array({"tc"})}}, json{{"DIR", Id}},
                    json{{"VARS", {{std::string(Id) + "_opt", {{"DEFAULT", "1"}, {"UI", {{"CONTROL", "bool"}, {"LABEL", Id}}}}}}}},
                    json{{"EXEC", json::array({ entry("fe.exe", "61001") })}} })} });
        };
        idx.Nodes["fe22"] = fe("fe22", "2.2");
        idx.Nodes["fe25"] = fe("fe25", "2.5");
        ManifestModel::DeriveFacts(idx);
        json Cfg = json{{"Settings", json::object()}};
        PreLaunchWindow W(&Cfg, &idx, {"fe22", "fe25"}, "61001");
        QCoreApplication::processEvents();
        QComboBox *Versions = nullptr;
        for (QComboBox *C : W.findChildren<QComboBox *>()) if (C->count() == 2 && C->findText("2.5") >= 0) Versions = C;
        QVERIFY2(Versions, "the graft versions are not the card's rows");
        const auto hasOpt = [&](const QString &K) {
            for (QWidget *X : W.findChildren<QWidget *>()) if (X->property("CVKey").toString() == K) return true;
            return false;
        };
        Versions->setCurrentIndex(Versions->findText("2.5"));
        QCoreApplication::processEvents();
        QVERIFY(hasOpt("fe25_opt") && !hasOpt("fe22_opt"));                   // the picked version is applied, only it
        if (auto *List = W.findChild<QTreeWidget *>("graftList"))
            for (QTreeWidgetItemIterator It(List); *It; ++It) QVERIFY((*It)->text(0) != "fe22" && (*It)->text(0) != "fe25");

        //A version with no entry of its own (a mod build) runs its base's: it is still a row of the base's card.
        idx.Nodes["mod"] = parse(json{ {"CID", "mod"}, {"LABEL", "mod"}, {"VARIANT", "1.0e + mod"},
            {"LAYERS", json::array({ json{{"ANY", json::array({"tc"})}}, json{{"DIR", "mod"}} })} });
        ManifestModel::DeriveFacts(idx);
        PreLaunchWindow W2(&Cfg, &idx, {"tc", "mod"}, "61");
        QComboBox *Rows = nullptr;
        for (QComboBox *C : W2.findChildren<QComboBox *>()) if (C->findText("1.0e") >= 0) Rows = C;
        QVERIFY(Rows);
        QVERIFY2(Rows->findText("1.0e + mod") >= 0, "a version with no entry of its own has no row");
    }

    // The tabs exist only when they have something in them: a game with no options and no add-ons opens on Advanced.
    // Teeth: always show Options and Add-ons.
    void emptyTabsAreNotShown()
    {
        NodeIndex idx;
        idx.Nodes["game"] = parse(json{ {"CID", "game"}, {"LABEL", "game"}, {"VARIANT", "v1"},
            {"LAYERS", json::array({ json{{"DIR", "game"}},
                json{{"EXEC", json::array({ json{{"LABEL", "Play"}, {"HOST", ManifestModel::MachinePlatform()}, {"EXE", "game.exe"},
                    {"TILE", {{"UID", "43"}, {"TITLE", "Game"}}}} })}} })} });
        ManifestModel::DeriveFacts(idx);
        json Cfg = json{{"Settings", json::object()}};
        PreLaunchWindow W(&Cfg, &idx, {"game"});
        W.show();
        QCoreApplication::processEvents();
        auto *Tabs = W.findChild<QTabWidget *>("prelaunchTabs");
        QVERIFY(Tabs);
        QStringList Shown;
        for (int i = 0; i < Tabs->count(); ++i) if (Tabs->isTabVisible(i)) Shown << Tabs->tabText(i);
        QCOMPARE(Shown, (QStringList{"Advanced", "Log"}));
        QCOMPARE(Tabs->tabText(Tabs->currentIndex()), QString("Advanced"));
    }

    // Options are a tree: UI.SECTION paths ('/' nests) build collapsed section rows, each option a row with its label
    // in column 0 and its control in column 1 (so controls line up whatever the label's length); a WHEN hides the
    // option's row, and a section left with nothing shown hides too. Teeth: ignore SECTION (options land at the top);
    // expand sections by default; hide only the control (the row stays); keep an emptied section.
    void optionsAreASectionedTree()
    {
        NodeIndex idx;
        idx.Nodes["game"] = parse(json{ {"CID", "game"}, {"LABEL", "game"}, {"VARIANT", "v1"},
            {"LAYERS", json::array({ json{{"DIR", "game"}},
                json{{"VARS", {
                    {"top", {{"DEFAULT", "1"}, {"UI", {{"CONTROL", "bool"}, {"LABEL", "Top level"}}}}},
                    {"a", {{"DEFAULT", "1"}, {"UI", {{"CONTROL", "bool"}, {"SECTION", "Mod/Interface"}, {"LABEL", "A"}}}}},
                    {"b", {{"DEFAULT", "0"}, {"UI", {{"CONTROL", "bool"}, {"SECTION", "Mod/Interface"}, {"LABEL", "A much, much longer label than the first"}}}}},
                    {"c", {{"DEFAULT", "x"}, {"WHEN", "%a%==1"}, {"UI", {{"CONTROL", "text"}, {"SECTION", "Mod/Only A"}, {"LABEL", "Only with A"}}}}}}}},
                json{{"EXEC", json::array({ json{{"LABEL", "Play"}, {"HOST", ManifestModel::MachinePlatform()}, {"EXE", "game.exe"},
                    {"TILE", {{"UID", "37"}, {"TITLE", "Game"}}}} })}} })} });
        ManifestModel::DeriveFacts(idx);
        json Cfg = json{{"Settings", json::object()}};
        PreLaunchWindow W(&Cfg, &idx, {"game"});
        W.resize(900, 700);
        W.show();
        QCoreApplication::processEvents();
        auto *T = W.findChild<QTreeWidget *>("optionsTree");
        QVERIFY(T);
        const auto find = [&](const QString &Text) -> QTreeWidgetItem * {
            for (QTreeWidgetItemIterator It(T); *It; ++It) if ((*It)->text(0) == Text) return *It;
            return nullptr;
        };
        QTreeWidgetItem *Mod = find("Mod"), *Ui = find("Interface"), *OnlyA = find("Only A"), *Top = find("Top level");
        QVERIFY(Mod && Ui && OnlyA && Top);
        QVERIFY(Top->parent() == nullptr);                                     // no SECTION: at the top
        QVERIFY(Ui->parent() == Mod && OnlyA->parent() == Mod);                // Mod/Interface nests under Mod
        QVERIFY(!Mod->isExpanded() && !Ui->isExpanded());                      // collapsed by default
        QTreeWidgetItem *Ra = find("A"), *Rb = find("A much, much longer label than the first"), *Rc = find("Only with A");
        QVERIFY(Ra && Rb && Rc && Ra->parent() == Ui && Rc->parent() == OnlyA);
        Mod->setExpanded(true); Ui->setExpanded(true); OnlyA->setExpanded(true);
        QCoreApplication::processEvents();
        QWidget *A = T->itemWidget(Ra, 1), *B = T->itemWidget(Rb, 1);
        QVERIFY(A && B);
        QCOMPARE(A->mapTo(T, QPoint(0, 0)).x(), B->mapTo(T, QPoint(0, 0)).x());   // one column
        QVERIFY(!Rc->isHidden() && !OnlyA->isHidden());
        qobject_cast<QCheckBox *>(A)->setChecked(false);
        QCoreApplication::processEvents();
        QVERIFY(Rc->isHidden());                                                 // the row goes as one
        QVERIFY(OnlyA->isHidden());                                              // and its section, now empty
        qobject_cast<QCheckBox *>(A)->setChecked(true);
        QCoreApplication::processEvents();
        QVERIFY(!Rc->isHidden() && !OnlyA->isHidden());
    }

    // A graft's SECTION puts it in a collapsed section row of the graft tree; it still ticks, and the tree read depth-
    // first is the order grafts apply in. Teeth: ignore SECTION (the graft sits at the top); read only top-level rows
    // (the sectioned graft is never collected).
    void graftsSitInTheirSections()
    {
        NodeIndex idx;
        idx.Nodes["game"] = parse(json{ {"CID", "game"}, {"LABEL", "game"}, {"VARIANT", "v1"},
            {"LAYERS", json::array({ json{{"DIR", "game"}}, json{{"EXEC", json::array({ json{{"LABEL", "Play"},
                {"HOST", ManifestModel::MachinePlatform()}, {"EXE", "game.exe"}, {"TILE", {{"UID", "57"}, {"TITLE", "Game"}}}} })}} })} });
        const auto graft = [](const char *Id, const char *Section, bool Rec) {
            json G{ {"CID", Id}, {"LABEL", Id}, {"LAYERS", json::array({ json{{"ANY", json::array({"game"})}}, json{{"DIR", Id}} })} };
            if (Section) G["SECTION"] = Section;
            if (Rec) G["RECOMMENDED"] = json::array({"57"});
            return parse(G);
        };
        idx.Nodes["ost"] = graft("ost", "Music/Soundtracks", true);
        idx.Nodes["fix"] = graft("fix", nullptr, false);
        ManifestModel::DeriveFacts(idx);
        json Cfg = json{{"Settings", json::object()}};
        PreLaunchWindow W(&Cfg, &idx, {"game"});
        W.show();
        QCoreApplication::processEvents();
        auto *T = W.findChild<QTreeWidget *>("graftList");
        QVERIFY(T);
        QTreeWidgetItem *Music = nullptr, *Sound = nullptr, *Ost = nullptr, *Fix = nullptr;
        for (QTreeWidgetItemIterator It(T); *It; ++It)
        {
            const QString X = (*It)->text(0);
            if (X == "Music") Music = *It; else if (X == "Soundtracks") Sound = *It; else if (X == "ost") Ost = *It; else if (X == "fix") Fix = *It;
        }
        QVERIFY(Music && Sound && Ost && Fix);
        QVERIFY(Sound->parent() == Music && Ost->parent() == Sound && Fix->parent() == nullptr);
        QVERIFY(!Music->isExpanded());
        QCOMPARE(Ost->checkState(0), Qt::Checked);                               // recommended, inside its section
        Fix->setCheckState(0, Qt::Checked);
        QCoreApplication::processEvents();
        const json US = PackageCatalog::GetPackageUserSettings(Cfg, idx.Find("game")->PackageUid);
        QVERIFY(US.contains("GRAFTS") && US["GRAFTS"].contains("57"));
        std::vector<std::string> Saved;
        for (const auto &G : US["GRAFTS"]["57"]) Saved.push_back(G.get<std::string>());
        QCOMPARE(Saved, (std::vector<std::string>{"ost", "fix"}));             // depth-first: the section's graft first
    }

    // A rebuild of the options (a graft ticked) must not leave the previous controls alive beside the new ones: clear()
    // only deleteLater()s a row's control, and the condition pass right after walks every control — the old ones point
    // at their freed rows (a use-after-free) and feed their stale values. Deferred deletes do not run inside
    // processEvents() here, so the leftover is caught by the ASan build (build-asan, VIDYAGOD_SANITIZE=ON): heap-use-
    // after-free in EvaluateVarConditions. Teeth (ASan): drop the synchronous delete.
    void aRebuildLeavesNoStaleOptionControls()
    {
        NodeIndex idx;
        idx.Nodes["game"] = parse(json{ {"CID", "game"}, {"LABEL", "game"}, {"VARIANT", "v1"},
            {"LAYERS", json::array({ json{{"DIR", "game"}},
                json{{"VARS", {
                    {"a", {{"DEFAULT", "1"}, {"UI", {{"CONTROL", "bool"}, {"LABEL", "A"}}}}},
                    {"c", {{"DEFAULT", "x"}, {"WHEN", "%a%==1"}, {"UI", {{"CONTROL", "text"}, {"LABEL", "Only with A"}}}}}}}},
                json{{"EXEC", json::array({ json{{"LABEL", "Play"}, {"HOST", ManifestModel::MachinePlatform()}, {"EXE", "game.exe"},
                    {"TILE", {{"UID", "67"}, {"TITLE", "Game"}}}} })}} })} });
        idx.Nodes["g"] = parse(json{ {"CID", "g"}, {"LABEL", "g"},
            {"LAYERS", json::array({ json{{"ANY", json::array({"game"})}}, json{{"DIR", "g"}} })} });
        ManifestModel::DeriveFacts(idx);
        json Cfg = json{{"Settings", json::object()}};
        PreLaunchWindow W(&Cfg, &idx, {"game"});
        W.show();
        QCoreApplication::processEvents();
        const auto controls = [&](const char *Key) {
            int N = 0;
            for (QWidget *X : W.findChildren<QWidget *>()) if (X->property("CVKey").toString() == Key) ++N;
            return N;
        };
        QCOMPARE(controls("c"), 1);
        auto *List = W.findChild<QTreeWidget *>("graftList");
        QVERIFY(List && List->topLevelItemCount() == 1);
        List->topLevelItem(0)->setCheckState(0, Qt::Checked);   // rebuilds the options
        QCoreApplication::processEvents();
        QCOMPARE(controls("c"), 1);
        QCOMPARE(controls("a"), 1);
    }

    // Sections group grafts for display, but grafts apply in their own order: X needs Y, yet X's section is shown above
    // Y's. Ticking Y, then X, then an unrelated graft must keep Y before X in what is saved and launched — read off
    // the tree depth-first, X would be judged before Y and silently dropped while shown ticked.
    // Teeth: collect the ticked grafts in tree order.
    void aGraftNeedingOneInALaterSectionStillApplies()
    {
        NodeIndex idx;
        idx.Nodes["game"] = parse(json{ {"CID", "game"}, {"LABEL", "game"}, {"VARIANT", "v1"},
            {"LAYERS", json::array({ json{{"DIR", "game"}}, json{{"EXEC", json::array({ json{{"LABEL", "Play"},
                {"HOST", ManifestModel::MachinePlatform()}, {"EXE", "game.exe"}, {"TILE", {{"UID", "77"}, {"TITLE", "Game"}}}} })}} })} });
        const auto graft = [](const char *Id, const char *Label, const char *Section, const char *On) {
            return parse(json{ {"CID", Id}, {"LABEL", Label}, {"SECTION", Section},
                {"LAYERS", json::array({ json{{"ANY", json::array({On})}}, json{{"DIR", Id}} })} });
        };
        idx.Nodes["z"] = graft("z", "Aaa", "Mods", "game");
        idx.Nodes["y"] = graft("y", "Base", "Core", "game");
        idx.Nodes["x"] = graft("x", "Needs base", "Mods", "y");
        ManifestModel::DeriveFacts(idx);
        json Cfg = json{{"Settings", json::object()}};
        PreLaunchWindow W(&Cfg, &idx, {"game"});
        W.show();
        QCoreApplication::processEvents();
        auto *T = W.findChild<QTreeWidget *>("graftList");
        const auto tick = [&](const char *Label) {
            for (QTreeWidgetItemIterator It(T); *It; ++It)
                if ((*It)->text(0) == Label) { (*It)->setCheckState(0, Qt::Checked); break; }
            QCoreApplication::processEvents();
        };
        const auto saved = [&] {
            const json US = PackageCatalog::GetPackageUserSettings(Cfg, idx.Find("game")->PackageUid);
            std::vector<std::string> G;
            if (US.contains("GRAFTS") && US["GRAFTS"].contains("77")) for (const auto &X : US["GRAFTS"]["77"]) G.push_back(X.get<std::string>());
            return G;
        };
        tick("Base");
        tick("Needs base");                                   // offered once Base is ticked
        tick("Aaa");
        const std::vector<std::string> S = saved();
        QVERIFY2(S.size() == 3, "all three ticked");
        const auto A = PackageCatalog::AppliedGrafts(idx, "game", S);
        QVERIFY2(std::find(A.begin(), A.end(), "x") != A.end(), "the graft that needs Base is applied, not dropped");
    }

private:
    QTemporaryDir *SuiteDataRoot = nullptr;
};

QTEST_MAIN(PreLaunchTest)
#include "test_prelaunch.moc"
