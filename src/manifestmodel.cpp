#include "manifestmodel.h"
#include "commonutils.h"
#include "nodelower.h"   // NodeLower::Lower — flat node payload → the executor's ordered layer sequence
#include "varsubst.h"    // VarSubst::ConditionParses — WHEN syntax lint
#include "launchparams.h"   // ContainerParams::GetVariablesMap — the single source of truth for built-in %variables%

#include <set>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <string_view>
#include <deque>
#include <fstream>
#include <functional>
#include <algorithm>
#include <bit>
#include <regex>
#include <cctype>
#include <zip.h>   // node-graph validation reads content zips to case-check CONTENTPATH against real files

const Node *NodeIndex::Find(const std::string &Key) const
{
    // Primary key = the map key. In the gigagraph catalog that is a node's CID (identity); the resolver walks edges
    // by CID, so every internal Find hits this fast path. (A NODE_ID-keyed index — a single working-tree bundle scan —
    // hits it too.)
    auto It = Nodes.find(Key);
    if (It != Nodes.end()) return &It->second;
    // Fallback alias: resolve a human NODE_ID label to its node. Only entry points (a GUI launch id, a persisted
    // reference) look up by NODE_ID against a CID-keyed index; it is never on the resolver's hot path, so the linear
    // scan is cheap in practice. NODE_IDs are unique within a local library (first match wins).
    for (const auto &[K, N] : Nodes)
        if (N.NodeId == Key) return &N;
    return nullptr;
}

