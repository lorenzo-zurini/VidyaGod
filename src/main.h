#ifndef MAIN_H
#define MAIN_H
#endif // MAIN_H

//Intentional: includes live outside the guard so this header can be used as a
//forward-declaration-only unit; the actual types are pulled in by the includes below.
#include <filesystem>
#include <iostream>
#include <fstream>
#include <list>
#include <map>

#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QMessageBox>
#include <QApplication>
#include <QStyleFactory>
#include <QFile>

#include "nlohmann/json.hpp"
#include "filesystemoperations.h"
#include "mainwindow.h"
#include "jsonoperations.h"

//Holds all parameters resolved from command-line arguments and auto-detection at startup.
//Populated once by ParseCommandLineArguments() and consulted by main() to decide
//whether to run headless (launch a package directly) or open the full GUI.
struct LaunchParameters
{
    std::filesystem::path CurrentPath = std::filesystem::current_path(); //Working directory at launch — used to auto-detect package dirs
    bool RunningHeadless = false;                                         //True when no GUI should be shown
    bool RunningInPackageDir = false;                                     //True when auto-detected as running inside a package directory
    bool HasHeadlessPackagePath = false;                                  //True when a package path has been resolved (CLI or auto-detect)
    std::filesystem::path HeadlessPackagePath;                            //Path to the package to launch in headless mode
    std::string HeadlessSubgameID;                                        //Subgame to launch (empty = use default)
    std::string HeadlessComponentID;                                      //Component override (empty = resolved from subgame)
    std::map<std::string, std::string> VariableOverrides;                 //Custom variable overrides from --var KEY=VALUE flags
};

int main(int argc, char *argv[]);

//Checks for required external binaries (unionfs, fuse-zip, fusermount, umu-run, bindfs).
//Logs any that are missing. Returns true if any dependency is absent, false if all found.
bool CheckExecutableDependencies();

//Loads or creates the GlobalConfigJSON file in AppDataDir.
//If the config does not exist yet, it is seeded with default table schemas,
//default runners (umu-proton for Windows, snes9x for SNES), and an empty library.
//Returns true on failure, false on success (shell exit-code convention).
bool InitializeGlobalConfigJSON(nlohmann::ordered_json * GlobalConfigJSON, QDir * AppDataDir);

//Parses argc/argv and returns a populated LaunchParameters struct.
//Recognized flags: --package <path>, --subgame <id>, --component <id>.
static LaunchParameters ParseCommandLineArguments(int argc, char* argv[]);

//Returns true if CurrentPath looks like a VidyaGod package directory
//(i.e. FSOps::CheckPackageValid passes for it).
//Enables automatic headless launch when the binary is run from inside a package.
static bool IsRunningInPackageDir(std::filesystem::path CurrentPath);
