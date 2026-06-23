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
//Install a runner NODE: hydrate its build layers IN PLACE into the runner's LIBRARY bundle dir, and (PREFIX_GENERATE)
//generate the one-time DEFPREFIX at <bundle>/__DEFPREFIX__/default. Idempotent. The runner-install entry point for
//the node world (Settings page / play-gate / the download flow). GlobalConfigJSON is currently unused.
bool ImportRunnerNode(nlohmann::ordered_json &GlobalConfigJSON, const NodeIndex &Idx,
                      const std::string &RunnerNodeId, std::string *Error = nullptr);

//Gather (without fetching) a runner node's missing build-layer download targets, appending to Out — so a download
//can pool a game's content with its runners' builds into ONE concurrent batch. After that batch fetches them,
//ImportRunnerNode just generates the DEFPREFIX (its own fetch loop sees the layers already present). Returns false
//(with *Error) only on a malformed runner node; "nothing to fetch" is success with Out unchanged.
bool CollectRunnerNodeTargets(const NodeIndex &Idx, const std::string &RunnerNodeId,
                              std::vector<IpfsWrapper::FetchTarget> &Out, std::string *Error = nullptr);

//True when a runner node is installed: every build VFS layer hydrated locally AND (PREFIX_GENERATE) its DEFPREFIX
//artifact exists. A runner that ships no build is always "installed".
bool RunnerNodeImported(const NodeIndex &Idx, const std::string &RunnerNodeId);
}

#endif // RUNNERINSTALL_H
