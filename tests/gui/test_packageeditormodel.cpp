// PackageEditorModel — the package editor's state: the package as a PkgDoc::Document, saving it (a generation-6
// re-mint that follows renamed nodes through the library), validating it as edited, this machine's layout for it, and
// what the rest of the library says about the nodes it names. Authoring runs (RunInNode) raise dialogs and build live
// containers, so they are out of scope here. A real temp data root holds a small library.

#include <QtTest>
#include "qtestjson.h"
#include "apppaths.h"
#include "packageeditor.h"
#include "packageeditormodel.h"
#include "cid.h"
#include "pkginsight.h"
#include "validationpanel.h"
#include "pkggraph.h"
#include "instancestore.h"

#include <QDir>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QLabel>
#include <QListWidget>

#include <algorithm>
#include <fstream>
#include <string>

using json = nlohmann::ordered_json;

namespace {
std::string Freeze(const QString &Dir, const json &N)
{
    const std::string C = Cid::OfNode(N);
    std::ofstream((Dir + "/" + QString::fromStdString(C) + ".json").toStdString(), std::ios::binary) << Cid::Canonical(N);
    return C;
}
json Zip(const std::string &Label, const std::string &File) { return json{{"LABEL", Label}, {"LAYERS", json::array({ json{{"ZIP", File}, {"TARGET", "FILES/game"}} })}}; }
json Over(const std::string &Label, const std::string &Ref) { return json{{"LABEL", Label}, {"LAYERS", json::array({ json{{"NODE", Ref}}, json{{"ENV", {{"A", "1"}}}} })}}; }
int NodeFiles(const QString &Dir) { return QDir(Dir).entryList({"baf*.json"}, QDir::Files).size(); }
}

class PackageEditorModelTest : public QObject
{
    Q_OBJECT
    QTemporaryDir *Root = nullptr;
    json Cfg = json{{"Settings", json::object()}};
    QString Lib, Pkg, Other;
    std::string Fonts, Game;

    //A library of two packages: "fonts" (one node) and the package under edit, "game", whose node contains it.
    void library()
    {
        QDir(Lib).removeRecursively();
        QDir().mkpath(Pkg); QDir().mkpath(Other);
        Fonts = Freeze(Other, Zip("Core Fonts", "fonts.zip"));
        Game = Freeze(Pkg, Over("Game", Fonts));
    }

private slots:
    //Claim a data root for the whole binary: SaveLayout flushes GlobalConfig.JSON to it, and without one that is the
    //developer's real ~/.VidyaGod.
    void initTestCase()
    {
        Root = new QTemporaryDir();
        QVERIFY(Root->isValid());
        AppPaths::SetDataRoot(Root->path().toStdString());
        Lib = Root->path() + "/LIBRARY";
        Pkg = Lib + "/Vendor/[1][v1] Game";
        Other = Lib + "/Vendor/[2][v1] Fonts";
    }
    void cleanupTestCase() { delete Root; Root = nullptr; }

    // A published package loads named by its files, and another package's node it names is known by the library:
    // its label and package (what the canvas's chip shows) and every other package's node is on offer for wiring in.
    void aPackageLoadsAndKnowsTheLibrary()
    {
        library();
        std::ofstream((Pkg + "/notes.json").toStdString()) << R"(["not a node"])";
        PackageEditorModel M(&Cfg, nullptr);
        M.initPackage(Pkg);
        QCOMPARE(M.doc().Count(), 1);
        QCOMPARE(M.doc().Handle(0), Game);
        QVERIFY(!M.isDirty());
        const auto E = M.externalInfo(Fonts);
        QCOMPARE(E.Label, std::string("Core Fonts"));
        QCOMPARE(E.Package, std::string("Fonts"));        // the folder name without its [tags]
        bool Offered = false, OwnOffered = false;
        for (const auto &O : M.offers()) { if (O.Cid == Fonts) Offered = true; if (O.Cid == Game) OwnOffered = true; }
        QVERIFY(Offered);
        QVERIFY(!OwnOffered);                               // this package's own nodes are not "another package's"
        //A package made (or saved) since, in this same editor, is on offer once another package is opened: the
        //library is read again on every load. Teeth: keep the library read at first use.
        const QString Third = Lib + "/Vendor/[3][v1] Mod";
        QDir().mkpath(Third);
        const std::string Mod = Freeze(Third, Zip("A Mod", "mod.zip"));
        M.initPackage(Pkg);
        bool ModOffered = false;
        for (const auto &O : M.offers()) if (O.Cid == Mod) ModOffered = true;
        QVERIFY(ModOffered);
        QDir(Third).removeRecursively();
    }

