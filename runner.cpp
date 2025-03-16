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
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Initializing Runner";

    this->PackageName = QString::fromStdString((*MANIFESTJSON)["PACKAGENAME"]);

    Runner::Paths["ProtonPath"] = "/usr/share/steam/compatibilitytools.d/proton-ge-custom/";
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] ProtonPath" << Runner::Paths["ProtonPath"];

    Runner::Paths["PackagePath"] = PackageDir->path();
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] PackagePath" << Runner::Paths["PackagePath"];

    Runner::Paths["RuntimePath"] = FSOps::SubPath(Paths["PackagePath"], "RUNTIME");
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] RuntimePath" << Runner::Paths["RuntimePath"];

    //Runner::Paths["ProgramPath"] = FSOps::SubPath(FSOps::SubPath(Paths["RuntimePath"], "drive_c"), QString::fromStdString((*MANIFESTJSON)["PACKAGENAME"]));
    //qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] ProgramPath" << Runner::Paths["ProgramPath"];

    Runner::Paths["MetaDataPath"] = FSOps::SubPath(Paths["PackagePath"], "METADATA");
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] MetaDataPath" << Runner::Paths["MetaDataPath"];

    Runner::Paths["PackageFilesPath"] = FSOps::SubPath(Paths["PackagePath"], "PACKAGEFILES");
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] PackageFilesPath" << Runner::Paths["PackageFilesPath"];

    Runner::Paths["UserDataPath"] = FSOps::SubPath(Paths["PackagePath"], "USERDATA");
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] UserDataPath" << Runner::Paths["UserDataPath"];

    Runner::Paths["TempPath"] = FSOps::SubPath(Paths["PackagePath"], "TEMP");
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] TempPath" << Runner::Paths["TempPath"];

    Runner::Paths["DefPrefixPath"] = FSOps::SubPath(Paths["TempPath"], "DEFPREFIX");
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] DefPrefixPath" << Runner::Paths["DefPrefixPath"];

    if (Runner::subgame != 0)
    {
        this->GameName = QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][subgame]["TITLE"]);
        qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] GameName: " << this->GameName;

        this->UMUID = QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][subgame]["UMUID"]);
        qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] UMUID: " << this->UMUID;

        this->ExePath = QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][subgame]["EXEPATH"]);
        qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] EXEPATH: " << this->ExePath;

        this->WorkDir = QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][subgame]["WORKDIR"]);
        qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] WORKDIR: " << this->WorkDir;

        //MUST MAKE THIS RESPECT QUOTES, ACCOUNT FOR EMPTY.
        if (!((*MANIFESTJSON)["SUBGAMES"][subgame]["EXEARGS"].empty() || (*MANIFESTJSON)["SUBGAMES"][subgame]["EXEARGS"] == "" || (*MANIFESTJSON)["SUBGAMES"][subgame]["EXEARGS"].is_null()))
        {
            Runner::ExeArgs = QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][subgame]["EXEARGS"]).split(" ");
            for (int i = 0; i < Runner::ExeArgs.count(); i++)
            {
                qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << QString("[OUT] ExeArg %1:").arg(i) << Runner::ExeArgs.at(i);
            }
        }

        if(component == 0)
        {
            component = QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][subgame]["COMPONENT"]).toInt();
            qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Component" << component;
        }
    }

    //CREATE RECIPE BY RESOLVING COMPONENT DEPENDENCY
    while (component != 0)
    {
        Runner::Recipe.prepend(component);
        qDebug() << component;

        if ((*MANIFESTJSON)["COMPONENTS"][component - 1]["PARENTCOMPONENT"].is_null())
        {
            qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Parent component is null, ending recipe.";
            break;
        }

        if ((*MANIFESTJSON)["COMPONENTS"][component - 1]["PARENTCOMPONENT"] == "")
        {
            qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Parent component is empty, ending recipe.";
            break;
        }

        if (QString::fromStdString((*MANIFESTJSON)["COMPONENTS"][component - 1]["PARENTCOMPONENT"]).toInt() == 0)
        {
            qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Parent component is 0, ending recipe.";
            break;
        }

        component = QString::fromStdString((*MANIFESTJSON)["COMPONENTS"][component - 1]["PARENTCOMPONENT"]).toInt();
    }
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Recipe:" << this->Recipe;

    Runner::WindowsPaths["WindowsProgramPath"] = QDir::cleanPath("C:/" + PackageDir->path()).replace("/", "\\");
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] WindowsProgramPath" << Runner::WindowsPaths["WindowsProgramPath"];

    Runner::WindowsPaths["WindowsProgramPathDoubleBackSlash"]   = QDir::cleanPath("C:/" + PackageDir->path()).replace("/", "\\\\");
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] WindowsProgramPathDoubleBackSlash" << Runner::WindowsPaths["WindowsProgramPathDoubleBackSlash"];

    Runner::SystemVariables["ScreenWidth"] = QString::number(QGuiApplication::primaryScreen()->geometry().width());
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] ScreenWidth" << Runner::SystemVariables["ScreenWidth"];

    Runner::SystemVariables["ScreenHeight"] = QString::number(QGuiApplication::primaryScreen()->geometry().height());
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] ScreenHeight" << Runner::SystemVariables["ScreenHeight"];

    //BUILD AN ARRAY CONTAINING ALL SUBCOMPONENTS, IN ORDER, FILTERED BY RECIPE.
    for (int i = 0; i < (*MANIFESTJSON)["COMPONENTS"].size(); i++)
    {
        if (Runner::Recipe.contains(i + 1))
        {
            for (int j = 0; j < (*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"].size(); j++)
            {
                Runner::SubComponentsArray.push_back((*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]);
                qDebug().noquote() << QTime::currentTime() << "Runner:" << "[OUT] Added COMPONENT" << i + 1 << "SUBCOMPONENT" << j + 1;
            }
        }
    }
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Completed SubComponentsArray:"<< QString::fromStdString(SubComponentsArray.dump(4));

    Runner::SubComponentsArray = JSONOps::ReplaceVariables(Runner::SubComponentsArray, Runner::WindowsPaths);
    Runner::SubComponentsArray = JSONOps::ReplaceVariables(Runner::SubComponentsArray, Runner::Paths);
    Runner::SubComponentsArray = JSONOps::ReplaceVariables(Runner::SubComponentsArray, Runner::SystemVariables);
    Runner::ExeArgs = StringListReplaceVariables(Runner::ExeArgs, Runner::WindowsPaths);
    //CAUTION! ESCAPE CHARACHTERS IN WINDOWS PATHS CRASHES JSON! FIXME
    Runner::ExeArgs = StringListReplaceVariables(Runner::ExeArgs, Runner::Paths);
    Runner::ExeArgs = StringListReplaceVariables(Runner::ExeArgs, Runner::SystemVariables);

    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Performed substitution on SubComponentsArray:"<< QString::fromStdString(SubComponentsArray.dump(4));
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Performed substitution on ExeArgs:"<< Runner::ExeArgs;
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Runner initialisation complete!"<< Runner::ExeArgs;
    return true;
}

bool Runner::BuildRuntime(QString OverrideRuntimePath, QString OverrideUserDataPath)
{
    QString FinalRuntimePath = this->Paths["RuntimePath"];
    if (!OverrideRuntimePath.isNull())
    {
        qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Passed RuntimePath" << OverrideRuntimePath;
        FinalRuntimePath = OverrideRuntimePath;
    }

    QString FinalUserDataPath = this->Paths["UserDataPath"];
    if (!OverrideUserDataPath.isNull())
    {
        qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Passed UserDataPath" << OverrideUserDataPath;
        FinalUserDataPath = OverrideUserDataPath;
    }

    //Creating all necessary directories based on the Paths QMap.
    //QMessageBox::warning(nullptr, "Building Runtime", "Creating directories.");
    for (auto [Name, Path] : this->Paths.asKeyValueRange())
    {
        QDir Dir(Path);
        if (Dir.mkpath(Path))
        {
            qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Created direcotry " << Name << "PATH: " << Path;
        }
        else
        {
            qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[ERR] Could not create directory " << Name << "PATH: " << Path;
            return false;
        }
    }
    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    //Start building UnionFSString.
    QString UnionFSString;

    //Initialising UMU prefix.
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Initialising prefix" << this->Paths["DefPrefixPath"];

    QProcessEnvironment RunProcessEnvironment = QProcessEnvironment::systemEnvironment();
    QProcess * RunProcess = new QProcess;
    RunProcess->setProgram("umu-run");
    RunProcess->setArguments({"wineboot"});

    RunProcessEnvironment.insert("PROTONPATH", this->Paths["ProtonPath"]);
    RunProcessEnvironment.insert("WINEPREFIX", this->Paths["DefPrefixPath"]);
    RunProcessEnvironment.insert("GAMEID", 0);
    RunProcessEnvironment.insert("PROTON_VERB", "waitforexitandrun");

    RunProcess->setProcessEnvironment(RunProcessEnvironment);
    RunProcess->start();
    RunProcess->waitForFinished(-1);

    qDebug().noquote() << RunProcess->readAllStandardError();
    qDebug().noquote() << RunProcess->readAllStandardOutput();

    if(RunProcess->exitCode() == 0)
    {
        delete RunProcess;
        qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Prefix initialisation successful!";
        UnionFSString.prepend(this->Paths["DefPrefixPath"] + "=RO");
    }
    else
    {
        delete RunProcess;
        qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[ERR] Prefix initialisation failed!";
        return false;
    }
    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    //QMessageBox::warning(nullptr, "Building Runtime", "Processing filesystem Subcomponents.");
    //Mounting filesystem components in TEMP and adding to UnionFSString
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Processing filesystem Subcomponents.";
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
            MountDir.mkpath(MountDir.path() + QDir::separator() + "drive_c" + QDir::separator() + this->PackageName);
            MountDir.cd(MountDir.path() + QDir::separator() + "drive_c" + QDir::separator() + this->PackageName);

            if (SubComponentJSON.contains("TARGET") || (!(SubComponentJSON["TARGET"].empty())))
            {
                MountDir.mkpath(QDir::cleanPath(MountDir.path() + QDir::separator() + QString::fromStdString(SubComponentJSON["TARGET"])));
                MountDir.cd(QDir::cleanPath(MountDir.path() + QDir::separator() + QString::fromStdString(SubComponentJSON["TARGET"])));
            }

            QString ZipFilePath = QDir::cleanPath(this->Paths["PackageFilesPath"] + QDir::separator() + QString::fromStdString(SubComponentJSON["PATH"]));

            qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Mounting ZipFileLayer" << ZipFilePath << "at" << MountDir.path();

            QProcess * MountZip = new QProcess;
            MountZip->setProgram("fuse-zip");
            MountZip->setArguments({"-r", ZipFilePath, MountDir.path()});
            MountZip->start();
            MountZip->waitForFinished(-1);

            if(MountZip->exitCode() == 0)
            {
                delete MountZip;
            }
            else
            {
                qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[ERR] Failed to mount zip file layer" << ZipFilePath;
                delete MountZip;
                return false;
            }
            qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] UnionFSString:" << UnionFSString;
        }
    }
    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    //QMessageBox::warning(nullptr, "Building Runtime", "Building UnionFS.");
    //FINALIZING UNIONFS STRING

    if (FinalUserDataPath == "READONLY")
    {
        qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] UnionFSString:" << "NO USERDATA - RUNTIME IS READONLY!";
    }
    else
    {
        UnionFSString.prepend(FinalUserDataPath + "=RW:");
        qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] UnionFSString:" << UnionFSString;
    }

    QDir(FinalUserDataPath).mkpath(FinalUserDataPath);
    QDir(FinalRuntimePath).mkpath(FinalRuntimePath);

    //Mounting UnionFS
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Building UnionFS.";
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] FINAL UnionFSString:" << UnionFSString;

    QProcess * BuildUnionFS = new QProcess;
    BuildUnionFS->setProgram("unionfs");
    BuildUnionFS->setArguments({"-o", "cow", "-o", "uid=1000", UnionFSString, FinalRuntimePath});
    BuildUnionFS->start();
    BuildUnionFS->waitForFinished(-1);
    qDebug().noquote() << BuildUnionFS->readAllStandardError();

    if(BuildUnionFS->exitCode() == 0)
    {
        qDebug().noquote() << QTime::currentTime().toString() << "FSOperations:" << "[OUT] Successfully mounted UnionFS!";
        delete BuildUnionFS;
    }
    else
    {
        qDebug().noquote() << QTime::currentTime().toString() << "FSOperations:" << "[ERR] Failed to mount UnionFS!";
        delete BuildUnionFS;
        return false;
    }
    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    //QMessageBox::warning(nullptr, "Building Runtime", "Processing other Subcomponents.");
    //PROCESSING OTHER SUBCOMPONENTS
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Processing other subcomponents.";
    for (int i = 0; i < Runner::SubComponentsArray.size(); i++)
    {
        nlohmann::ordered_json SubComponentJSON = Runner::SubComponentsArray[i];

        if (SubComponentJSON["TYPE"] == "RegEdit")
        {
            if(!RegAdd(SubComponentJSON))
            {
                return false;
            }
        }
        else if (SubComponentJSON["TYPE"] == "DllOverride")
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
        qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Passed RuntimePath" << OverrideRuntimePath;
        FinalRuntimePath = OverrideRuntimePath;
    }

    //Destroy UnionFS
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Unmounting UnionFS" << FinalRuntimePath;
    QProcess * UnmountUnionFS = new QProcess;
    UnmountUnionFS->setProgram("fusermount");
    UnmountUnionFS->setArguments({"-u", FinalRuntimePath});
    UnmountUnionFS->start();
    UnmountUnionFS->waitForFinished(-1);
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Exit status: " << UnmountUnionFS->exitCode();
    qDebug().noquote() << UnmountUnionFS->readAllStandardError();
    qDebug().noquote() << UnmountUnionFS->readAllStandardOutput();
    delete UnmountUnionFS;

    for (int i = 0; i < Runner::SubComponentsArray.size(); i++)
    {
        nlohmann::ordered_json SubComponentJSON = Runner::SubComponentsArray[i];
        if (SubComponentJSON["TYPE"] == "ZipFileLayer")
        {
            QDir SubComponentDir = this->Paths["TempPath"];
            SubComponentDir.mkdir("[" + QString::number(i) + "]");
            SubComponentDir.cd("[" + QString::number(i) + "]");

            QDir MountDir = SubComponentDir;
            MountDir.mkpath(MountDir.path() + QDir::separator() + "drive_c" + QDir::separator() + this->PackageName);
            MountDir.cd(MountDir.path() + QDir::separator() + "drive_c" + QDir::separator() + this->PackageName);

            if (SubComponentJSON.contains("TARGET") || (!(SubComponentJSON["TARGET"].empty())))
            {
                MountDir.mkpath(QDir::cleanPath(MountDir.path() + QDir::separator() + QString::fromStdString(SubComponentJSON["TARGET"])));
                MountDir.cd(QDir::cleanPath(MountDir.path() + QDir::separator() + QString::fromStdString(SubComponentJSON["TARGET"])));
            }

            QProcess * UnmountDir = new QProcess;
            UnmountDir->setProgram("fusermount");
            UnmountDir->setArguments({"-u", MountDir.path()});
            UnmountDir->start();
            UnmountDir->waitForFinished(-1);
            qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Exit status: " << UnmountDir->exitCode();
            qDebug().noquote() << UnmountDir->readAllStandardError();
            qDebug().noquote() << UnmountDir->readAllStandardOutput();

            if(UnmountDir->exitCode() == 0)
            {
                delete UnmountDir;
            }
            else
            {
                qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[ERR] Failed to unmount dir" << MountDir.path();
                delete UnmountDir;
                return false;
            }
        }
    }

    //QMessageBox::warning(nullptr, "Cleanup", "Removing mount points.");
    if (!QDir(Runner::Paths["TempPath"]).removeRecursively())
    {
        QMessageBox::warning(nullptr, "CLEANUP INCOMPLETE.", "Could not complete cleanup.");
        return false;
    }

    //QMessageBox::warning(nullptr, "Cleanup", "Removing runtime.");
    if (!QDir(FinalRuntimePath).removeRecursively())
    {
        QMessageBox::warning(nullptr, "CLEANUP INCOMPLETE.", "Could not complete cleanup.");
        return false;
    }

    return true;
}

