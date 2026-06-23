#ifndef TRAYCONTROLLER_H
#define TRAYCONTROLLER_H

#include <QObject>

class AppModel;
class QWidget;
class QSystemTrayIcon;

// ---------------------------------------------------------------------------
// TrayController — the daemon/foreground-hybrid seam. Owns the QSystemTrayIcon (a "VidyaGod is running" affordance
// with a Show / Quit menu) and reads the user's tray preferences from Settings.Tray. It owns NO window logic — it
// just answers "should closing/minimizing hide to tray?" and emits restoreRequested / quitRequested for MainWindow
// to act on. Persists the remembered open/hidden state (so the window relaunches the way the user left it).
//
// Settings.Tray defaults (all created on first read): CloseToTray=true, MinimizeToTray=true, StartInTray=false,
// LaunchHidden=false (the remembered last state). If no system tray is available, everything degrades to normal
// window behavior (close = quit) — available() is false and MainWindow consults it before hiding.
// ---------------------------------------------------------------------------
class TrayController : public QObject
{
    Q_OBJECT
public:
    TrayController(AppModel & model, QWidget * window, QObject * parent = nullptr);

    bool available()      const;                 // a system tray exists (else: normal window behavior)
    bool closeToTray()    const { return setting("CloseToTray", true); }
    bool minimizeToTray() const { return setting("MinimizeToTray", true); }
    bool startInTray()    const { return setting("StartInTray", false); }

    // The window should come up hidden in the tray: either the explicit Start-in-tray option, or the remembered
    // last state (it was hidden when the user quit). Only honored when a tray is actually available.
    bool shouldStartHidden() const { return available() && (startInTray() || setting("LaunchHidden", false)); }

    // Persist the remembered open/hidden state + flash a one-time "still running" hint the first time we hide.
    void rememberHidden(bool Hidden);
    void notifyHidden();

signals:
    void restoreRequested();   // tray icon clicked / "Show" chosen → MainWindow un-hides
    void quitRequested();      // tray "Quit" chosen → MainWindow really quits

private:
    bool setting(const char * Key, bool Default) const;

    AppModel &        Model;
    QSystemTrayIcon * Tray   = nullptr;
    bool              Hinted = false;   // showed the "still running" balloon once this session
};

#endif // TRAYCONTROLLER_H
