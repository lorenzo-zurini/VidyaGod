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

// ----- node graph (everything is a node; one edge) -----

const std::set<std::string> &NodeFields()
{
    //The whole top-level vocabulary of a node. A key outside this set is refused at lower (a typo'd payload key —
    //"LAYER" — would otherwise be a payload that silently never applies, the exact failure the lowerer exists to
    //prevent). COMMENT is author prose; POS is the canvas layout (stripped at freeze).
    static const std::set<std::string> F = {
        "CID", "LABEL", "WHEN", "TOGGLE", "PUBLISH", "POS", "COMMENT",
        "TILE", "ENTRYPOINTS", "OVER",
        "LAYERS", "PATCHES", "FILEEDITS", "REGEDITS", "DLLOVERRIDES", "VARS", "PERSISTS",
    };
    return F;
}

const std::vector<std::string> &PayloadKeys()
{
    static const std::vector<std::string> K = { "LAYERS", "PATCHES", "FILEEDITS", "REGEDITS", "DLLOVERRIDES", "VARS", "PERSISTS" };
    return K;
}

bool IsNodeObject(const nlohmann::ordered_json &J)
{
    if (!J.is_object()) return false;
    //A legacy TYPE-bearing object is refused outright, not read as a node: the library is migrated in one pass
    //and never read two ways. Detecting it by its own fields would silently index an un-migrated file as a
    //payload-less node — a package that quietly does less than it says.
    if (J.contains("TYPE")) return false;
    static const char *Marks[] = { "CID", "LABEL", "OVER", "TILE", "ENTRYPOINTS",
                                   "LAYERS", "PATCHES", "FILEEDITS", "REGEDITS", "DLLOVERRIDES", "VARS", "PERSISTS" };
    for (const char *M : Marks) if (J.contains(M)) return true;
    return false;
}

bool ParseOver(const nlohmann::ordered_json &Over, std::vector<OverReq> &Out, std::string *Error)
{
    Out.clear();
    auto Fail = [&](const std::string &Why) { if (Error) *Error = Why; Out.clear(); return false; };
    if (Over.is_null()) return true;
    if (!Over.is_array()) return Fail("OVER must be a list of requirements, not " + std::string(Over.type_name()));
    for (const auto &E : Over)
    {
        OverReq R;
        if (E.is_string())
        {
            if (E.get<std::string>().empty()) return Fail("OVER has an empty ref");
            R.Any.push_back(E.get<std::string>());
        }
        else if (E.is_array())
        {
            //An any-of group. A group of one is just a plain entry; an EMPTY group is unsatisfiable and almost
            //certainly an editing slip — say so rather than silently blocking the node forever.
            if (E.empty()) return Fail("OVER has an empty any-of group (nothing could ever satisfy it)");
            for (const auto &M : E)
            {
                if (!M.is_string() || M.get<std::string>().empty())
                    return Fail("OVER any-of group members must be refs (strings)");
                R.Any.push_back(M.get<std::string>());
            }
        }
        else if (E.is_object())
        {
            //{"NOT": cid} — the one negative form. Anything else object-shaped is unknown vocabulary.
            if (E.size() != 1 || !E.contains("NOT") || !E["NOT"].is_string() || E["NOT"].get<std::string>().empty())
                return Fail("OVER entry objects must be exactly {\"NOT\": ref}");
            R.Not = true;
            R.Any.push_back(E["NOT"].get<std::string>());
        }
        else return Fail("OVER entry must be a ref, an any-of list, or {\"NOT\": ref}, not " + std::string(E.type_name()));
        Out.push_back(std::move(R));
    }
    return true;
}

std::vector<std::string> OverRefs(const nlohmann::ordered_json &J, std::vector<std::string> *Excludes)
{
    std::vector<std::string> Out;
    if (!J.is_object() || !J.contains("OVER")) return Out;
    std::vector<OverReq> Reqs;
    if (!ParseOver(J["OVER"], Reqs)) return Out;
    for (const OverReq &R : Reqs)
    {
        if (R.Not) { if (Excludes) Excludes->push_back(R.Any.front()); continue; }
        for (const std::string &M : R.Any) Out.push_back(M);
    }
    return Out;
}

bool RemapOverRefs(nlohmann::ordered_json &J, const std::function<std::string(const std::string &)> &Map)
{
    if (!J.is_object() || !J.contains("OVER") || !J["OVER"].is_array()) return false;
    bool Changed = false;
    auto One = [&](nlohmann::ordered_json &S) {
        if (!S.is_string()) return;
        const std::string New = Map(S.get<std::string>());
        if (New != S.get<std::string>()) { S = New; Changed = true; }
    };
    for (auto &E : J["OVER"])
    {
        if (E.is_string())      One(E);
        else if (E.is_array())  for (auto &M : E) One(M);
        else if (E.is_object() && E.contains("NOT")) One(E["NOT"]);
    }
    return Changed;
}

