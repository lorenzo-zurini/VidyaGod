#include "containerwrapper.h"

ContainerWrapper::ContainerWrapper(nlohmann::ordered_json Passed_GlobalConfigJSON, nlohmann::ordered_json Passed_MANIFESTJSON, struct ContainerParams Passed_ContainerParams)
    : GlobalConfigJSON(Passed_GlobalConfigJSON), MANIFESTJSON(Passed_MANIFESTJSON), ContainerParams(Passed_ContainerParams)
{
    this->InitializeContainer();
}

ContainerParams::ContainerParams(std::filesystem::path Passed_PackagePath, int Passed_subgame, int Passed_component)
    : PackagePath(Passed_PackagePath), subgame(Passed_subgame), component(Passed_component)
{
    std::cout << std::chrono::system_clock::now() << "ContainerParams::ContainerParams: " << "[OUT] ContainerParams object created..." << std::endl;
}

bool ContainerWrapper::InitializeContainer()
{
    std::cout << std::chrono::system_clock::now() << "ContainerWrapper::InitializeContainerParams: " << "[OUT] Initializing container..." << std::endl;
    this->DecideComponent(this->MANIFESTJSON, this->ContainerParams);
    this->InitializeContainerParams(this->MANIFESTJSON, this->ContainerParams);
    this->CreateRecipe(this->MANIFESTJSON, this->ContainerParams);
    this->BuildSubComponentsArray(this->MANIFESTJSON, this->ContainerParams);
    this->VariableSubstitution(this->ContainerParams);
    std::cout << std::chrono::system_clock::now() << "ContainerWrapper::InitializeContainerParams: " << "[OUT] Container initialisation complete!" << std::endl;
}

bool ContainerWrapper::DecideComponent(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams)
{
    std::cout << std::chrono::system_clock::now() << "ContainerWrapper::InitializeContainerParams: " << "[OUT] Deciding component: subgame: " << ContainerParams.subgame << " component: " << ContainerParams.component << std::endl;
    if ((ContainerParams.subgame == 0) && (ContainerParams.component == 0))
    {
        std::cout << std::chrono::system_clock::now() << "ContainerWrapper::InitializeContainerParams: " << "[OUT] Neither subgame nor component specified, mounting defprefix." << ContainerParams.component << std::endl;
        return true;
    }
    else if ((ContainerParams.subgame != 0) && (ContainerParams.component == 0))
    {
        if (!MANIFESTJSON["SUBGAMES"][ContainerParams.subgame - 1]["COMPONENT"].is_null())
        {
            ContainerParams.component = QString::fromStdString(MANIFESTJSON["SUBGAMES"][ContainerParams.subgame - 1]["COMPONENT"]).toInt();
            std::cout << std::chrono::system_clock::now() << "ContainerWrapper::InitializeContainerParams: " << "[OUT] Only subgame specified, getting component from JSON: " << ContainerParams.component << std::endl;
        }
        std::cout << std::chrono::system_clock::now() << "ContainerWrapper::InitializeContainerParams: " << "[OUT] Running subgame " << MANIFESTJSON["SUBGAMES"][ContainerParams.subgame - 1]["TITLE"] << std::endl;
        return true;
    }
    else if ((ContainerParams.subgame == 0) && (ContainerParams.component != 0))
    {
        std::cout << std::chrono::system_clock::now() << "ContainerWrapper::InitializeContainerParams: " << "[OUT] Running component " << MANIFESTJSON["COMPONENTS"][ContainerParams.component - 1]["NAME"] << std::endl;
        return true;
    }
    else
    {
        std::cout << std::chrono::system_clock::now() << "ContainerWrapper::InitializeContainerParams: " << "[OUT] Running subgame " << ContainerParams.subgame << " component " << ContainerParams.component << std::endl;
        return true;
    }
    return false;
}

