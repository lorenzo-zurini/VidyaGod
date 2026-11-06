// What friends share, received: planning and landing a share's package folders, adopting them into the library,
// pruning what a newer share replaced, and whether a received closure is complete.
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

// Where a received package folder lands inside its package dir (NodeGraph::kPackageFolderDir — its nodes' bundle is
// the package dir).
static constexpr const char *kPackageFolderDir = NodeGraph::kPackageFolderDir;
// Bounds for the received-share planner (a friend's snapshot is UNTRUSTED input).
static constexpr size_t kMaxFriendRootsPerLib = 20000;    // cap a hostile/huge per-library item set (Go caps at 100k)
static constexpr size_t kMaxFriendRootsTotal  = 100000;   // snapshot-wide cap across all of one peer's libraries
using namespace Shelf;       // the library walks rows and grafts as the plan sees them
using namespace Installed;

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
std::string SafeSegment(const std::string &In)
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
void RemoveLandedNode(const std::filesystem::path &F, const std::string &Cid,
                      const std::vector<std::filesystem::path> &Elsewhere)
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
std::optional<std::vector<std::string>> FolderNodes(const std::string &FolderCid, std::string *Error)
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

} // namespace PackageCatalog
