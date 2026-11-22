#ifndef CONTROLLERPAGE_H
#define CONTROLLERPAGE_H

#include <QWidget>

class AppModel;

// ---------------------------------------------------------------------------
// ControllerPage — the Settings "Controller" page: how the app starts (controller mode, the desktop window, or by
// whether Steam's Game Mode is running), whether a controller drives the desktop window too, rumble as the focus
// moves, and the controllers connected now; outside controller mode, a button into it. Writes Settings.Controller
// through the AppModel and applies it to the window live.
// ---------------------------------------------------------------------------
class ControllerPage : public QWidget
{
    Q_OBJECT
public:
    ControllerPage(AppModel & model, QWidget * parent = nullptr);

private:
    AppModel & Model;
};

#endif // CONTROLLERPAGE_H
