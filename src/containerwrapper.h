#ifndef CONTAINERWRAPPER_H
#define CONTAINERWRAPPER_H

#include <nlohmann/json.hpp>
#include <iostream>
#include <cstring>
#include <ctime>
#include <regex>
#include <filesystem>




#include <QDir>
#include <QMessageBox>
#include <QGuiApplication>
#include <QScreen>




struct ContainerParams
{
public:
    ContainerParams(std::filesystem::path Passed_PackagePath, int Passed_subgame = 0, int Passed_component = 0);

    //Container params:
    //Package-specific:
    std::filesystem::path PackagePath;                  //PASSED
    std::string PackageName;                            //DERIVED FROM MANIFESTJSON
    std::string PackageUID;                             //DERIVED FORM MANIFESTJSON

    //(Sub)game specific:
    std::string GameName;                               //DERIVED FORM MANIFESTJSON
    std::vector<int> Recipe;                            //DERIVED FORM MANIFESTJSON
    nlohmann::ordered_json SubComponentsArray;          //DERIVED FORM MANIFESTJSON

    int subgame;                                        //PASSED
    int component;                                      //PASSED

    //Wine / Proton specific:
    std::string ExePath;                                //DERIVED FORM MANIFESTJSON
    std::string UMUID;                                  //DERIVED FORM MANIFESTJSON
    std::filesystem::path WorkDirPath;                  //DERIVED FORM MANIFESTJSON
    std::vector<std::string> ExeArgs;                   //DERIVED FORM MANIFESTJSON
};

class ContainerWrapper
{
public:
    ContainerWrapper(nlohmann::ordered_json Passed_GlobalConfigJSON, nlohmann::ordered_json Passed_MANIFESTJSON, ContainerParams Passed_ContainerParams);
    static bool DecideComponent(nlohmann::ordered_json MANIFESTJSON, ContainerParams &ContainerParams);
    static bool InitializeContainerParams(nlohmann::ordered_json MANIFESTJSON, ContainerParams &ContainerParams);
    static bool CreateRecipe(nlohmann::ordered_json MANIFESTJSON, ContainerParams &ContainerParams);
    static bool BuildSubComponentsArray(nlohmann::ordered_json MANIFESTJSON, ContainerParams &ContainerParams);
    static bool VariableSubstitution(ContainerParams &ContainerParams);
private:
    bool InitializeContainer();
    struct ContainerParams ContainerParams;
    nlohmann::ordered_json GlobalConfigJSON;
    nlohmann::ordered_json MANIFESTJSON;
};
#endif // CONTAINERWRAPPER_H
