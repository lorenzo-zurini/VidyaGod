#include "containerwrapper.h"

ContainerWrapper::ContainerWrapper(std::filesystem::path Passed_PackagePath, nlohmann::ordered_json Passed_MANIFESTJSON, nlohmann::ordered_json Passed_GlobalConfigJSON, int Passed_subgame, int Passed_component)
    : PackagePath(Passed_PackagePath), MANIFESTJSON(Passed_MANIFESTJSON), GlobalConfigJSON(Passed_GlobalConfigJSON), subgame(Passed_subgame), component(Passed_component)
{
    std::cout << QTime::currentTime().toString().toStdString() << "ContainerWrapper: " << "[OUT] Initializing container..." << std::endl;
    this->DecideComponent(this->MANIFESTJSON, this->subgame, this->component);
    this->InitializeRunnerParams();
    this->CreateRecipe();
}

bool ContainerWrapper::DecideComponent(nlohmann::ordered_json MANIFESTJSON, int &subgame, int &component)
{
    std::cout << QTime::currentTime().toString().toStdString() << "ContainerWrapper: " << "[OUT] Deciding component: subgame: " << subgame << " component: " << component << std::endl;
    if ((subgame == 0) && (component == 0))
    {
        std::cout << QTime::currentTime().toString().toStdString() << "ContainerWrapper: " << "[OUT] Neither subgame nor component specified, mounting defprefix." << component << std::endl;
        return true;
    }
    else if ((subgame != 0) && (component == 0))
    {
        if (!MANIFESTJSON["SUBGAMES"][subgame - 1]["COMPONENT"].is_null())
        {
            component = QString::fromStdString(MANIFESTJSON["SUBGAMES"][subgame - 1]["COMPONENT"]).toInt();
            std::cout << QTime::currentTime().toString().toStdString() << "ContainerWrapper: " << "[OUT] Only subgame specified, getting component from JSON: " << component << std::endl;
        }
        std::cout << QTime::currentTime().toString().toStdString() << "ContainerWrapper: " << "[OUT] Running subgame " << MANIFESTJSON["SUBGAMES"][subgame - 1]["TITLE"] << std::endl;
        return true;
    }
    else if ((subgame == 0) && (component != 0))
    {
        std::cout << QTime::currentTime().toString().toStdString() << "ContainerWrapper: " << "[OUT] Running component " << MANIFESTJSON["COMPONENTS"][component - 1]["NAME"] << std::endl;
        return true;
    }
    else
    {
        std::cout << QTime::currentTime().toString().toStdString() << "ContainerWrapper: " << "[OUT] Running subgame " << subgame << " component " << component << std::endl;
        return true;
    }
    return false;
}

bool ContainerWrapper::InitializeRunnerParams()
{
    //Package-specific:
    std::string PackageName = MANIFESTJSON["PACKAGENAME"];
    std::string PackageUID = MANIFESTJSON["PACKAGEUID"];

    //(Sub)game specific:
    std::string GameName = MANIFESTJSON["SUBGAMES"][subgame - 1]["TITLE"];
    std::string UMUID = MANIFESTJSON["SUBGAMES"][subgame - 1]["UMUID"];

    //Wine / Proton specific:
    std::string ExePath = MANIFESTJSON["SUBGAMES"][subgame - 1]["EXEPATH"];

    if (!(MANIFESTJSON["SUBGAMES"][subgame - 1]["WORKDIR"].empty() || MANIFESTJSON["SUBGAMES"][subgame - 1]["WORKDIR"] == "" || MANIFESTJSON["SUBGAMES"][subgame - 1]["WORKDIR"].is_null()))
    {
        this->WorkDirPath = this->PackagePath / "drive_c" / this->PackageUID / MANIFESTJSON["SUBGAMES"][subgame - 1]["WORKDIR"];
        std::cout << QTime::currentTime().toString().toStdString() << "ContainerWrapper:" << "[OUT] WORKDIR: " << this->WorkDirPath << std::endl;
    }
    else
    {
        this->WorkDirPath = this->PackagePath / "drive_c" / this->PackageUID;
        std::cout << QTime::currentTime().toString().toStdString() << "ContainerWrapper:" << "[OUT] WORKDIR: " << this->WorkDirPath << std::endl;
    }

    //MUST MAKE THIS RESPECT QUOTES, ACCOUNT FOR EMPTY.
    if (!(MANIFESTJSON["SUBGAMES"][subgame - 1]["EXEARGS"].empty() || MANIFESTJSON["SUBGAMES"][subgame - 1]["EXEARGS"] == "" || MANIFESTJSON["SUBGAMES"][subgame - 1]["EXEARGS"].is_null()))
    {
        std::string UnsplitExeArgs(MANIFESTJSON["SUBGAMES"][subgame - 1]["EXEARGS"]);
        this->ExeArgs = [](const std::string& s)
        {
            std::vector<std::string> out;
            std::istringstream iss(s);
            for (std::string tok; std::getline(iss, tok, ' ');)
            out.push_back(tok);
            return out;
        }
        (UnsplitExeArgs);

        for (int i = 0; i < this->ExeArgs.size(); i++)
        {
            std::cout << QTime::currentTime().toString().toStdString() << "ContainerWrapper:" << QString("[OUT] ExeArg %1:").arg(i).toStdString() << this->ExeArgs.at(i) << std::endl;
        }
    }
    return true;
}

