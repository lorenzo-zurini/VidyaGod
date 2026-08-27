#ifndef SANDBOXLAYER_H
#define SANDBOXLAYER_H

#include <string>
#include <vector>
#include <QStringList>

struct ContainerParams;

// ---------------------------------------------------------------------------
// SandboxLayer — wraps the resolved game command in bubblewrap (bwrap) for filesystem isolation and, for multiplayer,
// network-namespace scoping. This is the launcher's first real sandbox: today the game is spawned as a bare QProcess
// sharing the whole host (full read/write, host network). bwrap lets us (a) mount the host root read-only so native
// Linux games can't scribble on the system while keeping the game's runtime + save dir writable, and (b) optionally
// unshare the network namespace so the multiplayer overlay TUN is scoped to just the game.
//
// OPT-IN by design: enabled only when the launch carries a truthy VIDYAGOD_SANDBOX custom variable, so every existing
// launch is byte-identical unless the sandbox is explicitly requested (the session "play together" flow sets it).
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
    bool                     Enabled = false;
    NetMode                  Net = NetMode::Host;
    std::vector<std::string> WritableBinds;   // host paths bound read-WRITE (TempPath, UserData, home) over the RO root
    std::vector<Mount>       Mounts;          // vidyagodfs specs re-mounted inside the sandbox (game never sees the host mount)
    std::string              WorkDir;         // the game's CWD (under a mountpoint) — init chdir's here after mounting
    std::string              TunName, TunCidr, TunSock;   // overlay TUN (Isolated + a session); empty = none
    bool                     TunBridge = false;           // in-node NAT is up → sandbox-init adds a default route via the TUN
};

// Whether this launch should be sandboxed. An explicit VIDYAGOD_SANDBOX custom variable (on/off/true/1/…) always wins;
// otherwise the launcher default DefaultOn applies (Settings.SandboxByDefault, on by default). So sandboxing is on for
// every launch unless the machine can't run it (see Available) or the user turns it off globally / per-package.
bool Requested(const ContainerParams &CP, bool DefaultOn = true);

// Build the base sandbox Options (Enabled + Net) from the launch params; ContainerWrapper fills Mounts/WritableBinds/
// WorkDir/Tun before calling Wrap. DefaultOn is the launcher-wide default when no explicit override is present.
Options FromParams(const ContainerParams &CP, bool DefaultOn = true);

// True if the sandbox can ACTUALLY run on this machine — a real one-shot probe (spawn a trivial bwrap with the same
// namespace/cap flags we use), cached. Catches a missing bwrap AND the cases where bwrap exists but the sandbox can't
// be created (unprivileged user namespaces disabled, AppArmor restriction, no setuid). Gating "on by default" on this
// makes an un-sandboxable machine degrade to a normal unsandboxed launch instead of failing.
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
