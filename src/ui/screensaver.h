#ifndef SCREENSAVER_H
#define SCREENSAVER_H

#include <QObject>
#include <QPointer>
#include <QTimer>

#include <functional>

class MatrixRain;
class QWidget;

// ScreenSaver — after a while with no input, the controller-mode screen (an OLED handheld's whole screen) gives way to
// the digital rain (MatrixRain), so nothing stays lit in one place long enough to burn in. Any input ends it: a key,
// a click, a touch, the mouse or a trackpad moving, a controller's button or stick — and the input that ends it does
// nothing else (pressing A on a hidden tile must not start a game): while it shows, the rain is the controller's
// overlay (PadRouter) and key and mouse presses are swallowed. Never while a game runs or the screen is not shown
// (suspended), never when the interval is 0. The interval is read each time the wait starts over
// (Settings.ScreenSaverSeconds: SecondsIn), so a change in Settings applies at the next input.
class ScreenSaver : public QObject
{
    Q_OBJECT
public:
    static constexpr int DefaultSeconds = 60;
    // Covered: the widget the rain covers. Seconds: the idle time before it starts (0: never).
    ScreenSaver(QWidget *Covered, std::function<int()> Seconds, QObject *Parent = nullptr);
    ~ScreenSaver() override;

    // The setting: Settings.ScreenSaverSeconds of a config, DefaultSeconds when unset or not a whole number ≥ 0.
    template <class Json> static int SecondsIn(const Json &Config)
    {
        if (Config.contains("Settings") && Config["Settings"].is_object() && Config["Settings"].contains("ScreenSaverSeconds")
            && Config["Settings"]["ScreenSaverSeconds"].is_number_integer())
        {
            const auto S = Config["Settings"]["ScreenSaverSeconds"].template get<long long>();
            if (S >= 0) return S > 24 * 3600 ? 24 * 3600 : int(S);
        }
        return DefaultSeconds;
    }

    void setSuspended(bool On);   // a game runs, or the covered screen is not the one showing: no rain, no wait
    bool suspended() const { return Suspended; }
    bool showing() const;
    void showNow();               // the rain now (unless suspended)
    void dismiss();               // the rain goes; the wait starts over
    void activity();              // input: the wait starts over (the rain itself goes by dismiss)
    int idleRemainingMs() const;  // until the rain, or -1 when not waiting
    MatrixRain *rain() const;

    bool eventFilter(QObject *Watched, QEvent *E) override;

signals:
    void shown();
    void dismissed();

private:
    void rearm();

    QPointer<QWidget> Covered;
    std::function<int()> Seconds;
    QTimer Idle;
    class View;
    View *Rain = nullptr;
    bool Suspended = false;
    bool Showing = false;   // the rain is up (its own record: a hidden window hides the rain too, and it still runs)
};

#endif // SCREENSAVER_H