bool ContainerWrapper::CreateRecipe()
{
    //CREATE RECIPE BY RESOLVING COMPONENT DEPENDENCY
    std::vector<int> Recipe;
    while (component != 0)
    {
        Recipe.push_back(component);
        if (MANIFESTJSON["COMPONENTS"][component - 1]["PARENTCOMPONENT"].is_null())
        {
            std::cout << QTime::currentTime().toString().toStdString() << "ContainerWrapper:" << "[OUT] Parent component is null, ending recipe." << std::endl;
            break;
        }

        if (MANIFESTJSON["COMPONENTS"][component - 1]["PARENTCOMPONENT"] == "")
        {
            std::cout << QTime::currentTime().toString().toStdString() << "ContainerWrapper:" << "[OUT] Parent component is empty, ending recipe." << std::endl;
            break;
        }

        if (QString::fromStdString(MANIFESTJSON["COMPONENTS"][component - 1]["PARENTCOMPONENT"]).toInt() == 0)
        {
            std::cout << QTime::currentTime().toString().toStdString() << "ContainerWrapper:" << "[OUT] Parent component is 0, ending recipe." << std::endl;
            break;
        }

        component = QString::fromStdString(MANIFESTJSON["COMPONENTS"][component - 1]["PARENTCOMPONENT"]).toInt();
    }
    std::reverse(Recipe.begin(), Recipe.end());
    this->Recipe = Recipe;
    std::cout << "ContainerWrapper:" << "[OUT] Recipe: " << [&](const std::vector<int>& v){ std::string r; for (int x : v) r += std::to_string(x) + " "; return r;}(this->Recipe) << std::endl;
    return true;
}

/*

this->WindowsPaths["WindowsProgramPath"] = QDir::cleanPath("C:/" + this->PackageUID).replace("/", "\\");
std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] WindowsProgramPath" << this->WindowsPaths["WindowsProgramPath"].toStdString() << std::endl;

this->WindowsPaths["WindowsExePath"] = QDir::cleanPath("C:/" + this->PackageUID + QDir::separator() + this->ExePath).replace("/", "\\");
std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] WindowsExePath" << this->WindowsPaths["WindowsExePath"].toStdString() << std::endl;

this->WindowsPaths["WindowsProgramPathDoubleBackSlash"]   = QDir::cleanPath("C:/" + this->PackageUID).replace("/", "\\\\");
std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] WindowsProgramPathDoubleBackSlash" << this->WindowsPaths["WindowsProgramPathDoubleBackSlash"].toStdString() << std::endl;

//FIND ANOTHER WAY TO GET SCREEN VARIABLES THAT DOESNT RELY ON QT
//this->SystemVariables["ScreenWidth"] = QString::number(QGuiApplication::primaryScreen()->geometry().width());
//std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] ScreenWidth" << this->SystemVariables["ScreenWidth"].toStdString() << std::endl;

//FIND ANOTHER WAY TO GET SCREEN VARIABLES THAT DOESNT RELY ON QT
//this->SystemVariables["ScreenHeight"] = QString::number(QGuiApplication::primaryScreen()->geometry().height());
//std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] ScreenHeight" << this->SystemVariables["ScreenHeight"].toStdString() << std::endl;

//BUILD AN ARRAY CONTAINING ALL SUBCOMPONENTS, IN ORDER, FILTERED BY RECIPE.
for (int i = 0; i < (*MANIFESTJSON)["COMPONENTS"].size(); i++)
{
    if (this->Recipe.contains(i + 1))
    {
        for (int j = 0; j < (*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"].size(); j++)
        {
            this->SubComponentsArray.push_back((*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]);
            //qDebug().noquote() << QTime::currentTime() << "Runner:" << "[OUT] Added COMPONENT" << i + 1 << "SUBCOMPONENT" << j + 1;
        }
    }
}
std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] Completed SubComponentsArray:"<< SubComponentsArray.dump(4) << std::endl;

this->SubComponentsArray = JSONOps::ReplaceVariables(this->SubComponentsArray, this->WindowsPaths);
this->SubComponentsArray = JSONOps::ReplaceVariables(this->SubComponentsArray, this->Paths);
this->SubComponentsArray = JSONOps::ReplaceVariables(this->SubComponentsArray, this->SystemVariables);
this->ExeArgs = StringListReplaceVariables(this->ExeArgs, this->WindowsPaths);
//CAUTION! ESCAPE CHARACHTERS IN WINDOWS PATHS CRASHES JSON! FIXME
this->ExeArgs = StringListReplaceVariables(this->ExeArgs, this->Paths);
this->ExeArgs = StringListReplaceVariables(this->ExeArgs, this->SystemVariables);

///////////////////////////////////////////////////////////////////////////////////////////////
std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] Performed substitution on SubComponentsArray:"<< SubComponentsArray.dump(4) << std::endl;
//std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] Performed substitution on ExeArgs:"<< this->ExeArgs;
std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] Runner initialisation complete!" << std::endl;
return true;


*/

//Container Wrapper structure outline:
//
// -> STRUCT CONTAINERPARAMS
//    Contains all the information needed for building runtime and executing runner.
//
//
//
//   Runtime building (runner-indifferent):
//   Build VFS
//   Handle registry
//   Basically put all subcomponents in place
//   Download or find the actual runners
//
//   Runner execution part.
//   I need to generalize the runner class so it works with all kinds of runners.