    // Save re-mints: the edited node gets the CID of its new bytes, the old file goes, the renames are announced
    // (the canvas and the JSON panel follow them) and the package is clean. Teeth: write back under the old name.
    void savingAnEditRenamesTheNodeAndSaysSo()
    {
        library();
        PackageEditorModel M(&Cfg, nullptr);
        M.initPackage(Pkg);
        QSignalSpy Renamed(&M, &PackageEditorModel::handlesRenamed), Saved(&M, &PackageEditorModel::savedToDisk),
                   Dirty(&M, &PackageEditorModel::dirtyChanged);
        json N = M.doc().Node(0);
        N["LABEL"] = "Game v2";
        M.replaceNode(Game, N);
        QVERIFY(M.isDirty());
        QCOMPARE(Dirty.count(), 1);
        QString Err;
        QVERIFY2(M.Save(&Err), qPrintable(Err));
        QVERIFY(!M.isDirty());
        QCOMPARE(Renamed.count(), 1);
        QCOMPARE(Saved.count(), 1);
        const std::string New = M.lastRenames().at(Game);
        QCOMPARE(M.doc().Handle(0), New);
        QVERIFY(!QFile::exists(Pkg + "/" + QString::fromStdString(Game) + ".json"));
        QVERIFY(QFile::exists(Pkg + "/" + QString::fromStdString(New) + ".json"));
        QCOMPARE(NodeFiles(Pkg), 1);
    }

    // Validation reads the package AS EDITED: an unsaved edit that breaks a reference is flagged, on the node it is
    // about (by handle). Teeth: validate what is on disk (the break is invisible until saved).
    void validationSeesUnsavedEdits()
    {
        library();
        PackageEditorModel M(&Cfg, nullptr);
        M.initPackage(Pkg);
        M.Revalidate();
        QVERIFY2(M.validationErrors().empty(), M.validationErrors().empty() ? "" : M.validationErrors().front().c_str());
        json N = M.doc().Node(0);
        N["LAYERS"][0]["NODE"] = "bafkreinosuchnodeanywherexxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
        N["LAYERS"].push_back(json{{"ZIP", "a.zip"}, {"DIR", "b"}});   // a layer with two types: an error
        M.replaceNode(Game, N);
        M.Revalidate();
        QVERIFY(!M.validationErrors().empty());
        const auto Issues = M.issuesByHandle();
        QVERIFY2(Issues.count(Game), "the problem is not attached to its node");
    }

    // The validation panel says problems as a person reads them: the node by its label (the CID in the tooltip), and
    // a layer only waiting for its files in grey, "waiting", counted as no error. Teeth: show the raw message; count
    // the waiting layer as an error.
    void theValidationPanelSpeaksPlainly()
    {
        library();
        PackageEditorModel M(&Cfg, nullptr);
        M.initPackage(Pkg);
        json N = M.doc().Node(0);
        N["LAYERS"].push_back(json{{"ZIP", ""}, {"TARGET", "FILES/game"}});
        N["LABEL"] = "S.T.A.L.K.E.R.: Game (GOG)";               // a title with a colon and parentheses in it
        M.replaceNode(Game, N);
        M.Revalidate();
        ValidationPanel P(&M);
        QVERIFY2(P.title().contains("waiting for the game's files"), qUtf8Printable(P.title()));
        QListWidget *L = P.findChild<QListWidget *>();
        QVERIFY(L && L->count() == 1);
        QVERIFY2(L->item(0)->text().startsWith("S.T.A.L.K.E.R.: Game (GOG): waiting for its files"), qUtf8Printable(L->item(0)->text()));
        QVERIFY(L->item(0)->toolTip().contains("node '"));
        QCOMPARE(ValidationPanel::readable("node 'bafkreiabc' (TUNIC): layer 0 broke"), QString("TUNIC: layer 0 broke"));
        QCOMPARE(ValidationPanel::readable("node 'bafkreiabc' (Original (IWD2)): layer 0 (a.zip): broke"), QString("Original (IWD2): layer 0 (a.zip): broke"));
        //On the node itself, the message without the node's name.
        const std::string OnNode = M.issuesByHandle().at(Game).front();
        QVERIFY2(OnNode.rfind("layer", 0) == 0, OnNode.c_str());
        QCOMPARE(ValidationPanel::readable("node 'bafkreiabc': layer 0 broke"), QString("node 'bafkreiabc': layer 0 broke"));
        //Tagged by CID alone (most validator messages), the panel names the node by the package's own label for it.
        //Teeth: leave the CID; name a node the package does not hold.
        QCOMPARE(P.named("node '" + Game + "': VARIANT 'x' has no effective entry to run"),
                 QString("S.T.A.L.K.E.R.: Game (GOG): VARIANT 'x' has no effective entry to run"));
        QCOMPARE(P.named("node 'elsewhere': x"), QString("node 'elsewhere': x"));
        M.setValidationForTest({"node '" + Game + "': VARIANT 'x' has no effective entry to run"}, {});
        QVERIFY2(L->count() >= 1 && L->item(0)->text().startsWith("S.T.A.L.K.E.R.: Game (GOG): VARIANT"), qUtf8Printable(L->item(0)->text()));
    }

