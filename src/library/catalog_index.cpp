// The library index: built from the working tree (frozen to CIDs), cached on disk by its inputs, and shared.
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

// ---- the index cache ----
// The last full index built for a data dir, kept beside it with what it was built from: this program, the machine,
// the roots in scan order, and every node file in them with its size and modification time. A build whose inputs are
// the same loads it — one file instead of thousands, and none of the freeze and fold — and says again what the build
// that made it logged. Any difference (a node edited, added, removed; another binary) builds afresh and replaces it.
namespace
{
namespace fs = std::filesystem;

const std::set<std::string> kIndexLogContexts = {
    "NodeGraph::GatherWorkingTree", "NodeGraph::ReadTreeJsonBounded", "NodeGraph::TopoOrderForMint",
    "NodeGraph::FreezeToIndex", "ManifestModel::ParseNode", "PackageCatalog::BuildCatalogIndex" };

fs::path IndexCachePath() { return AppPaths::DataRoot() / "CACHE" / "index.cbor"; }

//The start of an index's inputs: the shape of the cache, then the version that wrote it. The last index
//(LastCatalogIndex) is read whatever version wrote it — shown until the check, which sees the program changed,
//replaces it — but never one of another shape.
const std::string kIndexShape = "vgindex 2 ";
std::string IndexFormat() { return kIndexShape + VIDYAGOD_VERSION + "\n"; }
const std::string kHydrationShape = "vghydration 1";

std::string IndexInputs(const nlohmann::ordered_json &Config, long long *Newest = nullptr)
{
    //The program: an AppImage's own file (its mounted binary sits at a new /tmp path every run), else the binary.
    const char *AppImage = std::getenv("APPIMAGE");
    const fs::path Exe = AppImage && *AppImage ? fs::path(AppImage) : Platform::SelfExe();
    std::string S = IndexFormat() + Exe.string() + '\t' + Platform::FileStamp(Exe) + '\n'
                    + ManifestModel::MachinePlatform() + '\n';
    std::vector<fs::path> Roots = { LibraryRootDir(Config), CatalogRootDir(Config) };
    for (const auto &D : LocalPackageDirs(Config)) Roots.push_back(D);
    for (const fs::path &Root : Roots)
    {
        S += "root\t" + Root.string() + '\n';
        const std::vector<fs::path> Files = NodeGraph::ListTreeFiles(Root);
        std::vector<std::string> Lines(Files.size());
        std::vector<long long> Times(Files.size(), 0);
        Parallel::For(Files.size(), [&](std::size_t I) { Lines[I] = Files[I].string() + '\t' + Platform::FileStamp(Files[I], &Times[I]) + '\n'; });
        for (const auto &L : Lines) S += L;
        if (Newest) for (const long long T : Times) *Newest = std::max(*Newest, T);
    }
    return S;
}

//Want: the inputs it must have been built from; null takes whatever it was built from (of this format), into *Had.
std::optional<NodeIndex> LoadIndexCache(const std::string *Want, std::vector<Diagnostics::Entry> &Said, std::string *Had = nullptr)
{
    if (!AppPaths::DataRootSet()) return std::nullopt;   // no data dir of its own: no cache (see SaveIndexCache)
    try
    {
        std::ifstream In(IndexCachePath(), std::ios::binary | std::ios::ate);
        if (!In) return std::nullopt;
        std::vector<std::uint8_t> Bytes(static_cast<std::size_t>(In.tellg()));
        In.seekg(0);
        if (!In.read(reinterpret_cast<char *>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()))) return std::nullopt;
        nlohmann::ordered_json H = nlohmann::ordered_json::from_cbor(Bytes, true, false);
        if (H.is_discarded() || !H.is_object()) return std::nullopt;
        const std::string Inputs = H.value("inputs", std::string());
        if (Want ? Inputs != *Want : Inputs.rfind(kIndexShape, 0) != 0) return std::nullopt;
        if (Had) *Had = Inputs;
        NodeIndex Idx;
        auto &Blobs = H.at("nodes");
        std::vector<std::pair<std::string, Node>> Nodes(Blobs.size());
        Parallel::For(Blobs.size(), [&](std::size_t I) {
            nlohmann::ordered_json J = nlohmann::ordered_json::from_cbor(Blobs[I].get_binary(), true, true);
            Nodes[I].first = J.at("key").get<std::string>();
            Nodes[I].second = ManifestModel::NodeFromJson(std::move(J));
        });
        for (auto &[K, N] : Nodes) Idx.Nodes.emplace_hint(Idx.Nodes.end(), std::move(K), std::move(N));
        ManifestModel::IndexTablesFromJson(H.at("tables"), Idx);
        Said.clear();
        for (const auto &E : H.at("log")) Said.push_back({static_cast<LogLevel>(E.at(0).get<int>()), E.at(1).get<std::string>(), E.at(2).get<std::string>()});
        LogReplay(Said);   // what the build that made it said, said again
        return Idx;
    }
    catch (const std::exception &) { return std::nullopt; }   // a cache of another shape, or damaged: built afresh, then replaced
}

