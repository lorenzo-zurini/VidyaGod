#ifndef VFSMOUNT_H
#define VFSMOUNT_H

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include "launchparams.h"   // ContainerParams

// VfsMount — the vidyagodfs FUSE mount subsystem (layer-spec build + mount/unmount + stale-runtime cleanup),
// lifted out of ContainerWrapper. Free functions over the shared ContainerParams.
namespace VfsMount
{
// Proton's UPSTREAM default PROTON_DLL_COPY value (the redist/insist-copy globs; everything else is symlinked).
// GE-Proton's protonfixes overrides this to "*" (copy EVERY builtin) so it can safely edit DLLs in a SHARED
// prefix — pointless for us, where the wine build is a read-only mount with a copy-up overlay. We restore this
// value at launch (via an injected user_settings.py) so builtins symlink into the mounted lib/wine, and the
// captured config_info records THIS as line 12 so proton's config-match fast-path still fires. One source of truth.
inline constexpr const char *ProtonDefaultDllCopy =
    "d3dcompiler_*.dll,d3dcsx*.dll,d3dx*.dll,dx8vb.dll,x3daudio*.dll,xactengine*.dll,xapofx*.dll,"
    "xaudio*.dll,xinput*.dll,atl1*.dll,atl.dll,concrt*.dll,msvcp1*.dll,msvcrt*.dll,msvcp7*.dll,"
    "msvcp6*.dll,msvcp_win.dll,msvcr1*.dll,msvcr7*.dll,vcamp1*.dll,vcomp1*.dll,vccorlib1*.dll,"
    "vcruntime1*.dll,ucrtbase.dll,ntdll.dll,vulkan-1.dll,ir50_32.dll";

//Builds the JSON layer-spec from the resolved container (DEFPREFIX base + VFS subcomponents target-rooted +
//PERSIST dirs as RW passthrough + the writable top branch).
nlohmann::ordered_json BuildLayerSpec(struct ContainerParams &ContainerParams);

//Writes the layer-spec and spawns vidyagodfs onto RuntimePath, polling mountinfo for readiness. Registers
//RuntimePath for non-lazy save-safe unmount when durable data is reachable through it.
bool MountVFS(struct ContainerParams &ContainerParams);

//Writes Spec to SpecPath, spawns vidyagodfs onto Mountpoint, polls until live. The low-level mount primitive
//shared by MountVFS, the runner mount, and runner install.
bool SpawnVidyagodfs(const nlohmann::ordered_json &Spec, const std::filesystem::path &Mountpoint,
                     const std::filesystem::path &SpecPath);

//Mounts the selected runner's build (RunnerLayers) read-only at RunnerMountPath. No-op when the runner ships no build.
bool MountRunnerBuild(struct ContainerParams &ContainerParams);

//Walks DirectoryPath recursively and warns (via QMessageBox) if any two paths differ only in case.
bool CheckCaseConflicts(std::filesystem::path DirectoryPath);

//Every current mountpoint at or beneath Prefix (from /proc/self/mountinfo), deepest-first (children before parents).
std::vector<std::string> MountpointsUnder(const std::filesystem::path &Prefix);

//Pre-launch hygiene: clears any runtime left under TempPath by a previously crashed/incomplete run — discovers
//mounts from mountinfo, unmounts deepest-first, then removes TempPath (left in place if any mount refuses to detach).
void CleanStaleRuntime(const std::filesystem::path &TempPath);
}

#endif // VFSMOUNT_H
