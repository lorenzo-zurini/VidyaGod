#include "fold.h"
#include "varsubst.h"   // EvaluateCondition / RenderValue — the one condition grammar and render formats

#include <algorithm>
#include <deque>
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
    static const std::vector<std::string> K = { "ZIP", "FILE", "DELTA", "CODED", "DIR", "NODE", "EDIT", "REG", "VARS", "ENV",
                                                "DLL", "EXEC", "KEEP", "INTO" };
    return K;
}

bool IsContent(const std::string &T) { return T == "ZIP" || T == "FILE" || T == "DELTA" || T == "CODED" || T == "DIR"; }

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
    //the EXEC tree (09): a subtree is kept at its own address; REG addresses are one key path, its keys separated by
    //backslashes (07): a key selects itself and every key below it; facts are never re-rooted
    if (Addr.compare(0, 5, "EXEC/") == 0 && Sel.compare(0, 5, "EXEC/") == 0)
    {
        std::string B = Sel; while (!B.empty() && B.back() == '/') B.pop_back();
        if (Addr == B || Addr.compare(0, B.size() + 1, B + "/") == 0) return Addr.substr(5);
        return std::nullopt;
    }
    if (Addr.compare(0, 4, "REG/") == 0 && Sel.compare(0, 4, "REG/") == 0)
    {
        std::string B = Sel; while (!B.empty() && (B.back() == '/' || B.back() == '\\')) B.pop_back();
        const std::string LA = Lower(Addr), LB = Lower(B);
        if (B == "REG" || LA == LB || LA.compare(0, LB.size() + 1, LB + "\\") == 0) return Addr.substr(4);
        return std::nullopt;
    }
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
    bool FilesOnly = false;                 // a part's footprint (its INTO's TAKE): it places files, never filters facts
};
using View = std::shared_ptr<const Step>;

bool Takes(const json *T) { return T && !T->is_null() && !(T->is_array() && T->empty()); }

View Within(const View &V, const json *Take, std::string At, bool FilesOnly = false)
{
    return std::make_shared<const Step>(Step{ Take, std::move(At), V, Takes(Take) || (V && V->AnyTake), FilesOnly });
}

bool ViewTakes(const View &V) { return V && V->AnyTake; }

//A footprint's TAKE as a NODE's would read it: a file or a folder it names STAYS where it is (a NODE's TAKE left-strips
//it under its TARGET; a footprint has no TARGET: what it does not rename keeps its place), so each such selection is
//the pair that names it as itself; the whole namespace and pairs stay as they are.
json FootprintTake(const json &Take)
{
    json Out = json::array();
    for (const auto &Sel : Take)
    {
        if (!Sel.is_string()) { Out.push_back(Sel); continue; }
        std::string S = Sel.get<std::string>();
        while (!S.empty() && S.back() == '/') S.pop_back();
        if (S.find('/') == std::string::npos) Out.push_back(S);            // "FILES": every file, where it is
        else Out.push_back(json::array({ S, S }));
    }
    return Out;
}

std::string ViewKey(const View &V)
{
    if (!ViewTakes(V)) return std::string();
    std::string K;
    for (const Step *S = V.get(); S; S = S->Next.get()) { K += S->FilesOnly ? "F" : ""; K += Takes(S->Take) ? S->Take->dump() : "-"; K += '\x1e'; K += S->At; K += '\x1d'; }
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
        if (!Takes(S->Take) || S->FilesOnly) continue;                    // a footprint never filters facts
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
            if (A) Addr = *A;
            else if (!S->FilesOnly) return std::nullopt;
            //a footprint selects the part's CONTENT (NodeLower mounts it through the view); an edit's file or a kept folder
            //it does not name is the part's still, where it is — a footprint renames such an address, never drops it
        }
        if (!S->At.empty()) Addr = "FILES/" + Place(NsPath(Addr).second, S->At);
    }
    return Addr;
}

void Put(json &Obj, const std::string &K, const json &V)          // a later writer moves to the end (python dict)
{
    Obj.erase(K);
    Obj[K] = V;
}

