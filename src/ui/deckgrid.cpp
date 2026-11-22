#include "deckgrid.h"

#include "deckstyle.h"
#include "libraryview.h"   // LibraryGameCard

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

using json = nlohmann::ordered_json;

DeckGrid::DeckGrid(QWidget *Parent) : QWidget(Parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setProperty("padOwnFocus", true);   // it lights its own focused cover: no ring over it
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    Anim = new QTimer(this);
    Anim->setInterval(16);
    connect(Anim, &QTimer::timeout, this, &DeckGrid::animate);
}

void DeckGrid::setCards(const QList<LibraryGameCard *> &C, const QVector<Section> &S)
{
    const QString Was = current() ? current()->GameKey : QString();
    Cards = C;
    Sections = S;
    Current = Cards.isEmpty() ? -1 : 0;
    relayout();
    if (!Was.isEmpty()) setCurrentKey(Was);
    Scroll = Target;
    update();
}

void DeckGrid::setStrip(bool On) { Strip = On; relayout(); update(); }
void DeckGrid::setLit(bool On) { if (Lit == On) return; Lit = On; Lift = 0; Anim->start(); update(); }
void DeckGrid::setEmptyText(const QString &T) { Empty = T; update(); }

LibraryGameCard *DeckGrid::current() const { return Current >= 0 && Current < Cards.size() ? Cards[Current] : nullptr; }

void DeckGrid::setCurrentIndex(int I)
{
    if (I < 0 || I >= Cards.size() || I == Current) return;
    Current = I;
    Lift = 0;
    reveal();
    Anim->start();
    update();
    emit currentChanged(current());
}

bool DeckGrid::setCurrentKey(const QString &Key)
{
    for (int I = 0; I < Cards.size(); ++I)
        if (Cards[I] && Cards[I]->GameKey == Key) { Current = -1; setCurrentIndex(I); return true; }
    return false;
}

QRectF DeckGrid::tileRect(int I) const
{
    if (I < 0 || I >= Rects.size()) return {};
    return Strip ? Rects[I].translated(-Scroll, 0) : Rects[I].translated(0, -Scroll);
}

double DeckGrid::maxScroll() const { return std::max(0.0, Content - (Strip ? width() : height())); }

void DeckGrid::relayout()
{
    const double S = DeckStyle::ScaleFor(window() ? window()->height() : height());
    Rects.clear();
    Headers.clear();
    if (Strip)
    {
        //One row: tiles as tall as the strip leaves room for (the lifted one included), side by side.
        const double Pad = 10 * S, Gap = 18 * S, Side = 40 * S;
        const double H = std::max(40.0, height() - 2 * Pad);
        Tile = QSizeF(H / 1.5, H);
        double X = Side;
        for (int I = 0; I < Cards.size(); ++I) { Rects.push_back(QRectF(X, Pad, Tile.width(), Tile.height())); X += Tile.width() + Gap; }
        Content = X - Gap + Side;
    }
    else
    {
        //A grid as wide as the screen allows at about 178 px a cover (six across a Deck), sections each on a new row
        //under their header.
        const double Side = 40 * S, Gap = 22 * S, RowGap = 26 * S, HeadH = 58 * S, Top = 14 * S;
        const int Cols = std::max(2, static_cast<int>((width() - 2 * Side + Gap) / (178 * S + Gap)));
        const double W = (width() - 2 * Side - (Cols - 1) * Gap) / Cols;
        Tile = QSizeF(W, W * 1.5);
        double Y = Top;
        int Col = 0;
        int Sec = 0;
        for (int I = 0; I < Cards.size(); ++I)
        {
            if (Sec < Sections.size() && Sections[Sec].First == I)
            {
                if (Col != 0) { Y += Tile.height() + RowGap; Col = 0; }
                Headers.push_back({ Sections[Sec].Name, QRectF(Side, Y, width() - 2 * Side, HeadH) });
                Y += HeadH;
                ++Sec;
            }
            Rects.push_back(QRectF(Side + Col * (W + Gap), Y, W, Tile.height()));
            if (++Col == Cols) { Col = 0; Y += Tile.height() + RowGap; }
        }
        Content = (Col == 0 ? Y : Y + Tile.height() + RowGap) + Top;
    }
    Target = std::clamp(Target, 0.0, maxScroll());
    reveal();
    Scroll = std::clamp(Scroll, 0.0, maxScroll());
}

void DeckGrid::resizeEvent(QResizeEvent *) { relayout(); Scroll = Target; }

void DeckGrid::reveal()
{
    if (Current < 0 || Current >= Rects.size()) return;
    const double S = DeckStyle::ScaleFor(window() ? window()->height() : height());
    const QRectF R = Rects[Current];
    const double Lo = Strip ? R.left() : R.top(), Hi = Strip ? R.right() : R.bottom(), View = Strip ? width() : height();
    //Keep a margin round the focused cover: its lift and glow, and a glimpse of what is next.
    const double M = Strip ? 60 * S : std::min(70 * S, View * 0.12);
    if (Lo - M < Target) Target = Lo - M;
    else if (Hi + M > Target + View) Target = Hi + M - View;
    //A card of the first row shows the top of the shelf, its header too.
    if (!Rects.isEmpty() && (Strip ? R.left() : R.top()) <= (Strip ? Rects.first().left() : Rects.first().top())) Target = 0;
    Target = std::clamp(Target, 0.0, maxScroll());
    Anim->start();
}

