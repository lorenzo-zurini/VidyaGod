#include "filesystemoperations.h"
#include "commonutils.h"

FSOps::FSOps() {}

//Checks that PackageDir points to a valid VidyaGod package.
//A package is considered valid when it contains a METADATA subdirectory;
//the actual MANIFEST.json inside it is validated separately by JSONOps::LoadJSON.
//Returns false (invalid) if the path is empty or the METADATA directory does not exist.
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
    //Without METADATA there is no MANIFEST.json, so the package cannot be launched.
    if (!QDir(QDir::cleanPath(PackageDir->path() + QDir::separator() + "METADATA")).exists())
    {
        LogErr("FSOperations", "Selected directory does not contain METADATA subdirectory.");
        return false;
    }

    LogSucc("FSOperations", "Valid package!");
    return true;
}

//Returns a platform-normalized path by joining Parent and SubDir with the OS separator
//and collapsing redundant separators and "." / ".." components.
QString FSOps::SubPath(QString Parent, QString SubDir)
{
    return QDir::cleanPath(Parent + QDir::separator() + SubDir);
}
