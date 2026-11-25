// Versions of an installed package: a peer's newer version found, staged, planned and switched to (journaled).
#include <atomic>
#include "packagecatalog.h"
#include "noderefs.h"
#include "sessions.h"
#include "nodestore.h"
#include "contentfile.h"
#include "nodefiles.h"
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
#include <QStandardPaths>
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
using namespace Shelf;       // the library walks rows and grafts as the plan sees them
using namespace Installed;

// ---------------------------------------------------------------------------------------------------------------------
// Versions of an installed package. A package has one home; a peer may share another version of a package installed
// here (they re-minted it: a library, a new engine build). There is no "newer" — the user picks a peer's version, and
// the package switches to it: every node it lists comes in, and an installed node it does not list goes only when
// nothing installed still contains it (other games keep working on the version they contain).
// ---------------------------------------------------------------------------------------------------------------------
namespace {
//A package folder's node CIDs, kept with its node files' stamps (Platform::FileStamp): the scan for other versions
//read every installed package's node files at every refresh of the packages page; one whose files are as they were is
//not read again.
std::mutex g_NodeCidsMu;
std::map<std::string, std::pair<std::string, std::vector<std::string>>> g_NodeCids;   // dir → {stamps, cids}
std::atomic<long long> g_NodeCidReads{0};

std::vector<std::string> PackageNodeCids(const std::filesystem::path &Dir)
{
    std::string Stamps;
    std::error_code Ec;
    std::vector<std::filesystem::path> Files;
    for (const auto &E : std::filesystem::directory_iterator(Dir, Ec))
        if (E.path().extension() == ".json") Files.push_back(E.path());
    std::sort(Files.begin(), Files.end());
    for (const auto &F : Files) Stamps += F.filename().string() + '\n' + Platform::FileStamp(F) + '\n';
    {
        std::lock_guard<std::mutex> Lk(g_NodeCidsMu);
        if (const auto It = g_NodeCids.find(Dir.string()); It != g_NodeCids.end() && !Ec && It->second.first == Stamps)
            return It->second.second;
    }
    ++g_NodeCidReads;
    std::vector<std::string> Out;
    for (const auto &[C, F] : NodeFiles::In(Dir)) Out.push_back(C);   // sorted: a map's keys
    if (!Ec) { std::lock_guard<std::mutex> Lk(g_NodeCidsMu); g_NodeCids[Dir.string()] = { Stamps, Out }; }
    return Out;
}
//A node file that is not its canonical form (a handle, a canvas position, other bytes): a draft never saved. A switch
//cannot judge it, so it is refused.
bool HasDraftNodes(const std::filesystem::path &Dir)
{
    for (const auto &[C, F] : NodeFiles::In(Dir)) if (!F.Frozen) return true;
    return false;
}
//The file of node C in Dir, or none.
std::filesystem::path NodeFileIn(const std::filesystem::path &Dir, const std::string &C)
{
    const auto Fs = NodeFiles::In(Dir);
    const auto It = Fs.find(C);
    return It == Fs.end() ? std::filesystem::path() : It->second.Path;
}
//A version's name: the last 12 hex digits of the FNV-1a hash of its node CIDs, sorted — the same listing, the same name.
std::string VersionId(const std::map<std::string, std::string> &Listing)
{
    uint64_t H = 1469598103934665603ull;
    for (const auto &[C, F] : Listing) { for (unsigned char X : C) { H ^= X; H *= 1099511628211ull; } H ^= '\n'; H *= 1099511628211ull; }
    char Buf[17];
    std::snprintf(Buf, sizeof Buf, "%016llx", static_cast<unsigned long long>(H));
    return std::string(Buf + 4);
}
std::string Tail(const std::string &S, size_t N) { return S.size() > N ? S.substr(S.size() - N) : S; }
std::filesystem::path UpdateStage(const nlohmann::ordered_json &Config, const PackageUpdate &U)
{
    //Per peer and version: two peers' versions of one package never share a stage.
    return std::filesystem::path(LibraryRootDir(Config)).parent_path() / "UPDATING" / NodeFiles::SafeName(U.Lib) / NodeFiles::SafeName(U.Pkg)
           / NodeFiles::SafeName(Tail(U.Peer, 8) + "-" + U.Version);
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
    for (const std::string &C : Listing) for (const auto &R : NodeRefs::Contained(NodeAt(C))) Q.push_back(R);
    while (!Q.empty())
    {
        const std::string C = Q.front(); Q.pop_front();
        if (!Candidates.count(C) || !Kept.insert(C).second) continue;
        for (const auto &R : NodeRefs::Contained(NodeAt(C))) Q.push_back(R);
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
    const std::map<std::string, NodeFiles::File> Here = NodeFiles::In(Dir);
    auto FileOf = [&](const std::string &C) {
        const auto It = Here.find(C);
        return It != Here.end() ? It->second.Path : S.Stage / (C + ".json");   // a staged node is named by its CID
    };
    std::vector<std::string> Home;
    for (const auto &[C, F] : Here) Home.push_back(C);
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
    for (const std::string &C : S.Kept) AfterFiles.push_back(FileOf(C));
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
} // namespace

std::vector<PackageUpdate> AvailableUpdates(const nlohmann::ordered_json &Config)
{
    namespace fs = std::filesystem;
    std::vector<PackageUpdate> Out;
    if (!Config.contains("PeerLibraries") || !Config["PeerLibraries"].is_object()) return Out;
    const fs::path LibRoot = LibraryRootDir(Config);
    std::shared_ptr<const NodeIndex> Idx;   // the library's index: read only when a version's nodes are all here
    std::error_code Ec;
    for (const auto &[Peer, Libs] : Config["PeerLibraries"].items())
    {
        const std::string Nick = ReceivedNickLabel(Config, Peer);
        std::map<std::pair<std::string, std::string>, std::map<std::string, std::string>> Versions;   // (lib, pkg) → cid → file
        for (const SheetEntry &E : WireEntries(Libs)) Versions[{ E.Lib, E.Pkg }][E.Cid] = E.File;
        const auto Folders = ReceivedFolderNames(Libs);                 // where each would have landed, and been installed
        for (const auto &[LP, Listing] : Versions)
        {
            const fs::path Dir = LibRoot / NodeFiles::SafeName(LP.first) / Folders.at(LP);
            if (!fs::is_directory(Dir, Ec)) continue;
            const std::vector<std::string> Home = PackageNodeCids(Dir);
            const std::set<std::string> Here(Home.begin(), Home.end());
            std::set<std::string> Theirs;
            for (const auto &[C, F] : Listing) Theirs.insert(C);
            if (Theirs == Here) continue;                                // this version: no index read (the test below says the same)
            //A version whose every node is here already (switched to before) is this package plus nodes kept for other
            //games: offered again only once one of those may go.
            if (std::includes(Here.begin(), Here.end(), Theirs.begin(), Theirs.end()))
            {
                std::set<std::string> Beyond;
                std::set_difference(Here.begin(), Here.end(), Theirs.begin(), Theirs.end(), std::inserter(Beyond, Beyond.end()));
                if (!Idx) Idx = CurrentIndex(Config);
                const std::vector<std::string> L(Theirs.begin(), Theirs.end());
                const auto Files = NodeFiles::In(Dir);
                if (KeptNodes(*Idx, Dir, L, Beyond, [&](const std::string &C) {
                        const auto It = Files.find(C);
                        return It == Files.end() ? nlohmann::ordered_json() : It->second.Node; }).size() == Beyond.size())
                    continue;
            }
            Out.push_back(PackageUpdate{ Dir, LP.first, Folders.at(LP), Peer, Nick, VersionId(Listing), Listing });
        }
    }
    return Out;
}

bool TrashPackage(const NodeIndex &Idx, const std::filesystem::path &Dir, std::string *Error)
{
    namespace fs = std::filesystem;
    std::lock_guard<std::mutex> Lk(PackageLock(Dir));
    std::error_code Ec;
    const fs::path Own = fs::weakly_canonical(Dir, Ec);
    std::set<std::string> Cids;
    for (const std::string &C : PackageNodeCids(Dir)) Cids.insert(C);
    for (const auto &[K, N] : Idx.Nodes)
    {
        if (!N.IsVariant() || fs::weakly_canonical(N.BundleDir, Ec) != Own) continue;
        ForEachContentLayer(Idx, K, {}, [&](const nlohmann::ordered_json&, const fs::path &Local, const std::string &Cid) {
            std::error_code E2;
            if (!Cid.empty() && PathUnder(Own, fs::weakly_canonical(Local, E2))) Cids.insert(Cid);
        }, true, true);
    }
    //Qt makes the trash with a plain mkdir: the data folder it lives in must be there (a fresh account has none yet).
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
    if (!QFile::moveToTrash(QString::fromStdString(Dir.string())))
    {
        if (Error) *Error = "it could not be moved to the trash (" + Dir.string() + "), so nothing was deleted";
        return false;
    }
    for (const std::string &C : Cids) IpfsWrapper::Unpin(C);
    IpfsWrapper::DropRefs({ Cids.begin(), Cids.end() });
    LogOut("PackageCatalog::TrashPackage", "Moved " + Dir.string() + " to the trash (" + std::to_string(Cids.size()) + " CIDs let go)");
    return true;
}

std::string PeerSharingPackage(const nlohmann::ordered_json &Config, const std::filesystem::path &Dir)
{
    namespace fs = std::filesystem;
    if (!Config.contains("PeerLibraries") || !Config["PeerLibraries"].is_object()) return {};
    const std::vector<std::string> Home = PackageNodeCids(Dir);
    if (Home.empty()) return {};
    const fs::path LibRoot = LibraryRootDir(Config), Want = Dir.lexically_normal();
    for (const auto &[Peer, Libs] : Config["PeerLibraries"].items())
    {
        std::map<std::pair<std::string, std::string>, std::set<std::string>> Listed;   // (lib, pkg) → its node CIDs
        for (const SheetEntry &E : WireEntries(Libs)) Listed[{ E.Lib, E.Pkg }].insert(E.Cid);
        const auto Folders = ReceivedFolderNames(Libs);
        for (const auto &[LP, Cids] : Listed)
        {
            const auto F = Folders.find(LP);
            if (F == Folders.end() || (LibRoot / NodeFiles::SafeName(LP.first) / F->second).lexically_normal() != Want) continue;
            if (std::all_of(Home.begin(), Home.end(), [&](const std::string &C) { return Cids.count(C) != 0; }))
                return ReceivedNickLabel(Config, Peer);
        }
    }
    return {};
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
        for (const auto &[C, F] : U.Listing) RemoveLandedNode(S.Stage / (C + ".json"), C);   // nobody here wants them
        std::error_code Rc;
        fs::remove_all(S.Stage, Rc);
        if (Error) *Error = Why;
        return false;
    };
    if (U.Listing.empty()) return Fail(U.Nick + "'s version of " + U.Pkg + " lists no nodes");
    //The version's node files, named by their CIDs in the stage: they take their names in the package when it switches.
    std::vector<IpfsWrapper::FetchTarget> Batch;
    for (const auto &[C, F] : U.Listing)
        Batch.push_back(IpfsWrapper::FetchTarget{ C, (S.Stage / (C + ".json")).string(), /*Optional=*/false, /*Dir=*/false, /*Verify=*/true });
    fs::create_directories(S.Stage, Ec);
    if (!IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch(Batch), 10 * 60 * 1000, &WErr))
        return Fail("fetching " + U.Nick + "'s version of " + U.Pkg + ": " + WErr);
    for (const auto &[C, F] : U.Listing)
    {
        std::string VErr;
        if (!NodeGraph::VerifyLanded(S.Stage / (C + ".json"), C, nullptr, &VErr))
            return Fail(U.Nick + "'s version of " + U.Pkg + ": node " + F + " refused: " + VErr);
        S.Listing.push_back(C);
    }
    PlanSwitch(*CurrentIndex(Config), S);
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
    {
        const auto Here = NodeFiles::In(Dir);
        for (const std::string &C : S.Listing)
            if (!fs::exists(S.Stage / (C + ".json"), Ec) && !Here.count(C))
                return Refuse("the staged version of " + Pkg + " is incomplete (node " + C + "): fetch it again");
    }
    if (!Resuming)
    {
        //Planned again, under the lock: the library may have changed while the user read the plan. It may delete
        //only what the user was shown.
        PlanSwitch(*CurrentIndex(Config), S);
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
    //A switch refused part way keeps what it already moved: the references of those files follow before it says no
    //(a node file moved in is skipped by the next attempt, so its reference would never follow).
    auto RefuseFollowing = [&](const std::string &Why, std::vector<std::pair<std::string, std::string>> Moved)
    {
        std::string MErr;
        if (!Moved.empty() && IpfsWrapper::MoveRefs(Moved, &MErr) < 0)
            LogWarn("PackageCatalog::ApplyStagedUpdate", "moved " + std::to_string(Moved.size())
                    + " file(s) of " + Pkg + ", but their references did not follow: " + MErr);
        return Refuse(Why);
    };
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
        if (Rc) return RefuseFollowing("cannot rename " + From.string() + ": " + Rc.message() + " (switch again to finish)",
                                       std::move(Followed));
        Followed.push_back({ From.string(), To.string() });
    }
    //Every reference move of the switch is one scan of every reference in the store: the content's moves join the
    //node files' below, in one, unless content is deleted in between (a delete reads where a reference points).
    std::vector<std::pair<std::string, std::string>> Later;
    if (!Followed.empty() && !S.Deletes.empty())
    {
        std::string MErr;
        if (IpfsWrapper::MoveRefs(Followed, &MErr) < 0)
            LogWarn("PackageCatalog::ApplyStagedUpdate", "renamed " + std::to_string(Followed.size())
                    + " file(s) but their references did not follow: " + MErr);
    }
    else Later = std::move(Followed);
    //2. Content no node of the package names any more goes (its reference with it, when it was the reference — then
    //   the same bytes kept under another name are referenced there).
    for (const auto &D : S.Deletes)
    {
        if (!fs::exists(D.File, Ec)) continue;
        RemoveLandedNode(D.File, D.Cid, D.Elsewhere);
        if (fs::exists(D.File, Ec)) LogWarn("PackageCatalog::ApplyStagedUpdate", "could not delete " + D.File.string());
    }
    //3. The version's node files move in (each referenced where it now lives, so the package serves and re-shares it),
    //4. the nodes nothing contains any more go, and 5. every node file takes its name among the package's new nodes.
    {
        const auto Here = NodeFiles::In(Dir);
        std::vector<std::pair<std::string, std::string>> Followed;
        for (const std::string &C : S.Listing)
        {
            if (Here.count(C)) continue;
            const fs::path From = S.Stage / (C + ".json"), To = Dir / (C + ".json");
            std::error_code Rc;
            fs::rename(From, To, Rc);
            if (Rc) { Rc.clear(); fs::copy_file(From, To, Rc); }
            if (Rc)
            {
                Later.insert(Later.end(), Followed.begin(), Followed.end());
                return RefuseFollowing("cannot move " + From.string() + " into " + Dir.string() + ": " + Rc.message()
                                       + " (switch again to finish)", std::move(Later));
            }
            Followed.push_back({ From.string(), To.string() });
        }
        for (const std::string &C : S.Removed)
            if (const auto It = Here.find(C); It != Here.end())
            {
                RemoveLandedNode(It->second.Path, C);
                if (fs::exists(It->second.Path, Ec))
                    LogWarn("PackageCatalog::ApplyStagedUpdate", "could not remove node " + It->second.Path.string());
            }
        std::vector<std::pair<fs::path, fs::path>> Named;
        std::string NErr;
        if (!NodeFiles::Normalize(Dir, &Named, &NErr)) LogWarn("PackageCatalog::ApplyStagedUpdate", Pkg + ": " + NErr);
        //A node file moved in and then named is one move, from the stage to its name; the rest as they are. One call:
        //a move each — content, node files, names — was a scan of every reference each.
        std::map<std::string, std::string> Renamed;
        for (const auto &[F, T] : Named) Renamed[F.string()] = T.string();
        std::vector<std::pair<std::string, std::string>> Refs = std::move(Later);
        for (auto &[From, To] : Followed)
            if (const auto It = Renamed.find(To); It != Renamed.end()) { To = It->second; Renamed.erase(It); }
        Refs.insert(Refs.end(), Followed.begin(), Followed.end());
        for (const auto &[F, T] : Renamed) Refs.push_back({ F, T });
        std::string MErr;
        if (!Refs.empty() && IpfsWrapper::MoveRefs(Refs, &MErr) < 0)
            LogWarn("PackageCatalog::ApplyStagedUpdate", "moved " + Pkg + "'s files, but their references did not follow: " + MErr);
    }
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
    for (const auto &[C, F] : S.U.Listing) RemoveLandedNode(S.Stage / (C + ".json"), C);   // nobody here wants them
    std::error_code Ec;
    std::filesystem::remove_all(S.Stage, Ec);
}

std::string PackagePin(const nlohmann::ordered_json &Config, const std::filesystem::path &PkgDir)
{
    //Keyed as publishing keys it: "<library>/<package>", and a package directly under the library root is in "Library".
    std::error_code Ec;
    const bool AtRoot = std::filesystem::equivalent(PkgDir.parent_path(), LibraryRootDir(Config), Ec) && !Ec;
    const std::string Key = (AtRoot ? std::string("Library") : PkgDir.parent_path().filename().string()) + "/" + PkgDir.filename().string();
    if (!Config.contains("Pins") || !Config["Pins"].is_object() || !Config["Pins"].contains(Key) || !Config["Pins"][Key].is_string())
        return {};
    return Config["Pins"][Key].get<std::string>();
}

long long DebugNodeCidReads() { return g_NodeCidReads.load(); }

} // namespace PackageCatalog
