#include "apppaths.h"

#include <QDir>

namespace
{
//Process-global, set once at startup. Empty until SetDataRoot() runs (then DataRoot() falls back to ~/.VidyaGod).
std::filesystem::path &RootRef()
{
    static std::filesystem::path Root;
    return Root;
}
}

namespace AppPaths
{
void SetDataRoot(const std::string &Path) { RootRef() = std::filesystem::path(Path); }

std::filesystem::path DataRoot()
{
    if (!RootRef().empty()) return RootRef();
    return std::filesystem::path(QDir::homePath().toStdString()) / ".VidyaGod";
}
}
