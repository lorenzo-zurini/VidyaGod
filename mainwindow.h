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

    void on_CreatePackageButton_clicked();

private:
    Ui::MainWindow *ui;

    QString ApplicationPath;
    QString ProtonPath;
    //QFile * GlobalConfigFile;
    nlohmann::ordered_json * GlobalConfigJSON;
    QSqlDatabase * GlobalDB;
    QSqlRelationalTableModel * LibraryModel;
    QSqlRelationalTableModel * PackagesModel;

    void InitClassVariables();


    bool InitializeUMUPrefix(QVariantMap * RunnerParams);
    bool ProcessSubComponents(const nlohmann::ordered_json SubComponentsArray, QDir * TempDir, QDir * PackageFilesDir, QDir * DefPrefixDir, QDir * ProgramRuntimeDir, QString * UnionFSString, const QString ParentPackage);
    bool RemoveSubComponents(nlohmann::ordered_json SubComponentsArray, QDir * TempDir, QDir * PackageFilesDir, QString * UnionFSString, const QString ParentPackage);
    bool MountZipFileLayer(const nlohmann::ordered_json SubComponentJSON, int LayerNumber, QDir * TempDir, QDir * PackageFilesDir, QString * UnionFSString, const QString ParentPackage);
    bool UnmountZipFileLayer(const nlohmann::ordered_json SubComponentJSON, int LayerNumber, QDir * TempDir, const QString ParentPackage);
    bool RegEdit(nlohmann::ordered_json SubComponentJSON, QDir * DefPrefixDir, QDir * ProgramRuntimeDir);
    bool RegAdd();
    bool BuildUnionFS(QString * UnionFSString, QDir * RuntimeDir, QDir * UserDataDir);
    bool DestroyUnionFS(QDir * RuntimeDir);
    bool RunWithUMU(QString WorkDirPath, QString WinePrefix, QString GAMEID, QString ExePath, QStringList ExeArgs);


    void ResetTables();
    bool CheckCaseConflicts(QDir * RuntimeDir);
};
#endif // MAINWINDOW_H
