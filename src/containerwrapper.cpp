#include "containerwrapper.h"

ContainerWrapper::ContainerWrapper(nlohmann::ordered_json &Passed_GlobalConfigJSON, nlohmann::ordered_json &Passed_MANIFESTJSON, struct ContainerParams &Passed_ContainerParams)
    : GlobalConfigJSON(Passed_GlobalConfigJSON), MANIFESTJSON(Passed_MANIFESTJSON), ContainerParams(Passed_ContainerParams)
{
    this->InitializeContainer();
}

ContainerParams::ContainerParams(std::filesystem::path Passed_PackagePath, std::string Passed_subgame_id, std::string Passed_component_id)
    : PackagePath(Passed_PackagePath), subgame_id(Passed_subgame_id), component_id(Passed_component_id)
{
    std::cout << std::chrono::system_clock::now() << " ContainerParams::ContainerParams: " << "[OUT] ContainerParams object created..." << std::endl;
}

int ContainerWrapper::FindSubgameIndex(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID)
{
    for (int i = 0; i < (int)MANIFESTJSON["SUBGAMES"].size(); i++)
    {
        if (!MANIFESTJSON["SUBGAMES"][i]["SUBGAMEID"].is_null() && MANIFESTJSON["SUBGAMES"][i]["SUBGAMEID"] == SubgameID)
        {
            return i;
        }
    }
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::FindSubgameIndex: " << "[ERR] Subgame ID not found: " << SubgameID << std::endl;
    return -1;
}

int ContainerWrapper::FindComponentIndex(const nlohmann::ordered_json &MANIFESTJSON, const std::string &ComponentID)
{
    if (ComponentID.empty()) return -1;
    for (int i = 0; i < (int)MANIFESTJSON["COMPONENTS"].size(); i++)
    {
        if (!MANIFESTJSON["COMPONENTS"][i]["COMPONENTID"].is_null() && MANIFESTJSON["COMPONENTS"][i]["COMPONENTID"] == ComponentID)
        {
            return i;
        }
    }
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::FindComponentIndex: " << "[ERR] Component ID not found: " << ComponentID << std::endl;
    return -1;
}

//TO-DO:
//CREATE SEPARATE CLASS FOR VFS
//CREATE WRAPPER CLASS FOR REGISTRY

bool ContainerWrapper::InitializeContainer()
{
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeContainer: " << "[OUT] Initializing container..." << std::endl;
    if(!this->DecideComponent(this->MANIFESTJSON, this->ContainerParams))
    {
        std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeContainer: " << "[ERR] ContainerWrapper::DecideComponent failed, aborting...." << std::endl;
        return false;
    }
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeContainer: " << "[OUT] ContainerWrapper::DecideComponent successful." << std::endl;

    if(!this->DeriveContainerParams(this->MANIFESTJSON, this->ContainerParams, this->GlobalConfigJSON))
    {
        std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeContainer: " << "[ERR] ContainerWrapper::DeriveContainerParams failed, aborting...." << std::endl;
        return false;
    }
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeContainer: " << "[OUT] ContainerWrapper::DeriveContainerParams successful." << std::endl;

    if(!this->CreateRecipe(this->MANIFESTJSON, this->ContainerParams))
    {
        std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeContainer: " << "[ERR] ContainerWrapper::CreateRecipe failed, aborting...." << std::endl;
        return false;
    }
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeContainer: " << "[OUT] ContainerWrapper::CreateRecipe successful." << std::endl;

    if(!this->BuildSubComponentsArray(this->MANIFESTJSON, this->ContainerParams))
    {
        std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeContainer: " << "[ERR] ContainerWrapper::BuildSubComponentsArray failed, aborting...." << std::endl;
        return false;
    }
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeContainer: " << "[OUT] ContainerWrapper::BuildSubComponentsArray successful." << std::endl;
    /*
    MOVE HERE: VARIABLE SUBSTITUTION FOR ARGS
    VARIABLE SUBSTITUTUION FOR SUBCOMPONENTSARRAY
    */
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeContainerParams: " << "[OUT] Container initialisation complete!" << std::endl;
    return true;
}

bool ContainerWrapper::BuildContainerRuntime()
{
    if (ContainerParams.RunnerTypeEnum == RunnerType::Wine)
    {
        this->InitializeDefPrefix(this->ContainerParams);

        //REGISTRY WRAPPER
        this->CreateFlatRegPatchJSON(this->ContainerParams);
        this->CreateRegPatchFiles(this->ContainerParams);
        this->MergeRegPatchFiles(this->ContainerParams);
    }

    //VFS WRAPPER
    this->PreMountFilesystemComponents(this->ContainerParams);
    this->FinalizeVFSString(this->ContainerParams);
    this->MountVFS(this->ContainerParams);
    this->CheckCaseConflicts(ContainerParams.RuntimePath);

    if (ContainerParams.RunnerTypeEnum == RunnerType::Wine)
    {
        this->ProcessDLLOverrides(this->ContainerParams);
        this->ProcessFileEdits(this->ContainerParams);
    }

    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::BuildContainerRuntime: " << "[OUT] Container ready to run!" << std::endl;
    return true;
}

bool ContainerWrapper::BuildVirtualFilesystem()
{
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::BuildVirtualFilesystem: " << "[OUT] Building virtual filesystem." << std::endl;
    return true;
}

