#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

#include <QFile>

#include <QSqlError>
#include <QSqlDatabase>
#include <QSqlRelationalTableModel>
#include <QSqlQuery>
#include <QSqlRecord>

#include <QStandardItemModel>

#include <iostream>
#include <fstream>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>

#include <QFileDialog>

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

private:
    Ui::MainWindow *ui;

    QDir * ApplicationDirectory;
    QFile * GlobalConfigFile;
    QJsonDocument * GlobalConfigJSON;
    QSqlDatabase * GlobalDB;
    QSqlRelationalTableModel * LibraryModel;
    QSqlRelationalTableModel * PackagesModel;

    void InitClassVariables();
    void InitGlobalConfigJSON();
    void InitDatabaseAndModel();
    void InitDBTable(QString TableName);
    QJsonDocument * LoadJSON(QFile * JSONFile);
    void SaveJSON(QJsonDocument * JSONDocument, QFile * JSONFile);
    void AddPackagetoDB(QJsonDocument * MANIFESTJSON, QDir * GameDir);
    void AddSubGamestoDB(QJsonDocument * MANIFESTJSON);
    void AddJSONObjectToDB(QString DBTable, QJsonObject MANIFESTJSON, QMap<QString, QString> ExtraData);
    void ResetTables();
};
#endif // MAINWINDOW_H
