#include "runnerinstall.h"
#include "ipfswrapper.h"     // IpfsWrapper (build-layer content fetch)


#include <filesystem>
#include <map>
#include <string>
#include <vector>

//VFS-layer helpers (IsVfsLayer/LayerType/ResolveNodeOrder/ResolveLayerSource/LayerLocator) live in ManifestModel.
using namespace ManifestModel;

namespace {
//A runner's BUILD = its content closure (PARENTS), excluding the runner node itself: the content nodes carrying the
//runner's VFS layers (proton/wine binaries, …), in load order. A runner node's OWN LAYERS are never the build (they
//are ignored — validation warns); the build always lives on PARENT content nodes.
std::vector<const Node *> RunnerBuildNodes(const NodeIndex &Idx, const std::string &RunnerNodeId)
{
    std::vector<const Node *> Out;
    for (const std::string &Id : ManifestModel::ResolveNodeOrder(Idx, RunnerNodeId, {}))
    {
        if (Id == RunnerNodeId) continue;
        const Node *C = Idx.Find(Id);
        if (C && C->Layers.is_array() && !C->Layers.empty()) Out.push_back(C);
    }
    return Out;
}

} // namespace

bool RunnerInstall::CollectRunnerNodeTargets(const NodeIndex &Idx, const std::string &RunnerNodeId,
                                             std::vector<IpfsWrapper::FetchTarget> &Out, std::string *Error)
{
    const Node *R = Idx.Find(RunnerNodeId);
    if (!R || !R->IsRunner()) { if (Error) *Error = "not a runner node: " + RunnerNodeId; return false; }
    std::error_code Ec;
    for (const Node *C : RunnerBuildNodes(Idx, RunnerNodeId))
        for (const auto &L : C->Layers)
        {
            if (!IsVfsLayer(LayerType(L))) continue;
            std::filesystem::path Local; std::string Cid;
            LayerLocator(L, C->BundleDir, Local, Cid);                            // resolve against the layer's OWN node bundle
            if (Cid.empty() || Local == C->BundleDir || std::filesystem::exists(Local, Ec)) continue;  // present / no source
            Out.push_back({Cid, Local.string(), false});
        }
    return true;
}


bool RunnerInstall::RunnerBuildPresent(const NodeIndex &Idx, const std::string &RunnerNodeId)
{
    const Node *R = Idx.Find(RunnerNodeId);
    if (!R || !R->IsRunner()) return false;

    //Every build VFS layer hydrated locally (the runner's content closure). A runner that ships no build is "present".
    std::error_code Ec;
    for (const Node *C : RunnerBuildNodes(Idx, RunnerNodeId))
        for (const auto &L : C->Layers)
        {
            if (!IsVfsLayer(LayerType(L))) continue;
            std::filesystem::path Local; std::string Cid;
            LayerLocator(L, C->BundleDir, Local, Cid);
            if (Local == C->BundleDir) continue;                                  // no PATH
            if (!std::filesystem::exists(Local, Ec)) return false;                // a build layer missing → not present
        }
    return true;
}

bool RunnerInstall::RunnerNodeImported(const NodeIndex &Idx, const std::string &RunnerNodeId)
{
    //A runner is "imported" once its build is hydrated locally. A PREFIX_GENERATE runner needs no separate step:
    //its prefix assembles from the build at launch via node-declared layers, so build-present == ready.
    return RunnerBuildPresent(Idx, RunnerNodeId);
}
