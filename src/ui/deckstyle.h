#ifndef DECKSTYLE_H
#define DECKSTYLE_H

#include "padinput.h"
#include "padnav.h"   // PadTarget::Hint

#include <QColor>
#include <QFont>
#include <QImage>
#include <QPainterPath>
#include <QList>
#include <QRectF>
#include <QString>

class QPainter;
class QPixmap;
class QWidget;

// ---------------------------------------------------------------------------
// DeckStyle — controller mode's look, taken from the app's own mark (the capsule: lime green and magenta, its "VG" in
// a heavy serif): a plum ground, lime for what the controller is on and for the action, magenta for its shadow;
// titles in a display serif, buttons and tabs shaped as capsules. One scale for every size (a Steam Deck's 1280×800
// is 1.0; a TV's 1080p is 1.35), and the controller's own button glyphs.
// ---------------------------------------------------------------------------
namespace DeckStyle
{
// The scale for a screen of this height: 800 px is 1.0.
double ScaleFor(int Height);
// A font of Px pixels (at scale 1) scaled by S, in the controller mode's face.
QFont Font(double Px, double S, int Weight = QFont::Normal);

// A display face (the serif the app's mark is set in) of Px pixels at scale 1, scaled by S: Black for titles, Italic
// (bold italic) for the labels over a row.
QFont Display(double Px, double S, bool Italic = false);

inline const QColor Ground(0x15, 0x0d, 0x19);      // the screen behind everything: plum, nearly black
inline const QColor GroundLow(0x0c, 0x07, 0x0e);   // its bottom edge, the bars
inline const QColor Panel(0x26, 0x1b, 0x2d);       // a raised surface: a row, a button at rest
inline const QColor PanelHi(0x36, 0x27, 0x40);     // a raised surface under focus, a selection
inline const QColor Text(0xf6, 0xee, 0xf7);
inline const QColor Dim(0xab, 0x99, 0xb2);
inline const QColor Faint(0x6e, 0x5c, 0x76);
inline const QColor Lime(0x3c, 0xe0, 0x5c);       // what the controller is on; the action (the mark's green)
inline const QColor LimeHi(0x8f, 0xf7, 0xa6);     // progress, a lit word
inline const QColor Ink(0x16, 0x09, 0x1b);        // text on lime
inline const QColor Magenta(0xc4, 0x12, 0xd2);    // the focus's shadow, a count (the mark's magenta)
inline const QColor MagentaHi(0xec, 0x6a, 0xf2);  // the label over a row
inline const QColor Gold(0xff, 0xc8, 0x3d);
inline const QColor Good(0x4f, 0xd1, 0x6c);
inline const QColor Bad(0xe0, 0x6a, 0x6a);

// Focus, the one way controller mode shows it: the shape lifted off a solid magenta shadow (drawn first, under it)
// and ringed in lime (drawn last, over it). Amount (0..1) eases it in.
void DrawLift(QPainter &P, const QPainterPath &Shape, double S, double Amount = 1);
void DrawRing(QPainter &P, const QPainterPath &Shape, double S, double Amount = 1);
// A capsule: a rounded rect whose ends are half circles.
QPainterPath Capsule(const QRectF &R);
// A button: a capsule, lime when focused; Primary (the one that plays) is ringed in lime at rest.
void DrawButton(QPainter &P, const QRectF &R, const QString &Label, bool Focused, bool Primary, double S);
double ButtonWidth(const QString &Label, bool Primary, double S);
// The label over a row ("Continue playing"): the display italic, magenta when it is the row the controller is in.
void DrawLabel(QPainter &P, const QRectF &R, const QString &Label, bool On, double S);
// A picture recoloured in the mark's two tones (its darks plum, its lights magenta): what a page shows behind itself.
QImage Duotone(const QImage &Image);
// The mark's two halves as a rule: lime on the left, magenta on the right.
void DrawSplitRule(QPainter &P, const QRectF &R);

// A controller button as the controller labels it, drawn in R (a face button round, a shoulder or trigger a pill,
// a direction a d-pad): Xbox's coloured letters, PlayStation's shapes, Nintendo's letters (A is the bottom button's
// meaning: Nintendo labels it B), the Steam Deck's L1/R1.
void DrawGlyph(QPainter &P, const QRectF &R, Pad::Button B, Pad::Glyphs G);
// How wide DrawGlyph draws B at height H (a pill is wider than a round button).
double GlyphWidth(Pad::Button B, Pad::Glyphs G, double H);
// The glyph's text, for a label or the CLI ("A", "✕", "L1", "☰").
QString GlyphText(Pad::Button B, Pad::Glyphs G);
// A row of hints — glyph, then what it does — right-aligned in R (left-aligned when Left). Returns its width.
double DrawHints(QPainter &P, const QRectF &R, const QList<PadTarget::Hint> &Hints, Pad::Glyphs G, double S, bool Left = false);

// The cover drawn to fill R (cropped to its aspect, never stretched), with rounded corners; the placeholder (the
// title on a tinted ground) when the cover is null.
void DrawCover(QPainter &P, const QRectF &R, const QPixmap &Cover, const QString &Title, double Radius);
// The style sheet the desktop pages take inside controller mode: bigger text, taller controls, room between them, and
// its colours. Colours go in the sheet, not a palette: under a style sheet Qt hands no palette down to the children.
QString HostedStyleSheet(double S);
}

#endif // DECKSTYLE_H
