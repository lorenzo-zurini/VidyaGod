#include "filesystemoperations.h"

FSOps::FSOps() {}

bool FSOps::CheckPackageValid(QDir * PackageDir)
{
    //Check if the path is empty, such as when the file picker was canceled.
    if (PackageDir->path().isEmpty())
    {
        qDebug() << QTime::currentTime().toString() << " [ERR] Path is empty. Canceled?";
        return false;
    }

    qDebug() << QTime::currentTime().toString() << " [OUT] Scanning " << PackageDir->path();

    //Check if the directory contains a METADATA subdirectory.
    if (!QDir(QDir::cleanPath(PackageDir->path() + QDir::separator() + "METADATA")).exists())
    {
        qDebug() << QTime::currentTime().toString() << " [ERR] Selected directory does not contain METADATA subdirectory.";
        return false;
    }

    qDebug() << QTime::currentTime().toString() << " [OUT] Valid package!";
    return true;
}

bool FSOps::CreateDirectories (QMap<QString, QString> PathsMap)
{
    for (auto [Name, Path] : PathsMap.asKeyValueRange())
    {
        QDir Dir(Path);
        if (Dir.mkpath(Path))
        {
            qDebug() << QTime::currentTime().toString() << " [OUT] Created direcotry " << Name << "PATH: " << Path;
        }
        else
        {
            qDebug() << QTime::currentTime().toString() << " [ERR] Could not create directory " << Name << "PATH: " << Path;
            return false;
        }
    }
    return true;
}

bool FSOps::MountZipFileLayer(nlohmann::ordered_json SubComponentJSON, int LayerNumber, QString TempPath, QString PackageFilesPath, QString * UnionFSString, QString ParentPackage)
{
    QDir SubComponentDir = TempPath;
    SubComponentDir.mkdir("[" + QString::number(LayerNumber) + "]");
    SubComponentDir.cd("[" + QString::number(LayerNumber) + "]");
    UnionFSString->prepend(SubComponentDir.path() + "=RO:");

    QDir MountDir = SubComponentDir;
    MountDir.mkpath(MountDir.path() + QDir::separator() + "drive_c" + QDir::separator() + ParentPackage);
    MountDir.cd(MountDir.path() + QDir::separator() + "drive_c" + QDir::separator() + ParentPackage);

    if (SubComponentJSON.contains("TARGET"))
    {
        if (!(SubComponentJSON["TARGET"].empty()))
        {
            MountDir.mkpath(QDir::cleanPath(MountDir.path() + QDir::separator() + QString::fromStdString(SubComponentJSON["TARGET"])));
            MountDir.cd(QDir::cleanPath(MountDir.path() + QDir::separator() + QString::fromStdString(SubComponentJSON["TARGET"])));
        }
    }

    QString FilePath = QDir::cleanPath(PackageFilesPath + QDir::separator() + QString::fromStdString(SubComponentJSON["PATH"]));

    qDebug() << QTime::currentTime().toString() << "[OUT] Mounting ZipFileLayer" << FilePath << "at" << MountDir.path();
    QProcess * MountZip = new QProcess;
    MountZip->setProgram("fuse-zip");
    MountZip->setArguments({"-r", FilePath, MountDir.path()});
    MountZip->start();
    MountZip->waitForFinished(-1);

    if(MountZip->exitCode() == 0)
    {
        delete MountZip;
        return true;
    }
    else
    {
        qDebug() << QTime::currentTime().toString() << "[ERR] Failed to mount zip file layer" << FilePath;
        delete MountZip;
        return false;
    }
}

