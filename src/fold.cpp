#include "fold.h"
#include "varsubst.h"   // EvaluateCondition / RenderValue — the one condition grammar and render formats

#include <algorithm>
#include <cctype>
#include <optional>
#include <set>
#include <unordered_set>

namespace Fold
{

namespace
{

const std::vector<std::string> &TypeKeys()
{
    static const std::vector<std::string> K = { "ZIP", "FILE", "DELTA", "DIR", "NODE", "EDIT", "REG", "VARS", "ENV",
                                                "DLL", "EXEC", "KEEP", "ANY", "NOT" };
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
        const std::string Tok = S.substr(A + 1, B - A - 1);
        const size_t C = Tok.find(':');
        const std::string Key = C == std::string::npos ? Tok : Tok.substr(0, C);
        const std::string Fmt = C == std::string::npos ? std::string() : Tok.substr(C + 1);
        const auto It = V.find(Key);
        if (It != V.end()) Out += Fmt.empty() ? It->second : VarSubst::RenderValue(It->second, Fmt);
        else { Out += '%'; Out += Tok; Out += '%'; }
        Pos = B + 1;
    }
    return Out;
}

bool When(const json &Expr, const Vars &V)
{
    return !Expr.is_string() || VarSubst::EvaluateCondition(Expr.get<std::string>(), V);
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

//EXEC entries fold per field (EXEC/<label>/<field>/…): objects merge, anything else is replaced; null deletes.
json Merge(const json &A, const json &B)
{
    if (!A.is_object() || !B.is_object()) return B;
    json Out = A;
    for (const auto &[K, V] : B.items())
    {
        if (V.is_null()) Out.erase(K);
        else if (V.is_object() && A.contains(K) && A[K].is_object()) Out[K] = Merge(A[K], V);
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
};

class Expander
{
public:
    Expander(const Library &L, const Vars &Wv, bool ExecOnly = false, int Flip = -1)
        : Lib(L), Wv(Wv), ExecOnly(ExecOnly), Flip(Flip) {}

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
        const Chain Inner = std::make_shared<const Occ>(Occ{ Key, Cid, Containers });
        ++OnPath[Cid];
        struct Leave { std::unordered_map<std::string, int> &M; const std::string &C; ~Leave() { if (--M[C] == 0) M.erase(C); } } L_{ OnPath, Cid };
        const json &Layers = E->Node->contains("LAYERS") ? (*E->Node)["LAYERS"] : Empty();
        for (int I = 0; I < static_cast<int>(Layers.size()); ++I)
        {
            const json &L = Layers[I];
            if (L.contains("WHEN") && !When(L["WHEN"], Wv)) continue;
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
                Expand(Str(L["NODE"]), Within(Through, L.contains("TAKE") ? &L["TAKE"] : nullptr, At), Sub, Inner);
            }
            else if (T == "ANY")
            {
                bool Met = false;
                for (const auto &M : L["ANY"])
                    for (const auto &O : Order) if (O.second == Str(M)) { Met = true; break; }
                if (!Met) Events.push_back({ "any-unmet", Cid });
            }
            else if (!ExecOnly || T == "EXEC")
                Slots.push_back({ T, &L, I, Inner, Cid, E->Dir, Prefix, Through });
        }
    }

    const Library &Lib;
    const Vars &Wv;
    const bool ExecOnly;
    const int Flip;                                                         // the held/move decision to take the other way
    int Decisions = 0;                                                      // held/move decisions taken so far
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

std::string TypeOf(const json &Layer)
{
    std::string Found;
    if (!Layer.is_object()) return Found;
    for (const auto &K : TypeKeys())
        if (Layer.contains(K)) { if (!Found.empty()) return std::string(); Found = K; }
    return Found;
}

Vars ResolveVars(const json &Decls, const Vars &Builtins, const Vars &Instance)
{
    std::vector<std::string> Keys;
    Vars Src;
    std::map<std::string, std::string> Gate;
    if (Decls.is_object())
        for (const auto &[K, D] : Decls.items())
        {
            if (!Src.count(K)) Keys.push_back(K);
            const auto It = Instance.find(K);
            Src[K] = It != Instance.end() ? It->second : (D.is_object() && D.contains("DEFAULT") ? Str(D["DEFAULT"]) : std::string());
            std::string W = D.is_object() ? Str(D.value("WHEN", json())) : std::string();
            if (W.empty() && D.is_object() && D.contains("UI") && D["UI"].is_object()) W = Str(D["UI"].value("WHEN", json()));
            if (!W.empty()) Gate[K] = W;
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
    std::string Out;
    size_t I = 0;
    while (I < Value.size())
    {
        const bool Start = std::isalpha(static_cast<unsigned char>(Value[I])) && I + 1 < Value.size() && Value[I + 1] == ':'
                        && (I == 0 || !std::isalnum(static_cast<unsigned char>(Value[I - 1])));
        bool Hit = false;
        if (Start)
            for (const auto &[G, A] : By)
            {
                if (L.compare(I, G.size(), G) != 0) continue;
                const size_t E = I + G.size();
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
                        const std::vector<std::string> &Grafts, bool ExecOnly, int Flip = -1)
{
    Plan P;
    // phase 1
    json Decls1 = json::object();
    std::set<std::string> Seen;
    Phase1Walk(Lib, Root, nullptr, Seen, Decls1);
    Vars Wv = Builtins;
    for (const auto &[K, V] : ResolveVars(Decls1, Builtins, Instance)) Wv[K] = V;
    P.WhenVars = Wv;
    // phase 2
    Expander X(Lib, Wv, ExecOnly, Flip);
    X.Expand(Root, nullptr, std::string(), nullptr);
    for (const auto &G : Grafts) X.Expand(G, nullptr, std::string(), nullptr);    // grafts after the variant, in order
    P.Error = X.Error;
    // phase 3
    std::vector<std::string> Nots;
    for (const Slot &S : X.Slots)
    {
        const json &L = *S.Layer;
        if (S.Kind == "NOT") { Nots.push_back(Str(L["NOT"])); continue; }
        if (IsContent(S.Kind))
        {
            Item It;
            It.Kind = S.Kind; It.Payload = Str(L[S.Kind]); It.Dir = S.Dir; It.From = S.From; It.At = S.At;
            if (ViewTakes(S.Through)) It.View = ViewJson(S.Through);
            It.Source = L.contains("SOURCE") ? L["SOURCE"] : json();
            It.Size = L.contains("SIZE") ? L["SIZE"] : json();
            It.Submounts = L.contains("SUBMOUNTS") ? L["SUBMOUNTS"] : json();
            It.Own = NsPath(L.contains("TARGET") ? Str(L["TARGET"]) : std::string("FILES")).second;
            It.Target = Place(It.Own, S.Prefix);
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
    std::unordered_set<std::string> Present(P.Order.begin(), P.Order.end());
    for (const auto &N : Nots) if (Present.count(N)) P.Events.push_back({ "not-hit", N });
    return P;
}

Plan Resolve(const Library &Lib, const std::string &Root, const Vars &Instance, const Vars &Builtins,
             const std::vector<std::string> &Grafts)
{
    return ResolveImpl(Lib, Root, Instance, Builtins, Grafts, false);
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
    GraftIndex Idx;
    for (const auto &[H, E] : Lib.Nodes)
    {
        if (!E.Node->contains("LAYERS") || !(*E.Node)["LAYERS"].is_array() || (*E.Node)["LAYERS"].empty()) continue;
        const json &L0 = (*E.Node)["LAYERS"][0];
        if (!L0.is_object() || !L0.contains("ANY")) continue;
        for (const auto &M : L0["ANY"]) Idx[Str(M)].push_back(H);
    }
    return Idx;
}

Offer OfferedGrafts(const Library &Lib, const GraftIndex &Idx, const Plan &P, const std::string &FaceUid)
{
    std::set<std::string> Set;
    for (const auto &C : P.Order)
        if (auto It = Idx.find(C); It != Idx.end()) Set.insert(It->second.begin(), It->second.end());
    Offer O;
    O.Offered.assign(Set.begin(), Set.end());
    auto Label = [&](const std::string &G) { const auto *E = Lib.Find(G); return E ? Str(E->Node->value("LABEL", json())) : std::string(); };
    std::sort(O.Offered.begin(), O.Offered.end(), [&](const std::string &A, const std::string &B) {
        const std::string La = Label(A), Lb = Label(B);
        return La != Lb ? La < Lb : A < B;
    });
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
    for (const auto &E : P.Events) if (E.first == "not-hit" || E.first == "any-unmet") ++N;
    return N;
}

std::vector<std::string> ApplyGrafts(const Library &Lib, const GraftIndex &Idx, const std::string &Root, const Vars &Instance,
                                     const Vars &Builtins, const std::string &FaceUid,
                                     const std::vector<std::string> *Requested, std::vector<std::string> *Dropped, bool EveryOffered)
{
    std::vector<std::string> Applied;
    std::set<std::string> Refused;
    Plan P = Resolve(Lib, Root, Instance, Builtins);
    const auto Has = [](const std::vector<std::string> &V, const std::string &X) { return std::find(V.begin(), V.end(), X) != V.end(); };
    //A graft applies when it is offered here and applying it leaves no requirement newly unmet (its NOT, an ANY).
    const auto Attempt = [&](const std::string &G) {
        if (Has(Applied, G) || Refused.count(G)) return;
        if (Has(OfferedGrafts(Lib, Idx, P, FaceUid).Offered, G))
        {
            std::vector<std::string> Trial = Applied;
            Trial.push_back(G);
            Plan T = Resolve(Lib, Root, Instance, Builtins, Trial);
            if (Unsatisfied(T) <= Unsatisfied(P)) { Applied = std::move(Trial); P = std::move(T); return; }
        }
        Refused.insert(G);
        if (Dropped) Dropped->push_back(G);
    };
    if (Requested)
    {
        for (const std::string &G : *Requested) Attempt(G);
        return Applied;
    }
    for (;;)
    {
        const Offer O = OfferedGrafts(Lib, Idx, P, FaceUid);
        std::vector<std::string> New;
        for (const std::string &G : EveryOffered ? O.Offered : O.Ticked) if (!Has(Applied, G) && !Refused.count(G)) New.push_back(G);
        if (New.empty()) return Applied;
        for (const std::string &G : New) Attempt(G);
    }
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
            E["submounts"] = I.Submounts; E["view"] = I.View;
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
