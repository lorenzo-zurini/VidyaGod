#ifndef RUNNERINSTALL_H
#define RUNNERINSTALL_H

#include <nlohmann/json.hpp>

#include <string>

#include "manifestmodel.h"   // NodeIndex
#include "ipfswrapper.h"     // IpfsWrapper::FetchTarget (download collection)

#include <vector>

// RunnerInstall — node-native runner installer: hydrates a ROLE:"runner" node's build (its PARENT content closure's
// VFS layers) and, for a PREFIX_GENERATE runner, generates the one-time read-only DEFPREFIX by mounting the build +
// running `wineboot` once. Needs the launch engine's mount (VfsMount) + wineboot machinery, hence its own unit
// rather than living in RunnerWrapper (path/predicate helpers).
namespace RunnerInstall
{
//Generate a PREFIX_GENERATE runner's one-time DEFPREFIX by mounting its (ALREADY-LOCAL) build and running `wineboot`
//once, at <bundle>/DEFPREFIX/default. This is the DEFPREFIX step ONLY — it does NOT fetch; the build must already
//be hydrated (the single download pump DownloadManager::beginDownload, or --import-runner, does that). A completion
//sentinel (<...>/default.ok, a sibling of the artifact so it is never mounted) marks a FULLY-built prefix; Force=true
//wipes any existing/partial artifact and rebuilds (a deliberate re-do). A non-PREFIX_GENERATE runner is a no-op
//success. On failure the fetched build is KEPT (only the partial prefix is dropped) so a retry is prefix-only, and the
//wineboot stderr tail is folded into *Error so the failure is diagnosable.

//Gather (without fetching) a runner node's missing build-layer download targets, appending to Out — so the single
//download batch (beginDownload) pools a game's content with its runners' builds. Returns false (with *Error) only on a
//malformed runner node; "nothing to fetch" is success with Out unchanged.
bool CollectRunnerNodeTargets(const NodeIndex &Idx, const std::string &RunnerNodeId,
                              std::vector<IpfsWrapper::FetchTarget> &Out, std::string *Error = nullptr);

//True when every build VFS layer of a runner node is hydrated locally (regardless of DEFPREFIX state). A runner that
//ships no build is always "present".
bool RunnerBuildPresent(const NodeIndex &Idx, const std::string &RunnerNodeId);

//True when a runner node is FULLY installed: build present AND (PREFIX_GENERATE) its DEFPREFIX completion sentinel
//exists. A runner that ships no build / generates no prefix is always "installed".
bool RunnerNodeImported(const NodeIndex &Idx, const std::string &RunnerNodeId);
}

#endif // RUNNERINSTALL_H
