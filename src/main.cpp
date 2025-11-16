#include "main.h"

int main(int argc, char *argv[])
{
    // Enable high-DPI scaling before QApplication is created
    QApplication Application(argc, argv);

    // Force a consistent Qt style
    Application.setStyle(QStyleFactory::create("Breeze"));

    // Optional: set a default font to ensure consistent spacing
    Application.setFont(QFont("DejaVu Sans", 10));

    //Find and create AppDir.
    QDir * AppDataDir = new QDir(QDir::homePath() +  "/.VidyaGod");
    AppDataDir->mkpath(".");

    //Check if dependencies exist in the system
    CheckExecutableDependencies();

    nlohmann::ordered_json * GlobalConfigJSON = new nlohmann::ordered_json();
    if (InitializeGlobalConfigJSON(GlobalConfigJSON, AppDataDir))
    {
        QMessageBox::warning(nullptr, "Fatal error", QString::fromStdString("GlobalConfigJSON initialization failed, aborting."));
        return 1;
    }

    //Create and launch MainWindow.
    MainWindow MainWindow(GlobalConfigJSON, AppDataDir);
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
        QMessageBox::warning(nullptr, "Missing dependencies", QString::fromStdString("The following required dependencies have not been detected on your system:\n\n" +
                                                                                     MissingDependencies + "\nPlease install them to ensure full program functionality!"));
        return 1;
    }

    return 0;
}

bool InitializeGlobalConfigJSON(nlohmann::ordered_json * GlobalConfigJSON, QDir * AppDataDir)
{
    //Handle GlobalConfigJSON.
    QFile * GlobalConfigFile = new QFile(AppDataDir->filePath("GlobalConfig.JSON"));
    if (!GlobalConfigFile->exists())
    {
        std::cout << QTime::currentTime().toString().toStdString() << " main.cpp: " << "[OUT] Config flie not deteced. Creating... " << std::endl;
        QFile * DefaultConfigFile = new QFile(QDir::cleanPath(QCoreApplication::applicationDirPath() + QDir::separator() + "DefaultConfig.JSON"));
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
    std::cout << GlobalConfigJSON->dump() << std::endl;
    return 0; //Success
}
