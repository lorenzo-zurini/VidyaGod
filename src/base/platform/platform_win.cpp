// Windows implementation of the Platform OS-abstraction seam. The Linux build compiles
// platform_posix.cpp instead (selected in CMakeLists).
#include "platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>   // FSCTL_GET_REPARSE_POINT
#include <shlobj.h>     // SHGetKnownFolderPath
#include <knownfolders.h>

#include <cctype>
#include <cstring>

#include <string>
#include <functional>

namespace Platform {

std::string FileStamp(const std::filesystem::path &P, long long *Modified)
{
    if (Modified) *Modified = 0;
    const auto U64 = [](DWORD Hi, DWORD Lo) { return (static_cast<unsigned long long>(Hi) << 32) | Lo; };
    //FILETIME counts 100 ns from 1601; the Unix epoch is 11644473600 s later.
    const auto Seconds = [](unsigned long long T) { return static_cast<long long>(T / 10000000ULL) - 11644473600LL; };
    //Opened for its attributes alone (any file another process holds, even exclusively for writing): the change time —
    //which a copy keeping the modification time (robocopy, an unzip) still moves — and the file's index on its volume
    //(the inode: a file replaced by another of the same size and times shows).
    const HANDLE H = ::CreateFileW(P.wstring().c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (H != INVALID_HANDLE_VALUE)
    {
        BY_HANDLE_FILE_INFORMATION I {};
        FILE_BASIC_INFO B {};
        const bool Ok = ::GetFileInformationByHandle(H, &I) && ::GetFileInformationByHandleEx(H, FileBasicInfo, &B, sizeof B);
        ::CloseHandle(H);
        if (Ok)
        {
            const unsigned long long Write = static_cast<unsigned long long>(B.LastWriteTime.QuadPart);
            if (Modified) *Modified = Seconds(Write);
            return std::to_string(U64(I.nFileSizeHigh, I.nFileSizeLow)) + ' ' + std::to_string(Write) + ' '
                   + std::to_string(static_cast<unsigned long long>(B.ChangeTime.QuadPart)) + ' '
                   + std::to_string(static_cast<unsigned long long>(B.CreationTime.QuadPart)) + ' '
                   + std::to_string(I.dwVolumeSerialNumber) + ':' + std::to_string(U64(I.nFileIndexHigh, I.nFileIndexLow));
        }
    }
    //Not openable at all (no access to its attributes): what the directory lists for it.
    WIN32_FILE_ATTRIBUTE_DATA A {};
    if (!GetFileAttributesExW(P.wstring().c_str(), GetFileExInfoStandard, &A)) return {};
    if (Modified) *Modified = Seconds(U64(A.ftLastWriteTime.dwHighDateTime, A.ftLastWriteTime.dwLowDateTime));
    return std::to_string(U64(A.nFileSizeHigh, A.nFileSizeLow)) + ' ' + std::to_string(U64(A.ftLastWriteTime.dwHighDateTime, A.ftLastWriteTime.dwLowDateTime))
           + ' ' + std::to_string(U64(A.ftCreationTime.dwHighDateTime, A.ftCreationTime.dwLowDateTime));
}

void ReleaseFreedMemory()
{
    HeapCompact(GetProcessHeap(), 0);
}

std::filesystem::path SelfExe()
{
    wchar_t buf[MAX_PATH];
    DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};   // empty on error / truncation
    return std::filesystem::path(std::wstring(buf, n));
}

ScopedFileLock::ScopedFileLock(const std::filesystem::path &File)
{
    HANDLE H = ::CreateFileW(File.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (H == INVALID_HANDLE_VALUE) return;
    Handle = reinterpret_cast<std::intptr_t>(H);
    OVERLAPPED O{};
    Held = ::LockFileEx(H, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &O) != 0;
}

ScopedFileLock::~ScopedFileLock()
{
    if (Handle != -1) ::CloseHandle(reinterpret_cast<HANDLE>(Handle));   // closing the handle releases the lock
}

bool AcquireSingleInstanceLock(const std::string &Key, std::string *Holder)
{
    // A session-local named mutex derived from Key (hashed to a bounded, name-safe token). The "Local\"
    // namespace scopes it to the user's session — matching the POSIX per-lock-file semantics. If the
    // mutex already exists, another live instance holds it. The handle is intentionally leaked so the
    // mutex lives for this process's lifetime (Windows frees it on exit/crash — no stale lock survives).
    std::wstring name = L"Local\\VidyaGod-" + std::to_wstring(std::hash<std::string>{}(Key));
    HANDLE h = ::CreateMutexW(nullptr, TRUE, name.c_str());
    if (h == nullptr) return false;
    if (::GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (Holder) *Holder = "";   // named-mutex holders aren't queryable; the POSIX build names the pid
        return false;               // another instance already holds it
    }
    return true;   // handle leaked on purpose
}

std::string MountPointTarget(const std::filesystem::path &Path)
{
    const std::wstring P = Path.wstring();
    if ((P.size() == 2 || (P.size() == 3 && (P[2] == L'\\' || P[2] == L'/'))) && P[1] == L':')   // a drive root
    {
        wchar_t Dev[1024];
        const std::wstring Letter = P.substr(0, 2);
        const DWORD N = ::QueryDosDeviceW(Letter.c_str(), Dev, 1024);
        if (N == 0) return {};
        const int L = static_cast<int>(wcslen(Dev));
        const int M = ::WideCharToMultiByte(CP_UTF8, 0, Dev, L, nullptr, 0, nullptr, nullptr);
        std::string Out(M > 0 ? M : 0, '\0');
        if (M > 0) ::WideCharToMultiByte(CP_UTF8, 0, Dev, L, Out.data(), M, nullptr, nullptr);
        return Out;
    }
    const DWORD A = ::GetFileAttributesW(P.c_str());
    if (A == INVALID_FILE_ATTRIBUTES || !(A & FILE_ATTRIBUTE_DIRECTORY) || !(A & FILE_ATTRIBUTE_REPARSE_POINT)) return {};
    HANDLE H = ::CreateFileW(P.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (H == INVALID_HANDLE_VALUE) return {};
    alignas(8) char Buf[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
    DWORD Got = 0;
    const BOOL Ok = ::DeviceIoControl(H, FSCTL_GET_REPARSE_POINT, nullptr, 0, Buf, sizeof Buf, &Got, nullptr);
    ::CloseHandle(H);
    if (!Ok || Got < 16) return {};
    //REPARSE_DATA_BUFFER, MountPointReparseBuffer: Tag, DataLength, Reserved, SubstituteNameOffset/Length, … PathBuffer.
    DWORD Tag; std::memcpy(&Tag, Buf, sizeof Tag);
    if (Tag != IO_REPARSE_TAG_MOUNT_POINT) return {};
    USHORT Off, Len; std::memcpy(&Off, Buf + 8, sizeof Off); std::memcpy(&Len, Buf + 10, sizeof Len);
    if (16u + Off + Len > Got) return {};
    //The junction's maker chose these bytes: a lone surrogate must not throw (path::string() does), it reads as U+FFFD.
    const int WLen = static_cast<int>(Len / sizeof(wchar_t));
    std::wstring W(WLen, L'\0');
    std::memcpy(W.data(), Buf + 16 + Off, WLen * sizeof(wchar_t));
    const int N = ::WideCharToMultiByte(CP_UTF8, 0, W.data(), WLen, nullptr, 0, nullptr, nullptr);
    std::string Out(N > 0 ? N : 0, '\0');
    if (N > 0) ::WideCharToMultiByte(CP_UTF8, 0, W.data(), WLen, Out.data(), N, nullptr, nullptr);
    return Out;
}

std::map<std::string, std::string> HostFolders()
{
    static const std::map<std::string, std::string> Cached = [] {   // the folders do not move while VidyaGod runs
    std::map<std::string, std::string> Out;
    const std::pair<const char *, const KNOWNFOLDERID *> Folders[] = {
        {"UserProfile", &FOLDERID_Profile},       {"AppData", &FOLDERID_RoamingAppData},
        {"LocalAppData", &FOLDERID_LocalAppData}, {"Documents", &FOLDERID_Documents},
        {"ProgramData", &FOLDERID_ProgramData},   {"Public", &FOLDERID_Public},
        {"Windows", &FOLDERID_Windows},           {"SysDir32", &FOLDERID_SystemX86},
        {"SysDir64", &FOLDERID_System},           {"ProgramFiles", &FOLDERID_ProgramFilesX64},
        {"ProgramFiles32", &FOLDERID_ProgramFilesX86}};
    for (const auto &[Name, Id] : Folders)
    {
        PWSTR W = nullptr;
        if (SUCCEEDED(::SHGetKnownFolderPath(*Id, KF_FLAG_DONT_VERIFY, nullptr, &W)) && W)
        {
            const int Len = static_cast<int>(wcslen(W));
            const int N = ::WideCharToMultiByte(CP_UTF8, 0, W, Len, nullptr, 0, nullptr, nullptr);
            std::string S(N > 0 ? N : 0, '\0');
            if (N > 0) ::WideCharToMultiByte(CP_UTF8, 0, W, Len, S.data(), N, nullptr, nullptr);
            if (!S.empty()) Out[Name] = S;
        }
        ::CoTaskMemFree(W);
    }
    return Out;
    }();
    return Cached;
}

std::string FreeDriveLetter(const std::string &Except)
{
    const DWORD Used = ::GetLogicalDrives();
    for (char C = 'Z'; C >= 'D'; --C)
    {
        const std::string L = std::string(1, C) + ":";
        if (!(Used & (1u << (C - 'A'))) && (Except.empty() || std::toupper(static_cast<unsigned char>(Except[0])) != C)) return L;
    }
    return {};
}

} // namespace Platform
