#ifndef SANDBOXLAYER_H
#define SANDBOXLAYER_H

#include <string>
#include <vector>
#include <QStringList>

struct ContainerParams;

// ---------------------------------------------------------------------------
// SandboxLayer — wraps the resolved game command in bubblewrap (bwrap) for filesystem isolation and, for multiplayer,
// network-namespace scoping: (a) the host root is mounted read-only so a game can't scribble on the system while its
// runtime + save dir stay writable, and (b) the network namespace can be unshared so the overlay TUN is scoped to just
// the game. On Windows the same role is played by a Sandboxie box (sandboxlayer_win.cpp).
//
// MANDATORY: every launch is sandboxed. A machine that can't sandbox (see Available) fails the launch
// (ContainerWrapper::Execute) rather than running the game unconfined; there is no opt-out.
// ---------------------------------------------------------------------------
namespace SandboxLayer {

// How the sandbox treats the network.
//   Host     — share the host network namespace; the overlay TUN lives on the host (works today, but host-wide).
//   Isolated — --unshare-net; the game gets its own empty netns and the overlay TUN must be injected into it
//              (fully scoped to the game — the end goal — but needs the TUN-in-netns step; see the .cpp note).
enum class NetMode { Host, Isolated };

// A vidyagodfs layer-spec the sandbox-init mounts INSIDE the namespace (spec written on the host in phase B).
struct Mount { std::string SpecPath; std::string Mountpoint; };

struct Options {
    NetMode                  Net = NetMode::Host;
    std::vector<std::string> WritableBinds;   // host paths bound read-WRITE (TempPath, UserData, home) over the RO root
    std::vector<Mount>       Mounts;          // vidyagodfs specs re-mounted inside the sandbox (game never sees the host mount)
    std::string              WorkDir;         // the game's CWD (under a mountpoint) — init chdir's here after mounting
    std::string              TunName, TunCidr, TunSock;   // overlay TUN (Isolated + a session); empty = none
    std::string              Cloak;                        // "<caption>|<maxwidth>": X-side unmap of a game's ugly startup window (empty = off)
    bool                     TunBridge = false;           // in-node NAT is up → sandbox-init adds a default route via the TUN
};

// Build the base sandbox Options (Net, from VIDYAGOD_SANDBOX_NET) from the launch params; ContainerWrapper fills
// Mounts/WritableBinds/WorkDir/Tun before calling Wrap.
Options FromParams(const ContainerParams &CP);

// True if the sandbox can ACTUALLY run on this machine — a real probe, run on every call so installing the missing
// piece needs no restart. Linux: spawn a trivial bwrap with the same namespace/cap flags a launch uses, which catches a
// missing bwrap AND a bwrap that can't create the sandbox (unprivileged user namespaces disabled, AppArmor, no setuid).
// Windows: Sandboxie-Plus installed and its SbieSvc service running. False = games can't launch here.
bool Available();

// Prepend a bwrap invocation to (Program, Arguments): on return Program == "bwrap" and Arguments is the full bwrap
// argv ending in `-- <origProgram> <origArgs...>`. Pure string construction (no side effects) so it is unit-testable
// without bwrap installed. The caller gates this on Available().
void Wrap(const Options &Opts, std::string &Program, QStringList &Arguments);

// Tear down the sandbox for a launch keyed by its WorkDir (the value passed as Options.WorkDir). On
// Windows this terminates the Sandboxie box (Start.exe /terminate) so no boxed process lingers after
// a KillGame; on Linux it is a no-op (bwrap dies with the game's process group). Safe to call even
// when the launch was never sandboxed.
void TerminateBox(const std::string &WorkDir);

// ---- the in-sandbox runtime (VidyaGod --sandbox-init …), implemented in sandboxinit.cpp ----
// True if this process was invoked as the sandbox init (bwrap's payload).
bool IsSandboxInit(int argc, char **argv);
// Run the sandbox init: mount vidyagodfs layer-specs, bring up the overlay TUN + hand its fd to the parent, then
// execvp the game. Never returns on success (becomes the game); returns an exit code on failure. Call FIRST in main(),
// before any Qt.
int RunSandboxInit(int argc, char **argv);

} // namespace SandboxLayer

#endif // SANDBOXLAYER_H
