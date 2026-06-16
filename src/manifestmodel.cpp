#include "manifestmodel.h"
#include "commonutils.h"

#include <set>
#include <fstream>

const Node *NodeIndex::Find(const std::string &NodeId) const
{
    auto It = Nodes.find(NodeId);
    return It == Nodes.end() ? nullptr : &It->second;
}

namespace ManifestModel {

// ----- node graph (everything is a node) -----

bool ParseNode(const nlohmann::ordered_json &J, const std::filesystem::path &File,
               const std::filesystem::path &BundleDir, Node &Out)
{
    if (!J.is_object() || !J.contains("NODE_ID") || !J["NODE_ID"].is_string()) return false;
    Out = Node{};
    Out.NodeId   = J["NODE_ID"].get<std::string>();
    if (Out.NodeId.empty()) return false;
    Out.Role     = J.value("ROLE", std::string("content"));
    Out.Uid      = J.value("UID", std::string());
    if (J.contains("META")    && J["META"].is_object())  Out.Meta  = J["META"];
    if (J.contains("EXEC")    && J["EXEC"].is_object())  Out.Exec  = J["EXEC"];
    if (J.contains("LAYERS")  && J["LAYERS"].is_array()) Out.Layers = J["LAYERS"];
    else                                                  Out.Layers = nlohmann::ordered_json::array();
    if (J.contains("PLATFORM") && J["PLATFORM"].is_object())
    {
        const auto &P = J["PLATFORM"];
        Out.HostPlatform = P.value("HOST", std::string());
        if (P.contains("GUEST") && P["GUEST"].is_array())
            for (const auto &G : P["GUEST"]) if (G.is_string()) Out.GuestPlatform.push_back(G.get<std::string>());
    }
    Out.Optional = J.value("OPTIONAL", false);
    Out.Default  = J.value("DEFAULT", true);
    if (J.contains("EXCLUDE") && J["EXCLUDE"].is_array())
        for (const auto &E : J["EXCLUDE"]) if (E.is_string() && !std::string(E).empty()) Out.Exclude.push_back(E.get<std::string>());
    if (J.contains("PARENTS") && J["PARENTS"].is_array())
        for (const auto &P : J["PARENTS"]) if (P.is_string() && !std::string(P).empty()) Out.Parents.push_back(P.get<std::string>());
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
        Node N;
        if (!ParseNode(J, Entry.path(), BundleDir, N)) continue;          // not a node file (no NODE_ID)
        if (Idx.Nodes.count(N.NodeId))
        { LogWarn("ManifestModel::ScanBundleNodes", "Duplicate NODE_ID '" + N.NodeId + "' (" + Entry.path().string() + ") — keeping first-seen."); continue; }
        Idx.Nodes.emplace(N.NodeId, std::move(N));
    }
}

NodeIndex BuildNodeIndex(const std::vector<std::filesystem::path> &LibraryRoots)
{
    NodeIndex Idx;
    std::error_code Ec;
    for (const auto &Root : LibraryRoots)
    {
        if (!std::filesystem::is_directory(Root, Ec)) continue;
        for (const auto &Bundle : std::filesystem::directory_iterator(Root, Ec))
            if (Bundle.is_directory(Ec)) ScanBundleNodes(Bundle.path(), Idx);
    }
    LogOut("ManifestModel::BuildNodeIndex", "Indexed " + std::to_string(Idx.Nodes.size()) + " node(s) across "
           + std::to_string(LibraryRoots.size()) + " root(s).");
    return Idx;
}

