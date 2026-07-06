#include "sandboxlayer.h"

#include <string>
#include <vector>
#include <filesystem>
#include <cstdio>
#include <cstring>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/if_tun.h>

// sandboxinit.cpp — the in-sandbox runtime (`VidyaGod --sandbox-init …`), i.e. the single command bwrap runs inside
// the hermetic namespace. Reuses the existing VidyaGod binary (already visible via --ro-bind / /) so there is no new
// artifact to ship. Runs BEFORE any Qt. Steps: (1) mount each vidyagodfs layer-spec inside the sandbox's mount
// namespace (fork the co-located vidyagodfs -f, poll until the mount appears); (2) if overlay args are present,
// create + address the TUN inside the sandbox's net namespace and hand its fd back to the parent's libp2p forwarder
// over a bound unix socket (SCM_RIGHTS); (3) execvp the game, becoming it. When the game exits, bwrap tears the
// namespace down and the mounts + TUN vanish — nothing leaks to the host.
//
// Everything here is deliberately low-level + dependency-free (no Qt): it is PID ~1 of a throwaway namespace.

namespace {

void Warn(const char *msg) { std::fprintf(stderr, "[sandbox-init] %s\n", msg); std::fflush(stderr); }

// Resolve the co-located vidyagodfs helper (beside this binary), mirroring VfsMount::VidyagodfsPath.
std::string VidyagodfsPath()
{
    std::error_code Ec;
    std::filesystem::path Self = std::filesystem::read_symlink("/proc/self/exe", Ec);
    if (!Ec)
    {
        std::filesystem::path Cand = Self.parent_path() / "vidyagodfs";
        if (std::filesystem::exists(Cand)) return Cand.string();
    }
    return "vidyagodfs"; // fall back to PATH
}

// Mount one layer-spec at mnt by forking vidyagodfs in the foreground (it keeps serving as our child), then poll the
// mountpoint's st_dev until it differs from its parent — the same liveness check vidyagodfs itself uses.
bool MountOne(const std::string &helper, const std::string &spec, const std::string &mnt)
{
    std::error_code Ec;
    std::filesystem::create_directories(mnt, Ec);

    // Record the mountpoint's device BEFORE mounting. It may already be a mount (the host runtime bound in via
    // --ro-bind / /), so we can't compare against the parent — we mount vidyagodfs OVER it and wait for st_dev to
    // CHANGE (each FUSE mount gets a unique anonymous device), which signals our fresh mount is on top.
    struct stat before{};
    const bool haveBefore = stat(mnt.c_str(), &before) == 0;

    pid_t pid = fork();
    if (pid < 0) { Warn("fork failed"); return false; }
    if (pid == 0)
    {
        // Child: become the FUSE daemon. -f keeps it in the foreground (no daemonize); it reparents to the sandbox
        // init once we execvp the game, and dies with the namespace.
        execlp(helper.c_str(), "vidyagodfs", spec.c_str(), mnt.c_str(), "-f", static_cast<char *>(nullptr));
        _exit(127);
    }

    for (int i = 0; i < 100; ++i)   // up to ~10 s
    {
        struct stat mst{};
        if (stat(mnt.c_str(), &mst) == 0 && (!haveBefore || mst.st_dev != before.st_dev)) return true;
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) { Warn("vidyagodfs exited before the mount appeared"); return false; }
        usleep(100000);
    }
    Warn("timed out waiting for the mount to appear");
    return false;
}

// Send a single fd over a connected unix socket via SCM_RIGHTS.
bool SendFd(int sock, int fd)
{
    char buf[1] = {'x'};
    struct iovec io = { buf, sizeof(buf) };
    char cbuf[CMSG_SPACE(sizeof(int))];
    std::memset(cbuf, 0, sizeof(cbuf));
    struct msghdr msg{};
    msg.msg_iov = &io; msg.msg_iovlen = 1;
    msg.msg_control = cbuf; msg.msg_controllen = sizeof(cbuf);
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS; cm->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cm), &fd, sizeof(int));
    return sendmsg(sock, &msg, 0) >= 0;
}

