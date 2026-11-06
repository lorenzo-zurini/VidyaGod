// POSIX implementation of the Platform OS-abstraction seam (Linux). The Windows build compiles
// platform_win.cpp instead (selected in CMakeLists).
#if defined(__linux__)
#include <sys/stat.h>
#include <malloc.h>   // malloc_trim (glibc)
#endif
#include "platform.h"

#include <system_error>
#include <fstream>
#include <cstdlib>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace Platform {

std::string FileStamp(const std::filesystem::path &P, long long *Modified)
{
    struct stat St {};
    if (Modified) *Modified = 0;
    if (::stat(P.c_str(), &St) != 0) return {};
    if (Modified) *Modified = static_cast<long long>(St.st_mtim.tv_sec);
    return std::to_string(St.st_size) + ' ' + std::to_string(St.st_mtim.tv_sec) + '.' + std::to_string(St.st_mtim.tv_nsec) + ' '
           + std::to_string(St.st_ctim.tv_sec) + '.' + std::to_string(St.st_ctim.tv_nsec) + ' ' + std::to_string(St.st_ino);
}

void ReleaseFreedMemory()
{
#ifdef __GLIBC__
    malloc_trim(0);
#endif
}

std::filesystem::path SelfExe()
{
    std::error_code ec;
    return std::filesystem::read_symlink("/proc/self/exe", ec);   // empty path on error (ec set)
}

bool AcquireSingleInstanceLock(const std::string &Key, std::string *Holder)
{
    // O_CLOEXEC is essential: spawned children (wine/proton/git/vidyagodfs) must NOT inherit this fd,
    // or a lingering wine background process (services.exe/wineserver/…) keeps the flock held after
    // VidyaGod exits, making every later run falsely abort with "another instance is already running".
    int Fd = ::open(Key.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (Fd < 0) return false;
    if (::flock(Fd, LOCK_EX | LOCK_NB) != 0)
    {
        // NAME the holder. flock has no F_GETLK-style query, so the file carries the winner's pid (written
        // below on acquire). The pid is advisory — the flock is the authority — but it turns "another
        // instance is running" from a blind hunt into one glance.
        if (Holder)
        {
            *Holder = "";
            char Buf[32] = {};
            if (::pread(Fd, Buf, sizeof(Buf) - 1, 0) > 0)
            {
                const long Pid = ::strtol(Buf, nullptr, 10);
                if (Pid > 0)
                {
                    *Holder = "pid " + std::to_string(Pid);
                    std::ifstream Cmd("/proc/" + std::to_string(Pid) + "/cmdline");
                    std::string Line((std::istreambuf_iterator<char>(Cmd)), std::istreambuf_iterator<char>());
                    for (char &C : Line) if (C == '\0') C = ' ';
                    if (!Line.empty()) *Holder += ": " + Line;
                    else *Holder += " (process gone — the lock fd survives in one of its children; "
                                    "check `fuser " + Key + "`)";
                }
            }
        }
        ::close(Fd);
        return false;
    }
    // Record our pid for the holder report above. Failure is harmless (the report just stays empty).
    if (::ftruncate(Fd, 0) == 0)
    {
        const std::string Pid = std::to_string(::getpid()) + "\n";
        if (::pwrite(Fd, Pid.c_str(), Pid.size(), 0) < 0) { /* advisory only */ }
    }
    return true;   // Fd intentionally leaked — the kernel releases the lock on process exit/crash
}

std::string MountPointTarget(const std::filesystem::path &) { return {}; }

std::map<std::string, std::string> HostFolders() { return {}; }

std::string FreeDriveLetter(const std::string &) { return {}; }

} // namespace Platform
