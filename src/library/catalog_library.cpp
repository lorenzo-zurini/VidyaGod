// The library as shared: which library a package is in, what a friend receives, and publishing the whole library.
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
        //A reference names a working-tree handle (a stored "CID" not yet re-minted) or, as a landed package's node
        //files all do, a node's CID: its file holds no handle, so only the CID the tree freezes to finds its package.
        const NodeIndex Frozen = NodeGraph::FreezeToIndex(Tree, Dirs);
        const auto PkgOfRef = [&](const std::string &R) -> std::optional<PkgKey> {
            if (const auto K = KeyOf.find(R); K != KeyOf.end()) return K->second;
            if (const auto N = Frozen.Nodes.find(R); N != Frozen.Nodes.end() && !N->second.BundleDir.empty())
                return PackageKeyOf(N->second.BundleDir, Root);
            return std::nullopt;
        };
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
                    if (const auto RK = PkgOfRef(R); RK && Sel.insert(*RK).second) Grew = true;
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
    //The share sheet: each published node's path, its file named by the rule among its package's nodes (chapter 02
    //§2.5). A package the selection only contains keeps its entries and pin while they are current.
    std::map<PkgKey, std::map<std::string, std::string>> Was;                         // package → cid → file
    if (Config.contains("ShareSheet") && Config["ShareSheet"].is_object())
        for (const SheetEntry &E : SheetEntries(Config["ShareSheet"])) Was[{E.Lib, E.Pkg}][E.Cid] = E.File;
    const nlohmann::ordered_json PinsWas = Config.contains("Pins") && Config["Pins"].is_object() ? Config["Pins"] : nlohmann::ordered_json::object();
    std::map<std::string, std::string> Sheet;                                          // cid → path, of what is published
    std::map<std::string, std::string> Pins;                                           // "lib/pkg" → its pin folder
    std::vector<std::string> Published;
    size_t Renamed = 0;
    for (auto &[LibName, Pkgs] : ByLibPkg)
    {
        for (auto &[Pkg, Cids] : Pkgs)
        {
            std::sort(Cids.begin(), Cids.end());
            Cids.erase(std::unique(Cids.begin(), Cids.end()), Cids.end());
            const std::vector<std::string> &Handles = HandlesOf[LibName][Pkg];
            std::map<std::string, nlohmann::ordered_json> Frozen;
            for (const std::string &H : Handles) Frozen.emplace(MR.HandleToCid.at(H), NodeGraph::FreezeNodeJson(Tree.at(H), MR.HandleToCid));
            const std::map<std::string, std::string> Files = NodeFiles::Names(Frozen);
            const std::string Key = LibName + "/" + Pkg;
            auto Record = [&](const std::string &Pin) {
                for (const auto &[C, F] : Files) Sheet[C] = Key + "/" + F;
                Pins[Key] = Pin;
                Published.push_back(Key);
            };
            if (Only && !Targets.count(PkgKey{LibName, Pkg}))
                if (const auto W = Was.find(PkgKey{LibName, Pkg});
                    W != Was.end() && W->second == Files && PinsWas.contains(Key) && PinsWas[Key].is_string())
                { Record(PinsWas[Key].get<std::string>()); continue; }
            //The frozen node files on disk take their names, and what refers to them follows (a received node's
            //file is referenced in place).
            std::vector<std::pair<std::filesystem::path, std::filesystem::path>> Moved;
            std::string NErr;
            if (!NodeFiles::Normalize(Dirs.at(Handles.front()), &Moved, &NErr))
                LogWarn("PackageCatalog::PublishLibrary", "package '" + Pkg + "': " + NErr);
            if (!Moved.empty())
            {
                std::vector<std::pair<std::string, std::string>> Refs;
                for (const auto &[From, To] : Moved) Refs.push_back({ From.string(), To.string() });
                std::string MErr;
                if (IpfsWrapper::MoveRefs(Refs, &MErr) < 0)
                    LogWarn("PackageCatalog::PublishLibrary", "package '" + Pkg + "': renamed its node files, but their "
                            "references did not follow: " + MErr);
                Renamed += Moved.size();
            }
            // The PIN folder: the package's node files under their names + its own content, linked — what a pinning
            // service pins, so it holds the package whole (references inside a node are plain CIDs: nothing follows
            // them). A library the package contains is its own package with its own pin: no byte is billed twice.
            std::map<std::string, std::string> Entries;
            for (const auto &[C, F] : Files) Entries[F] = C;
            std::string MErr;
            const std::string PCid = IpfsWrapper::MakeDir(Entries, &MErr);
            if (PCid.empty()) { if (Error) *Error = "package folder for '" + Pkg + "' not made: " + MErr; return {}; }
            std::map<std::string, std::string> Content;
            for (const std::string &H : Handles)
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
            Record(PinCid);
        }
    }
    //Recorded: the whole sheet, or the published packages' entries in place of their own (every other entry stays).
    if (Only)
    {
        std::set<std::string> Keys(Published.begin(), Published.end());
        for (const auto &[K, Files] : Was)
            if (!Keys.count(K.first + "/" + K.second))
                for (const auto &[C, F] : Files) Sheet.emplace(C, K.first + "/" + K.second + "/" + F);
        for (const auto &[K, V] : PinsWas.items()) if (V.is_string()) Pins.emplace(K, V.get<std::string>());
    }
    Config["ShareSheet"] = nlohmann::ordered_json::object();
    for (const auto &[C, P] : Sheet) Config["ShareSheet"][C] = P;
    Config["Pins"] = nlohmann::ordered_json::object();
    for (const auto &[K, P] : Pins) Config["Pins"][K] = P;
    LogSucc("PackageCatalog::PublishLibrary", "published " + std::to_string(MR.HandleToCid.size())
            + " node block(s) in " + std::to_string(Published.size()) + " package(s), "
            + std::to_string(Config["ShareSheet"].size()) + " entries in the share sheet, "
            + std::to_string(Renamed) + " node file(s) renamed"
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
    return Published;
}

} // namespace