void DeckGrid::animate()
{
    bool Moving = false;
    if (std::fabs(Target - Scroll) > 0.5) { Scroll += (Target - Scroll) * 0.28; Moving = true; }
    else Scroll = Target;
    if (Lift < 1) { Lift = std::min(1.0, Lift + 0.14); Moving = true; }
    update();
    if (!Moving) Anim->stop();
}

bool DeckGrid::padPressed(Pad::Button B)
{
    if (B == Pad::Button::A)
    {
        if (!current()) return false;
        emit activated(current());
        return true;
    }
    if (!Pad::IsDirection(B) || Cards.isEmpty()) return false;
    if (Current < 0) { setCurrentIndex(0); return true; }
    QVector<QRect> Others;
    for (int I = 0; I < Rects.size(); ++I) Others.push_back(I == Current ? QRect() : Rects[I].toRect());
    const int To = PadNav::Nearest(Rects[Current].toRect(), B, Others);
    if (To < 0) return false;   // the edge: the screen around it decides
    setCurrentIndex(To);
    PadInput::instance().tick();
    return true;
}

bool DeckGrid::padScrolled(double Dx, double Dy)
{
    Target = std::clamp(Target + (Strip ? Dx + Dy : Dy), 0.0, maxScroll());
    Scroll = Target;
    update();
    return true;
}

json DeckGrid::padState() const
{
    json J{{"kind", Strip ? "strip" : "grid"}, {"count", count()}, {"index", Current}};
    if (const LibraryGameCard *C = current()) J["card"] = json{{"title", C->GameTitle.toStdString()}, {"key", C->GameKey.toStdString()}};
    if (!Lit) J["lit"] = false;
    return J;
}

int DeckGrid::hit(const QPointF &At) const
{
    for (int I = 0; I < Rects.size(); ++I)
        if (tileRect(I).contains(At)) return I;
    return -1;
}

void DeckGrid::mousePressEvent(QMouseEvent *E)
{
    Pressed = true;
    Dragging = false;
    PressAt = E->position();
    PressScroll = Target;
}

void DeckGrid::mouseMoveEvent(QMouseEvent *E)
{
    if (!Pressed) return;
    const QPointF D = E->position() - PressAt;
    //A finger dragging the shelf (a touch screen), not a tap that wobbled.
    if (!Dragging && D.manhattanLength() > 14) Dragging = true;
    if (!Dragging) return;
    Target = Scroll = std::clamp(PressScroll - (Strip ? D.x() : D.y()), 0.0, maxScroll());
    update();
}

void DeckGrid::mouseReleaseEvent(QMouseEvent *E)
{
    if (!Pressed) return;
    Pressed = false;
    if (Dragging) { Dragging = false; return; }
    const int I = hit(E->position());
    if (I < 0) return;
    setFocus(Qt::MouseFocusReason);
    setCurrentIndex(I);
    emit activated(Cards[I]);   // a tap opens it, as on the Deck's own library
}

void DeckGrid::wheelEvent(QWheelEvent *E)
{
    const double D = -(E->angleDelta().y() != 0 ? E->angleDelta().y() : E->angleDelta().x()) * 0.9;
    Target = std::clamp(Target + D, 0.0, maxScroll());
    Anim->start();
}

