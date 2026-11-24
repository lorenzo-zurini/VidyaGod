#include "matrixrain.h"

#include <QPainterPath>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {
//The film's glyphs: half-width katakana (drawn mirrored, as on its screens), the digits, and a few symbols. The katakana
//are drawn from strokes here, not a font: few machines have a Japanese font (a Steam Deck has none), and the rain's
//look must not depend on one. Each stroke is a polyline in a unit cell (x right, y down).
struct KanaStrokes { char16_t Code; std::vector<std::vector<QPointF>> Strokes; };
const std::vector<KanaStrokes> &KanaTable()
{
    static const std::vector<KanaStrokes> T = {
        { u'ｦ', { { {0.18,0.2}, {0.82,0.2}, {0.6,0.55}, {0.25,0.92} }, { {0.2,0.48}, {0.72,0.48} } } },
        { u'ｱ', { { {0.15,0.15}, {0.85,0.15}, {0.62,0.42} }, { {0.5,0.3}, {0.48,0.6}, {0.25,0.9} } } },
        { u'ｲ', { { {0.8,0.1}, {0.2,0.55} }, { {0.55,0.35}, {0.55,0.92} } } },
        { u'ｳ', { { {0.5,0.05}, {0.5,0.2} }, { {0.18,0.35}, {0.18,0.22}, {0.82,0.22}, {0.78,0.5}, {0.4,0.92} } } },
        { u'ｴ', { { {0.2,0.2}, {0.8,0.2} }, { {0.5,0.2}, {0.5,0.85} }, { {0.12,0.85}, {0.88,0.85} } } },
        { u'ｵ', { { {0.1,0.35}, {0.9,0.35} }, { {0.6,0.08}, {0.6,0.9}, {0.45,0.82} }, { {0.55,0.38}, {0.15,0.8} } } },
        { u'ｶ', { { {0.15,0.3}, {0.8,0.3}, {0.75,0.85}, {0.6,0.78} }, { {0.45,0.08}, {0.42,0.5}, {0.18,0.9} } } },
        { u'ｷ', { { {0.15,0.3}, {0.85,0.25} }, { {0.1,0.55}, {0.9,0.5} }, { {0.4,0.08}, {0.55,0.92} } } },
        { u'ｸ', { { {0.4,0.08}, {0.15,0.45} }, { {0.32,0.22}, {0.82,0.22}, {0.7,0.5}, {0.3,0.92} } } },
        { u'ｹ', { { {0.3,0.08}, {0.12,0.5} }, { {0.25,0.32}, {0.88,0.32} }, { {0.62,0.32}, {0.6,0.6}, {0.35,0.92} } } },
        { u'ｺ', { { {0.18,0.22}, {0.8,0.22}, {0.8,0.82} }, { {0.18,0.82}, {0.82,0.82} } } },
        { u'ｻ', { { {0.1,0.32}, {0.9,0.32} }, { {0.32,0.12}, {0.32,0.55} }, { {0.68,0.1}, {0.68,0.55}, {0.4,0.92} } } },
        { u'ｼ', { { {0.2,0.15}, {0.35,0.25} }, { {0.15,0.42}, {0.3,0.52} }, { {0.18,0.9}, {0.85,0.3} } } },
        { u'ｽ', { { {0.18,0.2}, {0.8,0.2}, {0.55,0.6}, {0.15,0.92} }, { {0.55,0.6}, {0.88,0.9} } } },
        { u'ｾ', { { {0.1,0.4}, {0.88,0.32}, {0.6,0.55} }, { {0.35,0.1}, {0.35,0.85}, {0.85,0.85} } } },
        { u'ｿ', { { {0.2,0.15}, {0.32,0.42} }, { {0.82,0.12}, {0.65,0.6}, {0.25,0.92} } } },
        { u'ﾀ', { { {0.4,0.08}, {0.15,0.45} }, { {0.32,0.22}, {0.82,0.22}, {0.65,0.6}, {0.28,0.92} }, { {0.3,0.48}, {0.65,0.6} } } },
        { u'ﾂ', { { {0.15,0.25}, {0.25,0.4} }, { {0.42,0.18}, {0.52,0.35} }, { {0.85,0.18}, {0.65,0.65}, {0.3,0.92} } } },
        { u'ﾃ', { { {0.25,0.15}, {0.75,0.15} }, { {0.1,0.4}, {0.9,0.4} }, { {0.5,0.4}, {0.48,0.65}, {0.25,0.92} } } },
        { u'ﾅ', { { {0.1,0.35}, {0.9,0.35} }, { {0.5,0.08}, {0.5,0.55}, {0.25,0.92} } } },
        { u'ﾆ', { { {0.2,0.25}, {0.8,0.25} }, { {0.1,0.8}, {0.9,0.8} } } },
        { u'ﾇ', { { {0.18,0.2}, {0.82,0.2}, {0.55,0.7}, {0.2,0.92} }, { {0.3,0.45}, {0.85,0.88} } } },
        { u'ﾈ', { { {0.45,0.05}, {0.45,0.15} }, { {0.15,0.22}, {0.82,0.22}, {0.2,0.7} }, { {0.5,0.48}, {0.5,0.92} }, { {0.62,0.55}, {0.85,0.75} } } },
        { u'ﾊ', { { {0.35,0.25}, {0.12,0.75} }, { {0.62,0.25}, {0.88,0.75} } } },
        { u'ﾋ', { { {0.25,0.1}, {0.25,0.85}, {0.85,0.85} }, { {0.25,0.45}, {0.8,0.3} } } },
        { u'ﾎ', { { {0.1,0.32}, {0.9,0.32} }, { {0.5,0.08}, {0.5,0.92} }, { {0.35,0.5}, {0.15,0.8} }, { {0.65,0.5}, {0.85,0.8} } } },
        { u'ﾏ', { { {0.12,0.22}, {0.85,0.22}, {0.45,0.7} }, { {0.32,0.5}, {0.62,0.9} } } },
        { u'ﾐ', { { {0.25,0.15}, {0.75,0.3} }, { {0.25,0.42}, {0.7,0.55} }, { {0.2,0.7}, {0.8,0.88} } } },
        { u'ﾑ', { { {0.4,0.08}, {0.15,0.85}, {0.85,0.75} }, { {0.65,0.5}, {0.85,0.92} } } },
        { u'ﾒ', { { {0.8,0.1}, {0.2,0.9} }, { {0.25,0.4}, {0.8,0.85} } } },
        { u'ﾓ', { { {0.2,0.2}, {0.8,0.2} }, { {0.1,0.48}, {0.9,0.48} }, { {0.48,0.2}, {0.48,0.85}, {0.85,0.85} } } },
        { u'ﾔ', { { {0.1,0.38}, {0.9,0.3}, {0.75,0.55} }, { {0.35,0.1}, {0.55,0.92} } } },
        { u'ﾕ', { { {0.25,0.3}, {0.75,0.3}, {0.75,0.75} }, { {0.1,0.78}, {0.9,0.78} } } },
        { u'ﾗ', { { {0.25,0.12}, {0.75,0.12} }, { {0.18,0.35}, {0.82,0.35}, {0.7,0.65}, {0.3,0.92} } } },
        { u'ﾘ', { { {0.3,0.12}, {0.3,0.6} }, { {0.72,0.1}, {0.72,0.55}, {0.4,0.92} } } },
        { u'ﾜ', { { {0.2,0.25}, {0.2,0.5} }, { {0.2,0.25}, {0.82,0.25}, {0.75,0.6}, {0.35,0.92} } } },
        { u'ﾝ', { { {0.2,0.2}, {0.38,0.32} }, { {0.18,0.9}, {0.85,0.3} } } },
    };
    return T;
}
const KanaStrokes *StrokesOf(QChar G)
{
    for (const KanaStrokes &K : KanaTable()) if (K.Code == G.unicode()) return &K;
    return nullptr;
}
QString Plain() { return QStringLiteral("012345789Z:.\"=*+-<>¦|"); }