bool ContainerWrapper::DecideComponent(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams)
{
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DecideComponent: " << "[OUT] Deciding component: subgame_id: " << ContainerParams.subgame_id << " component_id: " << ContainerParams.component_id << std::endl;

    if (ContainerParams.subgame_id.empty() && ContainerParams.component_id.empty())
    {
        std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DecideComponent: " << "[OUT] Neither subgame nor component specified, mounting defprefix." << std::endl;
        return true;
    }
    else if (!ContainerParams.subgame_id.empty() && ContainerParams.component_id.empty())
    {
        int SubgameIdx = FindSubgameIndex(MANIFESTJSON, ContainerParams.subgame_id);
        if (SubgameIdx == -1) return false;
        auto &ComponentField = MANIFESTJSON["SUBGAMES"][SubgameIdx]["COMPONENT"];
        if (!ComponentField.is_null() && ComponentField != "")
        {
            ContainerParams.component_id = ComponentField;
            std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DecideComponent: " << "[OUT] Only subgame specified, resolved component_id: " << ContainerParams.component_id << std::endl;
        }
        std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DecideComponent: " << "[OUT] Running subgame " << MANIFESTJSON["SUBGAMES"][SubgameIdx]["TITLE"] << std::endl;
        return true;
    }
    else if (ContainerParams.subgame_id.empty() && !ContainerParams.component_id.empty())
    {
        int ComponentIdx = FindComponentIndex(MANIFESTJSON, ContainerParams.component_id);
        if (ComponentIdx == -1) return false;
        std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DecideComponent: " << "[OUT] Running component " << MANIFESTJSON["COMPONENTS"][ComponentIdx]["NAME"] << std::endl;
        return true;
    }
    else
    {
        std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DecideComponent: " << "[OUT] Running subgame " << ContainerParams.subgame_id << " component " << ContainerParams.component_id << std::endl;
        return true;
    }
    return false;
}

bool ContainerWrapper::CreateRecipe(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams)
{
    //CREATE RECIPE BY RESOLVING COMPONENT DEPENDENCY CHAIN
    ContainerParams.Recipe.clear();
    std::string CurrentID = ContainerParams.component_id;
    while (!CurrentID.empty())
    {
        int Idx = FindComponentIndex(MANIFESTJSON, CurrentID);
        if (Idx == -1) break;

        ContainerParams.Recipe.push_back(CurrentID);

        auto &ParentField = MANIFESTJSON["COMPONENTS"][Idx]["PARENTCOMPONENT"];
        if (ParentField.is_null() || ParentField == "")
        {
            std::cout << std::chrono::system_clock::now() << " ContainerWrapper::CreateRecipe: " << "[OUT] No parent, ending recipe." << std::endl;
            break;
        }
        CurrentID = ParentField;
    }
    std::reverse(ContainerParams.Recipe.begin(), ContainerParams.Recipe.end());
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::CreateRecipe: " << "[OUT] Recipe: " << [&](const std::vector<std::string>& v){ std::string r; for (const auto &x : v) r += x + " "; return r;}(ContainerParams.Recipe) << std::endl;
    return true;
}

bool ContainerWrapper::BuildSubComponentsArray(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams)
{
    //BUILD AN ARRAY CONTAINING ALL SUBCOMPONENTS, IN ORDER, FILTERED BY RECIPE.
    for (int i = 0; i < (int)MANIFESTJSON["COMPONENTS"].size(); i++)
    {
        std::string ComponentID = MANIFESTJSON["COMPONENTS"][i].contains("COMPONENTID") && !MANIFESTJSON["COMPONENTS"][i]["COMPONENTID"].is_null()
                                  ? std::string(MANIFESTJSON["COMPONENTS"][i]["COMPONENTID"])
                                  : "";
        if (std::find(ContainerParams.Recipe.begin(), ContainerParams.Recipe.end(), ComponentID) != ContainerParams.Recipe.end())
        {
            for (int j = 0; j < (int)MANIFESTJSON["COMPONENTS"][i]["SUBCOMPONENTS"].size(); j++)
            {
                std::string NewComponentJSONString = MANIFESTJSON["COMPONENTS"][i]["SUBCOMPONENTS"][j].dump();
                ContainerWrapper::StringVariableSubstitution(NewComponentJSONString, ContainerParams.GetVariablesMap());
                ContainerParams.SubComponentsArray.push_back(nlohmann::ordered_json::parse(NewComponentJSONString));
                std::cout << std::chrono::system_clock::now() << " ContainerWrapper::BuildSubComponentsArray: " << "[OUT] Added COMPONENT " << ComponentID << " SUBCOMPONENT " << j + 1 << std::endl;
            }
        }
    }
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::BuildSubComponentsArray: " << "[OUT] Completed SubComponentsArray:"<< std::endl << ContainerParams.SubComponentsArray.dump(4) << std::endl;
    return true;
}

