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

//The on-disk half of that check, for a path PathEscapesBase let through: true (Why set) when writing Normalised would
//follow a link out of Base — a link on the way, the file itself included, that leads nowhere (written through, it
//would create the file it names) or resolves outside Base; or something on the way that is neither a plain file nor
//a folder (a FIFO the game made would block the edit's open for good). Kept folders are served live, links included (Proton keeps its user-path links there), and the game
//can leave one there pointing at any file of the user's; the edit passes run outside the sandbox. A link that stays
//inside Base (Proton's "My Documents" -> "Documents") is followed as before.
bool LinkLeadsOut(const std::filesystem::path &Base, const std::filesystem::path &Normalised, std::string &Why);

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

//Sets one value in a JSON file. Pointer is a JSON Pointer (RFC 6901: "/video/resolution/width"); Value is JSON text
//(1920, true, "x", [1,2]) or, when it does not parse as JSON, a plain string. Objects missing on the way are created,
//and so is the file (absent or empty = {}). Key order is kept; comments are read but not written back. A file that
//is not JSON, or a pointer that runs through a value that is not an object/array, fails and leaves the file alone.
[[nodiscard]] bool JsonSet(const std::string &Pointer, const std::string &Value, const std::filesystem::path &FilePath);

//Writes Value as the complete content of FilePath, creating parent dirs if needed.
[[nodiscard]] bool FileOverwrite(const std::string &Value, const std::filesystem::path &FilePath);

//Appends Value as its own line at the end of FilePath, IDEMPOTENTLY (skips if an identical line already exists).
//Creates the file + parent dirs if absent, and inserts a separating newline if the file didn't end with one. The
//load-order primitive: each enabled node appends in recipe (topological) order, so the resulting config's order
//(openmw.cfg `content=`, a mod manifest, …) falls out of the node graph automatically.
[[nodiscard]] bool AppendLine(const std::string &Value, const std::filesystem::path &FilePath);
//The kept place (a kept folder's or kept file's runtime path) that removing FilePath — a resolved path under the
//runtime — would reach: the place itself, something inside it, a folder holding it, or a file a kept pattern matches;
//"" when none. Judged on the resolved path, lexically and through the links that exist in the runtime, so "a/../Saves",
//another anchor, a %variable% or a link to the folder all name the same place. (On Windows, MinGW's filesystem cannot
//see links, so a junction there is not resolved.)
std::string KeptPathMet(const struct ContainerParams &ContainerParams, const std::filesystem::path &FilePath);
//Removes FilePath (a file, or a folder and everything in it) — through the mounted union, where the file system keeps
//the removal as a whiteout in the session's write layer and the content beneath stays pristine. A link is removed as a
//link, never followed. False when nothing is there to remove (a typo, or content that changed beneath it).
[[nodiscard]] bool DeletePath(const std::filesystem::path &FilePath);
}

#endif // FILEEDITS_H
