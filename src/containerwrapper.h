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
    std::filesystem::path PackagePath;
    std::string PackageName;
    std::string PackageUID;

    //(Sub)game specific:
    std::string GameName;
    std::string UMUID;
    std::vector<int> Recipe;
    nlohmann::ordered_json SubComponentsArray;

    //Flags
    int subgame;                                                    //PASSED
    int component;                                                  //PASSED
    bool ReadOnlyVFS = false;                                       //SET

    //System Variables
    std::string ScreenWidth;
    std::string ScreenHeight;

    //Paths (that need to be created)
    std::filesystem::path RuntimePath;
    std::filesystem::path MetaDataPath;
    std::filesystem::path PackageFilesPath;
    std::filesystem::path UserDataPath;
    std::filesystem::path TempPath;
    std::filesystem::path ProgramPath;
    std::filesystem::path DefPrefixPath;
    std::filesystem::path ExePathRelative;
    std::filesystem::path ExePathComplete;
    std::filesystem::path ExePathInPrefix;

    //Paths (that don't need to be created)
    std::string WindowsProgramPath;
    std::string WindowsExePathComplete;
    std::string WindowsProgramPathDoubleBackSlash;
    std::filesystem::path WorkDirPathRelative;
    std::filesystem::path WorkDirPathComplete;

    //Wine / Proton specific:
    std::vector<std::string> ExeArgs;                               //DERIVED FORM MANIFESTJSON
    std::vector<std::string> DLLOverrides;                          //DERIVED FORM MANIFESTJSON

    //REGISTRYWRAPPER CLASS
    nlohmann::ordered_json FlatRegPatch;

    //VFSWRAPPER CLASS
    std::string VFSString;
    std::vector<std::filesystem::path> CleanupUnmountPaths;
    //std::vector<std::filesystem::path> CleanupDeletePaths;
    std::map<std::string, std::string> GetVariablesMap();
};

class ContainerWrapper
{
public:
    ContainerWrapper(nlohmann::ordered_json &Passed_GlobalConfigJSON, nlohmann::ordered_json &Passed_MANIFESTJSON, ContainerParams &Passed_ContainerParams);
    struct ContainerParams ContainerParams;

    bool BuildContainerRuntime();
    bool Execute();
    bool Cleanup();

    //Container initialization:
    static bool DecideComponent(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams);
    static bool CreateRecipe(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams);
    static bool BuildSubComponentsArray(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams);
    static bool DeriveContainerParams(nlohmann::ordered_json MANIFESTJSON, struct ContainerParams &ContainerParams);

    //Filesystem management:
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
    static bool StringVariableSubstitution(std::string &SourceString, const std::map<std::string, std::string> &VariablesMap);

private:
    bool InitializeContainer();
    bool BuildVirtualFilesystem();

    nlohmann::ordered_json GlobalConfigJSON;
    nlohmann::ordered_json MANIFESTJSON;
};
#endif // CONTAINERWRAPPER_H
