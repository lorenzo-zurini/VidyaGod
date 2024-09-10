#include "filesystemoperations.h"

FileSystemOperations::FileSystemOperations() {}

bool FileSystemOperations::CheckPackageValid(QDir * PackageDir)
{
    //Check if the path is empty, such as when the file picker was canceled.
    if (PackageDir->path().isEmpty())
    {
        qDebug() << QTime::currentTime().toString() << " [ERR] Path is empty. Canceled?";
        return false;
    }

    qDebug() << QTime::currentTime().toString() << " [OUT] Scanning " << PackageDir->path();

    //Check if the directory contains a METADATA subdirectory.
    if (!PackageDir->cd("METADATA"))
    {
        qDebug() << QTime::currentTime().toString() << " [ERR] Selected directory does not contain METADATA subdirectory.";
        return false;
    }

    return true;
}
