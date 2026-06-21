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
#include <QKeyEvent>
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
#include "ipfswrapper.h"
#include "libraryview.h"   // LibraryGameCard + LibraryView + IpfsFetchReady (moved out of this header)


class DownloadManager;   // owns the Catalog download lifecycle (extracted); a friend so it can drive the IPFS tab + cards
class IpfsTab;           // the extracted IPFS tab widget; a friend so it can read the catalog/config
class SettingsTab;       // the extracted Settings tab widget; a friend so it can read the catalog/config + rebuild

// ─────────────────────────────────────────────────────────────────────────────
// MainWindow
// ─────────────────────────────────────────────────────────────────────────────
class MainWindow : public QMainWindow
{
    Q_OBJECT
    friend class DownloadManager;
    friend class IpfsTab;
    friend class SettingsTab;
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

    // The global cross-bundle node graph — the node-native catalog source. Rebuilt from the configured repos on
    // each RebuildDynamicUI; owned here so every LibraryGameCard / PreLaunchWindow can borrow a stable pointer.
    NodeIndex CatalogIndex;
    // Repo display name for a bundle dir (matches the dir against the configured repo clone roots; "Local" if none).
    QString RepoNameForBundle(const std::filesystem::path & BundleDir) const;

    QTabWidget  * MainWindowTabWidget;
    QWidget     * LibraryTabWidget;
    QVBoxLayout * LibraryTabWidgetLayout;
    enum class SortMode { Name = 0, Date = 1, Series = 2 };

    LibraryView * View           = nullptr;
    int           CardPixelWidth = 185;
    SortMode      CurrentSort    = SortMode::Name;

    QList<LibraryGameCard *> * LibraryGameCards = new QList<LibraryGameCard *>();
    QList<LibraryGameCard *>   LibraryVisible;     // the search-filtered subset actually shown by the view
    QString                    LibrarySearch;      // case-insensitive title filter ("" = show all)
    QSet<QString>              LibraryCollapsedSeries;  // series names collapsed in Series view (persisted)

    QWidget     * PackagesTabWidget;
    QVBoxLayout * PackagesTabWidgetLayout;
    QScrollArea * PackagesScrollArea;
    // (the Settings tab — sidebar + Runners/Repos/Downloads/Paths pages — is now SettingsTab; see SettingsTabPtr)

    // ── Available tab (repo games to discover + download over IPFS) — same grid as Library, grouped by repo ──
    QWidget     * AvailableTabWidget = nullptr;
    LibraryView * AvailableView      = nullptr;   // shares the Library card grid (download-on-hover, grouped)
    QList<LibraryGameCard *> * AvailableGameCards = new QList<LibraryGameCard *>();  // full pool (all un-hydrated tiles)
    QList<LibraryGameCard *>   AvailableVisible;   // the search-filtered subset actually shown (no card recreation)
    QSet<QString> AvailableCollapsedRepos;        // repo names whose section is collapsed (persisted in Settings)
    QString       AvailableSearch;                // case-insensitive title filter for the Available grid
    DownloadManager * DownloadMgr = nullptr;      // owns the Catalog download lifecycle (dialog + worker + bookkeeping)
    SortMode      AvailableSort = SortMode::Name;  // within-group ordering on the Available grid

    // ── IPFS tab — extracted into its own widget class (src/ipfstab.{h,cpp}) ──
    IpfsTab       * IpfsTabPtr       = nullptr;
    SettingsTab   * SettingsTabPtr   = nullptr;   // Settings tab (src/settingstab.{h,cpp})
    QTimer        * CoverRefreshTimer = nullptr;  // debounces lazy cover-ready bursts into one card refresh

    void BuildStaticUI();
    void BuildLibraryGameCards();
    void BuildLibraryDynamicUI();
    void BuildPackagesDynamicUI();
    void BuildAvailableTab();
    void RebuildAvailableTab();   // EXPENSIVE: rebuild the full card pool (group enumeration + hydration stats); then filter
    void ApplyAvailableFilter();  // CHEAP: filter the existing pool by search + sort + group; no rebuild/restat (sort/search path)
    void sortCards();
    bool SaveGlobalConfigJSON();

protected:
    void closeEvent(QCloseEvent * e) override;
};

#endif // MAINWINDOW_H
