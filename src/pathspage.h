#ifndef PATHSPAGE_H
#define PATHSPAGE_H

#include <QWidget>

class AppModel;

// ---------------------------------------------------------------------------
// PathsPage — the Settings "Storage & Paths" page: the temporary/runtime root, the library folder, and the
// (read-only) app-data directory. Reads/writes Settings.Paths through the AppModel.
// ---------------------------------------------------------------------------
class PathsPage : public QWidget
{
    Q_OBJECT
public:
    PathsPage(AppModel &model, QWidget *parent = nullptr);

private:
    AppModel &Model;
};

#endif // PATHSPAGE_H
