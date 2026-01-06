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
#include "runner.h"

QT_BEGIN_NAMESPACE
namespace Ui
{
    class MainWindow;
}
QT_END_NAMESPACE


class LibraryGameCard : public QWidget
{
    Q_OBJECT
public:
    explicit LibraryGameCard(nlohmann::ordered_json * PassedGlogalConfigJSON, int PassedGame, int PassedSubGame, QWidget * parent);


    void InitializeClassVariables();

    //////HANDLE GRIDSIZE!
    int GridSize = 0;

signals:
    void Resized(QSize NewSize);

protected:
    void resizeEvent(QResizeEvent * event) override;
    void enterEvent(QEnterEvent * event) override;
    void leaveEvent(QEvent * event) override;

private:
    std::filesystem::path PackagePath;
    QString GameTitle;

    int Game;
    int SubGame;

    QPixmap * CoverPixmap;
    QLabel * CoverLabel;
    QLabel * TitleLabel;

    QVBoxLayout * LibraryGameCardLayout;

    QWidget * ButtonsGroupBox;
    QHBoxLayout * ButtonsGroupBoxLayout;

    QPushButton * PlayButton;
    QPushButton * EditButton;

    nlohmann::ordered_json * GlobalConfigJSON;
    nlohmann::ordered_json * MANIFESTJSON;

private slots:
    void on_GameCard_clicked();
    void on_PlayGameButton_clicked();
};
/////////////////////////////////////////////////////////////////////////////

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(nlohmann::ordered_json * PassedGlobalConfigJSON, QDir * AppDataDir, QWidget *parent = nullptr);
    ~MainWindow();

    QDir * AppDataDir;
    QFile * GlobalConfigFile;

    void RebuildDynamicUI();

private slots:
    void on_AddGameButton_clicked();
    void MainWindowGridSizeChanged();

private:
    Ui::MainWindow *ui;
    QString ApplicationPath;
    QString ProtonPath;
    nlohmann::ordered_json * GlobalConfigJSON;

    QTabWidget * MainWindowTabWidget = nullptr;

    QWidget * LibraryTabWidget;
    QVBoxLayout * LibraryTabWidgetLayout;
    QScrollArea * LibraryScrollArea;
    QList<LibraryGameCard *> * LibraryGameCards = new QList<LibraryGameCard *>({});

    QWidget * PackagesTabWidget;
    QVBoxLayout * PackagesTabWidgetLayout;
    QScrollArea * PackagesScrollArea;

    //QSlider * GridSizeSlider;

    void BuildStaticUI();
    void BuildLibraryGameCards();
    void BuildLibraryDynamicUI();
    void BuildPackagesDynamicUI();
    bool SaveGlobalConfigJSON();

protected:
    void resizeEvent(QResizeEvent *event) override;
};

#endif // MAINWINDOW_H