static bool ParseNodeOrThrow(const nlohmann::ordered_json &J, const std::filesystem::path &File,
                             const std::filesystem::path &BundleDir, Node &Out);

//THE boundary where untrusted JSON becomes a Node, and therefore the place that must be TOTAL.
//
//NodeLower::Lower already guarantees its OUTPUT is well-typed, which protects every consumer of Node::Layers.
//But ParseNode also reads facets off the RAW node — TOGGLE, LABEL, OVER, TILE, ENTRYPOINTS — and nlohmann's
//.value() throws on a type mismatch. `{"TOGGLE": true}` (a plausible authoring slip) would abort the process
//inside BuildNodeIndex, which runs at STARTUP. So the whole derivation is guarded and a throw becomes the same
//refusal a malformed payload produces: the node is indexed, named by validation, and never routed through.
bool ParseNode(const nlohmann::ordered_json &J, const std::filesystem::path &File,
               const std::filesystem::path &BundleDir, Node &Out)
{
    try
    {
        return ParseNodeOrThrow(J, File, BundleDir, Out);
    }
    catch (const std::exception &E)
    {
        if (!IsNodeObject(J)) return false;
        const std::string Id = (J.contains("LABEL") && J["LABEL"].is_string()) ? J["LABEL"].get<std::string>() : std::string();
        LogWarn("ManifestModel::ParseNode",
                "Malformed node '" + Id + "' — " + E.what() + " (" + File.string() + ")");
        Out = Node{};
        Out.NodeId     = Id;
        Out.Cid        = (J.contains("CID") && J["CID"].is_string()) ? J["CID"].get<std::string>() : std::string();
        Out.LowerError = "node '" + Id + "': malformed field (" + E.what() + ")";
        Out.Layers     = nlohmann::ordered_json::array();
        Out.Entrypoints = nlohmann::ordered_json::array();
        Out.Parents    = OverRefs(J, &Out.Excludes);
        Out.File      = File;
        Out.BundleDir = BundleDir;
        return true;
    }
}

//The default entrypoint of an ENTRYPOINTS array: the first RECOMMENDED one, else the first. -1 if none.
static int DefaultEntrypointIndex(const nlohmann::ordered_json &Eps)
{
    if (!Eps.is_array() || Eps.empty()) return -1;
    for (size_t i = 0; i < Eps.size(); ++i)
        if (Eps[i].is_object() && Eps[i].value("RECOMMENDED", false)) return (int)i;
    return 0;
}

static bool EntryIsRunner(const nlohmann::ordered_json &E)
{
    return E.is_object() && E.contains("GUEST") && E["GUEST"].is_array() && !E["GUEST"].empty();
}

//Apply one lowered entrypoint's view onto the node's launch fields.
static void ApplyEntrypoint(Node &N, const nlohmann::ordered_json &Entry, const nlohmann::ordered_json &Exec)
{
    N.Exec = Exec;
    if (EntryIsRunner(Entry))
    {
        N.HostPlatform = Entry.value("HOST", std::string());
        N.GuestPlatform.clear();
        for (const auto &G : Entry["GUEST"]) if (G.is_string()) N.GuestPlatform.push_back(G.get<std::string>());
    }
    else
    {
        N.HostPlatform = Entry.value("HOST", std::string());
        N.GuestPlatform.clear();
    }
    N.Label            = Entry.value("LABEL", N.NodeId);
    N.Recommended      = Entry.value("RECOMMENDED", false);
    N.RecommendedRunner= Entry.value("RUNNER", std::string());
}

