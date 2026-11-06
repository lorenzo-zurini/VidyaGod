// Versions of an installed package: a friend's newer version found, staged, planned and switched to (journaled).
#include "packagecatalog.h"
#include "noderefs.h"
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

} // namespace PackageCatalog
