#ifndef INSTANCESTORE_H
#define INSTANCESTORE_H

#include <string>
#include <vector>
#include <filesystem>

#include "nlohmann/json.hpp"

// ---------------------------------------------------------------------------
// InstanceStore — PrismLauncher-style per-game INSTANCES, the single owner of instance discovery + config I/O.
//
// An instance is one self-contained config + its own durable state for one game, living FLAT at
//   <root>/USERDATA/<PackageUID>/<InstanceName>/
//     ├─ instance.json    ← the config blob (ex-GlobalConfig LIBRARY[i].USERSETTINGS: VARIABLES / PREFERRED_RUNNER /
//     │                      RUNNER_CHAIN / MODULES / SKIP_LAUNCH_DIALOG, + soft SELECTED_VARIANT + LASTRUN)
//     ├─ <TARGET>/ …      ← one NAMED durable subdir per DeclarePersist (e.g. Saves/, AllData/, REGISTRY/, REGKEYS/)
// UserDataPath (the durable root) IS the instance dir; each persist maps a runtime PATH to <instdir>/<TARGET>.
// SAFETY: nothing ever mounts the instance-dir ROOT — every game-writable mount is a NAMED child (a persist TARGET),
// so instance.json at the root is never inside a mount and a sandboxed game can't read its secrets or tamper
// RUNNER_CHAIN/VARIABLES. (Ephemeral TEMP lives in /tmp, not here.) The PackageUID is SANITIZED to a single safe
// path segment (it is peer-authored) so it can never escape <root>.
//
// FILESYSTEM-AS-TRUTH: instances are DISCOVERED by scanning the package dir — there is no registry in GlobalConfig.
// If none exist, "DefaultInstance" is auto-created. The last-active / pre-selected instance is the one whose
// instance.json carries the newest LASTRUN. Deleting a dir just removes the instance.
//
// <root> = Settings.Paths.UserDataRoot, else AppPaths::DataRoot()/USERDATA (portable-aware). The CLI/in-package
// --userdata-dir override (AppPaths::UserDataPathOverride) points the exact userdata path and BYPASSES instances
// entirely (a self-contained portable runnable unit = one implicit instance) — the launch engine honours that.
// ---------------------------------------------------------------------------
namespace InstanceStore
{
inline constexpr const char *DefaultInstance = "DefaultInstance";
inline constexpr const char *ConfigFile      = "instance.json";

// The USERDATA root (Settings.Paths.UserDataRoot, else DataRoot()/USERDATA).
std::filesystem::path Root(const nlohmann::ordered_json &Cfg);
// <root>/<PackageUID> — the parent of a game's instances.
std::filesystem::path PackageDir(const nlohmann::ordered_json &Cfg, const std::string &PackageUID);
// <root>/<PackageUID>/<Instance> — the instance dir; this IS UserDataPath (persist TARGETs are its named subdirs).
std::filesystem::path InstanceDir(const nlohmann::ordered_json &Cfg, const std::string &PackageUID, const std::string &Instance);
// <root>/<PackageUID>/<Instance>/instance.json — the config, at the instance-dir root (never inside a persist TARGET).
std::filesystem::path ConfigPath(const nlohmann::ordered_json &Cfg, const std::string &PackageUID, const std::string &Instance);

// Reduce a peer-authored PackageUID to a single safe path segment (each char not in [A-Za-z0-9._-] → '_', and
// "."/".."/"" → "_"), so it can never contain a separator, "..", or an absolute path that escapes <root>.
std::string SanitizeUid(const std::string &PackageUID);

// Every instance of a game (subdirs of PackageDir), sorted NEWEST-LASTRUN-first (the order a picker should show).
// Empty when the game has no instances yet (the caller decides whether to auto-create).
std::vector<std::string> List(const nlohmann::ordered_json &Cfg, const std::string &PackageUID);

// The active instance = newest LASTRUN. AUTO-CREATES DefaultInstance (and returns it) when the game has none.
std::string Active(const nlohmann::ordered_json &Cfg, const std::string &PackageUID);

// The active instance NAME without creating anything: newest LASTRUN, or "DefaultInstance" if none exist yet.
// For PATH derivation (DerivePaths) — a dry resolve/audit must not materialise a dir; it is created lazily by the
// first config write or persist capture.
std::string ResolveActive(const nlohmann::ordered_json &Cfg, const std::string &PackageUID);

// Read an instance's config blob ({} if the file is absent or unparseable). Empty Instance ⇒ the active instance.
nlohmann::ordered_json ReadConfig(const nlohmann::ordered_json &Cfg, const std::string &PackageUID, const std::string &Instance = std::string());
// Atomically write an instance's config blob (creates the dir). Empty Instance ⇒ the active instance.
bool WriteConfig(const nlohmann::ordered_json &Cfg, const std::string &PackageUID, const std::string &Instance,
                 const nlohmann::ordered_json &Data, std::string *Error = nullptr);

// Lifecycle. Names are validated (ValidName); Create/Rename/Clone refuse an existing target; Delete rm-rf's the dir.
bool Create(const nlohmann::ordered_json &Cfg, const std::string &PackageUID, const std::string &Name, std::string *Error = nullptr);
bool Rename(const nlohmann::ordered_json &Cfg, const std::string &PackageUID, const std::string &From, const std::string &To, std::string *Error = nullptr);
bool Delete(const nlohmann::ordered_json &Cfg, const std::string &PackageUID, const std::string &Name, std::string *Error = nullptr);
bool Clone (const nlohmann::ordered_json &Cfg, const std::string &PackageUID, const std::string &From, const std::string &To, std::string *Error = nullptr);

// Stamp LASTRUN = now (UTC ISO-8601) on an instance's config — call when a launch actually fires. Empty ⇒ active.
void TouchLastRun(const nlohmann::ordered_json &Cfg, const std::string &PackageUID, const std::string &Instance = std::string());

// A filesystem-safe, single-segment instance name: non-empty, not "."/"..", no '/'/'\\', no control chars, ≤64 chars.
bool ValidName(const std::string &Name);
}

#endif // INSTANCESTORE_H
