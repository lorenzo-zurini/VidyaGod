#ifndef DEPCHECK_H
#define DEPCHECK_H

// DepCheck — the system check behind Settings → Dependencies: what VidyaGod itself needs to launch a game (the mount
// helper, its FUSE/WinFsp layer, the mandatory sandbox) and, on Linux, the libraries whose ABSENCE crashes Proton/Wine
// games (the #1 cause of Linux game crashes). It detects the actual .so files (not packages) because a missing .so —
// not a missing package — is what crashes a game, and .so names are identical across distros (only the providing
// package differs, which we map per-distro only for the "install this" hint). 32-bit (multilib) libs are the big
// offender for 32-bit games, so every library is probed for BOTH 64- and 32-bit. On Windows the list is only
// VidyaGod's own needs (Wine libraries don't apply).
//
// Deliberately Qt-free (std types) so the pure parse/classify logic is unit-testable in vg_tests; the Settings page
// (dependenciespage) is its only consumer — nothing checks at startup.

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace DepCheck
{

enum class Distro   { Arch, Debian, Fedora, OpenSUSE, Windows, Unknown };
enum class Category { Core, Graphics, WineLibs, Audio, Media };
enum class Kind     { Binary, File, Lib, VulkanDriver, Sandbox, WinFsp };   // Sandbox / WinFsp: one host-level probe each
enum class Status   { Ok, Partial, Missing };   // Partial = 64-bit present but 32-bit missing (32-bit games may crash)

// One checked dependency. For a Lib, any of Sonames present satisfies it (distros version the soname differently).
struct DepItem
{
    std::string Id;
    std::string Name;
    std::string Purpose;
    Category    Cat;
    Kind        kind;
    std::vector<std::string> Sonames;          // Lib: e.g. {"libgnutls.so.30"}
    std::string Token;                          // Binary: exec name on $PATH; File: filename beside the app binary (or on $PATH)
    std::map<Distro, std::string> Pkg64;        // package providing the 64-bit lib / the binary (per distro)
    std::map<Distro, std::string> Pkg32;        // package providing the 32-bit lib (per distro)
};

// This platform's table: VidyaGod's own needs, plus (Linux) the Lutris canonical Wine/Proton dependency set.
const std::vector<DepItem>& AllDeps();

// A probed snapshot of the host (built once; injectable for tests).
struct SystemState
{
    std::map<std::string, std::pair<bool, bool>> Libs;   // soname -> {have64, have32}
    std::set<std::string> Binaries;                      // Kind::Binary tokens found on $PATH
    std::set<std::string> Files;                          // Kind::File tokens found beside the app binary
    bool   VulkanIcd = false;                             // any *.json in /usr/share/vulkan/icd.d
    bool   SandboxUsable = false;                         // the sandbox really starts here (SandboxLayer::Available)
    bool   WinFsp    = false;                             // Windows: WinFsp's DLL is installed
    Distro distro    = Distro::Unknown;
};

// PURE: parse `ldconfig -p` output into soname -> {have64, have32} (64-bit tag contains "x86-64"; 32-bit is "x86-32"
// or the bare "(libc6)").
std::map<std::string, std::pair<bool, bool>> ParseLdconfig(const std::string& LdconfigOutput);

// PURE: classify one item against a probed state.
Status Check(const DepItem& Item, const SystemState& St);

// PURE: the names of the Core items (what a launch needs) that are not Ok — "" when this machine can run games.
std::vector<std::string> MissingCore(const SystemState& St);

// Probe the real host: `ldconfig -p`, $PATH scan for the binary tokens, bundled files beside the app binary, the
// Vulkan ICD dir, WinFsp's DLL, and /etc/os-release. The sandbox probe lives in SandboxLayer (Qt side, the same probe a
// launch uses), so the caller passes its result in.
SystemState ProbeSystem(bool SandboxUsable);

Distro      DistroFromOsRelease();              // reads /etc/os-release ID (Windows: Distro::Windows)
const char* DistroLabel(Distro D);
const char* CategoryLabel(Category C);

// The package to install for a not-Ok item on distro D: the 32-bit package when Partial, else the 64-bit/base package.
// "" when unknown for that distro.
std::string InstallHint(const DepItem& Item, Status St, Distro D);
// One command that installs every package of Pkgs with D's package manager ("sudo pacman -S --needed a b"); "" when
// D has none VidyaGod knows (Windows, an unknown distro) or Pkgs is empty. Repeats are dropped.
std::string InstallCommand(const std::vector<std::string>& Pkgs, Distro D);

} // namespace DepCheck

#endif // DEPCHECK_H
