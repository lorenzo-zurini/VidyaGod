#include "noderefs.h"
#include "fold.h"   // Fold::TypeOf — a layer's type key

#include <algorithm>

using json = nlohmann::ordered_json;

namespace NodeRefs
{

static bool IsNode(const json &N) { return N.is_object() && N.contains("LAYERS") && N["LAYERS"].is_array(); }

void Each(const json &Node, const std::function<void(const std::string &)> &Fn)
{
    if (!IsNode(Node)) return;
    for (const json &L : Node["LAYERS"])
    {
        if (!L.is_object()) continue;
        for (const char *K : {"NODE", "NOT"})
            if (L.contains(K) && L[K].is_string()) Fn(L[K].get<std::string>());
        if (L.contains("ANY") && L["ANY"].is_array())
            for (const json &M : L["ANY"]) if (M.is_string()) Fn(M.get<std::string>());
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
        if (L.contains("ANY") && L["ANY"].is_array())
            for (const json &M : L["ANY"]) if (M.is_string()) Requires->push_back(M.get<std::string>());
        if (L.contains("NOT") && L["NOT"].is_string()) Requires->push_back(L["NOT"].get<std::string>());
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
        for (const char *K : {"NODE", "NOT"}) if (L.contains(K)) One(L[K]);
        if (L.contains("ANY") && L["ANY"].is_array()) for (json &M : L["ANY"]) One(M);
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
    if (!Node.is_object() || Ref.empty() || (Type != "NODE" && Type != "NOT")) return false;
    if (!Node.contains("LAYERS") || Node["LAYERS"].is_null()) Node["LAYERS"] = json::array();
    if (!Node["LAYERS"].is_array()) return false;
    json &Ls = Node["LAYERS"];
    for (const auto &L : Ls)
        if (Fold::TypeOf(L) == Type && L[Type].is_string() && L[Type].get<std::string>() == Ref) return false;
    At = At < 0 ? (int)Ls.size() : std::min(At, (int)Ls.size());
    Ls.insert(Ls.begin() + At, json::object({{Type, Ref}}));
    return true;
}

bool Names(const json &L, const std::string &Ref)
{
    if (!L.is_object()) return false;
    auto Is = [&](const char *K) { return L.contains(K) && L[K].is_string() && L[K].get<std::string>() == Ref; };
    if (Is("NODE") || Is("NOT")) return true;
    if (L.contains("ANY") && L["ANY"].is_array())
        for (const json &M : L["ANY"]) if (M.is_string() && M.get<std::string>() == Ref) return true;
    return false;
}

bool Remove(json &Node, const std::string &Ref)
{
    if (!Node.is_object() || !Node.contains("LAYERS") || !Node["LAYERS"].is_array()) return false;
    json Kept = json::array();
    bool Changed = false;
    auto Names = [&](const json &V) { return V.is_string() && V.get<std::string>() == Ref; };
    for (const auto &L : Node["LAYERS"])
    {
        const std::string T = Fold::TypeOf(L);
        if ((T == "NODE" && Names(L["NODE"])) || (T == "NOT" && Names(L["NOT"]))) { Changed = true; continue; }
        if (T == "ANY" && L["ANY"].is_array())
        {
            json A = json::array();
            for (const auto &M : L["ANY"]) { if (Names(M)) { Changed = true; continue; } A.push_back(M); }
            if (A.empty() && !L["ANY"].empty()) continue;
            json L2 = L; L2["ANY"] = std::move(A);
            Kept.push_back(std::move(L2));
            continue;
        }
        Kept.push_back(L);
    }
    if (Changed) Node["LAYERS"] = std::move(Kept);
    return Changed;
}

} // namespace NodeRefs