bool FSOps::UnmountZipFileLayer(nlohmann::ordered_json SubComponentJSON, int LayerNumber, QString TempPath, QString ParentPackage)
{
    QDir SubComponentDir = TempPath;
    SubComponentDir.mkdir("[" + QString::number(LayerNumber) + "]");
    SubComponentDir.cd("[" + QString::number(LayerNumber) + "]");

    QDir MountDir = SubComponentDir;
    MountDir.cd(MountDir.path() + QDir::separator() + "drive_c" + QDir::separator() + ParentPackage);

    if (!(SubComponentJSON["TARGET"].empty()))
    {
        MountDir.mkpath(QDir::cleanPath(MountDir.path() + QDir::separator() + QString::fromStdString(SubComponentJSON["TARGET"])));
        MountDir.cd(QDir::cleanPath(MountDir.path() + QDir::separator() + QString::fromStdString(SubComponentJSON["TARGET"])));
    }

    QProcess * UnmountZips = new QProcess;
    UnmountZips->setProgram("fusermount");
    UnmountZips->setArguments({"-u", MountDir.path()});
    UnmountZips->start();
    UnmountZips->waitForFinished(-1);
    qDebug() << QTime::currentTime().toString() << "[OUT] UNMOUNT EXIT STATUS: " << UnmountZips->exitCode();
    delete UnmountZips;
    return true;
}

bool FSOps::BuildUnionFS(QString UnionFSString, QString RuntimePath, QString UserDataPath)
{
    //FINALIZE UNIONFS STRING
    UnionFSString.prepend(UserDataPath + "=RW:");

    //BUILD UNIONFS USING PARAMS
    qDebug() << QTime::currentTime().toString() << "[OUT] Building UnionFS RUNTIME.";
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] UnionFSString:" << UnionFSString;

    QProcess * BuildUnionFS = new QProcess;
    BuildUnionFS->setProgram("unionfs");
    BuildUnionFS->setArguments({"-o", "cow", "-o", "uid=1000", UnionFSString, RuntimePath});
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

bool FSOps::DestroyUnionFS(QString RuntimePath)
{
    QProcess * UnmountUnionFS = new QProcess;
    UnmountUnionFS->setProgram("fusermount");
    UnmountUnionFS->setArguments({"-u", RuntimePath});
    UnmountUnionFS->start();
    UnmountUnionFS->waitForFinished(-1);
    delete UnmountUnionFS;
    return true;
}

QString FSOps::SubPath(QString Parent, QString SubDir)
{
    return QDir::cleanPath(Parent + QDir::separator() + SubDir);
}

bool FSOps::CheckCaseConflicts(QString RuntimePath)
{
    QStringList * FileList = new QStringList;
    bool NoConflict = true;

    QDirIterator Iterator(RuntimePath, QDirIterator::Subdirectories);
    while (Iterator.hasNext()) {
        QString FilePath = Iterator.next().toLower();
        if (FileList->contains(FilePath))
        {
            NoConflict = false;
            qDebug() << QTime::currentTime() << " [ERR] Case conflict found: " << FilePath;
        }
        else
        {
            FileList->append(FilePath);
        }
    }
    delete FileList;
    return NoConflict;
}

bool FSOps::ConfigWrite(QString Key, QString Value, QString FilePath)
{
    qDebug() << QTime::currentTime() << " [OUT] Editing file: " << FilePath << " Key: " << Key << " Value: " << Value;
    QFile ConfigFile(FilePath);
    QTextStream OutFile(&ConfigFile);
    if (ConfigFile.open(QFile::ReadWrite | QFile::Text))
    {
        QStringList LinesList;
        while (!ConfigFile.atEnd())
        {
            LinesList.append(ConfigFile.readLine());
        }

        ConfigFile.seek(0);

        for (int i = 0; i < LinesList.count(); i++)
        {
            if (!(LinesList[i].length() < Key.length()))
            {
                if (LinesList[i].first(Key.length()) == Key)
                {
                    LinesList[i] = Key + Value;
                    OutFile << LinesList[i] << Qt::endl;
                }
                else
                {
                    OutFile << LinesList[i];
                }
            }
            else
            {
                OutFile << LinesList[i];
            }
        }
        ConfigFile.close();
        return true;
    }
    else
    {
        qDebug() << QTime::currentTime() << " [ERR] Could not open file for ConfigWrite: " << FilePath;
        return false;
    }
}
