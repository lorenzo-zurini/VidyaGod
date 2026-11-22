#include "deckstyle.h"

#include <QFontDatabase>
#include <QRegularExpression>
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

QFont Display(double Px, double S, bool Italic)
{
    //The faces ship inside the app (a Deck has neither): Noto Serif Display, Black and Bold Italic, cut to Latin.
    static const QString Black = [] {
        const QStringList F = QFontDatabase::applicationFontFamilies(QFontDatabase::addApplicationFont(":/fonts/NotoSerifDisplay-Black.ttf"));
        return F.isEmpty() ? QString() : F.first();
    }();
    static const QString BoldItalic = [] {
        const QStringList F = QFontDatabase::applicationFontFamilies(QFontDatabase::addApplicationFont(":/fonts/NotoSerifDisplay-BoldItalic.ttf"));
        return F.isEmpty() ? QString() : F.first();
    }();
    QFont F;
    const QString &Family = Italic ? BoldItalic : Black;
    if (!Family.isEmpty()) F.setFamilies({ Family, QStringLiteral("Noto Serif"), QStringLiteral("serif") });
    F.setPixelSize(std::max(8, static_cast<int>(std::lround(Px * S))));
    F.setWeight(Italic ? QFont::Bold : QFont::Black);
    F.setItalic(Italic);
    F.setHintingPreference(QFont::PreferNoHinting);
    return F;
}

QPainterPath Capsule(const QRectF &R)
{
    QPainterPath Path;
    const double Rad = std::min(R.width(), R.height()) / 2;
    Path.addRoundedRect(R, Rad, Rad);
    return Path;
}

void DrawLift(QPainter &P, const QPainterPath &Shape, double S, double Amount)
{
    if (Amount <= 0) return;
    const double O = 5 * S * Amount;
    QColor C = Magenta;
    C.setAlphaF(std::clamp(Amount, 0.0, 1.0));
    P.save();
    P.setRenderHint(QPainter::Antialiasing);
    P.fillPath(Shape.translated(O, O), C);
    P.restore();
}

void DrawRing(QPainter &P, const QPainterPath &Shape, double S, double Amount)
{
    if (Amount <= 0) return;
    QColor C = Lime;
    C.setAlphaF(std::clamp(Amount, 0.0, 1.0));
    P.save();
    P.setRenderHint(QPainter::Antialiasing);
    P.setPen(QPen(C, 3 * S));
    P.setBrush(Qt::NoBrush);
    P.drawPath(Shape);
    P.restore();
}

void DrawButton(QPainter &P, const QRectF &R, const QString &Label, bool Focused, bool Primary, double S)
{
    const QPainterPath Path = Capsule(R);
    P.save();
    P.setRenderHint(QPainter::Antialiasing);
    if (Focused) DrawLift(P, Path, S);
    P.fillPath(Path, Focused ? Lime : Panel);
    if (Primary && !Focused)
    {
        P.setPen(QPen(Lime, 2 * S));
        P.setBrush(Qt::NoBrush);
        P.drawPath(Capsule(R.adjusted(S, S, -S, -S)));
    }
    P.setFont(Font(Primary ? 19 : 17, S, QFont::Bold));
    P.setPen(Focused ? Ink : Primary ? LimeHi : Text);
    P.drawText(R, Qt::AlignCenter, Label);
    P.restore();
}

double ButtonWidth(const QString &Label, bool Primary, double S)
{
    return std::max((Primary ? 190 : 120) * S, QFontMetricsF(Font(Primary ? 19 : 17, S, QFont::Bold)).horizontalAdvance(Label) + 52 * S);
}

void DrawLabel(QPainter &P, const QRectF &R, const QString &Label, bool On, double S)
{
    P.save();
    P.setFont(Display(19, S, true));
    P.setPen(On ? MagentaHi : Dim);
    P.drawText(R, Qt::AlignLeft | Qt::AlignVCenter, Label);
    P.restore();
}

QImage Duotone(const QImage &Image)
{
    if (Image.isNull()) return {};
    //Luminance from the plum ground (its darks) to the mark's magenta (its lights).
    QImage Out = Image.convertToFormat(QImage::Format_RGB32);
    const QColor Lo(0x12, 0x0a, 0x16), Hi(0xa8, 0x3a, 0xb4);
    for (int Y = 0; Y < Out.height(); ++Y)
    {
        auto *Line = reinterpret_cast<QRgb *>(Out.scanLine(Y));
        for (int X = 0; X < Out.width(); ++X)
        {
            const double L = (0.2126 * qRed(Line[X]) + 0.7152 * qGreen(Line[X]) + 0.0722 * qBlue(Line[X])) / 255.0;
            const auto Mix = [L](int A, int B) { return static_cast<int>(std::lround(A + (B - A) * L)); };
            Line[X] = qRgb(Mix(Lo.red(), Hi.red()), Mix(Lo.green(), Hi.green()), Mix(Lo.blue(), Hi.blue()));
        }
    }
    return Out;
}

