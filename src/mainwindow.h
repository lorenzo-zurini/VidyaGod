#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QAbstractScrollArea>
#include <QFile>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QBoxLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QResizeEvent>
#include <QLabel>
#include <QPixmap>
#include <QFont>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QVector>
#include <QScrollBar>
#include <QListWidget>
#include <QStackedWidget>
#include <QComboBox>
#include <QLineEdit>
#include <QFormLayout>
#include <QTableWidget>
#include <QTreeWidget>
#include <QTimer>
#include <QHash>
#include <QSet>

#include "nlohmann/json.hpp"
#include "containerwrapper.h"


// ─────────────────────────────────────────────────────────────────────────────
// LibraryGameCard — plain data class, NOT a widget.
// ─────────────────────────────────────────────────────────────────────────────
class LibraryGameCard
{
public:
    LibraryGameCard(nlohmann::ordered_json * globalConfig, int game, std::string subgameId);
    ~LibraryGameCard();

    void InitializeClassVariables();
    void play();
    void edit();

    int                    Game;
    std::string            SubgameID;
    QString                GameTitle;
    std::filesystem::path  PackagePath;
    bool                   Downloading = false;   // Available tab: an import is in flight (paints a "Downloading…" overlay)
    QPixmap                CoverOriginal;
    QPixmap                CoverScaled;
    QString                CoverCid;      // cover's ipfs CID while not yet local (drives lazy-load refresh)
    QString                ElidedTitle;   // cached — recomputed only when card width changes

    // Sort keys — populated in InitializeClassVariables()
    QString                SortTitle;     // title with leading "The " stripped, lowercased
    QString                SortDate;      // RELEASEDATE string (ISO → lexicographic = chronological)
    QString                SortSeriesKey; // "SeriesName|##|SubseriesName|##" zero-padded
    QString                SeriesName;    // raw series name for grouping (empty if none)

    nlohmann::ordered_json * GlobalConfigJSON = nullptr;
    nlohmann::ordered_json * MANIFESTJSON     = nullptr;
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
    //Re-scale covers/titles at the CURRENT width and repaint, WITHOUT resetting the layout (used when a
    //card's data changed in place — e.g. after a package edit). No-op if the view hasn't been laid out yet.
    void refreshVisuals();

signals:
    void downloadRequested(LibraryGameCard * card);   // Available tab: a card was clicked to import it
    void groupToggled(const QString & name, bool collapsed);   // a section header was collapsed/expanded

protected:
    void resizeEvent(QResizeEvent * e) override;
    void scrollContentsBy(int, int) override;
    void wheelEvent(QWheelEvent * e) override;
    bool viewportEvent(QEvent * e) override;

private:
    void onPaint(QPaintEvent * e);
    void onMouseMove(QMouseEvent * e);
    void onMousePress(QMouseEvent * e);
    void onLeave();

    QList<LibraryGameCard *> * Cards = nullptr;
    QVector<QRect>             Rects;
    QVector<SeriesGroup>       SeriesGroups;
    QVector<Group>             Groups;       // collapsible repo sections; empty ⇒ flat grid
    QVector<QRect>             HeaderRects;  // one per Group (content coords); for collapse hit-testing
    QString                    HoverLabel = "▶  Play";   // centered hover-button caption
    bool                       EmitDownload = false;          // card click → downloadRequested vs play()
    bool                       ShowEditCorner = true;         // draw/handle the "···" edit corner
    int HoveredIdx = -1;
    int CardW = 0, CardH = 0;
    int LastCols = 0, LastHGap = 0;
    int ContentH = 0;   // total laid-out content height (cached so a vertical-only resize updates the scrollbar)

    static constexpr int MinGap   = 16;
    static constexpr int VPad     = 16;
    static constexpr int EditW    = 30;
    static constexpr int LineH    = 30;
    static constexpr int HeaderH  = 34;   // collapsible section header band height

    QFont        TitleFont, PlayFont;
    QFontMetrics TitleFM;
};


