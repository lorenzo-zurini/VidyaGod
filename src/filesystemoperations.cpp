#include "filesystemoperations.h"
#include "commonutils.h"
#include "jsonoperations.h"
#include "manifestmodel.h"

FSOps::FSOps() {}

//Checks that PackageDir points to a valid VidyaGod bundle (everything-is-a-node): a directory that holds at
//least one <node_id>.json node file. Returns false if the path is empty or no node file is found.
bool FSOps::CheckPackageValid(QDir * PackageDir)
{
    if (PackageDir->path().isEmpty())
    {
        LogErr("FSOperations", "Path is empty. Canceled?");
        return false;
    }

    LogOut("FSOperations", "Scanning " + PackageDir->path().toStdString());

    NodeIndex Idx;
    ManifestModel::ScanBundleNodes(PackageDir->path().toStdString(), Idx);
    if (Idx.Nodes.empty())
    {
        //A probe result, not a failure: both callers (the startup in-package check, the import recursion) treat
        //"not a bundle" as the normal negative. As an ERR it fired on EVERY app start and was the one permanent
        //red line in an otherwise clean log — exactly the noise that teaches people to skim past ERR.
        LogOut("FSOperations", "Not a package bundle (no NODE_ID node files here).");
        return false;
    }

    LogSucc("FSOperations", "Valid bundle!");
    return true;
}

//Returns a platform-normalized path by joining Parent and SubDir with the OS separator
//and collapsing redundant separators and "." / ".." components.
QString FSOps::SubPath(QString Parent, QString SubDir)
{
    return QDir::cleanPath(Parent + QDir::separator() + SubDir);
}