static bool ParseNodeOrThrow(const nlohmann::ordered_json &J, const std::filesystem::path &File,
                             const std::filesystem::path &BundleDir, Node &Out)
{
    if (!IsNodeObject(J)) return false;
    Out = Node{};
    // Identity/wiring is the CID. A working-tree node stores its last-minted CID in "CID" (its authoring handle,
    // which OVER refs name) — read it into Cid so Key() returns it. LABEL is the OPTIONAL cosmetic name, kept in
    // NodeId as a display fallback (and passed to the lowerer for error/layer naming). A FROZEN block has no "CID"
    // field (a block can't contain its own hash); its caller (FreezeToIndex) sets Cid to the DERIVED hash.
    Out.Cid      = (J.contains("CID") && J["CID"].is_string()) ? J["CID"].get<std::string>() : std::string();
    Out.NodeId   = (J.contains("LABEL") && J["LABEL"].is_string()) ? J["LABEL"].get<std::string>() : std::string();
    Out.File      = File;
    Out.BundleDir = BundleDir;
    Out.Entrypoints = nlohmann::ordered_json::array();
    Out.Layers = nlohmann::ordered_json::array();
    Out.Publish = J.contains("PUBLISH") && J["PUBLISH"].is_boolean() && J["PUBLISH"].get<bool>();
    Out.RawWhen = (J.contains("WHEN") && J["WHEN"].is_string()) ? J["WHEN"].get<std::string>() : std::string();

    //A refusal keeps the node INDEXED with the reason attached and no layers: validation names it, and
    //ResolveNodeOrder refuses to route through it. Dropping it removed the node from the graph entirely, so a
    //referrer dangled (loud) but a LEAF mistake was reported by nothing at all. Its edges are still read so a
    //scoped validate (`--validate-nodes <bundle>`) can reach it.
    auto Refuse = [&](const std::string &Why) {
        LogWarn("ManifestModel::ParseNode", "Malformed node — " + Why + " (" + File.string() + ")");
        Out.LowerError = Why;
        Out.Layers = nlohmann::ordered_json::array();
        Out.Parents = OverRefs(J, &Out.Excludes);
        return true;
    };

    //--- the edge ---
    std::string OverErr;
    if (J.contains("OVER") && !ParseOver(J["OVER"], Out.Over, &OverErr))
        return Refuse("node '" + Out.NodeId + "': " + OverErr);
    for (const OverReq &R : Out.Over)
    {
        if (R.Not) Out.Excludes.push_back(R.Any.front());
        else for (const std::string &M : R.Any) Out.Parents.push_back(M);
    }

    //--- the payload: every present payload array lowers into the executor's flat layer stream ---
    std::string LowerErr;
    Out.Layers = NodeLower::Lower(J, Out.NodeId, LowerErr);
    if (!LowerErr.empty()) return Refuse(LowerErr);

    //--- TOGGLE: present ⇒ user-toggleable, and the value IS the initial state. An UNKNOWN value is refused, not
    //guessed: `Out.Default = (Toggle != "off")` would make every typo — "of", "Off", "false" — read as ON. ---
    const std::string Toggle = J.value("TOGGLE", std::string());
    if (!Toggle.empty() && Toggle != "on" && Toggle != "off")
        return Refuse("node '" + Out.NodeId + "': TOGGLE must be \"on\" or \"off\", not \"" + Toggle + "\"");
    Out.Optional = !Toggle.empty();
    Out.Default  = (Toggle != "off");

    //--- TILE: identity of a title. UID keys saves/settings/the content root; missing, it used to fall back
    //silently at launch — and the user lost their saves to a path that moved. ---
    if (J.contains("TILE"))
    {
        const auto &T = J["TILE"];
        if (!T.is_object()) return Refuse("node '" + Out.NodeId + "': TILE must be an object");
        if (!T.contains("UID") || !T["UID"].is_string() || T["UID"].get<std::string>().empty())
            return Refuse("node '" + Out.NodeId + "': TILE has no UID (it keys saves, settings and the content root)");
        if (T.contains("PARENTUID") && !T["PARENTUID"].is_string())
            return Refuse("node '" + Out.NodeId + "': TILE.PARENTUID must be a string");
        if (T.contains("TITLE") && !T["TITLE"].is_string())
            return Refuse("node '" + Out.NodeId + "': TILE.TITLE must be a string");
        //Descriptive catalog fields live in an opaque META bag on disk (the engine names only a handful of
        //them); the tile consumers read them flat, so they re-join here.
        nlohmann::ordered_json Meta = nlohmann::ordered_json::object();
        for (const auto &[K, V] : T.items())
        {
            if (K == "META") continue;
            Meta[K] = V;
        }
        if (T.contains("META"))
        {
            if (!T["META"].is_object()) return Refuse("node '" + Out.NodeId + "': TILE.META must be an object");
            for (const auto &[K, V] : T["META"].items()) Meta[K] = V;
        }
        Out.OwnTile   = true;
        Out.Uid       = T["UID"].get<std::string>();
        Out.Uids      = { Out.Uid };
        Out.ParentUid = T.value("PARENTUID", std::string());
        Out.Meta      = std::move(Meta);
    }

    //--- ENTRYPOINTS: launchable iff present. Each entry lowers to the exec block the engine consumes; the
    //node's launch fields are the DEFAULT entry's view. ---
    if (J.contains("ENTRYPOINTS"))
    {
        const auto &Eps = J["ENTRYPOINTS"];
        if (!Eps.is_array()) return Refuse("node '" + Out.NodeId + "': ENTRYPOINTS must be a list");
        if (Eps.empty()) return Refuse("node '" + Out.NodeId + "': ENTRYPOINTS is empty (a launchable with nothing to run — remove it or add an entry)");
        for (const auto &E : Eps)
        {
            std::string EpErr;
            const nlohmann::ordered_json Ex = NodeLower::LowerEntrypoint(E, Out.NodeId, EpErr);
            if (!EpErr.empty()) return Refuse(EpErr);
            if (EntryIsRunner(E)) Out.HasRunner = true; else Out.HasExec = true;
        }
        Out.Entrypoints = Eps;
        const int D = DefaultEntrypointIndex(Eps);
        std::string EpErr;
        ApplyEntrypoint(Out, Eps[D], NodeLower::LowerEntrypoint(Eps[D], Out.NodeId, EpErr));
    }
    return true;
}

} // namespace ManifestModel

