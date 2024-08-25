#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include "ui_mainwindow.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    InitClassVariables();
    ResetTables();
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::InitClassVariables()
{
    ApplicationDirectory = new QDir(QCoreApplication::applicationDirPath());
    InitGlobalConfigJSON();
    InitDatabaseAndModel();
    ui->LibraryTableView->setModel(MainWindow::LibraryModel);
    ui->PackagesTableView->setModel(MainWindow::PackagesModel);
}

void MainWindow::InitDatabaseAndModel()
{
    qDebug() << QTime::currentTime() << " [OUT] Attempting database init... ";
    GlobalDB = new QSqlDatabase(QSqlDatabase::addDatabase("QSQLITE"));
    GlobalDB->setDatabaseName(ApplicationDirectory->filePath(QString("Global.DB")));

    if (GlobalDB->open())
    {
        qDebug() << QTime::currentTime() << " [OUT] " << GlobalDB->databaseName() << " opened successfully.";
    }
    else
    {
        qDebug() << QTime::currentTime() << " [ERR] " << ApplicationDirectory->filePath(QString("Global.DB")) << " could not be opened.";
        return;
    }

    LibraryModel = InitDBTable("LIBRARY", ui->LibraryTableView, GlobalDB);
    PackagesModel = InitDBTable("PACKAGES", ui->PackagesTableView, GlobalDB);
}

void MainWindow::InitGlobalConfigJSON()
{
    GlobalConfigFile = new QFile("GlobalConfig.JSON");
    if (!GlobalConfigFile->exists())
    {
        qDebug() << QTime::currentTime() << " [OUT] Config flie not deteced. Creating... ";
        QFile("DefaultConfig.JSON").copy("GlobalConfig.JSON");
    }
    GlobalConfigJSON = LoadJSON(GlobalConfigFile);
}

QJsonDocument * MainWindow::LoadJSON(QFile * JSONFile)
{
    qDebug() << QTime::currentTime() << " [OUT] Parsing JSON " << JSONFile->fileName();
    //ADD VALID JSON CHECK HERE

    if (JSONFile->open(QFile::ReadOnly))
    {
        qDebug() << QTime::currentTime() << " [OUT] File opened for reading successfully!";
        QJsonDocument * JSONDocument = new QJsonDocument(QJsonDocument::fromJson(JSONFile->readAll()));
        JSONFile->close();
        qDebug() << QTime::currentTime() << " [OUT] Parse done!";
        return JSONDocument;
    }
    else
    {
        qDebug() << QTime::currentTime() << " [ERR] Could not open file for reading!";
        return nullptr;
    }
}

void MainWindow::SaveJSON(QJsonDocument * JSONDocument, QFile * JSONFile)
{
    if (JSONFile->open(QFile::WriteOnly))
    {
        qDebug() << QTime::currentTime() << " [OUT] File opened for writing successfully!";
        qDebug() << QTime::currentTime() << " [OUT] Saved " << GlobalConfigFile->fileName();
        QTextStream OutFileStream(JSONFile);
        OutFileStream << *JSONDocument->toJson();
        JSONFile->close();
    }
    else
    {
        qDebug() << QTime::currentTime() << " [ERR] Could not open file for writing!";
    }
}

void MainWindow::on_TestButton_clicked()
{
    qDebug() << QTime::currentTime() << "[OUT] TEST";
}

void MainWindow::ResetTables()
{
    LibraryModel->select();
    PackagesModel->select();
    ui->LibraryTableView->resizeColumnsToContents();
    ui->PackagesTableView->resizeColumnsToContents();
}

void MainWindow::on_AddGameButton_clicked()
{
    QString GameDirPathString(QFileDialog::getExistingDirectory(this, "Select GAMEDIR..."));

    //Check if the path is empty, such as when the file picker was canceled.
    if (GameDirPathString.isEmpty())
    {
        qDebug() << QTime::currentTime() << " [ERR] Path is empty. Canceled?";
        return;
    }

    qDebug() << QTime::currentTime() << " [OUT] Scanning " << GameDirPathString;

    //Check if the directory contains a METADATA subdirectory.
    QDir GameDir(GameDirPathString);
    if (!GameDir.cd("METADATA"))
    {
        qDebug() << QTime::currentTime() << " [ERR] Selected directory does not contain METADATA subdirectory.";
        return;
    }

    //Check for the existence of MANIFEST.json
    QFile * MANIFESTFile = new QFile(GameDir.filePath("MANIFEST.json"));
    if (!MANIFESTFile->exists())
    {
        qDebug() << QTime::currentTime() << " [ERR] MANIFEST.json file not found in GAMEDIR/METADATA.";
        return;
    }
    qDebug() << QTime::currentTime() << " [OUT] Found " << MANIFESTFile->fileName();
    GameDir.cdUp();


    //Catch nullptr return value of the JSON, returned if parser errorred.
    QJsonDocument * MANIFESTJSON = LoadJSON(MANIFESTFile);
    if (MANIFESTJSON == nullptr)
    {
        qDebug() << QTime::currentTime() << " [ERR] Parser returned nullptr.";
        delete MANIFESTJSON;
        return;
    }
    else if (MANIFESTJSON->isNull())
    {
        qDebug() << QTime::currentTime() << " [ERR] Parser returned null JSON.";
        delete MANIFESTJSON;
        return;
    }
    else
    {
        qDebug() << QTime::currentTime() << " [OUT] Parser returned non-empty JSON.";
    }

    QSqlQuery CheckPackageExistsQuery(*GlobalDB);
    CheckPackageExistsQuery.exec("SELECT PACKAGEUID FROM PACKAGES;");
    while (CheckPackageExistsQuery.next())
    {
        if (CheckPackageExistsQuery.value(0).toInt() == QString((*MANIFESTJSON)["PACKAGEUID"].toString()).toInt())
        {
            qDebug() << QTime::currentTime() << " [ERR] Package already exists in library, aborting!";
            return;
        }
    }

    AddPackagetoDB(MANIFESTJSON, &GameDir);
    AddSubGamestoDB(MANIFESTJSON);

    delete MANIFESTJSON;
    delete MANIFESTFile;

    ResetTables();
}

