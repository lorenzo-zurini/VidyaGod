#include "nodegraph.h"
#include "cid.h"   // a node's identity: the CID of its canonical bytes

#include <mutex>
#include <unordered_map>
#include "ipfswrapper.h"     // BlockGet, BlockPut, FetchToPath
#include "commonutils.h"     // Log*

#include <deque>
#include <fstream>
#include <set>

// nodegraph.cpp — the gigagraph's I/O orchestration (calls the embedded node). The pure transforms it relies on
// (NormalizeLinks / FreezeNodeJson / TopoOrderForMint) live in nodegraphpure.cpp. See nodegraph.h.

namespace NodeGraph {

// Untrusted-block guards. A fetched node block's bytes hash to its CID, but the CID is attacker-chosen (pasted),
// so the CONTENT is untrusted: it can be arbitrarily deep JSON. nlohmann's parser is recursive-descent, so a deeply
// nested block overflows the stack before any callback can stop it. JsonDepthWithinLimit is a cheap string-aware
// bracket-depth pre-scan that rejects such a block WITHOUT recursing (run by VerifyNodeBytes; nodegraphpure.cpp —
// shared with GatherWorkingTree, whose scanned files include LANDED received blocks). And a hostile closure can be
// unbounded, so BuildFrozenIndex caps the node count.
static constexpr size_t kMaxClosureNodes = 200000;


NodeIndex BuildFrozenIndex(const std::vector<std::string> &RootCids, std::vector<std::string> *Missing)
{
    NodeIndex Idx;
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
        const std::string Js = IpfsWrapper::BlockGet(C, &Err);
        if (Js.empty())
        {
            LogWarn("NodeGraph::BuildFrozenIndex", "cannot fetch node block " + C + ": " + Err);
            if (Missing) Missing->push_back(C);
            continue;
        }
        nlohmann::ordered_json J;
        if (!VerifyNodeBytes(Js, C, J, &Err))   // untrusted bytes: depth-checked, canonical, hashing to C
        {
            LogWarn("NodeGraph::BuildFrozenIndex", "node block " + C + " refused: " + Err);
            if (Missing) Missing->push_back(C);
            continue;
        }

        Node N;
        if (!ManifestModel::ParseNode(J, {}, {}, N))   // not a node object
        {
            LogWarn("NodeGraph::BuildFrozenIndex", "block " + C + " is not a node");
            continue;
        }
        N.Cid = C;                                     // identity = the block's own CID
        auto [It, Ins] = Idx.Nodes.emplace(C, std::move(N));
        (void)Ins;                                     // Seen already guaranteed uniqueness
        // What it contains (NODE layers). ANY/NOT are compared, never fetched; SOURCE/COVER are content CIDs
        // fetched lazily at hydrate — never enqueued here.
        for (const std::string &P : It->second.Refs) Q.push_back(P);
    }
    ManifestModel::DeriveFacts(Idx);
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
        // Freeze (resolve handles→CIDs) and derive the CID: the hash of the canonical bytes, computed here — pure,
        // local, the same on every machine (Cid::OfNode).
        std::string Dangling, Err;
        const nlohmann::ordered_json Frozen = FreezeNodeJson(WorkingTree.at(Handle), HandleToCid, &WorkingTree, &Dangling);
        if (!Dangling.empty()) Err = "it references '" + Dangling + "', which did not freeze";
        const std::string Cid = Dangling.empty() ? Cid::OfNode(Frozen, &Err) : std::string();
        if (Cid.empty())
        {
            // A per-node failure: SKIP this node, never abort the whole index — one bad node must not empty the
            // library. Its referrers keep a ref to a CID nothing holds, which validation names.
            LogWarn("NodeGraph::FreezeToIndex", "skipping node '" + Handle + "': " + Err);
            ++Skipped;
            continue;
        }
        HandleToCid[Handle] = Cid;

        Node N;
        if (!ManifestModel::ParseNode(Frozen, {}, {}, N)) { ++Skipped; continue; }
        N.Cid = Cid;
        if (!Handle.empty() && (unsigned char)Handle[0] >= 0x20) N.Handle = Handle;   // the working tree's name for it
        const auto It = Dirs.find(Handle);
        if (It != Dirs.end()) N.BundleDir = It->second;   // on-disk content home → launch mounts from here
        // A handle-less node was gathered under "\x01<file>#<n>": that IS its file — the received-stub pruner needs it.
        if (!Handle.empty() && Handle[0] == '\x01')
            if (const size_t Hash = Handle.rfind('#'); Hash != std::string::npos) N.File = Handle.substr(1, Hash - 1);
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
    ManifestModel::DeriveFacts(Idx);
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

    // Bundle dir name from the tile: "[uid] title" (the pretty layout) — the tile its entries fold (DeriveFacts ran
    // inside BuildFrozenIndex).
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

    // Write each closure node VERBATIM as <cid>.json (the block IS the file; BuildFrozenIndex verified it) + fetch
    // its content leaves to their PATHs.
    int Files = 0, Fetched = 0, FetchFailed = 0;
    for (const auto &[C, N] : Idx.Nodes)
    {
        std::string Err;
        const std::string Js = IpfsWrapper::BlockGet(C, &Err);
        if (Js.empty()) { if (Error) *Error = "re-fetch " + C + ": " + Err; return {}; }
        { std::ofstream Out(Dir / (C + ".json"), std::ios::binary); Out << Js; }
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
        std::string Dangling, Err;
        const std::string Bytes = Cid::Canonical(FreezeNodeJson(Raw, Out.HandleToCid, &WorkingTree, &Dangling));
        if (!Dangling.empty()) Err = "it references '" + Dangling + "', which did not freeze";
        const std::string Want = Dangling.empty() ? Cid::OfBytes(Bytes, &Err) : std::string();
        const std::string Cid = Want.empty() ? std::string() : IpfsWrapper::BlockPut(Bytes, &Err);
        if (!Cid.empty() && Cid != Want)
        {   // the node stored something else than the app computes: every reference to it would dangle
            if (Error) *Error = "node '" + Handle + "' stored as " + Cid + " but its bytes hash to " + Want;
            return false;
        }
        if (Cid.empty())
        {
            // Dangling ref / bad link on one node — skip it (and its dependents will skip too), never abort the whole
            // publish. Mirrors FreezeToIndex: degrade per-node, not all-or-nothing.
            LogWarn("NodeGraph::Mint", "skipping node '" + Handle + "' (dangling ref / bad link): " + Err);
            ++Skipped;
            continue;
        }
        Out.HandleToCid[Handle] = Cid;

        // A playable list root: a variant (on the shelf). This is the LAUNCH axis (launch/CLI).
        if (Raw.contains("VARIANT")) Out.Launchables.push_back(Cid);
    }
    if (Skipped) LogWarn("NodeGraph::Mint", "skipped " + std::to_string(Skipped) + " node(s) with dangling/bad refs");
    LogSucc("NodeGraph::Mint", "froze " + std::to_string(Out.HandleToCid.size()) + " node(s), "
            + std::to_string(Out.Launchables.size()) + " launchable(s)");
    return true;
}

} // namespace NodeGraph