// ─────────────────────────────────────────────────────────────────────────────
// MainWindow
// ─────────────────────────────────────────────────────────────────────────────
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    MainWindow(nlohmann::ordered_json * GlobalConfigJSON, QDir * AppDataDir, QWidget * parent = nullptr);
    ~MainWindow();

    QDir  * AppDataDir;
    QFile * GlobalConfigFile;
    void RebuildDynamicUI();

    //Reacts to a package being saved in the editor: re-renders the matching game card(s) in place (no
    //delete, so a borrowing prelaunch's manifest pointer stays valid) and reloads any open prelaunch
    //dialog for that package. Static + self-locating so any editor (wherever opened) can drive it.
    static void RefreshPackage(const QString & PackagePath);

private slots:
    void on_AddGameButton_clicked();
    void MainWindowGridSizeChanged();

private:
    nlohmann::ordered_json * GlobalConfigJSON;

    QTabWidget  * MainWindowTabWidget;
    QWidget     * LibraryTabWidget;
    QVBoxLayout * LibraryTabWidgetLayout;
    enum class SortMode { Name = 0, Date = 1, Series = 2 };

    LibraryView * View           = nullptr;
    int           CardPixelWidth = 185;
    SortMode      CurrentSort    = SortMode::Name;

    QList<LibraryGameCard *> * LibraryGameCards = new QList<LibraryGameCard *>();

    QWidget     * PackagesTabWidget;
    QVBoxLayout * PackagesTabWidgetLayout;
    QScrollArea * PackagesScrollArea;

    // ── Settings tab (global configurator: sidebar + stacked category forms) ──
    QWidget        * SettingsTabWidget     = nullptr;
    QListWidget    * SettingsCategoryList  = nullptr;
    QStackedWidget * SettingsStack         = nullptr;
    QScrollArea    * SettingsRunnersScroll = nullptr; // rebuilt in place by RebuildSettingsRunnersPage()
    QScrollArea    * SettingsReposScroll   = nullptr; // rebuilt in place by RebuildSettingsReposPage()

    // ── Available tab (repo games to discover + download over IPFS) — same grid as Library, grouped by repo ──
    QWidget     * AvailableTabWidget = nullptr;
    LibraryView * AvailableView      = nullptr;   // shares the Library card grid (download-on-hover, grouped)
    QList<LibraryGameCard *> * AvailableGameCards = new QList<LibraryGameCard *>();
    QSet<QString> AvailableCollapsedRepos;        // repo names whose section is collapsed (persisted in Settings)
    QSet<QString> DownloadingUids;                // PACKAGEUIDs with an import in flight (survives rebuilds)
    SortMode      AvailableSort = SortMode::Name;  // within-group ordering on the Available grid

    // ── IPFS tab (Kubo transfers + seeded content; greyed when ipfs is absent) ──
    QWidget       * IpfsTabWidget   = nullptr;
    QLabel        * IpfsStatusLabel = nullptr;
    QTableWidget  * IpfsTransfers   = nullptr;   // Name | CID | Progress | Status — live fetches (sortable)
    QTreeWidget   * IpfsPins        = nullptr;   // pinned (seeded) CIDs, grouped by package (sortable)
    QTimer        * IpfsRefreshTimer = nullptr;
    QTimer        * CoverRefreshTimer = nullptr;  // debounces lazy cover-ready bursts into one card refresh
    QHash<QString, QTableWidgetItem*> IpfsTransferProgress; // CID → progress cell item (survives re-sorting)
    QHash<QString, QString> IpfsCidLabels;        // CID → human label ("<package> — <component>")
    QHash<QString, QString> IpfsCidPackages;      // CID → owning package name (for grouping)

    void BuildStaticUI();
    void BuildLibraryGameCards();
    void BuildLibraryDynamicUI();
    void BuildPackagesDynamicUI();
    void BuildSettingsTab();
    void RebuildSettingsRunnersPage();
    void RebuildSettingsReposPage();
    QWidget * BuildPathsSettingsPage();
    void BuildAvailableTab();
    void RebuildAvailableTab();   // grouped grid of un-hydrated repo games; click a card to download over IPFS
    void DownloadAvailable(LibraryGameCard * card);   // import (hydrate) the package behind an Available card
    void BuildIpfsTab();
    void RefreshIpfsTab();   // refresh status + seeded list (no-op when ipfs absent)
    void sortCards();
    bool SaveGlobalConfigJSON();

protected:
    void closeEvent(QCloseEvent * e) override;
};

#endif // MAINWINDOW_H
