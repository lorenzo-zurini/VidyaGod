#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include <iostream>

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
    //MainWindow::LibraryJSONDocument = new QJsonDocument(LoadJSON(QFile("LIBRARY.JSON")));
    MainWindow::LibraryModel = new QStandardItemModel;
}

QJsonDocument * MainWindow::LoadJSON(QFile * JSONFile)
{
    std::cout << QTime::currentTime().toString().toStdString() << " [OUT] Parsing JSON " << JSONFile->fileName().toStdString() << std::endl;
    //ADD VALID JSON CHECK HERE

    if (JSONFile->open(QFile::ReadOnly))
    {
        std::cout << QTime::currentTime().toString().toStdString() << " [OUT] File opened successfully!" << std::endl;

        QJsonDocument * JSONDocument = new QJsonDocument;
        QJsonParseError * Error = new QJsonParseError;
        JSONDocument->fromJson(JSONFile->readAll(), Error);
        std::cout << JSONDocument->toJson().toStdString() << std::endl;

        if (Error->error != QJsonParseError::NoError)
        {
            delete JSONDocument;
            std::cout << QTime::currentTime().toString().toStdString() << " [ERR] JSON parse error: " << Error->errorString().toStdString() << std::endl;
            return nullptr;
        }
        else
        {
            std::cout << QTime::currentTime().toString().toStdString() << " [OUT] JSON parsed successfully!" << std::endl;
            return JSONDocument;
        }
    }
}

void MainWindow::SaveJSON(QJsonDocument * JSONDocument, QFile * JSONFile)
{
    JSONFile->open(QFile::WriteOnly);
    JSONFile->write(JSONDocument->toJson());
    JSONFile->close();
}

void MainWindow::on_AddGameButton_clicked()
{
    QString GameDirPathString(QFileDialog::getExistingDirectory(this, "Select GAMEDIR..."));
    std::cout << QTime::currentTime().toString().toStdString() << " [OUT] Adding to library: " << GameDirPathString.toStdString() << std::endl;

    //Check if the path is empty, such as when the file picker was canceled.
    if (GameDirPathString.isEmpty())
    {
        std::cout << QTime::currentTime().toString().toStdString() << " [ERR] Path is empty. Canceled?" << std::endl;
        return;
    }

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


    QJsonDocument * MANIFESTJSON = LoadJSON(MANIFESTFile);
    if (MANIFESTJSON == nullptr)
    {
        return;
    }
    std::cout << "test" << MANIFESTJSON->toJson().toStdString() << "test2" << std::endl;
}

