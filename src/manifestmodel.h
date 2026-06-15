#ifndef MANIFESTMODEL_H
#define MANIFESTMODEL_H

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <filesystem>

// ---------------------------------------------------------------------------
// ManifestModel — pure, stateless queries over a package MANIFEST (no launch session, no instance state).
//
// Two groups: (1) VFS-layer helpers that centralise the "is this a content layer / where does it live" logic
// that used to be copy-pasted across the container code; (2) game/variant/module/component lookups and the
// manifest-only package predicates (hydrated / has-content / referenced CIDs). The launch engine
// (ContainerWrapper) and the sharing service (PackageCatalog) both build on these.
// ---------------------------------------------------------------------------

//A toggleable build module: a reference to a component (leaf OR internal node) the user can enable or disable.
//REQUIRED modules are always in the recipe and locked in the UI; optional ones (REQUIRED:false) are user-toggled,
//starting at DEFAULT. Used identically by game variants and runners (universal schema).
struct ModuleInfo {
    std::string Component;        // COMPONENT — the component id this module pulls in
    std::string Label;            // optional LABEL for the tree (falls back to component NAME/id)
    bool        Required = true;  // REQUIRED — defaults true (modules are required unless opted out)
    bool        Default  = true;  // DEFAULT — initial enabled state when optional; defaults true
    std::vector<std::string> Exclude; // EXCLUDE — component ids this module is mutually exclusive with (symmetric)
};

//One available variant for a subgame (or a runner). Selecting a variant = selecting which MODULES to build.
struct VariantInfo {
    std::string VariantID;             // VARIANT_ID field on the variant
    std::string Name;                  // optional NAME; falls back to VARIANT_ID for display
    bool        IsRecommended = false; // RECOMMENDED:true — shown with ⭐ in the picker
    std::string HostPlatform;          // HOST_PLATFORM — per-variant (game: target platform; runner: host OS)
    std::vector<std::string> GuestPlatform; // GUEST_PLATFORM — runner variants only (guests this variant serves)
    std::vector<ModuleInfo> Modules;   // MODULES array (toggleable component references, load order)
};

namespace ManifestModel {

// ----- VFS layer helpers (the dedup foundation) -----
// True if a subcomponent TYPE string names a VFS content layer (Zip/Dir/File).
bool IsVfsLayer(const std::string &Type);
// A subcomponent's TYPE ("" if absent).
std::string LayerType(const nlohmann::ordered_json &Sub);
// Invokes Fn on every VFS-layer subcomponent across a COMPONENTS array (any json shape is tolerated).
void ForEachVfsLayer(const nlohmann::ordered_json &Components,
                     const std::function<void(const nlohmann::ordered_json &Sub)> &Fn);
// A VFS layer's locators: its expected LOCAL path (PackagePath/PATH, SOURCE.PATH overriding) and its ipfs CID
// (empty if none). The local path is the content's home; the CID is a remote fallback to fetch it from.
void LayerLocator(const nlohmann::ordered_json &Sub, const std::filesystem::path &PackagePath,
                  std::filesystem::path &Local, std::string &Cid);
// Convenience: a layer's resolved LOCAL absolute path (PackagePath/PATH). Pure, local-only (no cache).
std::string ResolveLayerSource(const nlohmann::ordered_json &Sub, const std::filesystem::path &PackagePath);

// ----- platform -----
std::string MachinePlatform();   // the host platform a runner variant's HOST_PLATFORM must match (e.g. "linux64")
std::string HostPlatform();      // the platform token the host runs as (Phase-1 stub: "linux64")

// ----- game / component / variant / module lookups -----
int FindGameIndex(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID);     // index or -1
int FindComponentIndex(const nlohmann::ordered_json &MANIFESTJSON, const std::string &ComponentID); // index or -1
std::vector<VariantInfo> GetAvailableVariants(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID);
std::vector<ModuleInfo>  GetVariantModules(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID, const std::string &VariantID);
std::vector<ModuleInfo>  ParseModules(const nlohmann::ordered_json &ModulesArray);
// Resolves which of `Modules` are enabled into component ids (load order): REQUIRED||(state?:DEFAULT), REQUIRED
// propagating up the PARENTCOMPONENT chain, EXCLUDE mutual exclusion, and the hierarchy gate. Variants AND runners.
std::vector<std::string> ResolveEnabledModules(const std::vector<ModuleInfo> &Modules,
                                               const std::map<std::string, bool> &ModuleStates,
                                               const nlohmann::ordered_json &MANIFESTJSON);
// Convenience: a variant's default-enabled module components (REQUIRED||DEFAULT, no overrides).
std::vector<std::string> FindEndpointsForVariant(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID, const std::string &VariantID);

// ----- manifest-only package predicates -----
// Every distinct ipfs CID referenced by the package's content layers (the set an install must fetch).
std::vector<std::string> PackageIpfsCids(const nlohmann::ordered_json &Manifest);
// Every distinct cover-art CID (GAMES[].METADATA.COVER / legacy game-level COVER) — drives the IPFS "Assets" group.
std::vector<std::string> PackageCoverCids(const nlohmann::ordered_json &Manifest);
// True when every VFS content layer is present locally at its expected path (vacuously true if there are none).
bool PackageHydrated(const nlohmann::ordered_json &Manifest, const std::string &PackageDir);
// True iff the manifest declares any VFS content layer (distinguishes a real package from a content-less one).
bool PackageHasContent(const nlohmann::ordered_json &Manifest);

} // namespace ManifestModel

#endif // MANIFESTMODEL_H
