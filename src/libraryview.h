#ifndef LIBRARYVIEW_H
#define LIBRARYVIEW_H

#include <QAbstractScrollArea>
#include <QPixmap>
#include <QFont>
#include <QFontMetrics>
#include <QVector>
#include <QList>
#include <QString>
#include <QColor>
#include <QResizeEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QPaintEvent>

#include <string>
#include <vector>
#include <filesystem>

#include "nlohmann/json.hpp"
#include "manifestmodel.h"   // NodeIndex / Node

// True if the embedded in-process IPFS node can fetch content right NOW (started AND its network stack is up).
// Otherwise shows the reason (a QMessageBox parented to `parent`) and returns false. There is no external daemon.
// Lives here because the card grid's play()/download paths are its primary callers; MainWindow uses it too.
bool IpfsFetchReady(QWidget * parent);

// ─────────────────────────────────────────────────────────────────────────────
// LibraryGameCard — plain data class (one tile), NOT a widget. A tile = a GROUP of launchable nodes (the
// editions); it reads cover/title/sort from the group's representative (RECOMMENDED/first) node META and carries
// the per-card actions (play/edit). Rendered by LibraryView. Used by both the Library and Available tabs.
// ─────────────────────────────────────────────────────────────────────────────
class LibraryGameCard
{
public:
    LibraryGameCard(nlohmann::ordered_json * globalConfig, const NodeIndex * index,
                    std::vector<std::string> groupNodeIds);

    void InitializeClassVariables();
    void play();
    void edit();

    const NodeIndex *        Index = nullptr;          // shared global node graph (owned by MainWindow)
    std::vector<std::string> GroupNodeIds;             // the tile's launchable node ids (editions; RECOMMENDED first)
    std::string              RepNodeId;                // representative node (front of GroupNodeIds) for meta/cover/sort
    QString                  GroupKey;                 // tile grouping key (download tracking, repo grouping)
    std::string              RepUid;                   // representative node UID (USERSETTINGS key)
    QString                  GameTitle;
    std::filesystem::path    PackagePath;              // representative node's bundle dir
    bool                   Downloading = false;   // Available tab: an import is in flight (paints a "Downloading…" overlay)
    double                 DownloadPercent = -1.0; // 0..100 aggregate IPFS progress while Downloading; -1 = unknown
    QPixmap                CoverOriginal;
    QPixmap                CoverScaled;
    QString                CoverCid;      // cover's ipfs CID while not yet local (drives lazy-load refresh)
    QString                ElidedTitle;   // cached — recomputed only when card width changes

    // Catalog (Available) per-package rendering: a package's MAIN (base) game holds its other un-downloaded games as
    // Secondaries, drawn as small tiles beneath it. Secondary cards are flagged so the top-level grid skips them.
    bool                          IsSecondary = false;
    std::vector<LibraryGameCard*> Secondaries;     // non-owning (all cards live in the AvailableGameCards pool)
    QPixmap                       CoverSmall;       // cover scaled to the small secondary-tile size

    // Sort keys — populated in InitializeClassVariables()
    QString                SortTitle;     // title with leading "The " stripped, lowercased
    QString                SortDate;      // RELEASEDATE string (ISO → lexicographic = chronological)
    QString                SortSeriesKey; // "SeriesName|##|SubseriesName|##" zero-padded
    QString                SeriesName;    // raw series name for grouping (empty if none)

    nlohmann::ordered_json * GlobalConfigJSON = nullptr;
};


// ─────────────────────────────────────────────────────────────────────────────
// LibraryView — QAbstractScrollArea, software-rendered viewport.
// Adaptive spacing: gap = (vW - cols*cardW) / (cols+1), grows as window widens.
// Integer gap changes every ~(cols+1) pixels of drag, so full repaints are rare.
// ─────────────────────────────────────────────────────────────────────────────
class LibraryView : public QAbstractScrollArea
{
    Q_OBJECT
public:
    explicit LibraryView(QWidget * parent = nullptr);

    struct SeriesGroup { int first, last; QColor color; QString name; };
    //A collapsible section over a CONTIGUOUS card-index range (cards must be pre-sorted so each group is a run).
    //Used by the Available tab to group by repository; empty ⇒ the flat grid (Library tab, unchanged).
    struct Group { QString name; int first, last; bool collapsed; };

