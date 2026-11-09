#include "launchtree.h"
#include "manifestmodel.h"   // MakeVfsSpecLayer — the spec entry the launch builds from the same op
#include "nodelower.h"       // ContentOps — what the launch mounts
#include "zipscan.h"         // VidyaGodFS: a zip's central directory, from any byte source
#include <algorithm>

LaunchTree LaunchTree::Of(const Fold::Plan &P)
{
    LaunchTree L;
    nlohmann::ordered_json Layers = nlohmann::ordered_json::array();
    for (const auto &[J, Op] : NodeLower::ContentOps(P))
    {
        nlohmann::ordered_json X = ManifestModel::MakeVfsSpecLayer(Op, Op.value("PATH", std::string()), Op.value("TARGET", std::string()));
        if (X.is_null()) continue;
        L.LayerOf[J] = L.ItemOf.size();
        L.ItemOf.push_back(J);
        Layers.push_back(std::move(X));
    }
    const nlohmann::ordered_json Spec = { {"mountpoint", "/"}, {"readonly", true}, {"layers", Layers} };   // never mounted
    std::string Err;
    const bool Parsed = ParseSpecText(Spec.dump(), L.S, Err);
    if (!Parsed) { L.S = {}; L.ItemOf.clear(); L.LayerOf.clear(); }
    L.T = PlanTree(L.S);
    if (!Parsed) L.T.problems.push_back("the mount plan is malformed (" + Err + ")");   // the lowering's own shapes always parse
    return L;
}

const std::vector<std::string> *ArchiveNames::Of(const LaunchTree &L, size_t Layer, bool Folders)
{
    if (Layer >= L.S.layers.size()) return nullptr;
    const LayerSpec &X = L.S.layers[Layer];
    if (X.type != LayerType::Zip && X.type != LayerType::Delta) return nullptr;
    //A zip is its file; a delta, its file over the bases it names.
    const auto Key = [](const LayerSpec &Y) {
        std::string K = Y.source;
        for (const std::string &B : Y.bases) K += '\x1f' + B;
        return K;
    };
    const auto Ready = [&]() -> const std::vector<std::string> * {
        if (Folders) { const auto It = All.find(Key(X)); return It == All.end() ? nullptr : &It->second; }
        const auto It = Files.find(Key(X)); return It == Files.end() ? nullptr : &It->second;
    };
    if (const auto *R = Ready()) return R;
    if (Failed.count(Key(X))) return nullptr;
    const auto List = [&](const std::string &K, const std::shared_ptr<ByteSource> &Src) {
        if (All.count(K)) return;
        std::vector<std::string> A, F;
        if (!zipscan::WalkCentralDir(*Src, [&](const std::string &Name, uint16_t, uint64_t) {
                A.push_back(Name);
                std::string N = Name;
                std::replace(N.begin(), N.end(), '\\', '/');
                if (!N.empty() && N.back() != '/') F.push_back(std::move(N));
                return true; }))
            return;
        All.emplace(K, std::move(A));
        Files.emplace(K, std::move(F));
    };
    LayerBytes Bytes(L.S, L.T, std::vector<bool>(L.S.layers.size(), false));
    Bytes.OnBuilt = [&](size_t I, const std::shared_ptr<ByteSource> &Src) {
        if (L.S.layers[I].type == LayerType::Delta) List(Key(L.S.layers[I]), Src);   // a chain's members, on the way up
    };
    if (const std::shared_ptr<ByteSource> Src = Bytes.Get(Layer)) List(Key(X), Src);
    for (size_t I = 0; I < L.S.layers.size(); ++I)           // every delta on the way down that its files failed
        if (Bytes.Failed(I) && !L.T.broken[I]) Failed.insert(Key(L.S.layers[I]));
    return Ready();
}
