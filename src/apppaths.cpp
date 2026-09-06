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
AppPaths::Mode &ModeRef()
{
    static AppPaths::Mode M = AppPaths::Mode::Normal;
    return M;
}
std::filesystem::path &PkgOverrideRef()      { static std::filesystem::path P; return P; }
std::filesystem::path &RuntimeOverrideRef()  { static std::filesystem::path P; return P; }
std::filesystem::path &UserDataOverrideRef() { static std::filesystem::path P; return P; }
}

namespace AppPaths
{
void SetDataRoot(const std::string &Path) { RootRef() = std::filesystem::path(Path); }

std::filesystem::path DataRoot()
{
    if (!RootRef().empty()) return RootRef();
    return std::filesystem::path(QDir::homePath().toStdString()) / ".VidyaGod";
}

void SetMode(Mode M)  { ModeRef() = M; }
Mode GetMode()        { return ModeRef(); }
bool DaemonSupported()    { return ModeRef() != Mode::InPackage; }
// Autostart is the XDG .desktop login mechanism (src/autostart.cpp). It has NO Windows implementation yet — writing
// to GenericConfigLocation/autostart on Windows produces a file the OS never runs, so the toggle would fabricate a
// login entry that silently does nothing. Report unsupported on Windows until a registry Run-key backend exists, so
// the UI honestly disables the control rather than confirming a lie. (Registry-Run Windows autostart = follow-up.)
bool AutostartSupported()
{
#ifdef _WIN32
    return false;
#else
    return ModeRef() == Mode::Normal;
#endif
}

void SetPackagePathOverride(const std::string & Path)  { PkgOverrideRef()      = std::filesystem::path(Path); }
void SetRuntimePathOverride(const std::string & Path)  { RuntimeOverrideRef()  = std::filesystem::path(Path); }
void SetUserDataPathOverride(const std::string & Path) { UserDataOverrideRef() = std::filesystem::path(Path); }
std::filesystem::path PackagePathOverride()  { return PkgOverrideRef(); }
std::filesystem::path RuntimePathOverride()  { return RuntimeOverrideRef(); }
std::filesystem::path UserDataPathOverride() { return UserDataOverrideRef(); }
}
