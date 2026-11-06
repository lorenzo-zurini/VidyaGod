#include "packagecatalog.h"
#include "sessions.h"
#include "nodestore.h"
#include "contentfile.h"
#include "builtinrunners.h"
#include "fold.h"   // Fold — a row's grafts are what its resolution offers
#include "packagecatalog_p.h"
#include "apppaths.h"
#include "instancestore.h"
#include "manifestmodel.h"
#include "nodegraph.h"        // gigagraph catalog: GatherWorkingTree / FreezeToIndex
#include "parallel.h"
#include <tuple>
#include "platform/platform.h"   // SelfExe — the program an index cache was built by
#include <optional>
#include <mutex>
#include "commonutils.h"
#include "jsonoperations.h"
#include "ipfswrapper.h"
#include "downloadqueue.h"      // EnqueueBatch/WaitBatch — closure completion rides the ONE rolling queue
#include "runnerwrapper.h"
#include "varsubst.h"           // %KEY% substitution — resolve CustomVar-templated content-layer PATHs for hydration
#include "launchresolver.h"     // ResolveChainIds — pool a game's resolved runner chain into its hydrate (full-closure)
#include "launchparams.h"       // ContainerParams (minimal, for the chain resolve)

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStringList>
#include <cstdint>
#include <map>
#include <mutex>
#include <condition_variable>
#include <set>
#include <deque>
#include <functional>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <vector>

using namespace ManifestModel;   // LayerLocator / IsVfsLayer / …

namespace PackageCatalog {
using namespace Shelf;       // the library walks rows and grafts as the plan sees them
using namespace Installed;

bool CollectRunnerNodeTargets(const NodeIndex &Idx, const std::string &RunnerNodeId,
                              std::vector<IpfsWrapper::FetchTarget> &Out, std::string *Error)
{
    const Node *R = Idx.Find(RunnerNodeId);
    if (!R || !R->IsRunner()) { if (Error) *Error = "not a runner node: " + RunnerNodeId; return false; }
    std::error_code Ec;
    for (const Node *C : Installed::RunnerBuildNodes(Idx, RunnerNodeId))
        for (const auto &L : C->ContentLayers)
        {
            if (!ManifestModel::IsRunnerBuildLayer(L)) continue;                   // NOT IsVfsLayer — see RunnerBuildPresent
            std::filesystem::path Local; std::string Cid;
            ManifestModel::LayerLocator(L, C->BundleDir, Local, Cid);             // against the layer's OWN node bundle
            if (Cid.empty() || Local == C->BundleDir || std::filesystem::exists(Local, Ec)) continue;  // present / no source
            Out.push_back({Cid, Local.string(), false});
        }
    return true;
}

// ----- per-package user settings -----

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

// CATALOG and ASSETS are top-level SIBLINGS of LIBRARY (beside it, whether LibraryRoot is default or overridden):
//   LIBRARY/ = what you've installed (hydrated, published, seeded)
//   CATALOG/ = received browse stubs (pinned node blocks, NOT published) — <Nick> - <Lib>/<pkg>/<node>.json
//   ASSETS/  = content-addressed shared files (covers) — <cid>, one per CID, shared by every tile that references it
// The catalog scan spans LIBRARY + CATALOG (merged index resolves a LIBRARY game's dep even when the dep is a
// CATALOG stub); publish scans LIBRARY only.
std::string CatalogRootDir(const nlohmann::ordered_json &GlobalConfigJSON)
{
    return QDir::cleanPath(QString::fromStdString(
        (std::filesystem::path(LibraryDir(GlobalConfigJSON)).parent_path() / "CATALOG").string())).toStdString();
}
std::string AssetsRootDir(const nlohmann::ordered_json &GlobalConfigJSON)
{
    return QDir::cleanPath(QString::fromStdString(
        (std::filesystem::path(LibraryDir(GlobalConfigJSON)).parent_path() / "ASSETS").string())).toStdString();
}

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
std::string PackageSourceCID(const nlohmann::ordered_json &S)
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
std::string PackageSourceDir(const nlohmann::ordered_json &GlobalConfigJSON, const nlohmann::ordered_json &S)
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
    if (BundleDir.empty()) return false;
    // A bundle under CATALOG is a RECEIVED share — published-by-default content, NOT a locally-added out-of-tree
    // package. A catalog tile can never be "Local". ("Local" = a bundle in LIBRARY that isn't from a CID source.)
    if (PathUnder(CatalogRootDir(GlobalConfigJSON), BundleDir)) return false;
    return !PathUnderManagedSource(GlobalConfigJSON, BundleDir);
}

std::string GoldenSince(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &PackageUid)
{
    if (PackageUid.empty() || !GlobalConfigJSON.contains("Settings") || !GlobalConfigJSON["Settings"].is_object()) return std::string();
    const auto &S = GlobalConfigJSON["Settings"];
    if (!S.contains("GoldenPackages") || !S["GoldenPackages"].is_object() || !S["GoldenPackages"].contains(PackageUid)) return std::string();
    const auto &V = S["GoldenPackages"][PackageUid];
    return V.is_string() && !V.get<std::string>().empty() ? V.get<std::string>() : std::string("golden");   // a hand-edited non-date still marks it
}

void SetGolden(nlohmann::ordered_json &GlobalConfigJSON, const std::string &PackageUid, bool On, const std::string &Date)
{
    if (PackageUid.empty()) return;
    if (!GlobalConfigJSON.contains("Settings") || !GlobalConfigJSON["Settings"].is_object()) GlobalConfigJSON["Settings"] = nlohmann::ordered_json::object();
    auto &S = GlobalConfigJSON["Settings"];
    if (!S.contains("GoldenPackages") || !S["GoldenPackages"].is_object()) S["GoldenPackages"] = nlohmann::ordered_json::object();
    if (On) S["GoldenPackages"][PackageUid] = Date;
    else S["GoldenPackages"].erase(PackageUid);
}

std::vector<std::string> LibraryNames(const nlohmann::ordered_json &GlobalConfigJSON)
{
    std::vector<std::string> Out;
    std::error_code Ec;
    for (const auto &E : std::filesystem::directory_iterator(LibraryDir(GlobalConfigJSON), Ec))
        if (E.is_directory(Ec)) Out.push_back(E.path().filename().string());
    std::sort(Out.begin(), Out.end());
    return Out;
}

std::string FolderTitle(const std::filesystem::path &Dir)
{
    std::string N = Dir.filename().string();
    while (!N.empty() && N[0] == '[')
    {
        const size_t E = N.find(']');
        if (E == std::string::npos) break;
        N = N.substr(E + 1);
        while (!N.empty() && N[0] == ' ') N.erase(0, 1);
    }
    return N.empty() ? Dir.filename().string() : N;
}

