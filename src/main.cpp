#include "main.h"
#include "apppaths.h"
#include "commonutils.h"
#include "manifestmodel.h"
#include "packagecatalog.h"
#include "vgdelta.h"       // .vgdelta generate + verify (shared with vidyagodfs) — the --convert-delta-chain tool
#include "prelaunchwindow.h"
#include "gamepicker.h"
#include "containerwrapper.h"
#include "ipfswrapper.h"
#include "sandboxlayer.h"
#include "depcheck.h"
#include "vgipfsapi.h"

#include <QComboBox>
#include <QAbstractScrollArea>
#include <QWheelEvent>

#include <algorithm>
#include <chrono>
#include <thread>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

//Single-instance guard. Opens (creating if needed) a lock file under ~/.VidyaGod and takes an
//exclusive, non-blocking flock on it. Returns the held fd on success, or -1 if another live instance
//already holds it. The lock is owned by the open fd, so the kernel releases it automatically when this
//process exits OR crashes — a previous crashed instance never blocks a new one, and no stale-lock
//bookkeeping is needed. The returned fd is intentionally kept open for the whole process lifetime.
static int AcquireSingleInstanceLock(const std::string &LockPath)
{
    //O_CLOEXEC is essential: spawned children (wine/proton/git/vidyagodfs) must NOT inherit this fd, or a lingering
    //wine background process (services.exe/wineserver/…) keeps the flock held after VidyaGod exits, making every
    //later run falsely abort with "another instance is already running".
    int Fd = ::open(LockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (Fd < 0) return -1;
    if (::flock(Fd, LOCK_EX | LOCK_NB) != 0) { ::close(Fd); return -1; }
    return Fd;
}


//Application-wide event filter that stops a QComboBox from changing its value when the mouse
//wheel is scrolled over it (a very easy way to accidentally mutate settings). Qt's
//QComboBox::wheelEvent cycles the selection on hover-scroll regardless of focus, so the wheel is
//blocked on the (closed) combo control unconditionally — the value is still changed by clicking or
//by arrow keys. The open dropdown LIST is a separate widget, so scrolling an actually-open list
//still works. To keep scrollable pages usable, the swallowed wheel is redirected to the nearest
//enclosing scroll area so the page still scrolls past the combo. Installed once on the
//QApplication, so it covers every combo, including ones created dynamically.
class WheelGuard : public QObject
{
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject * Obj, QEvent * Event) override
    {
        if (Event->type() == QEvent::Wheel)
            if (QComboBox * Combo = qobject_cast<QComboBox *>(Obj))
            {
                //Forward the scroll to the surrounding scroll area (if any) instead of the combo.
                for (QWidget * W = Combo->parentWidget(); W; W = W->parentWidget())
                    if (QAbstractScrollArea * SA = qobject_cast<QAbstractScrollArea *>(W))
                    {
                        QCoreApplication::sendEvent(SA->viewport(), Event);
                        break;
                    }
                return true; //never let the hovered combo consume the wheel
            }
        return QObject::eventFilter(Obj, Event);
    }
};

//Serializes the resolved ContainerParams that feed the (frozen) runtime — the exact input the engine produces
//after the front-end resolution. Used by --resolve-only for golden-compare (synthesis bridge vs native consumer)
//and as a hang-free verification (no mount, no game). Field order is stable for diffing.
static nlohmann::ordered_json DumpResolution(const struct ContainerParams &CP)
{
    nlohmann::ordered_json J;
    J["PackageUID"]        = CP.PackageUID;
    J["GameName"]          = CP.GameName;
    J["Platform"]          = CP.Platform;
    J["Recipe"]            = CP.Recipe;
    J["RunnerName"]        = CP.RunnerName;
    //Runner daisy-chain (innermost→outermost): each link's node id, host, and exec — the shortest route resolved to
    //run this content on the machine, always ending in the native terminal.
    {
        nlohmann::ordered_json Chain = nlohmann::ordered_json::array();
        for (const RunnerLink &L : CP.RunnerChain)
            Chain.push_back({{"NodeId", L.NodeId}, {"Host", L.HostPlatform}, {"Guests", L.GuestPlatform},
                             {"Executable", L.Executable}, {"Native", L.NativeNamespace()}});
        J["RunnerChain"] = Chain;
    }
    J["RunnerExecutable"]  = CP.RunnerExecutable;
    J["RunnerArgs"]        = CP.RunnerArgs;
    J["RunnerEnv"]         = CP.RunnerEnv;
    J["RunnerRemoveEnv"]   = CP.RunnerRemoveEnv;
    J["ContentRoot"]       = CP.ContentRoot;
    J["PrefixRoot"]        = CP.PrefixRoot;
    J["PrefixGenerate"]    = CP.PrefixGenerate;
    J["ContentPath"]       = CP.ExePathRelative.string();
    J["Content"]           = CP.ExePathComplete.string();
    J["ExeArgs"]           = CP.ExeArgs;
    J["DLLOverrides"]      = CP.DLLOverrides;
    J["PersistAll"]        = CP.PersistAll;
    J["KeepDirs"]          = CP.KeepDirs;
    J["KeepFiles"]         = CP.KeepFiles;
    J["KeepRegHives"]      = CP.KeepRegHives;
    J["KeepRegKeys"]       = CP.KeepRegKeys;
    J["DropPaths"]         = CP.DropPaths;
    J["CustomVariables"]   = CP.CustomVariables;
    J["RunnerShipsBuild"]  = CP.RunnerShipsBuild;
    J["UnifiedRuntime"]    = CP.UnifiedRuntime;
    J["SubComponentsArray"]= CP.SubComponentsArray;
    return J;
}