    // A problem of the whole package that has a fix (a case conflict between two layers' files) says so, and
    // clicking it asks for that fix. Teeth: list it as a plain error; click into nothing.
    void aPackageWideProblemIsFixedFromThePanel()
    {
        QCOMPARE(PkgGraph::FixFor("content case conflict across layers: file 'A.exe' (a.zip) vs 'a.exe' (b.zip)").Action, std::string("fix_case"));
        library();
        PackageEditorModel M(&Cfg, nullptr);
        M.initPackage(Pkg);
        M.Revalidate();
        ValidationPanel P(&M);
        QListWidget *L = P.findChild<QListWidget *>();
        QVERIFY(L);
        L->clear();
        //The panel's own row for such a message, as refresh() builds it.
        M.setValidationForTest({"content case conflict across layers: file 'A.exe' (a.zip) vs 'a.exe' (b.zip)"}, {});
        QVERIFY(L->count() == 1);
        QVERIFY2(L->item(0)->text().contains("click to fix case"), qUtf8Printable(L->item(0)->text()));
        QSignalSpy Fix(&P, &ValidationPanel::fixClicked);
        emit L->itemClicked(L->item(0));
        QCOMPARE(Fix.count(), 1);
        QCOMPARE(Fix.at(0).at(0).toString(), QString("fix_case"));
    }

    // Changing the game's ID: its tiles, the tiles nested under it and RECOMMENDED follow, the folder is renamed, the
    // saves kept under the old ID move with it and the package reopens there. A taken ID or a non-number changes
    // nothing. Teeth: leave PARENTUID; keep the old folder; leave the saves behind; accept a taken ID.
    void theGameIdIsChangedEverywhere()
    {
        QTemporaryDir R; QVERIFY(R.isValid());
        json Cfg2 = json{{"Settings", {{"Paths", {{"LibraryRoot", (R.path() + "/LIBRARY").toStdString()}}}}}};
        AppPaths::SetDataRoot(R.path().toStdString());
        const QString Lib2 = R.path() + "/LIBRARY/VidyaGod";
        const QString P = Lib2 + "/[201801010001][v1.0] Anomaly";
        QDir().mkpath(P); QDir().mkpath(Lib2 + "/[55492][v1.0] Other");
        const json G = json::parse(R"({"LABEL":"G","VARIANT":"Original","RECOMMENDED":["201801010001"],"LAYERS":[{"EXEC":[{"LABEL":"Play","TILE":{"UID":"201801010001","TITLE":"A"}}]}]})");
        const json D = json::parse(R"({"LABEL":"D","LAYERS":[{"EXEC":[{"LABEL":"DLC","TILE":{"UID":"9","PARENTUID":"201801010001"}}]}]})");
        Freeze(P, G); Freeze(P, D);
        const QString Users = QString::fromStdString(InstanceStore::Root(Cfg2).string());
        QDir().mkpath(Users + "/201801010001/DefaultInstance");
        PackageEditorModel M(&Cfg2, nullptr);
        M.initPackage(P);
        QCOMPARE(M.gameId(), std::string("201801010001"));
        QString E;
        QVERIFY(!M.changeGameId("55492", &E));                    // taken
        QVERIFY(!M.changeGameId("2018-07-21", &E));               // not a number
        QVERIFY(QDir(P).exists());
        QVERIFY2(M.changeGameId("201807210001", &E), qUtf8Printable(E));
        const QString To = Lib2 + "/[201807210001][v1.0] Anomaly";
        QVERIFY(QDir(To).exists() && !QDir(P).exists());
        QCOMPARE(M.packagePath(), To);
        QVERIFY(QDir(Users + "/201807210001/DefaultInstance").exists() && !QDir(Users + "/201801010001").exists());
        std::string All;
        for (int I = 0; I < M.doc().Count(); ++I) All += M.doc().Node(I).dump();
        QVERIFY(All.find("201801010001") == std::string::npos);
        QVERIFY(All.find("\"PARENTUID\":\"201807210001\"") != std::string::npos);
        QVERIFY(All.find("\"RECOMMENDED\":[\"201807210001\"]") != std::string::npos);
        AppPaths::SetDataRoot(Root->path().toStdString());
    }

