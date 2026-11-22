#include "deckstyle.h"

#include <QFontDatabase>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include <algorithm>
#include <cmath>

namespace DeckStyle
{

double ScaleFor(int Height) { return std::clamp(Height / 800.0, 0.7, 2.7); }

QFont Font(double Px, double S, int Weight)
{
    QFont F;
    //The Deck's own UI face when it is installed (SteamOS ships Noto Sans), else the app's.
    static const bool HasNoto = QFontDatabase::families().contains("Noto Sans");
    if (HasNoto) F.setFamily("Noto Sans");
    F.setPixelSize(std::max(8, static_cast<int>(std::lround(Px * S))));
    F.setWeight(static_cast<QFont::Weight>(Weight));
    F.setHintingPreference(QFont::PreferNoHinting);
    return F;
}

namespace
{
bool Face(Pad::Button B) { return B == Pad::Button::A || B == Pad::Button::B || B == Pad::Button::X || B == Pad::Button::Y; }
bool Pill(Pad::Button B)
{
    using P = Pad::Button;
    return B == P::LB || B == P::RB || B == P::LT || B == P::RT || B == P::Start || B == P::Back || B == P::Guide;
}

QColor FaceColour(Pad::Button B, Pad::Glyphs G)
{
    using P = Pad::Button;
    switch (G)
    {
    case Pad::Glyphs::Xbox:
        return B == P::A ? QColor(0x6c, 0xc2, 0x4a) : B == P::B ? QColor(0xe2, 0x52, 0x48) : B == P::X ? QColor(0x46, 0x9c, 0xe8) : QColor(0xf2, 0xc2, 0x30);
    case Pad::Glyphs::PlayStation:
        return B == P::A ? QColor(0x8a, 0xb0, 0xe8) : B == P::B ? QColor(0xe8, 0x76, 0x76) : B == P::X ? QColor(0xe0, 0x92, 0xd0) : QColor(0x44, 0xc8, 0xae);
    default:
        return Text;   // Nintendo and the Deck: plain letters
    }
}
}

QString GlyphText(Pad::Button B, Pad::Glyphs G)
{
    using P = Pad::Button;
    const bool Ps = G == Pad::Glyphs::PlayStation, Nin = G == Pad::Glyphs::Nintendo, Deck = G == Pad::Glyphs::SteamDeck;
    switch (B)
    {
    case P::A: return Ps ? "✕" : Nin ? "B" : "A";
    case P::B: return Ps ? "○" : Nin ? "A" : "B";
    case P::X: return Ps ? "□" : Nin ? "Y" : "X";
    case P::Y: return Ps ? "△" : Nin ? "X" : "Y";
    case P::LB: return Ps || Deck ? "L1" : Nin ? "L" : "LB";
    case P::RB: return Ps || Deck ? "R1" : Nin ? "R" : "RB";
    case P::LT: return Ps || Deck ? "L2" : Nin ? "ZL" : "LT";
    case P::RT: return Ps || Deck ? "R2" : Nin ? "ZR" : "RT";
    case P::Start: return Nin ? "+" : "☰";
    case P::Back: return Nin ? "−" : "⧉";
    case P::Guide: return Ps ? "PS" : Nin ? "⌂" : Deck ? "STEAM" : "⊗";
    case P::L3: return "L3";
    case P::R3: return "R3";
    case P::Up: return "▲";
    case P::Down: return "▼";
    case P::Left: return "◀▶";
    case P::Right: return "▶";
    }
    return {};
}

double GlyphWidth(Pad::Button B, Pad::Glyphs G, double H)
{
    if (Pill(B)) return std::max(H * 1.5, H * 0.42 * GlyphText(B, G).size() + H * 0.7);
    if (B == Pad::Button::Left) return H * 1.35;   // ◀▶: "left and right"
    return H;
}

void DrawGlyph(QPainter &P, const QRectF &R, Pad::Button B, Pad::Glyphs G)
{
    P.save();
    P.setRenderHint(QPainter::Antialiasing);
    const double H = R.height();
    const QString T = GlyphText(B, G);
    QFont F = Font(1, 1, QFont::Bold);
    if (Face(B))
    {
        const QRectF C(R.left(), R.top(), H, H);
        P.setPen(Qt::NoPen);
        P.setBrush(QColor(0x2a, 0x33, 0x3d));
        P.drawEllipse(C);
        P.setPen(QPen(FaceColour(B, G), std::max(1.0, H * 0.07)));
        P.setBrush(Qt::NoBrush);
        P.drawEllipse(C.adjusted(H * 0.05, H * 0.05, -H * 0.05, -H * 0.05));
        F.setPixelSize(std::max(6, static_cast<int>(H * (G == Pad::Glyphs::PlayStation ? 0.5 : 0.56))));
        P.setFont(F);
        P.setPen(FaceColour(B, G));
        P.drawText(C, Qt::AlignCenter, T);
    }
    else if (Pill(B))
    {
        const QRectF C(R.left(), R.top() + H * 0.08, GlyphWidth(B, G, H), H * 0.84);
        QPainterPath Path;
        //Triggers are taller at the top, as they are on the controller: a pill with a flat bottom.
        const bool Trigger = B == Pad::Button::LT || B == Pad::Button::RT;
        Path.addRoundedRect(C, C.height() * (Trigger ? 0.32 : 0.5), C.height() * (Trigger ? 0.32 : 0.5));
        P.fillPath(Path, QColor(0x2a, 0x33, 0x3d));
        P.setPen(QPen(QColor(0x6a, 0x77, 0x84), std::max(1.0, H * 0.05)));
        P.drawPath(Path);
        F.setPixelSize(std::max(6, static_cast<int>(H * (T.size() > 3 ? 0.34 : 0.42))));
        P.setFont(F);
        P.setPen(Text);
        P.drawText(C, Qt::AlignCenter, T);
    }
    else
    {
        //A d-pad: a plus, with the pressed arm lit.
        const QRectF C(R.left(), R.top(), GlyphWidth(B, G, H), H);
        const double Arm = H * 0.3, Cx = C.center().x(), Cy = C.center().y();
        P.setPen(Qt::NoPen);
        P.setBrush(QColor(0x4a, 0x55, 0x61));
        P.drawRoundedRect(QRectF(Cx - Arm / 2, C.top() + H * 0.08, Arm, H * 0.84), 2, 2);
        P.drawRoundedRect(QRectF(Cx - H * 0.42, Cy - Arm / 2, H * 0.84, Arm), 2, 2);
        P.setBrush(Text);
        const auto Lit = [&](Pad::Button D) {
            switch (D)
            {
            case Pad::Button::Up:    P.drawRoundedRect(QRectF(Cx - Arm / 2, C.top() + H * 0.08, Arm, H * 0.3), 2, 2); break;
            case Pad::Button::Down:  P.drawRoundedRect(QRectF(Cx - Arm / 2, C.bottom() - H * 0.38, Arm, H * 0.3), 2, 2); break;
            case Pad::Button::Left:  P.drawRoundedRect(QRectF(Cx - H * 0.42, Cy - Arm / 2, H * 0.3, Arm), 2, 2); break;
            default:                 P.drawRoundedRect(QRectF(Cx + H * 0.12, Cy - Arm / 2, H * 0.3, Arm), 2, 2); break;
            }
        };
        if (B == Pad::Button::Left) { Lit(Pad::Button::Left); Lit(Pad::Button::Right); }   // "left and right"
        else if (Pad::IsDirection(B)) Lit(B);
        else
        {
            F.setPixelSize(std::max(6, static_cast<int>(H * 0.42)));
            P.setFont(F);
            P.drawText(C, Qt::AlignCenter, T);
        }
    }
    P.restore();
}

double DrawHints(QPainter &P, const QRectF &R, const QList<PadTarget::Hint> &Hints, Pad::Glyphs G, double S, bool Left)
{
    const double H = std::min(R.height(), 26.0 * S), Gap = 8 * S, Between = 26 * S;
    const QFont F = Font(15, S, QFont::DemiBold);
    const QFontMetricsF M(F);
    double Total = 0;
    for (int I = 0; I < Hints.size(); ++I)
        Total += GlyphWidth(Hints[I].B, G, H) + Gap + M.horizontalAdvance(Hints[I].Label) + (I + 1 < Hints.size() ? Between : 0);
    double X = Left ? R.left() : R.right() - Total;
    const double Y = R.center().y() - H / 2;
    P.save();
    P.setFont(F);
    for (const PadTarget::Hint &Hn : Hints)
    {
        DrawGlyph(P, QRectF(X, Y, H, H), Hn.B, G);
        X += GlyphWidth(Hn.B, G, H) + Gap;
        P.setPen(Text);
        const double W = M.horizontalAdvance(Hn.Label);
        P.drawText(QRectF(X, R.top(), W + 2, R.height()), Qt::AlignLeft | Qt::AlignVCenter, Hn.Label);
        X += W + Between;
    }
    P.restore();
    return Total;
}

void DrawCover(QPainter &P, const QRectF &R, const QPixmap &Cover, const QString &Title, double Radius)
{
    P.save();
    P.setRenderHint(QPainter::Antialiasing);
    P.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath Clip;
    Clip.addRoundedRect(R, Radius, Radius);
    P.setClipPath(Clip);
    if (!Cover.isNull())
    {
        //Fill the tile at the cover's own aspect: crop the excess, never stretch it.
        const QSizeF Src = Cover.deviceIndependentSize();
        const double K = std::max(R.width() / Src.width(), R.height() / Src.height());
        const QSizeF D(Src.width() * K, Src.height() * K);
        P.drawPixmap(QRectF(R.center().x() - D.width() / 2, R.center().y() - D.height() / 2, D.width(), D.height()), Cover,
                     QRectF(QPointF(0, 0), Cover.size()));
    }
    else
    {
        //No cover (yet): a tinted ground with the title, hued by the title so neighbours tell apart.
        const int Hue = static_cast<int>(qHash(Title) % 360);
        QLinearGradient Gr(R.topLeft(), R.bottomRight());
        Gr.setColorAt(0, QColor::fromHsv(Hue, 70, 70));
        Gr.setColorAt(1, QColor::fromHsv((Hue + 40) % 360, 80, 38));
        P.fillRect(R, Gr);
        QFont F = Font(1, 1, QFont::Bold);
        F.setPixelSize(std::max(9, static_cast<int>(R.width() * 0.11)));
        P.setFont(F);
        P.setPen(QColor(255, 255, 255, 225));
        P.drawText(R.adjusted(R.width() * 0.1, R.height() * 0.1, -R.width() * 0.1, -R.height() * 0.1),
                   Qt::AlignCenter | Qt::TextWordWrap, Title);
    }
    P.restore();
}

QString HostedStyleSheet(double S)
{
    const auto Px = [S](double V) { return QString::number(std::lround(V * S)); };
    return QString(
               "QWidget { font-size: %1px; }"
               "QPushButton, QToolButton { min-height: %2px; padding: %3px %4px; border-radius: %3px; }"
               "QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox { min-height: %2px; padding: 0 %3px; }"
               "QCheckBox, QRadioButton { spacing: %3px; min-height: %2px; }"
               "QCheckBox::indicator, QRadioButton::indicator { width: %5px; height: %5px; }"
               "QHeaderView::section { padding: %3px; }"
               "QListWidget#SettingsSidebar::item { padding: %3px %4px; }"
               "QTableView, QTreeView, QListView { outline: none; }"
               "QTableView::item, QTreeView::item, QListView::item { min-height: %2px; padding: 2px %3px; }"
               "QScrollBar:vertical { width: %6px; }"
               "QScrollBar:horizontal { height: %6px; }")
        .arg(Px(17), Px(38), Px(6), Px(14), Px(20), Px(14));
}

}
