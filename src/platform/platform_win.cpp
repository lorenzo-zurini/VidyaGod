// Windows implementation of the Platform OS-abstraction seam. The Linux build compiles
// platform_posix.cpp instead (selected in CMakeLists).
#include "platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <functional>

namespace Platform {

std::filesystem::path SelfExe()
{
    wchar_t buf[MAX_PATH];
    DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};   // empty on error / truncation
    return std::filesystem::path(std::wstring(buf, n));
}

bool AcquireSingleInstanceLock(const std::string &Key)
{
    // A session-local named mutex derived from Key (hashed to a bounded, name-safe token). The "Local\"
    // namespace scopes it to the user's session — matching the POSIX per-lock-file semantics. If the
    // mutex already exists, another live instance holds it. The handle is intentionally leaked so the
    // mutex lives for this process's lifetime (Windows frees it on exit/crash — no stale lock survives).
    std::wstring name = L"Local\\VidyaGod-" + std::to_wstring(std::hash<std::string>{}(Key));
    HANDLE h = ::CreateMutexW(nullptr, TRUE, name.c_str());
    if (h == nullptr) return false;
    if (::GetLastError() == ERROR_ALREADY_EXISTS) return false;   // another instance already holds it
    return true;   // handle leaked on purpose
}

} // namespace Platform
