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
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] Initializing Runner";

    Runner::Paths["ProtonPath"] = "/usr/share/steam/compatibilitytools.d/proton-ge-custom/";
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] ProtonPath" << Runner::Paths["ProtonPath"];

    Runner::Paths["PackagePath"] = PackageDir->path();
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] PackagePath" << Runner::Paths["PackagePath"];

    Runner::Paths["RuntimePath"] = FSOps::SubPath(Paths["PackagePath"], "RUNTIME");
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] RuntimePath" << Runner::Paths["RuntimePath"];

    Runner::Paths["ProgramPath"] = FSOps::SubPath(FSOps::SubPath(Paths["RuntimePath"], "drive_c"), QString::fromStdString((*MANIFESTJSON)["PACKAGENAME"]));
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] ProgramPath" << Runner::Paths["ProgramPath"];

    Runner::Paths["MetaDataPath"] = FSOps::SubPath(Paths["PackagePath"], "METADATA");
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] MetaDataPath" << Runner::Paths["MetaDataPath"];

    Runner::Paths["PackageFilesPath"] = FSOps::SubPath(Paths["PackagePath"], "PACKAGEFILES");
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] PackageFilesPath" << Runner::Paths["PackageFilesPath"];

    Runner::Paths["UserDataPath"] = FSOps::SubPath(Paths["PackagePath"], "USERDATA");
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] UserDataPath" << Runner::Paths["UserDataPath"];

    Runner::Paths["TempPath"] = FSOps::SubPath(Paths["PackagePath"], "TEMP");
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] TempPath" << Runner::Paths["TempPath"];

    Runner::Paths["DefPrefixPath"] = FSOps::SubPath(Paths["TempPath"], "DEFPREFIX");
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] DefPrefixPath" << Runner::Paths["DefPrefixPath"];

    if (Runner::subgame != 0)
    {
        Runner::GameName = QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][subgame]["TITLE"]);
        qDebug().noquote() << QTime::currentTime().toString() << "[OUT] GameName: " << Runner::GameName;

        Runner::UMUID = QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][subgame]["UMUID"]);
        qDebug().noquote() << QTime::currentTime().toString() << "[OUT] UMUID: " << Runner::GameName;

        Runner::ExePath = QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][subgame]["EXEPATH"]);
        qDebug().noquote() << QTime::currentTime().toString() << "[OUT] EXEPATH: " << Runner::GameName;

        //MUST MAKE THIS RESPECT QUOTES, ACCOUNT FOR EMPTY.
        if (!((*MANIFESTJSON)["SUBGAMES"][subgame]["EXEARGS"].empty() || (*MANIFESTJSON)["SUBGAMES"][subgame]["EXEARGS"] == "" || (*MANIFESTJSON)["SUBGAMES"][subgame]["EXEARGS"].is_null()))
        {
            Runner::ExeArgs = QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][subgame]["EXEARGS"]).split(" ");
            for (int i = 0; i < Runner::ExeArgs.count(); i++)
            {
                qDebug().noquote() << QTime::currentTime().toString() << QString("[OUT] ExeArg %1:").arg(i) << Runner::ExeArgs.at(i);
            }
        }

        Runner::Paths["WorkDirPath"] = FSOps::SubPath(Paths["ProgramPath"], QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][subgame]["WORKDIR"]));
        qDebug().noquote() << QTime::currentTime().toString() << "[OUT] WorkDirPath" << Runner::Paths["WorkDirPath"];

        if(component == 0)
        {
            component = QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][subgame]["COMPONENT"]).toInt();
            qDebug().noquote() << QTime::currentTime().toString() << "[OUT] Component" << component;
        }
    }

    //CREATE RECIPE BY RESOLVING COMPONENT DEPENDENCY
    while (component != 0)
    {
        Runner::Recipe.prepend(component);
        qDebug() << component;

        if ((*MANIFESTJSON)["COMPONENTS"][component - 1]["PARENTCOMPONENT"].is_null())
        {
            qDebug().noquote() << QTime::currentTime().toString() << "[OUT] Parent component is null, ending recipe.";
            break;
        }

        if ((*MANIFESTJSON)["COMPONENTS"][component - 1]["PARENTCOMPONENT"] == "")
        {
            qDebug().noquote() << QTime::currentTime().toString() << "[OUT] Parent component is empty, ending recipe.";
            break;
        }

        if (QString::fromStdString((*MANIFESTJSON)["COMPONENTS"][component - 1]["PARENTCOMPONENT"]).toInt() == 0)
        {
            qDebug().noquote() << QTime::currentTime().toString() << "[OUT] Parent component is 0, ending recipe.";
            break;
        }

        component = QString::fromStdString((*MANIFESTJSON)["COMPONENTS"][component - 1]["PARENTCOMPONENT"]).toInt();
    }
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] Recipe:" << this->Recipe;

    Runner::WindowsPaths["WindowsProgramPath"] = QDir::cleanPath("C:/" + PackageDir->path()).replace("/", "\\");
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] WindowsProgramPath" << Runner::WindowsPaths["WindowsProgramPath"];

    Runner::WindowsPaths["WindowsProgramPathDoubleBackSlash"]   = QDir::cleanPath("C:/" + PackageDir->path()).replace("/", "\\\\");
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] WindowsProgramPathDoubleBackSlash" << Runner::WindowsPaths["WindowsProgramPathDoubleBackSlash"];

    Runner::SystemVariables["ScreenWidth"] = QString::number(QGuiApplication::primaryScreen()->geometry().width());
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] ScreenWidth" << Runner::SystemVariables["ScreenWidth"];

    Runner::SystemVariables["ScreenHeight"] = QString::number(QGuiApplication::primaryScreen()->geometry().height());
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] ScreenHeight" << Runner::SystemVariables["ScreenHeight"];

    //BUILD AN ARRAY CONTAINING ALL SUBCOMPONENTS, IN ORDER, FILTERED BY RECIPE.
    for (int i = 0; i < (*MANIFESTJSON)["COMPONENTS"].size(); i++)
    {
        if (Runner::Recipe.contains(i + 1))
        {
            for (int j = 0; j < (*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"].size(); j++)
            {
                Runner::SubComponentsArray.push_back((*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]);
                qDebug().noquote() << QTime::currentTime() << "[OUT] Added COMPONENT" << i + 1 << "SUBCOMPONENT" << j + 1;
            }
        }
    }
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] Completed SubComponentsArray:"<< QString::fromStdString(SubComponentsArray.dump(4));

    Runner::SubComponentsArray = JSONOps::ReplaceVariables(Runner::SubComponentsArray, Runner::WindowsPaths);
    Runner::SubComponentsArray = JSONOps::ReplaceVariables(Runner::SubComponentsArray, Runner::Paths);
    Runner::SubComponentsArray = JSONOps::ReplaceVariables(Runner::SubComponentsArray, Runner::SystemVariables);
    Runner::ExeArgs = StringListReplaceVariables(Runner::ExeArgs, Runner::WindowsPaths);
    //CAUTION! ESCAPE CHARACHTERS IN WINDOWS PATHS CRASHES JSON! FIXME
    Runner::ExeArgs = StringListReplaceVariables(Runner::ExeArgs, Runner::Paths);
    Runner::ExeArgs = StringListReplaceVariables(Runner::ExeArgs, Runner::SystemVariables);

    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] Performed substitution on SubComponentsArray:"<< QString::fromStdString(SubComponentsArray.dump(4));
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] Performed substitution on ExeArgs:"<< Runner::ExeArgs;
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] Runner initialisation complete!"<< Runner::ExeArgs;
    return true;
}

