#ifndef PROCESSENV_H
#define PROCESSENV_H

#include <QProcessEnvironment>
#include <QStringList>

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// SystemToolEnv — environment for spawning SYSTEM binaries from inside an AppImage.
//
// The AppImage's AppRun launches VidyaGod with LD_LIBRARY_PATH=$APPDIR/usr/lib so the app finds its bundled Qt/
// libzip/libfuse. But that variable is inherited by every child process we spawn, and unrelated SYSTEM tools
// (git, ipfs, umu-run, fusermount3, …) then try to load the AppImage's bundled libraries instead of the host's
// and fail to even start. This returns a copy of Base with any LD_LIBRARY_PATH components under $APPDIR removed,
// so system tools resolve against the host's libraries. It is a no-op outside an AppImage (APPDIR unset).
//
// The bundled vidyagodfs helper is the deliberate exception — it links the bundled libs, so it is spawned with
// the unmodified (AppImage) environment, NOT through this.
// ---------------------------------------------------------------------------
inline QProcessEnvironment SystemToolEnv(QProcessEnvironment Env = QProcessEnvironment::systemEnvironment())
{
    if (!Env.contains("LD_LIBRARY_PATH")) return Env;
    const QString AppDir = Env.value("APPDIR");                                 // set by the AppImage runtime
    QStringList Kept;
    for (const QString &P : Env.value("LD_LIBRARY_PATH").split(':', Qt::SkipEmptyParts))
    {
        const bool UnderAppImage = (!AppDir.isEmpty() && P.startsWith(AppDir))   // $APPDIR/usr/lib
                                || P.startsWith(QStringLiteral("/tmp/.mount_")); // AppImage self-mount (APPDIR fallback)
        if (!UnderAppImage) Kept << P;
    }
    if (Kept == Env.value("LD_LIBRARY_PATH").split(':', Qt::SkipEmptyParts)) return Env;  // nothing stripped (not an AppImage)
    if (Kept.isEmpty()) Env.remove("LD_LIBRARY_PATH");
    else                Env.insert("LD_LIBRARY_PATH", Kept.join(':'));
    return Env;
}

// Synchronously runs Program with Arguments in ProcessEnvironment (waits indefinitely so long-running installers
// and wine boots don't time out). Dumps the child's stdout/stderr to the log. Returns the process exit code, or
// -1 if it failed to start or crashed. Defined in processenv.cpp. (Generic process runner — no container coupling;
// lifted out of ContainerWrapper.)
int RunCommand(std::string Program, std::vector<std::string> Arguments,
               QProcessEnvironment ProcessEnvironment = QProcessEnvironment::systemEnvironment(),
               const std::string &WorkingDirectory = "");

#endif // PROCESSENV_H
