#include "main.h"
#include "commonutils.h"

#include <QComboBox>
#include <QAbstractScrollArea>
#include <QWheelEvent>

//Application-wide event filter that stops a QComboBox from changing its value when the mouse
//wheel is scrolled over it (a very easy way to accidentally mutate settings). Qt's
//QComboBox::wheelEvent cycles the selection on hover-scroll regardless of focus, so the wheel is
//blocked on the (closed) combo control unconditionally — the value is still changed by clicking or
//by arrow keys. The open dropdown LIST is a separate widget, so scrolling an actually-open list
//still works. To keep scrollable pages usable, the swallowed wheel is redirected to the nearest
//enclosing scroll area so the page still scrolls past the combo. Installed once on the
//QApplication, so it covers every combo, including ones created dynamically.
class WheelGuard : public QObject
{
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject * Obj, QEvent * Event) override
    {
        if (Event->type() == QEvent::Wheel)
            if (QComboBox * Combo = qobject_cast<QComboBox *>(Obj))
            {
                //Forward the scroll to the surrounding scroll area (if any) instead of the combo.
                for (QWidget * W = Combo->parentWidget(); W; W = W->parentWidget())
                    if (QAbstractScrollArea * SA = qobject_cast<QAbstractScrollArea *>(W))
                    {
                        QCoreApplication::sendEvent(SA->viewport(), Event);
                        break;
                    }
                return true; //never let the hovered combo consume the wheel
            }
        return QObject::eventFilter(Obj, Event);
    }
};

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
        //Assemble the manifest from every *.json in the package dir, then validate.
        std::vector<std::string> AsmWarn, ValErr, ValWarn;
        if (!JSONOps::AssembleManifest(QString::fromStdString(LaunchParameters.HeadlessPackagePath), MANIFESTJSON, AsmWarn))
        {
            LogErr("main.cpp", "Fatal error: no usable JSON manifest in package, aborting.");
            return 1;
        }
        for (const auto &W : AsmWarn) LogWarn("main.cpp", "Manifest: " + W);
        JSONOps::ValidateManifest(MANIFESTJSON, ValErr, ValWarn);
        for (const auto &W : ValWarn) LogWarn("main.cpp", "Manifest: " + W);
        if (!ValErr.empty())
        {
            for (const auto &E : ValErr) LogErr("main.cpp", "Manifest error: " + E);
            LogErr("main.cpp", "Aborting due to manifest validation errors.");
            return 1;
        }

        //Construct the container, build its runtime (mounts, prefix, registry patches),
        //execute the game, then return — cleanup happens inside Execute/Cleanup.
        //VariantID is set BEFORE construction so DecideComponent/CreateRecipe resolve the
        //selected variant's ENDPOINTS. A direct --component still works via HeadlessComponentID.
        struct ContainerParams NewContainerParams = ContainerParams(LaunchParameters.HeadlessPackagePath, LaunchParameters.HeadlessSubgameID, LaunchParameters.HeadlessComponentID);
        NewContainerParams.VariableOverrides = LaunchParameters.VariableOverrides;
        NewContainerParams.VariantID = LaunchParameters.VariantID;
        NewContainerParams.RunnerID  = LaunchParameters.RunnerID;
        class ContainerWrapper NewContainerWrapper = ContainerWrapper(GlobalConfigJSON, MANIFESTJSON, NewContainerParams);
        if (!ContainerWrapper::ResolveExecutableDefinition(MANIFESTJSON, NewContainerWrapper.ContainerParams))
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

    //Block accidental hover-scroll from mutating combo boxes app-wide (see WheelGuard).
    Application.installEventFilter(new WheelGuard(&Application));

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

