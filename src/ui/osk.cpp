#include "osk.h"

#include "deckstyle.h"   // the hint row

#include <QAbstractSpinBox>
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QTextEdit>

#include <algorithm>
#include <cmath>

using json = nlohmann::ordered_json;

namespace
{
QPointer<OnScreenKeyboard> Current;

QString FieldText(QWidget *F)
{
    if (auto *E = qobject_cast<QLineEdit *>(F)) return E->text();
    if (auto *S = qobject_cast<QAbstractSpinBox *>(F)) return S->text();
    if (auto *P = qobject_cast<QPlainTextEdit *>(F)) return P->toPlainText();
    if (auto *T = qobject_cast<QTextEdit *>(F)) return T->toPlainText();
    return {};
}

//What the field asks for, for the keyboard's title: its accessible name (A11yNames gives every control one from what
//it shows), else its placeholder.
QString FieldTitle(QWidget *F)
{
    if (!F->accessibleName().isEmpty()) return F->accessibleName();
    if (auto *E = qobject_cast<QLineEdit *>(F); E && !E->placeholderText().isEmpty()) return E->placeholderText();
    return QStringLiteral("Type");
}

void WriteBack(QWidget *F, const QString &S)
{
    if (auto *E = qobject_cast<QLineEdit *>(F))
    {
        //As typing does it: the edit signals fire (a search field filters on textEdited), then Return finishes it.
        E->setFocus(Qt::OtherFocusReason);
        E->selectAll();
        if (S.isEmpty()) E->del(); else E->insert(S);
        QKeyEvent Press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier), Release(QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(E, &Press);
        QApplication::sendEvent(E, &Release);
        return;
    }
    if (auto *Sp = qobject_cast<QAbstractSpinBox *>(F))
    {
        if (auto *E = Sp->findChild<QLineEdit *>()) { E->selectAll(); E->insert(S); }
        Sp->interpretText();
        return;
    }
    if (auto *P = qobject_cast<QPlainTextEdit *>(F)) { P->setPlainText(S); return; }
    if (auto *T = qobject_cast<QTextEdit *>(F)) T->setPlainText(S);
}
}

OnScreenKeyboard::OnScreenKeyboard(QWidget *Window, const QString &T, const QString &Start)
    : QWidget(Window), Title(T), Text(Start), Original(Start), Cursor(static_cast<int>(Start.size()))
{
    setObjectName("onScreenKeyboard");
    setAccessibleName("On-screen keyboard");
    setProperty("padSkip", true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_DeleteOnClose);
    setGeometry(Window->rect());
    Window->installEventFilter(this);   // follow the window's size
    build();
}

OnScreenKeyboard *OnScreenKeyboard::current() { return Current; }

OnScreenKeyboard *OnScreenKeyboard::Open(QWidget *F)
{
    if (!F) return nullptr;
    if (Current) Current->finish(false);
    auto *K = new OnScreenKeyboard(F->window(), FieldTitle(F), FieldText(F));
    K->Field = F;
    Current = K;
    K->show();
    K->raise();
    K->setFocus(Qt::OtherFocusReason);
    PadRouter::instance().pushOverlay(K);
    return K;
}

OnScreenKeyboard *OnScreenKeyboard::Ask(QWidget *Window, const QString &T, const QString &Start,
                                        std::function<void(const QString &)> Done, std::function<void(const QString &)> Edit)
{
    if (!Window) return nullptr;
    if (Current) Current->finish(false);
    QWidget *Had = QApplication::focusWidget() && QApplication::focusWidget()->window() == Window->window()
                     ? QApplication::focusWidget() : Window->window()->focusWidget();
    auto *K = new OnScreenKeyboard(Window->window(), T, Start);
    K->Before = Had;
    K->OnDone = std::move(Done);
    K->OnEdit = std::move(Edit);
    Current = K;
    K->show();
    K->raise();
    K->setFocus(Qt::OtherFocusReason);
    PadRouter::instance().pushOverlay(K);
    return K;
}

//The last key pressed, for the next keyboard: it opens where the hand left off (a search typed letter by letter
//starts on its letters, not on "1").
static QString &LastKeyPressed() { static QString K; return K; }

QRectF OnScreenKeyboard::panel() const
{
    //A live search (OnEdit) docks a lower keyboard: the results it filters stay in sight above it.
    const double W = std::min(width() - 24.0, 1120.0),
                 H = OnEdit ? std::clamp(height() * 0.42, 240.0, 380.0) : std::clamp(height() * 0.56, 260.0, 470.0);
    return QRectF((width() - W) / 2, height() - H - 12, W, H);
}

