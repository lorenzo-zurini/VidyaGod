// Package sources (IPFS folder CIDs): their folders, syncing them into LIBRARY, adding, upgrading and removing
// one, and merging a worker's change to the config's lists back into the live config.
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
    for (char &c : N) if (c == '/' || c == '\\' || c == ':') c = '_';
    //A name of only dots and blanks is no folder of its own ("." is LIBRARY, ".." the data dir): the CID's prefix instead.
    if (N.find_first_not_of(". \t") == std::string::npos) { const std::string C = PackageSourceCID(S); N = C.size() > 12 ? C.substr(0, 12) : C; }
    return N.find_first_not_of(". \t") == std::string::npos ? std::string("cidsource") : N;
}
//The file a source's own fetch leaves in its folder, holding its CID: the folder is the source's to delete.
static constexpr const char *kSourceMarker = ".vgsource";
static bool SourceOwnsDir(const std::string &Dir, const std::string &Cid)
{
    std::ifstream F(std::filesystem::path(Dir) / kSourceMarker, std::ios::binary);
    std::string Got;
    return F && std::getline(F, Got) && !Cid.empty() && Got == Cid;
}
//A library this machine publishes (its share sheet names it): its folder is the user's own, never a source's.
static bool PublishedLibrary(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &Dir)
{
    if (!GlobalConfigJSON.contains("ShareSheet")) return false;
    const std::string Name = std::filesystem::path(Dir).filename().string();
    for (const auto &E : SheetEntries(GlobalConfigJSON["ShareSheet"])) if (E.Lib == Name) return true;
    return false;
}
//The packages a fetched tree holds: "." when the tree is one package, else its package folders' names.
static std::vector<std::string> FetchedPackages(const std::string &Tree);
//The marker, written: the source's CID, then the packages its fetch brought (one per line) — what the source indexes
//and what removing it deletes, never a package made in its folder since. False (and said) when it cannot be written.
static bool MarkSourceDir(const std::string &Dir, const std::string &Cid, const std::vector<std::string> &Packages)
{
    std::ofstream F(std::filesystem::path(Dir) / kSourceMarker, std::ios::binary | std::ios::trunc);
    F << Cid << "\n";
    for (const std::string &P : Packages) F << P << "\n";
    if (F.flush()) return true;
    LogWarn("PackageCatalog::MarkSourceDir", "could not mark " + Dir + " as the source " + Cid + "'s: removing the source will leave it in place");
    return false;
}
//The packages the marker lists (FetchedPackages at the fetch): their folders under Dir ("." = Dir itself).
static std::set<std::string> SourcePackages(const std::string &Dir)
{
    std::set<std::string> Out;
    std::ifstream F(std::filesystem::path(Dir) / kSourceMarker, std::ios::binary);
    std::string L;
    if (!std::getline(F, L)) return Out;                // the CID
    while (std::getline(F, L))
        if (!L.empty()) Out.insert(L == "." ? Dir : (std::filesystem::path(Dir) / L).string());
    return Out;
}
//A folder CID: a CIDv0 (Qm + 44 base58) or a CIDv1 in base32 (b…, lower case).
static bool LooksLikeFolderCid(const std::string &C)
{
    static const std::string B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    if (C.size() == 46 && C.rfind("Qm", 0) == 0) return C.find_first_not_of(B58) == std::string::npos;
    if (C.size() >= 50 && C[0] == 'b') return C.find_first_not_of("abcdefghijklmnopqrstuvwxyz234567") == std::string::npos;
    return false;
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
bool PathUnderManagedSource(const nlohmann::ordered_json &GlobalConfigJSON, const std::filesystem::path &Path)
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


static std::vector<std::string> FetchedPackages(const std::string &Tree)
{
    if (ScanBundleIdentity(Tree).Valid) return { "." };
    std::vector<std::string> Out;
    QDir D(QString::fromStdString(Tree));
    for (const QString &Sub : D.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        if (ScanBundleIdentity(D.filePath(Sub).toStdString()).Valid) Out.push_back(Sub.toStdString());
    return Out;
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
            //Fetched here: the folder and the packages it brought are this source's (removing the source may delete them).
            MarkSourceDir(Dir, Cid, FetchedPackages(Dir));
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

        //Its own folder: the packages its fetch brought, as the marker lists them (a package made in the folder since is
        //not the source's). Else, as found.
        const std::set<std::string> Brought = SourceOwnsDir(Dir, Cid) ? SourcePackages(Dir) : std::set<std::string>{};
        const auto Mine = [&](const std::string &PkgDir) { return !SourceOwnsDir(Dir, Cid) || Brought.count(PkgDir); };
        const BundleIdentity Self = ScanBundleIdentity(Dir);
        if (Self.Valid)
        { if (Mine(Dir)) Upsert(Dir, Self); }                                         // single-package CID
        else
        {
            QDir D(QString::fromStdString(Dir));                                      // collection CID → scan subdirs
            for (const QString &Sub : D.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
            {
                const QString PkgDir = D.filePath(Sub);
                const BundleIdentity Id = ScanBundleIdentity(PkgDir.toStdString());
                if (!Id.Valid || !Mine(PkgDir.toStdString())) continue;
                Upsert(PkgDir.toStdString(), Id);
            }
        }
        LogOut("PackageCatalog::SyncPackageSources", "Indexed CID source '" + PackageSourceName(Src) + "' ("
               + std::to_string(Packages) + " package(s), " + std::to_string(Games) + " launchable).");
    }
    return Indexed;
}

std::string SourceOfPackage(const nlohmann::ordered_json &GlobalConfigJSON, const std::filesystem::path &BundleDir)
{
    if (!GlobalConfigJSON.contains("LIBRARY") || !GlobalConfigJSON["LIBRARY"].is_array()) return {};
    std::error_code Ec;
    const auto Want = std::filesystem::weakly_canonical(BundleDir, Ec);
    for (const auto &E : GlobalConfigJSON["LIBRARY"])
    {
        if (!E.is_object() || std::filesystem::weakly_canonical(E.value("PATH", std::string()), Ec) != Want) continue;
        const std::string Cid = E.value("CIDSOURCE", std::string());
        if (Cid.empty() || !GlobalConfigJSON.contains("Settings") || !GlobalConfigJSON["Settings"].contains("PackageSources")) return {};
        for (const auto &S : GlobalConfigJSON["Settings"]["PackageSources"]) if (PackageSourceCID(S) == Cid) return Cid;
        return {};
    }
    return {};
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

bool AddPackageSource(nlohmann::ordered_json &GlobalConfigJSON, const std::string &Cid, const std::string &Name, std::string *Why)
{
    const auto No = [&](const std::string &W) { if (Why) *Why = W; return false; };
    if (Cid.empty()) return No("Enter the source's CID.");
    if (!LooksLikeFolderCid(Cid)) return No("'" + Cid + "' is not a CID (a folder CID starts with Qm or b).");
    if (!GlobalConfigJSON.contains("Settings") || !GlobalConfigJSON["Settings"].is_object())
        GlobalConfigJSON["Settings"] = nlohmann::ordered_json::object();
    auto &S = GlobalConfigJSON["Settings"];
    if (!S.contains("PackageSources") || !S["PackageSources"].is_array())
        S["PackageSources"] = nlohmann::ordered_json::array();
    for (const auto &E : S["PackageSources"]) if (PackageSourceCID(E) == Cid) return No("That source is already added.");
    nlohmann::ordered_json Src;
    Src["CID"] = Cid;
    if (!Name.empty()) Src["NAME"] = Name;
    //Its folder must be new: a name that is a folder already in the library (an authored library, another source's)
    //would make that folder the source's — and removing the source would delete it.
    {
        const std::string Dir = PackageSourceDir(GlobalConfigJSON, Src);
        const std::string Folder = std::filesystem::path(Dir).filename().string();
        for (const auto &E : S["PackageSources"])
            if (PackageSourceDir(GlobalConfigJSON, E) == Dir) return No("Another source already uses the folder '" + Folder + "': give this one another name.");
        std::error_code Ec;
        if (PublishedLibrary(GlobalConfigJSON, Dir) || (!std::filesystem::is_empty(Dir, Ec) && !Ec && !SourceOwnsDir(Dir, Cid)))
            return No("A folder named '" + Folder + "' is already in the library: give the source another name.");
    }
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
    //Content that only changes its place keeps its references: they follow it, every file in one move (below). A drop
    //and a re-add of each read every byte of it again — gigabytes, for a file that only moved.
    std::vector<std::pair<std::string, std::string>> Followed;

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
            Followed.push_back({ Path, Dest.string() });   // the reference still points at the OLD path: it follows
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
        Followed.push_back({ FromTo.first, FromTo.second });
    }
    if (std::string MErr; !Followed.empty() && IpfsWrapper::MoveRefs(Followed, &MErr) < 0)
    {
        //The references did not follow: re-pointed the long way, each dropped (a plain re-add dedup-skips and would
        //leave an unreadable block advertised) and added again from where it is now.
        LogWarn("PackageCatalog::ApplySourceUpgrade", "moving the references failed (" + MErr + "): re-adding the moved files");
        for (const auto &[From, To] : Followed)
        {
            std::string AddErr;
            const std::string Was = IpfsWrapper::ComputeCid(To, &AddErr);
            if (!Was.empty()) IpfsWrapper::DropRef(Was);
            if (IpfsWrapper::AddNoCopy(To, &AddErr).empty())
                LogWarn("PackageCatalog::ApplySourceUpgrade", "moved content not re-seeded: " + To + " (" + AddErr + ")");
        }
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
    const std::vector<std::string> NewPackages = FetchedPackages(P.StagingDir);   // what the new CID brings
    fs::remove_all(P.StagingDir, Ec);

    // 5. Point config at the new CID, and drop LIBRARY rows for packages that no longer exist so the catalog matches.
    auto &Sources = GlobalConfigJSON["Settings"]["PackageSources"];
    for (auto &S : Sources)
        if (PackageSourceName(S) == P.Name) { if (S.is_object()) S["CID"] = P.NewCid; else S = nlohmann::ordered_json{{"NAME", P.Name}, {"CID", P.NewCid}}; }
    MarkSourceDir(P.Dir, P.NewCid, NewPackages);   // the folder is the source's at its new CID, with what it brings
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
    const std::string Cid = PackageSourceCID(Arr[Index]);
    //Its files go only from a folder that is the source's (its marker: its fetch made it, or the sync marked it) and
    //never from a library this machine publishes: otherwise the source and the entries it made go, the files stay.
    const bool Owned = SourceOwnsDir(Dir, Cid) && !PublishedLibrary(GlobalConfigJSON, Dir)
                    && PathUnder(PackageSourcesRoot(GlobalConfigJSON), Dir)
                    && std::filesystem::path(Dir).lexically_normal() != std::filesystem::path(PackageSourcesRoot(GlobalConfigJSON)).lexically_normal();
    //The packages the source's sync indexed (its rows): what is the source's in its folder — not a package the user
    //made in there.
    const std::set<std::string> Brought = SourcePackages(Dir);
    const auto Ours = [&](const nlohmann::ordered_json &E) {
        return E.is_object() && E.value("CIDSOURCE", std::string()) == Cid && PathUnder(Dir, E.value("PATH", std::string()))
            && (!Owned || Brought.count(E.value("PATH", std::string())));
    };
    auto *Lib = GlobalConfigJSON.contains("LIBRARY") && GlobalConfigJSON["LIBRARY"].is_array() ? &GlobalConfigJSON["LIBRARY"] : nullptr;
    if (!Owned)
    {
        if (Lib) for (auto It = Lib->begin(); It != Lib->end(); ) It = Ours(*It) ? Lib->erase(It) : std::next(It);
        LogWarn("PackageCatalog::RemovePackageSource", "the source is removed, its folder " + Dir + " is left in place: "
                "it is not the source's (delete it yourself if it is)");
        Arr.erase(Arr.begin() + Index);
        return;
    }
    //Whatever goes, the folder is not the source's any more: a source added again fetches into a folder of its own.
    std::error_code EcM;
    std::filesystem::remove(std::filesystem::path(Dir) / kSourceMarker, EcM);

    if (KeepInstalled)
    {
        // Never rm a package the user has INSTALLED: dirs with hydrated content stay (files kept, still seeded), their
        // rows kept without the source (nothing can download them again: they are not dehydrated from the Library);
        // only stub-only packages go. The source dir itself is removed only if it ends up empty.
        if (Lib)
            for (int i = (int)Lib->size() - 1; i >= 0; --i)
            {
                if (!Ours((*Lib)[i])) continue;
                const std::string P = (*Lib)[i].value("PATH", std::string());
                if (DirHasContent(P)) { (*Lib)[i].erase("CIDSOURCE"); (*Lib)[i]["KEPT_FROM"] = Cid; continue; }
                std::error_code EcP; std::filesystem::remove_all(P, EcP); Lib->erase(Lib->begin() + i);
            }
        std::error_code EcD;
        std::filesystem::remove(Dir, EcD);                               // removes the source dir only if now empty
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
        //Together: a drop per CID, each its own commit, unpins and compaction request, for a whole source's content.
        //One call drops them all or none (no node to drop them in).
        if (Mine.empty()) {}
        else if (IpfsWrapper::DropRefs({ Mine.begin(), Mine.end() }))
            LogSucc("PackageCatalog::RemovePackageSource", "un-seeded the " + std::to_string(Mine.size())
                    + " CID(s) only the removed source held");
        else
            LogWarn("PackageCatalog::RemovePackageSource", "could not un-seed the " + std::to_string(Mine.size())
                    + " CID(s) only the removed source held (no IPFS node): a heal drops them once their files are gone");
    }

    //The source's packages go (each the sync indexed), then the folder if nothing else is in it — a package the user
    //made in there stays.
    if (Lib)
        for (auto It = Lib->begin(); It != Lib->end(); )
        {
            if (!Ours(*It)) { ++It; continue; }
            std::error_code EcP; std::filesystem::remove_all(It->value("PATH", std::string()), EcP);
            It = Lib->erase(It);
        }
    std::error_code Ec;
    std::filesystem::remove(Dir, Ec);
    if (std::filesystem::exists(Dir, Ec))
        LogWarn("PackageCatalog::RemovePackageSource", "the source's packages are deleted; its folder " + Dir + " holds other things and stays");
    Arr.erase(Arr.begin() + Index);
}

} // namespace PackageCatalog
