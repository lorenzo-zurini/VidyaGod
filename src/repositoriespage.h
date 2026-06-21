#ifndef REPOSITORIESPAGE_H
#define REPOSITORIESPAGE_H

#include <QWidget>

class AppModel;
class QScrollArea;

// ---------------------------------------------------------------------------
// RepositoriesPage — the Settings "Repositories" page: add/remove/sync the git repos that share dehydrated
// packages. Reads the repo list from the AppModel's config, performs add/remove/sync through the model (which does
// the git work off-thread), and rebuilds itself on the model's repositoriesChanged signal.
// ---------------------------------------------------------------------------
class RepositoriesPage : public QWidget
{
    Q_OBJECT
public:
    RepositoriesPage(AppModel &model, QWidget *parent = nullptr);

public slots:
    void rebuild();            // (re)build the repo cards + add row (on repositoriesChanged)

private:
    AppModel &Model;
    QScrollArea *Scroll = nullptr;
};

#endif // REPOSITORIESPAGE_H