// ---- composition (gen 9, MetaPackageFormat 05 §5.4) -------------------------------------------------------------
//Each occurrence (CID, TAKE view, TARGET prefix) is composed once, from its own list: its own layers are PROVIDED; a
//child's own layers are provided to the parent (everything a NODE layer with a TAKE takes is); anything deeper is
//CARRIED. Laid over what is composed so far: a carried slot already present is a stale copy (skipped); a provided one
//already present moves to the later place (later wins), except what CARRIED slots after it in its item rewrite (its
//old place kept, the moved copy excepting those addresses); anything new is appended.
struct Slot
{
    std::string Kind;
    const json *Layer = nullptr;
    int At = 0;
    std::string Owner;         // the occurrence whose own layer this is
    std::string From, Dir, Prefix;
    View Through;
    int Depth = 0;             // how many NODE layers in, where it was first composed (the editor's fold view)
    std::string Ref;           // NODE (only shown): the node the library does not hold; INTO is shown too
    std::vector<std::string> Exc;   // a re-provided copy: the addresses it leaves to what is below (sorted)
};

std::string OccKey(const std::string &Cid, const View &V, const std::string &Prefix) { return Cid + '\x1f' + ViewKey(V) + '\x1f' + Prefix; }

//A REG layer's path tree with its KEY (the key its paths are under, 07) put in front.
json RegTree(const json &L)
{
    const std::string Base = L.contains("KEY") ? Str(L["KEY"]) : std::string();
    if (Base.empty() || !L["REG"].is_object()) return L["REG"];
    json Out = json::object();
    for (const auto &[P, V] : L["REG"].items()) Out[P.empty() ? Base : Base + "\\" + P] = V;
    return Out;
}

class Composer
{
public:
    Composer(const Library &L, const Vars &Wv, bool ExecOnly, const std::set<std::string> *Members, const std::set<std::string> *Selected)
        : Lib(L), Wv(Wv), ExecOnly(ExecOnly), Members(Members), Selected(Selected) {}

    //A WHEN naming a node CID reads the FINAL composition (phase 0's members) or a part selected for the launch.
    bool Present(const std::string &Cid) const { return (Members && Members->count(Cid)) || (Selected && Selected->count(Cid)); }

    static std::string Identity(const Slot &S)
    {
        const json &L = *S.Layer;
        if (IsContent(S.Kind) && L.contains("SOURCE") && L["SOURCE"].is_string() && !Str(L["SOURCE"]).empty())
            return "C\x1f" + S.Kind + '\x1f' + Str(L["SOURCE"]) + '\x1f' + (L.contains("TARGET") ? Str(L["TARGET"]) : std::string("FILES"))
                 + '\x1f' + S.Prefix + '\x1f' + ViewKey(S.Through);
        return "W\x1f" + S.Owner + '\x1f' + std::to_string(S.At);
    }

    //The addresses a slot writes, as (raw, mapped): raw in the slot's own coordinates (what the fold checks an except
    //against), mapped where its view lands it (what coverage compares). EXEC is one coordinate (its restore works on
    //mapped paths). Content has none (an archive's insides are not the resolver's to know). resolve.py _addr_pairs.
    static std::vector<std::pair<std::string, std::string>> AddrPairs(const Slot &S)
    {
        std::vector<std::pair<std::string, std::string>> Out;
        const json &L = *S.Layer;
        const auto Same = [&](const std::string &A) { Out.emplace_back(A, A); };
        if (S.Kind == "EDIT") Same("FILES/" + Place(NsPath(Str(L["TARGET"])).second, S.Prefix));
        else if (S.Kind == "REG" && L["REG"].is_object())
        {
            for (const auto &[P, Vals] : RegTree(L).items())
            {
                if (!MapFact(S.Through, "REG/" + P)) continue;     // REG is taken where it is: the view only filters
                if (!Vals.is_object() || Vals.empty()) Same("REG/" + Lower(P));
                if (Vals.is_object()) for (const auto &[V, X] : Vals.items()) Same("REG/" + Lower(P) + "#" + Lower(V));
            }
        }
        else if ((S.Kind == "DLL" || S.Kind == "ENV" || S.Kind == "VARS") && L[S.Kind].is_object())
        {
            for (const auto &[N, X] : L[S.Kind].items())
                if (const auto A = MapFact(S.Through, S.Kind + "/" + N))
                {
                    if (S.Kind == "DLL") Out.emplace_back("DLL/" + Lower(N), "DLL/" + Lower(A->substr(A->find('/') + 1)));
                    else Out.emplace_back(S.Kind + "/" + N, *A);
                }
        }
        else if (S.Kind == "KEEP" && L["KEEP"].is_object())
        {
            for (const auto &[A00, X] : L["KEEP"].items())             // mapped as the fold maps it (FILES placed, REG by key)
            {
                std::string Inner = A00, Unit;
                bool IsValue = false;
                if (NsPath(Inner).first == "REG" && Inner.find('#') != std::string::npos)
                { const size_t H = Inner.rfind('#'); Unit = Inner.substr(H + 1); Inner = Inner.substr(0, H); IsValue = true; }
                const auto M = NsPath(Inner).first == "FILES" ? MapFiles(S.Through, Inner, S.Prefix) : MapFact(S.Through, Inner);
                if (M) Out.emplace_back("KEEP/" + A00, "KEEP/" + *M + (IsValue ? "#" + Unit : std::string()));
            }
        }
        else if (S.Kind == "EXEC" && L["EXEC"].is_object())
        {
            const std::function<void(const std::string &, const json &)> Walk = [&](const std::string &Addr, const json &V) {
                if (const auto A = MapFact(S.Through, Addr)) { Same(*A); return; }
                if (V.is_object()) for (const auto &[F, W] : V.items()) Walk(Addr + "/" + F, W);
            };
            for (const auto &[N, X] : L["EXEC"].items()) Walk("EXEC/" + N, X);
        }
        return Out;
    }