bool ContainerWrapper::DeriveContainerParams(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams, nlohmann::ordered_json GlobalConfigJSON)
{
    //Package-specific:
    ContainerParams.PackageName                         = MANIFESTJSON["PACKAGENAME"];
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DeriveContainerParams: " << "[OUT] PackageName: " << ContainerParams.PackageName << std::endl;

    ContainerParams.PackageUID                          = MANIFESTJSON["PACKAGEUID"];
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DeriveContainerParams: " << "[OUT] PackageUID: " << ContainerParams.PackageUID << std::endl;

    //(Sub)game specific:
    int SubgameIdx = FindSubgameIndex(MANIFESTJSON, ContainerParams.subgame_id);
    if (!ContainerParams.subgame_id.empty() && SubgameIdx != -1)
    {
        ContainerParams.GameName                        = MANIFESTJSON["SUBGAMES"][SubgameIdx]["TITLE"];
        ContainerParams.UMUID                           = MANIFESTJSON["SUBGAMES"][SubgameIdx]["UMUID"];
        ContainerParams.Platform                        = MANIFESTJSON["SUBGAMES"][SubgameIdx]["PLATFORM"];
    }
    else
    {
        ContainerParams.UMUID                           = "0";
        ContainerParams.Platform                        = "Microsoft Windows";
    }
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DeriveContainerParams: " << "[OUT] GameName: " << ContainerParams.GameName << std::endl;
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DeriveContainerParams: " << "[OUT] UMUID: " << ContainerParams.UMUID << std::endl;
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DeriveContainerParams: " << "[OUT] Platform: " << ContainerParams.Platform << std::endl;

    // Runner resolution: USERSETTINGS > RECOMMENDED_RUNNER > first available
    std::string PreferredRunner;
    if (GlobalConfigJSON.contains("USERSETTINGS") &&
        GlobalConfigJSON["USERSETTINGS"].contains(ContainerParams.PackageUID) &&
        GlobalConfigJSON["USERSETTINGS"][ContainerParams.PackageUID].contains("PREFERRED_RUNNER"))
    {
        PreferredRunner = GlobalConfigJSON["USERSETTINGS"][ContainerParams.PackageUID]["PREFERRED_RUNNER"];
    }
    if (PreferredRunner.empty() && !ContainerParams.subgame_id.empty() && SubgameIdx != -1)
    {
        auto &RecommendedField = MANIFESTJSON["SUBGAMES"][SubgameIdx]["RECOMMENDED_RUNNER"];
        if (!RecommendedField.is_null() && RecommendedField != "")
        {
            PreferredRunner = RecommendedField;
        }
    }

    nlohmann::ordered_json SelectedRunner;
    if (GlobalConfigJSON.contains("RUNNERS") && GlobalConfigJSON["RUNNERS"].contains(ContainerParams.Platform))
    {
        auto &Runners = GlobalConfigJSON["RUNNERS"][ContainerParams.Platform];
        for (auto &Runner : Runners)
        {
            if (!PreferredRunner.empty() && Runner["NAME"] == PreferredRunner)
            {
                SelectedRunner = Runner;
                break;
            }
        }
        if (SelectedRunner.is_null() && !Runners.empty())
        {
            SelectedRunner = Runners[0];
        }
    }

    if (!SelectedRunner.is_null())
    {
        ContainerParams.RunnerName       = SelectedRunner["NAME"];
        ContainerParams.RunnerExecutable = SelectedRunner["EXECUTABLE"];
        std::string RunnerTypeStr        = SelectedRunner["TYPE"];
        if (RunnerTypeStr == "wine")           ContainerParams.RunnerTypeEnum = RunnerType::Wine;
        else if (RunnerTypeStr == "emulator")  ContainerParams.RunnerTypeEnum = RunnerType::Emulator;
        else                                   ContainerParams.RunnerTypeEnum = RunnerType::Native;
        if (SelectedRunner.contains("ENV"))        ContainerParams.RunnerEnv = SelectedRunner["ENV"];
        if (SelectedRunner.contains("REMOVE_ENV")) for (auto &E : SelectedRunner["REMOVE_ENV"]) ContainerParams.RunnerRemoveEnv.push_back(E);
        if (SelectedRunner.contains("ARGS"))       for (auto &A : SelectedRunner["ARGS"])       ContainerParams.RunnerArgs.push_back(A);
    }
    else
    {
        // No runner found — fall back to umu-run/Wine for backwards compatibility
        ContainerParams.RunnerExecutable = "umu-run";
        ContainerParams.RunnerTypeEnum   = RunnerType::Wine;
        std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DeriveContainerParams: " << "[WARN] No runner found for platform '" << ContainerParams.Platform << "', falling back to umu-run." << std::endl;
    }
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DeriveContainerParams: " << "[OUT] Runner: " << ContainerParams.RunnerName << " (" << ContainerParams.RunnerExecutable << ")" << std::endl;

    //System Variables
    ContainerParams.ScreenWidth                         = std::to_string(QGuiApplication::primaryScreen()->geometry().width());
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DeriveContainerParams: " << "[OUT] ScreenWidth: " << ContainerParams.ScreenWidth << std::endl;

    ContainerParams.ScreenHeight                        = std::to_string(QGuiApplication::primaryScreen()->geometry().height());
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DeriveContainerParams: " << "[OUT] ScreenHeight: " << ContainerParams.ScreenHeight << std::endl;

    //Paths
    ContainerParams.RuntimePath                         = ContainerParams.PackagePath / "RUNTIME";
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DeriveContainerParams: " << "[OUT] RuntimePath: " << ContainerParams.RuntimePath << std::endl;

    ContainerParams.MetaDataPath                        = ContainerParams.PackagePath / "METADATA";
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DeriveContainerParams: " << "[OUT] MetaDataPath: " << ContainerParams.MetaDataPath << std::endl;

    ContainerParams.PackageFilesPath                    = ContainerParams.PackagePath / "PACKAGEFILES";
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DeriveContainerParams: " << "[OUT] PackageFilesPath: " << ContainerParams.PackageFilesPath << std::endl;

    ContainerParams.UserDataPath                        = ContainerParams.PackagePath / "USERDATA";
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DeriveContainerParams: " << "[OUT] UserDataPath: " << ContainerParams.UserDataPath << std::endl;

    ContainerParams.TempPath                            = ContainerParams.PackagePath / "TEMP";
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DeriveContainerParams: " << "[OUT] TempPath: " << ContainerParams.TempPath << std::endl;

    // Wine-specific: prefix lives under TEMP/DEFPREFIX, programs under drive_c/PACKAGEUID
    // Other runners: ProgramPath is the root of the RUNTIME mount
    if (ContainerParams.RunnerTypeEnum == RunnerType::Wine)
    {
        ContainerParams.ProgramPath                     = ContainerParams.RuntimePath / "drive_c" / ContainerParams.PackageUID;
        ContainerParams.DefPrefixPath                   = ContainerParams.TempPath / "DEFPREFIX";
        ContainerParams.WindowsProgramPath              = "C:\\" + ContainerParams.PackageUID;
        ContainerParams.WindowsProgramPathDoubleBackSlash = "C:\\\\" + ContainerParams.PackageUID;
    }
    else
    {
        ContainerParams.ProgramPath                     = ContainerParams.RuntimePath;
    }
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DeriveContainerParams: " << "[OUT] ProgramPath: " << ContainerParams.ProgramPath << std::endl;
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DeriveContainerParams: " << "[OUT] DefPrefixPath: " << ContainerParams.DefPrefixPath << std::endl;
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DeriveContainerParams: " << "[OUT] WindowsProgramPath: " << ContainerParams.WindowsProgramPath << std::endl;
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DeriveContainerParams: " << "[OUT] WindowsProgramPathDoubleBackSlash: " << ContainerParams.WindowsProgramPathDoubleBackSlash << std::endl;

    //Subgame-specific paths and args — only valid when a subgame is specified
    if (!ContainerParams.subgame_id.empty() && SubgameIdx != -1)
    {
        ContainerParams.ExePathRelative                 = std::filesystem::path(MANIFESTJSON["SUBGAMES"][SubgameIdx]["EXEPATH"]);
        std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DeriveContainerParams: " << "[OUT] ExePathRelative: " << ContainerParams.ExePathRelative << std::endl;

        ContainerParams.ExePathComplete                 = ContainerParams.ProgramPath / ContainerParams.ExePathRelative;
        std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DeriveContainerParams: " << "[OUT] ExePathComplete: " << ContainerParams.ExePathComplete << std::endl;

        if (ContainerParams.RunnerTypeEnum == RunnerType::Wine)
        {
            ContainerParams.ExePathInPrefix             = std::filesystem::path("C:") / ContainerParams.PackageUID / ContainerParams.ExePathRelative;
            ContainerParams.WindowsExePathComplete      = "C:\\" + ContainerParams.PackageUID + "\\" + ContainerParams.ExePathRelative.string();
            std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DeriveContainerParams: " << "[OUT] ExePathInPrefix: " << ContainerParams.ExePathInPrefix << std::endl;
            std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DeriveContainerParams: " << "[OUT] WindowsExePathComplete: " << ContainerParams.WindowsExePathComplete << std::endl;
        }

        auto &WorkDirField = MANIFESTJSON["SUBGAMES"][SubgameIdx]["WORKDIR"];
        if (!(WorkDirField.empty() || WorkDirField == "" || WorkDirField.is_null()))
        {
            ContainerParams.WorkDirPathRelative         = std::filesystem::path(WorkDirField);
            ContainerParams.WorkDirPathComplete         = ContainerParams.ProgramPath / ContainerParams.WorkDirPathRelative;
        }
        else
        {
            ContainerParams.WorkDirPathComplete         = ContainerParams.ProgramPath;
        }
        std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DeriveContainerParams: " << "[OUT] WorkDirPathComplete: " << ContainerParams.WorkDirPathComplete << std::endl;

        auto &ExeArgsField = MANIFESTJSON["SUBGAMES"][SubgameIdx]["EXEARGS"];
        if (!(ExeArgsField.empty() || ExeArgsField == "" || ExeArgsField.is_null()))
        {
            std::string UnsplitExeArgs(ExeArgsField);
            ContainerWrapper::StringVariableSubstitution(UnsplitExeArgs, ContainerParams.GetVariablesMap());
            ContainerParams.ExeArgs = [](const std::string& s){std::vector<std::string> out;std::istringstream iss(s);for (std::string tok; std::getline(iss, tok, ' ');)out.push_back(tok);return out;}(UnsplitExeArgs);
            for (int i = 0; i < (int)ContainerParams.ExeArgs.size(); i++)
            {
                std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DeriveContainerParams: " << QString("[OUT] ExeArg %1: ").arg(i).toStdString() << ContainerParams.ExeArgs.at(i) << std::endl;
            }
        }
    }
    else
    {
        ContainerParams.WorkDirPathComplete             = ContainerParams.ProgramPath;
    }
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DeriveContainerParams: " << "[OUT] Completed ContainerParams!" << std::endl;
    return true;
}