void SaveIndexCache(const std::string &Inputs, const NodeIndex &Idx, const std::vector<Diagnostics::Entry> &Said)
{
    if (!AppPaths::DataRootSet()) return;   // a test or tool that set no data dir never writes into ~/.VidyaGod
    std::vector<const std::pair<const std::string, Node> *> Items;
    for (const auto &KV : Idx.Nodes) Items.push_back(&KV);
    std::vector<nlohmann::ordered_json> Blobs(Items.size());
    Parallel::For(Items.size(), [&](std::size_t I) {
        Blobs[I] = nlohmann::ordered_json::binary(nlohmann::ordered_json::to_cbor(ManifestModel::NodeToJson(Items[I]->second)));
    });
    nlohmann::ordered_json Log = nlohmann::ordered_json::array();
    for (const auto &E : Said) Log.push_back(nlohmann::ordered_json::array({static_cast<int>(E.Level), E.Context, E.Message}));
    nlohmann::ordered_json H = {{"inputs", Inputs}, {"tables", ManifestModel::IndexTablesToJson(Idx)}, {"log", Log}};
    H["nodes"] = std::move(Blobs);
    const std::vector<std::uint8_t> Bytes = nlohmann::ordered_json::to_cbor(H);
    //One writer at a time in this process, each into a temp of its own (another process has another pid), renamed
    //over the cache whole: a reader sees the old cache or the new one, never part of one.
    static std::mutex SaveMu;
    std::lock_guard<std::mutex> Lk(SaveMu);
    std::error_code Ec;
    const fs::path Path = IndexCachePath();
    fs::create_directories(Path.parent_path(), Ec);
    //A temp a process left when it died mid-write (an hour old: no live writer takes that long) is removed.
    for (fs::directory_iterator It(Path.parent_path(), Ec), End; !Ec && It != End; It.increment(Ec))
        if (It->path().filename().string().rfind(Path.filename().string() + ".tmp", 0) == 0
            && fs::last_write_time(It->path(), Ec) < fs::file_time_type::clock::now() - std::chrono::hours(1))
            fs::remove(It->path(), Ec);
    Ec.clear();
    const fs::path Tmp = Path.string() + ".tmp" + std::to_string(QCoreApplication::applicationPid());
    {
        std::ofstream Out(Tmp, std::ios::binary | std::ios::trunc);
        if (!Out) return;
        Out.write(reinterpret_cast<const char *>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
        if (!Out) { Out.close(); fs::remove(Tmp, Ec); return; }
    }
    fs::rename(Tmp, Path, Ec);
    if (Ec) fs::remove(Tmp, Ec);
}

} // namespace

namespace
{
//BuildCatalogIndex once its inputs are known (Newest: the latest node file's time among them).
NodeIndex BuildFrom(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &Inputs, long long Newest,
                    std::chrono::steady_clock::time_point T0, std::vector<Diagnostics::Entry> *SaidOut)
{
    std::vector<Diagnostics::Entry> Replayed;
    if (std::optional<NodeIndex> Cached = LoadIndexCache(&Inputs, Replayed))
    {
        if (SaidOut) *SaidOut = std::move(Replayed);
        LogOut("PackageCatalog::BuildCatalogIndex", "Indexed " + std::to_string(Cached->Nodes.size()) + " node(s) from "
               + LibraryRootDir(GlobalConfigJSON) + " in " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - T0).count()) + " ms (the cached index: no node file changed)");
        return std::move(*Cached);
    }
    LogRecorder Said(kIndexLogContexts);
    NodeIndex Idx = ScanCatalogIndex(GlobalConfigJSON);
    std::vector<Diagnostics::Entry> Lines = Said.Take();
    //A node file written in the last few seconds could be written again within the same tick of its drive's clock
    //(two seconds on FAT) at the same size, and look unchanged: such a build is not kept (git's "racy" files).
    const long long Now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (Newest < Now - 3) SaveIndexCache(Inputs, Idx, Lines);
    if (SaidOut) *SaidOut = std::move(Lines);
    return Idx;
}

