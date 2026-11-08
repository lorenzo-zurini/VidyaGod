#include "sandboxlayer.h"
#include "instancestore.h"   // kBoxDir — where an instance keeps its box
#include "launchparams.h"
#include "platform/platform.h"
#include "commonutils.h"   // LogErr

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>

#include <unistd.h>

// See sandboxlayer.h. This builds a conservative, working-first bwrap policy: the host root is mounted READ-ONLY so a
// native game can't modify the system, while the game's session tree + its kept folders are re-bound writable,
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

// Look up a custom variable case-sensitively (the launch namespace uses UPPER_SNAKE keys).
static std::string Var(const ContainerParams &CP, const std::string &Key)
{
    auto It = CP.CustomVariables.find(Key);
    return It == CP.CustomVariables.end() ? std::string() : It->second;
}

Options ForLaunch(const ContainerParams &CP)
{
    Options O = FromParams(CP);
    for (const auto &[Spec, Mnt] : CP.SandboxMounts) O.Mounts.push_back({Spec, Mnt});
    O.WorkDir = CP.WorkDirPathComplete.string();
    return O;
}

Options FromParams(const ContainerParams &CP)
{
    Options O;
    O.Net = (Var(CP, "VIDYAGOD_SANDBOX_NET") == "isolated") ? NetMode::Isolated : NetMode::Host;
    if (!CP.TempPath.empty()) O.WritableBinds.push_back(CP.TempPath.string());
    // Of the instance folder, only the kept folders: the in-sandbox vidyagodfs serves each one live at its runtime
    // path. Kept files and registry keys are copied in and out by this process, outside the sandbox, so the folder
    // itself (instance.json: VARIABLES, RUNNER_CHAIN) and the stores beside it stay out of the game's reach.
    // A kept folder that is a link (left by a game when the whole instance folder was bound) is not bound: bwrap refuses
    // to mount on a link, and the folder it names is not the instance's to open to the game.
    if (!CP.UserDataPath.empty())
        for (const PersistTarget &D : CP.KeepDirs)
        {
            const std::filesystem::path Kept = CP.UserDataPath / D.Target;
            std::error_code Ec;
            if (std::filesystem::symlink_status(Kept, Ec).type() == std::filesystem::file_type::symlink)
                LogWarn("SandboxLayer::FromParams", "The kept folder " + Kept.string() + " is a link, not a folder — not "
                        "opened to the game (it sees what the link names, read-only). Replace the link with the folder.");
            else
                O.WritableBinds.push_back(Kept.string());
        }
#ifdef _WIN32
    // The box's own root: the game's prefix (BoxPrefix), in the instance (InstanceStore::kBoxDir): until the launch's
    // teardown stores them it holds the only copy of what the game keeps outside its game folder, so it is kept where a
    // crash, a TEMP cleanup or Windows' Storage Sense cannot take it; BoxLaunch empties it once stored.
    if (!CP.UserDataPath.empty()) O.BoxRoot = (CP.UserDataPath / InstanceStore::kBoxDir).string();
#endif
#ifndef _WIN32
    // The private home (see the header). The real one stays out of WritableBinds: the read-only root covers it.
    const char *H = std::getenv("HOME");
    O.RealHome = H ? H : "";
    if (!CP.TempPath.empty())
    {
        O.Home = (CP.TempPath / "HOME").string();
        // The GPU driver's shader caches, where the driver looks for them under the private home's XDG_CACHE_HOME
        // (NVIDIA: <cache>/nvidia/GLCache, measured on 580; Mesa: <cache>/mesa_shader_cache*) and under HOME (.nv,
        // NVIDIA's older and its CUDA cache). Wrap binds only the ones the real home has.
        if (!O.RealHome.empty())
        {
            const char *C = std::getenv("XDG_CACHE_HOME");   // a relative one is invalid (XDG spec): ignored
            const std::filesystem::path RealCache = (C && *C == '/') ? std::filesystem::path(C)
                                                                     : std::filesystem::path(O.RealHome) / ".cache";
            const std::filesystem::path Home(O.Home);
            for (const char *Dir : {"nvidia", "mesa_shader_cache", "mesa_shader_cache_db", "mesa_shader_cache_sf"})
                O.HomeCaches.emplace_back((RealCache / Dir).string(), (Home / ".cache" / Dir).string());
            O.HomeCaches.emplace_back((std::filesystem::path(O.RealHome) / ".nv").string(), (Home / ".nv").string());
        }
    }
#endif
    return O;
}

