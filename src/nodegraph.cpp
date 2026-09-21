#include "nodegraph.h"

#include <mutex>
#include <unordered_map>
#include "ipfswrapper.h"     // DagGet, DagPut, FetchToPath
#include "commonutils.h"     // Log*

#include <deque>
#include <fstream>
#include <set>

// nodegraph.cpp — the gigagraph's I/O orchestration (calls the embedded node). The pure transforms it relies on
// (NormalizeLinks / FreezeNodeJson / TopoOrderForMint) live in nodegraphpure.cpp. See nodegraph.h.

namespace NodeGraph {

// Untrusted-block guards. A fetched dag-json block's bytes hash to its CID, but the CID is attacker-chosen (pasted),
// so the CONTENT is untrusted: it can be arbitrarily deep JSON. nlohmann's parser is recursive-descent, so a deeply
// nested block overflows the stack before any callback can stop it. JsonDepthWithinLimit is a cheap string-aware
// bracket-depth pre-scan that rejects such a block WITHOUT recursing (JsonDepthWithinLimit, nodegraphpure.cpp —
// shared with GatherWorkingTree, whose scanned files include LANDED received blocks). And a hostile closure can be
// unbounded, so BuildFrozenIndex caps the node count.
static constexpr int    kMaxJsonDepth  = 64;
static constexpr size_t kMaxClosureNodes = 200000;

// Parse a block's bytes only after a depth check (never recurse into hostile JSON). False + empty J on reject/parse error.
static bool ParseBlockBounded(const std::string &Js, nlohmann::ordered_json &J)
{
    if (!JsonDepthWithinLimit(Js, kMaxJsonDepth)) return false;
    try { J = nlohmann::ordered_json::parse(Js); return true; }
    catch (const std::exception &) { return false; }
}

// True iff P resolves within Base (no `..` escape, not an absolute path elsewhere). Guards hydrate against a hostile
// content PATH / SOURCE.PATH writing outside the checkout dir. Works on not-yet-existent paths (weakly_canonical).
static bool PathWithin(const std::filesystem::path &Base, const std::filesystem::path &P)
{
    std::error_code Ec;
    std::filesystem::path B = std::filesystem::weakly_canonical(Base, Ec); if (Ec) B = Base.lexically_normal();
    std::filesystem::path Q = std::filesystem::weakly_canonical(P, Ec);    if (Ec) Q = P.lexically_normal();
    const std::filesystem::path Rel = Q.lexically_relative(B);
    if (Rel.empty()) return false;                       // unrelated / not under Base
    return Rel.begin()->native() != "..";                // first COMPONENT ".." ⇒ escapes (a leading-dot NAME like .wine is fine)
}

NodeIndex BuildFrozenIndex(const std::vector<std::string> &RootCids, std::vector<std::string> *Missing, bool Shallow, bool LocalOnly)
{
    NodeIndex Idx;
    if (Shallow)
    {
        // Browse: batch-fetch the roots + their tiles via the windowed session (the SAME rolling want-window +
        // friend-provider routing content uses) — NOT serial single-block gets; 900+ tiny blocks must not be 900
        // round-trips. Two rounds: the published roots, then their distinct LIBRARYITEM tiles.
        // LocalOnly (catalog-build): read only blocks already in the store, so we never stall on a friend block that
        // hasn't landed yet — the catalog shows what's present and re-renders as more arrive.
        const auto Fetch = LocalOnly ? &IpfsWrapper::DagGetManyLocal : &IpfsWrapper::DagGetMany;
        const std::map<std::string, std::string> RootBlocks = Fetch(RootCids);
        std::set<std::string> TileCids;
        for (const std::string &C : RootCids)
        {
            if (C.empty() || Idx.Nodes.count(C)) continue;
            const auto It = RootBlocks.find(C);
            if (It == RootBlocks.end()) { if (Missing) Missing->push_back(C); continue; }
            nlohmann::ordered_json J;
            if (!ParseBlockBounded(It->second, J)) { if (Missing) Missing->push_back(C); continue; }
            NormalizeLinks(J);
            Node N;
            if (!ManifestModel::ParseNode(J, {}, {}, N)) continue;
            N.Cid = C;
            auto [Nit, Ins] = Idx.Nodes.emplace(C, std::move(N));
            (void)Ins;
            if (!Nit->second.LibraryItem.empty()) TileCids.insert(Nit->second.LibraryItem);
        }
        std::vector<std::string> TV(TileCids.begin(), TileCids.end());
        const std::map<std::string, std::string> TileBlocks = Fetch(TV);
        for (const std::string &C : TV)
        {
            if (Idx.Nodes.count(C)) continue;
            const auto It = TileBlocks.find(C);
            if (It == TileBlocks.end()) continue;   // tile unfetchable → the card renders plainer; not fatal
            nlohmann::ordered_json J;
            if (!ParseBlockBounded(It->second, J)) continue;
            NormalizeLinks(J);
            Node N;
            if (!ManifestModel::ParseNode(J, {}, {}, N)) continue;
            N.Cid = C;
            Idx.Nodes.emplace(C, std::move(N));
        }
        ManifestModel::LinkGames(Idx);
        return Idx;
    }
    std::set<std::string> Seen;
    std::deque<std::string> Q(RootCids.begin(), RootCids.end());
    while (!Q.empty())
    {
        const std::string C = Q.front();
        Q.pop_front();
        if (C.empty() || !Seen.insert(C).second) continue;   // a shared node is reached many times — fetch once
        if (Idx.Nodes.size() >= kMaxClosureNodes)            // a hostile/looping closure must not run unbounded
        {
            LogErr("NodeGraph::BuildFrozenIndex", "closure exceeds " + std::to_string(kMaxClosureNodes)
                   + " nodes — stopping (root " + (RootCids.empty() ? "" : RootCids.front()) + ")");
            break;
        }

        std::string Err;
        const std::string Js = IpfsWrapper::DagGet(C, &Err);
        if (Js.empty())
        {
            LogWarn("NodeGraph::BuildFrozenIndex", "cannot fetch node block " + C + ": " + Err);
            if (Missing) Missing->push_back(C);
            continue;
        }
        nlohmann::ordered_json J;
        if (!ParseBlockBounded(Js, J))   // untrusted bytes: depth-checked, never recurse into hostile JSON
        {
            LogWarn("NodeGraph::BuildFrozenIndex", "unparseable / too-deep node block " + C);
            if (Missing) Missing->push_back(C);
            continue;
        }
        NormalizeLinks(J);

        Node N;
        if (!ManifestModel::ParseNode(J, {}, {}, N))   // no NODE_ID ⇒ not a node
        {
            LogWarn("NodeGraph::BuildFrozenIndex", "block " + C + " is not a node (no TYPE)");
            continue;
        }
        N.Cid = C;                                     // identity = the block's own CID
        auto [It, Ins] = Idx.Nodes.emplace(C, std::move(N));
        (void)Ins;                                     // Seen already guaranteed uniqueness
        // Recurse into PARENTS (composition edges) AND the LIBRARYITEM link (the tile — reachable ONLY this way now,
        // never via PARENTS). SOURCE/COVER are content-leaf (dag-pb) CIDs fetched lazily at hydrate — dagGet would
        // (correctly) refuse them — so they are never enqueued here.
        if (!Shallow)                                                    // full closure: composition edges too
            for (const std::string &P : It->second.Parents) Q.push_back(P);
        if (!It->second.LibraryItem.empty()) Q.push_back(It->second.LibraryItem);   // the tile — always (browse needs it)
    }
    ManifestModel::LinkGames(Idx);
    return Idx;
}

NodeIndex FreezeToIndex(const std::map<std::string, nlohmann::ordered_json> &WorkingTree,
                        const std::map<std::string, std::filesystem::path> &Dirs, std::string *Error)
{
    std::vector<std::string> Order;
    if (!TopoOrderForMint(WorkingTree, Order, Error)) return NodeIndex{};

    NodeIndex Idx;
    std::map<std::string, std::string> HandleToCid;
    std::set<std::string> AuthoredCids;   // CIDs whose index slot came from a LOCAL (non-synthetic) handle
    int Skipped = 0;
    for (const std::string &Handle : Order)
    {
        // Freeze (resolve handles→CIDs, linkify) and derive the CID with NO side effects. DagCid is PURE
        // (canonical bytes → CID), so memoize dump→cid across rebuilds: every catalog rebuild used to re-hash the
        // ENTIRE library through cgo — measured as a top slice of the GUI-thread startup freeze (rebuilds fire on
        // node-ready, source sync, friend snapshots, transfer completion…). Mutex'd: rebuilds run on several threads.
        static std::mutex CidMemoMu;
        static std::unordered_map<std::string, std::string> CidMemo;
        const nlohmann::ordered_json Frozen = FreezeNodeJson(WorkingTree.at(Handle), HandleToCid);
        std::string Err;
        std::string Cid;
        const std::string Dump = Frozen.dump();
        {
            std::lock_guard<std::mutex> Lk(CidMemoMu);
            const auto Mit = CidMemo.find(Dump);
            if (Mit != CidMemo.end()) Cid = Mit->second;
        }
        if (Cid.empty())
        {
            Cid = IpfsWrapper::DagCid(Dump, &Err);
            if (!Cid.empty())
            {
                std::lock_guard<std::mutex> Lk(CidMemoMu);
                if (CidMemo.size() >= 50000) CidMemo.clear();   // bound the memo; a reset just re-hashes once
                CidMemo.emplace(Dump, Cid);
            }
        }
        if (Cid.empty())
        {
            // A per-node failure — almost always a DANGLING ref (a PARENTS/LIBRARYITEM handle not in the tree, so it
            // linkified into an invalid CID). SKIP this node, never abort the whole index: one typo'd edge or one
            // deleted dependency must not empty the entire library. Its referrers will fail the same way and skip too,
            // so a dangling subtree drops while every other game survives.
            LogWarn("NodeGraph::FreezeToIndex", "skipping node '" + Handle + "' (dangling ref / bad link): " + Err);
            ++Skipped;
            continue;
        }
        HandleToCid[Handle] = Cid;

        // Build the in-memory Node from the resolved-but-plain form (links back to plain CID strings for ParseNode).
        nlohmann::ordered_json Plain = Frozen;
        NormalizeLinks(Plain);
        Node N;
        if (!ManifestModel::ParseNode(Plain, {}, {}, N)) { ++Skipped; continue; }
        N.Cid = Cid;
        const auto It = Dirs.find(Handle);
        if (It != Dirs.end()) N.BundleDir = It->second;   // on-disk content home → launch mounts from here
        // Two copies of one node (same content ⇒ same CID) can both be present: a LOCAL authored/installed one and a
        // RECEIVED one inside a just-hydrated friend package's closure. They are byte-identical, but their BundleDir
        // differs and launch mounts/seeds from BundleDir — so the LOCAL copy must win regardless of topo order. A
        // received/synthetic-gathered node has a control-byte ('\x01') handle; an authored/local one does not.
        const bool Authored = !Handle.empty() && (unsigned char)Handle[0] >= 0x20;
        const auto Ex = Idx.Nodes.find(Cid);
        if (Ex == Idx.Nodes.end()) { Idx.Nodes.emplace(Cid, std::move(N)); if (Authored) AuthoredCids.insert(Cid); }
        else if (Authored && !AuthoredCids.count(Cid))   // replace a received entry with the authored one (once)
        { Ex->second = std::move(N); AuthoredCids.insert(Cid); }
        // else: keep first-seen (existing authored, or both received)
    }
    if (Skipped) LogWarn("NodeGraph::FreezeToIndex", "skipped " + std::to_string(Skipped)
                         + " node(s) with dangling/bad refs — the rest of the library is intact");
    ManifestModel::LinkGames(Idx);
    return Idx;
}

// A single safe path segment: no separators, no leading dots (so a hostile TITLE can't escape DestRoot).
static std::string SanitizeSegment(std::string S)
{
    for (char &C : S) if (C == '/' || C == '\\' || C == '\0') C = '_';
    while (!S.empty() && (S.front() == '.' || S.front() == ' ')) S.erase(S.begin());
    if (S.empty()) S = "package";
    return S;
}

std::string HydratePackage(const std::filesystem::path &DestRoot, const std::string &LaunchableCid, std::string *Error)
{
    namespace fs = std::filesystem;
    std::vector<std::string> Missing;
    NodeIndex Idx = BuildFrozenIndex({LaunchableCid}, &Missing);
    if (!Missing.empty())
    { if (Error) *Error = "cannot fetch node block(s), first: " + Missing.front(); return {}; }
    const Node *Launch = Idx.Find(LaunchableCid);
    if (!Launch)
    { if (Error) *Error = "launchable " + LaunchableCid + " not a node after fetch"; return {}; }

    // Bundle dir name from the tile: "[uid] title" (the pretty layout). Meta is inherited via the LIBRARYITEM edge.
    std::string Uid   = Launch->Uid.empty() ? Launch->NodeId : Launch->Uid;
    std::string Title = Launch->Meta.is_object() ? Launch->Meta.value("TITLE", Launch->NodeId) : Launch->NodeId;
    const fs::path Dir    = DestRoot / SanitizeSegment("[" + Uid + "] " + Title);
    const fs::path Marker = Dir / ".vg-hydrating";   // present ⇒ an INCOMPLETE hydrate WE own → resumable, not foreign
    std::error_code Ec;
    if (fs::exists(Dir, Ec) && !fs::is_empty(Dir, Ec) && !fs::exists(Marker, Ec))
    {   // non-empty AND no marker ⇒ an authored / already-hydrated / name-collision dir — never clobber it
        if (Error) *Error = "already exists: " + Dir.string() + " — remove it first to re-download";
        return {};
    }
    fs::create_directories(Dir, Ec);
    if (Ec) { if (Error) *Error = "cannot create " + Dir.string() + ": " + Ec.message(); return {}; }
    { std::ofstream M(Marker); }   // claim the dir as ours so a failed run can be RETRIED (resumes into it), not bricked

    // Write each closure node's JSON (readable plain-CID form) + fetch its content leaves to their PATHs.
    int Files = 0, Fetched = 0, FetchFailed = 0;
    std::set<std::string> UsedFiles;   // guard two closure nodes sharing a NODE_ID label (attacker-mintable)
    for (const auto &[C, N] : Idx.Nodes)
    {
        std::string Err;
        const std::string Js = IpfsWrapper::DagGet(C, &Err);
        if (Js.empty()) { if (Error) *Error = "re-fetch " + C + ": " + Err; return {}; }
        nlohmann::ordered_json J;
        if (!ParseBlockBounded(Js, J)) { if (Error) *Error = "unparseable / too-deep block " + C; return {}; }
        NormalizeLinks(J);   // {"/":cid} → plain CID strings: readable, and re-freezes to the same CID
        // SECURITY: an honest frozen block never carries a top-level "CID" (it is stripped at freeze — a block can't
        // contain its own hash). A MALICIOUS block can embed one equal to a local node's handle to hijack it at the
        // next gather (and get baked into the author's files by StampNodeCids). A fetched block is identified solely by
        // the CID we fetched it BY, so drop any embedded handle — the node re-keys synthetically and can't hijack.
        if (J.is_object()) J.erase("CID");

        std::string Base = SanitizeSegment(N.NodeId);
        if (!UsedFiles.insert(Base).second) Base += "_" + C.substr(0, 12);   // NODE_ID collision → disambiguate by CID
        { std::ofstream Out(Dir / (Base + ".json")); Out << J.dump(2) << "\n"; }
        ++Files;

        // Content leaves: fetch each VFS layer's SOURCE.CID to its local PATH (BundleDir = Dir). Iterate N.Layers
        // directly with IsVfsLayer — the same shape HydrationMap uses (the flat lowered layers, not a COMPONENTS array).
        if (N.Layers.is_array())
            for (const auto &L : N.Layers)
            {
                if (!L.is_object() || !ManifestModel::IsVfsLayer(ManifestModel::LayerType(L))) continue;
                fs::path Local; std::string Cid;
                ManifestModel::LayerLocator(L, Dir, Local, Cid);
                if (Cid.empty() || Local.empty()) continue;
                if (!PathWithin(Dir, Local))   // hostile PATH / SOURCE.PATH ("../.." or absolute) must not escape the bundle
                {
                    LogWarn("NodeGraph::HydratePackage", "refusing out-of-bundle content path " + Local.string());
                    ++FetchFailed;
                    continue;
                }
                std::error_code Fe;
                if (fs::exists(Local, Fe)) continue;                    // already materialized
                std::error_code Pe; fs::create_directories(Local.parent_path(), Pe);
                std::string FErr;
                if (IpfsWrapper::FetchToPath(Cid, Local.string(), &FErr).empty())
                {
                    LogWarn("NodeGraph::HydratePackage", "content " + Cid + " -> " + Local.string() + " FAILED: " + FErr);
                    ++FetchFailed;
                }
                else ++Fetched;
            }
    }
    if (FetchFailed)   // an incomplete download is NOT a success — the game would not launch
    {
        // Marker LEFT in place: a later --hydrate/Add-game with the same CID resumes into this dir (the per-file
        // "already materialized" skip keeps what did land) instead of hitting the clobber refusal. One transient
        // blip never bricks the download.
        if (Error) *Error = std::to_string(FetchFailed) + " content file(s) could not be fetched into " + Dir.string()
                          + " — incomplete; re-run to resume";
        return {};
    }
    fs::remove(Marker, Ec);   // complete → the dir is now a normal checkout (a future re-hydrate refuses to clobber it)
    LogSucc("NodeGraph::HydratePackage", "hydrated " + Title + " -> " + Dir.string() + " ("
            + std::to_string(Files) + " node(s), " + std::to_string(Fetched) + " content file(s) fetched)");
    return Dir.string();
}

bool Mint(const std::map<std::string, nlohmann::ordered_json> &WorkingTree, MintResult &Out, std::string *Error)
{
    std::vector<std::string> Order;
    if (!TopoOrderForMint(WorkingTree, Order, Error)) return false;   // deps-first; cycle ⇒ refuse

    int Skipped = 0;
    for (const std::string &Handle : Order)
    {
        const nlohmann::ordered_json &Raw = WorkingTree.at(Handle);
        const nlohmann::ordered_json Frozen = FreezeNodeJson(Raw, Out.HandleToCid);
        std::string Err;
        const std::string Cid = IpfsWrapper::DagPut(Frozen.dump(), &Err);
        if (Cid.empty())
        {
            // Dangling ref / bad link on one node — skip it (and its dependents will skip too), never abort the whole
            // publish. Mirrors FreezeToIndex: degrade per-node, not all-or-nothing.
            LogWarn("NodeGraph::Mint", "skipping node '" + Handle + "' (dangling ref / bad link): " + Err);
            ++Skipped;
            continue;
        }
        Out.HandleToCid[Handle] = Cid;

        // A playable list root: a DeclareExec with NO GUEST. A GUEST-bearing DeclareExec is a runner (it provides
        // platforms) — distributed via a runner tile, not listed as a game. This is the LAUNCH axis (launch/CLI).
        if (Raw.value("TYPE", std::string()) == "DeclareExec"
            && !(Raw.contains("GUEST") && Raw["GUEST"].is_array() && !Raw["GUEST"].empty()))
            Out.Launchables.push_back(Cid);

        // The SHARE axis, decoupled from type: a node the author flagged PUBLISH=true is a shareable root (a game's
        // launchable, a runner exec, or a no-exec library head). This — not the launch axis — drives the share list.
        if (Raw.contains("PUBLISH") && Raw["PUBLISH"].is_boolean() && Raw["PUBLISH"].get<bool>())
            Out.Published.push_back(Cid);   // guarded: a hostile hydrated block's non-bool PUBLISH must not throw in the mint thread
    }
    if (Skipped) LogWarn("NodeGraph::Mint", "skipped " + std::to_string(Skipped) + " node(s) with dangling/bad refs");
    LogSucc("NodeGraph::Mint", "froze " + std::to_string(Out.HandleToCid.size()) + " node(s), "
            + std::to_string(Out.Launchables.size()) + " launchable(s)");
    return true;
}

} // namespace NodeGraph
