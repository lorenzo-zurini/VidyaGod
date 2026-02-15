#ifndef CONTAINERWRAPPER_H
#define CONTAINERWRAPPER_H

#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <cstring>
#include <ctime>
#include <regex>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <algorithm>

#include <QDir>
#include <QVector>
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
    bool ReadOnlyVFS = false;                                       //SET

    //Wine / Proton specific:
    std::string ExePath;                                            //DERIVED FORM MANIFESTJSON
    std::string UMUID;                                              //DERIVED FORM MANIFESTJSON
    std::filesystem::path WorkDirPath;                              //DERIVED FORM MANIFESTJSON
    std::vector<std::string> ExeArgs;                               //DERIVED FORM MANIFESTJSON
    std::vector<std::string> DLLOverrides;                          //DERIVED FORM MANIFESTJSON

    //Result of initialisation:
    nlohmann::ordered_json ContainerVariablesJSON;                  //DERIVED FROM CONTAINERPARAMS
};

class ContainerWrapper
{
public:
    ContainerWrapper(nlohmann::ordered_json Passed_GlobalConfigJSON, nlohmann::ordered_json Passed_MANIFESTJSON, ContainerParams Passed_ContainerParams);
    struct ContainerParams ContainerParams;

    bool BuildContainerRuntime();
    bool Execute();

    //Container initialization:
    static bool DecideComponent(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams);
    static bool InitializeContainerParams(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams);
    static bool CreateRecipe(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams);
    static bool BuildSubComponentsArray(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams);
    static bool CreateContainerVariablesJSON(struct ContainerParams &ContainerParams);
    static bool VariableSubstitution(struct ContainerParams &ContainerParams);

    //Filesystem management:
    static bool CreateDirectories(const nlohmann::ordered_json ContainerVariablesJSON);
    static bool PreMountFilesystemComponents(struct ContainerParams &ContainerParams);
    static bool BuildUnionFS(const nlohmann::ordered_json ContainerVariablesJSON);
    static bool AddToVFSString(struct ContainerParams &ContainerParams, std::string NewPath);
    static bool FinalizeVFSString(struct ContainerParams &ContainerParams);
    static bool MountVFS(struct ContainerParams &ContainerParams);
    static bool CheckCaseConflicts(std::filesystem::path RuntimePath);

    //Registry handling:
    static bool InitializeDefPrefix(struct ContainerParams &ContainerParams);
    static bool CreateFlatRegPatchJSON(struct ContainerParams &ContainerParams);
    static bool CreateRegPatchFiles(struct ContainerParams &ContainerParams);
    static bool MergeRegPatchFiles(struct ContainerParams &ContainerParams);

    //DLL overrides:
    static bool ProcessDLLOverrides(struct ContainerParams &ContainerParams);

    //FileEdits:
    static bool ProcessFileEdits(struct ContainerParams &ContainerParams);
    static bool ConfigWrite(std::string Key, std::string Value, std::filesystem::path FilePath);

    //Misc
    static int RunCommand(std::string Program, std::vector<std::string> Arguments, QProcessEnvironment ProcessEnvironment = QProcessEnvironment::InheritFromParent);


private:
    bool InitializeContainer();
    bool BuildVirtualFilesystem();

    nlohmann::ordered_json GlobalConfigJSON;
    nlohmann::ordered_json MANIFESTJSON;
};
#endif // CONTAINERWRAPPER_H