void OnScreenKeyboard::build()
{
    const QString Focused = Keys.isEmpty() || Focus >= Keys.size() ? QString() : Keys[Focus].Label;
    Keys.clear();
    QVector<QVector<Key>> Rows;
    const auto Chars = [this](const QString &S) {
        QVector<Key> R;
        for (const QChar C : S)
        {
            const QString Out = Shift && !Symbols ? QString(C).toUpper() : QString(C);
            R.push_back({ Out, Out, Kind::Char, 1, {} });
        }
        return R;
    };
    if (!Symbols)
    {
        Rows.push_back(Chars("1234567890"));
        Rows.back().push_back({ "⌫", {}, Kind::Backspace, 2, {} });
        Rows.push_back(Chars("qwertyuiop-_"));
        Rows.push_back(Chars("asdfghjkl'/"));
        Rows.push_back({ { "⇧", {}, Kind::Shift, 1.5, {} } });
        Rows.back() += Chars("zxcvbnm,.:");
        Rows.back().push_back({ "⇧", {}, Kind::Shift, 1.5, {} });
    }
    else
    {
        Rows.push_back(Chars("!@#$%^&*()"));
        Rows.back().push_back({ "⌫", {}, Kind::Backspace, 2, {} });
        Rows.push_back(Chars("~`|\\/[]{}<>?"));
        Rows.push_back(Chars("+=\";:,.'_-"));
        Rows.push_back(Chars("€£¥°§±×÷…"));
    }
    Rows.push_back({ { Symbols ? "ABC" : "&123", {}, Kind::Symbols, 2, {} }, { "◀", {}, Kind::Left, 1, {} },
                     { "Space", " ", Kind::Space, 5, {} }, { "▶", {}, Kind::Right, 1, {} }, { "Done", {}, Kind::Done, 3, {} } });
    //Laid out in the panel's key area: every row centred, one key unit wide enough that the widest row fits.
    const QRectF P = panel();
    const QRectF Area = P.adjusted(14, 70, -14, -50);   // the hint row under the keys
    double Widest = 0;
    for (const auto &R : Rows) { double U = 0; for (const Key &K : R) U += K.Units; Widest = std::max(Widest, U); }
    const double Gap = 6, Unit = (Area.width() + Gap) / Widest, RowH = (Area.height() + Gap) / Rows.size();
    for (int Y = 0; Y < Rows.size(); ++Y)
    {
        double Units = 0;
        for (const Key &K : Rows[Y]) Units += K.Units;
        double X = Area.left() + (Area.width() - (Units * Unit - Gap)) / 2;
        for (Key K : Rows[Y])
        {
            K.Rect = QRectF(X, Area.top() + Y * RowH, K.Units * Unit - Gap, RowH - Gap);
            X += K.Units * Unit;
            Keys.push_back(K);
        }
    }
    //Focus stays on the key it was on (Shift and the page switch rebuild the keys); a new keyboard starts on the key
    //pressed last, else on "q" (the letters, not the digits).
    const QString Want = !Focused.isEmpty() ? Focused : !LastKeyPressed().isEmpty() ? LastKeyPressed() : QStringLiteral("q");
    Focus = 0;
    for (int I = 0; I < Keys.size(); ++I)
        if (Keys[I].Label.compare(Want, Qt::CaseInsensitive) == 0) { Focus = I; break; }
    update();
}

void OnScreenKeyboard::resizeEvent(QResizeEvent *) { build(); }

bool OnScreenKeyboard::eventFilter(QObject *Watched, QEvent *Event)
{
    if (Watched == parentWidget() && Event->type() == QEvent::Resize) setGeometry(parentWidget()->rect());
    return QWidget::eventFilter(Watched, Event);
}

void OnScreenKeyboard::insert(const QString &S)
{
    Text.insert(Cursor, S);
    Cursor += static_cast<int>(S.size());
    //One capital after Shift, as a phone keyboard does it.
    if (Shift && !Symbols && !S.isEmpty() && S != " ") { Shift = false; build(); }
    edited();
}

void OnScreenKeyboard::backspace()
{
    if (Cursor <= 0) return;
    Text.remove(Cursor - 1, 1);
    --Cursor;
    edited();
}

void OnScreenKeyboard::edited()
{
    update();
    if (OnEdit) OnEdit(Text);
}

void OnScreenKeyboard::typeText(const QString &S)
{
    for (const QChar C : S) insert(QString(C));
}

