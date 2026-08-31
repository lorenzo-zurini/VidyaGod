#include "fileedits.h"
#include "commonutils.h"   // Log*
#include "varsubst.h"      // %variable% substitution in FileEdit VALUEs (e.g. config_info's %RunnerMount%/%TempPath%)

#include <filesystem>
#include <fstream>
#include <sstream>
#include <iterator>
#include <string>

//Collects DLLOVERRIDE values from all DllOverride subcomponents into DLLOverrides.
//The collected strings are later joined and assigned to WINEDLLOVERRIDES in Execute().
//Returns false immediately if any DllOverride subcomponent has a null DLLOVERRIDE field.
bool FileEdits::ProcessDLLOverrides(struct ContainerParams &ContainerParams)
{
    LogOut("FileEdits::ProcessDLLOverrides", "Processing DLL Overrides.");
    bool Ok = true;
    for (const auto &SubComponentJSON : ContainerParams.SubComponentsArray)
    {
        if (SubComponentJSON.value("TYPE", std::string()) != "DllOverride") continue;
        // A null DLLOVERRIDE is malformed package data: report it but keep collecting the valid ones —
        // abandoning the loop mid-way used to silently drop every override after the bad entry.
        if (SubComponentJSON.contains("DLLOVERRIDE") && !SubComponentJSON["DLLOVERRIDE"].is_null())
            ContainerParams.DLLOverrides.push_back(SubComponentJSON["DLLOVERRIDE"]);
        else
        {
            LogErr("FileEdits::ProcessDLLOverrides", "DllOverride with null/missing DLLOVERRIDE — skipped.");
            Ok = false;
        }
    }
    return Ok;
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

    // FILE and VALUE are %variable%-substituted (like layer TARGETs / runner ENV), so a FileEdit can write runtime-
    // derived content — e.g. proton's config_info marker whose paths are "%RunnerMount%/files/..." / "%TempPath%".
    const std::map<std::string, std::string> Vars = ContainerParams.GetVariablesMap();

    const std::string PassName = OverridePass ? "OVERRIDE" : "BASE";
    bool Ok = true;
    size_t Attempted = 0, Failed = 0;
    for (auto &Sub : ContainerParams.SubComponentsArray)
    {
        if (Sub.value("TYPE", std::string()) != "FileEdit") continue;
        if (Sub.value("OVERRIDE", false) != OverridePass) continue;

        std::string Mode  = Sub.value("MODE",  std::string());
        std::string File  = Sub.value("FILE",  std::string());
        std::string Value = Sub.value("VALUE", std::string());
        const std::string Key = Sub.value("KEY", std::string());
        VarSubst::StringVariableSubstitution(File,  Vars);
        VarSubst::StringVariableSubstitution(Value, Vars);
        std::filesystem::path FilePath = BasePath / File;
        ++Attempted;

        //An ABSOLUTE FILE silently escapes this pass: BasePath / File discards BasePath entirely when File is
        //absolute, so the edit lands somewhere the pass does not own (usually a path that does not exist yet).
        //Worms 4 shipped exactly this and printed one line nobody read.
        if (std::filesystem::path(File).is_absolute())
            LogWarn("FileEdits::ProcessFileEdits",
                    "FILE '" + File + "' is ABSOLUTE — it discards the " + PassName + " pass base ('"
                    + BasePath.string() + "'). Author it relative to the pass base instead.");

        bool EditOk = true;
        if (Mode == "ConfigWrite")
            EditOk = FileEdits::ConfigWrite(Key, Value, FilePath);
        else if (Mode == "Overwrite")
            EditOk = FileEdits::FileOverwrite(Value, FilePath);
        else if (Mode == "AppendLine")
            EditOk = FileEdits::AppendLine(Value, FilePath);
        else
        {
            LogWarn("FileEdits::ProcessFileEdits", "Unknown MODE: '" + Mode + "' — skipping.");
            EditOk = false;
        }
        if (!EditOk)
        {
            ++Failed;
            Ok = false;
            //Name the edit, not just the symptom. "Could not open file for reading" on its own gives no clue
            //WHICH package setting was lost.
            LogErr("FileEdits::ProcessFileEdits",
                   PassName + " FileEdit FAILED — mode=" + (Mode.empty() ? "(none)" : Mode)
                   + (Key.empty() ? std::string() : (" key='" + Key + "'"))
                   + " file='" + FilePath.string() + "'");
            //The single most common cause, and the one that is invisible without being told: a ConfigWrite reads
            //the file before rewriting it, and zip-supplied content only exists once the runtime is mounted.
            if (Mode == "ConfigWrite" && !OverridePass)
                LogErr("FileEdits::ProcessFileEdits",
                       "   ^ ConfigWrite in the BASE pass runs BEFORE the content is mounted, so the file does not "
                       "exist yet. Add \"OVERRIDE\": true to this FileEdit.");
        }
    }
    if (Attempted > 0)
        LogOut("FileEdits::ProcessFileEdits",
               PassName + " pass: " + std::to_string(Attempted - Failed) + "/" + std::to_string(Attempted)
               + " edit(s) applied" + (Failed ? (", " + std::to_string(Failed) + " FAILED") : std::string()));
    return Ok;
}

