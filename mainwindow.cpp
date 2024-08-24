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

void MainWindow::InitDBTable(std::string TableName)
{
    QString QueryString;

    QueryString.append("CREATE TABLE IF NOT EXISTS").append(" ").append(QString::fromStdString(TableName)).append(" (");
    for (auto ITEM : (*MainWindow::GlobalConfigJSON)["DefaultTables"][TableName]["COLUMNS"].items())
    {
        QueryString.append("\"").append(QString::fromStdString(ITEM.key())).append("\" ").append(QString::fromStdString(ITEM.value())).append(", ");
    }
    QueryString.append("PRIMARY KEY(\"").append(QString::fromStdString((*MainWindow::GlobalConfigJSON)["DefaultTables"][TableName]["PRIMARY KEY"])).append("\"))");
    QSqlQuery(*MainWindow::GlobalDB).exec(QueryString);
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

nlohmann::ordered_json * MainWindow::LoadJSON(QFile * JSONFile)
{
    qDebug() << QTime::currentTime() << " [OUT] Parsing JSON " << JSONFile->fileName();
    //ADD VALID JSON CHECK HERE

    if (JSONFile->open(QFile::ReadOnly))
    {
        qDebug() << QTime::currentTime() << " [OUT] File opened for reading successfully!";
        nlohmann::ordered_json * JsonDocument = new nlohmann::ordered_json(nlohmann::ordered_json::parse(JSONFile->readAll()));
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

void MainWindow::SaveJSON(nlohmann::ordered_json * JSONDocument, QFile * JSONFile)
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
    nlohmann::ordered_json * MANIFESTJSON = LoadJSON(MANIFESTFile);
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

void MainWindow::AddPackagetoDB(nlohmann::ordered_json * MANIFESTJSON, QDir * GameDir)
{
    std::cout << QTime::currentTime().toString().toStdString() << " [OUT] Adding to library: " << (*MANIFESTJSON)["PACKAGENAME"] << " UID: " << (*MANIFESTJSON)["PACKAGEUID"] << std::endl;
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

    AddPackageQuery.bindValue(":PACKAGEUID", QString::fromStdString(((*MANIFESTJSON)["PACKAGEUID"])));
    AddPackageQuery.bindValue(":PACKAGENAME", QString::fromStdString(((*MANIFESTJSON)["PACKAGENAME"])));
    AddPackageQuery.bindValue(":PACKAGEVERSION", QString::fromStdString(((*MANIFESTJSON)["PACKAGEVERSION"])));
    AddPackageQuery.bindValue(":PATH", GameDir->path());
    AddPackageQuery.exec();
}

void MainWindow::AddSubGamestoDB(nlohmann::ordered_json * MANIFESTJSON)
{
    std::cout << QTime::currentTime().toString().toStdString() << " [OUT] Adding subgames to library... " << std::endl;


    for (auto ITEM : (*MANIFESTJSON)["SUBGAMES"].items())
    {
        QString QueryString;
        QString ColumnString;
        QString ValueString;

        for (auto SUBITEM : ITEM.value().items())
        {
            ColumnString.append("\"").append(QString::fromStdString(SUBITEM.key())).append("\", ");
            ValueString.append("\"").append(QString::fromStdString(SUBITEM.value())).append("\", ");
        }

        ColumnString.append("\"PARENTPACKAGE\"");
        ValueString.append(QString::fromStdString(((*MANIFESTJSON)["PACKAGEUID"])));

        QueryString.append("INSERT INTO LIBRARY (").append(ColumnString).append(") VALUES (").append(ValueString).append(")");
        QSqlQuery TestQuery(*MainWindow::GlobalDB);
        TestQuery.exec(QueryString);
        qDebug().noquote() << TestQuery.lastError();
        qDebug().noquote() << TestQuery.lastQuery();
    }
}

void MainWindow::on_PlayGameButton_clicked()
{
    qDebug() << "PLAY";
}