    void setCards(QList<LibraryGameCard *> * cards);
    void prescaleCovers(int cardW);
    void layoutCards(int cardW);
    void setSeriesGroups(const QVector<SeriesGroup> & groups);
    //Collapsible repo sections (empty clears them → flat layout). Replaces the hover/edit affordances per the
    //action mode set via setHoverAction.
    void setGroups(const QVector<Group> & groups);
    //Hover-button caption + click behaviour: emitDownload=true makes a card click emit downloadRequested
    //(Available tab) instead of calling play(); label is the centered button text (e.g. "⬇  Download").
    void setHoverAction(const QString & label, bool emitDownload);
    //Centered hint painted when there are no cards (first-run guidance).
    void setEmptyMessage(const QString & message);
    //Re-scale covers/titles at the CURRENT width and repaint, WITHOUT resetting the layout (used when a
    //card's data changed in place — e.g. after a package edit). No-op if the view hasn't been laid out yet.
    void refreshVisuals();

signals:
    void downloadRequested(LibraryGameCard * card);   // Available tab: a card was clicked to import it
    void cancelRequested(LibraryGameCard * card);      // Available tab: a downloading card was clicked to cancel
    void groupToggled(const QString & name, bool collapsed);   // a section header was collapsed/expanded

protected:
    void resizeEvent(QResizeEvent * e) override;
    void scrollContentsBy(int, int) override;
    void wheelEvent(QWheelEvent * e) override;
    void keyPressEvent(QKeyEvent * e) override;
    bool viewportEvent(QEvent * e) override;

private:
    void onPaint(QPaintEvent * e);
    void onMouseMove(QMouseEvent * e);
    void onMousePress(QMouseEvent * e);
    void onLeave();
    // Keyboard navigation over the visible cards (skips collapsed/empty rects).
    void moveSelection(int dx, int dy);   // dx/dy in {-1,0,1}: step left/right or up/down by geometry
    void setSelected(int i);              // select + scroll into view + repaint
    void ensureCardVisible(int i);
    void activateCard(int i);             // Enter/Space: play() or emit downloadRequested(), per mode

    QList<LibraryGameCard *> * Cards = nullptr;
    QVector<QRect>             Rects;
    QVector<QVector<QRect>>    SecRects;     // SecRects[i] = small secondary-tile rects under card i (Catalog per-package)
    QVector<SeriesGroup>       SeriesGroups;
    QVector<Group>             Groups;       // collapsible repo sections; empty ⇒ flat grid
    QVector<QRect>             HeaderRects;  // one per Group (content coords); for collapse hit-testing
    QString                    HoverLabel = "▶  Play";   // centered hover-button caption
    QString                    EmptyMessage;             // shown centered when there are no cards
    bool                       EmitDownload = false;          // card click → downloadRequested vs play()
    bool                       ShowEditCorner = true;         // draw/handle the "···" edit corner
    int HoveredIdx = -1;
    int HoveredSecIdx = -1;   // when HoveredIdx is a package: which secondary sub-tile is hovered (-1 = the main tile)
    int SelectedIdx = -1;   // keyboard-focused card (-1 = none); painted with a focus ring
    int CardW = 0, CardH = 0;
    int LastCols = 0, LastHGap = 0;
    int ContentH = 0;   // total laid-out content height (cached so a vertical-only resize updates the scrollbar)

    static constexpr int MinGap   = 16;
    static constexpr int VPad     = 16;
    static constexpr int EditW    = 30;
    static constexpr int LineH    = 30;
    static constexpr int HeaderH  = 34;   // collapsible section header band height
    static constexpr int SecPerRow = 3;   // max secondary (smaller) tiles per row under a package's main game
    static constexpr int SecGap    = 8;   // gap around/between secondary tiles
    // Small secondary-tile width: ~45% of the main width, but never so wide that >SecPerRow won't fit.
    int  secTileW() const { return qMin(int(CardW * 0.46), (CardW - (SecPerRow + 1) * SecGap) / SecPerRow); }
    int  secTileH() const { return secTileW() * 3 / 2; }
    // Extra cell height a package needs below its main tile for n secondary tiles (0 if none).
    int  secBlockH(int n) const { return n <= 0 ? 0 : SecGap + ((n + SecPerRow - 1) / SecPerRow) * (secTileH() + SecGap); }

    QFont        TitleFont, PlayFont;
    QFontMetrics TitleFM;
};

#endif // LIBRARYVIEW_H