std::vector<SheetEntry> SheetEntries(const nlohmann::ordered_json &Sheet)
{
    std::vector<SheetEntry> Out;
    if (!Sheet.is_object()) return Out;
    for (const auto &[Cid, P] : Sheet.items())
    {
        if (!P.is_string()) continue;
        const std::string S = P.get<std::string>();
        const size_t A = S.find('/'), B = S.rfind('/');
        if (Cid.empty() || A == std::string::npos || A == B || S.find('/', A + 1) != B || A == 0 || B == A + 1 || B + 1 == S.size())
            continue;
        Out.push_back({ Cid, S.substr(0, A), S.substr(A + 1, B - A - 1), S.substr(B + 1) });
    }
    return Out;
}

std::vector<SheetEntry> WireEntries(const nlohmann::ordered_json &Libs)
{
    std::vector<SheetEntry> Out;
    if (!Libs.is_object()) return Out;
    for (const auto &[Lib, Items] : Libs.items())
        if (Items.is_array())
            for (const auto &It : Items)
            {
                if (!It.is_object()) continue;
                SheetEntry E{ It.value("cid", std::string()), Lib, It.value("pkg", std::string()), It.value("node", std::string()) };
                if (!E.Cid.empty() && !E.Lib.empty() && !E.Pkg.empty() && !E.File.empty()) Out.push_back(std::move(E));
            }
    return Out;
}

