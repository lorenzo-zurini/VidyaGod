// PkgActions — the node actions that touch real files. This is the riskiest code in the editor: zip<->dir
// conversion, re-store and make-delta all DELETE the source once the replacement exists, so a mistake here
// destroys content that may be the only copy on the machine.
//
// It was entirely uncovered. These tests drive the real conversions against real archives on disk and assert
// the property that matters most: on FAILURE, nothing is deleted. Confirmation dialogs are injected (the
// actions ask before destroying anything, and a modal blocks a headless run forever).

#include "qtestjson.h"
#include "pkgactions.h"
#include "ipfswrapper.h"
#include "manifestmodel.h"
#include "pkgcanvas.h"
#include "packageeditormodel.h"
#include "jsonraweditor.h"
#include <QPlainTextEdit>

#include "imgui.h"

#include <QtTest>
#include "apppaths.h"
#include <QTemporaryDir>
#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QElapsedTimer>

#include <fstream>
#include <zip.h>

using json = nlohmann::ordered_json;

class PkgActionsTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        //Claim a data root for the WHOLE binary — see the note in test_packagecatalog.cpp. Without it,
        //anything reaching SaveLayout writes to the developer's real ~/.VidyaGod/GlobalConfig.JSON.
        SuiteDataRoot = new QTemporaryDir();
        QVERIFY(SuiteDataRoot->isValid());
        AppPaths::SetDataRoot(SuiteDataRoot->path().toStdString());

        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        // zip/unzip are what the conversions shell out to; without them these tests would assert nothing.
        HaveZipTools = !QStandardPaths::findExecutable("zip").isEmpty()
                    && !QStandardPaths::findExecutable("unzip").isEmpty();
    }
    void cleanupTestCase() { ImGui::DestroyContext(); }

    // A zip -> dir -> zip round trip through the real actions: the content survives, and the source is gone
    // only after its replacement exists.
    void zipDirRoundTripPreservesContent()
    {
        if (!HaveZipTools) QSKIP("zip/unzip not installed");
        Fixture F(this);
        F.makeZip("content.zip", "hello/world.txt", "payload-abc");
        const int n = F.addContentNode("content.zip");

        F.act->perform(F.nodeId(n), "to_dir");
        QVERIFY(F.waitIdle(F.nodeId(n)));

        QCOMPARE(F.type(n), std::string("DIR"));
        const QString Dir = F.path("content");
        QVERIFY(QFile::exists(Dir + "/hello/world.txt"));
        QVERIFY(!QFile::exists(F.path("content.zip")));            // replaced, not duplicated
        QCOMPARE(F.read(Dir + "/hello/world.txt"), QByteArray("payload-abc"));

        F.act->perform(F.nodeId(n), "to_zip");
        QVERIFY(F.waitIdle(F.nodeId(n)));
        QCOMPARE(F.type(n), std::string("ZIP"));
        QCOMPARE(F.name(n), std::string("content.zip"));
        QVERIFY(QFile::exists(F.path("content.zip")));
        QVERIFY(!QDir(Dir).exists());
    }

    // A node with more than one folder (a patch captured over the game's zip): the validator's "unzipped authoring
    // layer" warning offers -> zip for THAT folder, and packing it converts that layer only - its TARGET and COMMENT
    // kept, the game's zip before it untouched. Teeth: FixFor offers nothing / the wrong name; the action packs the
    // node's first content layer; DirToZip drops the layer's other fields; the layer stays a DIR.
    void aSecondFolderLayerIsPackedFromItsWarning()
    {
        if (!HaveZipTools) QSKIP("zip/unzip not installed");
        Fixture F(this);
        F.makeZip("game.zip", "game.exe", "GAME");
        QDir().mkpath(F.path("Patch 4/data"));
        F.write(F.path("Patch 4/data/x.txt"), "patched");
        const int n = F.addContentNode("game.zip");
        F.setLayers(n, json::array({ json{{"ZIP", "game.zip"}, {"TARGET", "FILES/%GameDir%"}},
                                     json{{"COMMENT", "the patch"}, {"DIR", "Patch 4"}, {"TARGET", "FILES/%GameDir%"}} }));
        const PkgGraph::Fix Fx = PkgGraph::FixFor("Game: DIR 'Patch 4' is an unzipped authoring layer \xe2\x80\x94 convert it to a "
                                                  "STORE zip ('\xe2\x86\x92 ZIP') before publishing.");
        QCOMPARE(Fx.Action, std::string("to_zip:Patch 4"));
        QCOMPARE(PkgGraph::FixFor("DIR 'Patch 4' is an unzipped authoring layer - convert it").Action, std::string("to_zip:Patch 4"));

        F.act->perform(F.nodeId(n), QString::fromStdString(Fx.Action));
        QVERIFY(F.waitIdle(F.nodeId(n)));

        const json N = F.node(n);
        QCOMPARE(N["LAYERS"][0], (json{{"ZIP", "game.zip"}, {"TARGET", "FILES/%GameDir%"}}));
        QCOMPARE(N["LAYERS"][1], (json{{"COMMENT", "the patch"}, {"ZIP", "Patch 4.zip"}, {"TARGET", "FILES/%GameDir%"}}));
        QVERIFY(QFile::exists(F.path("Patch 4.zip")));
        QVERIFY(!QDir(F.path("Patch 4")).exists());
        QVERIFY(QFile::exists(F.path("game.zip")));
    }

    // THE property. `zip -r` UPDATES an existing archive rather than replacing it, so packing into a name a
    // sibling layer already owns used to merge into that archive and then delete the folder — losing the
    // folder and contaminating someone else's layer. It must refuse and change nothing.
    void packRefusesToClobberAnExistingArchive()
    {
        if (!HaveZipTools) QSKIP("zip/unzip not installed");
        Fixture F(this);
        QDir().mkpath(F.path("stuff"));
        F.write(F.path("stuff/a.txt"), "mine");
        F.write(F.path("stuff.zip"), "SOMEONE ELSE'S ARCHIVE");   // the name is taken
        const int n = F.addContentNode("stuff");
        F.setType(n, "DIR");

        F.act->perform(F.nodeId(n), "to_zip");
        QTest::qWait(300);

        QVERIFY(QDir(F.path("stuff")).exists());                   // the folder was NOT deleted
        QCOMPARE(F.read(F.path("stuff.zip")), QByteArray("SOMEONE ELSE'S ARCHIVE"));   // untouched
        QCOMPARE(F.type(n), std::string("DIR"));                          // node unchanged
        QVERIFY(!F.notices.isEmpty());                             // and it SAID so rather than failing quietly
        QVERIFY(F.notices.join(" ").contains("already exists"));
    }

    // The MIRROR of packRefusesToClobberAnExistingArchive, and the likelier collision of the two: round-tripping
    // -> zip / -> dir leaves a bundle holding both "stuff.zip" and "stuff/". Unpacking into the existing folder
    // would MERGE this archive into whatever layer already owns that name and then DELETE the zip — one layer
    // contaminated, the other destroyed, neither recoverable.
    //
    // Deliberately NOT gated on HaveZipTools: the guard runs before any tool is invoked, so this keeps its teeth
    // on a machine (or a CI image) with no zip/unzip — which is exactly where the other conversion tests skip.
    void unpackRefusesToClobberAnExistingFolder()
    {
        Fixture F(this);
        F.write(F.path("stuff.zip"), "MY ARCHIVE");
        QDir().mkpath(F.path("stuff"));
        F.write(F.path("stuff/theirs.txt"), "SOMEONE ELSE'S LAYER");   // the name is taken
        const int n = F.addContentNode("stuff.zip");

        F.act->perform(F.nodeId(n), "to_dir");
        QTest::qWait(300);

        QCOMPARE(F.read(F.path("stuff.zip")), QByteArray("MY ARCHIVE"));            // the zip was NOT deleted
        QCOMPARE(F.read(F.path("stuff/theirs.txt")), QByteArray("SOMEONE ELSE'S LAYER"));  // nor contaminated
        QCOMPARE(F.type(n), std::string("ZIP"));                                  // node unchanged
        QVERIFY(!F.notices.isEmpty());                                              // and it SAID so
        QVERIFY(F.notices.join(" ").contains("already exists"));

        // An EMPTY folder of that name is not a collision — it is the debris of a previous failed run, and
        // refusing on it would wedge the action with nothing for the author to see.
        Fixture G(this);
        G.write(G.path("stuff.zip"), "MY ARCHIVE");
        QDir().mkpath(G.path("stuff"));
        const int m = G.addContentNode("stuff.zip");
        G.act->setToolNamesForTest("definitely-not-a-real-zip-tool", "definitely-not-a-real-unzip-tool");
        G.act->perform(G.nodeId(m), "to_dir");
        QVERIFY(G.waitIdle(G.nodeId(m)));
        QVERIFY(G.notices.join(" ").contains("did not complete"));   // it got PAST the guard to the tool
    }

    // THE worst thing this editor can do. `Src = Bundle / PATH` had no containment check where the DELETION
    // happens — only in the file picker, which misses both ways in:
    //   * "."  — the directory picker OPENS in the bundle, so clicking OK on the bundle folder itself gives
    //            relativeFilePath(Bundle) == ".", which does not start with ".." and sails through. `-> zip`
    //            then packed the bundle into itself and remove_all()'d the whole thing, reporting SUCCESS.
    //   * "../sibling" — the PATH box is free text, so it can simply be typed, and the deletion landed
    //            outside the bundle entirely.
    void conversionsRefuseAPathThatIsNotInsideTheBundle()
    {
        Fixture F(this);
        QDir().mkpath(F.path("layer1"));
        F.write(F.path("layer1/game.exe"), "the game");
        F.write(F.path("readme.txt"), "keep me");

        // A sibling directory OUTSIDE the bundle, which "../" would reach.
        const QString Outside = QFileInfo(F.dir->path()).path() + "/vg_outside_probe";
        QDir().mkpath(Outside);
        F.write(Outside + "/precious.txt", "do not delete me");

        // EVERY action that touches a path, and every way out of the bundle. "nosuch/.." is the one that
        // canonicalises back to the bundle ROOT with a trailing separator — which the first version of the
        // guard compared as a different path and let through.
        // ("" is refused earlier, by the names-a-file check — also correct, also not what this pins.)
        for (const char *Bad : {".", "./", "..", "nosuch/..", "a/b/../../..",
                                "../vg_outside_probe", "../vg_outside_probe/precious.txt", "/tmp"})
        {
            const int n = F.addContentNode(Bad);
            // to_delta is omitted: it refuses earlier and for a different reason (no base to diff against),
            // which is also correct but not what this test is pinning.
            for (const char *Act : {"to_zip", "to_dir", "restore", "undelta"})
            {
                F.setType(n, (std::string(Act) == "to_zip") ? "DIR" : "ZIP");
                F.notices.clear();
                F.act->perform(F.nodeId(n), Act);
                QTest::qWait(120);
                // The REFUSAL is the assertion, not merely that nothing was destroyed: several of these
                // happen to fail later anyway (the zip tool errors on a nonexistent path), so a survival-only
                // check passes just as well with the guard removed.
                QVERIFY2(F.notices.join(" ").contains("does not point inside"),
                         qUtf8Printable(QString("%1 on file \"%2\" was not refused: %3")
                                            .arg(Act).arg(Bad).arg(F.notices.join(" | "))));
            }
        }

        // The bundle is intact...
        QVERIFY2(QFile::exists(F.path("layer1/game.exe")), "the bundle's content was destroyed");
        QVERIFY2(QFile::exists(F.path("readme.txt")), "the bundle's content was destroyed");
        // ...and so is everything outside it.
        QVERIFY2(QFile::exists(Outside + "/precious.txt"), "a directory OUTSIDE the bundle was destroyed");
        QVERIFY(!F.notices.isEmpty());
        QDir(Outside).removeRecursively();
    }

    // A conversion whose tool cannot run must end the action and leave the source alone. Previously
    // errorOccurred skipped the completion handler entirely: the node stayed locked forever and, in reStore,
    // the original had already been deleted.
    void aMissingToolUnlocksTheNodeAndKeepsTheSource()
    {
        Fixture F(this);
        F.write(F.path("content.zip"), "not-really-a-zip");
        const int n = F.addContentNode("content.zip");
        F.act->setToolNamesForTest("definitely-not-a-real-zip-tool", "definitely-not-a-real-unzip-tool");

        F.act->perform(F.nodeId(n), "to_dir");
        QVERIFY(F.waitIdle(F.nodeId(n)));                          // the node did NOT stay locked
        QVERIFY(QFile::exists(F.path("content.zip")));             // and the source is still here
        QVERIFY(!F.notices.isEmpty());                             // the failure was reported, not swallowed
        QCOMPARE(F.type(n), std::string("ZIP"));
    }

    // Declining the confirmation must be a complete no-op — these actions delete things.
    void decliningTheConfirmationChangesNothing()
    {
        if (!HaveZipTools) QSKIP("zip/unzip not installed");
        Fixture F(this, /*confirm=*/false);
        F.makeZip("content.zip", "a.txt", "keep-me");
        const int n = F.addContentNode("content.zip");

        F.act->perform(F.nodeId(n), "to_dir");
        QTest::qWait(200);
        QVERIFY(QFile::exists(F.path("content.zip")));
        QVERIFY(!F.canvas->isBusy(F.nodeId(n).toStdString()));
        QCOMPARE(F.type(n), std::string("ZIP"));
    }

    // The DEFAULT path — no injected handlers at all — is the one that ships, and the previous suite never
    // ran it: every fixture installed a Notify handler, which is precisely how an infinite self-recursion in
    // tell() passed a green suite. With no parent widget there is nothing to raise a modal over, so the
    // action must report by log and REFUSE rather than blocking or destroying anything unasked.
    void withNoHandlersNothingCrashesAndNothingIsDestroyed()
    {
        Fixture F(this, /*confirm=*/true, /*installHandlers=*/false);
        F.write(F.path("content.zip"), "keep-me");
        const int n = F.addContentNode("content.zip");

        F.act->perform(F.nodeId(n), "to_dir");        // would recurse forever, or pop a blocking modal
        QTest::qWait(200);
        QVERIFY(QFile::exists(F.path("content.zip")));                    // refused, so nothing was deleted
        QVERIFY(!F.canvas->isBusy(F.nodeId(n).toStdString()));

        F.setName(n, "");                                                 // drives the "Nothing to convert" report
        F.act->perform(F.nodeId(n), "to_zip");
        QTest::qWait(100);
        QVERIFY(true);                                                    // reaching here at all is the assertion
    }

    // The .reg parser's traps, none of which a "looks right" import would reveal. A truncated hex: value and
    // an inverted deletion both produce a plausible, WRONG registry layer that only shows up in-game.
    void regImportHandlesContinuationsDeletionsAndEscapes()
    {
        const QString Text = QStringLiteral(
            "Windows Registry Editor Version 5.00\r\n"
            "\r\n"
            "; a comment\r\n"
            "[HKEY_LOCAL_MACHINE\\Software\\Ubi Soft\\TONICT]\r\n"
            "\"Version\"=\"1.00\"\r\n"
            "\"Install Dir\"=\"C:\\\\Games\\\\Tonic\"\r\n"
            "\"Says\"=\"he said \\\"hi\\\"\"\r\n"
            "\"Blob\"=hex:01,02,03,\\\r\n"
            "  04,05,06,\\\r\n"
            "  07,08\r\n"
            "\"Stale\"=-\r\n"
            "@=\"default\"\r\n"
            "\r\n"
            "[-HKEY_LOCAL_MACHINE\\Software\\Gone]\r\n"
            "\"Ignored\"=\"should not appear\"\r\n");

        int deletions = -1;
        const auto rows = PkgActions::ParseRegExport(Text, &deletions);
        auto valueOf = [&](const char *name) -> std::string {
            for (const auto &r : rows) if (!r.KeyOnly && r.Name == name) return r.Value;
            return "<missing>";
        };

        QCOMPARE(valueOf("Version"), std::string("1.00"));
        QCOMPARE(valueOf("Install Dir"), std::string("C:\\Games\\Tonic"));   // .reg doubles backslashes
        QCOMPARE(valueOf("Says"), std::string("he said \"hi\""));            // ...and escapes quotes
        QCOMPARE(valueOf("Blob"), std::string("hex:01,02,03,04,05,06,07,08")); // the WHOLE value, not line 1
        QCOMPARE(valueOf("Stale"), std::string("<missing>"));                // a deletion is not a write
        // `@` is the key's DEFAULT value, which on disk is the EMPTY name. This test previously pinned the
        // literal name "@" — the bug, not the behaviour: a value CALLED @ that nothing reads.
        QCOMPARE(valueOf(""), std::string("default"));
        QCOMPARE(deletions, 2);                                              // the value AND the [-Key] section
        for (const auto &r : rows)
        {
            QVERIFY(r.Value != "should not appear");                         // nothing under a deleted key
            QVERIFY(r.Path.find("Gone") == std::string::npos);
        }
    }

    // ------------------------------------------------------------------------------------------------
    // import .reg — the only action that WRITES into a payload it did not create.
    // ------------------------------------------------------------------------------------------------

    // THE property, and the positive control for the ones below: an import into a node with no REG layer yet
    // appends one and lands the rows in BOTH architecture views.
    void importIntoANodeWithoutARegLayerLandsTheRows()
    {
        Fixture F(this);
        const int n = F.addRegNode();
        F.setLayers(n, json::array());             // nothing to lose
        F.save();

        F.pickThisFile(F.writeReg("good.reg"));
        F.act->perform(F.nodeId(n), "import_reg");
        QTest::qWait(100);

        const json Ls = F.node(n)["LAYERS"];
        QVERIFY2(Ls.is_array() && Ls.size() == 1 && Ls[0].contains("REG"), Ls.dump().c_str());
        QCOMPARE(Ls[0]["ARCH"], json::array({"32", "64"}));
        const std::string Dump = Ls[0]["REG"].dump();
        QVERIFY2(Dump.find("TONICT") != std::string::npos, Dump.c_str());
        QVERIFY2(Dump.find("1.00")   != std::string::npos, Dump.c_str());
    }

    // An existing REG layer is merged into, not replaced: its own rows and its views stay.
    void importMergesIntoTheFirstRegLayer()
    {
        Fixture F(this);
        const int n = F.addRegNode();
        F.setLayers(n, json::parse(R"([{"ZIP":"a.zip"},
            {"REG":{"HKLM":{"Software":{"Mine":{"Keep":"precious"}}}},"ARCH":["64"]}])"));
        F.save();

        F.pickThisFile(F.writeReg("good.reg"));
        F.act->perform(F.nodeId(n), "import_reg");
        QTest::qWait(100);

        const json L = F.node(n)["LAYERS"][1];
        QCOMPARE(L["ARCH"], json::array({"64"}));
        QCOMPARE(L["REG"]["HKLM"]["Software"]["Mine"]["Keep"], json("precious"));
        QVERIFY2(L["REG"].dump().find("TONICT") != std::string::npos, L.dump().c_str());
        QCOMPARE((int)F.node(n)["LAYERS"].size(), 2);   // no second REG layer
    }

    // A LAYERS that is not a list is the author's own hand-written shape: Import must refuse it, say why, and change
    // nothing ON DISK — the destruction used to be followed by SaveNodes(), and the author reaches for Import
    // precisely when the node is in a state they are trying to repair.
    void importRefusesALayersThatIsNotAList()
    {
        Fixture F(this);
        const int n = F.addRegNode();
        const json Hand = json::parse(R"({"REG":{"HKLM":{"Software":{"Mine":{"Keep":"precious"}}}}})");
        F.setLayers(n, Hand);
        F.save();

        F.pickThisFile(F.writeReg("good.reg"));
        F.act->perform(F.nodeId(n), "import_reg");
        QTest::qWait(100);

        QCOMPARE(F.node(n)["LAYERS"], Hand);              // in memory...
        const json Saved = json::parse(F.read(F.path(F.nodeFile(n))).toStdString());
        QCOMPARE(nlohmann::json(Saved["LAYERS"]), nlohmann::json(Hand));   // canonical on disk: keys sorted                            // ...and on disk
        QVERIFY2(F.notices.join(" ").contains("not a list"), qUtf8Printable(F.notices.join(" ")));
    }

    // The refusal holds one level down too: a REG layer whose tree is not an object. RegRowsInto would iterate it as
    // one and write back {"": ..., "HKLM": {...}} — the shape the canvas refuses to touch, reshaped and saved.
    void importRefusesARegLayerThatIsNotAHiveTree()
    {
        Fixture F(this);
        const int n = F.addRegNode();
        const json Hand = json::parse(R"([{"REG":["a hand-written entry"],"ARCH":["32"]}])");
        F.setLayers(n, Hand);
        F.save();

        F.pickThisFile(F.writeReg("good.reg"));
        F.act->perform(F.nodeId(n), "import_reg");
        QTest::qWait(100);

        QCOMPARE(F.node(n)["LAYERS"], Hand);
        const json Saved = json::parse(F.read(F.path(F.nodeFile(n))).toStdString());
        QCOMPARE(nlohmann::json(Saved["LAYERS"]), nlohmann::json(Hand));   // canonical on disk: keys sorted
        QVERIFY2(F.notices.join(" ").contains("not a hive tree"), qUtf8Printable(F.notices.join(" ")));
    }

    // A null tree IS materialised — there is nothing there to lose — and the layer's views stay as they are.
    void importIntoANullTreeLandsTheRows()
    {
        Fixture F(this);
        const int n = F.addRegNode();
        F.setLayers(n, json::parse(R"([{"REG":null,"ARCH":["32","64"]}])"));
        F.save();

        F.pickThisFile(F.writeReg("good.reg"));
        F.act->perform(F.nodeId(n), "import_reg");
        QTest::qWait(100);

        const json L = F.node(n)["LAYERS"][0];
        QVERIFY2(L["REG"].is_object() && L["REG"].dump().find("TONICT") != std::string::npos, L.dump().c_str());
        QCOMPARE(L["ARCH"], json::array({"32", "64"}));
    }

    // ------------------------------------------------------------------------------------------------
    // make delta / cover — generation 6 shapes
    // ------------------------------------------------------------------------------------------------

    // "-> delta" is offered — and the action runs — only for a zip whose node contains, EARLIER in its LAYERS and whole,
    // a node with a zip at the SAME target: the version it is a newer version of. A base at another target, placed
    // elsewhere or narrowed by a TAKE, or contained AFTER the content, is not the version before it.
    void aDeltaBaseIsTheZipBeneathAtTheSameTarget()
    {
        const auto nodes = [](const json &RefLayer, bool After, const char *BaseTarget) {
            json Base = {{"CID", "hB"}, {"LABEL", "b"}, {"LAYERS", json::array({ {{"ZIP", "base.zip"}, {"TARGET", BaseTarget}} })}};
            json Own = json::array({ {{"ZIP", "new.zip"}, {"TARGET", "FILES/C:/g"}} });
            if (After) Own.push_back(RefLayer); else Own.insert(Own.begin(), RefLayer);
            return json::array({ Base, json{{"CID", "hN"}, {"LABEL", "n"}, {"LAYERS", Own}} });
        };
        const auto base = [](const json &Arr) {
            std::vector<PkgGraph::NodeRef> R;
            for (const auto &N : Arr) R.push_back({N["CID"].get<std::string>(), &N});
            return R;
        };
        auto db = [&](const json &Arr) { return PkgGraph::DeltaBase(base(Arr), 1); };
        QCOMPARE(db(nodes({{"NODE", "hB"}}, false, "FILES/C:/g")), 0);
        QCOMPARE(db(nodes({{"NODE", "hB"}}, true, "FILES/C:/g")), -1);       // after it
        QCOMPARE(db(nodes({{"NODE", "hB"}}, false, "FILES/C:/other")), -1);  // elsewhere
        QCOMPARE(db(nodes({{"NODE", "hB"}, {"TARGET", "FILES/x"}}, false, "FILES/C:/g")), -1);
        QCOMPARE(db(nodes({{"NODE", "hB"}, {"TAKE", json::array({"FILES/a"})}}, false, "FILES/C:/g")), -1);
        QCOMPARE(db(nodes({{"ANY", json::array({"hB"})}}, false, "FILES/C:/g")), -1);  // not contained
        // The button asks the same question as the action.
        auto offers = [&](const json &D) {
            const bool HasBase = PkgGraph::ContentType(D[1]) == "ZIP" && PkgGraph::DeltaBase(base(D), 1) >= 0;   // as the canvas asks
            for (const auto &A : PkgGraph::ActionsFor(D[1], HasBase, {})) if (std::string(A.Id) == "to_delta") return true;
            return false;
        };
        QVERIFY(offers(nodes({{"NODE", "hB"}}, false, "FILES/C:/g")));
        QVERIFY(!offers(nodes({{"NODE", "hB"}}, false, "FILES/C:/other")));
    }

    // "-> delta" names the base it diffed against by CID (BASE_SOURCES): the base's SOURCE, computed and stamped on the
    // base when it had none (an unpublished zip). Undelta rebuilds over exactly what is named and leaves a plain zip
    // carrying no bases. Teeth: name nothing; leave the base without the CID it is named by; rebuild over another zip.
    void aDeltaNamesItsBaseByCidAndUndeltaReadsIt()
    {
        if (!HaveZipTools) QSKIP("zip/unzip not installed");
        Fixture F(this);
        QByteArray Body(6000, 'g');
        for (int K = 0; K < Body.size(); ++K) Body[K] = char('a' + (K * 7) % 26);
        F.makeZip("v1.zip", "game.bin", Body);
        F.makeZip("v2.zip", "game.bin", Body + "VERSION-TWO");
        F.makeZip("decoy.zip", "other.bin", Body + "DECOY");
        const QByteArray V2 = F.read(F.path("v2.zip"));
        const int d = F.addContentNode("decoy.zip");
        const int b = F.addContentNode("v1.zip");
        const int n = F.addContentNode("v2.zip");
        F.setLayers(n, json::array({ json{{"NODE", F.nodeId(d).toStdString()}}, json{{"NODE", F.nodeId(b).toStdString()}},
                                     json{{"ZIP", "v2.zip"}} }));
        F.save();
        const std::string Want = IpfsWrapper::ComputeCid(F.path("v1.zip").toStdString());
        QVERIFY(!Want.empty());

        F.act->perform(F.nodeId(n), "to_delta");
        QVERIFY(F.waitIdle(F.nodeId(n), 60000));
        QVERIFY2(F.notices.isEmpty(), qUtf8Printable(F.notices.join("\n")));
        const json D = F.node(n)["LAYERS"][(size_t)PkgGraph::ContentIndex(F.node(n))];
        QCOMPARE(PkgGraph::LayerType(D), std::string("DELTA"));
        QCOMPARE(D.value("BASE_SOURCES", json()), json::array({ Want }));
        QCOMPARE(F.node(b)["LAYERS"][0].value("SOURCE", json()), json(Want));
        QVERIFY(!F.node(d)["LAYERS"][0].contains("SOURCE"));          // only the base is named

        F.act->perform(F.nodeId(n), "undelta");
        QVERIFY(F.waitIdle(F.nodeId(n), 60000));
        QVERIFY2(F.notices.isEmpty(), qUtf8Printable(F.notices.join("\n")));
        const json Z = F.node(n)["LAYERS"][(size_t)PkgGraph::ContentIndex(F.node(n))];
        QCOMPARE(PkgGraph::LayerType(Z), std::string("ZIP"));
        QVERIFY2(!Z.contains("BASE_SOURCES"), Z.dump().c_str());
        QVERIFY(F.read(F.path(QString::fromStdString(Z["ZIP"].get<std::string>()))) == V2);
    }

    // Undelta turns the content layer back into a ZIP in place: its TARGET stays, the stale SOURCE/SIZE go. The
    // action's end state is what this pins (the reconstruction itself is vgdelta's, tested there).
    void aContentRewriteKeepsItsPlaceAndDropsTheOldBytesIdentity()
    {
        if (!HaveZipTools) QSKIP("zip/unzip not installed");
        Fixture F(this);
        F.makeZip("content.zip", "a.txt", "bytes");
        const int n = F.addContentNode("content.zip");
        F.setLayers(n, json::parse(R"([{"ENV":{"A":"1"}},
            {"ZIP":"content.zip","TARGET":"FILES/C:/g","SOURCE":"bafkreiold","SIZE":5,"WHEN":"%X% == 1"}])"));
        F.save();
        F.act->perform(F.nodeId(n), "to_dir");
        QVERIFY(F.waitIdle(F.nodeId(n)));
        const json L = F.node(n)["LAYERS"][1];
        QCOMPARE(PkgGraph::LayerType(L), std::string("DIR"));
        QCOMPARE(L["DIR"], json("content"));
        QCOMPARE(L["TARGET"], json("FILES/C:/g"));
        QCOMPARE(L["WHEN"], json("%X% == 1"));
        QVERIFY2(!L.contains("SOURCE") && !L.contains("SIZE"), L.dump().c_str());
        QCOMPARE(F.node(n)["LAYERS"][0], json::parse(R"({"ENV":{"A":"1"}})"));   // other layers untouched
    }

    // A cover belongs to a TILE on an EXEC entry: the picked image lands in that entry's TILE.COVER.FILE (the entry
    // carrying a TILE, not merely the first), keeping a string COVER a string; a malformed COVER is refused.
    void aCoverLandsOnTheTiledEntry()
    {
        Fixture F(this);
        F.write(F.path("cover.png"), "png");
        const int n = F.addRegNode();
        F.setLayers(n, json::parse(R"([{"EXEC":[{"LABEL":"Setup","HOST":"win32"},
            {"LABEL":"Play","HOST":"win32","TILE":{"UID":"1","COVER":{"FILE":"old.png","SOURCE":"bafkreiold"}}}]}])"));
        F.save();
        F.pickThisFile(F.path("cover.png"));
        F.act->perform(F.nodeId(n), "browse_cover");
        QTest::qWait(50);
        const json E = F.node(n)["LAYERS"][0]["EXEC"];
        QCOMPARE(E[1]["TILE"]["COVER"], json::parse(R"({"FILE":"cover.png"})"));
        QVERIFY(!E[0].contains("TILE"));

        { json N = F.node(n); N["LAYERS"][0]["EXEC"][1]["TILE"]["COVER"] = "old.png"; F.setNode(n, N); }
        F.act->perform(F.nodeId(n), "browse_cover");
        QTest::qWait(50);
        QCOMPARE(F.node(n)["LAYERS"][0]["EXEC"][1]["TILE"]["COVER"], json("cover.png"));

        { json N = F.node(n); N["LAYERS"][0]["EXEC"][1]["TILE"]["COVER"] = 5; F.setNode(n, N); }
        F.notices.clear();
        F.act->perform(F.nodeId(n), "browse_cover");
        QTest::qWait(50);
        QCOMPARE(F.node(n)["LAYERS"][0]["EXEC"][1]["TILE"]["COVER"], json(5));
        QVERIFY2(F.notices.join(" ").contains("not an object"), qUtf8Printable(F.notices.join(" ")));
    }

    // A node presenting two tiles (a game and its expansion) asks which one the cover is for, naming each tile once (a
    // mission pack's row of the game's tile is the same tile), and the image lands on that one alone; declined, nothing
    // changes. Teeth: never ask (the first tile wins); ask but put it on the first; list every entry.
    void aCoverAsksWhichOfTheNodesTiles()
    {
        Fixture F(this);
        QTemporaryDir Dl; QVERIFY(Dl.isValid());
        QVERIFY(QImage(600, 900, QImage::Format_RGB32).save(Dl.path() + "/aoc.png"));
        const int n = F.addRegNode();
        F.setLayers(n, json::parse(R"([{"EXEC":[{"LABEL":"Play","HOST":"win32","TILE":{"UID":"1","TITLE":"Base"}},
            {"LABEL":"Mission pack","HOST":"win32","TILE":{"UID":"1"}},
            {"LABEL":"Expansion","HOST":"win32","TILE":{"UID":"2","PARENTUID":"1"}}]}])"));
        F.save();
        QStringList Offered; int Answer = 1;
        F.act->setChooseHandler([&](const QString &, const QString &, const QStringList &O) { Offered = O; return Answer; });
        F.pickThisFile(Dl.path() + "/aoc.png");
        F.act->perform(F.nodeId(n), "browse_cover");
        QTest::qWait(50);
        QCOMPARE(Offered, QStringList({"Base", "Expansion"}));
        const json E = F.node(n)["LAYERS"][0]["EXEC"];
        QCOMPARE(E[2]["TILE"]["COVER"], json::parse(R"({"FILE":"aoc.png"})"));
        QVERIFY(!E[0]["TILE"].contains("COVER") && !E[1]["TILE"].contains("COVER"));
        Answer = -1;
        { json N = F.node(n); N["LAYERS"][0]["EXEC"][2]["TILE"].erase("COVER"); F.setNode(n, N); }
        F.act->perform(F.nodeId(n), "browse_cover");
        QTest::qWait(50);
        QVERIFY(!F.node(n)["LAYERS"][0]["EXEC"][2]["TILE"].contains("COVER"));
    }

    // A cover picked from anywhere (a download) is copied into the package under its own name and named by the tile;
    // the same image already there is reused; ANOTHER image of that name is never replaced. Teeth: refuse outside
    // images (the old rule); overwrite on a name clash.
    void aCoverFromElsewhereIsCopiedIn()
    {
        Fixture F(this);
        QTemporaryDir Dl; QVERIFY(Dl.isValid());
        F.write(Dl.path() + "/box.png", "the box art");
        const int n = F.addRegNode();
        F.setLayers(n, json::parse(R"([{"EXEC":[{"LABEL":"Play","HOST":"win32","TILE":{"UID":"1"}}]}])"));
        F.save();
        F.pickThisFile(Dl.path() + "/box.png");
        F.act->perform(F.nodeId(n), "browse_cover");
        QTest::qWait(50);
        QCOMPARE(F.node(n)["LAYERS"][0]["EXEC"][0]["TILE"]["COVER"], json::parse(R"({"FILE":"box.png"})"));
        QCOMPARE(F.read(F.path("box.png")), QByteArray("the box art"));
        QVERIFY(QFile::exists(Dl.path() + "/box.png"));
        F.act->perform(F.nodeId(n), "browse_cover");      // again: the copy there is the same image
        QTest::qWait(50);
        QVERIFY2(F.notices.isEmpty(), qUtf8Printable(F.notices.join(" ")));

        F.write(Dl.path() + "/box.png", "a different image");
        { json N = F.node(n); N["LAYERS"][0]["EXEC"][0]["TILE"].erase("COVER"); F.setNode(n, N); }
        F.act->perform(F.nodeId(n), "browse_cover");
        QTest::qWait(50);
        QCOMPARE(F.read(F.path("box.png")), QByteArray("the box art"));
        QVERIFY(!F.node(n)["LAYERS"][0]["EXEC"][0]["TILE"].contains("COVER"));
        QVERIFY(F.notices.join(" ").contains("already has another"));
    }

    // The shelf shows covers at 600x900: one that size goes on unasked; any other size (or a file that is no image) is
    // used only when the author says so - declined, the tile is unchanged. Teeth: never ask; ask about every cover.
    void aCoverOfAnotherSizeIsUsedOnlyWhenAsked()
    {
        Fixture F(this, /*confirm=*/false);
        QTemporaryDir Dl; QVERIFY(Dl.isValid());
        QVERIFY(QImage(600, 900, QImage::Format_RGB32).save(Dl.path() + "/right.png"));
        QVERIFY(QImage(460, 215, QImage::Format_RGB32).save(Dl.path() + "/wide.png"));
        const int n = F.addRegNode();
        F.setLayers(n, json::parse(R"([{"EXEC":[{"LABEL":"Play","HOST":"win32","TILE":{"UID":"1"}}]}])"));
        F.save();
        F.pickThisFile(Dl.path() + "/wide.png");
        F.act->perform(F.nodeId(n), "browse_cover");
        QTest::qWait(50);
        QVERIFY(!F.node(n)["LAYERS"][0]["EXEC"][0]["TILE"].contains("COVER"));
        QVERIFY(!QFile::exists(F.path("wide.png")));
        F.pickThisFile(Dl.path() + "/right.png");
        F.act->perform(F.nodeId(n), "browse_cover");
        QTest::qWait(50);
        QCOMPARE(F.node(n)["LAYERS"][0]["EXEC"][0]["TILE"]["COVER"], json::parse(R"({"FILE":"right.png"})"));
    }

    // GOG's install script, as layers: the registry values first (one REG, ARCH the game's, "$1" a hex DWORD, {app}
    // its folder as a Windows path), a file it writes and a text default it copies in as Overwrite seeds, a binary one
    // copied into the package, the save folder kept last; a base64 binary value (a CD key) becomes "hex:", a step that
    // only clears a key is dropped silently; its ini lines are ConfigWrites; Inno's escaped "{{" is a literal brace. GOG Galaxy's keys, installers and other languages' steps are left out and
    // named. Teeth: keep Galaxy's key; read "$1" as decimal text; put KEEP before the seeds; take every language; copy
    // text files; skip binary values; name the key-only step; skip ini lines; keep the drive; refuse Inno's "{{".
    void gogsInstallScriptBecomesLayers()
    {
        QTemporaryDir Sup; QVERIFY(Sup.isValid());
        {
            QFile T(Sup.path() + "/userdocs/user.ltx"); QDir().mkpath(Sup.path() + "/userdocs");
            QVERIFY(T.open(QIODevice::WriteOnly)); T.write("_preset Extreme\r\nbind left kLEFT\r\n");
        }
        { QFile B(Sup.path() + "/userdocs/traffic.net"); QVERIFY(B.open(QIODevice::WriteOnly)); B.write(QByteArray("\x01\x00\x02", 3)); }
        const json Script = json::parse(R"({"actions":[
          {"install":{"action":"saveToFile","arguments":{"filename":"{app}/fsgame.ltx","text":"$app_data_root$ = true | false| {userdocs}\\Stalker-SHOC\n"}},"languages":["*"],"name":"Generate fsgame.ltx"},
          {"install":{"action":"execute","arguments":{"argument":"+r fsgame.ltx","executable":"{sys}/attrib.exe"}},"languages":["*"],"name":"readonly"},
          {"install":{"action":"supportData","arguments":{"source":"{supportDir}/userdocs/user.ltx","target":"{userdocs}/STALKER-SHOC/user.ltx","type":"File"}},"languages":["*"]},
          {"install":{"action":"supportData","arguments":{"source":"{supportDir}/userdocs/traffic.net","target":"{userdocs}/STALKER-SHOC/traffic.net","type":"File"}},"languages":["*"]},
          {"install":{"action":"savePath","arguments":{"savePath":"{userdocs}/STALKER-SHOC","type":"Folder"}},"languages":["*"]},
          {"install":{"action":"setRegistry","arguments":{"root":"HKEY_LOCAL_MACHINE","subkey":"SOFTWARE\\GSC Game World\\STALKER-SHOC","valueData":"{app}","valueName":"InstallPath","valueType":"string"}},"languages":["*"]},
          {"install":{"action":"setRegistry","arguments":{"root":"HKEY_LOCAL_MACHINE","subkey":"SOFTWARE\\GSC Game World\\STALKER-SHOC","valueData":"$1a","valueName":"BonusPack1","valueType":"dword"}},"languages":["*"]},
          {"install":{"action":"setRegistry","arguments":{"root":"HKEY_LOCAL_MACHINE","subkey":"Software\\GOG.com\\Games\\{productID}","valueData":"x","valueName":"path","valueType":"String"}},"languages":["*"]},
          {"install":{"action":"setRegistry","arguments":{"root":"HKEY_CURRENT_USER","subkey":"Software\\G","valueData":"Deutsch","valueName":"Lang","valueType":"string"}},"languages":["de-DE"]},
          {"install":{"action":"setRegistry","arguments":{"deleteSubkeys":true,"root":"HKLM","subkey":"Software\\Sierra\\Old"}},"languages":["*"]},
          {"install":{"action":"setRegistry","arguments":{"root":"HKLM","subkey":"Software\\Sierra\\CDKeys","valueData":"4wBq","valueName":"EmpireEarth","valueType":"binary"}},"languages":["*"]},
          {"install":{"action":"setRegistry","arguments":{"root":"HKLM","subkey":"Software\\Sierra\\CDKeys","valueData":"\u007b{99797420-F5F5}","valueName":"Guid","valueType":"string"}},"languages":["*"]},
          {"install":{"action":"setIni","arguments":{"filename":"{app}\\icewind2.ini","keyName":"HD0:","keyType":"string","keyValue":"{app}","section":"Alias"}},"languages":["*"]},
          {"install":{"action":"setIni","arguments":{"filename":"{app}\\icewind2.ini","keyName":"CD1:","keyType":"string","keyValue":"{app}\\Data\\","section":"Alias"}},"languages":["*"]},
          {"install":{"action":"setIni","arguments":{"filename":"{app}\\icewind2.ini","keyName":"CD2:","keyType":"string","keyValue":"{app}\\CD2\\;G:\\CD2\\","section":"Alias"}},"languages":["*"]}
        ]})");
        const auto G = PkgActions::GogInstallSteps(Script, Sup.path().toStdString(), "32");
        QCOMPARE((int)G.Layers.size(), 6);
        QCOMPARE(G.Layers[0]["REG"], json::parse(R"({"HKLM":{"SOFTWARE":{"GSC Game World":{"STALKER-SHOC":{"InstallPath":"%GameDir:winpath%","BonusPack1":"dword:0000001a"}}},
                                                       "Software":{"Sierra":{"CDKeys":{"EmpireEarth":"hex:e3,00,6a","Guid":"{99797420-F5F5}"}}}}})"));
        QCOMPARE(G.Layers[0]["ARCH"], json::array({"32"}));
        QCOMPARE(G.Layers[1]["TARGET"], json("FILES/%GameDir%/fsgame.ltx"));
        QCOMPARE(G.Layers[1]["EDIT"][0]["VALUE"], json("$app_data_root$ = true | false| %Documents:winpath%\\Stalker-SHOC\n"));
        QCOMPARE(G.Layers[2]["TARGET"], json("FILES/%Documents%/STALKER-SHOC/user.ltx"));
        QCOMPARE(G.Layers[2]["EDIT"][0]["MODE"], json("Overwrite"));
        QCOMPARE(G.Layers[3]["FILE"], json("traffic.net"));
        QCOMPARE(G.Layers[3]["TARGET"], json("FILES/%Documents%/STALKER-SHOC"));
        QCOMPARE((int)G.Copies.size(), 1);
        //GOG's ini lines, one EDIT per file after the files it writes: another machine's drive ("G:\\") is left out, named.
        QCOMPARE(G.Layers[4]["TARGET"], json("FILES/%GameDir%/icewind2.ini"));
        QCOMPARE(G.Layers[4]["EDIT"], json::parse(R"([{"MODE":"ConfigWrite","KEY":"HD0:=","VALUE":"%GameDir:winpath%"},
                                                      {"MODE":"ConfigWrite","KEY":"CD1:=","VALUE":"%GameDir:winpath%\\Data\\"}])"));
        QCOMPARE(G.Layers[5]["KEEP"], json::parse(R"({"FILES/%Documents%/STALKER-SHOC/":true})"));
        QCOMPARE((int)G.Skipped.size(), 3);                       // attrib, Galaxy's key, the drive; the German step not even named
        QVERIFY(!PkgActions::GogInstallSteps(Script, Sup.path().toStdString(), "").Layers[0].contains("ARCH"));   // unknown: left to the author
    }

    // A GOG install brought in as a folder: its installer's own folders stay out of the zip, and its install steps
    // are offered and land on the node, the registry at the game's bitness. Declined, the node has only the files and
    // its program. Teeth: pack __redist; skip the offer; ignore the answer.
    void bringingInAGogInstallOffersItsSteps()
    {
        if (!HaveZipTools) QSKIP("zip/unzip not installed");
        auto Pe32 = [] { std::string H(256, '\0'); H[0] = 'M'; H[1] = 'Z'; H[0x3C] = 0x40; H.replace(0x40, 4, std::string("PE\0\0", 4)); H[0x44] = 0x4c; H[0x45] = 0x01; return QByteArray(H.data(), (int)H.size()); };
        for (int Answer : {0, 1})
        {
            Fixture F(this);
            QTemporaryDir Src; QVERIFY(Src.isValid());
            const QString G = Src.path() + "/G";
            F.write(G + "/bin/Game.exe", Pe32());
            F.write(G + "/goggame-1.info", R"({"playTasks":[{"isPrimary":true,"category":"game","path":"bin\\Game.exe"}]})");
            F.write(G + "/goggame-1.script", R"({"actions":[{"install":{"action":"setRegistry","arguments":{"root":"HKEY_LOCAL_MACHINE","subkey":"SOFTWARE\\Dev\\Game","valueData":"{app}","valueName":"InstallPath","valueType":"string"}},"languages":["*"]},
                {"install":{"action":"savePath","arguments":{"savePath":"{userdocs}/Game","type":"Folder"}},"languages":["*"]}]})");
            F.write(G + "/__redist/vcredist.exe", Pe32());
            F.write(G + "/__support/userdocs/user.cfg", "a=1");
            const int n = F.addContentNode("");
            F.setLayers(n, json::array({ json{{"ZIP", ""}, {"TARGET", "FILES/%GameDir%"}}, json{{"EXEC", json::array({ json{{"LABEL", "Play"}} })}} }));
            F.save();
            F.act->setChooseHandler([Answer](const QString &T, const QString &, const QStringList &) { return T.contains("GOG") ? Answer : 0; });
            F.act->dropFiles(F.nodeId(n), {G});
            QVERIFY(F.waitIdle(F.nodeId(n)));
            QTest::qWait(50);
            int Err = 0;
            zip_t *Z = zip_open(F.path("G.zip").toUtf8().constData(), ZIP_RDONLY, &Err);
            QVERIFY(Z);
            zip_stat_t St;
            QCOMPARE(zip_stat(Z, "bin/Game.exe", 0, &St), 0);
            QVERIFY(zip_stat(Z, "__redist/vcredist.exe", 0, &St) != 0);
            QVERIFY(zip_stat(Z, "__support/userdocs/user.cfg", 0, &St) != 0);
            zip_close(Z);
            const json L = F.node(n)["LAYERS"];
            QCOMPARE(L[1]["EXEC"][0]["HOST"], json("win32"));
            if (Answer == 0)
            {
                QCOMPARE((int)L.size(), 4);
                QCOMPARE(L[2]["ARCH"], json::array({"32"}));
                QCOMPARE(L[3]["KEEP"], json::parse(R"({"FILES/%Documents%/Game/":true})"));
            }
            else QCOMPARE((int)L.size(), 2);
        }
    }

    // An extracted GOG installer comes in named for its game - GOG's name, trademark signs and path characters out,
    // "(GOG)" after - not for the folder it was unpacked to; and GOG's app\ and __support\app\ (what its installer puts
    // in the game folder) are packed at the top, GOG Galaxy's web cache and icon and the rest of __support\ left out.
    // Teeth: name it after the folder ("x.zip"); keep app\ as a folder; keep webcache.zip; drop __support\app\.
    void bringingInAGogInstallNamesItForTheGameAndOpensItsAppFolders()
    {
        if (!HaveZipTools) QSKIP("zip/unzip not installed");
        Fixture F(this);
        QTemporaryDir Src; QVERIFY(Src.isValid());
        const QString G = Src.path() + "/x";
        F.write(G + "/Game.exe", "MZ");
        F.write(G + "/goggame-7.info", "{\"name\": \"Dungeon Keeper™ 2: Gold\", \"playTasks\": []}");
        F.write(G + "/app/Data/Text/speech.dat", "speech");
        F.write(G + "/app/webcache.zip", "cache");
        F.write(G + "/app/goggame-7.ico", "icon");
        F.write(G + "/__support/app/IWD2.exe", "MZ support");   // what GOG's script puts in the game folder
        F.write(G + "/__support/userdocs/user.ltx", "not the game folder's");
        const int n = F.addContentNode("");
        F.setLayers(n, json::array({ json{{"ZIP", ""}, {"TARGET", "FILES/%GameDir%"}} }));
        F.save();
        F.act->setChooseHandler([](const QString &, const QString &, const QStringList &) { return 1; });   // no GOG steps
        F.act->dropFiles(F.nodeId(n), {G});
        QVERIFY(F.waitIdle(F.nodeId(n)));
        QTest::qWait(50);
        QCOMPARE(F.name(n), std::string("Dungeon Keeper 2 Gold (GOG).zip"));
        int Err = 0;
        zip_t *Z = zip_open(F.path("Dungeon Keeper 2 Gold (GOG).zip").toUtf8().constData(), ZIP_RDONLY, &Err);
        QVERIFY(Z);
        zip_stat_t St;
        QCOMPARE(zip_stat(Z, "Game.exe", 0, &St), 0);
        QCOMPARE(zip_stat(Z, "Data/Text/speech.dat", 0, &St), 0);   // app\'s files at the top
        QCOMPARE((int)St.comp_method, (int)ZIP_CM_STORE);
        QVERIFY(zip_stat(Z, "app/Data/Text/speech.dat", 0, &St) != 0);
        QVERIFY(zip_stat(Z, "webcache.zip", 0, &St) != 0);
        QVERIFY(zip_stat(Z, "goggame-7.ico", 0, &St) != 0);
        QCOMPARE(zip_stat(Z, "IWD2.exe", 0, &St), 0);                 // __support\app\'s files at the top too
        QVERIFY(zip_stat(Z, "__support/app/IWD2.exe", 0, &St) != 0);
        QVERIFY(zip_stat(Z, "user.ltx", 0, &St) != 0);
        QVERIFY(zip_stat(Z, "userdocs/user.ltx", 0, &St) != 0);
        zip_close(Z);

        //An app\ holding only GOG Galaxy's files (Icewind Dale II) has nothing to pack: the rest still comes in; and GOG's
        //info file found in app\ (newer installers) names it all the same.
        QTemporaryDir Src2; QVERIFY(Src2.isValid());
        const QString G2 = Src2.path() + "/x";
        F.write(G2 + "/Data/a.bif", "bif");
        F.write(G2 + "/app/goggame-8.info", "{\"name\": \"Icewind Dale 2\", \"playTasks\": []}");   // in app\ (UT2004's installer)
        F.write(G2 + "/app/webcache.zip", "cache");
        F.write(G2 + "/app/goggame-8.ico", "icon");
        F.write(G2 + "/__support/app/IWD2.exe", "MZ");
        const int n2 = F.addContentNode("");
        F.setLayers(n2, json::array({ json{{"ZIP", ""}, {"TARGET", "FILES/%GameDir%"}} }));
        F.save();
        F.act->dropFiles(F.nodeId(n2), {G2});
        QVERIFY(F.waitIdle(F.nodeId(n2)));
        QTest::qWait(50);
        QCOMPARE(F.name(n2), std::string("Icewind Dale 2 (GOG).zip"));
        Z = zip_open(F.path("Icewind Dale 2 (GOG).zip").toUtf8().constData(), ZIP_RDONLY, &Err);
        QVERIFY(Z);
        QCOMPARE(zip_stat(Z, "IWD2.exe", 0, &St), 0);
        QCOMPARE(zip_stat(Z, "Data/a.bif", 0, &St), 0);
        zip_close(Z);

        //Everything inside app\, __support\ too (Unreal Tournament 2004's installer): app\'s files and its support
        //files at the top, nothing else to pack beside them, the support folder itself left out.
        QTemporaryDir Src3; QVERIFY(Src3.isValid());
        const QString G3 = Src3.path() + "/x";
        F.write(G3 + "/app/System/UT2004.exe", "MZ");
        F.write(G3 + "/app/goggame-9.info", "{\"name\": \"Unreal Tournament 2004\", \"playTasks\": []}");
        F.write(G3 + "/app/__support/app/System/Default.ini", "[FirstRun]");
        F.write(G3 + "/tmp/slide.jpg", "installer");
        const int n3 = F.addContentNode("");
        F.setLayers(n3, json::array({ json{{"ZIP", ""}, {"TARGET", "FILES/%GameDir%"}} }));
        F.save();
        F.act->dropFiles(F.nodeId(n3), {G3});
        QVERIFY(F.waitIdle(F.nodeId(n3)));
        QTest::qWait(50);
        QCOMPARE(F.name(n3), std::string("Unreal Tournament 2004 (GOG).zip"));
        Z = zip_open(F.path("Unreal Tournament 2004 (GOG).zip").toUtf8().constData(), ZIP_RDONLY, &Err);
        QVERIFY(Z);
        QCOMPARE(zip_stat(Z, "System/UT2004.exe", 0, &St), 0);
        QCOMPARE(zip_stat(Z, "System/Default.ini", 0, &St), 0);
        QVERIFY(zip_stat(Z, "__support/app/System/Default.ini", 0, &St) != 0);
        QVERIFY(zip_stat(Z, "slide.jpg", 0, &St) != 0 && zip_stat(Z, "tmp/slide.jpg", 0, &St) != 0);
        zip_close(Z);
    }

    // Files brought in to a node with a content layer waiting for them (one just added, empty) go there without a
    // question, the files it already names left as they are; a FILE layer brings in one file, copied into the package.
    // Teeth: ask "add or replace" anyway; fill the first content layer; offer a folder or a zip to a FILE layer.
    void aWaitingLayerTakesItsFilesAndAFileLayerOneFile()
    {
        if (!HaveZipTools) QSKIP("zip/unzip not installed");
        {
            Fixture F(this);
            F.makeZip("base.zip", "a.txt", "a");
            const int n = F.addContentNode("base.zip");
            F.setLayers(n, json::array({ json{{"ZIP", "base.zip"}}, json{{"ZIP", ""}, {"TARGET", "FILES/%GameDir%/Patch"}} }));
            F.save();
            QTemporaryDir Src; QVERIFY(Src.isValid());
            F.write(Src.path() + "/Patch 2/fix.exe", "MZ");
            QStringList Asked;
            F.act->setChooseHandler([&Asked](const QString &, const QString &Text, const QStringList &) { Asked << Text; return 0; });
            F.pickThisFile(Src.path() + "/Patch 2");
            F.act->perform(F.nodeId(n), "bring_in");
            QVERIFY(F.waitIdle(F.nodeId(n)));
            for (const QString &A : Asked) QVERIFY2(!A.contains("already has files"), qUtf8Printable(A));
            const json L = F.node(n)["LAYERS"];
            QCOMPARE((int)L.size(), 2);
            QCOMPARE(L[0]["ZIP"], json("base.zip"));
            QCOMPARE(L[1]["ZIP"], json("Patch 2.zip"));
            QCOMPARE(L[1]["TARGET"], json("FILES/%GameDir%/Patch"));
        }
        {
            Fixture F(this);
            const int n = F.addContentNode("");
            F.setLayers(n, json::array({ json{{"FILE", ""}, {"TARGET", "FILES/%GameDir%"}} }));
            F.save();
            QTemporaryDir Src; QVERIFY(Src.isValid());
            F.write(Src.path() + "/KEEPER.CFG", "INSTALL_TYPE=MAX\r\n");
            bool OfferedZip = false;
            F.act->setChooseHandler([&OfferedZip](const QString &, const QString &, const QStringList &B) { if (B.contains("A zip...")) OfferedZip = true; return 0; });
            F.pickThisFile(Src.path() + "/KEEPER.CFG");
            F.act->perform(F.nodeId(n), "bring_in");
            QVERIFY(F.waitIdle(F.nodeId(n)));
            QVERIFY(!OfferedZip);
            QCOMPARE(F.type(n), std::string("FILE"));
            QCOMPARE(F.name(n), std::string("KEEPER.CFG"));
            QCOMPARE(F.read(F.path("KEEPER.CFG")), QByteArray("INSTALL_TYPE=MAX\r\n"));
        }
    }

    // A long single-line JSON typed into the JSON pane, broken, gets a parse error quoting what it last read - one
    // unbreakable run as wide as the text. The pane still asks for no more width than it had: it once widened the
    // editor window off the screen. Teeth: let the status label size to its text.
    void theJsonPaneNeverWidensForALongLine()
    {
        Fixture F(this);
        const int n = F.addContentNode("");
        JsonRawEditor E(F.model);
        E.showNode(F.nodeId(n));
        const int Before = E.minimumSizeHint().width();
        auto *T = E.findChild<QPlainTextEdit *>();
        QVERIFY(T);
        T->setPlainText("{\"LABEL\": \"" + QString(3000, 'x') + "\", \"LAYERS\": [");
        QTest::qWait(20);
        QVERIFY2(E.minimumSizeHint().width() <= Before + 50, qUtf8Printable(QString("%1 -> %2").arg(Before).arg(E.minimumSizeHint().width())));
    }

    // app.info is a Unity game's company and product, one per line; anything else (one line, a path in it) is no
    // guide. Teeth: accept one line; let a slash through into the address.
    void aUnityGamesSavesAreReadFromItsAppInfo()
    {
        QVERIFY(PkgActions::UnityKeep("ZAUM Studio\r\nDisco Elysium\r\n")["KEEP"].contains("FILES/%UserProfile%/AppData/LocalLow/ZAUM Studio/Disco Elysium/"));
        QVERIFY(PkgActions::UnityKeep("OnlyOneLine").is_null());
        QVERIFY(PkgActions::UnityKeep("A/B\nC").is_null());
        QVERIFY(PkgActions::UnityKeep("").is_null());
    }

    // What a program runs on, read from its first bytes: the PE machine (i386 / x64), a bare MZ (DOS), ELF's class,
    // and the names that say it. Teeth: read the machine at a fixed offset; call every MZ Windows.
    void aProgramsPlatformIsReadFromItsHeader()
    {
        auto Pe = [](uint16_t Machine) {
            std::string H(256, '\0');
            H[0] = 'M'; H[1] = 'Z';
            H[0x3C] = (char)0x90;                                   // the PE header at 0x90, not where it usually is
            H.replace(0x90, 4, std::string("PE\0\0", 4));
            H[0x94] = (char)(Machine & 0xFF); H[0x95] = (char)(Machine >> 8);
            return H;
        };
        QCOMPARE(PkgActions::PlatformOf("game.exe", Pe(0x14c)), std::string("win32"));
        QCOMPARE(PkgActions::PlatformOf("game.exe", Pe(0x8664)), std::string("win64"));
        std::string Dos(128, '\0'); Dos[0] = 'M'; Dos[1] = 'Z';
        QCOMPARE(PkgActions::PlatformOf("GAME.EXE", Dos), std::string("dos"));
        std::string Elf(64, '\0'); Elf.replace(0, 4, "\x7f" "ELF"); Elf[4] = 2;
        QCOMPARE(PkgActions::PlatformOf("game", Elf), std::string("linux64"));
        QCOMPARE(PkgActions::PlatformOf("start.sh", "#!/bin/sh"), std::string("linux64"));
        QCOMPARE(PkgActions::PlatformOf("notes.exe", "hello"), std::string());
    }

    // Which program is the game: installers, uninstallers and redistributables set aside, the only one left - or the
    // only one at the top of the game's folder; never a guess between several. Teeth: keep the installers; pick the
    // first of several.
    void theLikelyProgramIsTheOnlyRealOne()
    {
        QCOMPARE(PkgActions::LikelyProgram({"%GameDir%/unins000.exe", "%GameDir%/Game.exe", "%GameDir%/vcredist_x86.exe"}), 1);
        QCOMPARE(PkgActions::LikelyProgram({"%GameDir%/bin/tool.exe", "%GameDir%/Game.exe"}), 1);
        QCOMPARE(PkgActions::LikelyProgram({"%GameDir%/a.exe", "%GameDir%/b.exe"}), -1);
        QCOMPARE(PkgActions::LikelyProgram({"%GameDir%/setup.exe"}), -1);
        //A launcher script beside programs is not the program (GTA.BAT starts the DOS build), nor a settings tool;
        //a script alone is. Teeth: let the shallow script win; keep SETTINGS.EXE in the running.
        QCOMPARE(PkgActions::LikelyProgram({"%GameDir%/GTA.BAT", "%GameDir%/gtawin/GRAND THEFT AUTO.EXE", "%GameDir%/gtawin/SETTINGS.EXE"}), 1);
        QCOMPARE(PkgActions::LikelyProgram({"%GameDir%/RUN.BAT", "%GameDir%/setup.exe"}), 0);
    }

    // Files brought in, the package's game entry still without a program gets one when the files make it clear:
    // the only likely program, its folder as the work dir, its platform from its header. A GOG install's play task
    // (path, arguments, working dir) wins over guessing. Teeth: never auto-detect; ignore the GOG info.
    void bringingInFilesFindsTheGamesProgram()
    {
        if (!HaveZipTools) QSKIP("zip/unzip not installed");
        auto Pe32 = [] { std::string H(256, '\0'); H[0] = 'M'; H[1] = 'Z'; H[0x3C] = 0x40; H.replace(0x40, 4, std::string("PE\0\0", 4)); H[0x44] = 0x4c; H[0x45] = 0x01; return QByteArray(H.data(), (int)H.size()); };
        {
            Fixture F(this);
            QTemporaryDir Src; QVERIFY(Src.isValid());
            F.write(Src.path() + "/Demo/Game.exe", Pe32());
            F.write(Src.path() + "/Demo/unins000.exe", Pe32());
            const int n = F.addContentNode("");
            F.setLayers(n, json::array({ json{{"ZIP", ""}, {"TARGET", "FILES/%GameDir%"}},
                                         json{{"EXEC", json::array({ json{{"LABEL", "Play"}, {"EXE", "%GameDir%/"}} })}} }));
            F.save();
            QStringList Said;
            connect(F.act, &PkgActions::status, this, [&Said](const QString &T) { Said << T; });
            F.act->dropFiles(F.nodeId(n), {Src.path() + "/Demo"});
            QVERIFY(F.waitIdle(F.nodeId(n)));
            const json E = F.node(n)["LAYERS"][1]["EXEC"][0];
            QCOMPARE(E["EXE"], json("%GameDir%/Game.exe"));
            QCOMPARE(E["WORKDIR"], json("%GameDir%"));
            QCOMPARE(E["HOST"], json("win32"));
            QCOMPARE(F.node(n)["LAYERS"][0]["ZIP"], json("Demo.zip"));   // the empty ZIP layer took the files
            QVERIFY(Said.join(" ").contains("Game.exe"));
        }
        {
            Fixture F(this);
            QTemporaryDir Src; QVERIFY(Src.isValid());
            F.write(Src.path() + "/G/bin/run.exe", Pe32());
            F.write(Src.path() + "/G/tools/editor.exe", Pe32());
            F.write(Src.path() + "/G/goggame-1207658691.info",
                    R"({"playTasks":[{"category":"tool","path":"tools\\editor.exe"},{"isPrimary":true,"category":"game","path":"bin\\run.exe","arguments":"-window -nointro","workingDir":"bin"}]})");
            const int n = F.addContentNode("");
            F.setLayers(n, json::array({ json{{"ZIP", "base.zip"}, {"TARGET", "FILES/%GameDir%"}},
                                         json{{"EXEC", json::array({ json{{"LABEL", "Play"}, {"HOST", "win32"}} })}} }));
            F.save();
            F.act->dropFiles(F.nodeId(n), {Src.path() + "/G"});
            QVERIFY(F.waitIdle(F.nodeId(n)));
            const json E = F.node(n)["LAYERS"][1]["EXEC"][0];
            QCOMPARE(E["EXE"], json("%GameDir%/bin/run.exe"));
            QCOMPARE(E["ARGS"], json::array({"-window", "-nointro"}));
            QCOMPARE(E["WORKDIR"], json("%GameDir%/bin"));
            QCOMPARE((int)F.node(n)["LAYERS"].size(), 3);           // its files stay; the new ones go in a new layer, last
            QCOMPARE(F.node(n)["LAYERS"][0]["ZIP"], json("base.zip"));
            QCOMPARE(F.node(n)["LAYERS"][2]["ZIP"], json("G.zip"));
        }
        {
            //GOG's play task in another case than the file (TUNIC.exe for Tunic.exe): the file's own case, and its
            //header read for the platform.
            Fixture F(this);
            QTemporaryDir Src; QVERIFY(Src.isValid());
            F.write(Src.path() + "/T/Tunic.exe", Pe32());
            F.write(Src.path() + "/T/UnityCrashHandler64.exe", Pe32());
            F.write(Src.path() + "/T/Editor.exe", Pe32());              // two likely programs: only the task decides
            F.write(Src.path() + "/T/goggame-1.info", R"({"playTasks":[{"isPrimary":true,"category":"game","path":"TUNIC.exe"}]})");
            F.write(Src.path() + "/T/Tunic_Data/app.info", "Andrew Shouldice\nSecret Legend");
            const int n = F.addContentNode("");
            F.setLayers(n, json::array({ json{{"ZIP", ""}, {"TARGET", "FILES/%GameDir%"}},
                                         json{{"EXEC", json::array({ json{{"LABEL", "Play"}} })}} }));
            F.save();
            F.act->dropFiles(F.nodeId(n), {Src.path() + "/T"});
            QVERIFY(F.waitIdle(F.nodeId(n)));
            const json E = F.node(n)["LAYERS"][1]["EXEC"][0];
            QCOMPARE(E["EXE"], json("%GameDir%/Tunic.exe"));
            QCOMPARE(E["HOST"], json("win32"));
            //A Unity game: where it saves, from its app.info, kept - once.
            const json K = F.node(n)["LAYERS"].back();
            QVERIFY2(K.contains("KEEP"), F.node(n).dump().c_str());
            QCOMPARE(K["KEEP"], json::parse(R"({"FILES/%UserProfile%/AppData/LocalLow/Andrew Shouldice/Secret Legend/":true,
                                                "REG/HKCU/Software/Andrew Shouldice/Secret Legend":true})"));
        }
        {
            //A play task naming no program in the files is no guide: the likeliest program is.
            Fixture F(this);
            QTemporaryDir Src; QVERIFY(Src.isValid());
            F.write(Src.path() + "/G/Game.exe", Pe32());
            F.write(Src.path() + "/G/goggame-1.info", R"({"playTasks":[{"isPrimary":true,"category":"game","path":"Launcher.exe"}]})");
            const int n = F.addContentNode("");
            F.setLayers(n, json::array({ json{{"ZIP", ""}, {"TARGET", "FILES/%GameDir%"}},
                                         json{{"EXEC", json::array({ json{{"LABEL", "Play"}} })}} }));
            F.save();
            F.act->dropFiles(F.nodeId(n), {Src.path() + "/G"});
            QVERIFY(F.waitIdle(F.nodeId(n)));
            QCOMPARE(F.node(n)["LAYERS"][1]["EXEC"][0]["EXE"], json("%GameDir%/Game.exe"));
        }
    }

    // Only the node the files came into, or a version over it, gets a program: the tile at the package's base (an
    // entry with no program by design) stays as it is when a runner's files come into a node of their own. Teeth:
    // fill the first waiting entry anywhere in the package; look in the receiving node only.
    void bringingInFilesFillsOnlyTheirOwnNodesEntries()
    {
        if (!HaveZipTools) QSKIP("zip/unzip not installed");
        auto Pe32 = [] { std::string H(256, '\0'); H[0] = 'M'; H[1] = 'Z'; H[0x3C] = 0x40; H.replace(0x40, 4, std::string("PE\0\0", 4)); H[0x44] = 0x4c; H[0x45] = 0x01; return QByteArray(H.data(), (int)H.size()); };
        Fixture F(this);
        F.makeZip("base.zip", "Launcher.exe", Pe32());
        const int b = F.addContentNode("base.zip");
        F.setLayers(b, json::array({ json{{"ZIP", "base.zip"}, {"TARGET", "FILES/%GameDir%"}},
                                     json{{"EXEC", json::array({ json{{"LABEL", "Play"}, {"TILE", json{{"UID", "1"}, {"TITLE", "Demo"}}}} })}} }));
        F.save();
        const json Base = F.node(b);
        QTemporaryDir Src; QVERIFY(Src.isValid());
        F.write(Src.path() + "/Engine/engine.exe", Pe32());
        const int r = F.addContentNode("");
        F.setLayers(r, json::array({ json{{"ZIP", ""}, {"TARGET", "FILES/%GameDir%/eng"}} }));
        F.save();
        F.act->dropFiles(F.nodeId(r), {Src.path() + "/Engine"});
        QVERIFY(F.waitIdle(F.nodeId(r)));
        QCOMPARE(F.node(b), Base);
        //A version over the node the files came into is theirs to fill.
        const int v = F.addContentNode("");
        F.setLayers(v, json::array({ json{{"NODE", F.nodeId(r).toStdString()}},
                                     json{{"EXEC", json::array({ json{{"LABEL", "Play"}, {"TILE", json{{"UID", "1"}}}} })}} }));
        F.save();
        QTemporaryDir More; QVERIFY(More.isValid());
        F.write(More.path() + "/Extra/readme.txt", "hi");
        F.act->dropFiles(F.nodeId(r), {More.path() + "/Extra"});
        QVERIFY(F.waitIdle(F.nodeId(r)));
        QCOMPARE(F.node(v)["LAYERS"][1]["EXEC"][0]["EXE"], json("%GameDir%/eng/engine.exe"));
        QCOMPARE(F.node(b), Base);
    }

    // Dropped files land by what they are: an image becomes the tile's cover, a .reg export a REG layer, anything else
    // is refused with a line saying what can be dropped. Teeth: take any file; drop silently.
    void droppedFilesLandByWhatTheyAre()
    {
        Fixture F(this);
        QTemporaryDir Src; QVERIFY(Src.isValid());
        F.write(Src.path() + "/box.jpg", "jpeg");
        F.write(Src.path() + "/game.reg", "Windows Registry Editor Version 5.00\r\n\r\n[HKEY_CURRENT_USER\\Software\\Demo]\r\n\"Lang\"=\"en\"\r\n");
        F.write(Src.path() + "/readme.txt", "hi");
        const int n = F.addRegNode();
        F.setLayers(n, json::parse(R"([{"EXEC":[{"LABEL":"Play","HOST":"win32","TILE":{"UID":"1"}}]}])"));
        F.save();
        QStringList Said;
        connect(F.act, &PkgActions::status, this, [&Said](const QString &T) { Said << T; });
        F.act->dropFiles(F.nodeId(n), {Src.path() + "/box.jpg", Src.path() + "/game.reg", Src.path() + "/readme.txt"});
        QTest::qWait(50);
        const json N = F.node(n);
        QCOMPARE(N["LAYERS"][0]["EXEC"][0]["TILE"]["COVER"], json::parse(R"({"FILE":"box.jpg"})"));
        bool Reg = false;
        for (const json &L : N["LAYERS"]) if (L.contains("REG")) Reg = true;
        QVERIFY(Reg);
        QVERIFY2(Said.join(" ").contains("readme.txt"), qUtf8Printable(Said.join(" | ")));
        QVERIFY(!QFile::exists(F.path("readme.txt")));
    }

    // What the game sees: the fold's files in order, a later layer winning a path (case-insensitively, as Windows
    // sees it) and the one it covers listed under it; a contained node's files where it is contained; a node of
    // another package noted, not listed. Teeth: first wins; forget what is covered; skip contained nodes.
    void theGameSeesTheFoldsFilesLaterWinning()
    {
        if (!HaveZipTools) QSKIP("zip/unzip not installed");
        Fixture F(this);
        F.makeZip("base.zip", "Data/Level1.dat", "base");
        F.write(F.path("fix/data/level1.DAT"), "fixed");
        F.write(F.path("fix/readme.txt"), "hi");
        const int b = F.addContentNode("base.zip");
        F.setLayers(b, json::array({ json{{"ZIP", "base.zip"}, {"TARGET", "FILES/%GameDir%"}} }));
        { json N = F.node(b); N["LABEL"] = "Base"; F.setNode(b, N); }
        F.save();
        const int v = F.addContentNode("");
        F.setLayers(v, json::array({ json{{"NODE", F.nodeId(b).toStdString()}}, json{{"DIR", "fix"}, {"TARGET", "FILES/%GameDir%"}},
                                     json{{"NODE", "bafkreiotherpackagexxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"}} }));
        { json N = F.node(v); N["LABEL"] = "Fixed"; F.setNode(v, N); }
        F.save();
        std::vector<std::string> Notes;
        const auto Files = PkgActions::GameSees(F.doc(), F.nodeId(v).toStdString(), F.dir->path().toStdString(), &Notes);
        QCOMPARE((int)Files.size(), 2);
        const auto &Lvl = Files[0].Path.find("evel1") != std::string::npos ? Files[0] : Files[1];
        QCOMPARE(Lvl.Path, std::string("%GameDir%/data/level1.DAT"));
        QCOMPARE(Lvl.From, std::string("Fixed > fix"));
        QCOMPARE((int)Lvl.Under.size(), 1);
        QCOMPARE(Lvl.Under[0], std::string("Base > base.zip"));
        QCOMPARE((int)Notes.size(), 1);
        QVERIFY(Notes[0].find("another package") != std::string::npos);

        //Reached through the library, the other package's node is walked like this package's: its files, from its
        //own folder, under its own name. Teeth: resolve its files in this package's folder; keep noting it.
        QTemporaryDir Lib; QVERIFY(Lib.isValid());
        {
            QDir().mkpath(Lib.path() + "/fonts");
            QFile Fo(Lib.path() + "/fonts/arial.ttf"); QVERIFY(Fo.open(QIODevice::WriteOnly)); Fo.write("ttf");
        }
        const json Fonts = json::parse(R"({"LABEL":"Core Fonts","LAYERS":[{"DIR":"fonts","TARGET":"FILES/%Windows%/Fonts"}]})");
        const PkgActions::OtherFn Other = [&](const std::string &C) {
            return C == "bafkreiotherpackagexxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                 ? PkgActions::OtherNode{&Fonts, Lib.path().toStdString(), "Core Fonts"} : PkgActions::OtherNode{};
        };
        Notes.clear();
        const auto Walked = PkgActions::GameSees(F.doc(), F.nodeId(v).toStdString(), F.dir->path().toStdString(), &Notes, Other);
        QCOMPARE((int)Walked.size(), 3);
        bool Font = false;
        for (const auto &W : Walked) if (W.Path == "%Windows%/Fonts/arial.ttf" && W.From == "Core Fonts > fonts") Font = true;
        QVERIFY(Font);
        QVERIFY(Notes.empty());
    }

    // What the game sees is what a launch mounts: a node contained with a TAKE gives only the files it takes, where
    // the TAKE puts them, and a NODE layer's TARGET places the whole node there. Teeth: read each layer's own TARGET
    // (the untaken file listed, the placed one at the root).
    void theGameSeesWhatTheTakesAndPlacementsGive()
    {
        if (!HaveZipTools) QSKIP("zip/unzip not installed");
        Fixture F(this);
        F.write(F.path("pack/maps/a.map"), "a");
        F.write(F.path("pack/docs/readme.txt"), "r");
        F.write(F.path("tools/edit.exe"), "e");
        const int p = F.addContentNode("");
        F.setLayers(p, json::array({ json{{"DIR", "pack"}, {"TARGET", "FILES/%GameDir%"}} }));
        const int t = F.addContentNode("");
        F.setLayers(t, json::array({ json{{"DIR", "tools"}} }));
        F.save();
        const int v = F.addContentNode("");
        F.setLayers(v, json::array({ json{{"NODE", F.nodeId(p).toStdString()},
                                          {"TAKE", json::array({ json::array({"FILES/%GameDir%/maps", "FILES/%GameDir%/baseq/maps"}) })}},
                                     json{{"NODE", F.nodeId(t).toStdString()}, {"TARGET", "FILES/%GameDir%/bin"}} }));
        F.save();
        std::set<std::string> Paths;
        for (const auto &S : PkgActions::GameSees(F.doc(), F.nodeId(v).toStdString(), F.dir->path().toStdString())) Paths.insert(S.Path);
        QCOMPARE(Paths, (std::set<std::string>{ "%GameDir%/baseq/maps/a.map", "%GameDir%/bin/edit.exe" }));
    }

    // Two versions compared: the layers only one folds, the files only one gives the game, and the files both give
    // from different layers; the menu's action reports them. Teeth: compare layer types only (same-type layers look
    // equal); call every shared file different.
    void twoVersionsAreComparedByLayersAndFiles()
    {
        if (!HaveZipTools) QSKIP("zip/unzip not installed");
        Fixture F(this);
        F.makeZip("base.zip", "game.exe", "mz");
        F.makeZip("base.zip", "data.pak", "same in both");     // zip adds to the archive: both versions get it from base
        F.write(F.path("hd/game.exe"), "hd");
        F.write(F.path("hd/hd.ini"), "x");
        const int b = F.addContentNode("base.zip");
        { json N = F.node(b); N["LABEL"] = "Original"; F.setNode(b, N); }
        F.save();
        const int p = F.addContentNode("");
        F.setLayers(p, json::array({ json{{"NODE", F.nodeId(b).toStdString()}}, json{{"DIR", "hd"}, {"TARGET", "FILES/%GameDir%"}} }));
        { json N = F.node(p); N["LABEL"] = "HD"; F.setNode(p, N); }
        F.save();
        const auto R = PkgActions::Compare(F.doc(), F.nodeId(b).toStdString(), F.nodeId(p).toStdString(), F.dir->path().toStdString());
        QCOMPARE((int)R.LayersOnlyA.size(), 0);
        QCOMPARE(R.LayersOnlyB, std::vector<std::string>{"DIR  hd"});
        QCOMPARE((int)R.FilesOnlyA.size(), 0);
        QCOMPARE(R.FilesOnlyB, std::vector<std::string>{"%GameDir%/hd.ini"});
        QCOMPARE((int)R.FilesDiffer.size(), 1);
        QVERIFY(R.FilesDiffer[0].rfind("%GameDir%/game.exe", 0) == 0);
        F.act->perform(F.nodeId(b), "compare:" + F.nodeId(p));
        QTest::qWait(50);
        QVERIFY2(F.notices.join(" ").contains("hd.ini"), qUtf8Printable(F.notices.join(" ")));
    }

    // "bring in files" with a FOLDER: an installed game anywhere on the machine becomes "<folder>.zip" in the package,
    // STORED (vidyagodfs reads entries in place), and the layer names it. The folder itself is left as it was.
    // Teeth: pack with deflate (the stored check fails); name it after the path as picked ("game/.zip").
    void bringingInAFolderPacksItStored()
    {
        if (!HaveZipTools) QSKIP("zip/unzip not installed");
        Fixture F(this);
        QTemporaryDir Src; QVERIFY(Src.isValid());
        const QString Game = Src.path() + "/Some Game";
        F.write(Game + "/bin/game.exe", QByteArray(4096, 'x'));
        F.write(Game + "/data/levels.dat", "levels");
        const int n = F.addContentNode("");
        F.chooseThis(0);
        F.pickThisFile(Game + "/");                               // a folder picker may hand it back with a slash
        F.act->perform(F.nodeId(n), "bring_in");
        QVERIFY(F.waitIdle(F.nodeId(n)));
        QVERIFY2(F.notices.isEmpty(), qUtf8Printable(F.notices.join(" ")));
        QCOMPARE(F.name(n), std::string("Some Game.zip"));
        QCOMPARE(F.type(n), std::string("ZIP"));
        int Err = 0;
        zip_t *Z = zip_open(F.path("Some Game.zip").toUtf8().constData(), ZIP_RDONLY, &Err);
        QVERIFY(Z);
        zip_stat_t St;
        QCOMPARE(zip_stat(Z, "bin/game.exe", 0, &St), 0);         // entries relative to the folder, not its path
        QCOMPARE((int)St.comp_method, (int)ZIP_CM_STORE);
        QCOMPARE(zip_stat(Z, "data/levels.dat", 0, &St), 0);
        zip_close(Z);
        QVERIFY(QFile::exists(Game + "/bin/game.exe"));            // the source is untouched
        QVERIFY(QDir(F.dir->path()).entryList({"*.tmp"}, QDir::Files).isEmpty());
    }

    // Files come in on any node: one holding a captured folder (a No-CD exe goes on top of it), one with no files yet
    // (a mod's own). Teeth: offer it only on a zip node, as it was (a DIR-captured game could take no fix).
    void everyNodeCanBringInFiles()
    {
        auto Offers = [](const json &N) {
            for (const auto &A : PkgGraph::ActionsFor(N, false, {})) if (std::string(A.Id) == "bring_in") return true;
            return false;
        };
        QVERIFY(Offers(json::parse(R"({"LABEL":"g","LAYERS":[{"DIR":"Game","TARGET":"FILES/%GameDir%"}]})")));
        QVERIFY(Offers(json::parse(R"({"LABEL":"g","LAYERS":[{"ZIP":"g.zip"}]})")));
        QVERIFY(Offers(json::parse(R"({"LABEL":"m","LAYERS":[{"DLL":{"d3d8":"n,b"}}]})")));
    }

    // A save renames a node away from its CID; another node later minted to that same CID (two empty "a mod for it"
    // nodes are byte-identical) is the node an action on that handle means - not the one the old rename went to.
    // Teeth: follow the rename first (the action lands on the first node).
    void anActionOnAHandleALiveNodeHasMeansThatNode()
    {
        Fixture F(this);
        const int a = F.addContentNode("");
        F.save();
        const std::string Old = F.nodeId(a).toStdString();
        { json N = F.node(a); N["LABEL"] = "renamed"; F.setNode(a, N); }
        F.save();                                              // Old -> a's new CID, remembered
        const int b = F.addContentNode("");                   // byte-identical to a's first form: CID Old again
        F.save();
        QCOMPARE(F.nodeId(b).toStdString(), Old);
        F.write(F.path("g/x.exe"), "MZ");
        F.chooseThis(0);                                       // a folder
        F.pickThisFile(F.path("g"));
        F.act->perform(QString::fromStdString(Old), "bring_in");
        QVERIFY(F.waitIdle(F.nodeId(b)));
        QCOMPARE(F.name(b), std::string("g.zip"));
        QCOMPARE(F.name(a), std::string(""));
    }

    // A compressed zip (a download) cannot mount: it comes in re-packed STORE, its files the same, the picked file
    // untouched. Teeth: copy it as it is (the package's zip is still DEFLATE).
    void bringingInACompressedZipStoresIt()
    {
        Fixture F(this);
        QTemporaryDir Src; QVERIFY(Src.isValid());
        QTemporaryDir Make; QVERIFY(Make.isValid());
        F.write(Make.path() + "/scripts/mod.asi", QByteArray(4000, 'a'));
        QProcess Z; Z.setWorkingDirectory(Make.path());
        Z.start("zip", {"-9", "-r", "-q", Src.path() + "/Mod.zip", "scripts"});
        QVERIFY(Z.waitForFinished() && Z.exitCode() == 0);
        QVERIFY(!ManifestModel::ZipFullyStored((Src.path() + "/Mod.zip").toStdString()));
        const int n = F.addContentNode("");
        F.chooseThis(1);
        F.pickThisFile(Src.path() + "/Mod.zip");
        F.act->perform(F.nodeId(n), "bring_in");
        QVERIFY(F.waitIdle(F.nodeId(n)));
        QCOMPARE(F.name(n), std::string("Mod.zip"));
        QVERIFY(ManifestModel::ZipFullyStored(F.path("Mod.zip").toStdString()));
        QProcess L; L.start("unzip", {"-Z1", F.path("Mod.zip")});
        QVERIFY(L.waitForFinished());
        QVERIFY(QString::fromUtf8(L.readAllStandardOutput()).contains("scripts/mod.asi"));
        QVERIFY(!ManifestModel::ZipFullyStored((Src.path() + "/Mod.zip").toStdString()));   // the picked file as it was
        QVERIFY(QDir(F.dir->path()).entryList({"*.tmp"}, QDir::Files | QDir::Dirs).isEmpty());
    }

    // ... with a ZIP: one from elsewhere is copied in under its own name; one already in the package is only named
    // (no second copy). Cancelling either question changes nothing. Teeth: copy the in-package zip too (a duplicate
    // appears); ignore the cancel (the node changes).
    void bringingInAZipCopiesItOrNamesTheOneHere()
    {
        Fixture F(this);
        QTemporaryDir Src; QVERIFY(Src.isValid());
        F.write(Src.path() + "/setup.zip", "PK-not-really");
        const int n = F.addContentNode("");
        F.chooseThis(-1);
        F.act->perform(F.nodeId(n), "bring_in");
        QTest::qWait(50);
        QCOMPARE(F.name(n), std::string(""));
        F.chooseThis(1);
        F.pickThisFile("");                                       // the file picker cancelled
        F.act->perform(F.nodeId(n), "bring_in");
        QTest::qWait(50);
        QCOMPARE(F.name(n), std::string(""));

        F.pickThisFile(Src.path() + "/setup.zip");
        F.act->perform(F.nodeId(n), "bring_in");
        QVERIFY(F.waitIdle(F.nodeId(n)));
        QCOMPARE(F.name(n), std::string("setup.zip"));
        QCOMPARE(F.read(F.path("setup.zip")), QByteArray("PK-not-really"));
        QVERIFY(QFile::exists(Src.path() + "/setup.zip"));

        F.write(F.path("sub/here.zip"), "already here");
        F.pickThisFile(F.path("sub/here.zip"));
        F.act->perform(F.nodeId(n), "bring_in");
        QTest::qWait(50);
        QCOMPARE(F.name(n), std::string("sub/here.zip"));
        QVERIFY(!QFile::exists(F.path("here.zip")));
    }

    // A name the package already has is never overwritten unasked. A file no node uses (left by a bring-in that was
    // never saved) is offered: kept and used as it is, or replaced; asked and cancelled, nothing changes. A file a node
    // uses keeps its name, the new files taking the next free one. A folder that holds the package is refused (packed
    // into itself). Teeth: overwrite unasked; ignore the answer; take a used name; drop the folder guard.
    void bringingInRefusesToClobberOrSwallowThePackage()
    {
        if (!HaveZipTools) QSKIP("zip/unzip not installed");
        QTemporaryDir Outer; QVERIFY(Outer.isValid());           // the package's parent: small, so a broken guard packs little
        Fixture F(this, true, true, Outer.path());
        QTemporaryDir Src; QVERIFY(Src.isValid());
        F.write(Src.path() + "/game/a.exe", "new");
        F.write(Src.path() + "/other/b.exe", "new");
        F.write(F.path("game.zip"), "SOMEONE ELSE'S ARCHIVE");
        F.write(F.path("other.zip"), "OLD");
        const int n = F.addContentNode("");
        //The folder question answered "a folder"; the one about a file no node uses by A; the one about the node's
        //own files (add over them / replace them) by B.
        auto Existing = [&F](int A, int B = 0)
        { F.act->setChooseHandler([A, B](const QString &, const QString &T, const QStringList &) { return T.contains("no node uses") ? A : T.contains("already has files") ? B : 0; }); };
        F.pickThisFile(Src.path() + "/game");
        Existing(-1);
        F.act->perform(F.nodeId(n), "bring_in");
        QTest::qWait(300);
        QCOMPARE(F.read(F.path("game.zip")), QByteArray("SOMEONE ELSE'S ARCHIVE"));
        QCOMPARE(F.name(n), std::string(""));

        Existing(0);                                              // use it
        F.act->perform(F.nodeId(n), "bring_in");
        QVERIFY(F.waitIdle(F.nodeId(n)));
        QCOMPARE(F.name(n), std::string("game.zip"));
        QCOMPARE(F.read(F.path("game.zip")), QByteArray("SOMEONE ELSE'S ARCHIVE"));

        Existing(0, 1);                                           // the node has game.zip now: replace it
        F.act->perform(F.nodeId(n), "bring_in");                  // ...under a free name: game.zip stays a file
        QVERIFY(F.waitIdle(F.nodeId(n)));
        QCOMPARE(F.name(n), std::string("game-2.zip"));
        QCOMPARE(F.read(F.path("game.zip")), QByteArray("SOMEONE ELSE'S ARCHIVE"));
        QVERIFY(F.read(F.path("game-2.zip")).startsWith("PK"));

        F.pickThisFile(Src.path() + "/other");
        Existing(1, 0);                                           // replace the stray other.zip; add it as a new layer
        F.act->perform(F.nodeId(n), "bring_in");
        QVERIFY(F.waitIdle(F.nodeId(n)));
        QCOMPARE(F.name(n), std::string("game-2.zip"));           // the node's files are never repointed by adding
        QCOMPARE(F.node(n)["LAYERS"].back()["ZIP"], json("other.zip"));
        QVERIFY(F.read(F.path("other.zip")).startsWith("PK"));

        F.notices.clear();
        const QString Parent = QFileInfo(F.dir->path()).absolutePath();
        for (const QString &P : {F.dir->path(), Parent})
        {
            F.pickThisFile(P);
            F.act->perform(F.nodeId(n), "bring_in");
            QTest::qWait(300);
            QVERIFY2(F.notices.join(" ").contains("holds this package"), qUtf8Printable(P));
            F.notices.clear();
        }
        QCOMPARE(F.name(n), std::string("game-2.zip"));           // the refusals changed nothing
        QVERIFY(!QFile::exists(Parent + "/" + QFileInfo(Parent).fileName() + ".zip"));
        QCOMPARE(QDir(F.dir->path()).entryList({"*.zip", "*.tmp"}, QDir::Files, QDir::Name), (QStringList{"game-2.zip", "game.zip", "other.zip"}));
    }

    // The Exe "pick": the programs in the node's files AND in the package's nodes it contains, each as the anchored
    // path an EXEC entry names (the layer's TARGET without "FILES/"), are offered sorted; the one chosen becomes the
    // entry's EXE, and a blank WORKDIR becomes its folder (one already set stays). Teeth: skip contained nodes (the
    // data node's program is not offered); drop the anchor; overwrite a set WORKDIR.
    void pickingTheProgramOffersWhatTheFilesHold()
    {
        if (!HaveZipTools) QSKIP("zip/unzip not installed");
        Fixture F(this);
        F.makeZip("data.zip", "Game/Run.EXE", "mz");
        F.write(F.path("tools/cfg.sh"), "#!/bin/sh");
        F.write(F.path("tools/readme.txt"), "not a program");
        const int d = F.addContentNode("data.zip");
        F.setLayers(d, json::array({ json{{"ZIP", "data.zip"}, {"TARGET", "FILES/%GameDir%"}},
                                     json{{"DIR", "tools"}, {"TARGET", "FILES/%GameDir%/Tools"}} }));
        F.save();
        const int v = F.addContentNode("");
        F.setLayers(v, json::array({ json{{"NODE", F.nodeId(d).toStdString()}},
                                     json{{"EXEC", json::array({ json{{"HOST", "win32"}}, json{{"HOST", "win32"}, {"WORKDIR", "%GameDir%"}} })}} }));
        F.save();
        QStringList Offered;
        F.act->setChooseHandler([&Offered](const QString &, const QString &, const QStringList &O) { Offered = O; return 0; });
        F.act->perform(F.nodeId(v), "pick_exe:L1/0");
        QTest::qWait(50);
        QCOMPARE(Offered, (QStringList{"%GameDir%/Game/Run.EXE", "%GameDir%/Tools/cfg.sh"}));
        json E = F.node(v)["LAYERS"][1]["EXEC"];
        QCOMPARE(E[0]["EXE"], json("%GameDir%/Game/Run.EXE"));
        QCOMPARE(E[0]["WORKDIR"], json("%GameDir%/Game"));
        F.act->setChooseHandler([](const QString &, const QString &, const QStringList &) { return 1; });
        F.act->perform(F.nodeId(v), "pick_exe:L1/1");
        QTest::qWait(50);
        E = F.node(v)["LAYERS"][1]["EXEC"];
        QCOMPARE(E[1]["EXE"], json("%GameDir%/Tools/cfg.sh"));
        QCOMPARE(E[1]["WORKDIR"], json("%GameDir%"));            // the author's work dir stays
    }

    // The pick offers first what a GOG install's play task names (whatever its case), and a Host left blank becomes the
    // picked program's own, from its header. Teeth: offer in name order only; leave the Host blank.
    void pickingTheProgramOffersTheGamesFirst()
    {
        auto Pe32 = [] { std::string H(256, '\0'); H[0] = 'M'; H[1] = 'Z'; H[0x3C] = 0x40; H.replace(0x40, 4, std::string("PE\0\0", 4)); H[0x44] = 0x4c; H[0x45] = 0x01; return QByteArray(H.data(), (int)H.size()); };
        Fixture F(this);
        F.write(F.path("g/Settings.exe"), Pe32());
        F.write(F.path("g/bin/XR_3DA.exe"), Pe32());
        F.write(F.path("g/goggame-1.info"), R"({"playTasks":[{"isPrimary":true,"category":"game","path":"bin\\xr_3da.exe"}]})");
        const int n = F.addContentNode("");
        F.setLayers(n, json::array({ json{{"DIR", "g"}, {"TARGET", "FILES/%GameDir%"}}, json{{"EXEC", json::array({ json{{"LABEL", "Play"}} })}} }));
        F.save();
        QStringList Offered;
        F.act->setChooseHandler([&Offered](const QString &, const QString &, const QStringList &O) { Offered = O; return 0; });
        F.act->perform(F.nodeId(n), "pick_exe:L1/0");
        QTest::qWait(50);
        QCOMPARE(Offered.value(0), QString("%GameDir%/bin/XR_3DA.exe"));
        const json E = F.node(n)["LAYERS"][1]["EXEC"][0];
        QCOMPARE(E["EXE"], json("%GameDir%/bin/XR_3DA.exe"));
        QCOMPARE(E["HOST"], json("win32"));
        //Picking a 64-bit program afterwards corrects the Host the 32-bit one left; a non-Windows Host is the author's.
        auto Pe64 = [] { std::string H(256, '\0'); H[0] = 'M'; H[1] = 'Z'; H[0x3C] = 0x40; H.replace(0x40, 4, std::string("PE\0\0", 4)); H[0x44] = (char)0x64; H[0x45] = (char)0x86; return QByteArray(H.data(), (int)H.size()); };
        F.write(F.path("g/bin64/Game64.exe"), Pe64());
        F.act->setChooseHandler([](const QString &, const QString &, const QStringList &O) { return (int)O.indexOf("%GameDir%/bin64/Game64.exe"); });
        F.act->perform(F.nodeId(n), "pick_exe:L1/0");
        QTest::qWait(50);
        QCOMPARE(F.node(n)["LAYERS"][1]["EXEC"][0]["HOST"], json("win64"));
        { json N = F.node(n); N["LAYERS"][1]["EXEC"][0]["HOST"] = "custom"; F.setNode(n, N); }
        F.act->perform(F.nodeId(n), "pick_exe:L1/0");
        QTest::qWait(50);
        QCOMPARE(F.node(n)["LAYERS"][1]["EXEC"][0]["HOST"], json("custom"));
    }

    // A program two layers supply (the game's exe and a No-CD one over it) is one program at one place: offered once,
    // and so it is the likely one beside a config tool. Teeth: list each layer's copy (the config tool comes first).
    void pickingTheProgramOffersAProgramOnceWhateverSuppliesIt()
    {
        Fixture F(this);
        F.write(F.path("g/SH3Config.exe"), "MZ");
        F.write(F.path("g/sh3.exe"), "MZ");
        F.write(F.path("nocd/sh3.exe"), "MZ");
        const int n = F.addContentNode("");
        F.setLayers(n, json::array({ json{{"DIR", "g"}, {"TARGET", "FILES/%GameDir%"}}, json{{"DIR", "nocd"}, {"TARGET", "FILES/%GameDir%"}},
                                     json{{"EXEC", json::array({ json{{"LABEL", "Play"}} })}} }));
        F.save();
        QStringList Offered;
        F.act->setChooseHandler([&Offered](const QString &, const QString &, const QStringList &O) { Offered = O; return -1; });
        F.act->perform(F.nodeId(n), "pick_exe:L2/0");
        QTest::qWait(50);
        QCOMPARE(Offered, QStringList({"%GameDir%/sh3.exe", "%GameDir%/SH3Config.exe"}));
    }

    // A disc with a Windows build, a DOS build and a launcher script named like the game (GTA London: gtawin/gta_uk.exe,
    // gtados/*.exe, london.bat): the pick preselects the Windows program for an entry not said to be anything else.
    // Teeth: preselect the script named like the game; preselect the first program (a DOS one).
    void pickingTheProgramPrefersAWindowsBuild()
    {
        auto Pe32 = [] { std::string H(256, '\0'); H[0] = 'M'; H[1] = 'Z'; H[0x3C] = 0x40; H.replace(0x40, 4, std::string("PE\0\0", 4)); H[0x44] = 0x4c; H[0x45] = 0x01; return QByteArray(H.data(), (int)H.size()); };
        Fixture F(this);
        F.write(F.path("g/london.bat"), "@echo off");
        F.write(F.path("g/gtados/gta24_uk.exe"), "MZ");                 // MZ alone: a DOS program
        F.write(F.path("g/gtados/k_uk.exe"), "MZ");
        F.write(F.path("g/gtawin/gta_uk.exe"), Pe32());
        const int n = F.addContentNode("");
        { json N = F.node(n); N["LABEL"] = "Grand Theft Auto: London 1969"; F.setNode(n, N); }
        F.setLayers(n, json::array({ json{{"DIR", "g"}, {"TARGET", "FILES/%GameDir%"}}, json{{"EXEC", json::array({ json{{"LABEL", "Play"}} })}} }));
        F.save();
        QStringList Offered;
        F.act->setChooseHandler([&Offered](const QString &, const QString &, const QStringList &O) { Offered = O; return -1; });
        F.act->perform(F.nodeId(n), "pick_exe:L1/0");
        QTest::qWait(50);
        QCOMPARE(Offered.value(0), QString("%GameDir%/gtawin/gta_uk.exe"));
        //An entry said to be for DOS is not steered to the Windows build.
        { json N = F.node(n); N["LAYERS"][1]["EXEC"][0]["HOST"] = "dos"; F.setNode(n, N); }
        F.act->perform(F.nodeId(n), "pick_exe:L1/0");
        QTest::qWait(50);
        QVERIFY(Offered.value(0) != QString("%GameDir%/gtawin/gta_uk.exe"));
    }

    // On a version over its game, the pick offers the version's OWN programs first and preselects the one named like
    // the version (Revision.exe for "Revision"; its config tools skipped), not the game's program the base's GOG task
    // names. Teeth: preselect over everything; no name match.
    void pickingAVersionsProgramOffersItsOwnFirst()
    {
        auto Pe32 = [] { std::string H(256, '\0'); H[0] = 'M'; H[1] = 'Z'; H[0x3C] = 0x40; H.replace(0x40, 4, std::string("PE\0\0", 4)); H[0x44] = 0x4c; H[0x45] = 0x01; return QByteArray(H.data(), (int)H.size()); };
        Fixture F(this);
        F.write(F.path("base/Bin/DeusEx.exe"), Pe32());
        F.write(F.path("base/goggame-1.info"), R"({"playTasks":[{"isPrimary":true,"category":"game","path":"System\\DeusEx.exe"}]})");
        F.write(F.path("rev/Revision/ConfigPatcher.exe"), Pe32());
        F.write(F.path("rev/Revision/System/Revision.exe"), Pe32());
        F.write(F.path("rev/Revision/TNM2/TNM.exe"), Pe32());
        const int b = F.addContentNode("");
        F.setLayers(b, json::array({ json{{"DIR", "base"}, {"TARGET", "FILES/%GameDir%"}} }));
        F.save();
        const int v = F.addContentNode("");
        F.setLayers(v, json::array({ json{{"NODE", F.nodeId(b).toStdString()}}, json{{"DIR", "rev"}, {"TARGET", "FILES/%GameDir%"}},
                                     json{{"EXEC", json::array({ json{{"LABEL", "Play"}} })}} }));
        { json N = F.node(v); N["VARIANT"] = "Revision"; F.setNode(v, N); }
        F.save();
        QStringList Offered;
        F.act->setChooseHandler([&Offered](const QString &, const QString &, const QStringList &O) { Offered = O; return 0; });
        F.act->perform(F.nodeId(v), "pick_exe:L2/0");
        QTest::qWait(50);
        QCOMPARE(Offered.value(0), QString("%GameDir%/Revision/System/Revision.exe"));
        QVERIFY(Offered.indexOf("%GameDir%/Bin/DeusEx.exe") > Offered.indexOf("%GameDir%/Revision/TNM2/TNM.exe"));
    }

    // A pick with nothing to offer says what to do first; a pick naming no real entry (hand-made, out of range,
    // negative) changes nothing and does not crash. Teeth: drop the negative-index guard (an out-of-range json index).
    void pickingTheProgramRefusesQuietlyWhatItCannotDo()
    {
        Fixture F(this);
        const int v = F.addContentNode("");
        F.setLayers(v, json::array({ json{{"EXEC", json::array({ json{{"HOST", "win32"}} })}} }));
        F.save();
        F.chooseThis(0);
        F.act->perform(F.nodeId(v), "pick_exe:L0/0");
        QTest::qWait(50);
        QVERIFY(F.notices.join(" ").contains("bring in files"));
        F.write(F.path("game/x.exe"), "mz");
        F.setLayers(v, json::array({ json{{"DIR", "game"}}, json{{"EXEC", json::array({ json{{"HOST", "win32"}} })}} }));
        F.save();
        const json Before = F.node(v);
        for (const char *W : {"pick_exe:L-1/0", "pick_exe:L1/-1", "pick_exe:L1/5", "pick_exe:L0/0", "pick_exe:L9/0", "pick_exe:junk"})
        {
            F.act->perform(F.nodeId(v), W);
            QTest::qWait(20);
            QCOMPARE(F.node(v), Before);
        }
    }

