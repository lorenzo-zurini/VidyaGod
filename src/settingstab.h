#ifndef SETTINGSTAB_H
#define SETTINGSTAB_H

#include <QWidget>

class AppModel;
class QListWidget;
class QStackedWidget;

// ---------------------------------------------------------------------------
// SettingsTab — the "Settings" tab: a category sidebar + a stack of self-contained page widgets (GeneralPage,
// PackagesView, RunnersPage, RepositoriesPage, IpfsSettingsPage, PathsPage). It is a thin container — no settings
// logic of its own; each
// page reads/writes through the AppModel and subscribes to the model's signals on its own.
// ---------------------------------------------------------------------------
class SettingsTab : public QWidget
{
    Q_OBJECT
public:
    SettingsTab(AppModel &model, QWidget *parent = nullptr);

private:
    AppModel &Model;
    QListWidget    *CategoryList = nullptr;
    QStackedWidget *Stack        = nullptr;
};

#endif // SETTINGSTAB_H
