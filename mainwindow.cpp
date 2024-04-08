#include "mainwindow.h"
#include "./ui_mainwindow.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    InitClassVariables();
    ui->setupUi(this);
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::InitClassVariables()
{
    MainWindow::GlobalConfigFile = new QFile("GlobalConfig.JSON");
    if (!GlobalConfigFile->exists())
    {
        std::cout << QTime::currentTime().toString().toStdString() << " [OUT] Config flie not deteced. Creating... " << std::endl;
        nlohmann::json DefaultJSON;
        DefaultJSON["LibraryUIDs"] = nlohmann::json::array();
        SaveJSON(&DefaultJSON, GlobalConfigFile);
    }
    MainWindow::GlobalConfigJSON = LoadJSON(GlobalConfigFile);
}

nlohmann::json MainWindow::LoadJSON(QFile * JSONFile)
{
    std::cout << QTime::currentTime().toString().toStdString() << " [OUT] Parsing JSON " << JSONFile->fileName().toStdString() << std::endl;
    //ADD VALID JSON CHECK HERE

    if (JSONFile->open(QFile::ReadOnly))
    {
        std::cout << QTime::currentTime().toString().toStdString() << " [OUT] File opened for reading successfully!" << std::endl;
        nlohmann::json JsonDocument = nlohmann::json::parse(JSONFile->readAll());
        JSONFile->close();
        return JsonDocument;
    }
    else
    {
        std::cout << QTime::currentTime().toString().toStdString() << " [ERR] Could not open file for reading!" << std::endl;
        return nlohmann::json();
    }
}

void MainWindow::SaveJSON(nlohmann::json * JSONDocument, QFile * JSONFile)
{
    if (JSONFile->open(QFile::WriteOnly))
    {
        std::cout << QTime::currentTime().toString().toStdString() << " [OUT] File opened for writing successfully!" << std::endl;
        std::cout << QTime::currentTime().toString().toStdString() << " [OUT] Saved " << GlobalConfigFile->fileName().toStdString() << std::endl;
        std::ofstream OutFileStream(JSONFile->fileName().toUtf8());
        OutFileStream << *JSONDocument;
        OutFileStream.close();
        JSONFile->close();
    }
    else
    {
        std::cout << QTime::currentTime().toString().toStdString() << " [ERR] Could not open file for writing!" << std::endl;
    }
}

void MainWindow::on_AddGameButton_clicked()
{
    QString GameDirPathString(QFileDialog::getExistingDirectory(this, "Select GAMEDIR..."));

    //Check if the path is empty, such as when the file picker was canceled.
    if (GameDirPathString.isEmpty())
    {
        std::cout << QTime::currentTime().toString().toStdString() << " [ERR] Path is empty. Canceled?" << std::endl;
        return;
    }

    std::cout << QTime::currentTime().toString().toStdString() << " [OUT] Scanning " << GameDirPathString.toStdString() << std::endl;

    //Check if the directory contains a METADATA subdirectory.
    QDir GameDir(GameDirPathString);
    if (!GameDir.cd("METADATA"))
    {
        std::cout << QTime::currentTime().toString().toStdString() << " [ERR] Selected directory does not contain METADATA subdirectory." << std::endl;
        return;
    }

    //Check for the existence of MANIFEST.json
    QFile * MANIFESTFile = new QFile(GameDir.filePath("MANIFEST.json"));
    if (!MANIFESTFile->exists())
    {
        std::cout << QTime::currentTime().toString().toStdString() << " [ERR] MANIFEST.json file not found in GAMEDIR/METADATA." << std::endl;
        return;
    }
    std::cout << QTime::currentTime().toString().toStdString() << " [OUT] Found " << MANIFESTFile->fileName().toStdString() << std::endl;

    //Catch null return value of the JSON, returned if parser errorred.
    nlohmann::json MANIFESTJSON = LoadJSON(MANIFESTFile);
    if (MANIFESTJSON.empty())
    {
        std::cout << QTime::currentTime().toString().toStdString() << " [ERR] Parser returned empty JSON." << std::endl;
        return;
    }

    std::cout << QTime::currentTime().toString().toStdString() << " [OUT] Adding to library: " << MANIFESTJSON["Name"] << " UID: " << MANIFESTJSON["UID"] <<  std::endl;

    //Checking if the game is already in library (by UID).
    for (auto ExistingUID : GlobalConfigJSON["LibraryUIDs"].items())
    {
        if (ExistingUID.value() == MANIFESTJSON["UID"])
        {
            std::cout << QTime::currentTime().toString().toStdString() << " [ERR] Game already exists in library, aborting!" << std::endl;
            return;
        }
    }
    GlobalConfigJSON["LibraryUIDs"].push_back(MANIFESTJSON["UID"]);


    GlobalConfigJSON["LibraryGames"][MANIFESTJSON["UID"]] = MANIFESTJSON;
    SaveJSON(&GlobalConfigJSON, GlobalConfigFile);
}