std::map<std::string, std::string> ContainerParams::GetVariablesMap()
{
    std::map<std::string, std::string> VariablesMap;
    VariablesMap["PackagePath"] = this->PackagePath;
    VariablesMap["PackageName"] = this->PackageName;
    VariablesMap["PackageUID"] = this->PackageUID;
    VariablesMap["GameName"] = this->GameName;
    VariablesMap["UMUID"] = this->UMUID;
    VariablesMap["ScreenWidth"] = this->ScreenWidth;
    VariablesMap["ScreenHeight"] = this->ScreenHeight;
    VariablesMap["RuntimePath"] = this->RuntimePath;
    VariablesMap["MetaDataPath"] = this->MetaDataPath;
    VariablesMap["PackageFilesPath"] = this->PackageFilesPath;
    VariablesMap["UserDataPath"] = this->UserDataPath;
    VariablesMap["TempPath"] = this->TempPath;
    VariablesMap["ProgramPath"] = this->ProgramPath;
    VariablesMap["DefPrefixPath"] = this->DefPrefixPath;
    VariablesMap["ExePathRelative"] = this->ExePathRelative;
    VariablesMap["ExePathComplete"] = this->ExePathComplete;
    VariablesMap["ExePathInPrefix"] = this->ExePathInPrefix;
    VariablesMap["WindowsProgramPath"] = this->WindowsProgramPath;
    VariablesMap["WindowsExePathComplete"] = this->WindowsExePathComplete;
    VariablesMap["WindowsProgramPathDoubleBackSlash"] = this->WindowsProgramPathDoubleBackSlash;
    VariablesMap["WorkDirPathRelative"] = this->WorkDirPathRelative;
    VariablesMap["WorkDirPathComplete"] = this->WorkDirPathComplete;
    return VariablesMap;
}

bool ContainerWrapper::StringVariableSubstitution(
    std::string &SourceString,
    const std::map<std::string, std::string>& VariablesMap)
{
    std::cout << "[OUT] ContainerWrapper::StringVariableSubstitution:"
              << " Starting substitution.\n";
    std::cout << "      Original string: \"" << SourceString << "\"\n";

    std::string result;
    bool replaced = false;
    size_t pos = 0;

    while (pos < SourceString.size())
    {
        size_t start = SourceString.find('%', pos);

        if (start == std::string::npos)
        {
            result += SourceString.substr(pos);
            break;
        }

        size_t end = SourceString.find('%', start + 1);

        if (end == std::string::npos)
        {
            std::cout << "[WARN] Unmatched '%' at position "
                      << start << ". Aborting further substitution.\n";
            result += SourceString.substr(pos);
            break;
        }

        // Append text before variable
        result += SourceString.substr(pos, start - pos);

        std::string key = SourceString.substr(start + 1, end - start - 1);

        std::cout << "[OUT] Found variable: %" << key << "%\n";

        auto it = VariablesMap.find(key);
        if (it != VariablesMap.end())
        {
            std::cout << "      Replacing with: \"" << it->second << "\"\n";
            result += it->second;
            replaced = true;
        }
        else
        {
            std::cout << "      [WARN] Variable not found in map. Leaving unchanged.\n";
            result += "%" + key + "%";
        }

        pos = end + 1;
    }

    std::cout << "[OUT] Final string: \"" << result << "\"\n";
    std::cout << "[OUT] Substitution performed: "
              << (replaced ? "YES" : "NO") << "\n";

    SourceString = std::move(result);
    return replaced;
}

