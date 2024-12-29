#include "runner.h"

Runner::Runner(
                QDir * PackageDir,
                nlohmann::ordered_json * MANIFESTJSON,
                nlohmann::ordered_json * GlobalConfigJSON,
                int subgame
              ):

                PackageDir(PackageDir),
                MANIFESTJSON(MANIFESTJSON),
                GlobalConfigJSON(GlobalConfigJSON),
                subgame(subgame)
{
    this->InitParams();
}

bool Runner::InitParams()
{
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] Initializing Runner";

    Runner::GameName = QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][subgame]["TITLE"]);
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] GameName: " << Runner::GameName;

    Runner::UMUID = QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][subgame]["UMUID"]);
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] UMUID: " << Runner::GameName;

    //Runner::ParentPackage = QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][subgame]["TITLE"]);
    //qDebug().noquote() << QTime::currentTime().toString() << " [OUT] GameName: " << Runner::GameName;

    Runner::ExePath = QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][subgame]["EXEPATH"]);
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] EXEPATH: " << Runner::GameName;

    //Runner::Recipe = QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][subgame]["TITLE"]);
    //qDebug().noquote() << QTime::currentTime().toString() << " [OUT] GameName: " << Runner::GameName;

    //MUST MAKE THIS RESPECT QUOTES, ACCOUNT FOR EMPTY.
    if (!((*MANIFESTJSON)["SUBGAMES"][subgame]["EXEARGS"].empty() || (*MANIFESTJSON)["SUBGAMES"][subgame]["EXEARGS"] == "" || (*MANIFESTJSON)["SUBGAMES"][subgame]["EXEARGS"].is_null()))
    {
        Runner::ExeArgs = QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][subgame]["EXEARGS"]).split(" ");
        for (int i = 0; i < Runner::ExeArgs.count(); i++)
        {
            qDebug().noquote() << QTime::currentTime().toString() << QString("[OUT] ExeArg %1:").arg(i) << Runner::ExeArgs.at(i);
        }
    }

    Runner::Paths["ProtonPath"] = "/usr/share/steam/compatibilitytools.d/proton-ge-custom/";
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] ProtonPath" << Runner::Paths["ProtonPath"];

    Runner::Paths["PackagePath"] = PackageDir->path();
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] PackagePath" << Runner::Paths["PackagePath"];

    Runner::Paths["RuntimePath"] = FSOps::SubPath(Paths["PackagePath"], "RUNTIME");
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] RuntimePath" << Runner::Paths["RuntimePath"];

    Runner::Paths["ProgramPath"] = FSOps::SubPath(FSOps::SubPath(Paths["RuntimePath"], "drive_c"), PackageDir->path());
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] ProgramPath" << Runner::Paths["ProgramPath"];

    Runner::Paths["WorkDirPath"] = FSOps::SubPath(Paths["ProgramPath"], QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][subgame]["WORKDIR"]));
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] WorkDirPath" << Runner::Paths["WorkDirPath"];

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

    Runner::WindowsPaths["WindowsProgramPath"] = QDir::cleanPath("C:/" + PackageDir->path()).replace("/", "\\");
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] WindowsProgramPath" << Runner::WindowsPaths["WindowsProgramPath"];

    Runner::WindowsPaths["WindowsProgramPathDoubleBackSlash"]   = QDir::cleanPath("C:/" + PackageDir->path()).replace("/", "\\\\");
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] WindowsProgramPathDoubleBackSlash" << Runner::WindowsPaths["WindowsProgramPathDoubleBackSlash"];

    Runner::SystemVariables["ScreenWidth"] = QString::number(QGuiApplication::primaryScreen()->geometry().width());
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] ScreenWidth" << Runner::SystemVariables["ScreenWidth"];

    Runner::SystemVariables["ScreenHeight"] = QString::number(QGuiApplication::primaryScreen()->geometry().height());
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] ScreenHeight" << Runner::SystemVariables["ScreenHeight"];

    //CREATE RECIPE BY RESOLVING COMPONENT DEPENDENCY
    int component = QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][subgame]["COMPONENT"]).toInt();
    while (component != 0)
    {
        Runner::Recipe.prepend(component - 1);
        qDebug() << component - 1;

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

    qDebug() << Runner::Recipe;

    //BUILD AN ARRAY CONTAINING ALL SUBCOMPONENTS, IN ORDER, FILTERED BY RECIPE.
    for (int i = 0; i < (*MANIFESTJSON)["COMPONENTS"].size(); i++)
    {
        if (Runner::Recipe.contains(i))
        {
            for (int j = 0; j < (*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"].size(); j++)
            {
                Runner::SubComponentsArray.push_back((*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]);
                qDebug().noquote() << QTime::currentTime() << " [OUT] Added COMPONENT" << i + 1 << "SUBCOMPONENT" << j + 1;
            }
        }
    }
    qDebug().noquote() << QTime::currentTime() << "[OUT] Completed SubComponentsArray:"<< QString::fromStdString(SubComponentsArray.dump(4));

    Runner::SubComponentsArray = JSONOps::ReplaceVariables(Runner::SubComponentsArray, Runner::WindowsPaths);
    Runner::SubComponentsArray = JSONOps::ReplaceVariables(Runner::SubComponentsArray, Runner::Paths);
    Runner::SubComponentsArray = JSONOps::ReplaceVariables(Runner::SubComponentsArray, Runner::SystemVariables);
    Runner::ExeArgs = StringListReplaceVariables(Runner::ExeArgs, Runner::WindowsPaths);
    //CAUTION! ESCAPE CHARACHTERS IN WINDOWS PATHS CRASHES JSON! FIXME
    Runner::ExeArgs = StringListReplaceVariables(Runner::ExeArgs, Runner::Paths);
    Runner::ExeArgs = StringListReplaceVariables(Runner::ExeArgs, Runner::SystemVariables);

    qDebug().noquote() << QTime::currentTime() << "[OUT] Performed substitution on SubComponentsArray:"<< QString::fromStdString(SubComponentsArray.dump(4));
    qDebug().noquote() << QTime::currentTime() << "[OUT] Performed substitution on ExeArgs:"<< Runner::ExeArgs;
    return true;
}

QStringList Runner::StringListReplaceVariables(QStringList OriginalStringList, QMap<QString, QString> VariableValues)
{
    for (auto [Variable, Value] : VariableValues.asKeyValueRange())
    {
        OriginalStringList.replaceInStrings(("%" + Variable + "%"), Value);
    }
    return OriginalStringList;
}
