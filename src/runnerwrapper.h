#ifndef RUNNERWRAPPER_H
#define RUNNERWRAPPER_H

#include <string>
#include <vector>
#include "nlohmann/json.hpp"

// ---------------------------------------------------------------------------
// RunnerWrapper — import-state helpers for runner *packages*, per runner VARIANT.
//
// A runner mirrors a game: `RUNNERS[].{RUNNER_ID, NAME, VARIANTS[]}` over the shared `COMPONENTS` pool.
// Each runner VARIANT carries the execution params (TYPE/EXECUTABLE/ENV/ARGS/PREFIX_SUBPATH), the guest
// platforms it serves (GUEST_PLATFORM) + its host (HOST_PLATFORM), and MODULES selecting its build
// component(s) (VFSZipLayer(s) with SOURCE {TYPE:"ipfs",CID} — never extracted).
//
// "Imported" is therefore per (RUNNER_ID, VARIANT_ID): the variant's build CIDs are fetched+pinned, and
// (for a wine variant) its one-time read-only DEFPREFIX artifact has been generated. These are pure helpers
// (no mounting / no wineboot); the install work lives in ContainerWrapper::ImportRunner.
//
// Single-runner-entity packages are assumed (RUNNERS[0] is the entity).
// ---------------------------------------------------------------------------
namespace RunnerWrapper {

// The read-only DEFPREFIX a wine runner generates once at import, inside its own LIBRARY package dir:
// <PackageDir>/__DEFPREFIX__/<VariantId>.
std::string DefPrefixArtifact(const std::string &PackageDir, const std::string &VariantId);

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

// Whether the variant ships a build (≥1 ipfs build CID) — i.e. it must be imported before use.
bool ShipsBuild(const nlohmann::ordered_json &RunnerPkg, const std::string &VariantId);

// Whether a variant's EXECUTABLE can actually run on this system. A build-based runner (EXECUTABLE resolves
// from its mounted build / contains a %VAR%) and a native passthrough (empty EXECUTABLE) are always available;
// a built-in PATH runner (a bare command like "wine"/"umu-run"/"snes9x") is available only if that command is
// found on PATH (or is an existing executable path). Gates such runners out of the picker/launch resolver.
bool ExecutableAvailable(const nlohmann::ordered_json &Variant);

// Imported = every build layer hydrated locally in PackageDir AND (wine) the DEFPREFIX exists. A variant that
// ships no build is always "imported" (nothing to fetch).
bool IsImported(const nlohmann::ordered_json &RunnerPkg, const std::string &PackageDir, const std::string &VariantId);

} // namespace RunnerWrapper

#endif // RUNNERWRAPPER_H
