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
#include <QProcess>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>

#include <QFileDialog>
#include <QMessageBox>
#include <QTableView>

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
    QDir * ProtonPath;
    QFile * GlobalConfigFile;
    QJsonDocument * GlobalConfigJSON;
    QSqlDatabase * GlobalDB;
    QSqlRelationalTableModel * LibraryModel;
    QSqlRelationalTableModel * PackagesModel;

    void InitClassVariables();
    void InitGlobalConfigJSON();
    void InitDatabaseAndModel();
    QSqlRelationalTableModel * InitDBTable(QString TableName, QObject * Parent, QSqlDatabase * DataBase);
    QJsonDocument * LoadJSON(QFile * JSONFile);
    void SaveJSON(QJsonDocument * JSONDocument, QFile * JSONFile);
    void AddPackagetoDB(QJsonDocument * MANIFESTJSON, QDir * GameDir);
    void AddSubGamestoDB(QJsonDocument * MANIFESTJSON);
    void AddJSONObjectToDB(QString DBTable, QJsonObject MANIFESTJSON, QMap<QString, QString> ExtraData);

    QJsonArray GetSubComponents(QDir *PackageDir, QList<int> Recipe);
    bool InitializeUMUPrefix(QDir *DefPrefixDir, QString * UnionFSString);
    bool ProcessSubComponents(const QJsonArray SubComponentsArray, QDir * TempDir, QDir * PackageFilesDir, QDir * DefPrefixDir, QDir * ProgramRuntimeDir, QString * UnionFSString, const QString ParentPackage);
    bool RemoveSubComponents(QJsonArray SubComponentsArray, QDir * TempDir, QDir * PackageFilesDir, QString * UnionFSString, const QString ParentPackage);
    bool MountZipFileLayer(const QJsonObject SubComponentJSON, int LayerNumber, QDir * TempDir, QDir * PackageFilesDir, QString * UnionFSString, const QString ParentPackage);
    bool UnmountZipFileLayer(const QJsonObject SubComponentJSON, int LayerNumber, QDir * TempDir, const QString ParentPackage);
    bool RegEdit(QJsonObject SubComponentJSON, QDir * DefPrefixDir, QDir * ProgramRuntimeDir);
    bool RegAdd();
    bool BuildUnionFS(QString * UnionFSString, QDir * RuntimeDir, QDir * UserDataDir);
    bool DestroyUnionFS(QDir * RuntimeDir);
    bool RunWithUMU(QDir * WorkDir,  QDir * WinePrefix, QString GAMEID = "0", QFile * ExeFile = new QFile(""), QStringList * ExeArgs = new QStringList({}));

    QList<int> IntListFromString(QString String);
    QString GetSelectedItemByColumn(QTableView * TableView, QString Column);
    QString GetItemFromOtherTableByRelation(QTableView * OtherTable, QString Relation, QString RelationColumn, QString ItemColumn);
    int GetRowByItemAndColumn(QTableView * TableView, QString Item, QString Column);
    int GetHeaderColumn(QAbstractItemModel * Model, QString Column);
    void ResetTables();
};
#endif // MAINWINDOW_H
