#include "nodegraph.h"
#include "cid.h"
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

bool VerifyNodeBytes(const std::string &Bytes, const std::string &ExpectCid, nlohmann::ordered_json &J, std::string *Error)
{
    const auto No = [&](const std::string &Why) { if (Error) *Error = Why; return false; };
    if (Bytes.size() > 256 * 1024) return No("over one 256 KiB block");
    if (Cid::OfBytes(Bytes) != ExpectCid) return No("its bytes do not hash to " + ExpectCid);
    if (!JsonDepthWithinLimit(Bytes, 64)) return No("nested too deep");
    J = nlohmann::ordered_json::parse(Bytes, nullptr, false);
    if (J.is_discarded() || !J.is_object()) return No("not a JSON object");
    if (!ManifestModel::IsNodeObject(J)) return No("not a node");
    //An honest node's bytes are canonical — and canonical bytes never carry its own handle ("CID") or canvas
    //coordinates ("POS"): the canonical form strips both, so bytes holding either fail here too. A handle in received
    //bytes would hijack a local node's name once the package is installed — refused, not repaired.
    if (Cid::Canonical(J) != Bytes) return No("not in canonical form (or carries a working-tree field: CID, POS)");
    return true;
}

bool VerifyLanded(const std::filesystem::path &File, const std::string &ExpectCid, nlohmann::ordered_json *Out, std::string *Error)
{
    std::ifstream In(File, std::ios::binary);
    if (!In) { if (Error) *Error = "not on disk"; return false; }
    std::string Bytes;
    char Buf[65536];
    while (In.read(Buf, sizeof Buf) || In.gcount() > 0)
    {
        Bytes.append(Buf, static_cast<size_t>(In.gcount()));
        if (Bytes.size() > 256 * 1024) { if (Error) *Error = "over one 256 KiB block"; return false; }
    }
    nlohmann::ordered_json J;
    if (!VerifyNodeBytes(Bytes, ExpectCid, J, Error)) return false;
    if (Out) *Out = std::move(J);
    return true;
}

// True iff P resolves within Base (no `..` escape, not an absolute path elsewhere). Guards hydrate against a hostile
// content PATH / SOURCE.PATH writing outside the checkout dir. Works on not-yet-existent paths (weakly_canonical).
bool PathWithin(const std::filesystem::path &Base, const std::filesystem::path &P)
{
    std::error_code Ec;
    std::filesystem::path B = std::filesystem::weakly_canonical(Base, Ec); if (Ec) B = Base.lexically_normal();
    std::filesystem::path Q = std::filesystem::weakly_canonical(P, Ec);    if (Ec) Q = P.lexically_normal();
    const std::filesystem::path Rel = Q.lexically_relative(B);
    if (Rel.empty()) return false;                       // unrelated / not under Base
    return Rel.begin()->native() != "..";                // first COMPONENT ".." ⇒ escapes (a leading-dot NAME like .wine is fine)
}

nlohmann::ordered_json FreezeNodeJson(nlohmann::ordered_json Raw,
                                      const std::map<std::string, std::string> &HandleToCid,
                                      const std::map<std::string, nlohmann::ordered_json> *Tree,
                                      std::string *Dangling)
{
    Raw.erase("POS");   // blueprint-canvas coordinates — non-semantic; never part of identity
    Raw.erase("CID");   // the node's OWN last-minted CID: a working-tree handle only (the block is ADDRESSED by its
                        // CID, so storing it inside would be circular). Stripped like POS → identity stays purely
                        // the hash of {TYPE, refs→CIDs, LABEL, payload}; the stored CID is never shipped.

    // Remap references (old handle CID → freshly-minted CID) as the cascade climbs. A ref absent from HandleToCid is
    // an already-frozen EXTERNAL dep (a shared library / cross-bundle node referenced by CID) — pass it through.
    // ONE remapper (ManifestModel::RemapNodeRefs) walks NODE, ANY and NOT refs alike, shared with the CID
    // write-back so the two can never disagree on where refs live. Every reference stays a plain CID string: there
    // are no links — what VidyaGod follows is decided by the field (NODE, SOURCE, TILE.COVER), not by the encoding.
    ManifestModel::RemapNodeRefs(Raw, [&](const std::string &Ref) {
        const auto It = HandleToCid.find(Ref);
        if (It != HandleToCid.end()) return It->second;
        if (Tree && Dangling && Dangling->empty() && Tree->count(Ref)) *Dangling = Ref;   // a tree node that did not freeze
        return Ref;
    });
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
    for (const std::string &P : ManifestModel::NodeRefs(Node, &Nots)) if (Tree.count(P)) Deps.push_back(P);
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
        if (E.is_directory(Ec) && IsFetchScratchDir(E.path())) { It.disable_recursion_pending(); continue; }
        if (!E.is_regular_file(Ec) || E.path().extension() != ".json") continue;
        // UNTRUSTED bytes can now live in the tree (a received share's block, landed verbatim by the fetch queue), so
        // the scan gets the same guards every fetched block gets — size cap + depth pre-scan BEFORE the recursive
        // parse/normalize, or a hostile deep block becomes a stack overflow that crashes EVERY startup scan.
        Ec.clear();
        nlohmann::ordered_json J;
        if (!ReadTreeJsonBounded(E.path(), J)) continue;
        const nlohmann::ordered_json &N = J;          // a file is ONE node: the file is the node's block
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
                                   : (std::string("\x01") + E.path().string() + "#1");
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
            Key = std::string("\x01") + E.path().string() + "#1";   // unique → never dropped
            if (Tree.count(Key)) continue;     // same file re-scanned (overlapping roots) → truly identical slot
        }
        Tree[Key] = N;
        const std::filesystem::path Parent = E.path().parent_path();
        Dirs[Key] = Parent.filename() == kPackageFolderDir ? Parent.parent_path() : Parent;   // a landed package folder
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
    // edge: the cyclic nodes still enter the order but fail at freeze (each references a node that has not frozen —
    // FreezeNodeJson reports it) and are SKIPPED per-node, while every acyclic package orders and freezes normally.
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
