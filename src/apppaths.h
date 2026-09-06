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

// The four run modes that decide AppDataDir + which daemon/persistence features are allowed (see the four-run-modes
// note). Set once in main() alongside the data root.
//   Normal    — default ~/.VidyaGod, no special path args: daemon YES, start-on-login YES.
//   Portable  — portable.txt / VidyaGod_data beside the app → app dir is the data dir: daemon YES, login NO.
//   InPackage — run inside a package dir (PreLaunchWindow only): daemon NO, login NO (run the game + exit).
//   CliPaths  — --data-dir / explicit CLI paths: daemon possible if it reaches the GUI, login NO.
enum class Mode { Normal, Portable, InPackage, CliPaths };
void SetMode(Mode M);
Mode GetMode();

bool DaemonSupported();      // tray / stay-resident — allowed unless In-package.
bool AutostartSupported();   // start-on-login (XDG autostart) — Normal mode only, and always false on Windows (no registry-Run backend yet).

// Optional per-launch path overrides (CLI: --package-dir / --runtime-dir / --userdata-dir; in-package sets all three
// = the package dir). Empty = derive normally (the default). Read by the launch engine: PackagePath in
// InitializeFromNode, RuntimePath + UserDataPath in DerivePaths. (--data-dir is handled via SetDataRoot above.)
void SetPackagePathOverride(const std::string & Path);
void SetRuntimePathOverride(const std::string & Path);
void SetUserDataPathOverride(const std::string & Path);
std::filesystem::path PackagePathOverride();    // empty if unset
std::filesystem::path RuntimePathOverride();     // empty if unset
std::filesystem::path UserDataPathOverride();    // empty if unset
}

#endif // APPPATHS_H
