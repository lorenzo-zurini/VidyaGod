#ifndef VIDYAGOD_PLATFORM_PLATFORM_H
#define VIDYAGOD_PLATFORM_PLATFORM_H

// ---------------------------------------------------------------------------
// Platform — the app's thin OS-abstraction seam: the scattered host-specific bits that differ
// between Linux and Windows, centralized so the rest of the app stays platform-neutral. The POSIX
// impl (platform_posix.cpp) is compiled on Linux; a Windows impl (platform_win.cpp) is added for
// the Windows port, selected at compile time in CMake. NOTE: "Platform" here means the *host OS*
// — distinct from a node's PLATFORM (win64/linux64) in the launch graph.
// ---------------------------------------------------------------------------

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace Platform {

// Absolute path of the running executable's own file, or an empty path on failure. Used to find
// files bundled beside the binary (the vidyagodfs helper, dependency assets) without needing Qt —
// so it works in headless mode before any QApplication exists.
//   POSIX:   read_symlink("/proc/self/exe")
//   Windows: GetModuleFileNameW(nullptr, …)
std::filesystem::path SelfExe();

// Single-instance guard: try to become the sole running instance, keyed by `Key` (a stable
// per-user string — the app passes its lock-file path). Returns true if acquired (held until the
// process exits — intentionally never released early, mirroring the lock fd/handle lifetime), false
// if another live instance already holds it (or the guard couldn't be established).
//   POSIX:   open(Key) + flock(LOCK_EX|LOCK_NB); the fd is leaked, so the kernel releases the lock
//            automatically on exit OR crash (no stale lock survives a crashed instance).
//   Windows: a named mutex derived from Key; ERROR_ALREADY_EXISTS ⇒ another instance holds it.
// On failure, *Holder (when non-null) receives a human-readable description of who holds the lock
// ("pid 30614: ./build/VidyaGod --node wipeout_xl_mp"), or "" when it cannot be determined. This exists because a
// held lock with no name attached cost an hour of blind process-hunting during the 2026-09-01 overnight session —
// "another instance is running" while every pgrep pattern missed the actual holder.
bool AcquireSingleInstanceLock(const std::string &Key, std::string *Holder = nullptr);

// What a mount-point junction at Path names, as the junction stores it (a WinFsp mount: "\\Device\\Volume{…}\\"), or ""
// when Path is not one. Reads the junction itself, never what it names, so it answers for one whose volume is gone.
//   POSIX:   always "" (FUSE mounts are not junctions)
//   Windows: FSCTL_GET_REPARSE_POINT, IO_REPARSE_TAG_MOUNT_POINT's substitute name; for a drive root ("W:\\"), the
//            device the letter names (QueryDosDevice: "\\Device\\Volume{…}")
std::string MountPointTarget(const std::filesystem::path &Path);

// The machine's own profile and system folders, by name: UserProfile, AppData, LocalAppData, Documents, ProgramData,
// Public, Windows, SysDir32 (the 32-bit system folder: SysWOW64 on a 64-bit Windows), SysDir64, ProgramFiles,
// ProgramFiles32. A windows-native game reads its anchors as these real paths (its sandbox box holds what the package
// places there). Read once per process; a folder the system does not report is left out.
//   POSIX:   empty (no Windows folders)
//   Windows: SHGetKnownFolderPath
std::map<std::string, std::string> HostFolders();

// The names HostFolders() can return, on every platform: a validator on Linux knows %HostAppData% & co. as built-in
// tokens though only Windows sets them.
inline const std::vector<std::string> &HostFolderNames()
{
    static const std::vector<std::string> Names = {"UserProfile", "AppData", "LocalAppData", "Documents", "ProgramData",
                                                   "Public", "Windows", "SysDir32", "SysDir64", "ProgramFiles", "ProgramFiles32"};
    return Names;
}

// A drive letter no volume holds ("Z:", searching Z down to D), other than Except; "" when none is free. Windows mounts
// a game's runtime at one: a Sandboxie box cannot load a program's DLLs from a volume mounted on a folder (measured:
// LoadLibrary fails "module not found" inside the box, works outside it and from a drive letter).
//   POSIX:   "" (FUSE mounts on folders)
//   Windows: GetLogicalDrives
std::string FreeDriveLetter(const std::string &Except = std::string());

} // namespace Platform

#endif // VIDYAGOD_PLATFORM_PLATFORM_H