int ContainerWrapper::RunCommand(std::string Program, std::vector<std::string> Arguments, QProcessEnvironment ProcessEnvironment)
{
    auto toQStringList = [](const std::vector<std::string>& v)
    {
        QStringList l; for (auto& s : v) l << QString::fromStdString(s); return l;
    };

    QStringList ArgumentsQList = toQStringList(Arguments);
    std::cout << std::chrono::system_clock::now() << "ContainerWrapper::RunCommand: " << "[OUT] Running program " << Program << " with arguments: " << ArgumentsQList.join(" ").toStdString() << std::endl;

    QProcess Process;
    Process.setProcessEnvironment(ProcessEnvironment);
    Process.setProgram(QString::fromStdString(Program));
    Process.setArguments(ArgumentsQList);

    Process.start();

    if (!Process.waitForFinished(-1))
    {
        return -1; //Failed to start or crashed
    }

    std::cout << Process.readAllStandardError().toStdString();
    std::cout << Process.readAllStandardOutput().toStdString();

    if (Process.exitStatus() == QProcess::NormalExit)
    {
        return Process.exitCode();
    }
    else
    {
        return -1; //Crashed
    }
}

//=====================================================================================================================================================================
//                                                                    REGISTRYWRAPPER CLASS
//=====================================================================================================================================================================

bool ContainerWrapper::CreateFlatRegPatchJSON(struct ContainerParams &ContainerParams)
{
    ContainerParams.FlatRegPatch.clear();
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::CreateFlatRegPatchJSON: " << "[OUT] Creating registry patch." << std::endl;
    for (int i = 0; i < ContainerParams.SubComponentsArray.size(); i++)
    {
        nlohmann::ordered_json SubComponentJSON = ContainerParams.SubComponentsArray[i];
        if (SubComponentJSON["TYPE"] == "RegEdit")
        {
            if (SubComponentJSON.contains("KEYVALUES"))
            {
                for (auto Item : SubComponentJSON["KEYVALUES"].items())
                {
                    if (!SubComponentJSON["KEYVALUES"][Item.key()].is_null())
                    {
                        ContainerParams.FlatRegPatch[std::string(SubComponentJSON["ARCHITECTURE"])][std::string(SubComponentJSON["REGPATH"])][Item.key()] = Item.value();
                    }
                    else
                    {
                        ContainerParams.FlatRegPatch[std::string(SubComponentJSON["ARCHITECTURE"])][std::string(SubComponentJSON["REGPATH"])][Item.key()] = nullptr;
                    }
                }
            }
            else
            {
                ContainerParams.FlatRegPatch[std::string(SubComponentJSON["ARCHITECTURE"])][std::string(SubComponentJSON["REGPATH"])] = nullptr;
            }
        }
    }
    std::cout << std::chrono::system_clock::now() << "ContainerWrapper::CreateFlatRegPatchJSON" << "[OUT] Successfully created FlatRegPatch: " << std::endl << ContainerParams.FlatRegPatch.dump(4) << std::endl;
    return true;
}

bool ContainerWrapper::CreateRegPatchFiles(struct ContainerParams &ContainerParams)
{
    auto normalizeRootKey = [](std::string path) {
        if (path.rfind("HKLM", 0) == 0) path.replace(0, 4, "HKEY_LOCAL_MACHINE");
        else if (path.rfind("HKCU", 0) == 0) path.replace(0, 4, "HKEY_CURRENT_USER");
        return path;
    };

    auto writeArch = [&](const std::string& arch) -> bool
    {
        const std::filesystem::path filePath = ContainerParams.TempPath / "DEFPREFIX" / "drive_c" / ("RegPatch" + arch + ".reg");
        std::filesystem::create_directories(filePath.parent_path());

        std::ofstream out(filePath, std::ios::out | std::ios::trunc);
        if (!out) return false;

        // Header
        out << "Windows Registry Editor Version 5.00\n\n";

        // Iterate all registry paths
        for (const auto& [rawPath, keySet] :
             ContainerParams.FlatRegPatch[arch].items())
        {
            std::string regPath = normalizeRootKey(rawPath);
            out << "[" << regPath << "]\n";

            // If keySet is null, just create the key (no values)
            if (keySet.is_object())
            {
                for (const auto& [key, value] : keySet.items())
                {
                    out << "\"" << key << "\"=";

                    if (value.is_null())
                    {
                        out << "\"\""; // empty string for null
                    }
                    else if (value.is_boolean())
                    {
                        out << "dword:" << (value.get<bool>() ? "00000001" : "00000000");
                    }
                    else if (value.is_number_integer() || value.is_number_unsigned())
                    {
                        out << "dword:"
                            << std::hex << std::setw(8) << std::setfill('0')
                            << value.get<uint32_t>()
                            << std::dec;
                    }
                    else if (value.is_string())
                    {
                        std::string s = value.get<std::string>();

                        // Already a literal (dword: or hex:) → write as-is
                        if (s.rfind("dword:", 0) == 0 || s.rfind("hex:", 0) == 0)
                        {
                            out << s;
                        }
                        else
                        {
                            // Escape backslashes
                            std::string escaped;
                            for (char c : s)
                                escaped += (c == '\\') ? "\\\\" : std::string(1, c);
                            out << "\"" << escaped << "\"";
                        }
                    }

                    out << "\n";
                }
            }

            out << "\n"; // blank line between keys
        }

        return true;
    };

    // Write both 32-bit and 64-bit patches
    return writeArch("32") && writeArch("64");
}

