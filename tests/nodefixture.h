#ifndef NODEFIXTURE_H
#define NODEFIXTURE_H

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Test scaffolding for the flat node schema.
//
// A node is ONE layer of one TYPE, and order between nodes is carried by PARENTS edges — so a fixture that
// used to be "a node with these three layers" is now a three-node chain. `Chain` builds exactly that, and
// returns a JSON ARRAY, which is also the on-disk form a single file may hold: the same value can be written
// with writeJson() or iterated to feed ParseNode directly.
//
// The LAST layer owns the bare `Id`: a launchable is the terminal node of its chain (it pulls its content as
// parents), and tests address it by its natural name. Earlier links get "<Id>__l<N>" — they are private
// plumbing, and nothing should reference them.
//
// Node-level selection fields (TOGGLE/EXCLUDE) go on the TAIL, which is where they belong: ResolveNodeOrder
// only descends into nodes it keeps, so switching the tail off drops the chain's private ancestors with it.
// ---------------------------------------------------------------------------

namespace NodeFixture
{

inline nlohmann::ordered_json Chain(const std::string &Id,
                                    const std::vector<nlohmann::ordered_json> &Layers,
                                    const std::vector<std::string> &Parents = {},
                                    const nlohmann::ordered_json &TailFields = nlohmann::ordered_json::object())
{
    nlohmann::ordered_json Out = nlohmann::ordered_json::array();
    const size_t Count = Layers.empty() ? 1 : Layers.size();
    for (size_t I = 0; I < Count; ++I)
    {
        // No layers at all ⇒ pure composition: the node exists only to carry PARENTS.
        nlohmann::ordered_json Nd = Layers.empty() ? nlohmann::ordered_json{{"TYPE", "Group"}} : Layers[I];
        const bool Tail = (I + 1 == Count);
        const std::string Handle = Tail ? Id : (Id + "__l" + std::to_string(I));
        nlohmann::ordered_json P = nlohmann::ordered_json::array();
        if (I == 0) for (const std::string &X : Parents) P.push_back(X);
        else        P.push_back(Id + "__l" + std::to_string(I - 1));
        // Model C: identity/wiring is the stored "CID" HANDLE (GatherWorkingTree keys by it); LABEL is purely
        // cosmetic. A fixture must set the handle so PARENTS refs resolve at freeze — the readable Id doubles as the
        // handle here (a node's stored CID is a working-tree handle, stripped + re-minted at publish). LABEL matches it
        // so the on-disk filename and the picker read naturally.
        Nd["CID"] = Handle;
        Nd["LABEL"] = Handle;
        Nd["PARENTS"] = std::move(P);
        if (Tail && TailFields.is_object())
            for (const auto &[K, V] : TailFields.items()) Nd[K] = V;
        Out.push_back(std::move(Nd));
    }
    return Out;
}

// ---- layer payload shorthands (the flat vocabulary, so fixtures read like real node files) ----

// A VFSLayer node holds a LAYERS list (batched); this builds the one-layer form used across the tests.
inline nlohmann::ordered_json Content(const char *Form, const std::string &Path, const std::string &Target = {})
{
    nlohmann::ordered_json Layer{{"FORM", Form}, {"PATH", Path}};
    if (!Target.empty()) Layer["TARGET"] = Target;
    return {{"TYPE", "VFSLayer"}, {"LAYERS", nlohmann::ordered_json::array({std::move(Layer)})}};
}

// Content whose bytes come from a backend rather than the bundle dir (missing locally ⇒ fetchable, not broken).
inline nlohmann::ordered_json ContentCid(const char *Form, const std::string &Path, const std::string &Cid,
                                         const std::string &Target = {})
{
    nlohmann::ordered_json L = Content(Form, Path, Target);
    L["LAYERS"][0]["SOURCE"] = nlohmann::ordered_json{{"TYPE", "ipfs"}, {"CID", Cid}};
    return L;
}

inline nlohmann::ordered_json Exec(const std::string &Host, const std::string &Path)
{
    return {{"TYPE", "DeclareExec"}, {"HOST", Host}, {"PATH", Path}};
}

// A runner is the same declaration with GUEST platforms — it provides an environment instead of being the
// terminal link. No GUEST ⇒ launchable.
inline nlohmann::ordered_json Runner(const std::string &Host, const std::vector<std::string> &Guest,
                                     const std::string &Path)
{
    nlohmann::ordered_json G = nlohmann::ordered_json::array();
    for (const std::string &X : Guest) G.push_back(X);
    return {{"TYPE", "DeclareExec"}, {"HOST", Host}, {"GUEST", G}, {"PATH", Path}};
}

inline nlohmann::ordered_json Tile(const std::string &Uid, const std::string &Title = {},
                                   const nlohmann::ordered_json &Meta = nlohmann::ordered_json::object())
{
    nlohmann::ordered_json L{{"TYPE", "DeclareLibraryItem"}, {"UID", Uid}};
    if (!Title.empty()) L["TITLE"] = Title;
    if (Meta.is_object() && !Meta.empty()) L["META"] = Meta;
    return L;
}

} // namespace NodeFixture

#endif // NODEFIXTURE_H
