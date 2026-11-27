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
    return UpdatesRoot(Config) / NodeFiles::SafeName(U.Lib) / NodeFiles::SafeName(U.Pkg) / NodeFiles::SafeName(Tail(U.Peer, 8) + "-" + U.Version);
}
//The stages a switch of this run is using (staged, not yet applied or discarded): the start's sweep leaves them be.
std::mutex g_StagesMu;
std::set<std::string> g_Stages;
void HoldStage(const std::filesystem::path &S) { std::lock_guard<std::mutex> Lk(g_StagesMu); g_Stages.insert(S.lexically_normal().string()); }
void LetStage(const std::filesystem::path &S) { std::lock_guard<std::mutex> Lk(g_StagesMu); g_Stages.erase(S.lexically_normal().string()); }
bool StageHeld(const std::filesystem::path &S) { std::lock_guard<std::mutex> Lk(g_StagesMu); return g_Stages.count(S.lexically_normal().string()) != 0; }
//A folder, then each folder above it that it leaves empty — up to Root, Root itself included, never above it.
void PruneEmpty(std::filesystem::path P, const std::filesystem::path &Root)
{
    const std::filesystem::path R = Root.lexically_normal();
    for (P = P.lexically_normal(); !P.empty() && NodeGraph::PathWithin(R, P); P = P.parent_path())
    {
        std::error_code Ec;
        if (!std::filesystem::is_directory(P, Ec) || !std::filesystem::is_empty(P, Ec) || Ec || !std::filesystem::remove(P, Ec)) return;
        if (P == R) return;
    }
}
//A stage goes: its fetches land nowhere any more (and no download row names a file in it), its node files with their
//references when the reference was to them (Keep: where a node's own copy stays, referenced there instead), the
//folder, and the folders above it it leaves empty.
void DropStage(const std::filesystem::path &Stage, const std::filesystem::path &Root,
               const std::map<std::string, std::filesystem::path> &Keep = {})
{
    namespace fs = std::filesystem;
    IpfsWrapper::ForgetDestsUnder(Stage.string());
    std::vector<LandedNode> Landed;
    std::error_code Ec;
    for (const auto &E : fs::directory_iterator(Stage, Ec))
    {
        const fs::path &P = E.path();
        if (P.extension() != ".json" || P.filename() == "switch.json") continue;
        const std::string C = P.stem().string();                                  // a staged node is named by its CID
        LandedNode N{ P, C, {} };
        if (const auto It = Keep.find(C); It != Keep.end()) N.Elsewhere.push_back(It->second);
        Landed.push_back(std::move(N));
    }
    RemoveLandedNodes(Landed);
    fs::remove_all(Stage, Ec);
    LetStage(Stage);
    PruneEmpty(Stage.parent_path(), Root);
}
nlohmann::ordered_json NodeFileJson(const std::filesystem::path &P)
{
    nlohmann::ordered_json J;
    return NodeGraph::ReadTreeJsonBounded(P, J) ? J : nlohmann::ordered_json(nlohmann::ordered_json::value_t::discarded);
}
} // namespace

//The installed nodes of Dir, among Candidates, that something still contains once Dir lists Listing: every other
//installed package, and the listed version itself (a version that contains the one before it). Hard links hold:
//containment and INTO (a mod installed for an end keeps that end); a condition's CIDs compare, never hold. NodeAt reads a node of the package or of its stage.
std::vector<std::string> KeptNodes(const NodeIndex &Idx, const std::filesystem::path &Dir, const std::vector<std::string> &Listing,
                                   const std::set<std::string> &Candidates,
                                   const std::function<nlohmann::ordered_json(const std::string &)> &NodeAt)
{
    std::set<std::string> Kept;
    std::deque<std::string> Q;
    for (const auto &[Id, N] : Idx.Nodes)
        if (!N.Received && N.BundleDir.lexically_normal() != Dir.lexically_normal())
        {
            for (const auto &R : N.Refs) Q.push_back(R);
            for (const auto &R : N.Into) Q.push_back(R);
        }
    for (const std::string &C : Listing) for (const auto &R : NodeRefs::Linked(NodeAt(C))) Q.push_back(R);
    while (!Q.empty())
    {
        const std::string C = Q.front(); Q.pop_front();
        if (!Candidates.count(C) || !Kept.insert(C).second) continue;
        for (const auto &R : NodeRefs::Linked(NodeAt(C))) Q.push_back(R);
    }
    return { Kept.begin(), Kept.end() };
}

