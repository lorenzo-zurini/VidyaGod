#include "containerwrapper.h"

ContainerWrapper::ContainerWrapper(nlohmann::ordered_json Passed_GlobalConfigJSON, nlohmann::ordered_json Passed_MANIFESTJSON, struct ContainerParams Passed_ContainerParams)
    : GlobalConfigJSON(Passed_GlobalConfigJSON), MANIFESTJSON(Passed_MANIFESTJSON), ContainerParams(Passed_ContainerParams)
{
    this->InitializeContainer();
}

ContainerParams::ContainerParams(std::filesystem::path Passed_PackagePath, int Passed_subgame, int Passed_component)
    : PackagePath(Passed_PackagePath), subgame(Passed_subgame), component(Passed_component)
{
    std::cout << std::chrono::system_clock::now() << " ContainerParams::ContainerParams: " << "[OUT] ContainerParams object created..." << std::endl;
}

bool ContainerWrapper::InitializeContainer()
{
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeContainer: " << "[OUT] Initializing container..." << std::endl;
    if(!this->DecideComponent(this->MANIFESTJSON, this->ContainerParams))
    {
        std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeContainer: " << "[ERR] ContainerWrapper::DecideComponent failed, aborting...." << std::endl;
        return false;
    }
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeContainer: " << "[OUT] ContainerWrapper::DecideComponent successful." << std::endl;

    if(!this->InitializeContainerParams(this->MANIFESTJSON, this->ContainerParams))
    {
        std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeContainer: " << "[ERR] ContainerWrapper::InitializeContainerParams failed, aborting...." << std::endl;
        return false;
    }
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeContainer: " << "[OUT] ContainerWrapper::InitializeContainerParams successful." << std::endl;

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

    if(!this->CreateContainerVariablesJSON(this->ContainerParams))
    {
        std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeContainer: " << "[ERR] ContainerWrapper::CreateContainerVariablesJSON failed, aborting...." << std::endl;
        return false;
    }
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeContainer: " << "[OUT] ContainerWrapper::CreateContainerVariablesJSON successful." << std::endl;


    if(!this->VariableSubstitution(this->ContainerParams))
    {
        std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeContainer: " << "[ERR] ContainerWrapper::VariableSubstitution failed, aborting...." << std::endl;
        return false;
    }
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeContainer: " << "[OUT] ContainerWrapper::VariableSubstitution successful." << std::endl;
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeContainerParams: " << "[OUT] Container initialisation complete!" << std::endl;
    return true;
}

bool ContainerWrapper::BuildContainerRuntime()
{
    this->CreateDirectories(this->ContainerParams.ContainerVariablesJSON);
    this->BuildVirtualFilesystem();
    return true;
}

bool ContainerWrapper::BuildVirtualFilesystem()
{
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::BuildVirtualFilesystem: " << "[OUT] Building virtual filesystem." << std::endl;
    this->InitializeDefPrefix(this->ContainerParams);
    this->CreateFlatRegPatchJSON(this->ContainerParams);
    this->CreateRegPatchFiles(this->ContainerParams);
    return true;
}

