#include "main.h"

int main(int argc, char *argv[])
{
    //Parse command line arguments and initialize RuntimeParameters struct.
    LaunchParameters LaunchParameters = ParseCommandLineArguments(argc, argv);
    std::cout << QTime::currentTime().toString().toStdString() << " main.cpp: " << "[OUT] Running VidyaGod in " << LaunchParameters.CurrentPath << std::endl;
    std::cout << QTime::currentTime().toString().toStdString() << " main.cpp: " << "[OUT] Headless PackagePath: " << LaunchParameters.HeadlessPackagePath << std::endl;
    std::cout << QTime::currentTime().toString().toStdString() << " main.cpp: " << "[OUT] Headless Subgame: " << LaunchParameters.HeadlessSubgame << std::endl;
    std::cout << QTime::currentTime().toString().toStdString() << " main.cpp: " << "[OUT] Headless Component: " << LaunchParameters.HeadlessComponent << std::endl;

    //Check if dependencies exist in the system
    CheckExecutableDependencies();

    if (!LaunchParameters.HasHeadlessPackagePath)
    {
        //Detect ./METADATA/MANIFEST.json to detemine if running in packagedir.
        //Start in GUI-less mode if so.
        std::cout << QTime::currentTime().toString().toStdString() << " main.cpp: " << "[OUT] Checking if running in a PACKAGEDIR." << std::endl;
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
    QDir * AppDataDir = new QDir(QDir::homePath() +  "/.VidyaGod");
    AppDataDir->mkpath(".");

    //Initialization of GlobalConfigJSON, the central data structure of the program.
    nlohmann::ordered_json GlobalConfigJSON;// = new nlohmann::ordered_json();
    if (InitializeGlobalConfigJSON(&GlobalConfigJSON, AppDataDir))
    {
        std::cout << QTime::currentTime().toString().toStdString() << " main.cpp: " << "[ERR] Fatal error. GlobalConfigJSON initialization failed, aborting." << std::endl;
        return 1;
    }

    //HEADLESS MODE:
    if(LaunchParameters.RunningHeadless)
    {
        std::cout << QTime::currentTime().toString().toStdString() << " main.cpp: " << "[OUT] Running package " << LaunchParameters.HeadlessPackagePath << " in HEADLESS mode." << std::endl;
        nlohmann::ordered_json MANIFESTJSON;// = new nlohmann::ordered_json;
        //CLEAN THIS CRAP BELOW
        if (JSONOps::LoadJSON(new QFile(QDir::cleanPath(QString::fromStdString(LaunchParameters.HeadlessPackagePath) + QDir::separator() + "METADATA" + QDir::separator() + "MANIFEST.json")), &MANIFESTJSON))
        {
            std::cout << QTime::currentTime().toString().toStdString() << " main.cpp: " << "[ERR] Fatal error. Paring MANIFESTJSON failed, aborting." << std::endl;
        }

        Runner * GameRunner = new Runner(new QDir(LaunchParameters.HeadlessPackagePath), &MANIFESTJSON, &GlobalConfigJSON, LaunchParameters.HeadlessSubgame);
        std::cout << QTime::currentTime().toString().toStdString() << " main.cpp: " << "[OUT] Executing pre-run cleanup." << std::endl;
        GameRunner->Cleanup();
        std::cout << QTime::currentTime().toString().toStdString() << " main.cpp: " << "[OUT] Building runtime." << std::endl;
        GameRunner->BuildRuntime();
        std::cout << QTime::currentTime().toString().toStdString() << " main.cpp: " << "[OUT] Running game" << MANIFESTJSON["SUBGAMES"][LaunchParameters.HeadlessSubgame - 1]["TITLE"] << std::endl;
        GameRunner->Run();
        GameRunner->Cleanup();

        delete GameRunner;
        return 0;
    }


    //Initializing GUI. MAKE SURE THERE ARE NOT QT GUI-RELATED CALLS ABOVE THIS LINE OR THE PROGRAM WILL CRASH.
    //---------------------------------------------------------------------------------------------------------
    QApplication Application(argc, argv);
    // Force a consistent Qt style
    Application.setStyle(QStyleFactory::create("Breeze"));

    // Optional: set a default font to ensure consistent spacing
    Application.setFont(QFont("DejaVu Sans", 10));

    //Create and launch MainWindow.
    MainWindow MainWindow(&GlobalConfigJSON, AppDataDir);
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
            std::cout << QTime::currentTime().toString().toStdString() << " main.cpp: [OUT] " << Binary << " found on the system." << std::endl;
        }
        else
        {
            std::cout << QTime::currentTime().toString().toStdString() << " main.cpp: [ERR] " << Binary << " NOT FOUND ON THE SYSTEM, VFS WILL NOT WORK." << std::endl;
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
    QFile * GlobalConfigFile = new QFile(AppDataDir->filePath("GlobalConfig.JSON"));
    if (!GlobalConfigFile->exists())
    {
        std::cout << QTime::currentTime().toString().toStdString() << " main.cpp: " << "[OUT] Config flie not deteced. Creating... " << std::endl;
        QFile * DefaultConfigFile = new QFile(QDir::cleanPath(QString::fromStdString(std::filesystem::current_path()) + QDir::separator() + "DefaultConfig.JSON"));
        if (!DefaultConfigFile->exists())
        {
            std::cout << QTime::currentTime().toString().toStdString() << " main.cpp: " << "[ERR] DefaultConfig.JSON not found, aborting." << std::endl;
            return 1; //Fail
        }

        DefaultConfigFile->copy(GlobalConfigFile->fileName());
        delete DefaultConfigFile;
    }

    if (JSONOps::LoadJSON(GlobalConfigFile, GlobalConfigJSON))
    {
        std::cout << QTime::currentTime().toString().toStdString() << " main.cpp: " << "[ERR] Failed to parse GlobalConfig.JSON, aborting." << std::endl;
        return 1; //Fail
    }

    std::cout << QTime::currentTime().toString().toStdString() << " main.cpp: " << "[OUT] GlobalConfigJSON initialized successfully:" << std::endl;
    //std::cout << GlobalConfigJSON->dump() << std::endl;
    return 0; //Success
}

bool IsRunningInPackageDir(std::filesystem::path CurrentPath)
{
    if (FSOps::CheckPackageValid(new QDir(CurrentPath))) //Convert FSOPS to stdlib! Remove unnecessary heap variable!
    {
        std::cout << QTime::currentTime().toString().toStdString() << " main.cpp: " << "[OUT] Running in PACKAGEDIR, running headless." << std::endl;
        return true;
    }
    else
    {
        std::cout << QTime::currentTime().toString().toStdString() << " main.cpp: " << "[OUT] Running outside PACKAGEDIR, launching GUI." << std::endl;
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
            RuntimeParameters.HeadlessSubgame = std::stoi(argv[++i]);
        }
        else if (arg == "--component" && i + 1 < argc)
        {
            RuntimeParameters.HeadlessComponent = std::stoi(argv[++i]);
        }
    }
    return RuntimeParameters;
}