nlohmann::ordered_json Node::ExecFor(const std::string &Lbl) const
{
    if (!Entrypoints.is_array() || Entrypoints.empty()) return nlohmann::ordered_json();
    if (Lbl.empty()) return Exec;
    const std::vector<std::string> Labels = EntrypointLabels();
    for (size_t i = 0; i < Labels.size(); ++i)
        if (Labels[i] == Lbl)
        {
            std::string Err;
            return NodeLower::LowerEntrypoint(Entrypoints[i], NodeId, Err);
        }
    return nlohmann::ordered_json();
}

std::vector<std::string> Node::EntrypointLabels() const
{
    std::vector<std::string> Out;
    if (!Entrypoints.is_array()) return Out;
    for (size_t i = 0; i < Entrypoints.size(); ++i)
    {
        const auto &E = Entrypoints[i];
        const std::string L = E.is_object() ? E.value("LABEL", std::string()) : std::string();
        Out.push_back(L.empty() ? std::to_string(i) : L);
    }
    return Out;
}

namespace ManifestModel {

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
        //it carries no semantics.
        nlohmann::ordered_json Single;                                    // only materialised for the 1-node form
        if (!J.is_array()) Single = nlohmann::ordered_json::array({J});   // (both operands lvalues ⇒ no copy of J)
        const nlohmann::ordered_json &Nodes = J.is_array() ? J : Single;
        for (const auto &Doc : Nodes)
        {
            if (Doc.is_object() && Doc.contains("TYPE"))
                LogWarn("ManifestModel::ScanBundleNodes", "Legacy TYPE node ignored (run tools/migrate_one_edge.py): " + Entry.path().string());
            Node N;
            if (!ParseNode(Doc, Entry.path(), BundleDir, N)) continue;    // not a node
            // Key by the node's HANDLE — its stored CID (Key() = Cid). A node with no stored CID (a never-minted
            // draft caught mid-author, or a frozen block with no "CID" field) gets a synthetic per-node key so
            // handle-less nodes never fold together. Refs (OVER) are CIDs, so they resolve against these keys.
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
    DeriveIdentity(Idx);
    LogOut("ManifestModel::BuildNodeIndex", "Indexed " + std::to_string(Idx.Nodes.size()) + " node(s) across "
           + std::to_string(LibraryRoots.size()) + " root(s).");
    return Idx;
}

void DeriveIdentity(NodeIndex &Idx)
{
    // Identity follows the edge: a node with its own TILE IS its identity; every other node belongs to whatever it
    // is OVER — the union of its positive requirements' identities, in OVER order (a mod for two games has both; an
    // any-of group contributes every member's). Memoized in ONE pass (O(N+E)) — a per-node closure walk is O(N²)
    // on a 900-deep Minecraft chain. Meta is the first identity's tile fields (own, else the first inherited tile's),
    // so the picker can show a graft under its game's title/cover.
    struct G { std::vector<std::string> Uids; nlohmann::ordered_json Meta; std::string ParentUid; bool Done = false; };
    std::unordered_map<std::string, G> Memo;
    std::function<const G &(const std::string &)> Of = [&](const std::string &Id) -> const G & {
        auto It = Memo.find(Id);
        if (It != Memo.end()) return It->second;
        G &g = Memo[Id];                                                     // placeholder first: cycle guard
        const Node *N = Idx.Find(Id);
        if (!N) return g;
        if (N->OwnTile)
        {
            g.Uids = { N->Uid }; g.Meta = N->Meta; g.ParentUid = N->ParentUid; g.Done = true;
            return g;
        }
        for (const std::string &P : N->Parents)
        {
            if (!Idx.Find(P)) continue;
            const G &pg = Of(P);
            for (const std::string &U : pg.Uids)
                if (std::find(g.Uids.begin(), g.Uids.end(), U) == g.Uids.end()) g.Uids.push_back(U);
            if (g.Meta.empty() && !pg.Meta.empty()) { g.Meta = pg.Meta; g.ParentUid = pg.ParentUid; }
        }
        g.Done = true;
        return g;
    };
    for (auto &[Id, N] : Idx.Nodes)
    {
        if (N.OwnTile) continue;
        const G &g = Of(Id);
        N.Uids      = g.Uids;
        N.Uid       = g.Uids.empty() ? std::string() : g.Uids.front();
        N.Meta      = g.Meta;
        N.ParentUid = g.ParentUid;
    }
}

//The one place the CNF is evaluated for CLOSURE building. Returns the members of a requirement that are kept,
//choosing for a group: an already-kept member wins; else the first member present in the index (a group with
//nothing selected on a launchable is a choice the instance has not made yet — the CLI/audit takes the first so
//resolution stays deterministic; the picker offers the choice).
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

