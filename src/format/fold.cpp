#include "fold.h"
#include "varsubst.h"   // EvaluateCondition / RenderValue — the one condition grammar and render formats

#include <algorithm>
#include <cctype>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_set>

namespace Fold
{

namespace
{

const std::vector<std::string> &TypeKeys()
{
    static const std::vector<std::string> K = { "ZIP", "FILE", "DELTA", "DIR", "NODE", "EDIT", "REG", "VARS", "ENV",
                                                "DLL", "EXEC", "KEEP", "INTO" };
    return K;
}

bool IsContent(const std::string &T) { return T == "ZIP" || T == "FILE" || T == "DELTA" || T == "DIR"; }

std::string Lower(std::string S)
{
    std::transform(S.begin(), S.end(), S.begin(), [](unsigned char C) { return static_cast<char>(std::tolower(C)); });
    return S;
}

std::string Str(const json &V)
{
    if (V.is_string()) return V.get<std::string>();
    if (V.is_null()) return std::string();
    return V.dump();
}

//%KEY% / %KEY:format% substitution; an unknown token stays as written, an unmatched '%' ends substitution. The
//same grammar as VarSubst::StringVariableSubstitution, without its per-token logging: the fixpoint below meets
//not-yet-resolved tokens on every pass by design.
std::string Subst(const std::string &S, const Vars &V)
{
    std::string Out;
    size_t Pos = 0;
    while (Pos < S.size())
    {
        const size_t A = S.find('%', Pos);
        if (A == std::string::npos) { Out += S.substr(Pos); break; }
        const size_t B = S.find('%', A + 1);
        if (B == std::string::npos) { Out += S.substr(Pos); break; }
        Out += S.substr(Pos, A - Pos);
        if (B == A + 1) { Out += "%%"; Pos = B + 1; continue; }   // a literal '%', kept for the final substitution
        const std::string Tok = S.substr(A + 1, B - A - 1);
        const size_t C = Tok.find(':');
        const std::string Key = C == std::string::npos ? Tok : Tok.substr(0, C);
        const std::string Fmt = C == std::string::npos ? std::string() : Tok.substr(C + 1);
        const auto It = V.find(Key);
        if (It != V.end()) Out += VarSubst::RenderInsert(It->second, Fmt, V, /*KeepEscapes=*/true);
        else { Out += '%'; Out += Tok; Out += '%'; }
        Pos = B + 1;
    }
    return Out;
}

//A layer WHEN that may name node CIDs: a named node holds when it is in the composition (beneath this point, or one of
//the parts selected for the launch).
bool When(const json &Expr, const Vars &V, const VarSubst::Presence &Present)
{
    return !Expr.is_string() || VarSubst::EvaluateCondition(Expr.get<std::string>(), V, Present);
}

std::pair<std::string, std::string> NsPath(const std::string &Addr)
{
    const size_t S = Addr.find('/');
    if (S == std::string::npos) return { Addr, std::string() };
    return { Addr.substr(0, S), Addr.substr(S + 1) };
}

//An anchored path (a %Anchor% or a drive) stays where it is; anything else is relative to its placement.
bool IsAbsolute(const std::string &P)
{
    std::string Head;
    if (P.size() >= 2 && P[0] == '%')
    {
        const size_t E = P.find('%', 1);
        if (E == std::string::npos || E == 1) return false;
        Head = P.substr(0, E + 1);
    }
    else if (P.size() >= 2 && std::isalpha(static_cast<unsigned char>(P[0])) && P[1] == ':') Head = P.substr(0, 2);
    else return false;
    return P.size() == Head.size() || P[Head.size()] == '/';
}

std::string Place(const std::string &P, const std::string &Prefix)
{
    if (Prefix.empty() || IsAbsolute(P)) return P;
    return Prefix + (P.empty() ? std::string() : "/" + P);
}

std::optional<std::string> TakeMatch(const std::string &Addr, const std::string &Sel)
{
    if (Sel == Addr)
    {
        const size_t S = Addr.rfind('/');
        return S == std::string::npos ? std::string() : Addr.substr(S + 1);
    }
    const bool Contents = !Sel.empty() && Sel.back() == '/';
    const std::string Base = Contents ? Sel.substr(0, Sel.size() - 1) : Sel;
    if (Addr.compare(0, Base.size() + 1, Base + "/") == 0)
    {
        const std::string Rest = Addr.substr(Base.size() + 1);
        if (Contents || Base.find('/') == std::string::npos) return Rest;       // contents, or a whole namespace
        return Base.substr(Base.rfind('/') + 1) + "/" + Rest;                    // a directory keeps its own name
    }
    return std::nullopt;
}

//What one TAKE keeps of an address, renamed; nullopt = not taken.
std::optional<std::string> Taken(const json &Take, const std::string &Addr)
{
    if (Take.is_null() || (Take.is_array() && Take.empty())) return Addr;
    for (const auto &Sel : Take)
    {
        if (Sel.is_array() && Sel.size() == 2)
        {
            const std::string Src = Str(Sel[0]);
            std::string Dst = Str(Sel[1]);
            const std::string Ns = Src.substr(0, Src.find('/'));
            if (Dst.compare(0, Ns.size() + 1, Ns + "/") != 0) Dst = Ns + "/" + Dst;
            if (Addr == Src) return Dst;
            std::string S2 = Src;
            while (!S2.empty() && S2.back() == '/') S2.pop_back();
            if (Addr.compare(0, S2.size() + 1, S2 + "/") == 0) return Dst + Addr.substr(S2.size());
        }
        else if (Sel.is_string())
        {
            const std::string S = Sel.get<std::string>();
            if (auto R = TakeMatch(Addr, S)) return S.substr(0, S.find('/')) + (R->empty() ? std::string() : "/" + *R);
        }
    }
    return std::nullopt;
}

//A VIEW is how an occurrence's addresses reach the root: its [TAKE, TARGET] steps, innermost first — the NODE layer
//that brought it in, then that node's own view. Facts go through the takes only (never re-rooted); FILES addresses
//go through each take and then its placement, so an outer TAKE sees an inner node's files where they landed. A view
//is a shared list (one step per expansion, like the occurrence chain): chains 900 deep must not copy it per level.
struct Step
{
    const json *Take;                       // the NODE layer's TAKE (nullptr: none)
    std::string At;                         // its TARGET's path ("" = placed where the container is)
    std::shared_ptr<const Step> Next;       // the container's view
    bool AnyTake;                           // this step or one outside it takes
};
using View = std::shared_ptr<const Step>;

bool Takes(const json *T) { return T && !T->is_null() && !(T->is_array() && T->empty()); }

View Within(const View &V, const json *Take, std::string At)
{
    return std::make_shared<const Step>(Step{ Take, std::move(At), V, Takes(Take) || (V && V->AnyTake) });
}

bool ViewTakes(const View &V) { return V && V->AnyTake; }

std::string ViewKey(const View &V)
{
    if (!ViewTakes(V)) return std::string();
    std::string K;
    for (const Step *S = V.get(); S; S = S->Next.get()) { K += Takes(S->Take) ? S->Take->dump() : "-"; K += '\x1e'; K += S->At; K += '\x1d'; }
    return K;
}

json ViewJson(const View &V)
{
    json A = json::array();
    for (const Step *S = V.get(); S; S = S->Next.get()) A.push_back(json::array({ Takes(S->Take) ? *S->Take : json(), S->At }));
    return A;
}

std::optional<std::string> MapFact(const View &V, std::string Addr)
{
    if (!ViewTakes(V)) return Addr;
    for (const Step *S = V.get(); S; S = S->Next.get())
    {
        if (!Takes(S->Take)) continue;
        const auto A = Taken(*S->Take, Addr);
        if (!A) return std::nullopt;
        Addr = *A;
    }
    return Addr;
}

//Prefix: the view's placements composed — a view with no take places by it alone.
std::optional<std::string> MapFiles(const View &V, std::string Addr, const std::string &Prefix)
{
    if (!ViewTakes(V)) return "FILES/" + Place(NsPath(Addr).second, Prefix);
    for (const Step *S = V.get(); S; S = S->Next.get())
    {
        if (Takes(S->Take))
        {
            const auto A = Taken(*S->Take, Addr);
            if (!A) return std::nullopt;
            Addr = *A;
        }
        if (!S->At.empty()) Addr = "FILES/" + Place(NsPath(Addr).second, S->At);
    }
    return Addr;
}

//EXEC entries fold per field (EXEC/<label>/<field>/…): objects merge, anything else is replaced; null deletes. A tile's
//COVER is one image (FILE, SOURCE and SIZE describe the same bytes) and is replaced whole: merged, a new FILE kept the
//SOURCE of the cover beneath, and the card showed that other image.
json Merge(const json &A, const json &B, const std::string &Field = std::string())
{
    if (!A.is_object() || !B.is_object()) return B;
    json Out = A;
    for (const auto &[K, V] : B.items())
    {
        if (V.is_null()) Out.erase(K);
        else if (Field == "TILE" && K == "COVER") Out[K] = V;
        else if (V.is_object() && A.contains(K) && A[K].is_object()) Out[K] = Merge(A[K], V, Field.empty() ? K : Field + "/" + K);
        else Out[K] = V;
    }
    return Out;
}

void Put(json &Obj, const std::string &K, const json &V)          // a later writer moves to the end (python dict)
{
    Obj.erase(K);
    Obj[K] = V;
}

// ---- expansion state -----------------------------------------------------------------------------------------
struct Occ { std::string Key, Cid; std::shared_ptr<const Occ> Parent; };
using Chain = std::shared_ptr<const Occ>;

//Walked by raw pointer: a shared_ptr copy per step is two atomic refcount ops, and deep chains (a version history
//nine hundred nodes long) made that the whole cost of a resolve.
bool ChainHasKey(const Chain &C, const std::string &Key)
{
    for (const Occ *P = C.get(); P; P = P->Parent.get()) if (P->Key == Key) return true;
    return false;
}

struct Slot
{
    std::string Kind;
    const json *Layer = nullptr;
    int At = 0;
    Chain Inner;               // the containers + this node's own occurrence
    std::string From, Dir, Prefix;
    View Through;
    int Depth = 0;             // how many NODE layers in
    std::string Ref;           // NODE (only shown): the node the library does not hold; INTO is shown too
};

class Expander
{
public:
    Expander(const Library &L, const Vars &Wv, bool ExecOnly = false, int Flip = -1, const std::set<std::string> *Selected = nullptr)
        : Lib(L), Wv(Wv), ExecOnly(ExecOnly), Flip(Flip), Selected(Selected) {}

