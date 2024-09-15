#include "umurunner.h"
#include "runnerparams.h"
#include "filesystemoperations.h"
#include "registryoperations.h"

UMURunner::UMURunner(QTableView * LibraryTableView, QTableView * PackagesTableView, QString ProtonPath)
{
    RunnerParams = new class RunnerParams(LibraryTableView, PackagesTableView, ProtonPath);
}

bool UMURunner::Run()
{
    if (!FSOps::CreateDirectories(RunnerParams->Paths))
    {
        return false;
    }

    if (!InitializeUMUPrefix(RunnerParams->Paths["DefPrefixPath"], RunnerParams->Paths["ProtonPath"], &RunnerParams->UnionFSString))
    {
        return false;
    }

    if (!ProcessFileSystemSubComponents())
    {
        return false;
    }

    if (!FSOps::BuildUnionFS(RunnerParams->UnionFSString, RunnerParams->Paths["RuntimePath"], RunnerParams->Paths["UserDataPath"]))
    {
        return false;
    }

    if (!ProcessOtherSubComponents())
    {
        return false;
    }

    FSOps::CheckCaseConflicts(RunnerParams->Paths["RuntimePath"]);

    bool ExitStatus = RunWithUMU(RunnerParams->Paths["ProtonPath"],
                                 RunnerParams->Paths["RuntimePath"],
                                 RunnerParams->ExePath,
                                 RunnerParams->ExeArgs,
                                 RunnerParams->Paths["WorkDirPath"],
                                 RunnerParams->UMUID,
                                 RunnerParams->DllOverrides);

    return ExitStatus;
}

bool UMURunner::InitializeUMUPrefix(QString PrefixPath, QString ProtonPath, QString * UnionFSString)
{
    qDebug() << QTime::currentTime().toString() << " [OUT] Initialising prefix" << PrefixPath;
    if (RunWithUMU(ProtonPath, PrefixPath, "wineboot"))
    {
        qDebug() << QTime::currentTime().toString() << " [OUT] Prefix initialisation successful!";
        UnionFSString->prepend(RunnerParams->Paths["DefPrefixPath"] + "=RO");
        return true;
    }
    qDebug() << QTime::currentTime().toString() << " [ERR] Prefix initialisation failed!";
    return false;
}

bool UMURunner::ProcessFileSystemSubComponents()
{
    for (int i = 0; i < RunnerParams->SubComponentsArray.count(); i++)
    {
        QJsonObject SubComponentJSON = RunnerParams->SubComponentsArray[i].toObject();

        if (SubComponentJSON["TYPE"].toString() == "ZipFileLayer")
        {
            if(!FSOps::MountZipFileLayer(SubComponentJSON, i, RunnerParams->Paths["TempPath"], RunnerParams->Paths["PackageFilesPath"], &RunnerParams->UnionFSString, RunnerParams->ParentPackage))
            {
                return false;
            }
        }
    }
    return true;
}

bool UMURunner::ProcessOtherSubComponents()
{
    for (int i = 0; i < RunnerParams->SubComponentsArray.count(); i++)
    {
        QJsonObject SubComponentJSON = RunnerParams->SubComponentsArray[i].toObject();

        if (SubComponentJSON["TYPE"].toString() == "RegEdit")
        {
            if(!RegOps::RegAdd(SubComponentJSON, RunnerParams->Paths["RuntimePath"], RunnerParams->Paths["ProtonPath"]))
            {
                return false;
            }
        }
        else if (SubComponentJSON["TYPE"].toString() == "DllOverride")
        {
            if (!RunnerParams->DllOverrides.isEmpty())
            {
                RunnerParams->DllOverrides.append(";" + SubComponentJSON["DLLOVERRIDE"].toString());
            }
            else
            {
                RunnerParams->DllOverrides.append(SubComponentJSON["DLLOVERRIDE"].toString());
            }
        }
        else if (SubComponentJSON["TYPE"].toString() == "FileEdit")
        {
            if (SubComponentJSON["MODE"].toString() == "ConfigWrite")
            {
                FSOps::ConfigWrite(SubComponentJSON["KEY"].toString(), SubComponentJSON["VALUE"].toString(), SubComponentJSON["FILE"].toString());
            }
        }
    }
    return true;
}

