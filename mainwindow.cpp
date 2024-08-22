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

    //QSqlQuery InitQuery(*GlobalDB);
    QSqlQuery(*GlobalDB).exec("CREATE TABLE IF NOT EXISTS PACKAGES"
                              "("   "PACKAGEUID INTEGER NOT NULL UNIQUE,"
                                    "PACKAGENAME TEXT NOT NULL,"
                                    "PACKAGEVERSION TEXT,"
                                    "PATH TEXT,"
                                    "PRIMARY KEY(PACKAGEUID)"               ")");

    QSqlQuery(*GlobalDB).exec("CREATE TABLE IF NOT EXISTS LIBRARY"
                              "("   "GAMEUID INTEGER NOT NULL UNIQUE,"
                                    "TITLE TEXT NOT NULL,"
                                    "PARENTPACKAGE INTEGER NOT NULL,"
                                    "TGDBID INTEGER,"
                                    "STEAMAPPID INTEGER,"
                                    "GOGPRODUCTID INTEGER,"
                                    "COVER TEXT,"
                                    "RELEASEDATE TEXT,"
                                    "EDITION TEXT,"
                                    "EDITIONDATE TEXT,"
                                    "DEVELOPER TEXT,"
                                    "PUBLISHER TEXT,"
                                    "SERIES TEXT,"
                                    "SERIESSORTNUMBER INTEGER,"
                                    "SUBSERIES TEXT,"
                                    "SUBSERIESSORTNUMBER INTEGER,"
                                    "EDITOR TEXT,"
                                    "ONLINEDRM TEXT,"
                                    "NETWORKMULTIPLAYER TEXT,"
                                    "DIRECTCONNECT TEXT,"
                                    "LANMULTIPLAYER TEXT,"
                                    "ONLINEMULTIPLAYER TEXT,"
                                    "NETWORKCOOP TEXT,"
                                    "LOCALMULTIPLAYER TEXT,"
                                    "LOCALCOOP TEXT,"
                                    "OTHERONLINEFEATURES TEXT,"
                                    "PRIMARY KEY(GAMEUID)"                  ")");

    MainWindow::LibraryModel = new QSqlRelationalTableModel(ui->LibraryTableView, *MainWindow::GlobalDB);
    MainWindow::LibraryModel->setTable("LIBRARY");
    MainWindow::LibraryModel->select();

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
        nlohmann::json DefaultJSON;
        DefaultJSON["LibraryUIDs"] = nlohmann::json::array();
        DefaultJSON["Library"] = nlohmann::json::array();
        SaveJSON(&DefaultJSON, GlobalConfigFile);
    }
    MainWindow::GlobalConfigJSON = LoadJSON(GlobalConfigFile);
}

nlohmann::json * MainWindow::LoadJSON(QFile * JSONFile)
{
    qDebug() << QTime::currentTime() << " [OUT] Parsing JSON " << JSONFile->fileName();
    //ADD VALID JSON CHECK HERE

    if (JSONFile->open(QFile::ReadOnly))
    {
        qDebug() << QTime::currentTime() << " [OUT] File opened for reading successfully!";
        nlohmann::json * JsonDocument = new nlohmann::json(nlohmann::json::parse(JSONFile->readAll()));
        JSONFile->close();
        qDebug() << QTime::currentTime() << " [OUT] Parse done!";
        return JsonDocument;
    }
    else
    {
        qDebug() << QTime::currentTime() << " [ERR] Could not open file for reading!";
        return nullptr;
    }
}

