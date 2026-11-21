// What friends share, received: planning and landing a share's node files, adopting them into the library,
// pruning what a newer share replaced, and whether a received closure is complete.
#include "packagecatalog.h"
#include "sessions.h"
#include "nodestore.h"
#include "contentfile.h"
#include "nodefiles.h"
#include "cid.h"
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
#include <cstdio>
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

// Bounds for the received-share planner (a friend's snapshot is UNTRUSTED input).
static constexpr size_t kMaxFriendRootsPerLib = 100000;   // node entries per library (Go caps a library at 100k too)
static constexpr size_t kMaxFriendRootsTotal  = 100000;   // node entries across all of one peer's libraries
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

std::vector<IpfsWrapper::FetchTarget> ReceivedFetchTargets(const std::vector<ReceivedFetch> &Plan)
{
    std::vector<IpfsWrapper::FetchTarget> Out;
    Out.reserve(Plan.size());
    std::set<std::string> Dirs;
    for (const ReceivedFetch &T : Plan)
    {
        if (const std::string D = std::filesystem::path(T.Dest).parent_path().string(); Dirs.insert(D).second)
            IpfsWrapper::DropRedirectsUnder(D);
        Out.push_back(IpfsWrapper::FetchTarget{ T.Cid, T.Dest, /*Optional=*/true, /*Dir=*/false, /*Verify=*/true });
    }
    return Out;
}

namespace {
//The last 8 hex digits of the 64-bit FNV-1a hash of S: what tells apart two of a friend's packages whose names make
//one folder name (chapter 15 §15.5).
std::string Fnv8(const std::string &S)
{
    uint64_t H = 1469598103934665603ull;
    for (unsigned char C : S) { H ^= C; H *= 1099511628211ull; }
    char Buf[17];
    std::snprintf(Buf, sizeof Buf, "%016llx", static_cast<unsigned long long>(H));
    return std::string(Buf + 8);
}
bool PlainCid(const std::string &C)
{
    return !C.empty() && C.size() <= 128 && std::all_of(C.begin(), C.end(), [](unsigned char c) { return std::isalnum(c); });
}
} // namespace

std::map<std::pair<std::string, std::string>, std::string> ReceivedFolderNames(const nlohmann::ordered_json &Libs)
{
    // A friend's package names, made folder names: names that clash (compared without case, as a case-insensitive file
    // system compares them) are EACH told apart by their name's hash — never one plain and one qualified, which would
    // make the name depend on the order the friend's packages arrive in.
    std::map<std::string, std::set<std::string>> Pkgs;                                     // lib → its package names
    for (const SheetEntry &E : WireEntries(Libs)) Pkgs[E.Lib].insert(E.Pkg);
    std::map<std::pair<std::string, std::string>, std::string> Out;
    for (const auto &[Lib, Names] : Pkgs)
    {
        std::map<std::string, std::vector<std::string>, ContentFile::NameLess> ByFolder;
        for (const std::string &P : Names) ByFolder[NodeFiles::SafeName(P)].push_back(P);
        for (const auto &[Folder, Claim] : ByFolder)
            for (const std::string &P : Claim)
                Out[{ Lib, P }] = Claim.size() == 1 ? NodeFiles::SafeName(P)
                                                    : NodeFiles::SafeName(NodeFiles::SafeName(P).substr(0, 108) + " (" + Fnv8(P) + ")");
    }
    return Out;
}

