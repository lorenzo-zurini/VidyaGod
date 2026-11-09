#include "nodegraph.h"
#include "manifestmodel.h"
#include "noderefs.h"
#include "parallel.h"
#include "contentfile.h"
#include "commonutils.h"
#include "fold.h"        // Fold::Resolve — a node's facts are folded, never inherited by graph distance
#include "nodelower.h"   // NodeLower — the gen-6 vocabulary check and the lowering to engine ops
#include "varsubst.h"    // VarSubst::ConditionParses — WHEN syntax lint
#include "bytesource.h"  // FdByteSource — a delta chain's zip listing (covered-edit check)
#include "vgdelta.h"     // DeltaByteSource — reconstructs a delta chain's zip on the fly
#include "zipscan.h"     // WalkCentralDir — the member names of a (reconstructed) zip
#include "hostio.h"      // HostIO::OpenRead / Close — the byte sources' fds
#include "registrywrapper.h"  // NamesNoKey — a registry KEEP must name a key
#include "platform/platform.h"  // HostFolderNames — %HostAppData% & co., set on Windows only

#include <atomic>
#include <thread>
#include <set>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <string_view>
#include <deque>
#include <cstring>
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
    // An empty key names no node: a reference not filled in yet (NODE "") would otherwise match the first node whose
    // working handle or LABEL is empty, and an editor showed an unrelated package's card for it.
    if (Key.empty()) return nullptr;
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

