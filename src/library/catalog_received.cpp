// What peers share, received: planning and landing a share's node files, adopting them into the library,
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

// Bounds for the received-share planner (a peer's snapshot is UNTRUSTED input).
static constexpr size_t kMaxPeerRootsPerLib = 100000;   // node entries per library (Go caps a library at 100k too)
static constexpr size_t kMaxPeerRootsTotal  = 100000;   // node entries across all of one peer's libraries
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
            if (Name == ".vgsource") continue;                          // a source's marker, not content
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
        IpfsWrapper::FetchTarget F{ T.Cid, T.Dest, /*Optional=*/true, /*Dir=*/false, /*Verify=*/true };
        F.Small = true;                                   // a node file: a peer's snapshot names it — at most a node's size
        Out.push_back(F);
    }
    return Out;
}

namespace {
//Node files of peers' shares this run refused (they did not verify, or were larger than a node): not fetched again,
//not counted as missing — one bad node made the receiver fetch it again, back to back, for as long as it ran.
//Two kinds: one that did not verify (the bytes a peer gave were not the CID's) is forgotten at a peer's next changed
//snapshot; one larger than a node is the CID's own, refused for the run.
std::mutex RefusedMu;
std::set<std::string> RefusedNodes, OversizeNodes;
bool Refused(const std::string &Cid) { std::lock_guard<std::mutex> G(RefusedMu); return RefusedNodes.count(Cid) > 0 || OversizeNodes.count(Cid) > 0; }
void Refuse(const std::string &Cid, bool Oversize = false)
{
    std::lock_guard<std::mutex> G(RefusedMu);
    (Oversize ? OversizeNodes : RefusedNodes).insert(Cid);
}
}

void ForgetRefusedNodes() { std::lock_guard<std::mutex> G(RefusedMu); RefusedNodes.clear(); }

namespace {
//The last 8 hex digits of the 64-bit FNV-1a hash of S: what tells apart two of a peer's packages whose names make
//one folder name (chapter 15 §15.5).
std::string Fnv8(const std::string &S)
{
    uint64_t H = 1469598103934665603ull;
    for (unsigned char C : S) { H ^= C; H *= 1099511628211ull; }
    char Buf[17];
    std::snprintf(Buf, sizeof Buf, "%016llx", static_cast<unsigned long long>(H));
    return std::string(Buf + 8);
}
//A node's CID as the format makes it (Cid::OfBytes): one raw block, CIDv1 base32 sha2-256 — it starts "bafkrei".
//Anything else a peer's snapshot names (a folder, a file DAG: "bafybei…", "Qm…") is no node file and is not fetched: a
//file DAG's root can say any size, and a few kilobytes of it written out can fill the disk. (The fetch refuses one
//too; a malformed rest fails to decode there.)
bool PlainCid(const std::string &C)
{
    return C.size() <= 128 && C.rfind("bafkrei", 0) == 0
        && std::all_of(C.begin(), C.end(), [](unsigned char c) { return std::islower(c) || std::isdigit(c); });
}
} // namespace

