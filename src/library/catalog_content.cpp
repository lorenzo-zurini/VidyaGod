// A package's content: what is installed, what a closure fetches (its own, its runners'), hydrating and
// dehydrating it.
#include "packagecatalog.h"
#include "sessions.h"
#include "nodestore.h"
#include "contentfile.h"
#include "builtinrunners.h"
#include "fold.h"   // Fold — a row's grafts are what its resolution offers
#include "packagecatalog_p.h"
#include "apppaths.h"
#include "instancestore.h"
#include "manifestmodel.h"
#include "nodegraph.h"        // gigagraph catalog: GatherWorkingTree / FreezeToIndex
#include "parallel.h"
#include <tuple>
#include "platform/platform.h"   // SelfExe — the program an index cache was built by
#include <optional>
#include <mutex>
#include "commonutils.h"
#include "jsonoperations.h"
#include "ipfswrapper.h"
#include "downloadqueue.h"      // EnqueueBatch/WaitBatch — closure completion rides the ONE rolling queue
#include "runnerwrapper.h"
#include "varsubst.h"           // %KEY% substitution — resolve CustomVar-templated content-layer PATHs for hydration
#include "launchresolver.h"     // ResolveChainIds — pool a game's resolved runner chain into its hydrate (full-closure)
#include "launchparams.h"       // ContainerParams (minimal, for the chain resolve)

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStringList>
#include <cstdint>
#include <map>
#include <mutex>
#include <condition_variable>
#include <set>
#include <deque>
#include <functional>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <vector>

using namespace ManifestModel;   // LayerLocator / IsVfsLayer / …