//Rewrites FilePath in-place: any line whose content starts with Key is replaced by Key+Value.
//All other lines are preserved verbatim. Used to patch INI-style config files where
//the key acts as a line prefix (e.g. "Resolution=") rather than a standalone token.
//Returns false if the file cannot be opened for reading or writing.
bool FileEdits::ConfigWrite(const std::string &Key, const std::string &Value, const std::filesystem::path &FilePath)
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

    size_t Matched = 0;
    for (auto& currentLine : lines)
    {
        //Match by prefix: if the line starts with Key, replace the whole line with Key+Value.
        if (currentLine.length() >= Key.length() && currentLine.compare(0, Key.length(), Key) == 0)
        {
            currentLine = Key + Value;
            ++Matched;
            outFile << currentLine << '\n';
        }
        else
        {
            outFile << currentLine << '\n';
        }
    }

    outFile.close();
    //A KEY that matches NOTHING rewrites the file unchanged and reports success — a typo in the key, or a config
    //whose format changed, then silently does nothing at all. This is the same class of failure as the base-pass
    //ConfigWrite: the edit "worked", the setting never applied. Say so.
    if (Matched == 0)
        LogWarn("FileEdits::ConfigWrite",
                "key '" + Key + "' matched NO line in " + FilePath.string()
                + " — the file was rewritten unchanged and this setting had no effect. Check the key spelling "
                  "(it is matched as a literal line PREFIX, including any spaces around '=').");
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

bool FileEdits::AppendLine(const std::string &Value, const std::filesystem::path &FilePath)
{
    std::error_code Ec;
    std::filesystem::create_directories(FilePath.parent_path(), Ec);   // no-op for an existing dir

    // Read the current content: check for an identical line (idempotency) and whether it ends with a newline.
    std::string Content;
    {
        std::ifstream In(FilePath, std::ios::binary);
        if (In) Content.assign(std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>());
    }
    {
        std::istringstream Ss(Content);
        std::string Line;
        while (std::getline(Ss, Line))
        {
            if (!Line.empty() && Line.back() == '\r') Line.pop_back();   // tolerate CRLF
            if (Line == Value) { LogOut("FileEdits::AppendLine", "already present in " + FilePath.string() + ": " + Value); return true; }
        }
    }

    std::ofstream Out(FilePath, std::ios::app);
    if (!Out) { LogErr("FileEdits::AppendLine", "Could not open for append: " + FilePath.string()); return false; }
    if (!Content.empty() && Content.back() != '\n') Out << '\n';        // don't fuse onto an unterminated last line
    Out << Value << '\n';
    LogOut("FileEdits::AppendLine", "Appended to " + FilePath.string() + ": " + Value);
    return true;
}