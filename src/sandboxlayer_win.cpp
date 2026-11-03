// Windows Sandboxie backend of SandboxLayer (#ifdef _WIN32). The Linux bwrap backend lives in
// sandboxlayer.cpp; this file provides the Windows Available()/Wrap() behind the SAME seam, so
// ContainerWrapper drives sandboxing identically on both OSes — Wrap() only rewrites (Program,
// Arguments), here to run the game inside a Sandboxie box via Start.exe.
//
// Parity mapping (bwrap → Sandboxie):
//   * host isolation (RO host, COW writes)      → a per-launch Sandboxie box (filesystem + registry
//                                                  virtualization is automatic for anything in the box)
//   * writable binds (game runtime + saves stay  → SbieIni `OpenFilePath` grants (BoxIniCommands): the WinFsp
//     VidyaGod-owned, not captured by the box)     mountpoints and folders pass THROUGH to the real location, so VidyaGod's own
//                                                  writelayer/Persist keeps owning persistence while
//                                                  Sandboxie captures/discards everything else
//   * `Program="bwrap" … -- <game>`             → `Program="Start.exe" /box:<box> /silent /wait <game>`
//
// Unlike Linux, the vidyagodfs mount is NOT re-created inside the sandbox — VidyaGod already mounted it
// on the host (WinFsp), and the boxed game reaches it through the OpenFilePath grant on its mountpoint
// (Options.Mounts). TUN fields are unused here (multiplayer = Wintun, a later phase).
//
// NOTE: launching a process INTO a box requires an interactive desktop session (Start.exe needs a real
// window station). Real launches run in the user's session, so this works there. From an SSH logon the boxed
// program runs as ANONYMOUS LOGON and never starts (Start.exe hangs); tools/winvm/vmrun.sh runs a test in the
// desktop session instead.
#ifdef _WIN32

#include "sandboxlayer.h"
#include "commonutils.h"   // LogErr

#include <QProcess>
#include <QString>
#include <QStringList>

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>

namespace SandboxLayer {

// Locate an installed Sandboxie-Plus (Start.exe + SbieIni.exe), typically %ProgramFiles%\Sandboxie-Plus.
// Sandboxie is an external dependency (installed separately, like WinFsp) — VidyaGod drives its Start.exe;
// it does not ship its own copy.
static std::filesystem::path SandboxieDir()
{
    std::error_code Ec;
    for (const char *Env : {"ProgramW6432", "ProgramFiles", "ProgramFiles(x86)"})
    {
        const char *P = std::getenv(Env);
        if (!P) continue;
        std::filesystem::path D = std::filesystem::path(P) / "Sandboxie-Plus";
        if (std::filesystem::exists(D / "Start.exe", Ec)) return D;
    }
    return {};
}

// The launch's box name, the same for every step of a launch (Wrap, PrepareBox, RunInBox, TerminateBox) and for every
// launch of one game instance: from its box root (in the instance's session folder), else its working folder. Not the
// working folder when there is a root: that is on the runtime's drive letter, which changes between launches, and each
// new name left one more box behind in Sandboxie.ini.
static std::string BoxNameFor(const Options &Opts)
{
    return "VidyaGod_" + std::to_string(std::hash<std::string>{}(Opts.BoxRoot.empty() ? Opts.WorkDir : Opts.BoxRoot));
}

// Start.exe /box:<box> /silent /terminate ends every program still running in the box (the game, Sandboxie's helper
// processes) and waits for them. /silent: an unknown box is otherwise answered with a dialog that waits for a click.
static void Terminate(const std::filesystem::path &Dir, const std::string &BoxName)
{
    QProcess Q;
    Q.start(QString::fromStdString((Dir / "Start.exe").string()),
            {"/box:" + QString::fromStdString(BoxName), "/silent", "/terminate"});
    Q.waitForFinished(5000);
}

// Usable when the install is present AND the Sandboxie service (SbieSvc, which fronts the SbieDrv kernel
// driver) is running. Mirrors the bwrap functional probe: if the machine can't sandbox, the launch is refused
// (ContainerWrapper::Execute) — there is no unsandboxed fallback.
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

    // One box per launch's runtime folder (so per game instance), named stably so every step of a launch — the box
    // prefix's registry import and export, the game, TerminateBox — reaches the same one. SbieIni notifies SbieSvc.
    const std::string BoxName = BoxNameFor(Opts);
    PrepareBox(Opts);
    //Likewise before the game: the box prefix's registry import ran in it a moment ago (see RunInBox).
    Terminate(Dir, BoxName);

    const QStringList A = BoxStartArgs(BoxName, Program, Arguments);
    Program   = (Dir / "Start.exe").string();
    Arguments = A;
}

void PrepareBox(const Options &Opts)
{
    const std::filesystem::path Dir = SandboxieDir();
    if (Dir.empty()) return;
    const QString SbieIni = QString::fromStdString((Dir / "SbieIni.exe").string());
    //A setting that did not land is a game that runs and cannot save (or is killed at its first file access): said loudly.
    for (const QStringList &Cmd : BoxIniCommands(BoxNameFor(Opts), Opts))
    {
        QProcess Q;
        Q.start(SbieIni, Cmd);
        if (!Q.waitForFinished(4000) || Q.exitStatus() != QProcess::NormalExit || Q.exitCode() != 0)
            LogErr("SandboxLayer::PrepareBox", "Sandboxie did not take the box setting \"" + Cmd.join(' ').toStdString()
                   + "\" (SbieIni " + (Q.error() == QProcess::FailedToStart ? std::string("did not start")
                                       : Q.state() != QProcess::NotRunning ? std::string("did not answer in 4 s")
                                       : "exit code " + std::to_string(Q.exitCode())) + ") — the game may fail to save.");
    }
}

int RunInBox(const Options &Opts, const std::string &Program, const QStringList &Args, int TimeoutMs)
{
    const std::filesystem::path Dir = SandboxieDir();
    if (Dir.empty()) return -1;
    const std::string BoxName = BoxNameFor(Opts);
    //The box may still be winding down from the program before (the previous kept key's reg export, a moment ago): a
    //program started then is killed at once (0x40010004, measured: AoE2's second kept key was lost so). Ended first.
    Terminate(Dir, BoxName);
    QProcess Q;
    Q.start(QString::fromStdString((Dir / "Start.exe").string()), BoxStartArgs(BoxName, Program, Args));
    if (!Q.waitForFinished(TimeoutMs)) { Terminate(Dir, BoxName); Q.kill(); Q.waitForFinished(2000); return -1; }
    return Q.exitStatus() == QProcess::NormalExit ? Q.exitCode() : -1;
}

bool EmptyBox(const Options &Opts)
{
    const std::filesystem::path Dir = SandboxieDir();
    if (Dir.empty() || (Opts.WorkDir.empty() && Opts.BoxRoot.empty())) return false;
    QProcess Q;
    Q.start(QString::fromStdString((Dir / "Start.exe").string()), {"/box:" + QString::fromStdString(BoxNameFor(Opts)), "delete_sandbox_silent"});
    if (!Q.waitForFinished(120000)) { Q.kill(); Q.waitForFinished(2000); return false; }
    return Q.exitStatus() == QProcess::NormalExit && Q.exitCode() == 0;
}

void TerminateBox(const Options &Opts)
{
    const std::filesystem::path Dir = SandboxieDir();
    if (Dir.empty() || (Opts.WorkDir.empty() && Opts.BoxRoot.empty())) return;
    Terminate(Dir, BoxNameFor(Opts));   // nothing survives a KillGame; harmless if the box was never used
}

} // namespace SandboxLayer

#endif // _WIN32