    //Is this node in the composition, for a WHEN that names it: an occurrence beneath this point, or a part selected for
    //the launch (selected parts are judged on the selection, so parts that exclude each other cannot chase each other).
    bool Present(const std::string &Cid) const { return Count.count(Cid) != 0 || (Selected && Selected->count(Cid) != 0); }

    void Expand(const std::string &Cid, const View &Through, const std::string &Prefix, Chain Containers)
    {
        const std::string Key = Cid + '\x1f' + ViewKey(Through) + '\x1f' + Prefix;
        if (OnPath.count(Cid)) { Events.push_back({ "cycle", Cid }); return; }   // a node inside itself
        if (auto It = First.find(Key); It != First.end())
        {
            const Chain Prev = It->second;
            const bool SameList = Prev && Containers && Prev->Key == Containers->Key;   // a later mention in the SAME list moves it
            if (SameList != (Decisions++ == Flip))                       // (Flip: DecidingMentions asks "and otherwise?")
            {
                Remove(Key);
                Events.push_back({ "move", Cid });
            }
            else { Events.push_back({ "held", Cid }); return; }
        }
        const Library::Entry *E = Lib.Find(Cid);
        if (!E) { Events.push_back({ "missing", Cid }); return; }
        First[Key] = Containers;
        Order.push_back({ Key, Cid });
        ++Count[Cid];
        const Chain Inner = std::make_shared<const Occ>(Occ{ Key, Cid, Containers });
        ++OnPath[Cid];
        struct Leave { std::unordered_map<std::string, int> &M; const std::string &C; ~Leave() { if (--M[C] == 0) M.erase(C); } } L_{ OnPath, Cid };
        const json &Layers = E->Node->contains("LAYERS") ? (*E->Node)["LAYERS"] : Empty();
        const VarSubst::Presence Pres = [this](const std::string &C) { return Present(C); };
        //A part whose INTO does not hold here (a WHEN on one of its INTO layers, say "only with this DLC") is not
        //applied at all, not partly.
        for (const json *In : IntoLayers(Layers))
            if (In->contains("WHEN") && !When((*In)["WHEN"], Wv, Pres)) { Events.push_back({ "into-off", Cid }); return; }
        for (int I = 0; I < static_cast<int>(Layers.size()); ++I)
        {
            const json &L = Layers[I];
            if (L.contains("WHEN") && !When(L["WHEN"], Wv, Pres)) continue;
            const std::string T = TypeOf(L);
            if (T.empty()) { Error = "node " + Cid + " layer " + std::to_string(I) + " has no single type key"; continue; }
            if (T == "NODE")
            {
                std::string Sub = Prefix, At;
                if (L.contains("TARGET"))
                {
                    const auto [Ns, P] = NsPath(Str(L["TARGET"]));
                    if (Ns != "FILES") { Error = "node " + Cid + ": NODE TARGET outside FILES"; continue; }
                    Sub = Place(P, Prefix);
                    At = P;
                }
                const std::string Ref = Str(L["NODE"]);
                const View V = Within(Through, L.contains("TAKE") ? &L["TAKE"] : nullptr, At);
                if (!ExecOnly && !Lib.Find(Ref)) Slots.push_back({ T, &L, I, Inner, Cid, E->Dir, Prefix, V, Depth, Ref });
                ++Depth;
                Expand(Ref, V, Sub, Inner);
                --Depth;
            }
            else if (T == "INTO")
            {
                //What a part goes into must be in the composition beneath it: for a part selected at launch that is
                //the end or a part applied to it (the hard link is the offer's, OfferedGrafts); for a part a node
                //contains, it is what that node placed beneath it (the container vouches for its own list).
                if (!ExecOnly) Slots.push_back({ T, &L, I, Inner, Cid, E->Dir, Prefix, Through, Depth, {} });
                bool Met = false;
                if (L["INTO"].is_array()) for (const auto &M : L["INTO"]) if (Count.count(Str(M))) { Met = true; break; }
                if (!Met) Events.push_back({ "into-unmet", Cid });
            }
            else if (!ExecOnly || T == "EXEC")
                Slots.push_back({ T, &L, I, Inner, Cid, E->Dir, Prefix, Through, Depth, {} });
        }
    }