void DrawSplitRule(QPainter &P, const QRectF &R)
{
    P.fillRect(QRectF(R.left(), R.top(), R.width() / 2, R.height()), Lime);
    P.fillRect(QRectF(R.left() + R.width() / 2, R.top(), R.width() / 2, R.height()), Magenta);
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
        P.setBrush(Panel);
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
        P.fillPath(Path, Panel);
        P.setPen(QPen(Faint, std::max(1.0, H * 0.05)));
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
        P.setBrush(Faint);
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
        //No cover (yet): the title in the display face on a ground of the mark's two tones — plum or a deep green,
        //by the title, so neighbours tell apart.
        const uint H = qHash(Title);
        const bool Green = H % 3 == 0;
        const int Hue = Green ? 120 + static_cast<int>(H % 30) : 280 + static_cast<int>(H % 36);
        QLinearGradient Gr(R.topLeft(), R.bottomRight());
        Gr.setColorAt(0, QColor::fromHsv(Hue, Green ? 120 : 110, Green ? 70 : 82));
        Gr.setColorAt(1, QColor::fromHsv((Hue + 18) % 360, 130, 34));
        P.fillRect(R, Gr);
        QFont F = Display(1, 1);
        F.setPixelSize(std::max(9, static_cast<int>(R.width() * 0.12)));
        P.setFont(F);
        P.setPen(QColor(255, 255, 255, 230));
        P.drawText(R.adjusted(R.width() * 0.1, R.height() * 0.1, -R.width() * 0.1, -R.height() * 0.1),
                   Qt::AlignCenter | Qt::TextWordWrap, Title);
    }
    P.restore();
}

QString HostedStyleSheet(double S)
{
    const auto Px = [S](double V) { return QString::number(std::lround(V * S)); };
    QString Sheet = QStringLiteral(
        "QWidget { font-size: @f17px; }"
        "QPushButton, QToolButton { min-height: @f38px; padding: @f6px @f18px; border-radius: @f20px; background: @Panel;"
        " border: 1px solid @PanelHi; color: @Text; }"
        "QPushButton:disabled, QToolButton:disabled { color: @Faint; background: @GroundLow; border-color: @Panel; }"
        "QPushButton:checked, QToolButton:checked { background: @Lime; color: @Ink; border-color: @Lime; }"
        "QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox { min-height: @f38px; padding: 0 @f10px; background: @GroundLow;"
        " border: 1px solid @PanelHi; border-radius: @f8px; color: @Text; selection-background-color: @Lime; selection-color: @Ink; }"
        "QComboBox QAbstractItemView { background: @Panel; color: @Text; selection-background-color: @Lime; selection-color: @Ink; }"
        "QCheckBox, QRadioButton { spacing: @f8px; min-height: @f38px; }"
        "QCheckBox::indicator, QRadioButton::indicator, QTableView::indicator { width: @f20px; height: @f20px; }"
        "QHeaderView { background: @Panel; border: none; }"
        "QHeaderView::section { padding: @f6px; background: @Panel; color: @Dim; border: none; border-right: 1px solid @Ground; }"
        "QListWidget#SettingsSidebar { background: @GroundLow; }"
        "QListWidget#SettingsSidebar::item { padding: @f6px @f14px; border-radius: @f18px; color: @Dim; }"
        "QListWidget#SettingsSidebar::item:selected { background: @Lime; color: @Ink; }"
        "QTableView, QTreeView, QListView { outline: none; background: @GroundLow; alternate-background-color: @Panel;"
        " gridline-color: @Panel; border: 1px solid @Panel; }"
        "QTableView::item, QTreeView::item, QListView::item { min-height: @f38px; padding: 2px @f6px; }"
        "QTableView::item:selected, QTreeView::item:selected, QListView::item:selected { background: @PanelHi; color: @Text; }"
        "QCheckBox::indicator, QTableView::indicator { background: @GroundLow; border: 2px solid @Faint; border-radius: @f5px; }"
        "QCheckBox::indicator:checked, QTableView::indicator:checked { background: @Lime; border-color: @Lime; }"
        "QCheckBox::indicator:disabled { border-color: @Panel; }"
        "QListWidget#IpfsFilterList { border-radius: @f8px; font-size: @f15px; }"
        "QListWidget#IpfsFilterList::item:selected { background: @PanelHi; color: @Text; }"
        "QTableWidget#IpfsStatusTable { background: @GroundLow; border: 1px solid @Panel; border-radius: @f8px; }"
        "QTableWidget#IpfsStatusTable QHeaderView::section { background: @GroundLow; color: @Dim; border: none; padding: @f2px @f4px;"
        " font-size: @f12px; }"
        "QTableWidget#IpfsStatusTable::item { min-height: 0px; padding: 0px; }"
        "QMenu { background: @Panel; color: @Text; border: 1px solid @PanelHi; border-radius: @f14px; padding: @f8px; font-size: @f18px; }"
        "QMenu::item { padding: @f9px @f28px; border-radius: @f18px; }"
        "QMenu::item:selected { background: @Lime; color: @Ink; }"
        "QMenu::item:disabled { color: @Faint; }"
        "QMenu::separator { height: 1px; background: @PanelHi; margin: @f6px @f12px; }"
        "QScrollBar:vertical { width: @f14px; }"
        "QScrollBar:horizontal { height: @f14px; }");
    //The sizes (@fN: N px at scale 1), then the colours, by name.
    static const QRegularExpression Size("@f(\\d+)");
    for (QRegularExpressionMatch M = Size.match(Sheet); M.hasMatch(); M = Size.match(Sheet))
        Sheet.replace(M.capturedStart(), M.capturedLength(), Px(M.captured(1).toDouble()));
    const std::pair<const char *, QColor> Named[] = { { "@GroundLow", GroundLow }, { "@Ground", Ground }, { "@PanelHi", PanelHi },
        { "@Panel", Panel }, { "@Text", Text }, { "@Dim", Dim }, { "@Faint", Faint }, { "@Lime", Lime }, { "@Ink", Ink } };
    for (const auto &[Name, C] : Named) Sheet.replace(QLatin1String(Name), C.name());
    return Sheet;
}

}
