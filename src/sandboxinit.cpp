#include "sandboxlayer.h"
#include "platform/platform.h"

#include <string>
#include <vector>
#include <filesystem>
#include <cstdio>
#include <cstring>

// The entire sandbox-init runtime is bwrap-specific (Linux mount + network namespaces, TUN, SCM_RIGHTS).
// It never runs on other platforms — Available() is false there and nothing invokes `--sandbox-init` — so
// guard the whole implementation (and its Linux-only headers). Windows isolation arrives later as a
// separate Sandboxie flow (port plan §3); until then a launch runs unsandboxed.
#ifdef __linux__

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
    std::filesystem::path Self = Platform::SelfExe();
    if (!Self.empty())
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

    for (int i = 0; i < 300; ++i)   // up to ~30 s (a very deep .vgdelta chain takes a few s to compose + enumerate;
    {                               // returns the instant the mount appears, so the higher cap only helps slow mounts
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
bool BringUpTun(const std::string &name, const std::string &cidr, const std::string &sockPath, bool bridge)
{
    int tun = open("/dev/net/tun", O_RDWR);
    if (tun < 0) { Warn("open /dev/net/tun failed"); return false; }
    struct ifreq ifr{};
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    std::strncpy(ifr.ifr_name, name.c_str(), IFNAMSIZ - 1);
    if (ioctl(tun, TUNSETIFF, &ifr) < 0) { Warn("TUNSETIFF failed"); close(tun); return false; }

    // Address it (CAP_NET_ADMIN holds in the sandbox userns). The /24 route is installed by the addr.
    // ALSO raise loopback: a freshly unshared netns has lo DOWN, and Windows games under wine lean on 127.0.0.1
    // constantly (self-IP via hostname, DirectPlay internals, winsock probes) — with lo down they see their own IP
    // as 0.0.0.0 and fail binds with "invalid argument" (observed: Age of Mythology multiplayer).
    // MTU 1280: every game packet must fit ONE QUIC datagram — at 1400 a large packet silently split onto the stream.
    std::string ipcmd = "ip link set dev lo up; "
                        "ip link set dev " + name + " mtu 1280; ip addr add " + cidr + " dev " + name
                        + "; ip link set dev " + name + " up";
    // BRIDGE (tri-plane): a gateway-less on-link default route sends every non-LAN packet up the TUN, where the
    // in-node gVisor NAT (natgateway.go) forwards it as a real host socket — internet + real-LAN unicast, no helper.
    // The 255.255.255.255/32 pin keeps LAN discovery broadcasts on the overlay plane (more specific than default);
    // rp_filter=0 because reflector-injected packets carry real-LAN source IPs whose return route is this same TUN
    // (strict reverse-path filtering would drop them). Without bridge, only the discovery route exists (pure vLAN).
    if (bridge)
        ipcmd += "; ip route add default dev " + name
               + "; sysctl -w net.ipv4.conf.all.rp_filter=0 net.ipv4.conf.default.rp_filter=0 >/dev/null 2>&1";
    ipcmd += "; ip route add 255.255.255.255/32 dev " + name;
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

// ---------------------------------------------------------------------------------------------------------------
// WINDOW CLOAK — keep a game's ugly startup window off the screen from the X SIDE.
//
// Why X and not win32: some launchers (Wipeout XL's NET-WOXL) create their window VISIBLE and then spend
// seconds doing network I/O without pumping messages. In that state every win32-side remedy loses: synchronous
// SetWindowPos/ShowWindow BLOCK on the busy thread, and SWP_ASYNCWINDOWPOS/ShowWindowAsync just queue behind
// the very work that keeps the window on screen. X window properties are SERVER-side — the owning thread's
// cooperation is irrelevant — so a tiny xcb watcher in the sandbox (where DISPLAY already points at the host
// X/XWayland) can hide the window for exactly its ugly phase.
//
// HOW it hides matters: an xcb_unmap_window looked perfect but wine notices the withdraw and the launcher then
// IGNORES the driver's posted Play click (SP soft-locked invisibly, driver "re-pressed" forever). Setting
// _NET_WM_WINDOW_OPACITY=0 instead leaves the window mapped, focused and fully clickable in win32 terms — the
// compositor just draws it at alpha 0.
//
// Spec: "<caption>|<maxwidth>". Windows whose WM_NAME equals <caption> and whose width is <= <maxwidth> get
// opacity 0 on sight. The first matching-caption window WIDER than <maxwidth> is the game itself: the watcher
// exits and never touches anything again. On timeout it restores opacity — an invisible interactive window
// must never be left for a human to not-find. The watcher dies with the sandbox (its pid namespace).
// ---------------------------------------------------------------------------------------------------------------
#include <xcb/xcb.h>

static bool CloakNameMatches(xcb_connection_t *c, xcb_window_t w, const std::string &caption)
{
    // WM_NAME (STRING) is what wine sets from the win32 caption; check _NET_WM_NAME (UTF8) too for safety.
    for (xcb_atom_t prop : {static_cast<xcb_atom_t>(XCB_ATOM_WM_NAME), static_cast<xcb_atom_t>(0)})
    {
        xcb_get_property_cookie_t ck;
        if (prop == 0)
        {
            xcb_intern_atom_reply_t *a =
                xcb_intern_atom_reply(c, xcb_intern_atom(c, 1, 12, "_NET_WM_NAME"), nullptr);
            if (!a) continue;
            ck = xcb_get_property(c, 0, w, a->atom, XCB_GET_PROPERTY_TYPE_ANY, 0, 64);
            free(a);
        }
        else
            ck = xcb_get_property(c, 0, w, prop, XCB_GET_PROPERTY_TYPE_ANY, 0, 64);
        xcb_get_property_reply_t *r = xcb_get_property_reply(c, ck, nullptr);
        if (!r) continue;
        const std::string name(static_cast<char *>(xcb_get_property_value(r)),
                               static_cast<size_t>(xcb_get_property_value_length(r)));
        free(r);
        if (name == caption) return true;
    }
    return false;
}

static void RunCloakWatcher(const std::string &caption, int maxWidth)
{
    xcb_connection_t *c = xcb_connect(nullptr, nullptr);
    if (!c || xcb_connection_has_error(c)) { fprintf(stderr, "[cloak] no X connection\n"); return; }
    xcb_intern_atom_reply_t *clientList =
        xcb_intern_atom_reply(c, xcb_intern_atom(c, 1, 16, "_NET_CLIENT_LIST"), nullptr);
    xcb_intern_atom_reply_t *opacityAtom =
        xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, 22, "_NET_WM_WINDOW_OPACITY"), nullptr);
    xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    fprintf(stderr, "[cloak] watching for '%s' (<=%dpx) on the X side\n", caption.c_str(), maxWidth);

    xcb_window_t lastHidden = 0;
    for (int tick = 0; tick < 1800; ++tick)   // 90s at 50ms
    {
        // Managed toplevels via EWMH _NET_CLIENT_LIST (KWin reparents toplevels, so root's direct children are
        // frames — the caption lives on the CLIENT window this list holds).
        if (clientList)
        {
            xcb_get_property_reply_t *lr = xcb_get_property_reply(
                c, xcb_get_property(c, 0, screen->root, clientList->atom, XCB_ATOM_WINDOW, 0, 256), nullptr);
            if (lr)
            {
                const int n = xcb_get_property_value_length(lr) / 4;
                const xcb_window_t *wins = static_cast<xcb_window_t *>(xcb_get_property_value(lr));
                for (int i = 0; i < n; ++i)
                {
                    if (!CloakNameMatches(c, wins[i], caption)) continue;
                    xcb_get_geometry_reply_t *g =
                        xcb_get_geometry_reply(c, xcb_get_geometry(c, wins[i]), nullptr);
                    if (!g) continue;
                    const int width = g->width;
                    free(g);
                    if (width > maxWidth)
                    {
                        // The game may REUSE the launcher's X window (Wipeout2 does) — merely exiting would
                        // leave it playing at alpha 0 forever. Strip the property from both candidates and
                        // ask the WM to activate the game: an unfocused game window never mode-switches.
                        if (opacityAtom)
                        {
                            xcb_delete_property(c, wins[i], opacityAtom->atom);
                            if (lastHidden && lastHidden != wins[i])
                                xcb_delete_property(c, lastHidden, opacityAtom->atom);
                        }
                        xcb_intern_atom_reply_t *activate =
                            xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, 18, "_NET_ACTIVE_WINDOW"), nullptr);
                        if (activate)
                        {
                            xcb_client_message_event_t ev = {};
                            ev.response_type  = XCB_CLIENT_MESSAGE;
                            ev.format         = 32;
                            ev.window         = wins[i];
                            ev.type           = activate->atom;
                            ev.data.data32[0] = 1;   // source: application
                            xcb_send_event(c, 0, screen->root,
                                           XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                                           XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                                           reinterpret_cast<const char *>(&ev));
                            free(activate);
                        }
                        xcb_flush(c);
                        fprintf(stderr, "[cloak] game window appeared (%dpx) — cloak off\n", width);
                        free(lr); free(clientList); free(opacityAtom); xcb_disconnect(c);
                        return;
                    }
                    if (opacityAtom)
                    {
                        const uint32_t zero = 0;
                        xcb_change_property(c, XCB_PROP_MODE_REPLACE, wins[i], opacityAtom->atom,
                                            XCB_ATOM_CARDINAL, 32, 1, &zero);
                        xcb_flush(c);
                        lastHidden = wins[i];
                    }
                }
                free(lr);
            }
        }
        usleep(50 * 1000);
    }
    // Timeout is the softlock guard's moment: if the launcher never advanced (a driver bug, a game quirk),
    // an invisible interactive window would be UNFINDABLE for the human. Make it visible again and bow out.
    if (lastHidden && opacityAtom)
    {
        xcb_delete_property(c, lastHidden, opacityAtom->atom);
        xcb_flush(c);
        fprintf(stderr, "[cloak] gave up after 90s -- restored the hidden window so a human can use it\n");
    }
    else
        fprintf(stderr, "[cloak] gave up after 90s\n");
    free(clientList);
    free(opacityAtom);
    xcb_disconnect(c);
}

