#include "padnav.h"

#include "osk.h"        // OnScreenKeyboard — A on a text field
#include "deckstyle.h"  // the focus ring's colours

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDialog>
#include <QGroupBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>

#include <algorithm>
#include <cmath>
#include <limits>

using json = nlohmann::ordered_json;

namespace
{
bool Horizontal(Pad::Button B) { return B == Pad::Button::Left || B == Pad::Button::Right; }

int KeyOf(Pad::Button B)
{
    switch (B)
    {
    case Pad::Button::Up:    return Qt::Key_Up;
    case Pad::Button::Down:  return Qt::Key_Down;
    case Pad::Button::Left:  return Qt::Key_Left;
    default:                 return Qt::Key_Right;
    }
}

void SendKey(QWidget *W, int Key)
{
    QKeyEvent Press(QEvent::KeyPress, Key, Qt::NoModifier), Release(QEvent::KeyRelease, Key, Qt::NoModifier);
    QApplication::sendEvent(W, &Press);
    QApplication::sendEvent(W, &Release);
}

//The part of a composite control navigation never stops on by itself: a spin box's or a combo box's own line edit,
//an editor an item view has open, a tab widget and its tab bar (the shoulders turn its pages).
bool InnerPart(QWidget *W, QWidget *Window)
{
    if (W->property("padSkip").toBool()) return true;
    if (qobject_cast<QTabBar *>(W) && qobject_cast<QTabWidget *>(W->parentWidget())) return true;
    //A tab widget itself is no stop either: focused, it hands the focus to its tab bar (which is not one), and the
    //next press starts over from the window's default button. Navigation goes through it to its page's controls.
    if (qobject_cast<QTabWidget *>(W)) return true;
    for (QWidget *P = W->parentWidget(); P && P != Window; P = P->parentWidget())
    {
        if (P->property("padSkip").toBool()) return true;
        if (qobject_cast<QAbstractSpinBox *>(P) || qobject_cast<QComboBox *>(P)) return true;
        if (auto *V = qobject_cast<QAbstractItemView *>(P))
            if (W->parentWidget() == V->viewport() && V->indexWidget(V->currentIndex()) != W
                && W->property("padCell").isNull() && !qobject_cast<QAbstractButton *>(W))
                return true;   // an open editor (a cell's own button — a check box in a table — is a control)
    }
    return false;
}

QWidget *FocusIn(QWidget *Window)
{
    if (!Window) return nullptr;
    QWidget *F = QApplication::focusWidget();
    if (F && F->window() == Window) return F;
    return Window->focusWidget();
}

void EnsureVisible(QWidget *W)
{
    for (QWidget *P = W->parentWidget(); P; P = P->parentWidget())
        if (auto *S = qobject_cast<QScrollArea *>(P)) S->ensureWidgetVisible(W, 32, 32);
}

QAbstractScrollArea *ScrollerOf(QWidget *W)
{
    for (QWidget *P = W; P && !P->isWindow(); P = P->parentWidget())
        if (auto *S = qobject_cast<QAbstractScrollArea *>(P))
        {
            const QScrollBar *V = S->verticalScrollBar(), *H = S->horizontalScrollBar();
            if ((V && V->maximum() > V->minimum()) || (H && H->maximum() > H->minimum())) return S;
        }
    return nullptr;
}

QTabWidget *TabsOf(QWidget *W)
{
    for (QWidget *P = W; P && !P->isWindow(); P = P->parentWidget())
        if (auto *T = qobject_cast<QTabWidget *>(P); T && T->isVisible() && T->count() > 1) return T;
    return nullptr;
}

//The pages of the nearest tab widget, one step on (no wrap); focus lands on the new page's first control.
bool TurnTab(QWidget *F, int Step)
{
    QTabWidget *T = TabsOf(F);
    if (!T) return false;
    for (int I = T->currentIndex() + Step; I >= 0 && I < T->count(); I += Step)
    {
        if (!T->isTabEnabled(I)) continue;
        T->setCurrentIndex(I);
        //The new page's first control in reading order.
        QWidget *First = nullptr;
        for (QWidget *C : PadNav::Focusable(T->window()))
        {
            if (!T->currentWidget() || !T->currentWidget()->isAncestorOf(C)) continue;
            const QRect R = PadNav::FocusRect(C), Fr = First ? PadNav::FocusRect(First) : QRect();
            if (!First || R.top() < Fr.top() || (R.top() == Fr.top() && R.left() < Fr.left())) First = C;
        }
        if (First) { First->setFocus(Qt::TabFocusReason); EnsureVisible(First); }
        return true;
    }
    return false;
}

//A press that stays inside the focused control: a list's next row, a slider's next step, a tree's branch opening.
//False when the control is at its edge that way — then focus leaves it.
bool MoveInside(QWidget *F, Pad::Button Dir)
{
    if (auto *V = qobject_cast<QAbstractItemView *>(F))
    {
        const QModelIndex Before = V->currentIndex();
        if (!Before.isValid())
        {
            if (Horizontal(Dir) || !V->model() || V->model()->rowCount(V->rootIndex()) == 0) return false;
            V->setCurrentIndex(V->model()->index(0, 0, V->rootIndex()));
            return true;
        }
        if (auto *T = qobject_cast<QTreeView *>(V); T && Horizontal(Dir))
        {
            const bool Was = T->isExpanded(Before);
            SendKey(V, KeyOf(Dir));
            return Was != T->isExpanded(Before) || V->currentIndex() != Before;
        }
        SendKey(V, KeyOf(Dir));
        return V->currentIndex() != Before;
    }
    if (const QVariant Before = F->property("padIndex"); Before.isValid())   // a widget that keeps its own selection
    {
        SendKey(F, KeyOf(Dir));
        return F->property("padIndex") != Before;
    }
    if (!Horizontal(Dir)) return false;
    const bool Up = Dir == Pad::Button::Right;
    //A choice that asks for it ("padAdjust": a game's option, the launch window's version) turns with left and right
    //as a console's settings do, to its ends; A still opens its list.
    if (auto *C = qobject_cast<QComboBox *>(F); C && C->property("padAdjust").toBool())
    {
        const int To = C->currentIndex() + (Up ? 1 : -1);
        if (To < 0 || To >= C->count()) return false;
        C->setCurrentIndex(To);
        return true;
    }
    if (auto *S = qobject_cast<QAbstractSlider *>(F))
    {
        const int Was = S->value();
        S->triggerAction(Up ? QAbstractSlider::SliderSingleStepAdd : QAbstractSlider::SliderSingleStepSub);
        return S->value() != Was;
    }
    if (auto *S = qobject_cast<QAbstractSpinBox *>(F))
    {
        if (S->isReadOnly()) return false;
        const QString Was = S->text();
        S->stepBy(Up ? 1 : -1);
        return S->text() != Was;
    }
    if (auto *T = qobject_cast<QTabBar *>(F))
    {
        const int To = T->currentIndex() + (Up ? 1 : -1);
        if (To < 0 || To >= T->count()) return false;
        T->setCurrentIndex(To);
        return true;
    }
    return false;
}

bool ContextMenu(QWidget *F)
{
    if (!F) return false;
    QWidget *Receiver = F;
    QPoint At = F->rect().center();
    if (auto *V = qobject_cast<QAbstractItemView *>(F))
    {
        Receiver = V->viewport();
        if (V->currentIndex().isValid()) At = V->visualRect(V->currentIndex()).center();
        else At = V->viewport()->rect().center();
    }
    switch (F->contextMenuPolicy())
    {
    case Qt::NoContextMenu:
    case Qt::PreventContextMenu:
        return false;
    case Qt::CustomContextMenu:
        //A scroll area's menu position is in its viewport's coordinates (QWidget::customContextMenuRequested).
        QMetaObject::invokeMethod(F, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, At));
        return true;
    default:
    {
        QContextMenuEvent E(QContextMenuEvent::Keyboard, At, Receiver->mapToGlobal(At));
        QApplication::sendEvent(Receiver, &E);
        return E.isAccepted();
    }
    }
}
}

