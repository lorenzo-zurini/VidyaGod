#include "decklist.h"
#include "deckshell.h"
#include "deckstyle.h"
#include "qrcode.h"
#include "padinput.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>

#include <algorithm>

using json = nlohmann::ordered_json;

DeckList::DeckList(DeckShell &S, const QString &Name, std::function<QList<DeckRow>()> B)
    : QWidget(nullptr), Shell(S), Build(std::move(B))
{
    setObjectName(Name);
    setFocusPolicy(Qt::StrongFocus);
    setProperty("padOwnFocus", true);
}

void DeckList::refresh()
{
    const QString Was = current();
    Rows = Build();
    Current = -1;
    for (int I = 0; I < Rows.size(); ++I) if (Rows[I].stops() && Rows[I].Id == Was) { Current = I; break; }
    if (Current < 0) for (int I = 0; I < Rows.size(); ++I) if (Rows[I].stops()) { Current = I; break; }
    if (!Armed.isEmpty() && current() != Armed) Armed.clear();
    keepInSight();
    update();
    emit PadRouter::instance().hintsChanged();
}

void DeckList::setNote(const QString &Text) { Note = Text; update(); }

QString DeckList::current() const { return Current >= 0 && Current < Rows.size() ? Rows[Current].Id : QString(); }

bool DeckList::setCurrent(const QString &Id)
{
    for (int I = 0; I < Rows.size(); ++I)
        if (Rows[I].stops() && Rows[I].Id == Id) { Current = I; Armed.clear(); keepInSight(); update(); emit PadRouter::instance().hintsChanged(); return true; }
    return false;
}

bool DeckList::press(const QString &Id)
{
    if (!setCurrent(Id)) return false;
    act(Current, 0);
    return true;
}

void DeckList::showEvent(QShowEvent *E)
{
    QWidget::showEvent(E);
    refresh();
}

double DeckList::rowHeight(const DeckRow &R) const
{
    const double S = Shell.scale();
    switch (R.K)
    {
    case DeckRow::Kind::Header: return 50 * S;
    case DeckRow::Kind::Qr:     return 250 * S;
    case DeckRow::Kind::Info:   return (R.Says.isEmpty() ? 40 : 66) * S;
    default:                    return (R.Says.isEmpty() ? 64 : 94) * S;
    }
}

QRectF DeckList::rowRect(int I) const
{
    const double S = Shell.scale();
    double Y = (Note.isEmpty() ? 16 : 60) * S - Scroll;
    for (int K = 0; K < I; ++K) Y += rowHeight(Rows[K]) + 10 * S;
    return QRectF(40 * S, Y, std::min(width() - 80 * S, 1100 * S), rowHeight(Rows[I]));
}

void DeckList::keepInSight()
{
    if (Current < 0 || height() <= 0) return;
    const double S = Shell.scale();
    QRectF R = rowRect(Current);
    //A group's title stays with its first row.
    if (Current > 0 && Rows[Current - 1].K == DeckRow::Kind::Header) R.setTop(rowRect(Current - 1).top());
    if (R.bottom() > height() - 10 * S) Scroll += R.bottom() - (height() - 10 * S);
    if (R.top() < 10 * S) Scroll = std::max(0.0, Scroll - (10 * S - R.top()));
}

void DeckList::act(int I, int Step)
{
    if (I < 0 || I >= Rows.size() || !Rows[I].Enabled) return;
    DeckRow &R = Rows[I];
    switch (R.K)
    {
    case DeckRow::Kind::Toggle:
    {
        const bool To = Step == 0 ? !R.On : Step > 0;
        if (To == R.On) return;
        R.On = To;
        if (R.Toggle) R.Toggle(To);
        break;
    }
    case DeckRow::Kind::Choice:
    {
        if (R.Values.isEmpty()) return;
        const int N = static_cast<int>(R.Values.size());
        const int To = Step == 0 ? (R.Value + 1) % N : std::clamp(R.Value + Step, 0, N - 1);
        if (To == R.Value) return;
        R.Value = To;
        if (R.Choose) R.Choose(To);
        break;
    }
    case DeckRow::Kind::Action:
        if (Step != 0) return;
        if (R.Confirm && Armed != R.Id) { Armed = R.Id; update(); emit PadRouter::instance().hintsChanged(); return; }
        Armed.clear();
        if (R.Press) { const auto P = R.Press; P(); }   // a copy: the press may rebuild the rows
        break;
    default: return;
    }
    PadInput::instance().tick();
    update();
    //What it changed is read back from the models (a refused change shows as it is).
    QTimer::singleShot(0, this, [this] { refresh(); });
}

