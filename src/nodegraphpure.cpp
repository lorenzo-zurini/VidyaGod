#include "nodegraph.h"
#include "commonutils.h"   // LogWarn — surface duplicate-handle shadowing (never silent)

#include <deque>
#include <fstream>
#include <functional>
#include <set>

// nodegraphpure.cpp — the gigagraph's PURE transforms: no IPFS, no Qt, no disk. Split out from nodegraph.cpp so they
// can be unit-tested with teeth (tests/test_nodegraph.cpp) without linking the embedded node. See nodegraph.h.

namespace NodeGraph {

void NormalizeLinks(nlohmann::ordered_json &J)
{
    if (J.is_object())
    {
        // dag-json link: an object whose SOLE key is "/" with a string value is a CID link → collapse to the string.
        // (A byte value {"/":{"bytes":...}} has a non-string "/" and is left untouched.)
        if (J.size() == 1)
        {
            auto It = J.find("/");
            if (It != J.end() && It->is_string())
            {
                J = It->get<std::string>();
                return;
            }
        }
        for (auto &El : J.items()) NormalizeLinks(El.value());
    }
    else if (J.is_array())
    {
        for (auto &El : J) NormalizeLinks(El);
    }
}

// Wrap a non-empty CID string as a dag-json link {"/":cid}. No-op for anything else (already a link, empty, non-string).
static void LinkifyCidString(nlohmann::ordered_json &Field)
{
    if (Field.is_string() && !Field.get<std::string>().empty())
        Field = nlohmann::ordered_json{{"/", Field.get<std::string>()}};
}

// Linkify every SOURCE.CID reachable in J (Content nodes' SOURCE, and nested COVER.SOURCE), recursively.
static void LinkifySources(nlohmann::ordered_json &J)
{
    if (J.is_object())
    {
        auto S = J.find("SOURCE");
        if (S != J.end() && S->is_object())
        {
            auto C = S->find("CID");
            if (C != S->end()) LinkifyCidString(*C);
        }
        for (auto &El : J.items()) LinkifySources(El.value());
    }
    else if (J.is_array())
    {
        for (auto &El : J) LinkifySources(El);
    }
}

nlohmann::ordered_json FreezeNodeJson(nlohmann::ordered_json Raw,
                                      const std::map<std::string, std::string> &HandleToCid)
{
    Raw.erase("POS");   // blueprint-canvas coordinates — the one non-semantic field; never part of identity

    // Resolve intra-tree handles → frozen CIDs. A ref absent from HandleToCid is an already-frozen EXTERNAL dep
    // (a shared library referenced by CID) — pass it through unchanged.
    const auto Resolve = [&](const std::string &Ref) -> std::string {
        const auto It = HandleToCid.find(Ref);
        return It != HandleToCid.end() ? It->second : Ref;
    };
    if (Raw.contains("PARENTS") && Raw["PARENTS"].is_array())
        for (auto &P : Raw["PARENTS"]) if (P.is_string()) P = Resolve(P.get<std::string>());
    if (Raw.contains("LIBRARYITEM") && Raw["LIBRARYITEM"].is_string())
        Raw["LIBRARYITEM"] = Resolve(Raw["LIBRARYITEM"].get<std::string>());

    // Linkify all CID-bearing fields into dag-json links (resolution above ran while they were still plain strings).
    if (Raw.contains("PARENTS") && Raw["PARENTS"].is_array())
        for (auto &P : Raw["PARENTS"]) LinkifyCidString(P);
    if (Raw.contains("LIBRARYITEM")) LinkifyCidString(Raw["LIBRARYITEM"]);
    LinkifySources(Raw);
    return Raw;
}

// The intra-tree node links a node depends on (must be frozen first): PARENTS + LIBRARYITEM that name a node in the
// working tree. Refs outside the tree are external (already frozen) and impose no order.
static std::vector<std::string> IntraTreeDeps(const nlohmann::ordered_json &Node,
                                              const std::map<std::string, nlohmann::ordered_json> &Tree)
{
    std::vector<std::string> Deps;
    if (Node.contains("PARENTS") && Node["PARENTS"].is_array())
        for (const auto &P : Node["PARENTS"])
            if (P.is_string() && Tree.count(P.get<std::string>())) Deps.push_back(P.get<std::string>());
    if (Node.contains("LIBRARYITEM") && Node["LIBRARYITEM"].is_string()
        && Tree.count(Node["LIBRARYITEM"].get<std::string>()))
        Deps.push_back(Node["LIBRARYITEM"].get<std::string>());
    return Deps;
}

void GatherWorkingTree(const std::filesystem::path &Root,
                       std::map<std::string, nlohmann::ordered_json> &Tree,
                       std::map<std::string, std::filesystem::path> &Dirs,
                       bool SkipReserved)
{
    namespace fs = std::filesystem;
    std::error_code Ec;
    if (!fs::is_directory(Root, Ec)) return;
    // error_code iteration with explicit increment: the range-for's operator++ THROWS (a permission-denied subdir
    // mid-walk would terminate the GUI thread). skip_permission_denied + increment(Ec) walks defensively.
    fs::recursive_directory_iterator It(Root, fs::directory_options::skip_permission_denied, Ec), End;
    for (; !Ec && It != End; It.increment(Ec))
    {
        const auto &E = *It;
        // Reserved received-stub dirs ("_friend_*"): never descend when SkipReserved (PublishLibrary must not mint a
        // friend's stub, and a hostile stub NODE_ID must not shadow our handle by winning first-seen).
        if (SkipReserved && E.is_directory(Ec) && E.path().filename().string().rfind("_friend_", 0) == 0)
        { It.disable_recursion_pending(); continue; }
        if (!E.is_regular_file(Ec) || E.path().extension() != ".json") continue;
        std::ifstream In(E.path(), std::ios::binary);
        nlohmann::ordered_json J;
        try { In >> J; }
        catch (const std::exception &) { continue; }   // skip unparseable — a scan must never throw
        NormalizeLinks(J);   // a RAW dag-json node block (a received share, landed verbatim by the fetch queue) uses
                             // {"/":cid} link objects — normalize to plain CID strings so it reads like any tree node
                             // (idempotent for ordinary handle-linked nodes; re-freezes to the identical CID)
        nlohmann::ordered_json Single;
        if (!J.is_array()) Single = nlohmann::ordered_json::array({J});
        const nlohmann::ordered_json &Nodes = J.is_array() ? J : Single;
        for (const auto &N : Nodes)
            if (N.is_object() && N.contains("NODE_ID") && N["NODE_ID"].is_string()
                && !N["NODE_ID"].get<std::string>().empty())
            {
                const std::string Id = N["NODE_ID"].get<std::string>();
                // Keep FIRST-seen and warn on a duplicate handle — never silently shadow (a hydrated foreign package
                // reusing a local handle must not overwrite the local node's identity/content without a trace).
                if (Tree.find(Id) != Tree.end())
                {
                    LogWarn("NodeGraph::GatherWorkingTree", "duplicate NODE_ID '" + Id + "' (" + E.path().string()
                            + ") — keeping first-seen");
                    continue;
                }
                Tree[Id] = N;
                Dirs[Id] = E.path().parent_path();
            }
    }
}

void LiftLibraryItemEdge(std::map<std::string, nlohmann::ordered_json> &Tree)
{
    // Which handles are tiles (DeclareLibraryItem nodes present in this tree).
    std::set<std::string> Tiles;
    for (const auto &[Handle, Doc] : Tree)
        if (Doc.value("TYPE", std::string()) == "DeclareLibraryItem") Tiles.insert(Handle);
    if (Tiles.empty()) return;

    const auto ParentsOf = [](const nlohmann::ordered_json &Doc) {
        std::vector<std::string> Ps;
        if (Doc.contains("PARENTS") && Doc["PARENTS"].is_array())
            for (const auto &P : Doc["PARENTS"]) if (P.is_string() && !P.get<std::string>().empty())
                Ps.push_back(P.get<std::string>());
        return Ps;
    };

    // (1) Each DeclareExec's tile = its NEAREST tile ANCESTOR via PARENTS (a tile is usually a transitive ancestor,
    // not a direct parent). BFS from the exec's direct parents outward — first tile reached is the nearest. Set it as
    // the LIBRARYITEM edge. (This mirrors LinkGames' "nearest presentable ancestor", resolved once here at freeze.)
    for (auto &[Handle, Doc] : Tree)
    {
        (void)Handle;
        if (Doc.value("TYPE", std::string()) != "DeclareExec") continue;
        if (Doc.contains("LIBRARYITEM") && Doc["LIBRARYITEM"].is_string()
            && !Doc["LIBRARYITEM"].get<std::string>().empty())
            continue;   // already lifted (idempotent)

        std::deque<std::string> Q;
        for (const auto &P : ParentsOf(Doc)) Q.push_back(P);
        std::set<std::string> Seen;
        std::string Found;
        while (!Q.empty())
        {
            const std::string P = Q.front();
            Q.pop_front();
            if (!Seen.insert(P).second) continue;
            if (Tiles.count(P)) { Found = P; break; }   // nearest tile (BFS order)
            const auto It = Tree.find(P);
            if (It != Tree.end()) for (const auto &Pp : ParentsOf(It->second)) Q.push_back(Pp);
        }
        if (!Found.empty()) Doc["LIBRARYITEM"] = Found;
    }

    // (2) Tiles are pure metadata (no layers) — strip them from EVERY node's PARENTS so the composition graph carries
    // no metadata edges. The tile now lives ONLY on the LIBRARYITEM edge. Safe for CFS: a layer-less node contributes
    // nothing to a resolved layer stack, so removing it never changes launch behavior.
    for (auto &[Handle, Doc] : Tree)
    {
        (void)Handle;
        if (!Doc.contains("PARENTS") || !Doc["PARENTS"].is_array()) continue;
        nlohmann::ordered_json Kept = nlohmann::ordered_json::array();
        for (const auto &P : Doc["PARENTS"])
            if (!(P.is_string() && Tiles.count(P.get<std::string>()))) Kept.push_back(P);
        Doc["PARENTS"] = std::move(Kept);
    }
}

bool TopoOrderForMint(const std::map<std::string, nlohmann::ordered_json> &WorkingTree,
                      std::vector<std::string> &Order, std::string *Error)
{
    (void)Error;   // no longer fails: a cycle is BROKEN, not fatal (see below)
    Order.clear();
    std::set<std::string> Visited, OnStack;
    int Broken = 0;

    // A content-addressed cycle (A→B→A) is unfreezable — each node's CID needs the other's first — so it must NOT
    // abort the whole order (that empties the entire library on one self-referential typo). Instead BREAK the back
    // edge: the cyclic nodes still enter the order but will fail DagCid at freeze (their cycle ref stays a handle →
    // invalid link) and be SKIPPED per-node, while every acyclic package orders and freezes normally.
    std::function<void(const std::string &)> Visit = [&](const std::string &Handle) {
        if (Visited.count(Handle)) return;
        if (OnStack.count(Handle)) { ++Broken; return; }   // back edge → break the cycle, don't recurse
        OnStack.insert(Handle);
        for (const std::string &Dep : IntraTreeDeps(WorkingTree.at(Handle), WorkingTree))
            Visit(Dep);
        OnStack.erase(Handle);
        Visited.insert(Handle);
        Order.push_back(Handle);   // post-order ⇒ deps before dependents
    };

    for (const auto &[Handle, Doc] : WorkingTree) { (void)Doc; Visit(Handle); }
    if (Broken)
        LogWarn("NodeGraph::TopoOrderForMint", std::to_string(Broken)
                + " cycle edge(s) broken — cyclic node(s) will be dropped at freeze; the rest is intact");
    return true;
}

} // namespace NodeGraph
