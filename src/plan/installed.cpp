#include "installed.h"
#include "builtinrunners.h"
#include "commonutils.h"
#include "contentfile.h"
#include "nodegraph.h"
#include "parallel.h"
#include "manifestmodel.h"
#include "runnerwrapper.h"
#include "varsubst.h"   // %KEY% substitution — resolve CustomVar-templated content-layer PATHs for hydration

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <set>

namespace Installed
{
using namespace ManifestModel;
using namespace Shelf;

std::vector<const Node *> RunnerBuildNodes(const NodeIndex &Idx, const std::string &RunnerNodeId)
{
    std::vector<const Node *> Out;
    for (const std::string &Id : ManifestModel::Closure(Idx, RunnerNodeId))
        if (const Node *C = Idx.Find(Id); C && !C->ContentLayers.empty()) Out.push_back(C);
    return Out;
}

bool RunnerBuildPresent(const NodeIndex &Idx, const std::string &RunnerNodeId)
{
    const Node *R = Idx.Find(RunnerNodeId);
    if (!R || !R->IsRunner()) return false;
    //Every build VFS layer hydrated locally (the runner's content closure). A runner that ships no build is "present".
    std::error_code Ec;
    for (const Node *C : RunnerBuildNodes(Idx, RunnerNodeId))
        for (const auto &L : C->ContentLayers)
        {
            //NOT IsVfsLayer: a prefix-assembly layer ("%DefaultPfxDir%") resolves to <bundle>/%DefaultPfxDir%,
            //which never exists on disk, so counting it here reported EVERY proton runner as not installed.
            if (!ManifestModel::IsRunnerBuildLayer(L)) continue;
            std::filesystem::path Local; std::string Cid;
            LayerLocator(L, C->BundleDir, Local, Cid);
            if (Local == C->BundleDir) continue;                                  // no PATH
            if (!std::filesystem::exists(Local, Ec)) return false;                // a build layer missing → not present
        }
    return true;
}

std::vector<const Node*> CompatibleRunners(const NodeIndex &Idx, const Node &Launch)
{
    std::vector<const Node*> Out;
    for (const auto &[Id, N] : Idx.Nodes)
    {
        (void)Id;
        if (!N.IsRunner() || N.HostPlatform != MachinePlatform()) continue;
        if (ServesPlatform(N, Launch.HostPlatform, Launch.BundleDir)) Out.push_back(&N);
    }
    return Out;   // no executable/install gate — these are runners that COULD run it once installed
}

bool RunnerInstalled(const NodeIndex &Idx, const std::string &RunnerNodeId)
{
    const Node *R = Idx.Find(RunnerNodeId);
    if (!R || !R->IsRunner()) return false;
    // Ships its own build (any VFS layer in its closure — local PATH or a remote CID) → must be imported (build
    // hydrated + DEFPREFIX). Otherwise it's a PATH runner → usable iff its executable resolves on this system.
    bool ShipsBuild = false;
    for (const std::string &Id : ManifestModel::Closure(Idx, R->Key()))
        if (const Node *N = Idx.Find(Id))
            for (const auto &L : N->ContentLayers) if (ManifestModel::IsRunnerBuildLayer(L)) { ShipsBuild = true; break; }
    if (ShipsBuild)
        return RunnerBuildPresent(Idx, RunnerNodeId);
    return RunnerWrapper::ExecutableAvailable(R->Exec);
}

bool IsEmbeddedRunner(const NodeIndex &Idx, const std::string &RunnerNodeId)
{
    const Node *R = Idx.Find(RunnerNodeId);
    if (!R || !R->IsRunner()) return false;
    for (const auto &[Id, N] : Idx.Nodes)
    {
        (void)Id;
        if (N.IsVariant() && N.BundleDir == R->BundleDir) return true;   // a game shares its bundle → embedded
    }
    return false;
}

std::vector<const Node*> UsableRunners(const NodeIndex &Idx, const Node &Launch)
{
    std::vector<const Node*> Out;
    for (const Node *R : CompatibleRunners(Idx, Launch))
        if (RunnerInstalled(Idx, R->NodeId)) Out.push_back(R);
    return Out;
}

bool HasUsableChain(const NodeIndex &Idx, const Node &Launch)
{
    //Installed runners only, walked from the launch's platform toward this machine's (the resolver's BFS edges): a
    //Windows-only engine counts — it runs under proton.
    const std::string Machine = MachinePlatform();
    std::set<std::string> Seen = { Launch.HostPlatform };
    std::deque<std::string> Todo = { Launch.HostPlatform };
    while (!Todo.empty())
    {
        const std::string P = Todo.front(); Todo.pop_front();
        if (P == Machine) return true;
        for (const auto &[Id, N] : Idx.Nodes)
            if (N.IsRunner() && ServesPlatform(N, P, Launch.BundleDir) && RunnerInstalled(Idx, N.Key())
                && Seen.insert(N.HostPlatform).second)
                Todo.push_back(N.HostPlatform);
    }
    return false;
}

std::vector<const Node*> CandidateRunners(const NodeIndex &Idx, const std::string &InputPlatform, const std::filesystem::path &LaunchBundleDir)
{
    //Installed runners that can consume InputPlatform (GUEST ∋ InputPlatform), regardless of HOST — a chain step's
    //choices. Unlike UsableRunners, HOST may be an intermediate platform (e.g. snes9x: snes→win32). Sorted by node id.
    std::vector<const Node*> Out;
    for (const auto &[Id, N] : Idx.Nodes)
    {
        (void)Id;
        if (!N.IsRunner()) continue;
        if (ServesPlatform(N, InputPlatform, LaunchBundleDir) && RunnerInstalled(Idx, N.Key())) Out.push_back(&N);
    }
    return Out;   // std::map iteration = sorted by node id
}

//Invoke Fn for every VFS layer in a launchable's content closure (runner build excluded), with its resolved local
//path + ipfs CID (resolved against the OWNING node's bundle dir — cross-bundle-correct).
void ForEachContentLayer(const NodeIndex &Idx, const std::string &LaunchNodeId,
    const GraftChoice &Grafts,
    const std::function<void(const nlohmann::ordered_json&, const std::filesystem::path&, const std::string&)> &Fn,
    bool WithGrafts, bool WithBase)
{
    //The launchable's whole closure (options included: a download is whole chains) — and, for the FETCH pool only
    //(WithGrafts), the chosen grafts' closures: a ticked mod's content is part of what the launch mounts. A dehydrate or
    //a hydration verdict never walks grafts: a graft is shared across every variant of its title.
    const Node *LN = Idx.Find(LaunchNodeId);
    const std::string LaunchKey = LN ? LN->Key() : LaunchNodeId;   // walk by the index key, never by a label that may repeat
    std::vector<std::string> Order = ManifestModel::Closure(Idx, LaunchKey);
    //A graft that is a version runs on a base it only names through ANY: what it mounts is the base's closure too.
    //(Not for a dehydrate: the base is a version of its own, with its own card.)
    if (const std::string Base = WithBase ? DefaultGraftBase(Idx, LaunchKey) : std::string(); !Base.empty())
    {
        std::vector<std::string> BaseFirst = ManifestModel::Closure(Idx, Base);
        std::set<std::string> Seen(BaseFirst.begin(), BaseFirst.end());
        for (const std::string &Id : Order) if (Seen.insert(Id).second) BaseFirst.push_back(Id);
        Order = std::move(BaseFirst);
    }
    if (WithGrafts && LN)
    {
        const std::vector<std::string> Chosen = AppliedGrafts(Idx, LaunchKey, Grafts);
        std::set<std::string> Seen(Order.begin(), Order.end());
        for (const std::string &G : Chosen)
            if (const Node *Gn = Idx.Find(G); Gn && !Gn->Received && !Gn->BundleDir.empty())
                for (const std::string &Id : ManifestModel::Closure(Idx, Gn->Key()))
                    if (Seen.insert(Id).second) Order.push_back(Id);
    }
    for (const std::string &Id : Order)
    {
        const Node *N = Idx.Find(Id);
        if (!N || !N->ContentLayers.is_array()) continue;
        for (const auto &L : N->ContentLayers)
        {
            if (!IsVfsLayer(LayerType(L))) continue;
            std::filesystem::path Local; std::string Cid;
            LayerLocator(L, N->BundleDir, Local, Cid);
            if (Local == N->BundleDir) continue;                                 // no PATH
            //A node's content lives in its own package dir: anything else (a peer's absolute or ".." name the
            //vocabulary would already refuse) is never read, sized or — above all — fetched INTO.
            if (!IsRuntimeSourcedLayer(L) && !NodeGraph::PathWithin(N->BundleDir, Local))
            {
                LogWarn("PackageCatalog::ForEachContentLayer", "node '" + N->NodeId + "': content " + Local.string()
                        + " lies outside its package dir " + N->BundleDir.string() + " — refused");
                continue;
            }
            Fn(L, Local, Cid);
        }
    }
}

// The global CustomVar default namespace (KEY -> DEFAULT) across the whole node graph. CustomVars resolve in ONE
// global namespace (see LaunchResolver::ResolveCustomVariables), so a single map suffices to substitute the %KEY%
// tokens that appear in content-layer PATHs — e.g. a shared library node whose PATH is "%dgv_zip%". Later (more
// specific) declarations win, matching the launch resolver's hierarchy; for hydration only the file identity matters.
static std::map<std::string, std::string> CustomVarDefaults(const NodeIndex &Idx)
{
    std::map<std::string, std::string> Vars;
    for (const auto &[Id, N] : Idx.Nodes)
    {
        (void)Id;
        for (const auto &[Key, Default] : N.VarDefaults) Vars[Key] = Default;
    }
    return Vars;
}

// How a content layer's PATH resolves against the local filesystem — the path-only half of its fetch state (the CID
// half is applied at the call site, since it decides fetchable-vs-broken). A PATH may be CustomVar-templated
// ("%dgv_zip%" -> a real zip); substitute the global var namespace first. A path that STILL carries a %token% after
// substitution is RUNTIME-sourced (a runner's "%RunnerMount%/..." prefix mount) — it resolves to a live mount at
// launch, not to on-disk content (mirrors LaunchSources' IsRuntimeSourcedLayer skip).
enum class PathState : std::uint8_t { Runtime, Present, Missing };

// Runtime-sourced is a property of the LAYER, and it is decided by the ONE predicate — never re-derived from
// the resolved absolute path. Deriving it from the path was wrong twice over:
//   * a library root containing a '%' segment (a folder literally named "My %Games%") made EVERY layer under
//     it classify as Runtime, so the whole library silently reported hydrated; and
//   * a URL-escaped filename ("100%25%20done.zip"), which the predicate deliberately treats as real content,
//     fell through to substitute-and-see, where the undefined %25% survived and again produced Runtime — so a
//     fetchable-missing layer was invisible to the download button and the game launched against a hole.
// A path whose tokens are ALL defined is CustomVar-templated ("%dgv_zip%" -> a real zip) and does resolve;
// one with any undefined token is a live runtime mount ("%RunnerMount%/...") and has no on-disk file at all.
// Asked this way rather than by substituting, because substitution LOGS a warning per undefined token and
// here an undefined token is the expected answer, not a problem.
static PathState ResolvePathState(const nlohmann::ordered_json &Layer, const std::filesystem::path &Local,
                                  const std::map<std::string, std::string> &Vars)
{
    const std::vector<std::string> Tokens = VarSubst::PathTokens(ManifestModel::LayerPathString(Layer));
    for (const std::string &T : Tokens) if (!Vars.count(T)) return PathState::Runtime;

    std::string S = Local.string();
    if (!Tokens.empty()) VarSubst::StringVariableSubstitution(S, Vars);   // every token IS defined
    std::error_code Ec;
    return std::filesystem::exists(S, Ec) ? PathState::Present : PathState::Missing;
}

// HYDRATION = "everything fetchable has been fetched" (NOT "every backing file exists"). A content layer is un-hydrated
// ONLY when it is FETCHABLE-MISSING: its backing file is absent AND it carries a remote source (CID) to fetch it from.
// A missing local-only layer (no CID) is not un-hydrated — there is nothing to fetch; it is a broken/incomplete package,
// which --validate-nodes owns, not the download button. A runtime-sourced %path% has no on-disk file at all.
// A RECEIVED node is a friend's: a file of it that is not here is not installed, CID or not (a friend's node naming
// no CID is one nobody can fetch — never one this machine has).
static bool LayerFetchableMissing(PathState PS, const std::string &Cid)
{
    return PS == PathState::Missing && !Cid.empty();
}

bool NodeHydrated(const NodeIndex &Idx, const std::string &LaunchNodeId)
{
    if (!Idx.Find(LaunchNodeId)) return false;
    const std::map<std::string, std::string> Vars = CustomVarDefaults(Idx);
    bool AllFetched = true;
    ForEachContentLayer(Idx, LaunchNodeId, {}, [&](const nlohmann::ordered_json&L, const std::filesystem::path &Local, const std::string &Cid){
        if (LayerFetchableMissing(ResolvePathState(L, Local, Vars), Cid)) AllFetched = false;
    });
    return AllFetched;
}

bool NodeHasContent(const NodeIndex &Idx, const std::string &LaunchNodeId)
{
    // True iff the node's closure defines at least one VFS content layer. Distinguishes a real (downloadable) game
    // from a content-less/malformed node, which is VACUOUSLY "hydrated" (no layers → nothing missing) and would
    // otherwise show up in the Library + Installed Packages with nothing to launch.
    bool Has = false;
    ForEachContentLayer(Idx, LaunchNodeId, {}, [&](const nlohmann::ordered_json&, const std::filesystem::path&, const std::string&){ Has = true; });
    return Has;
}

std::unordered_map<std::string, NodeHydration> HydrationMap(const NodeIndex &Idx)
{
    std::unordered_map<std::string, NodeHydration> Memo;
    const std::map<std::string, std::string> Vars = CustomVarDefaults(Idx);   // resolve %dgv_zip% etc. before stat'ing
    //Every content file's state first, each stat'd once and all of them at once (a library is thousands of files on
    //what may be a busy disk); keyed by the local path and the layer's PATH text, which together decide the answer.
    const auto StatKey = [](const nlohmann::ordered_json &L, const std::filesystem::path &Local) {
        return Local.string() + '\x1f' + LayerPathString(L);
    };
    std::unordered_map<std::string, PathState> StatCache;
    {
        std::vector<std::pair<const nlohmann::ordered_json *, std::filesystem::path>> Todo;
        for (const auto &[Id, N] : Idx.Nodes)
        {
            (void)Id;
            for (const auto &L : N.ContentLayers)
            {
                if (!L.is_object() || !IsVfsLayer(LayerType(L))) continue;
                std::filesystem::path Local; std::string Cid;
                LayerLocator(L, N.BundleDir, Local, Cid);
                if (StatCache.emplace(StatKey(L, Local), PathState::Missing).second) Todo.emplace_back(&L, std::move(Local));
            }
        }
        std::vector<PathState> States(Todo.size());
        Parallel::For(Todo.size(), [&](std::size_t I) { States[I] = ResolvePathState(*Todo[I].first, Todo[I].second, Vars); });
        for (std::size_t I = 0; I < Todo.size(); ++I) StatCache[StatKey(*Todo[I].first, Todo[I].second)] = States[I];
    }
    std::function<NodeHydration(const std::string &)> Compute = [&](const std::string &Id) -> NodeHydration {
        auto It = Memo.find(Id);
        if (It != Memo.end()) return It->second;
        Memo[Id] = {true, false};                                    // cycle guard (graph is a DAG; validation checks this)
        NodeHydration R;                                             // {Hydrated=true, HasContent=false}
        const Node *N = Idx.Find(Id);
        // Own content layers.
        if (N && N->ContentLayers.is_array())
            for (const auto &L : N->ContentLayers)
            {
                if (!L.is_object() || !IsVfsLayer(LayerType(L))) continue;
                R.HasContent = true;
                std::filesystem::path Local; std::string Cid;
                LayerLocator(L, N->BundleDir, Local, Cid);
                //Stat'd above — unless the path came out otherwise this time (a node file landed meanwhile, claiming the
                //same content name): then stat it now, as every lookup once did.
                const auto Sc = StatCache.find(StatKey(L, Local));
                const PathState PS = Sc != StatCache.end() ? Sc->second : ResolvePathState(L, Local, Vars);
                if (LayerFetchableMissing(PS, Cid) || (N->Received && PS == PathState::Missing)) R.Hydrated = false;
                if (PS != PathState::Missing) R.OnDisk = true;
                if (!Cid.empty()) R.HasCid = true;
            }
        // Fold in the parents' (already-memoized) closure result. A parent MISSING from the index means the closure
        // itself isn't fetched (a received share before install, or a broken ref) — that can never count as hydrated:
        // "hydrated" previously held VACUOUSLY for such nodes (no reachable content layers → nothing missing), which
        // filed un-installed received games under the Library tab as playable and left the Catalog empty.
        if (N)
            for (const std::string &P : N->Refs)
            {
                if (!Idx.Find(P)) { R.Hydrated = false; R.Incomplete = true; continue; }
                const NodeHydration PC = Compute(P);
                R.Hydrated    = R.Hydrated && PC.Hydrated;
                R.HasContent  = R.HasContent || PC.HasContent;
                R.OnDisk      = R.OnDisk || PC.OnDisk;
                R.HasCid      = R.HasCid || PC.HasCid;
                R.Incomplete  = R.Incomplete || PC.Incomplete;
            }
        Memo[Id] = R;
        return R;
    };
    for (const auto &[Id, N] : Idx.Nodes) { (void)N; Compute(Id); }
    //A graft that is a version is installed when the base it runs on is too (bases are never grafts: no chaining).
    for (const auto &[Id, Bases] : ManifestModel::AllGraftBases(Idx))
        if (const std::string Base = Bases.empty() ? std::string() : Bases.front(); !Base.empty())
        {
            const NodeHydration B = Memo[Base];
            Memo[Id].Hydrated   = Memo[Id].Hydrated && B.Hydrated;
            Memo[Id].HasContent = Memo[Id].HasContent || B.HasContent;
            Memo[Id].OnDisk     = Memo[Id].OnDisk || B.OnDisk;
            Memo[Id].HasCid     = Memo[Id].HasCid || B.HasCid;
            Memo[Id].Incomplete = Memo[Id].Incomplete || B.Incomplete;
        }
    return Memo;
}

} // namespace Installed