bool DeckList::padPressed(Pad::Button B)
{
    using P = Pad::Button;
    switch (B)
    {
    case P::Up: case P::Down:
    {
        const int Dir = B == P::Down ? 1 : -1;
        for (int I = Current + Dir; I >= 0 && I < Rows.size(); I += Dir)
            if (Rows[I].stops())
            {
                Current = I;
                Armed.clear();
                keepInSight();
                PadInput::instance().tick();
                update();
                emit PadRouter::instance().hintsChanged();
                break;
            }
        return true;
    }
    case P::A: act(Current, 0); return true;
    case P::Left: case P::Right:
        if (Current < 0 || (Rows[Current].K != DeckRow::Kind::Toggle && Rows[Current].K != DeckRow::Kind::Choice)) return true;
        act(Current, B == P::Right ? 1 : -1);
        return true;
    default: return false;
    }
}

QList<PadTarget::Hint> DeckList::padHints() const
{
    if (Current < 0) return {};
    const DeckRow &R = Rows[Current];
    if (!R.Enabled) return {};
    switch (R.K)
    {
    case DeckRow::Kind::Toggle: return { { Pad::Button::A, R.On ? "Turn off" : "Turn on" } };
    case DeckRow::Kind::Choice: return { { Pad::Button::Left, "Less" }, { Pad::Button::Right, "More" } };
    case DeckRow::Kind::Action: return { { Pad::Button::A, R.Confirm ? (Armed == R.Id ? "Confirm" : "Select") : "Select" } };
    default: return {};
    }
}

json DeckList::padState() const
{
    static const char *Kinds[] = { "header", "info", "toggle", "choice", "action", "qr" };
    json Out = json::array();
    for (const DeckRow &R : Rows)
    {
        json J{{"kind", Kinds[static_cast<int>(R.K)]}, {"title", R.Title.toStdString()}};
        if (!R.Id.isEmpty()) J["id"] = R.Id.toStdString();
        if (!R.Says.isEmpty()) J["says"] = R.Says.toStdString();
        if (R.K == DeckRow::Kind::Toggle) J["on"] = R.On;
        if (R.K == DeckRow::Kind::Choice) J["value"] = R.Values.value(R.Value).toStdString();
        if (R.K == DeckRow::Kind::Qr) J["code"] = R.Code.toStdString();
        if (!R.Enabled) J["enabled"] = false;
        Out.push_back(J);
    }
    json J{{"kind", objectName().toStdString()}, {"rows", Out}, {"current", current().toStdString()}};
    if (!Note.isEmpty()) J["note"] = Note.toStdString();
    if (!Armed.isEmpty()) J["armed"] = Armed.toStdString();
    return J;
}

void DeckList::mouseReleaseEvent(QMouseEvent *E)
{
    for (int I = 0; I < Rows.size(); ++I)
        if (Rows[I].stops() && rowRect(I).contains(E->position()))
        {
            Current = I;
            //A tap on a toggle's or a value's right end steps it; anywhere else it is A.
            act(I, 0);
            return;
        }
}