nlohmann::ordered_json Wire(const std::vector<SheetEntry> &Entries)
{
    std::map<std::string, std::map<std::pair<std::string, std::string>, std::string>> By;   // lib → (pkg, file) → cid
    for (const SheetEntry &E : Entries) By[E.Lib][{ E.Pkg, E.File }] = E.Cid;
    nlohmann::ordered_json Out = nlohmann::ordered_json::object();
    for (const auto &[Lib, Items] : By)
    {
        nlohmann::ordered_json A = nlohmann::ordered_json::array();
        for (const auto &[PF, Cid] : Items) A.push_back({ {"cid", Cid}, {"pkg", PF.first}, {"node", PF.second} });
        Out[Lib] = std::move(A);
    }
    return Out;
}

std::string SheetPath(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &Cid)
{
    if (GlobalConfigJSON.contains("ShareSheet") && GlobalConfigJSON["ShareSheet"].is_object()
        && GlobalConfigJSON["ShareSheet"].contains(Cid) && GlobalConfigJSON["ShareSheet"][Cid].is_string())
        return GlobalConfigJSON["ShareSheet"][Cid].get<std::string>();
    if (GlobalConfigJSON.contains("FriendLibraries") && GlobalConfigJSON["FriendLibraries"].is_object())
        for (const auto &[Peer, Libs] : GlobalConfigJSON["FriendLibraries"].items())
            for (const SheetEntry &E : WireEntries(Libs))
                if (E.Cid == Cid) return E.Lib + "/" + E.Pkg + "/" + E.File;
    return {};
}

std::vector<std::string> NodeContains(const NodeIndex &Idx, const std::string &Cid)
{
    if (const Node *N = Idx.Find(Cid); N && N->Key() == Cid) return N->Refs;
    if (!IpfsWrapper::HasLocal(Cid)) return {};                         // never fetched to be read: a sheet is ours
    const nlohmann::ordered_json J = NodeGraph::ParseBounded(IpfsWrapper::BlockGet(Cid));
    return ManifestModel::IsNodeObject(J) ? NodeRefs::Contained(J) : std::vector<std::string>();
}

std::vector<std::string> PackagesContained(const nlohmann::ordered_json &Sheet, const std::vector<std::string> &Packages,
                                           const std::function<std::vector<std::string>(const std::string &)> &Contains)
{
    std::map<std::string, std::string> PkgOf;                                          // cid → "lib/pkg"
    std::map<std::string, std::vector<std::string>> CidsOf;                            // "lib/pkg" → its cids
    for (const SheetEntry &E : SheetEntries(Sheet))
    {
        PkgOf.emplace(E.Cid, E.Lib + "/" + E.Pkg);
        CidsOf[E.Lib + "/" + E.Pkg].push_back(E.Cid);
    }
    std::set<std::string> Taken;
    std::deque<std::string> Todo(Packages.begin(), Packages.end());
    while (!Todo.empty())
    {
        const std::string K = Todo.front();
        Todo.pop_front();
        if (!CidsOf.count(K) || !Taken.insert(K).second) continue;
        for (const std::string &C : CidsOf[K])
            for (const std::string &R : Contains(C))
                if (const auto P = PkgOf.find(R); P != PkgOf.end()) Todo.push_back(P->second);
    }
    return { Taken.begin(), Taken.end() };
}

nlohmann::ordered_json SharedWith(const nlohmann::ordered_json &Sheet, const std::vector<std::string> &SharedLibs,
                                  const std::function<std::vector<std::string>(const std::string &)> &Contains)
{
    const std::vector<SheetEntry> All = SheetEntries(Sheet);
    std::vector<std::string> Start;
    for (const SheetEntry &E : All)
        if (std::find(SharedLibs.begin(), SharedLibs.end(), E.Lib) != SharedLibs.end()) Start.push_back(E.Lib + "/" + E.Pkg);
    const std::vector<std::string> Pkgs = PackagesContained(Sheet, Start, Contains);
    const std::set<std::string> Taken(Pkgs.begin(), Pkgs.end());
    std::vector<SheetEntry> Out;
    for (const SheetEntry &E : All) if (Taken.count(E.Lib + "/" + E.Pkg)) Out.push_back(E);
    return Wire(Out);
}

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