void DeckGrid::paintEvent(QPaintEvent *)
{
    QPainter P(this);
    P.setRenderHint(QPainter::Antialiasing);
    const double S = DeckStyle::ScaleFor(window() ? window()->height() : height());
    if (Cards.isEmpty())
    {
        P.setFont(DeckStyle::Font(19, S));
        P.setPen(DeckStyle::Dim);
        P.drawText(rect().adjusted(40 * S, 0, -40 * S, 0), Qt::AlignCenter | Qt::TextWordWrap, Empty);
        return;
    }
    //Section headers: the title in the display italic, and a rule.
    for (const auto &[Name, R0] : Headers)
    {
        const QRectF R = Strip ? R0 : R0.translated(0, -Scroll);
        if (R.bottom() < 0 || R.top() > height()) continue;
        const QFont F = DeckStyle::Display(19, S, true);
        P.setFont(F);
        P.setPen(DeckStyle::MagentaHi);
        P.drawText(R.adjusted(0, 0, 0, -18 * S), Qt::AlignLeft | Qt::AlignBottom, Name);
        const double Tw = QFontMetricsF(F).horizontalAdvance(Name) + 16 * S;
        P.fillRect(QRectF(R.left() + Tw, R.bottom() - 18 * S - QFontMetricsF(F).ascent() / 2, R.width() - Tw, 1), DeckStyle::Panel);
    }
    const auto Draw = [&](int I, bool Focused) {
        LibraryGameCard *C = Cards[I];
        QRectF R = tileRect(I);
        if (!R.intersects(QRectF(rect()).adjusted(-40, -40, 40, 40))) return;
        const double K = Focused ? 1.0 + 0.07 * (1 - std::pow(1 - Lift, 3)) : 1.0;
        if (K != 1.0) R = QRectF(R.center().x() - R.width() * K / 2, R.center().y() - R.height() * K / 2, R.width() * K, R.height() * K);
        const double Rad = 8 * S;
        QPainterPath Shape;
        Shape.addRoundedRect(R, Rad, Rad);
        if (Focused) DeckStyle::DrawLift(P, Shape, S * 1.3, Lift);   // the lit cover, lifted off its magenta shadow
        else
        {
            P.setPen(Qt::NoPen);
            P.setBrush(QColor(0, 0, 0, 90));
            P.drawRoundedRect(R.translated(0, 4 * S), Rad, Rad);
        }
        DeckStyle::DrawCover(P, R, C->CoverOriginal, C->GameTitle, Rad);
        //Badges: golden, local, the games a package holds besides this one, a download under way.
        QFont Bf = DeckStyle::Font(11, S, QFont::Bold);
        Bf.setLetterSpacing(QFont::PercentageSpacing, 108);
        P.setFont(Bf);
        const QFontMetricsF Bm(Bf);
        double Bx = R.left() + 7 * S;
        const auto Badge = [&](const QString &T, const QColor &Bg, const QColor &Fg) {
            const QRectF Br(Bx, R.top() + 7 * S, Bm.horizontalAdvance(T) + 12 * S, 20 * S);
            QPainterPath Bp;
            Bp = DeckStyle::Capsule(Br);
            P.fillPath(Bp, Bg);
            P.setPen(Fg);
            P.drawText(Br, Qt::AlignCenter, T);
            Bx = Br.right() + 5 * S;
        };
        if (C->Golden) Badge("★ GOLDEN", QColor(0x3a, 0x2c, 0x08, 235), DeckStyle::Gold);
        QColor BadgeGround = DeckStyle::GroundLow;
        BadgeGround.setAlpha(225);
        if (C->Local) Badge("LOCAL", BadgeGround, DeckStyle::Dim);
        if (!C->Secondaries.empty()) Badge(QString("+%1").arg(C->Secondaries.size()), BadgeGround, DeckStyle::Text);
        if (C->Downloading)
        {
            const QRectF Bar(R.left() + 8 * S, R.bottom() - 16 * S, R.width() - 16 * S, 7 * S);
            QPainterPath Track;
            Track.addRoundedRect(Bar, Bar.height() / 2, Bar.height() / 2);
            P.fillPath(Track, QColor(0, 0, 0, 170));
            if (C->DownloadPercent >= 0)
            {
                QPainterPath Fill;
                Fill.addRoundedRect(QRectF(Bar.left(), Bar.top(), Bar.width() * std::clamp(C->DownloadPercent / 100.0, 0.02, 1.0), Bar.height()),
                                    Bar.height() / 2, Bar.height() / 2);
                P.fillPath(Fill, DeckStyle::LimeHi);
            }
            const QString Pct = C->DownloadPercent >= 0 ? QString("%1%").arg(static_cast<int>(C->DownloadPercent)) : QStringLiteral("Queued");
            const QRectF Tr(R.left() + 8 * S, Bar.top() - 24 * S, Bm.horizontalAdvance(Pct) + 12 * S, 19 * S);
            QPainterPath Tp;
            Tp.addRoundedRect(Tr, 4 * S, 4 * S);
            P.fillPath(Tp, QColor(0, 0, 0, 190));
            P.setPen(DeckStyle::Text);
            P.drawText(Tr, Qt::AlignCenter, Pct);
        }
        if (Focused)
        {
            QPainterPath Edge;
            Edge.addRoundedRect(R.adjusted(-1.5 * S, -1.5 * S, 1.5 * S, 1.5 * S), Rad + 1.5 * S, Rad + 1.5 * S);
            DeckStyle::DrawRing(P, Edge, S, Lift);
        }
    };
    for (int I = 0; I < Cards.size(); ++I)
        if (I != Current || !Lit) Draw(I, false);
    if (Lit && Current >= 0 && Current < Cards.size()) Draw(Current, true);   // the lifted cover over its neighbours
    //A strip fades at its ends: there is more that way.
    if (Strip)
    {
        const double F = 46 * S;
        QColor G0 = DeckStyle::Ground, G1 = DeckStyle::Ground;
        G1.setAlpha(0);
        if (Scroll > 1)
        {
            QLinearGradient L(0, 0, F, 0);
            L.setColorAt(0, G0); L.setColorAt(1, G1);
            P.fillRect(QRectF(0, 0, F, height()), L);
        }
        if (Scroll < maxScroll() - 1)
        {
            QLinearGradient Rg(width() - F, 0, width(), 0);
            Rg.setColorAt(0, G1); Rg.setColorAt(1, G0);
            P.fillRect(QRectF(width() - F, 0, F, height()), Rg);
        }
    }
}