    //Phase 1 — determine the ENABLED set by a breadth-first walk over OVER from the launch node, applying each
    //node's own toggle/NOT gate. A plain requirement is always pulled; a TOGGLE'd one only if its toggle (else
    //DEFAULT) is on and no kept node excludes it. We only descend INTO a node we keep, so a toggled node's unique
    //ancestors are naturally dropped with it (the hierarchy gate).
    std::unordered_set<std::string> Enabled;
    std::unordered_set<std::string> ExcludedByKept;                        // ∪ of NOT targets of every kept node
    auto ToggleOf = [&](const std::string &Key, const Node *N) {
        auto It = Toggles.find(Key);
        if (It != Toggles.end()) return It->second;
        //Toggles are keyed by Key() (CID); a label-keyed entry (a CLI --module by LABEL) resolves too.
        if (N && !N->NodeId.empty()) { It = Toggles.find(N->NodeId); if (It != Toggles.end()) return It->second; }
        return N ? N->Default : true;
    };
    auto Keep = [&](const std::string &Id, const Node *N) {
        Enabled.insert(Id);
        if (N) for (const auto &E : N->Excludes) ExcludedByKept.insert(E);
    };
    //A candidate conflicts if it excludes an already-kept node, OR an already-kept node excludes it.
    auto Conflicts = [&](const std::string &Id, const Node &N) {
        for (const auto &E : N.Excludes) if (Enabled.count(E)) return true;
        return ExcludedByKept.count(Id) != 0;
    };
    std::vector<std::string> Frontier = { LaunchNodeId };
    size_t FrontierHead = 0;
    Keep(LaunchNodeId, Launch);
    auto ExplicitOn = [&](const std::string &Id) {
        auto It = Toggles.find(Id);
        if (It != Toggles.end() && It->second) return true;
        const Node *P = Idx.Find(Id);
        if (P && !P->NodeId.empty()) { It = Toggles.find(P->NodeId); return It != Toggles.end() && It->second; }
        return false;
    };
    //Any-of groups are DEFERRED: a group is a choice, and a member already kept through any other route (a plain
    //requirement elsewhere in the closure) must satisfy it — so plain requirements are walked to exhaustion
    //first, then each pending group is resolved against the kept set, and the walk resumes for what the pick
    //pulled in. Repeats until nothing is pending.
    std::vector<const OverReq *> PendingGroups;
    auto Consider = [&](const std::string &Pid) {
        if (Enabled.count(Pid)) return;
        const Node *P = Idx.Find(Pid);
        if (!P || !P->LowerError.empty()) { if (Missing) Missing->push_back(Pid); return; }
        if (P->Optional && !ToggleOf(Pid, P)) return;
        if (Conflicts(Pid, *P)) return;
        Keep(Pid, P); Frontier.push_back(Pid);
    };
    for (;;)
    {
        while (FrontierHead < Frontier.size())
        {
            const std::string Cur = Frontier[FrontierHead++];
            const Node *N = Idx.Find(Cur);
            if (!N) continue;
            //Plain candidates in OVER order, with the explicitly-toggled-on ones first so an explicit choice is
            //kept before a conflicting DEFAULT-on sibling (first-kept wins the NOT). Otherwise toggling a
            //mutually-exclusive option on would be silently dropped in favour of the other option's default.
            std::vector<std::string> Cands;
            for (const OverReq &R : N->Over)
            {
                if (R.Not) continue;
                if (R.IsGroup()) { PendingGroups.push_back(&R); continue; }
                Cands.push_back(R.Any.front());
            }
            std::stable_sort(Cands.begin(), Cands.end(),
                             [&](const std::string &A, const std::string &B) { return ExplicitOn(A) && !ExplicitOn(B); });
            for (const std::string &Pid : Cands) Consider(Pid);
        }
        if (PendingGroups.empty()) break;
        //Resolve ONE pending group per round: an already-kept member satisfies it; else an explicitly toggled-on
        //member; else the first member the index has. Then walk what that pulled in before the next group, so a
        //later group can be satisfied by an earlier pick.
        const OverReq *R = PendingGroups.front();
        PendingGroups.erase(PendingGroups.begin());
        std::string Pick;
        for (const std::string &M : R->Any) if (Enabled.count(M)) { Pick = M; break; }
        if (Pick.empty()) for (const std::string &M : R->Any) if (ExplicitOn(M) && Idx.Find(M)) { Pick = M; break; }
        if (Pick.empty()) for (const std::string &M : R->Any) if (Idx.Find(M)) { Pick = M; break; }
        if (Pick.empty()) { if (Missing) Missing->push_back(R->Any.front()); continue; }
        Consider(Pick);
    }