std::vector<ReceivedFetch> PlanReceivedFetches(const nlohmann::ordered_json &GlobalConfigJSON,
                                               const std::string &NickLabel, const nlohmann::ordered_json &Libs)
{
    namespace fs = std::filesystem;
    // UNTRUSTED input (a friend's snapshot = their share sheet's entries): every path segment goes through
    // NodeFiles::SafeName — a plain filename, no separators, never "", "." or "..". The sheet is the allow-list: one
    // file per entry, at that entry's path, holding that entry's CID (the fetch verifies the bytes).
    std::vector<ReceivedFetch> Out;
    const fs::path Root = fs::path(CatalogRootDir(GlobalConfigJSON));   // received stubs live in CATALOG, not LIBRARY
    const fs::path LibRoot = fs::path(LibraryRootDir(GlobalConfigJSON));
    const auto Folders = ReceivedFolderNames(Libs);
    std::map<std::pair<std::string, std::string>, std::vector<SheetEntry>> ByPkg;        // (lib, pkg) → its entries
    for (SheetEntry &E : WireEntries(Libs))
        if (PlainCid(E.Cid)) ByPkg[{ E.Lib, E.Pkg }].push_back(std::move(E));
    std::map<std::string, size_t> PerLib;
    size_t Total = 0;
    std::set<fs::path> Dirs;
    for (auto &[LP, Entries] : ByPkg)
    {
        // Bounds count node entries, and take a package whole or not at all: a package cut short would read complete.
        if (Total + Entries.size() > kMaxFriendRootsTotal || PerLib[LP.first] + Entries.size() > kMaxFriendRootsPerLib)
        {
            LogWarn("PackageCatalog::PlanReceivedFetches", "package '" + LP.second + "' of library '" + LP.first
                    + "' is past the " + std::to_string(kMaxFriendRootsTotal) + "-entry bound — left out");
            continue;
        }
        Total += Entries.size();
        PerLib[LP.first] += Entries.size();
        const fs::path Pkg = Root / NodeFiles::SafeName(NickLabel + " - " + LP.first) / Folders.at(LP);
        if (!Dirs.insert(Pkg).second) continue;                       // a qualified name that is another's plain one
        // A package the library already holds under this library and folder name was INSTALLED (adopted out of
        // CATALOG) or authored here: it is ours now and is not re-landed as a stub beside itself (a node has one home;
        // a stub beside it would hold copies).
        std::error_code Ec;
        if (fs::is_directory(LibRoot / NodeFiles::SafeName(LP.first) / Pkg.filename(), Ec)) continue;
        // Its files: names that clash (without case) are each bumped by their CID — the same CID twice is one file.
        std::map<std::string, std::map<std::string, std::string>, ContentFile::NameLess> ByName;   // name → CID → its own spelling
        for (const SheetEntry &E : Entries)
        {
            std::string Name = NodeFiles::SafeName(E.File);
            if (fs::path(Name).extension() != ".json") Name += ".json";  // a node file, whatever the friend named it
            ByName[Name].emplace(E.Cid, Name);
        }
        //One file, one CID: a bumped name another entry has as its own (a friend who named a file that way) is
        //planned once — whichever comes first — never two fetches racing for one file.
        std::set<std::string, ContentFile::NameLess> Planned;
        for (const auto &[Name, Cids] : ByName)
            for (const auto &[C, Own] : Cids)
            {
                const std::string File = Cids.size() == 1 ? Own : ContentFile::Bumped(Own, C);
                if (!Planned.insert(File).second)
                {
                    LogWarn("PackageCatalog::PlanReceivedFetches", "'" + File + "' of package '" + LP.second
                            + "' is named by two entries — " + C + " left out");
                    continue;
                }
                Out.push_back(ReceivedFetch{ C, (Pkg / File).generic_string() });
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

// What each received package holds: every friend's plan replayed (package dir → file → CID). Exact per friend — two
// friends sharing the same library and package name land in different dirs, and each dir is judged against its own
// friend's entries, never the other's.
static std::map<std::filesystem::path, std::map<std::filesystem::path, std::string>> ReceivedPackages(const nlohmann::ordered_json &Config)
{
    std::map<std::filesystem::path, std::map<std::filesystem::path, std::string>> Out;
    if (!Config.contains("FriendLibraries") || !Config["FriendLibraries"].is_object()) return Out;
    for (const auto &[Peer, Libs] : Config["FriendLibraries"].items())
        for (const ReceivedFetch &T : PlanReceivedFetches(Config, ReceivedNickLabel(Peer), Libs))
        {
            const std::filesystem::path F(T.Dest);
            Out[F.parent_path()][F] = T.Cid;
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
            if (NodeFiles::SafeName(Nick + " - " + L) == LibDirName) return L;
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
    const fs::path Dest = fs::path(LibraryRootDir(Config)) / NodeFiles::SafeName(Lib) / NodeFiles::SafeName(Src.filename().string());
    if (!NodeGraph::PathWithin(LibraryRootDir(Config), Dest))
    { if (Error) *Error = "refused: " + Dest.string() + " is outside the library"; return false; }
    if (!fs::is_directory(Src, Ec))
    {
        if (fs::is_directory(Dest, Ec)) { if (NewDir) *NewDir = Dest; return true; }   // installed meanwhile
        if (Error) *Error = "not a received package dir: " + PkgDir.string();
        return false;
    }
    if (fs::exists(Dest, Ec)) { if (Error) *Error = "a package named '" + Src.filename().string() + "' already exists in library '" + Lib + "'"; return false; }
    //Every file fetched into the package is referenced in place, and the references move to the installed place BEFORE
    //the package does. The move is
    //idempotent and never looks at the files, so a failure or crash anywhere up to the rename leaves a stub still
    //Received in CATALOG, which the next download adopts again. Moved after the rename, a failure left an installed
    //package (never adopted again) whose files read as missing, unservable for good.
    std::string MErr;
    if (IpfsWrapper::MoveRefs({{Src.string(), Dest.string()}}, &MErr) < 0)
    { if (Error) *Error = "cannot re-point the references of " + Src.string() + " to " + Dest.string() + ": " + MErr; return false; }
    auto Refuse = [&](const std::string &Why) {                            // the package stays where its files are
        std::string BErr;
        if (IpfsWrapper::MoveRefs({{Dest.string(), Src.string()}}, &BErr) < 0)
            LogErr("PackageCatalog::AdoptReceivedPackage", "cannot point the references back at " + Src.string() + ": " + BErr);
        if (Error) *Error = Why;
        return false;
    };
    IpfsWrapper::ForgetDestsUnder(Src.string());      // the queue must never re-land a file into the old stub
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
    // A later request for the old paths lands in the installed package; registered before the queue forgets them, so
    // no request in between re-materialises the stub.
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
    for (const auto &[Dir, Files] : ReceivedPackages(Config))
        for (const auto &[F, C] : Files)
            if (!std::filesystem::exists(F, Ec)) return true;
    return false;
}

bool LandReceivedPackages(const nlohmann::ordered_json &Config, std::string *Error)
{
    const IpfsWrapper::ForegroundHold Hold;                             // package after package: no gap for walks
    // Completes and checks each received package against its entries: a missing file is fetched, and every file is
    // verified — one that is not the bytes its entry says (or not a canonical node) is removed and the package
    // reported, never repaired.
    namespace fs = std::filesystem;
    std::error_code Ec;
    bool Ok = true;
    for (const auto &[Dir, Files] : ReceivedPackages(Config))
    {
        //A file there holding another node (the entry's previous generation, under the same name) goes first: the
        //fetch takes a file that is there for done when the node it asks for is held here.
        for (const auto &[F, C] : Files)
            if (fs::exists(F, Ec) && !NodeGraph::VerifyLanded(F, C, nullptr, nullptr))
            {
                std::ifstream In(F, std::ios::binary);
                const std::string Old = Cid::OfBytes(std::string((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>()));
                In.close();
                RemoveLandedNode(F, Old);
            }
        std::vector<IpfsWrapper::FetchTarget> Batch;
        for (const auto &[F, C] : Files)
        {
            if (fs::exists(F, Ec)) continue;
            Batch.push_back(IpfsWrapper::FetchTarget{ C, F.string(), /*Optional=*/false, /*Dir=*/false, /*Verify=*/true });
        }
        if (!Batch.empty())
        {
            std::string WErr;
            if (!IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch(Batch), 10 * 60 * 1000, &WErr))
            { if (Error) *Error = "package " + Dir.filename().string() + ": " + WErr; Ok = false; continue; }
        }
        for (const auto &[F, C] : Files)
        {
            std::string VErr;
            if (NodeGraph::VerifyLanded(F, C, nullptr, &VErr)) continue;
            RemoveLandedNode(F, C);
            if (Error) *Error = "package " + Dir.filename().string() + ": node " + F.filename().string() + " " + VErr;
            LogWarn("PackageCatalog::LandReceivedPackages", "package '" + Dir.filename().string() + "': node " + F.filename().string()
                    + " (" + C + ") refused: " + VErr);
            Ok = false;
        }
    }
    return Ok;
}

int PruneStaleReceived(const NodeIndex &Idx, const nlohmann::ordered_json &Config)
{
    // A received package holds exactly what its entries name (one home: what it contains from other packages lives
    // there). Any other node file in it is an older generation's — a re-published node under its previous CID. Remove it.
    namespace fs = std::filesystem;
    std::error_code Ec;
    int Removed = 0;
    const auto Packages = ReceivedPackages(Config);                        // one read, not one per stale file
    std::map<std::string, std::vector<fs::path>> Where;                    // cid → every path an entry gives it
    for (const auto &[Dir, Files] : Packages) for (const auto &[F, C] : Files) Where[C].push_back(F);
    for (const auto &[Dir, Files] : Packages)
    {
        if (!fs::is_directory(Dir, Ec)) continue;
        for (const auto &[Cid, F] : NodeFiles::In(Dir))
        {
            if (Files.count(F.Path)) continue;
            std::vector<fs::path> Elsewhere = Where[Cid];                  // the same node's other landed copies
            if (const Node *N = Idx.Find(Cid); N && !N->File.empty()) Elsewhere.push_back(N->File);
            RemoveLandedNode(F.Path, Cid, Elsewhere);
            if (!fs::exists(F.Path, Ec)) ++Removed;
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
