#include "manifestmodel.h"
#include "commonutils.h"
#include "fold.h"        // Fold::Resolve — a node's facts are folded, never inherited by graph distance
#include "nodelower.h"   // NodeLower — the gen-6 vocabulary check and the lowering to engine ops
#include "varsubst.h"    // VarSubst::ConditionParses — WHEN syntax lint
#include "launchparams.h"   // ContainerParams::GetVariablesMap — the single source of truth for built-in %variables%
#include "bytesource.h"  // FdByteSource — a delta chain's zip listing (covered-edit check)
#include "vgdelta.h"     // DeltaByteSource — reconstructs a delta chain's zip on the fly
#include "zipscan.h"     // WalkCentralDir — the member names of a (reconstructed) zip
#include "hostio.h"      // HostIO::OpenRead / Close — the byte sources' fds

#include <atomic>
#include <thread>
#include <set>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <string_view>
#include <deque>
#include <fstream>
#include <functional>
#include <climits>
#include <algorithm>
#include <bit>
#include <regex>
#include <cctype>
#include <zip.h>   // node-graph validation reads content zips to case-check CONTENTPATH against real files

const Node *NodeIndex::Find(const std::string &Key) const
{
    // Primary key = the map key: a node's CID handle. The resolver walks refs by CID, so every internal Find hits
    // this fast path.
    auto It = Nodes.find(Key);
    if (It != Nodes.end()) return &It->second;
    // Fallback alias: a human LABEL (a GUI launch id, a CLI argument). Never on the resolver's hot path; LABELs may
    // repeat, first match wins.
    for (const auto &[K, N] : Nodes)
        if (N.Handle == Key) return &N;                       // a working-tree handle (the stored CID before a re-freeze)
    for (const auto &[K, N] : Nodes)
        if (N.NodeId == Key) return &N;
    return nullptr;
}

namespace ManifestModel {

// ----- node graph -----

const std::set<std::string> &NodeFields()
{
    static const std::set<std::string> F = { "CID", "LABEL", "POS", "COMMENT", "VARIANT", "RECOMMENDED", "LAYERS" };
    return F;
}

bool IsNodeObject(const nlohmann::ordered_json &J)
{
    return J.is_object() && J.contains("LAYERS") && J["LAYERS"].is_array();
}

std::vector<std::string> NodeRefs(const nlohmann::ordered_json &J, std::vector<std::string> *Requires)
{
    std::vector<std::string> Out;
    if (!IsNodeObject(J)) return Out;
    for (const auto &L : J["LAYERS"])
    {
        if (!L.is_object()) continue;
        if (L.contains("NODE") && L["NODE"].is_string()) Out.push_back(L["NODE"].get<std::string>());
        if (!Requires) continue;
        if (L.contains("ANY") && L["ANY"].is_array())
            for (const auto &M : L["ANY"]) if (M.is_string()) Requires->push_back(M.get<std::string>());
        if (L.contains("NOT") && L["NOT"].is_string()) Requires->push_back(L["NOT"].get<std::string>());
    }
    return Out;
}

bool RemapNodeRefs(nlohmann::ordered_json &J, const std::function<std::string(const std::string &)> &Map)
{
    if (!IsNodeObject(J)) return false;
    bool Changed = false;
    auto One = [&](nlohmann::ordered_json &S) {
        if (!S.is_string()) return;
        const std::string New = Map(S.get<std::string>());
        if (New != S.get<std::string>()) { S = New; Changed = true; }
    };
    for (auto &L : J["LAYERS"])
    {
        if (!L.is_object()) continue;
        if (L.contains("NODE")) One(L["NODE"]);
        if (L.contains("NOT")) One(L["NOT"]);
        if (L.contains("ANY") && L["ANY"].is_array()) for (auto &M : L["ANY"]) One(M);
    }
    return Changed;
}

//THE boundary where untrusted JSON becomes a Node. It must be TOTAL: a node file arrives from a peer or from an
//author's typo, and BuildNodeIndex runs at startup — a throw here would keep the app from starting. Everything
//read below is checked by NodeLower::CheckNode first; a node that fails is indexed with the reason and no layers.
bool ParseNode(const nlohmann::ordered_json &J, const std::filesystem::path &File,
               const std::filesystem::path &BundleDir, Node &Out)
{
    if (!IsNodeObject(J)) return false;
    Out = Node{};
    Out.Cid       = (J.contains("CID") && J["CID"].is_string()) ? J["CID"].get<std::string>() : std::string();
    Out.NodeId    = (J.contains("LABEL") && J["LABEL"].is_string()) ? J["LABEL"].get<std::string>() : std::string();
    Out.File      = File;
    Out.BundleDir = BundleDir;
    Out.Json      = J;
    Out.Layers    = nlohmann::ordered_json::array();
    Out.Entries   = nlohmann::ordered_json::object();
    Out.LowerError = NodeLower::CheckNode(J, Out.NodeId.empty() ? Out.Cid : Out.NodeId);
    Out.Refs = NodeRefs(J, &Out.Requires);
    if (!Out.LowerError.empty())
    {
        LogWarn("ManifestModel::ParseNode", "Malformed node — " + Out.LowerError + " (" + File.string() + ")");
        return true;
    }
    if (J.contains("VARIANT")) Out.Variant = J["VARIANT"].get<std::string>();
    if (J.contains("RECOMMENDED")) for (const auto &U : J["RECOMMENDED"]) Out.Recommended.push_back(U.get<std::string>());
    Out.Layers = NodeLower::LowerNode(J, Out.NodeId);
    const auto &Ls = J["LAYERS"];
    Out.IsGraft = !Ls.empty() && Ls[0].contains("ANY");
    for (const auto &L : Ls)
        if (L.contains("EXEC"))
            for (const auto &E : L["EXEC"])
            {
                if (E.contains("GUEST") && E["GUEST"].is_array() && !E["GUEST"].empty()) Out.OwnRunner = true;
                if (E.contains("TILE")) Out.OwnTile = true;
            }
    return true;
}

} // namespace ManifestModel

//The default entry: the first game entry (no GUEST) for a runnable node, else the first runner entry.
static const nlohmann::ordered_json *DefaultEntry(const nlohmann::ordered_json &Entries)
{
    const nlohmann::ordered_json *Runner = nullptr;
    for (const auto &[L, E] : Entries.items())
    {
        const bool IsRunner = E.contains("GUEST") && E["GUEST"].is_array() && !E["GUEST"].empty();
        if (!IsRunner && E.contains("HOST")) return &E;
        if (IsRunner && !Runner) Runner = &E;
    }
    return Runner;
}

nlohmann::ordered_json Node::ExecFor(const std::string &Lbl) const
{
    if (!Entries.is_object() || Entries.empty()) return nlohmann::ordered_json();
    if (Lbl.empty()) return Exec;
    if (!Entries.contains(Lbl)) return nlohmann::ordered_json();
    return NodeLower::LowerEntry(Entries[Lbl]);
}

std::string Node::HostFor(const std::string &Lbl) const
{
    if (Lbl.empty()) return HostPlatform;
    const nlohmann::ordered_json E = ExecFor(Lbl);
    return E.is_object() ? E.value("PLATFORM", E.value("HOST", HostPlatform)) : HostPlatform;
}

std::vector<std::string> Node::EntrypointLabels() const
{
    std::vector<std::string> Out;
    if (Entries.is_object()) for (const auto &[L, E] : Entries.items()) Out.push_back(L);
    return Out;
}

