#ifndef DECKGRID_H
#define DECKGRID_H

#include "padnav.h"   // PadTarget

#include <QList>
#include <QRectF>
#include <QString>
#include <QVector>
#include <QWidget>

class LibraryGameCard;
class QTimer;

// ---------------------------------------------------------------------------
// DeckGrid — controller mode's shelf of covers: a grid (with section headers) or one strip, the focused cover lifted
// and lit, the view gliding to keep it in sight. Directions move the focus; A (or a tap) activates; a direction with
// nowhere to go is left to the screen around it (it moves the focus off the grid). The cards are the caller's.
// ---------------------------------------------------------------------------
class DeckGrid : public QWidget, public PadTarget
{
    Q_OBJECT
public:
    explicit DeckGrid(QWidget *Parent = nullptr);

    struct Section { QString Name; int First = 0, Last = 0; };
    // The cards shown (not owned; they must outlive the next setCards), with optional section headers.
    void setCards(const QList<LibraryGameCard *> &Cards, const QVector<Section> &Sections = {});
    // One horizontal strip instead of a grid.
    void setStrip(bool On);
    // Whether it shows its focus: a screen with several strips lights only the one the controller is in.
    void setLit(bool On);
    bool lit() const { return Lit; }
    void setEmptyText(const QString &Text);

    int count() const { return static_cast<int>(Cards.size()); }
    int currentIndex() const { return Current; }
    LibraryGameCard *current() const;
    void setCurrentIndex(int I);
    // Focus the card of this game (its GameKey) when it is shown: a rebuild keeps the focus where it was.
    bool setCurrentKey(const QString &Key);
    QSizeF tileSize() const { return Tile; }
    QRectF tileRect(int I) const;   // where card I is drawn (widget coordinates, scrolled)
    void repaintCards() { update(); }

    bool padPressed(Pad::Button B) override;
    bool padScrolled(double Dx, double Dy) override;
    nlohmann::ordered_json padState() const override;

signals:
    void activated(LibraryGameCard *Card);
    void currentChanged(LibraryGameCard *Card);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *E) override;
    void mouseMoveEvent(QMouseEvent *E) override;
    void mouseReleaseEvent(QMouseEvent *E) override;
    void wheelEvent(QWheelEvent *E) override;

private:
    void relayout();
    void reveal();      // scroll so the current card is in sight
    void animate();     // one frame of the glide and the lift
    int hit(const QPointF &At) const;
    double maxScroll() const;
    int pageFrom(int From, int Dir) const;   // the tile a screenful on from From (Dir ±1), or -1

    QList<LibraryGameCard *> Cards;
    QVector<Section> Sections;
    QVector<QRectF> Rects;                 // unscrolled
    QVector<QPair<QString, QRectF>> Headers;
    QSizeF Tile;
    double Content = 0;                    // the laid-out length along the scroll axis
    double Scroll = 0, Target = 0;         // along the scroll axis
    double Lift = 1;                       // the current card's lift (0 → 1 as focus arrives)
    int Current = -1;
    bool Strip = false, Lit = true;
    QString Empty;
    QTimer *Anim = nullptr;
    QPointF PressAt;
    double PressScroll = 0;
    bool Pressed = false, Dragging = false;
};

#endif // DECKGRID_H
