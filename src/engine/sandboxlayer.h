#ifndef SANDBOXLAYER_H
#define SANDBOXLAYER_H

#include <functional>
#include <string>
#include <utility>
#include <vector>
#include "platform/platform.h"
#include <QProcessEnvironment>
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

// A dynamic-loader variable (LD_LIBRARY_PATH, LD_PRELOAD) as the GAME's environment has it: set to Value, or removed.
// bwrap's payload is VidyaGod itself (--sandbox-init), so a runner's loader path would also load its bundled libraries
// into VidyaGod's own Qt/GLib before main() runs (a bundled GLib older than the host's: "undefined symbol" and exit
// 127). The init starts with VidyaGod's own values and applies these right before it execs the game.
struct GameEnvVar { std::string Name; bool Set = false; std::string Value; };

// Moves the loader variables (every LD_* name, and GLIBC_TUNABLES) whose value in Run (the game's composed
// environment) differs from Base (the environment the launcher itself runs with) out of Run: Run gets Base's value
// back, and the game's value is returned for the init to apply. Pure, so it is testable without a sandbox.
std::vector<GameEnvVar> TakeLoaderEnv(QProcessEnvironment &Run, const QProcessEnvironment &Base);

struct Options {
    NetMode                  Net = NetMode::Host;
    std::vector<std::string> WritableBinds;   // host paths bound read-WRITE (TempPath, the kept folders) over the RO root
    std::string              Home;            // the launched program's private $HOME (empty = none; see HomeEnv)
    std::string              RealHome;        // the user's real $HOME: read-only in the sandbox, never Home
    std::vector<std::pair<std::string, std::string>> HomeCaches;   // (real path, path in Home): GPU shader caches carried over
    std::vector<Mount>       Mounts;          // vidyagodfs specs re-mounted inside the sandbox (game never sees the host mount);
                                               // Windows: the host WinFsp mounts, which the box is granted (BoxIniCommands)
    std::string              WorkDir;         // the game's CWD (under a mountpoint) — init chdir's here after mounting
    std::string              TunName, TunCidr, TunSock;   // overlay TUN (Isolated + a session); empty = none
    std::string              Cloak;                        // "<caption>|<maxwidth>": X-side unmap of a game's ugly startup window (empty = off)
    std::string              BoxRoot;                      // Windows: the box's own root, the game's prefix (BoxPrefix); "" = Sandboxie's default
    bool                     TunBridge = false;           // in-node NAT is up → sandbox-init adds a default route via the TUN
    std::vector<GameEnvVar>  GameEnv;                      // loader variables the init applies to the game only (TakeLoaderEnv)
};

// Build the base sandbox Options from the launch params: Net (from VIDYAGOD_SANDBOX_NET), the writable binds (the
// session's TempPath and the instance's kept folders, never the instance folder itself), and on Linux the private home. ContainerWrapper fills Mounts/WorkDir/Tun
// before calling Wrap.
//
// PRIVATE HOME (Linux): the user's real $HOME is NOT writable inside the sandbox (it is part of the read-only root).
// The launched program's HOME and XDG_CONFIG/DATA/STATE/CACHE_HOME point at <TempPath>/HOME instead, a folder that is
// fresh every launch and removed with the session tree. So whatever a runner writes to ~/.config, ~/.local/share or
// ~/.cache is discarded unless the runner or package points that variable into the runtime and KEEPs it. The one
// thing carried over from the real home is the GPU driver's shader cache (HomeCaches): it is the driver's, not the
// game's, and losing it would recompile every shader on every launch.
Options FromParams(const ContainerParams &CP);
// The options a launch's sandbox runs with — the one builder Execute, the kill, the box's setup and its teardown
// share: FromParams, the mounts BuildContainerRuntime recorded for the sandbox (SandboxMounts), the working folder
// (the box's name comes from it, so every step reaches the box the game runs in).
Options ForLaunch(const ContainerParams &CP);

// Point the launch environment at Opts.Home: HOME and the XDG base directories, plus XAUTHORITY at the real
// ~/.Xauthority when the session names no other (Xlib would otherwise look for the cookie in the private home and the
// X server would refuse the game). No-op when Opts.Home is empty. Applied to the BASE environment, before any runner
// or package ENV, so a package that sets or removes one of these still wins.
void HomeEnv(const Options &Opts, QProcessEnvironment &Env);

// True if the sandbox can ACTUALLY run on this machine — a real probe, run on every call so installing the missing
// piece needs no restart. Linux: spawn a trivial bwrap with the same namespace/cap flags a launch uses, which catches a
// missing bwrap AND a bwrap that can't create the sandbox (unprivileged user namespaces disabled, AppArmor, no setuid).
// Windows: Sandboxie-Plus installed and its SbieSvc service running. False = games can't launch here.
bool Available();

// Prepend a bwrap invocation to (Program, Arguments): on return Program == "bwrap" and Arguments is the full bwrap
// argv ending in `-- <origProgram> <origArgs...>`. Pure string construction (no side effects) so it is unit-testable
// without bwrap installed. The caller gates this on Available().
void Wrap(const Options &Opts, std::string &Program, QStringList &Arguments);

