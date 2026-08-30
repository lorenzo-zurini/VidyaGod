#include "a11ynames.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextEdit>
#include <QWidget>

namespace A11yNames {
namespace {

// "Add + fetch"  → "AddFetch"; "&Save…" → "Save". objectNames are used by findChild in tests, so keep them
// terse and free of the punctuation that makes a name annoying to type.
QString Slug(const QString &Text)
{
    QString S = Text;
    S.remove('&');                                              // Qt mnemonic markers
    S.replace(QRegularExpression("[^A-Za-z0-9]+"), " ");         // punctuation/ellipses → separators
    QString Out;
    for (const QString &Word : S.split(' ', Qt::SkipEmptyParts))
        Out += Word.left(1).toUpper() + Word.mid(1);
    return Out.left(48);
}

// Human-facing label: keep the words, drop only the mnemonic marker and trailing separators, so the accessible
// name matches what the user actually reads on screen ("Close to tray", not "CloseToTray").
QString Pretty(const QString &Text)
{
    QString S = Text;
    S.remove('&');
    S.replace(QChar(0x2026), "");                               // …
    while (S.endsWith(':') || S.endsWith(' ') || S.endsWith('.')) S.chop(1);
    return S.trimmed();
}

// The QFormLayout label sitting beside this field, if any. This is the single most useful source of a name for
// an otherwise anonymous QLineEdit/QComboBox/QSpinBox — the user reads exactly this text next to the control.
QString FormLabelFor(QWidget *W)
{
    for (QWidget *P = W->parentWidget(); P; P = P->parentWidget())
    {
        if (auto *Form = qobject_cast<QFormLayout *>(P->layout()))
            if (QWidget *L = Form->labelForField(W))
                if (auto *Lbl = qobject_cast<QLabel *>(L))
                    return Pretty(Lbl->text());
        if (P->isWindow()) break;
    }
    return {};
}

// The QLabel immediately preceding this widget in its own layout — the non-form-layout equivalent of the above
// (rows built with QHBoxLayout: [label][field]).
QString PrecedingLabel(QWidget *W)
{
    QWidget *P = W->parentWidget();
    if (!P || !P->layout()) return {};
    QLayout *L = P->layout();
    int Idx = L->indexOf(W);
    for (int i = Idx - 1; i >= 0; --i)
    {
        QLayoutItem *It = L->itemAt(i);
        if (!It || !It->widget()) continue;
        if (auto *Lbl = qobject_cast<QLabel *>(It->widget())) return Pretty(Lbl->text());
        break;   // the nearest widget is not a label — do not reach further back and invent an association
    }
    return {};
}

// The enclosing group box's title, used to disambiguate repeated controls ("Remove" inside the "Demo" card).
QString GroupTitle(QWidget *W)
{
    for (QWidget *P = W->parentWidget(); P; P = P->parentWidget())
    {
        if (auto *G = qobject_cast<QGroupBox *>(P)) return Pretty(G->title());
        if (P->isWindow()) break;
    }
    return {};
}

}   // namespace

void ApplyToWidget(QWidget *W)
{
    if (!W) return;

    QString Name;      // human-facing (accessibleName)
    QString Kind;      // objectName suffix, so a slug collision across types stays distinguishable

    if (auto *B = qobject_cast<QAbstractButton *>(W))
    {
        Name = Pretty(B->text());
        Kind = B->isCheckable() ? "Check" : "Btn";
        // Icon-only buttons have no text at all; fall back to their tooltip, which is what a user hovers to read.
        if (Name.isEmpty()) Name = Pretty(B->toolTip());
    }
    else if (qobject_cast<QLineEdit *>(W) || qobject_cast<QComboBox *>(W) || qobject_cast<QAbstractSpinBox *>(W)
             || qobject_cast<QTextEdit *>(W) || qobject_cast<QPlainTextEdit *>(W))
    {
        Name = FormLabelFor(W);
        if (Name.isEmpty()) Name = PrecedingLabel(W);
        if (Name.isEmpty())
            if (auto *LE = qobject_cast<QLineEdit *>(W)) Name = Pretty(LE->placeholderText());
        if (Name.isEmpty()) Name = Pretty(W->toolTip());
        Kind = qobject_cast<QComboBox *>(W) ? "Combo" : "Field";
    }
    else if (auto *G = qobject_cast<QGroupBox *>(W))
    {
        Name = Pretty(G->title());
        Kind = "Group";
    }
    else if (qobject_cast<QAbstractItemView *>(W))
    {
        Name = PrecedingLabel(W);
        if (Name.isEmpty()) Name = Pretty(W->toolTip());
        Kind = "View";
    }
    else
    {
        return;   // layout/plain containers: nothing meaningful to say about them
    }

    if (Name.isEmpty()) return;   // nothing derivable — better anonymous than misleading

    // Qualify with the enclosing group so repeated controls stay distinct ("Demo" card's Remove vs another's).
    const QString Group = GroupTitle(W);
    const QString Qualified = (!Group.isEmpty() && Group != Name) ? Group + " " + Name : Name;

    if (W->accessibleName().isEmpty()) W->setAccessibleName(Qualified);
    if (W->objectName().isEmpty())     W->setObjectName(Slug(Qualified) + Kind);
}

void ApplyToTree(QWidget *Root)
{
    if (!Root) return;
    ApplyToWidget(Root);
    for (QWidget *C : Root->findChildren<QWidget *>()) ApplyToWidget(C);
}

namespace {
// Names each widget as it is polished. Polish fires once per widget, before it is first shown, and fires for
// widgets created later at runtime too — which is what makes this cover dynamically rebuilt pages without every
// page having to remember to call ApplyToTree.
class AutoNamer : public QObject
{
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject *Obj, QEvent *Ev) override
    {
        if (Ev->type() == QEvent::Polish)
            if (auto *W = qobject_cast<QWidget *>(Obj))
                ApplyToWidget(W);
        return QObject::eventFilter(Obj, Ev);
    }
};
}   // namespace

void Install(QObject *App)
{
    if (App) App->installEventFilter(new AutoNamer(App));
}

}   // namespace A11yNames
