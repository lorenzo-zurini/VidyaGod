#ifndef DBOPERATIONS_H
#define DBOPERATIONS_H

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include <QSqlDatabase>
#include <QSqlRelationalTableModel>
#include <QSqlQuery>

#include <QDir>
#include <QTableView>

class DBOps
{
public:
    DBOps();

    static QSqlDatabase * InitDatabase(QString FileName);
    static QSqlRelationalTableModel * InitDBTable(QString TableName, QSqlDatabase * DataBase, QJsonDocument * GlobalConfigJSON, QObject * Parent);
    static void AddPackagetoDB(QJsonDocument * MANIFESTJSON, QDir * GameDir, QSqlDatabase * GlobalDB, QJsonDocument * GlobalConfigJSON);
    static void AddSubGamestoDB(QJsonDocument * MANIFESTJSON, QSqlDatabase * GlobalDB, QJsonDocument * GlobalConfigJSON);
    static void AddJSONObjectToDB(QSqlDatabase * GlobalDB, QString DBTable, QJsonDocument * GlobalConfigJSON, QJsonObject JsonObject, QMap<QString, QString> ExtraData);
    static bool CheckPackageExists(QSqlDatabase * GlobalDB, int PackageUID);
    static QString GetItemFromOtherTableByRelation(QTableView * OtherTable, QString Relation, QString RelationColumn, QString ItemColumn);
    static QList<int> IntListFromString(QString String);
    static int GetRowByItemAndColumn(QTableView * TableView, QString Item, QString Column);
    static QString GetSelectedItemByColumn(QTableView * TableView, QString Column);
    static int GetHeaderColumn(QAbstractItemModel * Model, QString Column);
};

#endif // DBOPERATIONS_H
