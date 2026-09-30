// PackageEditorModel — the package editor's state: the package as a PkgDoc::Document, saving it (a generation-6
// re-mint that follows renamed nodes through the library), validating it as edited, this machine's layout for it, and
// what the rest of the library says about the nodes it names. Authoring runs (RunInNode) raise dialogs and build live
// containers, so they are out of scope here. A real temp data root holds a small library.

#include <QtTest>
#include "apppaths.h"
#include "packageeditor.h"
#include "packageeditormodel.h"
#include "cid.h"

#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>

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
        M.initPackage(Pkg, nullptr);
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
    }

    // Save re-mints: the edited node gets the CID of its new bytes, the old file goes, the renames are announced
    // (the canvas and the JSON panel follow them) and the package is clean. Teeth: write back under the old name.
    void savingAnEditRenamesTheNodeAndSaysSo()
    {
        library();
        PackageEditorModel M(&Cfg, nullptr);
        M.initPackage(Pkg, nullptr);
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
        M.initPackage(Pkg, nullptr);
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

    // Edits are checked in the background, without a call: the verdict arrives on its own. A check that was running
    // when the package changed is about the package BEFORE the change: it is thrown away and the check runs again.
    // Teeth: apply a finished check whatever it was about (the broken reference is still reported after the fix).
    void editsAreCheckedLiveAndNeverByAStaleVerdict()
    {
        library();
        PackageEditorModel M(&Cfg, nullptr);
        M.initPackage(Pkg, nullptr);
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

    // A capture creates a node CONTAINING the node it was anchored at, as an unsaved draft; saving names it.
    void aCaptureIsANewNodeContainingItsAnchor()
    {
        library();
        PackageEditorModel M(&Cfg, nullptr);
        M.initPackage(Pkg, nullptr);
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
        M.initPackage(Pkg, nullptr);
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
            M.initPackage(Pkg, nullptr);
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
        M2.initPackage(Pkg, nullptr);
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
        M.initPackage(Stub, nullptr);
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
        M.initPackage(Pkg, nullptr);
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
};

QTEST_MAIN(PackageEditorModelTest)
#include "test_packageeditormodel.moc"
