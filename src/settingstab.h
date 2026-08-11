#ifndef SETTINGSTAB_H
#define SETTINGSTAB_H

#include <QWidget>
#include <vector>
#include <functional>

class AppModel;
class QListWidget;
class QStackedWidget;

// ---------------------------------------------------------------------------
// SettingsTab — the "Settings" tab: a category sidebar + a stack of self-contained page widgets (GeneralPage,
// PackagesView, RunnersPage, SourcesPage, IpfsSettingsPage, PathsPage). It is a thin container — no settings
// logic of its own; each
// page reads/writes through the AppModel and subscribes to the model's signals on its own.
// ---------------------------------------------------------------------------
class SettingsTab : public QWidget
{
    Q_OBJECT
public:
    SettingsTab(AppModel &model, QWidget *parent = nullptr);

    void showPage(int row);   // build the page on first visit (lazy), then show it (also used to navigate)

private:
    AppModel &Model;
    QListWidget    *CategoryList = nullptr;
    QStackedWidget *Stack        = nullptr;
    std::vector<QWidget *>                 Pages;          // built lazily; nullptr until first visited
    std::vector<std::function<QWidget *()>> PageFactories;  // one per sidebar row
};

#endif // SETTINGSTAB_H
