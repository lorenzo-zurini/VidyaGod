#ifndef LIBRARYTAB_H
#define LIBRARYTAB_H

#include <QWidget>
#include <QList>
#include <QSet>
#include <QString>

#include "libraryview.h"   // LibraryView / LibraryGameCard / SortMode

class AppModel;

// ---------------------------------------------------------------------------
// LibraryTab — the "Library" tab: the hydrated games shown in the LibraryView card grid with a Name/Date/Series
// sort, a title search, a size picker, and collapsible Series sections. Reads the catalog/config through the
// AppModel and rebuilds itself on the model's catalogChanged / coversReady / cardSizeChanged signals. It owns its
// own card pool. Persists its sort mode + collapsed-series set through the model.
// ---------------------------------------------------------------------------
class LibraryTab : public QWidget
{
    Q_OBJECT
public:
    LibraryTab(AppModel &model, QWidget *parent = nullptr);
    ~LibraryTab() override;

public slots:
    void rebuild();                    // rebuild the card pool + sort + relayout (on catalogChanged)
    void refreshCovers();              // a lazy cover landed → repaint visible cards (on coversReady)
    void onCardSizeChanged(int cardW); // model card-size changed → re-check buttons + re-scale + relayout

private:
    void buildUi();
    void buildCards();                 // (re)create the card pool from the catalog (hydrated groups only)
    void sortAndFilter();              // sort the pool + apply the search → LibraryVisible + series groups
    void relayout();                   // prescale covers + layout at the current card width

    AppModel &Model;
    LibraryView *View = nullptr;
    QList<LibraryGameCard *> *LibraryGameCards = new QList<LibraryGameCard *>();
    QList<LibraryGameCard *>  LibraryVisible;     // the search-filtered subset actually shown
    QString                   LibrarySearch;      // case-insensitive title filter ("" = show all)
    SortMode                  CurrentSort = SortMode::Name;
    QSet<QString>             LibraryCollapsedSeries;  // series names collapsed in Series view (persisted)
};

#endif // LIBRARYTAB_H
