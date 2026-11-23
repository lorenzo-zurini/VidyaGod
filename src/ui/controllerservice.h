#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class QDBusConnection;
class QTimer;

// ---------------------------------------------------------------------------
// ControllerService — VidyaGod's own InputPlumber (the InputPlumber submodule, our fork), run as the user's child
// process: no system service, no root, no polkit. It owns the controllers it has a configuration for (handhelds' own
// controls, a few named pads), reads them through hidraw/evdev, and gives everything else virtual ones (uinput): a pad,
// a mouse and a touchpad for the trackpads, a keyboard. Layouts (InputPlumber's device profiles) are loaded into it at
// run time. Linux only.
//
// When it runs ("only while in use"): VidyaGod needs it for reasons (Need) — controller mode is open, a game it
// launched is running, or VidyaGod is a handheld's session (always). It runs while any reason holds and stops when
// none does, letting the controllers go back to everything else. A game never starts it: a game launched while it is
// not running plays with the controllers as they are (it would otherwise appear mid-launch, after the sandbox was
// built); a game launched while it runs keeps it running until the game ends.
//
// Games see only the virtual controllers: the sandbox covers the real devices' nodes (sourceNodes()).
//
// It never runs unless the app named its program (setProgram, from MainWindow): a test never starts the real one, and
// with a system-wide InputPlumber already running (a distro that ships it) VidyaGod uses none of its own.
// ---------------------------------------------------------------------------
class ControllerService : public QObject
{
    Q_OBJECT
public:
    static ControllerService &instance();

    enum class Need : std::uint8_t { ControllerMode = 1, Game = 2, Session = 4 };
    static QString NeedName(Need N);

    // The InputPlumber binary to run ("" = none: the service is unavailable) and the D-Bus bus it and VidyaGod meet
    // on: "session" (the default), or a bus address (a test's private bus).
    void setProgram(const QString &Path);
    QString program() const { return Program; }
    void setBus(const QString &Bus);
    QString bus() const { return Bus; }
    // Where its device configurations live (XDG data dir holding inputplumber/): the submodule's, bundled with the app.
    void setDataDir(const QString &Dir) { DataDir = Dir; }

    // A reason to run, held or let go. The service starts when the first is held (Game excepted, see above) and
    // stops when the last is let go.
    void want(Need N, bool On);
    bool wants(Need N) const { return (Needs & static_cast<std::uint8_t>(N)) != 0; }
    QStringList needs() const;

    // Why it cannot run here ("" = it can): no program named, not Linux, or a system-wide InputPlumber already runs.
    QString unavailable() const;
    bool available() const { return unavailable().isEmpty(); }
    // The process is up and answering on the bus.
    bool running() const { return Answering; }

    // A controller InputPlumber owns: its D-Bus path, name, loaded layout, and the device nodes it reads.
    struct Device { QString Path, Name, Profile; QStringList Sources; };
    QList<Device> devices() const;
    // The real device nodes behind every owned controller, with each one's joystick siblings (/dev/input/jsN):
    // what a game's sandbox covers so the game sees only the virtual controllers. Empty while not running.
    QStringList sourceNodes() const;
    // sourceNodes() as last seen on the main thread, for any thread (the sandbox, built on a launch's worker thread).
    std::vector<std::string> hiddenNodes() const;

    // A layout (an InputPlumber device profile, YAML) for every owned controller, now and for any that comes later.
    // "" = InputPlumber's own default. False, with Error, when a controller refuses it.
    bool loadLayout(const QString &Yaml, QString *Error = nullptr);
    QString layout() const { return Layout; }
    // A game's layout, over the one above while it is set (a game VidyaGod launched is running); "" lets it go and
    // the layout above is back. False, with Error, when a controller refuses it.
    bool setGameLayout(const QString &Yaml, QString *Error = nullptr);
    QString gameLayout() const { return GameLayout; }

    // InputPlumber's intercept on every owned controller: 0 nothing; 1 only Guide (which then switches it to 2 on its
    // own); 2 everything — the game gets no input, VidyaGod gets it all as uiEvent. The overlay's: 1 while a game runs.
    bool setIntercept(uint Mode, QString *Error = nullptr);
    uint intercept() const;   // the first controller's (0 when none)

    // Type into whatever has the keyboard (a game): text as key presses on InputPlumber's virtual keyboard (SendEvent),
    // with Shift for capitals and the shifted symbols of a US layout; "\n" is Enter, "\b" Backspace. Characters a US
    // keyboard cannot type are skipped (Error names them). False when no controller takes the keys.
    bool typeText(const QString &Text, QString *Error = nullptr);

    // The bus name InputPlumber serves (also the system service's).
    static constexpr const char *BusName = "org.shadowblip.InputPlumber";

    // For a test: forget every reason and stop (the singleton outlives a test case).
    void reset();

signals:
    void changed();   // started, stopped, or a controller came or went
    void gameChanged(bool Running);                     // a game VidyaGod launched began, or the last one ended
    void uiEvent(const QString &Event, double Value);    // an intercepted input: "ui_guide", "ui_accept", … (1 down, 0 up)

private slots:
    void onObjectsChanged();     // InputPlumber's ObjectManager: a controller came or went
    void onInputEvent(const QString &Event, double Value);   // InputPlumber's DBusDevice: an intercepted input

private:
    ControllerService();
    ~ControllerService() override;
    void update();               // start or stop for the reasons held
    void start();
    void stop();
    void onRegistered();         // InputPlumber took its bus name
    void applyLayout();          // the layout in force (the game's, else VidyaGod's) to every owned controller
    void takeControllers();      // tell InputPlumber to manage every controller, once its Manager is served
    bool applyTo(const QString &Yaml, QString *Error);   // one layout to every owned controller ("" = InputPlumber's default)
    QDBusConnection connection() const;

    QString Program, Bus = QStringLiteral("session"), DataDir, Layout, GameLayout;
    std::uint8_t Needs = 0;
    QProcess *Proc = nullptr;
    bool Answering = false;
    bool Managing = false;   // InputPlumber was told to take the controllers (ManageAllDevices)
    QTimer *GameWatch = nullptr; // lets go of Need::Game when VidyaGod's last launch ends
    bool GamesRan = false;       // what GameWatch saw last
    QString BusConnName;         // the named QDBusConnection for an address bus
    mutable std::mutex HiddenLock;
    std::vector<std::string> Hidden;   // sourceNodes(), refreshed on the main thread (refreshHidden)
    void refreshHidden();
};

// Whether an input device node (/dev/input/eventN, /dev/input/jsN, /dev/hidrawN) is a virtual one (uinput/uhid: under
// /devices/virtual in sysfs), read under SysRoot ("/sys" on a real system; a test's tree otherwise).
bool IsVirtualInputNode(const QString &Node, const QString &SysRoot = QStringLiteral("/sys"));
// The joystick nodes (/dev/input/jsN) of the same input device as an event node (/dev/input/eventN), under SysRoot.
QStringList JoystickSiblings(const QString &EventNode, const QString &SysRoot = QStringLiteral("/sys"));

// The key (InputPlumber's name, "KeyA") and whether Shift goes with it, for one character on a US keyboard; empty when
// there is none ("é").
struct KeyStroke { QString Key; bool Shift = false; };
KeyStroke KeyStrokeFor(QChar C);