    //Phase 2 — topo-order the enabled subgraph: emit a node only after all its enabled requirements (post-order
    //DFS following OVER in list order). The launchable, having no enabled dependants, is emitted last = highest
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

//Everything reachable from `Start` through positive OVER refs (the launchable's own composition, toggles
//ignored): a node in there is part of the launchable, never a graft on it.
static std::unordered_set<std::string> Reachable(const NodeIndex &Idx, const std::string &Start)
{
    std::unordered_set<std::string> Seen;
    std::vector<std::string> Stack{Start};
    while (!Stack.empty())
    {
        const std::string Cur = Stack.back(); Stack.pop_back();
        if (!Seen.insert(Cur).second) continue;
        const Node *N = Idx.Find(Cur);
        if (N) for (const std::string &P : N->Parents) Stack.push_back(P);
    }
    return Seen;
}

std::vector<GraftOffer> OfferedGrafts(const NodeIndex &Idx, const std::string &LaunchNodeId,
                                      const std::map<std::string, bool> &Toggles,
                                      const std::function<bool(const Node &)> &Scope)
{
    std::vector<GraftOffer> Out;
    const Node *Launch = Idx.Find(LaunchNodeId);
    if (!Launch || Launch->Uids.empty()) return Out;
    const std::string LaunchKey = Launch->Key();
    const std::unordered_set<std::string> Own = Reachable(Idx, LaunchKey);

    //Candidates: every node with the launchable's identity that is OVER something and is NOT part of the
    //launchable's own composition — a branch off somewhere in this title (a graft, or a graft on a graft) —
    //excluding launchables (those are VARIANTS, picked from the tile, never ticked). Deterministic order: by
    //label, then key.
    std::vector<const Node *> Cands;
    for (const auto &[Id, N] : Idx.Nodes)
    {
        if (N.Parents.empty() || Own.count(Id)) continue;
        if (N.IsLaunchable() || N.IsRunner() || !N.LowerError.empty()) continue;
        bool SameTitle = false;
        for (const std::string &U : N.Uids) for (const std::string &LU : Launch->Uids) if (U == LU) { SameTitle = true; break; }
        if (!SameTitle) continue;
        if (Scope && !Scope(N)) continue;
        Cands.push_back(&N);
    }
    std::sort(Cands.begin(), Cands.end(), [](const Node *A, const Node *B) {
        if (A->NodeId != B->NodeId) return A->NodeId < B->NodeId;
        return A->Key() < B->Key();
    });

    auto SelectedByUser = [&](const Node &N) {
        auto It = Toggles.find(N.Key());
        if (It != Toggles.end()) return It->second;
        if (!N.NodeId.empty()) { It = Toggles.find(N.NodeId); if (It != Toggles.end()) return It->second; }
        return N.Optional && N.Default;                        // TOGGLE "on" = the author's default: pre-ticked
    };

    //Fixpoint: the selected set = the launchable + every ticked graft that is applicable against the set so far.
    //Ticking a graft can make another applicable (tex → tex-hd), so iterate until nothing changes.
    std::unordered_set<std::string> Selected{ LaunchKey };
    std::unordered_map<const Node *, std::string> Blocker;
    auto Applicable = [&](const Node &G, std::string &Why) {
        for (const OverReq &R : G.Over)
        {
            if (R.Not)
            {
                if (Selected.count(R.Any.front())) { Why = R.Any.front(); return false; }
                continue;
            }
            bool Ok = false;
            for (const std::string &M : R.Any)
            {
                if (Selected.count(M)) { Ok = true; break; }
                const Node *Mn = Idx.Find(M);
                if (Mn && !Mn->HasIdentity()) { Ok = true; break; }        // substance: satisfied by mounting
            }
            if (!Ok) { Why = R.Any.front(); return false; }
        }
        return true;
    };
    for (bool Changed = true; Changed;)
    {
        Changed = false;
        for (const Node *G : Cands)
        {
            if (Selected.count(G->Key()) || !SelectedByUser(*G)) continue;
            std::string Why;
            if (Applicable(*G, Why)) { Selected.insert(G->Key()); Changed = true; }
        }
    }
    for (const Node *G : Cands)
    {
        GraftOffer O;
        O.Graft = G;
        O.Applicable = Applicable(*G, O.Blocker);
        O.Selected = O.Applicable && Selected.count(G->Key()) != 0;
        Out.push_back(std::move(O));
    }
    return Out;
}

std::vector<std::string> ResolveGraftOrder(const NodeIndex &Idx, const std::string &LaunchNodeId,
                                           const std::map<std::string, bool> &Toggles,
                                           const std::vector<std::string> &BaseOrder,
                                           const std::map<std::string, int> &Precedence,
                                           const std::function<bool(const Node &)> &Scope)
{
    std::vector<std::string> Out;
    std::vector<GraftOffer> Offers = OfferedGrafts(Idx, LaunchNodeId, Toggles, Scope);
    std::vector<const Node *> Active;
    for (const GraftOffer &O : Offers) if (O.Selected) Active.push_back(O.Graft);
    if (Active.empty()) return Out;
    //Precedence: higher = mounted later = wins at a conflict; ties by key so an untouched instance is reproducible.
    auto PrecOf = [&](const Node *N) {
        auto It = Precedence.find(N->Key());
        if (It != Precedence.end()) return It->second;
        if (!N->NodeId.empty()) { It = Precedence.find(N->NodeId); if (It != Precedence.end()) return It->second; }
        return 0;
    };
    std::stable_sort(Active.begin(), Active.end(), [&](const Node *A, const Node *B) {
        const int Pa = PrecOf(A), Pb = PrecOf(B);
        if (Pa != Pb) return Pa < Pb;
        return A->Key() < B->Key();
    });
    //Each graft's own closure (its substance requirements, its private ancestors) topo-ordered beneath it; nodes
    //already in the base mount, or already emitted by an earlier graft, are not repeated. Its identity-bearing
    //requirements are SELECTED by construction (that is what made it applicable) — they are in BaseOrder or among
    //the grafts — so descending into a plain requirement never pulls a second copy of the game.
    std::unordered_set<std::string> Seen(BaseOrder.begin(), BaseOrder.end());
    //The launch node is in BaseOrder under the id the CALLER passed (a LABEL from the CLI resolves through
    //Find), while a graft's closure names it by its Key(): both spellings are the same node, already mounted.
    if (const Node *L = Idx.Find(LaunchNodeId)) { Seen.insert(L->Key()); Seen.insert(LaunchNodeId); }
    std::unordered_set<std::string> ActiveKeys;
    for (const Node *G : Active) ActiveKeys.insert(G->Key());
    for (const Node *G : Active)
    {
        std::vector<std::string> Missing;
        //ResolveNodeOrder over the graft's closure with the same toggles: for a group, an already-selected member
        //(one in Seen) is preferred — mirror that by passing the selected keys as toggles-on.
        std::map<std::string, bool> T = Toggles;
        for (const std::string &S : Seen) T[S] = true;
        for (const std::string &Id : ResolveNodeOrder(Idx, G->Key(), T, &Missing))
        {
            if (Seen.count(Id)) continue;
            //A node OUTSIDE this graft's substance that the closure reached through a plain requirement (a
            //selected game/graft not yet mounted — cannot happen once BaseOrder holds the game) is emitted here
            //anyway: the mount must be complete before the graft applies.
            Seen.insert(Id);
            Out.push_back(Id);
        }
        for (const auto &M : Missing) LogWarn("ManifestModel::ResolveGraftOrder", "graft '" + G->NodeId + "': unresolved requirement " + M);
    }
    return Out;
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

        //OVER resolves: every positive ref must exist; a NOT naming a node we do not have is harmless (the excluded
        //node cannot be selected) but worth a word.
        for (const std::string &P : N.Parents)
            if (!Idx.Find(P)) Errors.push_back(Tag + ": OVER references missing node '" + P + "'");
        for (const std::string &E : N.Excludes)
            if (!Idx.Find(E)) Warnings.push_back(Tag + ": OVER NOT references missing node '" + E + "'");
        for (const OverReq &R : N.Over)
        {
            if (!R.IsGroup()) continue;
            bool AnyPresent = false;
            for (const std::string &M : R.Any) if (Idx.Find(M)) { AnyPresent = true; break; }
            if (!AnyPresent) Errors.push_back(Tag + ": OVER any-of group has no member in the library (unsatisfiable)");
            std::set<std::string> Seen;
            for (const std::string &M : R.Any) if (!Seen.insert(M).second) Warnings.push_back(Tag + ": OVER any-of group repeats '" + M + "'");
        }
        for (const std::string &E : N.Excludes)
            if (std::find(N.Parents.begin(), N.Parents.end(), E) != N.Parents.end())
                Errors.push_back(Tag + ": OVER both requires and excludes '" + E + "'");

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
            if (HasCycle(Id)) Errors.push_back(Tag + ": OVER forms a cycle");
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

        //(A NOT is one-sided by design and symmetric in EFFECT: selecting the named node makes this one unsatisfied.
        //There is no symmetry rule to lint.)

        if (N.IsLaunchable())
        {
            //Every entrypoint is a variant the user can pick, so every one is checked — not just the default.
            std::set<std::string> EpLabels;
            for (const std::string &L : N.EntrypointLabels())
                if (!EpLabels.insert(L).second) Errors.push_back(Tag + ": two ENTRYPOINTS share the LABEL '" + L + "' (the picker could not tell them apart)");
            for (const auto &E : N.Entrypoints)
            {
            std::string EpErr;
            const nlohmann::ordered_json Ex = NodeLower::LowerEntrypoint(E, N.NodeId, EpErr);
            if (!Ex.is_object() || Ex.contains("GUEST")) continue;   // a runner entry is checked below
            const std::string Host = Ex.value("PLATFORM", std::string());
            if (Host.empty()) Warnings.push_back(Tag + ": entrypoint has no HOST platform");
            else if (!HasRunnerFor(Host))
                Warnings.push_back(Tag + ": no runner node serves platform '" + Host + "' on this machine");

            //CONTENTPATH must case-EXACTLY match a real content file: the runner sees a case-sensitive mount, so a
            //typo'd case (e.g. MW4mercs.exe vs MW4Mercs.exe) means the exe is never found → silent crash on launch.
            //Only checked where the content is locally present (skips remote/un-hydrated nodes and %var% paths).
            std::string Cp = Ex.value("CONTENTPATH", std::string());
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
            }

            //Cross-layer case collisions in the merged content view (two layers' paths differing only in case) are
            //ERRORS: both files exist on the case-sensitive mount and a lookup can hit the wrong one → crashes.
            FindCrossLayerCaseCollisions(Idx, N, ZipCache, PrepCache, Errors, CrossLayerSeen);
        }
        if (N.IsRunner())
        {
            if (N.GuestPlatform.empty()) Warnings.push_back(Tag + ": runner declares no PLATFORM.GUEST");
            const bool PrefixGen = N.Exec.is_object() && N.Exec.value("PREFIX_GENERATE", false);
            const std::string CR = N.Exec.is_object() ? N.Exec.value("CONTENT_ROOT", std::string()) : std::string();
            if (PrefixGen && CR.find("drive_c") == std::string::npos)
                Warnings.push_back(Tag + ": PREFIX_GENERATE runner's CONTENT_ROOT has no drive_c");
        }
        //A launchable with NO identity of its own and none inherited is a game nobody can find: it appears under
        //no tile. A runner legitimately has none (runners are listed by platform, not by tile).
        if (N.IsLaunchable() && !N.IsRunner() && N.Uids.empty())
            Warnings.push_back(Tag + ": launchable has no identity (no TILE of its own and none reachable through OVER) — it appears under no tile");
    }

