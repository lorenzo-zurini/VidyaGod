#ifndef NODEFIXTURE_H
#define NODEFIXTURE_H

#include <nlohmann/json.hpp>
#include "manifestmodel.h"   // Node for Wire()

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Test scaffolding for generation-6 nodes: {CID, LABEL, VARIANT?, RECOMMENDED?, LAYERS}.
//
// A SECTION is a partial node — {"LAYERS": [...]} and/or node fields ({"VARIANT": …}). `Chain` builds a chain of
// nodes from sections — one node per section, each CONTAINING the previous through a leading NODE layer (the
// parent is a layer) — and returns a JSON ARRAY of node objects, ready to write one per file or to feed ParseNode.
//
// The LAST node owns the bare `Id`: a launchable is the head of its chain (it contains its content), and tests
// address it by its natural name. Earlier links get "<Id>__l<N>". `Over` refs are contained by the FIRST node.
// Node fields for the head (VARIANT, RECOMMENDED) go in TailFields or in the last section.
// ---------------------------------------------------------------------------

namespace NodeFixture
{

// Merge sections into ONE node object: LAYERS concatenate in order, other fields are set.
inline nlohmann::ordered_json Merge(std::initializer_list<nlohmann::ordered_json> Sections)
{
    nlohmann::ordered_json Out = nlohmann::ordered_json::object();
    for (const auto &S : Sections)
        for (const auto &[K, V] : S.items())
        {
            if (K == "LAYERS")
            {
                if (!Out.contains("LAYERS")) Out["LAYERS"] = nlohmann::ordered_json::array();
                for (const auto &L : V) Out["LAYERS"].push_back(L);
            }
            else Out[K] = V;
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
    nlohmann::ordered_json E{ {"LABEL", Label}, {"HOST", Host} };
    if (!Path.empty()) E["EXE"] = Path;
    return { {"LAYERS", nlohmann::ordered_json::array({ { {"EXEC", nlohmann::ordered_json::array({ E })} } })} };
}

// On the shelf: a variant named `Name` (it also needs a tiled entry in its fold to present a tile).
inline nlohmann::ordered_json Variant(const std::string &Name) { return { {"VARIANT", Name} }; }

// A runner: an entry with GUEST platforms — it provides an environment instead of running content.
inline nlohmann::ordered_json Runner(const std::string &Host, const std::vector<std::string> &Guest,
                                     const std::string &Path)
{
    nlohmann::ordered_json G = nlohmann::ordered_json::array();
    for (const std::string &X : Guest) G.push_back(X);
    nlohmann::ordered_json E{ {"LABEL", "run"}, {"HOST", Host}, {"GUEST", G}, {"EXE", Path} };
    return { {"LAYERS", nlohmann::ordered_json::array({ { {"EXEC", nlohmann::ordered_json::array({ E })} } })} };
}

// A tile rides the "Play" entry: a partial entry carrying TILE, folded with the entry that runs.
inline nlohmann::ordered_json Tile(const std::string &Uid, const std::string &Title = {},
                                   const nlohmann::ordered_json &Meta = nlohmann::ordered_json::object(),
                                   const std::string &ParentUid = {})
{
    nlohmann::ordered_json T{ {"UID", Uid} };
    if (!ParentUid.empty()) T["PARENTUID"] = ParentUid;
    if (!Title.empty()) T["TITLE"] = Title;
    if (Meta.is_object() && !Meta.empty()) T["META"] = Meta;
    nlohmann::ordered_json E{ {"LABEL", "Play"}, {"TILE", T} };
    return { {"LAYERS", nlohmann::ordered_json::array({ { {"EXEC", nlohmann::ordered_json::array({ E })} } })} };
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
    if (N.Json.is_object() && N.Json.contains("LAYERS"))
        for (const auto &X : N.Json["LAYERS"]) if (!X.contains("NODE")) L.push_back(X);
    if (!N.Json.is_object()) N.Json = nlohmann::ordered_json::object();
    N.Json["LAYERS"] = std::move(L);
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
