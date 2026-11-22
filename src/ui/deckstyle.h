#ifndef DECKSTYLE_H
#define DECKSTYLE_H

#include "padinput.h"
#include "padnav.h"   // PadTarget::Hint

#include <QColor>
#include <QFont>
#include <QList>
#include <QRectF>
#include <QString>

class QPainter;
class QPixmap;
class QWidget;

// ---------------------------------------------------------------------------
// DeckStyle — controller mode's look: its colours, its type, one scale for every size (a Steam Deck's 1280×800 is
// 1.0; a TV's 1080p is 1.35), and the controller's own button glyphs.
// ---------------------------------------------------------------------------
namespace DeckStyle
{
// The scale for a screen of this height: 800 px is 1.0.
double ScaleFor(int Height);
// A font of Px pixels (at scale 1) scaled by S, in the controller mode's face.
QFont Font(double Px, double S, int Weight = QFont::Normal);

inline const QColor Ground(0x0e, 0x13, 0x19);      // the screen behind everything
inline const QColor GroundLow(0x08, 0x0b, 0x0f);   // its bottom edge
inline const QColor Panel(0x1a, 0x21, 0x29);       // a raised surface: a row, a button at rest
inline const QColor PanelHi(0x25, 0x2f, 0x3a);     // a raised surface under focus
inline const QColor Text(0xe8, 0xee, 0xf3);
inline const QColor Dim(0x8f, 0x9b, 0xa6);
inline const QColor Faint(0x5a, 0x66, 0x72);
inline const QColor Accent(0x4a, 0x90, 0xd9);
inline const QColor AccentHi(0x7d, 0xb8, 0xff);
inline const QColor Gold(0xff, 0xc8, 0x3d);
inline const QColor Good(0x5f, 0xb5, 0x5f);
inline const QColor Bad(0xd0, 0x6a, 0x60);

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
// The style sheet the desktop pages take inside controller mode: bigger text, taller controls, room between them.
QString HostedStyleSheet(double S);
}

#endif // DECKSTYLE_H