    // Deleting every node of a saved package and saving is asked about first: the model says so. A new, never-saved
    // package with no nodes is not. Teeth: never say so; say so for an empty new folder.
    void savingAwayEveryNodeIsCaughtFirst()
    {
        library();
        PackageEditorModel M(&Cfg, nullptr);
        M.initPackage(Pkg);
        QVERIFY(!M.saveEmptiesPackage());
        M.doc().Remove(0);
        QVERIFY(M.saveEmptiesPackage());
        QTemporaryDir Empty; QVERIFY(Empty.isValid());
        PackageEditorModel N(&Cfg, nullptr);
        N.initPackage(Empty.path());
        QVERIFY(!N.saveEmptiesPackage());
    }

    // A file in the package folder no node names (a removed layer's zip) is reported, and can be deleted; files the
    // nodes name (a zip, a cover) and the node files are not. Teeth: report named files; skip the report; delete all.
    void unusedPackageFilesAreReportedAndDeleted()
    {
        library();
        QFile Z(Pkg + "/old layer.zip"); QVERIFY(Z.open(QIODevice::WriteOnly)); Z.close();
        QFile C(Pkg + "/cover.png"); QVERIFY(C.open(QIODevice::WriteOnly)); C.close();
        PackageEditorModel M(&Cfg, nullptr);
        M.initPackage(Pkg);
        json N = M.doc().Node(0);
        N["LAYERS"].push_back(json::parse(R"({"EXEC":[{"LABEL":"Play","TILE":{"UID":"1","COVER":{"FILE":"cover.png"}}}]})"));
        M.replaceNode(Game, N);
        QCOMPARE(M.unusedFiles(), QStringList{"old layer.zip"});
        M.Revalidate();
        QVERIFY(std::any_of(M.validationWarnings().begin(), M.validationWarnings().end(),
                            [](const std::string &W) { return W.find("'old layer.zip' is used by no node") != std::string::npos; }));
        QCOMPARE(PkgGraph::FixFor("package file 'x' is used by no node").Action, std::string("delete_unused"));
        QCOMPARE(M.deleteUnusedFiles(), 1);
        QVERIFY(!QFile::exists(Pkg + "/old layer.zip") && QFile::exists(Pkg + "/cover.png"));
        QFile::remove(Pkg + "/cover.png");
    }

    // A file is unused only when nothing names it: a bumped name (a CID's own copy of a shared name) is named by its
    // node's name + SOURCE, and a file the SAVED nodes name stays named while an unsaved edit drops it (a discard
    // would bring the name back). Teeth: drop the bumped names (the [12345678] copy is listed); read the Doc only
    // (saved.zip is listed).
    void bumpedAndSavedOnlyFilesAreNotUnused()
    {
        library();
        const std::string Src = "bafkreiaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa12345678";
        const json L = json{{"LABEL", "Lib"}, {"LAYERS", json::array({ json{{"ZIP", "lib.zip"}, {"SOURCE", Src}, {"TARGET", "FILES/game"}},
                                                                     json{{"ZIP", "saved.zip"}, {"TARGET", "FILES/game"}} })}};
        const std::string Lc = Freeze(Pkg, L);
        for (const char *F : {"/lib [12345678].zip", "/saved.zip"}) { QFile Z(Pkg + F); QVERIFY(Z.open(QIODevice::WriteOnly)); Z.close(); }
        PackageEditorModel M(&Cfg, nullptr);
        M.initPackage(Pkg);
        json Edited = L;
        Edited["LAYERS"].erase(1);                        // unsaved: no node in the editor names saved.zip any more
        M.replaceNode(Lc, Edited);
        QCOMPARE(M.unusedFiles(), QStringList{});
        QCOMPARE(M.deleteUnusedFiles(), 0);
        QVERIFY(QFile::exists(Pkg + "/lib [12345678].zip") && QFile::exists(Pkg + "/saved.zip"));
    }

