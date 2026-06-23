#ifndef APPPATHS_H
#define APPPATHS_H

#include <string>
#include <filesystem>

// ---------------------------------------------------------------------------
// AppPaths — the single source of truth for the app data root: the directory every VidyaGod-produced path hangs
// off of (the IPFS repo, the instance lock, the LIBRARY default, and the launch TEMP/RUNTIME/DEFPREFIX). Resolved
// ONCE at startup by main() — normally ~/.VidyaGod, or VidyaGod_data beside the app in portable mode, or the
// --data-dir override — and read everywhere else through DataRoot(). Rooting everything here means a single delete
// is a true clean slate, and portable / --data-dir relocate the WHOLE footprint (previously the launch runtime
// hardcoded ~/.VidyaGod/TEMP and leaked out of a portable / --data-dir instance).
//
// USERDATA is the deliberate exception: per-package save data lives beside the package bundle (PackagePath/USERDATA),
// not under the app data root, so it travels with the package and survives a data-root wipe.
// ---------------------------------------------------------------------------
namespace AppPaths
{
//Records the resolved app data root. Call once, early in main(), after the root is determined.
void SetDataRoot(const std::string &Path);

//The app data root. Falls back to ~/.VidyaGod if SetDataRoot was never called (headless tooling / safety net).
std::filesystem::path DataRoot();
}

#endif // APPPATHS_H
