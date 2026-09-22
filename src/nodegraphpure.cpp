#include "nodegraph.h"
#include "commonutils.h"   // LogWarn/LogErr — surface duplicate-handle conflicts (never silent)

#include <deque>
#include <fstream>
#include <functional>
#include <set>

// nodegraphpure.cpp — the gigagraph's PURE transforms: no IPFS, no Qt, no disk. Split out from nodegraph.cpp so they
// can be unit-tested with teeth (tests/test_nodegraph.cpp) without linking the embedded node. See nodegraph.h.

namespace NodeGraph {

bool JsonDepthWithinLimit(const std::string &S, int MaxDepth)
{
    int Depth = 0;
    bool InStr = false, Esc = false;
    for (const char C : S)
    {
        if (InStr) { if (Esc) Esc = false; else if (C == '\\') Esc = true; else if (C == '"') InStr = false; continue; }
        if (C == '"') InStr = true;
        else if (C == '{' || C == '[') { if (++Depth > MaxDepth) return false; }
        else if (C == '}' || C == ']') --Depth;
    }
    return true;
}

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
    Raw.erase("POS");   // blueprint-canvas coordinates — non-semantic; never part of identity
    Raw.erase("CID");   // the node's OWN last-minted CID: a working-tree handle only (the block is ADDRESSED by its
                        // CID, so storing it inside would be circular). Stripped like POS → identity stays purely
                        // the hash of {TYPE, refs→CIDs, LABEL, payload}; the stored CID is never shipped.

    // Remap references (old handle CID → freshly-minted CID) as the cascade climbs. A ref absent from HandleToCid is
    // an already-frozen EXTERNAL dep (a shared library / cross-bundle node referenced by CID) — pass it through.
    // ONE remapper (ManifestModel::RemapOverRefs) walks plain entries, any-of members and NOT refs alike, shared
    // with the CID write-back so the two can never disagree on where refs live.
    ManifestModel::RemapOverRefs(Raw, [&](const std::string &Ref) {
        const auto It = HandleToCid.find(Ref);
        return It != HandleToCid.end() ? It->second : Ref;
    });

    // Linkify the POSITIVE refs into dag-json links (resolution above ran while they were still plain strings). A
    // NOT ref stays a PLAIN STRING on purpose: it is part of the node's identity (the hash covers it) but NOT part
    // of its DAG closure — a recursive pin/fetch of this node must never pull in the node it excludes.
    if (Raw.contains("OVER") && Raw["OVER"].is_array())
        for (auto &E : Raw["OVER"])
        {
            if (E.is_string())     LinkifyCidString(E);
            else if (E.is_array()) for (auto &M : E) LinkifyCidString(M);
        }
    LinkifySources(Raw);
    return Raw;
}

// The intra-tree node refs a node depends on (must be frozen first): every OVER ref — plain, any-of member AND
// NOT (a NOT names a CID too, so the excluded node's fresh CID must exist before this node freezes) — that names
// a node in the working tree. Refs outside the tree are external (already frozen) and impose no order.
static std::vector<std::string> IntraTreeDeps(const nlohmann::ordered_json &Node,
                                              const std::map<std::string, nlohmann::ordered_json> &Tree)
{
    std::vector<std::string> Deps;
    std::vector<std::string> Nots;
    for (const std::string &P : ManifestModel::OverRefs(Node, &Nots)) if (Tree.count(P)) Deps.push_back(P);
    for (const std::string &P : Nots) if (Tree.count(P)) Deps.push_back(P);
    return Deps;
}

