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
//Builds the JSON layer-spec from the resolved container (DEFPREFIX base + VFS subcomponents target-rooted +
//PERSIST dirs as RW passthrough + the writable top branch).
nlohmann::ordered_json BuildLayerSpec(struct ContainerParams &ContainerParams);

//Creates the writable paths a built plan NAMES (the write branch + every RW passthrough source). Building a
//plan has no side effects, so the mount materializes what it asks for. False (logged) when one cannot be used: a
//kept folder that is a link (served as is, every write into the keep would land where it points), or a write branch
//that is a link and cannot be replaced. The mount must not go ahead then.
bool MaterializePlanPaths(const nlohmann::ordered_json &Spec);

//Readies a built plan for the FS: materialises the paths it names, THEN reports what is still missing. One
//function because the order is load-bearing — sweeping first flags the very paths the mount is about to
//create. Pinned by the_missing_source_sweep_does_not_flag_paths_the_mount_creates, which drives
//PrepareMount rather than the two halves separately. (NOT by the launch matrix: its golden captures the
//probe's stdout, and this warning is engine log output that never reaches it.) False when MaterializePlanPaths is.
bool PrepareMount(const nlohmann::ordered_json &Spec);

//Reports (and counts) layers in a built plan whose source does not exist on disk. Such a layer mounts EMPTY
//and the mount still SUCCEEDS, so the game just quietly misses those files. Asked by the mount, not by the plan
//builder: a plan is also built by callers that never mount one, where runtime-sourced layers do not exist yet.
//`SkipRuntimeSourced` excludes the layers that cannot exist before a mount — RW passthrough sources (created
//BY the mount) and runtime-sourced layers (reading from another mount). Pass true from callers that build a
//plan without mounting it; the mount itself passes false and checks everything.
size_t ReportMissingSources(const nlohmann::ordered_json &Spec, bool SkipRuntimeSourced = false);

//Writes the layer-spec and spawns vidyagodfs onto RuntimePath, polling mountinfo for readiness. Registers
//RuntimePath for non-lazy save-safe unmount when durable data is reachable through it.
[[nodiscard]] bool MountVFS(struct ContainerParams &ContainerParams);

//Writes Spec to SpecPath, spawns vidyagodfs onto Mountpoint, polls until live. The low-level mount primitive
//shared by MountVFS, the runner mount, and runner install.
[[nodiscard]] bool SpawnVidyagodfs(const nlohmann::ordered_json &Spec, const std::filesystem::path &Mountpoint,
                     const std::filesystem::path &SpecPath, long long *OutPid = nullptr);

//Mounts the selected runner's build (RunnerLayers) read-only at RunnerMountPath. No-op when the runner ships no build.
// The runner build's mount plan (the runner's own zip/delta chain, mounted beside the runtime). Split out from
// MountRunnerBuild so the plan can be asserted without mounting anything.
nlohmann::ordered_json BuildRunnerLayerSpec(struct ContainerParams &ContainerParams);
//True when this container has a runner build to mount (ships one, not a unified runtime, and has layers).
bool RunnerShipsMountableBuild(const struct ContainerParams &ContainerParams);
[[nodiscard]] bool MountRunnerBuild(struct ContainerParams &ContainerParams);

//Walks DirectoryPath recursively and warns (in the log) if any two paths differ only in case.
[[nodiscard]] bool CheckCaseConflicts(const std::filesystem::path &DirectoryPath);

//Pre-launch hygiene: clears any runtime left under TempPath by a previously crashed/incomplete run — discovers
//mounts from mountinfo, unmounts deepest-first, then removes TempPath (left in place if any mount refuses to detach).
void CleanStaleRuntime(const std::filesystem::path &TempPath);

//At startup: every session folder under Root (<Root>/<PackageUID>/<instance>) whose owner is gone is cleared as its next
//launch would clear it (CleanStaleRuntime), with what its writable layer held named in the log (a crash's unsaved
//writes), and the empty package folders go too. A folder with no lock that is still mounted is from a build before the
//lock and is left. Returns how many were cleared.
int SweepDeadSessions(const std::filesystem::path &Root);

//POSIX unmount helpers (no-ops on Windows — WinFsp mounts die with their serving process; see Cleanup):
//UnmountDurable: up to `Retries` polite `fusermount3 -u` attempts 200ms apart (gives a lingering
//wineserver time to release), falling back to a lazy `-uz` detach. Returns true iff the polite unmount
//succeeded — false means the mount was only lazily detached and a TEMP wipe would be unsafe.
[[nodiscard]] bool UnmountDurable(const std::string &Mount, int Retries = 5);
//UnmountLazy: fire-and-forget `-uz` — for ephemeral mounts whose backing store lives under TEMP. Windows: removes a WinFsp
//junction whose volume is gone (a live one is its helper's to unmount: left alone).
void UnmountLazy(const std::string &Mount);
}

#endif // VFSMOUNT_H
