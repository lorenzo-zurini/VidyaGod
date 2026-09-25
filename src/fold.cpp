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

//What TAKE keeps of an address, renamed; nullopt = not taken. A composed take is {"outer", "inner"}: inner first.
std::optional<std::string> Taken(const json &Take, const std::string &Addr)
{
    if (Take.is_null() || (Take.is_array() && Take.empty())) return Addr;
    if (Take.is_object())
    {
        auto A = Taken(Take["inner"], Addr);
        return A ? Taken(Take["outer"], *A) : std::nullopt;
    }
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

json ComposeTake(const json &Outer, const json &Inner)
{
    const bool NoOuter = Outer.is_null() || (Outer.is_array() && Outer.empty());
    const bool NoInner = Inner.is_null() || (Inner.is_array() && Inner.empty());
    if (NoOuter) return NoInner ? json() : Inner;
    if (NoInner) return Outer;
    json J = json::object();
    J["outer"] = Outer;
    J["inner"] = Inner;
    return J;
}

std::string TakeKey(const json &Take)
{
    return (Take.is_null() || (Take.is_array() && Take.empty())) ? std::string() : Take.dump();
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

bool ChainHasCid(Chain C, const std::string &Cid) { for (; C; C = C->Parent) if (C->Cid == Cid) return true; return false; }
bool ChainHasKey(Chain C, const std::string &Key) { for (; C; C = C->Parent) if (C->Key == Key) return true; return false; }

struct Slot
{
    std::string Kind;
    const json *Layer = nullptr;
    int At = 0;
    Chain Inner;               // the containers + this node's own occurrence
    std::string From, Dir, Prefix;
    json Take;
};

class Expander
{
public:
    Expander(const Library &L, const Vars &Wv) : Lib(L), Wv(Wv) {}

    void Expand(const std::string &Cid, const json &Take, const std::string &Prefix, Chain Containers)
    {
        const std::string Key = Cid + '\x1f' + TakeKey(Take) + '\x1f' + Prefix;
        if (ChainHasCid(Containers, Cid)) { Events.push_back({ "cycle", Cid }); return; }
        if (auto It = First.find(Key); It != First.end())
        {
            const Chain Prev = It->second;
            if (Prev && Containers && Prev->Key == Containers->Key)          // a later mention in the SAME list moves it
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
        const json &Layers = E->Node.contains("LAYERS") ? E->Node["LAYERS"] : Empty();
        for (int I = 0; I < static_cast<int>(Layers.size()); ++I)
        {
            const json &L = Layers[I];
            if (L.contains("WHEN") && !When(L["WHEN"], Wv)) continue;
            const std::string T = TypeOf(L);
            if (T.empty()) { Error = "node " + Cid + " layer " + std::to_string(I) + " has no single type key"; continue; }
            if (T == "NODE")
            {
                std::string Sub = Prefix;
                if (L.contains("TARGET"))
                {
                    const auto [Ns, P] = NsPath(Str(L["TARGET"]));
                    if (Ns != "FILES") { Error = "node " + Cid + ": NODE TARGET outside FILES"; continue; }
                    Sub = Place(P, Prefix);
                }
                Expand(Str(L["NODE"]), ComposeTake(Take, L.contains("TAKE") ? L["TAKE"] : json()), Sub, Inner);
            }
            else if (T == "ANY")
            {
                bool Met = false;
                for (const auto &M : L["ANY"])
                    for (const auto &O : Order) if (O.second == Str(M)) { Met = true; break; }
                if (!Met) Events.push_back({ "any-unmet", Cid });
            }
            else
                Slots.push_back({ T, &L, I, Inner, Cid, E->Dir, Prefix, Take });
        }
    }

    const Library &Lib;
    const Vars &Wv;
    std::vector<Slot> Slots;
    std::unordered_map<std::string, Chain> First;
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

void FoldReg(const json &Tree, const json &Arch, const std::string &Path, const json &Take, Plan &P)
{
    for (const auto &[K, V] : Tree.items())
    {
        const std::string Pth = Path.empty() ? K : Path + "\\" + K;
        std::string Addr = "REG/" + Pth;
        std::replace(Addr.begin(), Addr.end(), '\\', '/');
        if (V.is_object())
        {
            if (V.empty())                                     // an empty key: "create this key"
            {
                if (Taken(Take, Addr)) P.RegKeys[ArchKey(Arch) + '\x1f' + Lower(Pth)] = { Arch, Pth };
                continue;
            }
            FoldReg(V, Arch, Pth, Take, P);
        }
        else if (V.is_null())
        {
            if (!Path.empty()) P.Reg.erase(ArchKey(Arch) + '\x1f' + Lower(Path) + '\x1f' + Lower(K));   // null deletes
        }
        else
        {
            if (!Taken(Take, Addr)) continue;
            P.Reg[ArchKey(Arch) + '\x1f' + Lower(Path) + '\x1f' + Lower(K)] = { Arch, Path, K, V };
        }
    }
}

// Phase 1: the declarations a layer WHEN may read — ungated VARS, through ungated NODE layers, honouring TAKE.
void Phase1Walk(const Library &Lib, const std::string &Cid, const json &Take, std::set<std::string> &Seen, json &Decls)
{
    const std::string Key = Cid + '\x1f' + TakeKey(Take);
    const Library::Entry *E = Lib.Find(Cid);
    if (!E || !Seen.insert(Key).second) return;
    if (!E->Node.contains("LAYERS")) return;
    for (const auto &L : E->Node["LAYERS"])
    {
        if (L.contains("WHEN")) continue;
        if (L.contains("NODE")) Phase1Walk(Lib, Str(L["NODE"]), ComposeTake(Take, L.contains("TAKE") ? L["TAKE"] : json()), Seen, Decls);
        else if (L.contains("VARS") && L["VARS"].is_object())
            for (const auto &[K, D] : L["VARS"].items())
                if (auto A = Taken(Take, "VARS/" + K)) Put(Decls, A->substr(A->find('/') + 1), D);
    }
}

} // namespace

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

std::string ToLayout(const std::string &Path, const json &GuestRoots)
{
    if (!GuestRoots.is_object()) return Path;
    std::vector<std::string> Anchors;
    for (const auto &[A, V] : GuestRoots.items()) Anchors.push_back(A);
    std::stable_sort(Anchors.begin(), Anchors.end(), [](const std::string &A, const std::string &B) { return A.size() > B.size(); });
    const std::string Pl = Lower(Path);
    for (const auto &A : Anchors)
    {
        const std::string Al = Lower(A);
        if (Pl == Al || Pl.compare(0, Al.size() + 1, Al + "/") == 0) return Str(GuestRoots[A]) + Path.substr(A.size());
    }
    return Path;
}

Plan Resolve(const Library &Lib, const std::string &Root, const Vars &Instance, const Vars &Builtins,
             const std::vector<std::string> &Grafts)
{
    Plan P;
    // phase 1
    json Decls1 = json::object();
    std::set<std::string> Seen;
    Phase1Walk(Lib, Root, json(), Seen, Decls1);
    Vars Wv = Builtins;
    for (const auto &[K, V] : ResolveVars(Decls1, Builtins, Instance)) Wv[K] = V;
    P.WhenVars = Wv;
    // phase 2
    Expander X(Lib, Wv);
    X.Expand(Root, json(), std::string(), nullptr);
    for (const auto &G : Grafts) X.Expand(G, json(), std::string(), nullptr);    // grafts after the variant, in order
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
            It.Kind = S.Kind; It.Payload = Str(L[S.Kind]); It.Dir = S.Dir; It.From = S.From; It.At = S.At; It.Take = S.Take;
            It.Source = L.contains("SOURCE") ? L["SOURCE"] : json();
            It.Submounts = L.contains("SUBMOUNTS") ? L["SUBMOUNTS"] : json();
            It.Target = Place(NsPath(L.contains("TARGET") ? Str(L["TARGET"]) : std::string("FILES")).second, S.Prefix);
            P.Seq.push_back(std::move(It));
        }
        else if (S.Kind == "EDIT")
        {
            const auto A = Taken(S.Take, "FILES/" + NsPath(Str(L["TARGET"])).second);
            if (!A) continue;
            Item It;
            It.Kind = "EDIT"; It.Ops = L["EDIT"]; It.Target = Place(NsPath(*A).second, S.Prefix); It.From = S.From; It.At = S.At;
            P.Seq.push_back(std::move(It));
        }
        else if (S.Kind == "REG")
        {
            const json Arches = (L.contains("ARCH") && L["ARCH"].is_array() && !L["ARCH"].empty()) ? L["ARCH"] : json::array({ nullptr });
            for (const auto &Arch : Arches) FoldReg(L["REG"], Arch, std::string(), S.Take, P);
        }
        else if (S.Kind == "DLL")
        {
            for (const auto &[N, O] : L["DLL"].items())
            {
                const auto A = Taken(S.Take, "DLL/" + N);
                if (!A) continue;
                const std::string N2 = Lower(A->substr(A->find('/') + 1));
                P.Dll.erase(N2);
                if (!O.is_null()) P.Dll[N2] = O;
            }
        }
        else if (S.Kind == "ENV")
        {
            for (const auto &[N, V] : L["ENV"].items())
                if (const auto A = Taken(S.Take, "ENV/" + N)) Put(P.Env, A->substr(A->find('/') + 1), V);
        }
        else if (S.Kind == "VARS")
        {
            for (const auto &[N, D] : L["VARS"].items())
                if (const auto A = Taken(S.Take, "VARS/" + N)) Put(P.Decls, A->substr(A->find('/') + 1), D);
        }
        else if (S.Kind == "EXEC")
        {
            for (const auto &E : L["EXEC"])
            {
                const auto A = Taken(S.Take, "EXEC/" + Str(E.value("LABEL", json())));
                if (!A) continue;
                const std::string Lab = A->substr(A->find('/') + 1);
                json E2 = E;
                E2["LABEL"] = Lab;
                if (!S.Prefix.empty())
                    for (const char *F : { "EXE", "WORKDIR" })
                        if (E2.contains(F)) E2[F] = Place(Str(E2[F]), S.Prefix);
                if (P.Exec.contains(Lab)) P.Exec[Lab] = Merge(P.Exec[Lab], E2);
                else P.Exec[Lab] = E2;
            }
        }
        else if (S.Kind == "KEEP")
        {
            for (const auto &[A0, V] : L["KEEP"].items())
            {
                auto A = Taken(S.Take, A0);
                if (!A) continue;
                const auto [Ns, Pth] = NsPath(*A);
                if (Ns == "FILES") A = "FILES/" + Place(Pth, S.Prefix);
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

GraftIndex BuildGraftIndex(const Library &Lib)
{
    GraftIndex Idx;
    for (const auto &[H, E] : Lib.Nodes)
    {
        if (!E.Node.contains("LAYERS") || !E.Node["LAYERS"].is_array() || E.Node["LAYERS"].empty()) continue;
        const json &L0 = E.Node["LAYERS"][0];
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
    auto Label = [&](const std::string &G) { const auto *E = Lib.Find(G); return E ? Str(E->Node.value("LABEL", json())) : std::string(); };
    std::sort(O.Offered.begin(), O.Offered.end(), [&](const std::string &A, const std::string &B) {
        const std::string La = Label(A), Lb = Label(B);
        return La != Lb ? La < Lb : A < B;
    });
    for (const auto &G : O.Offered)
    {
        const auto *E = Lib.Find(G);
        if (!E || FaceUid.empty() || !E->Node.contains("RECOMMENDED") || !E->Node["RECOMMENDED"].is_array()) continue;
        for (const auto &U : E->Node["RECOMMENDED"]) if (Str(U) == FaceUid) { O.Ticked.push_back(G); break; }
    }
    return O;
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
            E["payload"] = I.Payload; E["dir"] = I.Dir; E["source"] = I.Source;
            E["submounts"] = I.Submounts; E["take"] = I.Take;
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
