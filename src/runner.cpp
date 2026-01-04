#include "runner.h"

Runner::Runner(
                QDir * PackageDir,
                nlohmann::ordered_json * MANIFESTJSON,
                nlohmann::ordered_json * GlobalConfigJSON,
                int subgame,
                int component
              ):
                PackageDir(PackageDir),
                MANIFESTJSON(MANIFESTJSON),
                GlobalConfigJSON(GlobalConfigJSON),
                subgame(subgame),
                component(component)
{
    this->InitParams();
}

bool Runner::InitParams()
{


































}

bool Runner::BuildRuntime(QString OverrideRuntimePath, QString OverrideUserDataPath)
{
    QString FinalRuntimePath = this->Paths["RuntimePath"];
    if (!OverrideRuntimePath.isNull())
    {
        std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] Passed RuntimePath" << OverrideRuntimePath.toStdString() << std::endl;
        FinalRuntimePath = OverrideRuntimePath;
    }

    QString FinalUserDataPath = this->Paths["UserDataPath"];
    if (!OverrideUserDataPath.isNull())
    {
        std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] Passed UserDataPath" << OverrideUserDataPath.toStdString() << std::endl;
        FinalUserDataPath = OverrideUserDataPath;
    }

    // Creating all necessary directories based on the Paths QMap.
    // QMessageBox::warning(nullptr, "Building Runtime", "Creating directories.");
    for (auto it = this->Paths.constBegin(); it != this->Paths.constEnd(); ++it)
    {
        const QString &Name = it.key();
        const QString &Path = it.value();
    
        QDir Dir(Path);
        if (Dir.mkpath(Path))
        {
            std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] Created directory " << Name.toStdString() << " PATH: " << Path.toStdString() << std::endl;
        }
        else
        {
            std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[ERR] Could not create directory " << Name.toStdString() << " PATH: " << Path.toStdString() << std::endl;
            return false;
        }
    }
    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    //Start building UnionFSString.
    QString UnionFSString;

    //Initialising UMU prefix.
    std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] Initialising prefix" << this->Paths["DefPrefixPath"].toStdString() << std::endl;

    QProcessEnvironment RunProcessEnvironment = QProcessEnvironment::systemEnvironment();
    QProcess * RunProcess = new QProcess;
    RunProcess->setProgram("umu-run");
    RunProcess->setArguments({"wineboot"});

    //RunProcessEnvironment.insert("PROTONPATH", this->Paths["ProtonPath"]);
    RunProcessEnvironment.insert("WINEPREFIX", this->Paths["DefPrefixPath"]);
    RunProcessEnvironment.insert("GAMEID", "0");
    RunProcessEnvironment.insert("PROTON_VERB", "waitforexitandrun");
    RunProcessEnvironment.remove("LD_LIBRARY_PATH");

    RunProcess->setProcessEnvironment(RunProcessEnvironment);
    RunProcess->start();
    RunProcess->waitForFinished(-1);

    std::cout << RunProcess->readAllStandardError().toStdString() << std::endl;
    std::cout << RunProcess->readAllStandardOutput().toStdString() << std::endl;

    if(RunProcess->exitCode() == 0)
    {
        delete RunProcess;
        std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] Prefix initialisation successful!" << std::endl;
        UnionFSString.prepend(this->Paths["DefPrefixPath"] + "=RO");
    }
    else
    {
        delete RunProcess;
        std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[ERR] Prefix initialisation failed!" << std::endl;
        return false;
    }
    ///////////////////////////////////////////////////////////////////////////////////////////////

    std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] Executing registry subcomponents" << std::endl;

    QProcessEnvironment RegAddProcessEnvironment = QProcessEnvironment::systemEnvironment();
    RegAddProcessEnvironment.insert("WINEPREFIX", this->Paths["DefPrefixPath"]);
    RegAddProcessEnvironment.insert("GAMEID", "0");
    RegAddProcessEnvironment.remove("LD_LIBRARY_PATH");


    QProcess * RegAddProcess = new QProcess;
    RegAddProcess->setProcessEnvironment(RegAddProcessEnvironment);
    RegAddProcess->setProgram("umu-run");

    for (int i = 0; i < Runner::SubComponentsArray.size(); i++)
    {
        nlohmann::ordered_json SubComponentJSON = Runner::SubComponentsArray[i];
        if (SubComponentJSON["TYPE"] == "RegEdit")
        {
            QString REGPATH = QString::fromStdString(SubComponentJSON["REGPATH"]);

            if (SubComponentJSON.contains("KEYVALUES"))
            {
                for (auto Item : SubComponentJSON["KEYVALUES"].items())
                {
                    QStringList CommandArgs;
                    CommandArgs.append("reg");
                    CommandArgs.append("add");      CommandArgs.append(REGPATH);
                    CommandArgs.append("/f");
                    CommandArgs.append("/v");       CommandArgs.append(QString::fromStdString(Item.key()));
                    CommandArgs.append("/t");       CommandArgs.append(QString::fromStdString(SubComponentJSON["KEYVALUES"][Item.key()]["TYPE"]));

                    if (SubComponentJSON["KEYVALUES"][Item.key()].contains("VALUE"))
                    {
                        CommandArgs.append("/d");       CommandArgs.append(QString::fromStdString(SubComponentJSON["KEYVALUES"][Item.key()]["VALUE"]));
                    }

                    if(SubComponentJSON["ARCHITECTURE"] == "32")
                    {
                        CommandArgs.append("/reg:32");
                    }

                    //std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] EXECUTING REGISTRY STRING: " << CommandArgs;
                    //this->Run("reg", CommandArgs);
                    RegAddProcess->setArguments(CommandArgs);
                    RegAddProcess->start();
                    RegAddProcess->waitForFinished(-1);

                    std::cout << RegAddProcess->readAllStandardError().toStdString() << std::endl;
                    std::cout << RegAddProcess->readAllStandardOutput().toStdString() << std::endl;
                }
            }
            else
            {
                QStringList CommandArgs;
                CommandArgs.append("reg");
                CommandArgs.append("add");
                CommandArgs.append(REGPATH);
                CommandArgs.append("/f");

                if(SubComponentJSON["ARCHITECTURE"] == "32")
                {
                    CommandArgs.append("/reg:32");
                }

                //std::cout << QTime::currentTime().toString().toStdString() << "REGOps:" << "[OUT] REG STRING: " << CommandArgs;
                //this->Run("reg", CommandArgs);
                RegAddProcess->setArguments(CommandArgs);
                RegAddProcess->start();
                RegAddProcess->waitForFinished(-1);

                std::cout << RegAddProcess->readAllStandardError().toStdString() << std::endl;
                std::cout << RegAddProcess->readAllStandardOutput().toStdString() << std::endl;
            }
        }
    }

    ///////////////////////////////////////////////////////////////////////////////////////////////
    //QMessageBox::warning(nullptr, "Building Runtime", "Processing filesystem Subcomponents.");
    //Mounting filesystem components in TEMP and adding to UnionFSString
    std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] Processing filesystem Subcomponents." << std::endl;
    for (int i = 0; i < Runner::SubComponentsArray.size(); i++)
    {
        nlohmann::ordered_json SubComponentJSON = Runner::SubComponentsArray[i];
        if (SubComponentJSON["TYPE"] == "ZipFileLayer")
        {
            QDir SubComponentDir = this->Paths["TempPath"];
            SubComponentDir.mkdir("[" + QString::number(i) + "]");
            SubComponentDir.cd("[" + QString::number(i) + "]");
            UnionFSString.prepend(SubComponentDir.path() + "=RO:");

            QDir MountDir = SubComponentDir;
            MountDir.mkpath(MountDir.path() + QDir::separator() + "drive_c" + QDir::separator() + this->PackageUID);
            MountDir.cd(MountDir.path() + QDir::separator() + "drive_c" + QDir::separator() + this->PackageUID);

            if (SubComponentJSON.contains("TARGET") || (!(SubComponentJSON["TARGET"].empty())))
            {
                MountDir.mkpath(QDir::cleanPath(MountDir.path() + QDir::separator() + QString::fromStdString(SubComponentJSON["TARGET"])));
                MountDir.cd(QDir::cleanPath(MountDir.path() + QDir::separator() + QString::fromStdString(SubComponentJSON["TARGET"])));
            }

            QString ZipFilePath = QDir::cleanPath(this->Paths["PackageFilesPath"] + QDir::separator() + QString::fromStdString(SubComponentJSON["PATH"]));

            std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] Mounting ZipFileLayer" << ZipFilePath.toStdString() << "at" << MountDir.path().toStdString() << std::endl;

            QProcess * MountZip = new QProcess;
            MountZip->setProgram("fuse-zip");
            MountZip->setArguments({"-r", ZipFilePath, MountDir.path()});
            MountZip->start();
            MountZip->waitForFinished(-1);

            std::cout << MountZip->readAllStandardError().toStdString() << std::endl;
            std::cout << MountZip->readAllStandardOutput().toStdString() << std::endl;

            if(MountZip->exitCode() == 0)
            {
                std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] Successfully mounted zip file layer" << ZipFilePath.toStdString() << std::endl;

                delete MountZip;
            }
            else
            {
                std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[ERR] Failed to mount zip file layer" << ZipFilePath.toStdString() << std::endl;
                delete MountZip;
                return false;
            }
            std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] UnionFSString:" << UnionFSString.toStdString() << std::endl;
        }
        else if (SubComponentJSON["TYPE"] == "DirLayer")
        {
            QDir SubComponentDir = this->Paths["TempPath"];
            SubComponentDir.mkdir("[" + QString::number(i) + "]");
            SubComponentDir.cd("[" + QString::number(i) + "]");
            UnionFSString.prepend(SubComponentDir.path() + "=RO:");

            QDir MountDir = SubComponentDir;
            MountDir.mkpath(MountDir.path() + QDir::separator() + "drive_c" + QDir::separator() + this->PackageUID);
            MountDir.cd(MountDir.path() + QDir::separator() + "drive_c" + QDir::separator() + this->PackageUID);

            if (SubComponentJSON.contains("TARGET") || (!(SubComponentJSON["TARGET"].empty())))
            {
                MountDir.mkpath(QDir::cleanPath(MountDir.path() + QDir::separator() + QString::fromStdString(SubComponentJSON["TARGET"])));
                MountDir.cd(QDir::cleanPath(MountDir.path() + QDir::separator() + QString::fromStdString(SubComponentJSON["TARGET"])));
            }

            QString DirPath = QDir::cleanPath(this->Paths["PackageFilesPath"] + QDir::separator() + QString::fromStdString(SubComponentJSON["PATH"]));

            std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] Mounting DirLayer" << DirPath.toStdString() << "at" << MountDir.path().toStdString() << std::endl;

            QProcess * BindDir = new QProcess;
            BindDir->setProgram("bindfs");
            BindDir->setArguments({"-r", DirPath, MountDir.path()});
            BindDir->start();
            BindDir->waitForFinished(-1);

            std::cout << BindDir->readAllStandardError().toStdString() << std::endl;
            std::cout << BindDir->readAllStandardOutput().toStdString() << std::endl;

            if(BindDir->exitCode() == 0)
            {
                std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] Successfully mounted dir layer" << DirPath.toStdString() << std::endl;

                delete BindDir;
            }
            else
            {
                std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[ERR] Failed to mount dir layer" << DirPath.toStdString() << std::endl;
                delete BindDir;
                return false;
            }
            std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] UnionFSString:" << UnionFSString.toStdString() << std::endl;
        }
    }
    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    //QMessageBox::warning(nullptr, "Building Runtime", "Building UnionFS.");
    //FINALIZING UNIONFS STRING

    if (FinalUserDataPath == "READONLY")
    {
        std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] UnionFSString:" << "NO USERDATA - RUNTIME IS READONLY!" << std::endl;
    }
    else
    {
        UnionFSString.prepend(FinalUserDataPath + "=RW:");
        std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] UnionFSString:" << UnionFSString.toStdString() << std::endl;
        QDir(FinalUserDataPath).mkpath(FinalUserDataPath);
    }

    QDir(FinalRuntimePath).mkpath(FinalRuntimePath);

    //Mounting UnionFS
    std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] Building UnionFS." << std::endl;
    std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] FINAL UnionFSString:" << UnionFSString.toStdString() << std::endl;

    QProcess * BuildUnionFS = new QProcess;
    BuildUnionFS->setProgram("unionfs");
    BuildUnionFS->setArguments({"-o", "cow", "-o", "uid=1000", UnionFSString, FinalRuntimePath});
    BuildUnionFS->start();
    BuildUnionFS->waitForFinished(-1);

    std::cout << BuildUnionFS->readAllStandardError().toStdString() << std::endl;
    std::cout << BuildUnionFS->readAllStandardOutput().toStdString() << std::endl;

    if(BuildUnionFS->exitCode() == 0)
    {
        std::cout << QTime::currentTime().toString().toStdString() << "FSOperations:" << "[OUT] Successfully mounted UnionFS!" << std::endl;
        delete BuildUnionFS;
    }
    else
    {
        std::cout << QTime::currentTime().toString().toStdString() << "FSOperations:" << "[ERR] Failed to mount UnionFS!" << std::endl;
        delete BuildUnionFS;
        return false;
    }
    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    //QMessageBox::warning(nullptr, "Building Runtime", "Processing other Subcomponents.");
    //PROCESSING OTHER SUBCOMPONENTS
    std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] Processing other subcomponents." << std::endl;
    for (int i = 0; i < Runner::SubComponentsArray.size(); i++)
    {
        nlohmann::ordered_json SubComponentJSON = Runner::SubComponentsArray[i];

        if (SubComponentJSON["TYPE"] == "DllOverride")
        {
            if (!Runner::DllOverrides.isEmpty())
            {
                Runner::DllOverrides.append(";" + QString::fromStdString(SubComponentJSON["DLLOVERRIDE"]));
            }
            else
            {
                Runner::DllOverrides.append(QString::fromStdString(SubComponentJSON["DLLOVERRIDE"]));
            }
        }
        else if (SubComponentJSON["TYPE"] == "FileEdit")
        {
            if (SubComponentJSON["MODE"] == "ConfigWrite")
            {
                FSOps::ConfigWrite(QString::fromStdString(SubComponentJSON["KEY"]), QString::fromStdString(SubComponentJSON["VALUE"]), QString::fromStdString(SubComponentJSON["FILE"]));
            }
        }
    }

    //QMessageBox::warning(nullptr, "Building Runtime", "Checking case conflicts.");
    FSOps::CheckCaseConflicts(FinalRuntimePath);

    return true;
}

