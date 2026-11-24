#ifndef MATRIXRAIN_H
#define MATRIXRAIN_H

#include <QElapsedTimer>
#include <QImage>
#include <QPixmap>
#include <QTimer>
#include <QWidget>

#include <random>
#include <vector>

// MatrixRain — the screensaver's digital rain, as the film has it: columns of mirrored half-width katakana, digits and
// a few symbols falling at their own speeds; each stream's head glyph near-white and glowing, the trail behind it green
// and fading out; the glyphs a stream leaves behind still flicker to others now and then. Black between.
//
// Cheap to draw: every glyph is rendered once per brightness into an atlas, and a frame is a black fill plus one
// pixmap blit per lit cell, at most FramesPerSecond times a second (an OLED's whole screen moves: nothing stays lit).
// advance() steps the rain by a given time without a clock (a test drives it).
class MatrixRain : public QWidget
{
    Q_OBJECT
public:
    explicit MatrixRain(QWidget *Parent = nullptr, unsigned Seed = std::random_device{}());

    static constexpr int FramesPerSecond = 30;
    static constexpr int Levels = 12;          // brightness steps of a trail glyph; the head is drawn on its own

    void start();                              // animate (and show)
    void stop();                               // stop animating (and hide)
    bool running() const { return Timer.isActive(); }
    void advance(double Seconds);              // the rain, Seconds later
    int columns() const { return int(Cols.size()); }
    int rows() const { return Rows; }
    int litCells() const;                      // cells drawn now (a test: the rain falls, and fades)
    static QString glyphs();                   // the glyphs it draws: half-width katakana, digits, a few symbols
    // One glyph, mirrored, in Color, in a CellW×CellH cell with Pad around it: the katakana from their strokes (no font
    // needed: few machines have a Japanese one), the rest from a monospace font.
    static QImage glyph(QChar G, int CellW, int CellH, QColor Color, int Pad = 0);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;

private:
    struct Drop { double Y = 0; double Speed = 0; int Length = 0; };   // Y: the head's row (fractional), cells/s
    struct Column { std::vector<int> Glyph; std::vector<Drop> Drops; double Rest = 0; };
    void buildAtlas();
    void layoutColumns();
    Drop newDrop(bool Anywhere);
    int randomGlyph();

    QTimer Timer;
    QElapsedTimer Clock;
    std::mt19937 Rng;
    QString Glyphs;
    int CellW = 0, CellH = 0, Rows = 0;
    std::vector<Column> Cols;
    std::vector<std::vector<QPixmap>> Atlas;   // [glyph][level], level Levels-1 the brightest trail glyph
    std::vector<QPixmap> Heads;                // [glyph]: the head, near-white with its glow
};

#endif // MATRIXRAIN_H