std::string PackageTitle(const std::filesystem::path &Dir)
{
    const std::string Name = Dir.filename().string();
    const size_t E = Name.find(']');
    if (Name.empty() || Name[0] != '[' || E == std::string::npos) return FolderTitle(Dir);
    const std::string Uid = Name.substr(1, E - 1);
    std::error_code Ec;
    for (const auto &F : std::filesystem::directory_iterator(Dir, Ec))
    {
        if (F.path().extension() != ".json" || !F.is_regular_file(Ec)) continue;
        std::ifstream In(F.path(), std::ios::binary);
        const std::string Text((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
        if (Text.find("\"TILE\"") == std::string::npos) continue;         // most nodes carry no tile: not parsed
        const nlohmann::ordered_json N = NodeGraph::ParseBounded(Text);
        if (!N.is_object() || !N.contains("LAYERS") || !N["LAYERS"].is_array()) continue;
        for (const auto &L : N["LAYERS"])
        {
            if (!L.is_object() || !L.contains("EXEC") || !L["EXEC"].is_array()) continue;
            for (const auto &En : L["EXEC"])
            {
                if (!En.is_object() || !En.contains("TILE") || !En["TILE"].is_object()) continue;
                const auto &T = En["TILE"];
                if (T.value("UID", std::string()) == Uid && T.contains("TITLE") && T["TITLE"].is_string()
                    && !T["TITLE"].get<std::string>().empty())
                    return T["TITLE"].get<std::string>();
            }
        }
    }
    return FolderTitle(Dir);
}

std::vector<AuthoredPackage> AuthoredPackages(const nlohmann::ordered_json &GlobalConfigJSON)
{
    std::vector<AuthoredPackage> Out;
    std::error_code Ec;
    for (const std::string &L : LibraryNames(GlobalConfigJSON))
        for (const auto &E : std::filesystem::directory_iterator(std::filesystem::path(LibraryDir(GlobalConfigJSON)) / L, Ec))
            if (E.is_directory(Ec)) Out.push_back({E.path(), PackageTitle(E.path()), L});
    for (const auto &D : LocalPackageDirs(GlobalConfigJSON)) Out.push_back({D, PackageTitle(D), std::string()});
    auto Lower = [](std::string T) { for (char &C : T) C = (char)std::tolower((unsigned char)C); return T; };
    std::sort(Out.begin(), Out.end(), [&](const AuthoredPackage &A, const AuthoredPackage &B) {
        const std::string La = Lower(A.Title), Lb = Lower(B.Title);
        return La != Lb ? La < Lb : A.Dir < B.Dir;
    });
    return Out;
}

namespace
{
//A folder name on every system: what Windows refuses becomes " -" for a colon (as in the library's other names)
//and "_" otherwise; no trailing dots or spaces.
std::string FolderSafe(const std::string &Title)
{
    std::string Safe;
    for (const char C : Title)
    {
        if (C == ':') Safe += " -";
        else if (std::string("/\\?*\"<>|").find(C) != std::string::npos || (unsigned char)C < 0x20) Safe += '_';
        else Safe += C;
    }
    while (!Safe.empty() && (Safe.back() == ' ' || Safe.back() == '.')) Safe.pop_back();
    while (!Safe.empty() && Safe.front() == ' ') Safe.erase(Safe.begin());
    return Safe;
}
}

std::filesystem::path CreatePackageFolder(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &Library,
                                          const std::string &Uid, const std::string &Title, std::string &Err)
{
    namespace fs = std::filesystem;
    const auto Names = LibraryNames(GlobalConfigJSON);
    if (std::find(Names.begin(), Names.end(), Library) == Names.end()) { Err = "There is no library named \"" + Library + "\"."; return {}; }
    if (Uid.empty() || !std::all_of(Uid.begin(), Uid.end(), [](unsigned char C) { return std::isdigit(C); }))
    { Err = "The game ID must be a number (its TheGamesDB ID)."; return {}; }
    const std::string Safe = FolderSafe(Title);
    if (Safe.empty()) { Err = "The game needs a title."; return {}; }
    const std::string Tag = "[" + Uid + "]";
    std::error_code Ec;
    for (const auto &L : Names)
        for (const auto &P : fs::directory_iterator(fs::path(LibraryDir(GlobalConfigJSON)) / L, Ec))
            if (P.path().filename().string().rfind(Tag, 0) == 0)
            { Err = "The library already has a package for game " + Uid + ": " + L + "/" + P.path().filename().string(); return {}; }
    const fs::path Dir = fs::path(LibraryDir(GlobalConfigJSON)) / Library / (Tag + "[v1.0] " + Safe);
    if (!fs::create_directories(Dir, Ec)) { Err = "Could not create " + Dir.string() + (Ec ? ": " + Ec.message() : std::string()); return {}; }
    return Dir;
}

std::filesystem::path CreateLibraryFolder(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &Library,
                                          const std::string &Name, std::string &Err)
{
    namespace fs = std::filesystem;
    const auto Names = LibraryNames(GlobalConfigJSON);
    if (std::find(Names.begin(), Names.end(), Library) == Names.end()) { Err = "There is no library named \"" + Library + "\"."; return {}; }
    const std::string Safe = FolderSafe(Name);
    if (Safe.empty() || Safe == "." || Safe == "..") { Err = "The library needs a name."; return {}; }
    const fs::path Dir = fs::path(LibraryDir(GlobalConfigJSON)) / Library / Safe;
    std::error_code Ec;
    if (fs::exists(Dir, Ec)) { Err = "The library already has a package named " + Library + "/" + Safe + "."; return {}; }
    if (!fs::create_directories(Dir, Ec)) { Err = "Could not create " + Dir.string() + (Ec ? ": " + Ec.message() : std::string()); return {}; }
    return Dir;
}

std::vector<std::filesystem::path> EditableRoots(const nlohmann::ordered_json &GlobalConfigJSON)
{
    std::vector<std::filesystem::path> Out{ std::filesystem::path(LibraryRootDir(GlobalConfigJSON)) };
    for (const auto &D : LocalPackageDirs(GlobalConfigJSON)) Out.push_back(D);
    return Out;
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
    ManifestModel::DeriveFacts(Idx);   // entries, runners and tiles are folded facts
    const Node *Rep = nullptr;                                                    // prefer a presentable launchable
    for (const auto &[NodeId, N] : Idx.Nodes)
    {
        if (N.IsVariant() && !N.IsGraft) { Id.HasLaunchable = true; if (!Rep || (N.Presentable() && !Rep->Presentable())) Rep = &N; }
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

//Upsert a LIBRARY entry for a static CID-source package: PATH + name + CIDSOURCE (its source's CID). Removal is
//explicit via RemovePackageSource (an immutable source's contents never change, so there is no reconcile/auto-prune).
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
        if (!SourceDirSynced(Dir, Ec)) return true;
    }
    return false;
}

bool SourceDirSynced(const std::string &Dir, std::error_code &Ec)
{
    //One predicate for "this source has been fetched", shared by the sync, the missing-sources check and
    //--download-all: a directory that exists, is READABLE and is non-empty. On any error Ec is set and the answer is
    //no — an unreadable dir is not synced, and the caller can say why.
    Ec.clear();
    if (!std::filesystem::is_directory(Dir, Ec) || Ec)
    {
        if (Ec == std::errc::no_such_file_or_directory) Ec.clear();   // not-yet-synced is a clean "no", not an error
        return false;
    }
    const bool Empty = std::filesystem::is_empty(Dir, Ec);
    return !Ec && !Empty;
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
        const bool Have = SourceDirSynced(Dir, Ec);
        if (!Have && Ec)
        {
            //A real filesystem error (EACCES, EIO — ENOENT is a clean "not yet synced") cannot be fixed by fetching:
            //materializing into an unreadable dir just fails the final rename, forever. Refuse loudly and skip.
            LogErr("PackageCatalog::SyncPackageSources", "Source dir " + Dir + " is unusable ("
                   + Ec.message() + ") — fix it and re-sync; skipping");
            continue;
        }
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

std::string LibraryEntryKey(const nlohmann::ordered_json &Entry)
{
    return Entry.is_object() ? Entry.value("PATH", std::string()) : std::string();
}

std::string SourceKey(const nlohmann::ordered_json &Source)
{
    if (Source.is_object() && Source.contains("NAME") && Source["NAME"].is_string() && !Source["NAME"].get<std::string>().empty())
        return "name:" + Source["NAME"].get<std::string>();
    return "cid:" + PackageSourceCID(Source);
}

void MergeListChange(nlohmann::ordered_json &Live, const nlohmann::ordered_json &Before, const nlohmann::ordered_json &After,
                     std::string (*Key)(const nlohmann::ordered_json &))
{
    if (!Live.is_array()) Live = nlohmann::ordered_json::array();
    const auto Items = [](const nlohmann::ordered_json &L) {
        std::vector<const nlohmann::ordered_json *> V;
        if (L.is_array()) for (const auto &E : L) V.push_back(&E);
        return V;
    };
    //What the worker took out and put in, entry by entry and by value (a list may hold two alike, or two of one key).
    std::vector<const nlohmann::ordered_json *> Gone = Items(Before), Came = Items(After);
    for (auto C = Came.begin(); C != Came.end();)
    {
        const auto G = std::find_if(Gone.begin(), Gone.end(), [&](const auto *X) { return *X == **C; });
        if (G != Gone.end()) { Gone.erase(G); C = Came.erase(C); } else ++C;
    }
    //A changed entry is one that went and one that came under one key: it is replaced where it stands.
    std::vector<int> Pair(Gone.size(), -1);
    std::vector<bool> Taken(Came.size(), false);
    for (size_t G = 0; G < Gone.size(); ++G)
        for (size_t C = 0; C < Came.size(); ++C)
            if (!Taken[C] && !Key(*Gone[G]).empty() && Key(*Gone[G]) == Key(*Came[C])) { Pair[G] = (int)C; Taken[C] = true; break; }
    std::vector<bool> Done(Came.size(), false);
    for (size_t G = 0; G < Gone.size(); ++G)
    {
        const auto It = std::find(Live.begin(), Live.end(), *Gone[G]);
        if (Pair[G] >= 0) Done[Pair[G]] = true;
        //Not in the live list as the worker saw it: removed or changed meanwhile — that change stands.
        if (It == Live.end()) continue;
        if (Pair[G] >= 0) *It = *Came[Pair[G]]; else Live.erase(It);
    }
    //A new entry another change already made (two syncs that each add a source's package): kept once, as the latest
    //change has it.
    for (size_t C = 0; C < Came.size(); ++C)
    {
        if (Done[C] || std::find(Live.begin(), Live.end(), *Came[C]) != Live.end()) continue;
        const std::string K = Key(*Came[C]);
        const auto Same = K.empty() ? Live.end() : std::find_if(Live.begin(), Live.end(), [&](const auto &E) { return Key(E) == K; });
        if (Same != Live.end()) *Same = *Came[C]; else Live.push_back(*Came[C]);
    }
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

namespace {
// Manifest files, relative to a source dir → their bytes. The manifest tree is what a collection CID actually
// addresses (Meta-CIDs are text-only), so this is the unit the upgrade diff compares.
std::map<std::string, std::string> ManifestTree(const std::string &Dir)
{
    namespace fs = std::filesystem;
    std::map<std::string, std::string> Tree;
    std::error_code Ec;
    if (!fs::is_directory(Dir, Ec)) return Tree;
    for (fs::recursive_directory_iterator It(Dir, fs::directory_options::skip_permission_denied, Ec), End;
         It != End; It.increment(Ec))
    {
        if (Ec) { Ec.clear(); continue; }
        if (!It->is_regular_file(Ec) || It->path().extension() != ".json") continue;
        std::ifstream F(It->path(), std::ios::binary);
        if (!F) continue;
        Tree[fs::relative(It->path(), Dir, Ec).generic_string()] =
            std::string(std::istreambuf_iterator<char>(F), std::istreambuf_iterator<char>());
    }
    return Tree;
}
// The top-level package folder of a manifest path ("[749][v1.0] Age of Empires II/aoe2_tc.json" → the bracketed
// folder). Comparing these across the two trees is the lineage evidence: a genuine upgrade shares most of them.
std::string TopFolder(const std::string &RelPath)
{
    const auto Slash = RelPath.find('/');
    return Slash == std::string::npos ? std::string() : RelPath.substr(0, Slash);
}
long long FileSize(const std::string &P)
{
    std::error_code Ec;
    const auto S = std::filesystem::file_size(P, Ec);
    return Ec ? 0LL : (long long)S;
}
}   // namespace

SourceUpgradePlan PlanSourceUpgrade(const nlohmann::ordered_json &GlobalConfigJSON,
                                    const std::string &SourceName, const std::string &NewCid, std::string *Error)
{
    namespace fs = std::filesystem;
    SourceUpgradePlan P;
    auto Fail = [&](const std::string &M) { if (Error) *Error = M; LogErr("PackageCatalog::PlanSourceUpgrade", M); return P; };
    if (NewCid.empty()) return Fail("empty CID");

    const nlohmann::ordered_json *Src = nullptr;
    if (GlobalConfigJSON.contains("Settings") && GlobalConfigJSON["Settings"].is_object()
        && GlobalConfigJSON["Settings"].contains("PackageSources"))
        for (const auto &S : GlobalConfigJSON["Settings"]["PackageSources"])
            if (PackageSourceName(S) == SourceName) { Src = &S; break; }
    if (!Src) return Fail("no package source named '" + SourceName + "'");

    P.Name   = SourceName;
    P.OldCid = PackageSourceCID(*Src);
    P.NewCid = NewCid;
    P.Dir    = PackageSourceDir(GlobalConfigJSON, *Src);
    if (P.OldCid == NewCid) return Fail("source '" + SourceName + "' is already at " + NewCid);

    const std::string Root = LibraryDir(GlobalConfigJSON);
    P.StagingDir    = Root + "/." + SourceName + ".upgrade";
    P.DeprecatedDir = Root + "/.deprecated/" + SourceName + "/" + (P.OldCid.empty() ? "unknown" : P.OldCid);

    // Fetch the NEW manifest tree to staging — never over the live dir, so a failed or partial fetch cannot leave a
    // half-upgraded source behind. (fetchDirOnce RemoveAll()s its destination, which is safe here and would be
    // catastrophic on the live dir: it holds the hydrated content.)
    std::error_code Ec;
    fs::remove_all(P.StagingDir, Ec);
    std::string FErr;
    if (IpfsWrapper::FetchDirToPath(NewCid, P.StagingDir, &FErr).empty())
        return Fail("could not fetch " + NewCid + ": " + FErr);

    const std::map<std::string, std::string> OldTree = ManifestTree(P.Dir);
    const std::map<std::string, std::string> NewTree = ManifestTree(P.StagingDir);
    if (NewTree.empty()) return Fail("fetched tree for " + NewCid + " contains no node files");

    std::set<std::string> OldPkgs, NewPkgs;
    for (const auto &[Rel, _] : OldTree) if (!TopFolder(Rel).empty()) OldPkgs.insert(TopFolder(Rel));
    for (const auto &[Rel, _] : NewTree) if (!TopFolder(Rel).empty()) NewPkgs.insert(TopFolder(Rel));
    P.OldPackages = (int)OldPkgs.size();
    P.NewPackages = (int)NewPkgs.size();
    for (const std::string &N : NewPkgs) if (OldPkgs.count(N)) ++P.SharedPackages;

    for (const auto &[Rel, Bytes] : NewTree)
    {
        const auto It = OldTree.find(Rel);
        if (It == OldTree.end())      P.JsonAdded.push_back(Rel);
        else if (It->second != Bytes) P.JsonChanged.push_back(Rel);
    }
    for (const auto &[Rel, _] : OldTree)
        if (!NewTree.count(Rel)) P.JsonRemoved.push_back(Rel);

    // Content diff. The OLD view is existing-only (what we actually hold); the NEW view must be the RECORDED view,
    // because the staged tree is manifests-only and every content file is absent by construction.
    const std::map<std::string, std::string> OldHave = ManifestTargets(P.Dir, false, true);
    const std::map<std::string, std::string> NewWant = ManifestTargets(P.StagingDir, false, false);

    std::map<std::string, std::string> HaveByCid;                       // CID → a local path holding those bytes
    for (const auto &[Path, Cid] : OldHave) HaveByCid.emplace(Cid, Path);

    std::set<std::string> StillReferenced;
    for (const auto &[StagedPath, Cid] : NewWant)
    {
        StillReferenced.insert(Cid);
        // Translate the staged path back into where the file will live under the real source dir.
        const std::string Rel  = fs::relative(StagedPath, P.StagingDir, Ec).generic_string();
        const std::string Dest = P.Dir + "/" + Rel;
        const auto Held = HaveByCid.find(Cid);
        if (Held == HaveByCid.end())          { P.ContentNew.push_back(Cid); continue; }   // hydrate on demand
        if (Held->second == Dest)             { P.ContentKeep[Dest] = Cid; P.KeptBytes += FileSize(Dest); continue; }
        // Same bytes, different location — a renamed package or a moved layer. Moving beats re-downloading.
        P.ContentMove[Cid] = { Held->second, Dest };
        P.KeptBytes += FileSize(Held->second);
    }
    for (const auto &[Path, Cid] : OldHave)
        if (!StillReferenced.count(Cid))
        { P.ContentDeprecate[Path] = Cid; P.DeprecatedBytes += FileSize(Path); }

    P.Valid = true;
    return P;
}

bool ApplySourceUpgrade(nlohmann::ordered_json &GlobalConfigJSON, const SourceUpgradePlan &P,
                        bool Force, std::string *Error)
{
    namespace fs = std::filesystem;
    auto Fail = [&](const std::string &M) { if (Error) *Error = M; LogErr("PackageCatalog::ApplySourceUpgrade", M); return false; };
    if (!P.Valid) return Fail("invalid plan");

    // A CID carries no ancestry, so a typo'd or unrelated collection looks exactly like a legitimate upgrade — except
    // that it shares no packages. Applying it would deprecate the ENTIRE library in one step.
    if (P.SharedPackages == 0 && P.OldPackages > 0 && !Force)
        return Fail("'" + P.NewCid + "' shares no packages with the current '" + P.Name + "' ("
                    + std::to_string(P.OldPackages) + " packages) — this looks like a DIFFERENT collection, not an "
                    "upgrade. Re-run with force to apply anyway.");

    std::error_code Ec;

    // 1. Demote the old version. Its manifests are COPIED (not moved) so the old collection CID keeps resolving; the
    //    content the new tree no longer wants is MOVED, since keeping two copies of gigabytes is not a kindness.
    if (!P.JsonRemoved.empty() || !P.ContentDeprecate.empty() || !P.JsonChanged.empty())
    {
        fs::create_directories(P.DeprecatedDir, Ec);
        for (const auto &[Rel, _] : ManifestTree(P.Dir))
        {
            const fs::path Dest = fs::path(P.DeprecatedDir) / Rel;
            fs::create_directories(Dest.parent_path(), Ec);
            fs::copy_file(fs::path(P.Dir) / Rel, Dest, fs::copy_options::overwrite_existing, Ec);
        }
        for (const auto &[Path, Cid] : P.ContentDeprecate)
        {
            const std::string Rel = fs::relative(Path, P.Dir, Ec).generic_string();
            const fs::path Dest = fs::path(P.DeprecatedDir) / Rel;
            fs::create_directories(Dest.parent_path(), Ec);
            fs::rename(Path, Dest, Ec);
            if (Ec) { Ec.clear(); continue; }
            // The reference still points at the OLD path, which no longer exists. Re-pointing needs the closure
            // dropped first — a plain re-add dedup-skips and would leave us advertising an unreadable block.
            IpfsWrapper::DropRef(Cid);
            std::string AddErr;
            if (IpfsWrapper::AddNoCopy(Dest.string(), &AddErr).empty())
                LogWarn("PackageCatalog::ApplySourceUpgrade", "deprecated content not re-seeded: " + Dest.string() + " (" + AddErr + ")");
        }
        // Re-point the OLD collection meta-CID at the preserved copy so peers still on it keep being served.
        if (!P.OldCid.empty())
        {
            IpfsWrapper::DropRef(P.OldCid);
            std::string AddErr;
            const std::string Got = IpfsWrapper::AddNoCopyMeta(P.DeprecatedDir, &AddErr);
            if (Got != P.OldCid)
                LogWarn("PackageCatalog::ApplySourceUpgrade",
                        "deprecated copy of '" + P.Name + "' re-seeded as " + (Got.empty() ? "nothing" : Got)
                        + ", expected " + P.OldCid + " — peers on the old CID may not be served");
        }
    }

    // 2. Relocate content that merely moved. BEFORE any deletion, or a renamed package's content is lost.
    for (const auto &[Cid, FromTo] : P.ContentMove)
    {
        fs::create_directories(fs::path(FromTo.second).parent_path(), Ec);
        fs::rename(FromTo.first, FromTo.second, Ec);
        if (Ec) { Ec.clear(); LogWarn("PackageCatalog::ApplySourceUpgrade", "could not move " + FromTo.first); continue; }
        IpfsWrapper::DropRef(Cid);
        std::string AddErr;
        IpfsWrapper::AddNoCopy(FromTo.second, &AddErr);
    }

    // 3. Swap the manifests: drop the ones the new tree no longer has, then copy the new tree over. Only *.json is
    //    touched — hydrated content, USERDATA, RUNTIME and anything else the user owns is left exactly as it is.
    for (const std::string &Rel : P.JsonRemoved) fs::remove(fs::path(P.Dir) / Rel, Ec);
    for (const auto &[Rel, _] : ManifestTree(P.StagingDir))
    {
        const fs::path Dest = fs::path(P.Dir) / Rel;
        fs::create_directories(Dest.parent_path(), Ec);
        fs::copy_file(fs::path(P.StagingDir) / Rel, Dest, fs::copy_options::overwrite_existing, Ec);
        if (Ec) { Ec.clear(); return Fail("could not write manifest " + Dest.string()); }
    }

    // 4. The staged fetch pinned NewCid with references into the STAGING dir, which is about to be deleted. Re-point
    //    it at the live dir — drop first, or the re-add dedup-skips and the refs keep aiming at the removed staging.
    IpfsWrapper::DropRef(P.NewCid);
    std::string AddErr;
    const std::string Got = IpfsWrapper::AddNoCopyMeta(P.Dir, &AddErr);
    if (Got != P.NewCid)
        LogWarn("PackageCatalog::ApplySourceUpgrade",
                "re-seeded '" + P.Name + "' as " + (Got.empty() ? "nothing" : Got) + ", expected " + P.NewCid
                + " — the source dir holds files the published tree does not");
    fs::remove_all(P.StagingDir, Ec);

    // 5. Point config at the new CID, and drop LIBRARY rows for packages that no longer exist so the catalog matches.
    auto &Sources = GlobalConfigJSON["Settings"]["PackageSources"];
    for (auto &S : Sources)
        if (PackageSourceName(S) == P.Name) { if (S.is_object()) S["CID"] = P.NewCid; else S = nlohmann::ordered_json{{"NAME", P.Name}, {"CID", P.NewCid}}; }
    if (GlobalConfigJSON.contains("LIBRARY") && GlobalConfigJSON["LIBRARY"].is_array())
    {
        auto &Lib = GlobalConfigJSON["LIBRARY"];
        for (auto It = Lib.begin(); It != Lib.end(); )
        {
            const std::string Pth = It->is_object() ? It->value("PATH", std::string()) : std::string();
            const bool Under = !Pth.empty() && PathUnder(P.Dir, Pth);
            It = (Under && !fs::is_directory(Pth, Ec)) ? Lib.erase(It) : std::next(It);
        }
    }

    LogSucc("PackageCatalog::ApplySourceUpgrade",
            "'" + P.Name + "' " + (P.OldCid.empty() ? "(none)" : P.OldCid) + " → " + P.NewCid);
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
    return Cids;
}


void RemovePackageSource(nlohmann::ordered_json &GlobalConfigJSON, int Index, bool KeepInstalled)
{
    if (!GlobalConfigJSON.contains("Settings") || !GlobalConfigJSON["Settings"].is_object()) return;
    auto &S = GlobalConfigJSON["Settings"];
    if (!S.contains("PackageSources") || !S["PackageSources"].is_array()) return;
    auto &Arr = S["PackageSources"];
    if (Index < 0 || Index >= (int)Arr.size()) return;

    const std::string Dir = PackageSourceDir(GlobalConfigJSON, Arr[Index]);

    if (KeepInstalled)
    {
        // Never rm a package the user has INSTALLED: dirs with hydrated content stay as local packages (files kept);
        // only stub-only packages go. No un-seed here: stubs seed nothing, and a kept install must stay seeded. The
        // source dir itself is removed only if it ends up empty.
        if (GlobalConfigJSON.contains("LIBRARY") && GlobalConfigJSON["LIBRARY"].is_array())
        {
            auto &Lib = GlobalConfigJSON["LIBRARY"];
            for (int i = (int)Lib.size() - 1; i >= 0; --i)
            {
                if (!Lib[i].is_object()) continue;
                const std::string P = Lib[i].value("PATH", std::string());
                if (P.empty() || !PathUnder(Dir, P)) continue;
                if (DirHasContent(P)) continue;                         // installed → kept, a local package
                else { std::error_code EcP; std::filesystem::remove_all(P, EcP); Lib.erase(Lib.begin() + i); }
            }
        }
        std::error_code EcD; std::filesystem::remove(Dir, EcD);          // removes the source dir only if now empty
        Arr.erase(Arr.begin() + Index);
        return;
    }

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

// The library a bundle belongs to: the first path segment of its dir relative to the LIBRARY root — i.e. the named
// collection dir it lives under. "" if the package sits directly under the root (no collection). A "library" is such
// a named dir; a package's library is chosen at publish time (the editor writes it into that dir).
std::string LibraryOf(const std::filesystem::path &BundleDir, const std::filesystem::path &Root)
{
    std::error_code Ec;
    const std::filesystem::path Rel = std::filesystem::relative(BundleDir, Root, Ec);
    if (Ec || Rel.empty()) return {};
    auto It = Rel.begin();
    const std::filesystem::path First = *It;
    if (++It == Rel.end()) return {};   // one segment = the package dir itself → no enclosing collection
    return First.string();
}

// Where a received package folder lands inside its package dir (NodeGraph::kPackageFolderDir — its nodes' bundle is
// the package dir).
static constexpr const char *kPackageFolderDir = NodeGraph::kPackageFolderDir;

nlohmann::ordered_json SharedWith(const nlohmann::ordered_json &Libraries, const std::vector<std::string> &SharedLibs)
{
    nlohmann::ordered_json Out = nlohmann::ordered_json::object();
    if (!Libraries.is_object()) return Out;
    std::set<std::pair<std::string, std::string>> Taken;
    std::deque<std::pair<std::string, std::string>> Todo;
    auto Row = [&](const std::string &L, const std::string &P) -> const nlohmann::ordered_json * {
        if (!Libraries.contains(L) || !Libraries[L].is_array()) return nullptr;
        for (const auto &E : Libraries[L]) if (E.is_object() && E.value("pkg", std::string()) == P) return &E;
        return nullptr;
    };
    for (const std::string &L : SharedLibs)
        if (Libraries.contains(L) && Libraries[L].is_array())
            for (const auto &E : Libraries[L])
                if (E.is_object() && !E.value("cid", std::string()).empty()) Todo.emplace_back(L, E.value("pkg", std::string()));
    while (!Todo.empty())
    {
        const auto Key = Todo.front(); Todo.pop_front();
        const nlohmann::ordered_json *E = Row(Key.first, Key.second);
        if (!E || !Taken.insert(Key).second) continue;
        if (!Out.contains(Key.first)) Out[Key.first] = nlohmann::ordered_json::array();
        Out[Key.first].push_back(*E);
        if (E->contains("needs") && (*E)["needs"].is_array())
            for (const auto &N : (*E)["needs"])
                if (N.is_object()) Todo.emplace_back(N.value("lib", std::string()), N.value("pkg", std::string()));
    }
    return Out;
}

std::vector<std::string> PublishLibrary(nlohmann::ordered_json &Config, std::string *Error, std::string *Gaps)
{
    const std::filesystem::path Root = LibraryRootDir(Config);
    std::map<std::string, nlohmann::ordered_json> Tree;
    std::map<std::string, std::filesystem::path>  Dirs;
    NodeGraph::GatherWorkingTree(Root, Tree, Dirs, /*SkipReserved=*/true);   // never mint a received friend stub
    if (Tree.empty()) { if (Error) *Error = "no packages to publish under " + Root.string(); return {}; }
    NodeStore::MintResult MR;
    if (!NodeStore::Mint(Tree, MR, Error)) return {};   // BlockPut every node → pinned + announced + seedable

    // Re-stabilise the on-disk tree at the freshly-minted identities: stamp each node's new CID + remap its refs.
    // Non-fatal — the blocks are already stored; a failed write-back only leaves the on-disk handles stale (freeze
    // still remaps them next time), and the editor's "edited since publish" badge is the only thing degraded.
    if (std::string WbErr; StampNodeCids(Root, MR.HandleToCid, &WbErr) < 0)
        LogWarn("PackageCatalog::PublishLibrary", "could not write minted CIDs back to the working tree: " + WbErr
                + " — handles left stale; re-run Verify & Publish");

    // SHARING IS PER PACKAGE. Every node of every bundle dir is shared with whoever gets its library — there is no
    // per-node flag (a forgotten one was an unshared game with nothing anywhere saying so). Each package is ONE UnixFS
    // folder of its node files (<cid>.json, the canonical bytes) — nothing else: the folder's own listing IS its node
    // list, and its name travels in the share entry. The folder is the unit a receiver lands (verbatim, like any
    // folder), the unit a pinning service pins (the package's own bytes; only a changed package re-pins), and
    // `Libraries` = {lib: [{cid: <package folder>, pkg: <dir>}]} is what the share sheet pushes to a friend. Off-IPFS
    // VidyaGod app data (friend channel).
    std::map<std::string, std::map<std::string, std::vector<std::string>>> ByLibPkg;   // lib → package dir → node cids
    std::map<std::string, std::map<std::string, std::vector<std::string>>> HandlesOf;  // lib → package dir → node handles
    std::vector<std::string> Unfrozen;                  // "<package>/<label>" of nodes Mint skipped
    std::map<std::string, size_t> Unheld;               // package → content CIDs it names that are not held here
    std::map<std::string, size_t> Unsourced;            // package → content layers that name no CID: no one can fetch them
    std::map<std::string, std::string> FirstPkg;        // node cid → the package that listed it first (its home)
    std::vector<std::string> Copies;                    // "<package>/<label>" of a node already listed by another package
    for (const auto &[Handle, Doc] : Tree)
    {
        const auto CidIt = MR.HandleToCid.find(Handle);
        if (CidIt == MR.HandleToCid.end())             // node was skipped (dangling/bad) — not shareable
        {
            Unfrozen.push_back(Dirs[Handle].filename().string() + "/" + Doc.value("LABEL", std::string("?")));
            continue;
        }
        std::error_code Ec;
        const std::filesystem::path Rel = std::filesystem::relative(Dirs[Handle], Root, Ec);
        std::vector<std::string> Seg;
        for (const auto &Part : Rel) if (Part != "." && !Part.empty()) Seg.push_back(Part.string());
        if (Ec || Seg.empty()) continue;
        const std::string LibName = Seg.size() >= 2 ? Seg[0] : std::string("Library");   // the collection dir
        const std::string Pkg     = Seg.size() >= 2 ? Seg[1] : Seg[0];                    // the bundle dir under it
        //One home: a node another package already lists is a copy — not shared again from here, and named below.
        if (const auto [F, Fresh] = FirstPkg.emplace(CidIt->second, LibName + "/" + Pkg); !Fresh && F->second != LibName + "/" + Pkg)
        {
            Copies.push_back(Pkg + "/" + Doc.value("LABEL", std::string("?")));
            continue;
        }
        ByLibPkg[LibName][Pkg].push_back(CidIt->second);
        HandlesOf[LibName][Pkg].push_back(Handle);
        //A file a layer names with no SOURCE is shared as a name only: a friend sees the game and can never fetch it.
        //A %variable% path is placed at launch, a DIR is local by nature (the validator's) — neither is counted here.
        if (Doc.contains("LAYERS") && Doc["LAYERS"].is_array())
            for (const auto &L : Doc["LAYERS"])
                for (const char *K : { "ZIP", "DELTA", "FILE" })
                    if (L.is_object() && L.contains(K) && L[K].is_string() && !L.contains("SOURCE")
                        && L[K].get<std::string>().find('%') == std::string::npos)
                        ++Unsourced[Pkg];
    }
    //Each node's home package, and from it what each package NEEDS: the other packages its nodes contain. A share is
    //closed over this (SharedWith): a friend given a game also gets the library packages it contains — never a copy.
    std::map<std::string, std::pair<std::string, std::string>> HomeOf;                 // node cid → (lib, package)
    for (const auto &[LibName, Pkgs] : ByLibPkg)
        for (const auto &[Pkg, Cids] : Pkgs)
            for (const auto &C : Cids) HomeOf.emplace(C, std::make_pair(LibName, Pkg));
    // A bounded display title for the receiver's dir segment (NAME_MAX): the dir basename, cut at a UTF-8 boundary. It
    // is also the package's name in the share entry, so "needs" names packages the same way.
    auto TitleOf = [](std::string Title) {
        if (Title.size() > 120)
        {
            Title.resize(120);
            while (!Title.empty() && (static_cast<unsigned char>(Title.back()) & 0xC0) == 0x80) Title.pop_back();
        }
        return Title;
    };
    auto Needs = [&](const std::string &LibName, const std::string &Pkg) {
        std::set<std::pair<std::string, std::string>> Out;
        for (const std::string &H : HandlesOf[LibName][Pkg])
            for (const std::string &R : ManifestModel::NodeRefs(Tree.at(H)))
            {
                const auto C = MR.HandleToCid.find(R);
                const auto Home = HomeOf.find(C != MR.HandleToCid.end() ? C->second : R);
                if (Home != HomeOf.end() && Home->second != std::make_pair(LibName, Pkg)) Out.insert(Home->second);
            }
        nlohmann::ordered_json J = nlohmann::ordered_json::array();
        for (const auto &[L, P] : Out) J.push_back({ {"lib", L}, {"pkg", TitleOf(P)} });
        return J;
    };
    nlohmann::ordered_json Libs = nlohmann::ordered_json::object();
    nlohmann::ordered_json Flat = nlohmann::ordered_json::array();
    size_t PkgCount = 0;
    for (auto &[LibName, Pkgs] : ByLibPkg)
    {
        for (auto &[Pkg, Cids] : Pkgs)
        {
            std::sort(Cids.begin(), Cids.end());
            Cids.erase(std::unique(Cids.begin(), Cids.end()), Cids.end());
            std::map<std::string, std::string> Entries;
            for (const auto &C : Cids) Entries[C + ".json"] = C;
            std::string MErr;
            const std::string PCid = IpfsWrapper::MakeDir(Entries, &MErr);
            if (PCid.empty()) { if (Error) *Error = "package folder for '" + Pkg + "' not made: " + MErr; return {}; }
            // The PIN folder: the package folder + the package's own content, linked — what a pinning service pins,
            // so it holds the package whole (references inside a node are plain CIDs: nothing follows them). A
            // library the package contains is its own package with its own pin: no byte is billed twice.
            std::map<std::string, std::string> Content;
            for (const std::string &H : HandlesOf[LibName][Pkg])
                for (const std::string &C : NodeContentCids(Tree.at(H))) Content[C] = C;
            std::map<std::string, std::string> Pin{ {"package", PCid} };
            if (!Content.empty())
            {   // the folder of the content held whole (every block, every backing file); the rest is named, not linked
                std::vector<std::string> NotWhole;
                const std::string CCid = IpfsWrapper::MakeDir(Content, &MErr, &NotWhole);
                if (CCid.empty() && NotWhole.size() != Content.size())
                { if (Error) *Error = "content folder for '" + Pkg + "' not made: " + MErr; return {}; }
                for (const std::string &C : NotWhole)
                {
                    ++Unheld[Pkg];
                    LogWarn("PackageCatalog::PublishLibrary", "package '" + Pkg + "': content " + C
                            + " is not held whole here (missing blocks or a moved/removed file) — its pin leaves it"
                              " out (seed it, then publish again)");
                }
                if (!CCid.empty()) Pin["content"] = CCid;
            }
            const std::string PinCid = IpfsWrapper::MakeDir(Pin, &MErr);
            if (PinCid.empty()) { if (Error) *Error = "pin folder for '" + Pkg + "' not made: " + MErr; return {}; }
            const std::string Title = TitleOf(Pkg);
            Libs[LibName].push_back(nlohmann::ordered_json{{"cid", PCid}, {"pin", PinCid}, {"node", Title}, {"pkg", Title},
                                                            {"title", Title}, {"nodes", Cids.size()},
                                                            {"needs", Needs(LibName, Pkg)}});
            Flat.push_back(PCid);
            ++PkgCount;
        }
    }
    // No library-level block: a library is a NAME that groups packages in the share sheet, never a CID — a CID over
    // the whole library would re-mint on every change to any package, and the package is the unit that changes.
    Config["Libraries"] = std::move(Libs);
    Config.erase("PublishedList");       // what the share list once was kept as: it is the return value, and Libraries
    Config.erase("PublishedManifest");
    LogSucc("PackageCatalog::PublishLibrary", "published " + std::to_string(MR.HandleToCid.size())
            + " node block(s) in " + std::to_string(PkgCount) + " package folder(s), "
            + std::to_string(Config["Libraries"].size()) + " library(ies)");
    // The verdict names what did not publish whole: a partial publish must never read as a clean one.
    std::string Gap;
    const auto List = [](const std::vector<std::string> &V) {
        std::string S;
        for (size_t I = 0; I < V.size() && I < 5; ++I) S += (I ? ", " : "") + V[I];
        return V.size() > 5 ? S + " and " + std::to_string(V.size() - 5) + " more" : S;
    };
    if (!Unfrozen.empty())
        Gap += std::to_string(Unfrozen.size()) + " node(s) did not freeze and were left out (" + List(Unfrozen) + ")";
    if (!Copies.empty())
        Gap += (Gap.empty() ? "" : "; ") + std::to_string(Copies.size()) + " node(s) are copies of a node another "
               "package holds (a node has one home: delete the copy, contain the node by CID): " + List(Copies);
    if (!Unheld.empty())
    {
        std::vector<std::string> P;
        for (const auto &[Pkg, N] : Unheld) P.push_back(Pkg + " (" + std::to_string(N) + ")");
        Gap += (Gap.empty() ? "" : "; ") + std::to_string(Unheld.size())
               + " package(s) name content this machine does not hold — no one can fetch it from here or from a pin: "
               + List(P);
    }
    if (!Unsourced.empty())
    {
        std::vector<std::string> P;
        for (const auto &[Pkg, N] : Unsourced) P.push_back(Pkg + " (" + std::to_string(N) + ")");
        Gap += (Gap.empty() ? "" : "; ") + std::to_string(Unsourced.size())
               + " package(s) have content files that name no SOURCE — a friend sees them and can never fetch them: "
               + List(P);
    }
    if (!Gap.empty()) LogErr("PackageCatalog::PublishLibrary", "published with gaps: " + Gap);
    if (Gaps) *Gaps = Gap;
    return Flat.get<std::vector<std::string>>();   // the share list: one package folder per package
}

// Bounds for the received-share planner (a friend's snapshot is UNTRUSTED input).
static constexpr size_t kMaxFriendRootsPerLib = 20000;    // cap a hostile/huge per-library item set (Go caps at 100k)
static constexpr size_t kMaxFriendRootsTotal  = 100000;   // snapshot-wide cap across all of one peer's libraries

// True if a dir holds any HYDRATED content (a file that isn't a node *.json) — i.e. the user has INSTALLED this
// package. Source removal must never delete such a dir: a withdrawal (or a transient failure) must never rm a
// user's installed game.
bool DirHasContent(const std::string &Dir)
{
    std::error_code Ec;
    if (!std::filesystem::is_directory(Dir, Ec)) return false;
    for (auto It = std::filesystem::recursive_directory_iterator(Dir, Ec);
         !Ec && It != std::filesystem::recursive_directory_iterator(); It.increment(Ec))
    {
        if (It->is_regular_file(Ec))
        {
            const std::string Name = It->path().filename().string();
            if (Name.size() < 5 || Name.compare(Name.size() - 5, 5, ".json") != 0) return true;
        }
    }
    return Ec ? true : false;   // couldn't fully inspect (permission/IO) → assume content; never rm what we can't read
}

// One path segment from UNTRUSTED input (a friend's snapshot: a library name, a package dir, a CID): plain filename
// characters only — no separator, no traversal — capped under NAME_MAX (a within-bounds snapshot must still yield
// satisfiable dests, or the queue retries an ENAMETOOLONG mkdir forever), never empty and never "." or "..".
static std::string SafeSegment(const std::string &In)
{
    std::string O;
    for (char c : In) O.push_back((std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.'
                                   || c == ' ' || c == '[' || c == ']' || c == '(' || c == ')') ? c : '_');
    if (O.size() > 120) O.resize(120);
    if (O.find_first_not_of('.') == std::string::npos) O = std::string(O.size() ? O.size() : 1, '_');   // "", ".", ".."
    //Names Windows cannot hold as given — the same on every platform, so every receiver lands the same name: a trailing
    //'.' or ' ' is silently dropped there (a folder "nick - .." is created as "nick -", and every path through the
    //name the plan holds then misses it), and a device name (CON, NUL, COM1, …, with any extension) is no file at all.
    for (size_t I = O.size(); I > 0 && (O[I - 1] == '.' || O[I - 1] == ' '); --I) O[I - 1] = '_';
    std::string Stem = O.substr(0, O.find('.'));
    while (!Stem.empty() && Stem.back() == ' ') Stem.pop_back();
    for (char &C : Stem) C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
    static const std::set<std::string> Devices = { "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9" };
    if (Devices.count(Stem)) O = "_" + O;
    return O;
}

std::vector<IpfsWrapper::FetchTarget> ReceivedFetchTargets(const std::vector<ReceivedFetch> &Plan)
{
    std::vector<IpfsWrapper::FetchTarget> Out;
    Out.reserve(Plan.size());
    for (const ReceivedFetch &T : Plan)
    {
        IpfsWrapper::DropRedirectsUnder(std::filesystem::path(T.Dest).parent_path().string());
        Out.push_back(IpfsWrapper::FetchTarget{ T.Cid, T.Dest, /*Optional=*/true, /*Dir=*/true, /*Verify=*/true });
    }
    return Out;
}

std::vector<ReceivedFetch> PlanReceivedFetches(const nlohmann::ordered_json &GlobalConfigJSON,
                                               const std::string &NickLabel, const nlohmann::ordered_json &Libs)
{
    namespace fs = std::filesystem;
    // UNTRUSTED input (a friend's snapshot): every path segment goes through SafeSegment — a plain filename, no
    // separators, never "", "." or ".." (a collision-qualified "X (<cid>)" keeps its parentheses, so adopting it and
    // re-planning agree on the name).
    std::vector<ReceivedFetch> Out;
    if (!Libs.is_object()) return Out;
    const fs::path Root = fs::path(CatalogRootDir(GlobalConfigJSON));   // received stubs live in CATALOG, not LIBRARY
    std::map<std::string, std::string> DestCid;   // dest → the CID that claimed it (the same CID twice = one target)
    auto Add = [&](const std::string &Cid, fs::path Dest, const std::string &LibName) {
        if (Cid.empty() || Cid.size() > 128) return;
        // Two packages sharing a dir name map to the same dir: the second gets a CID-qualified name instead of being
        // silently dropped.
        if (auto It = DestCid.find(Dest.string()); It != DestCid.end())
        {
            if (It->second == Cid) return;
            Dest = Dest.parent_path() / (Dest.filename().string() + " (" + Cid.substr(0, 12) + ")");
            if (DestCid.count(Dest.string())) return;
        }
        DestCid[Dest.string()] = Cid;   // claimed even when installed: the name the other package gets depends on it
        // A package the library already holds under this library and dir name was INSTALLED (adopted out of
        // CATALOG) or authored here: it is ours now and is not re-landed as a stub beside itself (a node has one
        // home; a stub beside it would hold copies). Its FINAL name: the unqualified one said "installed" for both of
        // two colliding packages, or re-planned the installed one.
        std::error_code Ec;
        if (fs::is_directory(fs::path(LibraryRootDir(GlobalConfigJSON)) / SafeSegment(LibName) / Dest.filename(), Ec)) return;
        Out.push_back(ReceivedFetch{Cid, (Dest / kPackageFolderDir).generic_string()});   // '/' on every host: compared as data
    };
    size_t Total = 0;   // snapshot-wide bound (a hostile friend could send many libs x many items)
    for (const auto &[LibName, Items] : Libs.items())
    {
        if (!Items.is_array()) continue;
        const fs::path LibDir = Root / SafeSegment(NickLabel + " - " + LibName);
        size_t PerLib = 0;
        for (const auto &It : Items)
        {
            if (Total >= kMaxFriendRootsTotal)
            {
                LogWarn("PackageCatalog::PlanReceivedFetches", "snapshot exceeds "
                        + std::to_string(kMaxFriendRootsTotal) + " total items — ignoring the rest");
                return Out;
            }
            if (++PerLib > kMaxFriendRootsPerLib)
            {
                LogWarn("PackageCatalog::PlanReceivedFetches", "library '" + LibName + "' exceeds "
                        + std::to_string(kMaxFriendRootsPerLib) + " items — capping");
                break;
            }
            ++Total;
            if (!It.is_object()) continue;
            const std::string Cid = It.value("cid", std::string());
            if (Cid.empty()) continue;
            // One entry = one PACKAGE FOLDER: it lands as <pkg dir>/.package/ — every node file verbatim — a dir
            // the fetch owns whole (a re-publish replaces it), while the package's
            // content hydrates beside it into <pkg dir>/. The seeder's real dir name keeps a multi-game package ONE
            // dir; a snapshot without it falls back to the CID.
            std::string PkgSeg = It.value("pkg", std::string());
            if (PkgSeg.empty()) PkgSeg = It.value("node", std::string());
            if (PkgSeg.empty()) PkgSeg = Cid.substr(0, 12);
            Add(Cid, LibDir / SafeSegment(PkgSeg), LibName);
        }
    }
    return Out;
}

//Removes a landed node file, and the node's reference to it when that was the reference: a fetched node file is
//referenced in place, so deleting it without this left an orphaned reference — hundreds per receive, each an
//"Errored: missing files" row and a re-seeding heal pass. A reference to ANOTHER copy of the block (the same node
//landed in two packages) is not orphaned by this removal and stays.
static void RemoveLandedNode(const std::filesystem::path &F, const std::string &Cid,
                             const std::vector<std::filesystem::path> &Elsewhere = {})
{
    std::error_code Ec;
    if (!std::filesystem::remove(F, Ec)) return;
    if (!IpfsWrapper::CidMissing(Cid)) return;                       // its reference is to another copy: nothing to do
    IpfsWrapper::DropRef(Cid);                                       // it was to this file: gone with it
    //The same node landed in another package too (a node two packages share): seed that copy, or it would be on disk
    //and silently not held — never served, never re-shared.
    for (const std::filesystem::path &S : Elsewhere)
        if (S != F && std::filesystem::is_regular_file(S, Ec))
        {
            std::string AErr;
            if (IpfsWrapper::AddNoCopy(S.string(), &AErr) == Cid) return;
        }
}

std::string ReceivedNickLabel(const std::string &PeerID)
{
    for (const auto &C : IpfsWrapper::FriendList()) if (C.PeerID == PeerID && !C.Nick.empty()) return C.Nick;
    return PeerID.size() > 8 ? PeerID.substr(PeerID.size() - 8) : PeerID;   // never an empty dir prefix
}

// Where each friend's share entries landed: the planner's own routing, replayed (landed folder dir → folder CID). Exact
// per friend — two friends sharing the same library and package name land in different dirs, and each dir is judged
// against its own friend's folder, never the other's.
static std::map<std::filesystem::path, std::string> LandedFolders(const nlohmann::ordered_json &Config)
{
    std::map<std::filesystem::path, std::string> Out;
    if (!Config.contains("FriendLibraries") || !Config["FriendLibraries"].is_object()) return Out;
    for (const auto &[Peer, Libs] : Config["FriendLibraries"].items())
        for (const ReceivedFetch &T : PlanReceivedFetches(Config, ReceivedNickLabel(Peer), Libs))
            Out[std::filesystem::path(T.Dest)] = T.Cid;
    return Out;
}

// A folder's node files — its listing's "<cid>.json" entries — read once per folder CID: a folder never changes (its
// CID is its content), and the receiver asks on every catalog pass. Empty optional: the folder is not held here.
static std::optional<std::vector<std::string>> FolderNodes(const std::string &FolderCid, std::string *Error)
{
    static std::mutex Mu;
    static std::map<std::string, std::vector<std::string>> Cache;
    {
        std::lock_guard<std::mutex> L(Mu);
        if (auto It = Cache.find(FolderCid); It != Cache.end()) return It->second;
    }
    std::map<std::string, std::string> Entries;
    if (!IpfsWrapper::DirEntries(FolderCid, Entries, Error)) return std::nullopt;
    std::vector<std::string> Cids;
    for (const auto &[Name, C] : Entries)
    {
        if (C.empty() || C.size() > 128 || Name != C + ".json") continue;   // a node file is named by its CID
        bool Plain = true;
        for (unsigned char ch : C) if (!std::isalnum(ch)) { Plain = false; break; }   // a CID is base32/58: a path is not
        if (Plain) Cids.push_back(C);
    }
    std::lock_guard<std::mutex> L(Mu);
    if (Cache.size() > 20000) Cache.clear();
    Cache[FolderCid] = Cids;
    return Cids;
}

// Says once per dir and reason why a landed package is not completed or pruned — silence here was a package quietly
// frozen. Informational: a folder not held yet is the normal moment after a friend re-publishes (every package's
// folder changes at once), not a fault. Keyed without the folder CID, so re-mints neither repeat it nor grow the set.
static void ReportUnjudged(const std::filesystem::path &Folder, const std::string &Why, const std::string &Detail)
{
    static std::mutex Mu;
    static std::set<std::string> Said;
    std::lock_guard<std::mutex> L(Mu);
    if (Said.insert(Folder.string() + "\n" + Why).second)
        LogOut("PackageCatalog::ReceivedPackageFolders", "received package " + Folder.parent_path().filename().string()
               + ": " + Why + Detail + " — not completed or pruned until it is");
}

// The received package folders on disk: every CATALOG/<nick - lib>/<pkg>/.package/ a friend's share entry landed
// (LandedFolders). Returns the folder dir → the node CIDs its folder lists: its own listing, read from blocks held here
// (the folder root lands first) — the share entry names the folder, nothing inside it repeats the list. A dir no
// current share entry routes to, or whose folder is not held, is left out, and said so.
static std::map<std::filesystem::path, std::vector<std::string>> ReceivedPackageFolders(const nlohmann::ordered_json &Config)
{
    namespace fs = std::filesystem;
    std::map<fs::path, std::vector<std::string>> Out;
    std::error_code Ec;
    const fs::path Root = fs::path(CatalogRootDir(Config));
    if (!fs::is_directory(Root, Ec)) return Out;
    const auto Landed = LandedFolders(Config);
    for (const auto &Lib : fs::directory_iterator(Root, Ec))
    {
        if (!Lib.is_directory(Ec)) continue;
        for (const auto &Pkg : fs::directory_iterator(Lib.path(), Ec))
        {
            if (!Pkg.is_directory(Ec)) continue;
            const fs::path Folder = Pkg.path() / kPackageFolderDir;
            if (!fs::is_directory(Folder, Ec)) continue;
            const auto It = Landed.find(Folder);
            if (It == Landed.end()) { ReportUnjudged(Folder, "no share entry routes here now", ""); continue; }
            std::string Err;
            auto Cids = FolderNodes(It->second, &Err);
            if (!Cids) { ReportUnjudged(Folder, "its folder is not held yet", " (" + It->second + ": " + Err + ")"); continue; }
            Out[Folder] = std::move(*Cids);
        }
    }
    return Out;
}

// The library NAME a received lib dir ("<nick> - <lib>") was planned under: the same shape the planner builds,
// matched against the snapshots we hold. "" when nothing matches (a dir of a forgotten friend).
static std::string ReceivedLibraryName(const nlohmann::ordered_json &Config, const std::string &LibDirName)
{
    if (!Config.contains("FriendLibraries") || !Config["FriendLibraries"].is_object()) return {};
    for (const auto &[P, Libs] : Config["FriendLibraries"].items())
    {
        const std::string Nick = ReceivedNickLabel(P);
        if (!Libs.is_object()) continue;
        for (const auto &[L, Items] : Libs.items())
            if (SafeSegment(Nick + " - " + L) == LibDirName) return L;
    }
    return {};
}

bool AdoptReceivedPackage(const nlohmann::ordered_json &Config, const std::filesystem::path &PkgDir, std::filesystem::path *NewDir, std::string *Error)
{
    // Installing a received package MOVES it out of CATALOG into LIBRARY/<lib>/<pkg>: from then on it is an ordinary
    // local package — launchable, its grafts offered, published with the library under the same CIDs. CATALOG holds
    // only un-installed stubs. A name collision with a package already in the library is refused, never merged.
    namespace fs = std::filesystem;
    //Concurrent downloads share packages — a runner every game's chain reaches (native-passthrough, proton): one adopt
    //at a time, and a package another download already installed is installed, not an error. Each download works from
    //its own index snapshot, so the second found the stub gone and FAILED ITS WHOLE GAME (17 of 34 in a replication).
    static std::mutex AdoptMu;
    std::lock_guard<std::mutex> Lk(AdoptMu);
    std::error_code Ec;
    const fs::path Cat = fs::path(CatalogRootDir(Config)).lexically_normal();
    const fs::path Src = PkgDir.lexically_normal();
    auto [CatEnd, SrcEnd] = std::mismatch(Cat.begin(), Cat.end(), Src.begin(), Src.end());
    if (Cat.empty() || CatEnd != Cat.end())
    { if (Error) *Error = "not a received package dir: " + PkgDir.string(); return false; }
    const std::string Lib = ReceivedLibraryName(Config, Src.parent_path().filename().string());
    if (Lib.empty()) { if (Error) *Error = "no share snapshot names " + Src.parent_path().filename().string(); return false; }
    //The library NAME is the friend's (their snapshot): one sanitised segment, and the destination must lie inside
    //the library whatever it held — an absolute or ".." name would otherwise plant the package anywhere writable.
    const fs::path Dest = fs::path(LibraryRootDir(Config)) / SafeSegment(Lib) / SafeSegment(Src.filename().string());
    if (!NodeGraph::PathWithin(LibraryRootDir(Config), Dest))
    { if (Error) *Error = "refused: " + Dest.string() + " is outside the library"; return false; }
    const fs::path Folder = Src / kPackageFolderDir;
    if (!fs::is_directory(Src, Ec))
    {
        if (fs::is_directory(Dest, Ec)) { if (NewDir) *NewDir = Dest; return true; }   // installed meanwhile
        if (Error) *Error = "not a received package dir: " + PkgDir.string();
        return false;
    }
    if (fs::exists(Dest, Ec)) { if (Error) *Error = "a package named '" + Src.filename().string() + "' already exists in library '" + Lib + "'"; return false; }
    // The landed folder was the receive artifact: its node files move up into the package dir (an installed package is
    // an ordinary bundle). A name already there is the same node (files are CID-named). The
    // folder goes only once every file is out and its references point at the new place.
    if (fs::is_directory(Folder, Ec))
    {
        for (const auto &F : fs::directory_iterator(Folder, Ec))
        {
            if (!F.is_regular_file(Ec)) continue;
            std::error_code Rc;
            if (!fs::exists(Src / F.path().filename(), Rc)) fs::rename(F.path(), Src / F.path().filename(), Rc);
            if (Rc) { if (Error) *Error = "cannot move " + F.path().string() + " into its package: " + Rc.message(); return false; }
        }
        fs::remove_all(Folder, Ec);
    }
    //Every file fetched into the package is referenced in place, and the references move to the installed place BEFORE
    //the package does, in one scan: the landed folder's (its files moved up) and the package dir's. The move is
    //idempotent and never looks at the files, so a failure or crash anywhere up to the rename leaves a stub still
    //Received in CATALOG, which the next download adopts again. Moved after the rename, a failure left an installed
    //package (never adopted again) whose files read as missing, unservable for good.
    std::string MErr;
    if (IpfsWrapper::MoveRefs({{Folder.string(), Dest.string()}, {Src.string(), Dest.string()}}, &MErr) < 0)
    { if (Error) *Error = "cannot re-point the references of " + Src.string() + " to " + Dest.string() + ": " + MErr; return false; }
    auto Refuse = [&](const std::string &Why) {                            // the package stays where its files are
        std::string BErr;
        if (IpfsWrapper::MoveRefs({{Dest.string(), Src.string()}}, &BErr) < 0)
            LogErr("PackageCatalog::AdoptReceivedPackage", "cannot point the references back at " + Src.string() + ": " + BErr);
        if (Error) *Error = Why;
        return false;
    };
    IpfsWrapper::ForgetDestsUnder(Folder.string());   // the queue must never re-land the folder into the old stub
    fs::create_directories(Dest.parent_path(), Ec);
    fs::rename(Src, Dest, Ec);
    if (Ec)
    {   // a different filesystem: copy, then remove the source; a partial copy goes, or it blocks every later install
        Ec.clear();
        fs::copy(Src, Dest, fs::copy_options::recursive, Ec);
        if (Ec)
        {
            const std::string Why = "cannot move " + Src.string() + " to " + Dest.string() + ": " + Ec.message();
            fs::remove_all(Dest, Ec);
            return Refuse(Why);
        }
        fs::remove_all(Src, Ec);
    }
    // A later request for the old paths lands in the installed package (landed folder: files moved up); registered
    // before the queue forgets them, so no request in between re-materialises the stub.
    IpfsWrapper::RedirectDestsUnder(Folder.string(), Dest.string());
    IpfsWrapper::RedirectDestsUnder(Src.string(), Dest.string());
    IpfsWrapper::ForgetDestsUnder(Src.string());
    if (fs::is_empty(Src.parent_path(), Ec)) fs::remove(Src.parent_path(), Ec);
    if (NewDir) *NewDir = Dest;
    LogOut("PackageCatalog::AdoptReceivedPackage", "installed '" + Src.filename().string() + "' into library '" + Lib + "'");
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Versions of an installed package. A package has one home; a friend may share another version of a package installed
// here (they re-minted it: a library, a new engine build). There is no "newer" — the user picks a friend's version, and
// the package switches to it: every node it lists comes in, and an installed node it does not list goes only when
// nothing installed still contains it (other games keep working on the version they contain).
// ---------------------------------------------------------------------------------------------------------------------
namespace {
std::vector<std::string> PackageNodeCids(const std::filesystem::path &Dir)
{
    std::vector<std::string> Out;
    std::error_code Ec;
    for (const auto &F : std::filesystem::directory_iterator(Dir, Ec))
    {
        const std::string Stem = F.path().stem().string();
        if (F.is_regular_file(Ec) && F.path().extension() == ".json" && Stem.rfind("baf", 0) == 0) Out.push_back(Stem);
    }
    std::sort(Out.begin(), Out.end());
    return Out;
}
//A node file not named by its CID: a never-minted draft. A switch cannot judge it, so it is refused.
bool HasDraftNodes(const std::filesystem::path &Dir)
{
    std::error_code Ec;
    for (const auto &F : std::filesystem::directory_iterator(Dir, Ec))
    {
        if (!F.is_regular_file(Ec) || F.path().extension() != ".json" || F.path().stem().string().rfind("baf", 0) == 0) continue;
        if (nlohmann::ordered_json J; NodeGraph::ReadTreeJsonBounded(F.path(), J) && ManifestModel::IsNodeObject(J)) return true;
    }
    return false;
}
std::string LocalFolderCid(const std::filesystem::path &Dir)
{
    static std::mutex Mu;
    static std::map<std::string, std::string> Cache;                   // the listing → its folder (a folder never changes)
    const std::vector<std::string> Cids = PackageNodeCids(Dir);
    if (Cids.empty()) return {};
    std::string Key;
    for (const auto &C : Cids) Key += C + "\n";
    {
        std::lock_guard<std::mutex> L(Mu);
        if (auto It = Cache.find(Key); It != Cache.end()) return It->second;
    }
    std::map<std::string, std::string> Entries;
    for (const auto &C : Cids) Entries[C + ".json"] = C;
    std::string Err;
    const std::string Cid = IpfsWrapper::MakeDir(Entries, &Err);
    if (Cid.empty()) return {};
    std::lock_guard<std::mutex> L(Mu);
    if (Cache.size() > 2000) Cache.clear();
    Cache[Key] = Cid;
    return Cid;
}
std::string Tail(const std::string &S, size_t N) { return S.size() > N ? S.substr(S.size() - N) : S; }
std::filesystem::path UpdateStage(const nlohmann::ordered_json &Config, const PackageUpdate &U)
{
    //Per friend and version: two friends' versions of one package never share a stage.
    return std::filesystem::path(LibraryRootDir(Config)).parent_path() / "UPDATING" / SafeSegment(U.Lib) / SafeSegment(U.Pkg)
           / SafeSegment(Tail(U.Peer, 8) + "-" + Tail(U.FolderCid, 12));
}
//One switch of a package at a time (staging, applying, discarding).
std::mutex &PackageLock(const std::filesystem::path &Dir)
{
    static std::mutex Mu;
    static std::map<std::string, std::unique_ptr<std::mutex>> Locks;
    std::lock_guard<std::mutex> L(Mu);
    auto &P = Locks[Dir.lexically_normal().string()];
    if (!P) P = std::make_unique<std::mutex>();
    return *P;
}
nlohmann::ordered_json NodeFileJson(const std::filesystem::path &P)
{
    nlohmann::ordered_json J;
    return NodeGraph::ReadTreeJsonBounded(P, J) ? J : nlohmann::ordered_json(nlohmann::ordered_json::value_t::discarded);
}
//The installed nodes of Dir, among Candidates, that something still contains once Dir lists Listing: every other
//installed package, and the listed version itself (a version that contains the one before it). Containment only —
//ANY/NOT compare, never hold. NodeAt reads a node of the package or of its stage.
std::vector<std::string> KeptNodes(const NodeIndex &Idx, const std::filesystem::path &Dir, const std::vector<std::string> &Listing,
                                   const std::set<std::string> &Candidates,
                                   const std::function<nlohmann::ordered_json(const std::string &)> &NodeAt)
{
    std::set<std::string> Kept;
    std::deque<std::string> Q;
    for (const auto &[Id, N] : Idx.Nodes)
        if (!N.Received && N.BundleDir.lexically_normal() != Dir.lexically_normal())
            for (const auto &R : N.Refs) Q.push_back(R);
    for (const std::string &C : Listing) for (const auto &R : ManifestModel::NodeRefs(NodeAt(C))) Q.push_back(R);
    while (!Q.empty())
    {
        const std::string C = Q.front(); Q.pop_front();
        if (!Candidates.count(C) || !Kept.insert(C).second) continue;
        for (const auto &R : ManifestModel::NodeRefs(NodeAt(C))) Q.push_back(R);
    }
    return { Kept.begin(), Kept.end() };
}
//The switch of S.U.Dir to S.Listing (its node files in the package or the stage), planned against the package as it
//is on disk now.
void PlanSwitch(const NodeIndex &Idx, StagedUpdate &S)
{
    namespace fs = std::filesystem;
    std::error_code Ec;
    const fs::path &Dir = S.U.Dir;
    auto FileOf = [&](const std::string &C) {
        return fs::exists(Dir / (C + ".json"), Ec) ? Dir / (C + ".json") : S.Stage / (C + ".json");
    };
    const std::vector<std::string> Home = PackageNodeCids(Dir);
    std::vector<std::string> Candidates;
    S.Added.clear(); S.Removed.clear(); S.Moves.clear(); S.Deletes.clear(); S.DeleteBytes = 0;
    std::set_difference(S.Listing.begin(), S.Listing.end(), Home.begin(), Home.end(), std::back_inserter(S.Added));
    std::set_difference(Home.begin(), Home.end(), S.Listing.begin(), S.Listing.end(), std::back_inserter(Candidates));
    S.Kept = KeptNodes(Idx, Dir, S.Listing, { Candidates.begin(), Candidates.end() },
                       [&](const std::string &C) { return NodeFileJson(FileOf(C)); });
    std::set_difference(Candidates.begin(), Candidates.end(), S.Kept.begin(), S.Kept.end(), std::back_inserter(S.Removed));
    //Content follows the node set. Before: what the installed node files claim (where each file is now). After: the
    //kept and listed nodes'. A (name, CID) that stays moves when its name is bumped or un-bumped; one that goes is
    //deleted — unless its CID is still wanted under another name and that file is not here yet: then it moves there.
    std::vector<fs::path> AfterFiles;
    for (const std::string &C : S.Listing) AfterFiles.push_back(FileOf(C));
    for (const std::string &C : S.Kept) AfterFiles.push_back(Dir / (C + ".json"));
    const ContentFile::ClaimMap Before = *ContentFile::Claims(Dir), After = ContentFile::ClaimsOfFiles(AfterFiles);
    std::set<fs::path> Targets;
    std::map<std::string, std::vector<fs::path>> WantedAt, AfterAt;   // CID → its after-paths (not present yet / all)
    for (const auto &[N, Cs] : After)
        for (const auto &C : Cs)
        {
            const fs::path P = ContentFile::PathUnder(Dir, N, C, After);
            AfterAt[C].push_back(P);
            if (!fs::exists(P, Ec)) WantedAt[C].push_back(P);
        }
    for (const auto &[N, Cs] : Before)
        for (const auto &C : Cs)
        {
            const fs::path From = ContentFile::PathUnder(Dir, N, C, Before);
            if (!fs::exists(From, Ec) || !NodeGraph::PathWithin(Dir, From)) continue;
            const auto A = After.find(N);
            if (A != After.end() && A->second.count(C))
            {
                const fs::path To = ContentFile::PathUnder(Dir, N, C, After);
                if (To != From && Targets.insert(To).second) S.Moves.push_back({ From, To });
                continue;
            }
            if (auto W = WantedAt.find(C); W != WantedAt.end() && !W->second.empty())
            {   // the same bytes under the version's own name: renamed, not fetched again
                const fs::path To = W->second.back();
                W->second.pop_back();
                if (Targets.insert(To).second) { S.Moves.push_back({ From, To }); continue; }
            }
            ContentDelete D{ From, C, {} };
            for (const fs::path &P : AfterAt[C]) if (P != From) D.Elsewhere.push_back(P);   // the CID stays under them
            S.DeleteBytes += fs::file_size(From, Ec);
            S.Deletes.push_back(std::move(D));
        }
}
//A switch's plan, written to its stage before the first change: a switch that stops half-way (a crash, a full disk)
//resumes as planned — re-planning a half-switched package would misread which version a file holds.
constexpr const char *kSwitchJournal = "switch.json";
bool SaveJournal(const StagedUpdate &S)
{
    nlohmann::ordered_json J = { {"listing", S.Listing}, {"added", S.Added}, {"removed", S.Removed}, {"kept", S.Kept},
                                 {"moves", nlohmann::ordered_json::array()}, {"deletes", nlohmann::ordered_json::array()},
                                 {"bytes", S.DeleteBytes} };
    for (const auto &[F, T] : S.Moves) J["moves"].push_back({ F.string(), T.string() });
    for (const auto &D : S.Deletes)
    {
        nlohmann::ordered_json E = nlohmann::ordered_json::array();
        for (const auto &P : D.Elsewhere) E.push_back(P.string());
        J["deletes"].push_back({ {"file", D.File.string()}, {"cid", D.Cid}, {"elsewhere", E} });
    }
    const std::filesystem::path P = S.Stage / kSwitchJournal, Tmp = S.Stage / (std::string(kSwitchJournal) + ".tmp");
    {
        std::ofstream O(Tmp, std::ios::binary | std::ios::trunc);
        O << J.dump(1);
        if (!O.flush()) return false;
    }
    std::error_code Ec;
    std::filesystem::rename(Tmp, P, Ec);
    return !Ec;
}
bool LoadJournal(StagedUpdate &S)
{
    const nlohmann::ordered_json J = NodeFileJson(S.Stage / kSwitchJournal);
    if (!J.is_object()) return false;
    auto Strs = [&](const char *K) {
        std::vector<std::string> V;
        if (J.contains(K) && J[K].is_array()) for (const auto &X : J[K]) if (X.is_string()) V.push_back(X.get<std::string>());
        return V;
    };
    S.Listing = Strs("listing"); S.Added = Strs("added"); S.Removed = Strs("removed"); S.Kept = Strs("kept");
    S.Moves.clear(); S.Deletes.clear();
    S.DeleteBytes = J.value("bytes", 0ull);
    if (J.contains("moves") && J["moves"].is_array())
        for (const auto &M : J["moves"])
            if (M.is_array() && M.size() == 2 && M[0].is_string() && M[1].is_string())
                S.Moves.push_back({ M[0].get<std::string>(), M[1].get<std::string>() });
    if (J.contains("deletes") && J["deletes"].is_array())
        for (const auto &D : J["deletes"])
        {
            if (!D.is_object() || !D.contains("file") || !D["file"].is_string()) continue;
            ContentDelete X{ D["file"].get<std::string>(), D.value("cid", std::string()), {} };
            if (D.contains("elsewhere") && D["elsewhere"].is_array())
                for (const auto &P : D["elsewhere"]) if (P.is_string()) X.Elsewhere.push_back(P.get<std::string>());
            S.Deletes.push_back(std::move(X));
        }
    return !S.Listing.empty();
}
//The friend's version a package was last switched to while it kept nodes beyond it: its folder stays pinned, so the
//listing is readable here and that version is known to be in (not offered again).
std::filesystem::path HeldVersionFile(const StagedUpdate &S) { return S.Stage.parent_path() / "held"; }
} // namespace

std::vector<PackageUpdate> AvailableUpdates(const nlohmann::ordered_json &Config)
{
    namespace fs = std::filesystem;
    std::vector<PackageUpdate> Out;
    if (!Config.contains("FriendLibraries") || !Config["FriendLibraries"].is_object()) return Out;
    const fs::path LibRoot = LibraryRootDir(Config);
    std::optional<NodeIndex> Idx;                                     // built only when a version's nodes are all here
    std::error_code Ec;
    for (const auto &[Peer, Libs] : Config["FriendLibraries"].items())
    {
        if (!Libs.is_object()) continue;
        const std::string Nick = ReceivedNickLabel(Peer);
        for (const auto &[Lib, Rows] : Libs.items())
        {
            if (!Rows.is_array()) continue;
            for (const auto &R : Rows)
            {
                if (!R.is_object()) continue;
                const std::string Folder = R.value("cid", std::string()), Pkg = SafeSegment(R.value("pkg", std::string()));
                const fs::path Dir = LibRoot / SafeSegment(Lib) / Pkg;
                if (Folder.empty() || Folder.size() > 128 || !fs::is_directory(Dir, Ec)) continue;
                const std::string Mine = LocalFolderCid(Dir);
                if (Mine.empty())
                {
                    LogWarn("PackageCatalog::AvailableUpdates", "package '" + Pkg + "': its node files are not all held here — "
                            "versions shared by friends cannot be compared until it is published or re-seeded");
                    continue;
                }
                if (Mine == Folder) continue;
                //A version whose listing is held here (switched to before) and whose every node is here already is
                //this package plus nodes kept for other games: offered again only once one of those may go.
                if (const auto Listing = FolderNodes(Folder, nullptr))
                {
                    const std::vector<std::string> Home = PackageNodeCids(Dir);
                    const std::set<std::string> Here(Home.begin(), Home.end());
                    if (std::all_of(Listing->begin(), Listing->end(), [&](const std::string &C) { return Here.count(C) > 0; }))
                    {
                        std::set<std::string> Beyond = Here;
                        for (const std::string &C : *Listing) Beyond.erase(C);
                        if (!Idx) Idx = BuildCatalogIndex(Config);
                        if (KeptNodes(*Idx, Dir, *Listing, Beyond, [&](const std::string &C) { return NodeFileJson(Dir / (C + ".json")); })
                                .size() == Beyond.size())
                            continue;
                    }
                }
                Out.push_back(PackageUpdate{ Dir, Lib, Pkg, Peer, Nick, Folder });
            }
        }
    }
    return Out;
}

bool StageUpdate(const nlohmann::ordered_json &Config, const PackageUpdate &U, StagedUpdate *Out, std::string *Error)
{
    namespace fs = std::filesystem;
    std::lock_guard<std::mutex> Lk(PackageLock(U.Dir));
    StagedUpdate S;
    S.U = U;
    S.Stage = UpdateStage(Config, U);
    std::error_code Ec;
    if (HasDraftNodes(U.Dir))
    { if (Error) *Error = U.Pkg + " holds node files that were never saved under their CID (drafts): open it in the package editor and save first"; return false; }
    if (fs::exists(S.Stage / kSwitchJournal, Ec) && LoadJournal(S))
    {   // a switch to this very version stopped half-way: finish it as it was planned
        if (Out) *Out = std::move(S);
        return true;
    }
    IpfsWrapper::ForgetDestsUnder(S.Stage.string());
    fs::remove_all(S.Stage, Ec);
    fs::create_directories(S.Stage.parent_path(), Ec);
    const IpfsWrapper::ForegroundHold Hold;
    std::string WErr;
    auto Fail = [&](const std::string &Why) {
        IpfsWrapper::ForgetDestsUnder(S.Stage.string());            // a fetch that lands later lands nowhere
        std::error_code Rc;
        fs::remove_all(S.Stage, Rc);
        IpfsWrapper::Unpin(U.FolderCid);                              // the fetch's pin: nobody here wants it
        if (Error) *Error = Why;
        return false;
    };
    if (!IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch({ IpfsWrapper::FetchTarget{ U.FolderCid, S.Stage.string(),
                                /*Optional=*/false, /*Dir=*/true, /*Verify=*/true } }), 10 * 60 * 1000, &WErr))
        return Fail("fetching " + U.Nick + "'s version of " + U.Pkg + ": " + WErr);
    std::string LErr;
    const auto Listing = FolderNodes(U.FolderCid, &LErr);
    if (!Listing || Listing->empty())
        return Fail(U.Nick + "'s version of " + U.Pkg + " lists no nodes" + (LErr.empty() ? "" : ": " + LErr));
    for (const std::string &C : *Listing)
    {
        std::string VErr;
        if (!NodeGraph::VerifyLanded(S.Stage / (C + ".json"), C, nullptr, &VErr))
            return Fail(U.Nick + "'s version of " + U.Pkg + ": node " + C + " refused: " + VErr);
    }
    S.Listing = *Listing;
    std::sort(S.Listing.begin(), S.Listing.end());
    PlanSwitch(BuildCatalogIndex(Config), S);
    if (Out) *Out = std::move(S);
    return true;
}

bool ApplyStagedUpdate(const nlohmann::ordered_json &Config, const StagedUpdate &Confirmed, std::string *Error)
{
    namespace fs = std::filesystem;
    std::lock_guard<std::mutex> Lk(PackageLock(Confirmed.U.Dir));
    std::error_code Ec;
    const fs::path &Dir = Confirmed.U.Dir;
    const std::string &Pkg = Confirmed.U.Pkg;
    auto Refuse = [&](const std::string &Why) { if (Error) *Error = Why; return false; };
    if (Sessions::RuntimesLive() > 0 || !Sessions::LiveMountpointsUnder(Sessions::Root()).empty())
        return Refuse("a game is running — close it first, then switch " + Pkg);
    if (IpfsWrapper::LandingUnder(Dir.string()))
        return Refuse("a download into " + Pkg + " is in progress — let it finish first");
    StagedUpdate S = Confirmed;
    const bool Resuming = LoadJournal(S);
    //Nothing is touched unless every node of the version is here (staged, or moved in by an earlier attempt).
    for (const std::string &C : S.Listing)
        if (!fs::exists(S.Stage / (C + ".json"), Ec) && !fs::exists(Dir / (C + ".json"), Ec))
            return Refuse("the staged version of " + Pkg + " is incomplete (node " + C + "): fetch it again");
    if (!Resuming)
    {
        //Planned again, under the lock: the library may have changed while the user read the plan. It may delete
        //only what the user was shown.
        PlanSwitch(BuildCatalogIndex(Config), S);
        std::set<fs::path> Shown;
        for (const auto &D : Confirmed.Deletes) Shown.insert(D.File);
        for (const auto &D : S.Deletes)
            if (!Shown.count(D.File))
                return Refuse(Pkg + " changed since the switch was planned (" + D.File.filename().string()
                              + " would be deleted too): switch again to review it");
        if (!SaveJournal(S)) return Refuse("cannot write the switch's plan into " + S.Stage.string());
    }
    //1. Content that stays but changes its file name (bumped beside another version, or the version's own name),
    //   never over a file already there; the references follow in one batch (a rename an earlier attempt made too).
    std::vector<std::pair<std::string, std::string>> Followed;
    for (const auto &[From, To] : S.Moves)
    {
        std::error_code Rc;
        const bool HaveFrom = fs::exists(From, Rc), HaveTo = fs::exists(To, Rc);
        if (HaveTo)
        {
            if (HaveFrom) LogWarn("PackageCatalog::ApplyStagedUpdate", "not renaming " + From.string() + ": "
                                  + To.string() + " is already there (left as it is)");
            else Followed.push_back({ From.string(), To.string() });
            continue;
        }
        if (!HaveFrom) continue;                                          // never landed: fetched when needed
        fs::create_directories(To.parent_path(), Rc);
        fs::rename(From, To, Rc);
        if (Rc) return Refuse("cannot rename " + From.string() + ": " + Rc.message() + " (switch again to finish)");
        Followed.push_back({ From.string(), To.string() });
    }
    if (!Followed.empty())
    {
        std::string MErr;
        if (IpfsWrapper::MoveRefs(Followed, &MErr) < 0)
            LogWarn("PackageCatalog::ApplyStagedUpdate", "renamed " + std::to_string(Followed.size())
                    + " file(s) but their references did not follow: " + MErr);
    }
    //2. Content no node of the package names any more goes (its reference with it, when it was the reference — then
    //   the same bytes kept under another name are referenced there).
    for (const auto &D : S.Deletes)
    {
        if (!fs::exists(D.File, Ec)) continue;
        RemoveLandedNode(D.File, D.Cid, D.Elsewhere);
        if (fs::exists(D.File, Ec)) LogWarn("PackageCatalog::ApplyStagedUpdate", "could not delete " + D.File.string());
    }
    //3. The version's node files move in (a staged file is a plain file: the folder fetch holds its blocks), each then
    //   referenced where it now lives, so the package serves and re-shares it.
    for (const std::string &C : S.Listing)
    {
        const fs::path From = S.Stage / (C + ".json"), To = Dir / (C + ".json");
        std::error_code Rc;
        if (fs::exists(To, Rc)) continue;
        fs::rename(From, To, Rc);
        if (Rc) { Rc.clear(); fs::copy_file(From, To, Rc); }
        if (Rc) return Refuse("cannot move " + From.string() + " into " + Dir.string() + ": " + Rc.message() + " (switch again to finish)");
        std::string AErr;
        if (IpfsWrapper::AddNoCopy(To.string(), &AErr) != C)
            LogWarn("PackageCatalog::ApplyStagedUpdate", "node " + C + " is in place but not referenced there: " + AErr);
    }
    //4. The nodes nothing contains any more.
    for (const std::string &C : S.Removed)
    {
        RemoveLandedNode(Dir / (C + ".json"), C);
        if (fs::exists(Dir / (C + ".json"), Ec))
            LogWarn("PackageCatalog::ApplyStagedUpdate", "could not remove node " + C + " from " + Dir.string());
    }
    //The stage's pin goes — unless nodes were kept beyond the version: then its folder stays held (one per package),
    //so the version is known to be in and not offered again.
    const fs::path Held = HeldVersionFile(S);
    std::string Before;
    std::getline(std::ifstream(Held), Before);
    if (!Before.empty() && Before != S.U.FolderCid) IpfsWrapper::Unpin(Before);
    if (S.Kept.empty())
    {
        IpfsWrapper::Unpin(S.U.FolderCid);
        fs::remove(Held, Ec);
    }
    else std::ofstream(Held, std::ios::trunc) << S.U.FolderCid << "\n";
    fs::remove_all(S.Stage, Ec);
    LogOut("PackageCatalog::ApplyStagedUpdate", "switched '" + Pkg + "' to " + S.U.Nick + "'s version: "
           + std::to_string(S.Added.size()) + " node(s) in, " + std::to_string(S.Removed.size()) + " out, "
           + std::to_string(S.Kept.size()) + " kept for what still contains them");
    return true;
}

void DiscardStagedUpdate(const StagedUpdate &S)
{
    std::lock_guard<std::mutex> Lk(PackageLock(S.U.Dir));
    IpfsWrapper::ForgetDestsUnder(S.Stage.string());
    std::error_code Ec;
    std::filesystem::remove_all(S.Stage, Ec);                             // plain files: the folder's pin holds its blocks
    std::string Held;
    std::getline(std::ifstream(HeldVersionFile(S)), Held);
    if (Held != S.U.FolderCid) IpfsWrapper::Unpin(S.U.FolderCid);         // …and nobody here wants it
}

std::string PackageFolderCid(const nlohmann::ordered_json &Config, const std::filesystem::path &PkgDir, const std::string &Field)
{
    const std::string Pkg = PkgDir.filename().string(), Parent = PkgDir.parent_path().filename().string();
    auto find = [&](const nlohmann::ordered_json &Libs, auto &&LibMatches) -> std::string {
        if (!Libs.is_object()) return {};
        for (const auto &[L, Rows] : Libs.items())
        {
            if (!Rows.is_array() || !LibMatches(L)) continue;
            for (const auto &R : Rows)
                if (R.is_object() && SafeSegment(R.value("pkg", std::string())) == Pkg && !R.value(Field, std::string()).empty())
                    return R.value(Field, std::string());
        }
        return {};
    };
    if (Config.contains("Libraries"))
        if (std::string C = find(Config["Libraries"], [&](const std::string &L) { return SafeSegment(L) == Parent; }); !C.empty())
            return C;
    if (Config.contains("FriendLibraries") && Config["FriendLibraries"].is_object())
        for (const auto &[Peer, Libs] : Config["FriendLibraries"].items())
            if (std::string C = find(Libs, [&](const std::string &L) {
                    const std::string S = SafeSegment(L);
                    return S == Parent || (Parent.size() > S.size() + 3 && Parent.compare(Parent.size() - S.size() - 3, std::string::npos, " - " + S) == 0);
                }); !C.empty())
                return C;
    return {};
}

bool ReceivedPackagesIncomplete(const nlohmann::ordered_json &Config)
{
    std::error_code Ec;
    for (const auto &[Dir, Cids] : ReceivedPackageFolders(Config))
        for (const std::string &C : Cids)
            if (!std::filesystem::exists(Dir / (C + ".json"), Ec)) return true;
    return false;
}

bool LandReceivedPackages(const nlohmann::ordered_json &Config, std::string *Error)
{
    const IpfsWrapper::ForegroundHold Hold;                             // package after package: no gap for walks
    // A package folder lands whole (the queue fetched it); this completes and checks it: a node file its folder
    // lists but the landing did not bring is fetched on its own, and every named file is verified — one that is not
    // the bytes its name says (or not a canonical node) is removed and the package reported, never repaired.
    namespace fs = std::filesystem;
    std::error_code Ec;
    bool Ok = true;
    for (const auto &[Dir, Cids] : ReceivedPackageFolders(Config))
    {
        std::vector<IpfsWrapper::FetchTarget> Batch;
        for (const std::string &C : Cids)
            if (!fs::exists(Dir / (C + ".json"), Ec))
                Batch.push_back(IpfsWrapper::FetchTarget{ C, (Dir / (C + ".json")).string(), /*Optional=*/false, /*Dir=*/false, /*Verify=*/true });
        if (!Batch.empty())
        {
            std::string WErr;
            if (!IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch(Batch), 10 * 60 * 1000, &WErr))
            { if (Error) *Error = "package " + Dir.filename().string() + ": " + WErr; Ok = false; continue; }
        }
        for (const std::string &C : Cids)
        {
            std::string VErr;
            if (NodeGraph::VerifyLanded(Dir / (C + ".json"), C, nullptr, &VErr)) continue;
            RemoveLandedNode(Dir / (C + ".json"), C);
            if (Error) *Error = "package " + Dir.filename().string() + ": node " + C.substr(0, 16) + "… " + VErr;
            LogWarn("PackageCatalog::LandReceivedPackages", "package '" + Dir.filename().string() + "': node " + C + " refused: " + VErr);
            Ok = false;
        }
    }
    return Ok;
}

int PruneStaleReceived(const NodeIndex &Idx, const nlohmann::ordered_json &Config)
{
    // A received package's landed folder holds exactly what the folder lists (one home: what it contains from other
    // packages lives there). Any other node file in it is an older generation's — a re-published node under its
    // previous CID. Remove it.
    namespace fs = std::filesystem;
    std::error_code Ec;
    int Removed = 0;
    const auto Manifests = ReceivedPackageFolders(Config);                 // one read, not one per stale file
    std::set<fs::path> Installed;                                            // installed packages hold CID-named nodes too
    for (const auto &[Id, N] : Idx.Nodes) if (!N.Received && !N.BundleDir.empty()) Installed.insert(N.BundleDir);
    for (const auto &[Dir, Cids] : Manifests)
    {
        //A received package holds exactly what its folder lists: a node it contains from another package lives in that
        //package (one home). A node file beyond the listing is an older generation's, or a copy an older build
        //fetched into this package to complete a closure.
        const std::set<std::string> Keep(Cids.begin(), Cids.end());
        // Per FILE, not per index node: the same CID landed in two libraries is ONE index entry, and a landed file is
        // named by its CID (a label-named file is a pre-manifest root: stale by definition).
        for (const auto &F : fs::directory_iterator(Dir, Ec))
        {
            const std::string Name = F.path().filename().string();
            if (!F.is_regular_file(Ec) || F.path().extension() != ".json") continue;
            if (Keep.count(F.path().stem().string())) continue;
            if (!std::all_of(Name.begin(), Name.end() - 5, [](unsigned char c) { return std::isalnum(c); })) continue;   // not a node file
            const std::string Cid = F.path().stem().string();
            std::vector<fs::path> Elsewhere;                         // the same node's other landed copies
            for (const auto &[Other, OtherCids] : Manifests)
                if (Other != Dir) { Elsewhere.push_back(Other / (Cid + ".json")); Elsewhere.push_back(Other.parent_path() / (Cid + ".json")); }
            for (const fs::path &P : Installed) Elsewhere.push_back(P / (Cid + ".json"));
            if (const Node *N = Idx.Find(Cid); N && !N->File.empty()) Elsewhere.push_back(N->File);
            RemoveLandedNode(F.path(), Cid, Elsewhere);
            if (!fs::exists(F.path(), Ec)) ++Removed;
        }
    }
    return Removed;
}

bool NodeClosureIncomplete(const NodeIndex &Idx, const std::string &Id)
{
    return !MissingFromClosure(Idx, Id).empty();
}

std::vector<std::string> MissingFromClosure(const NodeIndex &Idx, const std::string &Id)
{
    std::vector<std::string> Missing;
    if (!Idx.Find(Id)) return Missing;
    (void)ManifestModel::Closure(Idx, Id, &Missing);                       // every depth, not one hop
    std::sort(Missing.begin(), Missing.end());
    Missing.erase(std::unique(Missing.begin(), Missing.end()), Missing.end());
    return Missing;
}

// The gigagraph catalog: the on-disk library is the pretty, handle-based working tree (LIBRARY/[uid] Title/…); we
// derive the CID-addressed node graph from it in memory (NodeGraph::FreezeToIndex) — identity = each node's dag-json
// CID, NODE_ID demotes to a label, cross-package edges resolve to CIDs, and every node carries its on-disk BundleDir
// so launch mounts local content. No on-disk rewrite (git model: working tree is truth, CID index is derived on load;
// DagPut only happens when publishing).
#ifndef VIDYAGOD_VERSION
#define VIDYAGOD_VERSION ""
#endif

// ---- the index cache ----
// The last full index built for a data dir, kept beside it with what it was built from: this program, the machine,
// the roots in scan order, and every node file in them with its size and modification time. A build whose inputs are
// the same loads it — one file instead of thousands, and none of the freeze and fold — and says again what the build
// that made it logged. Any difference (a node edited, added, removed; another binary) builds afresh and replaces it.
namespace
{
namespace fs = std::filesystem;

const std::set<std::string> kIndexLogContexts = {
    "NodeGraph::GatherWorkingTree", "NodeGraph::ReadTreeJsonBounded", "NodeGraph::TopoOrderForMint",
    "NodeGraph::FreezeToIndex", "ManifestModel::ParseNode", "PackageCatalog::BuildCatalogIndex" };

fs::path IndexCachePath() { return AppPaths::DataRoot() / "CACHE" / "index.cbor"; }

std::string IndexInputs(const nlohmann::ordered_json &Config, long long *Newest = nullptr)
{
    //The program: an AppImage's own file (its mounted binary sits at a new /tmp path every run), else the binary.
    const char *AppImage = std::getenv("APPIMAGE");
    const fs::path Exe = AppImage && *AppImage ? fs::path(AppImage) : Platform::SelfExe();
    std::string S = std::string("vgindex 2 ") + VIDYAGOD_VERSION + "\n" + Exe.string() + '\t' + Platform::FileStamp(Exe) + '\n'
                    + ManifestModel::MachinePlatform() + '\n';
    std::vector<fs::path> Roots = { LibraryRootDir(Config), CatalogRootDir(Config) };
    for (const auto &D : LocalPackageDirs(Config)) Roots.push_back(D);
    for (const fs::path &Root : Roots)
    {
        S += "root\t" + Root.string() + '\n';
        const std::vector<fs::path> Files = NodeGraph::ListTreeFiles(Root);
        std::vector<std::string> Lines(Files.size());
        std::vector<long long> Times(Files.size(), 0);
        Parallel::For(Files.size(), [&](std::size_t I) { Lines[I] = Files[I].string() + '\t' + Platform::FileStamp(Files[I], &Times[I]) + '\n'; });
        for (const auto &L : Lines) S += L;
        if (Newest) for (const long long T : Times) *Newest = std::max(*Newest, T);
    }
    return S;
}

std::optional<NodeIndex> LoadIndexCache(const std::string &Inputs, std::vector<Diagnostics::Entry> &Said)
{
    if (!AppPaths::DataRootSet()) return std::nullopt;   // no data dir of its own: no cache (see SaveIndexCache)
    try
    {
        std::ifstream In(IndexCachePath(), std::ios::binary | std::ios::ate);
        if (!In) return std::nullopt;
        std::vector<std::uint8_t> Bytes(static_cast<std::size_t>(In.tellg()));
        In.seekg(0);
        if (!In.read(reinterpret_cast<char *>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()))) return std::nullopt;
        nlohmann::ordered_json H = nlohmann::ordered_json::from_cbor(Bytes, true, false);
        if (H.is_discarded() || !H.is_object() || H.value("inputs", std::string()) != Inputs) return std::nullopt;
        NodeIndex Idx;
        auto &Blobs = H.at("nodes");
        std::vector<std::pair<std::string, Node>> Nodes(Blobs.size());
        Parallel::For(Blobs.size(), [&](std::size_t I) {
            nlohmann::ordered_json J = nlohmann::ordered_json::from_cbor(Blobs[I].get_binary(), true, true);
            Nodes[I].first = J.at("key").get<std::string>();
            Nodes[I].second = ManifestModel::NodeFromJson(std::move(J));
        });
        for (auto &[K, N] : Nodes) Idx.Nodes.emplace_hint(Idx.Nodes.end(), std::move(K), std::move(N));
        ManifestModel::IndexTablesFromJson(H.at("tables"), Idx);
        Said.clear();
        for (const auto &E : H.at("log")) Said.push_back({static_cast<LogLevel>(E.at(0).get<int>()), E.at(1).get<std::string>(), E.at(2).get<std::string>()});
        LogReplay(Said);   // what the build that made it said, said again
        return Idx;
    }
    catch (const std::exception &) { return std::nullopt; }   // a cache of another shape, or damaged: built afresh, then replaced
}

void SaveIndexCache(const std::string &Inputs, const NodeIndex &Idx, const std::vector<Diagnostics::Entry> &Said)
{
    if (!AppPaths::DataRootSet()) return;   // a test or tool that set no data dir never writes into ~/.VidyaGod
    std::vector<const std::pair<const std::string, Node> *> Items;
    for (const auto &KV : Idx.Nodes) Items.push_back(&KV);
    std::vector<nlohmann::ordered_json> Blobs(Items.size());
    Parallel::For(Items.size(), [&](std::size_t I) {
        Blobs[I] = nlohmann::ordered_json::binary(nlohmann::ordered_json::to_cbor(ManifestModel::NodeToJson(Items[I]->second)));
    });
    nlohmann::ordered_json Log = nlohmann::ordered_json::array();
    for (const auto &E : Said) Log.push_back(nlohmann::ordered_json::array({static_cast<int>(E.Level), E.Context, E.Message}));
    nlohmann::ordered_json H = {{"inputs", Inputs}, {"tables", ManifestModel::IndexTablesToJson(Idx)}, {"log", Log}};
    H["nodes"] = std::move(Blobs);
    const std::vector<std::uint8_t> Bytes = nlohmann::ordered_json::to_cbor(H);
    //One writer at a time in this process, each into a temp of its own (another process has another pid), renamed
    //over the cache whole: a reader sees the old cache or the new one, never part of one.
    static std::mutex SaveMu;
    std::lock_guard<std::mutex> Lk(SaveMu);
    std::error_code Ec;
    const fs::path Path = IndexCachePath();
    fs::create_directories(Path.parent_path(), Ec);
    //A temp a process left when it died mid-write (an hour old: no live writer takes that long) is removed.
    for (fs::directory_iterator It(Path.parent_path(), Ec), End; !Ec && It != End; It.increment(Ec))
        if (It->path().filename().string().rfind(Path.filename().string() + ".tmp", 0) == 0
            && fs::last_write_time(It->path(), Ec) < fs::file_time_type::clock::now() - std::chrono::hours(1))
            fs::remove(It->path(), Ec);
    Ec.clear();
    const fs::path Tmp = Path.string() + ".tmp" + std::to_string(QCoreApplication::applicationPid());
    {
        std::ofstream Out(Tmp, std::ios::binary | std::ios::trunc);
        if (!Out) return;
        Out.write(reinterpret_cast<const char *>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
        if (!Out) { Out.close(); fs::remove(Tmp, Ec); return; }
    }
    fs::rename(Tmp, Path, Ec);
    if (Ec) fs::remove(Tmp, Ec);
}

} // namespace

NodeIndex BuildCatalogIndex(const nlohmann::ordered_json &GlobalConfigJSON, std::string *InputsOut, std::vector<Diagnostics::Entry> *SaidOut)
{
    const auto T0 = std::chrono::steady_clock::now();
    long long Newest = 0;
    const std::string Inputs = IndexInputs(GlobalConfigJSON, &Newest);
    if (InputsOut) *InputsOut = Inputs;
    std::vector<Diagnostics::Entry> Replayed;
    if (std::optional<NodeIndex> Cached = LoadIndexCache(Inputs, Replayed))
    {
        if (SaidOut) *SaidOut = std::move(Replayed);
        LogOut("PackageCatalog::BuildCatalogIndex", "Indexed " + std::to_string(Cached->Nodes.size()) + " node(s) from "
               + LibraryRootDir(GlobalConfigJSON) + " in " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - T0).count()) + " ms (the cached index: no node file changed)");
        return std::move(*Cached);
    }
    LogRecorder Said(kIndexLogContexts);
    NodeIndex Idx = ScanCatalogIndex(GlobalConfigJSON);
    std::vector<Diagnostics::Entry> Lines = Said.Take();
    //A node file written in the last few seconds could be written again within the same tick of its drive's clock
    //(two seconds on FAT) at the same size, and look unchanged: such a build is not kept (git's "racy" files).
    const long long Now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (Newest < Now - 3) SaveIndexCache(Inputs, Idx, Lines);
    if (SaidOut) *SaidOut = std::move(Lines);
    return Idx;
}

namespace
{
std::mutex g_AppIndexMu;
std::shared_ptr<const NodeIndex> g_AppIndex;
std::string g_AppIndexInputs;
std::vector<Diagnostics::Entry> g_AppIndexSaid;
}

IndexCacheState IndexCacheStatus(const nlohmann::ordered_json &GlobalConfigJSON)
{
    IndexCacheState St;
    const fs::path P = IndexCachePath();
    St.Path = P.string();
    std::error_code Ec;
    St.Exists = fs::exists(P, Ec);
    if (St.Exists) St.Bytes = fs::file_size(P, Ec);
    const std::string Inputs = IndexInputs(GlobalConfigJSON);
    St.Files = static_cast<std::size_t>(std::count(Inputs.begin(), Inputs.end(), '\n'));
    if (std::ifstream In(P, std::ios::binary | std::ios::ate); St.Exists && In)   // gone meanwhile: not current
    {
        const std::streamoff Size = In.tellg();
        std::vector<std::uint8_t> Bytes(Size > 0 ? static_cast<std::size_t>(Size) : 0);
        In.seekg(0);
        if (In.read(reinterpret_cast<char *>(Bytes.data()), static_cast<std::streamsize>(Bytes.size())))
        {
            const nlohmann::ordered_json H = nlohmann::ordered_json::from_cbor(Bytes, true, false);
            St.Current = !H.is_discarded() && H.is_object() && H.value("inputs", std::string()) == Inputs;
        }
    }
    return St;
}

std::optional<NodeIndex> ReadIndexCache(const nlohmann::ordered_json &GlobalConfigJSON)
{
    std::vector<Diagnostics::Entry> Said;
    LogBuffer Quiet;   // a read for comparison: what its build said is not said again here
    std::optional<NodeIndex> Idx = LoadIndexCache(IndexInputs(GlobalConfigJSON), Said);
    (void)Quiet.Take();
    return Idx;
}

bool ClearIndexCache()
{
    std::error_code Ec;
    fs::remove(IndexCachePath(), Ec);
    return !Ec;
}

void PublishAppIndex(std::shared_ptr<const NodeIndex> Idx, std::string Inputs, std::vector<Diagnostics::Entry> Said)
{
    std::lock_guard<std::mutex> Lk(g_AppIndexMu);
    g_AppIndex = std::move(Idx);
    g_AppIndexInputs = std::move(Inputs);
    g_AppIndexSaid = std::move(Said);
}

std::shared_ptr<const NodeIndex> CurrentIndex(const nlohmann::ordered_json &GlobalConfigJSON)
{
    std::shared_ptr<const NodeIndex> App;
    std::string AppInputs;
    std::vector<Diagnostics::Entry> AppSaid;
    { std::lock_guard<std::mutex> Lk(g_AppIndexMu); App = g_AppIndex; AppInputs = g_AppIndexInputs; AppSaid = g_AppIndexSaid; }
    if (App && IndexInputs(GlobalConfigJSON) == AppInputs) { LogReplay(AppSaid); return App; }
    return std::make_shared<const NodeIndex>(BuildCatalogIndex(GlobalConfigJSON));
}

NodeIndex ScanCatalogIndex(const nlohmann::ordered_json &GlobalConfigJSON)
{
    using Clock = std::chrono::steady_clock;
    const auto Ms = [](Clock::time_point A, Clock::time_point B) { return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(B - A).count()); };
    const auto T0 = Clock::now();
    std::map<std::string, nlohmann::ordered_json> Tree;
    std::map<std::string, std::filesystem::path>  Dirs;
    NodeGraph::GatherWorkingTree(LibraryRootDir(GlobalConfigJSON), Tree, Dirs);   // OUR nodes: stored CID handle trusted
    // Received browse stubs (a friend controls their bytes): gather with TrustStoredCid=FALSE so a crafted "CID" can't
    // hijack a local handle or a local node's external-dep ref, or forge an "authored" handle to steal a BundleDir.
    // They still merge in and resolve by their DERIVED CID in the frozen index (browse tiles link exec→tile fine).
    NodeGraph::GatherWorkingTree(CatalogRootDir(GlobalConfigJSON), Tree, Dirs, /*SkipReserved=*/false, /*TrustStoredCid=*/false);
    for (const auto &D : LocalPackageDirs(GlobalConfigJSON))   // externally-added bundles that live OUTSIDE LIBRARY
        NodeGraph::GatherWorkingTree(D, Tree, Dirs);
    //Runners every install has (a Windows game on Windows runs as itself): nothing to download for them.
    BuiltinRunners::AddTo(Tree, Dirs, LibraryRootDir(GlobalConfigJSON), ManifestModel::MachinePlatform());
    const auto T1 = Clock::now();
    NodeIndex Idx = NodeGraph::FreezeToIndex(Tree, Dirs);
    if (Idx.Nodes.empty() && !Tree.empty())
        LogErr("PackageCatalog::BuildCatalogIndex", "no node of " + std::to_string(Tree.size()) + " froze (the log above says why)");
    else
        LogOut("PackageCatalog::BuildCatalogIndex", "Indexed " + std::to_string(Idx.Nodes.size())
               + " node(s) by CID from " + LibraryRootDir(GlobalConfigJSON) + " in " + Ms(T0, Clock::now()) + " ms (read "
               + Ms(T0, T1) + ", freeze and facts " + Ms(T1, Clock::now()) + ")");
    //A node that lives under CATALOG is a RECEIVED stub: a friend's bytes, browsable and downloadable, never a
    //graft candidate (it would mount un-hydrated). Decided by where its file lives — the only fact that
    //distinguishes it, since the frozen index is one flat graph.
    //A node that is ALSO installed (a friend's stub of a package we have) is ours: it mounts and seeds from our copy.
    const std::filesystem::path Cat = std::filesystem::path(CatalogRootDir(GlobalConfigJSON)).lexically_normal();
    auto UnderCatalog = [&](const std::filesystem::path &Dir) {
        const std::filesystem::path D = Dir.lexically_normal();
        auto [CatEnd, DEnd] = std::mismatch(Cat.begin(), Cat.end(), D.begin(), D.end());
        (void)DEnd;
        return !Cat.empty() && CatEnd == Cat.end();
    };
    //A friend's catalog stub of a package that is installed here is a view of it, not a second home: the node reads
    //from the installed package. What is left in Duplicates after that is two installed homes — the validator's.
    for (auto &[Id, D] : Idx.Duplicates)
    {
        Node &N = Idx.Nodes.at(Id);
        if (UnderCatalog(N.BundleDir))
            if (const auto It = std::find_if(D.begin(), D.end(), [&](const auto &P) { return !UnderCatalog(P); }); It != D.end())
            {
                std::swap(N.BundleDir, *It);
                std::error_code Fe;
                if (const auto F = N.BundleDir / (Id + ".json"); std::filesystem::exists(F, Fe)) N.File = F;   // its file there too
            }
        D.erase(std::remove_if(D.begin(), D.end(), UnderCatalog), D.end());
    }
    for (auto It = Idx.Duplicates.begin(); It != Idx.Duplicates.end();) It = It->second.empty() ? Idx.Duplicates.erase(It) : std::next(It);
    for (auto &[Id, N] : Idx.Nodes)
    {
        (void)Id;
        N.Received = UnderCatalog(N.BundleDir);
    }

    return Idx;
}

std::shared_ptr<const NodeIndex> FreshCatalogIndex(const nlohmann::ordered_json &GlobalConfigJSON)
{
    // Single flight: a build serves every caller that asked before it STARTED (it scans the disk as it is now, after
    // their changes); a caller arriving mid-build waits for the next one.
    static std::mutex Mu;
    static std::condition_variable Cv;
    static std::uint64_t Asked = 0, Served = 0;
    static bool Building = false;
    static int Callers = 0;                                             // in here now; the last one out lets go of Latest
    static std::shared_ptr<const NodeIndex> Latest;
    std::unique_lock<std::mutex> Lk(Mu);
    const std::uint64_t Mine = ++Asked;
    ++Callers;
    for (;;)
    {
        if (Served >= Mine)
        {
            std::shared_ptr<const NodeIndex> Got = Latest;
            if (--Callers == 0) Latest.reset();                         // no 17 MB copy kept past its last reader
            return Got;
        }
        if (!Building)
        {
            Building = true;
            const std::uint64_t Covers = Asked;
            Lk.unlock();
            std::shared_ptr<const NodeIndex> Built;
            try { Built = std::make_shared<const NodeIndex>(BuildCatalogIndex(GlobalConfigJSON)); }
            catch (...) { Lk.lock(); Building = false; --Callers; Cv.notify_all(); throw; }   // the next caller builds
            Lk.lock();
            Latest = std::move(Built);
            Served = Covers;
            Building = false;
            Cv.notify_all();
            continue;
        }
        Cv.wait(Lk);
    }
}

namespace {
//Per package folder of the index: the node ScanBundleIdentity would name it by (a presentable launchable first, then
//any node with a UID, then the first) and its shelf rows — the same rule, read from the index instead of the disk.
struct BundleNodes { const Node *Rep = nullptr; bool RepLaunchable = false; std::vector<const Node *> Variants; };
std::map<std::filesystem::path, BundleNodes> NodesByBundle(const NodeIndex &Idx)
{
    std::map<std::filesystem::path, BundleNodes> Out;
    for (const auto &[K, N] : Idx.Nodes)
    {
        if (N.BundleDir.empty() || N.Received) continue;
        BundleNodes &B = Out[std::filesystem::path(N.BundleDir).lexically_normal()];
        if (N.IsVariant()) B.Variants.push_back(&N);
        const bool Launchable = N.IsVariant() && !N.IsGraft;
        if (Launchable && (!B.RepLaunchable || (N.Presentable() && !B.Rep->Presentable()))) { B.Rep = &N; B.RepLaunchable = true; }
        else if (!B.RepLaunchable && (!B.Rep || (B.Rep->Uid.empty() && !N.Uid.empty()))) B.Rep = &N;
    }
    return Out;
}
std::string IdentityUid(const Node &N) { return N.Uid.empty() ? N.NodeId : N.Uid; }
} // namespace

std::vector<InstalledPackage> InstalledPackages(const nlohmann::ordered_json &GlobalConfigJSON, const NodeIndex &Idx, bool Sizes)
{
    std::vector<InstalledPackage> Out;
    const auto Hyd = HydrationMap(Idx);
    const std::filesystem::path Root = LibraryRootDir(GlobalConfigJSON);
    for (const auto &[Dir, B] : NodesByBundle(Idx))   // a friend's CATALOG stubs are not in it (Node::Received)
    {
        if (!B.RepLaunchable) continue;
        bool Installed = false;
        for (const Node *V : B.Variants)
        {
            const auto It = Hyd.find(V->Key());
            Installed = Installed || (!V->IsGraft && It != Hyd.end() && It->second.OnDisk && It->second.Hydrated);
        }
        if (!Installed) continue;
        InstalledPackage P;
        P.Dir = Dir;
        P.Uid = IdentityUid(*B.Rep);
        P.Title = B.Rep->Meta.is_object() ? B.Rep->Meta.value("TITLE", B.Rep->NodeId) : B.Rep->NodeId;
        P.Local = Root.empty() || !PathUnder(Root, Dir);
        if (GlobalConfigJSON.contains("LIBRARY") && GlobalConfigJSON["LIBRARY"].is_array())
            for (const auto &E : GlobalConfigJSON["LIBRARY"])
                if (E.is_object() && std::filesystem::path(E.value("PATH", std::string())).lexically_normal() == Dir) { P.HasEntry = true; break; }
        P.Library = P.Local ? std::string() : LibraryOf(Dir, Root);
        for (const Node *V : B.Variants) if (!V->IsGraft) P.Variants.push_back(V->Variant);
        std::sort(P.Variants.begin(), P.Variants.end());
        P.Variants.erase(std::unique(P.Variants.begin(), P.Variants.end()), P.Variants.end());
        Out.push_back(std::move(P));
    }
    if (Sizes)
        Parallel::For(Out.size(), [&](std::size_t I) {
            std::error_code Ec;
            for (auto It = std::filesystem::recursive_directory_iterator(Out[I].Dir, std::filesystem::directory_options::skip_permission_denied, Ec);
                 !Ec && It != std::filesystem::recursive_directory_iterator(); It.increment(Ec))
                if (std::error_code E2; It->is_regular_file(E2)) Out[I].Bytes += It->file_size(E2);
        });
    std::sort(Out.begin(), Out.end(), [](const InstalledPackage &A, const InstalledPackage &B) {
        return std::tie(A.Local, A.Library, A.Title, A.Dir) < std::tie(B.Local, B.Library, B.Title, B.Dir);
    });
    return Out;
}

std::vector<std::filesystem::path> PackageDirsWithUid(const NodeIndex &Idx, const std::string &Uid)
{
    std::vector<std::filesystem::path> Out;
    for (const auto &[Dir, B] : NodesByBundle(Idx))
    {
        const std::string F = Dir.filename().string();   // "[7804][v1.0] Age of Mythology": the UID it was made under
        const bool Folder = F.size() > Uid.size() + 1 && F[0] == '[' && F.compare(1, Uid.size(), Uid) == 0 && F[Uid.size() + 1] == ']';
        if ((B.Rep && IdentityUid(*B.Rep) == Uid) || Folder) Out.push_back(Dir);
    }
    return Out;
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
    }, false, false);
    for (const std::string &C : Cids) { IpfsWrapper::Unpin(C); IpfsWrapper::DropRef(C); }
    return Removed;
}