    const Library &Lib;
    const Vars &Wv;
    const bool ExecOnly;
    const int Flip;                                                         // the held/move decision to take the other way
    const std::set<std::string> *Selected;                                  // the parts selected for the launch (WHEN presence)
    std::unordered_map<std::string, int> Count;                             // occurrences per node, for presence
    int Decisions = 0;                                                      // held/move decisions taken so far
    int Depth = 0;                                                          // NODE layers in, while expanding
    std::vector<Slot> Slots;
    std::unordered_map<std::string, Chain> First;
    std::unordered_map<std::string, int> OnPath;                             // nodes being expanded right now (cycle guard)
    std::vector<std::pair<std::string, std::string>> Order;                  // (occurrence key, cid)
    std::vector<std::pair<std::string, std::string>> Events;
    std::string Error;

private:
    static const json &Empty() { static const json E = json::array(); return E; }

    void Remove(const std::string &Key)
    {
        std::unordered_set<std::string> Gone;
        for (const auto &[K, C] : First) if (K == Key || ChainHasKey(C, Key)) Gone.insert(K);
        Slots.erase(std::remove_if(Slots.begin(), Slots.end(), [&](const Slot &S) { return ChainHasKey(S.Inner, Key); }),
                    Slots.end());
        for (const auto &K : Gone) First.erase(K);
        for (const auto &O : Order) if (Gone.count(O.first) && --Count[O.second] == 0) Count.erase(O.second);
        Order.erase(std::remove_if(Order.begin(), Order.end(), [&](const auto &O) { return Gone.count(O.first) != 0; }),
                    Order.end());
    }
};

std::string ArchKey(const json &Arch) { return Arch.is_null() ? std::string() : Str(Arch); }

void FoldReg(const json &Tree, const json &Arch, const std::string &Path, const View &V, Plan &P)
{
    for (const auto &[K, Val] : Tree.items())
    {
        const std::string Pth = Path.empty() ? K : Path + "\\" + K;
        std::string Addr = "REG/" + Pth;
        std::replace(Addr.begin(), Addr.end(), '\\', '/');
        if (Val.is_object())
        {
            if (Val.empty())                                     // an empty key: "create this key"
            {
                if (MapFact(V, Addr)) P.RegKeys[ArchKey(Arch) + '\x1f' + Lower(Pth)] = { Arch, Pth };
                continue;
            }
            FoldReg(Val, Arch, Pth, V, P);
        }
        else if (Val.is_null())                                  // null deletes: the value K, and the key Pth with all below it
        {
            if (!MapFact(V, Addr)) continue;
            const std::string A = ArchKey(Arch) + '\x1f', Sub = Lower(Pth);
            const auto Under = [&](const std::string &Key) {     // Key = A + path-lower [+ \x1f name]
                if (Key.compare(0, A.size() + Sub.size(), A + Sub) != 0) return false;
                const size_t E = A.size() + Sub.size();
                return E == Key.size() || Key[E] == '\x1f' || Key[E] == '\\';
            };
            P.Reg.erase(A + Lower(Path) + '\x1f' + Lower(K));
            for (auto It = P.Reg.begin(); It != P.Reg.end();) It = Under(It->first) ? P.Reg.erase(It) : std::next(It);
            for (auto It = P.RegKeys.begin(); It != P.RegKeys.end();) It = Under(It->first) ? P.RegKeys.erase(It) : std::next(It);
        }
        else
        {
            if (!MapFact(V, Addr)) continue;
            P.Reg[ArchKey(Arch) + '\x1f' + Lower(Path) + '\x1f' + Lower(K)] = { Arch, Path, K, Val };
        }
    }
}

// Phase 1: the declarations a layer WHEN may read — ungated VARS, through ungated NODE layers, honouring TAKE.
void Phase1Walk(const Library &Lib, const std::string &Cid, const View &V, std::set<std::string> &Seen, json &Decls)
{
    const std::string Key = Cid + '\x1f' + ViewKey(V);
    const Library::Entry *E = Lib.Find(Cid);
    if (!E || !Seen.insert(Key).second) return;
    if (!E->Node->contains("LAYERS")) return;
    for (const auto &L : (*E->Node)["LAYERS"])
    {
        if (L.contains("WHEN")) continue;
        if (L.contains("NODE")) Phase1Walk(Lib, Str(L["NODE"]), Within(V, L.contains("TAKE") ? &L["TAKE"] : nullptr, std::string()), Seen, Decls);
        else if (L.contains("VARS") && L["VARS"].is_object())
            for (const auto &[K, D] : L["VARS"].items())
                if (auto A = MapFact(V, "VARS/" + K)) Put(Decls, A->substr(A->find('/') + 1), D);
    }
}

} // namespace

std::string PlaceUnder(const std::string &Path, const std::string &Prefix) { return Place(Path, Prefix); }
std::optional<std::string> TakeView(const json &Take, const std::string &Addr) { return Taken(Take, Addr); }

std::optional<std::string> ThroughView(const json &View, std::string Addr)
{
    if (!View.is_array()) return Addr;
    for (const auto &St : View)
    {
        if (!St.is_array() || St.size() != 2) continue;
        if (Takes(&St[0]))
        {
            const auto A = Taken(St[0], Addr);
            if (!A) return std::nullopt;
            Addr = *A;
        }
        if (const std::string At = Str(St[1]); !At.empty()) Addr = "FILES/" + Place(NsPath(Addr).second, At);
    }
    return Addr;
}

std::string TypeOf(const json &Layer)
{
    std::string Found;
    if (!Layer.is_object()) return Found;
    for (const auto &K : TypeKeys())
        if (Layer.contains(K)) { if (!Found.empty()) return std::string(); Found = K; }
    return Found;
}

std::vector<const json *> IntoLayers(const json &Layers)
{
    std::vector<const json *> Out;
    if (!Layers.is_array()) return Out;
    for (const json &L : Layers)
    {
        if (TypeOf(L) != "INTO" || !L["INTO"].is_array()) break;
        Out.push_back(&L);
    }
    return Out;
}

Vars ResolveVars(const json &Decls, const Vars &Builtins, const Vars &Instance)
{
    std::vector<std::string> Keys;
    Vars Src;
    std::map<std::string, std::string> Gate;
    std::set<std::string> Eval;
    if (Decls.is_object())
        for (const auto &[K, D] : Decls.items())
        {
            if (!Src.count(K)) Keys.push_back(K);
            if (D.is_object() && D.contains("EVAL") && D["EVAL"].is_boolean() && D["EVAL"].get<bool>()) Eval.insert(K); else Eval.erase(K);
            const auto It = Instance.find(K);
            Src[K] = It != Instance.end() ? It->second : (D.is_object() && D.contains("DEFAULT") ? Str(D["DEFAULT"]) : std::string());
            std::string W = D.is_object() ? Str(D.value("WHEN", json())) : std::string();
            if (W.empty() && D.is_object() && D.contains("UI") && D["UI"].is_object()) W = Str(D["UI"].value("WHEN", json()));
            if (!W.empty()) Gate[K] = W;
            for (const auto &[E, V] : VarSubst::TableEntries(K, D))           // a MAP table: its entries, gated with it
            {
                if (!Src.count(E)) Keys.push_back(E);
                Src[E] = V;
                if (!W.empty()) Gate[E] = W; else Gate.erase(E);
            }
        }
    for (const auto &[K, V] : Instance) if (!Src.count(K)) { Src[K] = V; Keys.push_back(K); }
    Vars Cur = Src;
    for (int Pass = 0; Pass < 16; ++Pass)
    {
        Vars M = Builtins;
        for (const auto &[K, V] : Cur) M[K] = V;
        Vars Next;
        for (const auto &K : Keys)
        {
            const auto G = Gate.find(K);
            Next[K] = (G != Gate.end() && !VarSubst::EvaluateCondition(G->second, M)) ? std::string() : Subst(Src[K], M);
            long long N;
            if (Eval.count(K) && VarSubst::EvaluateInteger(Next[K], N)) Next[K] = std::to_string(N);   // EVAL: an integer
        }
        if (Next == Cur) break;
        Cur = std::move(Next);
    }
    return Cur;
}

std::string ToLayout(const std::string &Path, const json &Drives)
{
    if (!Drives.is_object()) return Path;
    std::vector<std::string> Anchors;
    for (const auto &[A, V] : Drives.items()) Anchors.push_back(A);
    std::stable_sort(Anchors.begin(), Anchors.end(), [](const std::string &A, const std::string &B) { return A.size() > B.size(); });
    const std::string Pl = Lower(Path);
    for (const auto &A : Anchors)
    {
        const std::string Al = Lower(A);
        if (Pl == Al || Pl.compare(0, Al.size() + 1, Al + "/") == 0) return Str(Drives[A]) + Path.substr(A.size());
    }
    return Path;
}

std::string ToAnchors(const std::string &Value, const std::map<std::string, std::string> &Anchors)
{
    auto Flat = [](std::string X) {                                      // case and separator, position for position
        for (char &C : X) C = C == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(C)));
        return X;
    };
    auto Norm = [&](const std::string &G) {
        std::string X = Flat(G);
        while (X.size() > 3 && X.back() == '/') X.pop_back();          // "C:\\" stays a drive root
        return X;
    };
    std::vector<std::pair<std::string, std::string>> By;                // normalized guest path -> anchor, longest first
    for (const auto &[A, G] : Anchors)
        if (G.size() >= 2 && std::isalpha(static_cast<unsigned char>(G[0])) && G[1] == ':') By.emplace_back(Norm(G), A);
    std::stable_sort(By.begin(), By.end(), [](const auto &X, const auto &Y) { return X.first.size() > Y.first.size(); });
    const std::string L = Flat(Value);
    //A separator in the guest path matches a RUN of them in the value: a typed registry string ("str(2):\"C:\\x\"") is
    //the raw .reg payload, its backslashes escaped — "C:\\Program Files" is still C:\Program Files.
    auto MatchEnd = [&](size_t I, const std::string &G) -> size_t {
        size_t J = I;
        for (size_t K = 0; K < G.size(); ++K)
        {
            if (J >= L.size() || L[J] != G[K]) return std::string::npos;
            ++J;
            if (G[K] == '/') while (J < L.size() && L[J] == '/') ++J;
        }
        return J;
    };
    std::string Out;
    size_t I = 0;
    while (I < Value.size())
    {
        //a path starts at the value's start, after a non-word character, or after a multi-string's "\0" separator
        const bool Start = std::isalpha(static_cast<unsigned char>(Value[I])) && I + 1 < Value.size() && Value[I + 1] == ':'
                        && (I == 0 || !std::isalnum(static_cast<unsigned char>(Value[I - 1]))
                            || (I >= 2 && Value[I - 1] == '0' && Value[I - 2] == '\\'));
        bool Hit = false;
        if (Start)
            for (const auto &[G, A] : By)
            {
                const size_t E = MatchEnd(I, G);
                if (E == std::string::npos) continue;
                const char N = E < Value.size() ? Value[E] : '\0';
                if (N != '\0' && N != '\\' && N != '/' && N != '"' && N != '\'' && N != ',' && N != ';') continue;   // C:\\8020 is not C:\\802
                Out += A;
                I = E;
                Hit = true;
                break;
            }
        if (!Hit) Out += Value[I++];
    }
    return Out;
}

