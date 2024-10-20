#include "dboperations.h"

DBOps::DBOps() {}

QSqlDatabase * DBOps::InitDatabase(QString FileName)
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

QSqlRelationalTableModel * DBOps::InitDBTable(QString TableName, QSqlDatabase * DataBase, nlohmann::ordered_json * GlobalConfigJSON, QObject * Parent)
{
    if (!DataBase->tables().contains(TableName))
    {
        QString QueryString;
        QueryString.append("CREATE TABLE IF NOT EXISTS " + TableName + " (");

        for (auto Item : (*GlobalConfigJSON)["DefaultTables"][TableName.toStdString()]["COLUMNS"].items())
        {
            QueryString.append(Item.key() + " " + Item.value().dump() + ", ");
        }
        QueryString.append("PRIMARY KEY(" + (*GlobalConfigJSON)["DefaultTables"][TableName.toStdString()]["PRIMARY KEY"].dump() + "))");
        qDebug().noquote() << QTime::currentTime().toString() << " [OUT] Executing SQL query:" << QueryString;
        QSqlQuery(*DataBase).exec(QueryString);
    }

    QSqlRelationalTableModel * Model = new QSqlRelationalTableModel(Parent, *DataBase);
    Model->setTable(TableName);
    Model->select();

    return Model;
}

void DBOps::AddPackagetoDB(nlohmann::ordered_json * MANIFESTJSON, QDir * GameDir, QSqlDatabase * GlobalDB, nlohmann::ordered_json * GlobalConfigJSON)
{
    qDebug().noquote() << QTime::currentTime().toString() << " [OUT] Adding to library: " << (*MANIFESTJSON)["PACKAGENAME"].dump() << " UID: " << (*MANIFESTJSON)["PACKAGEUID"].dump();

    QMap<QString, QString> PackagePathMap;
    PackagePathMap["PATH"] = "\"" + GameDir->path() + "\"";

    AddJSONObjectToDB(GlobalDB, "PACKAGES", GlobalConfigJSON, (*MANIFESTJSON), PackagePathMap);
}

void DBOps::AddSubGamestoDB(nlohmann::ordered_json * MANIFESTJSON, QSqlDatabase * GlobalDB, nlohmann::ordered_json * GlobalConfigJSON)
{
    qDebug() << QTime::currentTime().toString() << " [OUT] Adding subgames to library... ";

    QMap<QString, QString> ParentPackageMap;
    ParentPackageMap["PARENTPACKAGE"] = QString::fromStdString((*MANIFESTJSON)["PACKAGEUID"].dump());

    for (auto SubGameObject : (*MANIFESTJSON)["SUBGAMES"])
    {
        AddJSONObjectToDB(GlobalDB, "LIBRARY", GlobalConfigJSON, SubGameObject, ParentPackageMap);
    }
}

void DBOps::AddJSONObjectToDB(QSqlDatabase * GlobalDB, QString DBTable, nlohmann::ordered_json * GlobalConfigJSON, nlohmann::ordered_json JsonObject, QMap<QString, QString> ExtraData)
{
    QString QueryString;
    QString ColumnString;
    QString ValueString;

    //ITERATE OVER ENTRIES IN THE TABLE DEFINITIONS IN GLOBALCONFIGJSON
    for (auto Item : (*GlobalConfigJSON)["DefaultTables"][DBTable.toStdString()]["COLUMNS"].items())
    {
        //ADD THE CORRESPONDING VALUES TO THE SQLQUERY, SKIP IF EMPTY
        //if (JsonObject.value(Key).toString().isEmpty())
        if (JsonObject[Item.key()].empty())
        {
            continue;
        }
        //KEYS THAT ARE NOT IN THE TABLE DEFINITIONS IN DefaultConfig.JSON will not be added
        if (JsonObject.contains(Item.key()))
        {
            ColumnString.append(Item.key() + ", ");
            ValueString.append(JsonObject[Item.key()].dump() + ", ");
        }
    }

    //DO THE SAME FOR VALUES PASSED AS THE EXTRADATA QMAP
    for (auto [Column, Value] : ExtraData.asKeyValueRange())
    {
        if (Value.isEmpty())
        {
            continue;
        }
        if ((*GlobalConfigJSON)["DefaultTables"][DBTable.toStdString()]["COLUMNS"].contains(Column.toStdString()))
        {
            ColumnString.append(Column + ", ");
            ValueString.append(Value + ", ");
        }
    }

    //REMOVE THE TRAILING COMMA AND SPACE
    ColumnString.chop(2);
    ValueString.chop(2);

    //CONSTRUCT THE QUERY AND EXECUTE
    QueryString.append("INSERT INTO " + DBTable + " (" + ColumnString + ") VALUES (" + ValueString + ")");
    qDebug().noquote() << QTime::currentTime().toString() << " [OUT] Executing SQL query:" << QueryString;
    QSqlQuery(*GlobalDB).exec(QueryString);
}

bool DBOps::CheckPackageExists(QSqlDatabase * GlobalDB, int PackageUID)
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

QString DBOps::GetItemFromOtherTableByRelation(QTableView * OtherTable, QString Relation, QString RelationColumn, QString ItemColumn)
{
    int row = GetRowByItemAndColumn(OtherTable, Relation, RelationColumn);
    int col = GetHeaderColumn(OtherTable->model(), ItemColumn);
    QString ResultString = OtherTable->model()->data(OtherTable->model()->index(row, col)).toString();
    return ResultString;
}

QList<int> DBOps::IntListFromString(QString String)
{
    QList<int> IntList;
    foreach (QString SubString, String.split(","))
    {
        IntList.append(SubString.toInt());
    }
    return IntList;
}

int DBOps::GetRowByItemAndColumn(QTableView * TableView, QString Item, QString Column)
{
    QAbstractItemModel * Model = TableView->model();

    for (int i = 0; i < Model->rowCount(); i++)
    {
        if (Model->data(Model->index(i, GetHeaderColumn(Model, Column))).toString() == Item)
        {
            return i;
        }
    }
    qDebug() << QTime::currentTime().toString() << "[OUT] " << Item << " not found in column " << Column;
    return -1;
}

QString DBOps::GetSelectedItemByColumn(QTableView * TableView, QString Column)
{
    QString ResultString;
    QAbstractItemModel * Model = TableView->model();
    int ColumnIndex = GetHeaderColumn(Model, Column);
    ResultString = Model->data(Model->index(TableView->selectionModel()->currentIndex().row(), ColumnIndex)).toString();
    return ResultString;
}

int DBOps::GetHeaderColumn(QAbstractItemModel * Model, QString Column)
{
    for (int i = 0; i < Model->columnCount(); i++)
    {
        if (Model->headerData(i, Qt::Horizontal).toString() == Column)
        {
            return i;
        }
    }
    return 0;
}