namespace PackageCatalog {
using namespace Shelf;       // the library walks rows and grafts as the plan sees them
using namespace Installed;

bool CollectRunnerNodeTargets(const NodeIndex &Idx, const std::string &RunnerNodeId,
                              std::vector<IpfsWrapper::FetchTarget> &Out, std::string *Error)
{
    const Node *R = Idx.Find(RunnerNodeId);
    if (!R || !R->IsRunner()) { if (Error) *Error = "not a runner node: " + RunnerNodeId; return false; }
    std::error_code Ec;
    for (const Node *C : Installed::RunnerBuildNodes(Idx, RunnerNodeId))
        for (const auto &L : C->ContentLayers)
        {
            if (!ManifestModel::IsRunnerBuildLayer(L)) continue;                   // NOT IsVfsLayer — see RunnerBuildPresent
            std::filesystem::path Local; std::string Cid;
            ManifestModel::LayerLocator(L, C->BundleDir, Local, Cid);             // against the layer's OWN node bundle
            if (Cid.empty() || Local == C->BundleDir || std::filesystem::exists(Local, Ec)) continue;  // present / no source
            Out.push_back({Cid, Local.string(), false});
        }
    return true;
}

namespace {
//Per package folder of the index: the node ScanBundleIdentity would name it by (a presentable launchable first, then
//any node with a UID, then the first) and its shelf rows — the same rule, read from the index instead of the disk.
struct BundleNodes { const Node *Rep = nullptr; bool RepLaunchable = false; std::vector<const Node *> Variants; };
std::map<std::filesystem::path, BundleNodes> NodesByBundle(const NodeIndex &Idx)
{
    std::map<std::filesystem::path, BundleNodes> Out;
    for (const auto &[K, N] : Idx.Nodes)
    {
        if (N.BundleDir.empty() || N.Received) continue;
        BundleNodes &B = Out[std::filesystem::path(N.BundleDir).lexically_normal()];
        if (N.IsVariant()) B.Variants.push_back(&N);
        const bool Launchable = N.IsVariant() && !N.IsGraft;
        if (Launchable && (!B.RepLaunchable || (N.Presentable() && !B.Rep->Presentable()))) { B.Rep = &N; B.RepLaunchable = true; }
        else if (!B.RepLaunchable && (!B.Rep || (B.Rep->Uid.empty() && !N.Uid.empty()))) B.Rep = &N;
    }
    return Out;
}
std::string IdentityUid(const Node &N) { return N.Uid.empty() ? N.NodeId : N.Uid; }
} // namespace

std::vector<InstalledPackage> InstalledPackages(const nlohmann::ordered_json &GlobalConfigJSON, const NodeIndex &Idx, bool Sizes)
{
    std::vector<InstalledPackage> Out;
    const auto Hyd = HydrationMap(Idx);
    const std::filesystem::path Root = LibraryRootDir(GlobalConfigJSON);
    for (const auto &[Dir, B] : NodesByBundle(Idx))   // a friend's CATALOG stubs are not in it (Node::Received)
    {
        if (!B.RepLaunchable) continue;
        bool Installed = false;
        for (const Node *V : B.Variants)
        {
            const auto It = Hyd.find(V->Key());
            Installed = Installed || (!V->IsGraft && It != Hyd.end() && It->second.OnDisk && It->second.Hydrated);
        }
        if (!Installed) continue;
        InstalledPackage P;
        P.Dir = Dir;
        P.Uid = IdentityUid(*B.Rep);
        P.Title = B.Rep->Meta.is_object() ? B.Rep->Meta.value("TITLE", B.Rep->NodeId) : B.Rep->NodeId;
        P.Local = Root.empty() || !PathUnder(Root, Dir);
        if (GlobalConfigJSON.contains("LIBRARY") && GlobalConfigJSON["LIBRARY"].is_array())
            for (const auto &E : GlobalConfigJSON["LIBRARY"])
                if (E.is_object() && std::filesystem::path(E.value("PATH", std::string())).lexically_normal() == Dir) { P.HasEntry = true; break; }
        P.Library = P.Local ? std::string() : LibraryOf(Dir, Root);
        for (const Node *V : B.Variants) if (!V->IsGraft) P.Variants.push_back(V->Variant);
        std::sort(P.Variants.begin(), P.Variants.end());
        P.Variants.erase(std::unique(P.Variants.begin(), P.Variants.end()), P.Variants.end());
        Out.push_back(std::move(P));
    }
    if (Sizes)
        Parallel::For(Out.size(), [&](std::size_t I) {
            std::error_code Ec;
            for (auto It = std::filesystem::recursive_directory_iterator(Out[I].Dir, std::filesystem::directory_options::skip_permission_denied, Ec);
                 !Ec && It != std::filesystem::recursive_directory_iterator(); It.increment(Ec))
                if (std::error_code E2; It->is_regular_file(E2)) Out[I].Bytes += It->file_size(E2);
        });
    std::sort(Out.begin(), Out.end(), [](const InstalledPackage &A, const InstalledPackage &B) {
        return std::tie(A.Local, A.Library, A.Title, A.Dir) < std::tie(B.Local, B.Library, B.Title, B.Dir);
    });
    return Out;
}

std::vector<std::filesystem::path> PackageDirsWithUid(const NodeIndex &Idx, const std::string &Uid)
{
    std::vector<std::filesystem::path> Out;
    for (const auto &[Dir, B] : NodesByBundle(Idx))
    {
        const std::string F = Dir.filename().string();   // "[7804][v1.0] Age of Mythology": the UID it was made under
        const bool Folder = F.size() > Uid.size() + 1 && F[0] == '[' && F.compare(1, Uid.size(), Uid) == 0 && F[Uid.size() + 1] == ']';
        if ((B.Rep && IdentityUid(*B.Rep) == Uid) || Folder) Out.push_back(Dir);
    }
    return Out;
}

int DehydrateNode(const NodeIndex &Idx, const std::string &LaunchNodeId)
{
    // Inverse of Download: delete the node closure's local content-layer files (keeping the manifests + cover, so
    // the package returns to the Catalog as re-downloadable) and unpin + drop the references for their CIDs (so the
    // node stops "seeding" content that's no longer on disk). Cover art (META.COVER) is left in place — it's tiny,
    // git-tracked, and needed to render the catalog tile.
    int Removed = 0;
    std::set<std::string> Cids;
    ForEachContentLayer(Idx, LaunchNodeId, {}, [&](const nlohmann::ordered_json&, const std::filesystem::path &Local, const std::string &Cid){
        if (!Cid.empty()) Cids.insert(Cid);
        std::error_code Ec;
        if (std::filesystem::exists(Local, Ec) && std::filesystem::is_regular_file(Local, Ec))
        { std::filesystem::remove(Local, Ec); if (!Ec) ++Removed; }
    }, false, false);
    for (const std::string &C : Cids) { IpfsWrapper::Unpin(C); IpfsWrapper::DropRef(C); }
    return Removed;
}

std::vector<std::string> NodeContentCids(const NodeIndex &Idx, const std::string &LaunchNodeId, const GraftChoice &Grafts)
{
    std::vector<std::string> Cids;
    std::set<std::string> Seen;
    ForEachContentLayer(Idx, LaunchNodeId, Grafts, [&](const nlohmann::ordered_json&, const std::filesystem::path&, const std::string &Cid){
        if (!Cid.empty() && Seen.insert(Cid).second) Cids.push_back(Cid);
    });
    return Cids;
}

std::map<std::string, long long> NodeContentSizes(const NodeIndex &Idx, const std::string &LaunchNodeId,
                                                  const GraftChoice &Grafts)
{
    // The stamped SOURCE.SIZE per content CID — the sizes-in-JSON keystone: a download size must derive INSTANTLY
    // and OFFLINE from the graph, never from a ~35s network probe per CID (which left the pre-download dialog
    // "estimating…" for minutes). Only stamped (>0) entries are returned; the caller probes the rare unstamped rest.
    std::map<std::string, long long> Out;
    ForEachContentLayer(Idx, LaunchNodeId, Grafts, [&](const nlohmann::ordered_json &L, const std::filesystem::path&, const std::string &Cid){
        if (Cid.empty() || !L.contains("SOURCE") || !L["SOURCE"].is_object()) return;
        const long long Sz = L["SOURCE"].value("SIZE", (long long)0);
        if (Sz > 0) Out[Cid] = Sz;
    });
    return Out;
}

bool CollectContentTargets(const NodeIndex &Idx, const std::string &LaunchNodeId, const GraftChoice &Grafts,
                           std::vector<IpfsWrapper::FetchTarget> &Out, std::string *Error)
{
    auto Fail = [&](const std::string &M) -> bool { if (Error) *Error = M; LogErr("PackageCatalog::CollectContentTargets", M); return false; };
    const Node *Launch = Idx.Find(LaunchNodeId);
    if (!Launch) return Fail("launch node not found: " + LaunchNodeId);

    // Required content layers: fetch any that aren't already on disk; a missing layer with no IPFS source is fatal
    // for the game's OWN closure — a graft's (a ticked mod's) is reported and skipped, so one broken mod never
    // bricks the base game's download.
    bool MissingSource = false; std::string MissingErr;
    std::set<std::string> OwnLayers;
    ForEachContentLayer(Idx, LaunchNodeId, Grafts, [&](const nlohmann::ordered_json &, const std::filesystem::path &Local, const std::string &){
        OwnLayers.insert(Local.string()); });
    ForEachContentLayer(Idx, LaunchNodeId, Grafts, [&](const nlohmann::ordered_json &L, const std::filesystem::path &Local, const std::string &Cid){
        if (MissingSource) return;
        std::error_code Ec;
        if (std::filesystem::exists(Local, Ec)) return;                          // already present
        if (Cid.empty())
        {
            if (!OwnLayers.count(Local.string()))
            { LogWarn("PackageCatalog::CollectContentTargets", "graft layer has no local file and no IPFS source, skipped: " + Local.string()); return; }
            MissingSource = true; MissingErr = "missing local content with no IPFS source: " + Local.string(); return;
        }
        // Push the stamped SOURCE.SIZE to the node so a gateway-fallback fetch of this layer reports a real % (and a
        // pre-fetch total). Done HERE — at fetch-build, on the shared path for the GUI download, --fetch AND
        // --download-all — so it works headless with no GUI model, and before getRoot's gateway phase runs.
        if (L.contains("SOURCE") && L["SOURCE"].is_object())
        { const long long Sz = L["SOURCE"].value("SIZE", (long long)0); if (Sz > 0) IpfsWrapper::SetExpectedSize(Cid, Sz); }
        Out.push_back({Cid, Local.string(), false});
    }, /*WithGrafts=*/true);
    if (MissingSource) return Fail(MissingErr);

    // Cover: fetch if remote; if already on disk but un-seeded/orphaned, seed it by reference so a downloader serves
    // it too. Best-effort (a cover never fails a hydrate).
    if (Launch->Meta.is_object() && Launch->Meta.contains("COVER") && Launch->Meta["COVER"].is_object())
    {
        std::filesystem::path Local; std::string Cid;
        LayerLocator(Launch->Meta["COVER"], Launch->BundleDir, Local, Cid);
        std::error_code Ec;
        if (!Cid.empty() && Local != Launch->BundleDir)
        {
            if (!std::filesystem::exists(Local, Ec))
            {
                const auto &Cv = Launch->Meta["COVER"];                      // register the cover's stamped SIZE too
                if (Cv.contains("SOURCE") && Cv["SOURCE"].is_object())
                { const long long Sz = Cv["SOURCE"].value("SIZE", (long long)0); if (Sz > 0) IpfsWrapper::SetExpectedSize(Cid, Sz); }
                Out.push_back({Cid, Local.string(), true});                 // not local → fetch over IPFS (FetchToPath seeds it)
            }
            else if (!IpfsWrapper::HasLocal(Cid) || IpfsWrapper::CidMissing(Cid))
            {
                if (IpfsWrapper::CidMissing(Cid)) IpfsWrapper::DropRef(Cid);   // re-point a stale reference
                std::string E;
                if (IpfsWrapper::AddNoCopy(Local.string(), &E).empty())
                    LogWarn("PackageCatalog::CollectContentTargets", "could not seed local cover " + Local.string() + " (" + E + ")");
            }
        }
    }
    return true;
}

std::set<std::filesystem::path> PackagesToAdopt(const NodeIndex &Idx, const std::vector<std::string> &LaunchIds,
                                                const std::vector<std::string> &RunnerIds,
                                                const nlohmann::ordered_json &GlobalConfigJSON)
{
    std::set<std::filesystem::path> Out;
    auto Take = [&](const std::string &Root) {
        for (const std::string &Id : ManifestModel::Closure(Idx, Root))
            if (const Node *N = Idx.Find(Id); N && N->Received && !N->BundleDir.empty()) Out.insert(N->BundleDir);
    };
    for (const std::string &Lid : LaunchIds)
    {
        Take(Lid);
        for (const std::string &Rid : RunnerChainIds(Idx, Lid, GlobalConfigJSON)) Take(Rid);
    }
    for (const std::string &Rid : RunnerIds) Take(Rid);
    return Out;
}

std::vector<std::string> RunnerChainIds(const NodeIndex &Idx, const std::string &LaunchNodeId,
                                        const nlohmann::ordered_json &GlobalConfigJSON)
{
    const Node *Launch = Idx.Find(LaunchNodeId);
    if (!Launch) return {};
    if (Launch->IsGraft)
    {   // a graft runs on what it applies onto: that version's runtime
        const std::string Base = DefaultGraftBase(Idx, Launch->Key());
        return Base.empty() || Base == LaunchNodeId ? std::vector<std::string>{} : RunnerChainIds(Idx, Base, GlobalConfigJSON);
    }
    ContainerParams Cp(Launch->BundleDir);
    Cp.NodeIdx = &Idx; Cp.LaunchNodeId = LaunchNodeId; Cp.PackageUID = Launch->Uid;   // for the per-package runner pin lookup
    //Every ENTRYPOINT's chain: the entries may run on different platforms (a native and a win32 build), and a
    //downloaded game must be playable through whichever the user picks.
    std::vector<std::string> Ids;
    std::set<std::string> Seen;
    std::vector<std::string> Labels = Launch->EntrypointLabels();
    if (Labels.empty()) Labels.push_back(std::string());
    for (const std::string &Label : Labels)
    {
        Cp.Entrypoint = Label;
        for (const std::string &RunnerId : LaunchResolver::ResolveChainIds(Idx, *Launch, Cp, GlobalConfigJSON))
            if (RunnerId != LaunchResolver::kNativeTerminalId && Seen.insert(RunnerId).second) Ids.push_back(RunnerId);
    }
    return Ids;
}

void CollectRunnerChainTargets(const NodeIndex &Idx, const std::string &LaunchNodeId,
                               const nlohmann::ordered_json &GlobalConfigJSON, std::vector<IpfsWrapper::FetchTarget> &Out)
{
    // A downloaded game is only PLAYABLE once its runtime is present too — so a download pools the resolved runner
    // CHAIN's build content (e.g. a Minecraft version's java_<major> JRE, or a wine game's Proton build) into the same
    // fetch batch. The game's own closure is CollectContentTargets'; only the runners, which that walk skips, are added
    // here. A native / build-less runner contributes nothing; one whose build cannot be collected is on the record.
    for (const std::string &RunnerId : RunnerChainIds(Idx, LaunchNodeId, GlobalConfigJSON))
    {
        std::string E;
        if (!CollectRunnerNodeTargets(Idx, RunnerId, Out, &E))
            LogWarn("PackageCatalog::CollectRunnerChainTargets", "runner '" + RunnerId + "': " + E);
    }
}

bool Download(std::shared_ptr<const NodeIndex> Idx, const std::vector<std::string> &LaunchIds,
              const std::vector<std::string> &RunnerIds, const GraftChoice &Grafts,
              nlohmann::ordered_json Config, std::string *Error)
{
    const IpfsWrapper::ForegroundHold Hold;   // closures, installs, content: one download, no gap for walks
    std::string Err;
    const auto Fail = [&](std::string Why) { if (Error) *Error = std::move(Why); return false; };
    // Everything a download needs is in packages that have landed: a node a package contains comes with its own
    // package (a node has one home), never fetched into this one. A gap means that package is not here yet.
    {
        std::vector<std::string> All = LaunchIds;
        All.insert(All.end(), RunnerIds.begin(), RunnerIds.end());
        for (const std::string &Lid : LaunchIds)
            for (const std::string &Rid : RunnerChainIds(*Idx, Lid, Config)) All.push_back(Rid);
        for (const std::string &Id : All)
            if (const auto Missing = MissingFromClosure(*Idx, Id); !Missing.empty())
                return Fail("'" + Id + "' contains " + std::to_string(Missing.size()) + " node(s) from a package that has "
                            "not arrived (or is not shared with you), e.g. " + Missing.front());
    }
    // What it fetches, from an index: the selected launchables' content and each one's runner chain build, and the
    // runners asked for. Collected twice: first as a dry run over the received index, so a download that cannot
    // complete (content naming no source) fails BEFORE anything moves; then over the adopted packages' new home.
    // Moved first, a failed download left a package out of CATALOG and not installed: gone from both tabs.
    const auto Collect = [&](const NodeIndex &I, std::vector<IpfsWrapper::FetchTarget> &T) {
        for (const std::string &Lid : LaunchIds)
        {
            if (!CollectContentTargets(I, Lid, Grafts, T, &Err)) return false;
            CollectRunnerChainTargets(I, Lid, Config, T);
        }
        for (const std::string &Rid : RunnerIds)
            if (!CollectRunnerNodeTargets(I, Rid, T, &Err)) return false;
        return true;
    };
    if (std::vector<IpfsWrapper::FetchTarget> Dry; !Collect(*Idx, Dry)) return Fail(Err);
    // INSTALL = ADOPT: a received package (its nodes live under CATALOG) moves into LIBRARY/<lib>/<pkg> before a byte
    // of content is fetched, so the content lands in the library and the package is ours from here on — launchable,
    // its grafts offered, re-published with our library. Every package the closures reach: adopt, then re-index once.
    const std::set<std::filesystem::path> Adopt = PackagesToAdopt(*Idx, LaunchIds, RunnerIds, Config);
    for (const std::filesystem::path &D : Adopt)
        if (std::string AErr; !AdoptReceivedPackage(Config, D, nullptr, &AErr)) return Fail(AErr);
    if (!Adopt.empty()) Idx = FreshCatalogIndex(Config);
    // Pool EVERY fetch into ONE concurrent batch, so a game downloads TOGETHER with the runtime it needs (bounded by
    // MaxConcurrentDownloads; dedup handles a runner both resolved and asked for).
    std::vector<IpfsWrapper::FetchTarget> Targets;
    if (!Collect(*Idx, Targets)) return Fail(Err);
    Idx.reset();   // targets collected: the index is not held through the (long) fetch
    if (!IpfsWrapper::FetchTargetsConcurrent(Targets, &Err)) return Fail(Err);
    // Builds are now present locally → the runner is ready: its prefix assembles from the build at launch, so
    // fetching the build IS the install.
    LogSucc("PackageCatalog::Download", "Downloaded " + std::to_string(Targets.size()) + " file(s).");
    return true;
}

} // namespace PackageCatalog
