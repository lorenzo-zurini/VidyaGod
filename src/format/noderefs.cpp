#include "noderefs.h"
#include "fold.h"      // Fold::TypeOf — a layer's type key
#include "varsubst.h"  // VarSubst::IsNodeCid — the CIDs a WHEN names

#include <algorithm>
#include <cctype>

using json = nlohmann::ordered_json;

namespace NodeRefs
{

static bool IsNode(const json &N) { return N.is_object() && N.contains("LAYERS") && N["LAYERS"].is_array(); }

//An editor's handle for a node not saved yet ("draft-" + 12 hex digits): a condition may name one until the save
//freezes it to the node's CID, so it is found (and renamed) exactly as a CID is.
static bool IsDraftHandle(const std::string &W)
{
    if (W.size() != 18 || W.compare(0, 6, "draft-") != 0) return false;
    for (size_t k = 6; k < W.size(); ++k) if (!std::isxdigit(static_cast<unsigned char>(W[k]))) return false;
    return true;
}

//The node CIDs a WHEN condition names (MetaPackageFormat 08 §8.6), with where each starts: its bare words shaped like a
//node CID (or a draft handle, before the save), never inside a quoted literal or a %token%.
static std::vector<std::pair<size_t, std::string>> WhenCids(const std::string &E)
{
    std::vector<std::pair<size_t, std::string>> Out;
    const auto Stop = [](char c) { return std::isspace(static_cast<unsigned char>(c)) || c == '(' || c == ')' || c == '!' ||
                                          c == '&' || c == '|' || c == '=' || c == '"' || c == '\''; };
    for (size_t i = 0, n = E.size(); i < n;)
    {
        const char c = E[i];
        if (c == '"' || c == '\'') { const size_t j = E.find(c, i + 1); i = j == std::string::npos ? n : j + 1; continue; }
        if (c == '%') { const size_t j = E.find('%', i + 1); i = j == std::string::npos ? n : j + 1; continue; }
        if (Stop(c)) { ++i; continue; }
        size_t j = i;
        while (j < n && !Stop(E[j]) && E[j] != '%') ++j;
        const std::string W = E.substr(i, j - i);
        if (VarSubst::IsNodeCid(W) || IsDraftHandle(W)) Out.push_back({ i, W });
        i = j;
    }
    return Out;
}

std::vector<std::string> CidsInWhen(const std::string &Expr)
{
    std::vector<std::string> Out;
    for (const auto &P : WhenCids(Expr)) Out.push_back(P.second);
    return Out;
}

std::vector<std::string> IntoOf(const json &Node)
{
    std::vector<std::string> Out;
    if (!IsNode(Node)) return Out;
    for (const json *L : Fold::IntoLayers(Node["LAYERS"]))
        for (const json &M : (*L)["INTO"]) if (M.is_string()) Out.push_back(M.get<std::string>());
    return Out;
}

std::vector<std::string> Linked(const json &Node)
{
    std::vector<std::string> Out = Contained(Node);
    for (std::string &C : IntoOf(Node)) Out.push_back(std::move(C));
    return Out;
}

void Each(const json &Node, const std::function<void(const std::string &)> &Fn)
{
    if (!IsNode(Node)) return;
    for (const json &L : Node["LAYERS"])
    {
        if (!L.is_object()) continue;
        if (L.contains("NODE") && L["NODE"].is_string()) Fn(L["NODE"].get<std::string>());
        if (L.contains("INTO") && L["INTO"].is_array())
            for (const json &M : L["INTO"]) if (M.is_string()) Fn(M.get<std::string>());
        if (L.contains("WHEN") && L["WHEN"].is_string()) for (const auto &C : CidsInWhen(L["WHEN"].get<std::string>())) Fn(C);
        if (L.contains("CODED") && L.contains("DESCRIPTION") && L["DESCRIPTION"].is_string()) Fn(L["DESCRIPTION"].get<std::string>());
    }
}

std::vector<std::string> Contained(const json &Node, std::vector<std::string> *Requires)
{
    std::vector<std::string> Out;
    if (!IsNode(Node)) return Out;
    for (const json &L : Node["LAYERS"])
    {
        if (!L.is_object()) continue;
        if (L.contains("NODE") && L["NODE"].is_string()) Out.push_back(L["NODE"].get<std::string>());
        if (!Requires) continue;
        if (L.contains("INTO") && L["INTO"].is_array())
            for (const json &M : L["INTO"]) if (M.is_string()) Requires->push_back(M.get<std::string>());
        if (L.contains("WHEN") && L["WHEN"].is_string())
            for (const auto &C : CidsInWhen(L["WHEN"].get<std::string>())) Requires->push_back(C);
        if (L.contains("CODED") && L.contains("DESCRIPTION") && L["DESCRIPTION"].is_string())
            Requires->push_back(L["DESCRIPTION"].get<std::string>());   // its description's node (contained by a NODE layer too)
    }
    return Out;
}

bool Remap(json &Node, const std::function<std::string(const std::string &)> &Map)
{
    if (!IsNode(Node)) return false;
    bool Changed = false;
    const auto One = [&](json &S) {
        if (!S.is_string()) return;
        std::string New = Map(S.get<std::string>());
        if (New != S.get<std::string>()) { S = std::move(New); Changed = true; }
    };
    for (json &L : Node["LAYERS"])
    {
        if (!L.is_object()) continue;
        if (L.contains("NODE")) One(L["NODE"]);
        if (L.contains("INTO") && L["INTO"].is_array()) for (json &M : L["INTO"]) One(M);
        if (L.contains("CODED") && L.contains("DESCRIPTION")) One(L["DESCRIPTION"]);   // a coded stream's description node
        if (L.contains("WHEN") && L["WHEN"].is_string())
        {
            //a CID inside a condition is renamed in place, the rest of the condition kept byte for byte
            std::string E = L["WHEN"].get<std::string>();
            const auto Found = WhenCids(E);
            for (auto It = Found.rbegin(); It != Found.rend(); ++It)
            {
                const std::string New = Map(It->second);
                if (New != It->second) { E.replace(It->first, It->second.size(), New); Changed = true; }
            }
            L["WHEN"] = E;
        }
    }
    return Changed;
}

json Remapped(const json &Node, const std::map<std::string, std::string> &M)
{
    json Out = Node;
    if (!M.empty())
        Remap(Out, [&](const std::string &R) { const auto It = M.find(R); return It == M.end() ? R : It->second; });
    return Out;
}

bool Add(json &Node, const std::string &Ref)
{
    if (!Node.is_object() || Ref.empty()) return false;
    if (!Node.contains("LAYERS") || Node["LAYERS"].is_null()) Node["LAYERS"] = json::array();
    if (!Node["LAYERS"].is_array()) return false;
    json &Ls = Node["LAYERS"];
    size_t At = 0;
    for (const auto &L : Ls)
    {
        if (Fold::TypeOf(L) == "NODE" && L["NODE"].is_string() && L["NODE"].get<std::string>() == Ref) return false;
    }
    while (At < Ls.size() && Fold::TypeOf(Ls[At]) == "NODE") ++At;
    Ls.insert(Ls.begin() + (long)At, json::object({{"NODE", Ref}}));
    return true;
}

bool Insert(json &Node, const std::string &Type, const std::string &Ref, int At)
{
    if (At < 0 && Type == "NODE") return Add(Node, Ref);
    if (!Node.is_object() || Ref.empty() || (Type != "NODE" && Type != "INTO")) return false;
    if (!Node.contains("LAYERS") || Node["LAYERS"].is_null()) Node["LAYERS"] = json::array();
    if (!Node["LAYERS"].is_array()) return false;
    json &Ls = Node["LAYERS"];
    if (Type == "INTO")
    {   //A part's list opens with its INTO layers: each is a list of alternatives, and the part goes into all of them.
        //At picks the layer (-1 or 0: the first; one past the last: a new layer, "and also into"); Ref joins it. A
        //node that is not a part becomes one.
        size_t Run = 0;
        while (Run < Ls.size() && Fold::TypeOf(Ls[Run]) == "INTO" && Ls[Run]["INTO"].is_array()) ++Run;
        for (size_t K = 0; K < Run; ++K)
            if (std::find(Ls[K]["INTO"].begin(), Ls[K]["INTO"].end(), json(Ref)) != Ls[K]["INTO"].end()) return false;
        const size_t L = At < 0 ? 0 : static_cast<size_t>(At);
        if (L > Run) return false;
        if (L == Run) Ls.insert(Ls.begin() + static_cast<long>(Run), json::object({{"INTO", json::array()}}));
        Ls[L]["INTO"].push_back(Ref);
        return true;
    }
    for (const auto &L : Ls)
        if (Fold::TypeOf(L) == Type && L[Type].is_string() && L[Type].get<std::string>() == Ref) return false;
    At = At < 0 ? (int)Ls.size() : std::min(At, (int)Ls.size());
    Ls.insert(Ls.begin() + At, json::object({{Type, Ref}}));
    return true;
}

bool Names(const json &L, const std::string &Ref)
{
    if (!L.is_object()) return false;
    if (L.contains("NODE") && L["NODE"].is_string() && L["NODE"].get<std::string>() == Ref) return true;
    if (L.contains("INTO") && L["INTO"].is_array())
        for (const json &M : L["INTO"]) if (M.is_string() && M.get<std::string>() == Ref) return true;
    if (L.contains("WHEN") && L["WHEN"].is_string())
        for (const auto &C : CidsInWhen(L["WHEN"].get<std::string>())) if (C == Ref) return true;
    return false;
}

bool Remove(json &Node, const std::string &Ref)
{
    //NODE layers naming Ref go, and Ref leaves an INTO's list (a part going into nothing more stops being a part). A
    //condition naming Ref is left as written: a node that is not there reads false (08 §8.6), which is what it says.
    if (!Node.is_object() || !Node.contains("LAYERS") || !Node["LAYERS"].is_array()) return false;
    json Kept = json::array();
    bool Changed = false;
    auto Is = [&](const json &V) { return V.is_string() && V.get<std::string>() == Ref; };
    for (const auto &L : Node["LAYERS"])
    {
        const std::string T = Fold::TypeOf(L);
        if (T == "NODE" && Is(L["NODE"])) { Changed = true; continue; }
        if (T == "INTO" && L["INTO"].is_array())
        {
            json A = json::array();
            for (const auto &M : L["INTO"]) { if (Is(M)) { Changed = true; continue; } A.push_back(M); }
            if (A.empty() && !L["INTO"].empty()) continue;
            json L2 = L; L2["INTO"] = std::move(A);
            Kept.push_back(std::move(L2));
            continue;
        }
        Kept.push_back(L);
    }
    if (Changed) Node["LAYERS"] = std::move(Kept);
    return Changed;
}

} // namespace NodeRefs
