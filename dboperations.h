#ifndef DBOPERATIONS_H
#define DBOPERATIONS_H

#include <QSqlDatabase>
#include <QSqlRelationalTableModel>
#include <QSqlQuery>

#include <QDir>
#include <QTableView>
#include <nlohmann/json.hpp>

class DBOps
{
public:
    DBOps();

    static QSqlDatabase * InitDatabase(QString FileName);
    static QSqlRelationalTableModel * InitDBTable(QString TableName, QSqlDatabase * DataBase, nlohmann::ordered_json * GlobalConfigJSON, QObject * Parent);
    static void AddPackagetoDB(nlohmann::ordered_json * MANIFESTJSON, QDir * GameDir, QSqlDatabase * GlobalDB, nlohmann::ordered_json * GlobalConfigJSON);
    static void AddSubGamestoDB(nlohmann::ordered_json * MANIFESTJSON, QSqlDatabase * GlobalDB, nlohmann::ordered_json * GlobalConfigJSON);
    static void AddJSONObjectToDB(QSqlDatabase * GlobalDB, QString DBTable, nlohmann::ordered_json * GlobalConfigJSON, nlohmann::ordered_json JsonObject, QMap<QString, QString> ExtraData);
    static bool CheckPackageExists(QSqlDatabase * GlobalDB, QString PackageUID);
    static QString GetItemFromOtherTableByRelation(QTableView * OtherTable, QString Relation, QString RelationColumn, QString ItemColumn);
    static QList<int> IntListFromString(QString String);
    static int GetRowByItemAndColumn(QTableView * TableView, QString Item, QString Column);
    static QString GetSelectedItemByColumn(QTableView * TableView, QString Column);
    static int GetHeaderColumn(QAbstractItemModel * Model, QString Column);
};

#endif // DBOPERATIONS_H
