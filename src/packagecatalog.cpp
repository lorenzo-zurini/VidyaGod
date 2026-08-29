#include "packagecatalog.h"
#include "packagecatalog_p.h"
#include "apppaths.h"
#include "manifestmodel.h"
#include "commonutils.h"
#include "jsonoperations.h"
#include "ipfswrapper.h"
#include "runnerwrapper.h"
#include "containerwrapper.h"   // RunnerNodeImported (runner install state) — .cpp-only include avoids a header cycle
#include "varsubst.h"           // %KEY% substitution — resolve CustomVar-templated content-layer PATHs for hydration
#include "launchresolver.h"     // ResolveChainIds — pool a game's resolved runner chain into its hydrate (full-closure)
#include "runnerinstall.h"      // CollectRunnerNodeTargets — a runner node's build download targets
#include "launchparams.h"       // ContainerParams (minimal, for the chain resolve)

#include <QDir>
#include <QFile>
#include <QStringList>
#include <cstdint>
#include <map>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <vector>

using namespace ManifestModel;   // LayerLocator / IsVfsLayer / ForEachVfsLayer / PackageHydrated / PackageIpfsCids

namespace PackageCatalog {

// ----- per-package user settings -----

nlohmann::ordered_json GetPackageUserSettings(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &PackageUID)
{
    if (!GlobalConfigJSON.contains("LIBRARY")) return nlohmann::ordered_json::object();
    for (auto &Entry : GlobalConfigJSON["LIBRARY"])
        if (Entry.contains("PACKAGEUID") && std::string(Entry["PACKAGEUID"]) == PackageUID)
            return Entry.contains("USERSETTINGS") ? Entry["USERSETTINGS"] : nlohmann::ordered_json::object();
    return nlohmann::ordered_json::object();
}

nlohmann::ordered_json GetPackageVariables(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &PackageUID)
{
    nlohmann::ordered_json US = GetPackageUserSettings(GlobalConfigJSON, PackageUID);
    if (US.contains("VARIABLES") && US["VARIABLES"].is_object()) return US["VARIABLES"];
    return nlohmann::ordered_json::object();
}

void MergePackageVariables(nlohmann::ordered_json &GlobalConfigJSON, const std::string &PackageUID, const std::map<std::string, std::string> &NewVars)
{
    if (NewVars.empty()) return;
    nlohmann::ordered_json Vars = GetPackageVariables(GlobalConfigJSON, PackageUID);
    for (const auto &[K, V] : NewVars) Vars[K] = V;
    SetPackageUserSetting(GlobalConfigJSON, PackageUID, "VARIABLES", Vars);
}

void SetPackageUserSetting(nlohmann::ordered_json &GlobalConfigJSON, const std::string &PackageUID, const std::string &Key, const nlohmann::ordered_json &Value)
{
    if (!GlobalConfigJSON.contains("LIBRARY")) return;
    for (auto &Entry : GlobalConfigJSON["LIBRARY"])
        if (Entry.contains("PACKAGEUID") && std::string(Entry["PACKAGEUID"]) == PackageUID)
        {
            if (!Entry.contains("USERSETTINGS") || !Entry["USERSETTINGS"].is_object())
                Entry["USERSETTINGS"] = nlohmann::ordered_json::object();
            Entry["USERSETTINGS"][Key] = Value;
            return;
        }
}

// ----- on-disk locations -----

//The managed library root — repos clone here (one subfolder per repo); every package hydrates in place beside its
//manifest. Overridable via Settings.Paths.LibraryRoot; default ~/.VidyaGod/LIBRARY.
static std::string LibraryDir(const nlohmann::ordered_json &GlobalConfigJSON)
{
    if (GlobalConfigJSON.contains("Settings") && GlobalConfigJSON["Settings"].is_object())
    {
        const auto &S = GlobalConfigJSON["Settings"];
        if (S.contains("Paths") && S["Paths"].is_object() && S["Paths"].contains("LibraryRoot")
            && S["Paths"]["LibraryRoot"].is_string() && !std::string(S["Paths"]["LibraryRoot"]).empty())
            return QDir::cleanPath(QString::fromStdString(std::string(S["Paths"]["LibraryRoot"]))).toStdString();
    }
    return QDir::cleanPath(QString::fromStdString((AppPaths::DataRoot() / "LIBRARY").string())).toStdString();
}

std::string LibraryRootDir(const nlohmann::ordered_json &GlobalConfigJSON) { return LibraryDir(GlobalConfigJSON); }

// Pure path-prefix test: is Path inside Base? (weakly-canonical so it works whether or not either exists yet.)
static bool PathUnder(const std::filesystem::path &Base, const std::filesystem::path &Path)
{
    std::error_code Ec;
    const std::filesystem::path B = std::filesystem::weakly_canonical(Base, Ec);
    const std::filesystem::path P = std::filesystem::weakly_canonical(Path, Ec);
    auto It = std::mismatch(B.begin(), B.end(), P.begin(), P.end());
    return It.first == B.end();                                     // B is a prefix of P
}

// ----- package sources (IPFS folder CIDs) -----
//A source's CID (object {CID,NAME} or a bare CID string) and its on-disk name (NAME, else a filesystem-safe CID prefix).
static std::string PackageSourceCID(const nlohmann::ordered_json &S)
{
    if (S.is_object() && S.contains("CID") && S["CID"].is_string()) return std::string(S["CID"]);
    if (S.is_string()) return std::string(S);
    return std::string();
}
static std::string PackageSourceName(const nlohmann::ordered_json &S)
{
    std::string N = S.is_object() ? S.value("NAME", std::string()) : std::string();
    if (N.empty()) { const std::string C = PackageSourceCID(S); N = C.size() > 12 ? C.substr(0, 12) : C; }
    for (char &c : N) if (c == '/' || c == '\\' || c == ':') c = '_';
    return N.empty() ? std::string("cidsource") : N;
}
//CID package sources ARE the library now (git is gone): each fetches into LIBRARY/<name>. "Under a package source" =
//"under the LIBRARY root". Local (externally-added) packages live OUTSIDE LIBRARY, so they stay distinguishable.
static std::string PackageSourcesRoot(const nlohmann::ordered_json &GlobalConfigJSON)
{
    return LibraryDir(GlobalConfigJSON);
}
static std::string PackageSourceDir(const nlohmann::ordered_json &GlobalConfigJSON, const nlohmann::ordered_json &S)
{
    return QDir::cleanPath(QString::fromStdString(PackageSourcesRoot(GlobalConfigJSON) + "/" + PackageSourceName(S))).toStdString();
}

bool IsPackageSourcePath(const nlohmann::ordered_json &GlobalConfigJSON, const std::filesystem::path &BundleDir)
{
    return !BundleDir.empty() && PathUnder(PackageSourcesRoot(GlobalConfigJSON), BundleDir);
}

//A managed package: sourced from a CID package source (i.e. NOT a standalone locally-added bundle). (Git repos were
//removed — every shared package now comes from a Settings.PackageSources[] CID, so "managed" == "under a source dir".)
static bool PathUnderManagedSource(const nlohmann::ordered_json &GlobalConfigJSON, const std::filesystem::path &Path)
{
    return IsPackageSourcePath(GlobalConfigJSON, Path);
}

std::vector<std::string> PackageSourceDirs(const nlohmann::ordered_json &GlobalConfigJSON)
{
    std::vector<std::string> Dirs;
    if (!GlobalConfigJSON.contains("Settings") || !GlobalConfigJSON["Settings"].is_object()) return Dirs;
    const auto &S = GlobalConfigJSON["Settings"];
    if (!S.contains("PackageSources") || !S["PackageSources"].is_array()) return Dirs;
    std::error_code Ec;
    for (const auto &Src : S["PackageSources"])
    {
        if (PackageSourceCID(Src).empty()) continue;
        const std::string Dir = PackageSourceDir(GlobalConfigJSON, Src);
        if (std::filesystem::is_directory(Dir, Ec)) Dirs.push_back(Dir);   // existing = a scan root
    }
    return Dirs;
}

std::string PackageSourceNameForPath(const nlohmann::ordered_json &GlobalConfigJSON, const std::filesystem::path &BundleDir)
{
    if (BundleDir.empty() || !GlobalConfigJSON.contains("Settings") || !GlobalConfigJSON["Settings"].is_object()) return {};
    const auto &S = GlobalConfigJSON["Settings"];
    if (!S.contains("PackageSources") || !S["PackageSources"].is_array()) return {};
    for (const auto &Src : S["PackageSources"])
    {
        if (PackageSourceCID(Src).empty()) continue;
        if (PathUnder(PackageSourceDir(GlobalConfigJSON, Src), BundleDir)) return PackageSourceName(Src);
    }
    return {};
}

bool IsLocalPackagePath(const nlohmann::ordered_json &GlobalConfigJSON, const std::filesystem::path &BundleDir)
{
    return !BundleDir.empty() && !PathUnderManagedSource(GlobalConfigJSON, BundleDir);
}

std::vector<std::filesystem::path> LocalPackageDirs(const nlohmann::ordered_json &GlobalConfigJSON)
{
    std::vector<std::filesystem::path> Out;
    if (!GlobalConfigJSON.contains("LIBRARY") || !GlobalConfigJSON["LIBRARY"].is_array()) return Out;
    std::error_code Ec;
    for (const auto &E : GlobalConfigJSON["LIBRARY"])
    {
        const std::string Path = E.is_object() ? E.value("PATH", std::string()) : std::string();
        if (Path.empty()) continue;
        if (PathUnderManagedSource(GlobalConfigJSON, Path)) continue;   // repo / CID-source package — indexed via its root
        if (std::filesystem::is_directory(Path, Ec)) Out.emplace_back(Path);
    }
    return Out;
}

int PruneMovedLocalPackages(nlohmann::ordered_json &GlobalConfigJSON)
{
    if (!GlobalConfigJSON.contains("LIBRARY") || !GlobalConfigJSON["LIBRARY"].is_array()) return 0;
    auto &Lib = GlobalConfigJSON["LIBRARY"];
    std::error_code Ec;
    int Removed = 0;
    for (auto It = Lib.begin(); It != Lib.end(); )
    {
        const std::string Path = It->is_object() ? It->value("PATH", std::string()) : std::string();
        const bool Local = !Path.empty() && !PathUnderManagedSource(GlobalConfigJSON, Path);
        if (Local && !std::filesystem::is_directory(Path, Ec))
        {
            LogWarn("PackageCatalog::PruneMovedLocalPackages",
                    "Dropping moved/deleted local package '" + It->value("PACKAGENAME", Path) + "' (" + Path + ").");
            It = Lib.erase(It); ++Removed;
        }
        else ++It;
    }
    return Removed;
}

//A bundle's index identity, derived from its node files (everything-is-a-node): the representative launchable
//node's UID + title, and whether the bundle defines any launchable / runner node. Replaces the AssembleManifest
//PACKAGEUID/PACKAGENAME/HasGames/HasRunners reads.
BundleIdentity ScanBundleIdentity(const std::string &BundleDir)   // decl + BundleIdentity: packagecatalog_p.h
{
    BundleIdentity Id;
    NodeIndex Idx;
    ManifestModel::ScanBundleNodes(BundleDir, Idx);
    ManifestModel::LinkGames(Idx);   // a launchable variant inherits its tile's UID/TITLE (DeclareExec + DeclareLibraryItem
                                     // may live on separate nodes); without this, such a bundle is mislabelled by its exec node-id
    const Node *Rep = nullptr;                                                    // prefer a presentable launchable
    for (const auto &[NodeId, N] : Idx.Nodes)
    {
        if (N.IsLaunchable()) { Id.HasLaunchable = true; if (!Rep || (N.Presentable() && !Rep->Presentable())) Rep = &N; }
        if (N.IsRunner())     Id.HasRunner = true;
    }
    if (!Rep)                                                                     // runner-only / content-only bundle
        for (const auto &[NodeId, N] : Idx.Nodes) { if (!N.Uid.empty()) { Rep = &N; break; } }
    if (!Rep && !Idx.Nodes.empty()) Rep = &Idx.Nodes.begin()->second;             // no UID anywhere (runners) → use NODE_ID
    if (Rep)
    {
        Id.Uid  = Rep->Uid.empty() ? Rep->NodeId : Rep->Uid;
        Id.Name = Rep->Meta.is_object() ? Rep->Meta.value("TITLE", Rep->NodeId) : Rep->NodeId;
        Id.Valid = !Id.Uid.empty();
    }
    return Id;
}

//Upsert a LIBRARY entry for a CID-source package: PATH + name + CIDSOURCE (its source's CID). Removal is explicit via
//RemovePackageSource (an immutable source's contents never change, so there is no reconcile/auto-prune).
static void UpsertCidEntry(nlohmann::ordered_json &Arr, const std::string &Uid, const std::string &SourceCid,
                           const std::string &Dir, const std::string &Name)
{
    for (auto &E : Arr)
    {
        if (E.value("PACKAGEUID", std::string()) != Uid) continue;
        E["PACKAGENAME"] = Name; E["PATH"] = Dir; E["CIDSOURCE"] = SourceCid;
        return;
    }
    nlohmann::ordered_json Slim;
    Slim["PACKAGEUID"]  = Uid;
    Slim["PACKAGENAME"] = Name;
    Slim["PATH"]        = Dir;
    Slim["CIDSOURCE"]   = SourceCid;
    Arr.push_back(std::move(Slim));
}

bool HasMissingSources(const nlohmann::ordered_json &GlobalConfigJSON)
{
    if (!GlobalConfigJSON.contains("Settings") || !GlobalConfigJSON["Settings"].is_object()) return false;
    const auto &S = GlobalConfigJSON["Settings"];
    if (!S.contains("PackageSources") || !S["PackageSources"].is_array()) return false;
    std::error_code Ec;
    for (const auto &Src : S["PackageSources"])
    {
        if (PackageSourceCID(Src).empty()) continue;
        const std::string Dir = PackageSourceDir(GlobalConfigJSON, Src);
        if (!std::filesystem::is_directory(Dir, Ec) || std::filesystem::is_empty(Dir, Ec)) return true;
    }
    return false;
}

int SyncPackageSources(nlohmann::ordered_json &GlobalConfigJSON, std::string *Error)
{
    if (!GlobalConfigJSON.contains("Settings") || !GlobalConfigJSON["Settings"].is_object()) return 0;
    //Create LIBRARY BEFORE caching a reference into Settings — adding a top-level key reallocates the object's storage,
    //which would dangle the cached &S.
    if (!GlobalConfigJSON.contains("LIBRARY") || !GlobalConfigJSON["LIBRARY"].is_array())
        GlobalConfigJSON["LIBRARY"] = nlohmann::ordered_json::array();
    auto &S = GlobalConfigJSON["Settings"];
    if (!S.contains("PackageSources") || !S["PackageSources"].is_array()) return 0;

    int Indexed = 0;
    std::error_code Ec;
    for (auto &Src : S["PackageSources"])
    {
        const std::string Cid = PackageSourceCID(Src);
        if (Cid.empty()) continue;
        const std::string Dir = PackageSourceDir(GlobalConfigJSON, Src);

        //A CID is immutable → fetch the dehydrated folder once (dehydrated only; content hydrates later per-layer).
        const bool Have = std::filesystem::is_directory(Dir, Ec) && !std::filesystem::is_empty(Dir, Ec);
        if (!Have)
        {
            LogOut("PackageCatalog::SyncPackageSources", "Fetching CID package source " + Cid + " -> " + Dir);
            std::string FErr;
            if (IpfsWrapper::FetchDirToPath(Cid, Dir, &FErr).empty())
            {
                LogErr("PackageCatalog::SyncPackageSources", "Fetch failed for CID " + Cid + ": " + FErr);
                if (Error && Error->empty()) *Error = FErr;
                continue;
            }
        }

        //A CID source is EITHER a single package (the fetched dir is itself a bundle — per-package CID) OR a collection
        //(the fetched dir holds package subdirs). Try the dir-as-bundle case first so per-package CIDs are first-class;
        //fall back to scanning subdirs for a whole-library folder CID.
        int Games = 0, Packages = 0;
        auto Upsert = [&](const std::string &PkgDir, const BundleIdentity &Id) {
            UpsertCidEntry(GlobalConfigJSON["LIBRARY"], Id.Uid, Cid, PkgDir, Id.Name);
            ++Packages;
            if (Id.HasLaunchable) { ++Games; ++Indexed; }
        };

        const BundleIdentity Self = ScanBundleIdentity(Dir);
        if (Self.Valid)
            Upsert(Dir, Self);                                                        // single-package CID
        else
        {
            QDir D(QString::fromStdString(Dir));                                      // collection CID → scan subdirs
            for (const QString &Sub : D.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
            {
                const QString PkgDir = D.filePath(Sub);
                const BundleIdentity Id = ScanBundleIdentity(PkgDir.toStdString());
                if (!Id.Valid) continue;
                Upsert(PkgDir.toStdString(), Id);
            }
        }
        LogOut("PackageCatalog::SyncPackageSources", "Indexed CID source '" + PackageSourceName(Src) + "' ("
               + std::to_string(Packages) + " package(s), " + std::to_string(Games) + " launchable).");
    }
    return Indexed;
}

bool AddPackageSource(nlohmann::ordered_json &GlobalConfigJSON, const std::string &Cid, const std::string &Name)
{
    if (Cid.empty()) return false;
    if (!GlobalConfigJSON.contains("Settings") || !GlobalConfigJSON["Settings"].is_object())
        GlobalConfigJSON["Settings"] = nlohmann::ordered_json::object();
    auto &S = GlobalConfigJSON["Settings"];
    if (!S.contains("PackageSources") || !S["PackageSources"].is_array())
        S["PackageSources"] = nlohmann::ordered_json::array();
    for (const auto &E : S["PackageSources"]) if (PackageSourceCID(E) == Cid) return false;   // already added
    nlohmann::ordered_json Src;
    Src["CID"] = Cid;
    if (!Name.empty()) Src["NAME"] = Name;
    S["PackageSources"].push_back(std::move(Src));
    return true;
}

std::set<std::string> SourceContentCids(const nlohmann::ordered_json &GlobalConfigJSON,
                                        const nlohmann::ordered_json &Source)
{
    std::set<std::string> Cids;
    const std::string Cid = Source.is_object() ? Source.value("CID", std::string())
                          : (Source.is_string() ? Source.get<std::string>() : std::string());
    if (!Cid.empty()) Cids.insert(Cid);                                  // the collection meta-CID itself
    const std::string Dir = PackageSourceDir(GlobalConfigJSON, Source);
    if (!Dir.empty())
        for (const auto &[Path, C] : SeedTargets(Dir)) Cids.insert(C);   // every SOURCE.CID under it

    // Package meta-CIDs live only in config (they never appear inside a node JSON), keyed by package dir/UID.
    if (GlobalConfigJSON.contains("Settings") && GlobalConfigJSON["Settings"].is_object())
    {
        const auto &S = GlobalConfigJSON["Settings"];
        if (S.contains("PackageCids") && S["PackageCids"].is_object())
            for (const auto &[Key, Val] : S["PackageCids"].items())
                if (Val.is_string() && !Val.get<std::string>().empty() && !Dir.empty() && PathUnder(Dir, Key))
                    Cids.insert(Val.get<std::string>());
    }
    return Cids;
}

void RemovePackageSource(nlohmann::ordered_json &GlobalConfigJSON, int Index)
{
    if (!GlobalConfigJSON.contains("Settings") || !GlobalConfigJSON["Settings"].is_object()) return;
    auto &S = GlobalConfigJSON["Settings"];
    if (!S.contains("PackageSources") || !S["PackageSources"].is_array()) return;
    auto &Arr = S["PackageSources"];
    if (Index < 0 || Index >= (int)Arr.size()) return;

    const std::string Dir = PackageSourceDir(GlobalConfigJSON, Arr[Index]);

    //Un-seed BEFORE deleting the directory. A removed source's pins otherwise outlive it in the IPFS table, and the
    //moment the dir below is deleted every one of its no-copy references points at a gone file — so we keep
    //ADVERTISING blocks we can no longer read, and any peer that asks for them HANGS instead of failing over to
    //another provider. (Order matters: SeedTargets can only enumerate what is still on disk.) Only CIDs unique to
    //this source go — anything a surviving source still references is kept, since content is shared across sources.
    {
        std::set<std::string> Mine = SourceContentCids(GlobalConfigJSON, Arr[Index]);
        for (int i = 0; i < (int)Arr.size(); ++i)
        {
            if (i == Index) continue;
            for (const std::string &C : SourceContentCids(GlobalConfigJSON, Arr[i])) Mine.erase(C);
        }
        int Dropped = 0;
        for (const std::string &C : Mine) if (IpfsWrapper::DropRef(C)) ++Dropped;
        if (Dropped)
            LogSucc("PackageCatalog::RemovePackageSource",
                    "un-seeded " + std::to_string(Dropped) + "/" + std::to_string(Mine.size())
                    + " CID(s) of the removed source");
    }

    //Drop LIBRARY entries under this source's dir, then delete the fetched dir.
    if (GlobalConfigJSON.contains("LIBRARY") && GlobalConfigJSON["LIBRARY"].is_array())
    {
        auto &Lib = GlobalConfigJSON["LIBRARY"];
        for (auto It = Lib.begin(); It != Lib.end(); )
        {
            const std::string P = It->is_object() ? It->value("PATH", std::string()) : std::string();
            if (!P.empty() && PathUnder(Dir, P)) It = Lib.erase(It); else ++It;
        }
    }
    std::error_code Ec; std::filesystem::remove_all(Dir, Ec);
    Arr.erase(Arr.begin() + Index);
}

// ----- node-graph catalog (everything-is-a-node) -----

NodeIndex BuildCatalogIndex(const nlohmann::ordered_json &GlobalConfigJSON)
{
    std::vector<std::filesystem::path> Roots;
    std::vector<std::filesystem::path> Extra = LocalPackageDirs(GlobalConfigJSON);     // externally-added bundles (dir IS a bundle)
    //A CID package source is EITHER a collection (scan its subdirs) OR a single per-package bundle (the dir IS the
    //bundle — a per-package CID). Route each accordingly. (Git repos were removed — CID sources are the only channel.)
    for (const auto &D : PackageSourceDirs(GlobalConfigJSON))
    {
        if (ScanBundleIdentity(D).Valid) Extra.emplace_back(D);   // per-package CID → the dir itself is one bundle
        else                             Roots.emplace_back(D);   // collection CID → scan its package subdirs
    }
    return ManifestModel::BuildNodeIndex(Roots, Extra);
}

std::vector<std::vector<const Node*>> PresentableGroups(const NodeIndex &Idx)
{
    std::map<std::string, std::vector<const Node*>> Groups;
    for (const auto &[Id, N] : Idx.Nodes)
        if (N.IsLaunchable() && N.Presentable())
            Groups[N.GameKey()].push_back(&N);

    std::vector<std::vector<const Node*>> Out;
    for (auto &[K, V] : Groups)
    {
        std::sort(V.begin(), V.end(), [](const Node *A, const Node *B){
            if (A->Recommended != B->Recommended) return A->Recommended;     // recommended edition first
            return A->NodeId < B->NodeId;
        });
        Out.push_back(std::move(V));
    }
    auto Title = [](const Node *N){ return N->Meta.is_object() ? N->Meta.value("TITLE", N->NodeId) : N->NodeId; };
    std::sort(Out.begin(), Out.end(), [&](const std::vector<const Node*> &A, const std::vector<const Node*> &B){
        return Title(A.front()) < Title(B.front());
    });
    return Out;
}

std::vector<const Node*> RunnerCandidates(const NodeIndex &Idx, const Node &Launch)
{
    std::vector<const Node*> Out;
    for (const auto &[Id, N] : Idx.Nodes)
    {
        if (!N.IsRunner() || N.HostPlatform != MachinePlatform()) continue;
        bool Guest = false;
        for (const auto &G : N.GuestPlatform) if (G == Launch.HostPlatform) { Guest = true; break; }
        if (Guest && RunnerWrapper::ExecutableAvailable(N.Exec)) Out.push_back(&N);
    }
    return Out;   // std::map iteration = sorted by node id
}

std::vector<const Node*> CompatibleRunners(const NodeIndex &Idx, const Node &Launch)
{
    std::vector<const Node*> Out;
    for (const auto &[Id, N] : Idx.Nodes)
    {
        (void)Id;
        if (!N.IsRunner() || N.HostPlatform != MachinePlatform()) continue;
        for (const auto &G : N.GuestPlatform) if (G == Launch.HostPlatform) { Out.push_back(&N); break; }
    }
    return Out;   // no executable/install gate — these are runners that COULD run it once installed
}

bool RunnerInstalled(const NodeIndex &Idx, const std::string &RunnerNodeId)
{
    const Node *R = Idx.Find(RunnerNodeId);
    if (!R || !R->IsRunner()) return false;
    // Ships its own build (any VFS layer in its content closure — local PATH or a remote CID) → must be imported
    // (build hydrated + DEFPREFIX). Otherwise it's a PATH runner → usable iff its executable resolves on this system.
    bool ShipsBuild = false;
    for (const std::string &Id : ManifestModel::ResolveNodeOrder(Idx, RunnerNodeId, {}))
    {
        if (Id == RunnerNodeId) continue;
        const Node *N = Idx.Find(Id);
        if (!N || N->IsRunner() || !N->Layers.is_array()) continue;
        for (const auto &L : N->Layers) if (IsVfsLayer(LayerType(L))) { ShipsBuild = true; break; }
        if (ShipsBuild) break;
    }
    if (ShipsBuild)
        return RunnerInstall::RunnerNodeImported(Idx, RunnerNodeId);
    return RunnerWrapper::ExecutableAvailable(R->Exec);
}

bool IsEmbeddedRunner(const NodeIndex &Idx, const std::string &RunnerNodeId)
{
    const Node *R = Idx.Find(RunnerNodeId);
    if (!R || !R->IsRunner()) return false;
    for (const auto &[Id, N] : Idx.Nodes)
    {
        (void)Id;
        if (N.IsLaunchable() && N.BundleDir == R->BundleDir) return true;   // a game shares its bundle → embedded
    }
    return false;
}

std::vector<const Node*> UsableRunners(const NodeIndex &Idx, const Node &Launch)
{
    std::vector<const Node*> Out;
    for (const Node *R : CompatibleRunners(Idx, Launch))
        if (RunnerInstalled(Idx, R->NodeId)) Out.push_back(R);
    return Out;
}

std::vector<const Node*> CandidateRunners(const NodeIndex &Idx, const std::string &InputPlatform)
{
    //Installed runners that can consume InputPlatform (GUEST ∋ InputPlatform), regardless of HOST — a chain step's
    //choices. Unlike UsableRunners, HOST may be an intermediate platform (e.g. snes9x: snes→win32). Sorted by node id.
    std::vector<const Node*> Out;
    for (const auto &[Id, N] : Idx.Nodes)
    {
        (void)Id;
        if (!N.IsRunner()) continue;
        bool Serves = false;
        for (const auto &G : N.GuestPlatform) if (G == InputPlatform) { Serves = true; break; }
        if (Serves && RunnerInstalled(Idx, N.NodeId)) Out.push_back(&N);
    }
    return Out;   // std::map iteration = sorted by node id
}

//Invoke Fn for every VFS layer in a launchable's content closure (runner build excluded), with its resolved local
//path + ipfs CID (resolved against the OWNING node's bundle dir — cross-bundle-correct).
static void ForEachContentLayer(const NodeIndex &Idx, const std::string &LaunchNodeId,
    const std::map<std::string, bool> &Toggles,
    const std::function<void(const nlohmann::ordered_json&, const std::filesystem::path&, const std::string&)> &Fn)
{
    for (const std::string &Id : ManifestModel::ResolveNodeOrder(Idx, LaunchNodeId, Toggles))
    {
        const Node *N = Idx.Find(Id);
        if (!N || N->IsRunner() || !N->Layers.is_array()) continue;
        for (const auto &L : N->Layers)
        {
            if (!IsVfsLayer(LayerType(L))) continue;
            std::filesystem::path Local; std::string Cid;
            LayerLocator(L, N->BundleDir, Local, Cid);
            if (Local == N->BundleDir) continue;                                 // no PATH
            Fn(L, Local, Cid);
        }
    }
}

// The global CustomVar default namespace (KEY -> DEFAULT) across the whole node graph. CustomVars resolve in ONE
// global namespace (see LaunchResolver::ResolveCustomVariables), so a single map suffices to substitute the %KEY%
// tokens that appear in content-layer PATHs — e.g. a shared library node whose PATH is "%dgv_zip%". Later (more
// specific) declarations win, matching the launch resolver's hierarchy; for hydration only the file identity matters.
static std::map<std::string, std::string> CustomVarDefaults(const NodeIndex &Idx)
{
    std::map<std::string, std::string> Vars;
    for (const auto &[Id, N] : Idx.Nodes)
    {
        (void)Id;
        if (!N.Layers.is_array()) continue;
        for (const auto &L : N.Layers)
            if (L.is_object() && L.value("TYPE", std::string()) == "CustomVar")
            {
                const std::string Key = L.value("KEY", std::string());
                if (!Key.empty()) Vars[Key] = L.value("DEFAULT", std::string());
            }
    }
    return Vars;
}

// How a content layer's PATH resolves against the local filesystem — the path-only half of its fetch state (the CID
// half is applied at the call site, since it decides fetchable-vs-broken). A PATH may be CustomVar-templated
// ("%dgv_zip%" -> a real zip); substitute the global var namespace first. A path that STILL carries a %token% after
// substitution is RUNTIME-sourced (a runner's "%RunnerMount%/..." prefix mount) — it resolves to a live mount at
// launch, not to on-disk content (mirrors LaunchSources' IsRuntimeSourcedLayer skip).
enum class PathState : std::uint8_t { Runtime, Present, Missing };
static PathState ResolvePathState(const std::filesystem::path &Local, const std::map<std::string, std::string> &Vars)
{
    std::string S = Local.string();
    if (S.find('%') != std::string::npos)
    {
        VarSubst::StringVariableSubstitution(S, Vars);
        if (S.find('%') != std::string::npos) return PathState::Runtime;
    }
    std::error_code Ec;
    return std::filesystem::exists(S, Ec) ? PathState::Present : PathState::Missing;
}

// HYDRATION = "everything fetchable has been fetched" (NOT "every backing file exists"). A content layer is un-hydrated
// ONLY when it is FETCHABLE-MISSING: its backing file is absent AND it carries a remote source (CID) to fetch it from.
// A missing local-only layer (no CID) is not un-hydrated — there is nothing to fetch; it is a broken/incomplete package,
// which --validate-nodes owns, not the download button. A runtime-sourced %path% has no on-disk file at all.
static bool LayerFetchableMissing(PathState PS, const std::string &Cid)
{
    return PS == PathState::Missing && !Cid.empty();
}

bool NodeHydrated(const NodeIndex &Idx, const std::string &LaunchNodeId)
{
    if (!Idx.Find(LaunchNodeId)) return false;
    const std::map<std::string, std::string> Vars = CustomVarDefaults(Idx);
    bool AllFetched = true;
    ForEachContentLayer(Idx, LaunchNodeId, {}, [&](const nlohmann::ordered_json&, const std::filesystem::path &Local, const std::string &Cid){
        if (LayerFetchableMissing(ResolvePathState(Local, Vars), Cid)) AllFetched = false;
    });
    return AllFetched;
}

bool NodeHasContent(const NodeIndex &Idx, const std::string &LaunchNodeId)
{
    // True iff the node's closure defines at least one VFS content layer. Distinguishes a real (downloadable) game
    // from a content-less/malformed node, which is VACUOUSLY "hydrated" (no layers → nothing missing) and would
    // otherwise show up in the Library + Installed Packages with nothing to launch.
    bool Has = false;
    ForEachContentLayer(Idx, LaunchNodeId, {}, [&](const nlohmann::ordered_json&, const std::filesystem::path&, const std::string&){ Has = true; });
    return Has;
}

std::unordered_map<std::string, NodeHydration> HydrationMap(const NodeIndex &Idx)
{
    std::unordered_map<std::string, NodeHydration> Memo;
    std::unordered_map<std::string, PathState> StatCache;            // raw local path -> resolved path state (stat'd once)
    const std::map<std::string, std::string> Vars = CustomVarDefaults(Idx);   // resolve %dgv_zip% etc. before stat'ing
    std::function<NodeHydration(const std::string &)> Compute = [&](const std::string &Id) -> NodeHydration {
        auto It = Memo.find(Id);
        if (It != Memo.end()) return It->second;
        Memo[Id] = {true, false};                                    // cycle guard (graph is a DAG; validation checks this)
        NodeHydration R;                                             // {Hydrated=true, HasContent=false}
        const Node *N = Idx.Find(Id);
        // Own content layers (runner nodes contribute nothing — their layers are never mounted as content).
        if (N && !N->IsRunner() && N->Layers.is_array())
            for (const auto &L : N->Layers)
            {
                if (!L.is_object() || !IsVfsLayer(LayerType(L))) continue;
                R.HasContent = true;
                std::filesystem::path Local; std::string Cid;
                LayerLocator(L, N->BundleDir, Local, Cid);
                const std::string Key = Local.string();
                auto Sc = StatCache.find(Key);
                PathState PS;
                if (Sc != StatCache.end()) PS = Sc->second;
                else { PS = ResolvePathState(Local, Vars); StatCache[Key] = PS; }
                if (LayerFetchableMissing(PS, Cid)) R.Hydrated = false;
            }
        // Fold in the parents' (already-memoized) closure result.
        if (N)
            for (const std::string &P : N->Parents)
            {
                if (!Idx.Find(P)) continue;
                const NodeHydration PC = Compute(P);
                R.Hydrated    = R.Hydrated && PC.Hydrated;
                R.HasContent  = R.HasContent || PC.HasContent;
            }
        Memo[Id] = R;
        return R;
    };
    for (const auto &[Id, N] : Idx.Nodes) { (void)N; Compute(Id); }
    return Memo;
}

int DehydrateNode(const NodeIndex &Idx, const std::string &LaunchNodeId)
{
    // Inverse of HydrateNode: delete the node closure's local content-layer files (keeping the manifests + cover, so
    // the package returns to the Catalog as re-downloadable) and unpin + drop the references for their CIDs (so the
    // node stops "seeding" content that's no longer on disk). Cover art (META.COVER) is left in place — it's tiny,
    // git-tracked, and needed to render the catalog tile.
    int Removed = 0;
    std::set<std::string> Cids;
    ForEachContentLayer(Idx, LaunchNodeId, {}, [&](const nlohmann::ordered_json&, const std::filesystem::path &Local, const std::string &Cid){
        if (!Cid.empty()) Cids.insert(Cid);
        std::error_code Ec;
        if (std::filesystem::exists(Local, Ec) && std::filesystem::is_regular_file(Local, Ec))
        { std::filesystem::remove(Local, Ec); if (!Ec) ++Removed; }
    });
    for (const std::string &C : Cids) { IpfsWrapper::Unpin(C); IpfsWrapper::DropRef(C); }
    return Removed;
}

std::vector<std::string> NodeContentCids(const NodeIndex &Idx, const std::string &LaunchNodeId, const std::map<std::string, bool> &Toggles)
{
    std::vector<std::string> Cids;
    std::set<std::string> Seen;
    ForEachContentLayer(Idx, LaunchNodeId, Toggles, [&](const nlohmann::ordered_json&, const std::filesystem::path&, const std::string &Cid){
        if (!Cid.empty() && Seen.insert(Cid).second) Cids.push_back(Cid);
    });
    return Cids;
}

bool CollectContentTargets(const NodeIndex &Idx, const std::string &LaunchNodeId, const std::map<std::string, bool> &Toggles,
                           std::vector<IpfsWrapper::FetchTarget> &Out, std::string *Error)
{
    auto Fail = [&](const std::string &M) -> bool { if (Error) *Error = M; LogErr("PackageCatalog::CollectContentTargets", M); return false; };
    const Node *Launch = Idx.Find(LaunchNodeId);
    if (!Launch) return Fail("launch node not found: " + LaunchNodeId);

    // Required content layers: fetch any that aren't already on disk; a missing layer with no IPFS source is fatal.
    bool MissingSource = false; std::string MissingErr;
    ForEachContentLayer(Idx, LaunchNodeId, Toggles, [&](const nlohmann::ordered_json&, const std::filesystem::path &Local, const std::string &Cid){
        if (MissingSource) return;
        std::error_code Ec;
        if (std::filesystem::exists(Local, Ec)) return;                          // already present
        if (Cid.empty()) { MissingSource = true; MissingErr = "missing local content with no IPFS source: " + Local.string(); return; }
        Out.push_back({Cid, Local.string(), false});
    });
    if (MissingSource) return Fail(MissingErr);

    // Cover: fetch if remote; if already on disk but un-seeded/orphaned, seed it by reference so a downloader serves
    // it too. Best-effort (a cover never fails a hydrate).
    if (Launch->Meta.is_object() && Launch->Meta.contains("COVER") && Launch->Meta["COVER"].is_object())
    {
        std::filesystem::path Local; std::string Cid;
        LayerLocator(Launch->Meta["COVER"], Launch->BundleDir, Local, Cid);
        std::error_code Ec;
        if (!Cid.empty() && Local != Launch->BundleDir)
        {
            if (!std::filesystem::exists(Local, Ec))
                Out.push_back({Cid, Local.string(), true});                 // not local → fetch over IPFS (FetchToPath seeds it)
            else if (!IpfsWrapper::HasLocal(Cid) || IpfsWrapper::CidMissing(Cid))
            {
                if (IpfsWrapper::CidMissing(Cid)) IpfsWrapper::DropRef(Cid);   // re-point a stale reference
                std::string E;
                if (IpfsWrapper::AddNoCopy(Local.string(), &E).empty())
                    LogWarn("PackageCatalog::CollectContentTargets", "could not seed local cover " + Local.string() + " (" + E + ")");
            }
        }
    }
    return true;
}

bool CollectRunnerChainTargets(const NodeIndex &Idx, const std::string &LaunchNodeId,
                               const nlohmann::ordered_json &GlobalConfigJSON,
                               std::vector<IpfsWrapper::FetchTarget> &Out, std::string *Error)
{
    // A downloaded game is only PLAYABLE once its runtime is present too — so a full-closure hydrate pools the resolved
    // runner CHAIN's build content (e.g. a Minecraft version's java_<major> JRE, or a wine game's Proton build) into the
    // same fetch batch. The game's own PARENTS closure (dgVoodoo/jlib_*/DirectPlay content nodes) is already covered by
    // CollectContentTargets — only the runner, which the closure walk skips (IsRunner), is added here. A native /
    // build-less runner (kNativeTerminalId, a PATH runner) contributes nothing. Best-effort: an unresolvable chain is
    // not fatal to the game content fetch (the launch itself will report a missing runner).
    const Node *Launch = Idx.Find(LaunchNodeId);
    if (!Launch) return true;
    ContainerParams Cp(Launch->BundleDir, LaunchNodeId, std::string());
    Cp.NodeIdx = &Idx; Cp.LaunchNodeId = LaunchNodeId; Cp.PackageUID = Launch->Uid;   // for the per-package runner pin lookup
    for (const std::string &RunnerId : LaunchResolver::ResolveChainIds(Idx, *Launch, Cp, GlobalConfigJSON))
    {
        if (RunnerId == LaunchResolver::kNativeTerminalId) continue;
        std::string E;
        if (!RunnerInstall::CollectRunnerNodeTargets(Idx, RunnerId, Out, &E))
            LogWarn("PackageCatalog::CollectRunnerChainTargets", "runner '" + RunnerId + "': " + E);
    }
    (void)Error;
    return true;
}

bool HydrateNode(const NodeIndex &Idx, const std::string &LaunchNodeId, const std::map<std::string, bool> &Toggles,
                 std::string *Error, const nlohmann::ordered_json *GlobalConfigJSON)
{
    std::vector<IpfsWrapper::FetchTarget> Targets;
    if (!CollectContentTargets(Idx, LaunchNodeId, Toggles, Targets, Error)) return false;
    // Full-closure: also pool the resolved runner chain's build so the game is immediately launchable (identity = the
    // node's Declare* layers; a game hydrate pulls the whole runtime, no separate "install the runner" step).
    if (GlobalConfigJSON) CollectRunnerChainTargets(Idx, LaunchNodeId, *GlobalConfigJSON, Targets, Error);
    if (!IpfsWrapper::FetchTargetsConcurrent(Targets, Error))
    { LogErr("PackageCatalog::HydrateNode", "hydrate failed for '" + LaunchNodeId + "'"); return false; }
    LogSucc("PackageCatalog::HydrateNode", "Hydrated node '" + LaunchNodeId + "' (" + std::to_string(Targets.size()) + " file(s)).");
    return true;
}

} // namespace PackageCatalog
