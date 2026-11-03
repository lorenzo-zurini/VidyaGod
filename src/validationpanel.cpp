#include "validationpanel.h"
#include "packageeditormodel.h"
#include "pkggraph.h"

#include <QLabel>
#include <QListWidget>
#include <QRegularExpression>
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
        else if (const QString A = It->data(Qt::UserRole + 1).toString(); !A.isEmpty()) emit fixClicked(A);
    });
    if (Model) connect(Model, &PackageEditorModel::validationChanged, this, &ValidationPanel::refresh);
    refresh();
}

//A message as a person reads it: the node by its name ("node '<cid>' (TUNIC): ..." -> "TUNIC: ..."); the whole
//message, CID and all, stays in the tooltip.
QString ValidationPanel::readable(const std::string & M)
{
    //The label runs to the first "): " - a label may hold parentheses itself ("Original (IWD2)").
    static const QRegularExpression Named("^node '[^']*' \\((.*?)\\):\\s*");
    return QString::fromStdString(M).replace(Named, "\\1: ");
}

QString ValidationPanel::named(const std::string & M) const
{
    QString S = readable(M);
    static const QRegularExpression Bare("^node '([^']*)':\\s*");
    const QRegularExpressionMatch Hit = Bare.match(S);
    if (!Hit.hasMatch() || !Model) return S;
    const QString Label = labelOf(Hit.captured(1));
    return Label.isEmpty() ? S : Label + ": " + S.mid(Hit.capturedLength());
}

QString ValidationPanel::labelOf(const QString & Handle) const
{
    const int I = Model ? Model->doc().IndexOf(Handle.toStdString()) : -1;
    return I < 0 ? QString() : QString::fromStdString(Model->doc().Node(I).value("LABEL", std::string()));
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
    //Waiting for a step the author is about to take (files not brought in yet) is no error: said calmly, in grey.
    size_t Waiting = 0;
    for (const auto & E : Errors) if (PkgGraph::FixFor(E).Waiting) ++Waiting;
    const size_t Real = Errors.size() - Waiting;
    if (!Real && Warnings.empty()) setTitle("Validation - waiting for the game's files");
    else setTitle(QString("Validation - %1 error(s), %2 warning(s)").arg(Real).arg(Warnings.size())
                  + (Waiting ? QString(", waiting for files") : QString()));
    Hint->hide();
    List->show();
    auto Add = [&](const std::string & M, const QColor & C, const QString & Said) {
        auto * It = new QListWidgetItem(Said, List);
        It->setForeground(C);
        It->setToolTip(QString::fromStdString(M));
        It->setData(Qt::UserRole, QString::fromStdString(Model->handleInMessage(M)));
    };
    for (const auto & E : Errors)
    {
        const PkgGraph::Fix F = PkgGraph::FixFor(E);
        if (!F.Waiting && !F.Action.empty() && Model->handleInMessage(E).empty())
        {
            //A problem of the whole package with a fix: clicking it does the fix.
            Add(E, QColor(0xe0, 0x70, 0x70), named(E) + "  - click to " + QString::fromStdString(F.Label));
            List->item(List->count() - 1)->setData(Qt::UserRole + 1, QString::fromStdString(F.Action));
        }
        else if (!F.Waiting) Add(E, QColor(0xe0, 0x70, 0x70), named(E));
        else
        {
            //The node by its label as the message gives it (a title may hold a colon: "S.T.A.L.K.E.R.: ...").
            static const QRegularExpression Named("^node '([^']*)'(?: \\((.*?)\\):)?");
            const QRegularExpressionMatch Mt = Named.match(QString::fromStdString(E));
            const QString Who = !Mt.hasMatch() ? QString("A node") : Mt.captured(2).isEmpty() ? Mt.captured(1) : Mt.captured(2);
            Add(E, QColor(0x9a, 0xa2, 0xb0), Who + ": " + QString::fromStdString(F.Note) + " - " + QString::fromStdString(F.Label));
        }
    }
    for (const auto & W : Warnings)
    {
        const PkgGraph::Fix F = PkgGraph::FixFor(W);
        Add(W, QColor(0xd4, 0xb0, 0x40), named(W) + (F.Action.empty() || !Model->handleInMessage(W).empty() ? QString()
                                                         : "  - click to " + QString::fromStdString(F.Label)));
        if (!F.Action.empty() && Model->handleInMessage(W).empty()) List->item(List->count() - 1)->setData(Qt::UserRole + 1, QString::fromStdString(F.Action));
    }
}
