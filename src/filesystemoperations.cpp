#include "filesystemoperations.h"
#include "commonutils.h"
#include "jsonoperations.h"

FSOps::FSOps() {}

//Checks that PackageDir points to a valid VidyaGod package.
//A package is valid when its directory contains at least one *.json file whose assembled,
//merged manifest yields a PACKAGEUID (modular manifests — no single MANIFEST.json required).
//Returns false if the path is empty or no usable manifest with identity is found.
bool FSOps::CheckPackageValid(QDir * PackageDir)
{
    if (PackageDir->path().isEmpty())
    {
        LogErr("FSOperations", "Path is empty. Canceled?");
        return false;
    }

    LogOut("FSOperations", "Scanning " + PackageDir->path().toStdString());

    nlohmann::ordered_json Assembled;
    std::vector<std::string> AsmWarn;
    if (!JSONOps::AssembleManifest(PackageDir->path(), Assembled, AsmWarn) ||
        !Assembled.contains("PACKAGEUID") || Assembled["PACKAGEUID"].is_null())
    {
        LogErr("FSOperations", "Selected directory has no usable JSON manifest (PACKAGEUID).");
        return false;
    }
    if (Assembled.contains("__VG_ERRORS__"))
    {
        LogErr("FSOperations", "Directory holds conflicting package identities (one package per directory).");
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
