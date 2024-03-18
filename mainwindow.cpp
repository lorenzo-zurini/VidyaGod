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
    MainWindow::LibraryJSONDocument = new QJsonDocument(LoadJSON(QFile("LIBRARY.JSON")));
}

QJsonDocument MainWindow::LoadJSON(QFile JSONFile)
{
    JSONFile.open(QFile::ReadOnly);
    return QJsonDocument().fromJson(JSONFile.readAll());
    JSONFile.close();
}

void MainWindow::SaveJSON(QJsonDocument JSONDocument, QFile JSONFile)
{
    JSONFile.open(QFile::WriteOnly);
    JSONFile.write(JSONDocument.toJson());
    JSONFile.close();
}

void MainWindow::on_AddGameButton_clicked()
{
    QString GameDirPathString(QFileDialog::getExistingDirectory(this, "Select gamedir..."));
    if (GameDirPathString.isEmpty())
    {
        return;
    }
    QDir GameDir(GameDirPathString);
    GameDir.cd("METADATA");
    GameDir.filePath("METADATA.json");
    std::cout << GameDirPathString.toStdString() << std::endl;
}

