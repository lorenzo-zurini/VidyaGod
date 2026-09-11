// Tests for PackageEditorModel — the node-native bundle editor's state/signal hub (the AppModel-style central
// structure from the PackageEditor de-god). Drives the non-UI surface: node file I/O (LoadNodes/SaveNodes with
// rename re-filing + orphan cleanup), replaceNodeJson, validation signalling, and the catalog queries. Authoring
// runs (RunInNode/AnalyzeNodeRegistry) raise modal dialogs + build live containers, so they're out of scope here.
// Uses a real temp bundle dir and an empty GlobalConfig (no repositories → BuildExecIndex sees only this bundle).

#include <QtTest>
#include "packageeditor.h"

#include <QTemporaryDir>
#include <QDir>
#include <QFile>

#include "packageeditormodel.h"
#include "nodefixture.h"

#include <fstream>
#include <string>

using json = nlohmann::ordered_json;

namespace {
void writeNode(const QString & dir, const QString & file, const json & j)
{
    std::ofstream f((dir + "/" + file).toStdString());
    f << j.dump(2);
}
}

static int indexOfNode(PackageEditorModel & m, const std::string & id)
{
    const auto & Ns = m.doc()["NODES"];
    for (int i = 0; i < (int)Ns.size(); ++i)
        if (Ns[i].value("NODE_ID", std::string()) == id) return i;
    return -1;
}

class PackageEditorModelTest : public QObject
{
    Q_OBJECT
    json Cfg = json{{"Settings", json::object()}};   // no Repositories → BuildExecIndex sees only the bundle

private slots:
    // LoadNodes reads one-file-per-node, tags each with __FILE__, and skips non-node json.
    void load_nodes_reads_bundle_and_tags_files()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        writeNode(dir.path(), "base.json", json{{"NODE_ID", "base"}, {"TYPE", "Group"}});
        writeNode(dir.path(), "game.json", json{{"NODE_ID", "game"}, {"TYPE", "Group"},
                                               {"PARENTS", json::array({"base"})}});
        writeNode(dir.path(), "MANIFEST.json", json{{"LEGACY", true}});   // no NODE_ID → ignored

        PackageEditorModel m(&Cfg, nullptr);
        m.initPackage(dir.path(), nullptr);

