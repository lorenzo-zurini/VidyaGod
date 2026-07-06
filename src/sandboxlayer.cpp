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
    // Keep the mounted runtime writable (the game reads/writes its VFS runtime); fall back to the package dir.
    O.RuntimeRoot = (CP.UsesVFS && !CP.RuntimePath.empty()) ? CP.RuntimePath.string() : CP.PackagePath.string();
    if (const char *H = ::getenv("HOME")) O.HomeDir = H;
    return O;
}

bool Available()
{
    static const bool Ok = std::system("which bwrap > /dev/null 2>&1") == 0;
    return Ok;
}

void Wrap(const Options &Opts, std::string &Program, QStringList &Arguments)
{
    const QString OrigProgram = QString::fromStdString(Program);
    const QStringList OrigArgs = Arguments;

    QStringList A;
    auto Add = [&](std::initializer_list<const char *> Parts){ for (const char *P : Parts) A.append(QString::fromUtf8(P)); };

    Add({"--die-with-parent"});          // the sandbox dies if VidyaGod does
    Add({"--ro-bind", "/", "/"});         // whole host, READ-ONLY (native apps can't write the system)
    Add({"--dev-bind", "/dev", "/dev"});  // GPU (/dev/dri), input devices
    Add({"--proc", "/proc"});

    // Re-bind the writable surfaces on top of the read-only root (later binds win).
    if (!Opts.RuntimeRoot.empty() && std::filesystem::exists(Opts.RuntimeRoot))
    {
        A.append("--bind");
        A.append(QString::fromStdString(Opts.RuntimeRoot));
        A.append(QString::fromStdString(Opts.RuntimeRoot));
    }
    if (!Opts.HomeDir.empty() && std::filesystem::exists(Opts.HomeDir))
    {
        A.append("--bind");
        A.append(QString::fromStdString(Opts.HomeDir));
        A.append(QString::fromStdString(Opts.HomeDir));
    }
    // Display / audio / IPC sockets so the game still renders + plays sound.
    A.append("--bind"); A.append("/tmp"); A.append("/tmp");   // X11 (/tmp/.X11-unix) + general scratch
    const std::string RunUser = "/run/user/" + std::to_string(static_cast<unsigned>(::getuid()));
    if (std::filesystem::exists(RunUser))
    {
        A.append("--bind");
        A.append(QString::fromStdString(RunUser));   // Wayland / PipeWire / PulseAudio sockets
        A.append(QString::fromStdString(RunUser));
    }

    if (Opts.Net == NetMode::Isolated)
        A.append("--unshare-net");   // the game gets its own empty netns (overlay TUN injected separately)

    A.append("--");
    A.append(OrigProgram);
    A.append(OrigArgs);

    Program   = "bwrap";
    Arguments = A;
}

} // namespace SandboxLayer