static Plan ResolveImpl(const Library &Lib, const std::string &Root, const Vars &Instance, const Vars &Builtins,
                        const std::vector<std::string> &Grafts, bool ExecOnly, int Flip = -1,
                        const std::vector<std::string> *Selection = nullptr)
{
    Plan P;
    // phase 1
    json Decls1 = json::object();
    std::set<std::string> Seen;
    Phase1Walk(Lib, Root, nullptr, Seen, Decls1);
    //Grafts follow the variant in the list, so their plain declarations count too — a graft's own options (UserPatch's
    //installer features) gate its own layers.
    for (const auto &G : Grafts) Phase1Walk(Lib, G, nullptr, Seen, Decls1);
    Vars Wv = Builtins;
    for (const auto &[K, V] : ResolveVars(Decls1, Builtins, Instance)) Wv[K] = V;
    P.WhenVars = Wv;
    // phase 2
    const std::vector<std::string> &Sel = Selection ? *Selection : Grafts;
    const std::set<std::string> Selected(Sel.begin(), Sel.end());
    Expander X(Lib, Wv, ExecOnly, Flip, &Selected);
    X.Expand(Root, nullptr, std::string(), nullptr);
    //Parts follow the end, each after what it goes into (ApplyGrafts' order: a part after its target, siblings by CID).
    for (const auto &G : Grafts) X.Expand(G, nullptr, std::string(), nullptr);
    P.Parts = Grafts;
    P.Error = X.Error;
    // phase 3
    for (const Slot &S : X.Slots)
    {
        const json &L = *S.Layer;
        if (!ExecOnly) P.Steps.push_back({ S.From, S.At, S.Kind, S.Depth, ViewTakes(S.Through) ? ViewJson(S.Through) : json(), S.Ref });
        if (S.Kind == "NODE" || S.Kind == "INTO") continue;   // shown only
        if (IsContent(S.Kind))
        {
            Item It;
            It.Kind = S.Kind; It.Payload = Str(L[S.Kind]); It.Dir = S.Dir; It.From = S.From; It.At = S.At;
            if (ViewTakes(S.Through)) It.View = ViewJson(S.Through);
            It.Source = L.contains("SOURCE") ? L["SOURCE"] : json();
            It.Size = L.contains("SIZE") ? L["SIZE"] : json();
            It.Own = NsPath(L.contains("TARGET") ? Str(L["TARGET"]) : std::string("FILES")).second;
            It.Target = Place(It.Own, S.Prefix);
            if (S.Kind == "DELTA" && L.contains("BASE_SOURCES")) It.BaseSources = L["BASE_SOURCES"];
            P.Seq.push_back(std::move(It));
        }
        else if (S.Kind == "EDIT")
        {
            const auto A = MapFiles(S.Through, "FILES/" + NsPath(Str(L["TARGET"])).second, S.Prefix);
            if (!A) continue;
            Item It;
            It.Kind = "EDIT"; It.Ops = L["EDIT"]; It.Target = NsPath(*A).second; It.From = S.From; It.At = S.At;
            P.Seq.push_back(std::move(It));
        }
        else if (S.Kind == "REG")
        {
            const json Arches = (L.contains("ARCH") && L["ARCH"].is_array() && !L["ARCH"].empty()) ? L["ARCH"] : json::array({ nullptr });
            for (const auto &Arch : Arches) FoldReg(L["REG"], Arch, std::string(), S.Through, P);
        }
        else if (S.Kind == "DLL")
        {
            for (const auto &[N, O] : L["DLL"].items())
            {
                const auto A = MapFact(S.Through, "DLL/" + N);
                if (!A) continue;
                const std::string N2 = Lower(A->substr(A->find('/') + 1));
                P.Dll.erase(N2);
                if (!O.is_null()) P.Dll[N2] = O;
            }
        }
        else if (S.Kind == "ENV")
        {
            for (const auto &[N, V] : L["ENV"].items())
                if (const auto A = MapFact(S.Through, "ENV/" + N)) Put(P.Env, A->substr(A->find('/') + 1), V);
        }
        else if (S.Kind == "VARS")
        {
            for (const auto &[N, D] : L["VARS"].items())
                if (const auto A = MapFact(S.Through, "VARS/" + N)) Put(P.Decls, A->substr(A->find('/') + 1), D);
        }
        else if (S.Kind == "EXEC")
        {
            for (const auto &E : L["EXEC"])
            {
                const auto A = MapFact(S.Through, "EXEC/" + Str(E.value("LABEL", json())));
                if (!A) continue;
                const std::string Lab = A->substr(A->find('/') + 1);
                json E2 = E;
                E2["LABEL"] = Lab;
                if (!S.Prefix.empty() || ViewTakes(S.Through))         // its paths go where the node's files went
                    for (const char *F : { "EXE", "WORKDIR" })
                        if (E2.contains(F) && E2[F].is_string())
                        {
                            const auto M = MapFiles(S.Through, "FILES/" + Str(E2[F]), S.Prefix);
                            E2[F] = M ? NsPath(*M).second : Place(Str(E2[F]), S.Prefix);
                        }
                if (P.Exec.contains(Lab)) P.Exec[Lab] = Merge(P.Exec[Lab], E2);
                else P.Exec[Lab] = E2;
            }
        }
        else if (S.Kind == "KEEP")
        {
            for (const auto &[A0, V] : L["KEEP"].items())
            {
                const auto A = NsPath(A0).first == "FILES" ? MapFiles(S.Through, A0, S.Prefix) : MapFact(S.Through, A0);
                if (!A) continue;
                Put(P.Keep, *A, V);
            }
        }
    }
    for (const auto &O : X.Order) P.Order.push_back(O.second);
    P.Events = X.Events;
    return P;
}