// ═════════════════════════════════════════════════════════════════════════════
// PadNav
// ═════════════════════════════════════════════════════════════════════════════

namespace PadNav
{

int Nearest(const QRect &From, Pad::Button Dir, const QVector<QRect> &Candidates)
{
    //Along the way: how far past From's edge the candidate starts (0 when they overlap). Across: the gap between the
    //two on the other axis (0 when they overlap there), weighed heavier, so the control straight ahead wins over a
    //nearer one off to the side; then how far the centres are apart across, to settle ties.
    int Best = -1;
    double BestScore = std::numeric_limits<double>::max();
    const QPointF C = QRectF(From).center();
    for (int I = 0; I < Candidates.size(); ++I)
    {
        const QRect &R = Candidates[I];
        if (R.isEmpty()) continue;
        const QPointF P = QRectF(R).center();
        double Along = 0, Gap = 0, Off = 0;
        switch (Dir)
        {
        case Pad::Button::Down:
            if (P.y() <= C.y() + 0.5 || R.top() <= From.top()) continue;
            Along = std::max(0, R.top() - From.bottom());
            Gap = std::max({ 0, R.left() - From.right(), From.left() - R.right() });
            Off = std::fabs(P.x() - C.x());
            break;
        case Pad::Button::Up:
            if (P.y() >= C.y() - 0.5 || R.bottom() >= From.bottom()) continue;
            Along = std::max(0, From.top() - R.bottom());
            Gap = std::max({ 0, R.left() - From.right(), From.left() - R.right() });
            Off = std::fabs(P.x() - C.x());
            break;
        case Pad::Button::Right:
            if (P.x() <= C.x() + 0.5 || R.left() <= From.left()) continue;
            Along = std::max(0, R.left() - From.right());
            Gap = std::max({ 0, R.top() - From.bottom(), From.top() - R.bottom() });
            Off = std::fabs(P.y() - C.y());
            break;
        case Pad::Button::Left:
            if (P.x() >= C.x() - 0.5 || R.right() >= From.right()) continue;
            Along = std::max(0, From.left() - R.right());
            Gap = std::max({ 0, R.top() - From.bottom(), From.top() - R.bottom() });
            Off = std::fabs(P.y() - C.y());
            break;
        default:
            return -1;
        }
        //Two passes in one: a control sharing the row (or column) — no gap across — wins over every other, the nearest
        //along the way first; only when none does, the weighed score. (Weighed alone, a control a row away that started
        //"sooner" beat the one right beside: Right from the instance list went to Edit package above it.)
        const bool InLine = Gap == 0;
        //In line, being straight ahead counts too: the list centred below beats a small button off to the side that
        //starts a little sooner (the overlay's Resume → its layout list, not its Edit button).
        const double Score = InLine ? Along + 0.5 * Off : 1e6 + Along + 3.0 * Gap + 0.05 * Off;
        if (Score < BestScore) { BestScore = Score; Best = I; }
    }
    return Best;
}

QList<QWidget *> Focusable(QWidget *Window)
{
    QList<QWidget *> Out;
    if (!Window) return Out;
    for (QWidget *W : Window->findChildren<QWidget *>())
    {
        if (W->window() != Window || !W->isVisible() || !W->isEnabled()) continue;
        if (!(W->focusPolicy() & Qt::TabFocus)) continue;
        if (qobject_cast<QScrollBar *>(W) || W->size().isEmpty()) continue;
        //A scroll area that only holds a page (a hosted page's own QScrollArea) is a container, never a stop: taken as
        //one, it was the first stop on the page, and a move never goes into a stop's own children — every press on
        //the Peers page stayed on it. (A list, a table, a text view — scroll areas with content of their own — stay.)
        if (qobject_cast<QScrollArea *>(W)) continue;
        if (InnerPart(W, Window)) continue;
        Out.push_back(W);
    }
    return Out;
}

QRect FocusRect(QWidget *W)
{
    if (!W) return {};
    QWidget *Window = W->window();
    if (auto *V = qobject_cast<QAbstractItemView *>(W); V && V->currentIndex().isValid())
    {
        const QRect R = V->visualRect(V->currentIndex()).intersected(V->viewport()->rect());
        if (!R.isEmpty())
        {
            //A whole row of a tree, or of a table that selects rows, not one cell: the row is what is current.
            QRect Row = R;
            if (qobject_cast<QTreeView *>(V) || V->selectionBehavior() == QAbstractItemView::SelectRows)
                Row = QRect(0, R.top(), V->viewport()->width(), R.height());
            return QRect(V->viewport()->mapTo(Window, Row.topLeft()), Row.size());
        }
    }
    return QRect(W->mapTo(Window, QPoint(0, 0)), W->size());
}

bool Move(QWidget *Window, Pad::Button Dir)
{
    if (!Window || !Pad::IsDirection(Dir)) return false;
    const QList<QWidget *> All = Focusable(Window);
    if (All.isEmpty()) return false;
    QWidget *F = FocusIn(Window);
    if (!F || !All.contains(F))
    {
        //Nothing focused yet: the dialog's default button, else the first control in reading order.
        QWidget *First = nullptr;
        for (QWidget *W : All)
            if (auto *B = qobject_cast<QPushButton *>(W); B && B->isDefault()) First = W;
        if (!First)
            First = *std::min_element(All.begin(), All.end(), [&](QWidget *A, QWidget *B) {
                const QRect RA = FocusRect(A), RB = FocusRect(B);
                return RA.top() != RB.top() ? RA.top() < RB.top() : RA.left() < RB.left();
            });
        First->setFocus(Qt::TabFocusReason);
        EnsureVisible(First);
        return true;
    }
    QVector<QRect> Rects;
    QList<QWidget *> Others;
    for (QWidget *W : All)
    {
        if (W == F || F->isAncestorOf(W)) continue;
        Others.push_back(W);
        Rects.push_back(FocusRect(W));
    }
    const int I = Nearest(FocusRect(F), Dir, Rects);
    if (I < 0) return false;
    Others[I]->setFocus(Qt::TabFocusReason);
    EnsureVisible(Others[I]);
    return true;
}

bool Activate(QWidget *W)
{
    if (!W || !W->isEnabled()) return false;
    if (auto *B = qobject_cast<QPushButton *>(W); B && B->menu()) { B->showMenu(); return true; }
    if (auto *B = qobject_cast<QToolButton *>(W); B && (B->menu() || !B->actions().isEmpty()) && B->popupMode() != QToolButton::DelayedPopup)
    { B->showMenu(); return true; }
    if (auto *B = qobject_cast<QAbstractButton *>(W)) { B->click(); return true; }
    if (auto *C = qobject_cast<QComboBox *>(W)) { if (C->count() == 0) return false; C->showPopup(); return true; }
    if (auto *E = qobject_cast<QLineEdit *>(W))
    {
        if (E->isReadOnly()) return false;
        OnScreenKeyboard::Open(E);
        return true;
    }
    if (auto *S = qobject_cast<QAbstractSpinBox *>(W))
    {
        if (S->isReadOnly()) return false;
        OnScreenKeyboard::Open(S);
        return true;
    }
    if (auto *E = qobject_cast<QPlainTextEdit *>(W)) { if (E->isReadOnly()) return false; OnScreenKeyboard::Open(E); return true; }
    if (auto *E = qobject_cast<QTextEdit *>(W)) { if (E->isReadOnly()) return false; OnScreenKeyboard::Open(E); return true; }
    if (auto *G = qobject_cast<QGroupBox *>(W); G && G->isCheckable()) { G->setChecked(!G->isChecked()); return true; }
    if (auto *V = qobject_cast<QAbstractItemView *>(W))
    {
        QModelIndex I = V->currentIndex();
        if (!I.isValid())
        {
            if (!V->model() || V->model()->rowCount(V->rootIndex()) == 0) return false;
            V->setCurrentIndex(V->model()->index(0, 0, V->rootIndex()));
            return true;
        }
        //A cell's own control (a check box or a button set with setIndexWidget, often centred in a holder): press it.
        if (QWidget *Cell = V->indexWidget(I))
        {
            if (Activate(Cell)) return true;
            if (auto *Btn = Cell->findChild<QAbstractButton *>(); Btn && Btn->isEnabled()) { Btn->click(); return true; }
        }
        //A row's check box may sit in any of its columns (a list's tick, a table's "Share" column): the current
        //cell's, else the first of the row that shows one. A cell shows one when it has a check state — the
        //ItemIsUserCheckable flag alone is on every QTableWidgetItem, ticked or not.
        const auto Shows = [](const QModelIndex &X) {
            return (X.flags() & Qt::ItemIsUserCheckable) && (X.flags() & Qt::ItemIsEnabled) && X.data(Qt::CheckStateRole).isValid();
        };
        QModelIndex Check;
        if (Shows(I)) Check = I;
        else if (V->model())
            for (int C = 0; C < V->model()->columnCount(I.parent()) && !Check.isValid(); ++C)
                if (const QModelIndex Cell = V->model()->index(I.row(), C, I.parent()); Shows(Cell)) Check = Cell;
        if (Check.isValid())
        {
            const auto Now = static_cast<Qt::CheckState>(Check.data(Qt::CheckStateRole).toInt());
            V->model()->setData(Check, Now == Qt::Checked ? Qt::Unchecked : Qt::Checked, Qt::CheckStateRole);
            return true;
        }
        emit V->activated(I);
        if (auto *T = qobject_cast<QTreeView *>(V); T && V->model()->hasChildren(I)) T->setExpanded(I, !T->isExpanded(I));
        return true;
    }
    if (W->property("padIndex").isValid()) { SendKey(W, Qt::Key_Return); return true; }
    return false;
}

bool Back(QWidget *Window)
{
    if (QWidget *Pop = QApplication::activePopupWidget()) { SendKey(Pop->focusWidget() ? Pop->focusWidget() : Pop, Qt::Key_Escape); return true; }
    if (!Window) return false;
    QWidget *F = FocusIn(Window);
    //An item view's open editor (the focus is a widget in the view's viewport): Escape cancels the edit.
    for (QWidget *P = F; P && P != Window; P = P->parentWidget())
        if (auto *V = qobject_cast<QAbstractItemView *>(P); V && F != V && F->parentWidget() == V->viewport()
                                                            && !qobject_cast<QAbstractButton *>(F))
        { SendKey(F, Qt::Key_Escape); return true; }
    if (auto *D = qobject_cast<QDialog *>(Window)) { SendKey(D, Qt::Key_Escape); return true; }
    if (!qobject_cast<QMainWindow *>(Window) && Window->isWindow() && Window->parentWidget()) { Window->close(); return true; }
    return false;
}

QList<PadTarget::Hint> Hints(QWidget *W)
{
    using B = Pad::Button;
    QList<PadTarget::Hint> Out;
    if (!W) return Out;
    QString A;
    if (qobject_cast<QCheckBox *>(W) || qobject_cast<QRadioButton *>(W)) A = "Toggle";
    else if (qobject_cast<QAbstractButton *>(W)) A = "Select";
    else if (qobject_cast<QComboBox *>(W)) A = "Choose";
    else if ((qobject_cast<QLineEdit *>(W) && !qobject_cast<QLineEdit *>(W)->isReadOnly()) || qobject_cast<QAbstractSpinBox *>(W)) A = "Type";
    else if (auto *G = qobject_cast<QGroupBox *>(W); G && G->isCheckable()) A = "Toggle";
    else if (auto *V = qobject_cast<QAbstractItemView *>(W))
    {
        const QModelIndex I = V->currentIndex();
        QWidget *Cell = I.isValid() ? V->indexWidget(I) : nullptr;
        const bool Tick = (Cell && Cell->findChild<QCheckBox *>()) || (I.isValid() && I.data(Qt::CheckStateRole).isValid());
        A = Tick ? "Toggle" : Cell && Cell->findChild<QAbstractButton *>() ? "Select" : "Open";
    }
    else if (W->property("padIndex").isValid()) A = "Open";
    if (!A.isEmpty() && W->isEnabled()) Out.push_back({ B::A, A });
    if (qobject_cast<QAbstractSlider *>(W) || qobject_cast<QAbstractSpinBox *>(W)
        || (qobject_cast<QComboBox *>(W) && W->property("padAdjust").toBool()))
        Out.push_back({ B::Left, "Adjust" });
    if (W->contextMenuPolicy() == Qt::CustomContextMenu) Out.push_back({ B::X, "Menu" });
    if (TabsOf(W)) Out.push_back({ B::LB, "Tabs" });
    QWidget *Win = W->window();
    if (qobject_cast<QDialog *>(Win) || (!qobject_cast<QMainWindow *>(Win) && Win->parentWidget())) Out.push_back({ B::B, "Back" });
    return Out;
}

json Describe(QWidget *W)
{
    if (!W) return nullptr;
    json J{{"kind", W->metaObject()->className()}};
    if (!W->objectName().isEmpty()) J["name"] = W->objectName().toStdString();
    if (!W->accessibleName().isEmpty()) J["label"] = W->accessibleName().toStdString();
    if (auto *B = qobject_cast<QAbstractButton *>(W))
    {
        J["text"] = B->text().toStdString();
        if (B->isCheckable()) J["checked"] = B->isChecked();
    }
    else if (auto *C = qobject_cast<QComboBox *>(W)) J["text"] = C->currentText().toStdString();
    else if (auto *E = qobject_cast<QLineEdit *>(W)) J["text"] = E->text().toStdString();
    else if (auto *S = qobject_cast<QAbstractSpinBox *>(W)) J["text"] = S->text().toStdString();
    else if (auto *V = qobject_cast<QAbstractItemView *>(W); V && V->currentIndex().isValid())
    {
        const QModelIndex I = V->currentIndex();
        json It{{"row", I.row()}, {"column", I.column()}, {"text", I.data().toString().toStdString()}};
        if (I.data(Qt::CheckStateRole).isValid()) It["checked"] = I.data(Qt::CheckStateRole).toInt() == Qt::Checked;
        if (QWidget *Cell = V->indexWidget(I))
            if (auto *Btn = Cell->findChild<QAbstractButton *>(); Btn && Btn->isCheckable()) It["checked"] = Btn->isChecked();
        J["item"] = It;
    }
    if (const QVariant I = W->property("padIndex"); I.isValid()) J["index"] = I.toInt();
    if (!W->isEnabled()) J["enabled"] = false;
    return J;
}

}

