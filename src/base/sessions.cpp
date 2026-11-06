// Sessions — where launches keep their scratch, and whether a game is using it: the session root, each session's
// owner lock, the runtimes this process holds, and the mounts under a folder (POSIX: mountinfo; Windows: WinFsp
// junctions). Read by the launch engine, the startup sweep, Settings → Storage and a package switch alike.
#include "sessions.h"
#include "commonutils.h"        // Log*
#include "platform/platform.h"  // MountPointTarget (Windows)

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <functional>
#include <set>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/file.h>   // flock (session ownership)
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

std::filesystem::path Sessions::Root()
{
    std::error_code TmpEc;
    std::string TempLeaf = "VidyaGod";
#ifndef _WIN32
    TempLeaf += "-" + std::to_string(static_cast<unsigned long>(getuid()));
#endif
    std::filesystem::path TempRoot = std::filesystem::temp_directory_path(TmpEc) / TempLeaf;
    if (TmpEc) TempRoot = std::filesystem::path("/tmp") / TempLeaf;
    return TempRoot;
}

namespace { std::atomic<int> LiveRuntimes{0}; }
Sessions::RuntimeHold::RuntimeHold() { ++LiveRuntimes; }
Sessions::RuntimeHold::~RuntimeHold() { --LiveRuntimes; }
int Sessions::RuntimesLive() { return LiveRuntimes.load(); }

#ifdef _WIN32
//WinFsp mounts a folder as a mount-point junction to its volume (\Device\Volume{…}) and removes it when the serving
//process exits. A machine that dies with the mount up keeps the junction, naming a volume that is gone, and every
//later launch of that game failed ("the mount folder already exists"). True if Path is such a junction.
bool Sessions::IsWinFspJunction(const std::wstring &Path)
{
    return Platform::MountPointTarget(Path).rfind("\\Device\\Volume{", 0) == 0;
}

//A WinFsp junction whose volume is gone: following it finds no path (measured: ERROR_PATH_NOT_FOUND). Any other
//failure (access denied, a helper too busy to answer) is not taken for death: the mount may be live.
bool Sessions::JunctionDead(const std::wstring &Path)
{
    HANDLE H = ::CreateFileW(Path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);   // follows the junction
    if (H != INVALID_HANDLE_VALUE) { ::CloseHandle(H); return false; }
    const DWORD E = ::GetLastError();
    return E == ERROR_PATH_NOT_FOUND || E == ERROR_FILE_NOT_FOUND;
}

//UTF-16 → UTF-8 that never throws (path::string() does on a lone surrogate, and a game names the folders under TEMP):
//such a name reads as U+FFFD, no longer names the folder, and so is never removed — the wipe is refused instead.
static std::string Narrow(const std::wstring &W)
{
    const int N = ::WideCharToMultiByte(CP_UTF8, 0, W.data(), static_cast<int>(W.size()), nullptr, 0, nullptr, nullptr);
    std::string Out(N > 0 ? N : 0, '\0');
    if (N > 0) ::WideCharToMultiByte(CP_UTF8, 0, W.data(), static_cast<int>(W.size()), Out.data(), N, nullptr, nullptr);
    return Out;
}

