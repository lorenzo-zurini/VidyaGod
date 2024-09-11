#include "mainwindow.h"
#include "ui_mainwindow.h"

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
    ApplicationDirectory = new QDir(QCoreApplication::applicationDirPath());
    ProtonPath = new QDir("/usr/share/steam/compatibilitytools.d/proton-ge-custom/");
    GlobalConfigJSON = JSONOperations::InitGlobalConfigJSON(new QFile("GlobalConfig.JSON"));
    GlobalDB = DBOperations::InitDatabase(ApplicationDirectory->filePath("GlobalDB.sqlite"));

    LibraryModel = DBOperations::InitDBTable("LIBRARY", GlobalDB, GlobalConfigJSON, ui->LibraryTableView);
    ui->LibraryTableView->setModel(MainWindow::LibraryModel);

    PackagesModel = DBOperations::InitDBTable("PACKAGES",  GlobalDB, GlobalConfigJSON, ui->PackagesTableView);
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

    if(!FileSystemOperations::CheckPackageValid(PackageDir))
    {
        qDebug() << QTime::currentTime().toString() << " [ERR] Invalid package, aborting..";
        return;
    }


    //Catch nullptr return value of the JSON, returned if parser errorred.
    QJsonDocument * MANIFESTJSON = JSONOperations::LoadJSON(new QFile(QDir::cleanPath(PackageDir->path() + QDir::separator() + "METADATA" + QDir::separator() + "MANIFEST.json")));
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
    if (DBOperations::CheckPackageExists(GlobalDB, QString((*MANIFESTJSON)["PACKAGEUID"].toString()).toInt()))
    {
        return;
    }

    DBOperations::AddPackagetoDB(MANIFESTJSON, PackageDir, GlobalDB, GlobalConfigJSON);
    DBOperations::AddSubGamestoDB(MANIFESTJSON, GlobalDB, GlobalConfigJSON);

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

    qDebug() << QTime::currentTime().toString() << " [OUT] Attempting launch of " << GetSelectedItemByColumn(ui->LibraryTableView, "TITLE");

    QVariantMap * RunnerParams = new QVariantMap;
    (*RunnerParams)["GameName"] = GetSelectedItemByColumn(ui->LibraryTableView, "TITLE");
    (*RunnerParams)["UMUID"] = GetSelectedItemByColumn(ui->LibraryTableView, "UMUID");
    (*RunnerParams)["ParentPackage"] = GetSelectedItemByColumn(ui->LibraryTableView, "PARENTPACKAGE");
    (*RunnerParams)["ExePath"] = QDir::cleanPath("C:/" + (*RunnerParams)["ParentPackage"].toString() + QDir::separator() + GetSelectedItemByColumn(ui->LibraryTableView, "EXEPATH"));
    (*RunnerParams)["Recipe"].setValue<QList<int>>(IntListFromString(GetSelectedItemByColumn(ui->LibraryTableView, "RECIPE")));

    if (!GetSelectedItemByColumn(ui->LibraryTableView, "EXEARGS").isEmpty())
    {
        //MUST MAKE THIS RESPECT QUOTES
        (*RunnerParams)["ExeArgs"].toStringList().append(GetSelectedItemByColumn(ui->LibraryTableView, "EXEARGS").split(" "));
    }

    (*RunnerParams)["Paths"].toMap()["PackagePath"] = GetItemFromOtherTableByRelation(ui->PackagesTableView, (*RunnerParams)["Paths"].toMap()["ParentPackage"].toString(), "PACKAGEUID", "PATH");
    (*RunnerParams)["Paths"].toMap()["RuntimePath"] = FileSystemOperations::SubPath((*RunnerParams)["Paths"].toMap()["PackagePath"].toString(), "RUNTIME");
    (*RunnerParams)["Paths"].toMap()["WorkDir"] = FileSystemOperations::SubPath((*RunnerParams)["Paths"].toMap()["RuntimePath"].toString(), GetSelectedItemByColumn(ui->LibraryTableView, "WORKDIR"));
    (*RunnerParams)["Paths"].toMap()["ProgramPath"] = FileSystemOperations::SubPath(FileSystemOperations::SubPath((*RunnerParams)["Paths"].toMap()["RuntimePath"].toString(), "drive_c"), (*RunnerParams)["Paths"].toMap()["ParentPackage"].toString());
    (*RunnerParams)["Paths"].toMap()["MetaDataPath"] = FileSystemOperations::SubPath((*RunnerParams)["Paths"].toMap()["PackagePath"].toString(), "METADATA");
    (*RunnerParams)["Paths"].toMap()["PackageFilesPath"] = FileSystemOperations::SubPath((*RunnerParams)["Paths"].toMap()["PackagePath"].toString(), "PACKAGEFILES");
    (*RunnerParams)["Paths"].toMap()["UserDataPath"] = FileSystemOperations::SubPath((*RunnerParams)["Paths"].toMap()["PackagePath"].toString(), "USERDATA");
    (*RunnerParams)["Paths"].toMap()["TempPath"] = FileSystemOperations::SubPath((*RunnerParams)["Paths"].toMap()["PackagePath"].toString(), "TEMP");
    (*RunnerParams)["Paths"].toMap()["DefPrefixPath"] = FileSystemOperations::SubPath((*RunnerParams)["Paths"].toMap()["TempPath"].toString(), "DEFPREFIX");


    QJsonArray SubComponentsArray = GetSubComponents((*RunnerParams)["Paths"].toMap()["MetaDataPath"].toString(), (*RunnerParams)["Recipe"].toList());

    //START BUILDING THE STRING FOR THE UNIONFS
    QString * UnionFSString = new QString;


    if (!InitializeUMUPrefix(DefPrefixDir, UnionFSString))
    {
        delete ExeFile;
        delete ExeArgs;
        delete UnionFSString;
        return;
    }

    if (!ProcessSubComponents(SubComponentsArray, TempDir, PackageFilesDir, DefPrefixDir, ProgramRuntimeDir, UnionFSString, ParentPackage))
    {
        delete ExeFile;
        delete ExeArgs;
        delete UnionFSString;
        return;
    }

    if (!BuildUnionFS(UnionFSString, RuntimeDir, UserDataDir))
    {
        delete ExeFile;
        delete ExeArgs;
        delete UnionFSString;
        return;
    }

    CheckCaseConflicts(RuntimeDir);

    RunWithUMU(WorkDir, RuntimeDir, UMUID, ExeFile, ExeArgs);

    //CLEANUP=====================================================================================================
    QMessageBox::warning(this, "Ready for cleanup...", "Press OK to start cleanup");
    DestroyUnionFS(RuntimeDir);
    RemoveSubComponents(SubComponentsArray, TempDir, PackageFilesDir, UnionFSString, ParentPackage);

    TempDir->removeRecursively();
    RuntimeDir->removeRecursively();

    delete ExeFile;
    delete UnionFSString;
}