// ═════════════════════════════════════════════════════════════════════════════
// FocusRing — the controller's focus, drawn over whatever the window is (the generic widgets' own focus frames are a
// one-pixel dotted line a couch away from the screen)
// ═════════════════════════════════════════════════════════════════════════════

class FocusRing : public QWidget
{
public:
    explicit FocusRing(QWidget *Window) : QWidget(Window)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
        setProperty("padSkip", true);
        setObjectName("padFocusRing");
    }
    void place(const QRect &Target)
    {
        if (parentWidget()) setGeometry(parentWidget()->rect());
        raise();
        //Glide from where it was (a third of the way per frame), unless it was nowhere or the jump is to another
        //window's corner: then it is simply there.
        if (Shown.isEmpty() || Target.isEmpty()) Shown = Target;
        else
        {
            const auto Lerp = [](int A, int B) { return A + static_cast<int>(std::lround((B - A) * 0.45)); };
            Shown = QRect(QPoint(Lerp(Shown.left(), Target.left()), Lerp(Shown.top(), Target.top())),
                          QPoint(Lerp(Shown.right(), Target.right()), Lerp(Shown.bottom(), Target.bottom())));
            if ((Shown.topLeft() - Target.topLeft()).manhattanLength() < 2 && (Shown.bottomRight() - Target.bottomRight()).manhattanLength() < 2)
                Shown = Target;
        }
        if (Shown != Painted) { update(Painted.adjusted(-8, -8, 8, 8)); update(Shown.adjusted(-8, -8, 8, 8)); }
    }
    QRect shown() const { return Shown; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Painted = Shown;
        if (Shown.isEmpty()) return;
        QPainter P(this);
        P.setRenderHint(QPainter::Antialiasing);
        //Controller mode's focus, over a control it does not draw: the magenta shadow's edge offset under a lime ring.
        const QRectF Ring = QRectF(Shown).adjusted(-3, -3, 3, 3);
        P.setBrush(Qt::NoBrush);
        P.setPen(QPen(DeckStyle::Magenta, 3));
        P.drawRoundedRect(Ring.translated(3, 3), 8, 8);
        P.setPen(QPen(DeckStyle::Lime, 3));
        P.drawRoundedRect(Ring, 8, 8);
    }

