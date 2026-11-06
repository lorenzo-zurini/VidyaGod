#ifndef PATHSPAGE_H
#define PATHSPAGE_H

#include <QWidget>

class AppModel;
class QVBoxLayout;

// ---------------------------------------------------------------------------
// PathsPage — the Settings "Storage & Paths" page: the library folder and the (read-only) app-data directory
// (Settings.Paths through the AppModel), what each part of the data dir takes on disk with the space left, and the
// clean-ups that lose nothing (left-over sessions, the index cache).
// ---------------------------------------------------------------------------
class PathsPage : public QWidget
{
    Q_OBJECT
public:
    PathsPage(AppModel &model, QWidget *parent = nullptr);

private:
    void buildSpace(QVBoxLayout *pl);
    void buildCleanUp(QVBoxLayout *pl);

    AppModel &Model;
};

#endif // PATHSPAGE_H