std::map<std::pair<std::string, std::string>, std::string> ReceivedFolderNames(const nlohmann::ordered_json &Libs)
{
    // A peer's package names, made folder names: names that clash (compared without case, as a case-insensitive file
    // system compares them) are EACH told apart by their name's hash — never one plain and one qualified, which would
    // make the name depend on the order the peer's packages arrive in.
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
    // UNTRUSTED input (a peer's snapshot = their share sheet's entries): every path segment goes through
    // NodeFiles::SafeName — a plain filename, no separators, never "", "." or "..". The sheet is the allow-list: one
    // file per entry, at that entry's path, holding that entry's CID (the fetch verifies the bytes).
    std::vector<ReceivedFetch> Out;
    const fs::path Root = fs::path(CatalogRootDir(GlobalConfigJSON));   // received stubs live in CATALOG, not LIBRARY
    const fs::path LibRoot = fs::path(LibraryRootDir(GlobalConfigJSON));
    const auto Folders = ReceivedFolderNames(Libs);
    std::map<std::pair<std::string, std::string>, std::vector<SheetEntry>> ByPkg;        // (lib, pkg) → its entries
    //A package with an entry that names no node file (not a raw block's CID) is left out whole — planned without it,
    //it would read complete — and said.
    std::set<std::pair<std::string, std::string>> Unplannable;
    for (SheetEntry &E : WireEntries(Libs))
    {
        if (PlainCid(E.Cid)) ByPkg[{ E.Lib, E.Pkg }].push_back(std::move(E));
        else Unplannable.insert({ E.Lib, E.Pkg });
    }
    for (const auto &LP : Unplannable)
    {
        ByPkg.erase(LP);
        LogWarn("PackageCatalog::PlanReceivedFetches", "package '" + LP.second + "' of library '" + LP.first
                + "' names a file that is no node (not one raw block) — left out");
    }
    std::map<std::string, size_t> PerLib;
    size_t Total = 0;
    std::set<fs::path> Dirs;
    for (auto &[LP, Entries] : ByPkg)
    {
        // Bounds count node entries, and take a package whole or not at all: a package cut short would read complete.
        if (Total + Entries.size() > kMaxPeerRootsTotal || PerLib[LP.first] + Entries.size() > kMaxPeerRootsPerLib)
        {
            LogWarn("PackageCatalog::PlanReceivedFetches", "package '" + LP.second + "' of library '" + LP.first
                    + "' is past the " + std::to_string(kMaxPeerRootsTotal) + "-entry bound — left out");
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
            if (fs::path(Name).extension() != ".json") Name += ".json";  // a node file, whatever the peer named it
            ByName[Name].emplace(E.Cid, Name);
        }
        //One file, one CID: a bumped name another entry has as its own (a peer who named a file that way) is
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
    RemoveLandedNodes({ LandedNode{ F, Cid, Elsewhere } });
}

void RemoveLandedNodes(const std::vector<LandedNode> &Nodes)
{
    std::error_code Ec;
    std::vector<const LandedNode *> Dropped;
    std::vector<std::string> Cids;
    for (const LandedNode &N : Nodes)
    {
        if (!std::filesystem::remove(N.File, Ec)) continue;
        if (!IpfsWrapper::CidMissing(N.Cid)) continue;               // its reference is to another copy: nothing to do
        Dropped.push_back(&N);                                       // it was to this file: gone with it
        Cids.push_back(N.Cid);
    }
    IpfsWrapper::DropRefs(Cids);
    //The same node landed in another package too (a node two packages share): seed that copy, or it would be on disk
    //and silently not held — never served, never re-shared.
    for (const LandedNode *N : Dropped)
        for (const std::filesystem::path &S : N->Elsewhere)
            if (S != N->File && std::filesystem::is_regular_file(S, Ec))
            {
                std::string AErr;
                if (IpfsWrapper::AddNoCopy(S.string(), &AErr) == N->Cid) break;
            }
}

std::string ReceivedLiveNick(const std::string &PeerID)
{
    for (const auto &C : IpfsWrapper::PeerList()) if (C.PeerID == PeerID && !C.Nick.empty()) return C.Nick;
    return {};
}

static std::string IdTail(const std::string &PeerID, size_t N) { return PeerID.size() > N ? PeerID.substr(PeerID.size() - N) : PeerID; }

std::string ReceivedNickLabel(const nlohmann::ordered_json &Config, const std::string &PeerID)
{
    if (Config.contains("PeerLabels") && Config["PeerLabels"].is_object() && Config["PeerLabels"].contains(PeerID)
        && Config["PeerLabels"][PeerID].is_string() && !Config["PeerLabels"][PeerID].get<std::string>().empty())
        return Config["PeerLabels"][PeerID].get<std::string>();
    const std::string Nick = ReceivedLiveNick(PeerID);
    return Nick.empty() ? IdTail(PeerID, 8) : Nick;   // never an empty dir prefix
}

std::string AssignReceivedLabel(nlohmann::ordered_json &Config, const std::string &PeerID, const std::string &LiveNick)
{
    if (!Config.contains("PeerLabels") || !Config["PeerLabels"].is_object()) Config["PeerLabels"] = nlohmann::ordered_json::object();
    auto &Labels = Config["PeerLabels"];
    if (Labels.contains(PeerID) && Labels[PeerID].is_string() && !Labels[PeerID].get<std::string>().empty())
        return Labels[PeerID].get<std::string>();
    //Taken: another peer's label, compared as the folder names compare (case-insensitive file systems).
    const auto Key = [](const std::string &L) { std::string K = NodeFiles::SafeName(L); for (char &C : K) C = static_cast<char>(std::tolower(static_cast<unsigned char>(C))); return K; };
    std::set<std::string> Taken;
    for (const auto &[P, L] : Labels.items()) if (P != PeerID && L.is_string()) Taken.insert(Key(L.get<std::string>()));
    std::string Label = LiveNick.empty() ? IdTail(PeerID, 8) : LiveNick;
    if (Taken.count(Key(Label))) Label += " [" + IdTail(PeerID, 6) + "]";
    if (Taken.count(Key(Label))) Label = LiveNick + " [" + PeerID + "]";
    Labels[PeerID] = Label;
    return Label;
}

// What each received package holds: every peer's plan replayed (package dir → file → CID). Exact per peer — two
// peers sharing the same library and package name land in different dirs, and each dir is judged against its own
// peer's entries, never the other's.
static std::map<std::filesystem::path, std::map<std::filesystem::path, std::string>> ReceivedPackages(const nlohmann::ordered_json &Config)
{
    std::map<std::filesystem::path, std::map<std::filesystem::path, std::string>> Out;
    if (!Config.contains("PeerLibraries") || !Config["PeerLibraries"].is_object()) return Out;
    for (const auto &[Peer, Libs] : Config["PeerLibraries"].items())
        for (const ReceivedFetch &T : PlanReceivedFetches(Config, ReceivedNickLabel(Config, Peer), Libs))
        {
            const std::filesystem::path F(T.Dest);
            Out[F.parent_path()][F] = T.Cid;
        }
    return Out;
}

// The library NAME a received lib dir ("<nick> - <lib>") was planned under: the same shape the planner builds,
// matched against the snapshots we hold. "" when nothing matches (a dir of a forgotten peer).
static std::string ReceivedLibraryName(const nlohmann::ordered_json &Config, const std::string &LibDirName)
{
    if (!Config.contains("PeerLibraries") || !Config["PeerLibraries"].is_object()) return {};
    for (const auto &[P, Libs] : Config["PeerLibraries"].items())
    {
        const std::string Nick = ReceivedNickLabel(Config, P);
        if (!Libs.is_object()) continue;
        for (const auto &[L, Items] : Libs.items())
            if (NodeFiles::SafeName(Nick + " - " + L) == LibDirName) return L;
    }
    return {};
}

bool AdoptReceivedPackages(const nlohmann::ordered_json &Config, const std::vector<std::filesystem::path> &PkgDirs,
                           std::vector<std::filesystem::path> *NewDirs, std::string *Error)
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
    if (NewDirs) NewDirs->clear();
    const auto Fail = [&](const std::string &Why) { if (Error) *Error = Why; return false; };
    //Every package is judged before any moves: one refused refuses them all, with nothing half-installed.
    struct Move { fs::path Src, Dest; std::string Lib; };
    std::vector<Move> Moves;
    std::set<fs::path> SeenSrc, SeenDest;
    const fs::path Cat = fs::path(CatalogRootDir(Config)).lexically_normal();
    for (const fs::path &PkgDir : PkgDirs)
    {
        const fs::path Src = PkgDir.lexically_normal();
        if (!SeenSrc.insert(Src).second) continue;   // asked twice: installed once
        auto [CatEnd, SrcEnd] = std::mismatch(Cat.begin(), Cat.end(), Src.begin(), Src.end());
        if (Cat.empty() || CatEnd != Cat.end()) return Fail("not a received package dir: " + PkgDir.string());
        const std::string Lib = ReceivedLibraryName(Config, Src.parent_path().filename().string());
        if (Lib.empty()) return Fail("no share snapshot names " + Src.parent_path().filename().string());
        //The library NAME is the peer's (their snapshot): one sanitised segment, and the destination must lie inside
        //the library whatever it held — an absolute or ".." name would otherwise plant the package anywhere writable.
        const fs::path Dest = fs::path(LibraryRootDir(Config)) / NodeFiles::SafeName(Lib) / NodeFiles::SafeName(Src.filename().string());
        if (!NodeGraph::PathWithin(LibraryRootDir(Config), Dest)) return Fail("refused: " + Dest.string() + " is outside the library");
        if (!fs::is_directory(Src, Ec))
        {
            if (fs::is_directory(Dest, Ec)) { if (NewDirs) NewDirs->push_back(Dest); continue; }   // installed meanwhile
            return Fail("not a received package dir: " + PkgDir.string());
        }
        if (fs::exists(Dest, Ec)) return Fail("a package named '" + Src.filename().string() + "' already exists in library '" + Lib + "'");
        //Two received packages that would install as one (one package from two peers, of different generations): never
        //merged — refused, as the second always was when they were installed one by one.
        if (!SeenDest.insert(Dest.lexically_normal()).second)
            return Fail("two received packages would install as '" + Src.filename().string() + "' in library '" + Lib + "'");
        Moves.push_back({ Src, Dest, Lib });
    }
    if (Moves.empty()) return true;
    //Every file fetched into a package is referenced in place, and the references move to the installed place BEFORE
    //the packages do — all of them in ONE move: each move reads the whole reference index (a game's closure reaches
    //several packages, a scan each). The move is idempotent and never looks at the files, so a failure or crash
    //anywhere up to the renames leaves stubs still Received in CATALOG, which the next download adopts again. Moved
    //after the renames, a failure left an installed package (never adopted again) whose files read as missing.
    std::vector<std::pair<std::string, std::string>> Refs;
    for (const Move &M : Moves) Refs.push_back({ M.Src.string(), M.Dest.string() });
    if (std::string MErr; IpfsWrapper::MoveRefs(Refs, &MErr) < 0)
        return Fail("cannot re-point the references of the received package(s) to the library: " + MErr);
    //Then the queue lets go of every stub, before any moves: a file landing into one later would be referenced at a path
    //its package is about to leave. (Not before the move: refused, the stubs stay, and so must their fetches.)
    for (const Move &M : Moves) IpfsWrapper::ForgetDestsUnder(M.Src.string());
    std::vector<std::pair<std::string, std::string>> Back;   // packages that could not move: their references go back
    std::string Why;
    for (const Move &M : Moves)
    {
        fs::create_directories(M.Dest.parent_path(), Ec);
        fs::rename(M.Src, M.Dest, Ec);
        if (Ec)
        {   //A different filesystem: copy, then remove the source. A partial copy goes, or it blocks every later install —
            //it is all this call's: the destination did not exist (judged above, under AdoptMu), and no two packages of
            //one call share one.
            Ec.clear();
            fs::copy(M.Src, M.Dest, fs::copy_options::recursive, Ec);
            if (Ec)
            {
                if (Why.empty()) Why = "cannot move " + M.Src.string() + " to " + M.Dest.string() + ": " + Ec.message();
                std::error_code Rm;
                fs::remove_all(M.Dest, Rm);
                Back.push_back({ M.Dest.string(), M.Src.string() });
                continue;
            }
            fs::remove_all(M.Src, Ec);
        }
        // A later request for the old paths lands in the installed package; registered before the queue forgets them,
        // so no request in between re-materialises the stub.
        IpfsWrapper::RedirectDestsUnder(M.Src.string(), M.Dest.string());
        IpfsWrapper::ForgetDestsUnder(M.Src.string());
        if (fs::is_empty(M.Src.parent_path(), Ec)) fs::remove(M.Src.parent_path(), Ec);
        if (NewDirs) NewDirs->push_back(M.Dest);
        LogOut("PackageCatalog::AdoptReceivedPackages", "installed '" + M.Src.filename().string() + "' into library '" + M.Lib + "'");
    }
    if (Back.empty()) return true;
    if (std::string BErr; IpfsWrapper::MoveRefs(Back, &BErr) < 0)   // the packages stay where their files are
        LogErr("PackageCatalog::AdoptReceivedPackages", "cannot point the references back at the received package(s): " + BErr);
    return Fail(Why);
}