        QCOMPARE((int)m.doc()["NODES"].size(), 2);
        for (const auto & n : m.doc()["NODES"])
            QVERIFY(n.contains("__FILE__") && !std::string(n["__FILE__"]).empty());
    }

    // An empty bundle dir yields one placeholder content node so the editor always has something to show.
    void load_empty_dir_seeds_placeholder_node()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        PackageEditorModel m(&Cfg, nullptr);
        m.initPackage(dir.path(), nullptr);
        QCOMPARE((int)m.doc()["NODES"].size(), 1);
        QCOMPARE(m.doc()["NODES"][0].value("TYPE", std::string()), std::string("Group"));   // pure composition, no identity
    }

    // FileForNode prefers <NODE_ID>.json (so a rename re-files), falling back to __FILE__ then untitled.
    void file_for_node_prefers_node_id()
    {
        PackageEditorModel m(&Cfg, nullptr);
        QCOMPARE(m.FileForNode(json{{"NODE_ID", "wine"}}), QString("wine.json"));
        QCOMPARE(m.FileForNode(json{{"NODE_ID", ""}, {"__FILE__", "kept.json"}}), QString("kept.json"));
        QCOMPARE(m.FileForNode(json{{"NODE_ID", ""}}), QString("untitled_node.json"));
    }

    // SaveNodes after a rename writes the new file, deletes the stale one, and fires savedToDisk.
    void save_nodes_renames_refiles_and_cleans_orphan()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        writeNode(dir.path(), "old.json", json{{"NODE_ID", "old"}, {"TYPE", "Group"}});

        PackageEditorModel m(&Cfg, nullptr);
        m.initPackage(dir.path(), nullptr);
        QCOMPARE((int)m.doc()["NODES"].size(), 1);

        QSignalSpy saved(&m, &PackageEditorModel::savedToDisk);
        m.doc()["NODES"][0]["NODE_ID"] = "renamed";
        m.SaveNodes();

        QVERIFY(QFile::exists(dir.path() + "/renamed.json"));   // re-filed under the new id
        QVERIFY(!QFile::exists(dir.path() + "/old.json"));      // stale file removed
        QVERIFY(saved.count() >= 1);
    }

    // replaceNodeJson swaps a node's whole body, preserves the __FILE__ tag, persists, and rebuilds.
    void replace_node_json_preserves_tag_and_reloads()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        writeNode(dir.path(), "n.json", json{{"NODE_ID", "n"}, {"TYPE", "Group"}});

        PackageEditorModel m(&Cfg, nullptr);
        m.initPackage(dir.path(), nullptr);

        QSignalSpy reloaded(&m, &PackageEditorModel::documentReloaded);
        json swapped = NodeFixture::Exec("win32", "n.exe"); swapped["NODE_ID"] = "n";
        m.replaceNodeJson(0, swapped);

        QCOMPARE(m.doc()["NODES"][0].value("TYPE", std::string()), std::string("DeclareExec"));
        QVERIFY(m.doc()["NODES"][0].contains("__FILE__"));   // provenance tag survived the swap
        QVERIFY(reloaded.count() >= 1);
    }

    // Revalidate emits validationChanged and surfaces a real graph error (VFS layer missing PATH).
    void revalidate_flags_bad_graph_and_signals()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        writeNode(dir.path(), "game.json",
                  json{{"NODE_ID", "game"}, {"TYPE", "Content"}, {"FORM", "dir"}});   // content with no PATH → error

        PackageEditorModel m(&Cfg, nullptr);
        m.initPackage(dir.path(), nullptr);   // LoadNodes already validated once

        QSignalSpy valspy(&m, &PackageEditorModel::validationChanged);
        m.Revalidate();
        QCOMPARE(valspy.count(), 1);
        QVERIFY(!m.validationErrors().empty());
    }

    // A well-formed graph validates clean (no errors).
    void revalidate_clean_graph_has_no_errors()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        writeNode(dir.path(), "base.json", json{{"NODE_ID", "base"}, {"TYPE", "Group"}});
        writeNode(dir.path(), "game.json", json{{"NODE_ID", "game"}, {"TYPE", "Group"},
                                               {"PARENTS", json::array({"base"})}});

        PackageEditorModel m(&Cfg, nullptr);
        m.initPackage(dir.path(), nullptr);
        QVERIFY(m.validationErrors().empty());
    }

    // KnownNodeIds lists the bundle's nodes; KnownPlatforms always includes the common targets.
    // A capture creates a NODE parented at the anchor — that is what "capture at this point in the chain"
    // means structurally. It must also not collide with an existing id.
    void create_node_parents_at_the_anchor_and_uniquifies()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        writeNode(dir.path(), "base.json", json{{"NODE_ID", "base"}, {"TYPE", "Group"}});

        PackageEditorModel m(&Cfg, nullptr);
        m.initPackage(dir.path(), nullptr);

        const std::string a = m.createNode(json{{"TYPE","Content"},{"FORM","dir"},{"PATH","cap"}}, {"base"}, "cap_files");
        QCOMPARE(a, std::string("cap_files"));
        const int ai = indexOfNode(m, a);
        QVERIFY(ai >= 0);
        QCOMPARE(m.doc()["NODES"][ai]["PARENTS"].size(), size_t(1));
        QCOMPARE(m.doc()["NODES"][ai]["PARENTS"][0].get<std::string>(), std::string("base"));
        QCOMPARE(m.doc()["NODES"][ai]["TYPE"].get<std::string>(), std::string("Content"));

        // A second capture with the same hint must not overwrite the first.
        const std::string b = m.createNode(json{{"TYPE","Content"},{"FORM","dir"},{"PATH","cap2"}}, {"base"}, "cap_files");
        QVERIFY(b != a);
        QVERIFY(indexOfNode(m, b) >= 0);

        // And it is on disk, not just in the document.
        PackageEditorModel m2(&Cfg, nullptr);
        m2.initPackage(dir.path(), nullptr);
        QVERIFY(indexOfNode(m2, a) >= 0);
        QVERIFY(indexOfNode(m2, b) >= 0);
    }

    // A multi-node file may hold entries we do not recognise as nodes. SaveNodes rewrites the whole file from
    // what it loaded, so anything skipped on load is erased from disk the next time that file is touched.
    void unrecognised_entries_in_a_node_file_survive_a_save()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        // One node PLUS a stray — the shape where the node would otherwise be re-filed to <id>.json and the
        // original file swept away, taking the stray with it.
        writeNode(dir.path(), "pack.json", json::array({
            json{{"NODE_ID", "a"}, {"TYPE", "Group"}},
            json{{"NOTE", "keep me"}} }));

        PackageEditorModel m(&Cfg, nullptr);
        m.initPackage(dir.path(), nullptr);
        QCOMPARE((int)m.doc()["NODES"].size(), 1);
        m.doc()["NODES"][0]["PARENTS"] = json::array();       // touch something
        m.SaveNodes();

        QFile f(dir.path() + "/pack.json");
        QVERIFY2(f.exists(), "the file carrying the stray must not be swept");
        QVERIFY(f.open(QIODevice::ReadOnly));
        const json back = json::parse(f.readAll().toStdString(), nullptr, false);
        QVERIFY(back.is_array());
        bool sawNode = false, sawStray = false;
        for (const auto &e : back)
        {
            if (e.value("NODE_ID", std::string()) == "a") sawNode = true;
            if (e.value("NOTE", std::string()) == "keep me") sawStray = true;
        }
        QVERIFY(sawNode);
        QVERIFY2(sawStray, "the unrecognised entry was erased");
    }

    void known_node_ids_and_platforms()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        writeNode(dir.path(), "base.json", json{{"NODE_ID", "base"}, {"TYPE", "Group"}});

        PackageEditorModel m(&Cfg, nullptr);
        m.initPackage(dir.path(), nullptr);

        const auto ids = m.KnownNodeIds();
        QVERIFY(std::find(ids.begin(), ids.end(), "base") != ids.end());

        const auto plats = m.KnownPlatforms();
        QVERIFY(std::find(plats.begin(), plats.end(), "win32") != plats.end());
        QVERIFY(std::find(plats.begin(), plats.end(), "linux64") != plats.end());
    }

    // The blueprint canvas is Dear ImGui, which keeps ONE global context per process: a second editor's canvas
    // cannot render, and used to show up as a blank/garbage widget with the reason only in the log. OpenFor is
    // the single door - it raises the editor that is already open instead of building one that cannot work.
    void onlyOneEditorIsEverOpen()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        writeNode(dir.path(), "solo.json", json{{"NODE_ID", "solo"}, {"TYPE", "Group"}});
        json cfg = json{{"Settings", json::object()}};

        bool createdA = false, createdB = false;
        PackageEditor *a = PackageEditor::OpenFor(&cfg, nullptr, dir.path(), &createdA);
        QVERIFY(a);
        QVERIFY(createdA);
        PackageEditor *b = PackageEditor::OpenFor(&cfg, nullptr, dir.path(), &createdB);
        QCOMPARE(b, a);                 // the SAME editor, raised
        QVERIFY(!createdB);             // ...and the caller knows not to wire its signals twice

        delete a;                       // closing it releases the slot
        bool createdC = false;
        PackageEditor *c = PackageEditor::OpenFor(&cfg, nullptr, dir.path(), &createdC);
        QVERIFY(c && createdC);
        delete c;
    }
};

QTEST_MAIN(PackageEditorModelTest)
#include "test_packageeditormodel.moc"
