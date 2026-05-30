#include "main.h"
#include "commonutils.h"

int main(int argc, char *argv[])
{
    //Parse command line arguments and initialize RuntimeParameters struct.
    //Must happen before QApplication so headless runs never touch the display.
    LaunchParameters LaunchParameters = ParseCommandLineArguments(argc, argv);
    LogOut("main.cpp", "Running VidyaGod in " + LaunchParameters.CurrentPath.string());
    LogOut("main.cpp", "Headless PackagePath: " + LaunchParameters.HeadlessPackagePath.string());
    LogOut("main.cpp", "Headless SubgameID: " + LaunchParameters.HeadlessSubgameID);
    LogOut("main.cpp", "Headless ComponentID: " + LaunchParameters.HeadlessComponentID);

    //Check if dependencies exist in the system.
    //Non-fatal: warns but continues so the GUI still opens when VFS is not needed.
    CheckExecutableDependencies();

    //Auto-detect package-directory mode: if no --package flag was passed, check whether
    //the current working directory itself is a package. This lets users simply cd into a
    //package and run the binary directly without any extra flags.
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
    //Find and create AppDataDir (~/.VidyaGod). mkpath is a no-op if it already exists.
    QDir AppDataDir(QDir::homePath() +  "/.VidyaGod");
    AppDataDir.mkpath(".");

    //Initialization of GlobalConfigJSON, the central data structure of the program.
    //All runner definitions, library entries, and user settings live here.
    nlohmann::ordered_json GlobalConfigJSON;
    if (InitializeGlobalConfigJSON(&GlobalConfigJSON, &AppDataDir))
    {
        //InitializeGlobalConfigJSON returns true on FAILURE (shell exit-code convention).
        LogErr("main.cpp", "Fatal error. GlobalConfigJSON initialization failed, aborting.");
        return 1;
    }

    //HEADLESS MODE: build the container and run the game without showing any window.
    //Used both for the --package CLI flag and for auto-detected package-directory mode.
    if(LaunchParameters.RunningHeadless)
    {
        LogOut("main.cpp", "Running package " + LaunchParameters.HeadlessPackagePath.string() + " in HEADLESS mode.");
        nlohmann::ordered_json MANIFESTJSON;
        //CLEAN THIS CRAP BELOW
        //Load the package's MANIFEST.json — required before constructing ContainerParams.
        if (JSONOps::LoadJSON(new QFile(QDir::cleanPath(QString::fromStdString(LaunchParameters.HeadlessPackagePath) + QDir::separator() + "METADATA" + QDir::separator() + "MANIFEST.json")), &MANIFESTJSON))
        {
            LogErr("main.cpp", "Fatal error. Paring MANIFESTJSON failed, aborting.");
        }

        //Resolve component_id from --variant if supplied, otherwise let DecideComponent handle it.
        std::string HeadlessComponentID = LaunchParameters.HeadlessComponentID;
        if (HeadlessComponentID.empty() && !LaunchParameters.VariantID.empty())
        {
            //Read EXECUTABLE_ID from the subgame, then find the component for the requested variant.
            int SubgameIdx = ContainerWrapper::FindSubgameIndex(MANIFESTJSON, LaunchParameters.HeadlessSubgameID);
            if (SubgameIdx != -1)
            {
                auto &ExeIDField = MANIFESTJSON["SUBGAMES"][SubgameIdx]["EXECUTABLE_ID"];
                std::string ExecutableID = (!ExeIDField.is_null() && ExeIDField.is_string()) ? std::string(ExeIDField) : "";
                std::string ResolvedID = ContainerWrapper::FindComponentForVariant(MANIFESTJSON, ExecutableID, LaunchParameters.VariantID);
                if (!ResolvedID.empty()) HeadlessComponentID = ResolvedID;
            }
        }

        //Construct the container, build its runtime (mounts, prefix, registry patches),
        //execute the game, then return — cleanup happens inside Execute/Cleanup.
        struct ContainerParams NewContainerParams = ContainerParams(LaunchParameters.HeadlessPackagePath, LaunchParameters.HeadlessSubgameID, HeadlessComponentID);
        NewContainerParams.VariableOverrides = LaunchParameters.VariableOverrides;
        class ContainerWrapper NewContainerWrapper = ContainerWrapper(GlobalConfigJSON, MANIFESTJSON, NewContainerParams);
        if (!ContainerWrapper::ResolveExecutableDefinition(NewContainerWrapper.ContainerParams))
        {
            LogErr("main.cpp", "ResolveExecutableDefinition failed, aborting.");
            return 1;
        }
        NewContainerWrapper.BuildContainerRuntime();
        NewContainerWrapper.Execute();
        return 0;
    }


    //Initializing GUI. MAKE SURE THERE ARE NOT QT GUI-RELATED CALLS ABOVE THIS LINE OR THE PROGRAM WILL CRASH.
    //QApplication must be constructed before any QWidget or QPixmap usage;
    //headless code paths above deliberately avoid creating any Qt GUI objects.
    //---------------------------------------------------------------------------------------------------------
    QApplication Application(argc, argv);
    //Force a consistent Qt style so the UI looks the same regardless of the system theme.
    Application.setStyle(QStyleFactory::create("Fusion"));

    //Set a default font to ensure consistent spacing across desktop environments.
    Application.setFont(QFont("DejaVu Sans", 10));

    //Create and launch MainWindow. Passes GlobalConfigJSON and AppDataDir by pointer so
    //the window can persist changes (add/remove packages, save settings) to disk.
    MainWindow MainWindow(&GlobalConfigJSON, &AppDataDir);
    MainWindow.show();
    return Application.exec();
}

