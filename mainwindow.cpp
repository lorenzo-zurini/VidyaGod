#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "packageeditor.h"
#include "jsonoperations.h"
#include "dboperations.h"
#include "filesystemoperations.h"
#include "winerunner.h"
#include "umurunner.h"
#include "dosboxrunner.h"

//TO-DO: ADD VARIABLE SUBSTITUTION WITH ENV VARS AND AUTOCALC
//TO-DO: FIX EXE COMMAND LINE PARSING AND WORKDIR
//IMPLEMENT WINEDLLOVERRIDES
//IMPLEMENT MIRRORFS COMPONENTS
//ADD MORE RUNNERS BASED ON PLATFORM

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
    ApplicationPath = QCoreApplication::applicationDirPath();
    ProtonPath = "/usr/share/steam/compatibilitytools.d/proton-ge-custom/";
    GlobalConfigJSON = JSONOps::InitGlobalConfigJSON(new QFile("GlobalConfig.JSON"));
    GlobalDB = DBOps::InitDatabase(QDir::cleanPath(ApplicationPath + QDir::separator() + "GlobalDB.sqlite"));

    LibraryModel = DBOps::InitDBTable("LIBRARY", GlobalDB, GlobalConfigJSON, ui->LibraryTableView);
    ui->LibraryTableView->setModel(MainWindow::LibraryModel);

    PackagesModel = DBOps::InitDBTable("PACKAGES",  GlobalDB, GlobalConfigJSON, ui->PackagesTableView);
    ui->PackagesTableView->setModel(MainWindow::PackagesModel);
}

void MainWindow::on_TestButton_clicked()
{
    qDebug() << QTime::currentTime().toString() << "[OUT] TEST";
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
    QDir * PackageDir = new QDir(QFileDialog::getExistingDirectory(this, "Select GAMEDIR..."));

    if(!FSOps::CheckPackageValid(PackageDir))
    {
        qDebug() << QTime::currentTime().toString() << " [ERR] Invalid package, aborting..";
        return;
    }

    //Catch nullptr return value of the JSON, returned if parser errorred.
    QJsonDocument * MANIFESTJSON = JSONOps::LoadJSON(new QFile(QDir::cleanPath(PackageDir->path() + QDir::separator() + "METADATA" + QDir::separator() + "MANIFEST.json")));
    if (MANIFESTJSON == nullptr)
    {
        qDebug() << QTime::currentTime().toString() << " [ERR] Parser returned nullptr.";
        delete MANIFESTJSON;
        return;
    }
    else
    {
        qDebug() << QTime::currentTime().toString() << " [OUT] Parser returned non-empty JSON.";
    }

    //ABORT IF PACKAGE ALREADY EXISTS IN DB.
    if (DBOps::CheckPackageExists(GlobalDB, QString((*MANIFESTJSON)["PACKAGEUID"].toString()).toInt()))
    {
        return;
    }

    DBOps::AddPackagetoDB(MANIFESTJSON, PackageDir, GlobalDB, GlobalConfigJSON);
    DBOps::AddSubGamestoDB(MANIFESTJSON, GlobalDB, GlobalConfigJSON);

    delete PackageDir;
    delete MANIFESTJSON;

    ResetTables();
}

void MainWindow::on_PlayGameButton_clicked()
{
    //Check if selection exists.
    if (!ui->LibraryTableView->selectionModel()->hasSelection())
    {
        qDebug() << QTime::currentTime().toString() << " [OUT] NO SELECTION!";
        return;
    }

    qDebug() << QTime::currentTime().toString() << " [OUT] Attempting launch of " << DBOps::GetSelectedItemByColumn(ui->LibraryTableView, "TITLE");

    UMURunner * UMURunner = new class UMURunner(ui->LibraryTableView, ui->PackagesTableView, ProtonPath);

    UMURunner->Cleanup();
    UMURunner->Run();
    QMessageBox::warning(nullptr, "Ready for cleanup...", "Press OK to start cleanup");
    UMURunner->Cleanup();

    delete UMURunner;
}

void MainWindow::on_CreatePackageButton_clicked()
{
    PackageEditor * PackageEditor = new class PackageEditor(GlobalConfigJSON, QFileDialog::getExistingDirectory(this, "Select package directory..."), this);
    PackageEditor->show();
}