void MainWindow::SaveJSON(nlohmann::json * JSONDocument, QFile * JSONFile)
{
    if (JSONFile->open(QFile::WriteOnly))
    {
        qDebug() << QTime::currentTime() << " [OUT] File opened for writing successfully!";
        qDebug() << QTime::currentTime() << " [OUT] Saved " << GlobalConfigFile->fileName();
        std::ofstream OutFileStream(JSONFile->fileName().toUtf8());
        OutFileStream << *JSONDocument;
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
    nlohmann::json * MANIFESTJSON = LoadJSON(MANIFESTFile);
    if (MANIFESTJSON == nullptr)
    {
        qDebug() << QTime::currentTime() << " [ERR] Parser returned nullptr.";
        delete MANIFESTJSON;
        return;
    }
    else if (MANIFESTJSON->empty())
    {
        qDebug() << QTime::currentTime() << " [ERR] Parser returned empty JSON.";
        delete MANIFESTJSON;
        return;
    }
    else
    {
        qDebug() << QTime::currentTime() << " [OUT] Parser returned non-empty JSON.";
    }

    std::cout << QTime::currentTime().toString().toStdString() << " [OUT] Adding to library: " << (*MANIFESTJSON)["PACKAGENAME"] << " UID: " << (*MANIFESTJSON)["PACKAGEUID"] << std::endl;

    //Checking if the game is already in library (by UID).
    //if (GlobalConfigJSON->at("LibraryUIDs").contains(MANIFESTJSON->at("UID").get<std::string>()))
    //{
    //    qDebug() << QTime::currentTime() << " [ERR] Game already exists in library, aborting!";
    //    return;
    //}

    QSqlQuery AddPackageQuery(*MainWindow::GlobalDB);

    AddPackageQuery.prepare("INSERT INTO PACKAGES"
                            "("     "PACKAGEUID,"
                                    "PACKAGENAME,"
                                    "PACKAGEVERSION,"
                                    "PATH"              ")"
                            "VALUES"
                            "("     ":PACKAGEUID,"
                                    ":PACKAGENAME,"
                                    ":PACKAGEVERSION,"
                                    ":PATH"             ")");

    AddPackageQuery.bindValue(":PACKAGEUID", QString::fromStdString(((*MANIFESTJSON)["PACKAGEUID"])).toInt());
    AddPackageQuery.bindValue(":PACKAGENAME", QString::fromStdString(((*MANIFESTJSON)["PACKAGENAME"])));
    AddPackageQuery.bindValue(":PACKAGEVERSION", QString::fromStdString(((*MANIFESTJSON)["PACKAGEVERSION"])));
    AddPackageQuery.bindValue(":PATH", GameDir.path());
    AddPackageQuery.exec();

    //for (auto& ITEM : MANIFESTJSON->at("SUBGAMES").items())
    //{
    //    std::cout << "TESSSST" << ITEM.key() << ITEM.value();
    //}

    QSqlQuery AddGameQuery(*MainWindow::GlobalDB);

    AddGameQuery.prepare("INSERT INTO GAMES"
                         "("    "GAMEUID,"
                                "TITLE,"
                                "PARENTPACKAGE,"
                                "TGDBID,"
                                "STEAMAPPID,"
                                "GOGPRODUCTID,"
                                "COVER,"
                                "RELEASEDATE,"
                                "EDITION,"
                                "EDITIONDATE,"
                                "DEVELOPER,"
                                "PUBLISHER,"
                                "SERIES,"
                                "SERIESSORTNUMBER,"
                                "SUBSERIES,"
                                "SUBSERIESSORTNUMBER,"
                                "EDITOR,"
                                "ONLINEDRM,"
                                "NETWORKMULTIPLAYER,"
                                "DIRECTCONNECT,"
                                "LANMULTIPLAYER,"
                                "ONLINEMULTIPLAYER,"
                                "NETWORKCOOP,"
                                "LOCALMULTIPLAYER,"
                                "LOCALCOOP,"
                                "OTHERONLINEFEATURES"                 ")"
                         "VALUES"
                         "("    ":GAMEUID,"
                                ":TITLE,"
                                ":PARENTPACKAGE,"
                                ":TGDBID,"
                                ":STEAMAPPID,"
                                ":GOGPRODUCTID,"
                                ":COVER,"
                                ":RELEASEDATE,"
                                ":EDITION,"
                                ":EDITIONDATE,"
                                ":DEVELOPER,"
                                ":PUBLISHER,"
                                ":SERIES,"
                                ":SERIESSORTNUMBER,"
                                ":SUBSERIES,"
                                ":SUBSERIESSORTNUMBER,"
                                ":EDITOR,"
                                ":ONLINEDRM,"
                                ":NETWORKMULTIPLAYER,"
                                ":DIRECTCONNECT,"
                                ":LANMULTIPLAYER,"
                                ":ONLINEMULTIPLAYER,"
                                ":NETWORKCOOP,"
                                ":LOCALMULTIPLAYER,"
                                ":LOCALCOOP,"
                                ":OTHERONLINEFEATURES"                ")");

    AddGameQuery.bindValue(":GAMEUID", QString::fromStdString(((*MANIFESTJSON)["SUBGAMES"]["1"]["METADATA"]["GAMEUID"])).toInt());
    AddGameQuery.bindValue(":TITLE", QString::fromStdString(((*MANIFESTJSON)["SUBGAMES"]["1"]["METADATA"]["TITLE"])));
    AddGameQuery.bindValue(":PARENTPACKAGE", QString::fromStdString(((*MANIFESTJSON)["PACKAGEUID"])).toInt());
    AddGameQuery.bindValue(":TGDBID", QString::fromStdString(((*MANIFESTJSON)["SUBGAMES"]["1"]["METADATA"]["TGDBID"])).toInt());
    AddGameQuery.bindValue(":STEAMAPPID", QString::fromStdString(((*MANIFESTJSON)["SUBGAMES"]["1"]["METADATA"]["STEAMAPPID"])).toInt());
    AddGameQuery.bindValue(":GOGPRODUCTID", QString::fromStdString(((*MANIFESTJSON)["SUBGAMES"]["1"]["METADATA"]["GOGPRODUCTID"])).toInt());
    AddGameQuery.bindValue(":COVER", QString::fromStdString(((*MANIFESTJSON)["SUBGAMES"]["1"]["METADATA"]["COVER"])));
    AddGameQuery.bindValue(":RELEASEDATE", QString::fromStdString(((*MANIFESTJSON)["SUBGAMES"]["1"]["METADATA"]["RELEASEDATE"])));
    AddGameQuery.bindValue(":EDITION", QString::fromStdString(((*MANIFESTJSON)["SUBGAMES"]["1"]["METADATA"]["EDITION"])));
    AddGameQuery.bindValue(":EDITIONDATE", QString::fromStdString(((*MANIFESTJSON)["SUBGAMES"]["1"]["METADATA"]["EDITIONDATE"])));
    AddGameQuery.bindValue(":DEVELOPER", QString::fromStdString(((*MANIFESTJSON)["SUBGAMES"]["1"]["METADATA"]["DEVELOPER"])));
    AddGameQuery.bindValue(":PUBLISHER", QString::fromStdString(((*MANIFESTJSON)["SUBGAMES"]["1"]["METADATA"]["PUBLISHER"])));
    AddGameQuery.bindValue(":SERIES", QString::fromStdString(((*MANIFESTJSON)["SUBGAMES"]["1"]["METADATA"]["SERIES"])));
    AddGameQuery.bindValue(":SERIESSORTNUMBER", QString::fromStdString(((*MANIFESTJSON)["SUBGAMES"]["1"]["METADATA"]["SERIESSORTNUMBER"])).toInt());
    AddGameQuery.bindValue(":SUBSERIES", QString::fromStdString(((*MANIFESTJSON)["SUBGAMES"]["1"]["METADATA"]["SUBSERIES"])));
    AddGameQuery.bindValue(":SUBSERIESSORTNUMBER", QString::fromStdString(((*MANIFESTJSON)["SUBGAMES"]["1"]["METADATA"]["SUBSERIESSORTNUMBER"])).toInt());
    AddGameQuery.bindValue(":EDITOR", QString::fromStdString(((*MANIFESTJSON)["SUBGAMES"]["1"]["METADATA"]["EDITOR"])));
    AddGameQuery.bindValue(":ONLINEDRM", QString::fromStdString(((*MANIFESTJSON)["SUBGAMES"]["1"]["METADATA"]["ONLINEDRM"])));
    AddGameQuery.bindValue(":NETWORKMULTIPLAYER", QString::fromStdString(((*MANIFESTJSON)["SUBGAMES"]["1"]["METADATA"]["NETWORKMULTIPLAYER"])));
    AddGameQuery.bindValue(":DIRECTCONNECT", QString::fromStdString(((*MANIFESTJSON)["SUBGAMES"]["1"]["METADATA"]["DIRECTCONNECT"])));
    AddGameQuery.bindValue(":LANMULTIPLAYER", QString::fromStdString(((*MANIFESTJSON)["SUBGAMES"]["1"]["METADATA"]["LANMULTIPLAYER"])));
    AddGameQuery.bindValue(":ONLINEMULTIPLAYER", QString::fromStdString(((*MANIFESTJSON)["SUBGAMES"]["1"]["METADATA"]["ONLINEMULTIPLAYER"])));
    AddGameQuery.bindValue(":NETWORKCOOP", QString::fromStdString(((*MANIFESTJSON)["SUBGAMES"]["1"]["METADATA"]["NETWORKCOOP"])));
    AddGameQuery.bindValue(":LOCALMULTIPLAYER", QString::fromStdString(((*MANIFESTJSON)["SUBGAMES"]["1"]["METADATA"]["LOCALMULTIPLAYER"])));
    AddGameQuery.bindValue(":LOCALCOOP", QString::fromStdString(((*MANIFESTJSON)["SUBGAMES"]["1"]["METADATA"]["LOCALCOOP"])));
    AddGameQuery.bindValue(":OTHERONLINEFEATURES", QString::fromStdString(((*MANIFESTJSON)["SUBGAMES"]["1"]["METADATA"]["OTHERONLINEFEATURES"])));
    AddGameQuery.exec();
    qDebug() << AddGameQuery.lastError() << " " << AddGameQuery.lastQuery();

    MainWindow::ResetTables();
    delete MANIFESTJSON;
    delete MANIFESTFile;
}


void MainWindow::on_PlayGameButton_clicked()
{
    qDebug() << "PLAY";
}

