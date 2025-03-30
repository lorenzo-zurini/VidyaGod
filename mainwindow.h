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

#include "nlohmann/json.hpp"
#include "runner.h"

QT_BEGIN_NAMESPACE
namespace Ui
{
    class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void on_TestButton_clicked();
    void on_AddGameButton_clicked();
    void on_PlayGameButton_clicked();
    void on_EditGameButton_clicked();
    void on_CreatePackageButton_clicked();

private:
    Ui::MainWindow *ui;
    QString ApplicationPath;
    QString ProtonPath;
    nlohmann::ordered_json * GlobalConfigJSON;


    void InitClassVariables();




    void BuildUI();
    bool SaveGlobalConfigJSON();
};
#endif // MAINWINDOW_H