private:
    QRect Shown, Painted;
};

// ═════════════════════════════════════════════════════════════════════════════
// PadRouter
// ═════════════════════════════════════════════════════════════════════════════

PadRouter &PadRouter::instance()
{
    static PadRouter *R = new PadRouter();
    return *R;
}

void PadRouter::install()
{
    if (Installed) return;
    Installed = true;
    PadInput &In = PadInput::instance();
    connect(&In, &PadInput::pressed, this, [this](Pad::Button B) { route(B); });
    connect(&In, &PadInput::scrolled, this, &PadRouter::scroll);
    connect(&In, &PadInput::used, this, [this] { setPadMode(true); });
    qApp->installEventFilter(this);
    connect(qApp, &QApplication::focusChanged, this, [this] { emit hintsChanged(); kickRing(); });
    //The ring's frames run only while something can move it — never all day for an app idling in the tray.
    Tick = new QTimer(this);
    Tick->setInterval(16);
    connect(Tick, &QTimer::timeout, this, &PadRouter::updateRing);
    connect(this, &PadRouter::routed, this, [this] { kickRing(); });
}

void PadRouter::kickRing()
{
    TickUntil = QDateTime::currentMSecsSinceEpoch() + 900;   // a glide, a scroll settling, a page laid out
    if (Tick && !Tick->isActive()) Tick->start();
}

