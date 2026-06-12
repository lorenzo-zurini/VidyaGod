#ifndef RUNNERWRAPPER_H
#define RUNNERWRAPPER_H

#include <string>
#include <vector>
#include "nlohmann/json.hpp"

// ---------------------------------------------------------------------------
// RunnerWrapper — install-state helpers for runner *packages*, per runner VARIANT.
//
// A runner mirrors a game: `RUNNERS[].{RUNNER_ID, NAME, VARIANTS[]}` over the shared `COMPONENTS` pool.
// Each runner VARIANT carries the execution params (TYPE/EXECUTABLE/ENV/ARGS/PREFIX_SUBPATH), the guest
// platforms it serves (GUEST_PLATFORM) + its host (HOST_PLATFORM), and MODULES selecting its build
// component(s) (VFSZipLayer(s) with SOURCE {TYPE:"ipfs",CID} — never extracted).
//
// "Installed" is therefore per (RUNNER_ID, VARIANT_ID): the variant's build CIDs are fetched+pinned, and
// (for a wine variant) its one-time read-only DEFPREFIX artifact has been generated. These are pure helpers
// (no mounting / no wineboot); the install work lives in ContainerWrapper::InstallRunner.
//
// Single-runner-entity packages are assumed (RUNNERS[0] is the entity).
// ---------------------------------------------------------------------------
namespace RunnerWrapper {

// ~/.VidyaGod/DOWNLOADS/runners/<RunnerId>/<VariantId> — this runner-variant's install artifacts.
std::string ArtifactDir(const std::string &RunnerId, const std::string &VariantId);

// The read-only DEFPREFIX artifact generated once at install (ArtifactDir/DEFPREFIX).
std::string DefPrefixArtifact(const std::string &RunnerId, const std::string &VariantId);

// RUNNER_ID of the package's runner entity ("" if none).
std::string RunnerId(const nlohmann::ordered_json &RunnerPkg);

// The runner entity's variant ids, and the default (RECOMMENDED, else first; "" if none).
std::vector<std::string> VariantIds(const nlohmann::ordered_json &RunnerPkg);
std::string              DefaultVariantId(const nlohmann::ordered_json &RunnerPkg);

// The runner variant object (empty if not found). Pass "" for VariantId to get the default variant.
nlohmann::ordered_json Variant(const nlohmann::ordered_json &RunnerPkg, const std::string &VariantId);

// Whether the given runner variant is a wine/proton variant (needs a DEFPREFIX).
bool IsWineRunner(const nlohmann::ordered_json &RunnerPkg, const std::string &VariantId);

// Every ipfs CID the variant's build references (VFSZipLayer SOURCEs of the components its MODULES enable).
std::vector<std::string> BuildCids(const nlohmann::ordered_json &RunnerPkg, const std::string &VariantId);

// Whether the variant ships a build (≥1 ipfs build CID) — i.e. it must be installed before use.
bool ShipsBuild(const nlohmann::ordered_json &RunnerPkg, const std::string &VariantId);

// Installed = every build CID cached AND (wine) the DEFPREFIX artifact exists. A variant that ships no
// build is always "installed" (nothing to fetch).
bool IsInstalled(const nlohmann::ordered_json &RunnerPkg, const std::string &VariantId);

} // namespace RunnerWrapper

#endif // RUNNERWRAPPER_H