// Grammar:  --sandbox-init  [--mount <spec> <mnt>]…  [--tun <name> <cidr> <sock> <bridge|nobridge>]  [--cloak <caption>|<maxwidth>]  --  <program> [args…]
int RunSandboxInit(int argc, char **argv)
{
    std::vector<std::pair<std::string, std::string>> mounts;
    std::string tunName, tunCidr, tunSock, chdirTo, cloakSpec;
    bool tunBridge = false;
    std::vector<std::string> cmd;

    int i = 1;
    for (; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--sandbox-init") continue;
        if (a == "--mount" && i + 2 < argc) { mounts.emplace_back(argv[i + 1], argv[i + 2]); i += 2; }
        else if (a == "--tun" && i + 4 < argc) { tunName = argv[i + 1]; tunCidr = argv[i + 2]; tunSock = argv[i + 3];
                                                 tunBridge = std::string(argv[i + 4]) == "bridge"; i += 4; }
        else if (a == "--chdir" && i + 1 < argc) { chdirTo = argv[i + 1]; i += 1; }
        else if (a == "--cloak" && i + 1 < argc) { cloakSpec = argv[i + 1]; i += 1; }
        else if (a == "--") { ++i; break; }
    }
    for (; i < argc; ++i) cmd.emplace_back(argv[i]);
    if (cmd.empty()) { Warn("no game command after --"); return 2; }

    const std::string helper = VidyagodfsPath();
    for (const auto &[spec, mnt] : mounts)
        if (!MountOne(helper, spec, mnt)) { Warn(("mount failed: " + mnt).c_str()); return 1; }

    if (!tunName.empty() && !tunSock.empty())
        BringUpTun(tunName, tunCidr, tunSock, tunBridge);   // best-effort: the game still launches if the overlay can't come up

    // chdir into the game's working directory (lives under a mount we just created, so it couldn't be set earlier).
    if (!chdirTo.empty() && chdir(chdirTo.c_str()) != 0)
        Warn("chdir to the work dir failed (continuing)");

    // Window cloak: fork the X-side watcher BEFORE becoming the game. The child lives in this pid namespace,
    // so bwrap's teardown reaps it with everything else; no leak is possible.
    if (!cloakSpec.empty())
    {
        const size_t bar = cloakSpec.rfind('|');
        const std::string caption = bar == std::string::npos ? cloakSpec : cloakSpec.substr(0, bar);
        const int maxW = bar == std::string::npos ? 800 : atoi(cloakSpec.c_str() + bar + 1);
        if (fork() == 0)
        {
            RunCloakWatcher(caption, maxW > 0 ? maxW : 800);
            _exit(0);
        }
    }

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

#else  // !__linux__ — inert stubs; the bwrap sandbox-init is Linux-only (see the guard note above).

namespace SandboxLayer {
bool IsSandboxInit(int, char **)  { return false; }
int  RunSandboxInit(int, char **) { return 1; }
} // namespace SandboxLayer

#endif // __linux__