bool MainWindow::InitializeUMUPrefix(QDir * DefPrefixDir, QString * UnionFSString)
{
    qDebug() << QTime::currentTime().toString() << " [OUT] Initialising prefix" << DefPrefixDir->path();
    if (RunWithUMU(DefPrefixDir, DefPrefixDir, "0", new QFile("wineboot")))
    {
        qDebug() << QTime::currentTime().toString() << " [OUT] Prefix initialisation successful!";
        UnionFSString->prepend(DefPrefixDir->path() + "=RO");
        return true;
    }
    else
    {
        qDebug() << QTime::currentTime().toString() << " [ERR] Prefix initialisation failed!";
        return false;
    }
}

bool MainWindow::RunWithUMU(QDir * WorkDir, QDir * WinePrefix, QString GAMEID, QFile * ExeFile, QStringList * ExeArgs)
{
    qDebug() << QTime::currentTime().toString() << " [OUT] Executing with umu-launcher: " << ExeFile->fileName() << *ExeArgs;
    qDebug() << QTime::currentTime().toString() << " [OUT] WINEPREFIX:" << WinePrefix->path();
    qDebug() << QTime::currentTime().toString() << " [OUT] WORKDIR:" << WorkDir->path();

    QProcess * RunProcess = new QProcess(this);
    ExeArgs->prepend(ExeFile->fileName());
    RunProcess->setWorkingDirectory(WorkDir->path());
    RunProcess->setProgram("umu-run");

    if (!(*ExeArgs).isEmpty())
    {
        RunProcess->setArguments(*ExeArgs);
    }

    QProcessEnvironment RunProcessEnvironment = QProcessEnvironment::systemEnvironment();
    RunProcessEnvironment.insert("WINEPREFIX", WinePrefix->path());
    RunProcessEnvironment.insert("GAMEID", GAMEID);
    RunProcessEnvironment.insert("PROTONPATH", ProtonPath->path());
    RunProcessEnvironment.insert("PROTON_VERB", "waitforexitandrun");
    RunProcess->setProcessEnvironment(RunProcessEnvironment);
    RunProcess->start();
    RunProcess->waitForFinished(-1);
    qDebug().noquote() << RunProcess->readAllStandardError();
    qDebug().noquote() << RunProcess->readAllStandardOutput();

    if(RunProcess->exitCode() == 0)
    {
        delete RunProcess;
        delete ExeArgs;
        return true;
    }
    else
    {
        delete RunProcess;
        delete ExeArgs;
        return false;
    }
}

