#include "jsonraweditor.h"
#include "packageeditormodel.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

using json = nlohmann::ordered_json;

JsonRawEditor::JsonRawEditor(PackageEditorModel * model, QWidget * parent)
    : QWidget(parent), Model(model)
{
    QVBoxLayout * Layout = new QVBoxLayout(this);

    QHBoxLayout * FileRow = new QHBoxLayout();
    FileRow->addWidget(new QLabel("Node:", this));
    FileCombo = new QComboBox(this);
    FileRow->addWidget(FileCombo, 1);
    Layout->addLayout(FileRow);

    Text = new QTextEdit(this);
    Layout->addWidget(Text);
    connect(Text, &QTextEdit::textChanged, this, &JsonRawEditor::onTextChanged);

    SaveBtn = new QPushButton("Apply now", this);   // live apply is automatic; this is just an immediate flush
    Layout->addWidget(SaveBtn);
    connect(SaveBtn, &QPushButton::clicked, this, &JsonRawEditor::onSavePressed);

    // Live apply: a valid edit flows to the node after a short debounce — no Save click, and the canvas repaints.
    ApplyTimer = new QTimer(this);
    ApplyTimer->setSingleShot(true);
    connect(ApplyTimer, &QTimer::timeout, this, &JsonRawEditor::onApplyTimeout);

    rebuildCombo();
    // Structural change (a node added/removed) → refresh the node list, preserving the current pick by id.
    connect(Model, &PackageEditorModel::documentReloaded, this, &JsonRawEditor::rebuildCombo);
    connect(FileCombo, &QComboBox::currentIndexChanged, this, &JsonRawEditor::refreshText);
    // A disk write elsewhere → re-show the current node's JSON, UNLESS it was our own live apply (which would
    // otherwise reset the cursor mid-type).
    connect(Model, &PackageEditorModel::savedToDisk, this, &JsonRawEditor::refreshText);
    refreshText();
}

void JsonRawEditor::refreshText()
{
    if (!Text || !FileCombo || ApplyingLocally) return;
    const int Idx = FileCombo->currentIndex();
    QSignalBlocker B(Text);
    const auto & Nodes = Model->doc()["NODES"];
    if (Idx < 0 || Idx >= (int)Nodes.size()) { Text->setText("{}"); return; }
    json Out = Nodes[Idx]; Out.erase("__FILE__");
    Text->setText(QString::fromStdString(Out.dump(4)));
}

void JsonRawEditor::showNode(const QString & nodeId)
{
    if (!FileCombo) return;
    int Idx = FileCombo->findText(nodeId);
    if (Idx < 0) { rebuildCombo(); Idx = FileCombo->findText(nodeId); }
    if (Idx < 0) return;
    if (Idx == FileCombo->currentIndex()) { refreshText(); return; }
    QSignalBlocker B(FileCombo);
    FileCombo->setCurrentIndex(Idx);
    refreshText();
}

void JsonRawEditor::onTextChanged()
{
    if (!Text || !SaveBtn) return;
    const bool Valid = json::accept(Text->toPlainText().toUtf8());
    Text->setStyleSheet(Valid ? "" : "background-color:#58111A; color: white;");
    SaveBtn->setDisabled(!Valid);
    if (Valid && ApplyTimer) ApplyTimer->start(300);   // debounced live apply
}

void JsonRawEditor::onApplyTimeout()
{
    const int Idx = FileCombo ? FileCombo->currentIndex() : -1;
    if (Idx < 0 || Idx >= (int)Model->doc()["NODES"].size()) return;
    const QByteArray Data = Text->toPlainText().toUtf8();
    if (!json::accept(Data)) return;   // still invalid — do not write (red style already shown)
    ApplyingLocally = true;
    Model->updateNodeLive(Idx, json::parse(Data));   // content edit: canvas repaints, no shell rebuild
    ApplyingLocally = false;
}

void JsonRawEditor::onSavePressed()
{
    const int Idx = FileCombo ? FileCombo->currentIndex() : -1;
    if (Idx < 0 || Idx >= (int)Model->doc()["NODES"].size()) return;
    QByteArray Data = Text->toPlainText().toUtf8();
    if (!json::accept(Data)) { QMessageBox::warning(this, "Apply", "Invalid JSON — not applied."); return; }
    ApplyingLocally = true;
    Model->updateNodeLive(Idx, json::parse(Data));    // preserves __FILE__, saves, repaints — no widget rebuild
    ApplyingLocally = false;
}

void JsonRawEditor::rebuildCombo()
{
    if (!FileCombo) return;
    const QString Keep = FileCombo->currentText();
    QSignalBlocker B(FileCombo);
    FileCombo->clear();
    const auto & Nodes = Model->doc()["NODES"];
    for (int n = 0; n < (int)Nodes.size(); n++)
    {
        const std::string Id = Nodes[n].value("LABEL", std::string());
        FileCombo->addItem(QString::fromStdString(Id.empty() ? ("node " + std::to_string(n + 1)) : Id));
    }
    const int K = FileCombo->findText(Keep);
    FileCombo->setCurrentIndex(K >= 0 ? K : (FileCombo->count() ? 0 : -1));
}