//A trail glyph's colour at brightness T (0 the faintest, 1 just behind the head): the film's dark, mid and bright green.
QColor TrailColor(double T)
{
    const auto Mix = [](int A, int B, double X) { return int(std::lround(A + (B - A) * X)); };
    if (T < 0.5) { const double X = T / 0.5; return QColor(0, Mix(40, 143, X), Mix(0, 17, X)); }
    const double X = (T - 0.5) / 0.5;
    return QColor(Mix(0, 60, X * X), Mix(143, 255, X), Mix(17, 90, X));
}

//Box-blurs an image's alpha in place (two passes, radius R): the head's glow.
void BlurAlpha(QImage &Img, int R)
{
    const int W = Img.width(), H = Img.height();
    std::vector<int> A(size_t(W) * H), T(size_t(W) * H);
    for (int Y = 0; Y < H; ++Y)
        for (int X = 0; X < W; ++X) A[size_t(Y) * W + X] = qAlpha(Img.pixel(X, Y));
    for (int Pass = 0; Pass < 2; ++Pass)
    {
        for (int Y = 0; Y < H; ++Y)
            for (int X = 0; X < W; ++X)
            {
                int S = 0, N = 0;
                for (int D = -R; D <= R; ++D) if (X + D >= 0 && X + D < W) { S += A[size_t(Y) * W + X + D]; ++N; }
                T[size_t(Y) * W + X] = S / N;
            }
        for (int Y = 0; Y < H; ++Y)
            for (int X = 0; X < W; ++X)
            {
                int S = 0, N = 0;
                for (int D = -R; D <= R; ++D) if (Y + D >= 0 && Y + D < H) { S += T[size_t(Y + D) * W + X]; ++N; }
                A[size_t(Y) * W + X] = S / N;
            }
    }
    for (int Y = 0; Y < H; ++Y)
        for (int X = 0; X < W; ++X)
        {
            const QRgb P = Img.pixel(X, Y);
            Img.setPixel(X, Y, qRgba(qRed(P), qGreen(P), qBlue(P), A[size_t(Y) * W + X]));
        }
}
} // namespace