std::vector<std::string> ResolveNodeOrder(const NodeIndex &Idx, const std::string &LaunchNodeId,
                                          const std::map<std::string, bool> &Toggles,
                                          std::vector<std::string> *Missing)
{
    std::vector<std::string> Order;
    const Node *Launch = Idx.Find(LaunchNodeId);
    if (!Launch) { if (Missing) Missing->push_back(LaunchNodeId); return Order; }

    //Phase 1 — determine the ENABLED set by a breadth-first walk over PARENTS from the launch node, applying
    //each node's own optional/EXCLUDE gate. A required parent is always pulled; an optional one only if its
    //toggle (else DEFAULT) is on and it doesn't conflict with an already-kept node. We only descend INTO a node
    //we keep, so an optional node's unique ancestors are naturally dropped with it (the hierarchy gate).
    std::set<std::string> Enabled;
    std::set<std::string> Kept;                                            // for symmetric EXCLUDE (first-kept wins)
    auto Conflicts = [&](const Node &N) {
        for (const auto &E : N.Exclude) if (Kept.count(E)) return true;    // this node excludes a kept one
        for (const auto &K : Kept) { const Node *KN = Idx.Find(K); if (KN) for (const auto &E : KN->Exclude) if (E == N.NodeId) return true; }
        return false;
    };
    std::vector<std::string> Frontier = { LaunchNodeId };
    Enabled.insert(LaunchNodeId); Kept.insert(LaunchNodeId);
    while (!Frontier.empty())
    {
        const std::string Cur = Frontier.front(); Frontier.erase(Frontier.begin());
        const Node *N = Idx.Find(Cur);
        if (!N) continue;
        for (const std::string &Pid : N->Parents)
        {
            if (Enabled.count(Pid)) continue;
            const Node *P = Idx.Find(Pid);
            if (!P) { if (Missing) Missing->push_back(Pid); continue; }
            if (P->Optional)
            {
                const bool On = Toggles.count(Pid) ? Toggles.at(Pid) : P->Default;
                if (!On) continue;
            }
            if (Conflicts(*P)) continue;
            Enabled.insert(Pid); Kept.insert(Pid); Frontier.push_back(Pid);
        }
    }

    //Phase 2 — topo-order the enabled subgraph: emit a node only after all its enabled parents (post-order DFS
    //following PARENTS in list order). The launchable, having no enabled children, is emitted last = highest
    //priority. Cycles are reported and broken (the back-edge is skipped) so resolution still completes.
    std::set<std::string> Visited;
    std::set<std::string> OnStack;
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

nlohmann::ordered_json SynthesizeManifest(const NodeIndex &Idx, const std::string &LaunchNodeId,
                                          const std::map<std::string, bool> &Toggles,
                                          std::filesystem::path &OutPackageDir,
                                          std::vector<std::string> *Missing)
{
    using json = nlohmann::ordered_json;
    const Node *Launch = Idx.Find(LaunchNodeId);
    if (!Launch || Launch->IsRunner()) { if (Missing) Missing->push_back(LaunchNodeId); return json::object(); }
    OutPackageDir = Launch->BundleDir;

    const std::vector<std::string> Order = ResolveNodeOrder(Idx, LaunchNodeId, Toggles, Missing);

    //Content parents → a linear COMPONENT chain in resolved order (parent-before-child). The runner nodes in the
    //closure are skipped here (emitted as RUNNERS below); the launchable itself is handled as the GAME.
    json Components = json::array();
    std::string Prev, Top;
    for (const std::string &Id : Order)
    {
        if (Id == LaunchNodeId) continue;
        const Node *N = Idx.Find(Id);
        if (!N || N->IsRunner()) continue;
        Components.push_back(json{
            {"COMPONENTID",     N->NodeId},
            {"PARENTCOMPONENT", Prev.empty() ? json(nullptr) : json(Prev)},
            {"SUBCOMPONENTS",   N->Layers}});
        Prev = N->NodeId; Top = N->NodeId;
    }
    //The launchable's own contributions (if any) ride on top of the content chain.
    if (Launch->Layers.is_array() && !Launch->Layers.empty())
    {
        const std::string SelfId = Launch->NodeId + "__self";
        Components.push_back(json{
            {"COMPONENTID",     SelfId},
            {"PARENTCOMPONENT", Prev.empty() ? json(nullptr) : json(Prev)},
            {"SUBCOMPONENTS",   Launch->Layers}});
        Top = SelfId;
    }

    //GAME + variant from the launchable (EXEC fields — CONTENTPATH/EXEARGS — spread onto the variant).
    json Variant = json::object();
    Variant["VARIANT_ID"]    = "default";
    Variant["HOST_PLATFORM"] = Launch->HostPlatform;
    Variant["RECOMMENDED"]   = true;
    json Modules = json::array();
    if (!Top.empty()) Modules.push_back(json{{"COMPONENT", Top}});
    Variant["MODULES"] = Modules;
    if (Launch->Exec.is_object())
        for (auto &[K, V] : Launch->Exec.items()) Variant[K] = V;

    const std::string Title = Launch->Meta.is_object() ? Launch->Meta.value("TITLE", Launch->NodeId) : Launch->NodeId;
    json Game = json::object();
    Game["GAMEID"]  = Launch->NodeId;
    Game["GAMEUID"] = Launch->Uid.empty() ? Launch->NodeId : Launch->Uid;
    Game["TITLE"]   = Title;
    if (Launch->Meta.is_object()) Game["METADATA"] = Launch->Meta;
    Game["VARIANTS"] = json::array({Variant});

    json M = json::object();
    M["PACKAGEUID"]  = Launch->Uid.empty() ? Launch->NodeId : Launch->Uid;
    M["PACKAGENAME"] = Title;
    M["COMPONENTS"]  = Components;
    M["GAMES"]       = json::array({Game});

    //Every runner node → a RUNNER entry; the engine platform-selects by HOST/GUEST + preference.
    json Runners = json::array();
    for (const auto &[Id, N] : Idx.Nodes)
    {
        if (!N.IsRunner()) continue;
        json RV = json::object();
        RV["VARIANT_ID"]     = "default";
        RV["HOST_PLATFORM"]  = N.HostPlatform;
        RV["GUEST_PLATFORM"] = N.GuestPlatform;
        RV["RECOMMENDED"]    = true;
        if (N.Exec.is_object())
            for (auto &[K, V] : N.Exec.items()) RV[K] = V;       // EXECUTABLE/ARGS/ENV/REMOVE_ENV/CONTENT_ROOT/PREFIX_GENERATE
        Runners.push_back(json{
            {"RUNNER_ID", N.NodeId},
            {"NAME",      N.NodeId},
            {"VARIANTS",  json::array({RV})}});
    }
    M["RUNNERS"] = Runners;
    return M;
}

// ----- VFS layer helpers -----

bool IsVfsLayer(const std::string &Type)
{
    return Type == "VFSZipLayer" || Type == "VFSDirLayer" || Type == "VFSFileLayer";
}

std::string LayerType(const nlohmann::ordered_json &Sub)
{
    return Sub.is_object() ? Sub.value("TYPE", std::string()) : std::string();
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

std::string MachinePlatform() { return "linux64"; }
std::string HostPlatform()    { return "linux64"; }

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

} // namespace ManifestModel
