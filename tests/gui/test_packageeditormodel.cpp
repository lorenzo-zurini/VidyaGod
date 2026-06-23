// Tests for PackageEditorModel — the node-native bundle editor's state/signal hub (the AppModel-style central
// structure from the PackageEditor de-god). Drives the non-UI surface: node file I/O (LoadNodes/SaveNodes with
// rename re-filing + orphan cleanup), replaceNodeJson, validation signalling, and the catalog queries. Authoring
// runs (RunInNode/AnalyzeNodeRegistry) raise modal dialogs + build live containers, so they're out of scope here.
// Uses a real temp bundle dir and an empty GlobalConfig (no repositories → BuildExecIndex sees only this bundle).

#include <QtTest>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>

#include "packageeditormodel.h"

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

class PackageEditorModelTest : public QObject
{
    Q_OBJECT
    json Cfg = json{{"Settings", json::object()}};   // no Repositories → BuildExecIndex sees only the bundle

private slots:
    // LoadNodes reads one-file-per-node, tags each with __FILE__, and skips non-node json.
    void load_nodes_reads_bundle_and_tags_files()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        writeNode(dir.path(), "base.json", json{{"NODE_ID", "base"}, {"ROLE", "content"}, {"LAYERS", json::array()}});
        writeNode(dir.path(), "game.json", json{{"NODE_ID", "game"}, {"ROLE", "launchable"}, {"PARENTS", json::array({"base"})},
            {"LAYERS", json::array()}});
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
        QCOMPARE(m.doc()["NODES"][0].value("ROLE", std::string()), std::string("content"));
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
        writeNode(dir.path(), "old.json", json{{"NODE_ID", "old"}, {"ROLE", "content"}, {"LAYERS", json::array()}});

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
        writeNode(dir.path(), "n.json", json{{"NODE_ID", "n"}, {"ROLE", "content"}, {"LAYERS", json::array()}});

        PackageEditorModel m(&Cfg, nullptr);
        m.initPackage(dir.path(), nullptr);

        QSignalSpy reloaded(&m, &PackageEditorModel::documentReloaded);
        m.replaceNodeJson(0, json{{"NODE_ID", "n"}, {"ROLE", "launchable"}, {"LAYERS", json::array()}});

        QCOMPARE(m.doc()["NODES"][0].value("ROLE", std::string()), std::string("launchable"));
        QVERIFY(m.doc()["NODES"][0].contains("__FILE__"));   // provenance tag survived the swap
        QVERIFY(reloaded.count() >= 1);
    }

    // Revalidate emits validationChanged and surfaces a real graph error (VFS layer missing PATH).
    void revalidate_flags_bad_graph_and_signals()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        writeNode(dir.path(), "game.json", json{{"NODE_ID", "game"}, {"ROLE", "launchable"},
            {"LAYERS", json::array({ json{{"TYPE", "VFSDirLayer"}} })}});   // VFS layer with no PATH → error

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
        writeNode(dir.path(), "base.json", json{{"NODE_ID", "base"}, {"ROLE", "content"}, {"LAYERS", json::array()}});
        writeNode(dir.path(), "game.json", json{{"NODE_ID", "game"}, {"ROLE", "launchable"},
            {"PARENTS", json::array({"base"})}, {"LAYERS", json::array()}});

        PackageEditorModel m(&Cfg, nullptr);
        m.initPackage(dir.path(), nullptr);
        QVERIFY(m.validationErrors().empty());
    }

    // KnownNodeIds lists the bundle's nodes; KnownPlatforms always includes the common targets.
    void known_node_ids_and_platforms()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        writeNode(dir.path(), "base.json", json{{"NODE_ID", "base"}, {"ROLE", "content"}, {"LAYERS", json::array()}});

        PackageEditorModel m(&Cfg, nullptr);
        m.initPackage(dir.path(), nullptr);

        const auto ids = m.KnownNodeIds();
        QVERIFY(std::find(ids.begin(), ids.end(), "base") != ids.end());

        const auto plats = m.KnownPlatforms();
        QVERIFY(std::find(plats.begin(), plats.end(), "win32") != plats.end());
        QVERIFY(std::find(plats.begin(), plats.end(), "linux64") != plats.end());
    }
};

QTEST_MAIN(PackageEditorModelTest)
#include "test_packageeditormodel.moc"
