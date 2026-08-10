// POSIX implementation of the Platform OS-abstraction seam (Linux). The Windows build compiles
// platform_win.cpp instead (selected in CMakeLists).
#include "platform.h"

#include <system_error>

namespace Platform {

std::filesystem::path SelfExe()
{
    std::error_code ec;
    return std::filesystem::read_symlink("/proc/self/exe", ec);   // empty path on error (ec set)
}

} // namespace Platform
