#ifndef CONTAINERWRAPPER_H
#define CONTAINERWRAPPER_H

#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <cstring>
#include <ctime>
#include <regex>
#include <filesystem>




#include <QDir>
#include <QMessageBox>
#include <QGuiApplication>
#include <QScreen>
#include <QProcess>



struct ContainerParams
{
public:
    ContainerParams(std::filesystem::path Passed_PackagePath, int Passed_subgame = 0, int Passed_component = 0);

    //Container params:
    //Package-specific:
    std::filesystem::path PackagePath;                              //PASSED
    std::string PackageName;                                        //DERIVED FROM MANIFESTJSON
    std::string PackageUID;                                         //DERIVED FORM MANIFESTJSON

    //(Sub)game specific:
    std::string GameName;                                           //DERIVED FORM MANIFESTJSON
    std::vector<int> Recipe;                                        //DERIVED FORM MANIFESTJSON
    nlohmann::ordered_json SubComponentsArray;                      //DERIVED FORM MANIFESTJSON

    int subgame;                                                    //PASSED
    int component;                                                  //PASSED

    //Wine / Proton specific:
    std::string ExePath;                                            //DERIVED FORM MANIFESTJSON
    std::string UMUID;                                              //DERIVED FORM MANIFESTJSON
    std::filesystem::path WorkDirPath;                              //DERIVED FORM MANIFESTJSON
    std::vector<std::string> ExeArgs;                               //DERIVED FORM MANIFESTJSON

    //Result of initialisation:
    nlohmann::ordered_json ContainerVariablesJSON;                  //DERIVED FROM CONTAINERPARAMS
};

class ContainerWrapper
{
public:
    ContainerWrapper(nlohmann::ordered_json Passed_GlobalConfigJSON, nlohmann::ordered_json Passed_MANIFESTJSON, ContainerParams Passed_ContainerParams);
    struct ContainerParams ContainerParams;

    bool BuildContainerRuntime();

    //Container initialization:
    static bool DecideComponent(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams);
    static bool InitializeContainerParams(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams);
    static bool CreateRecipe(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams);
    static bool BuildSubComponentsArray(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams);
    static bool CreateContainerVariablesJSON(struct ContainerParams &ContainerParams);
    static bool VariableSubstitution(struct ContainerParams &ContainerParams);

    //Filesystem management:
    static bool CreateDirectories(const nlohmann::ordered_json ContainerVariablesJSON);

    //Runner-specific:
    static bool InitializeDefPrefix(struct ContainerParams &ContainerParams);
    static bool CreateFlatRegPatchJSON(struct ContainerParams &ContainerParams);
    static bool CreateRegPatchFiles(struct ContainerParams &ContainerParams);
    static bool MergeRegPatchFiles(struct ContainerParams &ContainerParams);

private:
    bool InitializeContainer();
    bool BuildVirtualFilesystem();

    nlohmann::ordered_json GlobalConfigJSON;
    nlohmann::ordered_json MANIFESTJSON;
};
#endif // CONTAINERWRAPPER_H