bool ReadTreeJsonBounded(const std::filesystem::path &File, nlohmann::ordered_json &J)
{
    std::error_code Ec;
    const auto Sz = std::filesystem::file_size(File, Ec);
    if (Ec || Sz > (8u << 20)) return false;
    std::ifstream In(File, std::ios::binary);
    if (!In) return false;
    std::string Bytes((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
    if (!JsonDepthWithinLimit(Bytes, 64)) { LogWarn("NodeGraph::ReadTreeJsonBounded", "skipping too-deep JSON " + File.string()); return false; }
    J = nlohmann::ordered_json::parse(Bytes, nullptr, false);
    return !J.is_discarded();
}

void GatherWorkingTree(const std::filesystem::path &Root,
                       std::map<std::string, nlohmann::ordered_json> &Tree,
                       std::map<std::string, std::filesystem::path> &Dirs,
                       bool SkipReserved, bool TrustStoredCid)
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
        // UNTRUSTED bytes can now live in the tree (a received share's block, landed verbatim by the fetch queue), so
        // the scan gets the same guards every fetched block gets — size cap + depth pre-scan BEFORE the recursive
        // parse/normalize, or a hostile deep block becomes a stack overflow that crashes EVERY startup scan.
        Ec.clear();
        nlohmann::ordered_json J;
        if (!ReadTreeJsonBounded(E.path(), J)) continue;
        NormalizeLinks(J);   // a RAW dag-json node block (a received share, landed verbatim by the fetch queue) uses
                             // {"/":cid} link objects — normalize to plain CID strings so it reads like any tree node
                             // (idempotent for ordinary handle-linked nodes; re-freezes to the identical CID)
        nlohmann::ordered_json Single;
        if (!J.is_array()) Single = nlohmann::ordered_json::array({J});
        const nlohmann::ordered_json &Nodes = J.is_array() ? J : Single;
        int NIdx = 0;
        for (const auto &N : Nodes)
        {
            ++NIdx;
            // A node is any object carrying a node field (ManifestModel::IsNodeObject). Identity is the CID, and OVER
            // refs name other nodes BY CID. The authoring handle is the node's OWN stored "CID" — the CID it last
            // minted to (stamped on a received block at fetch time; computed at create/publish for a local one). LABEL
            // is PURELY COSMETIC: an optional pretty name that travels in the block and is NEVER a key.
            if (N.is_object() && N.contains("TYPE"))
                LogWarn("NodeGraph::GatherWorkingTree", "legacy TYPE node ignored (run tools/migrate_one_edge.py): " + E.path().string());
            if (!ManifestModel::IsNodeObject(N)) continue;
            // From an UNTRUSTED root (received CATALOG stubs), the stored "CID" is attacker-controlled — ignore it so
            // the node is synthetic-keyed (browse-only, never a resolvable handle). An honest stub has none anyway.
            std::string Handle = (TrustStoredCid && N.contains("CID") && N["CID"].is_string()) ? N["CID"].get<std::string>() : std::string();
            // A real CID handle is base32/base58 — no control bytes. Reject a handle carrying any (< 0x20) as
            // handle-less (→ synthetic key): the synthetic key is control-byte-prefixed, so a hostile working-tree
            // "CID":"<path>#N" could otherwise forge exactly that key and evict a local node.
            for (unsigned char c : Handle) if (c < 0x20) { Handle.clear(); break; }
            // A node with no stored CID (a never-minted local draft, or a malformed file) still gathers/freezes/indexes,
            // but under a unique SYNTHETIC per-file key so it is never referenceable as a parent and never collides.
            const std::string Id = !Handle.empty()
                                       ? Handle
                                       : (std::string("\x01") + E.path().string() + "#" + std::to_string(NIdx));
            std::string Key = Id;
            const auto Prev = Tree.find(Key);
            if (Prev != Tree.end())
            {
                if (Prev->second == N) continue;   // identical → same node; keep one (the multi-seeder normal)
                // Same CID handle, DIFFERENT content: a CID is a content hash, so this only happens when a node's
                // stored "CID" is STALE (edited, not yet re-minted) or FORGED (a received block claiming a local CID).
                // Keep FIRST-SEEN — BuildCatalogIndex scans LIBRARY before CATALOG, so a local node always wins over a
                // later-scanned received one (a received block can never shadow a local handle by order, and received
                // nodes are browse-only, never minted). The loser is NOT dropped (that would vanish a real node) —
                // re-key it under a unique SYNTHETIC key so it still gathers/indexes by its own CID at freeze.
                LogWarn("NodeGraph::GatherWorkingTree", "duplicate node handle '" + Id + "' (" + E.path().string()
                        + ") — stale-or-forged stored CID; first-seen keeps the handle, this one indexes on its own CID");
                Key = std::string("\x01") + E.path().string() + "#" + std::to_string(NIdx);   // unique → never dropped
                if (Tree.count(Key)) continue;     // same file re-scanned (overlapping roots) → truly identical slot
            }
            Tree[Key] = N;
            Dirs[Key] = E.path().parent_path();
        }
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