bool ContainerWrapper::MergeRegPatchFiles(struct ContainerParams &ContainerParams)
{
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::MergeRegPatchFiles: " << "[OUT] Merging RegPatchFiles." << std::endl;

    // Prepare environment
    QProcessEnvironment ProcessEnvironment = QProcessEnvironment::systemEnvironment();
    ProcessEnvironment.insert("WINEPREFIX", QString::fromStdString(ContainerParams.DefPrefixPath));
    ProcessEnvironment.insert("GAMEID", "0");
    ProcessEnvironment.remove("LD_LIBRARY_PATH");

    int Merge32Complete = RunCommand(ContainerParams.RunnerExecutable, {"reg", "import", ContainerParams.DefPrefixPath / "drive_c" / "RegPatch32.reg"}, ProcessEnvironment);
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::MergeRegPatchFiles: " << "[OUT] Merged RegPatch32.reg. " << "EXIT CODE: " << Merge32Complete << std::endl;
    int Merge64Complete = RunCommand(ContainerParams.RunnerExecutable, {"reg", "import", ContainerParams.DefPrefixPath / "drive_c" / "RegPatch64.reg"}, ProcessEnvironment);
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::MergeRegPatchFiles: " << "[OUT] Merged RegPatch64.reg. " << "EXIT CODE: " << Merge64Complete << std::endl;
    return true;
}

//=====================================================================================================================================================================
//                                                                          VFSWRAPPER CLASS
//=====================================================================================================================================================================

bool ContainerWrapper::InitializeDefPrefix(struct ContainerParams &ContainerParams)
//Exclusive for wine / proton runners.
//Must be made to work with any runner, not just UMU...
{
    //Initialising UMU prefix.
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeDefPrefix: " << "[OUT] Initialising DefPrefix, path: " << ContainerParams.DefPrefixPath << std::endl;
    std::filesystem::create_directories(ContainerParams.DefPrefixPath);

    QProcessEnvironment RunProcessEnvironment = QProcessEnvironment::systemEnvironment();
    QProcess * RunProcess = new QProcess;
    RunProcess->setProgram(QString::fromStdString(ContainerParams.RunnerExecutable));
    RunProcess->setArguments({"wineboot"});

    //RunProcessEnvironment.insert("PROTONPATH", this->Paths["ProtonPath"]);
    RunProcessEnvironment.insert("WINEPREFIX", QString::fromStdString(ContainerParams.DefPrefixPath));
    RunProcessEnvironment.insert("GAMEID", "0");
    RunProcessEnvironment.insert("PROTON_VERB", "waitforexitandrun");
    RunProcessEnvironment.remove("LD_LIBRARY_PATH");

    RunProcess->setProcessEnvironment(RunProcessEnvironment);
    RunProcess->start();
    RunProcess->waitForFinished(-1);

    std::cout << RunProcess->readAllStandardError().toStdString() << std::endl;
    std::cout << RunProcess->readAllStandardOutput().toStdString() << std::endl;

    if (RunProcess->exitCode() == 0)
    {
        delete RunProcess;
        if (!ContainerWrapper::AddToVFSString(ContainerParams, ContainerParams.DefPrefixPath))
        {
            return false;
        }
        std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeDefPrefix: " << "[OUT] Prefix initialisation successful!" << std::endl;
        return true;
    }
    else
    {
        delete RunProcess;
        std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeDefPrefix: " << "[ERR] Prefix initialisation failed!" << std::endl;
        return false;
    }
}

bool ContainerWrapper::PreMountFilesystemComponents(struct ContainerParams &ContainerParams)
{
    //QMessageBox::warning(nullptr, "Building Runtime", "Processing filesystem Subcomponents.");
    //Mounting filesystem components in TEMP and adding to UnionFSString
    std::cout << std::chrono::system_clock::now() << "ContainerWrapper::PreMountFilesystemComponents: " << "[OUT] Processing filesystem Subcomponents." << std::endl;
    for (int i = 0; i < ContainerParams.SubComponentsArray.size(); i++)
    {
        nlohmann::ordered_json SubComponentJSON = ContainerParams.SubComponentsArray[i];
        if (SubComponentJSON["TYPE"] != "ZipFileLayer" && SubComponentJSON["TYPE"] != "DirLayer")
        {
            continue;
        }

        std::filesystem::path PreMountPath = ContainerParams.TempPath / std::string("[" + std::to_string(i) + "]");
        std::filesystem::create_directories(PreMountPath);
        std::filesystem::path TargetPath = PreMountPath / "drive_c" / ContainerParams.PackageUID;
        if (SubComponentJSON.contains("TARGET") || (!(SubComponentJSON["TARGET"].empty())))
        {
            TargetPath = TargetPath / SubComponentJSON["TARGET"];
        }
        std::filesystem::create_directories(TargetPath);

        std::filesystem::path SourcePath = ContainerParams.PackageFilesPath / SubComponentJSON["PATH"];
        if (SubComponentJSON["TYPE"] == "ZipFileLayer")
        {
            std::cout << std::chrono::system_clock::now() << "ContainerWrappNewPather::PreMountFilesystemComponents: " << "[OUT] Mounting ZipFileLayer " << SourcePath.string() << " at " << TargetPath.string() << std::endl;
            if (!ContainerWrapper::RunCommand("fuse-zip", {"-r", SourcePath, TargetPath}))
            {
                ContainerParams.CleanupUnmountPaths.push_back(TargetPath);
                if (!ContainerWrapper::AddToVFSString(ContainerParams, PreMountPath))
                {
                    return false;
                }
            }
            else
            {
                std::cout << std::chrono::system_clock::now() << "ContainerWrappNewPather::PreMountFilesystemComponents: " << "[ERR] FAILED TO MOUNT ZipFileLayer " << SourcePath.string() << std::endl;
            }
        }
        else if (SubComponentJSON["TYPE"] == "DirLayer")
        {
            std::cout << std::chrono::system_clock::now() << "ContainerWrapper::PreMountFilesystemComponents: " << "[OUT] Mounting DirLayer " << SourcePath.string() << " at " << TargetPath.string() << std::endl;
            if (!ContainerWrapper::RunCommand("bindfs", {"-r", SourcePath, TargetPath}))
            {
                ContainerParams.CleanupUnmountPaths.push_back(TargetPath);
                if (!ContainerWrapper::AddToVFSString(ContainerParams, PreMountPath))
                {
                    return false;
                }
            }
            else
            {
                std::cout << std::chrono::system_clock::now() << "ContainerWrappNewPather::PreMountFilesystemComponents: " << "[ERR] FAILED TO MOUNT DirLayer " << SourcePath.string() << std::endl;
            }
        }
    }
    std::cout << std::chrono::system_clock::now() << "ContainerWrapper::PreMountFilesystemComponents: " << "[OUT] Filesystem subcomponent pre-mount complete" << std::endl;
    return true;
}