bool Runner::BuildRuntime()
{
    //QMessageBox::warning(nullptr, "Building Runtime", "Creating directories.");
    if (!FSOps::CreateDirectories(Runner::Paths))
    {
        return false;
    }

    //QMessageBox::warning(nullptr, "Building Runtime", "Initializing UMU Prefix.");
    if (!InitializeUMUPrefix(Runner::Paths["DefPrefixPath"], Runner::Paths["ProtonPath"], &(Runner::UnionFSString)))
    {
        return false;
    }

    //QMessageBox::warning(nullptr, "Building Runtime", "Processing filesystem Subcomponents.");
    if (!ProcessFileSystemSubComponents())
    {
        return false;
    }

    //QMessageBox::warning(nullptr, "Building Runtime", "Building UnionFS.");
    if (!FSOps::BuildUnionFS(Runner::UnionFSString, Runner::Paths["RuntimePath"], Runner::Paths["UserDataPath"]))
    {
        return false;
    }

    //QMessageBox::warning(nullptr, "Building Runtime", "Processing other Subcomponents.");
    if (!ProcessOtherSubComponents())
    {
        return false;
    }

    //QMessageBox::warning(nullptr, "Building Runtime", "Checking case conflicts.");
    FSOps::CheckCaseConflicts(Runner::Paths["RuntimePath"]);

    return true;
}