QWidget *PadRouter::targetWindow()
{
    if (QWidget *Pop = QApplication::activePopupWidget()) return Pop;
    if (QWidget *M = QApplication::activeModalWidget()) return M;
    if (QWidget *A = QApplication::activeWindow()) return A;
    //Offscreen (tests, a headless bring-up) nothing is active: the newest visible dialog, else the main window.
    QWidget *Main = nullptr;
    for (QWidget *W : QApplication::topLevelWidgets())
    {
        if (!W->isVisible() || W->property("padSkip").toBool()) continue;
        if (qobject_cast<QDialog *>(W)) return W;
        if (!Main || qobject_cast<QMainWindow *>(W)) Main = W;
    }
    return Main;
}

void PadRouter::pushOverlay(QWidget *Overlay)
{
    popOverlay(Overlay);
    Overlays.push_back(Overlay);
    emit hintsChanged();
}

void PadRouter::popOverlay(QWidget *Overlay)
{
    Overlays.removeAll(Overlay);
    Overlays.removeAll(nullptr);
    emit hintsChanged();
}

QWidget *PadRouter::overlay() const
{
    for (int I = Overlays.size() - 1; I >= 0; --I)
        if (Overlays[I] && Overlays[I]->isVisible()) return Overlays[I];
    return nullptr;
}

