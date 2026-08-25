#include "cli/climodes.h"
#include "main.h"
#include "apppaths.h"
#include "platform/platform.h"
#include "commonutils.h"
#include "manifestmodel.h"
#include "packagecatalog.h"
#include "containerwrapper.h"
#include "ipfswrapper.h"
#include "jsonoperations.h"
#include "sandboxlayer.h"

#include <QDir>
#include <QFile>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;

//Headless "play together" capstone: bring the overlay up inside a NESTED sandbox (the real launch path, not the
//root-requiring host TUN) and ping the peer's vIP THROUGH the tunnel. Spawns bwrap running the sandbox-init, which
//creates + addresses the TUN inside the sandbox netns and hands its fd to our libp2p forwarder; the sandboxed shell
//then pings the peer. Returns the ping's exit code (0 = replies received = packets crossed the tunnel). This is what
//a real session game launch does, with `ping` standing in for the game. Rootless (CAP_NET_ADMIN via the userns).
#ifdef __linux__
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

#include "sandboxlayer.h"
#include <unistd.h>
#include <sys/wait.h>

#else  // !__linux__ — the overlay ping test is bwrap/TUN-based (Linux-only); inert stub elsewhere.
static int RunOverlaySandboxPing()
{
    LogErr("main.cpp", "--overlay ping test is only available on Linux (bwrap sandbox + TUN).");
    return -1;
}
#endif // __linux__

int CliModes::RunIpfsModes(LaunchParameters &LaunchParameters, nlohmann::ordered_json &GlobalConfigJSON, QDir &AppDataDir)
{
    (void)GlobalConfigJSON; (void)AppDataDir;
    if (LaunchParameters.PrintPeerId)
    {
        for (int i = 0; i < 30 && IpfsWrapper::Available() && !IpfsWrapper::DaemonRunning(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        LogOut("main.cpp", "PeerID: " + IpfsWrapper::PeerID());
        for (const std::string &A : IpfsWrapper::ListenAddrs()) LogOut("main.cpp", "addr: " + A);
        return 0;
    }

    //HEADLESS: list the recursively-pinned (seeded) CIDs in this repo, then exit (debug: what the IPFS tab would show).
    if (LaunchParameters.PrintPinLs)
    {
        for (int i = 0; i < 30 && IpfsWrapper::Available() && !IpfsWrapper::DaemonRunning(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        const std::vector<IpfsWrapper::PinEntry> Pins = IpfsWrapper::Pins();
        LogOut("main.cpp", "Pins: " + std::to_string(Pins.size()));
        for (const auto &P : Pins) LogOut("main.cpp", "pin: " + P.Cid);
        return 0;
    }

    //HEADLESS: drop a recursive pin (stop seeding + surfacing a superseded CID), then exit.
    if (!LaunchParameters.UnpinCid.empty())
    {
        for (int i = 0; i < 30 && IpfsWrapper::Available() && !IpfsWrapper::DaemonRunning(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        const bool Ok = IpfsWrapper::Unpin(LaunchParameters.UnpinCid);
        LogOut("main.cpp", (Ok ? "Unpinned " : "Unpin failed / not pinned: ") + LaunchParameters.UnpinCid);
        return Ok ? 0 : 1;
    }

    //HEADLESS: delete a CID's filestore-reference closure (+ unpin), so a subsequent add re-references its content at
    //its CURRENT path — the repair for an orphaned/stale reference the plain re-add would otherwise dedup-skip. Then exit.
    if (!LaunchParameters.DropRefCid.empty())
    {
        for (int i = 0; i < 30 && IpfsWrapper::Available() && !IpfsWrapper::DaemonRunning(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        const bool Ok = IpfsWrapper::DropRef(LaunchParameters.DropRefCid);
        LogOut("main.cpp", (Ok ? "Dropped reference closure for " : "Drop-ref failed: ") + LaunchParameters.DropRefCid);
        return Ok ? 0 : 1;
    }

    //HEADLESS: print this node's shareable friend code (its peer ID), then exit.
    if (LaunchParameters.PrintFriendCode)
    {
        for (int i = 0; i < 30 && IpfsWrapper::Available() && !IpfsWrapper::DaemonRunning(); ++i)
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
    return -1;   // no mode of this family requested
}
