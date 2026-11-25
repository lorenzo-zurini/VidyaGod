// The pre-launch window's graft list, driven through the real widgets: the list is the instance's ordered GRAFTS
// (the order they apply in), Move up / Move down reorder it, and a move that would leave a graft unapplied (a graft
// on a graft moved above the graft it needs) is refused on screen and changes nothing saved.

#include <QtTest>
#include "libraryview.h"   // LibraryGameCard — a card opens its tile's one launch window
#include <QMessageBox>
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
#include "cardpool.h"   // CardPool::LibraryRows — the rows an open window follows
#include "shelf.h"
#include "instancestore.h"
#include "manifestmodel.h"
#include "packagecatalog.h"
#include "prelaunchwindow.h"
#include "launchthread.h"
#include "keycapture.h"
#define SDL_MAIN_HANDLED   // SDL must not rename main() (it does on Windows)
#include <SDL.h>
#include <QToolButton>

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
            const json US = InstanceStore::ReadConfig(Cfg, idx.Find("game")->PackageUid);
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
        QCOMPARE(Shelf::AppliedGrafts(idx, "game", std::vector<std::string>{"both"}), (std::vector<std::string>{"both"}));

        // A list saved before the exclusion was enforced holds all three: the window ticks what the launch applies.
        // Teeth: tick the saved list as saved (all three show ticked).
        InstanceStore::SetSetting(Cfg, idx.Find("game")->PackageUid, "GRAFTS", json{{"17", json::array({"tc", "aok", "both"})}});
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
        InstanceStore::SetSetting(Cfg, idx.Find("game")->PackageUid, "GRAFTS", json{{"47", json::array({"ost-before-its-remint"})}});
        PreLaunchWindow W(&Cfg, &idx, {"game"});
        W.show();
        QCoreApplication::processEvents();
        auto *List = W.findChild<QTreeWidget *>("graftList");
        QVERIFY(List && List->topLevelItemCount() == 1);
        QCOMPARE(List->topLevelItem(0)->checkState(0), Qt::Checked);
        QVERIFY((Shelf::CurrentGraftChoice(idx, std::vector<std::string>{"ost"}) == std::vector<std::string>{"ost"}));
        QVERIFY(!Shelf::CurrentGraftChoice(idx, std::vector<std::string>{"ost", "gone"}));
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

    // An option's value stays in the format's spelling in the window: a DEFAULT "100%%" (100 percent) shows and is
    // collected as "100%%", so saving it and passing it as an override does not strip an escape per launch.
    // Teeth: write the DEFAULT out for the control (it holds "100%", and the next resolve reads a lone '%').
    void anOptionKeepsALiteralPercentEscaped()
    {
        NodeIndex idx;
        idx.Nodes["game"] = parse(json{ {"CID", "game"}, {"LABEL", "game"}, {"VARIANT", "v1"}, {"LAYERS", json::array({ json{{"DIR", "game"}},
                json{{"VARS", {{"opt", {{"DEFAULT", "100%%"}, {"UI", {{"CONTROL", "text"}, {"LABEL", "Option"}}}}}}}},
                json{{"EXEC", json::array({ json{{"LABEL", "Play"}, {"HOST", ManifestModel::MachinePlatform()}, {"EXE", "game.exe"},
                    {"TILE", {{"UID", "43"}, {"TITLE", "Game"}}}} })}} })} });
        ManifestModel::DeriveFacts(idx);
        json Cfg = json{{"Settings", json::object()}};
        PreLaunchWindow W(&Cfg, &idx, {"game"});
        QCoreApplication::processEvents();
        QString Got = "<none>";
        for (QLineEdit *E : W.findChildren<QLineEdit *>()) if (E->property("CVKey").toString() == "opt") Got = E->text();
        QCOMPARE(Got, QString("100%%"));
    }

    // An open launch dialog follows its package when it is edited: an edit re-mints the node (a new CID, the same
    // working-tree handle), and the dialog, reloaded onto the library's new index, shows the edited edition — not the
    // index it opened on, which no longer has it. Teeth: reload from the dialog's own index; follow by CID alone.
    void an_open_dialog_follows_its_edited_package()
    {
        const auto game = [](const char *Cid, const char *Variant) {
            Node N = parse(json{ {"CID", Cid}, {"LABEL", "game"}, {"VARIANT", Variant},
                {"LAYERS", json::array({ json{{"EXEC", json::array({ json{{"LABEL", "Play"},
                    {"HOST", ManifestModel::MachinePlatform()}, {"EXE", "game.exe"}, {"TILE", {{"UID", "7"}, {"TITLE", "Game"}}}} })}} })} });
            N.Handle = "gamehandle";
            return N;
        };
        auto Before = std::make_shared<NodeIndex>();
        Before->Nodes["bafkreioldgame"] = game("bafkreioldgame", "Original");
        ManifestModel::DeriveFacts(*Before);
        auto After = std::make_shared<NodeIndex>();
        After->Nodes["bafkreinewgame"] = game("bafkreinewgame", "Original (edited)");
        ManifestModel::DeriveFacts(*After);
        json Cfg = json{{"Settings", json::object()}};
        PreLaunchWindow W(&Cfg, std::shared_ptr<const NodeIndex>(Before), {"bafkreioldgame"}, "7");
        const auto Shows = [&](const QString &Text) {
            for (QComboBox *C : W.findChildren<QComboBox *>())
                for (int i = 0; i < C->count(); ++i) if (C->itemText(i).contains(Text)) return true;
            return false;
        };
        QVERIFY(Shows("Original"));
        W.ReloadAndRebuild(After);
        QVERIFY(Shows("(edited)"));
    }

    // A script re-mints a library-scanned node (no working-tree handle) while its launch window is open: a new CID,
    // the same node file (files are named by LABEL). The window, reloaded onto the new index, still lists that
    // version and still has it picked — not just the versions that kept their CIDs. A graft version's row stays the
    // picked row when the graft is re-minted, a graft's own card keeps its rows when that graft is, and a file two
    // fresh nodes claim is no evidence of which one the version became.
    // Teeth: follow by CID/handle alone (the version drops); compare paths unnormalised (the "./" spelling drops it);
    // take the first of two same-file nodes; leave the remembered row's ids stale (the base's own row is picked);
    // keep the window's tile graft from the old index (the card loses its rows).
    void an_open_dialog_follows_a_version_a_script_re_minted()
    {
        const auto Node_ = [](const std::string &File, const json &J) {
            Node N;
            ManifestModel::ParseNode(J, File, "/lib/JSRF", N);
            return N;
        };
        const auto Exec = [](const char *Uid) {
            return json{{"EXEC", json::array({ json{{"LABEL", "Play"}, {"HOST", ManifestModel::MachinePlatform()}, {"EXE", "game.exe"},
                {"TILE", {{"UID", Uid}, {"TITLE", "Jet Set Radio"}}}} })}};
        };
        const auto Version = [&](const char *Cid, const char *Variant, const std::string &File) {
            return Node_(File, json{ {"CID", Cid}, {"LABEL", Variant}, {"VARIANT", Variant}, {"LAYERS", json::array({ Exec("31") })} });
        };
        const auto Index = [](std::vector<Node> Ns) {
            auto I = std::make_shared<NodeIndex>();
            for (Node &N : Ns) I->Nodes[N.Cid] = std::move(N);
            ManifestModel::DeriveFacts(*I);
            return std::shared_ptr<const NodeIndex>(I);
        };
        const auto Versions = [](PreLaunchWindow &W) -> QComboBox * {
            for (QComboBox *C : W.findChildren<QComboBox *>())
                if (C->count() && C->itemData(0).toString().contains(QChar(0x1f))) return C;
            return nullptr;
        };
        const auto Rows = [](QComboBox *C) {
            QStringList L;
            for (int i = 0; i < C->count(); ++i) L << C->itemText(i);
            return L;
        };
        json Cfg = json{{"Settings", json::object()}};

        // A plain version re-minted: still listed, still picked (it is not the row a fallback to the first picks).
        {
            const auto Before = Index({ Version("bafkreiunleashed", "JSRF Unleashed", "/lib/JSRF/JSRF Unleashed.json"),
                                        Version("bafkreid43j", "Xbox", "/lib/JSRF/Xbox.json") });
            PreLaunchWindow W(&Cfg, Before, {"bafkreiunleashed", "bafkreid43j"}, "31");
            QComboBox *V = Versions(W);
            QVERIFY(V);
            QCOMPARE(Rows(V), QStringList({"JSRF Unleashed", "Xbox"}));
            V->setCurrentIndex(1);
            W.ReloadAndRebuild(Index({ Version("bafkreiunleashed", "JSRF Unleashed", "/lib/JSRF/JSRF Unleashed.json"),
                                       Version("bafkreic56g", "Xbox", "/lib/JSRF/./Xbox.json") }));
            V = Versions(W);
            QVERIFY(V);
            QCOMPARE(Rows(V), QStringList({"JSRF Unleashed", "Xbox"}));
            QCOMPARE(V->currentText(), QString("Xbox"));
            QVERIFY(V->currentData().toString().startsWith("bafkreic56g\x1f"));
        }
        // Two fresh nodes from the old file: neither is taken for the version, which is dropped.
        {
            const auto Before = Index({ Version("bafkreiunleashed", "JSRF Unleashed", "/lib/JSRF/JSRF Unleashed.json"),
                                        Version("bafkreid43j", "Xbox", "/lib/JSRF/Xbox.json") });
            PreLaunchWindow W(&Cfg, Before, {"bafkreiunleashed", "bafkreid43j"}, "31");
            W.ReloadAndRebuild(Index({ Version("bafkreiunleashed", "JSRF Unleashed", "/lib/JSRF/JSRF Unleashed.json"),
                                       Version("bafkreia", "Xbox A", "/lib/JSRF/Xbox.json"),
                                       Version("bafkreib", "Xbox B", "/lib/JSRF/Xbox.json") }));
            QCOMPARE(Rows(Versions(W)), QStringList({"JSRF Unleashed"}));
        }
        // A node with no file (a received stub) under a new CID: no file to go by, so it is not taken for the old one.
        {
            const auto Before = Index({ Version("bafkreiunleashed", "JSRF Unleashed", "/lib/JSRF/JSRF Unleashed.json"),
                                        Version("bafkreistubold", "Xbox", "") });
            PreLaunchWindow W(&Cfg, Before, {"bafkreiunleashed", "bafkreistubold"}, "31");
            QCOMPARE(Rows(Versions(W)), QStringList({"JSRF Unleashed", "Xbox"}));
            W.ReloadAndRebuild(Index({ Version("bafkreiunleashed", "JSRF Unleashed", "/lib/JSRF/JSRF Unleashed.json"),
                                       Version("bafkreistubnew", "Xbox (peer)", "") }));
            QCOMPARE(Rows(Versions(W)), QStringList({"JSRF Unleashed"}));
        }
        // A graft version re-minted: its row (on the base, which kept its CID) stays the picked one.
        {
            const auto Patched = [&](const char *Cid, const char *On = "bafkreibase") {
                return Node_("/lib/JSRF/Patched.json", json{ {"CID", Cid}, {"LABEL", "Patched"}, {"VARIANT", "Patched"},
                    {"LAYERS", json::array({ json{{"ANY", json::array({On})}}, json{{"DIR", "patch"}} })} });
            };
            PreLaunchWindow W(&Cfg, Index({ Version("bafkreibase", "Xbox", "/lib/JSRF/Xbox.json"), Patched("bafkreipatchold") }),
                              {"bafkreibase", "bafkreipatchold"}, "31");
            QComboBox *V = Versions(W);
            QVERIFY(V);
            QCOMPARE(Rows(V), QStringList({"Patched", "Xbox"}));
            V->setCurrentIndex(0);
            QCOMPARE(V->currentData().toString(), QString("bafkreibase\x1fPlay\x1frow\x1f" "bafkreipatchold"));
            W.ReloadAndRebuild(Index({ Version("bafkreibase", "Xbox", "/lib/JSRF/Xbox.json"), Patched("bafkreipatchnew") }));
            V = Versions(W);
            QVERIFY(V);
            QCOMPARE(Rows(V), QStringList({"Patched", "Xbox"}));
            QCOMPARE(V->currentData().toString(), QString("bafkreibase\x1fPlay\x1frow\x1f" "bafkreipatchnew"));
            // Then the base too, the graft re-minted onto it (a repoint cascade): still the graft's row.
            W.ReloadAndRebuild(Index({ Version("bafkreibasenew", "Xbox", "/lib/JSRF/Xbox.json"),
                                       Patched("bafkreipatchnewer", "bafkreibasenew") }));
            V = Versions(W);
            QVERIFY(V);
            QCOMPARE(Rows(V), QStringList({"Patched", "Xbox"}));
            QCOMPARE(V->currentData().toString(), QString("bafkreibasenew\x1fPlay\x1frow\x1f" "bafkreipatchnewer"));
        }
        // A graft's own card (tile 70, presented by the graft) keeps its rows when the graft is re-minted.
        {
            const auto Mod = [&](const char *Cid) {
                return Node_("/lib/JSRF/Mod.json", json{ {"CID", Cid}, {"LABEL", "Mod"},
                    {"LAYERS", json::array({ json{{"ANY", json::array({"bafkreibase"})}}, Exec("70") })} });
            };
            const auto Before = Index({ Version("bafkreibase", "Xbox", "/lib/JSRF/Xbox.json"), Mod("bafkreimodold") });
            QCOMPARE(Before->TileGraft.at("70"), std::string("bafkreimodold"));
            PreLaunchWindow W(&Cfg, Before, {"bafkreibase"}, "70");
            QVERIFY(Versions(W));
            QCOMPARE(Rows(Versions(W)), QStringList({"Xbox"}));
            W.ReloadAndRebuild(Index({ Version("bafkreibase", "Xbox", "/lib/JSRF/Xbox.json"), Mod("bafkreimodnew") }));
            QVERIFY(Versions(W));
            QCOMPARE(Rows(Versions(W)), QStringList({"Xbox"}));
        }
    }

    // A launch resolves on the graph its dialog shows (LaunchThread::Index, AppModel's snapshot), not on a re-scan of
    // the library: a node only that graph holds is found, and with no graph the launch refuses. Teeth: re-scan the
    // config in run() (the node is "not found" — the config names no library); ignore a missing graph.
    void aLaunchResolvesOnTheDialogsGraphNotARescan()
    {
        auto idx = std::make_shared<NodeIndex>();
        idx->Nodes["only_here"] = parse(json{ {"CID", "only_here"}, {"LABEL", "only_here"}, {"LAYERS", json::array({ json{{"DIR", "x"}} })} });
        ManifestModel::DeriveFacts(*idx);
        const auto run = [&](std::shared_ptr<const NodeIndex> Graph, QStringList &Log, QString &Finished) {
            LaunchThread T;
            T.GlobalConfigJSON = json{{"Settings", json::object()}};      // names no library: a re-scan finds nothing
            T.Index            = std::move(Graph);
            T.LaunchNodeId     = "only_here";
            QObject::connect(&T, &LaunchThread::logLine, &T, [&](int, QString C, QString M) { Log << C + "|" + M; }, Qt::DirectConnection);
            QObject::connect(&T, &LaunchThread::launchFinished, &T, [&](bool, QString E) { Finished = E; }, Qt::DirectConnection);
            T.start();
            QVERIFY(T.wait(30000));
        };
        QStringList Log; QString Finished;
        run(idx, Log, Finished);
        QVERIFY2(!Log.join("\n").contains("Launch node not found"), qPrintable(Log.join("\n")));
        QVERIFY2(Log.join("\n").contains("only_here"), qPrintable(Log.join("\n")));
        Log.clear(); Finished.clear();
        run(nullptr, Log, Finished);
        QCOMPARE(Finished, QString("No node graph to launch from."));
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

    // A game whose options are all in one section shows them: the lone section opens by itself (a folded row over an
    // empty pane hid them all). Teeth: no such rule (the section stays folded).
    void aLoneSectionOpensByItself()
    {
        NodeIndex idx;
        idx.Nodes["game"] = parse(json{ {"CID", "game"}, {"LABEL", "game"}, {"VARIANT", "v1"},
            {"LAYERS", json::array({ json{{"DIR", "game"}},
                json{{"VARS", {
                    {"a", {{"DEFAULT", "1"}, {"UI", {{"CONTROL", "bool"}, {"SECTION", "Matrix"}, {"LABEL", "A"}}}}},
                    {"b", {{"DEFAULT", "0"}, {"UI", {{"CONTROL", "bool"}, {"SECTION", "Matrix"}, {"LABEL", "B"}}}}}}}},
                json{{"EXEC", json::array({ json{{"LABEL", "Play"}, {"HOST", ManifestModel::MachinePlatform()}, {"EXE", "game.exe"},
                    {"TILE", {{"UID", "39"}, {"TITLE", "Game"}}}} })}} })} });
        ManifestModel::DeriveFacts(idx);
        json Cfg = json{{"Settings", json::object()}};
        PreLaunchWindow W(&Cfg, &idx, {"game"});
        W.show();
        QCoreApplication::processEvents();
        auto *T = W.findChild<QTreeWidget *>("optionsTree");
        QVERIFY(T && T->topLevelItemCount() == 1);
        QVERIFY(T->topLevelItem(0)->isExpanded());
        //No cover: the title on a cover-shaped card, not an empty block. Teeth: the label left empty.
        bool Shown = false;
        for (QLabel *L : W.findChildren<QLabel *>()) if (L->text() == "Game" && L->minimumHeight() > 100) Shown = true;
        QVERIFY(Shown);
    }

    // A `key` option is bound by pressing it: click the button, press a key (or a mouse button) over it. The value is
    // the neutral key name a WHEN (and the launch) reads; sides of Shift come from the native key; Escape binds
    // rather than cancels; focus leaving cancels; × unbinds; two options of a section on one key are both marked.
    // A binding off its default names the default; ↺ puts it back.
    // Teeth: map Key_Shift without the native keysym (left shift), let Escape cancel, collect no KeyCaptureField
    // value (the WHEN row stays hidden), skip MarkKeyConflicts, reset to "" — each fails a check below.
    void aKeyOptionIsBoundByPressingTheKey()
    {
        NodeIndex idx;
        const auto key = [](const char *Def, const char *Label) {
            return json{{"DEFAULT", Def}, {"UI", {{"CONTROL", "key"}, {"SECTION", "Input"}, {"LABEL", Label}}}};
        };
        idx.Nodes["game"] = parse(json{ {"CID", "game"}, {"LABEL", "game"}, {"VARIANT", "v1"},
            {"LAYERS", json::array({ json{{"DIR", "game"}},
                json{{"VARS", {
                    {"fwd", key("w", "Move forward")}, {"jump", key("space", "Jump")}, {"menu", key("escape", "Menu")},
                    {"only_q", {{"DEFAULT", "x"}, {"WHEN", "%jump%==q"}, {"UI", {{"CONTROL", "text"}, {"LABEL", "Only when jump is Q"}}}}}}}},
                json{{"EXEC", json::array({ json{{"LABEL", "Play"}, {"HOST", ManifestModel::MachinePlatform()}, {"EXE", "game.exe"},
                    {"TILE", {{"UID", "38"}, {"TITLE", "Game"}}}} })}} })} });
        ManifestModel::DeriveFacts(idx);
        json Cfg = json{{"Settings", json::object()}};
        InstanceStore::MergeVariables(Cfg, idx.Find("game")->PackageUid, {{"fwd", "e"}});   // the user's own
        PreLaunchWindow W(&Cfg, &idx, {"game"});
        W.resize(900, 700);
        W.show();
        QCoreApplication::processEvents();
        auto *T = W.findChild<QTreeWidget *>("optionsTree");
        QVERIFY(T);
        for (QTreeWidgetItemIterator It(T); *It; ++It) (*It)->setExpanded(true);
        QCoreApplication::processEvents();
        const auto field = [&](const QString &Label) -> KeyCaptureField * {
            for (auto *F : T->findChildren<KeyCaptureField *>()) if (F->property("CVLabel").toString() == Label) return F;
            return nullptr;
        };
        KeyCaptureField *Fwd = field("Move forward"), *Jump = field("Jump"), *Menu = field("Menu");
        QVERIFY(Fwd && Jump && Menu);
        QCOMPARE(Fwd->value(), std::string("e"));                          // the saved binding, its default named
        QCOMPARE(Fwd->button()->text(), QString("E  (default W)"));
        QTreeWidgetItem *OnlyQ = nullptr;
        for (QTreeWidgetItemIterator It(T); *It; ++It) if ((*It)->text(0) == "Only when jump is Q") OnlyQ = *It;
        QVERIFY(OnlyQ && OnlyQ->isHidden());

        //Click, press I: bound.
        QTest::mouseClick(Fwd->button(), Qt::LeftButton);
        QVERIFY(Fwd->capturing());
        QCOMPARE(Fwd->button()->text(), QString("Press a key…"));
        QTest::keyClick(Fwd->button(), Qt::Key_I);
        QVERIFY(!Fwd->capturing());
        QCOMPARE(Fwd->value(), std::string("i"));
        QCOMPARE(Fwd->button()->text(), QString("I  (default W)"));      // off its default: the default is named
        QVERIFY(Fwd->resetButton()->isEnabled());
        QVERIFY(!Menu->resetButton()->isEnabled());                        // on its default: nothing to reset

        //The bound key is what a WHEN reads.
        QTest::mouseClick(Jump->button(), Qt::LeftButton);
        QTest::keyClick(Jump->button(), Qt::Key_Q);
        QCOMPARE(Jump->value(), std::string("q"));
        QVERIFY(!OnlyQ->isHidden());

        //Right shift, as X11/Wayland report it (keycode 62, keysym Shift_R): a modifier binds once released alone.
        QTest::mouseClick(Menu->button(), Qt::LeftButton);
        QKeyEvent RShift(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier, 62, 0xffe2, 0);
        QCoreApplication::sendEvent(Menu->button(), &RShift);
        QVERIFY(Menu->capturing());
        QKeyEvent RShiftUp(QEvent::KeyRelease, Qt::Key_Shift, Qt::NoModifier, 62, 0xffe2, 0);
        QCoreApplication::sendEvent(Menu->button(), &RShiftUp);
        QCOMPARE(Menu->value(), std::string("right shift"));

        //Escape is a key like any other (a game's menu key); keypad keys are their own.
        QTest::mouseClick(Menu->button(), Qt::LeftButton);
        QTest::keyClick(Menu->button(), Qt::Key_Escape);
        QCOMPARE(Menu->value(), std::string("escape"));
        QTest::mouseClick(Menu->button(), Qt::LeftButton);
        QTest::keyClick(Menu->button(), Qt::Key_5, Qt::KeypadModifier);
        QCOMPARE(Menu->value(), std::string("keypad 5"));

        //A mouse button over the capturing button binds it.
        QTest::mouseClick(Menu->button(), Qt::LeftButton);
        QTest::mouseClick(Menu->button(), Qt::RightButton);
        QCOMPARE(Menu->value(), std::string("mouse right"));
        QVERIFY(!Menu->capturing());

        //A turn of the wheel binds by its direction; a horizontal tilt alone keeps waiting.
        const auto wheel = [&](QPoint Angle) {
            QWheelEvent Turn(QPointF(5, 5), Menu->button()->mapToGlobal(QPointF(5, 5)), QPoint(), Angle, Qt::NoButton,
                             Qt::NoModifier, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(Menu->button(), &Turn);
        };
        QTest::mouseClick(Menu->button(), Qt::LeftButton);
        wheel(QPoint(120, 0));
        QVERIFY(Menu->capturing());
        wheel(QPoint(0, -120));
        QCOMPARE(Menu->value(), std::string("mouse wheel down"));
        QTest::mouseClick(Menu->button(), Qt::LeftButton);
        wheel(QPoint(0, 120));
        QCOMPARE(Menu->value(), std::string("mouse wheel up"));
        QVERIFY(!Menu->capturing());
        QTest::mouseClick(Menu->button(), Qt::LeftButton);
        QTest::mouseClick(Menu->button(), Qt::RightButton);

        //Focus leaving cancels and keeps the binding.
        QTest::mouseClick(Menu->button(), Qt::LeftButton);
        QVERIFY(Menu->capturing());
        QFocusEvent Out(QEvent::FocusOut, Qt::OtherFocusReason);
        QCoreApplication::sendEvent(Menu->button(), &Out);
        QVERIFY(!Menu->capturing());
        QCOMPARE(Menu->value(), std::string("mouse right"));

        //Two options of the section on one key: both marked, each naming the other; unbinding one clears both.
        QTest::mouseClick(Jump->button(), Qt::LeftButton);
        QTest::keyClick(Jump->button(), Qt::Key_I);
        QVERIFY2(Fwd->button()->toolTip().contains("Jump"), qPrintable(Fwd->button()->toolTip()));
        QVERIFY2(Jump->button()->toolTip().contains("Move forward"), qPrintable(Jump->button()->toolTip()));
        QVERIFY(!Menu->button()->toolTip().contains("Also bound"));
        QTest::mouseClick(Jump->clearButton(), Qt::LeftButton);
        QCOMPARE(Jump->value(), std::string(""));
        QCOMPARE(Jump->button()->text(), QString("(none)  (default Space)"));
        QVERIFY(!Fwd->button()->toolTip().contains("Also bound"));

        //↺ puts the default back.
        QTest::mouseClick(Jump->resetButton(), Qt::LeftButton);
        QCOMPARE(Jump->value(), std::string("space"));
        QCOMPARE(Jump->button()->text(), QString("Space"));
        QVERIFY(!Jump->resetButton()->isEnabled());
    }

    // A key pressed while modifiers are held binds the combination ("shift+1": Shift+1 arrives as '!' with Shift held;
    // "ctrl+alt+a"), shown "Ctrl+Alt+A". The modifier presses before it bind nothing. A mouse button and the wheel take
    // modifiers too. A modifier released with nothing else pressed binds by itself, with its side; one released that was
    // held before the capture began binds nothing (the Ctrl of a Ctrl+click elsewhere). Teeth: bind a modifier on its
    // press (Shift alone binds before the 1); drop the modifiers from the key (the "1" binds); bind any modifier's release
    // (the stale Ctrl binds); drop them from the mouse (the "mouse left" binds).
    void aKeyHeldWithModifiersBindsTheCombination()
    {
        KeyCaptureField F("w", "w");
        F.show();
        const auto send = [&](QEvent::Type T, int Key, Qt::KeyboardModifiers M, quint32 Sym = 0) {
            QKeyEvent E(T, Key, M, 0, Sym, 0);
            QCoreApplication::sendEvent(F.button(), &E);
        };
        QTest::mouseClick(F.button(), Qt::LeftButton);
        send(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier, 0xffe1);
        QVERIFY(F.capturing());
        send(QEvent::KeyPress, Qt::Key_Exclam, Qt::ShiftModifier);
        QCOMPARE(F.value(), std::string("shift+1"));
        QVERIFY(!F.capturing());
        send(QEvent::KeyRelease, Qt::Key_Shift, Qt::NoModifier, 0xffe1);    // after the bind: nothing
        QCOMPARE(F.value(), std::string("shift+1"));

        QTest::mouseClick(F.button(), Qt::LeftButton);
        send(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier, 0xffe3);
        send(QEvent::KeyPress, Qt::Key_Alt, Qt::ControlModifier | Qt::AltModifier, 0xffe9);
        send(QEvent::KeyPress, Qt::Key_A, Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(F.value(), std::string("ctrl+alt+a"));
        QCOMPARE(F.button()->text(), QString("Ctrl+Alt+A  (default W)"));

        QTest::mouseClick(F.button(), Qt::LeftButton);
        QTest::mouseClick(F.button(), Qt::LeftButton, Qt::ShiftModifier);
        QCOMPARE(F.value(), std::string("shift+mouse left"));

        QTest::mouseClick(F.button(), Qt::LeftButton);
        send(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier, 0xffe3);  // held since before: binds nothing
        QVERIFY(F.capturing());
        send(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier, 0xffe4);
        send(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier, 0xffe4);
        QCOMPARE(F.value(), std::string("right ctrl"));
    }

    // A gamepad button binds a key option too: SDL's game-controller events, read while the button waits. A button
    // already held when the capture starts binds nothing (else whatever the player rests a thumb on wins), nor does one
    // pressed while the window sat idle; a trigger binds past half; once bound, presses no longer reach the option. Driven by an SDL virtual controller.
    // Sticks bind by direction; an axis already pushed when a capture starts must return to the centre first.
    // Teeth: skip Begin's flush (the A pressed while idle binds), drop the trigger branch, keep polling after the bind,
    // forget the held axes (the wiggle binds), name directions by the wrong sign.
    void aGamepadButtonBindsAKeyOption()
    {
        QVERIFY2(SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0, SDL_GetError());
        const int Idx = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 15, 0);
        QVERIFY2(Idx >= 0, SDL_GetError());
        char Guid[64];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(Idx), Guid, sizeof Guid);
        QVERIFY(SDL_GameControllerAddMapping((std::string(Guid) + ",VidyaGod test pad,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,"
            "start:b6,leftstick:b7,rightstick:b8,leftshoulder:b9,rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,"
            "dpright:b14,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,").c_str()) >= 0);
        SDL_Joystick *Pad = SDL_JoystickOpen(Idx);
        QVERIFY2(Pad, SDL_GetError());

        KeyCaptureField F("w", "w");
        F.show();
        SDL_JoystickSetVirtualButton(Pad, 1, SDL_PRESSED);                 // B, held before the capture starts
        QTest::mouseClick(F.button(), Qt::LeftButton);
        QVERIFY(F.capturing());
        QTest::qWait(100);
        QVERIFY(F.capturing());
        QCOMPARE(F.value(), std::string("w"));
        SDL_JoystickSetVirtualButton(Pad, 2, SDL_PRESSED);                 // X, pressed now
        QTRY_COMPARE(F.value(), std::string("pad x"));
        QVERIFY(!F.capturing());

        SDL_JoystickSetVirtualButton(Pad, 1, SDL_RELEASED);
        SDL_JoystickSetVirtualButton(Pad, 2, SDL_RELEASED);
        QTest::mouseClick(F.button(), Qt::LeftButton);
        SDL_JoystickSetVirtualAxis(Pad, 5, SDL_JOYSTICK_AXIS_MAX);         // right trigger, fully in
        QTRY_COMPARE(F.value(), std::string("pad right trigger"));

        SDL_JoystickSetVirtualButton(Pad, 0, SDL_PRESSED);                 // A, after the bind: ignored
        QTest::qWait(100);
        QCOMPARE(F.value(), std::string("pad right trigger"));

        //That press is still queued in SDL; the next capture must not take it as its answer.
        QTest::mouseClick(F.button(), Qt::LeftButton);
        QTest::qWait(100);
        QVERIFY(F.capturing());
        QCOMPARE(F.value(), std::string("pad right trigger"));
        SDL_JoystickSetVirtualButton(Pad, 3, SDL_PRESSED);                 // Y
        QTRY_COMPARE(F.value(), std::string("pad y"));

        //A stick direction binds past half (SDL's Y grows downward: negative is up).
        SDL_JoystickSetVirtualAxis(Pad, 5, 0);                             // the trigger let go
        QTest::qWait(50);
        QTest::mouseClick(F.button(), Qt::LeftButton);
        SDL_JoystickSetVirtualAxis(Pad, 1, SDL_JOYSTICK_AXIS_MIN);         // left stick up
        QTRY_COMPARE(F.value(), std::string("pad left stick up"));

        //Still held when the next capture starts: its wiggle binds nothing until it comes back to the centre.
        QTest::mouseClick(F.button(), Qt::LeftButton);
        SDL_JoystickSetVirtualAxis(Pad, 1, SDL_JOYSTICK_AXIS_MIN + 200);
        QTest::qWait(100);
        QVERIFY(F.capturing());
        SDL_JoystickSetVirtualAxis(Pad, 1, 0);
        QTest::qWait(50);
        SDL_JoystickSetVirtualAxis(Pad, 1, SDL_JOYSTICK_AXIS_MAX);         // left stick down
        QTRY_COMPARE(F.value(), std::string("pad left stick down"));
        SDL_JoystickSetVirtualAxis(Pad, 1, 0);
        QTest::qWait(50);
        QTest::mouseClick(F.button(), Qt::LeftButton);
        SDL_JoystickSetVirtualAxis(Pad, 2, SDL_JOYSTICK_AXIS_MIN);         // right stick left
        QTRY_COMPARE(F.value(), std::string("pad right stick left"));

        SDL_JoystickClose(Pad);
        SDL_JoystickDetachVirtual(Idx);
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    }

    // An option's WHEN may read a fact of the package: a var with no control (which game this is). The window judged
    // WHEN from its controls alone, so the fact read as empty and the option hid even where it applies (Horn of the
    // Abyss's health-bar option, on its own card). Teeth: evaluate from the controls only (the "hota" row hides).
    void anOptionsWhenReadsThePackagesFixedFacts()
    {
        const auto window = [](const char *Game) {
            auto idx = std::make_unique<NodeIndex>();
            idx->Nodes["game"] = parse(json{ {"CID", "game"}, {"LABEL", "game"}, {"VARIANT", "v1"},
                {"LAYERS", json::array({ json{{"DIR", "game"}},
                    json{{"VARS", {
                        {"which", {{"DEFAULT", Game}}},
                        {"bars", {{"DEFAULT", "1"}, {"WHEN", "%which%==hota"}, {"UI", {{"CONTROL", "bool"}, {"LABEL", "Health bars"}}}}}}}},
                    json{{"EXEC", json::array({ json{{"LABEL", "Play"}, {"HOST", ManifestModel::MachinePlatform()}, {"EXE", "game.exe"},
                        {"TILE", {{"UID", "37"}, {"TITLE", "Game"}}}} })}} })} });
            ManifestModel::DeriveFacts(*idx);
            return idx;
        };
        for (const char *Game : {"hota", "complete"})
        {
            auto idx = window(Game);
            json Cfg = json{{"Settings", json::object()}};
            PreLaunchWindow W(&Cfg, idx.get(), {"game"});
            W.show();
            QCoreApplication::processEvents();
            auto *T = W.findChild<QTreeWidget *>("optionsTree");
            QVERIFY(T);
            QTreeWidgetItem *Bars = nullptr;
            for (QTreeWidgetItemIterator It(T); *It; ++It) if ((*It)->text(0) == "Health bars") Bars = *It;
            QVERIFY(Bars);
            QCOMPARE(Bars->isHidden(), std::string(Game) != "hota");
        }
    }

    // A tile has one launch window: its card's options opened twice raise the one open (two windows could start two
    // launches of one game). Teeth: a new window per click.
    void aTileHasOneLaunchWindow()
    {
        NodeIndex idx;
        idx.Nodes["game"] = parse(json{ {"CID", "game"}, {"LABEL", "game"}, {"VARIANT", "v1"},
            {"LAYERS", json::array({ json{{"DIR", "game"}},
                json{{"EXEC", json::array({ json{{"LABEL", "Play"}, {"HOST", ManifestModel::MachinePlatform()}, {"EXE", "game.exe"},
                    {"TILE", {{"UID", "47"}, {"TITLE", "Game"}}}} })}} })} });
        ManifestModel::DeriveFacts(idx);
        json Cfg = json{{"Settings", json::object()}};
        LibraryGameCard Card(&Cfg, &idx, { "game" }, "47");
        Card.edit();
        Card.edit();
        QCoreApplication::processEvents();
        int Open = 0;
        for (QWidget *W : QApplication::topLevelWidgets()) if (qobject_cast<PreLaunchWindow *>(W) && W->isVisible()) ++Open;
        QCOMPARE(Open, 1);
        for (QWidget *W : QApplication::topLevelWidgets()) if (auto *P = qobject_cast<PreLaunchWindow *>(W)) P->close();
        QCoreApplication::processEvents();
    }

    // "Skip this window next time" shows what the instance has saved, so it can be unticked (it always showed unticked
    // and nothing wrote false: once ticked, the window never came back). Teeth: never load it.
    void skipThisWindowShowsTheSavedSetting()
    {
        NodeIndex idx;
        idx.Nodes["game"] = parse(json{ {"CID", "game"}, {"LABEL", "game"}, {"VARIANT", "v1"},
            {"LAYERS", json::array({ json{{"DIR", "game"}},
                json{{"EXEC", json::array({ json{{"LABEL", "Play"}, {"HOST", ManifestModel::MachinePlatform()}, {"EXE", "game.exe"},
                    {"TILE", {{"UID", "46"}, {"TITLE", "Game"}}}} })}} })} });
        ManifestModel::DeriveFacts(idx);
        json Cfg = json{{"Settings", json::object()}};
        QVERIFY(InstanceStore::WriteConfig(Cfg, idx.Find("game")->GameKey(), "Main", json{{"SKIP_LAUNCH_DIALOG", true}, {"LASTRUN", "2026-09-30T10:00:00Z"}}));
        PreLaunchWindow W(&Cfg, &idx, {"game"}, "46");
        W.show();
        QCoreApplication::processEvents();
        QCheckBox *Skip = nullptr;
        for (QCheckBox *C : W.findChildren<QCheckBox *>()) if (C->text().startsWith("Skip this window")) Skip = C;
        QVERIFY(Skip);
        QVERIFY(Skip->isChecked());
    }

    // Closing the launch window while its launch runs (Esc, the X, "close once started") hides it and the game goes on:
    // the window was deleted and its destructor stopped the game. Its card finds it again (one window per tile), and the
    // launch ending closes it — or brings it back when the launch failed. Teeth: close for real while running (deleted);
    // a second window per tile; stay hidden after a failure.
    void closingTheWindowWhileItsGameRunsHidesIt()
    {
        NodeIndex idx;
        idx.Nodes["game"] = parse(json{ {"CID", "game"}, {"LABEL", "game"}, {"VARIANT", "v1"},
            {"LAYERS", json::array({ json{{"DIR", "game"}},
                json{{"EXEC", json::array({ json{{"LABEL", "Play"}, {"HOST", ManifestModel::MachinePlatform()}, {"EXE", "game.exe"},
                    {"TILE", {{"UID", "45"}, {"TITLE", "Game"}}}} })}} })} });
        ManifestModel::DeriveFacts(idx);
        json Cfg = json{{"Settings", json::object()}};
        QPointer<PreLaunchWindow> W = new PreLaunchWindow(&Cfg, &idx, {"game"}, "45");
        W->show();
        QCoreApplication::processEvents();
        QCOMPARE(PreLaunchWindow::ExistingFor("45", {"game"}), W.data());
        W->runningForTest(true);
        W->reject();                                                       // Esc
        QCoreApplication::processEvents();
        QVERIFY(W && !W->isVisible());
        W->bringBack();
        QVERIFY(W->isVisible());
        W->close();                                                        // the title bar's X
        QCoreApplication::processEvents();
        QVERIFY(W && !W->isVisible());
        W->runningForTest(false);
        QMetaObject::invokeMethod(W, "onLaunchFinished", Q_ARG(bool, false), Q_ARG(QString, "it broke"));
        QCoreApplication::processEvents();
        QVERIFY(W && W->isVisible());                                      // a failure is shown
        W->runningForTest(true);
        W->close();
        W->runningForTest(false);
        QMetaObject::invokeMethod(W, "onLaunchFinished", Q_ARG(bool, true), Q_ARG(QString, ""));
        QTRY_VERIFY(!W);                                                   // ended well: closed (and deleted)
        QVERIFY(!PreLaunchWindow::ExistingFor("45", {"game"}));
        // Ended "well" but logged an error while hidden: shown, since the log is the only place it is said.
        QPointer<PreLaunchWindow> W2 = new PreLaunchWindow(&Cfg, &idx, {"game"}, "45");
        W2->show();
        W2->runningForTest(true);
        W2->close();
        QCoreApplication::processEvents();
        QVERIFY(W2 && !W2->isVisible());
        QMetaObject::invokeMethod(W2, "onLogLine", Q_ARG(int, 4), Q_ARG(QString, "Teardown"), Q_ARG(QString, "a save was not kept"));
        W2->runningForTest(false);
        QMetaObject::invokeMethod(W2, "onLaunchFinished", Q_ARG(bool, true), Q_ARG(QString, ""));
        QCoreApplication::processEvents();
        QVERIFY(W2 && W2->isVisible());
        W2->close();
        QTRY_VERIFY(!W2);
    }

    // An option declared in a layer behind a WHEN shows as the launch would have it: the window folds the game with the
    // instance's saved values, as the launch does (it folded with none: the layer never held, the option never showed).
    // Teeth: fold without the saved values (the "pro" instance shows no Pro option).
    void anOptionBehindALayersWhenShowsWhereTheLaunchHasIt()
    {
        NodeIndex idx;
        idx.Nodes["game"] = parse(json{ {"CID", "game"}, {"LABEL", "game"}, {"VARIANT", "v1"},
            {"LAYERS", json::array({ json{{"DIR", "game"}},
                json{{"VARS", {{"mode", {{"DEFAULT", "basic"}, {"UI", {{"CONTROL", "text"}, {"LABEL", "Mode"}}}}}}}},
                json{{"WHEN", "%mode%==pro"}, {"VARS", {{"proopt", {{"DEFAULT", "1"}, {"UI", {{"CONTROL", "bool"}, {"LABEL", "Pro option"}}}}}}}},
                json{{"EXEC", json::array({ json{{"LABEL", "Play"}, {"HOST", ManifestModel::MachinePlatform()}, {"EXE", "game.exe"},
                    {"TILE", {{"UID", "43"}, {"TITLE", "Game"}}}} })}} })} });
        ManifestModel::DeriveFacts(idx);
        const std::string Uid = idx.Find("game")->GameKey();
        for (const char *Mode : {"pro", "basic"})
        {
            json Cfg = json{{"Settings", json::object()}};
            QVERIFY(InstanceStore::WriteConfig(Cfg, Uid, "Main", json{{"VARIABLES", {{"mode", Mode}}}, {"LASTRUN", "2026-09-30T10:00:00Z"}}));
            PreLaunchWindow W(&Cfg, &idx, {"game"});
            W.show();
            QCoreApplication::processEvents();
            auto *T = W.findChild<QTreeWidget *>("optionsTree");
            QVERIFY(T);
            bool Pro = false;
            for (QTreeWidgetItemIterator It(T); *It; ++It) if ((*It)->text(0) == "Pro option" && !(*It)->isHidden()) Pro = true;
            QCOMPARE(Pro, std::string(Mode) == "pro");
        }
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
        const json US = InstanceStore::ReadConfig(Cfg, idx.Find("game")->PackageUid);
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
            const json US = InstanceStore::ReadConfig(Cfg, idx.Find("game")->PackageUid);
            std::vector<std::string> G;
            if (US.contains("GRAFTS") && US["GRAFTS"].contains("77")) for (const auto &X : US["GRAFTS"]["77"]) G.push_back(X.get<std::string>());
            return G;
        };
        tick("Base");
        tick("Needs base");                                   // offered once Base is ticked
        tick("Aaa");
        const std::vector<std::string> S = saved();
        QVERIFY2(S.size() == 3, "all three ticked");
        const auto A = Shelf::AppliedGrafts(idx, "game", S);
        QVERIFY2(std::find(A.begin(), A.end(), "x") != A.end(), "the graft that needs Base is applied, not dropped");
    }

    // A game with one version installed and another not (JSRF on a machine that took a peer's package: its Xbox
    // version plays, its Unleashed version's files were never fetched): the card holds the installed one and names the
    // other as not installed; the launch window lists both — the installed one first and picked, though the other is
    // recommended — and, on the one not installed, Play becomes Download: it asks the app's download pump for that
    // version under the tile's key, says "Downloading…" while it runs, and once the library has it the row plays.
    // Teeth: drop the not-installed rows from the card (or the window); sort them by recommendation alone (the window
    // opens on a version it cannot play); let Play launch a row that is not installed; leave the button as Download on
    // an installed row; ignore an in-flight download; leave an open window on the rows it opened with, or on a
    // version no longer offered.
    void a_version_not_installed_is_listed_and_downloads_from_the_window()
    {
        const auto Version = [](const char *Cid, const char *Variant, bool Rec) {
            json J = { {"CID", Cid}, {"LABEL", Variant}, {"VARIANT", Variant},
                {"LAYERS", json::array({ json{{"EXEC", json::array({ json{{"LABEL", "Play"}, {"HOST", ManifestModel::MachinePlatform()},
                    {"EXE", "game.exe"}, {"TILE", {{"UID", "31"}, {"TITLE", "Jet Set Radio"}}}} })}} })} };
            if (Rec) J["RECOMMENDED"] = json::array({"31"});
            Node N;
            ManifestModel::ParseNode(J, std::string("/lib/JSRF/") + Variant + ".json", "/lib/JSRF", N);
            return N;
        };
        auto Idx = std::make_shared<NodeIndex>();
        Idx->Nodes["bafkreiunleashed"] = Version("bafkreiunleashed", "JSRF Unleashed", true);
        Idx->Nodes["bafkreixbox"] = Version("bafkreixbox", "Xbox", false);
        ManifestModel::DeriveFacts(*Idx);

        // The card's rows: installed (its own file here), and not installed but fetchable (it names a CID).
        CardPool::HydrationMap Hyd;
        Hyd["bafkreixbox"] = { /*Hydrated*/ true, /*HasContent*/ true, /*OnDisk*/ true, /*HasCid*/ true, false };
        Hyd["bafkreiunleashed"] = { false, true, true, true, false };
        const auto R = CardPool::LibraryRows(*Idx, Hyd, "31");
        QVERIFY(R);
        QCOMPARE(R->Here, (std::vector<std::string>{"bafkreixbox"}));
        QCOMPARE(R->ToFetch, (std::vector<std::string>{"bafkreiunleashed"}));
        QVERIFY(!CardPool::LibraryRows(*Idx, Hyd, "32"));                     // no such tile
        {
            CardPool::HydrationMap None = Hyd;
            None["bafkreixbox"].Hydrated = false;                              // nothing of it installed: the Catalog's
            QVERIFY(!CardPool::LibraryRows(*Idx, None, "31"));
            CardPool::HydrationMap NoSource = Hyd;
            NoSource["bafkreiunleashed"].HasCid = false;                       // nothing to fetch it from: not offered
            QCOMPARE(CardPool::LibraryRows(*Idx, NoSource, "31")->ToFetch, std::vector<std::string>{});
        }

        // The app's download pump, as the main window sets it: what was asked, and whether it runs.
        QString AskedKey;
        std::vector<std::string> AskedIds;
        bool Running = false;
        PreLaunchWindow::SetVersionDownloads({
            [&](QWidget *, const QString &Key, const std::vector<std::string> &Ids) { AskedKey = Key; AskedIds = Ids; Running = true; return true; },
            [&](const QString &) { return Running; } });

        json Cfg = json{{"Settings", json::object()}};
        {
            PreLaunchWindow W(&Cfg, std::shared_ptr<const NodeIndex>(Idx), R->Here, "31");
            W.setNotInstalled(R->ToFetch);
            W.show();
            QCoreApplication::processEvents();
            QComboBox *V = nullptr;
            for (QComboBox *C : W.findChildren<QComboBox *>())
                if (C->count() && C->itemData(0).toString().contains(QChar(0x1f))) V = C;
            auto *Play = W.findChild<QPushButton *>("playButton");
            QVERIFY(V && Play);
            QStringList Rows;
            for (int i = 0; i < V->count(); ++i) Rows << V->itemText(i);
            QCOMPARE(Rows, QStringList({"Xbox", "⭐ JSRF Unleashed  (not installed)"}));
            QVERIFY(V->isVisibleTo(&W));                                        // two versions: the picker shows
            QCOMPARE(V->currentIndex(), 0);
            QVERIFY(!W.rowNotInstalled());
            QVERIFY2(Play->text().contains("Play"), qPrintable(Play->text()));

            V->setCurrentIndex(1);                                              // the version not installed
            QCoreApplication::processEvents();
            QVERIFY(W.rowNotInstalled());
            QVERIFY2(Play->text().contains("Download"), qPrintable(Play->text()));
            QVERIFY(Play->isEnabled());
            QTest::mouseClick(Play, Qt::LeftButton);
            QCOMPARE(AskedKey, QString("31"));
            QCOMPARE(AskedIds, (std::vector<std::string>{"bafkreiunleashed"}));
            QVERIFY2(Play->text().contains("Downloading"), qPrintable(Play->text()));
            QVERIFY(!Play->isEnabled());                                        // one download at a time

            V->setCurrentIndex(0);                                              // back on the installed one: it plays
            QCoreApplication::processEvents();
            QVERIFY2(Play->text().contains("Play"), qPrintable(Play->text()));
            V->setCurrentIndex(1);
            QCoreApplication::processEvents();

            // The download ends and the library has the version: the open window follows, on the row it had picked.
            Running = false;
            PreLaunchWindow::DownloadsChanged();
            QVERIFY2(Play->text().contains("Download") && Play->isEnabled(), qPrintable(Play->text()));
            PreLaunchWindow::FollowLibrary(Idx, [](const std::string &Face, std::vector<std::string> &Here, std::vector<std::string> &ToFetch) {
                if (Face != "31") return false;
                Here = {"bafkreiunleashed", "bafkreixbox"};
                ToFetch.clear();
                return true;
            });
            QCoreApplication::processEvents();
            Rows.clear();
            for (int i = 0; i < V->count(); ++i) Rows << V->itemText(i);
            QCOMPARE(Rows, QStringList({"⭐ JSRF Unleashed", "Xbox"}));
            QCOMPARE(V->currentText(), QString("⭐ JSRF Unleashed"));
            QVERIFY(!W.rowNotInstalled());
            QVERIFY2(Play->text().contains("Play"), qPrintable(Play->text()));
        }
        // A version that stops being offered without being installed (its source gone): the window no longer lists it.
        {
            PreLaunchWindow W(&Cfg, std::shared_ptr<const NodeIndex>(Idx), {"bafkreixbox"}, "31");
            W.setNotInstalled({"bafkreiunleashed"});
            PreLaunchWindow::FollowLibrary(Idx, [](const std::string &, std::vector<std::string> &Here, std::vector<std::string> &ToFetch) {
                Here = {"bafkreixbox"};
                ToFetch.clear();
                return true;
            });
            QStringList Rows;
            for (QComboBox *C : W.findChildren<QComboBox *>())
                if (C->count() && C->itemData(0).toString().contains(QChar(0x1f)))
                    for (int i = 0; i < C->count(); ++i) Rows << C->itemText(i);
            QCOMPARE(Rows, QStringList({"Xbox"}));
        }
        PreLaunchWindow::SetVersionDownloads({});
    }

private:
    QTemporaryDir *SuiteDataRoot = nullptr;
};

QTEST_MAIN(PreLaunchTest)
#include "test_prelaunch.moc"
