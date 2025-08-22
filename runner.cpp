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

    if ((this->subgame == 0) && (this->component == 0))
    {
        qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Neither subgame nor component specified, mounting defprefix." << component;
    }
    else if ((this->subgame != 0) && (this->component == 0))
    {
        if (!(*this->MANIFESTJSON)["SUBGAMES"][this->subgame - 1]["COMPONENT"].is_null())
        {
            component = QString::fromStdString((*this->MANIFESTJSON)["SUBGAMES"][this->subgame - 1]["COMPONENT"]).toInt();
        }
        qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Running subgame" << QString::fromStdString((*this->MANIFESTJSON)["SUBGAMES"][this->subgame - 1]["TITLE"]);
    }
    else if ((this->subgame == 0) && (this->component != 0))
    {
        qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Running component" << QString::fromStdString((*this->MANIFESTJSON)["COMPONENTS"][this->component - 1]["NAME"]);
    }
    else
    {
        qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Running subgame" << subgame << "component" << component;
    }

    this->PackageName = QString::fromStdString((*MANIFESTJSON)["PACKAGENAME"]);

    this->PackageUID = QString::fromStdString((*MANIFESTJSON)["PACKAGEUID"]);

    this->Paths["ProtonPath"] = "/usr/share/steam/compatibilitytools.d/proton-ge-custom/";
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] ProtonPath" << this->Paths["ProtonPath"];

    this->Paths["PackagePath"] = PackageDir->path();
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] PackagePath" << this->Paths["PackagePath"];

    this->Paths["RuntimePath"] = FSOps::SubPath(Paths["PackagePath"], "RUNTIME");
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] RuntimePath" << this->Paths["RuntimePath"];

    this->Paths["ProgramPath"] = FSOps::SubPath(FSOps::SubPath(Paths["RuntimePath"], "drive_c"), QString::fromStdString((*MANIFESTJSON)["PACKAGEUID"]));
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] ProgramPath" << Runner::Paths["ProgramPath"];

    this->Paths["MetaDataPath"] = FSOps::SubPath(Paths["PackagePath"], "METADATA");
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] MetaDataPath" << this->Paths["MetaDataPath"];

    this->Paths["PackageFilesPath"] = FSOps::SubPath(Paths["PackagePath"], "PACKAGEFILES");
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] PackageFilesPath" << this->Paths["PackageFilesPath"];

    this->Paths["UserDataPath"] = FSOps::SubPath(Paths["PackagePath"], "USERDATA");
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] UserDataPath" << this->Paths["UserDataPath"];

    this->Paths["TempPath"] = FSOps::SubPath(Paths["PackagePath"], "TEMP");
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] TempPath" << this->Paths["TempPath"];

    this->Paths["DefPrefixPath"] = FSOps::SubPath(Paths["TempPath"], "DEFPREFIX");
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] DefPrefixPath" << this->Paths["DefPrefixPath"];

    if (this->subgame != 0)
    {
        this->GameName = QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][subgame - 1]["TITLE"]);
        qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] GameName: " << this->GameName;

        this->UMUID = QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][subgame - 1]["UMUID"]);
        qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] UMUID: " << this->UMUID;

        this->ExePath = QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][subgame - 1]["EXEPATH"]);
        qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] EXEPATH: " << this->ExePath;

        if (!((*MANIFESTJSON)["SUBGAMES"][subgame - 1]["WORKDIR"].empty() || (*MANIFESTJSON)["SUBGAMES"][subgame - 1]["WORKDIR"] == "" || (*MANIFESTJSON)["SUBGAMES"][subgame - 1]["WORKDIR"].is_null()))
        {
            this->Paths["WorkDirPath"] = QDir::cleanPath(this->Paths["ProgramPath"] + QDir::separator() + QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][subgame - 1]["WORKDIR"]));
            qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] WORKDIR: " << this->Paths["WorkDirPath"];
        }
        else
        {
            this->Paths["WorkDirPath"] = this->Paths["ProgramPath"];
            qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] WORKDIR: " << this->Paths["WorkDirPath"];
        }

        //MUST MAKE THIS RESPECT QUOTES, ACCOUNT FOR EMPTY.
        if (!((*MANIFESTJSON)["SUBGAMES"][subgame - 1]["EXEARGS"].empty() || (*MANIFESTJSON)["SUBGAMES"][subgame - 1]["EXEARGS"] == "" || (*MANIFESTJSON)["SUBGAMES"][subgame - 1]["EXEARGS"].is_null()))
        {
            this->ExeArgs = QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][subgame - 1]["EXEARGS"]).split(" ");
            for (int i = 0; i < this->ExeArgs.count(); i++)
            {
                qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << QString("[OUT] ExeArg %1:").arg(i) << this->ExeArgs.at(i);
            }
        }
    }

    //CREATE RECIPE BY RESOLVING COMPONENT DEPENDENCY
    while (component != 0)
    {
        this->Recipe.prepend(component);
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

    this->WindowsPaths["WindowsProgramPath"] = QDir::cleanPath("C:/" + this->PackageUID).replace("/", "\\");
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] WindowsProgramPath" << this->WindowsPaths["WindowsProgramPath"];

    this->WindowsPaths["WindowsExePath"] = QDir::cleanPath("C:/" + this->PackageUID + QDir::separator() + this->ExePath).replace("/", "\\");
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] WindowsExePath" << this->WindowsPaths["WindowsExePath"];

    this->WindowsPaths["WindowsProgramPathDoubleBackSlash"]   = QDir::cleanPath("C:/" + this->PackageUID).replace("/", "\\\\");
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] WindowsProgramPathDoubleBackSlash" << this->WindowsPaths["WindowsProgramPathDoubleBackSlash"];

    this->SystemVariables["ScreenWidth"] = QString::number(QGuiApplication::primaryScreen()->geometry().width());
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] ScreenWidth" << this->SystemVariables["ScreenWidth"];

    this->SystemVariables["ScreenHeight"] = QString::number(QGuiApplication::primaryScreen()->geometry().height());
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] ScreenHeight" << this->SystemVariables["ScreenHeight"];

    //BUILD AN ARRAY CONTAINING ALL SUBCOMPONENTS, IN ORDER, FILTERED BY RECIPE.
    for (int i = 0; i < (*MANIFESTJSON)["COMPONENTS"].size(); i++)
    {
        if (this->Recipe.contains(i + 1))
        {
            for (int j = 0; j < (*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"].size(); j++)
            {
                this->SubComponentsArray.push_back((*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]);
                qDebug().noquote() << QTime::currentTime() << "Runner:" << "[OUT] Added COMPONENT" << i + 1 << "SUBCOMPONENT" << j + 1;
            }
        }
    }
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Completed SubComponentsArray:"<< QString::fromStdString(SubComponentsArray.dump(4));

    this->SubComponentsArray = JSONOps::ReplaceVariables(this->SubComponentsArray, this->WindowsPaths);
    this->SubComponentsArray = JSONOps::ReplaceVariables(this->SubComponentsArray, this->Paths);
    this->SubComponentsArray = JSONOps::ReplaceVariables(this->SubComponentsArray, this->SystemVariables);
    this->ExeArgs = StringListReplaceVariables(this->ExeArgs, this->WindowsPaths);
    //CAUTION! ESCAPE CHARACHTERS IN WINDOWS PATHS CRASHES JSON! FIXME
    this->ExeArgs = StringListReplaceVariables(this->ExeArgs, this->Paths);
    this->ExeArgs = StringListReplaceVariables(this->ExeArgs, this->SystemVariables);

    ///////////////////////////////////////////////////////////////////////////////////////////////
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Performed substitution on SubComponentsArray:"<< QString::fromStdString(SubComponentsArray.dump(4));
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Performed substitution on ExeArgs:"<< this->ExeArgs;
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Runner initialisation complete!";
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
    ///////////////////////////////////////////////////////////////////////////////////////////////

    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Executing registry subcomponents";

    QProcessEnvironment RegAddProcessEnvironment = QProcessEnvironment::systemEnvironment();
    RegAddProcessEnvironment.insert("WINEPREFIX", this->Paths["DefPrefixPath"]);


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

                    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] EXECUTING REGISTRY STRING: " << CommandArgs;
                    //this->Run("reg", CommandArgs);
                    RegAddProcess->setArguments(CommandArgs);
                    RegAddProcess->start();
                    RegAddProcess->waitForFinished(-1);

                    qDebug().noquote() << RegAddProcess->readAllStandardError();
                    qDebug().noquote() << RegAddProcess->readAllStandardOutput();
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

                qDebug().noquote() << QTime::currentTime().toString() << "REGOps:" << "[OUT] REG STRING: " << CommandArgs;
                //this->Run("reg", CommandArgs);
                RegAddProcess->setArguments(CommandArgs);
                RegAddProcess->start();
                RegAddProcess->waitForFinished(-1);

                qDebug().noquote() << RegAddProcess->readAllStandardError();
                qDebug().noquote() << RegAddProcess->readAllStandardOutput();
            }
        }
    }

    ///////////////////////////////////////////////////////////////////////////////////////////////
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
            MountDir.mkpath(MountDir.path() + QDir::separator() + "drive_c" + QDir::separator() + this->PackageUID);
            MountDir.cd(MountDir.path() + QDir::separator() + "drive_c" + QDir::separator() + this->PackageUID);

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

            qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Mounting DirLayer" << DirPath << "at" << MountDir.path();

            QProcess * BindDir = new QProcess;
            BindDir->setProgram("bindfs");
            BindDir->setArguments({"-r", DirPath, MountDir.path()});
            BindDir->start();
            BindDir->waitForFinished(-1);

            if(BindDir->exitCode() == 0)
            {
                delete BindDir;
            }
            else
            {
                qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[ERR] Failed to mount dir layer" << DirPath;
                qDebug().noquote() << BindDir->readAllStandardError();
                qDebug().noquote() << BindDir->readAllStandardOutput();
                delete BindDir;
                return false;
            }
            qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] UnionFSString:" << UnionFSString;
        }
    }
    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    //QMessageBox::warning(nullptr, "Building Runtime", "Building UnionFS.");
    //FINALIZING UNIONFS STRING
    //CAUTION - READONLY USERDATA MEANS THAT THE REGISTRY WILL NOT BE ADDED ON TOP!!!!
    //BECAUSE ATM IT IS DONE AFTER THE UNIONFS IS BUILT, ON TOP OF RUNTIME
    //THIS ALSO RAISES THE PROBLEM THAT WHEN USING THE PACKAGE EDITOR, THE REGISTRY CHANGGES IN USERDATA GET OVERWRITTEN
    //A SOLUTION TO THIS COULD BE MOVING REGISTRY PROCESSING EARLIER IN THE CHAIN

    if (FinalUserDataPath == "READONLY")
    {
        qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] UnionFSString:" << "NO USERDATA - RUNTIME IS READONLY!";
    }
    else
    {
        UnionFSString.prepend(FinalUserDataPath + "=RW:");
        qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] UnionFSString:" << UnionFSString;
        QDir(FinalUserDataPath).mkpath(FinalUserDataPath);
    }

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
        qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Passed RuntimePath" << OverrideRuntimePath;
        FinalRuntimePath = OverrideRuntimePath;
    }

    //Destroy UnionFS
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Unmounting UnionFS" << FinalRuntimePath;
    QProcess * UnmountUnionFS = new QProcess;
    UnmountUnionFS->setProgram("fusermount");
    UnmountUnionFS->setArguments({"-uz", FinalRuntimePath});
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

            qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Unmounting DirLayer" << DirPath << "at" << MountDir.path();

            QProcess * UnmountDir = new QProcess;
            UnmountDir->setProgram("fusermount");
            UnmountDir->setArguments({"-uz", MountDir.path()});
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
    QString FinalExePath = this->WindowsPaths["WindowsExePath"];
    QStringList FinalExeArgs = this->ExeArgs;
    QString FinalRuntimePath = this->Paths["RuntimePath"];

    if (!OverrideExePath.isEmpty())
    {
        qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Override ExePath:" << OverrideExePath;
        FinalExePath = OverrideExePath;
    }

    if (!OverrideExeArgs.isEmpty())
    {
        qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Override ExeArgs:" << OverrideExeArgs;
        FinalExeArgs = OverrideExeArgs;
    }

    if (!OverrideRuntimePath.isEmpty())
    {
        qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Override RuntimePath:" << OverrideRuntimePath;
        FinalRuntimePath = OverrideRuntimePath;
    }

    if(FinalExePath.isEmpty())
    {
        qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[ERR] ExePath is empty! Nothing to execute! Aborting";
        return false;
    }

    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] Executing with umu-launcher: " << FinalExePath << FinalExeArgs;
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] ProtonPath:" << this->Paths["ProtonPath"];
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] WinePrefix:" << FinalRuntimePath;
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] ExePath:" << FinalExePath;
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] ExeArgs:" << FinalExeArgs;
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] WorkDirPath:" << this->Paths["WorkDirPath"];
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] GAMEID:" << this->UMUID;
    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] DllOverrides:" << this->DllOverrides;

    QProcessEnvironment RunProcessEnvironment = QProcessEnvironment::systemEnvironment();
    QProcess * RunProcess = new QProcess;
    RunProcess->setProgram("umu-run");
    RunProcess->setWorkingDirectory(this->Paths["WorkDirPath"]);
    RunProcessEnvironment.insert("PROTONPATH", this->Paths["ProtonPath"]);
    RunProcessEnvironment.insert("WINEPREFIX", FinalRuntimePath);
    FinalExeArgs.prepend(FinalExePath);

    if (!FinalExeArgs.isEmpty())
    {
        RunProcess->setArguments(FinalExeArgs);
    }

    //if (!this->WorkDir.isEmpty())
    //{
    //    RunProcess->setWorkingDirectory(this->Paths["WorkDirPath"]);
    //    //RunProcess->setWorkingDirectory(FSOps::SubPath(FSOps::SubPath(FSOps::SubPath(FinalRuntimePath, "drive_c"), QString::fromStdString((*MANIFESTJSON)["PACKAGEUID"])), this->WorkDir));
    //    qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] WorkDirPath:" << RunProcess->workingDirectory();
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

            qDebug().noquote() << QTime::currentTime().toString() << "Runner:" << "[OUT] EXECUTING REGISTRY STRING: " << CommandArgs;
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
*/