    //Where a slot's writes land (mapped).
    static std::set<std::string> Addrs(const Slot &S)
    {
        std::set<std::string> Out;
        for (const auto &[R, M] : AddrPairs(S)) Out.insert(M);
        return Out;
    }

    //Address A covers M: the same, or A holds it (a key its values and subkeys, an entry its fields).
    static bool Covers(const std::string &A, const std::string &M)
    {
        if (A == M) return true;
        const auto Starts = [&](const std::string &P) { return M.size() > P.size() && M.compare(0, P.size(), P) == 0; };
        return Starts(A + "/") || (A.rfind("REG/", 0) == 0 && (Starts(A + "#") || Starts(A + "\\")));
    }

    static std::string ContentTarget(const Slot &S)
    {
        const json &L = *S.Layer;
        return Place(NsPath(L.contains("TARGET") ? Str(L["TARGET"]) : std::string("FILES")).second, S.Prefix);
    }

    //Does re-providing X change what wins: something after its old place writes where it writes?
    static bool Overrides(const Slot &X, const std::vector<Slot> &Cur, size_t From)
    {
        if (IsContent(X.Kind))
        {
            const std::string T = Lower(ContentTarget(X));
            const auto Under = [&](std::string U) {
                U = Lower(U);
                return T.empty() || U.empty() || U == T || U.compare(0, T.size() + 1, T + "/") == 0 || T.compare(0, U.size() + 1, U + "/") == 0;
            };
            for (size_t I = From; I < Cur.size(); ++I)
            {
                const Slot &Y = Cur[I];
                if (IsContent(Y.Kind) && Under(ContentTarget(Y))) return true;
                if (Y.Kind == "EDIT" && Under(Place(NsPath(Str((*Y.Layer)["TARGET"])).second, Y.Prefix))) return true;
            }
            return false;
        }
        const auto Mine = Addrs(X);
        for (size_t I = From; I < Cur.size(); ++I)
            for (const auto &A : Addrs(Cur[I]))
                for (const auto &M : Mine) if (Covers(A, M) || Covers(M, A)) return true;
        return false;
    }

    //What a moved slot X must not re-provide: what CARRIED slots after it in its item rewrite — an address of X's they
    //write or hold (a key deleted over X's value), or a part of an address X writes whole (the tile of an entry X
    //writes): that part stays as below.
    static std::set<std::string> ExceptFor(const Slot &X, const std::vector<const Slot *> &Later)
    {
        std::set<std::string> Out;
        if (IsContent(X.Kind))
        {
            const std::string T = "FILES/" + ContentTarget(X);
            std::string Tr = T; while (!Tr.empty() && Tr.back() == '/') Tr.pop_back();
            for (const Slot *Y : Later)
                if (Y->Kind == "EDIT")
                    for (const auto &A : Addrs(*Y))
                        if (A == T || A.compare(0, Tr.size() + 1, Tr + "/") == 0 || T == "FILES/") Out.insert(A);
            return Out;
        }
        const auto Mine = AddrPairs(X);
        for (const Slot *Y : Later)
            for (const auto &A : Addrs(*Y))
                for (const auto &[R, M] : Mine)
                {
                    if (Covers(A, M)) Out.insert(R);                      // the fold skips X's own address (raw)
                    //X writes the whole of it: an entry keeps that part from below (mapped: its restore path); a registry
                    //key written whole stays below whole; anything else, X's own address
                    else if (Covers(M, A)) Out.insert(M.rfind("EXEC/", 0) == 0 ? A : R);
                }
        return Out;
    }

