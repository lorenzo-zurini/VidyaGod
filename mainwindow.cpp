#include "mainwindow.h"
#include "./ui_mainwindow.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    InitClassVariables();
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
    ui->LibraryTableView->setModel(MainWindow::GlobalDBModel);
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
    QSqlQuery(*GlobalDB).exec("CREATE TABLE IF NOT EXISTS LIBRARY (UID INTEGER NOT NULL UNIQUE, NAME TEXT NOT NULL, PATH TEXT, PRIMARY KEY(UID))");
    MainWindow::GlobalDBModel = new QSqlRelationalTableModel(ui->LibraryTableView, *MainWindow::GlobalDB);
    MainWindow::GlobalDBModel->setTable("LIBRARY");
    MainWindow::GlobalDBModel->select();
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

    qDebug() << QTime::currentTime() << " [OUT] Adding to library: " << MANIFESTJSON->at("Name").get<std::string>() << " UID: " << MANIFESTJSON->at("UID").get<std::string>();

    //Checking if the game is already in library (by UID).
    if (GlobalConfigJSON->at("LibraryUIDs").contains(MANIFESTJSON->at("UID").get<std::string>()))
    {
        qDebug() << QTime::currentTime() << " [ERR] Game already exists in library, aborting!";
        return;
    }

    QSqlQuery AddGameQuery(*GlobalDB);
    AddGameQuery.prepare("INSERT INTO LIBRARY (UID, NAME, PATH) VALUES (:UID, :NAME, :PATH)");
    AddGameQuery.bindValue(":UID", QString::fromStdString(MANIFESTJSON->at("UID").get<std::string>()).toInt());
    AddGameQuery.bindValue(":NAME", QString::fromStdString(MANIFESTJSON->at("Name").get<std::string>()));
    AddGameQuery.bindValue(":PATH", GameDir.path());
    AddGameQuery.exec();

    MainWindow::GlobalDBModel->select();
    ui->LibraryTableView->resizeColumnsToContents();

    delete MANIFESTJSON;
    delete MANIFESTFile;
}


void MainWindow::on_TestButton_clicked()
{
    qDebug() << QTime::currentTime() << "[OUT] TEST";
}