fs::path HydrationCachePath() { return AppPaths::DataRoot() / "CACHE" / "hydration.cbor"; }
} // namespace

NodeIndex BuildCatalogIndex(const nlohmann::ordered_json &GlobalConfigJSON, std::string *InputsOut, std::vector<Diagnostics::Entry> *SaidOut)
{
    const auto T0 = std::chrono::steady_clock::now();
    long long Newest = 0;
    const std::string Inputs = IndexInputs(GlobalConfigJSON, &Newest);
    if (InputsOut) *InputsOut = Inputs;
    return BuildFrom(GlobalConfigJSON, Inputs, Newest, T0, SaidOut);
}

std::optional<NodeIndex> LastCatalogIndex(std::string *Inputs, std::vector<Diagnostics::Entry> *SaidOut)
{
    const auto T0 = std::chrono::steady_clock::now();
    std::vector<Diagnostics::Entry> Replayed;
    std::optional<NodeIndex> Idx = LoadIndexCache(nullptr, Replayed, Inputs);
    if (!Idx) return std::nullopt;
    if (SaidOut) *SaidOut = std::move(Replayed);
    LogOut("PackageCatalog::LastCatalogIndex", "Loaded the last index (" + std::to_string(Idx->Nodes.size()) + " node(s)) in "
           + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - T0).count())
           + " ms; the library is checked against it next");
    return Idx;
}

std::optional<NodeIndex> RefreshCatalogIndex(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &Have,
                                             std::string *InputsOut, std::vector<Diagnostics::Entry> *SaidOut)
{
    const auto T0 = std::chrono::steady_clock::now();
    long long Newest = 0;
    const std::string Inputs = IndexInputs(GlobalConfigJSON, &Newest);
    if (InputsOut) *InputsOut = Inputs;
    if (Inputs == Have) return std::nullopt;
    return BuildFrom(GlobalConfigJSON, Inputs, Newest, T0, SaidOut);
}

std::optional<std::unordered_map<std::string, NodeHydration>> LastHydration()
{
    if (!AppPaths::DataRootSet()) return std::nullopt;
    try
    {
        std::ifstream In(HydrationCachePath(), std::ios::binary | std::ios::ate);
        if (!In) return std::nullopt;
        std::vector<std::uint8_t> Bytes(static_cast<std::size_t>(In.tellg()));
        In.seekg(0);
        if (!In.read(reinterpret_cast<char *>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()))) return std::nullopt;
        const nlohmann::ordered_json H = nlohmann::ordered_json::from_cbor(Bytes, true, false);
        if (H.is_discarded() || !H.is_object() || H.value("format", std::string()) != kHydrationShape) return std::nullopt;
        std::unordered_map<std::string, NodeHydration> Out;
        for (const auto &[Key, V] : H.at("nodes").items())
            Out[Key] = NodeHydration{ V.at(0).get<bool>(), V.at(1).get<bool>(), V.at(2).get<bool>(), V.at(3).get<bool>(), V.at(4).get<bool>() };
        return Out;
    }
    catch (const std::exception &) { return std::nullopt; }
}

