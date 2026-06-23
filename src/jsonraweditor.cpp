#include "jsonraweditor.h"
#include "packageeditormodel.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTextEdit>
#include <QVBoxLayout>

using json = nlohmann::ordered_json;

JsonRawEditor::JsonRawEditor(PackageEditorModel * model, QWidget * parent)
    : QWidget(parent), Model(model)
{
    QVBoxLayout * Layout = new QVBoxLayout(this);

    QHBoxLayout * FileRow = new QHBoxLayout();
    FileRow->addWidget(new QLabel("Node:", this));
    FileCombo = new QComboBox(this);
    const auto & Nodes = Model->doc()["NODES"];
    for (int n = 0; n < (int)Nodes.size(); n++)
    {
        const std::string Id = Nodes[n].value("NODE_ID", std::string());
        FileCombo->addItem(QString::fromStdString(Id.empty() ? ("node " + std::to_string(n + 1)) : Id));
    }
    FileRow->addWidget(FileCombo, 1);
    Layout->addLayout(FileRow);

    Text = new QTextEdit(this);
    Layout->addWidget(Text);
    connect(Text, &QTextEdit::textChanged, this, &JsonRawEditor::onTextChanged);

    SaveBtn = new QPushButton("Save Node", this);
    Layout->addWidget(SaveBtn);
    connect(SaveBtn, &QPushButton::clicked, this, &JsonRawEditor::onSavePressed);

    connect(FileCombo, &QComboBox::currentIndexChanged, this, &JsonRawEditor::refreshText);
    // A disk write elsewhere (e.g. a field edit on a node tab) → re-show the current node's JSON.
    connect(Model, &PackageEditorModel::savedToDisk, this, &JsonRawEditor::refreshText);
    refreshText();
}

void JsonRawEditor::refreshText()
{
    if (!Text || !FileCombo) return;
    const int Idx = FileCombo->currentIndex();
    QSignalBlocker B(Text);
    const auto & Nodes = Model->doc()["NODES"];
    if (Idx < 0 || Idx >= (int)Nodes.size()) { Text->setText("{}"); return; }
    json Out = Nodes[Idx]; Out.erase("__FILE__");
    Text->setText(QString::fromStdString(Out.dump(4)));
}

void JsonRawEditor::onTextChanged()
{
    if (!Text || !SaveBtn) return;
    const bool Valid = json::accept(Text->toPlainText().toUtf8());
    Text->setStyleSheet(Valid ? "" : "background-color:#58111A; color: white;");
    SaveBtn->setDisabled(!Valid);
}

void JsonRawEditor::onSavePressed()
{
    const int Idx = FileCombo ? FileCombo->currentIndex() : -1;
    if (Idx < 0 || Idx >= (int)Model->doc()["NODES"].size()) return;
    QByteArray Data = Text->toPlainText().toUtf8();
    if (!json::accept(Data)) { QMessageBox::warning(this, "Save Node", "Invalid JSON — not saved."); return; }
    Model->replaceNodeJson(Idx, json::parse(Data));   // model preserves __FILE__, saves, and emits documentReloaded
}