namespace {
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
    //Which switch it is, too: the next start finishes it without being asked again (SweepUpdateStages).
    nlohmann::ordered_json Listed = nlohmann::ordered_json::object();
    for (const auto &[C, F] : S.U.Listing) Listed[C] = F;
    nlohmann::ordered_json J = { {"update", { {"dir", S.U.Dir.string()}, {"lib", S.U.Lib}, {"pkg", S.U.Pkg}, {"peer", S.U.Peer},
                                              {"nick", S.U.Nick}, {"version", S.U.Version}, {"listing", Listed} }},
                                 {"listing", S.Listing}, {"added", S.Added}, {"removed", S.Removed}, {"kept", S.Kept},
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
    if (S.U.Dir.empty() && J.contains("update") && J["update"].is_object())
    {   // a switch found by the start's sweep: which package, from whom
        const auto &U = J["update"];
        S.U.Dir = U.value("dir", std::string()); S.U.Lib = U.value("lib", std::string()); S.U.Pkg = U.value("pkg", std::string());
        S.U.Peer = U.value("peer", std::string()); S.U.Nick = U.value("nick", std::string()); S.U.Version = U.value("version", std::string());
        if (U.contains("listing") && U["listing"].is_object())
            for (const auto &[C, F] : U["listing"].items()) if (F.is_string()) S.U.Listing[C] = F.get<std::string>();
    }
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

std::mutex &PackageLock(const std::filesystem::path &Dir)
{
    //Keyed by the folder itself (links resolved): a trash, a switch and a staging of one package named two ways — through
    //a linked library root, a link in the library — still exclude each other.
    std::error_code Ec;
    const std::string Key = std::filesystem::weakly_canonical(Dir, Ec).string();
    static std::mutex Mu;
    static std::map<std::string, std::unique_ptr<std::mutex>> Locks;
    std::lock_guard<std::mutex> L(Mu);
    auto &P = Locks[Ec ? Dir.lexically_normal().string() : Key];
    if (!P) P = std::make_unique<std::mutex>();
    return *P;
}

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

bool TrashPackage(const NodeIndex &Idx, const std::filesystem::path &Dir, std::string *Error, const std::string &PinFolder)
{
    namespace fs = std::filesystem;
    std::error_code Ec;
    const fs::path Own = fs::weakly_canonical(Dir, Ec);   // the folder, not a link naming it: what the CIDs below are of
    std::lock_guard<std::mutex> Lk(PackageLock(Own));
    if (const std::string Why = UninstallBlocked(Own); !Why.empty()) { if (Error) *Error = Why; return false; }
    std::map<std::string, fs::path> Canon;
    const auto CanonDir = [&](const fs::path &P) -> const fs::path & {
        auto It = Canon.find(P.string());
        if (It == Canon.end()) { std::error_code E2; It = Canon.emplace(P.string(), fs::weakly_canonical(P, E2)).first; }
        return It->second;
    };
    //Its own CIDs, and those another package names too (a node of the same bytes, a file of the same content kept in its
    //own folder): dropping one of those would stop the other package serving, and reading as held whole.
    std::set<std::string> Cids, Others;
    for (const std::string &C : PackageNodeCids(Own)) Cids.insert(C);
    for (const auto &[K, N] : Idx.Nodes)
    {
        if (N.BundleDir.empty() || N.Received) continue;
        const bool Mine = CanonDir(N.BundleDir) == Own;
        if (!Mine) Others.insert(K);
        if (!N.IsVariant()) continue;
        ForEachContentLayer(Idx, K, {}, [&](const nlohmann::ordered_json&, const fs::path &Local, const std::string &Cid) {
            if (Cid.empty()) return;
            if (!PathUnder(Own, CanonDir(Local.parent_path()) / Local.filename())) Others.insert(Cid);
            else if (Mine) Cids.insert(Cid);
        }, true, true);
    }
    for (const std::string &C : Others) Cids.erase(C);
    //Qt makes the trash with a plain mkdir: the data folder it lives in must be there (a fresh account has none yet).
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
    if (!QFile::moveToTrash(QString::fromStdString(Own.string())))
    {
        if (Error) *Error = "it could not be moved to the trash (" + Own.string() + "), so nothing was deleted";
        return false;
    }
    for (const std::string &C : Cids) IpfsWrapper::Unpin(C);
    IpfsWrapper::DropRefs({ Cids.begin(), Cids.end() });
    if (!PinFolder.empty()) IpfsWrapper::Unpin(PinFolder);   // unpinned only: its blocks are others' too (a drop takes a closure)
    LogOut("PackageCatalog::TrashPackage", "Moved " + Own.string() + " to the trash (" + std::to_string(Cids.size()) + " CIDs let go)");
    return true;
}

std::string PeerListingPackage(const nlohmann::ordered_json &Config, const std::filesystem::path &Dir)
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

std::filesystem::path UpdatesRoot(const nlohmann::ordered_json &Config)
{
    return std::filesystem::path(LibraryRootDir(Config)).parent_path() / "UPDATING";
}

bool StagedPackageOf(const nlohmann::ordered_json &Config, const std::filesystem::path &P, std::string *Lib, std::string *Folder)
{
    const std::filesystem::path Rel = P.lexically_normal().lexically_relative(UpdatesRoot(Config).lexically_normal());
    auto It = Rel.begin();
    if (Rel.empty() || It == Rel.end() || *It == ".." || *It == ".") return false;
    const std::string L = (It++)->string();
    if (It == Rel.end()) return false;
    if (Lib) *Lib = L;
    if (Folder) *Folder = It->string();
    return true;
}

bool StageUpdate(const nlohmann::ordered_json &Config, const PackageUpdate &U, StagedUpdate *Out, std::string *Error,
                 const SwitchProgress &Say, const std::atomic<bool> *Cancel)
{
    namespace fs = std::filesystem;
    std::lock_guard<std::mutex> Lk(PackageLock(U.Dir));
    StagedUpdate S;
    S.U = U;
    S.Stage = UpdateStage(Config, U);
    S.StageRoot = UpdatesRoot(Config);
    std::error_code Ec;
    if (HasDraftNodes(U.Dir))
    { if (Error) *Error = U.Pkg + " holds node files that were never saved under their CID (drafts): open it in the package editor and save first"; return false; }
    HoldStage(S.Stage);
    if (fs::exists(S.Stage / kSwitchJournal, Ec) && LoadJournal(S))
    {   // a switch to this very version stopped half-way: finish it as it was planned
        if (Out) *Out = std::move(S);
        return true;
    }
    IpfsWrapper::ForgetDestsUnder(S.Stage.string());
    fs::remove_all(S.Stage, Ec);
    const IpfsWrapper::ForegroundHold Hold;
    std::string WErr;
    auto Fail = [&](const std::string &Why) {
        DropStage(S.Stage, S.StageRoot);                             // a fetch that lands later lands nowhere; nobody wants them
        if (Error) *Error = Why;
        return false;
    };
    if (U.Listing.empty()) return Fail(U.Nick + "'s version of " + U.Pkg + " lists no nodes");
    //The version's node files, named by their CIDs in the stage: they take their names in the package when it switches.
    std::vector<IpfsWrapper::FetchTarget> Batch;
    for (const auto &[C, F] : U.Listing)
        Batch.push_back(IpfsWrapper::FetchTarget{ C, (S.Stage / (C + ".json")).string(), /*Optional=*/false, /*Dir=*/false, /*Verify=*/true });
    fs::create_directories(S.Stage, Ec);
    const std::string What = U.Nick + "'s version of " + U.Pkg;
    if (Say) Say(0, "Getting " + What + " (" + std::to_string(Batch.size()) + " node file" + (Batch.size() == 1 ? "" : "s") + ")");
    {
        const IpfsWrapper::BatchHandle H = IpfsWrapper::EnqueueBatch(Batch);
        //A wait in short steps: a cancel is seen within one, and each says how many of the version's files are in.
        const auto Until = std::chrono::steady_clock::now() + std::chrono::minutes(10);
        for (size_t Was = static_cast<size_t>(-1);;)
        {
            if (IpfsWrapper::WaitBatch(H, 250, &WErr)) break;
            for (const auto &[C, F] : U.Listing)                      // a step's end is not a failure; a failed fetch is
                if (const std::string Why = IpfsWrapper::FailureOf(C); !Why.empty())
                    return Fail("fetching " + What + ": node " + F + ": " + Why);
            if (Cancel && Cancel->load()) return Fail("cancelled");
            if (std::chrono::steady_clock::now() > Until) return Fail("fetching " + What + ": timed out after 10 minutes");
            size_t In = 0;
            for (const auto &[C, F] : U.Listing) In += IpfsWrapper::JobSettled(C) ? 1 : 0;
            if (Say && In != Was) Say(double(In) / double(Batch.size()), "Getting " + What + ": " + std::to_string(In) + " of "
                                      + std::to_string(Batch.size()) + " node files");
            Was = In;
        }
    }
    if (Cancel && Cancel->load()) return Fail("cancelled");
    for (const auto &[C, F] : U.Listing)
    {
        std::string VErr;
        if (!NodeGraph::VerifyLanded(S.Stage / (C + ".json"), C, nullptr, &VErr))
            return Fail(What + ": node " + F + " refused: " + VErr);
        S.Listing.push_back(C);
    }
    if (Say) Say(1, "Comparing " + What + " with the installed one");
    PlanSwitch(*CurrentIndex(Config), S);
    if (Out) *Out = std::move(S);
    return true;
}

bool ApplyStagedUpdate(const nlohmann::ordered_json &Config, const StagedUpdate &Confirmed, std::string *Error,
                       const SwitchProgress &Say)
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
    if (S.StageRoot.empty()) S.StageRoot = UpdatesRoot(Config);
    const bool Resuming = LoadJournal(S);
    const std::string What = "Switching " + Pkg + " to " + S.U.Nick + "'s version";
    if (Say) Say(-1, What + (Resuming ? ": finishing the switch that stopped" : ""));
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
    if (Say && !S.Moves.empty()) Say(0.2, What + ": renaming " + std::to_string(S.Moves.size()) + " file(s)");
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
    if (Say && !S.Deletes.empty()) Say(0.4, What + ": removing " + std::to_string(S.Deletes.size()) + " file(s) it no longer has");
    //2. Content no node of the package names any more goes (its reference with it, when it was the reference — then
    //   the same bytes kept under another name are referenced there).
    for (const auto &D : S.Deletes)
    {
        if (!fs::exists(D.File, Ec)) continue;
        RemoveLandedNode(D.File, D.Cid, D.Elsewhere);
        if (fs::exists(D.File, Ec)) LogWarn("PackageCatalog::ApplyStagedUpdate", "could not delete " + D.File.string());
    }
    if (Say) Say(0.6, What + ": moving its node files in");
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
        if (Say) Say(0.8, What + ": updating what the node seeds");
        if (!Refs.empty() && IpfsWrapper::MoveRefs(Refs, &MErr) < 0)
            LogWarn("PackageCatalog::ApplyStagedUpdate", "moved " + Pkg + "'s files, but their references did not follow: " + MErr);
    }
    //What is left in the stage are copies of nodes the package already had: they go (a reference to a copy goes back to
    //the package's own file), and so does every folder the stage leaves empty — UPDATING with the last.
    {
        std::map<std::string, fs::path> Own;
        for (const auto &[C, F] : NodeFiles::In(Dir)) Own[C] = F.Path;
        DropStage(S.Stage, S.StageRoot, Own);
    }
    LogOut("PackageCatalog::ApplyStagedUpdate", "switched '" + Pkg + "' to " + S.U.Nick + "'s version: "
           + std::to_string(S.Added.size()) + " node(s) in, " + std::to_string(S.Removed.size()) + " out, "
           + std::to_string(S.Kept.size()) + " kept for what still contains them");
    return true;
}

