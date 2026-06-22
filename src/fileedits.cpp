#include "fileedits.h"
#include "commonutils.h"   // Log*

#include <filesystem>
#include <fstream>
#include <string>

//Collects DLLOVERRIDE values from all DllOverride subcomponents into DLLOverrides.
//The collected strings are later joined and assigned to WINEDLLOVERRIDES in Execute().
//Returns false immediately if any DllOverride subcomponent has a null DLLOVERRIDE field.
bool FileEdits::ProcessDLLOverrides(struct ContainerParams &ContainerParams)
{
    LogOut("FileEdits::ProcessDLLOverrides", "Processing DLL Overrides.");
    for (size_t i = 0; i < ContainerParams.SubComponentsArray.size(); i++)
    {
        nlohmann::ordered_json SubComponentJSON = ContainerParams.SubComponentsArray[i];
        if (SubComponentJSON["TYPE"] == "DllOverride")
        {
            if(!SubComponentJSON["DLLOVERRIDE"].is_null())
            {
                ContainerParams.DLLOverrides.push_back(SubComponentJSON["DLLOVERRIDE"]);
            }
            else
            {
                return false;
            }
        }
    }
    return true;
}

//Processes FileEdit subcomponents in two passes, split by the OVERRIDE flag.
//  OverridePass=false (base): writes under BaseDir — the DEFAULTDATA layer, between the component
//    layers and the WRITELAYER, so the user's persisted writes naturally shadow them.
//  OverridePass=true  (override): writes to RuntimePath via the union, COW-ing directly into
//    WRITELAYER — wins unconditionally over any previous state, including the user's.
//BaseDir defaults to RuntimePath (override) / DefPrefixPath (base) when not supplied.
//MUST BE RUN AFTER VARIABLE SUBSTITUTION (already done in BuildSubComponentsArray).
bool FileEdits::ProcessFileEdits(struct ContainerParams &ContainerParams, bool OverridePass,
                                        const std::filesystem::path &BaseDir)
{
    std::filesystem::path BasePath = !BaseDir.empty() ? BaseDir
                                     : (OverridePass ? ContainerParams.RuntimePath : ContainerParams.DefPrefixPath);
    LogOut("FileEdits::ProcessFileEdits",
           std::string(OverridePass ? "Override" : "Base") + " FileEdit pass. Base: " + BasePath.string());

    for (auto &Sub : ContainerParams.SubComponentsArray)
    {
        if (Sub.value("TYPE", std::string()) != "FileEdit") continue;
        if (Sub.value("OVERRIDE", false) != OverridePass) continue;

        std::string Mode  = Sub.value("MODE",  std::string());
        std::string File  = Sub.value("FILE",  std::string());
        std::string Value = Sub.value("VALUE", std::string());
        std::filesystem::path FilePath = BasePath / File;

        if (Mode == "ConfigWrite")
            FileEdits::ConfigWrite(Sub.value("KEY", std::string()), Value, FilePath);
        else if (Mode == "Overwrite")
            FileEdits::FileOverwrite(Value, FilePath);
        else
            LogWarn("FileEdits::ProcessFileEdits", "Unknown MODE: '" + Mode + "' — skipping.");
    }
    return true;
}

//Rewrites FilePath in-place: any line whose content starts with Key is replaced by Key+Value.
//All other lines are preserved verbatim. Used to patch INI-style config files where
//the key acts as a line prefix (e.g. "Resolution=") rather than a standalone token.
//Returns false if the file cannot be opened for reading or writing.
bool FileEdits::ConfigWrite(std::string Key, std::string Value, std::filesystem::path FilePath)
{
    LogOut("FileEdits::ConfigWrite", "FilePath: " + FilePath.string() + " Key: " + Key + " Value: " + Value);
    std::ifstream inFile(FilePath);
    if (!inFile.is_open())
    {
        LogErr("FileEdits::ConfigWrite", "Could not open file for reading: " + FilePath.string());
        return false;
    }

    //Read all lines first so the file can be truncated and rewritten without a temporary file.
    std::vector<std::string> lines;
    std::string line;

    while (std::getline(inFile, line))
    {
        lines.push_back(line);
    }

    inFile.close();

    std::ofstream outFile(FilePath, std::ios::trunc);
    if (!outFile.is_open())
    {
        LogErr("FileEdits::ConfigWrite", "Could not open file for writing: " + FilePath.string());
        return false;
    }

    for (auto& currentLine : lines)
    {
        //Match by prefix: if the line starts with Key, replace the whole line with Key+Value.
        if (currentLine.length() >= Key.length() && currentLine.compare(0, Key.length(), Key) == 0)
        {
            currentLine = Key + Value;
            outFile << currentLine << '\n';
        }
        else
        {
            outFile << currentLine << '\n';
        }
    }

    outFile.close();
    return true;
}

//Writes Value as the complete content of FilePath, creating parent directories if needed.
//Used for files whose entire content is the value (e.g. Warcraft III .w3k CD key files).
bool FileEdits::FileOverwrite(const std::string &Value, const std::filesystem::path &FilePath)
{
    LogOut("FileEdits::FileOverwrite", "Writing: " + FilePath.string());
    std::error_code ec;
    std::filesystem::create_directories(FilePath.parent_path(), ec);
    if (ec) { LogErr("FileEdits::FileOverwrite", "Could not create parent dirs: " + ec.message()); return false; }
    std::ofstream Out(FilePath, std::ios::out | std::ios::trunc);
    if (!Out) { LogErr("FileEdits::FileOverwrite", "Could not open for writing: " + FilePath.string()); return false; }
    Out << Value;
    return true;
}