Plan Resolve(const Library &Lib, const std::string &Root, const Vars &Instance, const Vars &Builtins,
             const std::vector<std::string> &Grafts, const std::vector<std::string> *Selection)
{
    return ResolveImpl(Lib, Root, Instance, Builtins, Grafts, false, -1, Selection);
}

//Two placed targets that can hold the same file: equal, or one a folder of the other (case aside; "" is the root).
static bool TargetsOverlap(const std::string &A, const std::string &B)
{
    const std::string X = Lower(A), Y = Lower(B);
    const std::string &S = X.size() <= Y.size() ? X : Y, &L = X.size() <= Y.size() ? Y : X;
    return S.empty() || L == S || L.compare(0, S.size() + 1, S + "/") == 0;
}

std::vector<std::pair<std::string, std::string>> DecidingMentions(const Library &Lib, const std::string &Root,
                                                                  const Vars &Instance, const Vars &Builtins,
                                                                  const std::vector<std::string> &Grafts, const Plan &P)
{
    std::vector<std::pair<std::string, std::string>> Out;
    std::vector<std::pair<std::string, std::string>> Decided;               // the held/move events, in decision order
    for (const auto &E : P.Events) if (E.first == "held" || E.first == "move") Decided.push_back(E);
    if (Decided.empty()) return Out;
    const json Facts = PlanToJson(P);
    const auto Key = [](const Item &I) { return I.Kind + '\x1f' + I.From + '\x1f' + std::to_string(I.At) + '\x1f' + I.Target; };
    for (int D = 0; D < (int)Decided.size(); ++D)
    {
        const Plan A = ResolveImpl(Lib, Root, Instance, Builtins, Grafts, false, D);
        const json AFacts = PlanToJson(A);
        bool Changed = false;
        for (const char *F : { "reg", "regkeys", "dll", "env", "decls", "exec", "keep" })
            if (Facts.value(F, json()) != AFacts.value(F, json())) { Changed = true; break; }
        //The file stack: only two layers that can hold the same file decide a winner between them — a flipped
        //decision that merely reorders layers at unrelated targets changes nothing anyone sees.
        if (!Changed)
        {
            std::unordered_map<std::string, size_t> At;
            for (size_t I = 0; I < A.Seq.size(); ++I) At.emplace(Key(A.Seq[I]), I);
            Changed = A.Seq.size() != P.Seq.size();
            for (size_t I = 0; I < P.Seq.size() && !Changed; ++I)
            {
                const auto Ai = At.find(Key(P.Seq[I]));
                if (Ai == At.end()) { Changed = true; break; }
                for (size_t J = I + 1; J < P.Seq.size() && !Changed; ++J)
                {
                    if (!TargetsOverlap(P.Seq[I].Target, P.Seq[J].Target)) continue;
                    const auto Aj = At.find(Key(P.Seq[J]));
                    Changed = Aj == At.end() || Aj->second < Ai->second;
                }
            }
        }
        if (Changed) Out.push_back(Decided[(size_t)D]);
    }
    return Out;
}