bool ContainerWrapper::AddToVFSString(struct ContainerParams &ContainerParams, std::string NewPath)
{
    if (ContainerParams.VFSString.empty())
    {
        ContainerParams.VFSString = NewPath + "=RO";
        std::cout << std::chrono::system_clock::now() << "ContainerWrapper::AddToVFSString:" << "[OUT] Initialized VFSString with " << NewPath << std::endl;
        return true;
    }
    else
    {
        ContainerParams.VFSString = NewPath + "=RO:" + ContainerParams.VFSString;
        std::cout << std::chrono::system_clock::now() << "ContainerWrapper::AddToVFSString:" << "[OUT] Added " << NewPath << " to VFSString" << std::endl;
        return true;
    }
}

bool ContainerWrapper::FinalizeVFSString(struct ContainerParams &ContainerParams)
{
    if (ContainerParams.ReadOnlyVFS)
    {
        std::cout << QTime::currentTime().toString().toStdString() << "ContainerWrapper::FinalizeVFSString: " << "[OUT] UnionFSString:" << "RUNTIME IS READONLY!" << std::endl;
    }
    else
    {
        if (ContainerParams.VFSString.empty())
        {
            std::cout << std::chrono::system_clock::now() << "ContainerWrapper::FinalizeVFSString: " << "[ERR] VFSString is null or empty..." << std::endl;
            return false;
        }
        else
        {
            std::filesystem::create_directories(ContainerParams.UserDataPath);
            ContainerParams.VFSString = ContainerParams.UserDataPath.string() + "=RW:" + ContainerParams.VFSString;
            std::cout << std::chrono::system_clock::now() << "ContainerWrapper::FinalizeVFSString: " << "[OUT] Finalized VFSString. Final VFSSTRING:" << std::endl << ContainerParams.VFSString << std::endl;
            return true;
        }
    }
    return true;
}


bool ContainerWrapper::MountVFS(struct ContainerParams &ContainerParams)
{
    if (ContainerParams.VFSString.empty())
    {
        std::cout << std::chrono::system_clock::now() << "ContainerWrapper::MountVFS: " << "[ERR] VFSString is null or empty..." << std::endl;
        return false;
    }
    std::cout << std::chrono::system_clock::now() << "ContainerWrapper::MountVFS: " << "[OUT] Proceeding with VFS mount....." << std::endl;
    std::filesystem::create_directories(ContainerParams.RuntimePath);
    int result = ContainerWrapper::RunCommand("unionfs", {"-o", "cow", "-o", "uid=1000", ContainerParams.VFSString, ContainerParams.RuntimePath});
    std::cout << std::chrono::system_clock::now() << "ContainerWrapper::MountVFS: " << "[OUT] EXIT CODE: " << result << std::endl;

    if (!result)
    {
        //Success
        ContainerParams.CleanupUnmountPaths.push_back(ContainerParams.RuntimePath);
        std::cout << std::chrono::system_clock::now() << "ContainerWrapper::MountVFS: " << "[OUT] Successfully mounted VFS." << std::endl;
        return true;
    }
    else
    {
        //Failiure
        std::cout << std::chrono::system_clock::now() << "ContainerWrapper::MountVFS: " << "[ERR] Failed to mount VFS." << std::endl;
        return false;
    }
}

bool ContainerWrapper::CheckCaseConflicts(std::filesystem::path DirectoryPath)
{
    std::unordered_set<std::string> FilePathList;
    std::unordered_set<std::string> CaseConflictList;
    bool NoConflict = true;
    for (const auto& FilePath : std::filesystem::recursive_directory_iterator(DirectoryPath))
    {
        std::string FilePathLowercase = FilePath.path().string();
        std::transform(FilePathLowercase.begin(), FilePathLowercase.end(),FilePathLowercase.begin(),[](unsigned char c){ return std::tolower(c); });

        if (FilePathList.find(FilePathLowercase) != FilePathList.end())
        {
            NoConflict = false;
            CaseConflictList.insert(FilePathLowercase);
        }
        else
        {
            FilePathList.insert(FilePathLowercase);
        }
    }

    if(!NoConflict)
    {
        std::ostringstream oss;
        std::for_each(CaseConflictList.begin(), CaseConflictList.end(),[&oss](const std::string& s){ oss << s << '\n'; });
        std::cerr << "CASE CONFLICTS:\n" << oss.str() << std::endl;
        QMessageBox::warning(nullptr, "CASE CONFLICTS!", QString::fromStdString(oss.str()));
        return false;
    }
    return true;
}


bool ContainerWrapper::ProcessDLLOverrides(struct ContainerParams &ContainerParams)
{
    std::cout << std::chrono::system_clock::now() << "ContainerWrapper::ProcessDLLOverrides: " << "[OUT] Processing DLL Overrides." << std::endl;
    for (int i = 0; i < ContainerParams.SubComponentsArray.size(); i++)
    {
        nlohmann::ordered_json SubComponentJSON = ContainerParams.SubComponentsArray[i];
        if (SubComponentJSON["TYPE"] == "DllOverride")
        {
            if(!SubComponentJSON["DLLOVERRIDE"].is_null())
            {
                ContainerParams.DLLOverrides.push_back(SubComponentJSON["DLLOVERRIDE"]);
            }
            else
            {
                return false;
            }
        }
    }
    return true;
}

bool ContainerWrapper::ProcessFileEdits(struct ContainerParams &ContainerParams)
{
    //MUST BE RUN AFTER VARIABLE SUBSTITUTION!
    std::cout << std::chrono::system_clock::now() << "ContainerWrapper::PreMountFilesystemComponents: " << "[OUT] Processing FileEdit Subcomponents." << std::endl;
    for (int i = 0; i < ContainerParams.SubComponentsArray.size(); i++)
    {
        nlohmann::ordered_json SubComponentJSON = ContainerParams.SubComponentsArray[i];
        if (SubComponentJSON["TYPE"] == "FileEdit")
        {
            if (SubComponentJSON["MODE"] == "ConfigWrite")
            {
                ContainerWrapper::ConfigWrite(SubComponentJSON["KEY"], SubComponentJSON["VALUE"], SubComponentJSON["FILE"]);
            }
        }
    }
    return true;
}

