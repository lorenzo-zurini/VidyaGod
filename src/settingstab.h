#ifndef SETTINGSTAB_H
#define SETTINGSTAB_H

#include <QWidget>

class MainWindow;
class QListWidget;
class QStackedWidget;
class QScrollArea;

// ---------------------------------------------------------------------------
// SettingsTab — the "Settings" tab extracted out of MainWindow: the category sidebar + stacked pages (Runners,
// Repositories, Downloads, Storage & Paths). The "Installed Packages" page (page 0) stays MainWindow-owned (it's
// woven into the catalog rebuild + Package Editor) and is embedded here via MainWindow's PackagesTabWidget. Reads
// catalog/config from MainWindow (friend). It IS the tab QWidget (MainWindow adds it).
// ---------------------------------------------------------------------------
class SettingsTab : public QWidget
{
    Q_OBJECT
public:
    SettingsTab(MainWindow &Owner, QWidget *parent = nullptr);

private:
    void buildUi();                  // was MainWindow::BuildSettingsTab
    void rebuildRunnersPage();       // was MainWindow::RebuildSettingsRunnersPage
    void rebuildReposPage();         // was MainWindow::RebuildSettingsReposPage
    QWidget *buildDownloadsPage();   // was MainWindow::BuildDownloadsSettingsPage
    QWidget *buildPathsPage();       // was MainWindow::BuildPathsSettingsPage

    MainWindow &Mw;
    QListWidget    *SettingsCategoryList  = nullptr;
    QStackedWidget *SettingsStack         = nullptr;
    QScrollArea    *SettingsRunnersScroll = nullptr;   // rebuilt in place by rebuildRunnersPage()
    QScrollArea    *SettingsReposScroll   = nullptr;   // rebuilt in place by rebuildReposPage()
};

#endif // SETTINGSTAB_H