    //An item's composition laid over what was composed BEFORE it: its slots are matched against that only, never
    //against each other (an item's composition is already decided — its own re-provided copy among it).
    std::vector<Slot> Combine(std::vector<Slot> Cur, const std::vector<Slot> &Sub, const std::string &Owner, bool TakeAll)
    {
        std::unordered_map<std::string, size_t> Idx;
        for (size_t I = 0; I < Cur.size(); ++I) Idx[Identity(Cur[I])] = I;
        std::vector<char> Drop(Cur.size(), 0);
        for (size_t J = 0; J < Sub.size(); ++J)
        {
            Slot X = Sub[J];
            const std::string K = Identity(X);
            const bool Provided = TakeAll || X.Owner == Owner;
            const auto It = Idx.find(K);
            if (It != Idx.end() && !Drop[It->second])
            {
                if (!Provided) continue;
                if (Overrides(X, Cur, It->second + 1))
                {
                    const std::pair<std::string, std::string> Ev{ "raise", X.From };
                    if (std::find(Events.begin(), Events.end(), Ev) == Events.end()) Events.push_back(Ev);
                }
                std::vector<const Slot *> Later;
                for (size_t L2 = J + 1; L2 < Sub.size(); ++L2) if (!(TakeAll || Sub[L2].Owner == Owner)) Later.push_back(&Sub[L2]);
                auto Exc = ExceptFor(X, Later);
                if (!Exc.empty())
                {
                    Exc.insert(X.Exc.begin(), X.Exc.end());
                    X.Exc.assign(Exc.begin(), Exc.end());
                    const Slot &Prev = Cur[It->second];          // an earlier re-provided copy this one covers goes
                    if (!Prev.Exc.empty() && std::includes(X.Exc.begin(), X.Exc.end(), Prev.Exc.begin(), Prev.Exc.end())) Drop[It->second] = 1;
                }
                else Drop[It->second] = 1;
            }
            Cur.push_back(std::move(X));
            Drop.push_back(0);
        }
        std::vector<Slot> Out;
        Out.reserve(Cur.size());
        for (size_t I = 0; I < Cur.size(); ++I) if (!Drop[I]) Out.push_back(std::move(Cur[I]));
        return Out;
    }

