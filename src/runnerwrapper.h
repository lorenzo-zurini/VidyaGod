#ifndef RUNNERWRAPPER_H
#define RUNNERWRAPPER_H

#include <string>
#include <vector>
#include "nlohmann/json.hpp"

// ---------------------------------------------------------------------------
// RunnerWrapper — install-state helpers for runner *packages*.
//
// A runner is delivered "like a normal package": HOST_PLATFORM + RUNNERS[0] +
// COMPONENTS whose SUBCOMPONENTS include VFSZipLayer(s) with SOURCE {TYPE:"ipfs",
// CID} (the runner build, e.g. a Proton zip — never extracted). A runner is
// "installed" once its IPFS layers are fetched+pinned and (for wine/proton) its
// one-time read-only DEFPREFIX artifact has been generated.
//
// These are pure helpers (no mounting / no wineboot). The actual install work —
// fetch + mount + generate DEFPREFIX — lives in ContainerWrapper::InstallRunner,
// which has the vidyagodfs machinery.
// ---------------------------------------------------------------------------
namespace RunnerWrapper {

// ~/.VidyaGod/DOWNLOADS/runners/<RunnerId> — where this runner's install artifacts live.
std::string ArtifactDir(const std::string &RunnerId);

// The read-only DEFPREFIX artifact generated once at install (ArtifactDir/DEFPREFIX).
std::string DefPrefixArtifact(const std::string &RunnerId);

// The RUNNER_ID of a runner package's first RUNNERS entry ("" if none).
std::string RunnerId(const nlohmann::ordered_json &RunnerPkg);

// Whether a runner package's RUNNERS[0] is a wine/proton runner (needs a DEFPREFIX).
bool IsWineRunner(const nlohmann::ordered_json &RunnerPkg);

// Whether a runner package ships its own build (has at least one VFSZipLayer with an ipfs SOURCE) — i.e.
// it must be installed before use. PATH runners (umu-run, system wine, emulators) have none.
bool ShipsBuild(const nlohmann::ordered_json &RunnerPkg);

// Every ipfs CID referenced by the runner package's VFSZipLayer subcomponents (the build to fetch).
std::vector<std::string> BuildCids(const nlohmann::ordered_json &RunnerPkg);

// Installed = every BuildCid is cached AND (wine) the DEFPREFIX artifact exists. A PATH runner
// (ShipsBuild == false) is always "installed" (nothing to fetch).
bool IsInstalled(const nlohmann::ordered_json &RunnerPkg);

} // namespace RunnerWrapper

#endif // RUNNERWRAPPER_H