QString PadRouter::route(Pad::Button B)
{
    using P = Pad::Button;
    QString What;
    const auto Done = [&](const QString &W) { What = W; emit routed(W); emit hintsChanged(); return W; };
    //The top overlay takes the press. One that is a window of controls (padNavInside: the in-game overlay) lets what it
    //does not take move inside it — never into the window behind it, whichever window is active.
    QWidget *Forced = nullptr;
    if (QWidget *O = overlay())
        if (auto *T = dynamic_cast<PadTarget *>(O))
        {
            if (T->padPressed(B) || !O->property("padNavInside").toBool()) return Done("overlay");
            Forced = O->window();
        }
    if (QWidget *Pop = QApplication::activePopupWidget())
    {
        QWidget *To = Pop->focusWidget() ? Pop->focusWidget() : Pop;
        if (Pad::IsDirection(B)) SendKey(To, KeyOf(B));
        else if (B == P::A) SendKey(To, Qt::Key_Return);
        else if (B == P::B) SendKey(To, Qt::Key_Escape);
        else return Done("");
        return Done("popup");
    }
    QWidget *Win = Forced ? Forced : targetWindow();
    if (!Win) return Done("");
    QWidget *F = FocusIn(Win);
    //The focus chain's PadTargets, innermost first — never past the window: a dialog's parent is the window it was
    //opened over, and that window's screen must not take a press meant for the dialog.
    for (QWidget *W = F ? F : Win; W; W = W->isWindow() ? nullptr : W->parentWidget())
        if (auto *T = dynamic_cast<PadTarget *>(W); T && T->padPressed(B))
            return Done(QString("target ") + W->metaObject()->className());
    if (!F) F = Win;
    bool Took = false;
    switch (B)
    {
    case P::Up: case P::Down: case P::Left: case P::Right:
        Took = MoveInside(F, B) || PadNav::Move(Win, B);
        if (Took) PadInput::instance().tick();
        else if (!Horizontal(B))
            if (QAbstractScrollArea *S = ScrollerOf(F))   // nothing more that way: show what is below
            {
                QScrollBar *V = S->verticalScrollBar();
                const int Was = V->value();
                V->triggerAction(B == P::Down ? QAbstractSlider::SliderSingleStepAdd : QAbstractSlider::SliderSingleStepSub);
                Took = V->value() != Was;
            }
        break;
    case P::A: Took = PadNav::Activate(F); break;
    case P::B: Took = PadNav::Back(Win); break;
    case P::X: Took = ContextMenu(F); break;
    case P::LB: case P::RB: Took = TurnTab(F, B == P::RB ? 1 : -1); break;
    case P::LT: case P::RT:
        Took = TurnTab(F, B == P::RT ? 1 : -1);
        if (!Took)
            if (QAbstractScrollArea *S = ScrollerOf(F))
            {
                QScrollBar *V = S->verticalScrollBar();
                const int Was = V->value();
                V->triggerAction(B == P::RT ? QAbstractSlider::SliderPageStepAdd : QAbstractSlider::SliderPageStepSub);
                Took = V->value() != Was;
            }
        break;
    default: break;
    }
    return Done(Took ? QStringLiteral("navigation") : QString());
}

