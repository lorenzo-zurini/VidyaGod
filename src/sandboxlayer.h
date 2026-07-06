#ifndef SANDBOXLAYER_H
#define SANDBOXLAYER_H

#include <string>
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

struct Options {
    bool        Enabled = false;
    NetMode     Net = NetMode::Host;
    std::string RuntimeRoot;   // the mounted game runtime (vidyagodfs root) or package dir — kept writable
    std::string HomeDir;       // the user's home / save dir — kept writable
};

// True if this launch requested the sandbox (VIDYAGOD_SANDBOX custom variable is truthy: on/true/1/yes).
bool Requested(const ContainerParams &CP);

// Build the sandbox Options from the resolved launch params (runtime root, home, network mode from VIDYAGOD_SANDBOX_NET).
Options FromParams(const ContainerParams &CP);

// True if the bwrap tool is available on the system (checked once).
bool Available();

// Prepend a bwrap invocation to (Program, Arguments): on return Program == "bwrap" and Arguments is the full bwrap
// argv ending in `-- <origProgram> <origArgs...>`. Pure string construction (no side effects) so it is unit-testable
// without bwrap installed. The caller gates this on Available().
void Wrap(const Options &Opts, std::string &Program, QStringList &Arguments);

} // namespace SandboxLayer

#endif // SANDBOXLAYER_H
