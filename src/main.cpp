#include "main.h"
#include "commonutils.h"

int main(int argc, char *argv[])
{
    //Parse command line arguments and initialize RuntimeParameters struct.
    LaunchParameters LaunchParameters = ParseCommandLineArguments(argc, argv);
    LogOut("main.cpp", "Running VidyaGod in " + LaunchParameters.CurrentPath.string());
    LogOut("main.cpp", "Headless PackagePath: " + LaunchParameters.HeadlessPackagePath.string());
    LogOut("main.cpp", "Headless SubgameID: " + LaunchParameters.HeadlessSubgameID);
    LogOut("main.cpp", "Headless ComponentID: " + LaunchParameters.HeadlessComponentID);

    //Check if dependencies exist in the system
    CheckExecutableDependencies();

    if (!LaunchParameters.HasHeadlessPackagePath)
    {
        //Detect ./METADATA/MANIFEST.json to detemine if running in packagedir.
        //Start in GUI-less mode if so.
        LogOut("main.cpp", "Checking if running in a PACKAGEDIR.");
        if(IsRunningInPackageDir(LaunchParameters.CurrentPath))
        {
            LaunchParameters.HasHeadlessPackagePath = true;
            LaunchParameters.RunningInPackageDir = true;
            LaunchParameters.HeadlessPackagePath = LaunchParameters.CurrentPath;
            LaunchParameters.RunningHeadless = true;
        }
    }

    //TO-DO: Add support for portable mode.
    //Find and create AppDataDir.
    QDir AppDataDir(QDir::homePath() +  "/.VidyaGod");
    AppDataDir.mkpath(".");

    //Initialization of GlobalConfigJSON, the central data structure of the program.
    nlohmann::ordered_json GlobalConfigJSON;// = new nlohmann::ordered_json();
    if (InitializeGlobalConfigJSON(&GlobalConfigJSON, &AppDataDir))
    {
        LogErr("main.cpp", "Fatal error. GlobalConfigJSON initialization failed, aborting.");
        return 1;
    }

    //HEADLESS MODE:
    if(LaunchParameters.RunningHeadless)
    {
        LogOut("main.cpp", "Running package " + LaunchParameters.HeadlessPackagePath.string() + " in HEADLESS mode.");
        nlohmann::ordered_json MANIFESTJSON;// = new nlohmann::ordered_json;
        //CLEAN THIS CRAP BELOW
        if (JSONOps::LoadJSON(new QFile(QDir::cleanPath(QString::fromStdString(LaunchParameters.HeadlessPackagePath) + QDir::separator() + "METADATA" + QDir::separator() + "MANIFEST.json")), &MANIFESTJSON))
        {
            LogErr("main.cpp", "Fatal error. Paring MANIFESTJSON failed, aborting.");
        }


        struct ContainerParams NewContainerParams = ContainerParams(LaunchParameters.HeadlessPackagePath, LaunchParameters.HeadlessSubgameID, LaunchParameters.HeadlessComponentID);
        class ContainerWrapper NewContainerWrapper = ContainerWrapper(GlobalConfigJSON, MANIFESTJSON, NewContainerParams);
        NewContainerWrapper.BuildContainerRuntime();
        NewContainerWrapper.Execute();
        return 0;
    }


    //Initializing GUI. MAKE SURE THERE ARE NOT QT GUI-RELATED CALLS ABOVE THIS LINE OR THE PROGRAM WILL CRASH.
    //---------------------------------------------------------------------------------------------------------
    QApplication Application(argc, argv);
    // Force a consistent Qt style
    Application.setStyle(QStyleFactory::create("Fusion"));

    // Optional: set a default font to ensure consistent spacing
    Application.setFont(QFont("DejaVu Sans", 10));

    //Create and launch MainWindow.
    MainWindow MainWindow(&GlobalConfigJSON, &AppDataDir);
    MainWindow.show();
    return Application.exec();
}

bool CheckExecutableDependencies()
{
    std::list<std::string> ExecutableDependencies = {"unionfs", "fuse-zip", "fusermount", "umu-run", "bindfs"};
    std::string MissingDependencies;
    bool error = 0;
    for (const std::string& Binary : ExecutableDependencies) {
        std::string Command = "which " + Binary + " > /dev/null 2>&1";
        if (std::system(Command.c_str()) == 0)
        {
            LogOut("main.cpp", Binary + " found on the system.");
        }
        else
        {
            LogErr("main.cpp", Binary + " NOT FOUND ON THE SYSTEM, VFS WILL NOT WORK.");
            MissingDependencies = MissingDependencies + Binary + " \n";
            error = 1;
        }
    }

    if (error)
    {
        //QMessageBox::warning(nullptr, "Missing dependencies", QString::fromStdString("The following required dependencies have not been detected on your system:\n\n" +
        //                                                                             MissingDependencies + "\nPlease install them to ensure full program functionality!"));
        return true;
    }

    return false;
}