//Headless "play together" capstone: bring the overlay up inside a NESTED sandbox (the real launch path, not the
//root-requiring host TUN) and ping the peer's vIP THROUGH the tunnel. Spawns bwrap running the sandbox-init, which
//creates + addresses the TUN inside the sandbox netns and hands its fd to our libp2p forwarder; the sandboxed shell
//then pings the peer. Returns the ping's exit code (0 = replies received = packets crossed the tunnel). This is what
//a real session game launch does, with `ping` standing in for the game. Rootless (CAP_NET_ADMIN via the userns).
static int RunOverlaySandboxPing()
{
    // Pull our vIP + the online friends' vIPs from the friend LAN (host-less; each vIP = f(peerID)); ping the first.
    const auto V = IpfsWrapper::LanLaunchVars();
    auto Get = [&](const char *K){ auto It = V.find(K); return It == V.end() ? std::string() : It->second; };
    const std::string MyVip = Get("VIDYAGOD_SELF_VIP"), Subnet = Get("VIDYAGOD_SUBNET");
    const std::string PeerVips = Get("VIDYAGOD_PEER_VIPS");
    const std::string PeerVip = PeerVips.substr(0, PeerVips.find(','));   // first online friend
    if (MyVip.empty() || PeerVip.empty()) { LogErr("main.cpp", "overlay: no self/peer vIP (need an online friend)"); return -1; }
    if (!SandboxLayer::Available()) { LogErr("main.cpp", "overlay: sandbox unavailable (no bwrap / userns disabled)"); return -1; }

    std::string Mask = "16";
    if (auto s = Subnet.rfind('/'); s != std::string::npos) Mask = Subnet.substr(s + 1);
    const std::string Sock = "/tmp/vgov-lan.sock";

    std::string Err;
    if (!IpfsWrapper::OverlayServe(Sock, &Err)) { LogErr("main.cpp", "overlay serve failed: " + Err); return -1; }

    SandboxLayer::Options O;
    O.Enabled = true;
    O.Net     = SandboxLayer::NetMode::Isolated;
    O.TunName = "vg-lan";
    O.TunCidr = MyVip + "/" + Mask;
    O.TunSock = Sock;
    std::string Program = "/bin/sh";
    QStringList Args{ "-c",
        QString::fromStdString("ip -o addr show 2>/dev/null | sed 's/^/[sandbox] /'; "
                               "echo '[sandbox] pinging peer " + PeerVip + " over the overlay…'; "
                               "sleep 5; ping -c 10 -W 3 " + PeerVip) };
    SandboxLayer::Wrap(O, Program, Args);   // → Program="bwrap", Args=full bwrap argv ending in `-- /bin/sh -c …`

    std::vector<std::string> Argv{ Program };
    for (const QString &A : Args) Argv.push_back(A.toStdString());
    std::vector<char *> C;
    for (auto &s : Argv) C.push_back(const_cast<char *>(s.c_str()));
    C.push_back(nullptr);

    LogOut("main.cpp", "overlay sandbox up (tun " + O.TunCidr + ") — pinging peer " + PeerVip + " through the tunnel…");
    int status = 0;
    pid_t pid = fork();
    if (pid == 0) { execvp(C[0], C.data()); _exit(127); }
    if (pid > 0) waitpid(pid, &status, 0);
    IpfsWrapper::OverlayStop();
    const int rc = (pid > 0 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    if (rc == 0) LogSucc("main.cpp", "OVERLAY PING SUCCEEDED — packets crossed the tunnel to " + PeerVip);
    else         LogErr ("main.cpp", "overlay ping did not get replies (rc=" + std::to_string(rc) + ")");
    return rc;
}

int main(int argc, char *argv[])
{
    //IN-SANDBOX INIT: when invoked as `VidyaGod --sandbox-init …` (bwrap's payload inside a nested sandbox), do the
    //low-level setup (mount vidyagodfs, bring up the overlay TUN + hand its fd to the parent) and execvp the game.
    //Must be the FIRST thing in main() — before the single-instance lock, config init, or any Qt — because this is
    //PID ~1 of a throwaway namespace, not a normal VidyaGod run.
    if (SandboxLayer::IsSandboxInit(argc, argv))
        return SandboxLayer::RunSandboxInit(argc, argv);

    //Parse command line arguments and initialize RuntimeParameters struct.
    //Must happen before QApplication so headless runs never touch the display.
    LaunchParameters LaunchParameters = ParseCommandLineArguments(argc, argv);
    LogOut("main.cpp", "Running VidyaGod in " + LaunchParameters.CurrentPath.string());
    LogOut("main.cpp", "Headless PackagePath: " + LaunchParameters.HeadlessPackagePath.string());
    LogOut("main.cpp", "Headless GameID: " + LaunchParameters.HeadlessGameID);
    LogOut("main.cpp", "Headless ComponentID: " + LaunchParameters.HeadlessComponentID);

    //Check if dependencies exist in the system.
    //Non-fatal: warns but continues so the GUI still opens when VFS is not needed. In GUI mode a dialog
    //(below, once QApplication exists) lists anything missing with install instructions.
    const std::vector<std::string> MissingDeps = DepCheck::MissingCritical();

    //Auto-detect package-directory mode: if no --package flag was passed, check whether
    //the current working directory itself is a package. This lets users simply cd into a
    //package and run the binary directly without any extra flags.
    if (!LaunchParameters.HasHeadlessPackagePath && LaunchParameters.DataDir.empty())
    {
        //If the working directory is itself a package bundle (has node files), run it self-contained: a single
        //PreLaunchWindow (no library, no IPFS), with ALL paths inside the package dir — equivalent to
        //  --data-dir DIR --package-dir DIR --runtime-dir DIR --userdata-dir DIR.
        //It is a GUI dialog (not headless) — even one game has runner/variant/module configs to choose.
        LogOut("main.cpp", "Checking if running in a PACKAGEDIR.");
        if (IsRunningInPackageDir(LaunchParameters.CurrentPath))
        {
            const std::string Dir = LaunchParameters.CurrentPath.string();
            LaunchParameters.HasHeadlessPackagePath = true;
            LaunchParameters.RunningInPackageDir = true;
            LaunchParameters.HeadlessPackagePath = LaunchParameters.CurrentPath;
            LaunchParameters.DataDir             = Dir;   // AppDataPath = package dir
            LaunchParameters.PackageDirOverride  = Dir;
            LaunchParameters.RuntimeDirOverride  = Dir;
            LaunchParameters.UserDataDirOverride = Dir;
        }
    }

    //Data dir: normally ~/.VidyaGod, but PORTABLE mode keeps everything beside the app so the whole
    //thing travels on a USB stick. The "app location" the user sees is the AppImage file ($APPIMAGE) when
    //running as one, else the binary's own directory. Portable is enabled by placing either a "portable"
    //marker file or a "VidyaGod_data" directory next to the app.
    std::filesystem::path AppLocation;
    if (const char *AppImagePath = ::getenv("APPIMAGE"))
        AppLocation = std::filesystem::path(AppImagePath).parent_path();
    else
    {
        std::error_code SelfEc;
        std::filesystem::path Self = std::filesystem::read_symlink("/proc/self/exe", SelfEc);
        if (!SelfEc) AppLocation = Self.parent_path();
    }

    QString AppDataPath = QDir::homePath() + "/.VidyaGod";
    bool Portable = false;
    if (!AppLocation.empty())
    {
        std::error_code Ec;
        std::filesystem::path PortableData = AppLocation / "VidyaGod_data";
        if (std::filesystem::exists(AppLocation / "portable", Ec) ||
            std::filesystem::exists(AppLocation / "portable.txt", Ec) ||
            std::filesystem::is_directory(PortableData, Ec))
        {
            AppDataPath = QString::fromStdString(PortableData.string());
            Portable = true;
        }
    }

    //--data-dir override (testing: run a second instance with its own repo + lock alongside the main one).
    if (!LaunchParameters.DataDir.empty())
        AppDataPath = QString::fromStdString(LaunchParameters.DataDir);

    //Find and create AppDataDir. mkpath is a no-op if it already exists.
    QDir AppDataDir(AppDataPath);
    AppDataDir.mkpath(".");
    //Publish the resolved root as the single source of truth for every VidyaGod-produced path (launch TEMP, the
    //LIBRARY default, …) so portable / --data-dir relocate the WHOLE footprint — not just the ipfs repo + lock.
    AppPaths::SetDataRoot(AppDataDir.absolutePath().toStdString());
    //Record the run mode — it gates daemon/tray + start-on-login (see AppPaths::Mode). In-package wins (run + exit,
    //no daemon); then explicit CLI paths; then portable; else normal.
    AppPaths::SetMode(LaunchParameters.RunningInPackageDir ? AppPaths::Mode::InPackage
                      : !LaunchParameters.DataDir.empty()  ? AppPaths::Mode::CliPaths
                      : Portable                           ? AppPaths::Mode::Portable
                                                           : AppPaths::Mode::Normal);
    //Per-launch path overrides (--package-dir / --runtime-dir / --userdata-dir), read by the launch engine.
    if (!LaunchParameters.PackageDirOverride.empty())  AppPaths::SetPackagePathOverride(LaunchParameters.PackageDirOverride);
    if (!LaunchParameters.RuntimeDirOverride.empty())  AppPaths::SetRuntimePathOverride(LaunchParameters.RuntimeDirOverride);
    if (!LaunchParameters.UserDataDirOverride.empty()) AppPaths::SetUserDataPathOverride(LaunchParameters.UserDataDirOverride);
    LogOut("main.cpp", (Portable ? "Portable mode — data dir: " : "Data dir: ") + AppDataPath.toStdString());

    //Single-instance guard (GUI and headless alike): VidyaGod may only run once at a time. This both
    //prevents two instances from fighting over the same runtime/TEMP and makes the stale-mount sweep
    //below safe — since we hold the lock, any leftover mounts must be from a crashed run, never a live
    //sibling. The fd is held for the whole process lifetime (released by the kernel on exit/crash).
    //--bypass-single-instance-lock: proceed even if the lock is held (e.g. a headless --resolve-only/--validate-nodes
    //check while the GUI is open). Skips the guard — so the stale-mount sweep below can no longer assume a leftover
    //mount is from a crashed run, not a live sibling; use it only for read-only/CLI runs you know are safe.
    bool BypassInstanceLock = false;
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--bypass-single-instance-lock") BypassInstanceLock = true;

    int InstanceLockFd = AcquireSingleInstanceLock((AppDataDir.absolutePath() + "/vidyagod.lock").toStdString());
    if (InstanceLockFd < 0 && BypassInstanceLock)
        LogWarn("main.cpp", "Single-instance lock held, but --bypass-single-instance-lock given — proceeding without the lock.");
    else if (InstanceLockFd < 0)
    {
        LogErr("main.cpp", "Another instance of VidyaGod is already running — aborting.");
        if (!LaunchParameters.RunningHeadless)
        {
            QApplication ErrApp(argc, argv);
            QMessageBox::warning(nullptr, "VidyaGod is already running",
                                 "Another instance of VidyaGod is already running.\n\n"
                                 "Only one instance can run at a time.");
        }
        return 1;
    }

    //We are the sole instance, so every mount still present under TEMP is a leftover from a crash.
    //Sweep them all now (the per-launch CleanStaleRuntime only covers the one game about to run).
    VfsMount::CleanStaleRuntime((AppDataDir.absolutePath() + "/TEMP").toStdString());

    //Initialization of GlobalConfigJSON, the central data structure of the program.
    //All runner definitions, library entries, and user settings live here.
    nlohmann::ordered_json GlobalConfigJSON;
    if (InitializeGlobalConfigJSON(&GlobalConfigJSON, &AppDataDir))
    {
        //InitializeGlobalConfigJSON returns true on FAILURE (shell exit-code convention).
        LogErr("main.cpp", "Fatal error. GlobalConfigJSON initialization failed, aborting.");
        return 1;
    }

    //Embedded IPFS node (libvgipfs): its private repo lives INSIDE the app data dir (<AppDataDir>/ipfs) so it shares
    //the lifecycle of the content it references (deleting the data dir wipes pins/refs = true clean slate).
    //Networking is OFF by default in the GUI and the node start is DEFERRED to after the window shows (it delays
    //startup) — MainWindow brings it up when the user enables networking. Headless CLI modes that actually need the
    //node (fetch/seed/import/publish/peer-id/--node launch) start it up front here; the path-only modes
    //(validate/list/resolve) and in-package launch never touch it.
    const std::string IpfsRepo = (AppDataDir.absolutePath() + "/IPFS").toStdString();
    const bool HeadlessNeedsNode =
        LaunchParameters.PrintPeerId || LaunchParameters.PrintPinLs || !LaunchParameters.UnpinCid.empty()
        || !LaunchParameters.DropRefCid.empty()
        || !LaunchParameters.FetchCid.empty() || !LaunchParameters.SeedDir.empty()
        || !LaunchParameters.ImportRunnerId.empty() || !LaunchParameters.ImportPackageUid.empty()
        || !LaunchParameters.PublishPackageDir.empty() || !LaunchParameters.PublishCidDir.empty()
        || !LaunchParameters.PublishMetaSrc.empty()
        || LaunchParameters.PrintFriendCode || LaunchParameters.FriendListOnly
        || !LaunchParameters.FriendAddCode.empty() || LaunchParameters.FriendServe
        || LaunchParameters.LanHarness
        || !LaunchParameters.LaunchNodeId.empty();
    if (HeadlessNeedsNode)
        IpfsWrapper::StartNode(IpfsRepo);   // non-fatal: a failed start just means fetches/seeds report errors

    //HEADLESS: print this node's peer ID + dialable addrs, then exit (so another node can --connect to it).
    if (LaunchParameters.PrintPeerId)
    {
        for (int i = 0; i < 30 && !IpfsWrapper::DaemonRunning(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        LogOut("main.cpp", "PeerID: " + IpfsWrapper::PeerID());
        for (const std::string &A : IpfsWrapper::ListenAddrs()) LogOut("main.cpp", "addr: " + A);
        return 0;
    }

    //HEADLESS: list the recursively-pinned (seeded) CIDs in this repo, then exit (debug: what the IPFS tab would show).
    if (LaunchParameters.PrintPinLs)
    {
        for (int i = 0; i < 30 && !IpfsWrapper::DaemonRunning(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        const std::vector<IpfsWrapper::PinEntry> Pins = IpfsWrapper::Pins();
        LogOut("main.cpp", "Pins: " + std::to_string(Pins.size()));
        for (const auto &P : Pins) LogOut("main.cpp", "pin: " + P.Cid);
        return 0;
    }

    //HEADLESS: drop a recursive pin (stop seeding + surfacing a superseded CID), then exit.
    if (!LaunchParameters.UnpinCid.empty())
    {
        for (int i = 0; i < 30 && !IpfsWrapper::DaemonRunning(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        const bool Ok = IpfsWrapper::Unpin(LaunchParameters.UnpinCid);
        LogOut("main.cpp", (Ok ? "Unpinned " : "Unpin failed / not pinned: ") + LaunchParameters.UnpinCid);
        return Ok ? 0 : 1;
    }

    //HEADLESS: delete a CID's filestore-reference closure (+ unpin), so a subsequent add re-references its content at
    //its CURRENT path — the repair for an orphaned/stale reference the plain re-add would otherwise dedup-skip. Then exit.
    if (!LaunchParameters.DropRefCid.empty())
    {
        for (int i = 0; i < 30 && !IpfsWrapper::DaemonRunning(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        const bool Ok = IpfsWrapper::DropRef(LaunchParameters.DropRefCid);
        LogOut("main.cpp", (Ok ? "Dropped reference closure for " : "Drop-ref failed: ") + LaunchParameters.DropRefCid);
        return Ok ? 0 : 1;
    }

    //HEADLESS: print this node's shareable friend code (its peer ID), then exit.
    if (LaunchParameters.PrintFriendCode)
    {
        for (int i = 0; i < 30 && !IpfsWrapper::DaemonRunning(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        LogOut("main.cpp", "FriendCode: " + IpfsWrapper::FriendCode());
        std::cout << IpfsWrapper::FriendCode() << "\n";   // machine-readable on stdout
        return 0;
    }

    //HEADLESS: print the address book (contacts + friendship state), then exit.
    if (LaunchParameters.FriendListOnly)
    {
        const std::vector<IpfsWrapper::Contact> Cs = IpfsWrapper::FriendList();
        LogOut("main.cpp", "Contacts: " + std::to_string(Cs.size()));
        for (const auto &C : Cs)
            LogOut("main.cpp", "  " + C.State + "  " + C.PeerID + "  nick='" + C.Nick + "'"
                   + (C.Online ? "  [online]" : ""));
        return 0;
    }

    //HEADLESS friends test harness: --friend-serve (stay online, auto-accept incoming) and --friend-add <CODE> (send a
    //request). Both warm the node online, optionally --connect a known peer for a deterministic direct link (no DHT
    //discovery needed over e.g. WireGuard), set our --friend-nick, then poll the address book for --friend-secs
    //seconds — auto-accepting any incoming request and logging state changes — before printing the final book. This
    //lets two machines complete a real mutual-consent handshake headlessly (see [[reference_headless_e2e_testing]]).
    if (LaunchParameters.FriendServe || !LaunchParameters.FriendAddCode.empty())
    {
        for (int i = 0; i < 30 && IpfsWrapper::PeerCount() < 1; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!LaunchParameters.ConnectAddr.empty())
        {
            const bool Ok = IpfsWrapper::Connect(LaunchParameters.ConnectAddr);
            LogOut("main.cpp", std::string("direct connect to ") + LaunchParameters.ConnectAddr + (Ok ? " : ok" : " : FAILED"));
        }
        if (!LaunchParameters.FriendNick.empty())
            IpfsWrapper::SetProfile(LaunchParameters.FriendNick, std::string());
        LogOut("main.cpp", std::string("friends: online=") + (IpfsWrapper::DaemonRunning() ? "yes" : "no")
               + " peers=" + std::to_string(IpfsWrapper::PeerCount()) + " code=" + IpfsWrapper::FriendCode());
        if (!LaunchParameters.FriendAddCode.empty())
        {
            std::string Err;
            const bool Ok = IpfsWrapper::FriendAdd(LaunchParameters.FriendAddCode, "hello from vidyagod", &Err);
            LogOut("main.cpp", Ok ? ("friend request sent to " + LaunchParameters.FriendAddCode)
                                  : ("friend request FAILED: " + Err));
        }
        std::map<std::string, std::string> LastState;   // peer → last-seen state, to log transitions
        for (int s = 0; s < LaunchParameters.FriendSecs; ++s)
        {
            for (const auto &C : IpfsWrapper::FriendList())
            {
                if (C.State == "incoming")   // auto-accept any inbound request
                {
                    std::string Err;
                    if (IpfsWrapper::FriendAccept(C.PeerID, &Err))
                        LogOut("main.cpp", "auto-accepted incoming request from " + C.PeerID + " ('" + C.Nick + "')");
                }
                if (LastState[C.PeerID] != C.State)
                {
                    LogOut("main.cpp", "contact " + C.PeerID + " -> " + C.State + " ('" + C.Nick + "')");
                    LastState[C.PeerID] = C.State;
                }
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        LogOut("main.cpp", "final address book:");
        int Accepted = 0;
        for (const auto &C : IpfsWrapper::FriendList())
        {
            if (C.State == "accepted") ++Accepted;
            LogOut("main.cpp", "  " + C.State + "  " + C.PeerID + "  nick='" + C.Nick + "'");
        }
        LogSucc("main.cpp", "friends handler done — " + std::to_string(Accepted) + " accepted friend(s)");
        return 0;
    }

    //HEADLESS friend-LAN harness: --lan warms online, optionally --connect a known peer (deterministic link over
    //WireGuard) + sets --friend-nick, then prints the virtual-LAN roster (our vIP + each online friend's vIP, all
    //host-less: vIP = f(peerID)) for --friend-secs seconds. With --overlay it brings the sandboxed overlay TUN up
    //inside a rootless bwrap netns and pings the first online friend through the tunnel — the end-to-end LAN test.
    if (LaunchParameters.LanHarness)
    {
        for (int i = 0; i < 30 && IpfsWrapper::PeerCount() < 1; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!LaunchParameters.ConnectAddr.empty())
        {
            const bool Ok = IpfsWrapper::Connect(LaunchParameters.ConnectAddr);
            LogOut("main.cpp", std::string("direct connect to ") + LaunchParameters.ConnectAddr + (Ok ? " : ok" : " : FAILED"));
        }
        if (!LaunchParameters.FriendNick.empty())
            IpfsWrapper::SetProfile(LaunchParameters.FriendNick, std::string());
        LogOut("main.cpp", std::string("lan: online=") + (IpfsWrapper::DaemonRunning() ? "yes" : "no")
               + " peers=" + std::to_string(IpfsWrapper::PeerCount()) + " code=" + IpfsWrapper::FriendCode());

        std::string LastRoster;   // print only when the LAN roster changes
        bool OverlayTried = false;
        for (int s = 0; s < LaunchParameters.FriendSecs; ++s)
        {
            const auto V = IpfsWrapper::LanLaunchVars();
            auto Get = [&](const char *K){ auto It = V.find(K); return It == V.end() ? std::string() : It->second; };
            const std::string Peers = Get("VIDYAGOD_PEER_VIPS");
            const std::string Now = "self " + Get("VIDYAGOD_SELF_VIP") + " | friends: " + (Peers.empty() ? "(none online)" : Peers);
            if (Now != LastRoster) { LogOut("main.cpp", "lan roster: " + Now); LastRoster = Now; }
            //Once at least one friend is online, bring the overlay up (nested sandbox — what a real game launch does)
            //and ping the friend through the tunnel. Attempt once; log the outcome.
            if (LaunchParameters.OverlayUp && !OverlayTried && !Peers.empty())
            {
                OverlayTried = true;
                RunOverlaySandboxPing();
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        LogSucc("main.cpp", "friend-LAN handler done");
        return 0;
    }

    //HEADLESS: fetch a single CID to a destination path then exit — a download throughput probe (any public/private
    //CID). Waits for the node's network to come up, times the transfer, and reports MB/s.
    if (!LaunchParameters.FetchCid.empty())
    {
        LogOut("main.cpp", "Fetch test: " + LaunchParameters.FetchCid + " -> " + LaunchParameters.FetchDest);
        // Warm up first: wait for a real peer set (or 30 s) so the timed fetch reflects a RUNNING node (the GUI),
        // not a cold start with 0 peers + an empty DHT routing table.
        for (int i = 0; i < 30 && IpfsWrapper::PeerCount() < 30; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        // Optional direct peering: dial a known seed so the transfer is peer-to-peer direct (no DHT/relay lottery).
        if (!LaunchParameters.ConnectAddr.empty())
        {
            const bool Ok = IpfsWrapper::Connect(LaunchParameters.ConnectAddr);
            LogOut("main.cpp", std::string("direct connect to ") + LaunchParameters.ConnectAddr + (Ok ? " : ok" : " : FAILED"));
        }
        LogOut("main.cpp", std::string("warmed: online=") + (IpfsWrapper::DaemonRunning() ? "yes" : "no")
               + " peers=" + std::to_string(IpfsWrapper::PeerCount()));
        const auto T0 = std::chrono::steady_clock::now();
        std::string Err;
        // QUEUE TEST (VG_QUEUE_TEST=1): enqueue the SAME CID at TWO dests through the CID-addressed DownloadQueue and
        // assert it fetched ONCE (dedup) and cross-dest materialized (dest2 hard-linked to dest — same inode). With
        // VG_FETCH_DEBUG=1 the log shows a single "fetchToPath ENTER" for the CID. Exercises the whole queue headlessly.
        if (std::getenv("VG_QUEUE_TEST"))
        {
            const std::string Dest2 = LaunchParameters.FetchDest + ".2";
            std::error_code Rc; std::filesystem::remove(Dest2, Rc);
            const bool Ok = IpfsWrapper::FetchTargetsConcurrent(
                { { LaunchParameters.FetchCid, LaunchParameters.FetchDest, false },
                  { LaunchParameters.FetchCid, Dest2,                      false } }, &Err);
            if (!Ok) { LogErr("main.cpp", "Queue test fetch failed: " + Err); return 1; }
            std::error_code E1, E2, E3;
            const bool Both  = std::filesystem::exists(LaunchParameters.FetchDest, E1) && std::filesystem::exists(Dest2, E2);
            const bool Same  = Both && std::filesystem::equivalent(LaunchParameters.FetchDest, Dest2, E3);
            const auto Sz1 = std::filesystem::file_size(LaunchParameters.FetchDest, E1);
            const auto Sz2 = std::filesystem::file_size(Dest2, E2);
            if (Both && Same && !E1 && !E2 && Sz1 == Sz2)
                LogSucc("main.cpp", "Queue test PASS: single fetch, cross-dest hard-link (" + std::to_string(Sz1) + " bytes, same inode)");
            else
            { LogErr("main.cpp", "Queue test FAIL: both=" + std::to_string(Both) + " same-inode=" + std::to_string(Same)
                     + " sz1=" + std::to_string(E1?0:Sz1) + " sz2=" + std::to_string(E2?0:Sz2)); return 1; }
            return 0;
        }
        if (LaunchParameters.FetchDirMode)
        {
            // Recursively materialize a FOLDER CID (the add-by-CID path) — verifies the node-file blocks are served.
            const std::string Got = IpfsWrapper::FetchDirToPath(LaunchParameters.FetchCid, LaunchParameters.FetchDest, &Err);
            const double Secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count();
            if (Got.empty()) { LogErr("main.cpp", "Fetch (dir) failed: " + Err); return 1; }
            std::error_code Ec; std::uintmax_t Bytes = 0; int Files = 0;
            if (std::filesystem::is_directory(LaunchParameters.FetchDest, Ec))
                for (const auto &E : std::filesystem::recursive_directory_iterator(LaunchParameters.FetchDest, Ec))
                    if (E.is_regular_file(Ec)) { ++Files; Bytes += E.file_size(Ec); }
            LogSucc("main.cpp", "Fetched folder: " + std::to_string(Files) + " file(s), " + std::to_string(Bytes)
                    + " bytes in " + std::to_string(Secs) + "s");
            return 0;
        }
        // CONCURRENT-FETCH probe (VG_FETCH_MULTI="cid2,cid3,…"): fetch FetchCid + the listed CIDs together through the
        // real DownloadQueue (FetchTargetsConcurrent), to reproduce/diagnose concurrent downloads stalling. With
        // VG_FETCH_DEBUG the per-file bitswap sessions + stalls are visible.
        if (const char *ML = std::getenv("VG_FETCH_MULTI"))
        {
            std::vector<IpfsWrapper::FetchTarget> Targets{ { LaunchParameters.FetchCid, LaunchParameters.FetchDest + ".0", false } };
            std::string List = ML;
            int i = 1;
            while (!List.empty())
            {
                const size_t c = List.find(',');
                const std::string Cid = List.substr(0, c);
                if (!Cid.empty())
                    Targets.push_back({ Cid, LaunchParameters.FetchDest + "." + std::to_string(i++), false });
                if (c == std::string::npos) break;
                List = List.substr(c + 1);
            }
            LogOut("main.cpp", "concurrent probe: fetching " + std::to_string(Targets.size()) + " CIDs together");
            const bool Ok = IpfsWrapper::FetchTargetsConcurrent(Targets, &Err);
            const double Secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count();
            LogOut("main.cpp", std::string("concurrent probe ") + (Ok ? "OK" : ("FAILED: " + Err)) + " in " + std::to_string(Secs) + "s");
            return Ok ? 0 : 1;
        }
        const std::string Got = IpfsWrapper::FetchToPath(LaunchParameters.FetchCid, LaunchParameters.FetchDest, &Err);
        const double Secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count();
        if (Got.empty()) { LogErr("main.cpp", "Fetch failed: " + Err); return 1; }
        std::error_code Ec; const auto Sz = std::filesystem::file_size(LaunchParameters.FetchDest, Ec);
        LogSucc("main.cpp", "Fetched " + std::to_string(Ec ? 0 : Sz) + " bytes in "
                + std::to_string(Secs) + "s  (" + std::to_string((Ec ? 0.0 : double(Sz)) / 1048576.0 / Secs) + " MB/s)");
        return 0;
    }

    //HEADLESS: seed a folder of published packages into the IPFS node (re-establish seeding from a master), then exit.
    if (!LaunchParameters.SeedDir.empty())
    {
        LogOut("main.cpp", std::string(LaunchParameters.SeedCoversOnly ? "Seeding COVERS ONLY from: " : "Seeding referenced content from: ")
                           + LaunchParameters.SeedDir);
        int Mismatched = 0;
        const int Seeded = PackageCatalog::SeedDirectory(LaunchParameters.SeedDir,
            [](int done, int total, const std::string &name){
                if (done == total || done % 10 == 0) LogOut("seed", std::to_string(done) + "/" + std::to_string(total) + "  " + name);
            }, &Mismatched, LaunchParameters.SeedCoversOnly, LaunchParameters.SeedOverwrite);
        LogSucc("main.cpp", "Seeded " + std::to_string(Seeded) + " file(s)"
                + (Mismatched ? ("; " + std::to_string(Mismatched) + " changed/un-seedable") : std::string()));
        return 0;
    }

    //HEADLESS: import a runner NODE (fetch its IPFS build + generate its DEFPREFIX artifact) then exit. Composed from
    //the SAME primitives the GUI download pump uses — collect targets → fetch → generate DEFPREFIX — no bespoke path.
    if (!LaunchParameters.ImportRunnerId.empty())
    {
        //--import-runner <RUNNER_NODE_ID> (node runners have no variants — a stray ":suffix" is ignored).
        std::string Id = LaunchParameters.ImportRunnerId;
        if (auto Colon = Id.find(':'); Colon != std::string::npos) Id = Id.substr(0, Colon);
        LogOut("main.cpp", "Importing runner node: " + Id);
        NodeIndex Index = PackageCatalog::BuildCatalogIndex(GlobalConfigJSON);   // repos + locally-added packages
        std::string Err;
        std::vector<IpfsWrapper::FetchTarget> Targets;
        bool Ok = RunnerInstall::CollectRunnerNodeTargets(Index, Id, Targets, &Err)
               && IpfsWrapper::FetchTargetsConcurrent(Targets, &Err)
               && RunnerInstall::GenerateRunnerDefPrefix(Index, Id, /*force*/false, &Err);
        LogOut("main.cpp", Ok ? "Runner imported." : ("Runner import failed: " + Err));
        return Ok ? 0 : 1;
    }

    //HEADLESS: (re)generate ONLY a runner's DEFPREFIX (force rebuild) — the deliberate prefix step, no download. The
    //build must already be hydrated (--import-runner or the GUI pump).
    if (!LaunchParameters.GenerateDefPrefixId.empty())
    {
        std::string Id = LaunchParameters.GenerateDefPrefixId;
        if (auto Colon = Id.find(':'); Colon != std::string::npos) Id = Id.substr(0, Colon);
        LogOut("main.cpp", "Generating DEFPREFIX for runner node: " + Id);
        NodeIndex Index = PackageCatalog::BuildCatalogIndex(GlobalConfigJSON);
        std::string Err;
        const bool Ok = RunnerInstall::GenerateRunnerDefPrefix(Index, Id, /*force*/true, &Err);
        LogOut("main.cpp", Ok ? "DEFPREFIX generated." : ("DEFPREFIX generation failed: " + Err));
        return Ok ? 0 : 1;
    }

    //HEADLESS: hydrate a library game in place (fetch its node closure's IPFS content) then exit. Resolve the
    //launchable node whose UID (or NODE_ID) matches the requested id, then hydrate its content closure.
    if (!LaunchParameters.ImportPackageUid.empty())
    {
        const std::string Uid = LaunchParameters.ImportPackageUid;
        LogOut("main.cpp", "Importing package (node closure) for: " + Uid);
        NodeIndex Index = PackageCatalog::BuildCatalogIndex(GlobalConfigJSON);   // repos + locally-added packages
        std::string LaunchId;
        for (const auto &[NId, N] : Index.Nodes)
            if (N.IsLaunchable() && (N.Uid == Uid || N.NodeId == Uid)) { LaunchId = NId; break; }
        if (LaunchId.empty()) { LogErr("main.cpp", "No launchable node found for '" + Uid + "'."); return 1; }
        std::string Err;
        const bool Ok = PackageCatalog::HydrateNode(Index, LaunchId, {}, &Err);
        LogOut("main.cpp", Ok ? "Package imported." : ("Package import failed: " + Err));
        return Ok ? 0 : 1;
    }

    //HEADLESS: publish (dehydrate) a local package — seed its layer content over IPFS, record the CIDs into the
    //manifest fragments in place, and optionally export a manifest-only copy — then exit.
    if (!LaunchParameters.PublishPackageDir.empty())
    {
        const std::string Dir  = LaunchParameters.PublishPackageDir;
        const std::string Dest = LaunchParameters.PublishToDir;
        LogOut("main.cpp", "Publishing package: " + Dir + (Dest.empty() ? "" : (" -> " + Dest)));
        std::string Err;
        const bool Ok = PackageCatalog::PublishPackage(Dir, Dest, &Err);
        LogOut("main.cpp", Ok ? "Package published." : ("Package publish failed: " + Err));
        return Ok ? 0 : 1;
    }

    //HEADLESS: recursively add a folder (of dehydrated packages) to IPFS BY REFERENCE and print its folder CID — the
    //value to hand out for the Library's "Add by CID". The folder's files are referenced in place, so keep the folder
    //on disk and run VidyaGod online to seed it. Content still seeds per-layer from wherever it lives.
    if (!LaunchParameters.PublishCidDir.empty())
    {
        std::string Err;
        const std::string Cid = IpfsWrapper::AddNoCopy(LaunchParameters.PublishCidDir, &Err);
        if (Cid.empty()) { LogErr("main.cpp", "publish-cid failed: " + Err); return 1; }
        // AddNoCopy eagerly announces the CID to the DHT (async), but this is a short-lived process — wait until the
        // provider record is actually out there (poll findprovs) so the CID is discoverable after we exit, instead of
        // killing the provide goroutine mid-flight. The long-running seeder (same peerID) then serves it.
        LogOut("main.cpp", "announcing " + Cid + " to the DHT…");
        for (int i = 0; i < 40 && IpfsWrapper::ProviderCount(Cid) < 1; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        LogSucc("main.cpp", "CID: " + Cid + " (providers: " + std::to_string(IpfsWrapper::ProviderCount(Cid)) + ")");
        std::cout << Cid << "\n";   // machine-readable on stdout
        return 0;
    }

    //HEADLESS: mint a JSON-only Meta-CID for a bundle or a collection of bundles — content-address content+covers,
    //mirror the JSON-only tree into a persistent staging dir (it seeds from there by reference), add it, print the CID.
    if (!LaunchParameters.PublishMetaSrc.empty())
    {
        const std::string Staging = LaunchParameters.PublishMetaStaging.empty()
            ? (LaunchParameters.PublishMetaSrc + ".meta") : LaunchParameters.PublishMetaStaging;
        std::string Err;
        const std::string Cid = PackageCatalog::PublishMetaCid(LaunchParameters.PublishMetaSrc, Staging, &Err);
        if (Cid.empty()) { LogErr("main.cpp", "publish-meta failed: " + Err); return 1; }
        LogSucc("main.cpp", "Meta-CID: " + Cid + "  (staging: " + Staging + ")");
        std::cout << Cid << "\n";   // machine-readable on stdout
        return 0;
    }

    //HEADLESS: validate the whole node graph (dangling/cyclic PARENTS, layer PATHs, runner resolution, ...).
    if (LaunchParameters.ValidateNodes)
    {
        NodeIndex Index = PackageCatalog::BuildCatalogIndex(GlobalConfigJSON);   // repos + locally-added packages
        std::vector<std::string> Errors, Warnings;
        ManifestModel::ValidateNodeGraph(Index, Errors, Warnings);
        for (const auto &W : Warnings) LogWarn("validate-nodes", W);
        for (const auto &E : Errors)   LogErr ("validate-nodes", E);
        LogOut("validate-nodes", "Validated " + std::to_string(Index.Nodes.size()) + " node(s): "
               + std::to_string(Errors.size()) + " error(s), " + std::to_string(Warnings.size()) + " warning(s).");
        return Errors.empty() ? 0 : 1;
    }

    //HEADLESS: reduce a version chain to one base full zip + per-version .vgdelta layers (byte-verified).
    //Each node after the first has its VFSZipLayer replaced by a VFSDeltaLayer diffed against the version
    //before it (overlay-relative chaining) and is reparented onto it; the now-redundant full zips are deleted.
    if (!LaunchParameters.ConvertDeltaChain.empty())
    {
        namespace fs = std::filesystem;
        const auto &Chain = LaunchParameters.ConvertDeltaChain;
        if (Chain.size() < 2) { LogErr("convert-delta", "need >=2 nodes: <baseNode> <newer> [<newer>...]"); return 1; }
        NodeIndex Index = PackageCatalog::BuildCatalogIndex(GlobalConfigJSON);

        // A ByteSource over an mmap (for byte-verification of the reconstructed view).
        struct MapSrc : ByteSource {
            int fd = -1; const uint8_t *p = nullptr; uint64_t n = 0;
            bool Map(const std::string &path) { fd = ::open(path.c_str(), O_RDONLY); if (fd < 0) return false;
                struct stat s{}; if (fstat(fd, &s)) return false; n = (uint64_t)s.st_size;
                p = (const uint8_t *)::mmap(nullptr, n, PROT_READ, MAP_PRIVATE, fd, 0); return p != MAP_FAILED; }
            ~MapSrc() override { if (p && p != MAP_FAILED) ::munmap((void *)p, n); if (fd >= 0) ::close(fd); }
            ssize_t pread(void *b, size_t k, uint64_t o) override { if (o >= n) return 0; uint64_t a = n - o; size_t w = k < a ? k : a; std::memcpy(b, p + o, w); return (ssize_t)w; }
            uint64_t size() const override { return n; }
        };

        struct V { const Node *node = nullptr; int layerIdx = -1; std::string zipPath, target; };
        std::vector<V> vs;
        for (const auto &id : Chain)
        {
            const Node *n = Index.Find(id);
            if (!n) { LogErr("convert-delta", "node not found: " + id); return 1; }
            V v; v.node = n;
            for (int k = 0; k < (int)n->Layers.size(); ++k)
                if (n->Layers[k].value("TYPE", std::string()) == "VFSZipLayer") { v.layerIdx = k; break; }
            if (v.layerIdx < 0) { LogErr("convert-delta", "no VFSZipLayer in " + id); return 1; }
            fs::path local; std::string cid;
            ManifestModel::LayerLocator(n->Layers[v.layerIdx], n->BundleDir, local, cid);
            v.zipPath = local.string();
            v.target  = n->Layers[v.layerIdx].value("TARGET", std::string());
            if (!fs::exists(v.zipPath)) { LogErr("convert-delta", "missing zip for " + id + ": " + v.zipPath); return 1; }
            vs.push_back(std::move(v));
        }

        uint64_t before = 0; for (auto &v : vs) before += fs::file_size(v.zipPath);
        uint64_t after  = fs::file_size(vs[0].zipPath);   // the base full zip stays

        // Pass 1: generate + byte-verify every delta against the ORIGINAL prior zip (all still present), write
        // the .vgdelta and rewrite the node JSON. Deletion of the redundant full zips is deferred to pass 2.
        std::vector<std::string> toDelete;
        for (size_t i = 1; i < vs.size(); ++i)
        {
            auto base = std::make_shared<MapSrc>();
            auto tgt  = std::make_shared<MapSrc>();
            if (!base->Map(vs[i - 1].zipPath) || !tgt->Map(vs[i].zipPath)) { LogErr("convert-delta", "mmap failed"); return 1; }
            LogOut("convert-delta", "diff " + vs[i].node->NodeId + " against " + vs[i - 1].node->NodeId + " ...");
            std::vector<uint8_t> delta = vgdelta::GenerateDelta(base->p, base->n, tgt->p, tgt->n);

            // byte-verify: reconstruct the target view (delta over the same base) and compare to the real zip.
            std::string derr;
            auto ds = vgdelta::DeltaByteSource::Create(std::make_shared<MemByteSource>(delta), base, derr, false);
            if (!ds || ds->size() != tgt->n) { LogErr("convert-delta", "verify: create/size (" + derr + ")"); return 1; }
            {
                const size_t CH = size_t(8) << 20; std::vector<uint8_t> buf(CH); uint64_t off = 0;
                while (off < tgt->n) {
                    size_t want = (size_t)std::min<uint64_t>(CH, tgt->n - off), got = 0;
                    while (got < want) { ssize_t r = ds->pread(buf.data() + got, want - got, off + got); if (r <= 0) { LogErr("convert-delta", "verify read"); return 1; } got += (size_t)r; }
                    if (std::memcmp(buf.data(), tgt->p + off, want) != 0) { LogErr("convert-delta", "BYTE MISMATCH — aborting, nothing changed"); return 1; }
                    off += want;
                }
            }

            // write the delta blob + rewrite the node JSON (reparent + swap the zip layer for a delta layer).
            std::string blob = "delta_from__" + vs[i - 1].node->NodeId + ".vgdelta";
            { std::ofstream o(vs[i].node->BundleDir / blob, std::ios::binary); o.write((const char *)delta.data(), (std::streamsize)delta.size()); }
            after += delta.size();

            nlohmann::ordered_json J; { std::ifstream in(vs[i].node->File); in >> J; }
            nlohmann::ordered_json parents = J.value("PARENTS", nlohmann::ordered_json::array());
            if (!parents.is_array()) parents = nlohmann::ordered_json::array();
            bool have = false; for (auto &p : parents) if (p == vs[i - 1].node->NodeId) have = true;
            if (!have) parents.push_back(vs[i - 1].node->NodeId);
            J["PARENTS"] = parents;
            J["LAYERS"][vs[i].layerIdx] = nlohmann::ordered_json{ {"TYPE", "VFSDeltaLayer"}, {"PATH", blob}, {"TARGET", vs[i].target} };
            { std::ofstream o(vs[i].node->File); o << J.dump(4) << "\n"; }

            toDelete.push_back(vs[i].zipPath);
            LogSucc("convert-delta", "  " + vs[i].node->NodeId + " ✓ verified  (" + std::to_string(delta.size() >> 20) + " MB delta)");
        }

        // The old full zips are now redundant (byte-verified recoverable from base + deltas), but deletion is
        // left to the caller so the conversion stays reversible until they choose to reclaim the space.
        uint64_t reclaim = 0;
        for (const auto &z : toDelete) { std::error_code ec; reclaim += fs::file_size(z, ec); }
        LogSucc("convert-delta", "chain reduced (verified): base+deltas = " + std::to_string(after >> 20)
                + " MB vs " + std::to_string(before >> 20) + " MB of full zips ("
                + std::to_string(before ? 100 - after * 100 / before : 0) + "% smaller). Reclaim "
                + std::to_string(reclaim >> 20) + " MB by deleting the now-unreferenced full zips:");
        for (const auto &z : toDelete) LogOut("convert-delta", "  rm  " + z);
        return 0;
    }

    //HEADLESS: print the presentable library tiles (grouped launchable nodes) + hydration status.
    if (LaunchParameters.ListNodes)
    {
        NodeIndex Index = PackageCatalog::BuildCatalogIndex(GlobalConfigJSON);
        auto Groups = PackageCatalog::PresentableGroups(Index);
        LogOut("list-nodes", std::to_string(Groups.size()) + " library tile(s):");
        for (const auto &G : Groups)
        {
            const Node *Rep = G.front();
            std::string Variants;
            for (const Node *N : G)
                Variants += " " + N->NodeId + (PackageCatalog::NodeHydrated(Index, N->NodeId) ? "[hydrated]" : "[remote]");
            LogOut("list-nodes", "  " + (Rep->Meta.is_object() ? Rep->Meta.value("TITLE", Rep->NodeId) : Rep->NodeId)
                   + "  (game " + Rep->GameKey() + "):" + Variants);
        }
        return 0;
    }

    //HEADLESS (node graph): launch a launchable node from the global, cross-bundle node index — the engine
    //resolves EVERYTHING natively from the node graph (LaunchResolver::InitializeFromNode). No manifest.
    if (!LaunchParameters.LaunchNodeId.empty())
    {
        LogOut("main.cpp", "Launching node '" + LaunchParameters.LaunchNodeId + "' from the global node graph.");
        NodeIndex Index = PackageCatalog::BuildCatalogIndex(GlobalConfigJSON);   // repos + locally-added packages
        if (!Index.Find(LaunchParameters.LaunchNodeId))
        { LogErr("main.cpp", "Node '" + LaunchParameters.LaunchNodeId + "' not found in the catalog, aborting."); return 1; }

        nlohmann::ordered_json MANIFESTJSON = nlohmann::ordered_json::object();   // engine fills this from the node graph
        struct ContainerParams NewContainerParams = ContainerParams(std::filesystem::path(), LaunchParameters.LaunchNodeId, std::string());
        NewContainerParams.NodeIdx           = &Index;
        NewContainerParams.LaunchNodeId      = LaunchParameters.LaunchNodeId;
        NewContainerParams.VariableOverrides = LaunchParameters.VariableOverrides;
        NewContainerParams.ModuleStates      = LaunchParameters.ModuleStates;
        NewContainerParams.VariantID         = "default";
        NewContainerParams.RunnerID          = LaunchParameters.RunnerID;
        //Runner daisy-chain: the explicit --runner chain wins; else the single RunnerID as a 1-step pin (else auto).
        if (!LaunchParameters.RunnerChain.empty())   NewContainerParams.RunnerChainIds = LaunchParameters.RunnerChain;
        else if (!LaunchParameters.RunnerID.empty()) NewContainerParams.RunnerChainIds = { LaunchParameters.RunnerID };
        class ContainerWrapper NewContainerWrapper = ContainerWrapper(GlobalConfigJSON, MANIFESTJSON, NewContainerParams);
        if (!LaunchResolver::ResolveExecutableDefinition(MANIFESTJSON, NewContainerWrapper.ContainerParams))
        { LogErr("main.cpp", "ResolveExecutableDefinition failed, aborting."); return 1; }
        if (LaunchParameters.ResolveOnly)
        {
            //Dump the resolved params (no mount, no game) for golden-compare / hang-free verification.
            //Under the app data dir so ALL VidyaGod-produced data lives in ~/.VidyaGod (a single delete = clean slate).
            const std::filesystem::path Out = std::filesystem::path(AppDataDir.absolutePath().toStdString())
                                              / ("vg_resolve_" + LaunchParameters.LaunchNodeId + ".json");
            std::ofstream OF(Out);
            OF << DumpResolution(NewContainerWrapper.ContainerParams).dump(2) << std::endl;
            LogSucc("main.cpp", "Resolved '" + LaunchParameters.LaunchNodeId + "' -> " + Out.string());
            return 0;
        }
        //Bail if the runtime couldn't be built (no compatible runner, unmountable/compressed layers, missing
        //dependencies, …). Without this the engine would proceed to Execute() an empty/garbage command and report
        //a misleading clean exit (code 0) for a launch that never actually ran.
        if (!NewContainerWrapper.BuildContainerRuntime())
        { LogErr("main.cpp", "Failed to build the container runtime for '" + LaunchParameters.LaunchNodeId
                 + "' — aborting launch (check the log above)."); NewContainerWrapper.Cleanup(); return 1; }
        NewContainerWrapper.Execute();
        if (NewContainerWrapper.LastCrashed || NewContainerWrapper.LastExitCode != 0)
            LogWarn("main.cpp", "Game did not exit cleanly (code " + std::to_string(NewContainerWrapper.LastExitCode) + ").");
        return NewContainerWrapper.LastCrashed ? 1 : NewContainerWrapper.LastExitCode;
    }

    //Initializing GUI. MAKE SURE THERE ARE NOT QT GUI-RELATED CALLS ABOVE THIS LINE OR THE PROGRAM WILL CRASH.
    //QApplication must be constructed before any QWidget or QPixmap usage;
    //headless code paths above deliberately avoid creating any Qt GUI objects.
    //---------------------------------------------------------------------------------------------------------
    QApplication Application(argc, argv);
    //Force a consistent Qt style so the UI looks the same regardless of the system theme.
    Application.setStyle(QStyleFactory::create("Fusion"));

    //Set a default font to ensure consistent spacing across desktop environments.
    Application.setFont(QFont("DejaVu Sans", 10));

    //Block accidental hover-scroll from mutating combo boxes app-wide (see WheelGuard).
    Application.installEventFilter(new WheelGuard(&Application));

    //Surface any missing dependencies now that we can show a dialog — with concrete install steps.
    //Non-blocking to the app: the user dismisses it and the library still opens (launching games fails
    //until the deps are present).
    if (!MissingDeps.empty())
    {
        QString Msg = "VidyaGod needs the following to launch games, but couldn't find them:\n\n";
        for (const std::string &Dep : MissingDeps) Msg += "•  " + QString::fromStdString(Dep) + "\n";
        Msg += "\nOpen Settings → Dependencies for the full system check (Vulkan, 32-bit libraries, audio, …) and the "
               "exact package to install for each. You can still browse your library meanwhile.";
        QMessageBox::warning(nullptr, "Missing dependencies", Msg);
    }

    //Create and launch MainWindow. Passes GlobalConfigJSON and AppDataDir by pointer so
    //the window can persist changes (add/remove packages, save settings) to disk.

    //IN-PACKAGE MODE: the working dir is a self-contained package bundle → run JUST its PreLaunchWindow (no library
    //tabs, no IPFS node). Scan the bundle for its launchable nodes and open the prelaunch dialog on the first
    //presentable group. (Multi-game packages — a bundle with several groups — get a picker later; for now the first
    //group, which covers the common single-game-with-configs case.) The dialog's NodeIndex is this local, kept alive
    //by Application.exec().
    if (LaunchParameters.RunningInPackageDir)
    {
        NodeIndex PkgIndex;
        ManifestModel::ScanBundleNodes(LaunchParameters.HeadlessPackagePath.string(), PkgIndex);
        auto Groups = PackageCatalog::PresentableGroups(PkgIndex);
        if (Groups.empty())
        {
            QMessageBox::critical(nullptr, "VidyaGod",
                "This folder has no launchable game in it (no presentable node group).");
            return 1;
        }
        //Multi-game bundle → a picker grid (clicking a game opens its PreLaunchWindow). Single game → straight to it.
        if (Groups.size() > 1)
        {
            GamePicker Picker(&GlobalConfigJSON, &PkgIndex);
            Picker.show();
            return Application.exec();
        }
        std::vector<std::string> GroupNodeIds;
        for (const Node *N : Groups.front()) GroupNodeIds.push_back(N->NodeId);
        PreLaunchWindow Dialog(&GlobalConfigJSON, &PkgIndex, GroupNodeIds);
        Dialog.show();
        return Application.exec();
    }

    MainWindow MainWindow(&GlobalConfigJSON, &AppDataDir);
    MainWindow.startup(LaunchParameters.StartInTray);   // show(), or come up hidden if --tray / Start-in-tray / remembered
    return Application.exec();
}


// The default package source: an IPFS folder CID of the dehydrated built-in runner packages (ge-proton / wine /
// umu-proton / snes9x / native-passthrough). Seeded into Settings.PackageSources so a fresh install has runners;
// fetched by PackageCatalog::SyncPackageSources once the IPFS node is online, then hydrated on install like any
// package. Immutable — bumping the runner set = a new CID here (and an app release). (Git repos were removed.)
static std::string DefaultRunnerSourceCID() { return "QmdMCw8q4g2AUcZ6h68GUb2UYXqT8PSi3CPpg6VzxTpvGH"; }   // JSON-only runners Meta-CID
// Same contract for the built-in LIBRARIES source: shared dependency nodes (DirectPlay, future codecs/redists) that
// games reference via PARENTS. It MUST always be present or a game's library-parent would dangle on a fresh install —
// exactly like the runners source. Immutable — bumping the library set = a new CID here (and an app release).
static std::string DefaultLibrarySourceCID() { return "QmexKye5YQj6isiriviSGh15s5rn7RNa224f58uScs3ZCx"; }   // JSON-only libraries Meta-CID

//Guarantees the GlobalConfig has the shape the app actually uses, seeding any missing piece:
//  LIBRARY (array of packages), Settings (object), Settings.PackageSources (the CID package sources).
//Runners (and every other shared package) come from the configured PackageSources.
//Returns true if it added anything, so the caller can persist a freshly-seeded config.
static bool EnsureGlobalConfigDefaults(nlohmann::ordered_json & gc)
{
    bool Changed = false;
    if (!gc.is_object())                                        { gc = nlohmann::ordered_json::object();             Changed = true; }
    if (!gc.contains("LIBRARY")  || !gc["LIBRARY"].is_array())  { gc["LIBRARY"]  = nlohmann::ordered_json::array();  Changed = true; }
    if (!gc.contains("Settings") || !gc["Settings"].is_object()){ gc["Settings"] = nlohmann::ordered_json::object(); Changed = true; }
    //Migration: git repositories were removed — drop any legacy Settings.Repositories (its LIBRARY/<repo> clones, if
    //any, become inert local dirs). Sharing is now solely via CID package sources.
    if (gc["Settings"].contains("Repositories")) { gc["Settings"].erase("Repositories"); Changed = true; }
    //PackageSources: ordered list of IPFS folder CIDs of dehydrated packages, fetched into <DataRoot>/LIBRARY/<name>
    //and indexed.
    if (!gc["Settings"].contains("PackageSources") || !gc["Settings"]["PackageSources"].is_array())
    {
        gc["Settings"]["PackageSources"] = nlohmann::ordered_json::array();
        Changed = true;
    }
    //ALWAYS guarantee the built-in runners source is present and points at the CURRENT hardcoded CID. Keyed by its
    //reserved NAME "VidyaGodRunners" (not the CID), so: a fresh config gets it, a user who never had it (or removed it)
    //gets it back, and a release that bumps DefaultRunnerSourceCID() re-points the existing entry — otherwise the
    //runners default only ever seeded on a config with NO PackageSources at all (e.g. one carrying only a library
    //source would silently lack the runners source, and its CID would never surface in Sources / the IPFS tab).
    {
        auto & Sources = gc["Settings"]["PackageSources"];
        auto It = std::find_if(Sources.begin(), Sources.end(), [](const nlohmann::ordered_json & S) {
            return S.is_object() && S.value("NAME", std::string()) == "VidyaGodRunners"; });
        if (It == Sources.end())
        {
            Sources.push_back(nlohmann::ordered_json{ {"NAME", "VidyaGodRunners"}, {"CID", DefaultRunnerSourceCID()} });
            Changed = true;
        }
        else if ((*It).value("CID", std::string()) != DefaultRunnerSourceCID())
        {
            (*It)["CID"] = DefaultRunnerSourceCID();
            Changed = true;
        }
    }
    //Same guarantee for the built-in LIBRARIES source (shared dependency nodes like DirectPlay that games PARENT).
    {
        auto & Sources = gc["Settings"]["PackageSources"];
        auto It = std::find_if(Sources.begin(), Sources.end(), [](const nlohmann::ordered_json & S) {
            return S.is_object() && S.value("NAME", std::string()) == "VidyaGodLibraries"; });
        if (It == Sources.end())
        {
            Sources.push_back(nlohmann::ordered_json{ {"NAME", "VidyaGodLibraries"}, {"CID", DefaultLibrarySourceCID()} });
            Changed = true;
        }
        else if ((*It).value("CID", std::string()) != DefaultLibrarySourceCID())
        {
            (*It)["CID"] = DefaultLibrarySourceCID();
            Changed = true;
        }
    }
    return Changed;
}

//Loads GlobalConfig.JSON from AppDataDir (or starts empty if it does not exist), then ensures the
//config has the shape the app uses via EnsureGlobalConfigDefaults. The file is (re)written when it
//was freshly created or when a missing top-level key had to be seeded.
//Returns true on FAILURE, false on SUCCESS — matches shell exit-code convention so
//callers can write `if (InitializeGlobalConfigJSON(...)) { /* handle error */ }`.
bool InitializeGlobalConfigJSON(nlohmann::ordered_json * GlobalConfigJSON, QDir * AppDataDir)
{
    QFile GlobalConfigFile(AppDataDir->filePath("GlobalConfig.JSON"));
    const bool Existed = GlobalConfigFile.exists();

    if (Existed && JSONOps::LoadJSON(&GlobalConfigFile, GlobalConfigJSON))
    {
        //LoadJSON returns non-zero on failure — don't silently overwrite a corrupt config.
        LogErr("main.cpp", "Failed to parse GlobalConfig.JSON, aborting.");
        return true; //fail
    }
    if (!Existed) LogOut("main.cpp", "Config file not detected. Creating defaults...");

    EnsureGlobalConfigDefaults(*GlobalConfigJSON);

    //Index the configured CID package sources: any already-fetched source dir (under LIBRARY) is scanned into the
    //un-hydrated LIBRARY index here. The FETCH of a not-yet-present source (e.g. the default runners on a first run)
    //needs the IPFS node online, so it happens later once networking is up (MainWindow::onNodeReady → syncSources, or
    //a headless mode that started the node). Mutates the config → always persist afterwards.
    PackageCatalog::SyncPackageSources(*GlobalConfigJSON);

    if (!JSONOps::SaveJSON(GlobalConfigJSON, &GlobalConfigFile))
    {
        LogErr("main.cpp", "GlobalConfig.JSON could not be written.");
        return true; //fail
    }

    LogSucc("main.cpp", "GlobalConfigJSON initialized successfully.");
    return false; //success
}

//Delegates to FSOps::CheckPackageValid to test whether CurrentPath contains MANIFEST.json.
bool IsRunningInPackageDir(std::filesystem::path CurrentPath)
{
    if (FSOps::CheckPackageValid(new QDir(CurrentPath))) //Convert FSOPS to stdlib! Remove unnecessary heap variable!
    {
        LogOut("main.cpp", "Running in PACKAGEDIR — opening its launcher (self-contained, no library/IPFS).");
        return true;
    }
    else
    {
        LogOut("main.cpp", "Running outside PACKAGEDIR, launching GUI.");
        return false;
    }
}

//Parses argc/argv sequentially.
//Recognized arguments:
//  --package <path>      : path to the package to run headlessly
//  --game <id>           : GAMEID string from the package's manifest
//  --component <id>      : COMPONENTID override (bypasses the game's default component)
//  --var KEY=VALUE       : override a CustomVar variable; may be repeated for multiple vars
//Unknown arguments are silently ignored.
LaunchParameters ParseCommandLineArguments(int argc, char* argv[])
{
    LaunchParameters RuntimeParameters;
    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];

        if (arg == "--package" && i + 1 < argc)
        {
            RuntimeParameters.HeadlessPackagePath = argv[++i];
            RuntimeParameters.HasHeadlessPackagePath = true;
            RuntimeParameters.RunningHeadless = true;
        }
        else if (arg == "--game" && i + 1 < argc)
        {
            RuntimeParameters.HeadlessGameID = argv[++i];
        }
        else if (arg == "--component" && i + 1 < argc)
        {
            RuntimeParameters.HeadlessComponentID = argv[++i];
        }
        else if (arg == "--node" && i + 1 < argc)
        {
            //Launch a launchable node from the global node graph (everything-is-a-node schema).
            RuntimeParameters.LaunchNodeId   = argv[++i];
            RuntimeParameters.RunningHeadless = true;
        }
        else if (arg == "--resolve-only" && i + 1 < argc)
        {
            //Resolve the node graph + dump ContainerParams to a file, then exit (no mount/launch).
            RuntimeParameters.LaunchNodeId   = argv[++i];
            RuntimeParameters.ResolveOnly    = true;
            RuntimeParameters.RunningHeadless = true;
        }
        else if (arg == "--validate-nodes")
        {
            RuntimeParameters.ValidateNodes   = true;
            RuntimeParameters.RunningHeadless = true;
        }
        else if (arg == "--convert-delta-chain")
        {
            //Everything after this flag (until the next --option) is a version chain: base node first, then
            //each newer version to reduce to a .vgdelta over the one before it.
            while (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0)
                RuntimeParameters.ConvertDeltaChain.push_back(argv[++i]);
            RuntimeParameters.RunningHeadless = true;
        }
        else if (arg == "--seed" && i + 1 < argc)
        {
            //Seed a folder's published content (LAYER/COVER SOURCE CIDs) into the IPFS node by reference, then exit.
            RuntimeParameters.SeedDir         = argv[++i];
            RuntimeParameters.RunningHeadless = true;
        }
        else if (arg == "--seed-covers" && i + 1 < argc)
        {
            //Re-pin ONLY the cover art (META.COVER refs) by reference — skips re-hashing the large game layers.
            RuntimeParameters.SeedDir         = argv[++i];
            RuntimeParameters.SeedCoversOnly  = true;
            RuntimeParameters.RunningHeadless = true;
        }
        else if (arg == "--overwrite")
        {
            //Modifier for --seed/--seed-covers: re-reference every file (default is additive — only new/orphaned).
            RuntimeParameters.SeedOverwrite = true;
        }
        else if (arg == "--fetch" && i + 2 < argc)
        {
            //Download-throughput probe: fetch a CID to a destination path then exit.
            RuntimeParameters.FetchCid         = argv[++i];
            RuntimeParameters.FetchDest        = argv[++i];
            RuntimeParameters.RunningHeadless  = true;
        }
        else if (arg == "--fetch-dir" && i + 2 < argc)
        {
            //Recursively materialize a FOLDER CID to a destination dir then exit (verifies add-by-CID node files).
            RuntimeParameters.FetchCid         = argv[++i];
            RuntimeParameters.FetchDest        = argv[++i];
            RuntimeParameters.FetchDirMode     = true;
            RuntimeParameters.RunningHeadless  = true;
        }
        else if (arg == "--peer-id")
        {
            RuntimeParameters.PrintPeerId      = true;
            RuntimeParameters.RunningHeadless  = true;
        }
        else if (arg == "--pin-ls")
        {
            RuntimeParameters.PrintPinLs       = true;
            RuntimeParameters.RunningHeadless  = true;
        }
        else if (arg == "--unpin" && i + 1 < argc)
        {
            RuntimeParameters.UnpinCid         = argv[++i];
            RuntimeParameters.RunningHeadless  = true;
        }
        else if (arg == "--drop-ref" && i + 1 < argc)
        {
            RuntimeParameters.DropRefCid       = argv[++i];
            RuntimeParameters.RunningHeadless  = true;
        }
        else if (arg == "--friend-code")
        {
            RuntimeParameters.PrintFriendCode  = true;
            RuntimeParameters.RunningHeadless  = true;
        }
        else if (arg == "--friend-ls")
        {
            RuntimeParameters.FriendListOnly   = true;
            RuntimeParameters.RunningHeadless  = true;
        }
        else if (arg == "--friend-add" && i + 1 < argc)
        {
            RuntimeParameters.FriendAddCode    = argv[++i];
            RuntimeParameters.RunningHeadless  = true;
        }
        else if (arg == "--friend-serve")
        {
            RuntimeParameters.FriendServe      = true;
            RuntimeParameters.RunningHeadless  = true;
        }
        else if (arg == "--friend-nick" && i + 1 < argc)
        {
            RuntimeParameters.FriendNick       = argv[++i];
        }
        else if (arg == "--friend-secs" && i + 1 < argc)
        {
            RuntimeParameters.FriendSecs       = std::max(1, std::atoi(argv[++i]));
        }
        else if (arg == "--lan")
        {
            RuntimeParameters.LanHarness       = true;
            RuntimeParameters.RunningHeadless  = true;
        }
        else if (arg == "--overlay")
        {
            RuntimeParameters.OverlayUp        = true;
        }
        else if (arg == "--tray")            // come up hidden in the tray (the start-on-login autostart entry uses this)
        {
            RuntimeParameters.StartInTray = true;
        }
        else if (arg == "--data-dir" && i + 1 < argc)
        {
            RuntimeParameters.DataDir          = argv[++i];
        }
        else if (arg == "--package-dir" && i + 1 < argc)
        {
            RuntimeParameters.PackageDirOverride = argv[++i];
        }
        else if (arg == "--runtime-dir" && i + 1 < argc)
        {
            RuntimeParameters.RuntimeDirOverride = argv[++i];
        }
        else if (arg == "--userdata-dir" && i + 1 < argc)
        {
            RuntimeParameters.UserDataDirOverride = argv[++i];
        }
        else if (arg == "--connect" && i + 1 < argc)
        {
            //Dial a known peer (full /p2p/ multiaddr) before fetching — direct peering / controlled benchmark.
            RuntimeParameters.ConnectAddr      = argv[++i];
            RuntimeParameters.RunningHeadless  = true;
        }
        else if (arg == "--list-nodes")
        {
            RuntimeParameters.ListNodes       = true;
            RuntimeParameters.RunningHeadless = true;
        }
        else if (arg == "--var" && i + 1 < argc)
        {
            //Expects KEY=VALUE format; silently skips malformed entries without '='.
            std::string kv = argv[++i];
            auto eq = kv.find('=');
            if (eq != std::string::npos)
                RuntimeParameters.VariableOverrides[kv.substr(0, eq)] = kv.substr(eq + 1);
        }
        else if (arg == "--module" && i + 1 < argc)
        {
            //Expects COMPONENT=on|off (also true|false / 1|0); toggles one optional module. Malformed skipped.
            std::string kv = argv[++i];
            auto eq = kv.find('=');
            if (eq != std::string::npos)
            {
                std::string val = kv.substr(eq + 1);
                bool on = (val == "on" || val == "true" || val == "1" || val == "yes");
                RuntimeParameters.ModuleStates[kv.substr(0, eq)] = on;
            }
        }
        else if (arg == "--variant" && i + 1 < argc)
        {
            RuntimeParameters.VariantID = argv[++i];
        }
        else if (arg == "--import-runner" && i + 1 < argc)
        {
            RuntimeParameters.ImportRunnerId = argv[++i];
        }
        else if (arg == "--generate-defprefix" && i + 1 < argc)
        {
            RuntimeParameters.GenerateDefPrefixId = argv[++i];
        }
        else if (arg == "--import-package" && i + 1 < argc)
        {
            RuntimeParameters.ImportPackageUid = argv[++i];
        }
        else if (arg == "--publish" && i + 1 < argc)
        {
            RuntimeParameters.PublishPackageDir = argv[++i];
        }
        else if (arg == "--publish-to" && i + 1 < argc)
        {
            RuntimeParameters.PublishToDir = argv[++i];
        }
        else if (arg == "--publish-cid" && i + 1 < argc)
        {
            RuntimeParameters.PublishCidDir  = argv[++i];
            RuntimeParameters.RunningHeadless = true;
        }
        else if (arg == "--publish-meta" && i + 1 < argc)
        {
            RuntimeParameters.PublishMetaSrc = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') RuntimeParameters.PublishMetaStaging = argv[++i];
            RuntimeParameters.RunningHeadless = true;
        }
        else if (arg == "--runner" && i + 1 < argc)
        {
            //Repeatable: each --runner appends one link to the daisy-chain (innermost→outermost). The first one also
            //sets RunnerID for back-compat (single-runner pin).
            std::string R = argv[++i];
            if (RuntimeParameters.RunnerID.empty()) RuntimeParameters.RunnerID = R;
            RuntimeParameters.RunnerChain.push_back(std::move(R));
        }
    }
    return RuntimeParameters;
}
