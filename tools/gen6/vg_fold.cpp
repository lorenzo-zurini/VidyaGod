// vg_fold — the C++ resolver (src/fold.cpp) in batch, for tools/gen6/fold_gate.py.
// argv[1] = a library root (every *.json with CID + LAYERS below it, _friend_* dirs skipped).
// stdin: one case per line — {"id", "root", "instance": {}, "builtins": {}, "grafts": [], "face": uid?}
// argv[2] == --cids: print every node's canonical CID instead (handle<TAB>cid).
// argv[2] == --entries-check: ResolveEntries(node).Exec == Resolve(node).Exec for every node.
// stdout: one line per case — {"id", "plan": PlanToJson, "offered": [...], "ticked": [...]}
#include "cid.h"
#include "fold.h"

#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using Fold::json;

static Fold::Vars ToVars(const json &J)
{
    Fold::Vars V;
    if (J.is_object()) for (const auto &[K, X] : J.items()) V[K] = X.is_string() ? X.get<std::string>() : X.dump();
    return V;
}

int main(int argc, char **argv)
{
    if (argc < 2) { std::cerr << "usage: vg_fold <library-root> < cases.jsonl\n"; return 2; }
    Fold::Library Lib;
    std::deque<json> Store;                                    // owns the node JSON the library points into
    for (auto It = fs::recursive_directory_iterator(argv[1]); It != fs::recursive_directory_iterator(); ++It)
    {
        if (It->is_directory() && It->path().filename().string().rfind("_friend_", 0) == 0) { It.disable_recursion_pending(); continue; }
        if (!It->is_regular_file() || It->path().extension() != ".json") continue;
        try
        {
            std::ifstream F(It->path());
            json J = json::parse(F);
            if (J.is_object() && J.contains("CID") && J.contains("LAYERS"))
            {
                Store.push_back(std::move(J));
                Lib.Nodes[Store.back()["CID"].get<std::string>()] = { &Store.back(), It->path().parent_path().string() };
            }
        }
        catch (const std::exception &) {}
    }
    if (argc > 2 && std::string(argv[2]) == "--cids")          // every node's gen-6 identity: handle<TAB>cid
    {
        for (const auto &[H, E] : Lib.Nodes)
        {
            std::string Err;
            std::cout << H << '\t' << Cid::OfNode(*E.Node, &Err) << '\n';
        }
        return 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--entries-check")  // ResolveEntries must fold EXEC exactly as Resolve
    {
        size_t Bad = 0;
        for (const auto &[H, E] : Lib.Nodes)
            if (Fold::ResolveEntries(Lib, H).Exec != Fold::Resolve(Lib, H).Exec) { if (++Bad <= 3) std::cout << "DIFFERS " << H << '\n'; }
        std::cout << "entries check: " << Lib.Nodes.size() << " nodes, " << Bad << " differ\n";
        return Bad ? 1 : 0;
    }
    const Fold::GraftIndex GIdx = Fold::BuildGraftIndex(Lib);
    std::string Line;
    while (std::getline(std::cin, Line))
    {
        if (Line.empty()) continue;
        const json C = json::parse(Line);
        std::vector<std::string> Grafts;
        for (const auto &G : C.value("grafts", json::array())) Grafts.push_back(G.get<std::string>());
        const Fold::Plan P = Fold::Resolve(Lib, C["root"].get<std::string>(), ToVars(C.value("instance", json())),
                                           ToVars(C.value("builtins", json())), Grafts);
        json Out = { { "id", C["id"] }, { "plan", Fold::PlanToJson(P) } };
        if (C.contains("face"))
        {
            const Fold::Offer O = Fold::OfferedGrafts(Lib, GIdx, P, C["face"].is_string() ? C["face"].get<std::string>() : std::string());
            Out["offered"] = O.Offered;
            Out["ticked"] = O.Ticked;
            const std::string Face = C["face"].is_string() ? C["face"].get<std::string>() : std::string();
            const std::string Root = C["root"].get<std::string>();
            Out["applied"] = Fold::ApplyGrafts(Lib, GIdx, Root, ToVars(C.value("instance", json())), ToVars(C.value("builtins", json())), Face, nullptr);
            Out["applied_req"] = Fold::ApplyGrafts(Lib, GIdx, Root, ToVars(C.value("instance", json())), ToVars(C.value("builtins", json())), Face, &Grafts);
        }
        std::cout << Out.dump() << '\n';
    }
    return 0;
}