    std::vector<Slot> Compose(const std::string &Cid, View Through, const std::string &Prefix, int Depth = 0)
    {
        const std::string Key = OccKey(Cid, Through, Prefix);
        if (auto M = Memo.find(Key); M != Memo.end()) return M->second;
        if (OnPath.count(Cid)) { Events.push_back({ "cycle", Cid }); return {}; }
        const Library::Entry *E = Lib.Find(Cid);
        if (!E) { Events.push_back({ "missing", Cid }); return {}; }
        const json &Layers = E->Node->contains("LAYERS") ? (*E->Node)["LAYERS"] : Empty();
        const VarSubst::Presence Pres = [this](const std::string &C) { return Present(C); };
        //A part whose INTO does not hold here is not applied at all, not partly, and is not in the composition.
        for (const json *In : IntoLayers(Layers))
            if (In->contains("WHEN") && !When((*In)["WHEN"], Wv, Pres)) { Events.push_back({ "into-off", Cid }); Memo[Key] = {}; return {}; }
        //A part's FOOTPRINT (23 §23.4): its INTO layers' TAKEs place its files, in layer order (the first innermost).
        {
            const auto Ins = IntoLayers(Layers);
            for (auto It = Ins.rbegin(); It != Ins.rend(); ++It)
                if ((*It)->contains("TAKE")) { Footprints.push_back(FootprintTake((**It)["TAKE"])); Through = Within(Through, &Footprints.back(), std::string(), true); }
        }
        if (!InOrder.count(Key)) { InOrder.insert(Key); Order.push_back({ Key, Cid }); }
        ++Count[Cid];
        ++OnPath[Cid];
        struct Leave { std::unordered_map<std::string, int> &M; const std::string &C; ~Leave() { if (--M[C] == 0) M.erase(C); } } L_{ OnPath, Cid };
        std::vector<Slot> Res;
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
                if (!ExecOnly && !Lib.Find(Ref)) Res = Combine(std::move(Res), { Slot{ T, &L, I, Key, Cid, E->Dir, Prefix, V, Depth, Ref, {} } }, Key, true);
                Res = Combine(std::move(Res), Compose(Ref, V, Sub, Depth + 1), OccKey(Ref, V, Sub), L.contains("TAKE"));
            }
            else if (T == "INTO")
            {
                //What a part goes into must be in the FINAL composition (judged after it, ResolveImpl), or selected.
                IntoChecks.push_back({ Cid, &L["INTO"] });
                if (!ExecOnly) Res = Combine(std::move(Res), { Slot{ T, &L, I, Key, Cid, E->Dir, Prefix, Through, Depth, {}, {} } }, Key, true);
            }
            else if (!ExecOnly || T == "EXEC")
                Res = Combine(std::move(Res), { Slot{ T, &L, I, Key, Cid, E->Dir, Prefix, Through, Depth, {}, {} } }, Key, true);
        }
        Memo[Key] = Res;
        return Res;
    }

    const Library &Lib;
    const Vars &Wv;
    const bool ExecOnly;
    const std::set<std::string> *Members;                                   // phase 0: the composition's nodes
    const std::set<std::string> *Selected;                                  // the parts selected for the launch (WHEN presence)
    std::unordered_map<std::string, int> Count;                             // compositions per node
    std::deque<json> Footprints;            // the footprints views point into (a deque: they never move)
    std::unordered_map<std::string, std::vector<Slot>> Memo;
    std::unordered_map<std::string, int> OnPath;                             // nodes being composed right now (cycle guard)
    std::unordered_set<std::string> InOrder;
    std::vector<std::pair<std::string, std::string>> Order;                  // (occurrence key, cid), first composition order
    std::vector<std::pair<std::string, const json *>> IntoChecks;            // (part, its INTO members)
    std::vector<std::pair<std::string, std::string>> Events;
    std::string Error;

private:
    static const json &Empty() { static const json E = json::array(); return E; }
};

std::string ArchKey(const json &Arch) { return Arch.is_null() ? std::string() : Str(Arch); }

//REG is a path tree (07): key path (backslashes, the hive first) -> {value name: value}; {} creates the key; null deletes
//the key and all below it; a value null deletes the value. Addresses: REG/<key path> (a value: REG/<key path>#<value>).
void FoldReg(const json &Tree, const json &Arch, const View &V, Plan &P, const std::set<std::string> &Exc)
{
    if (!Tree.is_object()) return;
    for (const auto &[Pth, Vals] : Tree.items())
    {
        if (!MapFact(V, "REG/" + Pth) || Exc.count("REG/" + Lower(Pth))) continue;
        const std::string A = ArchKey(Arch) + '\x1f';
        if (Vals.is_null())
        {
            const std::string Sub = Lower(Pth);
            const auto Under = [&](const std::string &Key) {     // Key = A + path-lower [+ \x1f name]
                if (Key.compare(0, A.size() + Sub.size(), A + Sub) != 0) return false;
                const size_t E = A.size() + Sub.size();
                return E == Key.size() || Key[E] == '\x1f' || Key[E] == '\\';
            };
            for (auto It = P.Reg.begin(); It != P.Reg.end();) It = Under(It->first) ? P.Reg.erase(It) : std::next(It);
            for (auto It = P.RegKeys.begin(); It != P.RegKeys.end();) It = Under(It->first) ? P.RegKeys.erase(It) : std::next(It);
            continue;
        }
        if (!Vals.is_object()) continue;
        if (Vals.empty()) { P.RegKeys[A + Lower(Pth)] = { Arch, Pth }; continue; }
        for (const auto &[K, Val] : Vals.items())
        {
            if (Exc.count("REG/" + Lower(Pth) + "#" + Lower(K))) continue;
            if (Val.is_null()) P.Reg.erase(A + Lower(Pth) + '\x1f' + Lower(K));
            else P.Reg[A + Lower(Pth) + '\x1f' + Lower(K)] = { Arch, Pth, K, Val };
        }
    }
}

