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
#include <string>

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

} // namespace Platform

#endif // VIDYAGOD_PLATFORM_PLATFORM_H
