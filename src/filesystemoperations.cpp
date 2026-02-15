#include "filesystemoperations.h"

FSOps::FSOps() {}

bool FSOps::CheckPackageValid(QDir * PackageDir)
{
    //Check if the path is empty, such as when the file picker was canceled.
    if (PackageDir->path().isEmpty())
    {
        std::cout << QTime::currentTime().toString().toStdString() << " FSOperations:" << " [ERR] Path is empty. Canceled?" << std::endl;
        return false;
    }

    std::cout << QTime::currentTime().toString().toStdString() << " FSOperations:" << " [OUT] Scanning" << PackageDir->path().toStdString() << std::endl;

    //Check if the directory contains a METADATA subdirectory.
    if (!QDir(QDir::cleanPath(PackageDir->path() + QDir::separator() + "METADATA")).exists())
    {
        std::cout << QTime::currentTime().toString().toStdString() << " FSOperations:" << " [ERR] Selected directory does not contain METADATA subdirectory." << std::endl;
        return false;
    }

    std::cout << QTime::currentTime().toString().toStdString() << " FSOperations:" << " [OUT] Valid package!" << std::endl;
    return true;
}

QString FSOps::SubPath(QString Parent, QString SubDir)
{
    return QDir::cleanPath(Parent + QDir::separator() + SubDir);
}