    // A capture of the files a node was waiting for takes the waiting layer's place (its COMMENT kept); one for
    // another place does not. Teeth: fill whatever TARGET; replace the layer whole.
    void aCaptureFillsTheLayerWaitingForIt()
    {
        library();
        PackageEditorModel M(&Cfg, nullptr);
        M.initPackage(Pkg);
        json N = M.doc().Node(0);
        N["LAYERS"].push_back(json{{"ZIP", ""}, {"TARGET", "FILES/%GameDir%"}, {"COMMENT", "the game"}});
        M.replaceNode(Game, N);
        const std::string H = M.doc().Handle(0);
        QVERIFY(!M.fillWaiting(H, json{{"DIR", "docs"}, {"TARGET", "FILES/%Documents%/G"}}));
        QVERIFY(M.fillWaiting(H, json{{"DIR", "game"}, {"TARGET", "FILES/%GameDir%"}}));
        QCOMPARE(M.doc().Node(0)["LAYERS"].back(), json::parse(R"({"DIR":"game","TARGET":"FILES/%GameDir%","COMMENT":"the game"})"));
        QVERIFY(!M.fillWaiting(H, json{{"DIR", "again"}, {"TARGET", "FILES/%GameDir%"}}));   // it waits for nothing now
    }

    // Edits are checked in the background, without a call: the verdict arrives on its own. A check that was running
    // when the package changed is about the package BEFORE the change: it is thrown away and the check runs again.
    // Teeth: apply a finished check whatever it was about (the broken reference is still reported after the fix).
    void editsAreCheckedLiveAndNeverByAStaleVerdict()
    {
        library();
        PackageEditorModel M(&Cfg, nullptr);
        M.initPackage(Pkg);
        json N = M.doc().Node(0);
        N["LAYERS"][0]["NODE"] = "bafkreinosuchnodeanywherexxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
        M.replaceNode(Game, N);
        QTRY_VERIFY_WITH_TIMEOUT(M.validated() && !M.validationErrors().empty(), 5000);
        QVERIFY(M.issuesByHandle().count(Game));
        //A check starts on the broken package; the fix lands before its result is read.
        M.validateNow();
        QSignalSpy Verdict(&M, &PackageEditorModel::validationChanged);
        N["LAYERS"][0]["NODE"] = Fonts;
        M.replaceNode(Game, N);
        QVERIFY(Verdict.wait(5000));
        QVERIFY2(M.validationErrors().empty(), M.validationErrors().empty() ? "" : M.validationErrors().front().c_str());
        QVERIFY(!M.validating());
    }

    // The editor reports on the package it has open: a library-wide finding about another package's nodes (two
    // runners disagreeing where %GameDir% is) is not this package's, while a broken node it CONTAINS is - whichever
    // package holds it. Teeth: keep every message (the runners' warning shows here); drop every other package's node
    // (the broken dependency vanishes).
    void problemsAreThisPackagesAndWhatItContains()
    {
        library();
        const QString Runners = Lib + "/Vendor/[3][v1] Runners";
        QDir().mkpath(Runners);
        auto Runner = [](const std::string &Label, const std::string &Root) {
            return json{{"LABEL", Label}, {"LAYERS", json::array({ json{{"EXEC", json::array({
                json{{"HOST", "linux64"}, {"GUEST", json::array({"win32"})}, {"EXE", "%RuntimePath%/run"},
                     {"GUEST_ROOTS", {{"%GameDir%", Root}}}} })}} })}};
        };
        Freeze(Runners, Runner("Runner A", "c:/a"));
        const std::string RunnerB = Freeze(Runners, Runner("Runner B", "c:/b"));
        PackageEditorModel M(&Cfg, nullptr);
        M.initPackage(Pkg);
        M.Revalidate();
        auto Mentions = [](const std::vector<std::string> &V, const std::string &What) {
            return std::any_of(V.begin(), V.end(), [&](const std::string &S) { return S.find(What) != std::string::npos; });
        };
        QVERIFY(!Mentions(M.validationWarnings(), "runners disagree"));
        QVERIFY(!Mentions(M.validationErrors(), RunnerB) && !Mentions(M.validationWarnings(), RunnerB));

        //Break the node the game contains, in its own package (it names a node that is nowhere): the game's editor says
        //so, about THAT node.
        QDir(Other).removeRecursively();
        QDir().mkpath(Other);
        const std::string BrokenId = Freeze(Other, Over("Core Fonts", "bafkreinosuchnodeanywherexxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"));
        PackageEditorModel M2(&Cfg, nullptr);              // the library as it is now
        M2.initPackage(Pkg);
        json G = M2.doc().Node(0);
        G["LAYERS"][0]["NODE"] = BrokenId;
        M2.replaceNode(Game, G);
        M2.Revalidate();
        const auto &E = M2.validationErrors();
        QVERIFY2(std::any_of(E.begin(), E.end(), [&](const std::string &S) { return S.rfind("node '" + BrokenId + "'", 0) == 0; }),
                 "a broken node the package contains went unreported");
        QVERIFY(!Mentions(M2.validationWarnings(), "runners disagree"));
    }