//Checks for all external binaries that the VFS and runner subsystems depend on.
//Uses `which` via std::system() rather than QProcess to keep this path dependency-free.
//Returns true if any binary is missing, false if everything is present.
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
        //Warning dialog is currently disabled to avoid blocking headless runs.
        //Re-enable the QMessageBox below when a non-blocking notification is desired.
        //QMessageBox::warning(nullptr, "Missing dependencies", QString::fromStdString("The following required dependencies have not been detected on your system:\n\n" +
        //                                                                             MissingDependencies + "\nPlease install them to ensure full program functionality!"));
        return true;
    }

    return false;
}

//Loads GlobalConfig.JSON from AppDataDir, or creates it with defaults if it does not exist.
//The defaults define the database table schemas (LIBRARY/PACKAGES column layouts),
//the built-in runner definitions (umu-proton for Windows, snes9x for SNES), and an
//empty LIBRARY array and USERSETTINGS object.
//Returns true on FAILURE, false on SUCCESS — matches shell exit-code convention so
//callers can write `if (InitializeGlobalConfigJSON(...)) { /* handle error */ }`.
bool InitializeGlobalConfigJSON(nlohmann::ordered_json * GlobalConfigJSON, QDir * AppDataDir)
{
    //Handle GlobalConfigJSON.
    QFile GlobalConfigFile(AppDataDir->filePath("GlobalConfig.JSON"));
    if (!GlobalConfigFile.exists())
    {
        LogOut("main.cpp", "Config flie not deteced. Creating... ");

        //Populate the schema for the LIBRARY table — each key is a column name and its
        //value is the default / type sentinel for that column.
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
        //Schema for the PACKAGES table (slim package registry, not per-subgame).
        (*GlobalConfigJSON)["DefaultTables"]["PACKAGES"]["COLUMNS"]["PACKAGEUID"]               = 0;
        (*GlobalConfigJSON)["DefaultTables"]["PACKAGES"]["COLUMNS"]["PACKAGENAME"]              = "";
        (*GlobalConfigJSON)["DefaultTables"]["PACKAGES"]["COLUMNS"]["PACKAGEVERSION"]           = "";
        (*GlobalConfigJSON)["DefaultTables"]["PACKAGES"]["COLUMNS"]["PATH"]                     = "";
        (*GlobalConfigJSON)["Settings"]["LibraryGridSize"]                                      = 5;

        //Default runners per platform — these become the starting point for RUNNERS in
        //GlobalConfig.JSON and can be extended or overridden by the user.
        //umu-proton handles Wine/Proton execution via the UMU launcher for Windows games.
        nlohmann::ordered_json UmuProton;
        UmuProton["NAME"]       = "umu-proton";
        UmuProton["TYPE"]       = "wine";
        UmuProton["EXECUTABLE"] = "umu-run";
        //ENV keys use %VARIABLE% tokens that are expanded by StringVariableSubstitution at runtime.
        UmuProton["ENV"]        = { {"WINEPREFIX", "%RuntimePath%"}, {"GAMEID", "%UMUID%"}, {"PROTON_VERB", "waitforexitandrun"} };
        //LD_LIBRARY_PATH must be removed to prevent system libraries from conflicting with Proton's.
        UmuProton["REMOVE_ENV"] = nlohmann::ordered_json::array({"LD_LIBRARY_PATH"});
        (*GlobalConfigJSON)["RUNNERS"]["Microsoft Windows"].push_back(UmuProton);

        //Plain Wine — no Proton/pressure-vessel container, sees FUSE mounts directly.
        nlohmann::ordered_json Wine;
        Wine["NAME"]       = "wine";
        Wine["TYPE"]       = "wine";
        Wine["EXECUTABLE"] = "wine";
        Wine["ENV"]        = { {"WINEPREFIX", "%RuntimePath%"} };
        Wine["REMOVE_ENV"] = nlohmann::ordered_json::array({"LD_LIBRARY_PATH"});
        (*GlobalConfigJSON)["RUNNERS"]["Microsoft Windows"].push_back(Wine);

        //snes9x handles SNES ROMs; the ROM path is passed as the sole argument.
        nlohmann::ordered_json Snes9x;
        Snes9x["NAME"]       = "snes9x";
        Snes9x["TYPE"]       = "emulator";
        Snes9x["EXECUTABLE"] = "snes9x";
        Snes9x["ENV"]        = nlohmann::ordered_json::object();
        Snes9x["REMOVE_ENV"] = nlohmann::ordered_json::array();
        Snes9x["ARGS"]       = nlohmann::ordered_json::array({"-fullscreen"}); //TO-DO: expose via settings
        (*GlobalConfigJSON)["RUNNERS"]["SNES"].push_back(Snes9x);

        //Custom platform: runner is always defined entirely inside the package MANIFEST.
        //GlobalConfig holds an empty array here — it is never populated from the online registry.
        (*GlobalConfigJSON)["RUNNERS"]["Custom"] = nlohmann::ordered_json::array();

        //Start with empty per-package user overrides and an empty library.
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
        //LoadJSON returns non-zero on failure.
        LogErr("main.cpp", "Failed to parse GlobalConfig.JSON, aborting.");
        return true; //Fail
    }
    else
    {
        LogSucc("main.cpp", "GlobalConfigJSON initialized successfully.");
        return false; //Success
    }
}

//Delegates to FSOps::CheckPackageValid to test whether CurrentPath contains a METADATA
//subdirectory — the minimum criterion for a valid package.
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

//Parses argc/argv sequentially.
//Recognized arguments:
//  --package <path>      : path to the package to run headlessly
//  --subgame <id>        : SUBGAMEID string from the package's MANIFEST.json
//  --component <id>      : COMPONENTID override (bypasses subgame's default component)
//  --var KEY=VALUE       : override a CustomVar variable; may be repeated for multiple vars
//Unknown arguments are silently ignored.
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
        else if (arg == "--var" && i + 1 < argc)
        {
            //Expects KEY=VALUE format; silently skips malformed entries without '='.
            std::string kv = argv[++i];
            auto eq = kv.find('=');
            if (eq != std::string::npos)
                RuntimeParameters.VariableOverrides[kv.substr(0, eq)] = kv.substr(eq + 1);
        }
        else if (arg == "--variant" && i + 1 < argc)
        {
            RuntimeParameters.VariantID = argv[++i];
        }
    }
    return RuntimeParameters;
}