QJsonArray MainWindow::GetSubComponents(QString MetaDataPath, QList<int> Recipe)
{
    //BUILD AN ARRAY CONTAINING ALL SUBCOMPONENTS, IN ORDER, FILTERED BY RECIPE.
    QDir MetaDataDir = PackagePath; MetaDataDir.cd("METADATA");
    QJsonDocument * MANIFESTJSON = JSONOperations::LoadJSON(new QFile(MetaDataDir.filePath("MANIFEST.json")));
    QJsonArray SubComponentsArray;
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

    delete MANIFESTJSON;
    return SubComponentsArray;
}

bool MainWindow::ProcessSubComponents(const QJsonArray SubComponentsArray, QDir * TempDir, QDir * PackageFilesDir, QDir * DefPrefixDir, QDir * ProgramRuntimeDir, QString * UnionFSString, const QString ParentPackage)
{
    for (int i = 0; i < SubComponentsArray.count(); i++)
    {
        QJsonObject SubComponentJSON = SubComponentsArray[i].toObject();

        if (SubComponentJSON["TYPE"].toString() == "ZipFileLayer")
        {
            if(!MountZipFileLayer(SubComponentJSON, i, TempDir, PackageFilesDir, UnionFSString, ParentPackage))
            {
                return false;
            }
        }
        else if (SubComponentJSON["TYPE"].toString() == "RegEdit")
        {
            if(!RegEdit(SubComponentJSON, DefPrefixDir, ProgramRuntimeDir))
            {
                return false;
            }
        }
        else
        {
            qDebug() << QTime::currentTime().toString() << " [ERR] Invalid SubComponent:" << i;
            return false;
        }
    }
    return true;
}

bool MainWindow::RemoveSubComponents(QJsonArray SubComponentsArray, QDir * TempDir, QDir * PackageFilesDir, QString * UnionFSString, const QString ParentPackage)
{
    for (int i = 0; i < SubComponentsArray.count(); i++)
    {
        QJsonObject SubComponentJSON = SubComponentsArray[i].toObject();

        if (SubComponentJSON["TYPE"].toString() == "ZipFileLayer")
        {
            UnmountZipFileLayer(SubComponentJSON, i, TempDir, ParentPackage);
        }
    }

    return true;
}

