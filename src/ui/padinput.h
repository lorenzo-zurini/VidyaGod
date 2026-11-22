#ifndef PADINPUT_H
#define PADINPUT_H

#include <QObject>
#include <QList>
#include <QString>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Pad — a game controller's controls by name, and the pure rules that turn sticks, triggers and held buttons into
// presses (testable without a controller).
// ---------------------------------------------------------------------------
namespace Pad
{
enum class Button : std::uint8_t { Up, Down, Left, Right, A, B, X, Y, LB, RB, LT, RT, Start, Back, Guide, L3, R3 };
inline constexpr int ButtonCount = 17;

// "up", "down", "left", "right", "a", "b", "x", "y", "lb", "rb", "lt", "rt", "start", "back", "guide", "l3", "r3".
std::string Name(Button B);
std::optional<Button> FromName(const std::string &Name);
std::vector<std::string> Names();
bool IsDirection(Button B);

// How a controller labels its buttons: the hint bar draws that family's glyphs. A is always the bottom face button
// (Nintendo's is labelled B there: what is drawn follows the label, the meaning follows the position).
enum class Glyphs : std::uint8_t { Xbox, PlayStation, Nintendo, SteamDeck };
std::string GlyphsName(Glyphs G);

// The directions a stick holds. A direction is held once its axis passes Press, and let go once it falls back inside
// Release, so a stick resting near the edge does not chatter; only the stronger axis counts while both are past.
struct Stick
{
    bool Up = false, Down = false, Left = false, Right = false;
    bool operator==(const Stick &O) const = default;
};
inline constexpr double StickPress = 0.55, StickRelease = 0.35;
Stick StickDirections(double X, double Y, const Stick &Was);   // X right, Y down, each -1..1
// A trigger is held past 0.6 and let go under 0.3.
bool TriggerHeld(double Value, bool Was);

// A held direction repeats: once after First ms, then every Every ms, every Fast ms once held for FastAfter.
class Repeater
{
public:
    static constexpr int First = 380, Every = 85, Fast = 45, FastAfter = 1400;
    void press(Button B, long long Now);
    void release(Button B);
    void releaseAll();
    std::vector<Button> due(long long Now);   // the repeats owed by Now (each held direction at most once per call)
    bool held(Button B) const;
private:
    struct Hold { bool On = false; long long Since = 0, Next = 0; };
    std::array<Hold, ButtonCount> Holds{};
};
}

// ---------------------------------------------------------------------------
// PadInput — the app's ONE reader of game controllers (SDL2's game-controller API): it turns them into presses for
// navigation (pressed / released / scrolled), and lends the raw event queue to a press-a-key capture while one runs.
//
// Presses reach the app only while it is the active application and no game it launched is running, so a game being
// played never drives the window behind it; while a launch runs the controllers are closed, so the game has them to
// itself. Steam Input's virtual controller (the Steam Deck's, in Game Mode) is a controller like any other; without
// Steam, controller mode reads the Deck's own (setDeckDirect).
// ---------------------------------------------------------------------------
class PadInput : public QObject
{
    Q_OBJECT
public:
    static PadInput &instance();

    // Read controllers for navigation (SDL starts the first time). Off: nothing is read, nothing is pressed.
    void setEnabled(bool On);
    bool enabled() const { return Enabled; }
    // A press as if from a controller — the same delivery as a real one, so the CLI and the tests drive what a
    // controller drives. It reaches the app even while it is not the active application.
    void inject(Pad::Button B);
    // A chord: Held held down (as held() reports them) while B is pressed, then let go.
    void injectChord(const std::vector<Pad::Button> &Held, Pad::Button B);

    struct PadInfo { QString Name; Pad::Glyphs Glyphs = Pad::Glyphs::Xbox; };
    QList<PadInfo> pads() const;
    Pad::Glyphs glyphs() const { return LastGlyphs; }   // the family of the controller used last (Xbox when none)
    // A launch is running: controllers are closed and nothing is pressed until it ends.
    bool suspended() const;
    // Whether a controller holds B down now (a chord: Start with Back held).
    bool held(Pad::Button B) const { return Down[static_cast<size_t>(B)]; }
    // A short rumble on the controller used last, when haptics are on (Settings.Controller.Haptics).
    void tick();
    void setHaptics(bool On) { Haptics = On; }

    // The Steam Deck's own controller, read directly (SDL's driver for it) rather than through Steam Input. Without
    // Steam running — a VidyaGod session of its own, the desktop — nothing else reads it: its kernel gamepad is
    // silent while the Deck is in lizard mode (buttons as keys, the trackpad as the mouse). Reading it turns lizard
    // mode off for as long as it is read, so controller mode turns this on and the desktop turns it off again, giving
    // the trackpad mouse back. A launched game always has the Deck to itself: it is let go while the game runs.
    void setDeckDirect(bool On);
    bool deckDirect() const { return DeckDirect && !suspended(); }   // read directly now

    // The launch window's press-a-key field: while a capture runs it reads the raw queue and navigation stands aside.
    // Begin drops whatever was queued (a button already held binds nothing); Poll gives the name of the first new
    // press ("pad a", "pad left stick up" — KeyCapture::NameOfPad*), else ""; End gives navigation the queue back.
    void captureBegin();
    std::string capturePoll();
    void captureEnd();
    bool capturing() const { return Capturing; }   // a capture holds the queue now

signals:
    void pressed(Pad::Button B);            // a press, or a held direction's repeat
    void released(Pad::Button B);
    void scrolled(double Dx, double Dy);    // the right stick, in pixels for this poll (already scaled by time)
    void padsChanged();                     // a controller came or went
    void used();                            // any controller input at all (the focus ring shows)

private:
    PadInput();
    void poll();
    bool start();                           // SDL's game-controller subsystem, once; false when unavailable
    void open(int Index);
    void closeAll();
    void deliver(Pad::Button B, bool Down, bool Synthetic);
    bool active() const;                    // the app may take a press now

    bool Enabled = false, Started = false, Ok = false, Capturing = false, Haptics = false, WasSuspended = false;
    bool DeckDirect = false;
    void applyDeckDirect();   // SDL's Deck driver on exactly while deckDirect()
    class QTimer *Timer = nullptr;
    struct Opened;                          // SDL handles, kept out of this header
    std::vector<Opened> *Pads = nullptr;
    int LastPad = -1;                       // the SDL instance id used last
    Pad::Glyphs LastGlyphs = Pad::Glyphs::Xbox;
    Pad::Stick Left;                        // the left stick's held directions
    bool LtHeld = false, RtHeld = false;
    std::array<bool, Pad::ButtonCount> Down{};   // what the controllers hold (dpad and stick merged)
    std::array<bool, 4> DpadHeld{};              // the d-pad by itself (up, down, left, right)
    Pad::Repeater Repeat;
    long long LastPoll = 0, LastInput = 0;   // the last poll; the last time a controller was used
    void pace();                              // the poll's interval for now (fast in use, slow idle, slower empty)
    std::vector<std::pair<int, int>> CaptureHeld;   // axes past half when the capture began
};

#endif // PADINPUT_H
