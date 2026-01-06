#ifndef CONTAINERWRAPPER_H
#define CONTAINERWRAPPER_H

#include <nlohmann/json.hpp>
#include <iostream>
#include <cstring>
#include <filesystem>
#include <QDir>

struct ContainerParams
{
public:
    //Container params:
    nlohmann::ordered_json GlobalConfigJSON;

    //Package-specific:
    nlohmann::ordered_json MANIFESTJSON;
    std::filesystem::path PackagePath;
    std::string PackageName;
    std::string PackageUID;

    //(Sub)game specific:
    std::string GameName;
    std::vector<int> Recipe;
    nlohmann::ordered_json SubComponentsArray;

    //Wine / Proton specific:
    std::string ExePath;
    std::string UMUID;
    std::filesystem::path WorkDirPath;
    std::vector<std::string> ExeArgs;

    int subgame;
    int component;
};

class ContainerWrapper
{
public:
    ContainerWrapper(ContainerParams Passed_ContainerParams);
    static bool DecideComponent(nlohmann::ordered_json MANIFESTJSON, ContainerParams &ContainerParams);
    static bool InitializeContainerParams(nlohmann::ordered_json MANIFESTJSON, ContainerParams &ContainerParams);
    static bool CreateRecipe(nlohmann::ordered_json MANIFESTJSON, ContainerParams &ContainerParams);
    static bool BuildSubComponentsArray(nlohmann::ordered_json MANIFESTJSON, ContainerParams &ContainerParams);
    static bool VariableSubstitution(ContainerParams &ContainerParams);
private:
    ContainerParams ContainerParams;
};
#endif // CONTAINERWRAPPER_H