void HomeEnv(const Options &Opts, QProcessEnvironment &Env)
{
    if (Opts.Home.empty()) return;
    const QString Home = QString::fromStdString(Opts.Home);
    Env.insert("HOME", Home);
    Env.insert("XDG_CONFIG_HOME", Home + "/.config");
    Env.insert("XDG_DATA_HOME",   Home + "/.local/share");
    Env.insert("XDG_STATE_HOME",  Home + "/.local/state");
    Env.insert("XDG_CACHE_HOME",  Home + "/.cache");
    if (!Env.contains("XAUTHORITY") && !Opts.RealHome.empty())
    {
        const std::filesystem::path Cookie = std::filesystem::path(Opts.RealHome) / ".Xauthority";
        std::error_code Ec;
        if (std::filesystem::exists(Cookie, Ec)) Env.insert("XAUTHORITY", QString::fromStdString(Cookie.string()));
    }
}

std::vector<std::string> HomeLeftovers(const std::string &Home, size_t Limit)
{
    std::vector<std::string> Out;
    std::error_code Ec;
    // symlink_status: the program can replace its home with a link (to /, to the real home); walking that would read
    // the whole host from the launch thread. The walk below never follows a link either (no follow_directory_symlink).
    if (Home.empty() || std::filesystem::symlink_status(Home, Ec).type() != std::filesystem::file_type::directory)
        return Out;
    size_t Visited = 0;   // every entry counts toward the cap: a tree of empty folders must not make the walk unbounded
    for (auto It = std::filesystem::recursive_directory_iterator(Home, std::filesystem::directory_options::skip_permission_denied, Ec);
         !Ec && It != std::filesystem::recursive_directory_iterator() && Visited++ < Limit; It.increment(Ec))
        if (It->is_symlink(Ec) || !It->is_directory(Ec))
        {
            std::string Name = It->path().lexically_relative(Home).generic_string();   // relative() would resolve links
            for (char &Ch : Name)   // the program chose the names, and they go to the log and the terminal
                if (static_cast<unsigned char>(Ch) < 0x20 || Ch == 0x7f) Ch = '?';
            Out.push_back(std::move(Name));
        }
    std::sort(Out.begin(), Out.end());
    return Out;
}

std::vector<GameEnvVar> TakeLoaderEnv(QProcessEnvironment &Run, const QProcessEnvironment &Base)
{
    std::vector<GameEnvVar> Out;
    QStringList Names;
    for (const QString &K : Run.keys() + Base.keys())
        if ((K.startsWith(QStringLiteral("LD_")) || K == QStringLiteral("GLIBC_TUNABLES")) && !Names.contains(K)) Names << K;
    Names.sort();
    for (const QString &Name : Names)
    {
        const bool GameSet = Run.contains(Name), BaseSet = Base.contains(Name);
        const QString GameVal = Run.value(Name);
        if (GameSet == BaseSet && GameVal == Base.value(Name)) continue;
        Out.push_back({Name.toStdString(), GameSet, GameVal.toStdString()});
        if (BaseSet) Run.insert(Name, Base.value(Name)); else Run.remove(Name);
    }
    return Out;
}