//The four built-in runners as a single readable JSON literal (flat first-class schema).
//GE-Proton is first so it is the default candidate for Microsoft Windows.
//TO-DO: make runner paths (PROTONPATH) configurable via settings rather than hardcoded.
static nlohmann::ordered_json DefaultRunners()
{
    return nlohmann::ordered_json::parse(R"JSON([
        {
            "RUNNER_ID": "ge-proton10-30", "NAME": "GE-Proton10-30", "TYPE": "wine",
            "PLATFORMS": ["Microsoft Windows"], "EXECUTABLE": "umu-run",
            "ENV": { "WINEPREFIX": "%RuntimePath%", "GAMEID": "%UMUID%", "PROTON_VERB": "waitforexitandrun",
                     "PROTONPATH": "/home/lorenzo-zurini/.local/share/Steam/compatibilitytools.d/GE-Proton10-30" },
            "REMOVE_ENV": ["LD_LIBRARY_PATH"], "ARGS": [], "ENDPOINTS": []
        },
        {
            "RUNNER_ID": "umu-proton", "NAME": "umu-proton", "TYPE": "wine",
            "PLATFORMS": ["Microsoft Windows"], "EXECUTABLE": "umu-run",
            "ENV": { "WINEPREFIX": "%RuntimePath%", "GAMEID": "%UMUID%", "PROTON_VERB": "waitforexitandrun" },
            "REMOVE_ENV": ["LD_LIBRARY_PATH"], "ARGS": [], "ENDPOINTS": []
        },
        {
            "RUNNER_ID": "wine", "NAME": "wine", "TYPE": "wine",
            "PLATFORMS": ["Microsoft Windows"], "EXECUTABLE": "wine",
            "ENV": { "WINEPREFIX": "%RuntimePath%" },
            "REMOVE_ENV": ["LD_LIBRARY_PATH"], "ARGS": [], "ENDPOINTS": []
        },
        {
            "RUNNER_ID": "snes9x", "NAME": "snes9x", "TYPE": "emulator",
            "PLATFORMS": ["SNES"], "EXECUTABLE": "snes9x",
            "ENV": {}, "REMOVE_ENV": [], "ARGS": ["-fullscreen"], "ENDPOINTS": []
        }
    ])JSON");
}

//Guarantees the GlobalConfig has the shape the app actually uses, seeding any missing piece:
//  RUNNERS (array of runners), LIBRARY (array of packages), Settings (object of UI prefs).
//Returns true if it added anything, so the caller can persist a freshly-seeded config.
static bool EnsureGlobalConfigDefaults(nlohmann::ordered_json & gc)
{
    bool Changed = false;
    if (!gc.is_object())                                        { gc = nlohmann::ordered_json::object();             Changed = true; }
    if (!gc.contains("RUNNERS")  || !gc["RUNNERS"].is_array())  { gc["RUNNERS"]  = DefaultRunners();                 Changed = true; }
    if (!gc.contains("LIBRARY")  || !gc["LIBRARY"].is_array())  { gc["LIBRARY"]  = nlohmann::ordered_json::array();  Changed = true; }
    if (!gc.contains("Settings") || !gc["Settings"].is_object()){ gc["Settings"] = nlohmann::ordered_json::object(); Changed = true; }
    return Changed;
}

//Loads GlobalConfig.JSON from AppDataDir (or starts empty if it does not exist), then ensures the
//config has the shape the app uses via EnsureGlobalConfigDefaults. The file is (re)written when it
//was freshly created or when a missing top-level key had to be seeded.
//Returns true on FAILURE, false on SUCCESS — matches shell exit-code convention so
//callers can write `if (InitializeGlobalConfigJSON(...)) { /* handle error */ }`.
bool InitializeGlobalConfigJSON(nlohmann::ordered_json * GlobalConfigJSON, QDir * AppDataDir)
{
    QFile GlobalConfigFile(AppDataDir->filePath("GlobalConfig.JSON"));
    const bool Existed = GlobalConfigFile.exists();

    if (Existed && JSONOps::LoadJSON(&GlobalConfigFile, GlobalConfigJSON))
    {
        //LoadJSON returns non-zero on failure — don't silently overwrite a corrupt config.
        LogErr("main.cpp", "Failed to parse GlobalConfig.JSON, aborting.");
        return true; //fail
    }
    if (!Existed) LogOut("main.cpp", "Config file not detected. Creating defaults...");

    const bool Changed = EnsureGlobalConfigDefaults(*GlobalConfigJSON);

    if ((!Existed || Changed) && !JSONOps::SaveJSON(GlobalConfigJSON, &GlobalConfigFile))
    {
        LogErr("main.cpp", "GlobalConfig.JSON could not be written.");
        return true; //fail
    }

    LogSucc("main.cpp", "GlobalConfigJSON initialized successfully.");
    return false; //success
}

//Delegates to FSOps::CheckPackageValid to test whether CurrentPath contains MANIFEST.json.
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
        else if (arg == "--runner" && i + 1 < argc)
        {
            RuntimeParameters.RunnerID = argv[++i];
        }
    }
    return RuntimeParameters;
}