std::vector<std::string> NodeContentCids(const NodeIndex &Idx, const std::string &LaunchNodeId, const GraftChoice &Grafts)
{
    std::vector<std::string> Cids;
    std::set<std::string> Seen;
    ForEachContentLayer(Idx, LaunchNodeId, Grafts, [&](const nlohmann::ordered_json&, const std::filesystem::path&, const std::string &Cid){
        if (!Cid.empty() && Seen.insert(Cid).second) Cids.push_back(Cid);
    });
    return Cids;
}

std::map<std::string, long long> NodeContentSizes(const NodeIndex &Idx, const std::string &LaunchNodeId,
                                                  const GraftChoice &Grafts)
{
    // The stamped SOURCE.SIZE per content CID — the sizes-in-JSON keystone: a download size must derive INSTANTLY
    // and OFFLINE from the graph, never from a ~35s network probe per CID (which left the pre-download dialog
    // "estimating…" for minutes). Only stamped (>0) entries are returned; the caller probes the rare unstamped rest.
    std::map<std::string, long long> Out;
    ForEachContentLayer(Idx, LaunchNodeId, Grafts, [&](const nlohmann::ordered_json &L, const std::filesystem::path&, const std::string &Cid){
        if (Cid.empty() || !L.contains("SOURCE") || !L["SOURCE"].is_object()) return;
        const long long Sz = L["SOURCE"].value("SIZE", (long long)0);
        if (Sz > 0) Out[Cid] = Sz;
    });
    return Out;
}

