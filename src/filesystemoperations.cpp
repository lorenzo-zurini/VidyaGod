#include "filesystemoperations.h"
#include "commonutils.h"

FSOps::FSOps() {}

bool FSOps::CheckPackageValid(QDir * PackageDir)
{
    //Check if the path is empty, such as when the file picker was canceled.
    if (PackageDir->path().isEmpty())
    {
        LogErr("FSOperations", "Path is empty. Canceled?");
        return false;
    }

    LogOut("FSOperations", "Scanning " + PackageDir->path().toStdString());

    //Check if the directory contains a METADATA subdirectory.
    if (!QDir(QDir::cleanPath(PackageDir->path() + QDir::separator() + "METADATA")).exists())
    {
        LogErr("FSOperations", "Selected directory does not contain METADATA subdirectory.");
        return false;
    }

    LogSucc("FSOperations", "Valid package!");
    return true;
}

QString FSOps::SubPath(QString Parent, QString SubDir)
{
    return QDir::cleanPath(Parent + QDir::separator() + SubDir);
}