std::vector<QStringList> BoxIniCommands(const std::string &Box, const Options &Opts,
                                        const std::function<std::string(const std::string &)> &MountTarget)
{
    const QString B = QString::fromStdString(Box);
    auto Win = [](const std::string &P) {
        QString G = QString::fromStdString(P);
        G.replace('/', '\\');
        while (G.size() > 3 && G.endsWith('\\')) G.chop(1);   // "C:\" keeps its separator
        return G;
    };
    //A box with a root of its own is the game's prefix (BoxPrefix): VidyaGod fills it before the game and empties it
    //after, so Sandboxie must not delete it when its last program ends (that is between the registry import and the
    //game). Without one, Sandboxie's default root, deleted as before.
    std::vector<QStringList> Out = { {"set", B, "Enabled", "y"},
                                     {"set", B, "AutoDelete", Opts.BoxRoot.empty() ? "y" : "n"},
                                     {"set", B, "FileRootPath", Opts.BoxRoot.empty() ? QString() : Win(Opts.BoxRoot)},
                                     {"set", B, "DropAdminRights", "y"} };
    QStringList Grants;
    auto Grant = [&](const std::string &P) {
        const QString G = Win(P);
        if (!G.isEmpty() && !Grants.contains(G, Qt::CaseInsensitive)) Grants << G;
    };
    //Only the mounts: the kept folders are reached through them (their passthrough), and a grant on TEMP or the
    //instance folder would let the game write there behind the mount's back.
    for (const Mount &M : Opts.Mounts) { Grant(M.Mountpoint); Grant(MountTarget(M.Mountpoint)); }
    // "set" replaces every value the box had, so a box re-used by the next launch holds this launch's grants only.
    Out.push_back({"set", B, "OpenFilePath", Grants.isEmpty() ? QString() : Grants.first()});
    for (int I = 1; I < Grants.size(); ++I) Out.push_back({"append", B, "OpenFilePath", Grants.at(I)});
    return Out;
}

QStringList BoxStartArgs(const std::string &Box, const std::string &Program, const QStringList &Arguments)
{
    QString P = QString::fromStdString(Program);
    P.replace('/', '\\');
    return QStringList{"/box:" + QString::fromStdString(Box), "/silent", "/wait", P} + Arguments;
}

void WrapGame(Options Opts, std::string &Program, QStringList &Arguments, QProcessEnvironment &RunEnv,
              const QProcessEnvironment &BaseEnv)
{
#ifndef _WIN32
    // The private home exists before the game starts: it is HOME, and some programs only create their folder in an
    // XDG directory, not the directory itself.
    if (!Opts.Home.empty())
        for (const char *Sub : {"", ".config", ".local/share", ".local/state", ".cache"})
        {
            std::error_code Ec;
            std::filesystem::create_directories(std::filesystem::path(Opts.Home) / Sub, Ec);
            if (Ec) LogErr("SandboxLayer::WrapGame", "could not create the private home (" + Opts.Home + "/" + Sub
                           + "): " + Ec.message() + " — the game starts without it");
        }
    Opts.GameEnv = TakeLoaderEnv(RunEnv, BaseEnv);
#else
    (void)RunEnv; (void)BaseEnv;
#endif
    Wrap(Opts, Program, Arguments);
}

#ifndef _WIN32   // ---- Linux bubblewrap backend. The Windows Sandboxie backend is in sandboxlayer_win.cpp ----
void PrepareBox(const Options &) {}
int RunInBox(const Options &, const std::string &, const QStringList &, int) { return -1; }
bool EmptyBox(const Options &) { return true; }

