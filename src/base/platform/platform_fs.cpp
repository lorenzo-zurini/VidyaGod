// The host's file system, link-aware, on every OS (platform_fs.cpp).
#include "platform/platform.h"

#include <functional>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Platform {

bool IsLink(const std::filesystem::path &P)
{
#ifdef _WIN32
    const DWORD A = ::GetFileAttributesW(P.wstring().c_str());
    return A != INVALID_FILE_ATTRIBUTES && (A & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    std::error_code Ec;
    return std::filesystem::is_symlink(std::filesystem::symlink_status(P, Ec));
#endif
}

bool RemoveTree(const std::filesystem::path &Root, std::vector<std::string> &Errors)
{
    namespace fs = std::filesystem;
    const size_t Before = Errors.size();
    auto Remove = [&](const fs::path &P) {
#ifdef _WIN32
        //A link as the link: MinGW's remove() reports nothing for a dangling one and leaves it.
        if (IsLink(P))
        {
            const std::wstring W = P.wstring();
            if (!::RemoveDirectoryW(W.c_str()) && !::DeleteFileW(W.c_str()))
                Errors.push_back(P.string() + ": a link that cannot be removed (error " + std::to_string(::GetLastError()) + ")");
            return;
        }
#endif
        std::error_code Ec;
        if (fs::remove(P, Ec) || !Ec) return;
        fs::permissions(P, fs::perms::owner_write, fs::perm_options::add, Ec);   // a read-only file a game left
        Ec.clear();
        if (!fs::remove(P, Ec) && Ec) Errors.push_back(P.string() + ": " + Ec.message());
    };
    std::function<void(const fs::path &)> Empty = [&](const fs::path &D) {
        std::error_code Ec;
        std::vector<fs::path> Kids;
        for (fs::directory_iterator It(D, Ec), End; !Ec && It != End; It.increment(Ec)) Kids.push_back(It->path());
        if (Ec) { Errors.push_back(D.string() + ": " + Ec.message()); return; }
        for (const fs::path &K : Kids)
        {
            std::error_code Ie;
            if (!IsLink(K) && fs::is_directory(K, Ie)) Empty(K);
            Remove(K);   // a link: the link itself, never what it names
        }
    };
    std::error_code Ec;
    if (!fs::exists(Root, Ec) && !IsLink(Root)) return true;
    if (!IsLink(Root) && fs::is_directory(Root, Ec)) Empty(Root);
    Remove(Root);
    return Errors.size() == Before;
}

} // namespace Platform