//An entry's file paths go where the node's files went.
json PlaceEntryPaths(json E, const View &V, const std::string &Prefix)
{
    if ((!Prefix.empty() || ViewTakes(V)) && E.is_object())
        for (const char *F : { "EXE", "WORKDIR" })
            if (E.contains(F) && E[F].is_string())
            {
                const auto M = MapFiles(V, "FILES/" + Str(E[F]), Prefix);
                E[F] = M ? NsPath(*M).second : Place(Str(E[F]), Prefix);
            }
    return E;
}

//Write Value at EXEC address Addr (whole), as the view maps it (09 §9.2: nothing folds); where the view takes only parts
//of it, each taken subtree lands at its own (possibly renamed) address.
//Cur at Parts back to what it was in Before (removed where Before had none), in place.
void RestorePath(json &Cur, const json &Before, const std::vector<std::string> &Parts)
{
    if (Parts.empty()) return;
    const json *Src = &Before;
    for (const auto &Q : Parts) { if (!Src->is_object() || !Src->contains(Q)) { Src = nullptr; break; } Src = &(*Src)[Q]; }
    json *N = &Cur;
    for (size_t I = 0; I + 1 < Parts.size(); ++I)
    {
        if (!N->is_object() || !N->contains(Parts[I]) || !(*N)[Parts[I]].is_object())
        {
            if (!Src) return;
            (*N)[Parts[I]] = json::object();
        }
        N = &(*N)[Parts[I]];
    }
    if (!Src) { if (N->is_object()) N->erase(Parts.back()); }
    else (*N)[Parts.back()] = *Src;
}

std::vector<std::string> Split(const std::string &S, char C)
{
    std::vector<std::string> Out;
    for (size_t B = 0, E; B <= S.size(); B = E + 1) { E = S.find(C, B); if (E == std::string::npos) E = S.size(); Out.push_back(S.substr(B, E - B)); }
    return Out;
}