bool InitializeGlobalConfigJSON(nlohmann::ordered_json * GlobalConfigJSON, QDir * AppDataDir)
{
    //Handle GlobalConfigJSON.
    QFile GlobalConfigFile(AppDataDir->filePath("GlobalConfig.JSON"));
    if (!GlobalConfigFile.exists())
    {
        LogOut("main.cpp", "Config flie not deteced. Creating... ");

        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["TITLE"]                     = "";
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["PARENTPACKAGE"]             = 0;
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["PLATFORM"]                  = "";
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["GAMEUID"]                   = 0;
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["EXEPATH"]                   = "";
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["EXEARGS"]                   = "";
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["WORKDIR"]                   = "";
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["TGDBID"]                    = 0;
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["STEAMAPPID"]                = 0;
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["UMUID"]                     = "0";
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["GOGPRODUCTID"]              = 0;
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["COVER"]                     = "";
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["RELEASEDATE"]               = "";
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["EDITION"]                   = "";
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["EDITIONDATE"]               = "";
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["DEVELOPER"]                 = "";
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["PUBLISHER"]                 = "";
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["SERIES"]                    = "";
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["SERIESSORTNUMBER"]          = 0;
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["SUBSERIES"]                 = "";
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["SUBSERIESSORTNUMBER"]       = 0;
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["EDITOR"]                    = false;
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["ONLINEDRM"]                 = false;
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["NETWORKMULTIPLAYER"]        = false;
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["DIRECTCONNECT"]             = false;
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["LANMULTIPLAYER"]            = false;
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["ONLINEMULTIPLAYER"]         = false;
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["NETWORKCOOP"]               = false;
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["LOCALMULTIPLAYER"]          = false;
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["LOCALCOOP"]                 = false;
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["OTHERONLINEFEATURES"]       = false;
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["COMPONENT"]                 = 0;
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["VARIANTS"]                  = "";
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"]["RECOMMENDED_RUNNER"]        = "";
        (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["PRIMARY KEY"]                          = "GAMEUID";
        (*GlobalConfigJSON)["DefaultTables"]["PACKAGES"]["COLUMNS"]["PACKAGEUID"]               = 0;
        (*GlobalConfigJSON)["DefaultTables"]["PACKAGES"]["COLUMNS"]["PACKAGENAME"]              = "";
        (*GlobalConfigJSON)["DefaultTables"]["PACKAGES"]["COLUMNS"]["PACKAGEVERSION"]           = "";
        (*GlobalConfigJSON)["DefaultTables"]["PACKAGES"]["COLUMNS"]["PATH"]                     = "";
        (*GlobalConfigJSON)["Settings"]["LibraryGridSize"]                                      = 5;

        // Default runners per platform
        nlohmann::ordered_json UmuProton;
        UmuProton["NAME"]       = "umu-proton";
        UmuProton["TYPE"]       = "wine";
        UmuProton["EXECUTABLE"] = "umu-run";
        UmuProton["ENV"]        = { {"WINEPREFIX", "%RuntimePath%"}, {"GAMEID", "%UMUID%"}, {"PROTON_VERB", "waitforexitandrun"} };
        UmuProton["REMOVE_ENV"] = nlohmann::ordered_json::array({"LD_LIBRARY_PATH"});
        (*GlobalConfigJSON)["RUNNERS"]["Microsoft Windows"].push_back(UmuProton);

        nlohmann::ordered_json Snes9x;
        Snes9x["NAME"]       = "snes9x";
        Snes9x["TYPE"]       = "emulator";
        Snes9x["EXECUTABLE"] = "snes9x";
        Snes9x["ENV"]        = nlohmann::ordered_json::object();
        Snes9x["REMOVE_ENV"] = nlohmann::ordered_json::array();
        Snes9x["ARGS"]       = nlohmann::ordered_json::array();
        (*GlobalConfigJSON)["RUNNERS"]["SNES"].push_back(Snes9x);

        (*GlobalConfigJSON)["USERSETTINGS"] = nlohmann::ordered_json::object();
        (*GlobalConfigJSON)["LIBRARY"]      = nlohmann::ordered_json::array();

        if (JSONOps::SaveJSON(GlobalConfigJSON, &GlobalConfigFile))
        {
            return false; //success
        }
        else
        {
            LogErr("main.cpp", "DefaultConfig.JSON could not be initialised.");
            return true; //fail
        }
    }
    else if (JSONOps::LoadJSON(&GlobalConfigFile, GlobalConfigJSON))
    {
        LogErr("main.cpp", "Failed to parse GlobalConfig.JSON, aborting.");
        return true; //Fail
    }
    else
    {
        LogSucc("main.cpp", "GlobalConfigJSON initialized successfully.");
        return false; //Success
    }
}

bool IsRunningInPackageDir(std::filesystem::path CurrentPath)
{
    if (FSOps::CheckPackageValid(new QDir(CurrentPath))) //Convert FSOPS to stdlib! Remove unnecessary heap variable!
    {
        LogOut("main.cpp", "Running in PACKAGEDIR, running headless.");
        return true;
    }
    else
    {
        LogOut("main.cpp", "Running outside PACKAGEDIR, launching GUI.");
        return false;
    }
}

LaunchParameters ParseCommandLineArguments(int argc, char* argv[])
{
    LaunchParameters RuntimeParameters;
    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];

        if (arg == "--package" && i + 1 < argc)
        {
            RuntimeParameters.HeadlessPackagePath = argv[++i];
            RuntimeParameters.HasHeadlessPackagePath = true;
            RuntimeParameters.RunningHeadless = true;
        }
        else if (arg == "--subgame" && i + 1 < argc)
        {
            RuntimeParameters.HeadlessSubgameID = argv[++i];
        }
        else if (arg == "--component" && i + 1 < argc)
        {
            RuntimeParameters.HeadlessComponentID = argv[++i];
        }
    }
    return RuntimeParameters;
}