bool Runner::InitializeUMUPrefix(QString PrefixPath, QString ProtonPath, QString * UnionFSString)
{
    qDebug().noquote() << QTime::currentTime().toString() << " [OUT] Initialising prefix" << PrefixPath;
    if (Run("wineboot"))
    {
        qDebug().noquote() << QTime::currentTime().toString() << " [OUT] Prefix initialisation successful!";
        UnionFSString->prepend(Runner::Paths["DefPrefixPath"] + "=RO");
        return true;
    }
    qDebug().noquote() << QTime::currentTime().toString() << " [ERR] Prefix initialisation failed!";
    return false;
}

bool Runner::ProcessFileSystemSubComponents()
{
    for (int i = 0; i < Runner::SubComponentsArray.size(); i++)
    {
        nlohmann::ordered_json SubComponentJSON = Runner::SubComponentsArray[i];

        if (SubComponentJSON["TYPE"] == "ZipFileLayer")
        {
            if(!FSOps::MountZipFileLayer(SubComponentJSON, i, Runner::Paths["TempPath"], Runner::Paths["PackageFilesPath"], &(Runner::UnionFSString), QString::fromStdString((*MANIFESTJSON)["PACKAGENAME"])))
            {
                return false;
            }
        }
    }
    return true;
}

bool Runner::ProcessOtherSubComponents()
{
    for (int i = 0; i < Runner::SubComponentsArray.size(); i++)
    {
        nlohmann::ordered_json SubComponentJSON = Runner::SubComponentsArray[i];

        if (SubComponentJSON["TYPE"] == "RegEdit")
        {
            if(!RegOps::RegAdd(SubComponentJSON, Runner::Paths["RuntimePath"], Runner::Paths["ProtonPath"]))
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
    return true;
}

bool Runner::Cleanup()
{
    //QMessageBox::warning(nullptr, "Cleanup", "Unmounting UnionFS.");
    if (!FSOps::DestroyUnionFS(Runner::Paths["RuntimePath"]))
    {
        QMessageBox::warning(nullptr, "CLEANUP INCOMPLETE.", "Could not complete cleanup.");
        return false;
    }

    //QMessageBox::warning(nullptr, "Cleanup", "Unmounting subcomponents.");
    if (!RemoveSubComponents())
    {
        QMessageBox::warning(nullptr, "CLEANUP INCOMPLETE.", "Could not complete cleanup.");
        return false;
    }

    //QMessageBox::warning(nullptr, "Cleanup", "Removing mount points.");
    if (!QDir(Runner::Paths["TempPath"]).removeRecursively())
    {
        QMessageBox::warning(nullptr, "CLEANUP INCOMPLETE.", "Could not complete cleanup.");
        return false;
    }

    //QMessageBox::warning(nullptr, "Cleanup", "Removing runtime.");
    if (!QDir(Runner::Paths["RuntimePath"]).removeRecursively())
    {
        QMessageBox::warning(nullptr, "CLEANUP INCOMPLETE.", "Could not complete cleanup.");
        return false;
    }

    return true;
}

bool Runner::RemoveSubComponents()
{
    for (int i = 0; i < Runner::SubComponentsArray.size(); i++)
    {
        nlohmann::ordered_json SubComponentJSON = Runner::SubComponentsArray[i];

        if (SubComponentJSON["TYPE"] == "ZipFileLayer")
        {
            FSOps::UnmountZipFileLayer(SubComponentJSON, i, Runner::Paths["TempPath"], QString::fromStdString((*MANIFESTJSON)["PACKAGENAME"]));
        }
    }

    return true;
}

//bool Runner::RunWithUMU(QString ProtonPath, QString WinePrefix, QString ExePath, QStringList ExeArgs, QString WorkDirPath, QString GAMEID, QString DllOverrides)
//{
//    qDebug().noquote() << QTime::currentTime().toString() << " [OUT] Executing with umu-launcher: " << ExePath << ExeArgs;
//    qDebug().noquote() << QTime::currentTime().toString() << " [OUT] ProtonPath:" << ProtonPath;
//    qDebug().noquote() << QTime::currentTime().toString() << " [OUT] WinePrefix:" << WinePrefix;
//    qDebug().noquote() << QTime::currentTime().toString() << " [OUT] ExePath:" << ExePath;
//    qDebug().noquote() << QTime::currentTime().toString() << " [OUT] ExeArgs:" << ExeArgs;
//    qDebug().noquote() << QTime::currentTime().toString() << " [OUT] WorkDirPath:" << WorkDirPath;
//    qDebug().noquote() << QTime::currentTime().toString() << " [OUT] GAMEID:" << GAMEID;
//    qDebug().noquote() << QTime::currentTime().toString() << " [OUT] DllOverrides:" << DllOverrides;
//
//
//    QProcessEnvironment RunProcessEnvironment = QProcessEnvironment::systemEnvironment();
//    QProcess * RunProcess = new QProcess;
//    RunProcess->setProgram("umu-run");
//
//    RunProcessEnvironment.insert("PROTONPATH", ProtonPath);
//    RunProcessEnvironment.insert("WINEPREFIX", WinePrefix);
//    ExeArgs.prepend(ExePath);
//
//    if (!ExeArgs.isEmpty())
//    {
//        RunProcess->setArguments(ExeArgs);
//    }
//
//    if (!WorkDirPath.isEmpty())
//    {
//        RunProcess->setWorkingDirectory(WorkDirPath);
//    }
//
//    RunProcessEnvironment.insert("GAMEID", GAMEID);
//
//    if (!DllOverrides.isEmpty())
//    {
//        RunProcessEnvironment.insert("WINEDLLOVERRIDES", DllOverrides);
//    }
//
//    RunProcessEnvironment.insert("PROTON_VERB", "waitforexitandrun");
//
//    RunProcess->setProcessEnvironment(RunProcessEnvironment);
//    RunProcess->start();
//    RunProcess->waitForFinished(-1);
//    qDebug().noquote() << RunProcess->readAllStandardError();
//    qDebug().noquote() << RunProcess->readAllStandardOutput();
//
//    if(RunProcess->exitCode() == 0)
//    {
//        delete RunProcess;
//        return true;
//    }
//    else
//    {
//        delete RunProcess;
//        return false;
//    }
//}

bool Runner::Run(QString OverrideExePath, QStringList OverrideExeArgs)
{
    QString FinalExePath = Runner::ExePath;
    QStringList FinalExeArgs = Runner::ExeArgs;

    if (OverrideExePath != "")
    {
        FinalExePath = OverrideExePath;
    }

    if (OverrideExeArgs != QStringList({}))
    {
        FinalExeArgs = OverrideExeArgs;
    }


    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] Executing with umu-launcher: " << ExePath << ExeArgs;
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] ProtonPath:" << Runner::Paths["ProtonPath"];
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] WinePrefix:" << Runner::Paths["RuntimePath"];
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] ExePath:" << FinalExePath;
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] ExeArgs:" << FinalExeArgs;
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] WorkDirPath:" << Runner::Paths["WorkDirPath"];
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] GAMEID:" << Runner::UMUID;
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] DllOverrides:" << Runner::DllOverrides;


    QProcessEnvironment RunProcessEnvironment = QProcessEnvironment::systemEnvironment();
    QProcess * RunProcess = new QProcess;
    RunProcess->setProgram("umu-run");

    RunProcessEnvironment.insert("PROTONPATH", Runner::Paths["ProtonPath"]);
    RunProcessEnvironment.insert("WINEPREFIX", Runner::Paths["RuntimePath"]);
    FinalExeArgs.prepend(FinalExePath);

    if (!FinalExeArgs.isEmpty())
    {
        RunProcess->setArguments(FinalExeArgs);
    }

    if (!Runner::Paths["WorkDirPath"].isEmpty())
    {
        RunProcess->setWorkingDirectory(Runner::Paths["WorkDirPath"]);
    }

    RunProcessEnvironment.insert("GAMEID", Runner::UMUID);

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
