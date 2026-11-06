#ifndef SESSIONS_H
#define SESSIONS_H

#include <filesystem>
#include <string>
#include <vector>

// Sessions — where every launch keeps its scratch, and whether a game is using it.
namespace Sessions
{

//Where every launch's session scratch lives (<tmp>/VidyaGod-<uid>): a runtime mounted under it is a game running.
std::filesystem::path Root();

//Every current mountpoint at or beneath Prefix, deepest-first (children before parents). Linux: from /proc/self/mountinfo.
//Windows: every WinFsp mount junction (a folder naming \Device\Volume{…}), live or left by a dead machine.
std::vector<std::string> MountpointsUnder(const std::filesystem::path &Prefix);
//The ones of them a game is using now. Linux: all of them. Windows: MountpointsUnder also lists a WinFsp junction a dead
//machine left behind, which serves nothing and must not read as "a game is running".
std::vector<std::string> LiveMountpointsUnder(const std::filesystem::path &Prefix);

//A game runtime this process is building, running or cleaning up — held from the start of the build to the end of
//the unmounts, on every platform (mountinfo is Linux's alone). Anything that must not change content under a running
//game (switching a package's version) asks RuntimesLive().
class RuntimeHold
{
public:
    RuntimeHold();
    ~RuntimeHold();
    RuntimeHold(const RuntimeHold &) = delete;
    RuntimeHold &operator=(const RuntimeHold &) = delete;
};
int RuntimesLive();

//A session folder's owner: the launch that built it holds an exclusive lock on <TempPath>.lock (beside the folder, so
//clearing the folder never removes it) from before anything is cleared or mounted to the end of its Cleanup, and the
//kernel lets it go when that process dies however it dies. So a held lock is a live launch (of this VidyaGod or another,
//a second data root included: all of them share the session root), and a folder whose lock nobody holds is a dead one.
//ClaimSession takes the lock: the fd, -2 when another live launch holds it, -3 when no lock can be made there. ReleaseSession removes the lock file and
//lets it go (Fd becomes -1); a claim checks that the file it locked is still the one at the path, so a release racing it
//is not a second owner. Windows: no lock (-1), never owned — sessions there are cleared by their instance's next launch.
bool SessionOwned(const std::filesystem::path &TempPath);
//The lock file of a session folder: <TempPath>.lock, beside it.
std::filesystem::path LockPath(const std::filesystem::path &TempPath);
int  ClaimSession(const std::filesystem::path &TempPath);
void ReleaseSession(int &Fd, const std::filesystem::path &TempPath);

#ifdef _WIN32
//WinFsp mounts a folder as a mount-point junction to its volume; one a dead machine left behind names a volume that is
//gone (JunctionDead) and serves nothing.
bool IsWinFspJunction(const std::wstring &Path);
bool JunctionDead(const std::wstring &Path);
#endif

} // namespace Sessions

#endif // SESSIONS_H
