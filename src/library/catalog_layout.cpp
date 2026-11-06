// The library on disk: where LIBRARY, CATALOG and ASSETS are, locally-added packages, package folders and their
// titles, the golden marker, and what a bundle folder is (ScanBundleIdentity).
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

// ----- per-package user settings -----

// ----- on-disk locations -----

//The managed library root — repos clone here (one subfolder per repo); every package hydrates in place beside its
//manifest. Overridable via Settings.Paths.LibraryRoot; default ~/.VidyaGod/LIBRARY.
std::string LibraryDir(const nlohmann::ordered_json &GlobalConfigJSON)
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
bool PathUnder(const std::filesystem::path &Base, const std::filesystem::path &Path)
{
    std::error_code Ec;
    const std::filesystem::path B = std::filesystem::weakly_canonical(Base, Ec);
    const std::filesystem::path P = std::filesystem::weakly_canonical(Path, Ec);
    auto It = std::mismatch(B.begin(), B.end(), P.begin(), P.end());
    return It.first == B.end();                                     // B is a prefix of P
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

} // namespace PackageCatalog
