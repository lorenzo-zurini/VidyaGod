// PkgActions — the node actions that touch real files. This is the riskiest code in the editor: zip<->dir
// conversion, re-store and make-delta all DELETE the source once the replacement exists, so a mistake here
// destroys content that may be the only copy on the machine.
//
// It was entirely uncovered. These tests drive the real conversions against real archives on disk and assert
// the property that matters most: on FAILURE, nothing is deleted. Confirmation dialogs are injected (the
// actions ask before destroying anything, and a modal blocks a headless run forever).

#include "pkgactions.h"
#include "pkgcanvas.h"
#include "packageeditormodel.h"

#include "imgui.h"
#include "imnodes.h"

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
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        unsigned char *px = nullptr; int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
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

        QCOMPARE(F.doc()["NODES"][n]["FORM"].get<std::string>(), std::string("dir"));
        const QString Dir = F.path("content");
        QVERIFY(QFile::exists(Dir + "/hello/world.txt"));
        QVERIFY(!QFile::exists(F.path("content.zip")));            // replaced, not duplicated
        QCOMPARE(F.read(Dir + "/hello/world.txt"), QByteArray("payload-abc"));

        F.act->perform(F.nodeId(n), "to_zip");
        QVERIFY(F.waitIdle(F.nodeId(n)));
        QCOMPARE(F.doc()["NODES"][n]["FORM"].get<std::string>(), std::string("zip"));
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
        F.doc()["NODES"][n]["FORM"] = "dir";

        F.act->perform(F.nodeId(n), "to_zip");
        QTest::qWait(300);

        QVERIFY(QDir(F.path("stuff")).exists());                   // the folder was NOT deleted
        QCOMPARE(F.read(F.path("stuff.zip")), QByteArray("SOMEONE ELSE'S ARCHIVE"));   // untouched
        QCOMPARE(F.doc()["NODES"][n]["FORM"].get<std::string>(), std::string("dir"));  // node unchanged
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
        QCOMPARE(F.doc()["NODES"][n]["FORM"].get<std::string>(), std::string("zip"));      // node unchanged
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
        // ("" is refused earlier, by the has-a-PATH check — also correct, also not what this pins.)
        for (const char *Bad : {".", "./", "..", "nosuch/..", "a/b/../../..",
                                "../vg_outside_probe", "../vg_outside_probe/precious.txt", "/tmp"})
        {
            const int n = F.addContentNode(Bad);
            // to_delta is omitted: it refuses earlier and for a different reason (no Content parent to diff
            // against), which is also correct but not what this test is pinning.
            for (const char *Act : {"to_zip", "to_dir", "restore", "undelta"})
            {
                F.doc()["NODES"][n]["FORM"] = (std::string(Act) == "to_zip") ? "dir" : "zip";
                F.notices.clear();
                F.act->perform(F.nodeId(n), Act);
                QTest::qWait(120);
                // The REFUSAL is the assertion, not merely that nothing was destroyed: several of these
                // happen to fail later anyway (the zip tool errors on a nonexistent path), so a survival-only
                // check passes just as well with the guard removed.
                QVERIFY2(F.notices.join(" ").contains("does not point inside"),
                         qUtf8Printable(QString("%1 on PATH \"%2\" was not refused: %3")
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
        QCOMPARE(F.doc()["NODES"][n]["FORM"].get<std::string>(), std::string("zip"));
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
        QCOMPARE(F.doc()["NODES"][n]["FORM"].get<std::string>(), std::string("zip"));
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

        F.doc()["NODES"][n]["PATH"] = "";                                 // drives the "Nothing to convert" report
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

    // THE property, and the positive control for the one below: an import into a node with no registry rows
    // yet materialises the entry and lands the rows in BOTH architecture views.
    void importIntoAnEmptyRegEditLandsTheRows()
    {
        Fixture F(this);
        const int n = F.addRegEditNode();
        F.doc()["NODES"][n].erase("EDITS");                         // absent = nothing to lose
        F.model->SaveNodes();

        F.pickThisFile(F.writeReg("good.reg"));
        F.act->perform(F.nodeId(n), "import_reg");
        QTest::qWait(100);

        const json &E = F.doc()["NODES"][n]["EDITS"];
        QVERIFY2(E.is_array() && !E.empty(), E.dump().c_str());
        QCOMPARE(E[0]["ARCHITECTURE"], json::array({"32", "64"}));
        const std::string Dump = E.dump();
        QVERIFY2(Dump.find("TONICT") != std::string::npos, Dump.c_str());
        QVERIFY2(Dump.find("1.00")   != std::string::npos, Dump.c_str());
    }

    // A hand-written `"EDITS": {"HKLM": {...}}` is the shape the canvas ALREADY refuses to overwrite from its
    // "+ group" button — but Import offered the same node the same destruction from a different button, and
    // then SAVED it. It is the likelier of the two, because the author reaches for Import precisely when the
    // node is in a state they are trying to repair. Refuse, say why, and change nothing on disk.
    void importRefusesAMalformedEditsInsteadOfDestroyingIt()
    {
        Fixture F(this);
        const int n = F.addRegEditNode();
        const json Hand = json::parse(R"({"HKLM":{"Software":{"Mine":{"Keep":"precious"}}}})");
        F.doc()["NODES"][n]["EDITS"] = Hand;                        // not an array — the author's own tree
        F.model->SaveNodes();

        F.pickThisFile(F.writeReg("good.reg"));
        F.act->perform(F.nodeId(n), "import_reg");
        QTest::qWait(100);

        QCOMPARE(F.doc()["NODES"][n]["EDITS"], Hand);               // in memory...
        // ...and ON DISK, which is what the button used to lose: the destruction was followed by SaveNodes().
        const json Saved = json::parse(F.read(F.path(F.nodeId(n) + ".json")).toStdString());
        QCOMPARE(Saved["EDITS"], Hand);
        QVERIFY(!F.notices.isEmpty());
        QVERIFY2(F.notices.join(" ").contains("not a list"), qUtf8Printable(F.notices.join(" ")));
    }

    // The refusal has to hold at the ENTRY too, not just the container. `"EDITS": ["x"]` is a list, so it
    // passed the check above — and RegRowsInto then iterated a string as an object and wrote it back as
    // {"": "x", "HKLM": {...}}. That is the exact shape drawRegEdits refuses to touch, reshaped and saved by
    // a different button on the same node.
    void importRefusesAMalformedEditsEntry()
    {
        Fixture F(this);
        const int n = F.addRegEditNode();
        const json Hand = json::parse(R"(["a hand-written entry"])");
        F.doc()["NODES"][n]["EDITS"] = Hand;
        F.model->SaveNodes();

        F.pickThisFile(F.writeReg("good.reg"));
        F.act->perform(F.nodeId(n), "import_reg");
        QTest::qWait(100);

        QCOMPARE(F.doc()["NODES"][n]["EDITS"], Hand);
        const json Saved = json::parse(F.read(F.path(F.nodeId(n) + ".json")).toStdString());
        QCOMPARE(Saved["EDITS"], Hand);
        QVERIFY2(F.notices.join(" ").contains("not an object"), qUtf8Printable(F.notices.join(" ")));
    }

    // A null first entry IS materialised — there is nothing there to lose — but into the same thing the
    // absent-EDITS rule writes, views and all. NodeLower turns a missing ARCHITECTURE into one un-redirected
    // view, so materialising a bare {} would land the import in one view and a 64-bit game would read nothing
    // from a node that validates clean.
    void importIntoANullEntryLandsInBothViews()
    {
        Fixture F(this);
        const int n = F.addRegEditNode();
        F.doc()["NODES"][n]["EDITS"] = json::array({nullptr});
        F.model->SaveNodes();

        F.pickThisFile(F.writeReg("good.reg"));
        F.act->perform(F.nodeId(n), "import_reg");
        QTest::qWait(100);

        const json &E = F.doc()["NODES"][n]["EDITS"];
        QVERIFY2(E.is_array() && !E.empty() && E[0].is_object(), E.dump().c_str());
        QCOMPARE(E[0]["ARCHITECTURE"], json::array({"32", "64"}));
        QVERIFY2(E[0].dump().find("TONICT") != std::string::npos, E[0].dump().c_str());
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
            canvas = new PkgCanvas(&model->doc(), [this] { model->SaveNodes(); }, parent);
            canvas->initContexts();
            act = new PkgActions(model, canvas, nullptr, parent);
            if (!installHandlers) return;                 // exercise the shipped defaults
            act->setConfirmHandler([confirm](const QString &, const QString &) { return confirm; });
            // Capture the outcome messages instead of raising modals — a dialog blocks a headless run, and
            // the refusal paths are exactly the ones worth asserting.
            act->setNotifyHandler([this](const QString &T, const QString &B) { notices << (T + ": " + B); });
        }
        ~Fixture() { canvas->shutdownContexts(); delete dir; }

        json &doc() { return model->doc(); }
        QString path(const QString &rel) const { return dir->path() + "/" + rel; }
        QString nodeId(int i) { return QString::fromStdString(doc()["NODES"][i]["NODE_ID"].get<std::string>()); }

        int addRegEditNode()
        {
            const int i = canvas->addNode("RegEdit");
            model->SaveNodes();
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
            const int i = canvas->addNode("Content");
            doc()["NODES"][i]["PATH"] = p.toStdString();
            model->SaveNodes();
            return i;
        }
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