bool Runner::Cleanup(QString OverrideRuntimePath)
{
    QString FinalRuntimePath = this->Paths["RuntimePath"];
    if (!OverrideRuntimePath.isNull())
    {
        std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] Passed RuntimePath" << OverrideRuntimePath.toStdString() << std::endl;
        FinalRuntimePath = OverrideRuntimePath;
    }

    //Destroy UnionFS
    std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] Unmounting UnionFS" << FinalRuntimePath.toStdString() << std::endl;
    QProcess * UnmountUnionFS = new QProcess;
    UnmountUnionFS->setProgram("fusermount");
    UnmountUnionFS->setArguments({"-uz", FinalRuntimePath});
    UnmountUnionFS->start();
    UnmountUnionFS->waitForFinished(-1);

    std::cout << UnmountUnionFS->readAllStandardError().toStdString() << std::endl;
    std::cout << UnmountUnionFS->readAllStandardOutput().toStdString() << std::endl;

    if(UnmountUnionFS->exitCode() == 0)
    {
        std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] Successfully UNmounted UnionFS!" << std::endl;
        delete UnmountUnionFS;
    }
    else
    {
        std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[ERR] Failed to UNmount UnionFS!" << std::endl;
        delete UnmountUnionFS;
        return false;
    }

    for (int i = 0; i < Runner::SubComponentsArray.size(); i++)
    {
        nlohmann::ordered_json SubComponentJSON = Runner::SubComponentsArray[i];
        if (SubComponentJSON["TYPE"] == "ZipFileLayer")
        {
            QDir SubComponentDir = this->Paths["TempPath"];
            SubComponentDir.mkdir("[" + QString::number(i) + "]");
            SubComponentDir.cd("[" + QString::number(i) + "]");

            QDir MountDir = SubComponentDir;
            MountDir.mkpath(MountDir.path() + QDir::separator() + "drive_c" + QDir::separator() + this->PackageUID);
            MountDir.cd(MountDir.path() + QDir::separator() + "drive_c" + QDir::separator() + this->PackageUID);

            if (SubComponentJSON.contains("TARGET") || (!(SubComponentJSON["TARGET"].empty())))
            {
                MountDir.mkpath(QDir::cleanPath(MountDir.path() + QDir::separator() + QString::fromStdString(SubComponentJSON["TARGET"])));
                MountDir.cd(QDir::cleanPath(MountDir.path() + QDir::separator() + QString::fromStdString(SubComponentJSON["TARGET"])));
            }

            QProcess * UnmountDir = new QProcess;
            UnmountDir->setProgram("fusermount");
            UnmountDir->setArguments({"-uz", MountDir.path()});
            UnmountDir->start();
            UnmountDir->waitForFinished(-1);

            std::cout << UnmountDir->readAllStandardError().toStdString() << std::endl;
            std::cout << UnmountDir->readAllStandardOutput().toStdString() << std::endl;

            if(UnmountDir->exitCode() == 0)
            {
                std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] Successfully unmounted dir" << MountDir.path().toStdString() << std::endl;
                delete UnmountDir;
            }
            else
            {
                std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[ERR] Failed to unmount dir" << MountDir.path().toStdString() << std::endl;
                delete UnmountDir;
                return false;
            }
        }
        else if (SubComponentJSON["TYPE"] == "DirLayer")
        {
            QDir SubComponentDir = this->Paths["TempPath"];
            SubComponentDir.mkdir("[" + QString::number(i) + "]");
            SubComponentDir.cd("[" + QString::number(i) + "]");

            QDir MountDir = SubComponentDir;
            MountDir.mkpath(MountDir.path() + QDir::separator() + "drive_c" + QDir::separator() + this->PackageUID);
            MountDir.cd(MountDir.path() + QDir::separator() + "drive_c" + QDir::separator() + this->PackageUID);

            if (SubComponentJSON.contains("TARGET") || (!(SubComponentJSON["TARGET"].empty())))
            {
                MountDir.mkpath(QDir::cleanPath(MountDir.path() + QDir::separator() + QString::fromStdString(SubComponentJSON["TARGET"])));
                MountDir.cd(QDir::cleanPath(MountDir.path() + QDir::separator() + QString::fromStdString(SubComponentJSON["TARGET"])));
            }

            QString DirPath = QDir::cleanPath(this->Paths["PackageFilesPath"] + QDir::separator() + QString::fromStdString(SubComponentJSON["PATH"]));

            std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] Unmounting DirLayer" << DirPath.toStdString() << "at" << MountDir.path().toStdString() << std::endl;

            QProcess * UnmountDir = new QProcess;
            UnmountDir->setProgram("fusermount");
            UnmountDir->setArguments({"-uz", MountDir.path()});
            UnmountDir->start();
            UnmountDir->waitForFinished(-1);

            std::cout << UnmountDir->readAllStandardError().toStdString() << std::endl;
            std::cout << UnmountDir->readAllStandardOutput().toStdString() << std::endl;

            if(UnmountDir->exitCode() == 0)
            {
                std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] Successfully unmounted dir" << MountDir.path().toStdString() << std::endl;
                delete UnmountDir;
            }
            else
            {
                std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[ERR] Failed to unmount dir" << MountDir.path().toStdString() << std::endl;
                delete UnmountDir;
                return false;
            }
        }
    }

    //QMessageBox::warning(nullptr, "Cleanup", "Removing mount points.");
    if (!QDir(Runner::Paths["TempPath"]).removeRecursively())
    {
        //QMessageBox::warning(nullptr, "CLEANUP INCOMPLETE.", "Could not complete cleanup.");
        return false;
    }

    //QMessageBox::warning(nullptr, "Cleanup", "Removing runtime.");
    if (!QDir(FinalRuntimePath).removeRecursively())
    {
        //QMessageBox::warning(nullptr, "CLEANUP INCOMPLETE.", "Could not complete cleanup.");
        return false;
    }

    return true;
}

