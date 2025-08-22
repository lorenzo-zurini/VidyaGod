#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

#include <QFile>
#include <QDir>
#include <QDirIterator>

#include <QSqlError>
#include <QSqlDatabase>
#include <QSqlRelationalTableModel>
#include <QSqlQuery>
#include <QSqlRecord>

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
#include "runner.h"

QT_BEGIN_NAMESPACE
namespace Ui
{
    class MainWindow;
}
QT_END_NAMESPACE


class LibraryGameCard : public QGroupBox
{
    Q_OBJECT
public:
    explicit LibraryGameCard(QWidget * parent = nullptr);
    void PassGlobalConfigJSON(nlohmann::ordered_json * PassedGlobalConfigJSON);
    void SetGame(int PassedGame, int PassedSubGame);
    void InitializeClassVariables();

    //////HANDLE GRIDSIZE!
    int GridSize = 0;

signals:
    void Resized(QSize NewSize);

protected:
    void resizeEvent(QResizeEvent * event) override;

private:
    QString PackagePath;
    QString GameTitle;

    int Game;
    int SubGame;

    QPixmap * CoverPixmap;
    QLabel * CoverLabel;
    QLabel * TitleLabel;

    QPushButton * PlayButton;
    QPushButton * EditButton;

    nlohmann::ordered_json * GlobalConfigJSON;
    nlohmann::ordered_json * MANIFESTJSON;

private slots:
    void on_PlayGameButton_clicked();
};
/////////////////////////////////////////////////////////////////////////////

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();
    void RebuildDynamicUI();

private slots:
    void on_TestButton_clicked();
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

    void InitClassVariables();

    void BuildStaticUI();
    void BuildLibraryGameCards();
    void BuildLibraryDynamicUI();
    void BuildPackagesDynamicUI();
    bool SaveGlobalConfigJSON();

protected:
    void resizeEvent(QResizeEvent *event) override;
};

#endif // MAINWINDOW_H
