#ifndef PADNAV_H
#define PADNAV_H

#include "padinput.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QVector>
#include <QWidget>

#include "nlohmann/json.hpp"

// ---------------------------------------------------------------------------
// A widget that takes controller presses itself — a controller-mode screen, the on-screen keyboard. A press goes to
// the focused widget's PadTargets, innermost first, and only then to generic navigation; so a screen handles what it
// knows (its grid, its own buttons) and leaves the rest (a dialog it opened, a desktop page it hosts) to that.
// ---------------------------------------------------------------------------
class PadTarget
{
public:
    virtual ~PadTarget() = default;
    virtual bool padPressed(Pad::Button B) = 0;                    // true: handled
    struct Hint { Pad::Button B; QString Label; };
    virtual QList<Hint> padHints() const { return {}; }           // what its buttons do now (the hint bar)
    virtual bool padScrolled(double /*Dx*/, double /*Dy*/) { return false; }
    // Its own state for `controller state` (the focused tile, the open page …); null when it has none to say.
    virtual nlohmann::ordered_json padState() const { return nullptr; }
};

// ---------------------------------------------------------------------------
// PadNav — controller navigation over ANY window, by what its widgets are: a direction focuses the nearest control
// that way (by geometry, as the eye sees the layout), A presses, toggles, opens or types, B backs out of a popup or a
// dialog, X opens a context menu, the shoulders turn a tab widget's pages, the right stick scrolls. Every dialog and
// settings page the desktop app has is controller-drivable without a line of its own.
// ---------------------------------------------------------------------------
namespace PadNav
{
// Of Candidates, the one focus moves to from From in direction Dir (all rectangles in one space): its index, or -1
// when nothing lies that way. Pure geometry.
int Nearest(const QRect &From, Pad::Button Dir, const QVector<QRect> &Candidates);
// The controls of Window that navigation may focus: shown, enabled, taking focus, and not the inner part of a
// control (a combo box's or a spin box's line edit, an item view's open editor). A widget with the dynamic property
// "padSkip" set is left out, with its children.
QList<QWidget *> Focusable(QWidget *Window);
// Where W is drawn, in its window's coordinates: an item view's current item rather than the whole view.
QRect FocusRect(QWidget *W);
// Focus the control nearest Dir from the focused one (the first control when none is). False: nothing lies that way.
bool Move(QWidget *Window, Pad::Button Dir);
// A on W: press a button, toggle a check box, open a combo box, type into a field (the on-screen keyboard), open an
// item view's current item (or toggle its check). False when W takes no A.
bool Activate(QWidget *W);
// B in Window: a popup closes, an item editor is cancelled, a dialog is rejected (as Escape would), another window
// that is not the main one closes. False when there is nothing to back out of.
bool Back(QWidget *Window);
// What generic navigation does with each button on W now, for the hint bar.
QList<PadTarget::Hint> Hints(QWidget *W);
// W as `controller state` says it: its kind, its name and text, and (an item view) its current item.
nlohmann::ordered_json Describe(QWidget *W);
}

// ---------------------------------------------------------------------------
// PadRouter — takes PadInput's presses to the UI: an open overlay (the on-screen keyboard) first, then an open popup
// (a menu, a combo box's list), then the focused widget's PadTargets, then PadNav. It also keeps the focus ring, which
// shows once a controller is used and goes when the mouse or a touch is.
// ---------------------------------------------------------------------------
class FocusRing;

class PadRouter : public QObject
{
    Q_OBJECT
public:
    static PadRouter &instance();
    // Connect to PadInput and watch the app (once; idempotent).
    void install();
    // One press, delivered as a controller's is: what took it ("" when nothing did).
    QString route(Pad::Button B);
    void scroll(double Dx, double Dy);
    // The window presses go to now: the open popup, else the modal dialog, else the active window, else the main
    // window (offscreen, nothing is "active").
    static QWidget *targetWindow();
    // An overlay takes every press while open (the on-screen keyboard); the last pushed first.
    void pushOverlay(QWidget *Overlay);
    void popOverlay(QWidget *Overlay);
    QWidget *overlay() const;
    // The hint bar's buttons now: the overlay's, else the focus chain's PadTargets', else generic navigation's.
    QList<PadTarget::Hint> hints() const;
    // Whether the controller was the last thing used (the focus ring shows), and setting it (tests, the CLI).
    bool padMode() const { return PadMode; }
    void setPadMode(bool On);
    nlohmann::ordered_json state() const;   // `controller state`: the target window, focus, overlay, hints, ring
    QRect ringRect() const;                 // where the focus ring is drawn (window coordinates; empty when hidden)

signals:
    void hintsChanged();
    void routed(QString What);

protected:
    bool eventFilter(QObject *Watched, QEvent *Event) override;

private:
    PadRouter() = default;
    bool Installed = false, PadMode = false;
    QList<QPointer<QWidget>> Overlays;
    QPointer<FocusRing> Ring;
    double ScrollX = 0, ScrollY = 0;   // the fractions of a pixel the right stick owes
    class QTimer *Tick = nullptr;      // the ring's frames: running only while something may move it
    qint64 TickUntil = 0;
    void kickRing();                   // frames for the next while (a press, a focus change, a scroll)
    void updateRing();
};

#endif // PADNAV_H