void OnScreenKeyboard::press(const Key &K)
{
    if (K.K == Kind::Char) LastKeyPressed() = K.Label;
    switch (K.K)
    {
    case Kind::Char:      insert(K.Out); break;
    case Kind::Space:     insert(" "); break;
    case Kind::Backspace: backspace(); break;
    case Kind::Shift:     Shift = !Shift; build(); break;
    case Kind::Symbols:   Symbols = !Symbols; Shift = false; build(); break;
    case Kind::Left:      Cursor = std::max(0, Cursor - 1); update(); break;
    case Kind::Right:     Cursor = std::min(static_cast<int>(Text.size()), Cursor + 1); update(); break;
    case Kind::Done:      accept(); break;
    }
}

void OnScreenKeyboard::accept() { finish(true); }
void OnScreenKeyboard::cancel() { finish(false); }

void OnScreenKeyboard::finish(bool Accepted)
{
    if (Closing) return;
    Closing = true;
    PadRouter::instance().popOverlay(this);
    if (Current == this) Current = nullptr;
    QPointer<QWidget> F = Field;
    QPointer<QWidget> Back = Before;
    const QString S = Text;
    const auto Done = OnDone;
    const auto Edit = OnEdit;
    const QString Was = Original;
    hide();
    deleteLater();
    if (F)
    {
        if (Accepted) WriteBack(F, S);
        F->setFocus(Qt::OtherFocusReason);
        return;
    }
    if (Back) Back->setFocus(Qt::OtherFocusReason);   // where the controller was, before the keyboard
    if (Accepted) { if (Done) Done(S); }
    else if (Edit && S != Was) Edit(Was);   // as it was: a search typed into and abandoned shows everything again
}

void OnScreenKeyboard::moveFocus(Pad::Button Dir)
{
    if (Keys.isEmpty()) return;
    const QRectF From = Keys[Focus].Rect;
    QVector<QRect> Rects;
    for (const Key &K : Keys) Rects.push_back(K.Rect.toRect());
    Rects[Focus] = QRect();
    int To = PadNav::Nearest(From.toRect(), Dir, Rects);
    if (To < 0 && (Dir == Pad::Button::Left || Dir == Pad::Button::Right))
    {
        //Off a row's end: round to its other end.
        To = Focus;
        for (int I = 0; I < Keys.size(); ++I)
        {
            if (std::fabs(Keys[I].Rect.center().y() - From.center().y()) > 1) continue;
            if (Dir == Pad::Button::Right ? Keys[I].Rect.left() < Keys[To].Rect.left() : Keys[I].Rect.left() > Keys[To].Rect.left()) To = I;
        }
    }
    if (To >= 0 && To != Focus) { Focus = To; PadInput::instance().tick(); update(); }
}

bool OnScreenKeyboard::padPressed(Pad::Button B)
{
    using P = Pad::Button;
    switch (B)
    {
    case P::Up: case P::Down: case P::Left: case P::Right: moveFocus(B); break;
    case P::A:     if (Focus < Keys.size()) press(Keys[Focus]); break;
    case P::B:     cancel(); break;
    case P::X:     backspace(); break;
    case P::Y:     insert(" "); break;
    case P::LB:    Cursor = std::max(0, Cursor - 1); update(); break;
    case P::RB:    Cursor = std::min(static_cast<int>(Text.size()), Cursor + 1); update(); break;
    case P::LT: case P::L3: Shift = !Shift; build(); break;
    case P::RT:    Symbols = !Symbols; Shift = false; build(); break;
    case P::Start: accept(); break;
    default: break;
    }
    return true;   // while open, every press is the keyboard's
}

QList<PadTarget::Hint> OnScreenKeyboard::padHints() const
{
    using P = Pad::Button;
    return { { P::A, "Type" }, { P::X, "Delete" }, { P::Y, "Space" }, { P::LT, "Shift" }, { P::RT, Symbols ? "Letters" : "Symbols" },
             { P::Start, "Done" }, { P::B, "Cancel" } };
}

json OnScreenKeyboard::padState() const
{
    json J{{"kind", "keyboard"}, {"title", Title.toStdString()}, {"text", Text.toStdString()}, {"cursor", Cursor},
           {"page", Symbols ? "symbols" : "letters"}, {"shift", Shift}};
    if (Focus < Keys.size()) J["key"] = Keys[Focus].Label.toStdString();
    if (Field) J["field"] = PadNav::Describe(Field);
    J["top"] = static_cast<int>(panel().top());   // where the keys start: what of the screen above stays in sight
    return J;
}

void OnScreenKeyboard::mousePressEvent(QMouseEvent *E)
{
    const QPointF At = E->position();
    for (int I = 0; I < Keys.size(); ++I)
        if (Keys[I].Rect.contains(At)) { Focus = I; press(Keys[I]); return; }
    if (!panel().contains(At)) cancel();   // a tap on the dimmed window above: as B
}