bool UMURunner::Cleanup()
{
    if (!FSOps::DestroyUnionFS(RunnerParams->Paths["RuntimePath"]))
    {
        QMessageBox::warning(nullptr, "CLEANUP INCOMPLETE.", "Could not complete cleanup.");
        return false;
    }

    if (!RemoveSubComponents())
    {
        QMessageBox::warning(nullptr, "CLEANUP INCOMPLETE.", "Could not complete cleanup.");
        return false;
    }

    if (!QDir(RunnerParams->Paths["TempPath"]).removeRecursively())
    {
        QMessageBox::warning(nullptr, "CLEANUP INCOMPLETE.", "Could not complete cleanup.");
        return false;
    }

    if (!QDir(RunnerParams->Paths["RuntimePath"]).removeRecursively())
    {
        QMessageBox::warning(nullptr, "CLEANUP INCOMPLETE.", "Could not complete cleanup.");
        return false;
    }

    return true;
}

bool UMURunner::RemoveSubComponents()
{
    for (int i = 0; i < RunnerParams->SubComponentsArray.count(); i++)
    {
        QJsonObject SubComponentJSON = RunnerParams->SubComponentsArray[i].toObject();

        if (SubComponentJSON["TYPE"].toString() == "ZipFileLayer")
        {
            FSOps::UnmountZipFileLayer(SubComponentJSON, i, RunnerParams->Paths["TempPath"], RunnerParams->ParentPackage);
        }
    }

    return true;
}

bool UMURunner::RunWithUMU(QString ProtonPath, QString WinePrefix, QString ExePath, QStringList ExeArgs, QString WorkDirPath, QString GAMEID, QString DllOverrides)
{
    qDebug() << QTime::currentTime().toString() << " [OUT] Executing with umu-launcher: " << ExePath << ExeArgs;
    qDebug() << QTime::currentTime().toString() << " [OUT] ProtonPath:" << ProtonPath;
    qDebug() << QTime::currentTime().toString() << " [OUT] WinePrefix:" << WinePrefix;
    qDebug() << QTime::currentTime().toString() << " [OUT] ExePath:" << ExePath;
    qDebug() << QTime::currentTime().toString() << " [OUT] ExeArgs:" << ExeArgs;
    qDebug() << QTime::currentTime().toString() << " [OUT] WorkDirPath:" << WorkDirPath;
    qDebug() << QTime::currentTime().toString() << " [OUT] GAMEID:" << GAMEID;
    qDebug() << QTime::currentTime().toString() << " [OUT] DllOverrides:" << DllOverrides;


    QProcessEnvironment RunProcessEnvironment = QProcessEnvironment::systemEnvironment();
    QProcess * RunProcess = new QProcess;
    RunProcess->setProgram("umu-run");

    RunProcessEnvironment.insert("PROTONPATH", ProtonPath);
    RunProcessEnvironment.insert("WINEPREFIX", WinePrefix);
    ExeArgs.prepend(ExePath);

    if (!ExeArgs.isEmpty())
    {
        RunProcess->setArguments(ExeArgs);
    }

    if (!WorkDirPath.isEmpty())
    {
        RunProcess->setWorkingDirectory(WorkDirPath);
    }

    RunProcessEnvironment.insert("GAMEID", GAMEID);

    if (!DllOverrides.isEmpty())
    {
        RunProcessEnvironment.insert("WINEDLLOVERRIDES", DllOverrides);
    }

    RunProcessEnvironment.insert("PROTON_VERB", "waitforexitandrun");

    RunProcess->setProcessEnvironment(RunProcessEnvironment);
    RunProcess->start();
    RunProcess->waitForFinished(-1);
    qDebug().noquote() << RunProcess->readAllStandardError();
    qDebug().noquote() << RunProcess->readAllStandardOutput();

    if(RunProcess->exitCode() == 0)
    {
        delete RunProcess;
        return true;
    }
    else
    {
        delete RunProcess;
        return false;
    }
}
