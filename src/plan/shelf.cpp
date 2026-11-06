#include "shelf.h"
#include "commonutils.h"
#include "fold.h"   // Fold — a row's grafts are what its resolution offers
#include "manifestmodel.h"

#include <algorithm>
#include <set>

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
        const auto Rec = [&](const Node *N) { return std::find(N->Recommended.begin(), N->Recommended.end(), U) != N->Recommended.end(); };
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

bool MoveGraft(const NodeIndex &Idx, const std::string &LaunchNodeId, std::vector<std::string> &Ticked, size_t From,
               int By, std::string *Why, const std::map<std::string, std::string> &Instance,
               const std::map<std::string, std::string> &Builtins)
{
    const long To = (long)From + By;
    if (From >= Ticked.size() || To < 0 || To >= (long)Ticked.size()) return false;
    std::vector<std::string> Proposed = Ticked;
    std::swap(Proposed[From], Proposed[(size_t)To]);
    //By WHICH grafts apply, not how many: two that exclude each other keep the count equal when the move swaps them.
    const std::vector<std::string> Before = AppliedGrafts(Idx, LaunchNodeId, Ticked, Instance, Builtins);
    const std::vector<std::string> After = AppliedGrafts(Idx, LaunchNodeId, Proposed, Instance, Builtins);
    std::vector<std::string> Dropped;
    for (const std::string &G : Before)
        if (std::find(After.begin(), After.end(), G) == After.end()) Dropped.push_back(G);
    if (!Dropped.empty())
    {
        if (Why)
        {
            std::string Names;
            for (const std::string &D : Dropped)
            {
                const Node *N = Idx.Find(D);
                Names += (Names.empty() ? "" : ", ") + std::string("'") + (N && !N->NodeId.empty() ? N->NodeId : D) + "'";
            }
            *Why = Names + " would no longer apply: a graft applies only after the grafts it needs.";
        }
        return false;
    }
    Ticked = std::move(Proposed);
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
        if (LocalGraft(Idx, G)) Out.push_back(G);
    return Out;
}

} // namespace Shelf
