#ifndef FILEEDITS_H
#define FILEEDITS_H

#include <filesystem>
#include <string>

#include "launchparams.h"   // ContainerParams

// FileEdits — applies the package-encoded FileEdit / DllOverride subcomponents, lifted out of ContainerWrapper.
// Free functions over the shared ContainerParams (the subcomponents are already %VARIABLE%-substituted).
namespace FileEdits
{
//Collects DLLOVERRIDE values from all DllOverride subcomponents into ContainerParams.DLLOverrides (later joined
//into WINEDLLOVERRIDES at launch).
bool ProcessDLLOverrides(struct ContainerParams &ContainerParams);

//Processes FileEdit subcomponents. OverridePass selects which edits run (the OVERRIDE flag must match it). BaseDir,
//when non-empty, is the directory matched edits are written under; otherwise it defaults to RuntimePath for the
//override pass and DefPrefixPath for the base pass.
bool ProcessFileEdits(struct ContainerParams &ContainerParams, bool OverridePass = false,
                      const std::filesystem::path &BaseDir = {});

//Reads FilePath line by line and replaces any line starting with Key with Key+Value (prefix-matched INI patching).
bool ConfigWrite(std::string Key, std::string Value, std::filesystem::path FilePath);

//Writes Value as the complete content of FilePath, creating parent dirs if needed.
bool FileOverwrite(const std::string &Value, const std::filesystem::path &FilePath);
}

#endif // FILEEDITS_H