bool Runner::Run(QString OverrideExePath, QStringList OverrideExeArgs, QString OverrideRuntimePath)
{
    QString FinalExePath = this->ExePath;
    QStringList FinalExeArgs = this->ExeArgs;
    QString FinalRuntimePath = this->Paths["RuntimePath"];

    if (!OverrideExePath.isNull())
    {
        FinalExePath = OverrideExePath;
    }

    if (!OverrideExeArgs.isEmpty())
    {
        FinalExeArgs = OverrideExeArgs;
    }

    if (!OverrideRuntimePath.isEmpty())
    {
        FinalRuntimePath = OverrideRuntimePath;
    }

    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Executing with umu-launcher: " << FinalExePath << FinalExeArgs;
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] ProtonPath:" << Runner::Paths["ProtonPath"];
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] WinePrefix:" << FinalRuntimePath;
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] ExePath:" << FinalExePath;
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] ExeArgs:" << FinalExeArgs;
    //qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] WorkDirPath:" << Runner::Paths["WorkDirPath"];
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] GAMEID:" << Runner::UMUID;
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] DllOverrides:" << Runner::DllOverrides;

    QProcessEnvironment RunProcessEnvironment = QProcessEnvironment::systemEnvironment();
    QProcess * RunProcess = new QProcess;
    RunProcess->setProgram("umu-run");

    RunProcessEnvironment.insert("PROTONPATH", this->Paths["ProtonPath"]);
    RunProcessEnvironment.insert("WINEPREFIX", FinalRuntimePath);
    FinalExeArgs.prepend(FinalExePath);

    if (!FinalExeArgs.isEmpty())
    {
        RunProcess->setArguments(FinalExeArgs);
    }

    if (!this->WorkDir.isEmpty())
    {
        //RunProcess->setWorkingDirectory(this->Paths["WorkDirPath"]);
        RunProcess->setWorkingDirectory(FSOps::SubPath(FSOps::SubPath(FSOps::SubPath(FinalRuntimePath, "drive_c"), QString::fromStdString((*MANIFESTJSON)["PACKAGENAME"])), this->WorkDir));
        qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] WorkDirPath:" << RunProcess->workingDirectory();
    }

    RunProcessEnvironment.insert("GAMEID", this->UMUID);

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

QStringList Runner::StringListReplaceVariables(QStringList OriginalStringList, QMap<QString, QString> VariableValues)
{
    for (auto [Variable, Value] : VariableValues.asKeyValueRange())
    {
        OriginalStringList.replaceInStrings(("%" + Variable + "%"), Value);
    }
    return OriginalStringList;
}

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

            qDebug().noquote() << QTime::currentTime().toString() << "REGOps:" << "[OUT] EXECUTING REGISTRY STRING: " << CommandArgs;
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

        qDebug().noquote() << QTime::currentTime().toString() << "REGOps:" << "[OUT] REG STRING: " << CommandArgs;
        this->Run("reg", CommandArgs);
    }
    return true;
}
