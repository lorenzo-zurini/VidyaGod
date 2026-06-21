#ifndef CATALOGTAB_H
#define CATALOGTAB_H

#include <QWidget>
#include <QList>
#include <QSet>
#include <QString>
#include <filesystem>

#include "mainwindow.h"     // MainWindow (friend host) + SortMode
#include "libraryview.h"    // LibraryView / LibraryGameCard

// ---------------------------------------------------------------------------
// CatalogTab — the "Catalog" (Available) tab extracted out of MainWindow: the un-hydrated repo games shown in the
// LibraryView card grid (per-package grouping, repo sections, sort/search), with download-on-hover. It IS the tab
// QWidget (MainWindow adds it). Reads the catalog/config from MainWindow (friend) and drives downloads via
// MainWindow's DownloadManager; DownloadManager in turn updates this tab's cards through the small public API.
// ---------------------------------------------------------------------------
class CatalogTab : public QWidget
{
    Q_OBJECT
public:
    CatalogTab(MainWindow &Owner, QWidget *parent = nullptr);

    void rebuild();                                          // was RebuildAvailableTab (full pool rebuild + relayout)
    void markDownloading(const QString &groupKey);          // DownloadManager: mark a package's card(s) downloading
    void setDownloadProgress(const QString &groupKey, double avg);  // DownloadManager: paint averaged % onto card(s)
    void refreshCovers();                                   // a cover finished loading → repaint visible cards

private:
    void buildUi();                                         // was BuildAvailableTab
    void applyFilter();                                     // was ApplyAvailableFilter (sort/search/group, no restat)
    QString repoNameForBundle(const std::filesystem::path &BundleDir) const;  // was RepoNameForBundle

    MainWindow &Mw;
    LibraryView *AvailableView = nullptr;
    QList<LibraryGameCard *> *AvailableGameCards = new QList<LibraryGameCard *>();  // full pool (all un-hydrated tiles)
    QList<LibraryGameCard *>  AvailableVisible;             // search-filtered subset actually shown
    QString       AvailableSearch;                          // case-insensitive title filter
    QSet<QString> AvailableCollapsedRepos;                  // collapsed repo sections (persisted)
    MainWindow::SortMode AvailableSort = MainWindow::SortMode::Name;   // within-group ordering
};

#endif // CATALOGTAB_H
