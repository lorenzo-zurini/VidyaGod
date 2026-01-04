#ifndef CONTAINERWRAPPER_H
#define CONTAINERWRAPPER_H

#include <nlohmann/json.hpp>
#include <iostream>
#include <cstring>
#include <filesystem>
#include <QDir>

struct RunnerPaths
{
public:
    std::filesystem::path PackagePath;
    //std::filesystem::path MetaDataPath;
    //std::filesystem::path PackageFilesPath;
    //std::filesystem::path RuntimePath;
    //std::filesystem::path UserDataPath;
    //std::filesystem::path TempPath;
    //std::filesystem::path ProgramPath;      UMU-Specific
    //std::filesystem::path ProtonPath;      UMU-Specific
    //std::filesystem::path DefPrefixPath;      UMU-Specific
};

struct RunnerParams
{
public:
    //RunnerPaths * RunnerPaths = new struct RunnerPaths;

};

class ContainerWrapper
{
public:
    //Container params:
    //Package-specific:
    std::filesystem::path PackagePath; //Passed on init.
    std::string PackageName;
    std::string PackageUID;

    //(Sub)game specific:
    std::string GameName;
    std::string UMUID;
    std::vector<int> Recipe;

    //Wine / Proton specific:
    std::string ExePath;
    std::filesystem::path WorkDirPath;
    std::vector<std::string> ExeArgs;

    int subgame;
    int component;

    ContainerWrapper(std::filesystem::path Passed_PackagePath, nlohmann::ordered_json Passed_MANIFESTJSON, nlohmann::ordered_json Passed_GlobalConfigJSON, int Passed_subgame = 0, int Passed_component = 0);
    static bool DecideComponent(nlohmann::ordered_json MANIFESTJSON, int &subgame, int &component);
    bool CreateRecipe();
private:
    nlohmann::ordered_json GlobalConfigJSON;
    nlohmann::ordered_json MANIFESTJSON;

    //RunnerParams * RunnerParams = new struct RunnerParams;

    bool InitializeRunnerParams();
};

#endif // CONTAINERWRAPPER_H