void OnScreenKeyboard::keyPressEvent(QKeyEvent *E)
{
    switch (E->key())
    {
    case Qt::Key_Escape:    cancel(); return;
    case Qt::Key_Return: case Qt::Key_Enter: accept(); return;
    case Qt::Key_Backspace: backspace(); return;
    case Qt::Key_Left:      Cursor = std::max(0, Cursor - 1); update(); return;
    case Qt::Key_Right:     Cursor = std::min(static_cast<int>(Text.size()), Cursor + 1); update(); return;
    case Qt::Key_Home:      Cursor = 0; update(); return;
    case Qt::Key_End:       Cursor = static_cast<int>(Text.size()); update(); return;
    default:
        if (!E->text().isEmpty() && E->text().at(0).isPrint()) { insert(E->text()); return; }
        QWidget::keyPressEvent(E);
    }
}

void OnScreenKeyboard::paintEvent(QPaintEvent *)
{
    QPainter P(this);
    P.setRenderHint(QPainter::Antialiasing);
    QColor Scrim = DeckStyle::GroundLow;
    Scrim.setAlpha(185);
    P.fillRect(rect(), Scrim);
    const QRectF Pn = panel();
    const double S = std::clamp(Pn.height() / 470.0, 0.6, 1.4);
    QPainterPath Bg;
    Bg.addRoundedRect(Pn, 20, 20);
    DeckStyle::DrawLift(P, Bg, S * 1.4);
    P.fillPath(Bg, DeckStyle::Panel);

    //The title, then the text being typed with its cursor.
    P.setFont(DeckStyle::Display(15, 1, true));
    P.setPen(DeckStyle::MagentaHi);
    P.drawText(QRectF(Pn.left() + 22, Pn.top() + 8, Pn.width() - 40, 22), Qt::AlignLeft | Qt::AlignVCenter, Title);
    const QRectF Box(Pn.left() + 16, Pn.top() + 30, Pn.width() - 32, 32);
    P.fillPath(DeckStyle::Capsule(Box), DeckStyle::GroundLow);
    QFont F = font();
    F.setPixelSize(19);
    P.setFont(F);
    const QFontMetricsF M(F);
    const QString Typed = Text.left(Cursor);
    double X = Box.left() + 16, Scroll = 0;
    const double Room = Box.width() - 32, CursorX = M.horizontalAdvance(Typed);
    if (CursorX > Room) Scroll = CursorX - Room;   // a long text scrolls so the cursor stays in sight
    P.save();
    P.setClipRect(Box.adjusted(12, 0, -12, 0));
    P.setPen(DeckStyle::Text);
    P.drawText(QPointF(X - Scroll, Box.center().y() + M.ascent() / 2 - 2), Text);
    P.fillRect(QRectF(X - Scroll + CursorX, Box.top() + 6, 2, Box.height() - 12), DeckStyle::Lime);
    P.restore();

    //The keys.
    F.setPixelSize(std::clamp(static_cast<int>(Keys.isEmpty() ? 18 : Keys[0].Rect.height() * 0.42), 13, 24));
    F.setWeight(QFont::DemiBold);
    P.setFont(F);
    for (int I = 0; I < Keys.size(); ++I)
    {
        const Key &K = Keys[I];
        const bool On = I == Focus;
        const bool Lit = (K.K == Kind::Shift && Shift) || (K.K == Kind::Symbols && Symbols);
        QPainterPath Kp;
        Kp.addRoundedRect(K.Rect, K.Rect.height() * 0.32, K.Rect.height() * 0.32);
        QColor Fill = K.K == Kind::Char || K.K == Kind::Space ? DeckStyle::PanelHi : QColor(0x2e, 0x21, 0x37);
        if (K.K == Kind::Done) Fill = DeckStyle::Magenta;
        if (Lit) Fill = QColor(0x5a, 0x2a, 0x66);
        if (On) { Fill = DeckStyle::Lime; DeckStyle::DrawLift(P, Kp, S * 0.8); }
        P.fillPath(Kp, Fill);
        P.setPen(On ? DeckStyle::Ink : DeckStyle::Text);
        P.drawText(K.Rect, Qt::AlignCenter, K.Label);
    }
    //What the buttons do, inside the keyboard: a dialog has no hint bar of its own, and the shell's is under it.
    DeckStyle::DrawHints(P, QRectF(Pn.left() + 18, Pn.bottom() - 44, Pn.width() - 36, 38), padHints(), PadInput::instance().glyphs(), S);
}
