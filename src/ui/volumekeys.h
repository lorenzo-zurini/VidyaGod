#ifndef VOLUMEKEYS_H
#define VOLUMEKEYS_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <optional>
#include <utility>
#include <vector>

class QTimer;

// The volume keys of a handheld (the Steam Deck's +/− on its top edge). In SteamOS, Steam answers them; in a VidyaGod
// session nothing did. InputPlumber owns the keyboard device they are on, and every layout VidyaGod hands it maps them
// to its D-Bus events (ControllerLayout: ui_volume_up / ui_volume_down / ui_volume_mute), which reach VidyaGod in any
// intercept mode, a game running or not. VolumeKeys turns those into the system's volume — the default output, through
// PipeWire's wpctl, else PulseAudio's pactl — and shows the level (VolumeToast) for a moment, over a game too.
namespace Volume {
struct Level { int Percent = -1; bool Muted = false; };   // Percent -1: not known

// What a tool says the default output's volume is: wpctl's "Volume: 0.55 [MUTED]", pactl's "Volume: front-left: …
// / 55% / …" (with its mute from "Mute: yes"). Nothing when it says nothing usable.
std::optional<Level> ParseWpctl(const QString &Out);
std::optional<Level> ParsePactl(const QString &VolumeOut, const QString &MuteOut);

// The commands for a step of Delta percent (capped at 100%), a mute toggle, and reading the level — for a tool
// ("wpctl" or "pactl"): {program, arguments} each.
using Command = std::pair<QString, QStringList>;
Command StepCommand(const QString &Tool, int Delta, int Now = -1);   // Now: the level known (-1: not); {"" , {}}: nothing to do
Command MuteCommand(const QString &Tool);
std::vector<Command> ReadCommands(const QString &Tool);
}

// A moment's display of the level: a bar and the percentage, at the bottom of the screen. Over VidyaGod's own window
// when no game runs; over a game, gamescope's external overlay (drawn above the game, taking no input — as MangoHud
// is). Hides itself after ShowMs.
class VolumeToast : public QWidget
{
    Q_OBJECT
public:
    static constexpr int ShowMs = 1500;
    explicit VolumeToast(QWidget *Parent = nullptr);
    void present(Volume::Level L);
    Volume::Level level() const { return Shown; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    Volume::Level Shown;
    QTimer *Hide = nullptr;
};

class VolumeKeys : public QObject
{
    Q_OBJECT
public:
    static VolumeKeys &instance();
    static constexpr int StepPercent = 5;

    void follow();                     // the controller service's ui_volume_* events (the app does this once)
    void step(int Delta);              // up (+) or down (−) by Delta percent
    void toggleMute();
    void refresh();                    // read the level (async): changed() when read
    Volume::Level level() const { return Current; }
    QString tool() const;              // "wpctl", "pactl", or "" when neither is installed
    void onUiEvent(const QString &Event, double Value);   // ui_volume_up/down/mute pressed (1) or released (0)

signals:
    void changed(Volume::Level L);

private:
    VolumeKeys();
    void run(const Volume::Command &C, bool ThenRead);
    void readNext(std::vector<Volume::Command> Left, QStringList Outs);   // the level, one command after another
    void repeat();

    Volume::Level Current;
    VolumeToast *Toast = nullptr;
    QTimer *Repeat = nullptr;          // a held key steps again, as a keyboard repeats
    int Held = 0;                      // the held key's step, 0 when none
    int Repeats = 0;                   // steps of this hold
    static constexpr int MaxRepeats = 56;   // ~7 s at 120 ms: a release that never came stops here
};

#endif // VOLUMEKEYS_H