private:
    bool HaveZipTools = false;

    // A real bundle on disk with a model, canvas and actions wired the way the editor wires them.
    struct Fixture
    {
        explicit Fixture(QObject *parent, bool confirm = true, bool installHandlers = true, const QString &under = {})
        {
            dir = under.isEmpty() ? new QTemporaryDir() : new QTemporaryDir(under + "/pkg-XXXXXX");
            cfg = json{{"Settings", json::object()}};
            model = new PackageEditorModel(&cfg, parent);
            model->initPackage(dir->path());
            canvas = new PkgCanvas(&model->doc(), parent);
            act = new PkgActions(model, canvas, nullptr, parent);
            if (!installHandlers) return;                 // exercise the shipped defaults
            act->setConfirmHandler([confirm](const QString &, const QString &) { return confirm; });
            // Capture the outcome messages instead of raising modals — a dialog blocks a headless run, and
            // the refusal paths are exactly the ones worth asserting.
            act->setNotifyHandler([this](const QString &T, const QString &B) { notices << (T + ": " + B); });
        }
        //The fixture owns what it made. Parented to the test object alone, they outlived it: a model's queued
        //validation fired in a LATER test and read the dead `cfg` (a segfault wherever that timing hit, e.g. CI).
        ~Fixture() { delete act; delete canvas; delete model; delete dir; }

        PkgDoc::Document &doc() { return model->doc(); }
        QString path(const QString &rel) const { return dir->path() + "/" + rel; }
        //A node by its INDEX, which a save keeps (the document keeps its order while it renames): its handle now,
        //its JSON now, and its file (named by its CID once saved).
        QString nodeId(int i) { return QString::fromStdString(doc().Handle(i)); }
        QString nodeFile(int i) { return QString::fromStdString(doc().File(i)); }
        json node(int i) { return doc().Node(i); }
        void setNode(int i, json N) { doc().Replace(i, std::move(N)); doc().Commit(); }
        void setLayers(int i, const json &L) { json N = node(i); N["LAYERS"] = L; setNode(i, N); }
        void save() { QString E; QVERIFY2(model->Save(&E), qUtf8Printable(E)); }

        int addRegNode()
        {
            const int i = canvas->addNode("REG", 0, 0);
            save();
            return i;
        }
        // Answer the file dialog with a path instead of raising a modal nobody can click.
        void pickThisFile(const QString &p)
        { act->setPickHandler([p](const QString &, const QString &, const QString &, bool) { return p; }); }
        // Answer a choice (a folder or a zip; which program) by index; -1 cancels.
        void chooseThis(int i)
        { act->setChooseHandler([i](const QString &, const QString &, const QStringList &) { return i; }); }
        // A small real regedit export, UTF-8 with CRLF the way wine writes one.
        QString writeReg(const QString &name)
        {
            const QString P = dir->path() + "/" + name;
            write(P, "Windows Registry Editor Version 5.00\r\n\r\n"
                     "[HKEY_LOCAL_MACHINE\\Software\\Ubi Soft\\TONICT]\r\n"
                     "\"Version\"=\"1.00\"\r\n");
            return P;
        }

        int addContentNode(const QString &p)
        {
            const int i = canvas->addNode("ZIP", 0, 0);        // one ZIP layer: the node's content layer
            setLayers(i, json::array({ json{{"ZIP", p.toStdString()}} }));
            save();
            return i;
        }
        // The content layer's type and file name, and rewriting them (as a hand edit would).
        std::string type(int i) { return PkgGraph::ContentType(node(i)); }
        std::string name(int i) { return PkgGraph::ContentName(node(i)); }
        void setType(int i, const std::string &T) { const std::string N = name(i); setLayers(i, json::array({ json{{T, N}} })); }
        void setName(int i, const std::string &N) { const std::string T = type(i); setLayers(i, json::array({ json{{T, N}} })); }
        void write(const QString &p, const QByteArray &b)
        { QDir().mkpath(QFileInfo(p).path()); QFile f(p); QVERIFY2(f.open(QIODevice::WriteOnly), qUtf8Printable(p)); f.write(b); }
        QByteArray read(const QString &p) const { QFile f(p); if (!f.open(QIODevice::ReadOnly)) return {}; return f.readAll(); }
        void makeZip(const QString &name, const QString &entry, const QByteArray &body)
        {
            const QString stage = dir->path() + "/__stage";
            QDir().mkpath(QFileInfo(stage + "/" + entry).path());
            write(stage + "/" + entry, body);
            QProcess z; z.setWorkingDirectory(stage);
            z.start("zip", {"-0", "-r", "-X", dir->path() + "/" + name, "."});
            z.waitForFinished(10000);
            QDir(stage).removeRecursively();
        }
        // The action is done when the canvas stops reporting the node busy.
        bool waitIdle(const QString &id, int ms = 15000)
        {
            const std::string Id = id.toStdString();
            QElapsedTimer t; t.start();
            while (canvas->isBusy(Id) && t.elapsed() < ms) QTest::qWait(25);
            QTest::qWait(50);                                      // let the completion handler finish
            return !canvas->isBusy(Id);
        }

        QStringList notices;
        QTemporaryDir *dir = nullptr;
        json cfg;
        PackageEditorModel *model = nullptr;
        PkgCanvas *canvas = nullptr;
        PkgActions *act = nullptr;
    };

private:
    QTemporaryDir *SuiteDataRoot = nullptr;
};

QTEST_MAIN(PkgActionsTest)
#include "test_pkgactions.moc"