void PadRouter::scroll(double Dx, double Dy)
{
    if (QWidget *O = overlay()) { if (auto *T = dynamic_cast<PadTarget *>(O)) T->padScrolled(Dx, Dy); return; }
    QWidget *Win = targetWindow();
    QWidget *F = FocusIn(Win);
    for (QWidget *W = F ? F : Win; W; W = W->isWindow() ? nullptr : W->parentWidget())
        if (auto *T = dynamic_cast<PadTarget *>(W); T && T->padScrolled(Dx, Dy)) return;
    QAbstractScrollArea *S = F ? ScrollerOf(F) : nullptr;
    if (!S) return;
    ScrollX += Dx; ScrollY += Dy;
    const int Ix = static_cast<int>(ScrollX), Iy = static_cast<int>(ScrollY);
    ScrollX -= Ix; ScrollY -= Iy;
    if (Iy) S->verticalScrollBar()->setValue(S->verticalScrollBar()->value() + Iy);
    if (Ix) S->horizontalScrollBar()->setValue(S->horizontalScrollBar()->value() + Ix);
    if (Ix || Iy) kickRing();   // what is focused moved with the page
}

QList<PadTarget::Hint> PadRouter::hints() const
{
    if (QWidget *O = overlay())
        if (auto *T = dynamic_cast<PadTarget *>(O)) return T->padHints();
    QWidget *Win = targetWindow();
    QWidget *F = FocusIn(Win);
    QList<PadTarget::Hint> Out;
    const auto Add = [&Out](const PadTarget::Hint &H) {
        for (const PadTarget::Hint &Have : Out) if (Have.B == H.B) return;   // the innermost says what a button does
        Out.push_back(H);
    };
    for (QWidget *W = F ? F : Win; W; W = W->isWindow() ? nullptr : W->parentWidget())
        if (auto *T = dynamic_cast<PadTarget *>(W)) for (const PadTarget::Hint &H : T->padHints()) Add(H);
    if (F) for (const PadTarget::Hint &H : PadNav::Hints(F)) Add(H);
    //One order everywhere, whoever said which: the face buttons, the d-pad, the shoulders, then B and the menu.
    static const Pad::Button Order[] = { Pad::Button::A, Pad::Button::X, Pad::Button::Y, Pad::Button::Left, Pad::Button::LB,
                                         Pad::Button::RB, Pad::Button::LT, Pad::Button::RT, Pad::Button::B, Pad::Button::Back,
                                         Pad::Button::Start };
    const auto Rank = [](Pad::Button B) {
        for (int I = 0; I < static_cast<int>(std::size(Order)); ++I) if (Order[I] == B) return I;
        return 99;
    };
    std::stable_sort(Out.begin(), Out.end(), [&](const PadTarget::Hint &X, const PadTarget::Hint &Y) { return Rank(X.B) < Rank(Y.B); });
    return Out;
}