Plan ResolveEntries(const Library &Lib, const std::string &Root)
{
    return ResolveImpl(Lib, Root, {}, {}, {}, true);
}

GraftIndex BuildGraftIndex(const Library &Lib)
{
    //Keyed by the members of a part's FIRST INTO layer: every part names at least one there, and the offer checks the rest.
    GraftIndex Idx;
    for (const auto &[H, E] : Lib.Nodes)
    {
        if (!E.Node->contains("LAYERS")) continue;
        const auto In = IntoLayers((*E.Node)["LAYERS"]);
        if (In.empty()) continue;
        for (const auto &M : (*In[0])["INTO"]) Idx[Str(M)].push_back(H);
    }
    return Idx;
}

namespace
{
std::vector<const json *> IntoOf(const Library &Lib, const std::string &G)
{
    const auto *E = Lib.Find(G);
    if (!E || !E->Node->contains("LAYERS")) return {};
    return IntoLayers((*E->Node)["LAYERS"]);
}
//How many INTO steps a part is from an end (a part into an end: 1, a part into that part: 2, …): the order parts are
//shown and applied in, so a part always follows what it goes into. Siblings have no order of their own.
int PartDepth(const Library &Lib, const std::string &G, std::set<std::string> &Seen)
{
    const auto In = IntoOf(Lib, G);
    if (In.empty() || !Seen.insert(G).second) return 0;
    int D = 0;
    for (const json *L : In) for (const auto &M : (*L)["INTO"]) D = std::max(D, PartDepth(Lib, Str(M), Seen));
    Seen.erase(G);
    return D + 1;
}
}