bool ContainerWrapper::DecideComponent(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams)
{
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DecideComponent: " << "[OUT] Deciding component: subgame: " << ContainerParams.subgame << " component: " << ContainerParams.component << std::endl;
    if ((ContainerParams.subgame == 0) && (ContainerParams.component == 0))
    {
        std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DecideComponent: " << "[OUT] Neither subgame nor component specified, mounting defprefix." << ContainerParams.component << std::endl;
        return true;
    }
    else if ((ContainerParams.subgame != 0) && (ContainerParams.component == 0))
    {
        if (!MANIFESTJSON["SUBGAMES"][ContainerParams.subgame - 1]["COMPONENT"].is_null())
        {
            ContainerParams.component = QString::fromStdString(MANIFESTJSON["SUBGAMES"][ContainerParams.subgame - 1]["COMPONENT"]).toInt();
            std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DecideComponent: " << "[OUT] Only subgame specified, getting component from JSON: " << ContainerParams.component << std::endl;
        }
        std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DecideComponent: " << "[OUT] Running subgame " << MANIFESTJSON["SUBGAMES"][ContainerParams.subgame - 1]["TITLE"] << std::endl;
        return true;
    }
    else if ((ContainerParams.subgame == 0) && (ContainerParams.component != 0))
    {
        std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DecideComponent: " << "[OUT] Running component " << MANIFESTJSON["COMPONENTS"][ContainerParams.component - 1]["NAME"] << std::endl;
        return true;
    }
    else
    {
        std::cout << std::chrono::system_clock::now() << " ContainerWrapper::DecideComponent: " << "[OUT] Running subgame " << ContainerParams.subgame << " component " << ContainerParams.component << std::endl;
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
        std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeContainerParams: " << "[OUT] WORKDIR: " << ContainerParams.WorkDirPath << std::endl;
    }
    else
    {
        ContainerParams.WorkDirPath = ContainerParams.PackagePath / "drive_c" / ContainerParams.PackageUID;
        std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeContainerParams: " << "[OUT] WORKDIR: " << ContainerParams.WorkDirPath << std::endl;
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
            std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeContainerParams: " << QString("[OUT] ExeArg %1: ").arg(i).toStdString() << ContainerParams.ExeArgs.at(i) << std::endl;
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
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::CreateRecipe: " << "[OUT] Recipe: " << [&](const std::vector<int>& v){ std::string r; for (int x : v) r += std::to_string(x) + " "; return r;}(ContainerParams.Recipe) << std::endl;
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
                std::cout << std::chrono::system_clock::now() << " ContainerWrapper::BuildSubComponentsArray: " << "[OUT] Added COMPONENT " << i + 1 << " SUBCOMPONENT " << j + 1 << std::endl;
            }
        }
    }
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::BuildSubComponentsArray: " << "[OUT] Completed SubComponentsArray:"<< std::endl << ContainerParams.SubComponentsArray.dump(4) << std::endl;
    return true;
}

bool ContainerWrapper::CreateContainerVariablesJSON(struct ContainerParams &ContainerParams)
{
    //System Variables
    ContainerParams.ContainerVariablesJSON["SystemVariables"]["ScreenWidth"]                       = QString::number(QGuiApplication::primaryScreen()->geometry().width()).toStdString();
    ContainerParams.ContainerVariablesJSON["SystemVariables"]["ScreenHeight"]                      = QString::number(QGuiApplication::primaryScreen()->geometry().height()).toStdString();

    //Paths
    ContainerParams.ContainerVariablesJSON["ContainerPaths"]["PackagePath"]                        = ContainerParams.PackagePath;
    ContainerParams.ContainerVariablesJSON["ContainerPaths"]["RuntimePath"]                        = ContainerParams.PackagePath / "RUNTIME";
    ContainerParams.ContainerVariablesJSON["ContainerPaths"]["MetaDataPath"]                       = ContainerParams.PackagePath / "METADATA";
    ContainerParams.ContainerVariablesJSON["ContainerPaths"]["PackageFilesPath"]                   = ContainerParams.PackagePath / "PACKAGEFILES";
    ContainerParams.ContainerVariablesJSON["ContainerPaths"]["UserDataPath"]                       = ContainerParams.PackagePath / "USERDATA";
    ContainerParams.ContainerVariablesJSON["ContainerPaths"]["TempPath"]                           = ContainerParams.PackagePath / "TEMP";

    //Wine / Proton / Windows specific paths, must be generalised somehow.
    //This is a temporary solution, those path formulas will be encoded in the runner deficnition for windows platform runners
    ContainerParams.ContainerVariablesJSON["RunnerPaths"]["ProgramPath"]                            = ContainerParams.PackagePath / "RUNTIME" / "drive_c"/ ContainerParams.PackageUID;
    ContainerParams.ContainerVariablesJSON["RunnerPaths"]["DefPrefixPath"]                          = ContainerParams.PackagePath / "TEMP" / "DEFPREFIX";
    ContainerParams.ContainerVariablesJSON["RunnerPaths"]["ExePathRelative"]                        = ContainerParams.ExePath;
    ContainerParams.ContainerVariablesJSON["RunnerPaths"]["ExePathComplete"]                        = ContainerParams.PackagePath / "RUNTIME" / "drive_c" / ContainerParams.PackageUID / ContainerParams.ExePath;

    //Windows Paths
    ContainerParams.ContainerVariablesJSON["WindowsPaths"]["WindowsProgramPath"]                   = "C:\\" + ContainerParams.PackageUID;
    ContainerParams.ContainerVariablesJSON["WindowsPaths"]["WindowsExePathCompete"]                = "C:\\" + ContainerParams.PackageUID + "\\" + ContainerParams.ExePath;
    ContainerParams.ContainerVariablesJSON["WindowsPaths"]["WindowsProgramPathDoubleBackSlash"]    = "C:\\\\" + ContainerParams.PackageUID;

    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::CreateContainerVariablesJSON: " << "[OUT] Completed ContainerVariablesJSON:" << std::endl << ContainerParams.ContainerVariablesJSON.dump(4) << std::endl;
    return true;
}