bool Runner::Run(QString OverrideExePath, QStringList OverrideExeArgs, QString OverrideRuntimePath)
{
    QString FinalExePath = this->WindowsPaths["WindowsExePath"];
    QStringList FinalExeArgs = this->ExeArgs;
    QString FinalRuntimePath = this->Paths["RuntimePath"];

    if (!OverrideExePath.isEmpty())
    {
        std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] Override ExePath:" << OverrideExePath.toStdString() << std::endl;
        FinalExePath = OverrideExePath;
    }

    if (!OverrideExeArgs.isEmpty())
    {
        //std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] Override ExeArgs:" << OverrideExeArgs.toStdString();
        FinalExeArgs = OverrideExeArgs;
    }

    if (!OverrideRuntimePath.isEmpty())
    {
        std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] Override RuntimePath:" << OverrideRuntimePath.toStdString() << std::endl;
        FinalRuntimePath = OverrideRuntimePath;
    }

    if(FinalExePath.isEmpty())
    {
        std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[ERR] ExePath is empty! Nothing to execute! Aborting" << std::endl;
        return false;
    }

    //std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] Executing with umu-launcher: " << FinalExePath.toStdString() << FinalExeArgs.toStdString();
    std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] ProtonPath:" << this->Paths["ProtonPath"].toStdString() << std::endl;
    std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] WinePrefix:" << FinalRuntimePath.toStdString() << std::endl;
    std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] ExePath:" << FinalExePath.toStdString() << std::endl;
    //std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] ExeArgs:" << FinalExeArgs.toStdString();
    std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] WorkDirPath:" << this->Paths["WorkDirPath"].toStdString() << std::endl;
    std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] GAMEID:" << this->UMUID.toStdString() << std::endl;
    std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] DllOverrides:" << this->DllOverrides.toStdString() << std::endl;

    QProcessEnvironment RunProcessEnvironment = QProcessEnvironment::systemEnvironment();
    QProcess * RunProcess = new QProcess;
    RunProcess->setProgram("umu-run");
    RunProcess->setWorkingDirectory(this->Paths["WorkDirPath"]);
    //RunProcessEnvironment.insert("PROTONPATH", this->Paths["ProtonPath"]);
    RunProcessEnvironment.insert("WINEPREFIX", FinalRuntimePath);
    RunProcessEnvironment.remove("LD_LIBRARY_PATH");
    FinalExeArgs.prepend(FinalExePath);

    if (!FinalExeArgs.isEmpty())
    {
        RunProcess->setArguments(FinalExeArgs);
    }

    //if (!this->WorkDir.isEmpty())
    //{
    //    RunProcess->setWorkingDirectory(this->Paths["WorkDirPath"]);
    //    //RunProcess->setWorkingDirectory(FSOps::SubPath(FSOps::SubPath(FSOps::SubPath(FinalRuntimePath, "drive_c"), QString::fromStdString((*MANIFESTJSON)["PACKAGEUID"])), this->WorkDir));
    //    std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] WorkDirPath:" << RunProcess->workingDirectory();
    //}

    RunProcessEnvironment.insert("GAMEID", this->UMUID);

    if (!DllOverrides.isEmpty())
    {
        RunProcessEnvironment.insert("WINEDLLOVERRIDES", DllOverrides);
    }

    RunProcessEnvironment.insert("PROTON_VERB", "waitforexitandrun");

    RunProcess->setProcessEnvironment(RunProcessEnvironment);
    RunProcess->start();
    RunProcess->waitForFinished(-1);
    std::cout << RunProcess->readAllStandardError().toStdString() << std::endl;
    std::cout << RunProcess->readAllStandardOutput().toStdString() << std::endl;

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

