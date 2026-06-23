#include "manifestmodel.h"
#include "commonutils.h"

#include <set>
#include <map>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <zip.h>   // node-graph validation reads content zips to case-check CONTENTPATH against real files

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
    Out.Group    = J.value("GROUP", std::string());
    Out.Label    = J.value("LABEL", std::string());
    Out.Recommended = J.value("RECOMMENDED", false);
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

namespace {

std::string ToLowerAscii(std::string S) { for (char &c : S) c = (char)std::tolower((unsigned char)c); return S; }

// The regular file paths inside a zip (slash-normalized, directory entries dropped). Empty on open failure.
const std::vector<std::string> &ZipEntriesCached(const std::string &Path,
        std::map<std::string, std::vector<std::string>> &Cache)
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

// True if every file entry in the zip is STOREd (uncompressed). VidyaGodFS serves zip entries by reading the
// backing file at the entry's offset — it does NOT inflate DEFLATE — so a compressed zip layer mounts to garbage
// and is rejected at launch (VfsMount: "Compressed zip layer (not STORE)"). Catch it at authoring/validate time.
// On the first compressed file entry, returns false and reports its name. Missing/unreadable zip → treated as OK
// (the PATH/local-presence checks cover that separately).
bool ZipAllStored(const std::string &Path, std::string &FirstCompressed)
{
    int Err = 0; bool AllStored = true;
    if (zip_t *Za = zip_open(Path.c_str(), ZIP_RDONLY, &Err))
    {
        const zip_int64_t N = zip_get_num_entries(Za, 0);
        for (zip_uint64_t i = 0; i < (zip_uint64_t)N; ++i)
        {
            zip_stat_t St; zip_stat_init(&St);
            if (zip_stat_index(Za, i, 0, &St) != 0) continue;
            if ((St.valid & ZIP_STAT_COMP_METHOD) && St.comp_method != ZIP_CM_STORE)
            { AllStored = false; FirstCompressed = St.name ? St.name : ""; break; }
        }
        zip_close(Za);
    }
    return AllStored;
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
        std::map<std::string, std::vector<std::string>> &ZipCache,
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
            if (std::filesystem::is_directory(Local, Ec))
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
        std::map<std::string, std::vector<std::string>> &ZipCache,
        std::vector<std::string> &Out, std::set<std::string> &GlobalSeen)
{
    std::map<std::string, std::pair<std::string, std::string>> Seen;  // lowercased path -> (exact path, layer label)
    std::set<std::string> LocalReported;                             // collapse repeats within this launchable

    auto Consider = [&](const std::string &F, const std::string &Lbl)
    {
        const std::string Lw = ToLowerAscii(F);
        auto It = Seen.find(Lw);
        if (It == Seen.end()) { Seen.emplace(Lw, std::make_pair(F, Lbl)); return; }
        const std::string Prev = It->second.first, PrevLbl = It->second.second;
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
            const std::string Lbl = N->NodeId + " (" + Local.filename().string() + ")";
            const std::string Target = L.value("TARGET", std::string());
            if (std::filesystem::is_directory(Local, Ec))
                for (auto It2 = std::filesystem::recursive_directory_iterator(Local, Ec);
                     !Ec && It2 != std::filesystem::recursive_directory_iterator(); It2.increment(Ec))
                { if (It2->is_regular_file(Ec)) Consider(JoinTarget(Target, std::filesystem::relative(It2->path(), Local, Ec).generic_string()), Lbl); }
            else
                for (const std::string &Entry : ZipEntriesCached(Local.string(), ZipCache))
                    Consider(JoinTarget(Target, Entry), Lbl);
        }
    }
}

} // namespace

void ValidateNodeGraph(const NodeIndex &Idx, std::vector<std::string> &Errors, std::vector<std::string> &Warnings,
                       const std::set<std::string> *OnlyNodes)
{
    const std::string Machine = MachinePlatform();
    std::map<std::string, std::vector<std::string>> ZipCache;   // local zip path -> its file entries (shared content read once)
    std::set<std::string> CrossLayerSeen;                       // dedup cross-layer case collisions across launchables

    //Does any runner node serve this guest platform on this machine? (used for launchable runner-resolution.)
    auto HasRunnerFor = [&](const std::string &Host) {
        for (const auto &[Id, R] : Idx.Nodes)
            if (R.IsRunner() && R.HostPlatform == Machine)
                for (const auto &G : R.GuestPlatform) if (G == Host) return true;
        return false;
    };

    for (const auto &[Id, N] : Idx.Nodes)
    {
        if (OnlyNodes && !OnlyNodes->count(Id)) continue;   // scoped validation: skip nodes outside the requested set
        const std::string Tag = "node '" + Id + "'";

        //PARENTS resolve.
        for (const std::string &P : N.Parents)
            if (!Idx.Find(P)) Errors.push_back(Tag + ": PARENTS references missing node '" + P + "'");

        //No cycles reachable from this node (DFS over PARENTS).
        {
            std::set<std::string> OnStack, Done;
            std::function<bool(const std::string &)> HasCycle = [&](const std::string &Cur) -> bool {
                if (Done.count(Cur)) return false;
                if (OnStack.count(Cur)) return true;
                OnStack.insert(Cur);
                const Node *C = Idx.Find(Cur);
                if (C) for (const std::string &P : C->Parents) if (Idx.Find(P) && HasCycle(P)) return true;
                OnStack.erase(Cur); Done.insert(Cur);
                return false;
            };
            if (HasCycle(Id)) Errors.push_back(Tag + ": PARENTS form a cycle");
        }

        //VFS layers must declare a local PATH (or SOURCE.PATH).
        if (N.Layers.is_array())
            for (const auto &L : N.Layers)
                if (L.is_object() && IsVfsLayer(L.value("TYPE", std::string())))
                {
                    const bool HasPath = (L.contains("PATH") && L["PATH"].is_string() && !std::string(L["PATH"]).empty())
                        || (L.contains("SOURCE") && L["SOURCE"].is_object() && L["SOURCE"].contains("PATH")
                            && L["SOURCE"]["PATH"].is_string() && !std::string(L["SOURCE"]["PATH"]).empty());
                    if (!HasPath) Errors.push_back(Tag + ": a " + L.value("TYPE", std::string("VFS")) + " layer has no PATH");

                    //A locally-present zip layer must be STOREd (uncompressed) or it will not mount. Catch it here
                    //(authoring/validate/publish) rather than at launch.
                    if (HasPath && L.value("TYPE", std::string()) == "VFSZipLayer")
                    {
                        const std::string Local = ResolveLayerSource(L, N.BundleDir);
                        std::string FirstCompressed;
                        if (!Local.empty() && std::filesystem::exists(Local) && !ZipAllStored(Local, FirstCompressed))
                            Errors.push_back(Tag + ": VFSZipLayer '" + std::string(L.value("PATH", std::string()))
                                + "' is DEFLATE-compressed (entry '" + FirstCompressed + "') — VidyaGodFS requires a "
                                "STORE (uncompressed) zip; it will not mount. Re-create it with `zip -0`.");
                    }
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
                        Warnings.push_back(Tag + ": CONTENTPATH '" + Cp + "' not found in the package content");
                }
            }

            //Cross-layer case collisions in the merged content view (two layers' paths differing only in case) are
            //ERRORS: both files exist on the case-sensitive mount and a lookup can hit the wrong one → crashes.
            FindCrossLayerCaseCollisions(Idx, N, ZipCache, Errors, CrossLayerSeen);
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
