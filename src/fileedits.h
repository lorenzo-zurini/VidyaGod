#ifndef FILEEDITS_H
#define FILEEDITS_H

#include <filesystem>
#include <string>

#include "launchparams.h"   // ContainerParams

// FileEdits — applies the package-encoded FileEdit / DllOverride subcomponents, lifted out of ContainerWrapper.
// Free functions over the shared ContainerParams (the subcomponents are already %VARIABLE%-substituted).
namespace FileEdits
{

//True when joining `Rel` onto `Base` lands OUTSIDE `Base`; `OutNormalised` receives the normalised join either
//way. Shared by FileEdit and BinaryPatch because a package arrives from a peer by CID — FILE is untrusted
//input — and because two copies of a security check is how one of them silently stops matching the other.
//Purely LEXICAL: it stops "..", an absolute path replacing the base, and a Windows drive-relative path. It
//does NOT resolve symlinks, so a link inside the mounted runtime is still followed.
bool PathEscapesBase(const std::filesystem::path &Joined, const std::filesystem::path &Base,
                     std::filesystem::path &OutNormalised);

//Collects DLLOVERRIDE values from all DllOverride subcomponents into ContainerParams.DLLOverrides (later joined
//into WINEDLLOVERRIDES at launch).
[[nodiscard]] bool ProcessDLLOverrides(struct ContainerParams &ContainerParams);

//Processes FileEdit subcomponents. OverridePass selects which edits run (the OVERRIDE flag must match it). BaseDir,
//when non-empty, is the directory matched edits are written under; otherwise it defaults to RuntimePath for the
//override pass and DefPrefixPath for the base pass.
[[nodiscard]] bool ProcessFileEdits(struct ContainerParams &ContainerParams, bool OverridePass = false,
                      const std::filesystem::path &BaseDir = {});

//Reads FilePath line by line and replaces any line starting with Key with Key+Value (prefix-matched INI patching).
[[nodiscard]] bool ConfigWrite(const std::string &Key, const std::string &Value, const std::filesystem::path &FilePath);

//Writes Value as the complete content of FilePath, creating parent dirs if needed.
[[nodiscard]] bool FileOverwrite(const std::string &Value, const std::filesystem::path &FilePath);

//Appends Value as its own line at the end of FilePath, IDEMPOTENTLY (skips if an identical line already exists).
//Creates the file + parent dirs if absent, and inserts a separating newline if the file didn't end with one. The
//load-order primitive: each enabled node appends in recipe (topological) order, so the resulting config's order
//(openmw.cfg `content=`, a mod manifest, …) falls out of the node graph automatically.
[[nodiscard]] bool AppendLine(const std::string &Value, const std::filesystem::path &FilePath);
}

#endif // FILEEDITS_H
