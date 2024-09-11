#ifndef DBOPERATIONS_H
#define DBOPERATIONS_H

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include <QSqlDatabase>
#include <QSqlRelationalTableModel>
#include <QSqlQuery>

#include <QDir>

class DBOperations
{
public:
    DBOperations();

    static QSqlDatabase * InitDatabase(QString FileName);
    static QSqlRelationalTableModel * InitDBTable(QString TableName, QSqlDatabase * DataBase, QJsonDocument * GlobalConfigJSON, QObject * Parent);
    static void AddPackagetoDB(QJsonDocument * MANIFESTJSON, QDir * GameDir, QSqlDatabase * GlobalDB, QJsonDocument * GlobalConfigJSON);
    static void AddSubGamestoDB(QJsonDocument * MANIFESTJSON, QSqlDatabase * GlobalDB, QJsonDocument * GlobalConfigJSON);
    static void AddJSONObjectToDB(QSqlDatabase * GlobalDB, QString DBTable, QJsonDocument * GlobalConfigJSON, QJsonObject JsonObject, QMap<QString, QString> ExtraData);
    static bool CheckPackageExists(QSqlDatabase * GlobalDB, int PackageUID);
};

#endif // DBOPERATIONS_H
