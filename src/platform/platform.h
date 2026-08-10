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

namespace Platform {

// Absolute path of the running executable's own file, or an empty path on failure. Used to find
// files bundled beside the binary (the vidyagodfs helper, dependency assets) without needing Qt —
// so it works in headless mode before any QApplication exists.
//   POSIX:   read_symlink("/proc/self/exe")
//   Windows: GetModuleFileNameW(nullptr, …)
std::filesystem::path SelfExe();

} // namespace Platform

#endif // VIDYAGOD_PLATFORM_PLATFORM_H
