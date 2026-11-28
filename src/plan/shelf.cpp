#include "shelf.h"
#include "commonutils.h"
#include "fold.h"   // Fold — a row's grafts are what its resolution offers
#include "launchtree.h"   // LaunchTree / ArchiveNames — an archive's members, as the mount reads them
#include "noderefs.h"
#include "manifestmodel.h"

#include <algorithm>
#include <set>
#include <cstdio>
#include <filesystem>
#include <functional>

namespace Shelf
{
using namespace ManifestModel;

std::string RowUnderTile(const NodeIndex &Idx, const std::string &Uid, const std::string &Variant, std::string *Why)
{
    for (const Tile &T : Tiles(Idx))
    {
        if (T.Uid != Uid) continue;
        if (Variant.empty()) return T.Rows.front()->Key();
        for (const Node *N : T.Rows) if (N->Variant == Variant) return N->Key();
        if (Why)
        {
            std::string Names;
            for (const Node *N : T.Rows) Names += (Names.empty() ? "" : ", ") + N->Variant;
            *Why = "tile '" + Uid + "' has no variant '" + Variant + "' (it has: " + Names + ")";
        }
        return {};
    }
    if (Why) *Why = "no tile with UID '" + Uid + "' in the library";
    return {};
}

std::vector<std::string> GraftBases(const NodeIndex &Idx, const std::string &Graft) { return ManifestModel::GraftBases(Idx, Graft); }

std::string DefaultGraftBase(const NodeIndex &Idx, const std::string &Graft)
{
    const Node *G = Idx.Find(Graft);
    if (!G || !G->IsGraft) return {};
    const std::vector<std::string> Bases = GraftBases(Idx, G->Key());
    return Bases.empty() ? std::string() : Bases.front();
}

std::vector<Tile> Tiles(const NodeIndex &Idx)
{
    auto Parent = [&](const std::string &Uid) {
        const nlohmann::ordered_json *T = Idx.Tile(Uid);
        return T && T->contains("PARENTUID") && (*T)["PARENTUID"].is_string() ? (*T)["PARENTUID"].get<std::string>() : std::string();
    };
    auto TitleOf = [&](const std::string &Uid) {
        const nlohmann::ordered_json *T = Idx.Tile(Uid);
        return T ? T->value("TITLE", Uid) : Uid;
    };
    //A tile's chain up to its family root (root first): the sort key that keeps a family together, parent before child.
    auto Chain = [&](std::string Uid) {
        std::vector<std::string> Up;
        std::set<std::string> Seen;
        while (!Uid.empty() && Seen.insert(Uid).second) { Up.push_back(Uid); Uid = Parent(Uid); }
        std::reverse(Up.begin(), Up.end());
        return Up;
    };
    std::map<std::string, Tile> ByUid;
    const auto AddRow = [&](const std::string &U, const Node *N) {
        const nlohmann::ordered_json *T = Idx.Tile(U);
        if (!T) return;
        Tile &S = ByUid[U];
        if (S.Uid.empty()) { S.Uid = U; S.Tile = *T; }
        if (std::find(S.Rows.begin(), S.Rows.end(), N) == S.Rows.end()) S.Rows.push_back(N);
    };
    for (const auto &[Id, N] : Idx.Nodes)
    {
        if (!N.IsVariant()) continue;
        if (!N.IsGraft || !N.Faces.empty()) { for (const std::string &U : N.Faces) AddRow(U, &N); continue; }
        //A graft version presenting no tile of its own is another version of what it applies onto.
        for (const std::string &B : GraftBases(Idx, Id))
            if (const Node *BN = Idx.Find(B)) for (const std::string &U : BN->Faces) AddRow(U, &N);
    }
    //Tiles grafts present: rows are the versions the graft applies onto.
    for (const auto &[U, G] : Idx.TileGraft)
    {
        const nlohmann::ordered_json *T = Idx.Tile(U);
        const std::vector<std::string> Bases = GraftBases(Idx, G);
        if (!T || Bases.empty()) continue;                 // nothing here it applies onto: no card
        Tile &S = ByUid[U];
        if (S.Uid.empty()) { S.Uid = U; S.Tile = *T; }
        S.Graft = G;
        for (const std::string &B : Bases) if (const Node *BN = Idx.Find(B)) S.Rows.push_back(BN);
    }
    std::vector<Tile> Out;
    for (auto &[U, S] : ByUid)
    {
        const auto Rec = [&](const Node *N) { return N->IsRecommended(U); };
        std::stable_sort(S.Rows.begin(), S.Rows.end(), [&](const Node *A, const Node *B) {
            if (Rec(A) != Rec(B)) return Rec(A);
            if (A->Variant != B->Variant) return A->Variant < B->Variant;
            return A->Key() < B->Key();
        });
        Out.push_back(std::move(S));
    }
    //Families by their base game's title; within a family, parent before child, siblings by title.
    std::stable_sort(Out.begin(), Out.end(), [&](const Tile &A, const Tile &B) {
        const std::vector<std::string> Ca = Chain(A.Uid), Cb = Chain(B.Uid);
        for (size_t I = 0; I < std::min(Ca.size(), Cb.size()); ++I)
            if (Ca[I] != Cb[I])
            {
                const std::string Ta = TitleOf(Ca[I]), Tb = TitleOf(Cb[I]);
                return Ta != Tb ? Ta < Tb : Ca[I] < Cb[I];
            }
        return Ca.size() < Cb.size();
    });
    return Out;
}

namespace {
bool LocalGraft(const NodeIndex &Idx, const std::string &G)
{
    const Node *N = Idx.Find(G);
    return N && !N->Received && !N->BundleDir.empty();
}
}

std::vector<std::string> AppliedGrafts(const NodeIndex &Idx, const std::string &LaunchNodeId, const GraftChoice &Chosen,
                                       const std::map<std::string, std::string> &Instance,
                                       const std::map<std::string, std::string> &Builtins, std::vector<std::string> *Dropped)
{
    const Node *L = Idx.Find(LaunchNodeId);
    if (!L) return {};
    const Fold::Library Lib = ManifestModel::LibraryOf(Idx);
    const Fold::GraftIndex GIdx = Fold::BuildGraftIndex(Lib);
    //The launched tile (%UID%, when the caller resolved one): what a fresh instance ticks is RECOMMENDED under IT.
    const auto U = Builtins.find("UID");
    const std::string Face = U != Builtins.end() && !U->second.empty() ? U->second : L->Uid;
    std::vector<std::string> Requested;
    if (Chosen)
        for (const std::string &G : *Chosen) { const Node *Gn = Idx.Find(G); Requested.push_back(Gn ? Gn->Key() : G); }
    else
        Requested = Fold::ApplyGrafts(Lib, GIdx, L->Key(), Instance, Builtins, Face, nullptr);
    //Only local grafts apply; judging the list again keeps a graft that needed a dropped one out as well.
    std::vector<std::string> Local;
    for (const std::string &G : Requested)
        if (LocalGraft(Idx, G)) Local.push_back(G);
        else if (Dropped) Dropped->push_back(G);
    return Fold::ApplyGrafts(Lib, GIdx, L->Key(), Instance, Builtins, Face, &Local, Dropped);
}

std::vector<PartConflict> PartConflicts(const NodeIndex &Idx, const std::string &LaunchNodeId, const std::vector<std::string> &Applied,
                                        const std::map<std::string, std::string> &Instance,
                                        const std::map<std::string, std::string> &Builtins, size_t Max)
{
    std::vector<PartConflict> Out;
    const Node *L = Idx.Find(LaunchNodeId);
    if (!L || Applied.size() < 2) return Out;
    const Fold::Library Lib = ManifestModel::LibraryOf(Idx);
    ArchiveNames Names;
    InnerUnits Inner;
    //A part's footprint: what applying it (after the parts before it) changes — address -> the value it writes.
    using Print = std::map<std::string, std::string>;
    std::vector<Print> Prints;
    //A binary edit's unit is its byte range (MetaPackageFormat 23 §23.4: disjoint ranges merge, overlap conflicts).
    struct Range { std::string File; uint64_t At = 0, End = 0; std::string Value; };
    std::vector<std::vector<Range>> Ranges;
    const auto Facts = [](const Fold::Plan &P) {
        Print F;
        for (const auto &[K, V] : P.Reg) F["REG/" + K] = V.Value.dump();
        for (const auto &[K, V] : P.Dll.items()) F["DLL/" + K] = V.dump();
        for (const auto &[K, V] : P.Env.items()) F["ENV/" + K] = V.dump();
        for (const auto &[K, V] : P.Decls.items()) F["VARS/" + K] = V.dump();
        return F;
    };
    std::vector<std::string> Before;
    Fold::Plan Prev = Fold::Resolve(Lib, L->Key(), Instance, Builtins, Before);
    for (const std::string &G : Applied)
    {
        std::vector<std::string> With = Before; With.push_back(G);
        Fold::Plan P = Fold::Resolve(Lib, L->Key(), Instance, Builtins, With);
        Print F;
        const Print Was = Facts(Prev), Now = Facts(P);
        for (const auto &[K, V] : Now) if (const auto It = Was.find(K); It == Was.end() || It->second != V) F[K] = V;
        for (const auto &[K, V] : Was) if (!Now.count(K)) F[K] = "(removed)";
        //its own items: those after the ones before it (parts are expanded after the end, in order)
        const LaunchTree T = LaunchTree::Of(P);
        std::vector<Range> Rs;
        for (size_t J = Prev.Seq.size(); J < P.Seq.size(); ++J)
        {
            const Fold::Item &I = P.Seq[J];
            const std::string Src = I.Source.is_string() ? I.Source.get<std::string>() : I.Payload;
            if (I.Kind == "EDIT")
            {
                if (I.Ops.is_array())
                    for (const auto &Op : I.Ops)
                    {
                        if (!Op.is_object()) continue;
                        const auto S = [&](const char *K) { return Op.contains(K) && Op[K].is_string() ? Op[K].get<std::string>() : std::string(); };
                        const std::string Mode = S("MODE"), Key = S("KEY"), File = "FILES/" + I.Target;
                        if ((Mode == "Poke" || Mode == "Replace" || Mode == "Cave" || Mode == "Or") && !S("OFFSET").empty() && S("ANCHOR").empty())
                        {   //a range: its offset and the bytes it writes (a %variable% value: at least its first byte)
                            uint64_t At = 0;
                            try { At = std::stoull(S("OFFSET"), nullptr, 0); } catch (...) { F[File + "@" + S("OFFSET")] += Op.dump(); continue; }
                            const std::string V = Mode == "Cave" ? S("PAYLOAD") : S("VALUE");
                            const uint64_t Len = V.find('%') == std::string::npos && V.size() >= 2 ? V.size() / 2 : 1;
                            Rs.push_back({ File, At, At + Len, Op.dump() });
                        }
                        else if (!S("ANCHOR").empty()) F[File + "@anchor:" + S("ANCHOR")] += Op.dump();
                        else if (Mode == "ReplaceAll") F[File + "#replaceall:" + S("EXPECT")] += Op.dump();
                        else if (Mode == "AppendLine") F[File + "#line:" + S("VALUE")] = "line";   // lines: a set (union)
                        else F[File + (Key.empty() ? std::string() : "#" + Key)] += Op.dump();
                    }
                continue;
            }
            //Where the tree places a file of this layer (MetaPackageFormat 23 §23.4: a part's footprint is what it writes
            //WHERE it lands — through any TAKE, a NODE's or its INTO's): EVERY place the layer's mounts put it (selections
            //that overlap mount it at each: a rename plus "FILES" is the file twice), as the mount reads them. Inner: a zip
            //member's path, or "" for a file or coded layer's own file (Name: its name in the node). None: not taken.
            const auto Placed = [&](size_t Layer, const std::string &Inner, const std::string &Name) {
                std::vector<std::string> At;
                for (const auto &[Src, Dst] : T.T.mounts[Layer])
                {
                    const std::string Sep = Dst.empty() ? "" : "/";
                    if (Inner.empty()) At.push_back(Src.empty() ? Dst + Sep + Name : Dst);   // a file: under its name, or renamed
                    else if (Src.empty()) At.push_back(Dst + Sep + Inner);
                    else if (Inner == Src) At.push_back(Dst);
                    else if (Inner.size() > Src.size() && Inner.compare(0, Src.size() + 1, Src + "/") == 0) At.push_back(Dst + Sep + Inner.substr(Src.size() + 1));
                }
                return At;
            };
            //a file's inner units (an archive's members beside loose files, a plugin's records): in the folder it lands in
            const auto InnerOf = [&](size_t Layer, const std::string &Member, const std::string &At) {
                const std::string Dir = std::filesystem::path(At).parent_path().generic_string();
                if (const auto *U = Inner.Of(T, Layer, Member))
                    for (const InnerUnit &X : *U)
                        F[X.Address.rfind("RECORD/", 0) == 0 ? X.Address : "FILES/" + Dir + (Dir.empty() ? "" : "/") + X.Address] = X.Value;
            };
            const std::string Name = I.Kind == "CODED" ? I.Name : std::filesystem::path(I.Payload).filename().string();
            if (T.S.layers.empty())                                      // a plan the FS could not read: where the layers say
            {
                if (I.Kind == "FILE" || I.Kind == "CODED") F["FILES/" + I.Target + (I.Target.empty() ? "" : "/") + Name] = Src;
                continue;
            }
            const auto Lay = T.LayerOf.find(J);
            if (Lay == T.LayerOf.end() || T.S.layers[Lay->second].bytesOnly) continue;   // read for its bytes only: writes nothing
            if (I.Kind == "FILE" || I.Kind == "CODED")
            {
                for (const std::string &At : Placed(Lay->second, "", Name)) { F["FILES/" + At] = Src; InnerOf(Lay->second, "", At); }
                continue;
            }
            if (const auto *M = Names.Of(T, Lay->second, false))
                for (const std::string &E : *M)
                    for (const std::string &At : Placed(Lay->second, E, ""))
                    {
                        F["FILES/" + At] = Src + ":" + E;
                        if (InnerUnits::KindOf(E)) InnerOf(Lay->second, E, At);
                    }
        }
        Prints.push_back(std::move(F));
        Ranges.push_back(std::move(Rs));
        Before = std::move(With);
        Prev = std::move(P);
    }
    //what each part goes into, transitively, among the applied ones: a part and one it goes into never conflict
    std::map<std::string, std::set<std::string>> Under;
    const std::set<std::string> AppliedSet(Applied.begin(), Applied.end());
    std::function<void(const std::string &, std::set<std::string> &)> Up = [&](const std::string &G, std::set<std::string> &Acc) {
        const Node *N = Idx.Find(G);
        if (!N) return;
        for (const std::string &T : N->Into) if (AppliedSet.count(T) && Acc.insert(T).second) Up(T, Acc);
    };
    for (const std::string &G : Applied) Up(G, Under[G]);
    const auto Lower = [](std::string S) { std::transform(S.begin(), S.end(), S.begin(), [](unsigned char C) { return std::tolower(C); }); return S; };
    for (size_t A = 0; A < Applied.size(); ++A)
    {
        std::map<std::string, const std::string *> ByLower;   // FILES addresses compare without case
        for (const auto &[K, V] : Prints[A]) ByLower[K.rfind("FILES/", 0) == 0 ? Lower(K) : K] = &V;
        for (size_t B = A + 1; B < Applied.size(); ++B)
        {
            if (Under[Applied[A]].count(Applied[B]) || Under[Applied[B]].count(Applied[A])) continue;
            for (const auto &[K, V] : Prints[B])
            {
                const auto It = ByLower.find(K.rfind("FILES/", 0) == 0 ? Lower(K) : K);
                if (It == ByLower.end() || *It->second == V) continue;
                Out.push_back({ K, Applied[A], Applied[B] });
                if (Out.size() >= Max) return Out;
            }
            for (const Range &Ra : Ranges[A])
                for (const Range &Rb : Ranges[B])
                    if (Lower(Ra.File) == Lower(Rb.File) && Ra.At < Rb.End && Rb.At < Ra.End && Ra.Value != Rb.Value)
                    {
                        char Buf[64];
                        std::snprintf(Buf, sizeof Buf, "@0x%llx-0x%llx", (unsigned long long)std::max(Ra.At, Rb.At), (unsigned long long)std::min(Ra.End, Rb.End));
                        Out.push_back({ Rb.File + Buf, Applied[A], Applied[B] });
                        if (Out.size() >= Max) return Out;
                    }
        }
    }
    return Out;
}

std::optional<std::vector<std::string>> IntoChain(const NodeIndex &Idx, const std::string &Part, const std::string &End)
{
    std::set<std::string> OnPath;
    std::function<std::optional<std::vector<std::string>>(const std::string &)> Walk = [&](const std::string &G) -> std::optional<std::vector<std::string>> {
        const Node *N = Idx.Find(G);
        if (!N || !OnPath.insert(G).second) return std::nullopt;
        for (const std::string &T : N->Into)
        {
            const Node *Tn = Idx.Find(T);
            const std::string K = Tn ? Tn->Key() : T;
            if (K == End) { OnPath.erase(G); return std::vector<std::string>{}; }
            if (Tn && Tn->IsGraft && !Tn->Received)                     // a path through parts that are here
                if (auto Below = Walk(K)) { Below->push_back(K); OnPath.erase(G); return Below; }
        }
        OnPath.erase(G);
        return std::nullopt;
    };
    return Walk(Part);
}

std::vector<std::string> OrderResolved(const NodeIndex &Idx, const std::vector<std::string> &Applied, const nlohmann::ordered_json &Resolutions)
{
    if (!Resolutions.is_object() || Resolutions.empty() || Applied.size() < 2) return Applied;
    std::set<std::string> Winners;
    for (const auto &[Addr, W] : Resolutions.items()) if (W.is_string()) Winners.insert(W.get<std::string>());
    //Kahn's order over "after what it goes into" (among the applied): each step takes the earliest ready part that is
    //not kept, so every other part keeps its order (the applied order is already such an order) and a kept part goes
    //as late as its links allow.
    //A part needs, of each INTO layer, ONE member before it (a layer's members are alternatives, as ApplyGrafts reads
    //them).
    const std::set<std::string> In(Applied.begin(), Applied.end());
    std::map<std::string, std::vector<std::vector<std::string>>> Needs;
    for (const std::string &G : Applied)
        if (const Node *N = Idx.Find(G); N && N->Json->contains("LAYERS"))
            for (const auto *L : Fold::IntoLayers((*N->Json)["LAYERS"]))
            {
                //a layer with a member that is not an applied part (the end, what lies beneath) is met from the start
                std::vector<std::string> Alt;
                bool Met = false;
                for (const auto &M : (*L)["INTO"])
                {
                    if (!M.is_string() || M.get<std::string>() == G) continue;
                    if (In.count(M.get<std::string>())) Alt.push_back(M.get<std::string>()); else Met = true;
                }
                if (!Met && !Alt.empty()) Needs[G].push_back(std::move(Alt));
            }
    std::vector<std::string> Out;
    std::set<std::string> Done;
    while (Out.size() < Applied.size())
    {
        std::string Pick;
        for (int Pass = 0; Pass < 2 && Pick.empty(); ++Pass)
            for (const std::string &G : Applied)
            {
                if (Done.count(G) || (Pass == 0 && Winners.count(G))) continue;
                bool Ready = true;
                for (const auto &Alt : Needs[G])
                    if (std::none_of(Alt.begin(), Alt.end(), [&](const std::string &T) { return Done.count(T) > 0; })) { Ready = false; break; }
                if (Ready) { Pick = G; break; }
            }
        if (Pick.empty()) return Applied;            // a cycle among the applied: leave the order as it was
        Out.push_back(Pick);
        Done.insert(Pick);
    }
    return Out;
}

std::string ResolutionFor(const nlohmann::ordered_json &Resolutions, const std::string &Address)
{
    if (!Resolutions.is_object()) return {};
    if (const auto It = Resolutions.find(Address); It != Resolutions.end() && It->is_string()) return It->get<std::string>();
    if (Address.rfind("FILES/", 0) != 0) return {};
    const auto Lower = [](std::string S) { std::transform(S.begin(), S.end(), S.begin(), [](unsigned char C) { return std::tolower(C); }); return S; };
    const std::string L = Lower(Address);
    for (const auto &[K, V] : Resolutions.items()) if (V.is_string() && Lower(K) == L) return V.get<std::string>();
    return {};
}

bool ResolutionHolds(const std::vector<std::string> &Order, const PartConflict &C, const std::string &Kept,
                     const std::vector<PartConflict> &All)
{
    //the kept part must write this address (be one of a conflict's parts there) and fold after both parts of this pair
    const auto Lower = [](std::string S) { std::transform(S.begin(), S.end(), S.begin(), [](unsigned char X) { return std::tolower(X); }); return S; };
    const auto Same = [&](const std::string &X) { return X == C.Address || (X.rfind("FILES/", 0) == 0 && Lower(X) == Lower(C.Address)); };
    bool Writes = Kept == C.A || Kept == C.B;
    for (const auto &O : All) if (!Writes && Same(O.Address) && (O.A == Kept || O.B == Kept)) Writes = true;
    if (!Writes) return false;
    const auto K = std::find(Order.begin(), Order.end(), Kept);
    if (K == Order.end()) return false;
    for (const std::string &P : { C.A, C.B })
    {
        if (P == Kept) continue;
        const auto Pi = std::find(Order.begin(), Order.end(), P);
        if (Pi == Order.end() || Pi > K) return false;
    }
    return true;
}

GraftChoice CurrentGraftChoice(const NodeIndex &Idx, const GraftChoice &Saved)
{
    if (!Saved) return Saved;
    for (const std::string &G : *Saved)
        if (!Idx.Find(G))
        {
            LogOut("PackageCatalog::CurrentGraftChoice", "saved graft list names " + G + ", no longer in the library — "
                   "saved against an older version of the package; using the tile's defaults");
            return std::nullopt;
        }
    return Saved;
}

std::vector<std::string> TickGraft(const NodeIndex &Idx, const std::string &LaunchNodeId, const std::vector<std::string> &Ticked,
                                   const std::string &G, std::vector<std::string> *Unticked,
                                   const std::map<std::string, std::string> &Instance,
                                   const std::map<std::string, std::string> &Builtins)
{
    const auto Applies = [&](std::vector<std::string> List) {
        List.push_back(G);
        const std::vector<std::string> A = AppliedGrafts(Idx, LaunchNodeId, List, Instance, Builtins);
        return std::find(A.begin(), A.end(), G) != A.end();
    };
    //In list order: X goes when G applies after the grafts kept so far but not once X is among them — so several
    //exclusions go at once, and a graft that needs an earlier one is judged with it in place.
    std::vector<std::string> Keep;
    for (const std::string &X : Ticked)
    {
        if (X == G) continue;
        std::vector<std::string> With = Keep;
        With.push_back(X);
        if (Applies(Keep) && !Applies(With)) { if (Unticked) Unticked->push_back(X); continue; }
        Keep = std::move(With);
    }
    //G needs a graft ticked after the one that excludes it (it could not apply at that point): the one whose removal
    //lets it apply goes.
    for (size_t I = 0; I < Keep.size() && !Applies(Keep);)
    {
        std::vector<std::string> Without = Keep;
        Without.erase(Without.begin() + (long)I);
        if (Applies(Without)) { if (Unticked) Unticked->push_back(Keep[I]); Keep = std::move(Without); }
        else ++I;
    }
    //The other way round (exclusion is a condition, and only one side may carry it: "never with aok" on tc): a ticked
    //part that applied before G and does not once G is ticked goes too, so ticking either side unticks the other.
    {
        const std::vector<std::string> Before = AppliedGrafts(Idx, LaunchNodeId, Keep, Instance, Builtins);
        std::vector<std::string> With = Keep; With.push_back(G);
        const std::vector<std::string> After = AppliedGrafts(Idx, LaunchNodeId, With, Instance, Builtins);
        std::vector<std::string> Still;
        for (const std::string &X : Keep)
        {
            const bool Was = std::find(Before.begin(), Before.end(), X) != Before.end();
            const bool Is = std::find(After.begin(), After.end(), X) != After.end();
            if (Was && !Is) { if (Unticked) Unticked->push_back(X); continue; }
            Still.push_back(X);
        }
        Keep = std::move(Still);
    }
    Keep.push_back(G);
    return Keep;
}

std::vector<std::string> OfferedGrafts(const NodeIndex &Idx, const std::string &LaunchNodeId, std::vector<std::string> *PreTicked,
                                       const GraftChoice &Chosen, const std::string &FaceUid)
{
    if (PreTicked) PreTicked->clear();
    const Node *L = Idx.Find(LaunchNodeId);
    if (!L) return {};
    const std::string Face = FaceUid.empty() ? L->Uid : FaceUid;
    const std::map<std::string, std::string> Builtins{ {"UID", Face} };
    const Fold::Library Lib = ManifestModel::LibraryOf(Idx);
    const std::vector<std::string> Fresh = AppliedGrafts(Idx, LaunchNodeId, std::nullopt, {}, Builtins);
    if (PreTicked) *PreTicked = Fresh;
    const Fold::Plan P = Fold::Resolve(Lib, L->Key(), {}, Builtins, Chosen ? AppliedGrafts(Idx, LaunchNodeId, Chosen, {}, Builtins) : Fresh);
    std::vector<std::string> Out;
    for (const auto &G : Fold::OfferedGrafts(Lib, Fold::BuildGraftIndex(Lib), P, Face).Offered)
    {
        //A part with an entry of its own (a source port's engine row, Forgotten Empires) is a ROW of its card, launched
        //from it, never an add-on of this one to tick (09 §9.4) — the engine still offers it, for the rows that force it.
        const Node *Gn = Idx.Find(G);
        if (Gn && Fold::OwnsGameEntry(*Gn->Json)) continue;
        if (LocalGraft(Idx, G)) Out.push_back(G);
    }
    return Out;
}

} // namespace Shelf