bool IsNodeObject(const nlohmann::ordered_json &J)
{
    return J.is_object() && J.contains("LAYERS") && J["LAYERS"].is_array();
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
    //A published (generation-6) node carries no "CID": it is NAMED by one — its file is <cid>.json. Without this every
    //such node fell back to its LABEL as its key, so the references between them (all CIDs) dangled and two nodes
    //sharing a LABEL collided.
    if (Out.Cid.empty())
    {
        const std::string Stem = File.stem().string();
        bool Cidish = Stem.size() >= 50 && Stem.rfind("bafk", 0) == 0;
        for (char C : Stem) if (!((C >= 'a' && C <= 'z') || (C >= '2' && C <= '7'))) Cidish = false;
        if (Cidish) Out.Cid = Stem;
    }
    Out.NodeId    = (J.contains("LABEL") && J["LABEL"].is_string()) ? J["LABEL"].get<std::string>() : std::string();
    Out.File      = File;
    Out.BundleDir = BundleDir;
    Out.Json      = J;
    Out.ContentLayers = nlohmann::ordered_json::array();
    Out.Entries   = nlohmann::ordered_json::object();
    //Named by its KEY (its CID), the label alongside: a message must name exactly one node, and labels repeat.
    Out.LowerError = Out.Cid.empty() ? NodeLower::CheckNode(J, Out.NodeId) : NodeLower::CheckNode(J, Out.Cid, Out.NodeId);
    Out.Refs = NodeRefs::Contained(J, &Out.Requires);
    if (!Out.LowerError.empty())
    {
        LogWarn("ManifestModel::ParseNode", "Malformed node — " + Out.LowerError + " (" + File.string() + ")");
        return true;
    }
    if (J.contains("VARIANT")) Out.Variant = J["VARIANT"].get<std::string>();
    if (J.contains("RECOMMENDED")) for (const auto &U : J["RECOMMENDED"]) Out.Recommended.push_back(U.get<std::string>());
    for (auto &L : NodeLower::LowerNode(J, Out.NodeId))
    {
        const std::string T = L.is_object() ? L.value("TYPE", std::string()) : std::string();
        if (T == "CustomVar" && !L.value("KEY", std::string()).empty())
            Out.VarDefaults.emplace_back(L.value("KEY", std::string()), L.value("DEFAULT", std::string()));
        if (IsVfsLayer(T)) Out.ContentLayers.push_back(std::move(L));
    }
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

//The node's members, written and read back in one place (the static_assert below trips when one is added on the
//platform the app is built on, so the list here is checked against the struct, not left to memory).
#if defined(__x86_64__) && defined(__GLIBCXX__) && !defined(_WIN32)
static_assert(sizeof(Node) == 624, "Node changed: add the new member to NodeToJson and NodeFromJson, then update this size");
static_assert(sizeof(NodeIndex) == 216, "NodeIndex changed: add the new member to IndexTablesToJson and IndexTablesFromJson, then update this size");
#endif

nlohmann::ordered_json NodeToJson(const Node &N)
{
    using json = nlohmann::ordered_json;
    json V = json::array();
    for (const auto &[K, D] : N.VarDefaults) V.push_back(json::array({K, D}));
    return json{{"key", N.Key()}, {"cid", N.Cid}, {"handle", N.Handle}, {"label", N.NodeId}, {"lower_error", N.LowerError},
                {"variant", N.Variant}, {"recommended", N.Recommended}, {"json", *N.Json}, {"content_layers", N.ContentLayers},
                {"var_defaults", V}, {"refs", N.Refs}, {"requires", N.Requires}, {"graft", N.IsGraft}, {"entries", N.Entries},
                {"has_exec", N.HasExec}, {"has_runner", N.HasRunner}, {"own_runner", N.OwnRunner}, {"entry", N.Label},
                {"host", N.HostPlatform}, {"guest", N.GuestPlatform}, {"exec", N.Exec}, {"faces", N.Faces}, {"uid", N.Uid},
                {"package_uid", N.PackageUid}, {"meta", N.Meta}, {"own_tile", N.OwnTile}, {"file", N.File.string()},
                {"bundle", N.BundleDir.string()}, {"received", N.Received}};
}

Node NodeFromJson(nlohmann::ordered_json J)
{
    Node N;
    const auto Strs = [&](const char *K) { return J[K].get<std::vector<std::string>>(); };
    N.Cid = J["cid"].get<std::string>(); N.Handle = J["handle"].get<std::string>(); N.NodeId = J["label"].get<std::string>();
    N.LowerError = J["lower_error"].get<std::string>(); N.Variant = J["variant"].get<std::string>();
    N.Recommended = Strs("recommended");
    N.Json = std::move(J["json"]);
    N.ContentLayers = std::move(J["content_layers"]);
    for (const auto &P : J["var_defaults"]) N.VarDefaults.emplace_back(P[0].get<std::string>(), P[1].get<std::string>());
    N.Refs = Strs("refs"); N.Requires = Strs("requires"); N.IsGraft = J["graft"].get<bool>();
    N.Entries = std::move(J["entries"]);
    N.HasExec = J["has_exec"].get<bool>(); N.HasRunner = J["has_runner"].get<bool>(); N.OwnRunner = J["own_runner"].get<bool>();
    N.Label = J["entry"].get<std::string>(); N.HostPlatform = J["host"].get<std::string>(); N.GuestPlatform = Strs("guest");
    N.Exec = std::move(J["exec"]);
    N.Faces = Strs("faces"); N.Uid = J["uid"].get<std::string>(); N.PackageUid = J["package_uid"].get<std::string>();
    N.Meta = std::move(J["meta"]);
    N.OwnTile = J["own_tile"].get<bool>();
    N.File = J["file"].get<std::string>(); N.BundleDir = J["bundle"].get<std::string>(); N.Received = J["received"].get<bool>();
    return N;
}

nlohmann::ordered_json IndexTablesToJson(const NodeIndex &Idx)
{
    using json = nlohmann::ordered_json;
    json Dup = json::object();
    for (const auto &[Id, Ps] : Idx.Duplicates) { json &D = Dup[Id] = json::array(); for (const auto &P : Ps) D.push_back(P.string()); }
    json Ref = json::array();
    for (const auto &R : Idx.RefusedNodes)
        Ref.push_back({{"handle", R.Handle}, {"file", R.File.string()}, {"bundle", R.BundleDir.string()}, {"why", R.Why}, {"too_large", R.TooLarge}});
    json Tiles = json::object();
    for (const auto &[U, T] : Idx.Tiles) Tiles[U] = T;
    json Graft = json::object();
    for (const auto &[U, G] : Idx.TileGraft) Graft[U] = G;
    return json{{"tiles", Tiles}, {"tile_graft", Graft}, {"duplicates", Dup}, {"refused", Ref}};
}

void IndexTablesFromJson(const nlohmann::ordered_json &J, NodeIndex &Idx)
{
    for (const auto &[U, T] : J["tiles"].items()) Idx.Tiles[U] = T;
    for (const auto &[U, G] : J["tile_graft"].items()) Idx.TileGraft[U] = G.get<std::string>();
    for (const auto &[Id, Ps] : J["duplicates"].items()) for (const auto &P : Ps) Idx.Duplicates[Id].push_back(P.get<std::string>());
    for (const auto &R : J["refused"])
    {
        NodeIndex::Refused X;
        X.Handle = R["handle"].get<std::string>(); X.File = R["file"].get<std::string>(); X.BundleDir = R["bundle"].get<std::string>();
        X.Why = R["why"].get<std::string>(); X.TooLarge = R["too_large"].get<bool>();
        Idx.RefusedNodes.push_back(std::move(X));
    }
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

nlohmann::ordered_json Node::LoweredLayers() const
{
    return LowerError.empty() ? NodeLower::LowerNode(*Json, NodeId) : nlohmann::ordered_json::array();
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
        nlohmann::ordered_json J;
        if (!NodeGraph::ReadTreeJsonBounded(Entry.path(), J))
        { LogWarn("ManifestModel::ScanBundleNodes", "Skipping unparseable " + Entry.path().string()); continue; }
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
        if (N.LowerError.empty()) L.Nodes.emplace(K, Fold::Library::Entry{ &*N.Json, N.BundleDir.string() });
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
    Parallel::For(Work.size(), [&](size_t I) {
        Node &N = *Work[I].second;
        N.Entries = N.LowerError.empty() ? Fold::ResolveEntries(Lib, *Work[I].first).Exec : nlohmann::ordered_json::object();
    });
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
    Idx.Tiles.clear();
    for (const auto &[Uid, T] : Tiles) Idx.Tiles[Uid] = FlatTile(T);
    Idx.TileGraft.clear();
    for (const auto &[K, N] : Idx.Nodes)
        if (N.IsGraft && N.Variant.empty())
            for (const std::string &U : N.Faces) Idx.TileGraft.emplace(U, K);
    for (auto &[K, N] : Idx.Nodes)
    {
        if (N.Faces.empty()) continue;
        N.Uid = N.Faces.front();
        N.PackageUid = Root(N.Uid);
        N.Meta = Idx.Tiles[N.Uid];
    }
}

//Node -> the nodes naming it, over the whole index: what GraftBases climbs.
static std::map<std::string, std::vector<const std::string *>> ContainedByMap(const NodeIndex &Idx)
{
    std::map<std::string, std::vector<const std::string *>> ContainedBy;
    for (const auto &[Id, N] : Idx.Nodes) for (const std::string &R : N.Refs) ContainedBy[R].push_back(&Id);
    return ContainedBy;
}

static std::vector<std::string> GraftBasesWith(const NodeIndex &Idx, const std::string &Graft,
                                               const std::map<std::string, std::vector<const std::string *>> &ContainedBy)
{
    std::vector<std::string> Out;
    const Node *G = Idx.Find(Graft);
    if (!G || !G->IsGraft || !G->Json->contains("LAYERS") || (*G->Json)["LAYERS"].empty()) return Out;
    const nlohmann::ordered_json &First = (*G->Json)["LAYERS"][0];
    std::set<std::string> Seen;
    std::vector<std::string> Todo;
    if (First.contains("ANY") && First["ANY"].is_array())
        for (const auto &A : First["ANY"]) if (A.is_string()) Todo.push_back(A.get<std::string>());
    std::vector<const Node *> Bases;
    while (!Todo.empty())
    {
        const std::string X = Todo.back(); Todo.pop_back();
        if (!Seen.insert(X).second) continue;
        if (const Node *V = Idx.Find(X); V && V->IsVariant() && !V->IsGraft) Bases.push_back(V);
        if (const auto It = ContainedBy.find(X); It != ContainedBy.end()) for (const std::string *P : It->second) Todo.push_back(*P);
    }
    std::sort(Bases.begin(), Bases.end(), [](const Node *A, const Node *B) {
        return A->Variant != B->Variant ? A->Variant < B->Variant : A->Key() < B->Key(); });
    for (const Node *B : Bases) Out.push_back(B->Key());
    return Out;
}

std::vector<std::string> GraftBases(const NodeIndex &Idx, const std::string &Graft)
{
    return GraftBasesWith(Idx, Graft, ContainedByMap(Idx));
}

std::map<std::string, std::vector<std::string>> AllGraftBases(const NodeIndex &Idx)
{
    const auto ContainedBy = ContainedByMap(Idx);   // once for every graft, not once per graft
    std::map<std::string, std::vector<std::string>> Out;
    for (const auto &[Id, N] : Idx.Nodes) if (N.IsGraft) Out.emplace(Id, GraftBasesWith(Idx, Id, ContainedBy));
    return Out;
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

    auto CarriesContent = [](const Node *N) { return !N->ContentLayers.empty(); };

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
            if (N->Json->contains("LAYERS"))
                for (const auto &L : (*N->Json)["LAYERS"])
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


bool IsRuntimeSourcedLayer(const nlohmann::ordered_json &Sub)
{
    return VarSubst::RuntimeSourced(LayerPathString(Sub));
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

//Build ONE vidyagodfs spec-layer object from a package VFS layer `Sub`, with caller-resolved `Source`/`Target`.
//Returns a null json if `Sub` is not a VFS layer (caller skips). The source/target resolution stays at the call site
//because it genuinely differs (package path vs per-link base prefix vs var-subst).
nlohmann::ordered_json MakeVfsSpecLayer(const nlohmann::ordered_json &Sub, const std::string &Source,
                                        const std::string &Target)
{
    const std::string LType = VfsSpecType(Sub.value("TYPE", std::string()));
    if (LType.empty()) return nullptr;
    nlohmann::ordered_json J = {{"type", LType}, {"source", Source}, {"target", Target},
                                {"submounts", Sub.value("SUBMOUNTS", nlohmann::ordered_json::array())}, {"rw", false}};
    if (LType == "zip" || LType == "delta")
    {
        std::filesystem::path Local; std::string Cid;
        LayerLocator(Sub, std::filesystem::path(), Local, Cid);
        if (!Cid.empty()) J["cid"] = Cid;
        if (Sub.contains("BYTES_ONLY") && Sub["BYTES_ONLY"].is_boolean() && Sub["BYTES_ONLY"].get<bool>()) J["bytesOnly"] = true;
    }
    if (LType == "delta") J["bases"] = LayerBaseSources(Sub);   // by CID: the FS finds each file (treeplan.h)
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

std::vector<std::string> LayerBaseSources(const nlohmann::ordered_json &Sub)
{
    std::vector<std::string> Out;
    if (!Sub.is_object() || !Sub.contains("BASE_SOURCES")) return Out;
    const bool IsDelta = LayerType(Sub) == "VFSDeltaLayer" || Sub.value("FORM", std::string()) == "delta";
    if (!IsDelta) return Out;
    const auto &B = Sub["BASE_SOURCES"];
    const bool WellFormed = B.is_array() && [&]{ for (const auto &E : B) if (!E.is_string()) return false; return true; }();
    if (!WellFormed)
    {
        LogErr("ManifestModel::LayerBaseSources",
               "Layer " + Sub.value("TYPE", std::string("?")) + " '" + Sub.value("PATH", std::string("?"))
                   + "': BASE_SOURCES must be an array of CIDs — the delta names no base, so it will not mount.");
        return Out;
    }
    for (const auto &E : B) Out.push_back(E.get<std::string>());
    return Out;
}

void LayerLocator(const nlohmann::ordered_json &Sub, const std::filesystem::path &PackagePath,
                  std::filesystem::path &Local, std::string &Cid)
{
    std::string PathStr = VarSubst::Unescape(Sub.value("PATH", std::string()));   // a file's name, as written out (%% → %)
    Cid.clear();
    if (Sub.contains("SOURCE") && Sub["SOURCE"].is_object())
    {
        const auto &Src = Sub["SOURCE"];
        PathStr = VarSubst::Unescape(Src.value("PATH", PathStr));             // SOURCE.PATH overrides the local path
        if (Src.value("TYPE", std::string("path")) == "ipfs") Cid = Src.value("CID", std::string());
    }
    std::filesystem::path P(PathStr);
    //A content file of the package: its bumped name when two versions of that name are kept side by side.
    Local = P.is_absolute() ? P : ContentFile::Path(PackagePath, PathStr, Cid);
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
#else
std::string MachinePlatform() { return "linux64"; }
#endif

bool ServesPlatform(const Node &Runner, const std::string &Platform, const std::filesystem::path &LaunchBundleDir)
{
    if (std::find(Runner.GuestPlatform.begin(), Runner.GuestPlatform.end(), Platform) == Runner.GuestPlatform.end()) return false;
    return Platform != kCustomPlatform || (!LaunchBundleDir.empty() && Runner.BundleDir == LaunchBundleDir);
}


// ----- manifest-only package predicates -----

// ---------------------------------------------------------------------------
// Case-conflict resolution — the fix for FindCrossLayerCaseCollisions errors.
// Two layers in a launchable's closure contribute paths differing only in case (e.g. base 'server.dll' + patch
// 'Server.dll', or base 'CONTENT/' + add-on 'content/'). On the case-sensitive FUSE mount both exist, so the
// higher-priority layer fails to override and wine's lookup can hit the wrong file. We CANONICALIZE: within each
// closure (base-first load order) the first exact case seen for a path component is canonical; any later layer's
// mismatch is renamed to it. Only the higher-priority zips are rewritten (unpack→rename→repackage, STORE, same
// structure); the base is never touched.
// ---------------------------------------------------------------------------

} // namespace ManifestModel