    //----- TILE lint: every node of one UID agrees on TITLE/COVER (the launcher shows one card per UID and reads
    //the fields off any of its nodes); PARENTUID names a UID some node carries. -----
    {
        std::map<std::string, const Node *> FirstOf;
        std::set<std::string> AllUids;
        for (const auto &[Id, N] : Idx.Nodes) if (N.OwnTile) AllUids.insert(N.Uid);
        for (const auto &[Id, N] : Idx.Nodes)
        {
            if (!N.OwnTile) continue;
            auto It = FirstOf.find(N.Uid);
            if (It == FirstOf.end()) { FirstOf[N.Uid] = &N; }
            else if (OnlyNodes == nullptr || OnlyNodes->count(Id))
            {
                const Node &F = *It->second;
                if (F.Meta.value("TITLE", std::string()) != N.Meta.value("TITLE", std::string()))
                    Warnings.push_back("node '" + Id + "': TILE UID '" + N.Uid + "' TITLE differs from node '" + F.Key() + "' ('"
                                       + N.Meta.value("TITLE", std::string()) + "' vs '" + F.Meta.value("TITLE", std::string()) + "') — one UID, one title");
                if (F.Meta.contains("COVER") && N.Meta.contains("COVER") && F.Meta["COVER"] != N.Meta["COVER"])
                    Warnings.push_back("node '" + Id + "': TILE UID '" + N.Uid + "' COVER differs from node '" + F.Key() + "' — one UID, one cover");
            }
            if (!N.ParentUid.empty() && !AllUids.count(N.ParentUid) && (OnlyNodes == nullptr || OnlyNodes->count(Id)))
                Warnings.push_back("node '" + Id + "': TILE PARENTUID '" + N.ParentUid + "' names no tile in the library");
            if (!N.ParentUid.empty() && N.ParentUid == N.Uid)
                Errors.push_back("node '" + Id + "': TILE PARENTUID is its own UID");
        }
    }