namespace ManifestModel {

// ----- node graph (everything is a node) -----

static bool ParseNodeOrThrow(const nlohmann::ordered_json &J, const std::filesystem::path &File,
                             const std::filesystem::path &BundleDir, Node &Out);

//THE boundary where untrusted JSON becomes a Node, and therefore the place that must be TOTAL.
//
//NodeLower::Lower already guarantees its OUTPUT is well-typed, which protects every consumer of Node::Layers.
//But ParseNode also reads a few fields off the RAW node — TOGGLE, NODE_ID, PARENTS, EXCLUDE — and nlohmann's
//.value() throws on a type mismatch. `{"TOGGLE": true}` (a plausible authoring slip: the field reads like a
//boolean) therefore still aborted the process inside BuildNodeIndex, which runs at STARTUP.
//
//Rather than hardening each read and hoping the next one added remembers, the whole derivation is guarded and
//a throw becomes the same refusal a malformed payload produces: the node is indexed, named by validation, and
//never routed through.
bool ParseNode(const nlohmann::ordered_json &J, const std::filesystem::path &File,
               const std::filesystem::path &BundleDir, Node &Out)
{
    try
    {
        return ParseNodeOrThrow(J, File, BundleDir, Out);
    }
    catch (const std::exception &E)
    {
        // Identity is the CID; LABEL is the optional pretty name/handle. A node is any object with a TYPE — that,
        // not a name, is what makes it a node now.
        if (!J.is_object() || !J.contains("TYPE") || !J["TYPE"].is_string()) return false;
        const std::string Id = (J.contains("LABEL") && J["LABEL"].is_string()) ? J["LABEL"].get<std::string>() : std::string();
        LogWarn("ManifestModel::ParseNode",
                "Malformed node '" + Id + "' — " + E.what() + " (" + File.string() + ")");
        Out = Node{};
        Out.NodeId     = Id;
        Out.LowerError = "node '" + Id + "': malformed field (" + E.what() + ")";
        Out.Layers     = nlohmann::ordered_json::array();
        if (J.contains("PARENTS") && J["PARENTS"].is_array())
            for (const auto &P : J["PARENTS"])
                if (P.is_string() && !P.get<std::string>().empty()) Out.Parents.push_back(P.get<std::string>());
        Out.File      = File;
        Out.BundleDir = BundleDir;
        return true;
    }
}

static bool ParseNodeOrThrow(const nlohmann::ordered_json &J, const std::filesystem::path &File,
                             const std::filesystem::path &BundleDir, Node &Out)
{
    // A node is an object with a TYPE (identity = CID). LABEL is the OPTIONAL pretty name + intra-tree authoring
    // handle (NODE_ID is gone). Out.NodeId now carries the LABEL value — it may be empty (a nameless node is still a
    // node, identified by its CID once frozen).
    if (!J.is_object() || !J.contains("TYPE") || !J["TYPE"].is_string()) return false;
    Out = Node{};
    // Model C: identity/wiring is the CID. A working-tree node stores its last-minted CID in "CID" (its authoring
    // handle, which PARENTS reference) — read it into Cid so Key() returns it. LABEL is the OPTIONAL cosmetic name,
    // kept in NodeId as a display fallback (and passed to the lowerer for error/layer naming). A FROZEN block has no
    // "CID" field (a block can't contain its own hash); its caller (FreezeToIndex) sets Cid to the DERIVED hash.
    Out.Cid      = (J.contains("CID") && J["CID"].is_string()) ? J["CID"].get<std::string>() : std::string();
    Out.NodeId   = (J.contains("LABEL") && J["LABEL"].is_string()) ? J["LABEL"].get<std::string>() : std::string();
    //A node IS one layer of one TYPE; NodeLower expands its (possibly batched) payload into the ordered
    //layer sequence the launch engine consumes. A malformed payload is refused here rather than launched
    //half-understood — the node is skipped and the graph reports it missing at resolve time.
    std::string LowerErr;
    Out.Layers = NodeLower::Lower(J, Out.NodeId, LowerErr);
    if (!LowerErr.empty())
    {
        //INDEXED, not dropped. Dropping it removed the node from the graph entirely, so a referrer dangled
        //(loud) but a LEAF mistake was reported by nothing at all — --validate-nodes said 0 errors while a
        //layer had silently vanished, contradicting the spec's own "unknown TYPE is an error". Keep it with no
        //layers and the reason attached: validation names it, and ResolveNodeOrder refuses to route through it.
        LogWarn("ManifestModel::ParseNode", "Malformed node — " + LowerErr + " (" + File.string() + ")");
        Out.LowerError = LowerErr;
        Out.Layers = nlohmann::ordered_json::array();
        //Its PROVENANCE and EDGES still have to be filled in. Returning bare left File/BundleDir/Parents empty,
        //and every scope selector matches on UID or BundleDir — so the broken node could never enter a scoped
        //set, and `--validate-nodes <bundle>` (the PRE-PUBLISH check) reported the very bundle containing the
        //mistake as clean. Only the whole-graph form saw it.
        if (J.contains("PARENTS") && J["PARENTS"].is_array())
            for (const auto &P : J["PARENTS"])
                if (P.is_string() && !P.get<std::string>().empty()) Out.Parents.push_back(P.get<std::string>());
        Out.File      = File;
        Out.BundleDir = BundleDir;
        return true;
    }

    //--- Identity is DERIVED entirely from the Declare* layers. A node with none is plain content. ---
    for (const auto &L : Out.Layers)
    {
        if (!L.is_object()) continue;
        const std::string T = L.value("TYPE", std::string());
        if (T == "DeclareExec")
        {
            Out.HasExec = true;
            Out.Exec = L; Out.Exec.erase("TYPE");                       // CONTENTPATH/EXEARGS/WORKDIR (+ PLATFORM/LABEL/RECOMMENDED)
            Out.HostPlatform = L.value("PLATFORM", Out.HostPlatform);
            Out.Label        = L.value("LABEL", Out.Label);
            Out.Recommended  = L.value("RECOMMENDED", Out.Recommended);
            Out.RecommendedRunner = L.value("RUNNER", Out.RecommendedRunner);   // soft package-side runner recommendation
        }
        else if (T == "DeclareRunner")
        {
            Out.HasRunner = true;
            Out.Exec = L; Out.Exec.erase("TYPE"); Out.Exec.erase("HOST"); Out.Exec.erase("GUEST");   // EXECUTABLE/ARGS/ENV/…
            Out.HostPlatform = L.value("HOST", Out.HostPlatform);
            Out.GuestPlatform.clear();
            if (L.contains("GUEST") && L["GUEST"].is_array())
                for (const auto &G : L["GUEST"]) if (G.is_string()) Out.GuestPlatform.push_back(G.get<std::string>());
        }
        else if (T == "DeclareLibraryItem")
        {
            Out.Meta = L; Out.Meta.erase("TYPE");                       // TITLE/COVER/UID + descriptive metadata
            Out.Uid  = L.value("UID", Out.Uid);
        }
    }
    //TOGGLE replaces the old OPTIONAL+DEFAULT pair: present ⇒ user-toggleable, and the value IS the
    //initial state. (They were never independent — DEFAULT was meaningless without OPTIONAL — and the
    //old name collided with CustomVar's own DEFAULT once payloads were hoisted onto the node.)
    //It belongs on the TAIL of a module's chain: ResolveNodeOrder only descends into nodes it keeps, so
    //switching the tail off drops the module's private ancestors with it.
    //An UNKNOWN value is refused, not guessed. `Out.Default = (Toggle != "off")` means every typo — "of",
    //"Off", "false" — reads as ON, i.e. the author asks for opt-in and ships opt-out. The two legal values
    //are the whole vocabulary, so anything else is a mistake worth naming.
    Out.RawWhen = (J.contains("WHEN") && J["WHEN"].is_string()) ? J["WHEN"].get<std::string>() : std::string();
    const std::string Toggle = J.value("TOGGLE", std::string());
    if (!Toggle.empty() && Toggle != "on" && Toggle != "off")
    {
        LogWarn("ManifestModel::ParseNode", "Malformed node '" + Out.NodeId + "' — TOGGLE must be \"on\" or "
                                            "\"off\", not \"" + Toggle + "\" (" + File.string() + ")");
        Out.LowerError = "node '" + Out.NodeId + "': TOGGLE must be \"on\" or \"off\", not \"" + Toggle + "\"";
        Out.Layers = nlohmann::ordered_json::array();
    }
    Out.Optional = !Toggle.empty();
    Out.Default  = (Toggle != "off");
    if (J.contains("EXCLUDE") && J["EXCLUDE"].is_array())
        for (const auto &E : J["EXCLUDE"]) if (E.is_string() && !std::string(E).empty()) Out.Exclude.push_back(E.get<std::string>());
    if (J.contains("PARENTS") && J["PARENTS"].is_array())
        for (const auto &P : J["PARENTS"]) if (P.is_string() && !std::string(P).empty()) Out.Parents.push_back(P.get<std::string>());
    //LIBRARYITEM — a DeclareExec's dedicated tile link, off the composition graph (never a PARENT). A CID when frozen,
    //a NODE_ID handle in a working tree (links are normalized to plain strings before ParseNode, so it reads as one).
    if (J.contains("LIBRARYITEM") && J["LIBRARYITEM"].is_string() && !J["LIBRARYITEM"].get<std::string>().empty())
        Out.LibraryItem = J["LIBRARYITEM"].get<std::string>();
    Out.File      = File;
    Out.BundleDir = BundleDir;
    return true;
}

void ScanBundleNodes(const std::filesystem::path &BundleDir, NodeIndex &Idx)
{
    std::error_code Ec;
    if (!std::filesystem::is_directory(BundleDir, Ec)) return;
    for (const auto &Entry : std::filesystem::directory_iterator(BundleDir, Ec))
    {
        if (!Entry.is_regular_file(Ec) || Entry.path().extension() != ".json") continue;
        std::ifstream In(Entry.path(), std::ios::binary);
        if (!In) continue;
        nlohmann::ordered_json J;
        try { In >> J; }
        catch (const std::exception &E)
        { LogWarn("ManifestModel::ScanBundleNodes", "Skipping unparseable " + Entry.path().string() + ": " + E.what()); continue; }
        //A file holds ONE node or an ARRAY of them. Grouping nodes into files is pure presentation —
        //it carries no semantics, so an author can keep a package's display vars together, split a
        //patch set out, or leave one node per file, without any of it meaning anything to the graph.
        nlohmann::ordered_json Single;                                    // only materialised for the 1-node form
        if (!J.is_array()) Single = nlohmann::ordered_json::array({J});   // (both operands lvalues ⇒ no copy of J)
        const nlohmann::ordered_json &Nodes = J.is_array() ? J : Single;
        for (const auto &Doc : Nodes)
        {
            Node N;
            if (!ParseNode(Doc, Entry.path(), BundleDir, N)) continue;    // not a node (no TYPE / bad payload)
            // Key by the node's HANDLE — its stored CID (Key() = Cid). A node with no stored CID (a never-minted
            // draft caught mid-author, or a frozen block with no "CID" field) gets a synthetic per-node key so
            // handle-less nodes never fold together. Refs (PARENTS) are CIDs, so they resolve against these keys.
            std::string K = N.Key();
            for (unsigned char c : K) if (c < 0x20) { K.clear(); break; }   // control-byte handle -> synthetic (unforgeable key)
            if (K.empty()) K = std::string("\x01") + Entry.path().string() + "#unnamed" + std::to_string(Idx.Nodes.size());  // control-byte prefix: unforgeable
            else if (Idx.Nodes.count(K))
            { LogWarn("ManifestModel::ScanBundleNodes", "Duplicate node handle '" + K + "' (" + Entry.path().string() + ") — keeping first-seen."); continue; }
            Idx.Nodes.emplace(K, std::move(N));
        }
    }
}

NodeIndex BuildNodeIndex(const std::vector<std::filesystem::path> &LibraryRoots,
                         const std::vector<std::filesystem::path> &ExtraBundleDirs)
{
    NodeIndex Idx;
    std::error_code Ec;
    for (const auto &Root : LibraryRoots)
    {
        if (!std::filesystem::is_directory(Root, Ec)) continue;
        for (const auto &Bundle : std::filesystem::directory_iterator(Root, Ec))
            if (Bundle.is_directory(Ec)) ScanBundleNodes(Bundle.path(), Idx);
    }
    for (const auto &Bundle : ExtraBundleDirs)               // locally-added packages: each dir IS a bundle
        if (std::filesystem::is_directory(Bundle, Ec)) ScanBundleNodes(Bundle, Idx);
    LinkGames(Idx);
    LogOut("ManifestModel::BuildNodeIndex", "Indexed " + std::to_string(Idx.Nodes.size()) + " node(s) across "
           + std::to_string(LibraryRoots.size()) + " root(s).");
    return Idx;
}

nlohmann::ordered_json ComposeAcrossClosure(
    const NodeIndex &Idx, const std::string &NodeId, const std::map<std::string, bool> &Toggles,
    const std::function<const nlohmann::ordered_json *(const Node &)> &Pick)
{
    nlohmann::ordered_json Out = nlohmann::ordered_json::object();
    for (const std::string &Id : ResolveNodeOrder(Idx, NodeId, Toggles))     // parents first, NodeId last = highest priority
    {
        const Node *N = Idx.Find(Id);
        if (!N) continue;
        const nlohmann::ordered_json *Obj = Pick(*N);
        if (!Obj || !Obj->is_object()) continue;
        for (const auto &[K, V] : Obj->items())
        {
            //ENV is a BAG of independent keys, not a single value, so a more-specific node replacing it
            //wholesale silently deletes the base's variables: a variant declaring ENV {B} over a base
            //declaring ENV {A} loses A. That was harmless only while nothing consumed a launchable's ENV;
            //now it reaches the process, so it is a missing environment variable at launch with no
            //diagnostic. Merge key-wise — the more-specific node still wins on a shared KEY.
            //ENV is a BAG of keys: merge, do not replace. ENV_REMOVE/REMOVE_ENV is a LIST of keys and has
            //exactly the same property — a variant declaring ENV_REMOVE ["B"] over a base declaring ["A"]
            //silently stopped removing A.
            if (K == "ENV" && V.is_object() && Out.contains(K) && Out[K].is_object())
            {
                for (const auto &[EK, EV] : V.items()) Out[K][EK] = EV;
                continue;
            }
            if ((K == "ENV_REMOVE" || K == "REMOVE_ENV") && V.is_array()
                && Out.contains(K) && Out[K].is_array())
            {
                for (const auto &E : V)
                {
                    bool Seen = false;
                    for (const auto &X : Out[K]) if (X == E) { Seen = true; break; }
                    if (!Seen) Out[K].push_back(E);
                }
                continue;
            }
            Out[K] = V;                                                      // later (more-specific) node wins, field-by-field
        }
    }
    return Out;
}

void LinkGames(NodeIndex &Idx)
{
    // A "game node" is a node with its own library metadata (Presentable). A launchable VARIANT that lacks its own
    // metadata belongs to the game node nearest in its PARENTS closure: it inherits that tile's Meta/UID and records it
    // as its Game key — so the library groups variants under one tile via the graph edge (no GAME string). A launchable
    // that IS already presentable is a self-contained single-variant tile; left as-is.
    //
    // Meta inheritance is FIELD-LEVEL last-wins across the closure (ComposeAcrossClosure over every presentable node) —
    // the same merge as the launch-time DeclareExec composition — so a variant can override an individual tile field and
    // still inherit the rest. The Game KEY itself is the nearest presentable ancestor (grouping is a single edge).
    // Computed in ONE memoized pass over PARENTS (O(N+E)) rather than a ResolveNodeOrder + ComposeAcrossClosure walk
    // PER launchable — the latter is O(N²) for deep chains (e.g. a 900-version Minecraft package whose launchables all
    // group under one tile through a ~900-deep delta chain: the per-launchable closure walk timed the catalog out).
    // Per node: `Nearest` = the nearest presentable node INCLUDING itself; `Meta` = presentable Meta field-merged over
    // its closure (parents-before-children, own last) — matching ComposeAcrossClosure for the single-game-ancestor case.
    struct G { std::string Nearest; nlohmann::ordered_json Meta; };
    std::unordered_map<std::string, G> Memo;
    std::function<const G &(const std::string &)> Compose = [&](const std::string &Id) -> const G & {
        auto It = Memo.find(Id);
        if (It != Memo.end()) return It->second;
        G &g = Memo[Id];                                                     // insert placeholder (cycle guard; graph is a DAG)
        g.Meta = nlohmann::ordered_json::object();
        const Node *N = Idx.Find(Id);
        if (!N) return g;
        for (const std::string &P : N->Parents)                             // parents first = lower priority
        {
            if (!Idx.Find(P)) continue;
            const G &pg = Compose(P);
            for (const auto &[K, V] : pg.Meta.items()) g.Meta[K] = V;
        }
        if (N->Presentable())
        {
            for (const auto &[K, V] : N->Meta.items()) g.Meta[K] = V;        // own metadata wins (most specific)
            g.Nearest = Id;
        }
        else                                                                // nearest presentable ancestor: a presentable
        {                                                                   // direct parent first, else the nearest via a parent
            for (const std::string &P : N->Parents) { const Node *A = Idx.Find(P); if (A && A->Presentable()) { g.Nearest = P; break; } }
            if (g.Nearest.empty())
                for (const std::string &P : N->Parents) { if (!Idx.Find(P)) continue; const G &pg = Compose(P); if (!pg.Nearest.empty()) { g.Nearest = pg.Nearest; break; } }
        }
        return g;
    };
    for (const auto &[Id, N] : Idx.Nodes)
    {
        if (!N.IsLaunchable() || N.Presentable()) continue;                  // not a variant needing a game link
        Node &Nn = Idx.Nodes[Id];
        //Gigagraph form: the launchable carries an explicit LIBRARYITEM edge to its tile — grouping/Meta/UID come
        //straight from that node (O(1), no ancestor walk). This is the dedicated metadata edge that keeps PARENTS
        //pure composition; see nodegraph.h. Falls through to the legacy PARENTS ancestor-walk when absent (a working
        //tree or pre-cutover data whose tile is still a PARENT).
        if (!N.LibraryItem.empty())
        {
            const Node *Tile = Idx.Find(N.LibraryItem);
            if (Tile && Tile->Presentable())
            {
                Nn.Game = N.LibraryItem;                                     // grouping key = the tile's id/CID
                Nn.Meta = Tile->Meta;
                if (Nn.Uid.empty()) Nn.Uid = Tile->Meta.value("UID", std::string());
            }
            continue;
        }
        const G &g = Compose(Id);
        if (g.Meta.empty() || g.Nearest.empty()) continue;                   // no presentable ancestor → not a variant
        Nn.Game = g.Nearest;                                                 // nearest presentable ancestor (the tile key)
        Nn.Meta = g.Meta;                                                    // the closure-composed tile metadata
        if (Nn.Uid.empty()) Nn.Uid = g.Meta.value("UID", std::string());
    }
}

std::vector<std::string> ResolveNodeOrder(const NodeIndex &Idx, const std::string &LaunchNodeId,
                                          const std::map<std::string, bool> &Toggles,
                                          std::vector<std::string> *Missing)
{
    std::vector<std::string> Order;
    const Node *Launch = Idx.Find(LaunchNodeId);
    //A node whose payload never lowered is unusable: routing through it would apply nothing where the author
    //declared something. Treated as missing so the launch refuses loudly instead of quietly doing less.
    if (!Launch || !Launch->LowerError.empty())
    { if (Missing) Missing->push_back(LaunchNodeId); return Order; }

    //Phase 1 — determine the ENABLED set by a breadth-first walk over PARENTS from the launch node, applying
    //each node's own optional/EXCLUDE gate. A required parent is always pulled; an optional one only if its
    //toggle (else DEFAULT) is on and it doesn't conflict with an already-kept node. We only descend INTO a node
    //we keep, so an optional node's unique ancestors are naturally dropped with it (the hierarchy gate).
    //Pure membership sets (never iterated in order — the topological order is carried by `Order` below), so hash
    //sets over the tree-based std::set: on a deep chain these are probed millions of times and string-key tree
    //compares were a top validate-pass cost.
    std::unordered_set<std::string> Enabled;
    std::unordered_set<std::string> KeptNodeIds;                           // NODE_ID labels of kept nodes — EXCLUDE names nodes
    std::unordered_set<std::string> ExcludedByKept;                        // ∪ of Exclude targets of every kept node
    //Keep a node: record it, and fold its Exclude list into ExcludedByKept so the symmetric check below is O(1).
    //EXCLUDE lists (and toggles) name nodes by NODE_ID, while the index/traversal keys by CID — so EXCLUDE is matched
    //in NODE_ID space (KeptNodeIds), never against the CID-keyed Enabled set.
    auto Keep = [&](const std::string &Id, const Node *N) {
        Enabled.insert(Id);
        if (N) { KeptNodeIds.insert(N->NodeId); for (const auto &E : N->Exclude) ExcludedByKept.insert(E); }
    };
    //A candidate conflicts if it excludes an already-kept node, OR an already-kept node excludes it. Both arms match
    //in NODE_ID space (EXCLUDE entries and NODE_IDs), never against CIDs.
    auto Conflicts = [&](const Node &N) {
        for (const auto &E : N.Exclude) if (KeptNodeIds.count(E)) return true;   // this node excludes a kept one
        return ExcludedByKept.count(N.NodeId) != 0;                             // a kept node excludes this one
    };
    // Index-based head instead of erase(begin()): popping a vector's front shifts every element, turning a deep
    // chain's walk quadratic (a 900-deep delta chain = ~400k string moves per resolve).
    std::vector<std::string> Frontier = { LaunchNodeId };
    size_t FrontierHead = 0;
    Keep(LaunchNodeId, Idx.Find(LaunchNodeId));
    while (FrontierHead < Frontier.size())
    {
        const std::string Cur = Frontier[FrontierHead++];
        const Node *N = Idx.Find(Cur);
        if (!N) continue;
        //Consider EXPLICITLY-toggled-on parents before the rest, so an explicit choice is kept before any
        //conflicting DEFAULT-on sibling (first-kept wins the EXCLUDE). Otherwise toggling a mutually-exclusive
        //option on would be silently dropped in favour of the other option's default. Stable: order is otherwise
        //preserved (PARENTS list order).
        //Toggles are keyed by NODE_ID (what the picker/CLI store), while Parents are CIDs — resolve each to its node
        //and look the toggle up by NODE_ID, or every toggle silently misses in the CID-keyed catalog.
        auto ExplicitOn = [&](const std::string &Id) {
            const Node *P = Idx.Find(Id);
            if (!P) return false;
            auto It = Toggles.find(P->NodeId);
            return It != Toggles.end() && It->second;
        };
        std::vector<std::string> Parents(N->Parents.begin(), N->Parents.end());
        std::stable_sort(Parents.begin(), Parents.end(),
                         [&](const std::string &A, const std::string &B) { return ExplicitOn(A) && !ExplicitOn(B); });
        for (const std::string &Pid : Parents)
        {
            if (Enabled.count(Pid)) continue;
            const Node *P = Idx.Find(Pid);
            if (!P || !P->LowerError.empty()) { if (Missing) Missing->push_back(Pid); continue; }
            if (P->Optional)
            {
                const bool On = Toggles.count(P->NodeId) ? Toggles.at(P->NodeId) : P->Default;   // toggles keyed by NODE_ID
                if (!On) continue;
            }
            if (Conflicts(*P)) continue;
            Keep(Pid, P); Frontier.push_back(Pid);
        }
    }

    //Phase 2 — topo-order the enabled subgraph: emit a node only after all its enabled parents (post-order DFS
    //following PARENTS in list order). The launchable, having no enabled children, is emitted last = highest
    //priority. Cycles are reported and broken (the back-edge is skipped) so resolution still completes.
    std::unordered_set<std::string> Visited;
    std::unordered_set<std::string> OnStack;
    std::function<void(const std::string &)> Emit = [&](const std::string &Id) {
        if (Visited.count(Id)) return;
        if (OnStack.count(Id)) { LogWarn("ManifestModel::ResolveNodeOrder", "Cycle through node '" + Id + "' — breaking edge."); return; }
        OnStack.insert(Id);
        const Node *N = Idx.Find(Id);
        if (N) for (const std::string &Pid : N->Parents) if (Enabled.count(Pid)) Emit(Pid);
        OnStack.erase(Id);
        Visited.insert(Id);
        Order.push_back(Id);
    };
    Emit(LaunchNodeId);
    return Order;
}

std::vector<const Node*> OptionalNodes(const NodeIndex &Idx, const std::string &LaunchNodeId)
{
    std::vector<const Node*> Out;
    std::set<std::string> Seen{LaunchNodeId};
    std::vector<std::string> Frontier{LaunchNodeId};
    while (!Frontier.empty())
    {
        const std::string Cur = Frontier.back(); Frontier.pop_back();
        const Node *N = Idx.Find(Cur);
        if (!N) continue;
        for (const std::string &P : N->Parents)
        {
            if (!Seen.insert(P).second) continue;
            const Node *PN = Idx.Find(P);
            if (!PN || PN->IsRunner()) continue;
            if (PN->Optional) Out.push_back(PN);
            Frontier.push_back(P);
        }
    }
    return Out;
}

std::vector<EndpointInfo> TileEndpoints(const NodeIndex &Idx, const std::vector<std::string> &VariantIds)
{
    // Endpoints are computed over the CONTENT-CARRYING subgraph, not raw graph sinks. Lesson learned the hard way:
    // every launchable variant is typically a graph sink (MC's per-version "..._game" wrapper holds only
    // DeclareExec/CustomVar and hangs OFF the delta chain — nothing depends on it), so "sinks of the node DAG"
    // yielded 903 endpoints. What actually nests is the CONTENT: a content-sink is a content node no other content
    // node (transitively) depends on, and the endpoint shown to the user is the VARIANT with the smallest closure
    // covering that sink — "the version that owns the tip". Variants covering no sink are dominated (their content
    // ⊆ an endpoint's) and remain reachable via the dialog's Custom list.

    // 1. Union closure over all variants: one shared-visited DFS — O(distinct nodes), never per-variant.
    std::unordered_set<std::string> Visited;
    std::vector<std::string> Stack(VariantIds.begin(), VariantIds.end());
    std::vector<const Node*> Union;
    while (!Stack.empty())
    {
        const std::string Cur = Stack.back(); Stack.pop_back();
        if (!Visited.insert(Cur).second) continue;
        const Node *N = Idx.Find(Cur);
        if (!N) continue;
        Union.push_back(N);
        for (const std::string &Pid : N->Parents) Stack.push_back(Pid);
    }
    const int Nn = (int)Union.size();
    std::unordered_map<std::string, int> IxOf;
    for (int i = 0; i < Nn; ++i) IxOf[Union[i]->NodeId] = i;

    auto CarriesContent = [](const Node *N) {
        if (N->IsRunner() || !N->Layers.is_array()) return false;
        for (const auto &L : N->Layers) if (IsVfsLayer(LayerType(L))) return true;
        return false;
    };

    // 2. Per-node closure BITSETS, memoized bottom-up (closure(n) = self ∪ closure(parents)) — cheap: Nn ≤ a few
    //    thousand, so Nn²/64 words total. Everything below is bit arithmetic on these.
    const int Words = (Nn + 63) / 64;
    std::vector<std::vector<uint64_t>> Clo(Nn);
    std::function<const std::vector<uint64_t>&(int)> CloOf = [&](int I) -> const std::vector<uint64_t>& {
        if (!Clo[I].empty()) return Clo[I];
        std::vector<uint64_t> B(Words, 0);
        B[I >> 6] |= (uint64_t)1 << (I & 63);
        for (const std::string &Pid : Union[I]->Parents)
            if (auto It = IxOf.find(Pid); It != IxOf.end())
            {
                const std::vector<uint64_t> &P = CloOf(It->second);
                for (int W = 0; W < Words; ++W) B[W] |= P[W];
            }
        Clo[I] = std::move(B);
        return Clo[I];
    };
    for (int i = 0; i < Nn; ++i) CloOf(i);

    std::vector<uint64_t> ContentMask(Words, 0);
    for (int i = 0; i < Nn; ++i)
        if (CarriesContent(Union[i])) ContentMask[i >> 6] |= (uint64_t)1 << (i & 63);

    // 3. The REQUIRED region: reachable without descending into Optional nodes — an optional add-on's private
    //    content must not surface endpoints (it belongs to the optional-content section).
    std::vector<uint64_t> Required(Words, 0);
    {
        const std::unordered_set<std::string> VarSet(VariantIds.begin(), VariantIds.end());
        std::unordered_set<std::string> Seen;
        std::vector<std::string> St(VariantIds.begin(), VariantIds.end());
        while (!St.empty())
        {
            const std::string Cur = St.back(); St.pop_back();
            if (!Seen.insert(Cur).second) continue;
            auto It = IxOf.find(Cur);
            if (It == IxOf.end()) continue;
            const Node *N = Union[It->second];
            if (N->Optional && !VarSet.count(Cur)) continue;           // don't descend into optionals
            Required[It->second >> 6] |= (uint64_t)1 << (It->second & 63);
            for (const std::string &Pid : N->Parents) St.push_back(Pid);
        }
    }

    // 4. GREEDY SET-COVER over the required content: repeatedly pick the variant covering the most still-uncovered
    //    content bits. Nesting collapses to one pick; genuine branches each get one; small unique-content leaves
    //    (natives bundles, wrapper scripts) trail at the end with tiny gains. Deterministic tie-break: larger total
    //    content, then node id.
    std::vector<int> VarIx; VarIx.reserve(VariantIds.size());
    for (const std::string &V : VariantIds)
        if (auto It = IxOf.find(V); It != IxOf.end()) VarIx.push_back(It->second);

    std::vector<uint64_t> Uncovered(Words);
    for (int W = 0; W < Words; ++W) Uncovered[W] = Required[W] & ContentMask[W];
    std::vector<int> Picks;
    std::vector<int> PickGains;
    std::unordered_set<int> Picked;
    for (;;)
    {
        int Best = -1, BestGain = 0, BestTotal = 0;
        for (int V : VarIx)
        {
            if (Picked.count(V)) continue;
            int Gain = 0, Total = 0;
            for (int W = 0; W < Words; ++W)
            {
                Gain  += (int)std::popcount(Clo[V][W] & Uncovered[W]);
                Total += (int)std::popcount(Clo[V][W] & ContentMask[W]);
            }
            if (Gain > BestGain
                || (Gain == BestGain && Gain > 0
                    && (Total > BestTotal || (Total == BestTotal && Best >= 0 && Union[V]->NodeId < Union[Best]->NodeId))))
            { Best = V; BestGain = Gain; BestTotal = Total; }
        }
        if (Best < 0 || BestGain == 0) break;
        Picks.push_back(Best); PickGains.push_back(BestGain); Picked.insert(Best);
        for (int W = 0; W < Words; ++W) Uncovered[W] &= ~Clo[Best][W];
    }
    // Contentless tile (nothing to download by closure) → degrade to the variants themselves so the dialog isn't empty.
    if (Picks.empty())
    {
        Picks = VarIx;
        PickGains.assign(Picks.size(), 1);
    }

    // 5. Rows: a pick stands alone only when it contributed a MEANINGFUL share of the content (≥5% of the total —
    //    real branches like MC's forked snapshot chains, AoE2's editions); the long tail of small unique-content
    //    owners (per-era natives bundles, wrapper scripts — 11 of them in the real MC graph) folds into one
    //    "Everything else" row. Always at least one named row; capped so the dialog stays a short honest list.
    int TotalContent = 0;
    for (int W = 0; W < Words; ++W) TotalContent += (int)std::popcount(Required[W] & ContentMask[W]);
    constexpr size_t MaxNamedRows = 6;
    size_t Named = 0;
    while (Named < Picks.size() && Named < MaxNamedRows
           && (Named == 0 || (long long)PickGains[Named] * 20 >= TotalContent))
        ++Named;
    if (Named + 1 == Picks.size()) ++Named;   // never fold a single leftover — its own row is shorter than the fold
    std::vector<EndpointInfo> Out;
    for (size_t K = 0; K < Named; ++K)
    {
        const Node *N = Union[Picks[K]];
        EndpointInfo E;
        E.Ids   = { N->NodeId };
        E.Label = !N->Label.empty() ? N->Label
                  : (N->Meta.is_object() ? N->Meta.value("TITLE", N->NodeId) : N->NodeId);
        for (int V : VarIx)
        {
            bool Sub = true;
            for (int W = 0; W < Words && Sub; ++W)
                if ((Clo[V][W] & ContentMask[W]) & ~(Clo[Picks[K]][W] & ContentMask[W])) Sub = false;
            if (Sub) E.LaunchableCount++;
        }
        Out.push_back(std::move(E));
    }
    if (Named < Picks.size())
    {
        EndpointInfo Rest;
        Rest.Label = "Everything else";
        for (size_t K = Named; K < Picks.size(); ++K) Rest.Ids.push_back(Union[Picks[K]]->NodeId);
        Out.push_back(std::move(Rest));
    }
    return Out;
}

namespace {

std::string ToLowerAscii(std::string S) { for (char &c : S) c = (char)std::tolower((unsigned char)c); return S; }

// The regular file paths inside a zip (slash-normalized, directory entries dropped). Empty on open failure.
const std::vector<std::string> &ZipEntriesCached(const std::string &Path,
        std::unordered_map<std::string, std::vector<std::string>> &Cache)
{
    auto It = Cache.find(Path);
    if (It != Cache.end()) return It->second;
    std::vector<std::string> Out;
    int Err = 0;
    if (zip_t *Za = zip_open(Path.c_str(), ZIP_RDONLY, &Err))
    {
        const zip_int64_t N = zip_get_num_entries(Za, 0);
        for (zip_uint64_t i = 0; i < (zip_uint64_t)N; ++i)
        {
            const char *Name = zip_get_name(Za, i, ZIP_FL_ENC_RAW);
            if (!Name || !*Name) continue;
            std::string S = Name;
            std::replace(S.begin(), S.end(), '\\', '/');
            if (S.back() == '/') continue;                                   // directory entry
            Out.push_back(std::move(S));
        }
        zip_close(Za);
    }
    return Cache.emplace(Path, std::move(Out)).first->second;
}


// Join a layer's TARGET (mount offset within the content root) and an in-layer relative path into one
// content-root-relative slash path.
std::string JoinTarget(std::string Target, std::string Rel)
{
    std::replace(Target.begin(), Target.end(), '\\', '/');
    std::replace(Rel.begin(), Rel.end(), '\\', '/');
    while (!Target.empty() && Target.front() == '/') Target.erase(Target.begin());
    while (!Target.empty() && Target.back()  == '/') Target.pop_back();
    while (!Rel.empty() && Rel.front() == '/') Rel.erase(Rel.begin());
    return Target.empty() ? Rel : Target + "/" + Rel;
}

// The case-sensitive set of content-root-relative file paths a launchable's closure provides, from LOCALLY-present
// VFS layers only (zips via libzip, dirs walked). AnyLocal=≥1 layer read; AllLocal=every VFS layer was on disk.
void GatherLaunchContentFiles(const NodeIndex &Idx, const Node &Launch,
        std::unordered_map<std::string, std::vector<std::string>> &ZipCache,
        std::set<std::string> &Files, bool &AnyLocal, bool &AllLocal)
{
    AnyLocal = false; AllLocal = true;
    for (const std::string &Id : ResolveNodeOrder(Idx, Launch.NodeId, {}))
    {
        const Node *N = Idx.Find(Id);
        if (!N || N->IsRunner() || !N->Layers.is_array()) continue;
        for (const auto &L : N->Layers)
        {
            if (!L.is_object() || !IsVfsLayer(LayerType(L))) continue;
            std::filesystem::path Local; std::string Cid;
            LayerLocator(L, N->BundleDir, Local, Cid);
            if (Local == N->BundleDir) continue;                             // no PATH at all
            std::error_code Ec;
            if (!std::filesystem::exists(Local, Ec)) { AllLocal = false; continue; }   // remote-only / not hydrated
            AnyLocal = true;
            const std::string Target = L.value("TARGET", std::string());
            if (LayerType(L) == "VFSFileLayer")
                //A VFSFileLayer mounts one file at TARGET/<source basename> (overlay.cpp: fileVPath) — its Local is a
                //regular file, NOT a zip, so it must be added directly (else ZipEntriesCached opens it as an archive,
                //finds nothing, and the CONTENTPATH check false-positives "not found in the package content").
                Files.insert(JoinTarget(Target, Local.filename().string()));
            else if (std::filesystem::is_directory(Local, Ec))
                for (auto It = std::filesystem::recursive_directory_iterator(Local, Ec);
                     !Ec && It != std::filesystem::recursive_directory_iterator(); It.increment(Ec))
                {
                    if (!It->is_regular_file(Ec)) continue;
                    Files.insert(JoinTarget(Target, std::filesystem::relative(It->path(), Local, Ec).generic_string()));
                }
            else
                for (const std::string &Entry : ZipEntriesCached(Local.string(), ZipCache))
                    Files.insert(JoinTarget(Target, Entry));
        }
    }
}

// One layer's contribution to the merged content view, fully prepared for the case lint: the joined
// content-root-relative path plus its lowered form. Cached per (local path, TARGET) because this is where
// --validate-nodes actually spent its time: 903 Minecraft variants share a handful of layer files, and the lint
// re-ran JoinTarget + ToLowerAscii over every entry of every shared layer once PER LAUNCHABLE — 87% of a 49-second
// validate by perf, all of it recomputing strings that never change within a run. Dir layers additionally re-walked
// the filesystem per launchable; the walk is now taken once too.
struct PreparedEntry { std::string Exact, Lower; };
const std::vector<PreparedEntry> &LayerEntriesPrepared(const std::string &LocalPath, bool IsDir, const std::string &Target,
        std::unordered_map<std::string, std::vector<std::string>> &ZipCache,
        std::unordered_map<std::string, std::vector<PreparedEntry>> &PrepCache)
{
    const std::string Key = LocalPath + '\x1f' + Target;
    auto It = PrepCache.find(Key);
    if (It != PrepCache.end()) return It->second;
    std::vector<PreparedEntry> Out;
    auto Add = [&](std::string Rel) {
        std::string Exact = JoinTarget(Target, std::move(Rel));
        std::string Lower = ToLowerAscii(Exact);
        Out.push_back({std::move(Exact), std::move(Lower)});
    };
    std::error_code Ec;
    if (IsDir)
        for (auto W = std::filesystem::recursive_directory_iterator(LocalPath, Ec);
             !Ec && W != std::filesystem::recursive_directory_iterator(); W.increment(Ec))
        { if (W->is_regular_file(Ec)) Add(std::filesystem::relative(W->path(), LocalPath, Ec).generic_string()); }
    else
        for (const std::string &Entry : ZipEntriesCached(LocalPath, ZipCache))
            Add(Entry);
    return PrepCache.emplace(Key, std::move(Out)).first->second;
}

// Path components of a slash path.
std::vector<std::string> SplitPath(const std::string &S)
{
    std::vector<std::string> V; size_t P = 0, Q;
    while ((Q = S.find('/', P)) != std::string::npos) { V.push_back(S.substr(P, Q - P)); P = Q + 1; }
    V.push_back(S.substr(P));
    return V;
}

// Reports case collisions across DIFFERENT layers in a launchable's merged content view: two layers contributing
// paths that differ only in case (e.g. base ships 'MAPS/foo', a patch ships 'maps/foo'). On the case-sensitive mount
// both exist, so a lookup can resolve to the wrong file → crashes / missing data. Collisions WITHIN one layer are the
// upstream game's own content (unavoidable, merges fine on real Windows) and are deliberately ignored. A directory-case
// difference collapses to one report; GlobalSeen dedups the same collision across launchables that share the content.
void FindCrossLayerCaseCollisions(const NodeIndex &Idx, const Node &Launch,
        std::unordered_map<std::string, std::vector<std::string>> &ZipCache,
        std::unordered_map<std::string, std::vector<PreparedEntry>> &PrepCache,
        std::vector<std::string> &Out, std::unordered_set<std::string> &GlobalSeen)
{
    // Values POINT INTO PrepCache / the per-node label — safe because the cache only ever grows within one
    // validate run and a vector's heap buffer doesn't move when the cache map rehashes.
    std::unordered_map<std::string_view, std::pair<std::string_view, const std::string *>> Seen;  // lowered -> (exact, label)
    std::unordered_set<std::string> LocalReported;                             // collapse repeats within this launchable

    auto Consider = [&](const PreparedEntry &E, const std::string &Lbl)
    {
        auto It = Seen.find(std::string_view(E.Lower));
        if (It == Seen.end()) { Seen.emplace(std::string_view(E.Lower), std::make_pair(std::string_view(E.Exact), &Lbl)); return; }
        const std::string Prev(It->second.first), PrevLbl = *It->second.second, F = E.Exact;
        if (Prev == F || PrevLbl == Lbl) return;                     // same case, or same layer → not a cross-layer case collision
        const std::vector<std::string> A = SplitPath(Prev), B = SplitPath(F);
        size_t i = 0; while (i < A.size() && i < B.size() && A[i] == B[i]) ++i;
        if (i >= A.size() || i >= B.size()) return;
        std::string Key; for (size_t k = 0; k <= i; ++k) { if (k) Key += '/'; Key += ToLowerAscii(A[k]); }
        if (!LocalReported.insert(Key).second) return;               // already reported this dir/file collision here
        if (!GlobalSeen.insert(Key + "|" + ToLowerAscii(PrevLbl) + "|" + ToLowerAscii(Lbl)).second) return;
        const bool IsDir = (i + 1 < A.size()) || (i + 1 < B.size());
        Out.push_back(std::string("content case conflict across layers: ") + (IsDir ? "directory '" : "file '")
                      + A[i] + "' (" + PrevLbl + ") vs '" + B[i] + "' (" + Lbl + ") — collide in the merged view");
    };

    std::deque<std::string> Labels;                              // stable storage — Seen holds pointers into it
    for (const std::string &Id : ResolveNodeOrder(Idx, Launch.NodeId, {}))
    {
        const Node *N = Idx.Find(Id);
        if (!N || N->IsRunner() || !N->Layers.is_array()) continue;
        for (const auto &L : N->Layers)
        {
            if (!L.is_object() || !IsVfsLayer(LayerType(L))) continue;
            std::filesystem::path Local; std::string Cid;
            LayerLocator(L, N->BundleDir, Local, Cid);
            if (Local == N->BundleDir) continue;
            std::error_code Ec;
            if (!std::filesystem::exists(Local, Ec)) continue;
            const std::string &Lbl = Labels.emplace_back(N->NodeId + " (" + Local.filename().string() + ")");
            const bool IsDir = std::filesystem::is_directory(Local, Ec);
            for (const PreparedEntry &E : LayerEntriesPrepared(Local.string(), IsDir,
                                                               L.value("TARGET", std::string()), ZipCache, PrepCache))
                Consider(E, Lbl);
        }
    }
}

} // namespace

void ValidateNodeGraph(const NodeIndex &Idx, std::vector<std::string> &Errors, std::vector<std::string> &Warnings,
                       const std::set<std::string> *OnlyNodes)
{
    const std::string Machine = MachinePlatform();
    std::unordered_map<std::string, std::vector<std::string>> ZipCache;   // local zip path -> its file entries (shared content read once)
    std::unordered_map<std::string, std::vector<PreparedEntry>> PrepCache; // (layer path, TARGET) -> prepared case-lint entries (see LayerEntriesPrepared)
    std::unordered_set<std::string> CrossLayerSeen;             // dedup cross-layer case collisions across launchables

    //Does any runner node serve this guest platform on this machine? (used for launchable runner-resolution.)
    auto HasRunnerFor = [&](const std::string &Host) {
        for (const auto &[Id, R] : Idx.Nodes)
            if (R.IsRunner() && R.HostPlatform == Machine)
                for (const auto &G : R.GuestPlatform) if (G == Host) return true;
        return false;
    };

    //A VFS layer whose PATH/SOURCE.PATH carries a %VAR% is RUNTIME-SOURCED (IsRuntimeSourcedLayer): its source is a
    //mount path resolved at launch (e.g. a proton prefix-assembly layer PATH="%DefaultPfxDir%" pointing into the
    //runner mount), NOT local authoring content. Such layers are never seeded to IPFS, so the "convert to STORE zip"
    //warning doesn't apply to them.
    const auto &RuntimeSourced = IsRuntimeSourcedLayer;

    //Cycle-detection memo shared across the whole pass: a node proven acyclic (fully explored with no back-edge)
    //never participates in a cycle, so later roots skip it. Hoisting this out of the per-node loop turns the
    //check from O(N·depth) — every node re-walking its whole ancestor chain, quadratic on a deep delta chain — into
    //O(N+E) total. OnStack stays per-root (a cycle is a back-edge on the current root's path).
    std::unordered_set<std::string> CycleDone;
    for (const auto &[Id, N] : Idx.Nodes)
    {
        if (OnlyNodes && !OnlyNodes->count(Id)) continue;   // scoped validation: skip nodes outside the requested set
        const std::string Tag = "node '" + Id + "'";

        //PARENTS resolve.
        for (const std::string &P : N.Parents)
            if (!Idx.Find(P)) Errors.push_back(Tag + ": PARENTS references missing node '" + P + "'");

        //No cycles reachable from this node (DFS over PARENTS).
        {
            std::unordered_set<std::string> OnStack;
            std::function<bool(const std::string &)> HasCycle = [&](const std::string &Cur) -> bool {
                if (CycleDone.count(Cur)) return false;
                if (OnStack.count(Cur)) return true;
                OnStack.insert(Cur);
                const Node *C = Idx.Find(Cur);
                if (C) for (const std::string &P : C->Parents) if (Idx.Find(P) && HasCycle(P)) return true;
                OnStack.erase(Cur); CycleDone.insert(Cur);
                return false;
            };
            if (HasCycle(Id)) Errors.push_back(Tag + ": PARENTS form a cycle");
        }

        //(The old "a runner node carries its own VFS layer — runner layers are IGNORED" warning is GONE, because the
        //footgun it guarded is now unrepresentable: a node is one layer of one TYPE, so a runner DECLARATION node
        //cannot also carry content. Whether a VFS layer in a runner's closure is the BUILD or prefix ASSEMBLY is
        //decided by IsRuntimeSourcedLayer, not by which node it sits on — so there is no longer a place to put a
        //layer where it would be silently dropped.)

        //VFS layers must declare a local PATH (or SOURCE.PATH).
        if (N.Layers.is_array())
            for (const auto &L : N.Layers)
                if (L.is_object() && IsVfsLayer(L.value("TYPE", std::string())))
                {
                    const bool HasPath = (L.contains("PATH") && L["PATH"].is_string() && !std::string(L["PATH"]).empty())
                        || (L.contains("SOURCE") && L["SOURCE"].is_object() && L["SOURCE"].contains("PATH")
                            && L["SOURCE"]["PATH"].is_string() && !std::string(L["SOURCE"]["PATH"]).empty());
                    const std::string LType = L.value("TYPE", std::string());
                    if (!HasPath) Errors.push_back(Tag + ": a " + (LType.empty() ? "VFS" : LType) + " layer has no PATH");

                    //A VFSDirLayer is an UNZIPPED authoring intermediary: it runs from the editor but cannot be
                    //published (IPFS seeds files/zips only). Warn — not an error — so the node still test-runs; use
                    //the layer's "→ ZIP" button to convert it to a STORE zip before publishing.
                    if (LType == "VFSDirLayer" && !RuntimeSourced(L))
                        Warnings.push_back(Tag + ": VFSDirLayer '" + std::string(L.value("PATH", std::string()))
                            + "' is an unzipped authoring layer — convert it to a STORE zip ('→ ZIP') before publishing "
                            "(dir layers cannot be seeded to IPFS).");

                    //A locally-present zip layer must be STOREd (uncompressed) or it will not mount. Catch it here
                    //(authoring/validate/publish) rather than at launch.
                    if (HasPath && LType == "VFSZipLayer")
                    {
                        const std::string Local = ResolveLayerSource(L, N.BundleDir);
                        std::string FirstCompressed;
                        if (!Local.empty() && std::filesystem::exists(Local) && !ZipFullyStored(Local, &FirstCompressed))
                            Errors.push_back(Tag + ": VFSZipLayer '" + std::string(L.value("PATH", std::string()))
                                + "' is DEFLATE-compressed (entry '" + FirstCompressed + "') — VidyaGodFS requires a "
                                "STORE (uncompressed) zip; it will not mount. Re-create it with `zip -0`.");
                    }
                }

        //BinaryPatch structural lint: a malformed patch does nothing (or worse, patches blindly). Mirror the
        //audit's checks here so the editor/validate flags them before launch.
        if (N.Layers.is_array())
            for (const auto &L : N.Layers)
            {
                if (!L.is_object() || L.value("TYPE", std::string()) != "BinaryPatch") continue;
                const std::string M = L.value("MODE", std::string());
                if (M != "Replace" && M != "Cave" && M != "Poke")
                    Errors.push_back(Tag + ": BinaryPatch MODE '" + M + "' is not Replace/Cave/Poke.");
                if (!L.contains("ANCHOR") && !L.contains("OFFSET"))
                    Errors.push_back(Tag + ": BinaryPatch has neither ANCHOR nor OFFSET (no site to patch).");
                if (!L.contains("EXPECT"))
                    Warnings.push_back(Tag + ": BinaryPatch has no EXPECT guard — it will patch without verifying "
                        "the original bytes (a mispointed FILE or stale offset corrupts silently).");
                if (M == "Replace" && !L.contains("REPLACE"))
                    Errors.push_back(Tag + ": Replace BinaryPatch has no REPLACE bytes.");
                if (M == "Poke" && !L.contains("VALUE"))
                    Errors.push_back(Tag + ": Poke BinaryPatch has no VALUE.");
                if (M == "Cave" && !L.contains("PAYLOAD"))
                    Errors.push_back(Tag + ": Cave BinaryPatch has no PAYLOAD.");
                if (const std::string F = L.value("FILE", std::string());
                    F.rfind("%RuntimePath%", 0) == 0 || (!F.empty() && F[0] == '/'))
                    Errors.push_back(Tag + ": BinaryPatch FILE '" + F + "' is absolute; author it relative to the mount root.");
            }

        //EXCLUDE symmetry.
        for (const std::string &E : N.Exclude)
        {
            const Node *Other = Idx.Find(E);
            if (!Other) { Warnings.push_back(Tag + ": EXCLUDE references missing node '" + E + "'"); continue; }
            if (std::find(Other->Exclude.begin(), Other->Exclude.end(), Id) == Other->Exclude.end())
                Warnings.push_back(Tag + ": EXCLUDE '" + E + "' is not symmetric (the other node does not exclude this one)");
        }

        if (N.IsLaunchable())
        {
            if (N.HostPlatform.empty()) Warnings.push_back(Tag + ": launchable has no PLATFORM.HOST");
            else if (!HasRunnerFor(N.HostPlatform))
                Warnings.push_back(Tag + ": no runner node serves platform '" + N.HostPlatform + "' on this machine");

            //CONTENTPATH must case-EXACTLY match a real content file: the runner sees a case-sensitive mount, so a
            //typo'd case (e.g. MW4mercs.exe vs MW4Mercs.exe) means the exe is never found → silent crash on launch.
            //Only checked where the content is locally present (skips remote/un-hydrated nodes and %var% paths).
            std::string Cp = N.Exec.is_object() ? N.Exec.value("CONTENTPATH", std::string()) : std::string();
            if (!Cp.empty() && Cp.find('%') == std::string::npos)
            {
                std::replace(Cp.begin(), Cp.end(), '\\', '/');
                while (Cp.rfind("./", 0) == 0) Cp.erase(0, 2);
                while (!Cp.empty() && Cp.front() == '/') Cp.erase(Cp.begin());

                std::set<std::string> Files; bool AnyLocal = false, AllLocal = true;
                GatherLaunchContentFiles(Idx, N, ZipCache, Files, AnyLocal, AllLocal);

                if (AnyLocal && !Cp.empty() && !Files.count(Cp))
                {
                    const std::string CpL = ToLowerAscii(Cp);
                    std::string Hit;
                    for (const std::string &F : Files) if (ToLowerAscii(F) == CpL) { Hit = F; break; }
                    if (!Hit.empty())
                        Errors.push_back(Tag + ": CONTENTPATH '" + Cp + "' case-mismatches content file '" + Hit
                                         + "' — the mount is case-sensitive, fix the casing");
                    else if (AllLocal)
                    {
                        //Common authoring slip: a hand-made zip nests the content under a top dir (e.g. "payload/"),
                        //so CONTENTPATH "game.exe" lands at "payload/game.exe". If exactly the same basename exists
                        //nested, suggest the full path rather than just "not found".
                        const std::string Base = Cp.substr(Cp.find_last_of('/') + 1);
                        const std::string BaseL = ToLowerAscii(Base);
                        std::string Suggest; int Matches = 0;
                        for (const std::string &F : Files)
                        {
                            const std::string FB = F.substr(F.find_last_of('/') + 1);
                            if (ToLowerAscii(FB) == BaseL) { Suggest = F; ++Matches; }
                        }
                        if (Matches == 1 && Suggest != Cp)
                            Warnings.push_back(Tag + ": CONTENTPATH '" + Cp + "' not found at the content root — did you "
                                "mean '" + Suggest + "'? (a zip layer that nests its files under a top folder shifts the path)");
                        else
                            Warnings.push_back(Tag + ": CONTENTPATH '" + Cp + "' not found in the package content");
                    }
                }
            }

            //Cross-layer case collisions in the merged content view (two layers' paths differing only in case) are
            //ERRORS: both files exist on the case-sensitive mount and a lookup can hit the wrong one → crashes.
            FindCrossLayerCaseCollisions(Idx, N, ZipCache, PrepCache, Errors, CrossLayerSeen);
        }
        else if (N.IsRunner())
        {
            if (N.GuestPlatform.empty()) Warnings.push_back(Tag + ": runner declares no PLATFORM.GUEST");
            const bool PrefixGen = N.Exec.is_object() && N.Exec.value("PREFIX_GENERATE", false);
            const std::string CR = N.Exec.is_object() ? N.Exec.value("CONTENT_ROOT", std::string()) : std::string();
            if (PrefixGen && CR.find("drive_c") == std::string::npos)
                Warnings.push_back(Tag + ": PREFIX_GENERATE runner's CONTENT_ROOT has no drive_c");
        }
    }

    //----- Declare* identity lint: at most one of each per node; a DeclareExec needs a CONTENTPATH (or it can't run);
    //a DeclareLibraryItem tile needs at least one launchable (its own DeclareExec, or a variant that parents it). -----
    for (const auto &[Id, N] : Idx.Nodes)
    {
        if (OnlyNodes && !OnlyNodes->count(Id)) continue;
        if (!N.Layers.is_array()) continue;
        const std::string Tag = "node '" + Id + "'";
        //The payload never lowered — an unknown TYPE, an unknown FORM, malformed EDITS. This is the rule the
        //spec states as "an unknown TYPE is an error, not something to ignore": a node whose payload nobody
        //applies is a package that quietly does less than it says.
        //LowerError already names the node (NodeLower prefixes every message), so no Tag here — it read
        //"node 'x': node 'x': ...".
        if (!N.LowerError.empty()) Errors.push_back(N.LowerError);
        //A WHEN on a DeclareExec or a DeclareLibraryItem has NO CONSUMER: those payloads are folded into the
        //node's identity at index time and never re-evaluated, so the condition would be silently ignored —
        //a node that looks conditional and is not. TOGGLE is the mechanism for "this node is off"; say so
        //rather than accepting a declaration nothing honours.
        for (const auto &L : N.Layers)
        {
            if (!L.is_object() || !L.contains("WHEN")) continue;
            const std::string LT = L.value("TYPE", std::string());
            if (LT == "DeclareExec" || LT == "DeclareLibraryItem" || LT == "DeclareRunner")
                Errors.push_back(Tag + ": WHEN on a " + LT + " is never evaluated (its payload becomes the "
                                 "node's identity at index time). Use TOGGLE to make the node opt-in.");
        }
        //...and the same for Group, from the other direction: it emits NO layers, so there is nothing for the
        //condition to be stamped on and it evaporates in silence. Group exists to carry PARENTS — "this module
        //IS these parents" — which makes it the most natural place to write a conditional module, and the
        //whole subtree would then apply unconditionally.
        if (N.Layers.is_array() && N.Layers.empty() && !N.RawWhen.empty())
            Errors.push_back(Tag + ": WHEN on a payload-less node is never evaluated (it emits no layer to "
                             "gate). Use TOGGLE to make the node opt-in, or move the condition to a child.");
        int NLib = 0;
        for (const auto &L : N.Layers)
            if (L.is_object() && L.value("TYPE", std::string()) == "DeclareLibraryItem") ++NLib;
        //The "more than one Declare* layer on a node" family of rules is GONE, not forgotten: a node is one
        //layer of one TYPE, so those states are unrepresentable and the checks could never fire. What replaces
        //them are the two rules the flat model makes checkable for the first time — a tile is now its own node,
        //so its UID is one field on one node, and a launchable's tile is now an ANCESTOR, so "which tile?" is a
        //graph question with a possible wrong answer.
        if (NLib)
        {
            //A tile's UID keys saved state, settings, and the content root inside the prefix. Missing, it used
            //to fall back silently at launch — and the user lost their saves to a path that moved.
            if (N.Uid.empty())
                Errors.push_back(Tag + ": DeclareLibraryItem has no UID (it keys saves, settings and the "
                                       "content root — a missing one silently relocates all three)");
        }
        if (NLib && !N.IsLaunchable())   // a tile node: needs a launchable variant somewhere that parents it
        {
            bool HasVariant = false;
            for (const auto &[VId, V] : Idx.Nodes)
                if (V.IsLaunchable() && V.GameKey() == Id) { HasVariant = true; break; }
            if (!HasVariant) Warnings.push_back(Tag + ": DeclareLibraryItem tile has no launchable variant (no DeclareExec node parents it)");
        }
    }

    //----- AMBIGUOUS TILE: a launchable that reaches TWO different DeclareLibraryItem ancestors. --------------
    //LinkGames binds a variant to the NEAREST presentable ancestor, taking the first match in PARENTS order —
    //so a second reachable tile is resolved by array position, which the format says is meaningless (I9). The
    //variant then shows under one game, keyed by that game's UID, with nothing saying the other was dropped.
    //Unrepresentable before the tile became its own node; now a graph question with a wrong answer available.
    {
        //Up to two DISTINCT presentable ancestors per node, memoized. Two is all the rule needs, and it keeps
        //the walk O(N+E) on a 900-deep Minecraft chain instead of materialising an ancestor set per launchable.
        //A TILE is a node that CARRIES a DeclareLibraryItem layer — not one that is `Presentable()`. LinkGames
        //runs inside BuildNodeIndex and copies the tile's Meta onto every variant, so by the time validation
        //runs every linked launchable is Presentable() too. Using that predicate here made the rule skip
        //exactly the nodes it exists to check: it fired in a unit test that built an index by hand (no
        //LinkGames) and never once against the real pipeline.
        auto IsTile = [](const Node &N) {
            if (!N.Layers.is_array()) return false;
            for (const auto &L : N.Layers)
                if (L.is_object() && L.value("TYPE", std::string()) == "DeclareLibraryItem") return true;
            return false;
        };
        std::unordered_map<std::string, std::vector<std::string>> Tiles;
        std::function<const std::vector<std::string> &(const std::string &)> Reach =
            [&](const std::string &Nid) -> const std::vector<std::string> & {
            auto It = Tiles.find(Nid);
            if (It != Tiles.end()) return It->second;
            std::vector<std::string> &Out = Tiles[Nid];          // placeholder first: cycle guard
            const Node *Nn = Idx.Find(Nid);
            if (!Nn) return Out;
            if (IsTile(*Nn)) { Out.push_back(Nid); return Out; }          // a tile terminates the search
            for (const std::string &P : Nn->Parents)
            {
                if (!Idx.Find(P)) continue;
                for (const std::string &T : Reach(P))
                {
                    if (std::find(Out.begin(), Out.end(), T) != Out.end()) continue;
                    Out.push_back(T);
                    if (Out.size() >= 2) return Out;
                }
                if (Out.size() >= 2) break;
            }
            return Out;
        };
        for (const auto &[Id, N] : Idx.Nodes)
        {
            if (OnlyNodes && !OnlyNodes->count(Id)) continue;
            if (!N.IsLaunchable() || IsTile(N)) continue;
            const std::vector<std::string> &T = Reach(Id);
            if (T.size() >= 2)
                Errors.push_back("node '" + Id + "': reaches more than one DeclareLibraryItem ('" + T[0]
                                 + "' and '" + T[1] + "') — which tile it belongs to is decided by PARENTS order, "
                                   "which carries no meaning. Give it exactly one.");
        }
    }

    //----- WHEN lint: a malformed condition fail-opens (silently always-applies), so catch it statically. -----
    //(Undefined %KEY% refs inside a WHEN are already caught by the reference scan below, which dumps every layer.)
    for (const auto &[Id, N] : Idx.Nodes)
        if (N.Layers.is_array())
            for (const auto &L : N.Layers)
                if (L.is_object() && L.contains("WHEN") && L["WHEN"].is_string()
                    && !VarSubst::ConditionParses(std::string(L["WHEN"])))
                    Errors.push_back("node '" + Id + "': malformed WHEN condition \"" + std::string(L["WHEN"])
                                     + "\" — it would silently always-apply. Grammar: %KEY% == value, && || ! ( ).");

    //----- CustomVar lint: undefined %KEY% references (typos) + orphan UI options (dead knobs) -----
    //The built-in tokens always available at substitution time. Derived from ContainerParams::GetVariablesMap() (the
    //single source of truth — a default instance yields every built-in KEY) so the lint stays in sync automatically as
    //runtime vars change instead of drifting from a hand-maintained list. Plus per-context tokens that live outside the
    //global map: %REL% is injected by the guest-path templater (LaunchResolver). A %KEY% that is neither a built-in nor
    //declared by some CustomVar is almost always a typo and would survive as a literal.
    static const std::set<std::string> Builtins = []{
        // Context tokens that are valid built-ins but NOT in a default GetVariablesMap because they are injected at
        // launch AFTER a runtime probe, so they can't be known statically — always legitimate references, named here:
        //  • %REL% — injected by the guest-path templater (LaunchResolver).
        //  • the prefix-layout probe vars — set by ContainerWrapper once the runner build is mounted and its layout
        //    (files/ vs dist/, lib vs lib64, system.reg mtime) is probed; the node-declared proton prefix layers
        //    reference them (see ContainerWrapper::BuildContainerRuntime).
        //  • the VIDYAGOD_* session tokens — injected per launch from the friend LAN / presence layer
        //    (ContainerParams::SessionVars), so they are equally unknowable statically. Without them every
        //    package that legitimately references one is reported as an undefined-variable typo.
        std::set<std::string> B = { "REL", "DefaultPfxDir", "WineFontsDir", "WineLibDir",
                                    "WineSys32Dir", "WineSysWow64Dir", "SysRegMtime",
                                    "VIDYAGOD_SANDBOX", "VIDYAGOD_SANDBOX_NET", "VIDYAGOD_SELF_VIP",
                                    "VIDYAGOD_SELF_NAME", "VIDYAGOD_SUBNET", "VIDYAGOD_PEER_VIPS",
                                    "VIDYAGOD_PEER_NAMES",
                                    "VIDYAGOD_LAN_BRIDGE", "VIDYAGOD_LAN_HOSTRELAY" };
        for (const auto &[K, V] : ContainerParams(std::filesystem::path(), std::string(), std::string()).GetVariablesMap())
            B.insert(K);
        return B;
    }();

    std::set<std::string> Declared;                       // every CustomVar KEY in the graph
    std::map<std::string, std::string> UiDeclared;        // UI-facet option KEY -> its declaring node id
    for (const auto &[Id, N] : Idx.Nodes)
        if (N.Layers.is_array())
            for (const auto &L : N.Layers)
                if (L.is_object() && L.value("TYPE", std::string()) == "CustomVar")
                {
                    const std::string K = L.value("KEY", std::string());
                    if (K.empty()) continue;
                    Declared.insert(K);
                    if (L.contains("UI") && L["UI"].is_object()) UiDeclared.emplace(K, Id);
                }

    const std::regex Tok(R"(%([A-Za-z0-9_]+)(?::[A-Za-z0-9]+)?%)");   // %KEY% or %KEY:format%
    std::set<std::string> ReferencedAll;
    for (const auto &[Id, N] : Idx.Nodes)
    {
        std::string Scan;
        if (N.Layers.is_array()) Scan += N.Layers.dump();
        if (N.Exec.is_object())  Scan += N.Exec.dump();
        std::set<std::string> Refs;
        for (auto It = std::sregex_iterator(Scan.begin(), Scan.end(), Tok); It != std::sregex_iterator(); ++It)
            Refs.insert((*It)[1].str());
        for (const std::string &R : Refs)
        {
            ReferencedAll.insert(R);
            if (!Builtins.count(R) && !Declared.count(R) && (!OnlyNodes || OnlyNodes->count(Id)))
                Errors.push_back("node '" + Id + "': references undefined variable %" + R
                                 + "% — no CustomVar declares it (typo?)");
        }
    }

    //Orphan options (a declared user knob nobody reads) only matter for full-graph validation, not the launch gate.
    if (!OnlyNodes)
        for (const auto &[K, Owner] : UiDeclared)
            if (!ReferencedAll.count(K))
                Warnings.push_back("node '" + Owner + "': option %" + K
                                   + "% has a UI but is referenced nowhere (dead knob)");

    //----- DeclarePersist lint: the unified persistence primitive. Each layer promotes a file path or registry key
    //to a durable TARGET under the instance. PATH "" persists the WHOLE scope (a PersistAll) — legal but discouraged
    //(it drags the instance's own config into the game-writable mount), so it's an authoring warning; nothing should
    //rely on it. An unknown SCOPE is a structural mistake. -----
    for (const auto &[Id, N] : Idx.Nodes)
    {
        if (OnlyNodes && !OnlyNodes->count(Id)) continue;
        if (!N.Layers.is_array()) continue;
        const std::string Tag = "node '" + Id + "'";
        for (const auto &L : N.Layers)
        {
            if (!L.is_object() || L.value("TYPE", std::string()) != "DeclarePersist") continue;
            const std::string Scope = ToLowerAscii(L.value("SCOPE", std::string("file")));
            const std::string Path  = L.value("PATH", std::string());
            if (Scope != "file" && Scope != "registry")
                Warnings.push_back(Tag + ": DeclarePersist SCOPE '" + Scope + "' is not 'file' or 'registry'");
            if (Path.empty())
                Warnings.push_back(Tag + ": a DeclarePersist with an empty PATH persists the ENTIRE " + Scope
                                   + " scope — prefer mapping named TARGETs (nothing should need a catch-all persist)");
            //A file TARGET names a subdir directly under the instance, beside its OWN state — so instance.json /
            //REGISTRY / REGKEYS are off-limits (the resolver refuses them at launch; flag it at authoring time too).
            if (Scope == "file" && L.contains("TARGET") && L["TARGET"].is_string())
            {
                const std::string T = ToLowerAscii(L["TARGET"].get<std::string>());
                if (T == "instance.json" || T == "registry" || T == "regkeys")
                    Warnings.push_back(Tag + ": DeclarePersist TARGET '" + L["TARGET"].get<std::string>()
                                       + "' is RESERVED for the instance's own state — it will be refused at launch");
            }
        }
    }
}


// ----- VFS layer helpers -----

bool ZipFullyStored(const std::string &ZipPath, std::string *FirstCompressed)
{
    int Err = 0; bool AllStored = true;
    if (zip_t *Za = zip_open(ZipPath.c_str(), ZIP_RDONLY, &Err))
    {
        const zip_int64_t N = zip_get_num_entries(Za, 0);
        for (zip_uint64_t i = 0; i < (zip_uint64_t)N; ++i)
        {
            zip_stat_t St; zip_stat_init(&St);
            if (zip_stat_index(Za, i, 0, &St) != 0) continue;
            if ((St.valid & ZIP_STAT_COMP_METHOD) && St.comp_method != ZIP_CM_STORE)
            { AllStored = false; if (FirstCompressed) *FirstCompressed = St.name ? St.name : ""; break; }
        }
        zip_close(Za);
    }
    return AllStored;
}

//The vidyagodfs mount-spec "type" for a package layer TYPE ("zip"/"dir"/"file"/"delta"), or "" if not a VFS layer.
//THE single source of truth for the layer→spec kind: every mount-spec builder (game content, inner-runner nesting,
//runner build, DEFPREFIX gen) maps through here, so none can silently drop a layer kind (a real bug once: the runner
//builders omitted VFSDeltaLayer → delta-chained runners served only their base).
std::string VfsSpecType(const std::string &Type)
{
    return Type == "VFSZipLayer"  ? "zip" : Type == "VFSDirLayer"   ? "dir"
         : Type == "VFSFileLayer" ? "file": Type == "VFSDeltaLayer" ? "delta" : "";
}

void ForEachClosureNode(const NodeIndex &Idx, const std::string &RootId,
                        const std::map<std::string, bool> &Toggles,
                        const std::function<void(const Node &)> &Visit)
{
    for (const std::string &Id : ResolveNodeOrder(Idx, RootId, Toggles))
    {
        if (Id == RootId) continue;
        const Node *N = Idx.Find(Id);
        if (N) Visit(*N);
    }
}

bool IsVfsLayer(const std::string &Type) { return !VfsSpecType(Type).empty(); }

std::string LayerPathString(const nlohmann::ordered_json &Sub)
{
    if (!Sub.is_object()) return {};
    std::string P = Sub.value("PATH", std::string());
    if (Sub.contains("SOURCE") && Sub["SOURCE"].is_object()) P = Sub["SOURCE"].value("PATH", P);
    return P;
}

std::vector<std::string> PathVariableTokens(const std::string &P)
{
    //A %variable% is a MATCHED PAIR around an IDENTIFIER, not merely the presence of a '%'. A filename may
    //legitimately contain one (URL-escaped names are common in scraped content: "100%25%20done.zip" has a
    //matched pair around "25"), and treating that as a variable silently excluded a real file from hydration,
    //verification AND a runner's build.
    std::vector<std::string> Out;
    for (size_t I = P.find('%'); I != std::string::npos; I = P.find('%', I + 1))
    {
        const size_t Close = P.find('%', I + 1);
        if (Close == std::string::npos) break;
        if (Close > I + 1 && P.find('/', I) > Close)
        {
            const std::string Tok = P.substr(I + 1, Close - I - 1);
            if ((std::isalpha((unsigned char)Tok[0]) || Tok[0] == '_')
                && std::all_of(Tok.begin(), Tok.end(),
                               [](unsigned char C) { return std::isalnum(C) || C == '_'; }))
                Out.push_back(Tok);
        }
        I = Close;
    }
    return Out;
}

bool IsRuntimeSourcedLayer(const nlohmann::ordered_json &Sub)
{
    return !PathVariableTokens(LayerPathString(Sub)).empty();
}

bool IsRunnerBuildLayer(const nlohmann::ordered_json &Sub)
{
    return IsVfsLayer(LayerType(Sub)) && !IsRuntimeSourcedLayer(Sub);
}

std::string NormalizeTargetPath(std::string P)
{
    for (char &c : P) if (c == '\\') c = '/';
    //Collapse repeated separators. A TARGET is authored by concatenation — "%PrefixRoot%/drive_c/..." — and
    //%PrefixRoot% is EMPTY for wine-at-root and "pfx" for proton, so composing it routinely yields "//". Left
    //alone, "pfx//drive_c/x" and "pfx/drive_c/x" are two DIFFERENT mount targets naming the same directory, and
    //a layer silently lands somewhere nothing reads.
    std::string Out;
    Out.reserve(P.size());
    for (char c : P)
        if (!(c == '/' && !Out.empty() && Out.back() == '/')) Out.push_back(c);
    P.swap(Out);
    while (!P.empty() && P.front() == '/') P.erase(P.begin());
    while (!P.empty() && P.back()  == '/') P.pop_back();
    return P;
}

std::string LayerType(const nlohmann::ordered_json &Sub)
{
    return Sub.is_object() ? Sub.value("TYPE", std::string()) : std::string();
}

//Build ONE vidyagodfs spec-layer object from a package VFS layer `Sub`, with caller-resolved `Source`/`Target` (and,
//for a delta, its resolved byte-BASES in order). Returns a null json if `Sub` is not a VFS layer (caller skips).
//Centralizes the entry skeleton + the delta base fields so every mount builder stays byte-for-byte consistent. The
//source/target resolution stays at the call site because it genuinely differs (package path vs per-link base prefix
//vs var-subst).
nlohmann::ordered_json MakeVfsSpecLayer(const nlohmann::ordered_json &Sub, const std::string &Source,
                                        const std::string &Target, const std::vector<std::string> &BaseTargets)
{
    const std::string LType = VfsSpecType(Sub.value("TYPE", std::string()));
    if (LType.empty()) return nullptr;
    nlohmann::ordered_json J = {{"type", LType}, {"source", Source}, {"target", Target},
                                {"submounts", Sub.value("SUBMOUNTS", nlohmann::ordered_json::array())}, {"rw", false}};
    //ONE base is the singular key the FS has always read; SEVERAL is the multi-base form it stitches with a
    //ConcatByteSource. ONE key for one base or many: "" is a legitimate target (the VFS root), so a singular
    //key could not distinguish a base DECLARED at the root from no base at all — the FS would silently fall back
    //to the delta's own target, reconstruct against the wrong bytes, and skip the layer.
    //No delta-only check here: whether a layer HAS a byte-base is LayerBaseTargets' single answer, and every
    //caller's vector comes from it. A second gate here would be redundant, and a redundant gate is one that no
    //test can pin — remove either and the suite stays green, which is how a guard quietly stops guarding.
    if (!BaseTargets.empty()) J["baseTargets"] = BaseTargets;
    //RUNTIME-SOURCED: this layer reads from a path that only exists once something ELSE is mounted (a prefix
    //assembly layer under %RunnerMount%). Recorded here because it is the last place that still has the node —
    //by the time a plan is inspected the source is a resolved absolute path indistinguishable from any other.
    //A caller that is not mounting (--audit-packages builds 960 plans) must not report these as missing.
    if (IsRuntimeSourcedLayer(Sub)) J["runtimeSourced"] = true;
    return J;
}

//Does this string still contain an unresolved %TOKEN%? The resolver leaves reference cycles and typos literal,
//and a literal %token% reaching a command line, a mount target or a file path is never what the author meant.
//ONE definition: the mount builder used to ask "is there a '%' in here" and the audit asked this, so a target
//like "save50%" was an error on every launch and clean in the audit, and "%a-b%" was the reverse — two opinions
//about the same question, one per call site, which is the shape of every bug this subsystem keeps producing.
bool HasLiveToken(const std::string &S)
{
    //A SURVIVING '%' is the signal, not a well-formed %IDENT% pair. Requiring the pair shape silently excused
    //the malformed cases — "%PrefixRoot/drive_c/%UID%" (missing close), "%Prefix Root%", "%X-TARGET%" — which
    //are precisely the typos substitution cannot resolve and therefore the ones that reach a real path literally.
    //The pair shape now only decides how the message is PHRASED, not whether there is a problem.
    return S.find('%') != std::string::npos;
}

std::vector<std::string> LayerBaseTargets(const nlohmann::ordered_json &Sub)
{
    std::vector<std::string> Out;
    //Accepts BOTH shapes a delta is written in — the LOWERED layer (TYPE "VFSDeltaLayer") and the raw batched
    //LAYERS ENTRY it came from (FORM "delta", no TYPE key) — because the key and its meaning are identical in
    //both and the editor reads layer entries while the mounter reads lowered layers. Splitting that into two
    //readers is how this question came to be answered differently in four places to begin with.
    if (!Sub.is_object()) return Out;
    const std::string T = LayerType(Sub);
    const bool IsDelta = (T == "VFSDeltaLayer")
                      || (Sub.value("FORM", std::string()) == "delta");   // a raw LAYERS entry carries FORM, no TYPE
    if (!IsDelta) return Out;                                                // only a delta has a byte-base
    //ONE key, always a list. A one-entry list is the ordinary cross-target delta; several are a concatenation.
    //A malformed shape yields NOTHING and says so, rather than a SHORTER list: the bases are concatenated, so a
    //missing entry is a base of the wrong length — the delta fails its size check, the FS drops the layer, and
    //the content is gone. vidyagodfs refuses the same shape for the same reason (layerspec.cpp); a reader that
    //quietly patched it up here would hand the mounter a plan the FS would never have accepted.
    if (!Sub.contains("BASE_TARGETS")) return Out;
    const auto &B = Sub["BASE_TARGETS"];
    const bool WellFormed = B.is_array() && [&]{ for (const auto &E : B) if (!E.is_string()) return false; return true; }();
    if (!WellFormed)
    {
        LogErr("ManifestModel::LayerBaseTargets",
               "Layer " + Sub.value("TYPE", std::string("?")) + " '" + Sub.value("PATH", std::string("?"))
                   + "': BASE_TARGETS must be an array of strings — the delta's byte-base is IGNORED, so it will "
                     "reconstruct against its own target or be skipped entirely.");
        return Out;
    }
    for (const auto &E : B) Out.push_back(E.get<std::string>());
    return Out;
}

void ForEachVfsLayer(const nlohmann::ordered_json &Components,
                     const std::function<void(const nlohmann::ordered_json &)> &Fn)
{
    if (!Components.is_array()) return;
    for (const auto &C : Components)
    {
        if (!C.is_object() || !C.contains("SUBCOMPONENTS") || !C["SUBCOMPONENTS"].is_array()) continue;
        for (const auto &S : C["SUBCOMPONENTS"])
            if (IsVfsLayer(LayerType(S))) Fn(S);
    }
}

void LayerLocator(const nlohmann::ordered_json &Sub, const std::filesystem::path &PackagePath,
                  std::filesystem::path &Local, std::string &Cid)
{
    std::string PathStr = Sub.value("PATH", std::string());
    Cid.clear();
    if (Sub.contains("SOURCE") && Sub["SOURCE"].is_object())
    {
        const auto &Src = Sub["SOURCE"];
        PathStr = Src.value("PATH", PathStr);                                  // SOURCE.PATH overrides the local path
        if (Src.value("TYPE", std::string("path")) == "ipfs") Cid = Src.value("CID", std::string());
    }
    std::filesystem::path P(PathStr);
    Local = P.is_absolute() ? P : (PackagePath / P);
}

std::string ResolveLayerSource(const nlohmann::ordered_json &Sub, const std::filesystem::path &PackagePath)
{
    std::filesystem::path Local; std::string Cid;
    LayerLocator(Sub, PackagePath, Local, Cid);
    return Local.string();
}

// ----- platform -----

// The host platform a runner variant's HOST_PLATFORM must match, and which native content runs directly
// through the appended native terminal (win64 games on Windows, linux64 on Linux).
#ifdef _WIN32
std::string MachinePlatform() { return "win64"; }
std::string HostPlatform()    { return "win64"; }
#else
std::string MachinePlatform() { return "linux64"; }
std::string HostPlatform()    { return "linux64"; }
#endif

// ----- game / component / variant / module lookups -----

int FindGameIndex(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID)
{
    for (int i = 0; i < (int)MANIFESTJSON["GAMES"].size(); i++)
        if (!MANIFESTJSON["GAMES"][i]["GAMEID"].is_null() && MANIFESTJSON["GAMES"][i]["GAMEID"] == SubgameID)
            return i;
    LogErr("ManifestModel::FindGameIndex", "Subgame ID not found: " + SubgameID);
    return -1;
}

int FindComponentIndex(const nlohmann::ordered_json &MANIFESTJSON, const std::string &ComponentID)
{
    if (ComponentID.empty()) return -1;
    for (int i = 0; i < (int)MANIFESTJSON["COMPONENTS"].size(); i++)
        if (!MANIFESTJSON["COMPONENTS"][i]["COMPONENTID"].is_null() && MANIFESTJSON["COMPONENTS"][i]["COMPONENTID"] == ComponentID)
            return i;
    LogErr("ManifestModel::FindComponentIndex", "Component ID not found: " + ComponentID);
    return -1;
}

std::vector<ModuleInfo> ParseModules(const nlohmann::ordered_json &ModulesArray)
{
    std::vector<ModuleInfo> Modules;
    if (!ModulesArray.is_array()) return Modules;
    for (const auto &M : ModulesArray)
    {
        if (!M.is_object()) continue;
        ModuleInfo Info;
        Info.Component = M.value("COMPONENT", std::string());
        if (Info.Component.empty()) continue;
        Info.Label    = M.value("LABEL", std::string());
        Info.Required = M.value("REQUIRED", true);
        Info.Default  = M.value("DEFAULT", true);
        if (M.contains("EXCLUDE") && M["EXCLUDE"].is_array())
            for (const auto &E : M["EXCLUDE"])
                if (E.is_string() && !std::string(E).empty()) Info.Exclude.push_back(std::string(E));
        Modules.push_back(std::move(Info));
    }
    return Modules;
}

std::vector<VariantInfo> GetAvailableVariants(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID)
{
    std::vector<VariantInfo> Variants;
    int SubgameIdx = FindGameIndex(MANIFESTJSON, SubgameID);
    if (SubgameIdx == -1) return Variants;
    auto &Subgame = MANIFESTJSON["GAMES"][SubgameIdx];
    if (!Subgame.contains("VARIANTS") || !Subgame["VARIANTS"].is_array()) return Variants;
    for (auto &V : Subgame["VARIANTS"])
    {
        VariantInfo Info;
        Info.VariantID     = V.value("VARIANT_ID", std::string());
        Info.Name          = V.value("NAME", Info.VariantID);
        Info.IsRecommended = V.value("RECOMMENDED", false);
        Info.HostPlatform  = V.value("HOST_PLATFORM", std::string());
        if (V.contains("GUEST_PLATFORM") && V["GUEST_PLATFORM"].is_array())   // runner variants only
            for (const auto &P : V["GUEST_PLATFORM"]) if (P.is_string()) Info.GuestPlatform.push_back(std::string(P));
        Info.Modules       = ParseModules(V.value("MODULES", nlohmann::ordered_json::array()));
        Variants.push_back(std::move(Info));
    }
    return Variants;
}

std::vector<ModuleInfo> GetVariantModules(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID, const std::string &VariantID)
{
    int SubgameIdx = FindGameIndex(MANIFESTJSON, SubgameID);
    if (SubgameIdx == -1) return {};
    auto &Subgame = MANIFESTJSON["GAMES"][SubgameIdx];
    if (!Subgame.contains("VARIANTS") || !Subgame["VARIANTS"].is_array()) return {};
    for (auto &V : Subgame["VARIANTS"])
        if (V.value("VARIANT_ID", std::string()) == VariantID)
            return ParseModules(V.value("MODULES", nlohmann::ordered_json::array()));
    return {};
}

std::vector<std::string> ResolveEnabledModules(const std::vector<ModuleInfo> &Modules,
                                               const std::map<std::string, bool> &ModuleStates,
                                               const nlohmann::ordered_json &MANIFESTJSON)
{
    std::map<std::string, bool> InModules; // component → is a module here
    for (const auto &M : Modules) InModules[M.Component] = true;

    //All module-ancestors of a component (PARENTCOMPONENT chain entries that are themselves modules).
    auto ModuleAncestors = [&](const std::string &Component) {
        std::vector<std::string> Ancestors;
        int Idx = FindComponentIndex(MANIFESTJSON, Component);
        while (Idx != -1)
        {
            const auto &Comp = MANIFESTJSON["COMPONENTS"][Idx];
            if (!Comp.contains("PARENTCOMPONENT")) break;          // a component may have no parent (root / runner build)
            const auto &ParentField = Comp["PARENTCOMPONENT"];
            if (ParentField.is_null() || ParentField == "") break;
            std::string Parent = ParentField;
            if (InModules.count(Parent)) Ancestors.push_back(Parent);
            Idx = FindComponentIndex(MANIFESTJSON, Parent);
        }
        return Ancestors;
    };

    //Step 1: raw enable.
    std::map<std::string, bool> Enabled;
    for (const auto &M : Modules)
        Enabled[M.Component] = M.Required ? true
                             : (ModuleStates.count(M.Component) ? ModuleStates.at(M.Component) : M.Default);

    //Step 2: a REQUIRED module forces all its module-ancestors enabled (a required child keeps its parents).
    for (const auto &M : Modules)
        if (M.Required)
            for (const auto &A : ModuleAncestors(M.Component)) Enabled[A] = true;

    //Step 2.5: mutual exclusion (EXCLUDE) — symmetric; REQUIRED kept first, then each still-enabled optional in
    //declaration order is dropped if it conflicts with anything kept (first-declared of a set survives).
    std::map<std::string, std::set<std::string>> ExclAdj;
    for (const auto &M : Modules)
        for (const auto &E : M.Exclude) { ExclAdj[M.Component].insert(E); ExclAdj[E].insert(M.Component); }
    if (!ExclAdj.empty())
    {
        std::set<std::string> Kept;
        auto Conflicts = [&](const std::string &C) {
            auto it = ExclAdj.find(C);
            if (it == ExclAdj.end()) return false;
            for (const auto &K : Kept) if (it->second.count(K)) return true;
            return false;
        };
        for (const auto &M : Modules) if (Enabled[M.Component] && M.Required)  Kept.insert(M.Component);
        for (const auto &M : Modules)
        {
            if (!Enabled[M.Component] || M.Required) continue;
            if (Conflicts(M.Component)) Enabled[M.Component] = false;
            else                        Kept.insert(M.Component);
        }
    }

    //Step 3: hierarchy gate — drop any module with a disabled module-ancestor (full chain).
    std::vector<std::string> EnabledComponents;
    for (const auto &M : Modules)
    {
        if (!Enabled[M.Component]) continue;
        bool AncestorsOk = true;
        for (const auto &A : ModuleAncestors(M.Component))
            if (!Enabled[A]) { AncestorsOk = false; break; }
        if (AncestorsOk) EnabledComponents.push_back(M.Component);
    }
    return EnabledComponents;
}

std::vector<std::string> FindEndpointsForVariant(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID, const std::string &VariantID)
{
    return ResolveEnabledModules(GetVariantModules(MANIFESTJSON, SubgameID, VariantID), {}, MANIFESTJSON);
}

// ----- manifest-only package predicates -----

std::vector<std::string> PackageIpfsCids(const nlohmann::ordered_json &Manifest)
{
    std::vector<std::string> Cids;
    std::set<std::string> Seen;
    if (!Manifest.contains("COMPONENTS")) return Cids;
    ForEachVfsLayer(Manifest["COMPONENTS"], [&](const nlohmann::ordered_json &S){
        if (!S.contains("SOURCE") || !S["SOURCE"].is_object()) return;
        const auto &Src = S["SOURCE"];
        if (Src.value("TYPE", std::string()) != "ipfs") return;
        const std::string Cid = Src.value("CID", std::string());
        if (!Cid.empty() && Seen.insert(Cid).second) Cids.push_back(Cid);
    });
    return Cids;
}

std::vector<std::string> PackageCoverCids(const nlohmann::ordered_json &Manifest)
{
    std::vector<std::string> Cids;
    std::set<std::string> Seen;
    if (!Manifest.contains("GAMES") || !Manifest["GAMES"].is_array()) return Cids;
    auto Consider = [&](const nlohmann::ordered_json &Holder)
    {
        if (!Holder.contains("COVER") || !Holder["COVER"].is_object()) return;
        const auto &Cv = Holder["COVER"];
        if (!Cv.contains("SOURCE") || !Cv["SOURCE"].is_object() || Cv["SOURCE"].value("TYPE", std::string()) != "ipfs") return;
        const std::string Cid = Cv["SOURCE"].value("CID", std::string());
        if (!Cid.empty() && Seen.insert(Cid).second) Cids.push_back(Cid);
    };
    for (const auto &G : Manifest["GAMES"])
    {
        if (!G.is_object()) continue;
        if (G.contains("METADATA") && G["METADATA"].is_object()) Consider(G["METADATA"]);
        Consider(G);
    }
    return Cids;
}

bool PackageHydrated(const nlohmann::ordered_json &Manifest, const std::string &PackageDir)
{
    if (!Manifest.contains("COMPONENTS")) return true;
    const std::filesystem::path Pkg(PackageDir);
    bool AllPresent = true;
    ForEachVfsLayer(Manifest["COMPONENTS"], [&](const nlohmann::ordered_json &S){
        if (!AllPresent) return;
        std::filesystem::path Local; std::string Cid;
        LayerLocator(S, Pkg, Local, Cid);
        std::error_code Ec;
        if (!std::filesystem::exists(Local, Ec)) AllPresent = false;            // a layer's content is missing
    });
    return AllPresent;
}

bool PackageHasContent(const nlohmann::ordered_json &Manifest)
{
    if (!Manifest.contains("COMPONENTS")) return false;
    bool Any = false;
    ForEachVfsLayer(Manifest["COMPONENTS"], [&](const nlohmann::ordered_json &){ Any = true; });
    return Any;
}

// ---------------------------------------------------------------------------
// Case-conflict resolution — the fix for FindCrossLayerCaseCollisions errors.
// Two layers in a launchable's closure contribute paths differing only in case (e.g. base 'server.dll' + patch
// 'Server.dll', or base 'CONTENT/' + add-on 'content/'). On the case-sensitive FUSE mount both exist, so the
// higher-priority layer fails to override and wine's lookup can hit the wrong file. We CANONICALIZE: within each
// closure (base-first load order) the first exact case seen for a path component is canonical; any later layer's
// mismatch is renamed to it. Only the higher-priority zips are rewritten (unpack→rename→repackage, STORE, same
// structure); the base is never touched.
// ---------------------------------------------------------------------------

// ALL entries of a zip including explicit directory entries (trailing '/' kept). ZipEntriesCached drops dirs —
// correct for content checks, but the case rewrite must canonicalize dir entries too: a renamed file leaves its
// old-case parent dir entry behind as an EMPTY husk ("maps/" next to "MAPS/…"), and wine's case-insensitive
// lookup can resolve into the husk (Halo CE: "missing bitmaps" — maps/ empty, everything under MAPS/).
static const std::vector<std::string> &ZipAllEntriesRaw(const std::string &Path,
        std::unordered_map<std::string, std::vector<std::string>> &Cache)
{
    auto It = Cache.find(Path);
    if (It != Cache.end()) return It->second;
    std::vector<std::string> Out;
    int Err = 0;
    if (zip_t *Za = zip_open(Path.c_str(), ZIP_RDONLY, &Err))
    {
        const zip_int64_t N = zip_get_num_entries(Za, 0);
        for (zip_uint64_t i = 0; i < (zip_uint64_t)N; ++i)
            if (const char *Name = zip_get_name(Za, i, ZIP_FL_ENC_RAW)) Out.emplace_back(Name);
        zip_close(Za);
    }
    return Cache.emplace(Path, std::move(Out)).first->second;
}

// Component-wise canonicalization across every launchable closure → per-zip {oldEntry -> newEntry}.
static std::map<std::string, std::map<std::string, std::string>>
ComputeCaseRenames(const NodeIndex &Idx, std::vector<std::string> &Log)
{
    std::unordered_map<std::string, std::vector<std::string>> AllCache;
    std::map<std::string, std::map<std::string, std::string>> Renames;   // zipPath -> (old -> new)

    for (const auto &[LId, LN] : Idx.Nodes)
    {
        if (!LN.IsLaunchable()) continue;
        std::map<std::string, std::string> Canon;   // lowercased prefix key -> canonical exact component

        for (const std::string &Id : ResolveNodeOrder(Idx, LN.NodeId, {}))
        {
            const Node *N = Idx.Find(Id);
            if (!N || N->IsRunner() || !N->Layers.is_array()) continue;
            for (const auto &L : N->Layers)
            {
                if (!L.is_object() || !IsVfsLayer(LayerType(L))) continue;
                std::filesystem::path Local; std::string Cid;
                LayerLocator(L, N->BundleDir, Local, Cid);
                std::error_code Ec;
                if (Local == N->BundleDir || !std::filesystem::exists(Local, Ec)
                    || std::filesystem::is_directory(Local, Ec)) continue;   // only rewrite ZIPs (skip dir layers)
                const std::string Target  = L.value("TARGET", std::string());
                const std::string ZipPath = Local.string();

                for (std::string Entry : ZipAllEntriesRaw(ZipPath, AllCache))
                {
                    // Explicit directory entries flow through the SAME component canonicalization (their
                    // trailing '/' stripped for the walk, restored on the rename target) — that is what
                    // retargets a stale-case dir husk onto its canonical spelling instead of leaving it behind.
                    const bool IsDir = !Entry.empty() && Entry.back() == '/';
                    if (IsDir) Entry.pop_back();
                    if (Entry.empty()) continue;
                    const std::string Full = JoinTarget(Target, Entry);
                    std::vector<std::string> Comp = SplitPath(Full);
                    std::string Prefix, NewFull;
                    bool Changed = false;
                    for (size_t k = 0; k < Comp.size(); ++k)
                    {
                        const std::string Key = Prefix.empty() ? ToLowerAscii(Comp[k]) : (Prefix + "/" + ToLowerAscii(Comp[k]));
                        auto It = Canon.find(Key);
                        std::string CanonC;
                        if (It == Canon.end()) { Canon.emplace(Key, Comp[k]); CanonC = Comp[k]; }
                        else CanonC = It->second;
                        if (CanonC != Comp[k]) Changed = true;
                        if (k) NewFull += '/';
                        NewFull += CanonC;
                        Prefix = Key;
                    }
                    if (!Changed) continue;
                    std::string NewEntry = NewFull;
                    if (!Target.empty())
                    {
                        const std::string Pfx = Target + "/";
                        if (NewFull.rfind(Pfx, 0) != 0) continue;   // defensive: Target case must be stable
                        NewEntry = NewFull.substr(Pfx.size());
                    }
                    if (IsDir) { Entry += '/'; NewEntry += '/'; }   // match the zip's dir-entry spelling
                    auto &M = Renames[ZipPath];
                    auto Ex = M.find(Entry);
                    if (Ex != M.end() && Ex->second != NewEntry)
                        Log.push_back("case-fix: conflicting canonicalization of '" + Entry + "' in " + ZipPath + " (skipped)");
                    else
                        M[Entry] = NewEntry;
                }
            }
        }
    }
    return Renames;
}

// Rewrite one zip applying entry renames — libzip copies each entry's data (STORE, no re-compress) into a temp
// archive under the new name, preserving structure/attrs/mtime, then atomically replaces the original.
static bool ApplyZipRenames(const std::string &ZipPath, const std::map<std::string, std::string> &Renames,
                            std::vector<std::string> &Log)
{
    int Err = 0;
    zip_t *Src = zip_open(ZipPath.c_str(), ZIP_RDONLY, &Err);
    if (!Src) { Log.push_back("case-fix: cannot open " + ZipPath); return false; }
    const std::string Tmp = ZipPath + ".casefix.tmp";
    zip_t *Dst = zip_open(Tmp.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &Err);
    if (!Dst) { zip_close(Src); Log.push_back("case-fix: cannot create " + Tmp); return false; }

    const zip_int64_t N = zip_get_num_entries(Src, 0);
    bool Ok = true;
    for (zip_int64_t i = 0; i < N && Ok; ++i)
    {
        const char *Name = zip_get_name(Src, (zip_uint64_t)i, 0);
        if (!Name) { Ok = false; break; }
        std::string Nm = Name;
        auto It = Renames.find(Nm);
        std::string NewNm = (It != Renames.end()) ? It->second : Nm;

        if (!Nm.empty() && Nm.back() == '/')   // explicit directory entry
        {
            std::string D = NewNm; if (!D.empty() && D.back() == '/') D.pop_back();
            // Canonicalization can map several case-variant dir entries onto ONE spelling — the collapse is the
            // point (the husk merges into the real dir), so "already exists" is success, not failure.
            if (zip_dir_add(Dst, D.c_str(), ZIP_FL_ENC_UTF_8) < 0
                && zip_error_code_zip(zip_get_error(Dst)) != ZIP_ER_EXISTS) Ok = false;
            continue;
        }
        zip_source_t *S = zip_source_zip_file(Dst, Src, (zip_uint64_t)i, 0, 0, -1, nullptr);
        if (!S) { Ok = false; break; }
        zip_int64_t Ix = zip_file_add(Dst, NewNm.c_str(), S, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE);
        if (Ix < 0) { zip_source_free(S); Ok = false; break; }
        zip_set_file_compression(Dst, (zip_uint64_t)Ix, ZIP_CM_STORE, 0);   // keep STORE (FUSE needs it)
        zip_uint8_t Ops = 0; zip_uint32_t Attr = 0;
        if (zip_file_get_external_attributes(Src, (zip_uint64_t)i, 0, &Ops, &Attr) == 0)
            zip_file_set_external_attributes(Dst, (zip_uint64_t)Ix, 0, Ops, Attr);
        zip_stat_t St; zip_stat_init(&St);
        if (zip_stat_index(Src, (zip_uint64_t)i, 0, &St) == 0 && (St.valid & ZIP_STAT_MTIME))
            zip_file_set_mtime(Dst, (zip_uint64_t)Ix, St.mtime, 0);
    }

    std::error_code Ec;
    if (!Ok) { zip_discard(Dst); zip_close(Src); std::filesystem::remove(Tmp, Ec); Log.push_back("case-fix: rewrite failed " + ZipPath); return false; }
    if (zip_close(Dst) < 0) { zip_close(Src); std::filesystem::remove(Tmp, Ec); Log.push_back("case-fix: finalize failed " + ZipPath); return false; }
    zip_close(Src);
    std::filesystem::rename(Tmp, ZipPath, Ec);   // atomic replace
    if (Ec) { Log.push_back("case-fix: replace failed " + ZipPath); return false; }
    return true;
}

int FixCaseConflicts(const NodeIndex &Idx, std::vector<std::string> &Log, const std::filesystem::path *ScopeDir)
{
    auto Renames = ComputeCaseRenames(Idx, Log);
    std::string Scope;
    if (ScopeDir) { std::error_code Ec; Scope = std::filesystem::weakly_canonical(*ScopeDir, Ec).string(); if (Ec) Scope = ScopeDir->string(); }
    int Fixed = 0;
    for (const auto &[ZipPath, M] : Renames)
    {
        if (M.empty()) continue;
        if (!Scope.empty() && ZipPath.rfind(Scope, 0) != 0) continue;   // outside the requested bundle → leave it
        Log.push_back("case-fix: " + std::filesystem::path(ZipPath).filename().string()
                      + " — " + std::to_string(M.size()) + (M.size() == 1 ? " entry" : " entries"));
        for (const auto &[O, Nw] : M) Log.push_back("    " + O + "  ->  " + Nw);
        if (ApplyZipRenames(ZipPath, M, Log)) ++Fixed;
    }
    return Fixed;
}

} // namespace ManifestModel
