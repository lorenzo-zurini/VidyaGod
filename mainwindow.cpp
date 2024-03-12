#include "mainwindow.h"
#include "./ui_mainwindow.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
}

MainWindow::~MainWindow()
{
    delete ui;
}

QJsonDocument MainWindow::LoadJSON(QFile JSONFile)
{
    JSONFile.open(QFile::ReadOnly);
    return QJsonDocument().fromJson(JSONFile.readAll());
    JSONFile.close();
}

bool SaveJSON(QJsonDocument JSONDocument, QFile JSONFile)
{
    JSONFile.open(QFile::WriteOnly);
    return JSONFile.write(JSONDocument.toJson());
    JSONFile.close();
}
