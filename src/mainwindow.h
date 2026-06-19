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

    // ── Settings tab (global configurator: sidebar + stacked category forms) ──
    QWidget        * SettingsTabWidget     = nullptr;
    QListWidget    * SettingsCategoryList  = nullptr;
    QStackedWidget * SettingsStack         = nullptr;
    QScrollArea    * SettingsRunnersScroll = nullptr; // rebuilt in place by RebuildSettingsRunnersPage()
    QScrollArea    * SettingsReposScroll   = nullptr; // rebuilt in place by RebuildSettingsReposPage()

    // ── Available tab (repo games to discover + download over IPFS) — same grid as Library, grouped by repo ──
    QWidget     * AvailableTabWidget = nullptr;
    LibraryView * AvailableView      = nullptr;   // shares the Library card grid (download-on-hover, grouped)
    QList<LibraryGameCard *> * AvailableGameCards = new QList<LibraryGameCard *>();  // full pool (all un-hydrated tiles)
    QList<LibraryGameCard *>   AvailableVisible;   // the search-filtered subset actually shown (no card recreation)
    QSet<QString> AvailableCollapsedRepos;        // repo names whose section is collapsed (persisted in Settings)
    QString       AvailableSearch;                // case-insensitive title filter for the Available grid
    QSet<QString> DownloadingUids;                // PACKAGEUIDs with an import in flight (survives rebuilds)
    QSet<QString> CancellingUids;                 // PACKAGEUIDs the user cancelled (suppresses the failure dialog)
    QHash<QString, QString>      DownloadCidToUid; // in-flight content CID → its owning PACKAGEUID
    QHash<QString, QStringList>  DownloadUidCids;  // PACKAGEUID → its content CIDs (for averaging progress)
    QHash<QString, double>       DownloadCidPct;   // in-flight CID → latest percent (0..100)
    SortMode      AvailableSort = SortMode::Name;  // within-group ordering on the Available grid

    // ── IPFS tab (embedded-node transfers + seeded content; greyed if the node didn't start) ──
    QWidget       * IpfsTabWidget   = nullptr;
    QLabel        * IpfsStatusLabel = nullptr;
    QTableWidget  * IpfsTransfers   = nullptr;   // Name|Size|Progress|Speed|Status|CID — live + queued fetches (sortable)
    QTreeWidget   * IpfsPins        = nullptr;   // pinned (seeded) CIDs, grouped by package (Name|Size|Health|CID)
    QTimer        * IpfsRefreshTimer = nullptr;
    bool            IpfsRefreshInFlight = false;   // a worker is already gathering ipfs status — skip re-entrancy
    QTimer        * CoverRefreshTimer = nullptr;  // debounces lazy cover-ready bursts into one card refresh
    QHash<QString, QTableWidgetItem*> IpfsTransferProgress; // CID → progress cell item (survives re-sorting)
    QSet<QString>   IpfsTransferQueued;           // CIDs shown as "Queued" (in a download but not yet started)
    QHash<QString, QPair<qlonglong,qlonglong>> IpfsTransferSpeed;  // CID → {sampleBytes, sampleMs} for the rate calc
    QHash<QString, QString> IpfsCidLabels;        // CID → human label ("<package> — <component>")
    QHash<QString, QString> IpfsCidPackages;      // CID → owning package name (for grouping)
    QHash<QString, IpfsWrapper::StatInfo> IpfsCidStat;  // CID → size + provider count (cached; Refresh clears it)
    QHash<QString, QTreeWidgetItem*> IpfsPinGroups;     // package name → group row (incremental tree, no rebuild)
    QHash<QString, QTreeWidgetItem*> IpfsPinChildren;   // CID → leaf row (so refresh updates in place, no jumping)
    bool            IpfsHealthInFlight = false;         // a background provider-count pass is running

    void BuildStaticUI();
    void BuildLibraryGameCards();
    void BuildLibraryDynamicUI();
    void BuildPackagesDynamicUI();
    void BuildSettingsTab();
    void RebuildSettingsRunnersPage();
    void RebuildSettingsReposPage();
    QWidget * BuildPathsSettingsPage();
    void BuildAvailableTab();
    void RebuildAvailableTab();   // EXPENSIVE: rebuild the full card pool (group enumeration + hydration stats); then filter
    void ApplyAvailableFilter();  // CHEAP: filter the existing pool by search + sort + group; no rebuild/restat (sort/search path)
    void DownloadAvailable(LibraryGameCard * card);   // import (hydrate) the package behind an Available card
    void BuildIpfsTab();
    // Ensure a transfers-table row exists for a CID (create it with `status` if absent); returns its progress item.
    QTableWidgetItem * EnsureTransferRow(const QString & cid, const QString & status);
    void RefreshIpfsTab();   // kicks an off-thread gather of status + seeded list (no-op when ipfs absent)
    void ApplyIpfsSnapshot(bool daemon, int peers, const QString & repo,
                           const std::vector<IpfsWrapper::PinEntry> & pins,
                           const QHash<QString, long long> & sizes);  // GUI-thread incremental paint of the gathered data
    void GatherIpfsHealth();   // off-thread provider-count ("network availability") pass for un-queried pins
    void sortCards();
    bool SaveGlobalConfigJSON();

protected:
    void closeEvent(QCloseEvent * e) override;
};

#endif // MAINWINDOW_H
