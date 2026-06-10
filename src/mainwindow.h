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
    QPixmap                CoverOriginal;
    QPixmap                CoverScaled;
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

    void setCards(QList<LibraryGameCard *> * cards);
    void prescaleCovers(int cardW);
    void layoutCards(int cardW);
    void setSeriesGroups(const QVector<SeriesGroup> & groups);
    //Re-scale covers/titles at the CURRENT width and repaint, WITHOUT resetting the layout (used when a
    //card's data changed in place — e.g. after a package edit). No-op if the view hasn't been laid out yet.
    void refreshVisuals();

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
    int HoveredIdx = -1;
    int CardW = 0, CardH = 0;
    int LastCols = 0, LastHGap = 0;

    static constexpr int MinGap = 16;
    static constexpr int VPad   = 16;
    static constexpr int EditW  = 30;
    static constexpr int LineH  = 30;

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

    void BuildStaticUI();
    void BuildLibraryGameCards();
    void BuildLibraryDynamicUI();
    void BuildPackagesDynamicUI();
    void BuildSettingsTab();
    void RebuildSettingsRunnersPage();
    QWidget * BuildPathsSettingsPage();
    void sortCards();
    bool SaveGlobalConfigJSON();

protected:
    void closeEvent(QCloseEvent * e) override;
};

#endif // MAINWINDOW_H