void FoldExec(json &Exec, const std::string &Addr, const json &Value, const View &V, const std::string &Prefix)
{
    if (const auto A = MapFact(V, Addr))
    {
        std::vector<std::string> Parts;
        for (size_t S = A->find('/') + 1, E; S <= A->size(); S = E + 1) { E = A->find('/', S); if (E == std::string::npos) E = A->size(); Parts.push_back(A->substr(S, E - S)); }
        if (Parts.size() == 1)
        {
            if (Value.is_null()) { Exec.erase(Parts[0]); return; }   // null deletes the entry (an inherited label renamed)
            Put(Exec, Parts[0], PlaceEntryPaths(Value, V, Prefix));
            return;
        }
        json Val = Value;
        if ((Parts[1] == "EXE" || Parts[1] == "WORKDIR") && Val.is_string() && (!Prefix.empty() || ViewTakes(V)))
        {
            const auto M = MapFiles(V, "FILES/" + Str(Val), Prefix);
            Val = M ? NsPath(*M).second : Place(Str(Val), Prefix);
        }
        if (!Exec.contains(Parts[0]) || !Exec[Parts[0]].is_object()) Exec[Parts[0]] = json::object();
        json *N = &Exec[Parts[0]];
        for (size_t I = 1; I + 1 < Parts.size(); ++I)
        {
            if (!N->contains(Parts[I]) || !(*N)[Parts[I]].is_object()) (*N)[Parts[I]] = json::object();
            N = &(*N)[Parts[I]];
        }
        (*N)[Parts.back()] = Val;
        return;
    }
    if (Value.is_object())
        for (const auto &[F, X] : Value.items()) FoldExec(Exec, Addr + "/" + F, X, V, Prefix);
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

bool OwnsGameEntry(const json &Node)
{
    if (!Node.is_object() || !Node.contains("LAYERS") || !Node["LAYERS"].is_array()) return false;
    for (const auto &L : Node["LAYERS"])
        if (L.is_object() && L.contains("EXEC") && L["EXEC"].is_object())
            for (const auto &[Lab, E] : L["EXEC"].items())
                if (E.is_object() && !(E.contains("GUEST") && E["GUEST"].is_array() && !E["GUEST"].empty())) return true;
    return false;
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
                        const std::vector<std::string> &Grafts, bool ExecOnly, const std::vector<std::string> *Selection = nullptr)
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
    // phase 0 + composition: a WHEN naming a node CID reads the FINAL composition — composed again until its members
    // are stable; members that come back are conditions contradicting each other (refused)
    const std::vector<std::string> &Sel = Selection ? *Selection : Grafts;
    const std::set<std::string> Selected(Sel.begin(), Sel.end());
    std::set<std::string> Members;
    std::vector<std::set<std::string>> Before;
    std::unique_ptr<Composer> X;
    std::vector<Slot> Res;
    constexpr int MembershipRounds = 64;   // the fixpoint's bound (12 §12.1): beyond it, refused (resolve.py MEMBERSHIP_ROUNDS)
    bool Settled = false;
    for (int Round = 0; Round < MembershipRounds; ++Round)
    {
        X = std::make_unique<Composer>(Lib, Wv, ExecOnly, &Members, &Selected);
        Res = X->Compose(Root, nullptr, std::string());
        //Parts follow the end, each after what it goes into (ApplyGrafts' order: a part after its target, siblings by CID).
        for (const auto &G : Grafts) Res = X->Combine(std::move(Res), X->Compose(G, nullptr, std::string()), OccKey(G, nullptr, std::string()), true);
        std::set<std::string> Got;
        for (const auto &[C, N] : X->Count) Got.insert(C);
        if (Got == Members) { Settled = true; break; }
        if (std::find(Before.begin(), Before.end(), Got) != Before.end())
        {
            std::string Diff;
            std::vector<std::string> Sym;
            std::set_symmetric_difference(Got.begin(), Got.end(), Members.begin(), Members.end(), std::back_inserter(Sym));
            for (const auto &C : Sym) Diff += (Diff.empty() ? "" : ", ") + C;
            P.Error = "the conditions naming nodes contradict each other: " + Diff;
            Settled = true;
            break;
        }
        Before.push_back(Members);
        Members = std::move(Got);
    }
    if (!Settled) P.Error = "the conditions naming nodes did not settle in " + std::to_string(MembershipRounds) + " rounds";
    P.Parts = Grafts;
    if (P.Error.empty()) P.Error = X->Error;
    for (const auto &[Part, In] : X->IntoChecks)                           // INTO judged on the final composition
    {
        bool Met = false;
        if (In->is_array()) for (const auto &M : *In) if (X->Count.count(Str(M)) || Selected.count(Str(M))) { Met = true; break; }
        if (!Met) X->Events.push_back({ "into-unmet", Part });
    }
    // the fold
    for (const Slot &S : Res)
    {
        const json &L = *S.Layer;
        const std::set<std::string> Exc(S.Exc.begin(), S.Exc.end());
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
            if ((S.Kind == "DELTA" || S.Kind == "CODED") && L.contains("BASE_SOURCES")) It.BaseSources = L["BASE_SOURCES"];
            if (S.Kind == "CODED")
            {   //a coded stream: its format's description (the layer's own) and the name it appears by
                It.Description = L.contains("DESCRIPTION") ? L["DESCRIPTION"] : json();
                It.Name = L.contains("NAME") ? Str(L["NAME"]) : std::string();
            }
            for (const auto &A : S.Exc) It.Except.push_back(NsPath(A).second);   // a re-provided copy: these stay below
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
            const json Tree = RegTree(L);
            for (const auto &Arch : Arches) FoldReg(Tree, Arch, S.Through, P, Exc);
        }
        else if (S.Kind == "DLL")
        {
            for (const auto &[N, O] : L["DLL"].items())
            {
                if (Exc.count("DLL/" + Lower(N))) continue;
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
                if (!Exc.count("ENV/" + N))
                    if (const auto A = MapFact(S.Through, "ENV/" + N)) Put(P.Env, A->substr(A->find('/') + 1), V);
        }
        else if (S.Kind == "VARS")
        {
            for (const auto &[N, D] : L["VARS"].items())
                if (!Exc.count("VARS/" + N))
                    if (const auto A = MapFact(S.Through, "VARS/" + N)) Put(P.Decls, A->substr(A->find('/') + 1), D);
        }
        else if (S.Kind == "EXEC")
        {
            if (L["EXEC"].is_object())
            {
                const bool Keep = std::any_of(Exc.begin(), Exc.end(), [](const std::string &A) { return A.rfind("EXEC/", 0) == 0; });
                const json Before = Keep ? P.Exec : json();
                for (const auto &[Lab, E] : L["EXEC"].items()) FoldExec(P.Exec, "EXEC/" + Lab, E, S.Through, S.Prefix);
                for (const std::string &A : Exc)                 // a re-provided copy: these parts stay as below
                    if (A.rfind("EXEC/", 0) == 0) RestorePath(P.Exec, Before, Split(A.substr(5), '/'));
            }
        }
        else if (S.Kind == "KEEP")
        {
            for (const auto &[A00, V] : L["KEEP"].items())
            {
                if (Exc.count("KEEP/" + A00)) continue;
                std::string A0 = A00, Unit;
                bool IsValue = false;   // a value: <key>#<value> (13) — the default value is the empty name, "<key>#"
                if (NsPath(A0).first == "REG" && A0.find('#') != std::string::npos)
                {
                    const size_t H = A0.rfind('#');
                    Unit = A0.substr(H + 1); A0 = A0.substr(0, H); IsValue = true;
                }
                auto A = NsPath(A0).first == "FILES" ? MapFiles(S.Through, A0, S.Prefix) : MapFact(S.Through, A0);
                if (!A) continue;
                if (IsValue) *A += "#" + Unit;
                Put(P.Keep, *A, V);
            }
        }
    }
    for (const auto &O : X->Order) P.Order.push_back(O.second);
    P.Events = X->Events;
    return P;
}