namespace ManifestModel {

void ScanBundleNodes(const std::filesystem::path &BundleDir, NodeIndex &Idx)
{
    std::error_code Ec;
    if (!std::filesystem::is_directory(BundleDir, Ec)) return;
    for (const auto &Entry : std::filesystem::directory_iterator(BundleDir, Ec))
    {
        if (!Entry.is_regular_file(Ec) || Entry.path().extension() != ".json") continue;
        std::ifstream In(Entry.path(), std::ios::binary);
        if (!In) continue;
        nlohmann::ordered_json J;
        try { In >> J; }
        catch (const std::exception &E)
        { LogWarn("ManifestModel::ScanBundleNodes", "Skipping unparseable " + Entry.path().string() + ": " + E.what()); continue; }
        Node N;
        if (!ParseNode(J, Entry.path(), BundleDir, N)) continue;         // not a node
        // Key by the node's HANDLE — its stored CID. A node with no stored CID (a never-minted draft) gets a
        // synthetic per-file key so handle-less nodes never fold together.
        std::string K = N.Key();
        for (unsigned char c : K) if (c < 0x20) { K.clear(); break; }    // control-byte handle -> synthetic (unforgeable key)
        if (K.empty()) K = std::string("\x01") + Entry.path().string() + "#unnamed" + std::to_string(Idx.Nodes.size());
        else if (Idx.Nodes.count(K))
        { LogWarn("ManifestModel::ScanBundleNodes", "Duplicate node handle '" + K + "' (" + Entry.path().string() + ") — keeping first-seen."); continue; }
        Idx.Nodes.emplace(K, std::move(N));
    }
}

NodeIndex BuildNodeIndex(const std::vector<std::filesystem::path> &LibraryRoots,
                         const std::vector<std::filesystem::path> &ExtraBundleDirs)
{
    NodeIndex Idx;
    std::error_code Ec;
    for (const auto &Root : LibraryRoots)
    {
        if (!std::filesystem::is_directory(Root, Ec)) continue;
        for (const auto &Bundle : std::filesystem::directory_iterator(Root, Ec))
            if (Bundle.is_directory(Ec)) ScanBundleNodes(Bundle.path(), Idx);
    }
    for (const auto &Bundle : ExtraBundleDirs)               // locally-added packages: each dir IS a bundle
        if (std::filesystem::is_directory(Bundle, Ec)) ScanBundleNodes(Bundle, Idx);
    DeriveFacts(Idx);
    LogOut("ManifestModel::BuildNodeIndex", "Indexed " + std::to_string(Idx.Nodes.size()) + " node(s) across "
           + std::to_string(LibraryRoots.size()) + " root(s).");
    return Idx;
}

Fold::Library LibraryOf(const NodeIndex &Idx)
{
    Fold::Library L;
    L.Nodes.reserve(Idx.Nodes.size());
    for (const auto &[K, N] : Idx.Nodes)
        if (N.LowerError.empty()) L.Nodes.emplace(K, Fold::Library::Entry{ &N.Json, N.BundleDir.string() });
    return L;
}

//A tile's cover in the engine's layer vocabulary ({PATH, SOURCE{TYPE, CID, SIZE}}), the shape every cover reader
//(LayerLocator) already takes; the rest of the tile flat, META fields beside the tile's own (the tile's own win).
static nlohmann::ordered_json FlatTile(const nlohmann::ordered_json &T)
{
    nlohmann::ordered_json M = nlohmann::ordered_json::object();
    for (const auto &[K, V] : T.items())
    {
        if (K == "META") continue;
        if (K == "COVER" && V.is_object())
        {
            nlohmann::ordered_json C = { {"PATH", V.value("FILE", std::string())} };
            if (V.contains("SOURCE") && V["SOURCE"].is_string())
            {
                C["SOURCE"] = { {"TYPE", "ipfs"}, {"CID", V["SOURCE"]} };
                if (V.contains("SIZE") && V["SIZE"].is_number()) C["SOURCE"]["SIZE"] = V["SIZE"];
            }
            M["COVER"] = std::move(C);
            continue;
        }
        M[K] = V;
    }
    if (T.contains("META") && T["META"].is_object())
        for (const auto &[K, V] : T["META"].items())
            if (!M.contains(K)) M[K] = V;
    return M;
}

void DeriveFacts(NodeIndex &Idx)
{
    const Fold::Library Lib = LibraryOf(Idx);
    // 1. Each node's own fold: its effective entries (the folded EXEC) and the tiles they present.
    struct Presented { std::string Node; nlohmann::ordered_json Tile; bool Variant = false, Recommended = false; std::string Sort; };
    std::map<std::string, std::vector<Presented>> ByUid;
    //Each node's entries, resolved in parallel: the library is read-only and every resolve is independent.
    std::vector<std::pair<const std::string *, Node *>> Work;
    for (auto &[K, N] : Idx.Nodes) Work.push_back({ &K, &N });
    {
        std::atomic<size_t> Next{ 0 };
        auto Run = [&] {
            for (size_t I; (I = Next.fetch_add(1)) < Work.size();)
            {
                Node &N = *Work[I].second;
                N.Entries = N.LowerError.empty() ? Fold::ResolveEntries(Lib, *Work[I].first).Exec : nlohmann::ordered_json::object();
            }
        };
        const unsigned Threads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
        std::vector<std::thread> Pool;
        for (unsigned T = 1; T < Threads; ++T) Pool.emplace_back(Run);
        Run();
        for (auto &T : Pool) T.join();
    }
    for (auto &[K, N] : Idx.Nodes)
    {
        N.HasExec = N.HasRunner = false;
        N.Exec = nlohmann::ordered_json();
        N.HostPlatform.clear(); N.GuestPlatform.clear(); N.Label.clear(); N.Faces.clear();
        N.Uid.clear(); N.PackageUid.clear(); N.Meta = nlohmann::ordered_json();
        if (!N.LowerError.empty()) continue;
        for (const auto &[L, E] : N.Entries.items())
        {
            const bool IsRunner = E.contains("GUEST") && E["GUEST"].is_array() && !E["GUEST"].empty();
            if (IsRunner) N.HasRunner = true;
            else if (E.contains("HOST")) N.HasExec = true;
            if (!IsRunner && E.contains("TILE") && E["TILE"].is_object() && E["TILE"].contains("UID"))
            {
                const std::string Uid = E["TILE"]["UID"].get<std::string>();
                if (std::find(N.Faces.begin(), N.Faces.end(), Uid) == N.Faces.end()) N.Faces.push_back(Uid);
                ByUid[Uid].push_back({ K, E["TILE"], !N.Variant.empty(),
                                       std::find(N.Recommended.begin(), N.Recommended.end(), Uid) != N.Recommended.end(),
                                       N.Variant + '\x1f' + K });
            }
        }
        if (const nlohmann::ordered_json *D = DefaultEntry(N.Entries))
        {
            N.Exec = NodeLower::LowerEntry(*D);
            N.Label = D->value("LABEL", std::string());
            N.HostPlatform = D->value("HOST", std::string());
            if (D->contains("GUEST") && (*D)["GUEST"].is_array())
                for (const auto &G : (*D)["GUEST"]) if (G.is_string()) N.GuestPlatform.push_back(G.get<std::string>());
        }
    }
    // 2. The tile table: faces with one UID are one tile. Its presentation is its recommended variant's folded tile,
    //    else its first variant's (by VARIANT, then handle), else — no variant presents it — any node's.
    std::map<std::string, nlohmann::ordered_json> Tiles;
    for (auto &[Uid, Ps] : ByUid)
    {
        std::stable_sort(Ps.begin(), Ps.end(), [](const Presented &A, const Presented &B) {
            if (A.Variant != B.Variant) return A.Variant;
            if (A.Recommended != B.Recommended) return A.Recommended;
            return A.Sort < B.Sort;
        });
        Tiles[Uid] = Ps.front().Tile;
    }
    auto Root = [&](std::string Uid) {
        std::set<std::string> Seen;
        for (;;)
        {
            auto It = Tiles.find(Uid);
            if (It == Tiles.end() || !It->second.contains("PARENTUID") || !Seen.insert(Uid).second) return Uid;
            Uid = It->second["PARENTUID"].get<std::string>();
        }
    };
    for (auto &[K, N] : Idx.Nodes)
    {
        if (N.Faces.empty()) continue;
        N.Uid = N.Faces.front();
        N.PackageUid = Root(N.Uid);
        N.Meta = FlatTile(Tiles[N.Uid]);
    }
}

std::vector<std::string> Closure(const NodeIndex &Idx, const std::string &Root, std::vector<std::string> *Missing)
{
    std::vector<std::string> Order;
    std::unordered_set<std::string> Visited, OnStack;
    std::function<void(const std::string &)> Emit = [&](const std::string &Id) {
        if (Visited.count(Id) || OnStack.count(Id)) return;              // a cycle is broken at its back-edge
        const Node *N = Idx.Find(Id);
        if (!N) { if (Missing) Missing->push_back(Id); Visited.insert(Id); return; }
        OnStack.insert(Id);
        for (const std::string &R : N->Refs) Emit(R);
        OnStack.erase(Id);
        Visited.insert(Id);
        Order.push_back(N->Key());
    };
    Emit(Root);
    return Order;
}

std::vector<EndpointInfo> TileEndpoints(const NodeIndex &Idx, const std::vector<std::string> &VariantIds)
{
    // Endpoints are computed over the CONTENT-CARRYING subgraph, not raw graph sinks: every variant is typically a
    // sink (nothing contains it), so "sinks" would be one row per version. What nests is the CONTENT; a greedy
    // set-cover over it picks the variants that own the tips.

    // 1. Union closure over all variants: one shared-visited DFS — O(distinct nodes), never per-variant.
    std::unordered_set<std::string> Visited;
    std::vector<std::string> Stack(VariantIds.begin(), VariantIds.end());
    std::vector<const Node*> Union;
    while (!Stack.empty())
    {
        const std::string Cur = Stack.back(); Stack.pop_back();
        if (!Visited.insert(Cur).second) continue;
        const Node *N = Idx.Find(Cur);
        if (!N) continue;
        Union.push_back(N);
        for (const std::string &Pid : N->Refs) Stack.push_back(Pid);
    }
    const int Nn = (int)Union.size();
    std::unordered_map<std::string, int> IxOf;
    for (int i = 0; i < Nn; ++i) { IxOf[Union[i]->Key()] = i; IxOf.emplace(Union[i]->NodeId, i); }

    auto CarriesContent = [](const Node *N) {
        if (!N->Layers.is_array()) return false;
        for (const auto &L : N->Layers) if (IsVfsLayer(LayerType(L))) return true;
        return false;
    };

    // 2. Per-node closure BITSETS, memoized bottom-up (closure(n) = self ∪ closure(refs)).
    const int Words = (Nn + 63) / 64;
    std::vector<std::vector<uint64_t>> Clo(Nn);
    std::function<const std::vector<uint64_t>&(int)> CloOf = [&](int I) -> const std::vector<uint64_t>& {
        if (!Clo[I].empty()) return Clo[I];
        std::vector<uint64_t> B(Words, 0);
        B[I >> 6] |= (uint64_t)1 << (I & 63);
        Clo[I] = B;                                                   // placeholder: a cycle reads the partial set
        for (const std::string &Pid : Union[I]->Refs)
            if (auto It = IxOf.find(Pid); It != IxOf.end())
            {
                const std::vector<uint64_t> P = CloOf(It->second);
                for (int W = 0; W < Words; ++W) B[W] |= P[W];
            }
        Clo[I] = std::move(B);
        return Clo[I];
    };
    for (int i = 0; i < Nn; ++i) CloOf(i);

    std::vector<uint64_t> ContentMask(Words, 0);
    for (int i = 0; i < Nn; ++i)
        if (CarriesContent(Union[i])) ContentMask[i >> 6] |= (uint64_t)1 << (i & 63);

    // 3. The REQUIRED region: reachable through UNGATED NODE layers only — an option's content (a WHEN-gated NODE
    //    layer) must not surface endpoints of its own.
    std::vector<uint64_t> Required(Words, 0);
    {
        std::unordered_set<std::string> Seen;
        std::vector<std::string> St(VariantIds.begin(), VariantIds.end());
        while (!St.empty())
        {
            const std::string Cur = St.back(); St.pop_back();
            if (!Seen.insert(Cur).second) continue;
            auto It = IxOf.find(Cur);
            if (It == IxOf.end()) continue;
            const Node *N = Union[It->second];
            Required[It->second >> 6] |= (uint64_t)1 << (It->second & 63);
            if (N->Json.contains("LAYERS"))
                for (const auto &L : N->Json["LAYERS"])
                    if (L.is_object() && L.contains("NODE") && L["NODE"].is_string() && !L.contains("WHEN"))
                        St.push_back(L["NODE"].get<std::string>());
        }
    }

    // 4. GREEDY SET-COVER over the required content (deterministic tie-break: larger total content, then key).
    std::vector<int> VarIx; VarIx.reserve(VariantIds.size());
    for (const std::string &V : VariantIds)
        if (auto It = IxOf.find(V); It != IxOf.end()) VarIx.push_back(It->second);

    std::vector<uint64_t> Uncovered(Words);
    for (int W = 0; W < Words; ++W) Uncovered[W] = Required[W] & ContentMask[W];
    std::vector<int> Picks;
    std::vector<int> PickGains;
    std::unordered_set<int> Picked;
    for (;;)
    {
        int Best = -1, BestGain = 0, BestTotal = 0;
        for (int V : VarIx)
        {
            if (Picked.count(V)) continue;
            int Gain = 0, Total = 0;
            for (int W = 0; W < Words; ++W)
            {
                Gain  += (int)std::popcount(Clo[V][W] & Uncovered[W]);
                Total += (int)std::popcount(Clo[V][W] & ContentMask[W]);
            }
            if (Gain > BestGain
                || (Gain == BestGain && Gain > 0
                    && (Total > BestTotal || (Total == BestTotal && Best >= 0 && Union[V]->Key() < Union[Best]->Key()))))
            { Best = V; BestGain = Gain; BestTotal = Total; }
        }
        if (Best < 0 || BestGain == 0) break;
        Picks.push_back(Best); PickGains.push_back(BestGain); Picked.insert(Best);
        for (int W = 0; W < Words; ++W) Uncovered[W] &= ~Clo[Best][W];
    }
    if (Picks.empty())                     // contentless tile → the variants themselves, so the dialog isn't empty
    {
        Picks = VarIx;
        PickGains.assign(Picks.size(), 1);
    }

    // 5. Rows: a pick stands alone only when it contributed ≥5% of the content; the long tail folds into
    //    "Everything else". Always at least one named row; capped.
    int TotalContent = 0;
    for (int W = 0; W < Words; ++W) TotalContent += (int)std::popcount(Required[W] & ContentMask[W]);
    constexpr size_t MaxNamedRows = 6;
    size_t Named = 0;
    while (Named < Picks.size() && Named < MaxNamedRows
           && (Named == 0 || (long long)PickGains[Named] * 20 >= TotalContent))
        ++Named;
    if (Named + 1 == Picks.size()) ++Named;
    std::vector<EndpointInfo> Out;
    for (size_t K = 0; K < Named; ++K)
    {
        const Node *N = Union[Picks[K]];
        EndpointInfo E;
        E.Ids   = { N->Key() };
        E.Label = !N->Variant.empty() ? N->Variant
                  : (N->Meta.is_object() ? N->Meta.value("TITLE", N->NodeId) : N->NodeId);
        for (int V : VarIx)
        {
            bool Sub = true;
            for (int W = 0; W < Words && Sub; ++W)
                if ((Clo[V][W] & ContentMask[W]) & ~(Clo[Picks[K]][W] & ContentMask[W])) Sub = false;
            if (Sub) E.LaunchableCount++;
        }
        Out.push_back(std::move(E));
    }
    if (Named < Picks.size())
    {
        EndpointInfo Rest;
        Rest.Label = "Everything else";
        for (size_t K = Named; K < Picks.size(); ++K) Rest.Ids.push_back(Union[Picks[K]]->Key());
        Out.push_back(std::move(Rest));
    }
    return Out;
}

namespace {

std::string ToLowerAscii(std::string S) { for (char &c : S) c = (char)std::tolower((unsigned char)c); return S; }

// The regular file paths inside a zip (slash-normalized, directory entries dropped). Empty on open failure.
const std::vector<std::string> &ZipEntriesCached(const std::string &Path,
        std::unordered_map<std::string, std::vector<std::string>> &Cache)
{
    auto It = Cache.find(Path);
    if (It != Cache.end()) return It->second;
    std::vector<std::string> Out;
    int Err = 0;
    if (zip_t *Za = zip_open(Path.c_str(), ZIP_RDONLY, &Err))
    {
        const zip_int64_t N = zip_get_num_entries(Za, 0);
        for (zip_uint64_t i = 0; i < (zip_uint64_t)N; ++i)
        {
            const char *Name = zip_get_name(Za, i, ZIP_FL_ENC_RAW);
            if (!Name || !*Name) continue;
            std::string S = Name;
            std::replace(S.begin(), S.end(), '\\', '/');
            if (S.back() == '/') continue;                                   // directory entry
            Out.push_back(std::move(S));
        }
        zip_close(Za);
    }
    return Cache.emplace(Path, std::move(Out)).first->second;
}


// Join a layer's TARGET (mount offset within the content root) and an in-layer relative path into one
// content-root-relative slash path.
std::string JoinTarget(std::string Target, std::string Rel)
{
    std::replace(Target.begin(), Target.end(), '\\', '/');
    std::replace(Rel.begin(), Rel.end(), '\\', '/');
    while (!Target.empty() && Target.front() == '/') Target.erase(Target.begin());
    while (!Target.empty() && Target.back()  == '/') Target.pop_back();
    while (!Rel.empty() && Rel.front() == '/') Rel.erase(Rel.begin());
    return Target.empty() ? Rel : Target + "/" + Rel;
}

// A resolved content item's local file: its bundle dir + payload (a %runtime% payload has none).
std::filesystem::path ItemLocal(const Fold::Item &I)
{
    if (I.Dir.empty() || I.Payload.find('%') != std::string::npos) return {};
    return std::filesystem::path(I.Dir) / I.Payload;
}

// The case-sensitive set of target-relative file paths a launchable's resolved plan mounts, from LOCALLY-present
// content only (zips via libzip, dirs walked). AnyLocal = ≥1 layer read; AllLocal = every content layer was on disk.
void GatherLaunchContentFiles(const Fold::Plan &P,
        std::unordered_map<std::string, std::vector<std::string>> &ZipCache,
        std::set<std::string> &Files, bool &AnyLocal, bool &AllLocal)
{
    AnyLocal = false; AllLocal = true;
    for (const Fold::Item &I : P.Seq)
    {
        if (I.Kind == "EDIT") continue;
        const std::filesystem::path Local = ItemLocal(I);
        if (Local.empty()) continue;
        std::error_code Ec;
        if (!std::filesystem::exists(Local, Ec)) { AllLocal = false; continue; }   // remote-only / not hydrated
        AnyLocal = true;
        if (I.Kind == "FILE")
            //A FILE layer mounts one file at TARGET/<its name> (overlay.cpp: fileVPath).
            Files.insert(JoinTarget(I.Target, Local.filename().string()));
        else if (std::filesystem::is_directory(Local, Ec))
            for (auto It = std::filesystem::recursive_directory_iterator(Local, Ec);
                 !Ec && It != std::filesystem::recursive_directory_iterator(); It.increment(Ec))
            {
                if (!It->is_regular_file(Ec)) continue;
                Files.insert(JoinTarget(I.Target, std::filesystem::relative(It->path(), Local, Ec).generic_string()));
            }
        else
            for (const std::string &Entry : ZipEntriesCached(Local.string(), ZipCache))
                Files.insert(JoinTarget(I.Target, Entry));
    }
}

// ----- what a content layer REPLACES (the covered-edit check, §1.5; mirrors tools/gen6/resolve.py provided()) -----
// The lower-cased member names of the zip content item Seq[J] mounts: a ZIP's own, a DELTA's = the zip its chain
// reconstructs (its base = the content beneath at the same TARGET, down to the zip). nullopt when unknowable.
std::optional<std::set<std::string>> MemberNames(const std::vector<Fold::Item> &Seq, size_t J,
        std::map<std::vector<std::string>, std::optional<std::set<std::string>>> &Cache)
{
    const Fold::Item &C = Seq[J];
    std::vector<std::string> Chain = { ItemLocal(C).string() };
    if (Chain[0].empty()) return std::nullopt;
    if (C.Kind == "DELTA")
    {
        bool Based = false;
        for (size_t I = J; I-- > 0;)
        {
            const Fold::Item &B = Seq[I];
            if (B.Kind == "EDIT" || B.Target != C.Target) continue;
            if ((B.Kind != "ZIP" && B.Kind != "DELTA") || ItemLocal(B).empty()) return std::nullopt;
            Chain.insert(Chain.begin(), ItemLocal(B).string());
            if (B.Kind == "ZIP") { Based = true; break; }
        }
        if (!Based) return std::nullopt;
    }
    if (auto It = Cache.find(Chain); It != Cache.end()) return It->second;
    std::optional<std::set<std::string>> Names;
    auto Open = [](const std::string &P) -> std::shared_ptr<ByteSource> {
        const HostIO::Fd Fd = HostIO::Open(P, 0 /*O_RDONLY*/);
        if (Fd < 0) return nullptr;
        HostIO::Stat St;
        if (HostIO::Fstat(Fd, St) != 0) { HostIO::Close(Fd); return nullptr; }
        return std::make_shared<FdByteSource>(Fd, St.size, /*owns=*/true);
    };
    std::shared_ptr<ByteSource> Src = Open(Chain[0]);
    for (size_t I = 1; Src && I < Chain.size(); ++I)
    {
        std::shared_ptr<ByteSource> D = Open(Chain[I]);
        std::string Err;
        Src = D ? vgdelta::DeltaByteSource::Create(D, Src, Err) : nullptr;
    }
    if (Src)
    {
        std::set<std::string> S;
        const bool Ok = zipscan::WalkCentralDir(*Src, [&](const std::string &Name, uint16_t, uint64_t) {
            std::string N = Name;
            std::replace(N.begin(), N.end(), '\\', '/');
            while (!N.empty() && N.back() == '/') N.pop_back();
            S.insert(ToLowerAscii(N));
            return true;
        });
        if (Ok) Names = std::move(S);
    }
    return Cache.emplace(Chain, Names).first->second;
}

// The lower-cased paths content Seq[J] provides as the mount sees them (SUBMOUNTS mount only their sub-paths, a
// FILE lands at TARGET/<name>), or nullopt = everything under its TARGET (a DIR, an unreadable file).
std::optional<std::set<std::string>> Provided(const std::vector<Fold::Item> &Seq, size_t J,
        std::map<std::vector<std::string>, std::optional<std::set<std::string>>> &Cache)
{
    const Fold::Item &C = Seq[J];
    const std::string T = ToLowerAscii(C.Target);
    const std::string Pre = T.empty() ? std::string() : T + "/";
    if (C.Kind == "DIR" || C.Dir.empty()) return std::nullopt;
    if (C.Kind == "FILE") return std::set<std::string>{ Pre + ToLowerAscii(std::filesystem::path(C.Payload).filename().string()) };
    auto Names = MemberNames(Seq, J, Cache);
    if (!Names) return std::nullopt;
    std::set<std::string> Out;
    if (!C.Submounts.is_array() || C.Submounts.empty())
    {
        for (const auto &N : *Names) Out.insert(Pre + N);
        return Out;
    }
    for (const auto &Sm : C.Submounts)
    {
        const std::string X = Sm.is_string() ? Sm.get<std::string>() : std::string();
        const size_t Colon = X.find(':');
        if (Colon == std::string::npos) continue;
        std::string Src = ToLowerAscii(X.substr(0, Colon)), Dst = ToLowerAscii(X.substr(Colon + 1));
        while (!Src.empty() && Src.front() == '/') Src.erase(Src.begin());
        while (!Src.empty() && Src.back() == '/') Src.pop_back();
        while (!Dst.empty() && Dst.back() == '/') Dst.pop_back();
        for (const auto &N : *Names)
        {
            if (N == Src) Out.insert(Dst);
            else if (N.rfind(Src + "/", 0) == 0) Out.insert(Dst + N.substr(Src.size()));
        }
    }
    return Out;
}

bool Covers(const std::vector<Fold::Item> &Seq, size_t J, const std::string &File,
            std::map<std::vector<std::string>, std::optional<std::set<std::string>>> &Cache)
{
    const std::string F = ToLowerAscii(File);
    const auto P = Provided(Seq, J, Cache);
    if (!P)
    {
        const std::string T = ToLowerAscii(Seq[J].Target);
        return F == T || F.rfind(T + "/", 0) == 0 || T.empty();
    }
    return P->count(F) != 0;
}

// One layer's contribution to the merged content view, fully prepared for the case lint: the joined
// content-root-relative path plus its lowered form. Cached per (local path, TARGET) because this is where
// --validate-nodes actually spent its time: 903 Minecraft variants share a handful of layer files, and the lint
// re-ran JoinTarget + ToLowerAscii over every entry of every shared layer once PER LAUNCHABLE — 87% of a 49-second
// validate by perf, all of it recomputing strings that never change within a run. Dir layers additionally re-walked
// the filesystem per launchable; the walk is now taken once too.
struct PreparedEntry { std::string Exact, Lower; };
const std::vector<PreparedEntry> &LayerEntriesPrepared(const std::string &LocalPath, bool IsDir, const std::string &Target,
        std::unordered_map<std::string, std::vector<std::string>> &ZipCache,
        std::unordered_map<std::string, std::vector<PreparedEntry>> &PrepCache)
{
    const std::string Key = LocalPath + '\x1f' + Target;
    auto It = PrepCache.find(Key);
    if (It != PrepCache.end()) return It->second;
    std::vector<PreparedEntry> Out;
    auto Add = [&](std::string Rel) {
        std::string Exact = JoinTarget(Target, std::move(Rel));
        std::string Lower = ToLowerAscii(Exact);
        Out.push_back({std::move(Exact), std::move(Lower)});
    };
    std::error_code Ec;
    if (IsDir)
        for (auto W = std::filesystem::recursive_directory_iterator(LocalPath, Ec);
             !Ec && W != std::filesystem::recursive_directory_iterator(); W.increment(Ec))
        { if (W->is_regular_file(Ec)) Add(std::filesystem::relative(W->path(), LocalPath, Ec).generic_string()); }
    else
        for (const std::string &Entry : ZipEntriesCached(LocalPath, ZipCache))
            Add(Entry);
    return PrepCache.emplace(Key, std::move(Out)).first->second;
}

// Path components of a slash path.
std::vector<std::string> SplitPath(const std::string &S)
{
    std::vector<std::string> V; size_t P = 0, Q;
    while ((Q = S.find('/', P)) != std::string::npos) { V.push_back(S.substr(P, Q - P)); P = Q + 1; }
    V.push_back(S.substr(P));
    return V;
}

// Reports case collisions across DIFFERENT layers in a launchable's merged content view: two layers contributing
// paths that differ only in case (e.g. base ships 'MAPS/foo', a patch ships 'maps/foo'). On the case-sensitive mount
// both exist, so a lookup can resolve to the wrong file → crashes / missing data. Collisions WITHIN one layer are the
// upstream game's own content and are deliberately ignored. GlobalSeen dedups across launchables sharing content.
void FindCrossLayerCaseCollisions(const Fold::Plan &P,
        std::unordered_map<std::string, std::vector<std::string>> &ZipCache,
        std::unordered_map<std::string, std::vector<PreparedEntry>> &PrepCache,
        std::vector<std::string> &Out, std::unordered_set<std::string> &GlobalSeen)
{
    std::unordered_map<std::string_view, std::pair<std::string_view, const std::string *>> Seen;  // lowered -> (exact, label)
    std::unordered_set<std::string> LocalReported;

    auto Consider = [&](const PreparedEntry &E, const std::string &Lbl)
    {
        auto It = Seen.find(std::string_view(E.Lower));
        if (It == Seen.end()) { Seen.emplace(std::string_view(E.Lower), std::make_pair(std::string_view(E.Exact), &Lbl)); return; }
        const std::string Prev(It->second.first), PrevLbl = *It->second.second, F = E.Exact;
        if (Prev == F || PrevLbl == Lbl) return;
        const std::vector<std::string> A = SplitPath(Prev), B = SplitPath(F);
        size_t i = 0; while (i < A.size() && i < B.size() && A[i] == B[i]) ++i;
        if (i >= A.size() || i >= B.size()) return;
        std::string Key; for (size_t k = 0; k <= i; ++k) { if (k) Key += '/'; Key += ToLowerAscii(A[k]); }
        if (!LocalReported.insert(Key).second) return;
        if (!GlobalSeen.insert(Key + "|" + ToLowerAscii(PrevLbl) + "|" + ToLowerAscii(Lbl)).second) return;
        const bool IsDir = (i + 1 < A.size()) || (i + 1 < B.size());
        Out.push_back(std::string("content case conflict across layers: ") + (IsDir ? "directory '" : "file '")
                      + A[i] + "' (" + PrevLbl + ") vs '" + B[i] + "' (" + Lbl + ") — collide in the merged view");
    };

    std::deque<std::string> Labels;                              // stable storage — Seen holds pointers into it
    for (const Fold::Item &I : P.Seq)
    {
        if (I.Kind == "EDIT") continue;
        const std::filesystem::path Local = ItemLocal(I);
        if (Local.empty()) continue;
        std::error_code Ec;
        if (!std::filesystem::exists(Local, Ec)) continue;
        const std::string &Lbl = Labels.emplace_back(Local.filename().string());
        const bool IsDir = std::filesystem::is_directory(Local, Ec);
        for (const PreparedEntry &E : LayerEntriesPrepared(Local.string(), IsDir, I.Target, ZipCache, PrepCache))
            Consider(E, Lbl);
    }
}

} // namespace

void ValidateNodeGraph(const NodeIndex &Idx, std::vector<std::string> &Errors, std::vector<std::string> &Warnings,
                       const std::set<std::string> *OnlyNodes)
{
    const std::string Machine = MachinePlatform();
    std::unordered_map<std::string, std::vector<std::string>> ZipCache;   // local zip path -> its file entries
    std::unordered_map<std::string, std::vector<PreparedEntry>> PrepCache; // (layer path, TARGET) -> prepared case-lint entries
    std::map<std::vector<std::string>, std::optional<std::set<std::string>>> NameCache;   // content chain -> member names
    std::unordered_set<std::string> CrossLayerSeen;
    const Fold::Library Lib = LibraryOf(Idx);
    const Fold::GraftIndex Grafts = Fold::BuildGraftIndex(Lib);
    auto InScope = [&](const std::string &Id) { return !OnlyNodes || OnlyNodes->count(Id); };

    auto HasRunnerFor = [&](const std::string &Host) {
        for (const auto &[Id, R] : Idx.Nodes)
            if (R.OwnRunner && R.HostPlatform == Machine)
                for (const auto &G : R.GuestPlatform) if (G == Host) return true;
        return false;
    };
    std::set<std::string> KnownTiles;
    for (const auto &[Id, N] : Idx.Nodes) for (const auto &U : N.Faces) KnownTiles.insert(U);

    //Cycle memo shared across the pass: a node proven acyclic never participates in a cycle (O(N+E) overall).
    std::unordered_set<std::string> CycleDone;
    for (const auto &[Id, N] : Idx.Nodes)
    {
        if (!InScope(Id)) continue;
        const std::string Tag = "node '" + Id + "'";
        //A malformed node is indexed with the reason and routed through by nothing — a package that quietly does
        //less than it says. Name it.
        if (!N.LowerError.empty()) { Errors.push_back(N.LowerError); continue; }

        //Refs: a contained node must exist; an ANY member or a NOT may name a node this library lacks (a graft for a
        //version you don't have; an exclusion that cannot happen), but an ANY with no member here is unsatisfiable.
        for (const std::string &R : N.Refs)
            if (!Idx.Find(R)) Errors.push_back(Tag + ": contains missing node '" + R + "'");
        for (const auto &L : N.Json["LAYERS"])
        {
            if (L.contains("ANY"))
            {
                bool Present = false;
                for (const auto &M : L["ANY"]) if (Idx.Find(M.get<std::string>())) { Present = true; break; }
                if (!Present) Warnings.push_back(Tag + ": ANY names no node in this library (nothing it applies onto is here)");
            }
            if (L.contains("NOT") && !Idx.Find(L["NOT"].get<std::string>()))
                Warnings.push_back(Tag + ": NOT names node '" + L["NOT"].get<std::string>() + "', which is not in the library");
        }
        {
            std::unordered_set<std::string> OnStack;
            std::function<bool(const std::string &)> HasCycle = [&](const std::string &Cur) -> bool {
                if (CycleDone.count(Cur)) return false;
                if (OnStack.count(Cur)) return true;
                OnStack.insert(Cur);
                const Node *C = Idx.Find(Cur);
                if (C) for (const std::string &P : C->Refs) if (Idx.Find(P) && HasCycle(P)) return true;
                OnStack.erase(Cur); CycleDone.insert(Cur);
                return false;
            };
            if (HasCycle(Id)) Errors.push_back(Tag + ": NODE layers form a cycle");
        }

        //Content: a local zip must be STOREd (VidyaGodFS reads entries at their backing offset); a DIR that is not a
        //%runtime% path is an authoring intermediary that cannot be published.
        for (const auto &L : N.Layers)
        {
            const std::string LType = LayerType(L);
            if (!IsVfsLayer(LType)) continue;
            if (LType == "VFSDirLayer" && !IsRuntimeSourcedLayer(L))
                Warnings.push_back(Tag + ": DIR '" + L.value("PATH", std::string())
                    + "' is an unzipped authoring layer — convert it to a STORE zip ('→ ZIP') before publishing.");
            if (LType == "VFSZipLayer")
            {
                const std::string Local = ResolveLayerSource(L, N.BundleDir);
                std::string FirstCompressed;
                if (!Local.empty() && std::filesystem::exists(Local) && !ZipFullyStored(Local, &FirstCompressed))
                    Errors.push_back(Tag + ": ZIP '" + L.value("PATH", std::string()) + "' is DEFLATE-compressed (entry '"
                        + FirstCompressed + "') — VidyaGodFS requires a STORE (uncompressed) zip. Re-create it with `zip -0`.");
            }
        }
        //Binary EDIT ops: a malformed one does nothing, or patches blindly.
        for (const auto &L : N.Layers)
        {
            if (LayerType(L) != "BinaryPatch") continue;
            const std::string M = L.value("MODE", std::string());
            if (!L.contains("ANCHOR") && !L.contains("OFFSET"))
                Errors.push_back(Tag + ": binary EDIT op has neither ANCHOR nor OFFSET (no site to patch).");
            if (!L.contains("EXPECT"))
                Warnings.push_back(Tag + ": binary EDIT op has no EXPECT guard — it patches without verifying the original bytes.");
            if (M == "Replace" && !L.contains("REPLACE")) Errors.push_back(Tag + ": Replace has no REPLACE bytes.");
            if (M == "Poke" && !L.contains("VALUE"))      Errors.push_back(Tag + ": Poke has no VALUE.");
            if (M == "Cave" && !L.contains("PAYLOAD"))    Errors.push_back(Tag + ": Cave has no PAYLOAD.");
        }

        //Runners: what they serve, and how a prefix they generate maps guest coordinates.
        if (N.OwnRunner)
        {
            if (N.GuestPlatform.empty()) Warnings.push_back(Tag + ": runner entry declares no GUEST platforms");
            if (N.Exec.is_object() && N.Exec.value("PREFIX_GENERATE", false) && !N.Exec.contains("GUEST_ROOTS"))
                Warnings.push_back(Tag + ": a runner that generates a prefix declares no GUEST_ROOTS — packages' C:/ paths cannot be placed");
        }

        //The node's own facets.
        if (!N.Variant.empty() && !N.HasExec)
            Errors.push_back(Tag + ": VARIANT '" + N.Variant + "' has no effective entry to run");
        if (!N.Variant.empty() && N.HasExec && N.Faces.empty())
            Errors.push_back(Tag + ": VARIANT '" + N.Variant + "' presents no tile (no entry in its fold carries a TILE) — it appears under no card");
        for (const std::string &U : N.Recommended)
        {
            if (!N.Variant.empty() && std::find(N.Faces.begin(), N.Faces.end(), U) == N.Faces.end())
                Errors.push_back(Tag + ": RECOMMENDED names tile '" + U + "', which this variant does not present");
            else if (N.Variant.empty() && !N.IsGraft)
                Warnings.push_back(Tag + ": RECOMMENDED on a node that is neither a variant nor a graft means nothing");
            else if (N.IsGraft && !KnownTiles.count(U))
                Warnings.push_back(Tag + ": RECOMMENDED names tile '" + U + "', which no variant in this library presents");
        }
        if (N.Json["LAYERS"].empty()) Warnings.push_back(Tag + ": pointless node (no layers)");

        //Per ROW (a variant; a runner root): resolve it — default options, and every bool option on with every
        //offered graft ticked — and check what the resolution itself can see.
        if (!N.IsVariant() && !N.OwnRunner) continue;
        std::vector<std::pair<Fold::Vars, std::vector<std::string>>> Configs = { {{}, {}} };
        {
            const Fold::Plan P0 = Fold::Resolve(Lib, Id);
            Fold::Vars AllOn;
            for (const auto &[K, D] : P0.Decls.items())
                if (D.is_object() && D.contains("UI") && D["UI"].is_object() && D["UI"].value("CONTROL", std::string()) == "bool")
                    AllOn[K] = "1";
            //every offered graft, a graft on a graft included (offered once the graft it needs is applied)
            const std::vector<std::string> All = Fold::ApplyGrafts(Lib, Grafts, Id, AllOn, {}, N.Uid, nullptr, nullptr, true);
            if (!AllOn.empty() || !All.empty()) Configs.push_back({ AllOn, All });
        }
        std::set<std::string> Said;                                     // one report per problem across configs
        auto Once = [&](std::vector<std::string> &Where, const std::string &Msg) { if (Said.insert(Msg).second) Where.push_back(Msg); };
        for (const auto &[Inst, Gs] : Configs)
        {
            const Fold::Plan P = Fold::Resolve(Lib, Id, Inst, {}, Gs);
            if (!P.Error.empty()) Once(Errors, Tag + ": " + P.Error);
            for (const auto &[Ev, Cid] : P.Events)
            {
                if (Ev == "cycle")     Once(Errors, Tag + ": resolving it meets a cycle through '" + Cid + "'");
                if (Ev == "missing")   Once(Errors, Tag + ": resolving it needs node '" + Cid + "', which is not in the library");
                if (Ev == "any-unmet" && !Gs.empty()) Once(Warnings, Tag + ": graft '" + Cid + "' is offered but its ANY does not hold");
                else if (Ev == "any-unmet") Once(Errors, Tag + ": '" + Cid + "' requires (ANY) something this row does not contain");
                if (Ev == "not-hit")   Once(Errors, Tag + ": its resolution contains '" + Cid + "', which a NOT excludes");
            }
            //A node mentioned twice: where it counts (the first mention holds; a later one in the same list moves it)
            //is a rule, not a choice the author made — warn where the other reading would change what wins.
            for (const auto &[Ev, Cid] : Fold::DecidingMentions(Lib, Id, Inst, {}, Gs, P))
                Once(Warnings, Tag + ": '" + [&]{ const Node *M = Idx.Find(Cid); return M && !M->NodeId.empty() ? M->NodeId + "' (" + Cid + ")" : Cid + "'"; }()
                     + " is mentioned twice and " + (Ev == "held"
                     ? std::string("its FIRST mention counts — folding it at the later one would change what wins")
                     : std::string("its LATER mention in the same list counts — folding it at the first would change what wins")));
            //A DELTA needs its base beneath: content at its target, lower in the stack.
            for (size_t J = 0; J < P.Seq.size(); ++J)
            {
                const Fold::Item &D = P.Seq[J];
                if (D.Kind != "DELTA") continue;
                bool Base = false;
                for (size_t I = 0; I < J && !Base; ++I)
                    Base = P.Seq[I].Kind != "EDIT" && P.Seq[I].Target == D.Target;
                if (!Base) Once(Errors, Tag + ": DELTA '" + D.Payload + "' has no content beneath it at '" + D.Target + "' to rebuild from");
            }
            //Covered edits: an EDIT beneath a layer that replaces its file has no effect — always a mistake.
            for (size_t I = 0; I < P.Seq.size(); ++I)
            {
                if (P.Seq[I].Kind != "EDIT") continue;
                for (size_t J = I + 1; J < P.Seq.size(); ++J)
                    if (P.Seq[J].Kind != "EDIT" && Covers(P.Seq, J, P.Seq[I].Target, NameCache))
                    {
                        Once(Errors, Tag + ": the EDIT of '" + P.Seq[I].Target + "' (from '" + P.Seq[I].From + "') lies beneath '"
                             + P.Seq[J].Payload + "', which replaces that file — move the EDIT above it");
                        break;
                    }
            }
        }
        if (!N.IsVariant()) continue;
        const Fold::Plan P = Fold::Resolve(Lib, Id);
        //Every entry is a way to run the user can pick, so every one is checked.
        for (const auto &[Lbl, E] : N.Entries.items())
        {
            if (E.contains("GUEST") && E["GUEST"].is_array() && !E["GUEST"].empty()) continue;
            const std::string Host = E.value("HOST", std::string());
            if (Host.empty()) { Warnings.push_back(Tag + ": entry '" + Lbl + "' has no HOST platform"); continue; }
            if (!HasRunnerFor(Host)) Warnings.push_back(Tag + ": no runner serves platform '" + Host + "' on this machine");
            //EXE must case-EXACTLY match a real content file: the mount is case-sensitive. Checked where the content
            //is local and the path is plain (a %variable% or a guest drive is placed at launch).
            std::string Cp = E.value("EXE", std::string());
            if (Cp.empty() || Cp.find('%') != std::string::npos || Cp.find(':') != std::string::npos) continue;
            std::replace(Cp.begin(), Cp.end(), '\\', '/');
            while (Cp.rfind("./", 0) == 0) Cp.erase(0, 2);
            while (!Cp.empty() && Cp.front() == '/') Cp.erase(Cp.begin());
            std::set<std::string> Files; bool AnyLocal = false, AllLocal = true;
            GatherLaunchContentFiles(P, ZipCache, Files, AnyLocal, AllLocal);
            if (!AnyLocal || Files.count(Cp)) continue;
            const std::string CpL = ToLowerAscii(Cp);
            std::string Hit;
            for (const std::string &F : Files) if (ToLowerAscii(F) == CpL) { Hit = F; break; }
            if (!Hit.empty())
                Errors.push_back(Tag + ": EXE '" + Cp + "' case-mismatches content file '" + Hit + "' — the mount is case-sensitive");
            else if (AllLocal)
                Warnings.push_back(Tag + ": EXE '" + Cp + "' is not in the package content");
        }
        FindCrossLayerCaseCollisions(P, ZipCache, PrepCache, Errors, CrossLayerSeen);
    }

    //----- WHEN: a malformed condition fail-opens (silently always-applies); and a LAYER's WHEN may only read what
    //phase 1 resolves — instance values, built-ins, and ungated VARS defaults (§4.1). -----
    static const std::set<std::string> Builtins = []{
        std::set<std::string> B = { "REL", "DefaultPfxDir", "WineFontsDir", "WineLibDir", "WineSys32Dir", "WineSysWow64Dir",
                                    "SysRegMtime", "UID", "VIDYAGOD_SANDBOX", "VIDYAGOD_SANDBOX_NET", "VIDYAGOD_SELF_VIP",
                                    "VIDYAGOD_SELF_NAME", "VIDYAGOD_SUBNET", "VIDYAGOD_PEER_VIPS", "VIDYAGOD_PEER_NAMES",
                                    "VIDYAGOD_LAN_BRIDGE", "VIDYAGOD_LAN_HOSTRELAY" };
        for (const auto &[K, V] : ContainerParams(std::filesystem::path(), std::string(), std::string()).GetVariablesMap())
            B.insert(K);
        return B;
    }();
    std::set<std::string> Declared, DeclaredUngated;
    std::map<std::string, std::string> UiDeclared;                       // option KEY -> its declaring node
    //A guest-root anchor a runner declares (GUEST_ROOTS {"%UserProfile%": ...}) is a canonical coordinate the launch
    //maps to where the runner puts it — not a variable, and never a typo.
    std::set<std::string> Anchors;
    for (const auto &[Id, N] : Idx.Nodes)
        for (const auto &L : N.Json["LAYERS"])
            if (L.contains("EXEC") && L["EXEC"].is_array())
                for (const auto &E : L["EXEC"])
                    if (E.is_object() && E.contains("GUEST_ROOTS") && E["GUEST_ROOTS"].is_object())
                        for (const auto &[A, To] : E["GUEST_ROOTS"].items())
                            if (A.size() > 2 && A.front() == '%' && A.back() == '%') Anchors.insert(A.substr(1, A.size() - 2));
    for (const auto &[Id, N] : Idx.Nodes)
    {
        if (!N.LowerError.empty()) continue;
        for (const auto &L : N.Json["LAYERS"])
            if (L.contains("VARS"))
                for (const auto &[K, D] : L["VARS"].items())
                {
                    Declared.insert(K);
                    if (!L.contains("WHEN")) DeclaredUngated.insert(K);
                    if (D.contains("UI")) UiDeclared.emplace(K, Id);
                }
    }
    const std::regex Tok(R"(%([A-Za-z0-9_]+)(?::[A-Za-z0-9]+)?%)");     // %KEY% or %KEY:format%
    std::set<std::string> ReferencedAll;
    for (const auto &[Id, N] : Idx.Nodes)
    {
        if (!N.LowerError.empty()) continue;
        const std::string Tag = "node '" + Id + "'";
        for (const auto &L : N.Json["LAYERS"])
        {
            std::vector<std::string> Conds;
            if (L.contains("WHEN")) Conds.push_back(L["WHEN"].get<std::string>());
            if (L.contains("VARS")) for (const auto &[K, D] : L["VARS"].items()) if (D.contains("WHEN")) Conds.push_back(D["WHEN"].get<std::string>());
            for (const auto &C : Conds)
                if (!VarSubst::ConditionParses(C) && InScope(Id))
                    Errors.push_back(Tag + ": malformed WHEN \"" + C + "\" — it would silently always-apply. Grammar: %KEY% == value, && || ! ( ).");
            if (L.contains("WHEN") && InScope(Id))
            {
                const std::string W = L["WHEN"].get<std::string>();
                for (auto It = std::sregex_iterator(W.begin(), W.end(), Tok); It != std::sregex_iterator(); ++It)
                {
                    const std::string K = (*It)[1].str();
                    if (!Builtins.count(K) && Declared.count(K) && !DeclaredUngated.count(K))
                        Errors.push_back(Tag + ": a layer WHEN reads %" + K + "%, which is only declared in WHEN-gated "
                                         "layers — a layer condition may only read ungated variables (§4.1)");
                }
            }
        }
        //Undefined %KEY% references (typos) — over everything the node writes and runs.
        const std::string Scan = N.Json["LAYERS"].dump();
        std::set<std::string> Refs;
        for (auto It = std::sregex_iterator(Scan.begin(), Scan.end(), Tok); It != std::sregex_iterator(); ++It)
            Refs.insert((*It)[1].str());
        for (const std::string &R : Refs)
        {
            ReferencedAll.insert(R);
            if (!Builtins.count(R) && !Declared.count(R) && !Anchors.count(R) && InScope(Id))
                Errors.push_back(Tag + ": references undefined variable %" + R + "% — no VARS declares it (typo?)");
        }
    }
    if (!OnlyNodes)
        for (const auto &[K, Owner] : UiDeclared)
            if (!ReferencedAll.count(K) && !Idx.Find(Owner)->Json.dump().empty())
            {
                //An option read by nothing is a dead knob — unless it is a bool option gating a layer (its reader is
                //that WHEN, which the scan above already counts).
                Warnings.push_back("node '" + Owner + "': option %" + K + "% has a UI but is referenced nowhere (dead knob)");
            }

    //----- KEEP lint: what the user owns persists under a NAMED dir beside the instance's own state. -----
    for (const auto &[Id, N] : Idx.Nodes)
    {
        if (!InScope(Id) || !N.LowerError.empty()) continue;
        const std::string Tag = "node '" + Id + "'";
        for (const auto &L : N.Layers)
        {
            if (LayerType(L) != "DeclarePersist") continue;
            const std::string Path = L.value("PATH", std::string());
            if (Path.empty())
                Warnings.push_back(Tag + ": a KEEP of a whole namespace persists ALL of it — prefer named addresses");
            if (L.value("SCOPE", std::string()) == "file" && L.contains("TARGET") && L["TARGET"].is_string())
            {
                const std::string T = ToLowerAscii(L["TARGET"].get<std::string>());
                if (T == "instance.json" || T == "registry" || T == "regkeys")
                    Warnings.push_back(Tag + ": KEEP NAME '" + L["TARGET"].get<std::string>()
                                       + "' is RESERVED for the instance's own state — it will be refused at launch");
            }
        }
    }
}



// ----- VFS layer helpers -----

bool ZipFullyStored(const std::string &ZipPath, std::string *FirstCompressed)
{
    int Err = 0; bool AllStored = true;
    if (zip_t *Za = zip_open(ZipPath.c_str(), ZIP_RDONLY, &Err))
    {
        const zip_int64_t N = zip_get_num_entries(Za, 0);
        for (zip_uint64_t i = 0; i < (zip_uint64_t)N; ++i)
        {
            zip_stat_t St; zip_stat_init(&St);
            if (zip_stat_index(Za, i, 0, &St) != 0) continue;
            if ((St.valid & ZIP_STAT_COMP_METHOD) && St.comp_method != ZIP_CM_STORE)
            { AllStored = false; if (FirstCompressed) *FirstCompressed = St.name ? St.name : ""; break; }
        }
        zip_close(Za);
    }
    return AllStored;
}

//The vidyagodfs mount-spec "type" for a package layer TYPE ("zip"/"dir"/"file"/"delta"), or "" if not a VFS layer.
//THE single source of truth for the layer→spec kind: every mount-spec builder (game content, inner-runner nesting,
//runner build, DEFPREFIX gen) maps through here, so none can silently drop a layer kind (a real bug once: the runner
//builders omitted VFSDeltaLayer → delta-chained runners served only their base).
std::string VfsSpecType(const std::string &Type)
{
    return Type == "VFSZipLayer"  ? "zip" : Type == "VFSDirLayer"   ? "dir"
         : Type == "VFSFileLayer" ? "file": Type == "VFSDeltaLayer" ? "delta" : "";
}


bool IsVfsLayer(const std::string &Type) { return !VfsSpecType(Type).empty(); }

std::string LayerPathString(const nlohmann::ordered_json &Sub)
{
    if (!Sub.is_object()) return {};
    std::string P = Sub.value("PATH", std::string());
    if (Sub.contains("SOURCE") && Sub["SOURCE"].is_object()) P = Sub["SOURCE"].value("PATH", P);
    return P;
}

std::vector<std::string> PathVariableTokens(const std::string &P)
{
    //A %variable% is a MATCHED PAIR around an IDENTIFIER, not merely the presence of a '%'. A filename may
    //legitimately contain one (URL-escaped names are common in scraped content: "100%25%20done.zip" has a
    //matched pair around "25"), and treating that as a variable silently excluded a real file from hydration,
    //verification AND a runner's build.
    std::vector<std::string> Out;
    for (size_t I = P.find('%'); I != std::string::npos; I = P.find('%', I + 1))
    {
        const size_t Close = P.find('%', I + 1);
        if (Close == std::string::npos) break;
        if (Close > I + 1 && P.find('/', I) > Close)
        {
            const std::string Tok = P.substr(I + 1, Close - I - 1);
            if ((std::isalpha((unsigned char)Tok[0]) || Tok[0] == '_')
                && std::all_of(Tok.begin(), Tok.end(),
                               [](unsigned char C) { return std::isalnum(C) || C == '_'; }))
                Out.push_back(Tok);
        }
        I = Close;
    }
    return Out;
}

bool IsRuntimeSourcedLayer(const nlohmann::ordered_json &Sub)
{
    return !PathVariableTokens(LayerPathString(Sub)).empty();
}

bool IsRunnerBuildLayer(const nlohmann::ordered_json &Sub)
{
    return IsVfsLayer(LayerType(Sub)) && !IsRuntimeSourcedLayer(Sub);
}

std::string NormalizeTargetPath(std::string P)
{
    for (char &c : P) if (c == '\\') c = '/';
    //Collapse repeated separators. A TARGET is authored by concatenation — "%PrefixRoot%/drive_c/..." — and
    //%PrefixRoot% is EMPTY for wine-at-root and "pfx" for proton, so composing it routinely yields "//". Left
    //alone, "pfx//drive_c/x" and "pfx/drive_c/x" are two DIFFERENT mount targets naming the same directory, and
    //a layer silently lands somewhere nothing reads.
    std::string Out;
    Out.reserve(P.size());
    for (char c : P)
        if (!(c == '/' && !Out.empty() && Out.back() == '/')) Out.push_back(c);
    P.swap(Out);
    while (!P.empty() && P.front() == '/') P.erase(P.begin());
    while (!P.empty() && P.back()  == '/') P.pop_back();
    return P;
}

std::string LayerType(const nlohmann::ordered_json &Sub)
{
    return Sub.is_object() ? Sub.value("TYPE", std::string()) : std::string();
}

//Build ONE vidyagodfs spec-layer object from a package VFS layer `Sub`, with caller-resolved `Source`/`Target` (and,
//for a delta, its resolved byte-BASES in order). Returns a null json if `Sub` is not a VFS layer (caller skips).
//Centralizes the entry skeleton + the delta base fields so every mount builder stays byte-for-byte consistent. The
//source/target resolution stays at the call site because it genuinely differs (package path vs per-link base prefix
//vs var-subst).
nlohmann::ordered_json MakeVfsSpecLayer(const nlohmann::ordered_json &Sub, const std::string &Source,
                                        const std::string &Target, const std::vector<std::string> &BaseTargets)
{
    const std::string LType = VfsSpecType(Sub.value("TYPE", std::string()));
    if (LType.empty()) return nullptr;
    nlohmann::ordered_json J = {{"type", LType}, {"source", Source}, {"target", Target},
                                {"submounts", Sub.value("SUBMOUNTS", nlohmann::ordered_json::array())}, {"rw", false}};
    //ONE base is the singular key the FS has always read; SEVERAL is the multi-base form it stitches with a
    //ConcatByteSource. ONE key for one base or many: "" is a legitimate target (the VFS root), so a singular
    //key could not distinguish a base DECLARED at the root from no base at all — the FS would silently fall back
    //to the delta's own target, reconstruct against the wrong bytes, and skip the layer.
    //No delta-only check here: whether a layer HAS a byte-base is LayerBaseTargets' single answer, and every
    //caller's vector comes from it. A second gate here would be redundant, and a redundant gate is one that no
    //test can pin — remove either and the suite stays green, which is how a guard quietly stops guarding.
    if (!BaseTargets.empty()) J["baseTargets"] = BaseTargets;
    //RUNTIME-SOURCED: this layer reads from a path that only exists once something ELSE is mounted (a prefix
    //assembly layer under %RunnerMount%). Recorded here because it is the last place that still has the node —
    //by the time a plan is inspected the source is a resolved absolute path indistinguishable from any other.
    //A caller that is not mounting (--audit-packages builds 960 plans) must not report these as missing.
    if (IsRuntimeSourcedLayer(Sub)) J["runtimeSourced"] = true;
    return J;
}

//Does this string still contain an unresolved %TOKEN%? The resolver leaves reference cycles and typos literal,
//and a literal %token% reaching a command line, a mount target or a file path is never what the author meant.
//ONE definition: the mount builder used to ask "is there a '%' in here" and the audit asked this, so a target
//like "save50%" was an error on every launch and clean in the audit, and "%a-b%" was the reverse — two opinions
//about the same question, one per call site, which is the shape of every bug this subsystem keeps producing.
bool HasLiveToken(const std::string &S)
{
    //A SURVIVING '%' is the signal, not a well-formed %IDENT% pair. Requiring the pair shape silently excused
    //the malformed cases — "%PrefixRoot/drive_c/%UID%" (missing close), "%Prefix Root%", "%X-TARGET%" — which
    //are precisely the typos substitution cannot resolve and therefore the ones that reach a real path literally.
    //The pair shape now only decides how the message is PHRASED, not whether there is a problem.
    return S.find('%') != std::string::npos;
}

std::vector<std::string> LayerBaseTargets(const nlohmann::ordered_json &Sub)
{
    std::vector<std::string> Out;
    //Accepts BOTH shapes a delta is written in — the LOWERED layer (TYPE "VFSDeltaLayer") and the raw batched
    //LAYERS ENTRY it came from (FORM "delta", no TYPE key) — because the key and its meaning are identical in
    //both and the editor reads layer entries while the mounter reads lowered layers. Splitting that into two
    //readers is how this question came to be answered differently in four places to begin with.
    if (!Sub.is_object()) return Out;
    const std::string T = LayerType(Sub);
    const bool IsDelta = (T == "VFSDeltaLayer")
                      || (Sub.value("FORM", std::string()) == "delta");   // a raw LAYERS entry carries FORM, no TYPE
    if (!IsDelta) return Out;                                                // only a delta has a byte-base
    //ONE key, always a list. A one-entry list is the ordinary cross-target delta; several are a concatenation.
    //A malformed shape yields NOTHING and says so, rather than a SHORTER list: the bases are concatenated, so a
    //missing entry is a base of the wrong length — the delta fails its size check, the FS drops the layer, and
    //the content is gone. vidyagodfs refuses the same shape for the same reason (layerspec.cpp); a reader that
    //quietly patched it up here would hand the mounter a plan the FS would never have accepted.
    if (!Sub.contains("BASE_TARGETS")) return Out;
    const auto &B = Sub["BASE_TARGETS"];
    const bool WellFormed = B.is_array() && [&]{ for (const auto &E : B) if (!E.is_string()) return false; return true; }();
    if (!WellFormed)
    {
        LogErr("ManifestModel::LayerBaseTargets",
               "Layer " + Sub.value("TYPE", std::string("?")) + " '" + Sub.value("PATH", std::string("?"))
                   + "': BASE_TARGETS must be an array of strings — the delta's byte-base is IGNORED, so it will "
                     "reconstruct against its own target or be skipped entirely.");
        return Out;
    }
    for (const auto &E : B) Out.push_back(E.get<std::string>());
    return Out;
}

void ForEachVfsLayer(const nlohmann::ordered_json &Components,
                     const std::function<void(const nlohmann::ordered_json &)> &Fn)
{
    if (!Components.is_array()) return;
    for (const auto &C : Components)
    {
        if (!C.is_object() || !C.contains("SUBCOMPONENTS") || !C["SUBCOMPONENTS"].is_array()) continue;
        for (const auto &S : C["SUBCOMPONENTS"])
            if (IsVfsLayer(LayerType(S))) Fn(S);
    }
}

void LayerLocator(const nlohmann::ordered_json &Sub, const std::filesystem::path &PackagePath,
                  std::filesystem::path &Local, std::string &Cid)
{
    std::string PathStr = Sub.value("PATH", std::string());
    Cid.clear();
    if (Sub.contains("SOURCE") && Sub["SOURCE"].is_object())
    {
        const auto &Src = Sub["SOURCE"];
        PathStr = Src.value("PATH", PathStr);                                  // SOURCE.PATH overrides the local path
        if (Src.value("TYPE", std::string("path")) == "ipfs") Cid = Src.value("CID", std::string());
    }
    std::filesystem::path P(PathStr);
    Local = P.is_absolute() ? P : (PackagePath / P);
}

std::string ResolveLayerSource(const nlohmann::ordered_json &Sub, const std::filesystem::path &PackagePath)
{
    std::filesystem::path Local; std::string Cid;
    LayerLocator(Sub, PackagePath, Local, Cid);
    return Local.string();
}

// ----- platform -----

// The host platform a runner variant's HOST_PLATFORM must match, and which native content runs directly
// through the appended native terminal (win64 games on Windows, linux64 on Linux).
#ifdef _WIN32
std::string MachinePlatform() { return "win64"; }
std::string HostPlatform()    { return "win64"; }
#else
std::string MachinePlatform() { return "linux64"; }
std::string HostPlatform()    { return "linux64"; }
#endif

// ----- game / component / variant / module lookups -----

int FindGameIndex(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID)
{
    for (int i = 0; i < (int)MANIFESTJSON["GAMES"].size(); i++)
        if (!MANIFESTJSON["GAMES"][i]["GAMEID"].is_null() && MANIFESTJSON["GAMES"][i]["GAMEID"] == SubgameID)
            return i;
    LogErr("ManifestModel::FindGameIndex", "Subgame ID not found: " + SubgameID);
    return -1;
}

int FindComponentIndex(const nlohmann::ordered_json &MANIFESTJSON, const std::string &ComponentID)
{
    if (ComponentID.empty()) return -1;
    for (int i = 0; i < (int)MANIFESTJSON["COMPONENTS"].size(); i++)
        if (!MANIFESTJSON["COMPONENTS"][i]["COMPONENTID"].is_null() && MANIFESTJSON["COMPONENTS"][i]["COMPONENTID"] == ComponentID)
            return i;
    LogErr("ManifestModel::FindComponentIndex", "Component ID not found: " + ComponentID);
    return -1;
}

std::vector<ModuleInfo> ParseModules(const nlohmann::ordered_json &ModulesArray)
{
    std::vector<ModuleInfo> Modules;
    if (!ModulesArray.is_array()) return Modules;
    for (const auto &M : ModulesArray)
    {
        if (!M.is_object()) continue;
        ModuleInfo Info;
        Info.Component = M.value("COMPONENT", std::string());
        if (Info.Component.empty()) continue;
        Info.Label    = M.value("LABEL", std::string());
        Info.Required = M.value("REQUIRED", true);
        Info.Default  = M.value("DEFAULT", true);
        if (M.contains("EXCLUDE") && M["EXCLUDE"].is_array())
            for (const auto &E : M["EXCLUDE"])
                if (E.is_string() && !std::string(E).empty()) Info.Exclude.push_back(std::string(E));
        Modules.push_back(std::move(Info));
    }
    return Modules;
}

std::vector<VariantInfo> GetAvailableVariants(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID)
{
    std::vector<VariantInfo> Variants;
    int SubgameIdx = FindGameIndex(MANIFESTJSON, SubgameID);
    if (SubgameIdx == -1) return Variants;
    auto &Subgame = MANIFESTJSON["GAMES"][SubgameIdx];
    if (!Subgame.contains("VARIANTS") || !Subgame["VARIANTS"].is_array()) return Variants;
    for (auto &V : Subgame["VARIANTS"])
    {
        VariantInfo Info;
        Info.VariantID     = V.value("VARIANT_ID", std::string());
        Info.Name          = V.value("NAME", Info.VariantID);
        Info.IsRecommended = V.value("RECOMMENDED", false);
        Info.HostPlatform  = V.value("HOST_PLATFORM", std::string());
        if (V.contains("GUEST_PLATFORM") && V["GUEST_PLATFORM"].is_array())   // runner variants only
            for (const auto &P : V["GUEST_PLATFORM"]) if (P.is_string()) Info.GuestPlatform.push_back(std::string(P));
        Info.Modules       = ParseModules(V.value("MODULES", nlohmann::ordered_json::array()));
        Variants.push_back(std::move(Info));
    }
    return Variants;
}

std::vector<ModuleInfo> GetVariantModules(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID, const std::string &VariantID)
{
    int SubgameIdx = FindGameIndex(MANIFESTJSON, SubgameID);
    if (SubgameIdx == -1) return {};
    auto &Subgame = MANIFESTJSON["GAMES"][SubgameIdx];
    if (!Subgame.contains("VARIANTS") || !Subgame["VARIANTS"].is_array()) return {};
    for (auto &V : Subgame["VARIANTS"])
        if (V.value("VARIANT_ID", std::string()) == VariantID)
            return ParseModules(V.value("MODULES", nlohmann::ordered_json::array()));
    return {};
}

std::vector<std::string> ResolveEnabledModules(const std::vector<ModuleInfo> &Modules,
                                               const std::map<std::string, bool> &ModuleStates,
                                               const nlohmann::ordered_json &MANIFESTJSON)
{
    std::map<std::string, bool> InModules; // component → is a module here
    for (const auto &M : Modules) InModules[M.Component] = true;

    //All module-ancestors of a component (PARENTCOMPONENT chain entries that are themselves modules).
    auto ModuleAncestors = [&](const std::string &Component) {
        std::vector<std::string> Ancestors;
        int Idx = FindComponentIndex(MANIFESTJSON, Component);
        while (Idx != -1)
        {
            const auto &Comp = MANIFESTJSON["COMPONENTS"][Idx];
            if (!Comp.contains("PARENTCOMPONENT")) break;          // a component may have no parent (root / runner build)
            const auto &ParentField = Comp["PARENTCOMPONENT"];
            if (ParentField.is_null() || ParentField == "") break;
            std::string Parent = ParentField;
            if (InModules.count(Parent)) Ancestors.push_back(Parent);
            Idx = FindComponentIndex(MANIFESTJSON, Parent);
        }
        return Ancestors;
    };

    //Step 1: raw enable.
    std::map<std::string, bool> Enabled;
    for (const auto &M : Modules)
        Enabled[M.Component] = M.Required ? true
                             : (ModuleStates.count(M.Component) ? ModuleStates.at(M.Component) : M.Default);

    //Step 2: a REQUIRED module forces all its module-ancestors enabled (a required child keeps its parents).
    for (const auto &M : Modules)
        if (M.Required)
            for (const auto &A : ModuleAncestors(M.Component)) Enabled[A] = true;

    //Step 2.5: mutual exclusion (EXCLUDE) — symmetric; REQUIRED kept first, then each still-enabled optional in
    //declaration order is dropped if it conflicts with anything kept (first-declared of a set survives).
    std::map<std::string, std::set<std::string>> ExclAdj;
    for (const auto &M : Modules)
        for (const auto &E : M.Exclude) { ExclAdj[M.Component].insert(E); ExclAdj[E].insert(M.Component); }
    if (!ExclAdj.empty())
    {
        std::set<std::string> Kept;
        auto Conflicts = [&](const std::string &C) {
            auto it = ExclAdj.find(C);
            if (it == ExclAdj.end()) return false;
            for (const auto &K : Kept) if (it->second.count(K)) return true;
            return false;
        };
        for (const auto &M : Modules) if (Enabled[M.Component] && M.Required)  Kept.insert(M.Component);
        for (const auto &M : Modules)
        {
            if (!Enabled[M.Component] || M.Required) continue;
            if (Conflicts(M.Component)) Enabled[M.Component] = false;
            else                        Kept.insert(M.Component);
        }
    }

    //Step 3: hierarchy gate — drop any module with a disabled module-ancestor (full chain).
    std::vector<std::string> EnabledComponents;
    for (const auto &M : Modules)
    {
        if (!Enabled[M.Component]) continue;
        bool AncestorsOk = true;
        for (const auto &A : ModuleAncestors(M.Component))
            if (!Enabled[A]) { AncestorsOk = false; break; }
        if (AncestorsOk) EnabledComponents.push_back(M.Component);
    }
    return EnabledComponents;
}

std::vector<std::string> FindEndpointsForVariant(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID, const std::string &VariantID)
{
    return ResolveEnabledModules(GetVariantModules(MANIFESTJSON, SubgameID, VariantID), {}, MANIFESTJSON);
}

// ----- manifest-only package predicates -----

std::vector<std::string> PackageIpfsCids(const nlohmann::ordered_json &Manifest)
{
    std::vector<std::string> Cids;
    std::set<std::string> Seen;
    if (!Manifest.contains("COMPONENTS")) return Cids;
    ForEachVfsLayer(Manifest["COMPONENTS"], [&](const nlohmann::ordered_json &S){
        if (!S.contains("SOURCE") || !S["SOURCE"].is_object()) return;
        const auto &Src = S["SOURCE"];
        if (Src.value("TYPE", std::string()) != "ipfs") return;
        const std::string Cid = Src.value("CID", std::string());
        if (!Cid.empty() && Seen.insert(Cid).second) Cids.push_back(Cid);
    });
    return Cids;
}

std::vector<std::string> PackageCoverCids(const nlohmann::ordered_json &Manifest)
{
    std::vector<std::string> Cids;
    std::set<std::string> Seen;
    if (!Manifest.contains("GAMES") || !Manifest["GAMES"].is_array()) return Cids;
    auto Consider = [&](const nlohmann::ordered_json &Holder)
    {
        if (!Holder.contains("COVER") || !Holder["COVER"].is_object()) return;
        const auto &Cv = Holder["COVER"];
        if (!Cv.contains("SOURCE") || !Cv["SOURCE"].is_object() || Cv["SOURCE"].value("TYPE", std::string()) != "ipfs") return;
        const std::string Cid = Cv["SOURCE"].value("CID", std::string());
        if (!Cid.empty() && Seen.insert(Cid).second) Cids.push_back(Cid);
    };
    for (const auto &G : Manifest["GAMES"])
    {
        if (!G.is_object()) continue;
        if (G.contains("METADATA") && G["METADATA"].is_object()) Consider(G["METADATA"]);
        Consider(G);
    }
    return Cids;
}

bool PackageHydrated(const nlohmann::ordered_json &Manifest, const std::string &PackageDir)
{
    if (!Manifest.contains("COMPONENTS")) return true;
    const std::filesystem::path Pkg(PackageDir);
    bool AllPresent = true;
    ForEachVfsLayer(Manifest["COMPONENTS"], [&](const nlohmann::ordered_json &S){
        if (!AllPresent) return;
        std::filesystem::path Local; std::string Cid;
        LayerLocator(S, Pkg, Local, Cid);
        std::error_code Ec;
        if (!std::filesystem::exists(Local, Ec)) AllPresent = false;            // a layer's content is missing
    });
    return AllPresent;
}

bool PackageHasContent(const nlohmann::ordered_json &Manifest)
{
    if (!Manifest.contains("COMPONENTS")) return false;
    bool Any = false;
    ForEachVfsLayer(Manifest["COMPONENTS"], [&](const nlohmann::ordered_json &){ Any = true; });
    return Any;
}

// ---------------------------------------------------------------------------
// Case-conflict resolution — the fix for FindCrossLayerCaseCollisions errors.
// Two layers in a launchable's closure contribute paths differing only in case (e.g. base 'server.dll' + patch
// 'Server.dll', or base 'CONTENT/' + add-on 'content/'). On the case-sensitive FUSE mount both exist, so the
// higher-priority layer fails to override and wine's lookup can hit the wrong file. We CANONICALIZE: within each
// closure (base-first load order) the first exact case seen for a path component is canonical; any later layer's
// mismatch is renamed to it. Only the higher-priority zips are rewritten (unpack→rename→repackage, STORE, same
// structure); the base is never touched.
// ---------------------------------------------------------------------------

// ALL entries of a zip including explicit directory entries (trailing '/' kept). ZipEntriesCached drops dirs —
// correct for content checks, but the case rewrite must canonicalize dir entries too: a renamed file leaves its
// old-case parent dir entry behind as an EMPTY husk ("maps/" next to "MAPS/…"), and wine's case-insensitive
// lookup can resolve into the husk (Halo CE: "missing bitmaps" — maps/ empty, everything under MAPS/).
static const std::vector<std::string> &ZipAllEntriesRaw(const std::string &Path,
        std::unordered_map<std::string, std::vector<std::string>> &Cache)
{
    auto It = Cache.find(Path);
    if (It != Cache.end()) return It->second;
    std::vector<std::string> Out;
    int Err = 0;
    if (zip_t *Za = zip_open(Path.c_str(), ZIP_RDONLY, &Err))
    {
        const zip_int64_t N = zip_get_num_entries(Za, 0);
        for (zip_uint64_t i = 0; i < (zip_uint64_t)N; ++i)
            if (const char *Name = zip_get_name(Za, i, ZIP_FL_ENC_RAW)) Out.emplace_back(Name);
        zip_close(Za);
    }
    return Cache.emplace(Path, std::move(Out)).first->second;
}

// Component-wise canonicalization across every launchable closure → per-zip {oldEntry -> newEntry}.
static std::map<std::string, std::map<std::string, std::string>>
ComputeCaseRenames(const NodeIndex &Idx, std::vector<std::string> &Log)
{
    std::unordered_map<std::string, std::vector<std::string>> AllCache;
    std::map<std::string, std::map<std::string, std::string>> Renames;   // zipPath -> (old -> new)

    const Fold::Library Lib = LibraryOf(Idx);
    for (const auto &[LId, LN] : Idx.Nodes)
    {
        if (!LN.IsVariant()) continue;
        std::map<std::string, std::string> Canon;   // lowercased prefix key -> canonical exact component

        const Fold::Plan Plan = Fold::Resolve(Lib, LId);
        {
            for (const Fold::Item &I : Plan.Seq)
            {
                if (I.Kind == "EDIT") continue;
                const std::filesystem::path Local = ItemLocal(I);
                std::error_code Ec;
                if (Local.empty() || !std::filesystem::exists(Local, Ec)
                    || std::filesystem::is_directory(Local, Ec)) continue;   // only rewrite ZIPs (skip dir layers)
                const std::string Target  = I.Target;
                const std::string ZipPath = Local.string();

                for (std::string Entry : ZipAllEntriesRaw(ZipPath, AllCache))
                {
                    // Explicit directory entries flow through the SAME component canonicalization (their
                    // trailing '/' stripped for the walk, restored on the rename target) — that is what
                    // retargets a stale-case dir husk onto its canonical spelling instead of leaving it behind.
                    const bool IsDir = !Entry.empty() && Entry.back() == '/';
                    if (IsDir) Entry.pop_back();
                    if (Entry.empty()) continue;
                    const std::string Full = JoinTarget(Target, Entry);
                    std::vector<std::string> Comp = SplitPath(Full);
                    std::string Prefix, NewFull;
                    bool Changed = false;
                    for (size_t k = 0; k < Comp.size(); ++k)
                    {
                        const std::string Key = Prefix.empty() ? ToLowerAscii(Comp[k]) : (Prefix + "/" + ToLowerAscii(Comp[k]));
                        auto It = Canon.find(Key);
                        std::string CanonC;
                        if (It == Canon.end()) { Canon.emplace(Key, Comp[k]); CanonC = Comp[k]; }
                        else CanonC = It->second;
                        if (CanonC != Comp[k]) Changed = true;
                        if (k) NewFull += '/';
                        NewFull += CanonC;
                        Prefix = Key;
                    }
                    if (!Changed) continue;
                    std::string NewEntry = NewFull;
                    if (!Target.empty())
                    {
                        const std::string Pfx = Target + "/";
                        if (NewFull.rfind(Pfx, 0) != 0) continue;   // defensive: Target case must be stable
                        NewEntry = NewFull.substr(Pfx.size());
                    }
                    if (IsDir) { Entry += '/'; NewEntry += '/'; }   // match the zip's dir-entry spelling
                    auto &M = Renames[ZipPath];
                    auto Ex = M.find(Entry);
                    if (Ex != M.end() && Ex->second != NewEntry)
                        Log.push_back("case-fix: conflicting canonicalization of '" + Entry + "' in " + ZipPath + " (skipped)");
                    else
                        M[Entry] = NewEntry;
                }
            }
        }
    }
    return Renames;
}

// Rewrite one zip applying entry renames — libzip copies each entry's data (STORE, no re-compress) into a temp
// archive under the new name, preserving structure/attrs/mtime, then atomically replaces the original.
static bool ApplyZipRenames(const std::string &ZipPath, const std::map<std::string, std::string> &Renames,
                            std::vector<std::string> &Log)
{
    int Err = 0;
    zip_t *Src = zip_open(ZipPath.c_str(), ZIP_RDONLY, &Err);
    if (!Src) { Log.push_back("case-fix: cannot open " + ZipPath); return false; }
    const std::string Tmp = ZipPath + ".casefix.tmp";
    zip_t *Dst = zip_open(Tmp.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &Err);
    if (!Dst) { zip_close(Src); Log.push_back("case-fix: cannot create " + Tmp); return false; }

    const zip_int64_t N = zip_get_num_entries(Src, 0);
    bool Ok = true;
    for (zip_int64_t i = 0; i < N && Ok; ++i)
    {
        const char *Name = zip_get_name(Src, (zip_uint64_t)i, 0);
        if (!Name) { Ok = false; break; }
        std::string Nm = Name;
        auto It = Renames.find(Nm);
        std::string NewNm = (It != Renames.end()) ? It->second : Nm;

        if (!Nm.empty() && Nm.back() == '/')   // explicit directory entry
        {
            std::string D = NewNm; if (!D.empty() && D.back() == '/') D.pop_back();
            // Canonicalization can map several case-variant dir entries onto ONE spelling — the collapse is the
            // point (the husk merges into the real dir), so "already exists" is success, not failure.
            if (zip_dir_add(Dst, D.c_str(), ZIP_FL_ENC_UTF_8) < 0
                && zip_error_code_zip(zip_get_error(Dst)) != ZIP_ER_EXISTS) Ok = false;
            continue;
        }
        zip_source_t *S = zip_source_zip_file(Dst, Src, (zip_uint64_t)i, 0, 0, -1, nullptr);
        if (!S) { Ok = false; break; }
        zip_int64_t Ix = zip_file_add(Dst, NewNm.c_str(), S, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE);
        if (Ix < 0) { zip_source_free(S); Ok = false; break; }
        zip_set_file_compression(Dst, (zip_uint64_t)Ix, ZIP_CM_STORE, 0);   // keep STORE (FUSE needs it)
        zip_uint8_t Ops = 0; zip_uint32_t Attr = 0;
        if (zip_file_get_external_attributes(Src, (zip_uint64_t)i, 0, &Ops, &Attr) == 0)
            zip_file_set_external_attributes(Dst, (zip_uint64_t)Ix, 0, Ops, Attr);
        zip_stat_t St; zip_stat_init(&St);
        if (zip_stat_index(Src, (zip_uint64_t)i, 0, &St) == 0 && (St.valid & ZIP_STAT_MTIME))
            zip_file_set_mtime(Dst, (zip_uint64_t)Ix, St.mtime, 0);
    }

    std::error_code Ec;
    if (!Ok) { zip_discard(Dst); zip_close(Src); std::filesystem::remove(Tmp, Ec); Log.push_back("case-fix: rewrite failed " + ZipPath); return false; }
    if (zip_close(Dst) < 0) { zip_close(Src); std::filesystem::remove(Tmp, Ec); Log.push_back("case-fix: finalize failed " + ZipPath); return false; }
    zip_close(Src);
    std::filesystem::rename(Tmp, ZipPath, Ec);   // atomic replace
    if (Ec) { Log.push_back("case-fix: replace failed " + ZipPath); return false; }
    return true;
}

int FixCaseConflicts(const NodeIndex &Idx, std::vector<std::string> &Log, const std::filesystem::path *ScopeDir)
{
    auto Renames = ComputeCaseRenames(Idx, Log);
    std::string Scope;
    if (ScopeDir) { std::error_code Ec; Scope = std::filesystem::weakly_canonical(*ScopeDir, Ec).string(); if (Ec) Scope = ScopeDir->string(); }
    int Fixed = 0;
    for (const auto &[ZipPath, M] : Renames)
    {
        if (M.empty()) continue;
        if (!Scope.empty() && ZipPath.rfind(Scope, 0) != 0) continue;   // outside the requested bundle → leave it
        Log.push_back("case-fix: " + std::filesystem::path(ZipPath).filename().string()
                      + " — " + std::to_string(M.size()) + (M.size() == 1 ? " entry" : " entries"));
        for (const auto &[O, Nw] : M) Log.push_back("    " + O + "  ->  " + Nw);
        if (ApplyZipRenames(ZipPath, M, Log)) ++Fixed;
    }
    return Fixed;
}

} // namespace ManifestModel

