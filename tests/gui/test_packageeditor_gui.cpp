// GUI tests for the PackageEditor de-god: drive the real PackageEditorModel + section widgets in-process with
// QTest, asserting that edits flow into the working document and that the model's signals fire. Runs headless under
// the offscreen QPA platform (set by ctest). These cover the wiring that the de-god introduced — field edits →
// doc, structural edits → documentReloaded, save/validate round-trips — without needing a display or hardware.

#include <QtTest>
#include <QLineEdit>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <nlohmann/json.hpp>

#include "packageeditor.h"
#include "packageeditormodel.h"
#include "nodesections.h"
#include "jsonoperations.h"

using json = nlohmann::ordered_json;

namespace {
// A minimal GlobalConfig with no repositories, so BuildExecIndex only scans the bundle under test (no network/IPFS).
json MinimalConfig()
{
    return json{{"Settings", {{"Repositories", json::array()}}}};
}

// Write a node fragment <id>.json into dir.
void WriteNode(const QString & dir, const QString & id, const json & extra = json::object())
{
    json node = json{{"NODE_ID", id.toStdString()}, {"ROLE", "content"}, {"LAYERS", json::array()}};
    for (auto & [k, v] : extra.items()) node[k] = v;
    QFile f(dir + "/" + id + ".json");
    JSONOps::SaveJSON(&node, &f);
}

// Find the QLineEdit in `root` whose "JSONPath" property equals `pointer`.
QLineEdit * FieldByPath(QWidget * root, const QString & pointer)
{
    for (QLineEdit * le : root->findChildren<QLineEdit *>())
        if (le->property("JSONPath").toString() == pointer) return le;
    return nullptr;
}
}

class PackageEditorGuiTest : public QObject
{
    Q_OBJECT
private slots:
    // Empty bundle → the model seeds one default node so the editor always has something to show.
    void loads_default_node_for_empty_bundle()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const json cfg = MinimalConfig();
        PackageEditorModel model(&cfg, nullptr);
        model.initPackage(dir.path(), nullptr);
        QCOMPARE((int)model.doc()["NODES"].size(), 1);
    }

    // Load real node files, then a value edit + SaveNodes persists to disk and survives a reload.
    void save_roundtrip_persists_field()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        WriteNode(dir.path(), "game");
        const json cfg = MinimalConfig();
        PackageEditorModel model(&cfg, nullptr);
        model.initPackage(dir.path(), nullptr);
        QCOMPARE((int)model.doc()["NODES"].size(), 1);

        model.doc()[json::json_pointer("/NODES/0/META/TITLE")] = "Hello";
        model.SaveNodes();

        PackageEditorModel reloaded(&cfg, nullptr);
        reloaded.initPackage(dir.path(), nullptr);
        QCOMPARE(reloaded.doc()["NODES"][0]["META"].value("TITLE", std::string()), std::string("Hello"));
    }

    // replaceNodeJson swaps a node (keeping __FILE__) and asks the views to rebuild.
    void replaceNodeJson_emits_documentReloaded()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        WriteNode(dir.path(), "game");
        const json cfg = MinimalConfig();
        PackageEditorModel model(&cfg, nullptr);
        model.initPackage(dir.path(), nullptr);

        QSignalSpy reloadSpy(&model, &PackageEditorModel::documentReloaded);
        model.replaceNodeJson(0, json{{"NODE_ID", "game"}, {"ROLE", "launchable"}, {"LAYERS", json::array()}});
        QCOMPARE(reloadSpy.count(), 1);
        QCOMPARE(model.doc()["NODES"][0].value("ROLE", std::string()), std::string("launchable"));
    }

    // SaveNodes re-runs validation and notifies the panel.
    void save_emits_validationChanged()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        WriteNode(dir.path(), "game");
        const json cfg = MinimalConfig();
        PackageEditorModel model(&cfg, nullptr);
        model.initPackage(dir.path(), nullptr);

        QSignalSpy valSpy(&model, &PackageEditorModel::validationChanged);
        model.SaveNodes();
        QVERIFY(valSpy.count() >= 1);
    }

    // The Identity section: editing the NODE_ID field writes the doc and triggers a structural rebuild.
    void identity_section_edits_model()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        WriteNode(dir.path(), "game");
        const json cfg = MinimalConfig();
        PackageEditorModel model(&cfg, nullptr);
        model.initPackage(dir.path(), nullptr);

        NodeIdentitySection section(&model, 0);
        QLineEdit * idField = FieldByPath(&section, "/NODES/0/NODE_ID");
        QVERIFY2(idField, "Identity section should expose a NODE_ID field bound to /NODES/0/NODE_ID");

        QSignalSpy reloadSpy(&model, &PackageEditorModel::documentReloaded);
        idField->setText("renamed");
        QTest::keyClick(idField, Qt::Key_Return);   // → editingFinished → onFieldEdited + requestReload

        QCOMPARE(model.doc()["NODES"][0].value("NODE_ID", std::string()), std::string("renamed"));
        QCOMPARE(reloadSpy.count(), 1);
    }

    // Regression: renaming a NODE_ID with Enter (field still focused) triggers a tab rebuild that deleteLater()s the
    // page holding that focused field. On destruction the field fired editingFinished → a slot on the half-destroyed
    // section → Qt abort. BuildUI now blocks the old subtree's signals first. This must complete without crashing.
    void rename_node_id_via_enter_does_not_crash_on_rebuild()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        WriteNode(dir.path(), "game");
        const json cfg = MinimalConfig();

        PackageEditor editor(&cfg, nullptr, dir.path());
        editor.resize(900, 600);
        editor.show();
        QApplication::processEvents();

        QLineEdit * idField = nullptr;
        for (QLineEdit * le : editor.findChildren<QLineEdit *>())
            if (le->property("JSONPath").toString() == "/NODES/0/NODE_ID") { idField = le; break; }
        QVERIFY2(idField, "expected a /NODES/0/NODE_ID field in the open editor");

        idField->setFocus();
        idField->setText("renamed_via_enter");
        QTest::keyClick(idField, Qt::Key_Return);   // → onFieldEdited + requestReload → BuildUI → deleteLater(old page)
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);  // run the deleteLater that would crash
        QApplication::processEvents();

        // Reaching here without aborting is the assertion; confirm the rename took (file re-filed).
        QVERIFY(QFile::exists(dir.path() + "/renamed_via_enter.json"));
    }

    // The Layers section renders per-TYPE sub-editors for the node's existing layers.
    void layers_section_renders_existing_layer()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        WriteNode(dir.path(), "game",
                  json{{"LAYERS", json::array({ json{{"TYPE", "RegEdit"}, {"REGPATH", "HKCU\\X"}, {"KEYVALUES", json::object()}} })}});
        const json cfg = MinimalConfig();
        PackageEditorModel model(&cfg, nullptr);
        model.initPackage(dir.path(), nullptr);

        NodeLayersSection section(&model, 0);
        QVERIFY2(FieldByPath(&section, "/NODES/0/LAYERS/0/REGPATH"),
                 "Layers section should render a RegEdit REGPATH field for the existing layer");
    }
};

QTEST_MAIN(PackageEditorGuiTest)
#include "test_packageeditor_gui.moc"
