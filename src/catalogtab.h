#ifndef CATALOGTAB_H
#define CATALOGTAB_H

#include <QWidget>
#include <QList>
#include <QSet>
#include <QHash>
#include <QString>
#include <filesystem>

#include "libraryview.h"    // LibraryView / LibraryGameCard / SortMode

class AppModel;

// ---------------------------------------------------------------------------
// CatalogTab — the "Catalog" (Available) tab: the un-hydrated repo games shown in the LibraryView card grid
// (per-package grouping, repo sections, sort/search), with download-on-hover. Reads the catalog/config through the
// AppModel. It does NOT know about the DownloadManager or the IPFS tab: it emits download/cancel requests as signals
// and receives download state back through slots the DownloadManager's signals connect to. It tracks its own
// in-flight set so a rebuild can restore the "Downloading…" overlay without querying anyone.
// ---------------------------------------------------------------------------
class CatalogTab : public QWidget
{
    Q_OBJECT
public:
    CatalogTab(AppModel &model, QWidget *parent = nullptr);

public slots:
    void rebuild();                                          // full pool rebuild + relayout (on catalogChanged)
    void markDownloading(const QString &groupKey);          // a package's card(s) → downloading
    void setDownloadProgress(const QString &groupKey, double avg);  // paint averaged % onto card(s)
    void clearDownloading(const QString &groupKey);         // download finished → drop the in-flight overlay state
    void refreshCovers();                                   // a cover finished loading → repaint visible cards
    void onCardSizeChanged(int cardW);                      // model card-size changed → re-check buttons + relayout

signals:
    void downloadRequested(LibraryGameCard *card);          // a card was clicked to import it
    void cancelRequested(LibraryGameCard *card);            // a downloading card was clicked to cancel

private:
    void buildUi();
    void applyFilter();                                     // sort/search/group, no restat
    void openPackageSourcesDialog();                        // "Add CID" — manage IPFS folder-CID package sources
    QString repoNameForBundle(const std::filesystem::path &BundleDir) const;

    AppModel &Model;
    LibraryView *AvailableView = nullptr;
    QList<LibraryGameCard *> *AvailableGameCards = new QList<LibraryGameCard *>();  // full pool (all un-hydrated tiles)
    QList<LibraryGameCard *>  AvailableVisible;             // search-filtered subset actually shown
    QString       AvailableSearch;                          // case-insensitive title filter
    QSet<QString> AvailableCollapsedRepos;                  // collapsed repo sections (persisted)
    SortMode      AvailableSort = SortMode::Name;           // within-group ordering
    QHash<QString, double> ActiveDownloads;                 // groupKey → latest % for in-flight downloads (overlay restore)
};

#endif // CATALOGTAB_H
