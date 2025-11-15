#include "filesystemoperations.h"

FSOps::FSOps() {}

bool FSOps::CheckPackageValid(QDir * PackageDir)
{
    //Check if the path is empty, such as when the file picker was canceled.
    if (PackageDir->path().isEmpty())
    {
        std::cout << QTime::currentTime().toString().toStdString() << "FSOperations:" << "[ERR] Path is empty. Canceled?" << std::endl;
        return false;
    }

    std::cout << QTime::currentTime().toString().toStdString() << "FSOperations:" << "[OUT] Scanning" << PackageDir->path().toStdString() << std::endl;

    //Check if the directory contains a METADATA subdirectory.
    if (!QDir(QDir::cleanPath(PackageDir->path() + QDir::separator() + "METADATA")).exists())
    {
        std::cout << QTime::currentTime().toString().toStdString() << "FSOperations:" << "[ERR] Selected directory does not contain METADATA subdirectory." << std::endl;
        return false;
    }

    std::cout << QTime::currentTime().toString().toStdString() << "FSOperations:" << "[OUT] Valid package!" << std::endl;
    return true;
}

//bool FSOps::MountZipFileLayer(nlohmann::ordered_json SubComponentJSON, int LayerNumber, QString TempPath, QString PackageFilesPath, QString * UnionFSString, QString PackageName)
//{
//
//}

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
            //qDebug().noquote() << QTime::currentTime() << "FSOperations:" << "[ERR] Case conflict found: " << FilePath;
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
    //qDebug().noquote() << QTime::currentTime() << "FSOperations:" << "[OUT] Editing file:" << FilePath << " Key: " << Key << " Value: " << Value;
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
        //qDebug().noquote() << QTime::currentTime() << "FSOperations:" << "[ERR] Could not open file for ConfigWrite: " << FilePath;
        return false;
    }
}