void PadRouter::setPadMode(bool On)
{
    if (PadMode == On) return;
    PadMode = On;
    kickRing();
    updateRing();
}

bool PadRouter::eventFilter(QObject *Watched, QEvent *Event)
{
    //A real pointer — a click, a touch, a wheel — puts the controller's focus ring away until the controller is next
    //used. (A mouse MOVE alone does not: the Deck's trackpads and a bumped mouse would make it flicker.)
    switch (Event->type())
    {
    case QEvent::MouseButtonPress:
    case QEvent::TouchBegin:
    case QEvent::Wheel:
        if (Watched->isWidgetType()) setPadMode(false);
        break;
    default: break;
    }
    return false;
}

void PadRouter::updateRing()
{
    if (Tick && Tick->isActive() && QDateTime::currentMSecsSinceEpoch() > TickUntil) Tick->stop();
    QWidget *Win = PadMode ? targetWindow() : nullptr;
    if (Win && QApplication::activePopupWidget() == Win) Win = nullptr;   // a menu draws its own current item
    QWidget *F = Win ? FocusIn(Win) : nullptr;
    //A screen that draws its own focus (the controller mode's grid) wants no ring over it: the nearest widget that
    //says either way decides (a desktop page hosted in the controller shell says it wants the ring).
    for (QWidget *W = F; W; W = W->isWindow() ? nullptr : W->parentWidget())
        if (const QVariant Own = W->property("padOwnFocus"); Own.isValid()) { if (Own.toBool()) F = nullptr; break; }
    if (overlay()) F = nullptr;
    if (!F || F == Win)
    {
        if (Ring) { Ring->place({}); Ring->hide(); }
        return;
    }
    if (!Ring || Ring->parentWidget() != Win)
    {
        delete Ring;
        Ring = new FocusRing(Win);
    }
    Ring->show();
    Ring->place(PadNav::FocusRect(F));
}

QRect PadRouter::ringRect() const { return Ring && Ring->isVisible() ? Ring->shown() : QRect(); }

json PadRouter::state() const
{
    QWidget *Win = targetWindow();
    QWidget *F = FocusIn(Win);
    json J;
    J["window"] = Win ? json{{"kind", Win->metaObject()->className()}, {"title", Win->windowTitle().toStdString()}} : json(nullptr);
    J["focus"] = PadNav::Describe(F);
    json Chain = json::array();
    for (QWidget *W = F ? F : Win; W; W = W->isWindow() ? nullptr : W->parentWidget())
        if (auto *T = dynamic_cast<PadTarget *>(W))
            if (json S = T->padState(); !S.is_null()) Chain.push_back(S);
    if (!Chain.empty()) J["screen"] = Chain;
    if (QWidget *O = overlay())
        if (auto *T = dynamic_cast<PadTarget *>(O)) J["overlay"] = T->padState();
    json H = json::array();
    for (const PadTarget::Hint &Hn : hints()) H.push_back(json{{"button", Pad::Name(Hn.B)}, {"does", Hn.Label.toStdString()}});
    J["hints"] = H;
    J["padMode"] = PadMode;
    if (const QRect R = ringRect(); !R.isEmpty()) J["ring"] = json{R.x(), R.y(), R.width(), R.height()};
    return J;
}
