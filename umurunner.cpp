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

    if (!ProcessSubComponents())
    {
        return false;
    }

    if (!FSOps::BuildUnionFS(RunnerParams->UnionFSString, RunnerParams->Paths["RuntimePath"], RunnerParams->Paths["UserDataPath"]))
    {
        return false;
    }

    FSOps::CheckCaseConflicts(RunnerParams->Paths["RuntimePath"]);

    bool ExitStatus = RunWithUMU(RunnerParams->Paths["ProtonPath"],
                                 RunnerParams->Paths["WorkDirPath"],
                                 RunnerParams->Paths["RuntimePath"],
                                 RunnerParams->UMUID,
                                 RunnerParams->ExePath,
                                 RunnerParams->ExeArgs);

    return ExitStatus;
}

bool UMURunner::InitializeUMUPrefix(QString PrefixPath, QString ProtonPath, QString * UnionFSString)
{
    qDebug() << QTime::currentTime().toString() << " [OUT] Initialising prefix" << PrefixPath;
    if (RunWithUMU(ProtonPath, PrefixPath, PrefixPath, "0", "wineboot", {}))
    {
        qDebug() << QTime::currentTime().toString() << " [OUT] Prefix initialisation successful!";
        UnionFSString->prepend(RunnerParams->Paths["DefPrefixPath"] + "=RO");
        return true;
    }
    qDebug() << QTime::currentTime().toString() << " [ERR] Prefix initialisation failed!";
    return false;
}

bool UMURunner::ProcessSubComponents()
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
        else if (SubComponentJSON["TYPE"].toString() == "RegEdit")
        {
            if(!RegOps::RegAdd(SubComponentJSON, RunnerParams->Paths["DefPrefixPath"], RunnerParams->Paths["ProtonPath"]))
            {
                return false;
            }
        }
        else
        {
            qDebug() << QTime::currentTime().toString() << " [ERR] Invalid SubComponent:" << i;
            return false;
        }
    }
    return true;
}


bool UMURunner::Cleanup()
{
    QMessageBox::warning(nullptr, "Ready for cleanup...", "Press OK to start cleanup");

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

bool UMURunner::RunWithUMU(QString ProtonPath, QString WorkDirPath, QString WinePrefix, QString GAMEID, QString ExePath, QStringList ExeArgs)
{
    qDebug() << QTime::currentTime().toString() << " [OUT] Executing with umu-launcher: " << ExePath << ExeArgs;
    qDebug() << QTime::currentTime().toString() << " [OUT] WINEPREFIX:" << WinePrefix;
    qDebug() << QTime::currentTime().toString() << " [OUT] WORKDIR:" << WorkDirPath;
    qDebug() << QTime::currentTime().toString() << " [OUT] GAMEID:" << GAMEID;
    qDebug() << QTime::currentTime().toString() << " [OUT] ExePath:" << ExePath;
    qDebug() << QTime::currentTime().toString() << " [OUT] ExeArgs:" << ExeArgs;

    QProcess * RunProcess = new QProcess;

    ExeArgs.prepend(ExePath);
    RunProcess->setWorkingDirectory(WorkDirPath);
    RunProcess->setProgram("umu-run");
    RunProcess->setArguments(ExeArgs);

    QProcessEnvironment RunProcessEnvironment = QProcessEnvironment::systemEnvironment();
    RunProcessEnvironment.insert("WINEPREFIX", WinePrefix);
    RunProcessEnvironment.insert("GAMEID", GAMEID);
    RunProcessEnvironment.insert("PROTONPATH", ProtonPath);
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