// The files a program left in its private home (relative paths, sorted), read at teardown before the session tree is
// removed: what it wrote under ~ and nothing kept. Logged so a package author sees which folder to point into the
// runtime and KEEP, instead of a setting or a save that silently never comes back. Symlinks are listed, not followed,
// and a Home that is itself a symlink is not walked (empty result). Stops after Limit entries (folders included, so
// the result can be shorter than Limit with more left unread); control characters in a
// name become '?' (the program chose the names).
std::vector<std::string> HomeLeftovers(const std::string &Home, size_t Limit = 1000);

// The SbieIni.exe argument lists that define a launch's Sandboxie box, in order (Windows: PrepareBox runs them). The box
// gets Opts.BoxRoot as its root (FileRootPath) and is not auto-deleted then (BoxPrefix fills and empties it), and drops
// administrator rights. A boxed program reaches a WinFsp mount only through an OpenFilePath grant naming the mountpoint
// itself: one on a folder inside the mount gets the program killed at its first file access there, and one on a parent
// folder does not reach through the mount's reparse point. Sandboxie also matches backslashed paths only. And it keeps
// what a granted mountpoint resolved to: relaunched seconds later (a new WinFsp volume at the same folder), the game
// read its files but every write failed, its saves included. So each mountpoint is granted together with the volume
// it names now (MountTarget: "\\Device\\Volume{…}"), and nothing else: the kept folders are reached through the mount.
// The grants are backslashed, without a trailing separator, each once, and they REPLACE the box's earlier ones (the box
// is re-used by the game's next launch). Pure but for MountTarget, so it is testable without Sandboxie.
std::vector<QStringList> BoxIniCommands(const std::string &Box, const Options &Opts,
                                        const std::function<std::string(const std::string &)> &MountTarget
                                            = [](const std::string &P) { return Platform::MountPointTarget(P); });

// Start.exe's arguments for running Program in Box: `/box:<box> /silent /wait <program> <args...>`. /wait keeps Start.exe
// alive until the program exits, so the launch tracks the game's lifetime and gets its exit code. The program path is
// backslashed: Start.exe answers a path with a '/' in it with "Could not invoke program … cannot find the file", in a
// dialog that waits for a click even with /silent. The game starts in Start.exe's own working directory: Start.exe has
// no option for it, and an unknown one ("/wrk:") made it exit 1 without starting the game. Pure, so it is testable.
QStringList BoxStartArgs(const std::string &Box, const std::string &Program, const QStringList &Arguments);

// What a launch calls: on Linux, creates the private home (Opts.Home and its XDG folders), takes the game's loader
// variables out of RunEnv (TakeLoaderEnv against BaseEnv) into Opts.GameEnv for the init to apply, then Wrap. On Windows (no re-invoked init) just Wrap.
void WrapGame(Options Opts, std::string &Program, QStringList &Arguments, QProcessEnvironment &RunEnv,
              const QProcessEnvironment &BaseEnv);

// Tear down the sandbox for a launch (its Options, as Wrap got them). On Windows this terminates the Sandboxie box
// (Start.exe /terminate) so no boxed process lingers after a KillGame; on Linux it is a no-op (bwrap dies with the
// game's process group). Safe to call even when the launch was never sandboxed.
void TerminateBox(const Options &Opts);

// Windows: defines the launch's box (BoxIniCommands) — before anything runs in it. No-op elsewhere.
void PrepareBox(const Options &Opts);

// Windows: runs Program with Args in the launch's box, first ending whatever still runs there (a program started while
// the box winds down is killed at once, 0x40010004), and waits up to TimeoutMs. Its exit code, or -1 when it did not
// start or finish (it is then ended). For what a box prefix runs inside the box: reg.exe import / export. -1 elsewhere.
int RunInBox(const Options &Opts, const std::string &Program, const QStringList &Args, int TimeoutMs);

// Windows: empties the launch's box the way Sandboxie does (Start.exe delete_sandbox_silent; end what runs in it first,
// TerminateBox): its registry hive unloaded, its folder removed — measured: the folder is gone when it returns (~1 s). Deleting the
// files ourselves raced the hive, which SbieSvc holds for a moment after the last program ends. False when Sandboxie
// did not finish it. Elsewhere true (no box).
bool EmptyBox(const Options &Opts);

// ---- the in-sandbox runtime (VidyaGod --sandbox-init …), implemented in sandboxinit.cpp ----
// True if this process was invoked as the sandbox init (bwrap's payload).
bool IsSandboxInit(int argc, char **argv);
// Run the sandbox init: mount vidyagodfs layer-specs, bring up the overlay TUN + hand its fd to the parent, then
// execvp the game. Never returns on success (becomes the game); returns an exit code on failure. Call FIRST in main(),
// before any Qt.
int RunSandboxInit(int argc, char **argv);

} // namespace SandboxLayer

#endif // SANDBOXLAYER_H