bool AdoptReceivedPackage(const nlohmann::ordered_json &Config, const std::filesystem::path &PkgDir, std::filesystem::path *NewDir, std::string *Error)
{
    std::vector<std::filesystem::path> New;
    if (!AdoptReceivedPackages(Config, { PkgDir }, &New, Error)) return false;
    if (NewDir && !New.empty()) *NewDir = New.front();
    return true;
}
bool ReceivedPackagesIncomplete(const nlohmann::ordered_json &Config)
{
    std::error_code Ec;
    for (const auto &[Dir, Files] : ReceivedPackages(Config))
        for (const auto &[F, C] : Files)
            if (!std::filesystem::exists(F, Ec) && !Refused(C)) return true;
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
    //Every package's missing files in one batch, one wait: a package after another, each waited for, waited on the
    //slowest file of each in turn; and every file was verified before and again after, fetched or not.
    const auto Packages = ReceivedPackages(Config);
    std::vector<IpfsWrapper::FetchTarget> Batch;
    std::set<fs::path> Fetched;                                         // what this pass asks for: verified once landed
    for (const auto &[Dir, Files] : Packages)
        for (const auto &[F, C] : Files)
        {
            //A file there holding another node (the entry's previous generation, under the same name) goes first: the
            //fetch takes a file that is there for done when the node it asks for is held here.
            if (Refused(C)) continue;                                  // refused once: not fetched again this run
            if (fs::exists(F, Ec))
            {
                std::string VErr;
                if (NodeGraph::VerifyLanded(F, C, nullptr, &VErr)) continue;   // in place, and what its entry says
                //No node is larger than 256 KiB: a file that is goes without being read into memory to name it.
                if (fs::file_size(F, Ec) > Cid::MaxNodeBytes) fs::remove(F, Ec);
                else
                {
                    std::ifstream In(F, std::ios::binary);
                    const std::string Old = Cid::OfBytes(std::string((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>()));
                    In.close();
                    RemoveLandedNode(F, Old);
                }
            }
            if (Fetched.insert(F).second)
            {
                IpfsWrapper::FetchTarget T{ C, F.string(), /*Optional=*/true, /*Dir=*/false, /*Verify=*/true };
                T.Small = true;
                Batch.push_back(T);
            }
        }
    std::string WErr;
    //Optional: a file that does not land fails its own package (below), never the wait for every other package's.
    //Given up only when nothing has landed for 10 minutes: one deadline for every package's files ended a large
    //receive (thousands of node files from a slow peer) part way while it was landing steadily.
    if (!Batch.empty() && !IpfsWrapper::WaitBatchWhileMoving(IpfsWrapper::EnqueueBatch(Batch), 10 * 60 * 1000, &WErr))
        LogWarn("PackageCatalog::LandReceivedPackages", WErr + ": what has not landed is fetched on the next pass");
    for (const auto &[Dir, Files] : Packages)
        for (const auto &[F, C] : Files)
        {
            if (!Fetched.count(F)) continue;                             // checked above: in place, as its entry says
            std::string VErr;
            if (!fs::exists(F, Ec))
            {
                const std::string Why = IpfsWrapper::FailureOf(C);
                VErr = "was not fetched" + (!Why.empty() ? " (" + Why + ")" : WErr.empty() ? std::string() : " (" + WErr + ")");
            }
            else if (NodeGraph::VerifyLanded(F, C, nullptr, &VErr)) continue;
            else { RemoveLandedNode(F, C); Refuse(C); }
            //Refused in the node (larger than a node may be): as final as a node that does not verify.
            if (const std::string Why = IpfsWrapper::FailureOf(C); Why.find("larger than it may be") != std::string::npos) Refuse(C, true);
            if (Error) *Error = "package " + Dir.filename().string() + ": node " + F.filename().string() + " " + VErr;
            LogWarn("PackageCatalog::LandReceivedPackages", "package '" + Dir.filename().string() + "': node " + F.filename().string()
                    + " (" + C + ") refused: " + VErr);
            Ok = false;
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
    std::vector<LandedNode> Stale;
    std::map<std::string, std::vector<fs::path>> Where;                    // cid → every path an entry gives it
    for (const auto &[Dir, Files] : Packages) for (const auto &[F, C] : Files) Where[C].push_back(F);
    for (const auto &[Dir, Files] : Packages)
    {
        if (!fs::is_directory(Dir, Ec)) continue;
        //What an entry names is kept, and never read: reading every received node file (thousands, on the GUI thread
        //after each landing) to skip them was the whole cost. Only the others are read, for their CIDs.
        std::set<fs::path> Named;
        for (const auto &[F, C] : Files) Named.insert(F);
        for (const auto &[Cid, F] : NodeFiles::In(Dir, nullptr, &Named))
        {
            std::vector<fs::path> Elsewhere = Where[Cid];                  // the same node's other landed copies
            if (const Node *N = Idx.Find(Cid); N && !N->File.empty()) Elsewhere.push_back(N->File);
            Stale.push_back({ F.Path, Cid, std::move(Elsewhere) });
        }
    }
    RemoveLandedNodes(Stale);                                              // every package's, one drop of their references
    for (const LandedNode &N : Stale) if (!fs::exists(N.File, Ec)) ++Removed;
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