void SaveLastHydration(const std::unordered_map<std::string, NodeHydration> &Hyd)
{
    if (!AppPaths::DataRootSet()) return;   // a test or tool that set no data dir never writes into ~/.VidyaGod
    nlohmann::ordered_json Nodes = nlohmann::ordered_json::object();
    for (const auto &[Key, H] : Hyd) Nodes[Key] = { H.Hydrated, H.HasContent, H.OnDisk, H.HasCid, H.Incomplete };
    const std::vector<std::uint8_t> Bytes = nlohmann::ordered_json::to_cbor(nlohmann::ordered_json{{"format", kHydrationShape}, {"nodes", std::move(Nodes)}});
    static std::mutex SaveMu;
    std::lock_guard<std::mutex> Lk(SaveMu);
    std::error_code Ec;
    const fs::path Path = HydrationCachePath();
    fs::create_directories(Path.parent_path(), Ec);
    const fs::path Tmp = Path.string() + ".tmp" + std::to_string(QCoreApplication::applicationPid());
    {
        std::ofstream Out(Tmp, std::ios::binary | std::ios::trunc);
        if (!Out) return;
        Out.write(reinterpret_cast<const char *>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
        if (!Out) { Out.close(); fs::remove(Tmp, Ec); return; }
    }
    fs::rename(Tmp, Path, Ec);
    if (Ec) fs::remove(Tmp, Ec);
}

namespace
{
std::mutex g_AppIndexMu;
std::shared_ptr<const NodeIndex> g_AppIndex;
std::string g_AppIndexInputs;
std::vector<Diagnostics::Entry> g_AppIndexSaid;
}

IndexCacheState IndexCacheStatus(const nlohmann::ordered_json &GlobalConfigJSON)
{
    IndexCacheState St;
    const fs::path P = IndexCachePath();
    St.Path = P.string();
    std::error_code Ec;
    St.Exists = fs::exists(P, Ec);
    if (St.Exists) St.Bytes = fs::file_size(P, Ec);
    const std::string Inputs = IndexInputs(GlobalConfigJSON);
    St.Files = static_cast<std::size_t>(std::count(Inputs.begin(), Inputs.end(), '\n'));
    if (std::ifstream In(P, std::ios::binary | std::ios::ate); St.Exists && In)   // gone meanwhile: not current
    {
        const std::streamoff Size = In.tellg();
        std::vector<std::uint8_t> Bytes(Size > 0 ? static_cast<std::size_t>(Size) : 0);
        In.seekg(0);
        if (In.read(reinterpret_cast<char *>(Bytes.data()), static_cast<std::streamsize>(Bytes.size())))
        {
            const nlohmann::ordered_json H = nlohmann::ordered_json::from_cbor(Bytes, true, false);
            St.Current = !H.is_discarded() && H.is_object() && H.value("inputs", std::string()) == Inputs;
        }
    }
    return St;
}

std::optional<NodeIndex> ReadIndexCache(const nlohmann::ordered_json &GlobalConfigJSON)
{
    std::vector<Diagnostics::Entry> Said;
    LogBuffer Quiet;   // a read for comparison: what its build said is not said again here
    const std::string Inputs = IndexInputs(GlobalConfigJSON);
    std::optional<NodeIndex> Idx = LoadIndexCache(&Inputs, Said);
    (void)Quiet.Take();
    return Idx;
}

bool ClearIndexCache()
{
    std::error_code Ec, Hc;
    fs::remove(IndexCachePath(), Ec);
    fs::remove(HydrationCachePath(), Hc);
    return !Ec && !Hc;
}

void PublishAppIndex(std::shared_ptr<const NodeIndex> Idx, std::string Inputs, std::vector<Diagnostics::Entry> Said)
{
    std::lock_guard<std::mutex> Lk(g_AppIndexMu);
    g_AppIndex = std::move(Idx);
    g_AppIndexInputs = std::move(Inputs);
    g_AppIndexSaid = std::move(Said);
}

std::shared_ptr<const NodeIndex> CurrentIndex(const nlohmann::ordered_json &GlobalConfigJSON)
{
    std::shared_ptr<const NodeIndex> App;
    std::string AppInputs;
    std::vector<Diagnostics::Entry> AppSaid;
    { std::lock_guard<std::mutex> Lk(g_AppIndexMu); App = g_AppIndex; AppInputs = g_AppIndexInputs; AppSaid = g_AppIndexSaid; }
    if (App && IndexInputs(GlobalConfigJSON) == AppInputs) { LogReplay(AppSaid); return App; }
    return std::make_shared<const NodeIndex>(BuildCatalogIndex(GlobalConfigJSON));
}

NodeIndex ScanCatalogIndex(const nlohmann::ordered_json &GlobalConfigJSON)
{
    using Clock = std::chrono::steady_clock;
    const auto Ms = [](Clock::time_point A, Clock::time_point B) { return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(B - A).count()); };
    const auto T0 = Clock::now();
    std::map<std::string, nlohmann::ordered_json> Tree;
    std::map<std::string, std::filesystem::path>  Dirs;
    NodeGraph::GatherWorkingTree(LibraryRootDir(GlobalConfigJSON), Tree, Dirs);   // OUR nodes: stored CID handle trusted
    // Received browse stubs (a friend controls their bytes): gather with TrustStoredCid=FALSE so a crafted "CID" can't
    // hijack a local handle or a local node's external-dep ref, or forge an "authored" handle to steal a BundleDir.
    // They still merge in and resolve by their DERIVED CID in the frozen index (browse tiles link exec→tile fine).
    NodeGraph::GatherWorkingTree(CatalogRootDir(GlobalConfigJSON), Tree, Dirs, /*SkipReserved=*/false, /*TrustStoredCid=*/false);
    for (const auto &D : LocalPackageDirs(GlobalConfigJSON))   // externally-added bundles that live OUTSIDE LIBRARY
        NodeGraph::GatherWorkingTree(D, Tree, Dirs);
    //Runners every install has (a Windows game on Windows runs as itself): nothing to download for them.
    BuiltinRunners::AddTo(Tree, Dirs, LibraryRootDir(GlobalConfigJSON), ManifestModel::MachinePlatform());
    const auto T1 = Clock::now();
    NodeIndex Idx = NodeGraph::FreezeToIndex(Tree, Dirs);
    if (Idx.Nodes.empty() && !Tree.empty())
        LogErr("PackageCatalog::BuildCatalogIndex", "no node of " + std::to_string(Tree.size()) + " froze (the log above says why)");
    else
        LogOut("PackageCatalog::BuildCatalogIndex", "Indexed " + std::to_string(Idx.Nodes.size())
               + " node(s) by CID from " + LibraryRootDir(GlobalConfigJSON) + " in " + Ms(T0, Clock::now()) + " ms (read "
               + Ms(T0, T1) + ", freeze and facts " + Ms(T1, Clock::now()) + ")");
    //A node that lives under CATALOG is a RECEIVED stub: a friend's bytes, browsable and downloadable, never a
    //graft candidate (it would mount un-hydrated). Decided by where its file lives — the only fact that
    //distinguishes it, since the frozen index is one flat graph.
    //A node that is ALSO installed (a friend's stub of a package we have) is ours: it mounts and seeds from our copy.
    const std::filesystem::path Cat = std::filesystem::path(CatalogRootDir(GlobalConfigJSON)).lexically_normal();
    auto UnderCatalog = [&](const std::filesystem::path &Dir) {
        const std::filesystem::path D = Dir.lexically_normal();
        auto [CatEnd, DEnd] = std::mismatch(Cat.begin(), Cat.end(), D.begin(), D.end());
        (void)DEnd;
        return !Cat.empty() && CatEnd == Cat.end();
    };
    //A friend's catalog stub of a package that is installed here is a view of it, not a second home: the node reads
    //from the installed package. What is left in Duplicates after that is two installed homes — the validator's.
    for (auto &[Id, D] : Idx.Duplicates)
    {
        Node &N = Idx.Nodes.at(Id);
        if (UnderCatalog(N.BundleDir))
            if (const auto It = std::find_if(D.begin(), D.end(), [&](const auto &P) { return !UnderCatalog(P); }); It != D.end())
            {
                std::swap(N.BundleDir, *It);
                std::error_code Fe;
                if (const auto F = N.BundleDir / (Id + ".json"); std::filesystem::exists(F, Fe)) N.File = F;   // its file there too
            }
        D.erase(std::remove_if(D.begin(), D.end(), UnderCatalog), D.end());
    }
    for (auto It = Idx.Duplicates.begin(); It != Idx.Duplicates.end();) It = It->second.empty() ? Idx.Duplicates.erase(It) : std::next(It);
    for (auto &[Id, N] : Idx.Nodes)
    {
        (void)Id;
        N.Received = UnderCatalog(N.BundleDir);
    }

    return Idx;
}

