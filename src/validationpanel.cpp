#include "validationpanel.h"
#include "packageeditormodel.h"

#include <QLabel>
#include <QListWidget>
#include <QVBoxLayout>

ValidationPanel::ValidationPanel(PackageEditorModel * model, QWidget * parent)
    : QGroupBox("Validation", parent), Model(model)
{
    QVBoxLayout * L = new QVBoxLayout(this);
    L->setContentsMargins(6, 2, 6, 6);
    Hint = new QLabel(this);
    Hint->setWordWrap(true);
    L->addWidget(Hint);
    List = new QListWidget(this);
    List->setWordWrap(true);
    List->setMaximumHeight(170);
    List->setToolTip("Click a problem to go to its node");
    L->addWidget(List);
    connect(List, &QListWidget::itemClicked, this, [this](QListWidgetItem * It) {
        const QString H = It->data(Qt::UserRole).toString();
        if (!H.isEmpty()) emit nodeClicked(H);
    });
    if (Model) connect(Model, &PackageEditorModel::validationChanged, this, &ValidationPanel::refresh);
    refresh();
}

void ValidationPanel::refresh()
{
    if (!Model) return;
    List->clear();
    if (!Model->validated())
    {
        setTitle("Validation - not checked");
        Hint->setText("<span style='color:#888'>Validate checks references, layer paths, runner resolution and case "
                      "collisions for the package as it is now, unsaved edits included.</span>");
        Hint->show();
        List->hide();
        return;
    }
    const auto & Errors = Model->validationErrors();
    const auto & Warnings = Model->validationWarnings();
    if (Errors.empty() && Warnings.empty())
    {
        setTitle("Validation - no problems");
        Hint->setText("<span style='color:#3fae5a'>✓ No problems found.</span>");
        Hint->show();
        List->hide();
        return;
    }
    setTitle(QString("Validation - %1 error(s), %2 warning(s)").arg(Errors.size()).arg(Warnings.size()));
    Hint->hide();
    List->show();
    auto Add = [&](const std::string & M, bool Error) {
        auto * It = new QListWidgetItem(QString::fromStdString(M), List);
        It->setForeground(Error ? QColor(0xe0, 0x70, 0x70) : QColor(0xd4, 0xb0, 0x40));
        It->setData(Qt::UserRole, QString::fromStdString(Model->handleInMessage(M)));
    };
    for (const auto & E : Errors) Add(E, true);
    for (const auto & W : Warnings) Add(W, false);
}