QStringList Runner::StringListReplaceVariables(QStringList OriginalStringList, QMap<QString, QString> VariableValues)
{
    // Iterate using a const iterator (Qt 6.2+ compatible)
    for (auto it = VariableValues.constBegin(); it != VariableValues.constEnd(); ++it)
    {
        const QString &Variable = it.key();
        const QString &Value = it.value();

        OriginalStringList.replaceInStrings("%" + Variable + "%", Value);
    }

    return OriginalStringList;
}

/*
bool Runner::RegAdd(nlohmann::ordered_json SubComponentJSON)
{
    QString REGPATH = QString::fromStdString(SubComponentJSON["REGPATH"]);

    if (SubComponentJSON.contains("KEYVALUES"))
    {
        for (auto Item : SubComponentJSON["KEYVALUES"].items())
        {
            QStringList CommandArgs;
            CommandArgs.append("add");      CommandArgs.append(REGPATH);
            CommandArgs.append("/f");
            CommandArgs.append("/v");       CommandArgs.append(QString::fromStdString(Item.key()));
            CommandArgs.append("/t");       CommandArgs.append(QString::fromStdString(SubComponentJSON["KEYVALUES"][Item.key()]["TYPE"]));

            if (SubComponentJSON["KEYVALUES"][Item.key()].contains("VALUE"))
            {
                CommandArgs.append("/d");       CommandArgs.append(QString::fromStdString(SubComponentJSON["KEYVALUES"][Item.key()]["VALUE"]));
            }

            if(SubComponentJSON["ARCHITECTURE"] == "32")
            {
                CommandArgs.append("/reg:32");
            }

            std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] EXECUTING REGISTRY STRING: " << CommandArgs;
            this->Run("reg", CommandArgs);
        }
    }
    else
    {
        QStringList CommandArgs;
        CommandArgs.append("add");      CommandArgs.append(REGPATH);
        CommandArgs.append("/f");

        if(SubComponentJSON["ARCHITECTURE"] == "32")
        {
            CommandArgs.append("/reg:32");
        }

        std::cout << QTime::currentTime().toString().toStdString() << "REGOps:" << "[OUT] REG STRING: " << CommandArgs;
        this->Run("reg", CommandArgs);
    }
    return true;
}
*/