Offer OfferedGrafts(const Library &Lib, const GraftIndex &Idx, const Plan &P, const std::string &FaceUid)
{
    //THE HARD LINK (MetaPackageFormat 12 §12.5): a part is offered where what it goes INTO is the end being launched
    //(the root, the first occurrence) or a part applied to it — never because that node merely occurs somewhere
    //beneath (a version under a later version, a game under a derived end). Parts off the end's path are discarded.
    std::set<std::string> Points;
    if (!P.Order.empty()) Points.insert(P.Order.front());
    Points.insert(P.Parts.begin(), P.Parts.end());
    std::set<std::string> Set;
    for (const auto &C : Points)
        if (auto It = Idx.find(C); It != Idx.end()) Set.insert(It->second.begin(), It->second.end());
    //Several INTO layers: the part goes into ALL of them (a patch between two mods), each layer naming alternatives. It
    //is offered only once every layer has a member that is the end or an applied part.
    for (auto It = Set.begin(); It != Set.end();)
    {
        bool All = true;
        for (const json *L : IntoOf(Lib, *It))
        {
            bool One = false;
            for (const auto &M : (*L)["INTO"]) if (Points.count(Str(M))) { One = true; break; }
            if (!One) { All = false; break; }
        }
        It = All ? std::next(It) : Set.erase(It);
    }
    //A part a node of the row contains (FE holds UserPatch in a NODE layer) is part of it, not an option; an applied
    //part sits at the top, contained by nothing, and stays offered.
    for (const auto &C : P.Order)
        if (const auto *E = Lib.Find(C); E && E->Node->contains("LAYERS") && (*E->Node)["LAYERS"].is_array())
            for (const auto &L : (*E->Node)["LAYERS"])
                if (L.is_object() && L.contains("NODE")) Set.erase(Str(L["NODE"]));
    //No order is chosen between parts (none can exist: it would need knowledge of every mod): the list shows a part
    //after what it goes into, then by LABEL, then CID — a presentation, never a fold order.
    auto Label = [&](const std::string &G) { const auto *E = Lib.Find(G); return E ? Str(E->Node->value("LABEL", json())) : std::string(); };
    std::vector<std::pair<std::tuple<int, std::string, std::string>, std::string>> Keyed;
    for (const auto &G : Set) { std::set<std::string> Seen; Keyed.push_back({ { PartDepth(Lib, G, Seen), Label(G), G }, G }); }
    std::sort(Keyed.begin(), Keyed.end());
    Offer O;
    for (const auto &K : Keyed) O.Offered.push_back(K.second);
    for (const auto &G : O.Offered)
    {
        const auto *E = Lib.Find(G);
        if (!E || FaceUid.empty() || !E->Node->contains("RECOMMENDED") || !(*E->Node)["RECOMMENDED"].is_array()) continue;
        for (const auto &U : (*E->Node)["RECOMMENDED"]) if (Str(U) == FaceUid) { O.Ticked.push_back(G); break; }
    }
    return O;
}

