#ifndef GENERALPAGE_H
#define GENERALPAGE_H

#include <QWidget>

class AppModel;

// ---------------------------------------------------------------------------
// GeneralPage — the Settings "General" page: the tray / daemon-hybrid behavior toggles (close to tray, minimize to
// tray, start in tray). Reads/writes Settings.Tray through the AppModel. MainWindow reads these live (no signal
// needed) when it decides whether a close/minimize should hide to the tray.
// ---------------------------------------------------------------------------
class GeneralPage : public QWidget
{
    Q_OBJECT
public:
    GeneralPage(AppModel & model, QWidget * parent = nullptr);

private:
    AppModel & Model;
};

#endif // GENERALPAGE_H