bool ContainerWrapper::VariableSubstitution(struct ContainerParams &ContainerParams)
{
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::VariableSubstitution: " << "[OUT] Performing variable substitution on SubComponentsArray." << std::endl;
    std::string SubComponentsArrayString = ContainerParams.SubComponentsArray.dump();

    for (const auto& [Category, Item] : ContainerParams.ContainerVariablesJSON.items())
    {
        for (const auto& [Key, Value] : Item.items())
        {
            SubComponentsArrayString = std::regex_replace(SubComponentsArrayString, std::regex("%" + Key + "%"), std::string(Value));
        }
    }
    ContainerParams.SubComponentsArray = nlohmann::ordered_json::parse(SubComponentsArrayString);
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::VariableSubstitution: " << "[OUT] Performed variable substitution on SubComponentsArray. Result: " << std::endl << ContainerParams.SubComponentsArray.dump(4) << std::endl;

    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::VariableSubstitution: " << "[OUT] Performing variable substitution on ExeArgs." << std::endl;
    for (const auto& [Category, Item] : ContainerParams.ContainerVariablesJSON.items())
    {
        for (const auto& [Key, Value] : Item.items())
        {
            for (auto& Token : ContainerParams.ExeArgs)
            {
                Token = std::regex_replace(Token, std::regex("%" + Key + "%"), std::string(Value));
            }
        }
    }
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::VariableSubstitution: " << "[OUT] Performed variable substitution on ExeArgs. Result: " << [](const auto& args){ std::string s; for(auto& a:args) s += a + " "; if(!s.empty()) s.pop_back(); return s;}(ContainerParams.ExeArgs) << std::endl;
    return true;
}

bool ContainerWrapper::CreateDirectories(const nlohmann::ordered_json ContainerVariablesJSON)
{
    // Creating all necessary directories based on the Paths QMap.
    // QMessageBox::warning(nullptr, "Building Runtime", "Creating directories.");
    for (const auto& [Key, Value] : ContainerVariablesJSON["ContainerPaths"].items())
    {
        if (std::filesystem::exists(Value) && std::filesystem::is_directory(Value))
        {
            std::cout << std::chrono::system_clock::now() << " ContainerWrapper::CreateDirectories: " << "[OUT] Directory: " << Value << " already exists, skipping..." << std::endl;
            continue;
        }

        std::cout << std::chrono::system_clock::now() << " ContainerWrapper::CreateDirectories: " << "[OUT] Attempting to create directory: " << Key << " PATH: " << Value << std::endl;
        if (std::filesystem::create_directories(Value))
        {
            std::cout << std::chrono::system_clock::now() << " ContainerWrapper::CreateDirectories: " << "[OUT] Created directory: " << Key << " PATH: " << Value << std::endl;
        }
        else
        {
            std::cout << std::chrono::system_clock::now() << " ContainerWrapper::CreateDirectories: " << "[ERR] Could not create directory " << Key << " PATH: " << Value << std::endl;
            return false;
        }
    }
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::CreateDirectories: " << "[OUT] Directory creation complete." << std::endl;
    return true;
}

bool ContainerWrapper::InitializeDefPrefix(struct ContainerParams &ContainerParams)
//Exclusive for wine / proton runners.
//Must be made to work with any runner, not just UMU...
{
    //Initialising UMU prefix.
    std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeDefPrefix: " << "[OUT] Initialising DefPrefix, path: " << ContainerParams.ContainerVariablesJSON["RunnerPaths"]["DefPrefixPath"] << std::endl;

    QProcessEnvironment RunProcessEnvironment = QProcessEnvironment::systemEnvironment();
    QProcess * RunProcess = new QProcess;
    RunProcess->setProgram("umu-run");
    RunProcess->setArguments({"wineboot"});

    //RunProcessEnvironment.insert("PROTONPATH", this->Paths["ProtonPath"]);
    RunProcessEnvironment.insert("WINEPREFIX", QString::fromStdString(ContainerParams.ContainerVariablesJSON["RunnerPaths"]["DefPrefixPath"]));
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
        if (ContainerParams.ContainerVariablesJSON["VFSString"].is_null() or ContainerParams.ContainerVariablesJSON["VFSString"].empty())
        {
            ContainerParams.ContainerVariablesJSON["VFSString"] = std::string(ContainerParams.ContainerVariablesJSON["RunnerPaths"]["DefPrefixPath"]) + "=RO";
        }
        else if (ContainerParams.ContainerVariablesJSON["VFSString"].is_string())
        {
            ContainerParams.ContainerVariablesJSON["VFSString"] = std::string(ContainerParams.ContainerVariablesJSON["RunnerPaths"]["DefPrefixPath"]) + "=RO" + std::string(ContainerParams.ContainerVariablesJSON["VFSString"]);
        }
        else
        {
            std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeDefPrefix: " << "[OUT] Prefix initialisation successful!" << std::endl;
            return false;
        }
        return true;
    }
    else
    {
        delete RunProcess;
        std::cout << std::chrono::system_clock::now() << " ContainerWrapper::InitializeDefPrefix: " << "[ERR] Prefix initialisation failed!" << std::endl;
        return false;
    }
}

