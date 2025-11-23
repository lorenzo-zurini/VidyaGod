#ifndef MAIN_H
#define MAIN_H

#endif // MAIN_H
#include <filesystem>
#include <iostream>
#include <fstream>

#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QMessageBox>
#include <QApplication>
#include <QStyleFactory>
#include <QFile>

#include "nlohmann/json.hpp"
#include "filesystemoperations.h"
#include "mainwindow.h"
#include "runner.h"


struct LaunchParameters
{
    std::filesystem::path CurrentPath = std::filesystem::current_path();
    bool RunningHeadless = false;
    bool RunningInPackageDir = false;
    bool HasHeadlessPackagePath = false;
    std::filesystem::path HeadlessPackagePath;
    int HeadlessSubgame = 1;
    int HeadlessComponent = 0;
};

int main(int argc, char *argv[]);
bool CheckExecutableDependencies();
bool InitializeGlobalConfigJSON(nlohmann::ordered_json * GlobalConfigJSON, QDir * AppDataDir);
static LaunchParameters ParseCommandLineArguments(int argc, char* argv[]);
static bool IsRunningInPackageDir(std::filesystem::path CurrentPath);