//Every WinFsp mount junction at or under Prefix, live or left by a dead machine, found without walking into one. (Other
//junctions are not followed here either, but remove_all would: not this function's guarantee.)
std::vector<std::string> Sessions::MountpointsUnder(const std::filesystem::path &Prefix)
{
    std::vector<std::string> Result;
    if (Prefix.empty()) return Result;
    std::function<void(const std::wstring &)> Walk = [&](const std::wstring &Dir) {
        if (IsWinFspJunction(Dir)) { Result.push_back(Narrow(Dir)); return; }
        const DWORD A = ::GetFileAttributesW(Dir.c_str());
        if (A == INVALID_FILE_ATTRIBUTES || !(A & FILE_ATTRIBUTE_DIRECTORY) || (A & FILE_ATTRIBUTE_REPARSE_POINT)) return;
        WIN32_FIND_DATAW F;
        HANDLE H = ::FindFirstFileW((Dir + L"\\*").c_str(), &F);
        if (H == INVALID_HANDLE_VALUE) return;
        do {
            const std::wstring N = F.cFileName;
            if (N == L"." || N == L".." || !(F.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            Walk(Dir + L"\\" + N);
        } while (::FindNextFileW(H, &F));
        ::FindClose(H);
    };
    Walk(Prefix.wstring());
    return Result;
}

std::vector<std::string> Sessions::LiveMountpointsUnder(const std::filesystem::path &Prefix)
{
    std::vector<std::string> Live;
    for (const std::string &M : MountpointsUnder(Prefix))
        if (!JunctionDead(std::filesystem::path(M).wstring())) Live.push_back(M);
    return Live;
}
#else
std::vector<std::string> Sessions::MountpointsUnder(const std::filesystem::path &Prefix)
{
    std::vector<std::string> Result;
    if (Prefix.empty()) return Result;

    std::ifstream Mounts("/proc/self/mountinfo");
    if (!Mounts.is_open()) return Result;

    //mountinfo escapes space/tab/newline/backslash as 3-digit octal (\040 etc.); decode them.
    auto Unescape = [](const std::string &In) -> std::string
    {
        std::string Out; Out.reserve(In.size());
        for (size_t i = 0; i < In.size(); ++i)
        {
            if (In[i] == '\\' && i + 3 < In.size()
                && In[i+1] >= '0' && In[i+1] <= '7'
                && In[i+2] >= '0' && In[i+2] <= '7'
                && In[i+3] >= '0' && In[i+3] <= '7')
            {
                Out.push_back(static_cast<char>((In[i+1]-'0')*64 + (In[i+2]-'0')*8 + (In[i+3]-'0')));
                i += 3;
            }
            else Out.push_back(In[i]);
        }
        return Out;
    };

    const std::string PrefixStr = Prefix.string();
    std::string Line;
    while (std::getline(Mounts, Line))
    {
        //Field 5 (0-indexed 4) of a mountinfo line is the mount point.
        std::istringstream Iss(Line);
        std::string Field, MountPoint;
        for (int Idx = 0; Iss >> Field; ++Idx) { if (Idx == 4) { MountPoint = Field; break; } }
        if (MountPoint.empty()) continue;
        MountPoint = Unescape(MountPoint);

        if (MountPoint == PrefixStr || MountPoint.rfind(PrefixStr + "/", 0) == 0)
            Result.push_back(MountPoint);
    }

    //Deepest-first (longest path first) so children unmount before their parents.
    std::sort(Result.begin(), Result.end(),
              [](const std::string &A, const std::string &B){ return A.size() > B.size(); });
    return Result;
}

std::vector<std::string> Sessions::LiveMountpointsUnder(const std::filesystem::path &Prefix)
{
    return MountpointsUnder(Prefix);   // a FUSE mount in mountinfo is live
}
#endif

std::filesystem::path Sessions::LockPath(const std::filesystem::path &TempPath)
{
    return TempPath.parent_path() / (TempPath.filename().string() + ".lock");
}

bool Sessions::SessionOwned(const std::filesystem::path &TempPath)
{
#ifdef _WIN32
    (void)TempPath;
    return false;
#else
    const int Fd = ::open(LockPath(TempPath).c_str(), O_RDONLY | O_CLOEXEC);
    if (Fd < 0) return false;
    const bool Held = ::flock(Fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK;
    ::close(Fd);
    return Held;
#endif
}

int Sessions::ClaimSession(const std::filesystem::path &TempPath)
{
#ifdef _WIN32
    (void)TempPath;
    return -1;
#else
    const std::filesystem::path Lock = LockPath(TempPath);
    std::error_code Ec;
    std::filesystem::create_directories(Lock.parent_path(), Ec);
    //A release unlinks the file after this open and before this flock: the lock taken is then on a file no one else
    //can find. Taken again on the file now at the path, so two launches never both own one folder.
    for (int Attempt = 0; Attempt < 3; ++Attempt)
    {
        //Close-on-exec: the game must not inherit it, or a helper the game leaves running would hold the session "live".
        const int Fd = ::open(Lock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (Fd < 0)
        {
            LogErr("Sessions::ClaimSession", "Could not create " + Lock.string() + ": " + std::strerror(errno));
            return -3;
        }
        if (::flock(Fd, LOCK_EX | LOCK_NB) != 0)
        {
            const int Err = errno;
            ::close(Fd);
            if (Err == EWOULDBLOCK) return -2;
            LogErr("Sessions::ClaimSession", "Could not lock " + Lock.string() + ": " + std::strerror(Err));
            return -3;
        }
        struct stat Held {}, Now {};
        if (::fstat(Fd, &Held) == 0 && ::stat(Lock.c_str(), &Now) == 0 && Held.st_dev == Now.st_dev && Held.st_ino == Now.st_ino)
            return Fd;
        ::close(Fd);
    }
    LogErr("Sessions::ClaimSession", "The lock " + Lock.string() + " kept changing under the claim.");
    return -3;
#endif
}

void Sessions::ReleaseSession(int &Fd, const std::filesystem::path &TempPath)
{
#ifndef _WIN32
    //Unlinked while held, then let go: a claim that opened the old file meanwhile sees it is no longer at the path.
    if (Fd >= 0) { ::unlink(LockPath(TempPath).c_str()); ::close(Fd); }
#else
    (void)TempPath;
#endif
    Fd = -1;
}