// Create + address the overlay TUN inside this (sandbox) net namespace and hand its fd to the parent forwarder over
// sockPath. name<=15 chars; cidr e.g. "10.66.42.2/24". Returns true on success (best-effort — a failure leaves the
// game running without the overlay rather than blocking the launch).
bool BringUpTun(const std::string &name, const std::string &cidr, const std::string &sockPath)
{
    int tun = open("/dev/net/tun", O_RDWR);
    if (tun < 0) { Warn("open /dev/net/tun failed"); return false; }
    struct ifreq ifr{};
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    std::strncpy(ifr.ifr_name, name.c_str(), IFNAMSIZ - 1);
    if (ioctl(tun, TUNSETIFF, &ifr) < 0) { Warn("TUNSETIFF failed"); close(tun); return false; }

    // Address it (CAP_NET_ADMIN holds in the sandbox userns). The /24 route is installed by the addr.
    const std::string ipcmd = "ip link set dev " + name + " mtu 1400; ip addr add " + cidr + " dev " + name
                              + "; ip link set dev " + name + " up";
    if (std::system(ipcmd.c_str()) != 0) Warn("ip configuration returned non-zero (continuing)");

    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) { close(tun); return false; }
    struct sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, sockPath.c_str(), sizeof(sa.sun_path) - 1);
    bool connected = false;
    for (int i = 0; i < 50; ++i) { if (connect(s, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) == 0) { connected = true; break; } usleep(100000); }
    if (!connected) { Warn("could not connect to the overlay socket"); close(s); close(tun); return false; }

    const bool ok = SendFd(s, tun);
    close(s);
    close(tun);   // the parent now holds a dup via SCM_RIGHTS, which keeps the device alive
    if (!ok) Warn("sending the TUN fd failed");
    return ok;
}

} // namespace

namespace SandboxLayer {

bool IsSandboxInit(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--sandbox-init") == 0) return true;
    return false;
}

// Grammar:  --sandbox-init  [--mount <spec> <mnt>]…  [--tun <name> <cidr> <sock>]  --  <program> [args…]
int RunSandboxInit(int argc, char **argv)
{
    std::vector<std::pair<std::string, std::string>> mounts;
    std::string tunName, tunCidr, tunSock, chdirTo;
    std::vector<std::string> cmd;

    int i = 1;
    for (; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--sandbox-init") continue;
        if (a == "--mount" && i + 2 < argc) { mounts.emplace_back(argv[i + 1], argv[i + 2]); i += 2; }
        else if (a == "--tun" && i + 3 < argc) { tunName = argv[i + 1]; tunCidr = argv[i + 2]; tunSock = argv[i + 3]; i += 3; }
        else if (a == "--chdir" && i + 1 < argc) { chdirTo = argv[i + 1]; i += 1; }
        else if (a == "--") { ++i; break; }
    }
    for (; i < argc; ++i) cmd.emplace_back(argv[i]);
    if (cmd.empty()) { Warn("no game command after --"); return 2; }

    const std::string helper = VidyagodfsPath();
    for (const auto &[spec, mnt] : mounts)
        if (!MountOne(helper, spec, mnt)) { Warn(("mount failed: " + mnt).c_str()); return 1; }

    if (!tunName.empty() && !tunSock.empty())
        BringUpTun(tunName, tunCidr, tunSock);   // best-effort: the game still launches if the overlay can't come up

    // chdir into the game's working directory (lives under a mount we just created, so it couldn't be set earlier).
    if (!chdirTo.empty() && chdir(chdirTo.c_str()) != 0)
        Warn("chdir to the work dir failed (continuing)");

    // Become the game.
    std::vector<char *> cargv;
    cargv.reserve(cmd.size() + 1);
    for (auto &s : cmd) cargv.push_back(const_cast<char *>(s.c_str()));
    cargv.push_back(nullptr);
    execvp(cargv[0], cargv.data());
    Warn("execvp of the game failed");
    return 127;
}

} // namespace SandboxLayer