bool MainWindow::BuildUnionFS(QString * UnionFSString, QDir * RuntimeDir, QDir * UserDataDir)
{
    //FINALIZE UNIONFS STRING
    UnionFSString->prepend(UserDataDir->path() + "=RW:");

    //BUILD UNIONFS USING PARAMS
    qDebug() << QTime::currentTime().toString() << "[OUT] Building UnionFS RUNTIME.";
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] UnionFSString:" << *UnionFSString;

    QProcess * BuildUnionFS = new QProcess(this);
    BuildUnionFS->setProgram("unionfs");
    BuildUnionFS->setArguments({"-o", "cow", "-o", "uid=1000", *UnionFSString, RuntimeDir->path()});
    BuildUnionFS->start();
    BuildUnionFS->waitForFinished(-1);
    qDebug().noquote() << BuildUnionFS->readAllStandardError();

    if(BuildUnionFS->exitCode() == 0)
    {
        qDebug() << QTime::currentTime().toString() << "[OUT] Successfully mounted UnionFS RUNTIME!";
        delete BuildUnionFS;
        return true;
    }
    else
    {
        qDebug() << QTime::currentTime().toString() << "[ERR] Failed to mount UnionFS RUNTIME!";
        delete BuildUnionFS;
        return false;
    }
}

bool MainWindow::DestroyUnionFS(QDir * RuntimeDir)
{
    QProcess * UnmountUnionFS = new QProcess(this);
    UnmountUnionFS->setProgram("fusermount");
    UnmountUnionFS->setArguments({"-u", RuntimeDir->path()});
    UnmountUnionFS->start();
    UnmountUnionFS->waitForFinished(-1);
    delete UnmountUnionFS;
    return true;
}

bool MainWindow::MountZipFileLayer(const QJsonObject SubComponentJSON, int LayerNumber, QDir * TempDir, QDir * PackageFilesDir, QString * UnionFSString, const QString ParentPackage)
{
    QDir SubComponentDir = *TempDir;
    SubComponentDir.mkdir("[" + QString::number(LayerNumber) + "]");
    SubComponentDir.cd("[" + QString::number(LayerNumber) + "]");
    UnionFSString->prepend(SubComponentDir.path() + "=RO:");

    QDir MountDir = SubComponentDir;
    MountDir.mkpath(MountDir.path() + QDir::separator() + "drive_c" + QDir::separator() + ParentPackage);
    MountDir.cd(MountDir.path() + QDir::separator() + "drive_c" + QDir::separator() + ParentPackage);

    if (!(SubComponentJSON["TARGET"].toString().isEmpty()))
    {
        MountDir.mkpath(QDir::cleanPath(MountDir.path() + QDir::separator() + SubComponentJSON["TARGET"].toString()));
        MountDir.cd(QDir::cleanPath(MountDir.path() + QDir::separator() + SubComponentJSON["TARGET"].toString()));
    }

    qDebug() << QTime::currentTime().toString() << "[OUT] Mounting ZipFileLayer" << PackageFilesDir->filePath(SubComponentJSON["PATH"].toString()) << "at" << MountDir.path();
    QProcess * MountZip = new QProcess(this);
    MountZip->setProgram("fuse-zip");
    MountZip->setArguments({"-r", PackageFilesDir->filePath(SubComponentJSON["PATH"].toString()), MountDir.path()});
    MountZip->start();
    MountZip->waitForFinished(-1);

    if(MountZip->exitCode() == 0)
    {
        delete MountZip;
        return true;
    }
    else
    {
        qDebug() << QTime::currentTime().toString() << "[ERR] Failed to mount zip file layer" << PackageFilesDir->filePath(SubComponentJSON["PATH"].toString());
        delete MountZip;
        return false;
    }
}

bool MainWindow::UnmountZipFileLayer(const QJsonObject SubComponentJSON, int LayerNumber, QDir * TempDir, const QString ParentPackage)
{
    QDir SubComponentDir = *TempDir;
    SubComponentDir.mkdir("[" + QString::number(LayerNumber) + "]");
    SubComponentDir.cd("[" + QString::number(LayerNumber) + "]");

    QDir MountDir = SubComponentDir;
    MountDir.cd(MountDir.path() + QDir::separator() + "drive_c" + QDir::separator() + ParentPackage);

    if (!(SubComponentJSON["TARGET"].toString().isEmpty()))
    {
        MountDir.mkpath(QDir::cleanPath(MountDir.path() + QDir::separator() + SubComponentJSON["TARGET"].toString()));
        MountDir.cd(QDir::cleanPath(MountDir.path() + QDir::separator() + SubComponentJSON["TARGET"].toString()));
    }

    QProcess * UnmountZips = new QProcess(this);
    UnmountZips->setProgram("fusermount");
    UnmountZips->setArguments({"-u", MountDir.path()});
    UnmountZips->start();
    UnmountZips->waitForFinished(-1);
    qDebug() << QTime::currentTime().toString() << "[OUT] UNMOUNT EXIT STATUS: " << UnmountZips->exitCode();
    delete UnmountZips;
    return true;
}