QString MatrixRain::glyphs()
{
    QString G;
    for (const KanaStrokes &K : KanaTable()) G += QChar(K.Code);
    return G + Plain();
}

QImage MatrixRain::glyph(QChar G, int CellW, int CellH, QColor Color, int Pad)
{
    QImage Img(CellW + 2 * Pad, CellH + 2 * Pad, QImage::Format_ARGB32_Premultiplied);
    Img.fill(Qt::transparent);
    QPainter P(&Img);
    P.setRenderHint(QPainter::Antialiasing);
    P.translate(Pad + CellW, Pad);   // mirrored: the film's glyphs read right to left
    P.scale(-1, 1);
    if (const KanaStrokes *K = StrokesOf(G))
    {   // inset a little, as a font's glyph sits in its cell; the weight a monospace digit has at this size
        const QRectF Box(CellW * 0.14, CellH * 0.14, CellW * 0.72, CellH * 0.72);
        P.setPen(QPen(Color, std::max(1.2, CellH * 0.085), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        for (const auto &Stroke : K->Strokes)
        {
            QPainterPath Path;
            for (size_t I = 0; I < Stroke.size(); ++I)
            {
                const QPointF Pt(Box.left() + Stroke[I].x() * Box.width(), Box.top() + Stroke[I].y() * Box.height());
                if (I == 0) Path.moveTo(Pt); else Path.lineTo(Pt);
            }
            P.drawPath(Path);
        }
        return Img;
    }
    QFont F(QStringLiteral("monospace"));
    F.setStyleHint(QFont::Monospace);
    F.setPixelSize(std::max(8, int(std::lround(CellH / 1.08))));
    F.setStyleStrategy(QFont::PreferAntialias);
    P.setFont(F);
    P.setPen(Color);
    P.drawText(QRect(0, 0, CellW, CellH), Qt::AlignCenter, QString(G));
    return Img;
}

MatrixRain::MatrixRain(QWidget *Parent, unsigned Seed) : QWidget(Parent), Rng(Seed)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
    setObjectName(QStringLiteral("matrixRain"));
    Timer.setInterval(1000 / FramesPerSecond);
    connect(&Timer, &QTimer::timeout, this, [this] {
        const double Dt = std::min(0.1, Clock.restart() / 1000.0);   // a stalled frame is not a jump
        advance(Dt);
        update();
    });
}

void MatrixRain::start()
{
    if (Atlas.empty() || Cols.empty()) layoutColumns();
    Clock.start();
    Timer.start();
    show();
    raise();
}

void MatrixRain::stop()
{
    Timer.stop();
    hide();
}

void MatrixRain::resizeEvent(QResizeEvent *)
{
    layoutColumns();
}

void MatrixRain::buildAtlas()
{
    Glyphs = glyphs();
    const int Px = std::max(12, height() / 40);
    CellH = std::max(8, int(std::lround(Px * 1.08)));
    CellW = std::max(6, int(std::lround(Px * 0.66)));
    Atlas.assign(size_t(Glyphs.size()), {});
    Heads.assign(size_t(Glyphs.size()), {});
    const int Pad = std::max(2, CellH / 4);
    for (int I = 0; I < Glyphs.size(); ++I)
    {
        for (int L = 0; L < Levels; ++L)
            Atlas[size_t(I)].push_back(QPixmap::fromImage(glyph(Glyphs[I], CellW, CellH, TrailColor(double(L + 1) / Levels), 0)));
        QImage Glow = glyph(Glyphs[I], CellW, CellH, QColor(90, 255, 120), Pad).convertToFormat(QImage::Format_ARGB32);
        BlurAlpha(Glow, std::max(1, Pad / 2));
        QPainter P(&Glow);
        P.drawImage(0, 0, glyph(Glyphs[I], CellW, CellH, QColor(225, 255, 230), Pad));   // the crisp head over its glow
        P.end();
        Heads[size_t(I)] = QPixmap::fromImage(Glow);
    }
}

void MatrixRain::layoutColumns()
{
    if (width() <= 0 || height() <= 0) return;
    buildAtlas();
    Rows = std::max(1, height() / CellH + 1);
    Cols.assign(size_t(std::max(1, width() / CellW + 1)), {});
    for (Column &C : Cols)
    {
        C.Glyph.resize(size_t(Rows));
        for (int &G : C.Glyph) G = randomGlyph();
        std::uniform_real_distribution<double> Chance(0, 1);
        if (Chance(Rng) < 0.7) C.Drops.push_back(newDrop(/*Anywhere=*/true));   // a screen already raining
        C.Rest = std::uniform_real_distribution<double>(0, 3)(Rng);
    }
}

MatrixRain::Drop MatrixRain::newDrop(bool Anywhere)
{
    Drop D;
    D.Speed = std::uniform_real_distribution<double>(7, 19)(Rng);
    D.Length = std::uniform_int_distribution<int>(std::max(4, Rows / 5), std::max(6, Rows * 3 / 4))(Rng);
    D.Y = Anywhere ? std::uniform_real_distribution<double>(0, Rows + D.Length)(Rng) : -1;
    return D;
}

int MatrixRain::randomGlyph()
{
    return std::uniform_int_distribution<int>(0, std::max(0, int(Glyphs.size()) - 1))(Rng);
}

void MatrixRain::advance(double Seconds)
{
    std::uniform_real_distribution<double> Chance(0, 1);
    for (Column &C : Cols)
    {
        for (Drop &D : C.Drops)
        {
            const int Was = int(std::floor(D.Y));
            D.Y += D.Speed * Seconds;
            for (int R = Was + 1; R <= int(std::floor(D.Y)); ++R)   // the rows the head reached: new glyphs there
                if (R >= 0 && R < Rows) C.Glyph[size_t(R)] = randomGlyph();
            const int Head = int(std::floor(D.Y));
            if (Head >= 0 && Head < Rows && Chance(Rng) < 0.5) C.Glyph[size_t(Head)] = randomGlyph();   // the head flickers
        }
        C.Drops.erase(std::remove_if(C.Drops.begin(), C.Drops.end(), [&](const Drop &D) { return D.Y - D.Length > Rows; }),
                      C.Drops.end());
        C.Rest -= Seconds;
        if (C.Rest <= 0 && C.Drops.size() < 2)
        {
            C.Drops.push_back(newDrop(false));
            const Drop &D = C.Drops.back();
            C.Rest = (D.Length + std::uniform_real_distribution<double>(2, Rows)(Rng)) / D.Speed;
        }
        if (Chance(Rng) < Seconds * 2.0 && Rows > 0)   // a glyph left behind changes, now and then
            C.Glyph[size_t(std::uniform_int_distribution<int>(0, Rows - 1)(Rng))] = randomGlyph();
    }
}

int MatrixRain::litCells() const
{
    int N = 0;
    for (const Column &C : Cols)
    {
        std::vector<bool> Lit(size_t(Rows), false);
        for (const Drop &D : C.Drops)
            for (int K = 0, Head = int(std::floor(D.Y)); K <= D.Length; ++K)
                if (Head - K >= 0 && Head - K < Rows) Lit[size_t(Head - K)] = true;
        N += int(std::count(Lit.begin(), Lit.end(), true));
    }
    return N;
}

void MatrixRain::paintEvent(QPaintEvent *)
{
    QPainter P(this);
    P.fillRect(rect(), Qt::black);
    if (Atlas.empty()) return;
    const int Pad = Heads.empty() ? 0 : (Heads.front().width() - CellW) / 2;
    for (size_t X = 0; X < Cols.size(); ++X)
    {
        const Column &C = Cols[X];
        for (const Drop &D : C.Drops)
        {
            const int Head = int(std::floor(D.Y));
            for (int K = D.Length; K >= 0; --K)   // the faint end first: the head is drawn last, over its glow's reach
            {
                const int R = Head - K;
                if (R < 0 || R >= Rows) continue;
                const int G = C.Glyph[size_t(R)];
                if (K == 0) { P.drawPixmap(int(X) * CellW - Pad, R * CellH - Pad, Heads[size_t(G)]); continue; }
                const double T = 1.0 - double(K - 1) / D.Length;   // 1 just behind the head, towards 0 at the tail
                const int L = std::clamp(int(T * Levels) - 1, 0, Levels - 1);
                P.drawPixmap(int(X) * CellW, R * CellH, Atlas[size_t(G)][size_t(L)]);
            }
        }
    }
}
