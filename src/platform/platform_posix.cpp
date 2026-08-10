// POSIX implementation of the Platform OS-abstraction seam (Linux). The Windows build compiles
// platform_win.cpp instead (selected in CMakeLists).
#include "platform.h"

#include <system_error>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace Platform {

std::filesystem::path SelfExe()
{
    std::error_code ec;
    return std::filesystem::read_symlink("/proc/self/exe", ec);   // empty path on error (ec set)
}

bool AcquireSingleInstanceLock(const std::string &Key)
{
    // O_CLOEXEC is essential: spawned children (wine/proton/git/vidyagodfs) must NOT inherit this fd,
    // or a lingering wine background process (services.exe/wineserver/…) keeps the flock held after
    // VidyaGod exits, making every later run falsely abort with "another instance is already running".
    int Fd = ::open(Key.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (Fd < 0) return false;
    if (::flock(Fd, LOCK_EX | LOCK_NB) != 0) { ::close(Fd); return false; }
    return true;   // Fd intentionally leaked — the kernel releases the lock on process exit/crash
}

} // namespace Platform
