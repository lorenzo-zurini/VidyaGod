// The library as shared: which library a package is in, what a friend receives, and publishing the whole library.
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

namespace {

using PkgKey = std::pair<std::string, std::string>;   // (library, package dir)

//A folder's package under the library root: (its collection, its bundle dir); a folder directly under the root is in
//"Library". Nothing for the root itself or a folder outside it.
std::optional<PkgKey> PackageKeyOf(const std::filesystem::path &Dir, const std::filesystem::path &Root)
{
    std::error_code Ec;
    const std::filesystem::path Rel = std::filesystem::relative(Dir, Root, Ec);
    std::vector<std::string> Seg;
    for (const auto &Part : Rel) if (Part != "." && !Part.empty()) Seg.push_back(Part.string());
    if (Ec || Seg.empty() || Seg[0] == "..") return std::nullopt;
    return Seg.size() >= 2 ? PkgKey{Seg[0], Seg[1]} : PkgKey{"Library", Seg[0]};
}

//The share row of a package in Libraries[Lib] (by its name), or nullptr.
const nlohmann::ordered_json *RowOf(const nlohmann::ordered_json &Config, const std::string &Lib, const std::string &Pkg)
{
    if (!Config.contains("Libraries") || !Config["Libraries"].is_object() || !Config["Libraries"].contains(Lib)
        || !Config["Libraries"][Lib].is_array()) return nullptr;
    for (const auto &R : Config["Libraries"][Lib]) if (R.is_object() && R.value("pkg", std::string()) == Pkg) return &R;
    return nullptr;
}

//Puts Row in Libraries[Lib] in place of the row with its package name, or after the others.
void MergeRow(nlohmann::ordered_json &Libraries, const std::string &Lib, nlohmann::ordered_json Row)
{
    if (!Libraries.contains(Lib) || !Libraries[Lib].is_array()) Libraries[Lib] = nlohmann::ordered_json::array();
    for (auto &R : Libraries[Lib])
        if (R.is_object() && R.value("pkg", std::string()) == Row.value("pkg", std::string())) { R = std::move(Row); return; }
    Libraries[Lib].push_back(std::move(Row));
}

//The publish itself. Only == nullptr: every package, and Libraries is replaced whole (a deleted package leaves it).
//Otherwise those packages AND every package they contain, transitively (a node freezes only once what it names has):
//their nodes alone are minted; each named package is rebuilt (folder, pin, content check, needs), a contained one only
//when its record is stale (its folder CID moved, or it has no record or pin), and their rows are merged into Libraries
//in place — every other package's row is left as it was. A node that is a copy of one in a package outside the
//selection is not seen as a copy here (only the whole publish sees every package).
std::vector<std::string> Publish(nlohmann::ordered_json &Config, const std::vector<std::filesystem::path> *Only,
                                 std::string *Error, std::string *Gaps)
{
    const std::filesystem::path Root = LibraryRootDir(Config);
    std::map<std::string, nlohmann::ordered_json> Tree;
    std::map<std::string, std::filesystem::path>  Dirs;
    NodeGraph::GatherWorkingTree(Root, Tree, Dirs, /*SkipReserved=*/true);   // never mint a received friend stub
    if (Tree.empty()) { if (Error) *Error = "no packages to publish under " + Root.string(); return {}; }

    std::map<std::string, PkgKey> KeyOf;                                     // node handle → its package
    for (const auto &[Handle, D] : Dirs)
        if (const auto K = PackageKeyOf(D, Root)) KeyOf.emplace(Handle, *K);
    std::set<PkgKey> Targets, Sel;
    std::map<std::string, nlohmann::ordered_json> Sub;
    if (Only)
    {
        for (const auto &D : *Only)
            if (const auto K = PackageKeyOf(std::filesystem::absolute(D), std::filesystem::absolute(Root))) Targets.insert(*K);
            else { if (Error) *Error = "'" + D.string() + "' is not a package under " + Root.string(); return {}; }
        Sel = Targets;
        for (bool Grew = true; Grew;)                                        // close over what the selection contains
        {
            Grew = false;
            for (const auto &[Handle, Doc] : Tree)
            {
                const auto K = KeyOf.find(Handle);
                if (K == KeyOf.end() || !Sel.count(K->second)) continue;
                std::vector<std::string> Nots;
                std::vector<std::string> Refs = NodeRefs::Contained(Doc, &Nots);
                Refs.insert(Refs.end(), Nots.begin(), Nots.end());
                for (const std::string &R : Refs)
                    if (const auto RK = KeyOf.find(R); RK != KeyOf.end() && Sel.insert(RK->second).second) Grew = true;
            }
        }
        for (const auto &[Handle, Doc] : Tree)
            if (const auto K = KeyOf.find(Handle); K != KeyOf.end() && Sel.count(K->second)) Sub.emplace(Handle, Doc);
        for (const PkgKey &T : Targets)
            if (std::none_of(Sub.begin(), Sub.end(), [&](const auto &E) { return KeyOf.at(E.first) == T; }))
            { if (Error) *Error = "no nodes in package '" + T.second + "' (library '" + T.first + "')"; return {}; }
    }
    const std::map<std::string, nlohmann::ordered_json> &MintTree = Only ? Sub : Tree;
    NodeStore::MintResult MR;
    if (!NodeStore::Mint(MintTree, MR, Error)) return {};   // BlockPut every node → pinned + announced + seedable

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
    for (const auto &[Handle, Doc] : MintTree)
    {
        const auto CidIt = MR.HandleToCid.find(Handle);
        if (CidIt == MR.HandleToCid.end())             // node was skipped (dangling/bad) — not shareable
        {
            const auto Why = MR.Skipped.find(Handle);
            Unfrozen.push_back(Dirs[Handle].filename().string() + "/" + Doc.value("LABEL", std::string("?"))
                               + (Why == MR.Skipped.end() ? std::string() : ": " + Why->second));
            continue;
        }
        const auto Key = KeyOf.find(Handle);
        if (Key == KeyOf.end()) continue;
        const std::string &LibName = Key->second.first;    // the collection dir
        const std::string &Pkg     = Key->second.second;   // the bundle dir under it
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
            for (const std::string &R : NodeRefs::Contained(Tree.at(H)))
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
            //A package the selection only contains keeps its record while that record is current.
            if (Only && !Targets.count(PkgKey{LibName, Pkg}))
                if (const nlohmann::ordered_json *Was = RowOf(Config, LibName, TitleOf(Pkg));
                    Was && Was->value("cid", std::string()) == PCid && !Was->value("pin", std::string()).empty())
                {
                    Libs[LibName].push_back(*Was);
                    Flat.push_back(PCid);
                    ++PkgCount;
                    continue;
                }
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
    if (!Only) Config["Libraries"] = std::move(Libs);
    else
    {   // merged in place: the published packages' rows replace their own, every other row stays as it was
        if (!Config.contains("Libraries") || !Config["Libraries"].is_object()) Config["Libraries"] = nlohmann::ordered_json::object();
        for (auto &[LibName, Rows] : Libs.items())
            for (auto &R : Rows) MergeRow(Config["Libraries"], LibName, std::move(R));
    }
    Config.erase("PublishedList");       // what the share list once was kept as: it is the return value, and Libraries
    Config.erase("PublishedManifest");
    LogSucc("PackageCatalog::PublishLibrary", "published " + std::to_string(MR.HandleToCid.size())
            + " node block(s) in " + std::to_string(PkgCount) + " package folder(s), "
            + std::to_string(Config["Libraries"].size()) + " library(ies)"
            + (Only ? " (" + std::to_string(Targets.size()) + " package(s) named, " + std::to_string(Sel.size() - Targets.size())
                      + " contained)" : std::string()));
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

} // namespace

std::vector<std::string> PublishLibrary(nlohmann::ordered_json &Config, std::string *Error, std::string *Gaps)
{
    return Publish(Config, nullptr, Error, Gaps);
}

std::vector<std::string> PublishPackages(nlohmann::ordered_json &Config, const std::vector<std::filesystem::path> &PackageDirs,
                                         std::string *Error, std::string *Gaps)
{
    if (PackageDirs.empty()) { if (Error) *Error = "no package named"; return {}; }
    return Publish(Config, &PackageDirs, Error, Gaps);
}


} // namespace PackageCatalog