std::shared_ptr<const NodeIndex> FreshCatalogIndex(const nlohmann::ordered_json &GlobalConfigJSON)
{
    // Single flight: a build serves every caller that asked before it STARTED (it scans the disk as it is now, after
    // their changes); a caller arriving mid-build waits for the next one.
    static std::mutex Mu;
    static std::condition_variable Cv;
    static std::uint64_t Asked = 0, Served = 0;
    static bool Building = false;
    static int Callers = 0;                                             // in here now; the last one out lets go of Latest
    static std::shared_ptr<const NodeIndex> Latest;
    std::unique_lock<std::mutex> Lk(Mu);
    const std::uint64_t Mine = ++Asked;
    ++Callers;
    for (;;)
    {
        if (Served >= Mine)
        {
            std::shared_ptr<const NodeIndex> Got = Latest;
            if (--Callers == 0) Latest.reset();                         // no 17 MB copy kept past its last reader
            return Got;
        }
        if (!Building)
        {
            Building = true;
            const std::uint64_t Covers = Asked;
            Lk.unlock();
            std::shared_ptr<const NodeIndex> Built;
            try { Built = std::make_shared<const NodeIndex>(BuildCatalogIndex(GlobalConfigJSON)); }
            catch (...) { Lk.lock(); Building = false; --Callers; Cv.notify_all(); throw; }   // the next caller builds
            Lk.lock();
            Latest = std::move(Built);
            Served = Covers;
            Building = false;
            Cv.notify_all();
            continue;
        }
        Cv.wait(Lk);
    }
}


} // namespace PackageCatalog
