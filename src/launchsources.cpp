#include "launchsources.h"
#include "ipfswrapper.h"     // IpfsWrapper (backend fetch/has-local)
#include "commonutils.h"     // Log*

#include <filesystem>
#include <set>
#include <string>
#include <vector>

//VFS layer helpers (IsVfsLayer/LayerLocator/ResolveLayerSource/...) live in ManifestModel; bring them in unqualified.
using namespace ManifestModel;

//Returns the unresolved dependency locators for a built container — drives the portable/standalone
//readiness warning. Iteration 1 verifies every VFS layer's resolved source exists.
//TODO(sharing): also verify the resolved runner + its RUNTIME + cross-package component references, and
//distinguish a fully-bundled (portable) chain from one needing external/global packages.
std::vector<std::string> LaunchSources::VerifyDependencies(const struct ContainerParams &ContainerParams)
{
    std::vector<std::string> Missing;
    for (const auto &Sub : ContainerParams.SubComponentsArray)
    {
        const std::string Type = Sub.value("TYPE", std::string());
        if (!IsVfsLayer(Type)) continue;
        std::filesystem::path Local; std::string Cid;
        LayerLocator(Sub, ContainerParams.PackagePath, Local, Cid);
        std::error_code Ec;
        if (std::filesystem::exists(Local, Ec)) continue;        // present locally (highest priority)
        if (!Cid.empty()) continue;                              // fetchable from a backend — not "missing"
        LogErr("LaunchSources::VerifyDependencies", "Layer content unavailable (no local file, no source): " + Local.string());
        Missing.push_back(Local.string());
    }
    return Missing;
}

//Pre-flight CHECK (never fetches — safe to call from the GUI thread, e.g. the play() gate). Returns false,
//blocking the launch, only when something can't be satisfied:
//  - a RUNNER build CID isn't cached (runners are fetched ahead of time by ImportRunner — "import the runner"),
//  - the wine runner's DEFPREFIX artifact is missing,
//  - a GAME VFS layer has neither local content NOR a backend (CID) to fetch it from.
//A game layer that is missing locally but HAS a CID is fine here — MaterializeLayers fetches it on the worker.
bool LaunchSources::EnsureSources(struct ContainerParams &ContainerParams)
{
    bool Ok = true;
    //Runner build layers must be hydrated locally in the runner's library dir (ImportRunner fetches them ahead
    //of time — "import the runner"). Missing → the runner isn't installed; block the launch.
    for (const auto &Sub : ContainerParams.RunnerLayers)
    {
        const std::string Type = Sub.value("TYPE", std::string());
        if (!IsVfsLayer(Type)) continue;
        std::filesystem::path Local; std::string Cid;
        LayerLocator(Sub, ContainerParams.RunnerPackagePath, Local, Cid);
        std::error_code Ec;
        if (std::filesystem::exists(Local, Ec))
        {
            //IPFS fetch (FetchToPath) drops the executable bit, so a loose runner binary (e.g. a bundled
            //AppImage as a VFSFileLayer) lands as 0644 and can't be exec'd once mounted. Zip/Dir layers keep
            //their internal entry modes, so this only matters for a single-file runner build. Restore +x.
            if (Type == "VFSFileLayer")
                std::filesystem::permissions(Local, std::filesystem::perms::owner_exec
                    | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
                    std::filesystem::perm_options::add, Ec);
            continue;
        }
        Ok = false;
        LogErr("LaunchSources::EnsureSources", "Runner not imported: missing build " + Local.string());
    }

    for (const auto &Sub : ContainerParams.SubComponentsArray)                   // game VFS layers: local OR fetchable
    {
        const std::string Type = Sub.value("TYPE", std::string());
        if (!IsVfsLayer(Type)) continue;
        std::filesystem::path Local; std::string Cid;
        LayerLocator(Sub, ContainerParams.PackagePath, Local, Cid);
        std::error_code Ec;
        if (std::filesystem::exists(Local, Ec) || !Cid.empty()) continue;        // present, or a backend can provide it
        Ok = false;
        LogErr("LaunchSources::EnsureSources", "Missing layer content (no local file, no source): " + Local.string());
    }

    //An installed prefix-generating runner must have its one-time DEFPREFIX artifact (generated at import).
    if (ContainerParams.RunnerShipsBuild && ContainerParams.PrefixGenerate)
    {
        std::error_code Ec;
        if (ContainerParams.DefPrefixPath.empty() || !std::filesystem::exists(ContainerParams.DefPrefixPath, Ec))
        {
            Ok = false;
            LogErr("LaunchSources::EnsureSources", "Runner not imported: missing DEFPREFIX artifact " + ContainerParams.DefPrefixPath.string());
        }
    }

    if (!Ok) LogErr("LaunchSources::EnsureSources", "Required dependencies are unavailable — launch blocked.");
    return Ok;
}

//Hierarchical fallback MATERIALIZER (worker thread only — may block on a download). For each GAME VFS layer
//whose local content is missing, fetches it from a backend (IPFS now) straight to its expected local PATH, so
//the package self-heals and subsequent launches are fully local. Runner builds are not materialized here (they
//stay in the shared cache, fetched at runner import). Returns false if a required layer could not be fetched.
bool LaunchSources::MaterializeLayers(struct ContainerParams &ContainerParams)
{
    bool Ok = true;
    for (const auto &Sub : ContainerParams.SubComponentsArray)
    {
        const std::string Type = Sub.value("TYPE", std::string());
        if (!IsVfsLayer(Type)) continue;
        std::filesystem::path Local; std::string Cid;
        LayerLocator(Sub, ContainerParams.PackagePath, Local, Cid);
        std::error_code Ec;
        if (std::filesystem::exists(Local, Ec)) continue;                        // already local — highest priority
        if (Cid.empty()) continue;                                              // no backend (flagged by EnsureSources)
        std::string Err;                                                        // fetch from IPFS to the expected path
        if (IpfsWrapper::FetchToPath(Cid, Local.string(), &Err).empty())
        {
            Ok = false;
            LogErr("LaunchSources::MaterializeLayers", "Could not fetch layer CID " + Cid + " -> " + Local.string() + " (" + Err + ")");
        }
    }
    return Ok;
}