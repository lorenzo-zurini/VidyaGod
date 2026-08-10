// Windows Sandboxie backend of SandboxLayer (#ifdef _WIN32). The Linux bwrap backend lives in
// sandboxlayer.cpp; this file provides the Windows Available()/Wrap() behind the SAME seam, so
// ContainerWrapper drives sandboxing identically on both OSes — Wrap() only rewrites (Program,
// Arguments), here to run the game inside a Sandboxie box via Start.exe.
//
// Parity mapping (bwrap → Sandboxie):
//   * host isolation (RO host, COW writes)      → a per-launch Sandboxie box (filesystem + registry
//                                                  virtualization is automatic for anything in the box)
//   * writable binds (game runtime + saves stay  → SbieIni `OpenFilePath` grants: those paths pass
//     VidyaGod-owned, not captured by the box)     THROUGH to the real location, so VidyaGod's own
//                                                  writelayer/Persist keeps owning persistence while
//                                                  Sandboxie captures/discards everything else
//   * `Program="bwrap" … -- <game>`             → `Program="Start.exe" /box:<box> /silent /wait <game>`
//
// Unlike Linux, the vidyagodfs mount is NOT re-created inside the sandbox — VidyaGod already mounted it
// on the host (WinFsp), and the boxed game reaches it through the OpenFilePath grant on WorkDir. So
// Options.Mounts / TUN fields are unused here (multiplayer = Wintun, a later phase).
//
// NOTE: launching a process INTO a box requires an interactive desktop session (Start.exe needs a real
// window station). Real launches run in the user's session, so this works there; it cannot be verified
// from a headless/SSH session (where the box child won't start).
#ifdef _WIN32

#include "sandboxlayer.h"
#include "launchparams.h"      // ContainerParams (for FromParams' Var lookups, declared in sandboxlayer.cpp)
#include "platform/platform.h"

#include <QProcess>
#include <QString>
#include <QStringList>

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>

namespace SandboxLayer {

// Locate the Sandboxie-Plus install (Start.exe + SbieIni.exe), typically %ProgramFiles%\Sandboxie-Plus.
static std::filesystem::path SandboxieDir()
{
    for (const char *Env : {"ProgramW6432", "ProgramFiles", "ProgramFiles(x86)"})
    {
        const char *P = std::getenv(Env);
        if (!P) continue;
        std::error_code Ec;
        std::filesystem::path D = std::filesystem::path(P) / "Sandboxie-Plus";
        if (std::filesystem::exists(D / "Start.exe", Ec)) return D;
    }
    return {};
}

// The per-launch box name, derived stably from the game's runtime dir so Wrap() and TerminateBox()
// agree without threading extra state through ContainerParams.
static std::string BoxNameFor(const std::string &WorkDir)
{
    return "VidyaGod_" + std::to_string(std::hash<std::string>{}(WorkDir));
}

// Usable when the install is present AND the Sandboxie service (SbieSvc, which fronts the SbieDrv kernel
// driver) is running. Mirrors the bwrap functional probe: if the machine can't sandbox, a launch simply
// degrades to unsandboxed (ContainerWrapper gates Wrap() on Available()).
bool Available()
{
    if (SandboxieDir().empty()) return false;
    QProcess Q;
    Q.start("sc", {"query", "SbieSvc"});
    if (!Q.waitForFinished(3000)) { Q.kill(); return false; }
    return QString::fromLocal8Bit(Q.readAllStandardOutput()).contains("RUNNING");
}

void Wrap(const Options &Opts, std::string &Program, QStringList &Arguments)
{
    const std::filesystem::path Dir = SandboxieDir();
    if (Dir.empty()) return;   // gated by Available(); no-op if somehow gone

    const QString    OrigProgram = QString::fromStdString(Program);
    const QStringList OrigArgs   = Arguments;

    // A per-launch box, named stably from the game's runtime dir so re-launches of the same package reuse
    // one box (its virtualized state can carry across runs where useful; captured writes are auto-deleted).
    const std::string BoxName = BoxNameFor(Opts.WorkDir);
    const QString     Box     = QString::fromStdString(BoxName);
    const QString     SbieIni = QString::fromStdString((Dir / "SbieIni.exe").string());

    auto SetIni = [&](const QString &Key, const QString &Value) {
        QProcess Q;
        Q.start(SbieIni, {"set", Box, Key, Value});   // SbieIni notifies SbieSvc to reload the config
        Q.waitForFinished(4000);
    };
    auto AppendIni = [&](const QString &Key, const QString &Value) {
        QProcess Q;
        Q.start(SbieIni, {"append", Box, Key, Value});
        Q.waitForFinished(4000);
    };

    // Define/refresh the box. Enabled + AutoDelete (captured writes are throwaway — real persistence is via
    // the OpenFilePath pass-throughs below, which VidyaGod's writelayer/Persist owns).
    SetIni("Enabled", "y");
    SetIni("AutoDelete", "y");
    // Pass THROUGH to VidyaGod's own writable surfaces so the box doesn't shadow them: the WinFsp runtime
    // mount (its writelayer captures the game's writes) + any explicit writable binds (saves/user data).
    if (!Opts.WorkDir.empty()) AppendIni("OpenFilePath", QString::fromStdString(Opts.WorkDir));
    for (const std::string &P : Opts.WritableBinds)
        if (!P.empty()) AppendIni("OpenFilePath", QString::fromStdString(P));

    // Wrap: Start.exe /box:<box> /silent /wait <origProgram> <origArgs...>. /wait keeps Start.exe alive
    // until the game exits, so ContainerWrapper's QProcess tracks the real game lifetime (and its stdio).
    QStringList A;
    A << ("/box:" + Box) << "/silent" << "/wait";
    if (!Opts.WorkDir.empty()) A << ("/wrk:" + QString::fromStdString(Opts.WorkDir));
    A << OrigProgram << OrigArgs;

    Program   = (Dir / "Start.exe").string();
    Arguments = A;
}

void TerminateBox(const std::string &WorkDir)
{
    const std::filesystem::path Dir = SandboxieDir();
    if (Dir.empty() || WorkDir.empty()) return;
    // Start.exe /box:<box> /terminate reaps every process still running in the box (the game plus any
    // Sandboxie helper processes), so nothing survives a KillGame. Harmless if the box was never used.
    QProcess Q;
    Q.start(QString::fromStdString((Dir / "Start.exe").string()),
            {"/box:" + QString::fromStdString(BoxNameFor(WorkDir)), "/terminate"});
    Q.waitForFinished(5000);
}

} // namespace SandboxLayer

#endif // _WIN32