bool CollectContentTargets(const NodeIndex &Idx, const std::string &LaunchNodeId, const GraftChoice &Grafts,
                           std::vector<IpfsWrapper::FetchTarget> &Out, std::string *Error)
{
    auto Fail = [&](const std::string &M) -> bool { if (Error) *Error = M; LogErr("PackageCatalog::CollectContentTargets", M); return false; };
    const Node *Launch = Idx.Find(LaunchNodeId);
    if (!Launch) return Fail("launch node not found: " + LaunchNodeId);

    // Required content layers: fetch any that aren't already on disk; a missing layer with no IPFS source is fatal
    // for the game's OWN closure — a graft's (a ticked mod's) is reported and skipped, so one broken mod never
    // bricks the base game's download.
    bool MissingSource = false; std::string MissingErr;
    std::set<std::string> OwnLayers;
    ForEachContentLayer(Idx, LaunchNodeId, Grafts, [&](const nlohmann::ordered_json &, const std::filesystem::path &Local, const std::string &){
        OwnLayers.insert(Local.string()); });
    ForEachContentLayer(Idx, LaunchNodeId, Grafts, [&](const nlohmann::ordered_json &L, const std::filesystem::path &Local, const std::string &Cid){
        if (MissingSource) return;
        std::error_code Ec;
        if (std::filesystem::exists(Local, Ec)) return;                          // already present
        if (Cid.empty())
        {
            if (!OwnLayers.count(Local.string()))
            { LogWarn("PackageCatalog::CollectContentTargets", "graft layer has no local file and no IPFS source, skipped: " + Local.string()); return; }
            MissingSource = true; MissingErr = "missing local content with no IPFS source: " + Local.string(); return;
        }
        // Push the stamped SOURCE.SIZE to the node so a gateway-fallback fetch of this layer reports a real % (and a
        // pre-fetch total). Done HERE — at fetch-build, on the shared path for the GUI download, --fetch AND
        // --download-all — so it works headless with no GUI model, and before getRoot's gateway phase runs.
        if (L.contains("SOURCE") && L["SOURCE"].is_object())
        { const long long Sz = L["SOURCE"].value("SIZE", (long long)0); if (Sz > 0) IpfsWrapper::SetExpectedSize(Cid, Sz); }
        Out.push_back({Cid, Local.string(), false});
    }, /*WithGrafts=*/true);
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
            {
                const auto &Cv = Launch->Meta["COVER"];                      // register the cover's stamped SIZE too
                if (Cv.contains("SOURCE") && Cv["SOURCE"].is_object())
                { const long long Sz = Cv["SOURCE"].value("SIZE", (long long)0); if (Sz > 0) IpfsWrapper::SetExpectedSize(Cid, Sz); }
                Out.push_back({Cid, Local.string(), true});                 // not local → fetch over IPFS (FetchToPath seeds it)
            }
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

std::set<std::filesystem::path> PackagesToAdopt(const NodeIndex &Idx, const std::vector<std::string> &LaunchIds,
                                                const std::vector<std::string> &RunnerIds,
                                                const nlohmann::ordered_json &GlobalConfigJSON)
{
    std::set<std::filesystem::path> Out;
    auto Take = [&](const std::string &Root) {
        for (const std::string &Id : ManifestModel::Closure(Idx, Root))
            if (const Node *N = Idx.Find(Id); N && N->Received && !N->BundleDir.empty()) Out.insert(N->BundleDir);
    };
    for (const std::string &Lid : LaunchIds)
    {
        Take(Lid);
        for (const std::string &Rid : RunnerChainIds(Idx, Lid, GlobalConfigJSON)) Take(Rid);
    }
    for (const std::string &Rid : RunnerIds) Take(Rid);
    return Out;
}

std::vector<std::string> RunnerChainIds(const NodeIndex &Idx, const std::string &LaunchNodeId,
                                        const nlohmann::ordered_json &GlobalConfigJSON)
{
    const Node *Launch = Idx.Find(LaunchNodeId);
    if (!Launch) return {};
    ContainerParams Cp(Launch->BundleDir);
    Cp.NodeIdx = &Idx; Cp.LaunchNodeId = LaunchNodeId; Cp.PackageUID = Launch->Uid;
    std::vector<std::string> Ids;
    for (const std::string &RunnerId : LaunchResolver::ResolveChainIds(Idx, *Launch, Cp, GlobalConfigJSON))
        if (RunnerId != LaunchResolver::kNativeTerminalId) Ids.push_back(RunnerId);
    return Ids;
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
    if (Launch->IsGraft)
    {   // a graft runs on what it applies onto: that version's runtime
        const std::string Base = DefaultGraftBase(Idx, Launch->Key());
        return Base.empty() || CollectRunnerChainTargets(Idx, Base, GlobalConfigJSON, Out, Error);
    }
    ContainerParams Cp(Launch->BundleDir);
    Cp.NodeIdx = &Idx; Cp.LaunchNodeId = LaunchNodeId; Cp.PackageUID = Launch->Uid;   // for the per-package runner pin lookup
    //Every ENTRYPOINT's chain: the entries may run on different platforms (a native and a win32 build), and a
    //hydrated game must be playable through whichever the user picks.
    std::set<std::string> Pooled;
    std::vector<std::string> Labels = Launch->EntrypointLabels();
    if (Labels.empty()) Labels.push_back(std::string());
    for (const std::string &Label : Labels)
    {
        Cp.Entrypoint = Label;
        for (const std::string &RunnerId : LaunchResolver::ResolveChainIds(Idx, *Launch, Cp, GlobalConfigJSON))
        {
            if (RunnerId == LaunchResolver::kNativeTerminalId || !Pooled.insert(RunnerId).second) continue;
            std::string E;
            if (!CollectRunnerNodeTargets(Idx, RunnerId, Out, &E))
                LogWarn("PackageCatalog::CollectRunnerChainTargets", "runner '" + RunnerId + "': " + E);
        }
    }
    (void)Error;
    return true;
}

bool HydrateNode(const NodeIndex &Idx, const std::string &LaunchNodeId, const GraftChoice &Grafts,
                 std::string *Error, const nlohmann::ordered_json *GlobalConfigJSON)
{
    std::vector<IpfsWrapper::FetchTarget> Targets;
    if (!CollectContentTargets(Idx, LaunchNodeId, Grafts, Targets, Error)) return false;
    // Full-closure: also pool the resolved runner chain's build so the game is immediately launchable (identity = the
    // node's Declare* layers; a game hydrate pulls the whole runtime, no separate "install the runner" step).
    //Discarding this told the caller the hydrate SUCCEEDED with the runner build silently absent from the batch:
    //the package downloads, reports done, and then cannot launch — and the download UI has nothing left to say.
    if (GlobalConfigJSON && !CollectRunnerChainTargets(Idx, LaunchNodeId, *GlobalConfigJSON, Targets, Error))
        LogErr("PackageCatalog::HydrateNode", "could not resolve the runner chain for '" + LaunchNodeId
                   + "' — its runtime will NOT be fetched, so the package will download but not launch"
                   + (Error && !Error->empty() ? " (" + *Error + ")" : ""));
    if (!IpfsWrapper::FetchTargetsConcurrent(Targets, Error))
    { LogErr("PackageCatalog::HydrateNode", "hydrate failed for '" + LaunchNodeId + "'"); return false; }
    LogSucc("PackageCatalog::HydrateNode", "Hydrated node '" + LaunchNodeId + "' (" + std::to_string(Targets.size()) + " file(s)).");
    return true;
}

} // namespace PackageCatalog
