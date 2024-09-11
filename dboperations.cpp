#include "dboperations.h"

DBOperations::DBOperations() {}

QSqlDatabase * DBOperations::InitDatabase(QString FileName)
{
    qDebug() << QTime::currentTime().toString() << " [OUT] Attempting database init... ";
    QSqlDatabase * NewDataBase = new QSqlDatabase(QSqlDatabase::addDatabase("QSQLITE"));
    NewDataBase->setDatabaseName(FileName);

    if (NewDataBase->open())
    {
        qDebug() << QTime::currentTime().toString() << " [OUT] " << NewDataBase->databaseName() << " opened successfully.";
        return NewDataBase;
    }
    else
    {
        qDebug() << QTime::currentTime().toString() << " [ERR] " << FileName << " could not be opened.";
        return nullptr;
    }
}

QSqlRelationalTableModel * DBOperations::InitDBTable(QString TableName, QSqlDatabase * DataBase, QJsonDocument * GlobalConfigJSON, QObject * Parent)
{
    if (!DataBase->tables().contains(TableName))
    {
        QString QueryString;
        QueryString.append("CREATE TABLE IF NOT EXISTS " + TableName + " (");

        for (auto Key : (*GlobalConfigJSON)["DefaultTables"][TableName]["COLUMNS"].toObject().keys())
        {
            QueryString.append("\"" + Key + "\" " + (*GlobalConfigJSON)["DefaultTables"][TableName]["COLUMNS"].toObject().value(Key).toString() + ", ");
        }
        QueryString.append("PRIMARY KEY(\"" + (*GlobalConfigJSON)["DefaultTables"][TableName]["PRIMARY KEY"].toString() + "\"))");
        QSqlQuery(*DataBase).exec(QueryString);
    }

    QSqlRelationalTableModel * Model = new QSqlRelationalTableModel(Parent, *DataBase);
    Model->setTable(TableName);
    Model->select();

    return Model;
}

void DBOperations::AddPackagetoDB(QJsonDocument * MANIFESTJSON, QDir * GameDir, QSqlDatabase * GlobalDB, QJsonDocument * GlobalConfigJSON)
{
    qDebug() << QTime::currentTime().toString() << " [OUT] Adding to library: " << (*MANIFESTJSON)["PACKAGENAME"].toString() << " UID: " << (*MANIFESTJSON)["PACKAGEUID"].toString();

    QMap<QString, QString> PackagePathMap;
    PackagePathMap["PATH"] = GameDir->path();

    AddJSONObjectToDB(GlobalDB, "PACKAGES", GlobalConfigJSON, (*MANIFESTJSON).object(), PackagePathMap);
}

void DBOperations::AddSubGamestoDB(QJsonDocument * MANIFESTJSON, QSqlDatabase * GlobalDB, QJsonDocument * GlobalConfigJSON)
{
    qDebug() << QTime::currentTime().toString() << " [OUT] Adding subgames to library... ";

    QMap<QString, QString> ParentPackageMap;
    ParentPackageMap["PARENTPACKAGE"] = (*MANIFESTJSON)["PACKAGEUID"].toString();

    for (auto SubGameObject : (*MANIFESTJSON)["SUBGAMES"].toArray())
    {
        AddJSONObjectToDB(GlobalDB, "LIBRARY", GlobalConfigJSON, SubGameObject.toObject(), ParentPackageMap);
    }
}

void DBOperations::AddJSONObjectToDB(QSqlDatabase * GlobalDB, QString DBTable, QJsonDocument * GlobalConfigJSON, QJsonObject JsonObject, QMap<QString, QString> ExtraData)
{
    QString QueryString;
    QString ColumnString;
    QString ValueString;

    //ITERATE OVER ENTRIES IN THE TABLE DEFINITIONS IN GLOBALCONFIGJSON
    for (auto Key : (*GlobalConfigJSON)["DefaultTables"][DBTable]["COLUMNS"].toObject().keys())
    {
        //ADD THE CORRESPONDING VALUES TO THE SQLQUERY, SKIP IF EMPTY
        if (JsonObject.value(Key).toString().isEmpty())
        {
            continue;
        }
        //KEYS THAT ARE NOT IN THE TABLE DEFINITIONS IN DefaultConfig.JSON will not be added
        if (JsonObject.keys().contains(Key))
        {
            ColumnString.append("\"" + Key + "\", ");
            ValueString.append("\"" + JsonObject.value(Key).toString() + "\", ");
        }
    }

    //DO THE SAME FOR VALUES PASSED AS THE EXTRADATA QMAP
    for (auto [Column, Value] : ExtraData.asKeyValueRange())
    {
        if (Value.isEmpty())
        {
            continue;
        }
        if ((*GlobalConfigJSON)["DefaultTables"][DBTable]["COLUMNS"].toObject().keys().contains(Column))
        {
            ColumnString.append("\"" + Column + "\", ");
            ValueString.append("\"" + Value + "\", ");
        }
    }

    //REMOVE THE TRAILING COMMA AND SPACE
    ColumnString.chop(2);
    ValueString.chop(2);

    //CONSTRUCT THE QUERY AND EXECUTE
    QueryString.append("INSERT INTO " + DBTable + " (" + ColumnString + ") VALUES (" + ValueString + ")");
    QSqlQuery(*GlobalDB).exec(QueryString);
}

bool DBOperations::CheckPackageExists(QSqlDatabase * GlobalDB, int PackageUID)
{
    QSqlQuery CheckPackageExistsQuery(*GlobalDB);
    CheckPackageExistsQuery.exec("SELECT PACKAGEUID FROM PACKAGES;");
    while (CheckPackageExistsQuery.next())
    {
        if (CheckPackageExistsQuery.value(0).toInt() == PackageUID)
        {
            qDebug() << QTime::currentTime().toString() << " [ERR] Package already exists in library, aborting!";
            return true;
        }
    }
    return false;
}
