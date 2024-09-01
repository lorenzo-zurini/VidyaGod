#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include "ui_mainwindow.h"
#include <iostream>

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
    ProtonPath = new QDir("/usr/share/steam/compatibilitytools.d/proton-ge-custom/");
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

    if (!JSONFile->exists())
    {
        qDebug() << QTime::currentTime() << " [ERR] File " << JSONFile->fileName() << " does not exist.";
        return nullptr;
    }
    if (JSONFile->open(QFile::ReadOnly))
    {
        qDebug() << QTime::currentTime() << " [OUT] File " << JSONFile->fileName() << " opened for reading successfully!";
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

    //Catch nullptr return value of the JSON, returned if parser errorred.
    QJsonDocument * MANIFESTJSON = LoadJSON(new QFile(GameDir.filePath("MANIFEST.json")));
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
        GameDir.cdUp();
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
        qDebug() << "QUERYSTRING: " << QueryString;
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
            if (JsonObject.value(Key).toString().isEmpty())
            {
                continue;
            }
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
    QString GameName = GetSelectedItemByColumn(ui->LibraryTableView, "TITLE");
    qDebug() << QTime::currentTime() << " [OUT] Attempting launch of " << GameName;

    //Make a list of integers from the RECIPE string.
    QList<int> Recipe;
    foreach (QString string, GetSelectedItemByColumn(ui->LibraryTableView, "RECIPE").split(","))
    {
        Recipe.append(string.toInt());
    }

    QString ParentPackage = GetSelectedItemByColumn(ui->LibraryTableView, "PARENTPACKAGE");

    //Get Package PATH from the PACKAGES table by GAMEUID.
    int row = GetRowByItemAndColumn(ui->PackagesTableView, ParentPackage, "PACKAGEUID");
    int col = GetHeaderColumn(ui->PackagesTableView->model(), "PATH");
    QString PackagePath = ui->PackagesTableView->model()->data(ui->PackagesTableView->model()->index(row, col)).toString();

    //Initalise directories.
    QDir PackageDir(PackagePath);
    qDebug() << QTime::currentTime() << "[OUT] PACKAGEDIR: " << PackageDir.path();

    QDir MetaDataDir = PackageDir;
    MetaDataDir.cd("METADATA");
    qDebug() << QTime::currentTime() << "[OUT] METADATADIR: " << MetaDataDir.path();

    QDir PackageFilesDir = PackageDir;
    PackageFilesDir.cd("PACKAGEFILES");
    qDebug() << QTime::currentTime() << "[OUT] PACKAGEFILES: " << PackageFilesDir.path();

    QDir UserDataDir = PackageDir;
    UserDataDir.mkdir("USERDATA");
    UserDataDir.cd("USERDATA");
    qDebug() << QTime::currentTime() << "[OUT] USERDATA: " << UserDataDir.path();

    QDir TempDir = PackageDir;
    TempDir.mkdir("TEMP");
    TempDir.cd("TEMP");
    qDebug() << QTime::currentTime() << "[OUT] TEMP: " << TempDir.path();

    QDir RuntimeDir = PackageDir;
    RuntimeDir.mkdir("RUNTIME");
    RuntimeDir.cd("RUNTIME");
    qDebug() << QTime::currentTime() << "[OUT] RUNTIME: " << RuntimeDir.path();

    QDir ProgramRuntimeDir = RuntimeDir;
    ProgramRuntimeDir.mkdir("drive_c");
    ProgramRuntimeDir.cd("drive_c");
    ProgramRuntimeDir.mkdir(ParentPackage);
    ProgramRuntimeDir.cd(ParentPackage);
    qDebug() << QTime::currentTime() << "[OUT] RUNTIMEPROGRAMDIR: " << RuntimeDir.path();

    QDir DefPrefixDir = TempDir;
    DefPrefixDir.mkdir("DEFPREFIX");
    DefPrefixDir.cd("DEFPREFIX");
    qDebug() << QTime::currentTime() << "[OUT] DEFPREFIX: " << DefPrefixDir.path();

    QFile ExeFile("C:/" + ParentPackage + "/" + GetSelectedItemByColumn(ui->LibraryTableView, "EXEPATH"));
    QString ExeArgs(GetSelectedItemByColumn(ui->LibraryTableView, "EXEPATH"));
    QString UMUID(GetSelectedItemByColumn(ui->LibraryTableView, "UMUID"));

    QJsonDocument * MANIFESTJSON = LoadJSON(new QFile(MetaDataDir.filePath("MANIFEST.json")));

    //BUILD AN ARRAY CONTAINING ALL SUBCOMPONENTS, IN ORDER, FILTERED BY RECIPE.
    QJsonArray SubComponentsArray;
    //qDebug() << QTime::currentTime() << " [OUT] COMPONENTS: ";
    for (int i = 0; i < (*MANIFESTJSON)["COMPONENTS"].toArray().count(); i++)
    {
        if (Recipe.contains(i))
        {
            for (int j = 0; j < (*MANIFESTJSON)["COMPONENTS"].toArray()[i].toObject()["SUBCOMPONENTS"].toArray().count(); j++)
            {
                SubComponentsArray.append((*MANIFESTJSON)["COMPONENTS"].toArray()[i].toObject()["SUBCOMPONENTS"].toArray()[j].toObject());
            }
        }
    }

    //START BUILDING THE STRING FOR THE UNIONFS
    QString UnionFSString;

    //INITIALISE umu wineprefix
    QProcess * umuInit = new QProcess(this);
    umuInit->setProgram("umu-run");
    umuInit->setArguments({""});

    QProcessEnvironment umuInitEnvironment = QProcessEnvironment::systemEnvironment();
    umuInitEnvironment.insert("WINEPREFIX", DefPrefixDir.path());
    umuInitEnvironment.insert("GAMEID", UMUID);
    umuInitEnvironment.insert("PROTONPATH", ProtonPath->path());
    umuInitEnvironment.insert("PROTON_VERB", "waitforexitandrun");
    umuInit->setProcessEnvironment(umuInitEnvironment);
    umuInit->start();
    umuInit->waitForFinished(-1);
    delete umuInit;

    UnionFSString.prepend(DefPrefixDir.path() + "=RO");

    //MOUNT FILE LAYERS IN TEMP
    for (int i = 0; i < SubComponentsArray.count(); i++)
    {
        QJsonObject SubComponentJSON = SubComponentsArray[i].toObject();

        if (SubComponentJSON["TYPE"].toString() == "ZipFileLayer")
        {
            QDir SubComponentDir = TempDir;
            SubComponentDir.mkdir("[" + QString::number(i) + "]");
            SubComponentDir.cd("[" + QString::number(i) + "]");
            UnionFSString.prepend(SubComponentDir.path() + "=RO:");

            QDir MountDir = SubComponentDir;
            MountDir.mkpath(MountDir.path() + "/drive_c/" + ParentPackage);
            MountDir.cd(MountDir.path() + "/drive_c/" + ParentPackage);

            if (!(SubComponentJSON["TARGET"].toString() == ""))
            {
                QString PathWithTarget = MountDir.path() + "/" + SubComponentJSON["TARGET"].toString();
                MountDir.mkpath(PathWithTarget);
                MountDir.cd(PathWithTarget);
            }

            QProcess * MountZip = new QProcess(this);
            MountZip->setProgram("fuse-zip");
            MountZip->setArguments({"-r", PackageFilesDir.filePath(SubComponentJSON["PATH"].toString()), MountDir.path()});
            MountZip->start();
            MountZip->waitForFinished(-1);
            delete MountZip;
        }
    }

    UnionFSString.prepend(UserDataDir.path() + "=RW:");

    //BUILD UNIONFS
    QProcess * BuildUnionFS = new QProcess(this);
    BuildUnionFS->setProgram("unionfs");
    BuildUnionFS->setArguments({"-o", "cow", "-o", "uid=1000", UnionFSString, RuntimeDir.path()});
    BuildUnionFS->start();
    BuildUnionFS->waitForFinished(-1);
    qDebug().noquote() << BuildUnionFS->arguments();
    qDebug().noquote() << "ERROR: " << BuildUnionFS->readAllStandardError();
    qDebug().noquote() << "OUTPUT: " << BuildUnionFS->readAllStandardOutput();
    delete BuildUnionFS;

    //BUILD REGISTRY





    //LAUNCH
    QProcess * RunGame = new QProcess(this);

    RunGame->setWorkingDirectory(ProgramRuntimeDir.path());
    RunGame->setProgram("umu-run");
    RunGame->setArguments({(ExeFile.fileName()), ExeArgs});

    QProcessEnvironment RunGameEnvironment = QProcessEnvironment::systemEnvironment();
    RunGameEnvironment.insert("WINEPREFIX", RuntimeDir.path());
    RunGameEnvironment.insert("GAMEID", "0");
    RunGameEnvironment.insert("PROTONPATH", ProtonPath->path());
    RunGameEnvironment.insert("PROTON_VERB", "waitforexitandrun");
    RunGame->setProcessEnvironment(RunGameEnvironment);
    RunGame->start();
    RunGame->waitForFinished(-1);
    qDebug().noquote() << "ERROR: " << RunGame->readAllStandardError();
    qDebug().noquote() << "OUTPUT: " << RunGame->readAllStandardOutput();
    delete RunGame;


    //CLEANUP=====================================================================================================
    QMessageBox::warning(this, "Ready for cleanup...", "Press OK to start cleanup");

    QProcess * UnmountUnionFS = new QProcess(this);
    UnmountUnionFS->setProgram("fusermount");
    UnmountUnionFS->setArguments({"-u", RuntimeDir.path()});
    UnmountUnionFS->start();
    UnmountUnionFS->waitForFinished(-1);
    delete UnmountUnionFS;

    for (int i = 0; i < SubComponentsArray.count(); i++)
    {
        QJsonObject SubComponentJSON = SubComponentsArray[i].toObject();

        if (SubComponentJSON["TYPE"].toString() == "ZipFileLayer")
        {
            QDir SubComponentDir = TempDir;
            SubComponentDir.mkdir("[" + QString::number(i) + "]");
            SubComponentDir.cd("[" + QString::number(i) + "]");

            QDir MountDir = SubComponentDir;
            MountDir.mkpath(MountDir.path() + "/drive_c/" + ParentPackage);
            MountDir.cd(MountDir.path() + "/drive_c/" + ParentPackage);

            if (!(SubComponentJSON["TARGET"].toString() == ""))
            {
                QString PathWithTarget = MountDir.path() + "/" + SubComponentJSON["TARGET"].toString();
                MountDir.mkpath(PathWithTarget);
                MountDir.cd(PathWithTarget);
            }

            QProcess * UnmountZips = new QProcess(this);
            UnmountZips->setProgram("fusermount");
            UnmountZips->setArguments({"-u", MountDir.path()});
            UnmountZips->start();
            UnmountZips->waitForFinished(-1);
            delete UnmountZips;
        }
    }

    TempDir.removeRecursively();
    RuntimeDir.removeRecursively();
    delete MANIFESTJSON;
}

int MainWindow::GetRowByItemAndColumn(QTableView * TableView, QString Item, QString Column)
{
    QAbstractItemModel * Model = TableView->model();

    for (int i = 0; i < Model->rowCount(); i++)
    {
        if (Model->data(Model->index(i, GetHeaderColumn(Model, Column))).toString() == Item)
        {
            return i;
        }
    }
    qDebug() << "[OUT] " << Item << " not found in column " << Column;
    return -1;
}

QString MainWindow::GetSelectedItemByColumn(QTableView * TableView, QString Column)
{
    QString ResultString;
    QAbstractItemModel * Model = TableView->model();
    int ColumnIndex = GetHeaderColumn(Model, Column);
    ResultString = Model->data(Model->index(TableView->selectionModel()->currentIndex().row(), ColumnIndex)).toString();
    return ResultString;
}

int MainWindow::GetHeaderColumn(QAbstractItemModel * Model, QString Column)
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
