#ifndef RUNNERINSTALL_H
#define RUNNERINSTALL_H

#include <nlohmann/json.hpp>

#include <string>

#include "manifestmodel.h"   // NodeIndex

// RunnerInstall — installs a runner's build + generates its one-time DEFPREFIX, lifted out of ContainerWrapper.
// It needs the launch engine's mount (VfsMount::SpawnVidyagodfs) + wineboot machinery, hence its own unit rather
// than living in RunnerWrapper (which is pure data/predicates).
namespace RunnerInstall
{
//Installs one runner VARIANT: hydrates its build layers IN PLACE into the runner's LIBRARY dir (PackageDir), and
//for a wine variant generates the one-time DEFPREFIX at PackageDir/__DEFPREFIX__/<variant>. VariantId "" = default.
bool ImportRunner(nlohmann::ordered_json &GlobalConfigJSON, const nlohmann::ordered_json &RunnerPkg,
                  const std::string &PackageDir, const std::string &VariantId = std::string(), std::string *Error = nullptr);

//Node-native runner install: synthesizes a minimal runner package from a ROLE:"runner" node + its content closure
//(the build) and runs ImportRunner — hydrating the build and generating the one-time DEFPREFIX at
//<bundle>/__DEFPREFIX__/default. The runner-install entry point for the node world (Settings page / play-gate).
bool ImportRunnerNode(nlohmann::ordered_json &GlobalConfigJSON, const NodeIndex &Idx,
                      const std::string &RunnerNodeId, std::string *Error = nullptr);

//True when a runner node is installed: every build VFS layer hydrated locally AND (PREFIX_GENERATE) its DEFPREFIX
//artifact exists. A runner that ships no build is always "installed".
bool RunnerNodeImported(const NodeIndex &Idx, const std::string &RunnerNodeId);
}

#endif // RUNNERINSTALL_H
