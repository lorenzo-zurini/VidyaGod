#include "runnerparams.h"

RunnerParams::RunnerParams(QTableView * LibraryTableView, QTableView * PackagesTableView, QString ProtonPath)
{
    GameName                    = DBOps::GetSelectedItemByColumn(LibraryTableView, "TITLE");
    UMUID                       = DBOps::GetSelectedItemByColumn(LibraryTableView, "UMUID");
    ParentPackage               = DBOps::GetSelectedItemByColumn(LibraryTableView, "PARENTPACKAGE");
    ExePath                     = QDir::cleanPath("C:/" + ParentPackage + QDir::separator() + DBOps::GetSelectedItemByColumn(LibraryTableView, "EXEPATH"));
    Recipe                      = DBOps::IntListFromString(DBOps::GetSelectedItemByColumn(LibraryTableView, "RECIPE"));

    //MUST MAKE THIS RESPECT QUOTES, ACCOUNT FOR EMPTY.
    ExeArgs                     = DBOps::GetSelectedItemByColumn(LibraryTableView, "EXEARGS").split(" ");

    Paths["ProtonPath"]         = ProtonPath;
    Paths["PackagePath"]        = DBOps::GetItemFromOtherTableByRelation(PackagesTableView, ParentPackage, "PACKAGEUID", "PATH");
    Paths["RuntimePath"]        = FSOps::SubPath(Paths["PackagePath"], "RUNTIME");
    Paths["ProgramPath"]        = FSOps::SubPath(FSOps::SubPath(Paths["RuntimePath"], "drive_c"), ParentPackage);
    Paths["WorkDirPath"]        = FSOps::SubPath(Paths["ProgramPath"], DBOps::GetSelectedItemByColumn(LibraryTableView, "WORKDIR"));
    Paths["MetaDataPath"]       = FSOps::SubPath(Paths["PackagePath"], "METADATA");
    Paths["PackageFilesPath"]   = FSOps::SubPath(Paths["PackagePath"], "PACKAGEFILES");
    Paths["UserDataPath"]       = FSOps::SubPath(Paths["PackagePath"], "USERDATA");
    Paths["TempPath"]           = FSOps::SubPath(Paths["PackagePath"], "TEMP");
    Paths["DefPrefixPath"]      = FSOps::SubPath(Paths["TempPath"], "DEFPREFIX");

    SubComponentsArray          = JSONOps::GetSubComponents(Paths["MetaDataPath"], Recipe);
}