bool MainWindow::RegEdit(QJsonObject SubComponentJSON, QDir * DefPrefixDir, QDir * ProgramRuntimeDir)
{
    QString REGPATH = SubComponentJSON["REGPATH"].toString();

    if (SubComponentJSON.keys().contains("KEYVALUES"))
    {
        for (auto Key : SubComponentJSON["KEYVALUES"].toObject().keys())
        {
            QStringList * CommandArgs = new QStringList();
            CommandArgs->append("add");      CommandArgs->append(REGPATH);
            CommandArgs->append("/v");       CommandArgs->append(Key);
            CommandArgs->append("/t");       CommandArgs->append(SubComponentJSON["KEYVALUES"].toObject()[Key].toObject()["TYPE"].toString());
            if (SubComponentJSON["KEYVALUES"].toObject()[Key].toObject().keys().contains("VALUE"))
            {
                CommandArgs->append("/d");       CommandArgs->append(SubComponentJSON["KEYVALUES"].toObject()[Key].toObject()["VALUE"].toString());
            }
            CommandArgs->append("/f");

            if(SubComponentJSON["ARCHITECTURE"].toString() == "32")
            {
                CommandArgs->append("/reg:32");
            }
            qDebug().noquote() << QTime::currentTime().toString() << "REG STRING: " << *CommandArgs;
            RunWithUMU(DefPrefixDir, DefPrefixDir, "0", new QFile("reg"), CommandArgs);
        }
    }
    else
    {
        QStringList * CommandArgs = new QStringList();
        CommandArgs->append("add");      CommandArgs->append(REGPATH);
        CommandArgs->append("/f");

        if(SubComponentJSON["ARCHITECTURE"].toString() == "32")
        {
            CommandArgs->append("/reg:32");
        }

        qDebug().noquote() << QTime::currentTime().toString() << "REG STRING: " << *CommandArgs;
        RunWithUMU(DefPrefixDir, DefPrefixDir, "0", new QFile("reg"), CommandArgs);
    }
    return true;
}

//bool MainWindow::RegAdd()
//{
//
//}

QString MainWindow::GetItemFromOtherTableByRelation(QTableView * OtherTable, QString Relation, QString RelationColumn, QString ItemColumn)
{
    int row = GetRowByItemAndColumn(OtherTable, Relation, RelationColumn);
    int col = GetHeaderColumn(OtherTable->model(), ItemColumn);
    QString ResultString = OtherTable->model()->data(OtherTable->model()->index(row, col)).toString();
    return ResultString;
}

QList<int> MainWindow::IntListFromString(QString String)
{
    QList<int> IntList;
    foreach (QString SubString, String.split(","))
    {
        IntList.append(SubString.toInt());
    }
    return IntList;
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
    qDebug() << QTime::currentTime().toString() << "[OUT] " << Item << " not found in column " << Column;
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

bool MainWindow::CheckCaseConflicts(QDir * RuntimeDir)
{
    QStringList * FileList = new QStringList;
    bool NoConflict = true;

    QDirIterator Iterator(*RuntimeDir, QDirIterator::Subdirectories);
    while (Iterator.hasNext()) {
        QString FilePath = Iterator.next().toLower();
        if (FileList->contains(FilePath))
        {
            NoConflict = false;
            qDebug() << QTime::currentTime() << "[ERR] Case conflict found: " << FilePath;
        }
        else
        {
            FileList->append(FilePath);
        }
    }
    delete FileList;
    return NoConflict;
}