    // The node panel's tabs say what the selected node amounts to: its fold in order (here its contained node from
    // another package - walked, by its name and package - then its own ENV layer) and its shelf card, recomputed when
    // it is selected. Teeth: show nothing until the next edit; fill the tab not on screen only; only note the other
    // package's node.
    void theNodePanelShowsTheFoldAndTheCard()
    {
        library();
        PackageEditorModel M(&Cfg, nullptr);
        M.initPackage(Pkg);
        QTabWidget Tabs;
        Tabs.addTab(new QWidget, "JSON");
        PkgInsight In(&M, &Tabs);
        QCOMPARE(Tabs.count(), 4);                          // JSON + game files, fold order, card
        Tabs.setCurrentIndex(2);                            // Fold order, nothing selected yet
        QTest::qWait(400);
        QCOMPARE(Tabs.currentWidget()->findChild<QTreeWidget *>()->topLevelItemCount(), 0);
        In.showNode(QString::fromStdString(Game));          // selecting the node fills it
        QTRY_VERIFY_WITH_TIMEOUT(Tabs.currentWidget()->findChild<QTreeWidget *>()->topLevelItemCount() == 2, 3000);
        QTreeWidget *Fold = Tabs.currentWidget()->findChild<QTreeWidget *>();
        QVERIFY2(Fold->topLevelItem(0)->text(1).contains("ZIP") && Fold->topLevelItem(0)->text(2).contains("Core Fonts"),
                 qUtf8Printable(Fold->topLevelItem(0)->text(1) + " | " + Fold->topLevelItem(0)->text(2)));
        QVERIFY(Fold->topLevelItem(1)->text(1).contains("ENV"));
        Tabs.setCurrentIndex(3);                            // Shelf card
        bool Said = false;
        QTRY_VERIFY_WITH_TIMEOUT([&] { for (QLabel *L : Tabs.currentWidget()->findChildren<QLabel *>()) if (L->text().contains("no tile yet")) Said = true; return Said; }(), 3000);
    }

