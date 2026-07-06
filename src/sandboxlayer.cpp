#include "sandboxlayer.h"
#include "launchparams.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>

#include <unistd.h>

// See sandboxlayer.h. This builds a conservative, working-first bwrap policy: the host root is mounted READ-ONLY so a
// native game can't modify the system, while the game's own runtime + the user's home (saves) are re-bound writable,
// and /dev + /proc + /tmp + the per-user runtime dir (Wayland/X11/PipeWire/Pulse sockets) are passed through so the
// game still renders and plays audio. Later, more-specific binds override the read-only root (bwrap applies them in
// order). Network scoping (--unshare-net) is emitted for NetMode::Isolated.
//
// NOTE (the remaining hardware step): in Isolated mode the game's netns starts empty, so the overlay TUN must be
// created INSIDE it. Because the netns is owned by bwrap's user namespace, that injection needs to join both the user
// and net namespaces of the sandbox (the slirp4netns/pasta pattern) — it can't be done from the parent by a plain
// setns(CLONE_NEWNET) alone. Until that lands, the session flow uses NetMode::Host (overlay TUN on the host, which is
// already proven end-to-end), which the game sees because Host mode shares the net namespace.

namespace SandboxLayer {

static bool Truthy(const std::string &V)
{
    std::string L;
    L.reserve(V.size());
    for (char C : V) L.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(C))));
    return L == "on" || L == "true" || L == "1" || L == "yes";
}

// Look up a custom variable case-sensitively (the launch namespace uses UPPER_SNAKE keys).
static std::string Var(const ContainerParams &CP, const std::string &Key)
{
    auto It = CP.CustomVariables.find(Key);
    return It == CP.CustomVariables.end() ? std::string() : It->second;
}

bool Requested(const ContainerParams &CP)
{
    return Truthy(Var(CP, "VIDYAGOD_SANDBOX"));
}

Options FromParams(const ContainerParams &CP)
{
    Options O;
    O.Enabled = Requested(CP);
    O.Net = (Var(CP, "VIDYAGOD_SANDBOX_NET") == "isolated") ? NetMode::Isolated : NetMode::Host;
    return O;
}

bool Available()
{
    static const bool Ok = std::system("which bwrap > /dev/null 2>&1") == 0;
    return Ok;
}

// Resolve this executable's path so the sandbox can re-invoke it as `--sandbox-init` (it's already visible via
// --ro-bind / /).
static std::string SelfExe()
{
    std::error_code Ec;
    std::filesystem::path Self = std::filesystem::read_symlink("/proc/self/exe", Ec);
    return Ec ? std::string("VidyaGod") : Self.string();
}

void Wrap(const Options &Opts, std::string &Program, QStringList &Arguments)
{
    const QString OrigProgram = QString::fromStdString(Program);
    const QStringList OrigArgs = Arguments;
    const bool Isolated = (Opts.Net == NetMode::Isolated);

    QStringList A;
    // One hermetic namespace bundle. --unshare-user gives us CAP_SYS_ADMIN in it (retained via --cap-add) so the
    // sandbox-init can mount vidyagodfs; uid stays the real uid (wine/proton refuse to run as root). Isolated mode
    // also unshares the net ns and keeps CAP_NET_ADMIN so the init can bring up the overlay TUN there.
    A << "--die-with-parent" << "--unshare-user" << "--unshare-pid" << "--unshare-ipc"
      << "--cap-add" << "CAP_SYS_ADMIN";
    if (Isolated) A << "--unshare-net" << "--cap-add" << "CAP_NET_ADMIN";
    A << "--ro-bind" << "/" << "/"            // whole host READ-ONLY (native apps can't write the system)
      << "--dev-bind" << "/dev" << "/dev"     // GPU (/dev/dri) + /dev/fuse + /dev/net/tun
      << "--proc" << "/proc";

    // Writable surfaces over the RO root (the vidyagodfs mount lives under TempPath; saves under UserData/home).
    auto bindRW = [&](const std::string &P){
        if (!P.empty() && std::filesystem::exists(P)) { A << "--bind" << QString::fromStdString(P) << QString::fromStdString(P); }
    };
    for (const std::string &P : Opts.WritableBinds) bindRW(P);
    bindRW("/tmp");                                                        // X11 socket + scratch
    bindRW("/run/user/" + std::to_string(static_cast<unsigned>(::getuid()))); // Wayland / PipeWire / Pulse

    // The payload: re-invoke ourselves as the in-sandbox init, which mounts the specs (+ TUN) then execs the game.
    A << "--" << QString::fromStdString(SelfExe()) << "--sandbox-init";
    for (const Mount &M : Opts.Mounts)
        A << "--mount" << QString::fromStdString(M.SpecPath) << QString::fromStdString(M.Mountpoint);
    if (!Opts.TunName.empty() && !Opts.TunSock.empty())
        A << "--tun" << QString::fromStdString(Opts.TunName) << QString::fromStdString(Opts.TunCidr) << QString::fromStdString(Opts.TunSock);
    if (!Opts.WorkDir.empty())
        A << "--chdir" << QString::fromStdString(Opts.WorkDir);
    A << "--" << OrigProgram << OrigArgs;

    Program   = "bwrap";
    Arguments = A;
}

} // namespace SandboxLayer
