#ifndef NODEFIXTURE_H
#define NODEFIXTURE_H

#include <nlohmann/json.hpp>
#include "manifestmodel.h"   // Node / OverReq for Wire()

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Test scaffolding for the one-edge graph.
//
// A node is facets + any subset of payload sections + the one edge OVER. `Chain` builds a chain of nodes from
// a list of section objects — one node per object, each OVER the previous — and returns a JSON ARRAY, which
// is also the on-disk form a single file may hold: the same value can be written with writeJson() or iterated
// to feed ParseNode directly.
//
// The LAST object owns the bare `Id`: a launchable is the terminal node of its chain (it is OVER its content),
// and tests address it by its natural name. Earlier links get "<Id>__l<N>" — they are private plumbing, and
// nothing should reference them.
//
// Node-level selection fields (TOGGLE, a NOT) go on the TAIL, which is where they belong: ResolveNodeOrder
// only descends into nodes it keeps, so switching the tail off drops the chain's private ancestors with it.
// ---------------------------------------------------------------------------

namespace NodeFixture
{

inline nlohmann::ordered_json Chain(const std::string &Id,
                                    const std::vector<nlohmann::ordered_json> &Sections,
                                    const std::vector<std::string> &Over = {},
                                    const nlohmann::ordered_json &TailFields = nlohmann::ordered_json::object())
{
    nlohmann::ordered_json Out = nlohmann::ordered_json::array();
    const size_t Count = Sections.empty() ? 1 : Sections.size();
    for (size_t I = 0; I < Count; ++I)
    {
        // No sections at all ⇒ a plain node: it exists only to be OVER other nodes.
        nlohmann::ordered_json Nd = Sections.empty() ? nlohmann::ordered_json::object() : Sections[I];
        const bool Tail = (I + 1 == Count);
        const std::string Handle = Tail ? Id : (Id + "__l" + std::to_string(I));
        nlohmann::ordered_json O = nlohmann::ordered_json::array();
        if (I == 0) for (const std::string &X : Over) O.push_back(X);
        else        O.push_back(Id + "__l" + std::to_string(I - 1));
        // Model C: identity/wiring is the stored "CID" HANDLE (GatherWorkingTree keys by it); LABEL is purely
        // cosmetic. A fixture must set the handle so OVER refs resolve at freeze — the readable Id doubles as the
        // handle here (a node's stored CID is a working-tree handle, stripped + re-minted at publish). LABEL matches
        // it so the on-disk filename and the picker read naturally.
        Nd["CID"] = Handle;
        Nd["LABEL"] = Handle;
        if (!O.empty()) Nd["OVER"] = std::move(O);
        if (Tail && TailFields.is_object())
            for (const auto &[K, V] : TailFields.items()) Nd[K] = V;
        Out.push_back(std::move(Nd));
    }
    return Out;
}

// ---- section shorthands (the format's vocabulary, so fixtures read like real node files) ----

// A LAYERS section holding one VFS layer.
inline nlohmann::ordered_json Content(const char *Form, const std::string &Path, const std::string &Target = {})
{
    nlohmann::ordered_json Layer{{"FORM", Form}, {"PATH", Path}};
    if (!Target.empty()) Layer["TARGET"] = Target;
    return {{"LAYERS", nlohmann::ordered_json::array({std::move(Layer)})}};
}

// Content whose bytes come from a backend rather than the bundle dir (missing locally ⇒ fetchable, not broken).
inline nlohmann::ordered_json ContentCid(const char *Form, const std::string &Path, const std::string &Cid,
                                         const std::string &Target = {})
{
    nlohmann::ordered_json L = Content(Form, Path, Target);
    L["LAYERS"][0]["SOURCE"] = nlohmann::ordered_json{{"TYPE", "ipfs"}, {"CID", Cid}};
    return L;
}

// An ENTRYPOINTS section holding one launchable entry (no GUEST ⇒ launchable).
inline nlohmann::ordered_json Exec(const std::string &Host, const std::string &Path)
{
    return {{"ENTRYPOINTS", nlohmann::ordered_json::array({ nlohmann::ordered_json{{"HOST", Host}, {"PATH", Path}} })}};
}

// On the shelf: a variant named `Name` (a node also needs effective entries to be a variant).
inline nlohmann::ordered_json Variant(const std::string &Name) { return {{"VARIANT", Name}}; }

// A runner is the same declaration with GUEST platforms — it provides an environment instead of being the
// terminal link.
inline nlohmann::ordered_json Runner(const std::string &Host, const std::vector<std::string> &Guest,
                                     const std::string &Path)
{
    nlohmann::ordered_json G = nlohmann::ordered_json::array();
    for (const std::string &X : Guest) G.push_back(X);
    return {{"ENTRYPOINTS", nlohmann::ordered_json::array({ nlohmann::ordered_json{{"HOST", Host}, {"GUEST", G}, {"PATH", Path}} })}};
}

// A TILE section: the identity of a title. On a launchable it IS its tile; on a plain node it is inherited by
// everything OVER it.
inline nlohmann::ordered_json Tile(const std::string &Uid, const std::string &Title = {},
                                   const nlohmann::ordered_json &Meta = nlohmann::ordered_json::object())
{
    nlohmann::ordered_json T{{"UID", Uid}};
    if (!Title.empty()) T["TITLE"] = Title;
    if (Meta.is_object() && !Meta.empty()) T["META"] = Meta;
    return {{"TILE", T}};
}

// Wire a hand-built Node (bypassing ParseNode) OVER the given refs — sets both the CNF and the flattened refs, as
// ParseNode would.
inline void Wire(struct Node &N, const std::vector<std::string> &Refs)
{
    N.Over.clear(); N.Parents = Refs; N.Composes = Refs;
    for (const std::string &R : Refs) { OverReq Q; Q.Any = {R}; N.Over.push_back(Q); }
}

// Merge sections into ONE node object (a pluripotent node: e.g. Content(...) + Exec(...) + Tile(...)).
inline nlohmann::ordered_json Merge(std::initializer_list<nlohmann::ordered_json> Sections)
{
    nlohmann::ordered_json Out = nlohmann::ordered_json::object();
    for (const auto &S : Sections) for (const auto &[K, V] : S.items()) Out[K] = V;
    return Out;
}

} // namespace NodeFixture

#endif // NODEFIXTURE_H
