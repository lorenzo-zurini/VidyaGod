#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include "ui_mainwindow.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    MainWindow::InitClassVariables();
    MainWindow::ResetTables();
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::InitClassVariables()
{
    MainWindow::ApplicationDirectory = new QDir(QCoreApplication::applicationDirPath());
    MainWindow::InitGlobalConfigJSON();
    MainWindow::InitDatabaseAndModel();
    ui->LibraryTableView->setModel(MainWindow::LibraryModel);
    ui->PackagesTableView->setModel(MainWindow::PackagesModel);
}

void MainWindow::InitDatabaseAndModel()
{
    qDebug() << QTime::currentTime() << " [OUT] Attempting database init... ";
    MainWindow::GlobalDB = new QSqlDatabase(QSqlDatabase::addDatabase("QSQLITE"));
    MainWindow::GlobalDB->setDatabaseName(ApplicationDirectory->filePath(QString("Global.DB")));

    if (MainWindow::GlobalDB->open())
    {
        qDebug() << QTime::currentTime() << " [OUT] " << MainWindow::GlobalDB->databaseName() << " opened successfully.";
    }
    else
    {
        qDebug() << QTime::currentTime() << " [ERR] " << ApplicationDirectory->filePath(QString("Global.DB")) << " could not be opened.";
        return;
    }

    MainWindow::InitDBTable("LIBRARY");
    MainWindow::LibraryModel = new QSqlRelationalTableModel(ui->LibraryTableView, *MainWindow::GlobalDB);
    MainWindow::LibraryModel->setTable("LIBRARY");
    MainWindow::LibraryModel->select();


    MainWindow::InitDBTable("PACKAGES");
    MainWindow::PackagesModel = new QSqlRelationalTableModel(ui->PackagesTableView, *MainWindow::GlobalDB);
    MainWindow::PackagesModel->setTable("PACKAGES");
    MainWindow::PackagesModel->select();
}

void MainWindow::InitGlobalConfigJSON()
{
    MainWindow::GlobalConfigFile = new QFile("GlobalConfig.JSON");
    if (!GlobalConfigFile->exists())
    {
        qDebug() << QTime::currentTime() << " [OUT] Config flie not deteced. Creating... ";
        QFile("DefaultConfig.JSON").copy("GlobalConfig.JSON");
    }
    MainWindow::GlobalConfigJSON = LoadJSON(GlobalConfigFile);
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
        std::ofstream OutFileStream(JSONFile->fileName().toUtf8());
        OutFileStream << *JSONDocument->toJson();
        OutFileStream.close();
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
    MainWindow::LibraryModel->select();
    MainWindow::PackagesModel->select();
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

    //QSqlQuery CheckGameExistsQuery(*MainWindow::GlobalDB);
    //CheckGameExistsQuery.exec("SELECT GAMEUID FROM LIBRARY");
    //while (CheckGameExistsQuery.next())
    //{
    //    qDebug() << QTime::currentTime() << " [ERR] Game already exists in library, aborting!";
    //}

    MainWindow::AddPackagetoDB(MANIFESTJSON, &GameDir);
    MainWindow::AddSubGamestoDB(MANIFESTJSON);

    delete MANIFESTJSON;
    delete MANIFESTFile;

    MainWindow::ResetTables();
}

void MainWindow::InitDBTable(QString TableName)
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

void MainWindow::AddPackagetoDB(QJsonDocument * MANIFESTJSON, QDir * GameDir)
{
    qDebug() << QTime::currentTime().toString().toStdString() << " [OUT] Adding to library: " << (*MANIFESTJSON)["PACKAGENAME"] << " UID: " << (*MANIFESTJSON)["PACKAGEUID"];

    QMap<QString, QString> PackagePathMap;
    PackagePathMap["PATH"] = GameDir->path();

    MainWindow::AddJSONObjectToDB("PACKAGES", (*MANIFESTJSON).object(), PackagePathMap);
    QSqlQuery AddPackageQuery(*MainWindow::GlobalDB);
}

void MainWindow::AddSubGamestoDB(QJsonDocument * MANIFESTJSON)
{
    qDebug() << QTime::currentTime().toString().toStdString() << " [OUT] Adding subgames to library... ";

    QMap<QString, QString> ParentPackageMap;
    ParentPackageMap["PARENTPACKAGE"] = (*MANIFESTJSON)["PACKAGEUID"].toString();

    for (auto SubGameObject : (*MANIFESTJSON)["SUBGAMES"].toArray())
    {
        MainWindow::AddJSONObjectToDB("LIBRARY", SubGameObject.toObject(), ParentPackageMap);
    }
}

void MainWindow::AddJSONObjectToDB(QString DBTable, QJsonObject JsonObject, QMap<QString, QString> ExtraData)
{
        QString QueryString;
        QString ColumnString;
        QString ValueString;

        for (auto Key : (*MainWindow::GlobalConfigJSON)["DefaultTables"][DBTable]["COLUMNS"].toObject().keys())
        {
            if (JsonObject.keys().contains(Key))
            {
                ColumnString.append("\"" + Key + "\", ");
                ValueString.append("\"" + JsonObject.value(Key).toString() + "\", ");
            }
        }

        for (auto [Column, Value] : ExtraData.asKeyValueRange())
        {
            if ((*MainWindow::GlobalConfigJSON)["DefaultTables"][DBTable]["COLUMNS"].toObject().keys().contains(Column))
            {
                ColumnString.append("\"" + Column + "\", ");
                ValueString.append("\"" + Value + "\", ");
            }
        }

        ColumnString.chop(2);
        ValueString.chop(2);

        QueryString.append("INSERT INTO " + DBTable + " (" + ColumnString + ") VALUES (" + ValueString + ")");
        QSqlQuery(*MainWindow::GlobalDB).exec(QueryString);
}

void MainWindow::on_PlayGameButton_clicked()
{
    qDebug() << "PLAY";
}