bool DiscardStagedUpdate(const StagedUpdate &S)
{
    std::lock_guard<std::mutex> Lk(PackageLock(S.U.Dir));
    std::error_code Ec;
    if (std::filesystem::exists(S.Stage / kSwitchJournal, Ec))
    {
        LogWarn("PackageCatalog::DiscardStagedUpdate", "kept " + S.Stage.string() + ": the switch of " + S.U.Pkg
                + " has begun; the next start finishes it");
        LetStage(S.Stage);                                             // this run holds it no more: a sweep may finish it
        return false;
    }
    DropStage(S.Stage, S.StageRoot.empty() ? S.Stage.parent_path().parent_path().parent_path() : S.StageRoot);
    return true;
}

std::vector<std::string> SweepUpdateStages(const nlohmann::ordered_json &Config, std::vector<std::string> *Errors)
{
    namespace fs = std::filesystem;
    std::vector<std::string> Done;
    const fs::path Root = UpdatesRoot(Config);
    std::error_code Ec;
    if (!fs::is_directory(Root, Ec)) return Done;
    //UPDATING/<library>/<package>/<stage>: nothing else is ever made there.
    std::vector<fs::path> Stages, Folders;
    for (const auto &L : fs::directory_iterator(Root, Ec))
    {
        if (!L.is_directory(Ec)) continue;
        Folders.push_back(L.path());
        for (const auto &P : fs::directory_iterator(L.path(), Ec))
        {
            if (!P.is_directory(Ec)) continue;
            Folders.push_back(P.path());
            for (const auto &St : fs::directory_iterator(P.path(), Ec)) if (St.is_directory(Ec)) Stages.push_back(St.path());
        }
    }
    for (const fs::path &St : Stages)
    {
        if (StageHeld(St)) continue;                                   // a switch of this run is using it
        StagedUpdate S;
        S.Stage = St;
        S.StageRoot = Root;
        if (fs::exists(St / kSwitchJournal, Ec) && LoadJournal(S) && !S.U.Dir.empty())
        {
            std::string Err;
            if (ApplyStagedUpdate(Config, S, &Err)) Done.push_back(S.U.Pkg);
            else if (Errors) Errors->push_back(S.U.Pkg + ": " + Err);
            continue;
        }
        DropStage(St, Root);                                           // a fetch nothing will apply
    }
    //Folders an older stage left empty (each emptied stage prunes its own; these had none left in them).
    for (auto It = Folders.rbegin(); It != Folders.rend(); ++It) PruneEmpty(*It, Root);
    PruneEmpty(Root, Root);
    return Done;
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