int Unsatisfied(const Plan &P)
{
    int N = 0;
    for (const auto &E : P.Events) if (E.first == "into-unmet") ++N;
    return N;
}

std::vector<std::string> ApplyGrafts(const Library &Lib, const GraftIndex &Idx, const std::string &Root, const Vars &Instance,
                                     const Vars &Builtins, const std::string &FaceUid,
                                     const std::vector<std::string> *Requested, std::vector<std::string> *Dropped, bool EveryOffered)
{
    //The selection is a SET. Parts are applied in rounds: each round applies every selected part that is offered on the
    //end with the parts before it applied (a part after what it goes into), siblings by CID — the order the fold reads
    //them in, never a preference. A part whose INTO does not hold (its WHEN), or that leaves a requirement unmet, is
    //dropped and reported.
    std::vector<std::string> Applied;
    std::set<std::string> Refused;
    const auto Has = [](const std::vector<std::string> &V, const std::string &X) { return std::find(V.begin(), V.end(), X) != V.end(); };
    std::vector<std::string> Want;
    if (Requested) { for (const auto &G : *Requested) if (!Has(Want, G)) Want.push_back(G); }
    Plan P = Resolve(Lib, Root, Instance, Builtins, Applied);
    //Judged on the whole selection: a part's WHEN sees every selected part as present (presence is the selection's).
    const auto Trial = [&](const std::string &G, const std::vector<std::string> &Selection) {
        std::vector<std::string> T = Applied; T.push_back(G);
        std::vector<std::string> Sel = Selection; for (const auto &X : T) if (!Has(Sel, X)) Sel.push_back(X);
        return Resolve(Lib, Root, Instance, Builtins, T, &Sel);   // expands T; a WHEN reads the selection
    };
    for (bool Fresh = !Requested;;)
    {
        std::vector<std::string> Round;
        const Offer O = OfferedGrafts(Lib, Idx, P, FaceUid);
        if (Requested) { for (const auto &G : O.Offered) if (Has(Want, G) && !Has(Applied, G) && !Refused.count(G)) Round.push_back(G); }
        else for (const auto &G : EveryOffered ? O.Offered : O.Ticked) if (!Has(Applied, G) && !Refused.count(G)) Round.push_back(G);
        std::sort(Round.begin(), Round.end());
        if (Round.empty()) break;
        bool Any = false;
        for (const auto &G : Round)
        {
            Plan T = Trial(G, Fresh ? Round : Want);
            bool Off = false;
            for (const auto &E : T.Events) if (E.second == G && (E.first == "into-off" || E.first == "into-unmet")) Off = true;
            if (!Off && Unsatisfied(T) <= Unsatisfied(P)) { Applied.push_back(G); P = std::move(T); Any = true; }
            else { Refused.insert(G); if (Dropped) Dropped->push_back(G); }
        }
        if (!Any) break;
    }
    if (Requested && Dropped)
        for (const auto &G : Want) if (!Has(Applied, G) && !Refused.count(G)) Dropped->push_back(G);   // never offered here
    return Applied;
}

json PlanToJson(const Plan &P)
{
    json J = json::object();
    json Seq = json::array();
    for (const Item &I : P.Seq)
    {
        json E = json::object();
        E["kind"] = I.Kind;
        if (I.Kind == "EDIT") E["ops"] = I.Ops;
        else
        {
            E["payload"] = I.Payload; E["dir"] = I.Dir; E["source"] = I.Source; E["size"] = I.Size;
            E["view"] = I.View;
            if (!I.BaseSources.is_null()) E["base_sources"] = I.BaseSources;
        }
        E["target"] = I.Target; E["from"] = I.From; E["at"] = I.At;
        Seq.push_back(std::move(E));
    }
    J["seq"] = Seq;
    json Reg = json::array();
    for (const auto &[K, V] : P.Reg)
        Reg.push_back(json::array({ V.Arch, Lower(V.Path), Lower(V.Name), V.Path, V.Name, V.Value }));
    J["reg"] = Reg;
    json Keys = json::array();
    for (const auto &[K, V] : P.RegKeys) Keys.push_back(json::array({ V.first, Lower(V.second), V.second }));
    J["regkeys"] = Keys;
    auto Pairs = [](const json &Obj) { json A = json::array(); for (const auto &[K, V] : Obj.items()) A.push_back(json::array({ K, V })); return A; };
    J["dll"] = Pairs(P.Dll); J["env"] = Pairs(P.Env); J["exec"] = Pairs(P.Exec); J["keep"] = Pairs(P.Keep); J["decls"] = Pairs(P.Decls);
    J["order"] = P.Order;
    J["parts"] = P.Parts;
    json Ev = json::array();
    for (const auto &[K, C] : P.Events) Ev.push_back(json::array({ K, C }));
    J["events"] = Ev;
    json Wv = json::object();
    for (const auto &[K, V] : P.WhenVars) Wv[K] = V;
    J["when_vars"] = Wv;
    J["error"] = P.Error;
    return J;
}

} // namespace Fold
