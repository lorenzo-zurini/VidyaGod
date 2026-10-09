// PkgActions — the node actions that touch real files. This is the riskiest code in the editor: zip<->dir
// conversion, re-store and make-delta all DELETE the source once the replacement exists, so a mistake here
// destroys content that may be the only copy on the machine.
//
// It was entirely uncovered. These tests drive the real conversions against real archives on disk and assert
// the property that matters most: on FAILURE, nothing is deleted. Confirmation dialogs are injected (the
// actions ask before destroying anything, and a modal blocks a headless run forever).

#include "qtestjson.h"
#include "pkgactions.h"
#include "pkgcanvas.h"
#include "packageeditormodel.h"

#include "imgui.h"

#include <QtTest>
#include "apppaths.h"
#include <QTemporaryDir>
#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QElapsedTimer>

#include <fstream>

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

    // A delta's base is the nearest content at its own target, so "-> delta" is offered — and the action runs — only
    // for a zip whose node contains, EARLIER in its LAYERS and whole, a node with a zip at the SAME target. A base at
    // another target, placed elsewhere or narrowed by a TAKE, or contained AFTER the content, could never be paired.
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
        QCOMPARE(db(nodes({{"NODE", "hB"}}, false, "FILES/C:/g")), std::string("base.zip"));
        QCOMPARE(db(nodes({{"NODE", "hB"}}, true, "FILES/C:/g")), std::string());      // after it
        QCOMPARE(db(nodes({{"NODE", "hB"}}, false, "FILES/C:/other")), std::string()); // elsewhere
        QCOMPARE(db(nodes({{"NODE", "hB"}, {"TARGET", "FILES/x"}}, false, "FILES/C:/g")), std::string());
        QCOMPARE(db(nodes({{"NODE", "hB"}, {"TAKE", json::array({"FILES/a"})}}, false, "FILES/C:/g")), std::string());
        QCOMPARE(db(nodes({{"ANY", json::array({"hB"})}}, false, "FILES/C:/g")), std::string()); // not contained
        // The button asks the same question as the action.
        auto offers = [&](const json &D) {
            const PkgGraph::Graph G = PkgGraph::Build(base(D));
            for (const auto &A : PkgGraph::ActionsFor(D[1], G.Nodes[1].HasDeltaBase, {})) if (std::string(A.Id) == "to_delta") return true;
            return false;
        };
        QVERIFY(offers(nodes({{"NODE", "hB"}}, false, "FILES/C:/g")));
        QVERIFY(!offers(nodes({{"NODE", "hB"}}, false, "FILES/C:/other")));
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

private:
    bool HaveZipTools = false;

    // A real bundle on disk with a model, canvas and actions wired the way the editor wires them.
    struct Fixture
    {
        explicit Fixture(QObject *parent, bool confirm = true, bool installHandlers = true)
        {
            dir = new QTemporaryDir();
            cfg = json{{"Settings", json::object()}};
            model = new PackageEditorModel(&cfg, nullptr, parent);
            model->initPackage(dir->path(), nullptr);
            canvas = new PkgCanvas(&model->doc(), parent);
            act = new PkgActions(model, canvas, nullptr, parent);
            if (!installHandlers) return;                 // exercise the shipped defaults
            act->setConfirmHandler([confirm](const QString &, const QString &) { return confirm; });
            // Capture the outcome messages instead of raising modals — a dialog blocks a headless run, and
            // the refusal paths are exactly the ones worth asserting.
            act->setNotifyHandler([this](const QString &T, const QString &B) { notices << (T + ": " + B); });
        }
        ~Fixture() { delete dir; }

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