bool ContainerWrapper::ConfigWrite(std::string Key, std::string Value, std::filesystem::path FilePath)
{
    std::cout << std::chrono::system_clock::now() << "ContainerWrapper::ConfigWrite: " << "FilePath: " << FilePath << " Key: " << Key << " Value: " << Value << std::endl;
    std::ifstream inFile(FilePath);
    if (!inFile.is_open())
    {
        std::cout << std::chrono::system_clock::now() << "ContainerWrapper::ConfigWrite: " << "[ERR] Could not open file for reading: " << FilePath << std::endl;
        return false;
    }

    std::vector<std::string> lines;
    std::string line;

    while (std::getline(inFile, line))
    {
        lines.push_back(line);
    }

    inFile.close();

    std::ofstream outFile(FilePath, std::ios::trunc);
    if (!outFile.is_open())
    {
        std::cout << std::chrono::system_clock::now() << "ContainerWrapper::ConfigWrite: " << "[ERR] Could not open file for writing: " << FilePath << std::endl;
        return false;
    }

    for (auto& currentLine : lines)
    {
        if (currentLine.length() >= Key.length() && currentLine.compare(0, Key.length(), Key) == 0)
        {
            currentLine = Key + Value;
            outFile << currentLine << '\n';
        }
        else
        {
            outFile << currentLine << '\n';
        }
    }

    outFile.close();
    return true;
}



//=====================================================================================================================================================================
//                                                                          RUNNER CLASS
//=====================================================================================================================================================================

bool ContainerWrapper::Execute(std::string OverrideExe)
{
    std::string FinalExe;
    if (!OverrideExe.empty())
    {
        FinalExe = OverrideExe;
    }
    else if (ContainerParams.RunnerTypeEnum == RunnerType::Wine)
    {
        FinalExe = ContainerParams.ExePathInPrefix.string();
    }
    else
    {
        FinalExe = ContainerParams.ExePathComplete.string();
    }

    if (FinalExe.empty())
    {
        std::cout << std::chrono::system_clock::now() << " ContainerWrapper::Execute: " << "[ERR] No exe to run (no subgame and no override). Aborting." << std::endl;
        return false;
    }

    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::Execute: " << "[OUT] Runner: " << ContainerParams.RunnerExecutable << std::endl;
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::Execute: " << "[OUT] Executing: " << FinalExe << std::endl;
    std::cout << QTime::currentTime().toString().toStdString() << " ContainerWrapper::Execute: " << "[OUT] WorkDirPath: " << ContainerParams.WorkDirPathComplete << std::endl;

    QProcessEnvironment RunProcessEnvironment = QProcessEnvironment::systemEnvironment();

    // Apply runner-level env removes first
    for (const std::string &Key : ContainerParams.RunnerRemoveEnv)
    {
        RunProcessEnvironment.remove(QString::fromStdString(Key));
    }

    // Apply runner ENV with template substitution
    for (auto &[Key, Value] : ContainerParams.RunnerEnv.items())
    {
        std::string ExpandedValue = Value.get<std::string>();
        ContainerWrapper::StringVariableSubstitution(ExpandedValue, ContainerParams.GetVariablesMap());
        RunProcessEnvironment.insert(QString::fromStdString(Key), QString::fromStdString(ExpandedValue));
    }

    // Wine-specific: WINEDLLOVERRIDES
    if (ContainerParams.RunnerTypeEnum == RunnerType::Wine && !ContainerParams.DLLOverrides.empty())
    {
        RunProcessEnvironment.insert("WINEDLLOVERRIDES", QString::fromStdString(std::accumulate(ContainerParams.DLLOverrides.begin(), ContainerParams.DLLOverrides.end(), std::string{}, [](auto a, auto b){ return a+b;})));
    }

    std::filesystem::path FinalWorkDir = ContainerParams.WorkDirPathComplete;
    if (!std::filesystem::exists(FinalWorkDir))
    {
        std::cout << std::chrono::system_clock::now() << " ContainerWrapper::Execute: " << "[OUT] WorkDirPath does not exist, falling back to RuntimePath." << std::endl;
        FinalWorkDir = ContainerParams.RuntimePath;
    }

    QProcess RunProcess;
    RunProcess.setProgram(QString::fromStdString(ContainerParams.RunnerExecutable));
    RunProcess.setWorkingDirectory(QString::fromStdString(FinalWorkDir));

    // For emulator/native runners: prepend RunnerArgs (template-expanded), then exe
    // For wine runners: exe first, then ExeArgs
    QStringList Arguments;
    if (ContainerParams.RunnerTypeEnum != RunnerType::Wine)
    {
        for (std::string Arg : ContainerParams.RunnerArgs)
        {
            ContainerWrapper::StringVariableSubstitution(Arg, ContainerParams.GetVariablesMap());
            Arguments.append(QString::fromStdString(Arg));
        }
        if (!FinalExe.empty())
        {
            Arguments.append(QString::fromStdString(FinalExe));
        }
    }
    else
    {
        Arguments.append(QString::fromStdString(FinalExe));
        if (OverrideExe.empty() && !ContainerParams.ExeArgs.empty())
        {
            for (std::string Arg : ContainerParams.ExeArgs)
            {
                Arguments.append(QString::fromStdString(Arg));
            }
        }
    }
    RunProcess.setArguments(Arguments);
    RunProcess.setProcessEnvironment(RunProcessEnvironment);
    RunProcess.start();
    RunProcess.waitForFinished(-1);
    std::cout << RunProcess.readAllStandardError().toStdString() << std::endl;
    std::cout << RunProcess.readAllStandardOutput().toStdString() << std::endl;

    if(RunProcess.exitCode() == 0)
    {
        return true;
    }
    else
    {
        return false;
    }
}

bool ContainerWrapper::Cleanup()
{
    for (std::filesystem::path UnmountPath : ContainerParams.CleanupUnmountPaths)
    {
        ContainerWrapper::RunCommand("fusermount", {"-uz", UnmountPath});
    }
    std::filesystem::remove_all(this->ContainerParams.RuntimePath);
    std::filesystem::remove_all(this->ContainerParams.TempPath);
    return true;
}