Plan Resolve(const Library &Lib, const std::string &Root, const Vars &Instance, const Vars &Builtins,
             const std::vector<std::string> &Grafts, const std::vector<std::string> *Selection)
{
    return ResolveImpl(Lib, Root, Instance, Builtins, Grafts, false, Selection);
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
    //Pre-ticked: a part whose INTO layer naming what it goes into here says RECOMMENDED — never one carrying an entry of
    //its own (a graft card or row is launched from its own face, 09 / 23).
    (void)FaceUid;
    for (const auto &G : O.Offered)
    {
        const auto *E = Lib.Find(G);
        if (!E || !E->Node->contains("LAYERS") || !(*E->Node)["LAYERS"].is_array()) continue;
        if (OwnsGameEntry(*E->Node)) continue;
        for (const json *L : IntoOf(Lib, G))
        {
            if (!(L->contains("RECOMMENDED") && (*L)["RECOMMENDED"].is_boolean() && (*L)["RECOMMENDED"].get<bool>())) continue;
            bool Here = false;
            for (const auto &M : (*L)["INTO"]) if (Points.count(Str(M))) { Here = true; break; }
            if (Here) { O.Ticked.push_back(G); break; }
        }
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
    for (;;)
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
            //judged beside what can be present: the parts offered this round and those applied — never a requested part
            //that is not offered here (it will not be in the launch); the final pass below settles later rounds
            Plan T = Trial(G, Round);
            bool Off = false;
            for (const auto &E : T.Events) if (E.second == G && (E.first == "into-off" || E.first == "into-unmet")) Off = true;
            if (!Off && Unsatisfied(T) <= Unsatisfied(P)) { Applied.push_back(G); P = std::move(T); Any = true; }
            else { Refused.insert(G); if (Dropped) Dropped->push_back(G); }
        }
        if (!Any) break;
    }
    //Judged once more as it will launch: the composition is what is applied (a requested part not offered here is not
    //present). A part off or unmet there goes, until nothing changes — else a part judged beside a part that never
    //came would launch off, or leave the row blocked.
    for (;;)
    {
        const Plan F = Resolve(Lib, Root, Instance, Builtins, Applied, &Applied);
        std::set<std::string> Bad;
        for (const auto &E : F.Events)
            if ((E.first == "into-off" || E.first == "into-unmet") && Has(Applied, E.second)) Bad.insert(E.second);
        if (Bad.empty()) break;
        std::vector<std::string> Kept;
        for (const auto &G : Applied) if (!Bad.count(G)) Kept.push_back(G);
        Applied = std::move(Kept);
        for (const auto &G : Bad) { Refused.insert(G); if (Dropped) Dropped->push_back(G); }
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
            if (I.Kind == "CODED") { E["description"] = I.Description; E["name"] = I.Name; }
            if (!I.Except.empty()) E["except"] = I.Except;
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