bool ContainerWrapper::InitializeContainerParams(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams)
{
    //Package-specific:
    ContainerParams.PackageName     = MANIFESTJSON["PACKAGENAME"];
    ContainerParams.PackageUID      = MANIFESTJSON["PACKAGEUID"];

    //(Sub)game specific:
    ContainerParams.PackageName     = MANIFESTJSON["SUBGAMES"][ContainerParams.subgame - 1]["TITLE"];
    ContainerParams.UMUID           = MANIFESTJSON["SUBGAMES"][ContainerParams.subgame - 1]["UMUID"];

    //Wine / Proton specific:
    ContainerParams.ExePath         = MANIFESTJSON["SUBGAMES"][ContainerParams.subgame - 1]["EXEPATH"];

    if (!(MANIFESTJSON["SUBGAMES"][ContainerParams.subgame - 1]["WORKDIR"].empty() || MANIFESTJSON["SUBGAMES"][ContainerParams.subgame - 1]["WORKDIR"] == "" || MANIFESTJSON["SUBGAMES"][ContainerParams.subgame - 1]["WORKDIR"].is_null()))
    {
        ContainerParams.WorkDirPath = ContainerParams.PackagePath / "drive_c" / ContainerParams.PackageUID / MANIFESTJSON["SUBGAMES"][ContainerParams.subgame - 1]["WORKDIR"];
        std::cout << std::chrono::system_clock::now() << "ContainerWrapper::InitializeContainerParams: " << "[OUT] WORKDIR: " << ContainerParams.WorkDirPath << std::endl;
    }
    else
    {
        ContainerParams.WorkDirPath = ContainerParams.PackagePath / "drive_c" / ContainerParams.PackageUID;
        std::cout << std::chrono::system_clock::now() << "ContainerWrapper::InitializeContainerParams: " << "[OUT] WORKDIR: " << ContainerParams.WorkDirPath << std::endl;
    }

    //MUST MAKE THIS RESPECT QUOTES, ACCOUNT FOR EMPTY.
    if (!(MANIFESTJSON["SUBGAMES"][ContainerParams.subgame - 1]["EXEARGS"].empty() || MANIFESTJSON["SUBGAMES"][ContainerParams.subgame - 1]["EXEARGS"] == "" || MANIFESTJSON["SUBGAMES"][ContainerParams.subgame - 1]["EXEARGS"].is_null()))
    {
        std::string UnsplitExeArgs(MANIFESTJSON["SUBGAMES"][ContainerParams.subgame - 1]["EXEARGS"]);
        ContainerParams.ExeArgs = [](const std::string& s)
        {
            std::vector<std::string> out;
            std::istringstream iss(s);
            for (std::string tok; std::getline(iss, tok, ' ');)
            out.push_back(tok);
            return out;
        }
        (UnsplitExeArgs);

        for (int i = 0; i < ContainerParams.ExeArgs.size(); i++)
        {
            std::cout << std::chrono::system_clock::now() << "ContainerWrapper::InitializeContainerParams: " << QString("[OUT] ExeArg %1: ").arg(i).toStdString() << ContainerParams.ExeArgs.at(i) << std::endl;
        }
    }
    return true;
}

bool ContainerWrapper::CreateRecipe(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams)
{
    //CREATE RECIPE BY RESOLVING COMPONENT DEPENDENCY
    ContainerParams.Recipe.clear();
    while (ContainerParams.component != 0)
    {
        ContainerParams.Recipe.push_back(ContainerParams.component);
        if (MANIFESTJSON["COMPONENTS"][ContainerParams.component - 1]["PARENTCOMPONENT"].is_null())
        {
            std::cout << std::chrono::system_clock::now() << " ContainerWrapper::CreateRecipe: " << "[OUT] Parent component is null, ending recipe." << std::endl;
            break;
        }

        if (MANIFESTJSON["COMPONENTS"][ContainerParams.component - 1]["PARENTCOMPONENT"] == "")
        {
            std::cout << std::chrono::system_clock::now() << " ContainerWrapper::CreateRecipe: " << "[OUT] Parent component is empty, ending recipe." << std::endl;
            break;
        }

        if (QString::fromStdString(MANIFESTJSON["COMPONENTS"][ContainerParams.component - 1]["PARENTCOMPONENT"]).toInt() == 0)
        {
            std::cout << std::chrono::system_clock::now() << " ContainerWrapper::CreateRecipe: " << "[OUT] Parent component is 0, ending recipe." << std::endl;
            break;
        }

        ContainerParams.component = QString::fromStdString(MANIFESTJSON["COMPONENTS"][ContainerParams.component - 1]["PARENTCOMPONENT"]).toInt();
    }
    std::reverse(ContainerParams.Recipe.begin(), ContainerParams.Recipe.end());
    std::cout << std::chrono::system_clock::now() << "ContainerWrapper::CreateRecipe: " << "[OUT] Recipe: " << [&](const std::vector<int>& v){ std::string r; for (int x : v) r += std::to_string(x) + " "; return r;}(ContainerParams.Recipe) << std::endl;
    return true;
}