bool ContainerWrapper::CreateFlatRegPatchJSON(struct ContainerParams &ContainerParams)
{
    ContainerParams.ContainerVariablesJSON["FlatRegPatch"].clear();
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
                        ContainerParams.ContainerVariablesJSON["FlatRegPatch"][std::string(SubComponentJSON["ARCHITECTURE"])][std::string(SubComponentJSON["REGPATH"])][Item.key()] = Item.value();
                    }
                    else
                    {
                        ContainerParams.ContainerVariablesJSON["FlatRegPatch"][std::string(SubComponentJSON["ARCHITECTURE"])][std::string(SubComponentJSON["REGPATH"])][Item.key()] = nullptr;
                    }
                }
            }
            else
            {
                ContainerParams.ContainerVariablesJSON["FlatRegPatch"][std::string(SubComponentJSON["ARCHITECTURE"])][std::string(SubComponentJSON["REGPATH"])] = nullptr;
            }
        }
    }
    std::cout << std::chrono::system_clock::now() << "ContainerWrapper::CreateFlatRegPatchJSON" << "[OUT] Successfully created FlatRegPatch: " << std::endl << ContainerParams.ContainerVariablesJSON["FlatRegPatch"].dump(4) << std::endl;
    return true;
}

bool ContainerWrapper::CreateRegPatchFiles(struct ContainerParams &ContainerParams)
{
    //ADD NULL CHECKS AT ALL STEPS!!!
    std::ofstream RegPatch32(std::filesystem::path(ContainerParams.ContainerVariablesJSON["ContainerPaths"]["TempPath"]) / "RegPatch32.reg");
    RegPatch32 << "Windows Registry Editor Version 5.00" << std::endl << std::endl << std::endl;
    for (const auto& [RegPath, KeyValuePairSet] : ContainerParams.ContainerVariablesJSON["FlatRegPatch"]["32"].items())
    {
        RegPatch32 << "[" << RegPath << "]" << std::endl;
        for (const auto& [Key, Value] : KeyValuePairSet.items())
        {
            RegPatch32 << Key << "=" << Value << std::endl;
        }
        RegPatch32 << std::endl;
    }
    RegPatch32.close();




    std::ofstream RegPatch64(std::filesystem::path(ContainerParams.ContainerVariablesJSON["ContainerPaths"]["TempPath"]) / "RegPatch64.reg");
    return true;
}


/*
[Software\\Ciocoferoni] 1767816595
#time=1dc8011971bd90e
"CACAMAKOKO"=dword:00001337
"CIOCOFERONN"=dword:00000000
"MOKOBEZOKO"=dword:00001488
"TESTOFERON"=dword:00000001
*/


/*
{
    std::cout << QTime::currentTime().toString().toStdString() << "Runner:" << "[OUT] Executing registry subcomponents" << std::endl;

    QProcessEnvironment RegAddProcessEnvironment = QProcessEnvironment::systemEnvironment();
    RegAddProcessEnvironment.insert("WINEPREFIX", this->Paths["DefPrefixPath"]);
    RegAddProcessEnvironment.insert("GAMEID", "0");
    RegAddProcessEnvironment.remove("LD_LIBRARY_PATH");


    QProcess * RegAddProcess = new QProcess;
    RegAddProcess->setProcessEnvironment(RegAddProcessEnvironment);
    RegAddProcess->setProgram("umu-run");



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
*/

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







/*
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
}*/
