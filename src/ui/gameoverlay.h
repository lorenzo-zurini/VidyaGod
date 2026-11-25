#pragma once

#include "padnav.h"

#include <QWidget>

#include <functional>

#include <utility>
#include <vector>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QTimer;

// The X11 properties that make a window gamescope's overlay (gamescope's steamcompmgr): STEAM_OVERLAY marks it, drawn
// above the game at its _NET_WM_WINDOW_OPACITY (0 = not shown); STEAM_INPUT_FOCUS gives it the input. Open: shown and
// taking the input; closed: neither. Pure, so it is testable without gamescope.
std::vector<std::pair<const char *, unsigned>> OverlayProperties(bool Open);
// Set them on a window, when the app is on X11 (under gamescope a VidyaGod session is): a no-op elsewhere.
void MarkOverlayWindow(QWidget *W, bool Open);
// A window drawn above a game under gamescope without taking its input (gamescope's external overlay, as MangoHud
// is): the volume toast. A no-op off X11.
void MarkExternalOverlay(QWidget *W);

// ---------------------------------------------------------------------------
// GameOverlay — VidyaGod's in-game overlay (Steam's Guide-button overlay): over a game VidyaGod launched, opened by the
// controller's Guide button, driven by the controller like the rest of VidyaGod. It offers: resume; the game's
// controller layout (switch it live, or open the configurator); quit the game (a second press confirms); and the
// time and battery.
//
// How the Guide button reaches it: while a game runs the controller service asks InputPlumber to intercept the Guide
// button only (intercept 1); a press switches InputPlumber to intercept everything (2), so the game gets no input and
// every press reaches VidyaGod over D-Bus (ControllerService::uiEvent), which drives this window as a controller would.
// Closing goes back to intercepting the Guide button only; the game's end lets go of the intercept.
//
// Under gamescope (a handheld's VidyaGod session) it is gamescope's overlay window (MarkOverlayWindow), drawn above
// the game. On a desktop without gamescope it is a window over the game, stays on top.
// ---------------------------------------------------------------------------
class GameOverlay : public QWidget, public PadTarget
{
    Q_OBJECT
public:
    static GameOverlay &instance();
    // Follow the controller service (its game and its intercepted input): the app does this once (MainWindow).
    void attach();

    // The game running now (its launch window tells it: title, its key for layouts, how to quit it), and its end.
    void setGame(const QString &Title, const QString &Key, std::function<void()> Quit);
    void clearGame();
    bool hasGame() const { return !GameKey.isEmpty() || !GameTitle.isEmpty(); }

    bool isOpen() const { return Open; }
    void openOverlay();
    void closeOverlay();
    QString title() const;        // the game's, as shown
    QString layoutName() const;   // the game's controller layout, as shown ("Gamepad": the controller as it is)
    bool setLayout(const QString &Name, QString *Error = nullptr);   // switch the game's layout, live
    void editLayout();            // the configurator, for the game
    void quitGame();              // the first press arms, the second quits
    // Type into the game (its keyboard: InputPlumber's virtual one), then back to it. Enter after it when asked (the
    // overlay's "and press Enter", on by default: a chat line, a name).
    bool typeIntoGame(const QString &Text, QString *Error = nullptr);
    bool quitArmed() const;
    void disarmQuit();            // armed no more (focus moved off it, or it waited too long)

    // The intercepted input, as the controller service hands it over (public: a test drives it as InputPlumber does).
    void onUiEvent(const QString &Event, double Value);
    void onGameChanged(bool Running);

    bool padPressed(Pad::Button B) override;
    QList<PadTarget::Hint> padHints() const override;

protected:
    void paintEvent(QPaintEvent *) override;
    void keyPressEvent(QKeyEvent *E) override;

private:
    GameOverlay();
    void refresh();                     // title, layout list, clock, battery

    bool Open = false, Attached = false;
    QString GameTitle, GameKey;
    std::function<void()> QuitGame;
    QLabel *Title = nullptr, *Clock = nullptr, *Battery = nullptr, *Hints = nullptr;
    QPushButton *Resume = nullptr, *EditButton = nullptr, *QuitButton = nullptr;
    QComboBox *Layouts = nullptr;
    QLineEdit *TypeEdit = nullptr;
    QPushButton *TypeButton = nullptr;
    QCheckBox *TypeEnter = nullptr;
    QTimer *Tick = nullptr;
};

// The controller button an intercepted InputPlumber event stands for (ui_accept → A, ui_up → Up, …); false for one
// that stands for none (ui_touch, ui_volume_up, …).
bool PadButtonFromUiEvent(const QString &Event, Pad::Button &B);
