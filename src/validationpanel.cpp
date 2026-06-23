#include "validationpanel.h"
#include "packageeditormodel.h"

#include <QTextEdit>
#include <QVBoxLayout>

ValidationPanel::ValidationPanel(PackageEditorModel * model, QWidget * parent)
    : QGroupBox("Validation", parent), Model(model)
{
    QVBoxLayout * Layout = new QVBoxLayout(this);
    Layout->setContentsMargins(6, 2, 6, 6);
    View = new QTextEdit(this);
    View->setReadOnly(true);
    View->setMaximumHeight(150);
    Layout->addWidget(View);

    if (Model) connect(Model, &PackageEditorModel::validationChanged, this, &ValidationPanel::refresh);
    refresh();
}

void ValidationPanel::refresh()
{
    if (!Model) return;
    const auto & Errors   = Model->validationErrors();
    const auto & Warnings = Model->validationWarnings();

    if (Errors.empty() && Warnings.empty())
        setTitle("Validation  —  ✓ OK");
    else if (Errors.empty())
        setTitle(QString("Validation  —  %1 warning(s)").arg(Warnings.size()));
    else
        setTitle(QString("⚠ Validation  —  %1 error(s), %2 warning(s)").arg(Errors.size()).arg(Warnings.size()));

    QString Html;
    if (Errors.empty() && Warnings.empty())
        Html = "<span style='color:#3fae5a'>✓ No problems found.</span>";
    else
    {
        for (const auto & E : Errors)
            Html += "<div style='color:#d9534f'><b>ERROR:</b> " + QString::fromStdString(E).toHtmlEscaped() + "</div>";
        for (const auto & W : Warnings)
            Html += "<div style='color:#c9a227'>warning: " + QString::fromStdString(W).toHtmlEscaped() + "</div>";
    }
    View->setHtml(Html);
    setStyleSheet(Errors.empty() ? QString()
                                 : "QGroupBox{border:1px solid #d9534f;border-radius:4px;margin-top:6px;}"
                                   "QGroupBox::title{subcontrol-origin:margin;left:8px;color:#d9534f;}");
}
