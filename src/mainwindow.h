#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <iostream>

#include <QFile>
#include <QDir>
#include <QDirIterator>

#include <QStandardItemModel>
#include <QProcess>

#include <QFileDialog>
#include <QMessageBox>
#include <QTableView>
#include <QPushButton>
#include <QBoxLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QResizeEvent>

#include "nlohmann/json.hpp"
#include "containerwrapper.h"

QT_BEGIN_NAMESPACE
namespace Ui
{
    class MainWindow;
}
QT_END_NAMESPACE

//A single card in the library grid, representing one subgame within a package.
//Each card shows the cover art and a Play button; one LibraryGameCard is created
//per SUBGAMES entry, so a package with multiple subgames produces multiple cards.
class LibraryGameCard : public QWidget
{
    Q_OBJECT
public:
    explicit LibraryGameCard(nlohmann::ordered_json * PassedGlogalConfigJSON, int PassedGame, std::string PassedSubgameID, QWidget * parent);

    //Loads the subgame title and cover art from the package MANIFEST.json.
    //Must be called after construction — separated from the constructor so the
    //card widget exists before any file I/O is attempted.
    void InitializeClassVariables();

    //////HANDLE GRIDSIZE!
    int GridSize = 0; //Number of columns in the library grid; set by BuildLibraryDynamicUI before layout

signals:
    void Resized(QSize NewSize); //Emitted on resize so parent can reflow if needed

protected:
    //Keeps the cover label at a 2:3 aspect ratio (width * 3/2) as the card resizes.
    void resizeEvent(QResizeEvent * event) override;
    void enterEvent(QEnterEvent * event) override;
    void leaveEvent(QEvent * event) override;

private:
    //Package location on disk — needed to load cover art and build ContainerParams.
    std::filesystem::path PackagePath;
    QString GameTitle;

    int Game;             //Index into GlobalConfigJSON["LIBRARY"] — identifies the parent package
    std::string SubgameID; //SUBGAMEID string from MANIFEST.json["SUBGAMES"]

    //Cover art label; scaled to fill the card width at a fixed 2:3 ratio.
    QPixmap * CoverPixmap;
    QLabel * CoverLabel;
    QLabel * TitleLabel;

    QVBoxLayout * LibraryGameCardLayout;

    //Button row at the bottom of the card.
    QWidget * ButtonsGroupBox;
    QHBoxLayout * ButtonsGroupBoxLayout;

    QPushButton * PlayButton; //Launches the subgame via ContainerWrapper
    QPushButton * EditButton; //Reserved for future per-game settings ("...")

    //Pointers into the application-level JSON stores — not owned by this class.
    nlohmann::ordered_json * GlobalConfigJSON;
    nlohmann::ordered_json * MANIFESTJSON; //Loaded from PackagePath/METADATA/MANIFEST.json during InitializeClassVariables

private slots:
    void on_GameCard_clicked();
    //Builds a ContainerWrapper, mounts the runtime, runs the game, waits for exit,
    //then triggers cleanup. Displays a critical dialog on launch failure.
    void on_PlayGameButton_clicked();
    //Opens PreLaunchWindow unconditionally (ignores SKIP_LAUNCH_DIALOG) for reconfiguration.
    void on_EditGameButton_clicked();
};
/////////////////////////////////////////////////////////////////////////////

//The application's main window. Owns the two-tab layout (Library / Packages),
//the list of LibraryGameCards, and the GlobalConfigJSON pointer used for persistence.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(nlohmann::ordered_json * PassedGlobalConfigJSON, QDir * AppDataDir, QWidget *parent = nullptr);
    ~MainWindow();

    QDir * AppDataDir;       //~/.VidyaGod — where GlobalConfig.JSON lives
    QFile * GlobalConfigFile;

    //Rebuilds both the Library and Packages dynamic UI from scratch.
    //Called after adding or removing a package to keep the view in sync.
    void RebuildDynamicUI();

private slots:
    //Opens a directory picker, validates the selected package, and adds a slim
    //entry (UID, name, version, path) to GlobalConfigJSON["LIBRARY"].
    void on_AddGameButton_clicked();
    //Updates LibraryGridSize in GlobalConfigJSON and redraws the library grid.
    void MainWindowGridSizeChanged();

private:
    Ui::MainWindow *ui;
    QString ApplicationPath;
    QString ProtonPath;
    nlohmann::ordered_json * GlobalConfigJSON; //Shared with LibraryGameCards — not owned here

    QTabWidget * MainWindowTabWidget; //Top-level tab container (Library | Packages)

    //Library tab — scrollable grid of LibraryGameCards.
    QWidget * LibraryTabWidget;
    QVBoxLayout * LibraryTabWidgetLayout;
    QScrollArea * LibraryScrollArea;
    QList<LibraryGameCard *> * LibraryGameCards = new QList<LibraryGameCard *>({}); //All cards, rebuilt on RebuildDynamicUI

    //Packages tab — tabular list of installed packages with remove buttons.
    QWidget * PackagesTabWidget;
    QVBoxLayout * PackagesTabWidgetLayout;
    QScrollArea * PackagesScrollArea;

    //QSlider * GridSizeSlider; //Grid size control — currently disabled

    //Builds the static skeleton (tab widget, scroll areas, toolbar buttons).
    //Must run before any dynamic UI methods.
    void BuildStaticUI();
    //Creates one LibraryGameCard per subgame across all library entries.
    //Populates LibraryGameCards; must run before BuildLibraryDynamicUI.
    void BuildLibraryGameCards();
    //Lays out LibraryGameCards into a QGridLayout inside the LibraryScrollArea.
    //Safe to call repeatedly — reparents existing cards before deleting the old widget.
    void BuildLibraryDynamicUI();
    //Populates the Packages tab with one row per library entry (name, UID, version, remove button).
    void BuildPackagesDynamicUI();
    //Persists GlobalConfigJSON to GlobalConfig.JSON on disk.
    bool SaveGlobalConfigJSON();

protected:
    void resizeEvent(QResizeEvent *event) override;
};

#endif // MAINWINDOW_H