    // Unsaved work is kept aside as a draft: written only while there is unsaved work, read back as the document
    // (handles, nodes, a removed node gone again) and dropped on request. Teeth: write a draft when clean; restore
    // without removing the nodes the draft no longer has.
    void unsavedWorkSurvivesAsADraft()
    {
        library();
        {
            PackageEditorModel M(&Cfg, nullptr);
            M.initPackage(Pkg);
            M.dropDraft();
            QVERIFY(!M.writeDraft());                       // nothing unsaved: no draft
            QCOMPARE(M.draftTime(), 0);
            json N = M.doc().Node(0); N["LABEL"] = "Edited in a crash"; M.replaceNode(Game, N);
            M.createNode(json{{"LAYERS", json::array({ json{{"ENV", {{"X", "1"}}}} })}}, {}, "New one");
            QVERIFY(M.writeDraft());
            QVERIFY(M.draftTime() > 0);
        }                                                   // the editor goes away unsaved
        PackageEditorModel M(&Cfg, nullptr);
        M.initPackage(Pkg);
        QCOMPARE(M.doc().Count(), 1);                       // the files on disk: as before
        QVERIFY(M.draftTime() > 0);
        QVERIFY(M.restoreDraft());
        QCOMPARE(M.doc().Count(), 2);
        QCOMPARE(M.doc().Node(M.doc().IndexOf(Game))["LABEL"], json("Edited in a crash"));
        QVERIFY(M.isDirty());
        //A draft that has fewer nodes than the document: the extra one goes.
        json Kept = M.doc().Node(M.doc().IndexOf(Game));
        M.dropDraft();
        QCOMPARE(M.draftTime(), 0);
        M.doc().Remove(M.doc().IndexOf(Game) == 0 ? 1 : 0);
        M.doc().Commit();
        QVERIFY(M.writeDraft());
        PackageEditorModel M2(&Cfg, nullptr);
        M2.initPackage(Pkg);
        M2.createNode(json{{"LAYERS", json::array()}}, {}, "Stray");
        QVERIFY(M2.restoreDraft());
        QCOMPARE(M2.doc().Count(), 1);
        QCOMPARE(M2.doc().Node(0), Kept);
        M2.dropDraft();
    }

    // The tour is shown until it is finished once on this machine: the mark is kept in GlobalConfig. Teeth: never
    // remember it; remember it without writing the config.
    void theTourIsRememberedOnceSeen()
    {
        json C = json{{"Settings", json::object()}};
        PackageEditorModel M(&C, nullptr);
        QVERIFY(!M.tourSeen());
        M.markTourSeen();
        QVERIFY(M.tourSeen());
        QFile F(Root->path() + "/GlobalConfig.JSON");
        QVERIFY(F.open(QIODevice::ReadOnly));
        QVERIFY(F.readAll().contains("EditorTourSeen"));
    }

    // A capture creates a node CONTAINING the node it was anchored at, as an unsaved draft; saving names it.
    void aCaptureIsANewNodeContainingItsAnchor()
    {
        library();
        PackageEditorModel M(&Cfg, nullptr);
        M.initPackage(Pkg);
        const std::string H = M.createNode(json{{"LAYERS", json::array({ json{{"DIR", "captured"}} })}}, {Game}, "Captured files");
        QVERIFY(H.rfind("draft-", 0) == 0);
        QVERIFY(M.isDirty());
        const int I = M.doc().IndexOf(H);
        QCOMPARE(M.doc().Node(I)["LAYERS"][0]["NODE"].get<std::string>(), Game);   // the anchor first
        QCOMPARE(M.doc().Node(I)["LAYERS"][1]["DIR"].get<std::string>(), std::string("captured"));
        QCOMPARE(NodeFiles(Pkg), 1);                        // nothing written yet
        QVERIFY(M.Save());
        QCOMPARE(NodeFiles(Pkg), 2);
        QVERIFY(M.lastRenames().count(H));
    }

    // A problem with a node not saved yet is still about THAT node (by its draft handle), so the canvas marks it.
    // Teeth: name it the way a library scan does (by label, since a draft has no CID) — the mark has nowhere to go.
    void aDraftsProblemIsOnTheDraft()
    {
        library();
        PackageEditorModel M(&Cfg, nullptr);
        M.initPackage(Pkg);
        const std::string H = M.createNode(json{{"LAYERS", json::array({ json{{"ZIP", ""}} })}}, {}, "Setup files");
        M.Revalidate();
        QVERIFY(!M.validationErrors().empty());
        QVERIFY2(M.issuesByHandle().count(H), M.validationErrors().front().c_str());
    }

    // This machine's positions live in GlobalConfig, keyed by node, and follow a save's renames; a corrupt one is
    // refused on load (the node is laid out instead). Teeth: key by the old handle after a save.
    void theLayoutFollowsRenamesAndRefusesCorruption()
    {
        library();
        {
            PackageEditorModel M(&Cfg, nullptr);
            M.initPackage(Pkg);
            M.doc().SetPos(Game, {120, 340});
            M.doc().Commit();
            M.noteEdited();
            json N = M.doc().Node(0); N["LABEL"] = "Moved"; M.replaceNode(Game, N);
            QVERIFY(M.Save());
        }
        QVERIFY(Cfg.contains("EDITORLAYOUT"));
        const json &L = Cfg["EDITORLAYOUT"].begin().value();
        QCOMPARE((int)L.size(), 1);
        QVERIFY(!L.contains(Game));                         // the old name is gone...
        const std::string New = L.begin().key();
        QVERIFY(QFile::exists(Pkg + "/" + QString::fromStdString(New) + ".json"));   // ...and the key is the node's file
        //Corrupt the stored position: it is refused.
        Cfg["EDITORLAYOUT"].begin().value()[New] = json::array({1e300, 5});
        PackageEditorModel M2(&Cfg, nullptr);
        M2.initPackage(Pkg);
        QVERIFY(!M2.doc().Positions().count(New));
    }