bool Available()
{
    // Functional probe (not just `which bwrap`): actually create a throwaway sandbox with the SAME namespace + cap
    // flags the real launch uses, running `true`. Exit 0 ⇒ this machine can sandbox. Not cached: a few ms per launch,
    // and a bwrap installed while the app runs (or after Settings → Dependencies said it was missing) is seen at once.
    // Silenced — the caller reports what a failure means.
    // `env -u LD_LIBRARY_PATH` mirrors SystemToolEnv (the real launch strips the AppImage's bundled lib path so the
    // system bwrap loads host libs); without it the probe could spuriously fail under an AppImage.
    return std::system(
        "env -u LD_LIBRARY_PATH bwrap --unshare-user --unshare-pid --cap-add CAP_SYS_ADMIN "
        "--ro-bind / / --dev-bind /dev /dev --proc /proc -- true > /dev/null 2>&1") == 0;   // `true` via PATH (no /bin/true on NixOS)
}

// Resolve this executable's path so the sandbox can re-invoke it as `--sandbox-init` (it's already visible via
// --ro-bind / /).
static std::string SelfExe()
{
    std::filesystem::path Self = Platform::SelfExe();
    return Self.empty() ? std::string("VidyaGod") : Self.string();
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
    // Isolated net: keep CAP_NET_ADMIN (bring up the overlay TUN) + CAP_NET_RAW (the game's own sockets, incl. ICMP for
    // ping) — both are namespaced to the sandbox's netns, powerless outside it.
    if (Isolated) A << "--unshare-net" << "--cap-add" << "CAP_NET_ADMIN" << "--cap-add" << "CAP_NET_RAW";
    A << "--ro-bind" << "/" << "/"            // whole host READ-ONLY (native apps can't write the system)
      << "--dev-bind" << "/dev" << "/dev"     // GPU (/dev/dri) + /dev/fuse + /dev/net/tun
      << "--proc" << "/proc";

    // Writable surfaces over the RO root (the vidyagodfs mount lives under TempPath; saves under UserData/home).
    auto bindRW = [&](const std::string &P){
        if (!P.empty() && std::filesystem::exists(P)) { A << "--bind" << QString::fromStdString(P) << QString::fromStdString(P); }
    };
    for (const std::string &P : Opts.WritableBinds) bindRW(P);
    bindRW("/tmp");                                                        // X11 socket + scratch
#ifdef __linux__
    bindRW("/run/user/" + std::to_string(static_cast<unsigned>(::getuid()))); // Wayland / PipeWire / Pulse
#endif
    // The real GPU shader caches into the private home. After /tmp: the home lives under TempPath, which is under
    // /tmp, and a later bind of /tmp would cover them.
    for (const auto &[Real, InHome] : Opts.HomeCaches)
        if (std::error_code Ec; std::filesystem::is_directory(Real, Ec)) A << "--bind" << QString::fromStdString(Real) << QString::fromStdString(InHome);

    // The payload: re-invoke ourselves as the in-sandbox init, which mounts the specs (+ TUN) then execs the game.
    A << "--" << QString::fromStdString(SelfExe()) << "--sandbox-init";
    for (const Mount &M : Opts.Mounts)
        A << "--mount" << QString::fromStdString(M.SpecPath) << QString::fromStdString(M.Mountpoint);
    if (!Opts.TunName.empty() && !Opts.TunSock.empty())
        A << "--tun" << QString::fromStdString(Opts.TunName) << QString::fromStdString(Opts.TunCidr)
          << QString::fromStdString(Opts.TunSock) << (Opts.TunBridge ? "bridge" : "nobridge");
    if (!Opts.WorkDir.empty())
        A << "--chdir" << QString::fromStdString(Opts.WorkDir);
    if (!Opts.Cloak.empty())
        A << "--cloak" << QString::fromStdString(Opts.Cloak);
    for (const GameEnvVar &E : Opts.GameEnv)
        if (E.Set) A << "--env" << QString::fromStdString(E.Name + "=" + E.Value);
        else       A << "--unenv" << QString::fromStdString(E.Name);
    A << "--" << OrigProgram << OrigArgs;

    Program   = "bwrap";
    Arguments = A;
}

void TerminateBox(const Options &) {}   // no-op: bwrap dies with the game's process group
#endif // !_WIN32

} // namespace SandboxLayer