    //----- Node lint: a payload that never lowered; a WHEN with nothing to gate. -----
    for (const auto &[Id, N] : Idx.Nodes)
    {
        if (OnlyNodes && !OnlyNodes->count(Id)) continue;
        if (!N.Layers.is_array()) continue;
        const std::string Tag = "node '" + Id + "'";
        //The payload never lowered — an unknown field, an unknown FORM, malformed EDITS. A node whose payload
        //nobody applies is a package that quietly does less than it says. LowerError already names the node.
        if (!N.LowerError.empty()) Errors.push_back(N.LowerError);
        //A WHEN on a node with no payload has NO CONSUMER: TILE and ENTRYPOINTS are folded into the node's
        //identity at index time and never re-evaluated, and a pure composition node emits no layer for the
        //condition to ride on. TOGGLE is the mechanism for "this node is off".
        if (N.Layers.empty() && !N.RawWhen.empty())
            Errors.push_back(Tag + ": WHEN on a payload-less node is never evaluated (it emits no layer to gate). "
                             "Use TOGGLE to make the node opt-in, or move the condition to a payload entry.");
        //A node that is nothing at all — no payload, no tile, no entrypoint, no edge — is a mistake, not a node.
        if (N.Layers.empty() && !N.OwnTile && !N.IsLaunchable() && !N.IsRunner() && N.Over.empty())
            Warnings.push_back(Tag + ": pointless node (no payload, no TILE, no ENTRYPOINTS, no OVER)");
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