bool ContainerWrapper::BuildSubComponentsArray(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams)
{
    //BUILD AN ARRAY CONTAINING ALL SUBCOMPONENTS, IN ORDER, FILTERED BY RECIPE.
    for (int i = 0; i < MANIFESTJSON["COMPONENTS"].size(); i++)
    {
        if (std::find(ContainerParams.Recipe.begin(), ContainerParams.Recipe.end(), int(i + 1)) != ContainerParams.Recipe.end())
        {
            for (int j = 0; j < MANIFESTJSON["COMPONENTS"][i]["SUBCOMPONENTS"].size(); j++)
            {
                ContainerParams.SubComponentsArray.push_back(MANIFESTJSON["COMPONENTS"][i]["SUBCOMPONENTS"][j]);
                std::cout << std::chrono::system_clock::now() << " ContainerWrapper::BuildSubComponentsArray: " << "[OUT] Added COMPONENT" << i + 1 << "SUBCOMPONENT" << j + 1 << std::endl;
            }
        }
    }
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::BuildSubComponentsArray: " << "[OUT] Completed SubComponentsArray:"<< std::endl << ContainerParams.SubComponentsArray.dump(4) << std::endl;
    return true;
}

bool ContainerWrapper::VariableSubstitution(struct ContainerParams &ContainerParams)
//SPLIT THIS FUNCTION!
{
    std::map<std::string, std::string> VariableSubstitutionMap;

    //Paths
    VariableSubstitutionMap["PackagePath"]                          = ContainerParams.PackagePath;
    VariableSubstitutionMap["RuntimePath"]                          = ContainerParams.PackagePath / "RUNTIME";
    VariableSubstitutionMap["ProgramPath"]                          = ContainerParams.PackagePath / "RUNTIME" / "drive_c"/ ContainerParams.PackageUID;
    VariableSubstitutionMap["MetaDataPath"]                         = ContainerParams.PackagePath / "METADATA";
    VariableSubstitutionMap["PackageFilesPath"]                     = ContainerParams.PackagePath / "PACKAGEFILES";
    VariableSubstitutionMap["UserDataPath"]                         = ContainerParams.PackagePath / "USERDATA";
    VariableSubstitutionMap["TempPath"]                             = ContainerParams.PackagePath / "TEMP";
    VariableSubstitutionMap["DefPrefixPath"]                        = ContainerParams.PackagePath / "TEMP" / "DEFPREFIX";
    VariableSubstitutionMap["ExePathRelative"]                      = ContainerParams.ExePath;
    VariableSubstitutionMap["ExePathComplete"]                      = ContainerParams.PackagePath / "RUNTIME" / "drive_c" / ContainerParams.PackageUID / ContainerParams.ExePath;

    //Windows Paths
    VariableSubstitutionMap["WindowsProgramPath"]                   = "C:\\" + ContainerParams.PackageUID;
    VariableSubstitutionMap["WindowsExePathCompete"]                = "C:\\" + ContainerParams.PackageUID + "\\" + ContainerParams.ExePath;
    VariableSubstitutionMap["WindowsProgramPathDoubleBackSlash"]    = "C:\\\\" + ContainerParams.PackageUID;

    //System Variables
    VariableSubstitutionMap["ScreenWidth"]                          = QString::number(QGuiApplication::primaryScreen()->geometry().width()).toStdString();
    VariableSubstitutionMap["ScreenHeight"]                         = QString::number(QGuiApplication::primaryScreen()->geometry().height()).toStdString();

    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::VariableSubstitution: " << "[OUT] Completed VariableSubstitutionMap:" << std::endl;
    for (const auto& [Key, Value] : VariableSubstitutionMap)
    {
        std::cout << Key << "\t\t\t\t\t" << Value << std::endl;
    }


    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::VariableSubstitution: " << "[OUT] Performing variable substitution on SubComponentsArray." << std::endl;
    std::string SubComponentsArrayString = ContainerParams.SubComponentsArray.dump();
    for (const auto& [Key, Value] : VariableSubstitutionMap)
    {
        SubComponentsArrayString = std::regex_replace(SubComponentsArrayString, std::regex("%" + Key + "%"), Value);
    }
    ContainerParams.SubComponentsArray = nlohmann::ordered_json::parse(SubComponentsArrayString);
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::VariableSubstitution: " << "[OUT] Performed variable substitution on SubComponentsArray. Result: " << std::endl << ContainerParams.SubComponentsArray.dump(4) << std::endl;

    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::VariableSubstitution: " << "[OUT] Performing variable substitution on ExeArgs." << std::endl;
    for (const auto& [Key, Value] : VariableSubstitutionMap)
    {
        for (auto& Token : ContainerParams.ExeArgs)
        {
            Token = std::regex_replace(Token, std::regex("%" + Key + "%"), Value);
        }
    }
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::VariableSubstitution: " << "[OUT] Performed variable substitution on ExeArgs. Result: " << [](const auto& args){ std::string s; for(auto& a:args) s += a + " "; if(!s.empty()) s.pop_back(); return s;}(ContainerParams.ExeArgs) << std::endl;
    return true;
}

/*


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
