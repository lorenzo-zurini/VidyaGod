#include "jsonraweditor.h"
#include "packageeditormodel.h"

#include <QFontDatabase>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTimer>
#include <QVBoxLayout>

using json = nlohmann::ordered_json;

JsonRawEditor::JsonRawEditor(PackageEditorModel * model, QWidget * parent)
    : QWidget(parent), Model(model)
{
    QVBoxLayout * L = new QVBoxLayout(this);
    L->setContentsMargins(6, 6, 6, 6);
    L->setSpacing(4);
    Title = new QLabel(this);
    Title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    Title->setWordWrap(true);
    L->addWidget(Title);
    Text = new QPlainTextEdit(this);
    Text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    Text->setLineWrapMode(QPlainTextEdit::NoWrap);
    Text->setTabStopDistance(4 * Text->fontMetrics().horizontalAdvance(' '));
    L->addWidget(Text, 1);
    Status = new QLabel(this);
    Status->setWordWrap(true);
    L->addWidget(Status);
    ApplyTimer = new QTimer(this);
    ApplyTimer->setSingleShot(true);
    connect(ApplyTimer, &QTimer::timeout, this, &JsonRawEditor::onApplyTimeout);
    connect(Text, &QPlainTextEdit::textChanged, this, &JsonRawEditor::onTextChanged);
    if (Model)
    {
        connect(Model, &PackageEditorModel::documentReloaded, this, [this] { showNode(QString()); });
        connect(Model, &PackageEditorModel::nodeContentChanged, this, &JsonRawEditor::refresh);
        connect(Model, &PackageEditorModel::handlesRenamed, this, [this] {
            const auto & R = Model->lastRenames();
            if (const auto It = R.find(Handle); It != R.end()) Handle = It->second;
            refresh();
        });
    }
    showNode(QString());
}

void JsonRawEditor::showNode(const QString & H)
{
    if (ApplyTimer->isActive()) { ApplyTimer->stop(); onApplyTimeout(); }   // a pending edit lands on the node it was for
    Handle = H.toStdString();
    Shown.clear();
    refresh();
}

void JsonRawEditor::refresh()
{
    if (Applying || !Model) return;
    const int I = Handle.empty() ? -1 : Model->doc().IndexOf(Handle);
    if (I < 0)
    {
        Title->setText("<span style='color:#888'>Select a node on the canvas to see and edit its JSON.</span>");
        Text->blockSignals(true); Text->setPlainText(QString()); Text->blockSignals(false);
        Text->setEnabled(false);
        Status->clear();
        Shown.clear();
        return;
    }
    Text->setEnabled(true);
    const json & N = Model->doc().Node(I);
    const std::string Label = N.is_object() && N.contains("LABEL") && N["LABEL"].is_string() ? N["LABEL"].get<std::string>() : std::string();
    const bool Draft = Handle.rfind("draft-", 0) == 0;
    Title->setText("<b>" + QString::fromStdString(Label.empty() ? std::string("(unnamed)") : Label).toHtmlEscaped() + "</b><br>"
                   "<span style='color:#888'>" + (Draft ? QString("new - named when the package is saved")
                                                        : QString::fromStdString(Handle)) + "</span>");
    const std::string Now = N.dump(2);
    //Changed elsewhere (the canvas, an undo) while not being typed into: show the new content.
    if (Now != Shown && !Text->hasFocus())
    {
        Text->blockSignals(true);
        Text->setPlainText(QString::fromStdString(Now));
        Text->blockSignals(false);
        Shown = Now;
        Status->clear();
        Text->setStyleSheet(QString());
    }
}

void JsonRawEditor::onTextChanged()
{
    const QByteArray Data = Text->toPlainText().toUtf8();
    const bool Valid = json::accept(Data);
    Text->setStyleSheet(Valid ? QString() : QString("QPlainTextEdit{background-color:#3a1418;}"));
    if (!Valid)
    {
        //Say where, not just that: the parser's own message has the line and column.
        try { const json Probe = json::parse(Data); (void)Probe; }
        catch (const json::parse_error & E) { Status->setText("<span style='color:#e07070'>" + QString(E.what()).toHtmlEscaped() + "</span>"); }
        ApplyTimer->stop();
        return;
    }
    Status->setText("<span style='color:#888'>applying...</span>");
    ApplyTimer->start(350);
}

void JsonRawEditor::onApplyTimeout()
{
    if (!Model || Handle.empty()) return;
    const QByteArray Data = Text->toPlainText().toUtf8();
    if (!json::accept(Data)) return;
    json N = json::parse(Data);
    if (!N.is_object()) { Status->setText("<span style='color:#e07070'>A node is a JSON object.</span>"); return; }
    Applying = true;
    Model->replaceNode(Handle, N);
    Applying = false;
    Shown = N.dump(2);
    Status->setText("<span style='color:#6c6'>applied (Ctrl+Z on the canvas undoes it)</span>");
}