    // A package received from a friend (a CATALOG stub) is theirs: the editor may show it but never saves into it.
    // Teeth: drop the refusal (the stub is re-minted in place).
    void aReceivedPackageIsNeverSaved()
    {
        library();
        const QString Stub = Root->path() + "/CATALOG/Bob - Games/[9][v1] Theirs";
        QDir().mkpath(Stub);
        const std::string C = Freeze(Stub, Zip("Theirs", "t.zip"));
        PackageEditorModel M(&Cfg, nullptr);
        M.initPackage(Stub);
        json N = M.doc().Node(0); N["LABEL"] = "mine"; M.replaceNode(C, N);
        QString Err;
        QVERIFY(!M.Save(&Err));
        QVERIFY(Err.contains("friend"));
        QVERIFY(QFile::exists(Stub + "/" + QString::fromStdString(C) + ".json"));
        QCOMPARE(NodeFiles(Stub), 1);
    }

    // A JSON file in the package that is not a node is never loaded, rewritten or swept by a save.
    void aFileThatIsNotANodeSurvivesASave()
    {
        library();
        std::ofstream((Pkg + "/notes.json").toStdString()) << R"(["keep", "me"])";
        PackageEditorModel M(&Cfg, nullptr);
        M.initPackage(Pkg);
        json N = M.doc().Node(0); N["LABEL"] = "x"; M.replaceNode(Game, N);
        QVERIFY(M.Save());
        std::ifstream In((Pkg + "/notes.json").toStdString());
        json J; In >> J;
        QCOMPARE(J, json::array({"keep", "me"}));
    }

    // The canvas is Dear ImGui, which keeps ONE global context per process: OpenFor is the single door — it raises
    // (and switches) the editor already open instead of building one that cannot render.
    void onlyOneEditorIsEverOpen()
    {
        library();
        bool CreatedA = false, CreatedB = false;
        PackageEditor *A = PackageEditor::OpenFor(&Cfg, nullptr, Pkg, &CreatedA);
        QVERIFY(A && CreatedA);
        PackageEditor *B = PackageEditor::OpenFor(&Cfg, nullptr, Pkg, &CreatedB);
        QCOMPARE(B, A);
        QVERIFY(!CreatedB);
        delete A;
        bool CreatedC = false;
        PackageEditor *C = PackageEditor::OpenFor(&Cfg, nullptr, Pkg, &CreatedC);
        QVERIFY(C && CreatedC);
        delete C;
    }

    // Every editor the door makes is handed to the host's hook once — the library's, a launch window's and a command's
    // alike — and raising the open one is not a new editor. Teeth: wire per opener (as the three did) and the hook
    // never runs.
    void everyEditorGoesThroughTheHostsHook()
    {
        library();
        int Hooked = 0;
        PackageEditor::SetOnCreated([&Hooked](PackageEditor *) { ++Hooked; });
        PackageEditor *A = PackageEditor::OpenFor(&Cfg, nullptr, Pkg);
        PackageEditor::OpenFor(&Cfg, nullptr, Pkg);
        QCOMPARE(Hooked, 1);
        delete A;
        PackageEditor::SetOnCreated({});
    }

    // A node the panel names may be the malformed one it reports: a LABEL that is not text is no name, never a throw.
    // Teeth: read the LABEL as text and naming the message throws.
    void aMalformedLabelNamesNothing()
    {
        library();
        PackageEditorModel M(&Cfg, nullptr);
        M.initPackage(Pkg);
        json N = M.doc().Node(0);
        N["LABEL"] = 5;
        M.replaceNode(Game, N);
        ValidationPanel P(&M);
        QCOMPARE(P.named("node '" + Game + "': x"), QString("node '" + QString::fromStdString(Game) + "': x"));
    }
};

QTEST_MAIN(PackageEditorModelTest)
#include "test_packageeditormodel.moc"