QSqlRelationalTableModel * MainWindow::InitDBTable(QString TableName, QObject * Parent, QSqlDatabase * DataBase)
{
    if (!MainWindow::GlobalDB->tables().contains(TableName))
    {
        QString QueryString;
        QueryString.append("CREATE TABLE IF NOT EXISTS " + TableName + " (");

        for (auto Key : (*MainWindow::GlobalConfigJSON)["DefaultTables"][TableName]["COLUMNS"].toObject().keys())
        {
            QueryString.append("\"" + Key + "\" " + (*MainWindow::GlobalConfigJSON)["DefaultTables"][TableName]["COLUMNS"].toObject().value(Key).toString() + ", ");
        }
        QueryString.append("PRIMARY KEY(\"" + (*MainWindow::GlobalConfigJSON)["DefaultTables"][TableName]["PRIMARY KEY"].toString() + "\"))");
        QSqlQuery(*MainWindow::GlobalDB).exec(QueryString);
    }

    QSqlRelationalTableModel * Model = new QSqlRelationalTableModel(Parent, *DataBase);
    Model->setTable(TableName);
    Model->select();

    return Model;
}

void MainWindow::AddPackagetoDB(QJsonDocument * MANIFESTJSON, QDir * GameDir)
{
    qDebug() << QTime::currentTime().toString().toStdString() << " [OUT] Adding to library: " << (*MANIFESTJSON)["PACKAGENAME"].toString() << " UID: " << (*MANIFESTJSON)["PACKAGEUID"].toString();

    QMap<QString, QString> PackagePathMap;
    PackagePathMap["PATH"] = GameDir->path();

    AddJSONObjectToDB("PACKAGES", (*MANIFESTJSON).object(), PackagePathMap);
}

void MainWindow::AddSubGamestoDB(QJsonDocument * MANIFESTJSON)
{
    qDebug() << QTime::currentTime().toString().toStdString() << " [OUT] Adding subgames to library... ";

    QMap<QString, QString> ParentPackageMap;
    ParentPackageMap["PARENTPACKAGE"] = (*MANIFESTJSON)["PACKAGEUID"].toString();

    for (auto SubGameObject : (*MANIFESTJSON)["SUBGAMES"].toArray())
    {
        AddJSONObjectToDB("LIBRARY", SubGameObject.toObject(), ParentPackageMap);
    }
}

void MainWindow::AddJSONObjectToDB(QString DBTable, QJsonObject JsonObject, QMap<QString, QString> ExtraData)
{
        QString QueryString;
        QString ColumnString;
        QString ValueString;

        for (auto Key : (*GlobalConfigJSON)["DefaultTables"][DBTable]["COLUMNS"].toObject().keys())
        {
            if (JsonObject.keys().contains(Key))
            {
                ColumnString.append("\"" + Key + "\", ");
                ValueString.append("\"" + JsonObject.value(Key).toString() + "\", ");
            }
        }

        for (auto [Column, Value] : ExtraData.asKeyValueRange())
        {
            if ((*GlobalConfigJSON)["DefaultTables"][DBTable]["COLUMNS"].toObject().keys().contains(Column))
            {
                ColumnString.append("\"" + Column + "\", ");
                ValueString.append("\"" + Value + "\", ");
            }
        }

        ColumnString.chop(2);
        ValueString.chop(2);

        QueryString.append("INSERT INTO " + DBTable + " (" + ColumnString + ") VALUES (" + ValueString + ")");
        QSqlQuery(*GlobalDB).exec(QueryString);
}

void MainWindow::on_PlayGameButton_clicked()
{
    if (!ui->LibraryTableView->selectionModel()->hasSelection())
    {
        qDebug() << QTime::currentTime() << " [OUT] NO SELECTION!";
        return;
    }
    qDebug() << QTime::currentTime() << " [OUT] Attempting launch of " << GetSelectedItemByColumn(ui->LibraryTableView, "TITLE");


    QJsonDocument * MANIFESTJSON;




    qDebug() << QTime::currentTime() << " [OUT] COMPONENTS: "




    //COMPILE DATA



    //PREPARE DIRS




    //WINEBOOT




    //MOUNT ZIPS




    //BUILD OVERLAYS




    //BUILD REGISTRY





    //BUILD COMMAND LINE




    //LAUNCH




    QProcess * GameProcess = new QProcess;
    delete GameProcess;
}

QString MainWindow::GetSelectedItemByColumn(QTableView * TableView, QString Column)
{
    QString ResultString;
    QAbstractItemModel * Model = TableView->model();
    int ColumnIndex = GetModelColumnIndex(Model, Column);
    ResultString = Model->data(Model->index(TableView->selectionModel()->currentIndex().row(), ColumnIndex)).toString();
    return ResultString;
}

int MainWindow::GetModelColumnIndex(QAbstractItemModel * Model, QString Column)
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
