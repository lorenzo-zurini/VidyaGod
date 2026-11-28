#ifndef NODEFIXTURE_H
#define NODEFIXTURE_H

#include <nlohmann/json.hpp>
#include "manifestmodel.h"   // Node for Wire()

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Test scaffolding for nodes: {CID, LABEL, VARIANT?, LAYERS}.
//
// A SECTION is a partial node — {"LAYERS": [...]} and/or node fields ({"VARIANT": …}). `Chain` builds a chain of
// nodes from sections — one node per section, each CONTAINING the previous through a leading NODE layer (the
// parent is a layer) — and returns a JSON ARRAY of node objects, ready to write one per file or to feed ParseNode.
//
// The LAST node owns the bare `Id`: a launchable is the head of its chain (it contains its content), and tests
// address it by its natural name. Earlier links get "<Id>__l<N>". `Over` refs are contained by the FIRST node.
// Node fields for the head (VARIANT) go in TailFields or in the last section.
// ---------------------------------------------------------------------------

namespace NodeFixture
{

// Merge sections into ONE node object: LAYERS concatenate in order, other fields are set. The shorthands' EXEC
// layers become the node's ONE EXEC layer, an entry's fields combined across them: Exec(...) and Tile(...) in one
// Merge write one Play entry that runs and has a tile (an entry is replaced whole, so two layers would not).
inline nlohmann::ordered_json Merge(const std::vector<nlohmann::ordered_json> &Sections)
{
    nlohmann::ordered_json Out = nlohmann::ordered_json::object();
    int ExecAt = -1;
    for (const auto &S : Sections)
        for (const auto &[K, V] : S.items())
        {
            if (K == "LAYERS")
            {
                if (!Out.contains("LAYERS")) Out["LAYERS"] = nlohmann::ordered_json::array();
                for (const auto &L : V)
                {
                    if (L.is_object() && L.size() == 1 && L.contains("EXEC") && L["EXEC"].is_object())
                    {
                        if (ExecAt < 0) { ExecAt = (int)Out["LAYERS"].size(); Out["LAYERS"].push_back({ {"EXEC", nlohmann::ordered_json::object()} }); }
                        auto &Ex = Out["LAYERS"][(size_t)ExecAt]["EXEC"];
                        for (const auto &[Lab, E] : L["EXEC"].items())
                        {
                            if (!Ex.contains(Lab)) Ex[Lab] = nlohmann::ordered_json::object();
                            for (const auto &[F, X] : E.items()) Ex[Lab][F] = X;
                        }
                        continue;
                    }
                    Out["LAYERS"].push_back(L);
                }
            }
            else Out[K] = V;
        }
    return Out;
}

// An EXEC object from built entries, keyed by each entry's LABEL ("Play" when it has none), the LABEL moving to the
// key: for tests whose helpers build entries one by one.
template <class J>
inline J ExecOf(std::initializer_list<J> Entries)
{
    J Out = J::object();
    for (J E : Entries)
    {
        const std::string Lab = E.contains("LABEL") && E["LABEL"].is_string() ? E["LABEL"].template get<std::string>() : std::string("Play");
        E.erase("LABEL");
        Out[Lab] = std::move(E);
    }
    return Out;
}

inline nlohmann::ordered_json Chain(const std::string &Id,
                                    const std::vector<nlohmann::ordered_json> &Sections,
                                    const std::vector<std::string> &Over = {},
                                    const nlohmann::ordered_json &TailFields = nlohmann::ordered_json::object())
{
    nlohmann::ordered_json Out = nlohmann::ordered_json::array();
    const size_t Count = Sections.empty() ? 1 : Sections.size();
    for (size_t I = 0; I < Count; ++I)
    {
        const nlohmann::ordered_json Sec = Sections.empty() ? nlohmann::ordered_json::object() : Sections[I];
        const bool Tail = (I + 1 == Count);
        const std::string Handle = Tail ? Id : (Id + "__l" + std::to_string(I));
        nlohmann::ordered_json Layers = nlohmann::ordered_json::array();
        if (I == 0) for (const std::string &X : Over) Layers.push_back({ {"NODE", X} });
        else        Layers.push_back({ {"NODE", Id + "__l" + std::to_string(I - 1)} });
        // The readable Id doubles as the working-tree handle (the stored "CID") so NODE refs resolve at freeze;
        // LABEL matches it so files and pickers read naturally.
        nlohmann::ordered_json Nd = { {"CID", Handle}, {"LABEL", Handle} };
        for (const auto &[K, V] : Sec.items())
        {
            if (K == "LAYERS") for (const auto &L : V) Layers.push_back(L);
            else Nd[K] = V;
        }
        if (Tail && TailFields.is_object())
            for (const auto &[K, V] : TailFields.items())
            {
                if (K == "LAYERS") for (const auto &L : V) Layers.push_back(L);
                else Nd[K] = V;
            }
        Nd["LAYERS"] = std::move(Layers);
        Out.push_back(std::move(Nd));
    }
    return Out;
}

// ---- section shorthands (the format's vocabulary, so fixtures read like real node files) ----

inline std::string FilesAddr(const std::string &Target) { return Target.empty() ? std::string("FILES") : "FILES/" + Target; }

// One content layer: Form = "zip" | "dir" | "file" | "delta".
inline nlohmann::ordered_json Content(const char *Form, const std::string &Path, const std::string &Target = {})
{
    std::string Key = Form;
    for (char &C : Key) C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
    nlohmann::ordered_json Layer{ {Key, Path} };
    if (!Target.empty()) Layer["TARGET"] = FilesAddr(Target);
    return { {"LAYERS", nlohmann::ordered_json::array({ std::move(Layer) })} };
}

// A delta over the layers whose SOURCE CIDs it names, in order (BASE_SOURCES); Cid its own SOURCE, when a delta names it.
inline nlohmann::ordered_json DeltaOver(const std::string &Path, std::vector<std::string> Bases, const std::string &Target = {},
                                        const std::string &Cid = {})
{
    nlohmann::ordered_json L = Content("delta", Path, Target);
    L["LAYERS"][0]["BASE_SOURCES"] = std::move(Bases);
    if (!Cid.empty()) L["LAYERS"][0]["SOURCE"] = Cid;
    return L;
}

// Content whose bytes come from a backend rather than the bundle dir (missing locally ⇒ fetchable, not broken).
inline nlohmann::ordered_json ContentCid(const char *Form, const std::string &Path, const std::string &Cid,
                                         const std::string &Target = {})
{
    nlohmann::ordered_json L = Content(Form, Path, Target);
    L["LAYERS"][0]["SOURCE"] = Cid;
    return L;
}

// A game entry ("Play"; no GUEST ⇒ it runs content).
inline nlohmann::ordered_json Exec(const std::string &Host, const std::string &Path, const std::string &Label = "Play")
{
    nlohmann::ordered_json E{ {"HOST", Host} };
    if (!Path.empty()) E["EXE"] = Path;
    return { {"LAYERS", nlohmann::ordered_json::array({ { {"EXEC", nlohmann::ordered_json{ {Label, E} }} } })} };
}

// On the shelf: a variant named `Name` (it also needs a tiled entry in its fold to present a tile).
inline nlohmann::ordered_json Variant(const std::string &Name) { return { {"VARIANT", Name} }; }

// A runner: an entry with GUEST platforms — it provides an environment instead of running content.
inline nlohmann::ordered_json Runner(const std::string &Host, const std::vector<std::string> &Guest,
                                     const std::string &Path)
{
    nlohmann::ordered_json G = nlohmann::ordered_json::array();
    for (const std::string &X : Guest) G.push_back(X);
    nlohmann::ordered_json E{ {"HOST", Host}, {"GUEST", G}, {"EXE", Path} };
    return { {"LAYERS", nlohmann::ordered_json::array({ { {"EXEC", nlohmann::ordered_json{ {"run", E} }} } })} };
}

// A tile rides the "Play" entry: in a Merge with Exec(...), the same entry; alone, an entry that only has a tile.
inline nlohmann::ordered_json Tile(const std::string &Uid, const std::string &Title = {},
                                   const nlohmann::ordered_json &Meta = nlohmann::ordered_json::object(),
                                   const std::string &ParentUid = {})
{
    nlohmann::ordered_json T{ {"UID", Uid} };
    if (!ParentUid.empty()) T["PARENTUID"] = ParentUid;
    if (!Title.empty()) T["TITLE"] = Title;
    if (Meta.is_object() && !Meta.empty()) T["META"] = Meta;
    nlohmann::ordered_json E{ {"TILE", T} };
    return { {"LAYERS", nlohmann::ordered_json::array({ { {"EXEC", nlohmann::ordered_json{ {"Play", E} }} } })} };
}

// The tile of a tile node, taken onto this node's Play entry (EXEC/Play/TILE, placed over the entry it writes): how a
// row keeps the card's tile kept in one node of its package. As a LATER section than the entry.
inline nlohmann::ordered_json TakeTile(const std::string &TileNode, const std::string &Label = "Play")
{
    nlohmann::ordered_json T = nlohmann::ordered_json::array();
    if (Label == "Play") T.push_back("EXEC/Play/TILE");
    else T.push_back(nlohmann::ordered_json::array({"EXEC/Play/TILE", "EXEC/" + Label + "/TILE"}));
    return { {"LAYERS", nlohmann::ordered_json::array({ { {"NODE", TileNode}, {"TAKE", T} } })} };
}

// In a Merge: the Play entry is its card's recommended row.
inline nlohmann::ordered_json Recommended(const std::string &Label = "Play")
{
    return { {"LAYERS", nlohmann::ordered_json::array({ { {"EXEC", nlohmann::ordered_json{ {Label, { {"RECOMMENDED", true} }} }} } })} };
}

// A section holding arbitrary layers.
inline nlohmann::ordered_json Layers(std::initializer_list<nlohmann::ordered_json> Ls)
{
    nlohmann::ordered_json A = nlohmann::ordered_json::array();
    for (const auto &L : Ls) A.push_back(L);
    return { {"LAYERS", A} };
}

// Wire a hand-built Node (bypassing ParseNode) to CONTAIN the given refs — the refs and the NODE layers, as
// ParseNode would see them.
inline void Wire(struct Node &N, const std::vector<std::string> &Refs)
{
    N.Refs = Refs;
    nlohmann::ordered_json L = nlohmann::ordered_json::array();
    for (const std::string &R : Refs) L.push_back({ {"NODE", R} });
    nlohmann::ordered_json J = N.Json->is_object() ? *N.Json : nlohmann::ordered_json::object();
    if (J.contains("LAYERS"))
        for (const auto &X : J["LAYERS"]) if (!X.contains("NODE")) L.push_back(X);
    J["LAYERS"] = std::move(L);
    N.Json = std::move(J);   // a node's JSON is shared and read-only: replaced whole
}

// Write fixture JSON to Path. A node file holds ONE node (the file is the node's block), so a Chain's array is
// written one node per file: the head at Path, each earlier link beside it as "<LABEL>.json". Anything that is
// not an array of nodes is written as-is.
template <class Json>
inline void WriteNodes(const std::string &Path, const Json &J)
{
    const auto Put = [](const std::filesystem::path &P, const Json &X) { std::ofstream F(P); F << X.dump(2); };
    const bool Nodes = J.is_array() && !J.empty() && J.back().is_object() && J.back().contains("LAYERS");
    if (!Nodes) { Put(Path, J); return; }
    const std::filesystem::path Dir = std::filesystem::path(Path).parent_path();
    for (size_t I = 0; I + 1 < J.size(); ++I)
        Put(Dir / (J[I].value("LABEL", std::string("node") + std::to_string(I)) + ".json"), J[I]);
    Put(Path, J.back());
}

} // namespace NodeFixture

#endif // NODEFIXTURE_H