void DeckList::paintEvent(QPaintEvent *)
{
    QPainter P(this);
    P.setRenderHint(QPainter::Antialiasing);
    const double S = Shell.scale();
    if (!Note.isEmpty())
    {
        P.setFont(DeckStyle::Font(16, S, QFont::DemiBold));
        P.setPen(DeckStyle::Gold);
        P.drawText(QRectF(40 * S, 12 * S, width() - 80 * S, 36 * S), Qt::AlignLeft | Qt::AlignVCenter, Note);
    }
    P.save();
    P.setClipRect(QRectF(0, (Note.isEmpty() ? 0 : 54) * S, width(), height()));
    for (int I = 0; I < Rows.size(); ++I)
    {
        const QRectF R = rowRect(I);
        if (R.bottom() < 0 || R.top() > height()) continue;
        const DeckRow &Rw = Rows[I];
        const bool On = I == Current;
        if (Rw.K == DeckRow::Kind::Header)
        {
            P.setFont(DeckStyle::Font(15, S, QFont::Bold));
            P.setPen(DeckStyle::MagentaHi);
            P.drawText(R.adjusted(4 * S, 0, 0, -6 * S), Qt::AlignLeft | Qt::AlignBottom, Rw.Title.toUpper());
            continue;
        }
        if (Rw.K == DeckRow::Kind::Info)
        {
            P.setFont(DeckStyle::Font(18, S, QFont::DemiBold));
            P.setPen(DeckStyle::Text);
            P.drawText(QRectF(R.left() + 4 * S, R.top(), R.width(), 34 * S), Qt::AlignLeft | Qt::AlignVCenter, Rw.Title);
            if (!Rw.Says.isEmpty())
            {
                P.setFont(DeckStyle::Font(15, S));
                P.setPen(DeckStyle::Dim);
                P.drawText(QRectF(R.left() + 4 * S, R.top() + 32 * S, R.width(), 28 * S), Qt::AlignLeft | Qt::AlignVCenter, Rw.Says);
            }
            continue;
        }
        if (Rw.K == DeckRow::Kind::Qr)
        {
            //The code on white, with its quiet zone: a camera reads dark on light.
            const auto M = QrCode::Encode(Rw.Code.toStdString());
            const double Side = R.height() - 10 * S;
            const QRectF Box(R.left(), R.top() + 5 * S, Side, Side);
            P.fillRect(Box, Qt::white);
            if (!M.empty())
            {
                const int N = static_cast<int>(M.size());
                const double Cell = Side / (N + 8);
                P.setPen(Qt::NoPen);
                P.setBrush(Qt::black);
                for (int Y = 0; Y < N; ++Y)
                    for (int X = 0; X < N; ++X)
                        if (M[Y][X]) P.drawRect(QRectF(Box.left() + (X + 4) * Cell, Box.top() + (Y + 4) * Cell, Cell + 0.5, Cell + 0.5));
            }
            const double X = Box.right() + 28 * S;
            P.setFont(DeckStyle::Display(34, S));
            P.setPen(DeckStyle::Text);
            P.drawText(QRectF(X, Box.top() + 20 * S, R.right() - X, 50 * S), Qt::AlignLeft | Qt::AlignVCenter, Rw.Title);
            P.setFont(DeckStyle::Font(16, S));
            P.setPen(DeckStyle::Dim);
            P.drawText(QRectF(X, Box.top() + 80 * S, R.right() - X, Box.height() - 90 * S), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, Rw.Says);
            continue;
        }
        QPainterPath Bg;
        Bg.addRoundedRect(R, 14 * S, 14 * S);
        if (On) DeckStyle::DrawLift(P, Bg, S);
        P.fillPath(Bg, On ? DeckStyle::PanelHi : DeckStyle::Panel);
        if (On) DeckStyle::DrawRing(P, Bg, S);
        const QColor Tc = Rw.Enabled ? DeckStyle::Text : DeckStyle::Faint;
        const double Right = 200 * S;
        const QString Title = (Rw.K == DeckRow::Kind::Action && Armed == Rw.Id) ? "Press A again to " + Rw.Title.left(1).toLower() + Rw.Title.mid(1) : Rw.Title;
        P.setFont(DeckStyle::Font(19, S, QFont::DemiBold));
        P.setPen(Armed == Rw.Id ? DeckStyle::Gold : Tc);
        P.drawText(QRectF(R.left() + 22 * S, R.top() + 12 * S, R.width() - Right - 30 * S, 34 * S), Qt::AlignLeft | Qt::AlignVCenter, Title);
        if (!Rw.Says.isEmpty())
        {
            P.setFont(DeckStyle::Font(15, S));
            P.setPen(Rw.Enabled ? DeckStyle::Dim : DeckStyle::Faint);
            P.drawText(QRectF(R.left() + 22 * S, R.top() + 46 * S, R.width() - Right - 30 * S, 36 * S), Qt::AlignLeft | Qt::AlignVCenter, Rw.Says);
        }
        const QRectF Side(R.right() - Right, R.top(), Right - 22 * S, R.height());
        if (Rw.K == DeckRow::Kind::Toggle)
        {
            const QRectF Track(Side.right() - 64 * S, Side.center().y() - 16 * S, 64 * S, 32 * S);
            P.fillPath(DeckStyle::Capsule(Track), Rw.On ? DeckStyle::Lime : DeckStyle::GroundLow);
            const double K = 26 * S;
            const QRectF Knob(Rw.On ? Track.right() - K - 3 * S : Track.left() + 3 * S, Track.top() + 3 * S, K, K);
            P.setPen(Qt::NoPen);
            P.setBrush(Rw.On ? DeckStyle::Ink : DeckStyle::Dim);
            P.drawEllipse(Knob);
        }
        else if (Rw.K == DeckRow::Kind::Choice)
        {
            P.setFont(DeckStyle::Font(18, S, QFont::DemiBold));
            P.setPen(Tc);
            P.drawText(Side, Qt::AlignRight | Qt::AlignVCenter, "‹  " + Rw.Values.value(Rw.Value) + "  ›");
        }
        else
        {
            P.setFont(DeckStyle::Font(22, S, QFont::DemiBold));
            P.setPen(DeckStyle::Dim);
            P.drawText(Side, Qt::AlignRight | Qt::AlignVCenter, "›");
        }
    }
    P.restore();
}
