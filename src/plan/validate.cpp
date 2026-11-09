// Validate — the checks over the library a launch would meet: the format's rules node by node, then each launchable
// resolved (its content, deltas and edits against the real files, the runner chain, what a variable needs), and the
// shelf; and the fix for content whose names collide by case across layers. Reads the content (zips, delta chains)
// but never changes anything but what FixCaseConflicts is asked to fix.
#include "validate.h"
#include "manifestmodel.h"
#include "nodegraph.h"
#include "parallel.h"
#include "contentfile.h"
#include "commonutils.h"
#include "fold.h"        // Fold::Resolve — a node's facts are folded, never inherited by graph distance
#include "nodelower.h"   // NodeLower — the gen-6 vocabulary check and the lowering to engine ops
#include "varsubst.h"    // VarSubst::ConditionParses — WHEN syntax lint
#include "launchparams.h"   // ContainerParams::GetVariablesMap — the single source of truth for built-in %variables%
#include "launchtree.h"  // LaunchTree — a launchable's files as VidyaGodFS reads them: bases, hiding, case
#include "registrywrapper.h"  // NamesNoKey — a registry KEEP must name a key
#include "platform/platform.h"  // HostFolderNames — %HostAppData% & co., set on Windows only

#include <algorithm>
#include <atomic>
#include <cctype>
#include <climits>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <zip.h>

namespace Validate {
using namespace ManifestModel;

namespace {

std::string ToLowerAscii(std::string S) { for (char &c : S) c = (char)std::tolower((unsigned char)c); return S; }

// The regular file paths inside a zip (slash-normalized, directory entries dropped). Empty on open failure.
const std::vector<std::string> &ZipEntriesCached(const std::string &Path,
        std::unordered_map<std::string, std::vector<std::string>> &Cache)
{
    auto It = Cache.find(Path);
    if (It != Cache.end()) return It->second;
    std::vector<std::string> Out;
    int Err = 0;
    if (zip_t *Za = zip_open(Path.c_str(), ZIP_RDONLY, &Err))
    {
        const zip_int64_t N = zip_get_num_entries(Za, 0);
        for (zip_uint64_t i = 0; i < (zip_uint64_t)N; ++i)
        {
            const char *Name = zip_get_name(Za, i, ZIP_FL_ENC_RAW);
            if (!Name || !*Name) continue;
            std::string S = Name;
            std::replace(S.begin(), S.end(), '\\', '/');
            if (S.back() == '/') continue;                                   // directory entry
            Out.push_back(std::move(S));
        }
        zip_close(Za);
    }
    return Cache.emplace(Path, std::move(Out)).first->second;
}


// Join a layer's TARGET (mount offset within the content root) and an in-layer relative path into one
// content-root-relative slash path.
std::string JoinTarget(std::string Target, std::string Rel)
{
    std::replace(Target.begin(), Target.end(), '\\', '/');
    std::replace(Rel.begin(), Rel.end(), '\\', '/');
    while (!Target.empty() && Target.front() == '/') Target.erase(Target.begin());
    while (!Target.empty() && Target.back()  == '/') Target.pop_back();
    while (!Rel.empty() && Rel.front() == '/') Rel.erase(Rel.begin());
    return Target.empty() ? Rel : Target + "/" + Rel;
}

// A resolved content item's local file: its bundle dir + payload (a %runtime% payload has none).
std::filesystem::path ItemLocal(const Fold::Item &I)
{
    if (I.Dir.empty() || VarSubst::RuntimeSourced(I.Payload)) return {};
    return ContentFile::Path(I.Dir, I.Payload, I.Source.is_string() ? I.Source.get<std::string>() : std::string());
}

// The bytes a content item's archive view holds, as a delta built over it reads them: a ZIP's file size, a DELTA's
// rebuilt size (its header). Base = the bytes a DELTA was built over (its header). nullopt: not here to read.
struct ArchiveBytes { std::optional<uint64_t> View, Base; };
ArchiveBytes ArchiveSizes(const Fold::Item &I, std::map<std::string, ArchiveBytes> &Cache)
{
    const std::filesystem::path Local = ItemLocal(I);
    if (Local.empty()) return {};
    if (auto It = Cache.find(Local.string()); It != Cache.end()) return It->second;
    ArchiveBytes Out;
    std::error_code Ec;
    if (I.Kind == "ZIP")
    {
        const uint64_t N = std::filesystem::file_size(Local, Ec);
        if (!Ec) Out.View = N;
    }
    else if (I.Kind == "DELTA")
    {
        //Header: "VGD1" | u32 version | u32 block | u64 target size | u64 base size (little-endian).
        std::ifstream F(Local, std::ios::binary);
        unsigned char H[28] = {};
        if (F.read(reinterpret_cast<char *>(H), sizeof H) && std::memcmp(H, "VGD1", 4) == 0)
        {
            auto U64 = [&](size_t O) { uint64_t V = 0; for (size_t B = 8; B-- > 0;) V = (V << 8) | H[O + B]; return V; };
            Out.View = U64(12);
            Out.Base = U64(20);
        }
    }
    return Cache.emplace(Local.string(), Out).first->second;
}

// The case-sensitive set of target-relative file paths a launchable's resolved plan mounts, from LOCALLY-present
// content only (zips via libzip, dirs walked). AnyLocal = ≥1 layer read; AllLocal = every content layer was on disk.
void GatherLaunchContentFiles(const Fold::Plan &P,
        std::unordered_map<std::string, std::vector<std::string>> &ZipCache,
        std::set<std::string> &Files, bool &AnyLocal, bool &AllLocal)
{
    AnyLocal = false; AllLocal = true;
    for (const Fold::Item &I : P.Seq)
    {
        if (I.Kind == "EDIT") continue;
        const std::filesystem::path Local = ItemLocal(I);
        if (Local.empty()) continue;
        std::error_code Ec;
        if (!std::filesystem::exists(Local, Ec)) { AllLocal = false; continue; }   // remote-only / not hydrated
        AnyLocal = true;
        if (I.Kind == "FILE")
            //A FILE layer mounts one file at TARGET/<its name> (overlay.cpp: fileVPath).
            Files.insert(JoinTarget(I.Target, Local.filename().string()));
        else if (std::filesystem::is_directory(Local, Ec))
            for (auto It = std::filesystem::recursive_directory_iterator(Local, Ec);
                 !Ec && It != std::filesystem::recursive_directory_iterator(); It.increment(Ec))
            {
                if (!It->is_regular_file(Ec)) continue;
                Files.insert(JoinTarget(I.Target, std::filesystem::relative(It->path(), Local, Ec).generic_string()));
            }
        else
            for (const std::string &Entry : ZipEntriesCached(Local.string(), ZipCache))
                Files.insert(JoinTarget(I.Target, Entry));
    }
}

// ----- what a content layer REPLACES (the covered-edit check, §1.5; mirrors tools/gen6/resolve.py provided()) -----
// What the validator reads of a launchable's files: its tree as VidyaGodFS makes it (LaunchTree), the archives' member
// names (shared across a package's launchables: a chain is listed once), and those names lower-cased, once per archive.
struct TreeNames
{
    ArchiveNames Names;
    std::map<const std::vector<std::string> *, std::set<std::string>> Lower;
    std::map<std::string, std::vector<std::string>> Dirs;   // a dir layer's source -> its files, relative

    const std::set<std::string> *LowerOf(const LaunchTree &L, size_t Layer)
    {
        const std::vector<std::string> *E = Names.Of(L, Layer, false);
        if (!E) return nullptr;
        auto [It, New] = Lower.try_emplace(E);
        if (New) for (const auto &N : *E) It->second.insert(ToLowerAscii(N));
        return &It->second;
    }
    const std::vector<std::string> *DirFiles(const std::string &Dir)
    {
        if (const auto It = Dirs.find(Dir); It != Dirs.end()) return &It->second;
        std::error_code Ec;
        if (!std::filesystem::is_directory(Dir, Ec)) return nullptr;
        std::vector<std::string> Out;
        for (auto W = std::filesystem::recursive_directory_iterator(Dir, Ec);
             !Ec && W != std::filesystem::recursive_directory_iterator(); W.increment(Ec))
            if (W->is_regular_file(Ec)) Out.push_back(std::filesystem::relative(W->path(), Dir, Ec).generic_string());
        return &Dirs.emplace(Dir, std::move(Out)).first->second;
    }
    //A layer's contents for the FS's case queries: an archive's member names (Folders: as the zip holds them, folder
    //entries kept), a dir's files; a FILE layer the FS places itself.
    EntriesFn Entries(const LaunchTree &L, bool Folders)
    {
        return [this, &L, Folders](size_t I) -> const std::vector<std::string> * {
            const LayerSpec &X = L.S.layers[I];
            if (X.type == ::LayerType::Dir) return DirFiles(X.source);
            return Names.Of(L, I, Folders);
        };
    }
};

// The lower-cased paths content Seq[J] provides as the mount sees them (a FILE lands at TARGET/<name>), or nullopt =
// everything under its TARGET (a DIR, an archive not readable here).
std::optional<std::set<std::string>> Provided(const Fold::Plan &P, const LaunchTree &L, size_t J, TreeNames &Tn)
{
    const Fold::Item &C = P.Seq[J];
    const std::string T = ToLowerAscii(C.Target);
    const std::string Pre = T.empty() ? std::string() : T + "/";
    if (C.Kind == "DIR" || C.Dir.empty()) return std::nullopt;
    if (C.Kind == "FILE") return std::set<std::string>{ Pre + ToLowerAscii(std::filesystem::path(C.Payload).filename().string()) };
    const auto It = L.LayerOf.find(J);
    const std::set<std::string> *Names = It == L.LayerOf.end() ? nullptr : Tn.LowerOf(L, It->second);
    if (!Names) return std::nullopt;
    std::set<std::string> Out;
    for (const auto &N : *Names) Out.insert(Pre + N);
    return Out;
}

bool Covers(const Fold::Plan &P, const LaunchTree &L, size_t J, const std::string &File, TreeNames &Tn)
{
    const std::string F = ToLowerAscii(File);
    const auto Pv = Provided(P, L, J, Tn);
    if (!Pv)
    {
        const std::string T = ToLowerAscii(P.Seq[J].Target);
        return F == T || F.rfind(T + "/", 0) == 0 || T.empty();
    }
    return Pv->count(F) != 0;
}

std::string FileName(const std::string &Path) { return std::filesystem::path(Path).filename().string(); }

// Reports case collisions across DIFFERENT layers in a launchable's merged content view, as VidyaGodFS places and hides
// them (CaseConflicts): two layers contributing paths that differ only in case (e.g. base ships 'MAPS/foo', a patch
// ships 'maps/foo'). On the case-sensitive mount both exist, so a lookup can resolve to the wrong file → crashes /
// missing data. Collisions WITHIN one archive are the upstream game's own content and are ignored. GlobalSeen dedups
// across launchables sharing content.
void FindCrossLayerCaseCollisions(const LaunchTree &L, TreeNames &Tn, std::vector<std::string> &Out,
                                  std::unordered_set<std::string> &GlobalSeen, std::vector<std::string> *KeysOut)
{
    for (const CaseConflict &C : CaseConflicts(L.S, L.T, Tn.Entries(L, false)))
    {
        const std::string A = FileName(L.S.layers[C.first].source), B = FileName(L.S.layers[C.second].source);
        const std::string Seen1 = C.key + "|" + ToLowerAscii(A) + "|" + ToLowerAscii(B);
        if (!GlobalSeen.insert(Seen1).second) continue;
        if (KeysOut) KeysOut->push_back(Seen1);   // the caller de-duplicates across nodes by it
        Out.push_back(std::string("content case conflict across layers: ") + (C.dir ? "directory '" : "file '")
                      + C.firstName + "' (" + A + ") vs '" + C.secondName + "' (" + B + ") — collide in the merged view");
    }
}

} // namespace

std::vector<std::string> Shelf(const NodeIndex &Idx, const std::set<std::string> *OnlyNodes)
{
    std::map<std::string, std::vector<const Node *>> Presenters;   // UID -> the nodes with it among their faces
    for (const auto &[K, N] : Idx.Nodes)
        for (const std::string &U : N.Faces) Presenters[U].push_back(&N);
    std::vector<std::string> Out;
    for (const auto &[U, T] : Idx.Tiles)
    {
        const auto &Ps = Presenters[U];
        if (OnlyNodes && std::none_of(Ps.begin(), Ps.end(), [&](const Node *N) { return OnlyNodes->count(N->NodeId) != 0; }))
            continue;
        const Node *Shown = nullptr;   // the node the presentation comes from: recommended variant, else first variant
        for (const Node *N : Ps)
        {
            if (!N->IsVariant()) continue;
            const bool Rec = std::find(N->Recommended.begin(), N->Recommended.end(), U) != N->Recommended.end();
            const bool ShownRec = Shown && std::find(Shown->Recommended.begin(), Shown->Recommended.end(), U) != Shown->Recommended.end();
            if (!Shown || (Rec && !ShownRec) || (Rec == ShownRec && N->Variant + '\x1f' + N->NodeId < Shown->Variant + '\x1f' + Shown->NodeId))
                Shown = N;
        }
        const Node *Where = Shown ? Shown : (Ps.empty() ? nullptr : Ps.front());
        const std::string Pkg = Where ? std::filesystem::path(Where->BundleDir).filename().string() : std::string("?");
        const std::string Title = T.contains("TITLE") && T["TITLE"].is_string() ? T["TITLE"].get<std::string>() : std::string();
        const std::string Name = "tile " + U + (Title.empty() ? std::string() : " '" + Title + "'") + " (" + Pkg + ")";
        if (!Shown && !Idx.TileGraft.count(U))
            Out.push_back(Name + ": no VARIANT presents it — the shelf shows a card with nothing to launch. Give the node "
                          "that presents it VARIANT and RECOMMENDED [\"" + U + "\"].");
        if (Title.empty())
            Out.push_back(Name + ": the shelf takes this tile from " + (Shown ? "variant '" + Shown->Variant + "'" : std::string("its presenting node"))
                          + ", whose folded tile has no TITLE — the card is named by a CID. Put the full tile (TITLE, COVER, "
                          "META) in a node every variant of the game contains (\"tile at the base\"), keeping only {\"UID\"} on the variants.");
    }
    return Out;
}

void Nodes(const NodeIndex &Idx, std::vector<std::string> &Errors, std::vector<std::string> &Warnings,
                       const std::set<std::string> *OnlyNodes)
{
    const std::string Machine = MachinePlatform();
    const Fold::Library Lib = LibraryOf(Idx);
    const Fold::GraftIndex Grafts = Fold::BuildGraftIndex(Lib);
    //What a row is checked with "every offered graft": add-ons only. A graft that is a version, or presents a card of
    //its own, is applied as the row (first), never offered on top of another one — checked as an add-on, it landed
    //after the add-ons that build on it (Horn of the Abyss's zip over the HD mod's patch of h3hota.exe).
    Fold::GraftIndex AddOns = Grafts;
    for (auto &[Member, Gs] : AddOns)
        Gs.erase(std::remove_if(Gs.begin(), Gs.end(), [&](const std::string &G) {
            const Node *Gn = Idx.Find(G);
            if (Gn && !Gn->Variant.empty()) return true;
            for (const auto &[U, Card] : Idx.TileGraft) if (Card == G) return true;
            return false;
        }), Gs.end());
    auto InScope = [&](const std::string &Id) { return !OnlyNodes || OnlyNodes->count(Id); };

    //A node the index refused has no CID, so nothing can contain it: its container reports a missing node while the
    //file sits in the package. Name the file, and for one too large the split that brings it under (§2.3). The name
    //it is contained by is its handle, else its file's (a node file is named by its CID). In scope when a scoped node
    //shares its package or contains it.
    std::map<std::string, const NodeIndex::Refused *> RefusedByName;
    for (const auto &R : Idx.RefusedNodes)
    {
        const std::string Name = R.Handle.empty() ? R.File.stem().string() : R.Handle;
        RefusedByName.emplace(Name, &R);
        const auto Near = [&](const std::string &Id) {
            const Node *N = Idx.Find(Id);
            return N && ((!R.BundleDir.empty() && N->BundleDir == R.BundleDir)
                         || std::find(N->Refs.begin(), N->Refs.end(), Name) != N->Refs.end());
        };
        if (OnlyNodes && std::none_of(OnlyNodes->begin(), OnlyNodes->end(), Near)) continue;
        Errors.push_back((R.File.empty() ? "node '" + R.Handle + "'" : "node file '" + R.File.string() + "'")
                         + " is refused: " + R.Why
                         + (R.TooLarge ? " — move a run of its layers into a node of their own and put a NODE layer "
                                         "naming it in their place (it expands in place, so nothing changes), or "
                                         "Delete a whole folder instead of each file in it" : std::string()));
    }

    //A platform is served when a CHAIN of runners reaches this machine (a Windows-only engine for its own platform runs
    //under proton): the same guest→host edges the resolver's BFS walks (ResolveChainIds).
    auto HasRunnerFor = [&](const std::string &Host, const std::filesystem::path &Bundle) {
        std::set<std::string> Seen = { Host };
        std::deque<std::string> Todo = { Host };
        while (!Todo.empty())
        {
            const std::string P = Todo.front(); Todo.pop_front();
            for (const auto &[Id, R] : Idx.Nodes)
            {
                if (!R.OwnRunner || !ServesPlatform(R, P, Bundle)) continue;
                if (R.HostPlatform == Machine) return true;
                if (Seen.insert(R.HostPlatform).second) Todo.push_back(R.HostPlatform);
            }
        }
        return false;
    };
    std::set<std::string> KnownTiles;
    for (const auto &[Id, N] : Idx.Nodes) for (const auto &U : N.Faces) KnownTiles.insert(U);

    //Packages are checked on every core: a node writes only its own findings, and the nodes of one package are checked
    //one after another on one thread, sharing its read caches (zip listings, delta listings — a version chain is listed
    //once, not once per version — archive sizes, the cycle memo). The findings are then taken in node order, a case
    //conflict across layers reported once across the library as the pass always has: the same report as before.
    struct Worker
    {
        std::unordered_map<std::string, std::vector<std::string>> ZipCache;   // local zip path -> its file entries
        TreeNames Tree;                                                                          // archive names, lower-cased; dir files
        std::map<std::string, ArchiveBytes> SizeCache;                                          // archive file -> its sizes
        std::unordered_set<std::string> CycleDone;   // a node proven acyclic never participates in a cycle (O(N+E) overall)
    };
    struct NodeOut { std::vector<std::string> Errors, Warnings, Collisions, CollisionKeys; std::unordered_set<std::string> CollisionSeen; };
    const std::map<std::string, std::vector<std::string>> AllBases = AllGraftBases(Idx);
    std::vector<const std::pair<const std::string, Node> *> Items;
    for (const auto &KV : Idx.Nodes) Items.push_back(&KV);
    std::vector<NodeOut> Outs(Items.size());
    std::map<std::filesystem::path, std::vector<std::size_t>> ByPackage;   // package folder -> its nodes, in node order
    for (std::size_t I = 0; I < Items.size(); ++I) if (InScope(Items[I]->first)) ByPackage[Items[I]->second.BundleDir].push_back(I);
    std::vector<const std::vector<std::size_t> *> Packages;
    for (const auto &[Dir, Ns] : ByPackage) { (void)Dir; Packages.push_back(&Ns); }
    const auto CheckNode = [&](Worker &Wk, const std::string &Id, const Node &N, NodeOut &Res)
    {
        if (!InScope(Id)) return;
        const std::string Tag = "node '" + Id + "'";
        //A malformed node is indexed with the reason and routed through by nothing — a package that quietly does
        //less than it says. Name it.
        if (!N.LowerError.empty()) { Res.Errors.push_back(N.LowerError); return; }
        //One home: the same node in a second package is a copy that drifts from its home's files and pins, and makes
        //"whose engine is this" a guess. It belongs in one package; the others contain it by CID.
        if (const auto D = Idx.Duplicates.find(Id); D != Idx.Duplicates.end())
            for (const auto &Other : D->second)
                Res.Errors.push_back(Tag + (N.NodeId.empty() ? std::string() : " (" + N.NodeId + ")") + " is in package '"
                                 + N.BundleDir.filename().string() + "' and again in '" + Other.filename().string()
                                 + "' — a node has one home package; delete the copy and contain the node by CID");

        //Refs: a contained node must exist; an ANY member or a NOT may name a node this library lacks (a graft for a
        //version you don't have; an exclusion that cannot happen), but an ANY with no member here is unsatisfiable.
        for (const std::string &R : N.Refs)
            if (!Idx.Find(R))
                Res.Errors.push_back(Tag + ": contains missing node '" + R + "'"
                                 + (RefusedByName.count(R) ? " (its file is here but refused — see its error)" : ""));
        for (const auto &L : (*N.Json)["LAYERS"])
        {
            if (L.contains("ANY"))
            {
                bool Present = false;
                for (const auto &M : L["ANY"]) if (Idx.Find(M.get<std::string>())) { Present = true; break; }
                if (!Present) Res.Warnings.push_back(Tag + ": ANY names no node in this library (nothing it applies onto is here)");
            }
            if (L.contains("NOT") && !Idx.Find(L["NOT"].get<std::string>()))
                Res.Warnings.push_back(Tag + ": NOT names node '" + L["NOT"].get<std::string>() + "', which is not in the library");
        }
        {
            std::unordered_set<std::string> OnStack;
            std::function<bool(const std::string &)> HasCycle = [&](const std::string &Cur) -> bool {
                if (Wk.CycleDone.count(Cur)) return false;
                if (OnStack.count(Cur)) return true;
                OnStack.insert(Cur);
                const Node *C = Idx.Find(Cur);
                if (C) for (const std::string &P : C->Refs) if (Idx.Find(P) && HasCycle(P)) return true;
                OnStack.erase(Cur); Wk.CycleDone.insert(Cur);
                return false;
            };
            if (HasCycle(Id)) Res.Errors.push_back(Tag + ": NODE layers form a cycle");
        }

        //Content: a local zip must be STOREd (VidyaGodFS reads entries at their backing offset); a DIR that is not a
        //%runtime% path is an authoring intermediary that cannot be published.
        for (const auto &L : N.ContentLayers)
        {
            const std::string LType = ManifestModel::LayerType(L);
            if (!IsVfsLayer(LType)) continue;
            if (LType == "VFSDirLayer" && !IsRuntimeSourcedLayer(L))
                Res.Warnings.push_back(Tag + ": DIR '" + L.value("PATH", std::string())
                    + "' is an unzipped authoring layer — convert it to a STORE zip ('→ ZIP') before publishing.");
            if (LType == "VFSZipLayer")
            {
                const std::string Local = ResolveLayerSource(L, N.BundleDir);
                std::string FirstCompressed;
                if (!Local.empty() && std::filesystem::exists(Local) && !ZipFullyStored(Local, &FirstCompressed))
                    Res.Errors.push_back(Tag + ": ZIP '" + L.value("PATH", std::string()) + "' is DEFLATE-compressed (entry '"
                        + FirstCompressed + "') — VidyaGodFS requires a STORE (uncompressed) zip. Re-create it with `zip -0`.");
            }
        }
        //Binary EDIT ops: a malformed one does nothing, or patches blindly.
        for (const auto &L : N.LoweredLayers())
        {
            if (ManifestModel::LayerType(L) != "BinaryPatch") continue;
            const std::string M = L.value("MODE", std::string());
            if (M == "ReplaceAll")
            {
                if (L.contains("ANCHOR") || L.contains("OFFSET")) Res.Errors.push_back(Tag + ": ReplaceAll takes no ANCHOR or OFFSET (it patches every match of EXPECT).");
                if (!L.contains("EXPECT")) Res.Errors.push_back(Tag + ": ReplaceAll has no EXPECT (the bytes to find).");
                if (!L.contains("REPLACE")) Res.Errors.push_back(Tag + ": ReplaceAll has no REPLACE bytes.");
                else if (L.contains("EXPECT"))
                {
                    //Bytes, not text: hex may be spaced ("ba 00 00"). A REPLACE read from a variable ("%fov:map:FOV%") has
                    //its length only once substituted - the launch checks it then, and reports a mismatch.
                    auto Hex = [](std::string H) { H.erase(std::remove_if(H.begin(), H.end(), [](unsigned char C) { return std::isspace(C); }), H.end()); return H; };
                    const std::string R = Hex(L.value("REPLACE", std::string())), E = Hex(L.value("EXPECT", std::string()));
                    if (R.find('%') == std::string::npos && E.find('%') == std::string::npos && R.size() != E.size())
                        Res.Errors.push_back(Tag + ": ReplaceAll REPLACE must be as long as EXPECT.");
                }
                continue;
            }
            if (!L.contains("ANCHOR") && !L.contains("OFFSET"))
                Res.Errors.push_back(Tag + ": binary EDIT op has neither ANCHOR nor OFFSET (no site to patch).");
            if (!L.contains("EXPECT") && M != "Or")
                Res.Warnings.push_back(Tag + ": binary EDIT op has no EXPECT guard — it patches without verifying the original bytes.");
            if (M == "Or" && L.contains("EXPECT")) Res.Errors.push_back(Tag + ": Or takes no EXPECT (the byte also carries other options' bits).");
            if (M == "Or" && !L.contains("VALUE")) Res.Errors.push_back(Tag + ": Or has no VALUE (the bits to set).");
            if (M == "Replace" && !L.contains("REPLACE")) Res.Errors.push_back(Tag + ": Replace has no REPLACE bytes.");
            if (M == "Poke" && !L.contains("VALUE"))      Res.Errors.push_back(Tag + ": Poke has no VALUE.");
            if (M == "Cave" && !L.contains("PAYLOAD"))    Res.Errors.push_back(Tag + ": Cave has no PAYLOAD.");
        }

        //Runners: what they serve, and how a prefix they generate maps guest coordinates.
        if (N.OwnRunner)
        {
            if (N.GuestPlatform.empty()) Res.Warnings.push_back(Tag + ": runner entry declares no GUEST platforms");
            if (N.Exec.is_object() && N.Exec.value("PREFIX_GENERATE", false) && !(N.Exec.contains("GUEST_ROOTS") && N.Exec.contains("DRIVES")))
                Res.Warnings.push_back(Tag + ": a runner that generates a prefix declares no GUEST_ROOTS/DRIVES — packages' anchored paths cannot be placed");
        }

        //The node's own facets.
        if (!N.Variant.empty() && !N.HasExec && !N.IsGraft)   // a graft version runs its base's entries
            Res.Errors.push_back(Tag + ": VARIANT '" + N.Variant + "' has no effective entry to run");
        if (!N.Variant.empty() && N.HasExec && N.Faces.empty() && !N.IsGraft)   // a graft version: under its bases' cards
            Res.Errors.push_back(Tag + ": VARIANT '" + N.Variant + "' presents no tile (no entry in its fold carries a TILE) — it appears under no card");
        for (const std::string &U : N.Recommended)
        {
            if (!N.Variant.empty() && !(N.IsGraft && N.Faces.empty()) && std::find(N.Faces.begin(), N.Faces.end(), U) == N.Faces.end())
                Res.Errors.push_back(Tag + ": RECOMMENDED names tile '" + U + "', which this variant does not present");
            else if (N.Variant.empty() && !N.IsGraft)
                Res.Warnings.push_back(Tag + ": RECOMMENDED on a node that is neither a variant nor a graft means nothing");
            else if (N.IsGraft && !KnownTiles.count(U))
                Res.Warnings.push_back(Tag + ": RECOMMENDED names tile '" + U + "', which no variant in this library presents");
        }
        if ((*N.Json)["LAYERS"].empty()) Res.Warnings.push_back(Tag + ": pointless node (no layers)");

        //Per ROW (a variant; a runner root): resolve it — default options, and every bool option on with every
        //offered graft ticked — and check what the resolution itself can see.
        if (!N.IsVariant() && !N.OwnRunner) return;
        //A graft that is a version is checked as it runs: on every version it applies onto (each is a row), with it
        //applied first.
        std::vector<std::string> Roots = { Id };
        std::vector<std::string> Base0;
        if (N.IsGraft)
        {
            Roots = AllBases.count(Id) ? AllBases.at(Id) : std::vector<std::string>();
            if (Roots.empty()) { Res.Errors.push_back(Tag + ": a graft that is a version, with nothing in this library it applies onto"); return; }
            Base0 = { Id };
        }
        const std::string Root = Roots.front();
        struct Config { std::string Root; Fold::Vars Inst; std::vector<std::string> Grafts; };
        std::vector<Config> Configs;
        for (const std::string &R : Roots)
        {
            Configs.push_back({ R, {}, Base0 });
            const Fold::Plan P0 = Fold::Resolve(Lib, R, {}, {}, Base0);
            Fold::Vars AllOn;
            for (const auto &[K, D] : P0.Decls.items())
                if (D.is_object() && D.contains("UI") && D["UI"].is_object() && D["UI"].value("CONTROL", std::string()) == "bool")
                    AllOn[K] = "1";
            //every offered graft, a graft on a graft included (offered once the graft it needs is applied)
            std::vector<std::string> All = Base0;
            //...minus what the row already contains (the launch window does not offer those either)
            std::set<std::string> Contained;
            for (const std::string &C : Closure(Idx, R)) Contained.insert(C);
            for (const std::string &B : Base0) for (const std::string &C : Closure(Idx, B)) Contained.insert(C);
            for (const std::string &G : Fold::ApplyGrafts(Lib, AddOns, R, AllOn, {}, N.Uid, nullptr, nullptr, true))
                if (G != Id && !Contained.count(G)) All.push_back(G);
            if (!AllOn.empty() || All.size() > Base0.size()) Configs.push_back({ R, AllOn, All });
        }
        std::set<std::string> Said;                                     // one report per problem across configs
        auto Once = [&](std::vector<std::string> &Where, const std::string &Msg) { if (Said.insert(Msg).second) Where.push_back(Msg); };
        for (const auto &[CRoot, Inst, Gs] : Configs)
        {
            const Fold::Plan P = Fold::Resolve(Lib, CRoot, Inst, {}, Gs);
            if (!P.Error.empty()) Once(Res.Errors, Tag + ": " + P.Error);
            //What the launch keeps is the FOLDED KEEP, across every node it contains: two kept files or folders stored
            //under one name (a base game's Save/ and its mod's Revision/Save/) - the launch keeps the first, silently.
            {
                std::map<std::string, std::string> Stored;   // durable name (lower) -> the address that took it
                for (const auto &[A, V] : P.Keep.items())
                {
                    if (A.rfind("FILES/", 0) != 0 || (V.is_boolean() && !V.get<bool>())) continue;
                    const std::string Path = A.substr(6);
                    std::string Name = V.is_object() && V.contains("NAME") && V["NAME"].is_string() ? V["NAME"].get<std::string>() : Path;
                    while (!Name.empty() && (Name.back() == '/' || Name.back() == '\\')) Name.pop_back();
                    if (const auto Slash = Name.find_last_of("/\\"); Slash != std::string::npos) Name = Name.substr(Slash + 1);
                    if (const auto [It, New] = Stored.emplace(ToLowerAscii(Name), Path); !New && It->second != Path)
                        Once(Res.Errors, Tag + ": KEEP '" + Path + "' is stored as '" + Name + "' like '" + It->second +
                                     "', which takes the name first - it would not be kept; give it its own NAME");
                }
            }
            //A registry key kept for one bitness while the package writes it for another only: the kept view is one the
            //game never uses, and nothing it changes is kept (a KEEP ["64"] over a REG layer ["32"]).
            for (const auto &[A, V] : P.Keep.items())
            {
                if (A.rfind("REG/", 0) != 0 || !V.is_object() || !V.contains("ARCH") || !V["ARCH"].is_array()) continue;
                //Both spelled alike: lower case, one '\\' between segments, HKLM for its long name. Only HKLM\Software
                //is a different key per bitness (HKCU is shared).
                const auto Spell = [](const std::string &Key) {
                    std::string Out, Cur;
                    for (const char C : ToLowerAscii(Key) + "\\")
                        if (C == '/' || C == '\\') { if (!Cur.empty()) Out += (Out.empty() ? "" : "\\") + (Out.empty() && Cur == "hkey_local_machine" ? std::string("hklm") : Cur); Cur.clear(); }
                        else Cur.push_back(C);
                    return Out;
                };
                const std::string Key = Spell(A.substr(4));
                if (Key.rfind("hklm\\software\\", 0) != 0) continue;
                const auto Under = [&](const std::string &Path) {
                    const std::string S = Spell(Path);
                    return S == Key || S.rfind(Key + "\\", 0) == 0;
                };
                std::set<std::string> Written;
                for (const auto &[K, R] : P.Reg) if (R.Arch.is_string() && Under(R.Path)) Written.insert(R.Arch.get<std::string>());
                for (const auto &[K, R] : P.RegKeys) if (R.first.is_string() && Under(R.second)) Written.insert(R.first.get<std::string>());
                bool Meets = Written.empty();
                for (const auto &Ar : V["ARCH"]) if (Ar.is_string() && Written.count(Ar.get<std::string>())) Meets = true;
                if (!Meets)
                    Once(Res.Warnings, Tag + ": KEEP '" + A + "' keeps the key for ARCH " + V["ARCH"].dump() + ", but its REG layers write it "
                                   "for another bitness only - the view kept is one the game never reads");
            }
            for (const auto &[Ev, Cid] : P.Events)
            {
                if (Ev == "cycle")     Once(Res.Errors, Tag + ": resolving it meets a cycle through '" + Cid + "'");
                if (Ev == "missing")   Once(Res.Errors, Tag + ": resolving it needs node '" + Cid + "', which is not in the library");
                if (Ev == "any-unmet" && !Gs.empty()) Once(Res.Warnings, Tag + ": graft '" + Cid + "' is offered but its ANY does not hold");
                else if (Ev == "any-unmet") Once(Res.Errors, Tag + ": '" + Cid + "' requires (ANY) something this row does not contain");
                if (Ev == "not-hit")   Once(Res.Errors, Tag + ": its resolution contains '" + Cid + "', which a NOT excludes");
            }
            //A node mentioned twice: where it counts (the first mention holds; a later one in the same list moves it)
            //is a rule, not a choice the author made — warn where the other reading would change what wins.
            for (const auto &[Ev, Cid] : Fold::DecidingMentions(Lib, Id, Inst, {}, Gs, P))
                Once(Res.Warnings, Tag + ": '" + [&]{ const Node *M = Idx.Find(Cid); return M && !M->NodeId.empty() ? M->NodeId + "' (" + Cid + ")" : Cid + "'"; }()
                     + " is mentioned twice and " + (Ev == "held"
                     ? std::string("its FIRST mention counts — folding it at the later one would change what wins")
                     : std::string("its LATER mention in the same list counts — folding it at the first would change what wins")));
            //A DELTA names its bases by CID (chapter 04 §4.1): VidyaGodFS's reading of this launch says which cannot
            //rebuild (a file it names is not in it), and the bytes of the files it names must be what its header says it
            //was built over.
            const LaunchTree Lt = LaunchTree::Of(P);
            for (const std::string &Why : Lt.T.problems)
                Once(Res.Errors, Tag + ": " + Why + " — contain the node that holds the file it names");
            for (size_t D = 0; D < Lt.S.layers.size(); ++D)
            {
                if (Lt.S.layers[D].type != ::LayerType::Delta || Lt.T.broken[D]) continue;
                const Fold::Item &C = P.Seq[Lt.ItemOf[D]];
                std::optional<uint64_t> Have = 0;
                std::string Names;
                for (size_t B : Lt.T.bases[D])
                {
                    const Fold::Item &Bi = P.Seq[Lt.ItemOf[B]];
                    const std::optional<uint64_t> Bytes = ArchiveSizes(Bi, Wk.SizeCache).View;
                    if (!Bytes) { Have.reset(); break; }
                    *Have += *Bytes;
                    Names += std::string(Names.empty() ? "" : " + ") + "'" + Bi.Payload + "'";
                }
                const std::optional<uint64_t> Built = ArchiveSizes(C, Wk.SizeCache).Base;
                if (Have && Built && *Have != *Built)
                    Once(Res.Errors, Tag + ": DELTA '" + C.Payload + "' was built over " + std::to_string(*Built)
                         + " bytes, but the bases it names are " + Names + " (" + std::to_string(*Have)
                         + " bytes) — name the files it was built over, in the order it joined them");
            }
            //Covered edits: an EDIT beneath a layer that replaces its file has no effect — always a mistake.
            for (size_t I = 0; I < P.Seq.size(); ++I)
            {
                if (P.Seq[I].Kind != "EDIT") continue;
                for (size_t J = I + 1; J < P.Seq.size(); ++J)
                    if (P.Seq[J].Kind != "EDIT" && Covers(P, Lt, J, P.Seq[I].Target, Wk.Tree))
                    {
                        Once(Res.Errors, Tag + ": the EDIT of '" + P.Seq[I].Target + "' (from '" + P.Seq[I].From + "') lies beneath '"
                             + P.Seq[J].Payload + "', which replaces that file — move the EDIT above it");
                        break;
                    }
            }
            //A launch option written into a file the user keeps: on a kept address an edit is the package's default,
            //applied only until the user has a saved copy — so the option works on the first launch and never again.
            //The option's key has to be taken back (KEEP false) to be re-applied over the user's copy.
            for (const Fold::Item &I : P.Seq)
                if (I.Kind == "EDIT" && I.Ops.is_array())
                    for (const auto &O : I.Ops)
                    {
                        const std::string Addr = NodeLower::EditAddress(O, I.Target);
                        //A Delete never reaches the user's state: the launch refuses one that would.
                        if (O.value("MODE", std::string()) == "Delete")
                        {
                            if (const std::string K = NodeLower::KeptUnder(P.Keep, Addr); !K.empty())
                                Once(Res.Errors, Tag + ": the Delete of '" + I.Target + "' (from '" + I.From + "') reaches '" + K
                                     + "', which the user keeps — a package never deletes the user's state");
                            continue;
                        }
                        if (!NodeLower::UserOwned(P.Keep, Addr)) continue;
                        //...whether the option gives the value or picks the op (its WHEN)
                        std::vector<std::string> Keys = VarSubst::TokenKeys(O.value("VALUE", std::string()));
                        for (const std::string &K : VarSubst::TokenKeys(O.value("WHEN", std::string()))) Keys.push_back(K);
                        for (const std::string &K : Keys)
                            if (P.Decls.contains(K) && P.Decls[K].is_object() && P.Decls[K].contains("UI"))
                            {
                                Once(Res.Warnings, Tag + ": launch option '" + K + "' is written to '" + Addr + "', which the user keeps — "
                                     "it applies only until the first saved copy. Take the address back: KEEP {\"" + Addr + "\": false}");
                                break;
                            }
                    }
        }
        if (!N.IsVariant()) return;
        const Fold::Plan P = Fold::Resolve(Lib, Root, {}, {}, Base0);
        //Every entry is a way to run the user can pick, so every one is checked.
        for (const auto &[Lbl, E] : N.Entries.items())
        {
            if (E.contains("GUEST") && E["GUEST"].is_array() && !E["GUEST"].empty()) continue;
            const std::string Host = E.value("HOST", std::string());
            if (Host.empty()) { Res.Warnings.push_back(Tag + ": entry '" + Lbl + "' has no HOST platform"); continue; }
            if (!HasRunnerFor(Host, N.BundleDir)) Res.Warnings.push_back(Tag + ": no runner serves platform '" + Host + "' on this machine");
            //Soft: a library without the recommended runner launches on the default chain. Said, so a typo is seen.
            if (const std::string Rec = E.value("RECOMMENDEDRUNNER", std::string()); !Rec.empty())
            {
                bool Have = false;
                for (const auto &[RK, R] : Idx.Nodes) Have = Have || (R.IsRunner() && R.NodeId == Rec);
                if (!Have) Res.Warnings.push_back(Tag + ": entry '" + Lbl + "' recommends runner '" + Rec + "', which no runner in this library is (its LABEL)");
            }
            //A folder is never a program: the starter Exe ("%GameDir%/") left as it was.
            if (const std::string X = E.value("EXE", std::string()); !X.empty() && (X.back() == '/' || X.back() == '\\'))
            { Res.Errors.push_back(Tag + ": entry '" + Lbl + "' runs a folder (EXE '" + X + "') - pick the game's program"); continue; }
            //EXE must case-EXACTLY match a real content file: the mount is case-sensitive. Checked where the content
            //is local and the path is plain (a %variable% or a guest drive is placed at launch).
            std::string Cp = E.value("EXE", std::string());
            if (Cp.empty() || Cp.find('%') != std::string::npos || Cp.find(':') != std::string::npos) continue;
            std::replace(Cp.begin(), Cp.end(), '\\', '/');
            while (Cp.rfind("./", 0) == 0) Cp.erase(0, 2);
            while (!Cp.empty() && Cp.front() == '/') Cp.erase(Cp.begin());
            std::set<std::string> Files; bool AnyLocal = false, AllLocal = true;
            GatherLaunchContentFiles(P, Wk.ZipCache, Files, AnyLocal, AllLocal);
            if (!AnyLocal || Files.count(Cp)) continue;
            const std::string CpL = ToLowerAscii(Cp);
            std::string Hit;
            for (const std::string &F : Files) if (ToLowerAscii(F) == CpL) { Hit = F; break; }
            if (!Hit.empty())
                Res.Errors.push_back(Tag + ": EXE '" + Cp + "' case-mismatches content file '" + Hit + "' — the mount is case-sensitive");
            else if (AllLocal)
                Res.Warnings.push_back(Tag + ": EXE '" + Cp + "' is not in the package content");
        }
        FindCrossLayerCaseCollisions(LaunchTree::Of(P), Wk.Tree, Res.Collisions, Res.CollisionSeen, &Res.CollisionKeys);
    };
    Parallel::ForWith<Worker>(Packages.size(), [&](Worker &Wk, std::size_t K) {
        for (const std::size_t I : *Packages[K]) CheckNode(Wk, Items[I]->first, Items[I]->second, Outs[I]);
        Wk = Worker{};   // a package's listings serve its own nodes: dropped, or every thread kept every package's
    });
    std::unordered_set<std::string> CrossLayerSeen;
    for (NodeOut &O : Outs)
    {
        for (auto &E : O.Errors) Errors.push_back(std::move(E));
        for (auto &Wn : O.Warnings) Warnings.push_back(std::move(Wn));
        for (std::size_t I = 0; I < O.Collisions.size(); ++I)
            if (CrossLayerSeen.insert(O.CollisionKeys[I]).second) Errors.push_back(std::move(O.Collisions[I]));
    }

    //----- WHEN: a malformed condition fail-opens (silently always-applies); and a LAYER's WHEN may only read what
    //phase 1 resolves — instance values, built-ins, and ungated VARS defaults (§4.1). -----
    static const std::set<std::string> Builtins = []{
        std::set<std::string> B = { "REL", "DefaultPfxDir", "WineFontsDir", "WineLibDir", "WineSys32Dir", "WineSysWow64Dir",
                                    "SysRegMtime", "UID", "VIDYAGOD_SANDBOX_NET", "VIDYAGOD_SELF_VIP",
                                    "VIDYAGOD_SELF_NAME", "VIDYAGOD_SUBNET", "VIDYAGOD_PEER_VIPS", "VIDYAGOD_PEER_NAMES",
                                    "VIDYAGOD_LAN_BRIDGE", "VIDYAGOD_LAN_HOSTRELAY" };
        for (const auto &[K, V] : ContainerParams().GetVariablesMap())
            B.insert(K);
        for (const std::string &N : Platform::HostFolderNames()) B.insert("Host" + N);   // Windows sets them, any host validates
        return B;
    }();
    std::set<std::string> Declared, DeclaredUngated;
    std::set<std::string> Tables;                      // declarations with a MAP: what %KEY:map:TABLE% may name
    std::map<std::string, std::string> UiDeclared;                       // option KEY -> its declaring node
    //A guest-root anchor a runner declares (GUEST_ROOTS {"%UserProfile%": ...}) is a canonical coordinate the launch
    //maps to where the runner puts it — not a variable, and never a typo.
    std::set<std::string> Anchors;
    std::map<std::string, std::string> AnchorGuest;                      // %Anchor% -> its guest path, lowercased, '/'
    for (const auto &[Id, N] : Idx.Nodes)
        for (const auto &L : (*N.Json)["LAYERS"])
            if (L.contains("EXEC") && L["EXEC"].is_array())
                for (const auto &E : L["EXEC"])
                    if (E.is_object() && E.contains("GUEST_ROOTS") && E["GUEST_ROOTS"].is_object())
                        for (const auto &[A, To] : E["GUEST_ROOTS"].items())
                            if (A.size() > 2 && A.front() == '%' && A.back() == '%')
                            {
                                Anchors.insert(A.substr(1, A.size() - 2));
                                std::string G = ToLowerAscii(To.is_string() ? To.get<std::string>() : std::string());
                                //A native guest's anchor is a folder of its runtime (%RuntimePath%/n64/…), that runner's
                                //own layout: nothing a package's spelling is checked against, and no disagreement.
                                if (G.rfind("%runtimepath%", 0) == 0) continue;
                                std::replace(G.begin(), G.end(), '\\', '/');
                                const auto [It, New] = AnchorGuest.emplace(A, G);
                                if (!New && It->second != G)       // validated against one spelling: say which differ
                                    Warnings.push_back("runners disagree where " + A + " is ('" + It->second + "' vs '" + G
                                                       + "', node '" + Id + "') — packages are checked against the first one");
                            }
    //A package names places only by anchor: a drive letter (C:/…, E:\) is where one runner happens to put things —
    //and a place has ONE spelling, its most specific anchor (%SysDir32%/x, never %Windows%/syswow64/x), because
    //addresses fold and own by their spelling. A runner's own GUEST_ROOTS/DRIVES are the map, not a use.
    {
        //a drive path (C:\x, E:/), or a value that is only a drive ("E:") — not a config key like "/W:"
        const std::regex Drive(R"((^|[^A-Za-z0-9])[A-Za-z]:[\\/]|^[A-Za-z]:$)");
        //an anchor and the path after it — up to the next token, so a second anchor in the same value is checked too
        const std::regex Anchored(R"(%([A-Za-z0-9_]+)%([\\/][^"%]*)?)");
        std::function<void(const nlohmann::ordered_json &, const std::string &, std::set<std::string> &)> Scan =
            [&](const nlohmann::ordered_json &J, const std::string &Where, std::set<std::string> &Out) {
                auto Check = [&](const std::string &S) {
                    //A whole file's text (an Overwrite seed) is quoted by its offending line alone, not its kilobytes.
                    if (std::smatch M; std::regex_search(S, M, Drive))
                    {
                        const size_t At = (size_t)M.position(0);
                        const size_t B = S.find_last_of("\r\n", At), E = S.find_first_of("\r\n", At);
                        const std::string Line = S.substr(B == std::string::npos ? 0 : B + 1, E == std::string::npos ? std::string::npos : E - (B == std::string::npos ? 0 : B + 1));
                        Out.insert(Where + ": '" + Line + "' names a drive — use an anchor (%GameDir%, %SysDir32%, %ProgramData%, …)");
                    }
                    for (auto It = std::sregex_iterator(S.begin(), S.end(), Anchored); It != std::sregex_iterator(); ++It)
                    {
                        const std::string A = "%" + (*It)[1].str() + "%";
                        const auto G = AnchorGuest.find(A);
                        if (G == AnchorGuest.end()) continue;
                        std::string Rest = ToLowerAscii((*It)[2].str());
                        std::replace(Rest.begin(), Rest.end(), '\\', '/');
                        const std::string Full = G->second + Rest;
                        for (const auto &[B, BG] : AnchorGuest)
                            if (B != A && BG.size() > G->second.size() && Full.compare(0, BG.size(), BG) == 0
                                && (Full.size() == BG.size() || Full[BG.size()] == '/'))
                                Out.insert(Where + ": '" + S + "' is inside " + B + " — write it from " + B);
                    }
                };
                if (J.is_string()) Check(J.get<std::string>());
                else if (J.is_array()) for (const auto &X : J) Scan(X, Where, Out);
                else if (J.is_object())
                    for (const auto &[K, V] : J.items())
                    {
                        if (K == "COMMENT") continue;                        // prose, not a place
                        Check(K);
                        Scan(V, Where, Out);
                    }
            };
        //In a PATH an anchor is where the path starts: "mods/%GameDir%/x" would substitute to "mods/C:\802/x" and mount
        //literally, silently. (A value may embed one anywhere: "RunDll32 %ProgramFiles32%\x.dll".)
        auto PathCheck = [&](const std::string &P, const std::string &Where, std::set<std::string> &Out) {
            for (const std::string &Name : Anchors)                      // every anchor, a native runner's too
            {
                const std::string A = "%" + Name + "%";
                for (size_t At = P.find(A); At != std::string::npos; At = P.find(A, At + 1))
                    if (At > 0) Out.insert(Where + ": path '" + P + "' has " + A + " inside it — an anchor starts a path");
            }
        };
        auto Paths = [&](const nlohmann::ordered_json &L, const std::string &Where, std::set<std::string> &Out) {
            auto Addr = [](const std::string &A) { const size_t S = A.find('/'); return S == std::string::npos ? std::string() : A.substr(S + 1); };
            if (L.contains("TARGET") && L["TARGET"].is_string()) PathCheck(Addr(L["TARGET"].get<std::string>()), Where, Out);
            //A TAKE pair's destination may carry the source's namespace or not (Fold::Taken adds it): the path is
            //what follows it — "FILES/%SysDir32%/x.dll" and "%SysDir32%/x.dll" name the same place.
            if (L.contains("TAKE") && L["TAKE"].is_array())
                for (const auto &T : L["TAKE"])
                    if (T.is_array() && T.size() == 2 && T[0].is_string() && T[1].is_string())
                    {
                        const std::string Src = T[0].get<std::string>(), Ns = Src.substr(0, Src.find('/')) + "/";
                        std::string Dst = T[1].get<std::string>();
                        if (Dst.compare(0, Ns.size(), Ns) == 0) Dst.erase(0, Ns.size());
                        PathCheck(Dst, Where, Out);
                    }
            if (L.contains("KEEP") && L["KEEP"].is_object())
                for (const auto &[K, V] : L["KEEP"].items()) PathCheck(Addr(K), Where, Out);
            if (L.contains("EXEC") && L["EXEC"].is_array())
                for (const auto &E : L["EXEC"])
                    for (const char *F : {"EXE", "WORKDIR"})
                        if (E.is_object() && E.contains(F) && E[F].is_string()) PathCheck(E[F].get<std::string>(), Where, Out);
        };
        for (const auto &[Id, N] : Idx.Nodes)
        {
            if (!InScope(Id)) continue;
            std::set<std::string> Found;
            for (nlohmann::ordered_json L : (*N.Json)["LAYERS"])
            {
                if (L.contains("EXEC") && L["EXEC"].is_array())
                    for (auto &E : L["EXEC"])
                        if (E.is_object()) { E.erase("GUEST_ROOTS"); E.erase("DRIVES"); }
                Scan(L, "node '" + Id + "'", Found);
                Paths(L, "node '" + Id + "'", Found);
            }
            Errors.insert(Errors.end(), Found.begin(), Found.end());
        }
    }
    for (const auto &[Id, N] : Idx.Nodes)
    {
        if (!N.LowerError.empty()) continue;
        for (const auto &L : (*N.Json)["LAYERS"])
            if (L.contains("VARS"))
                for (const auto &[K, D] : L["VARS"].items())
                {
                    Declared.insert(K);
                    if (!L.contains("WHEN")) DeclaredUngated.insert(K);
                    if (D.contains("UI")) UiDeclared.emplace(K, Id);
                    if (D.contains("MAP")) Tables.insert(K);
                }
    }
    //Two key options of one launch-window SECTION bound to the same key by default: one press, two actions.
    for (const auto &[Id, N] : Idx.Nodes)
    {
        if (!InScope(Id) || !N.LowerError.empty()) continue;
        std::map<std::pair<std::string, std::string>, std::string> Bound;    // (section, key) → the option holding it
        for (const auto &L : (*N.Json)["LAYERS"])
            if (L.contains("VARS"))
                for (const auto &[K, D] : L["VARS"].items())
                {
                    if (!D.contains("UI") || D["UI"].value("CONTROL", std::string()) != "key") continue;
                    const std::string Key = D.value("DEFAULT", std::string());
                    if (Key.empty()) continue;
                    const auto [It, New] = Bound.emplace(std::make_pair(D["UI"].value("SECTION", std::string()), Key), K);
                    if (!New)
                        Warnings.push_back("node '" + Id + "': options %" + It->second + "% and %" + K + "% are both bound to '" +
                                           Key + "' by default");
                }
    }
    //The %KEY%, %KEY:format% and %KEY:map:TABLE% tokens of every string in a node's layers, read pairwise as substitution
    //reads them, "%%" being a literal '%' (an installer's registry value named %%Value%% is no variable): each token as
    //(key, the table a :map: format names, or "" ).
    const auto NodeTokens = [](const nlohmann::ordered_json &J) {
        std::vector<std::pair<std::string, std::string>> Out;
        const auto Ident = [](const std::string &K) {
            return !K.empty() && std::all_of(K.begin(), K.end(), [](unsigned char C) { return std::isalnum(C) || C == '_'; });
        };
        const auto Scan = [&](const std::string &S) {
            for (size_t P = 0;;)
            {
                const size_t A = S.find('%', P);
                if (A == std::string::npos) break;
                const size_t B = S.find('%', A + 1);
                if (B == std::string::npos) break;
                const std::string T = S.substr(A + 1, B - A - 1);
                P = B + 1;
                const std::string K = T.substr(0, T.find(':'));
                if (!Ident(K)) continue;                                   // "%%", or a '%' that is no token
                const std::string F = T.size() > K.size() ? T.substr(K.size() + 1) : std::string();
                //A :map: format names its table (which may be empty: that, too, is a table nothing declares).
                Out.emplace_back(K, F.rfind("map:", 0) == 0 ? ":" + F.substr(4) : std::string());
            }
        };
        std::vector<const nlohmann::ordered_json *> Stack{ &J };
        while (!Stack.empty())
        {
            const nlohmann::ordered_json *V = Stack.back();
            Stack.pop_back();
            if (V->is_string()) Scan(V->get<std::string>());
            else if (V->is_object()) for (const auto &[K, C] : V->items()) { Scan(K); Stack.push_back(&C); }
            else if (V->is_array()) for (const auto &C : *V) Stack.push_back(&C);
        }
        return Out;
    };
    std::set<std::string> ReferencedAll;
    for (const auto &[Id, N] : Idx.Nodes)
    {
        if (!N.LowerError.empty()) continue;
        const std::string Tag = "node '" + Id + "'";
        for (const auto &L : (*N.Json)["LAYERS"])
        {
            std::vector<std::string> Conds;
            if (L.contains("WHEN")) Conds.push_back(L["WHEN"].get<std::string>());
            if (L.contains("VARS")) for (const auto &[K, D] : L["VARS"].items()) if (D.contains("WHEN")) Conds.push_back(D["WHEN"].get<std::string>());
            for (const auto &C : Conds)
                if (!VarSubst::ConditionParses(C) && InScope(Id))
                    Errors.push_back(Tag + ": malformed WHEN \"" + C + "\" — it would silently always-apply. Grammar: %KEY% == value, && || ! ( ).");
            if (L.contains("WHEN") && InScope(Id))
            {
                const std::string W = L["WHEN"].get<std::string>();
                for (const auto &[K, Table] : NodeTokens(nlohmann::ordered_json(W)))
                {
                    if (!Builtins.count(K) && Declared.count(K) && !DeclaredUngated.count(K))
                        Errors.push_back(Tag + ": a layer WHEN reads %" + K + "%, which is only declared in WHEN-gated "
                                         "layers — a layer condition may only read ungated variables (§4.1)");
                }
            }
        }
        //Undefined %KEY% references (typos) — over everything the node writes and runs. Not its prose: a COMMENT
        //(on a layer or an op) and a layer's LABEL/SECTION are never rendered, so a '%' in them references nothing.
        nlohmann::ordered_json Live = (*N.Json)["LAYERS"];
        for (auto &L : Live)
        {
            if (!L.is_object()) continue;
            L.erase("COMMENT"); L.erase("LABEL"); L.erase("SECTION");
            if (L.contains("EDIT") && L["EDIT"].is_array())
                for (auto &Op : L["EDIT"]) if (Op.is_object()) Op.erase("COMMENT");
        }
        std::set<std::string> Refs;
        for (const auto &[K, Table] : NodeTokens(Live))
        {
            Refs.insert(K);
            //A %KEY:map:TABLE% reading a table nothing declares would pass every value through unchanged, silently.
            if (!Table.empty() && !Tables.count(Table.substr(1)) && InScope(Id))
                Errors.push_back(Tag + ": %" + K + ":map:" + Table.substr(1) + "% reads the table '" + Table.substr(1) +
                                 "', but no VARS declares a MAP by that name (typo?)");
        }
        for (const std::string &R : Refs)
        {
            ReferencedAll.insert(R);
            if (!Builtins.count(R) && !Declared.count(R) && !Anchors.count(R) && InScope(Id))
            {
                //With no runner here, no place (%GameDir%, %Documents%...) is defined at all: say that, not "typo?".
                static const std::set<std::string> Places = {"GameDir", "Documents", "AppData", "LocalAppData", "UserProfile", "ProgramData",
                                                             "ProgramFiles", "ProgramFiles32", "Windows", "SysDir32", "SysDir64", "SavedGames"};
                Errors.push_back(Tag + (Anchors.empty() && Places.count(R) ? ": uses %" + R + "%, a place runners define - no runner is installed on this machine yet"
                                                                         : ": references undefined variable %" + R + "% — no VARS declares it (typo?)"));
            }
        }
    }
    if (!OnlyNodes)
        for (const auto &[K, Owner] : UiDeclared)
            if (!ReferencedAll.count(K) && !Idx.Find(Owner)->Json->dump().empty())
            {
                //An option read by nothing is a dead knob — unless it is a bool option gating a layer (its reader is
                //that WHEN, which the scan above already counts).
                Warnings.push_back("node '" + Owner + "': option %" + K + "% has a UI but is referenced nowhere (dead knob)");
            }

    //----- KEEP lint: what the user owns persists under a NAMED dir beside the instance's own state. -----
    for (const auto &[Id, N] : Idx.Nodes)
    {
        if (!InScope(Id) || !N.LowerError.empty()) continue;
        const std::string Tag = "node '" + Id + "'";
        std::map<std::string, std::string> Stored;   // durable name (lower) -> the address stored under it
        for (const auto &L : N.LoweredLayers())
        {
            if (ManifestModel::LayerType(L) != "DeclarePersist") continue;
            const std::string Path = L.value("PATH", std::string());
            if (L.value("SCOPE", std::string()) == "registry" && RegistryWrapper::NamesNoKey(Path))
                Errors.push_back(Tag + ": a KEEP of a whole registry hive ('" + Path + "') — only keys persist; name them "
                                       "(e.g. REG/HKCU/Software/<Vendor>/<Game>)");
            else if (Path.empty())
                Warnings.push_back(Tag + ": a KEEP of a whole namespace persists ALL of it — prefer named addresses");
            if (L.value("SCOPE", std::string()) == "file")
            {
                //The durable name: NAME if given, else the address's last segment (as DerivePersistence defaults it).
                std::string Name = L.contains("TARGET") && L["TARGET"].is_string() ? L["TARGET"].get<std::string>() : Path;
                while (!Name.empty() && (Name.back() == '/' || Name.back() == '\\')) Name.pop_back();
                if (const auto Slash = Name.find_last_of("/\\"); Slash != std::string::npos) Name = Name.substr(Slash + 1);
                const std::string T = ToLowerAscii(Name);
                if (T == "instance.json" || T == "registry" || T == "regkeys")
                    Warnings.push_back(Tag + ": KEEP NAME '" + Name + "' is RESERVED for the instance's own state — it "
                                       "will be refused at launch" + (L.contains("TARGET") ? "" : " (give the KEEP a NAME)"));
                //Two addresses stored under one name would share (and clobber) one folder: the launch keeps only the
                //first, so the other's saves silently vanish (a game and its expansion both keeping "Saved Games/").
                else if (const auto [It, New] = Stored.emplace(T, Path); !New && It->second != Path)
                    Errors.push_back(Tag + ": KEEP '" + Path + "' is stored as '" + Name + "' like '" + It->second +
                                     "', which takes the name first - it would not be kept; give it its own NAME");
            }
        }
    }    for (std::string &W : Shelf(Idx, OnlyNodes)) Warnings.push_back(std::move(W));
}




// ALL entries of a zip including explicit directory entries (trailing '/' kept). ZipEntriesCached drops dirs —
// correct for content checks, but the case rewrite must canonicalize dir entries too: a renamed file leaves its
// old-case parent dir entry behind as an EMPTY husk ("maps/" next to "MAPS/…"), and wine's case-insensitive
// lookup can resolve into the husk (Halo CE: "missing bitmaps" — maps/ empty, everything under MAPS/).
static const std::vector<std::string> &ZipAllEntriesRaw(const std::string &Path,
        std::unordered_map<std::string, std::vector<std::string>> &Cache)
{
    auto It = Cache.find(Path);
    if (It != Cache.end()) return It->second;
    std::vector<std::string> Out;
    int Err = 0;
    if (zip_t *Za = zip_open(Path.c_str(), ZIP_RDONLY, &Err))
    {
        const zip_int64_t N = zip_get_num_entries(Za, 0);
        for (zip_uint64_t i = 0; i < (zip_uint64_t)N; ++i)
            if (const char *Name = zip_get_name(Za, i, ZIP_FL_ENC_RAW)) Out.emplace_back(Name);
        zip_close(Za);
    }
    return Cache.emplace(Path, std::move(Out)).first->second;
}

// Component-wise canonicalization across every launchable closure → per-zip {oldEntry -> newEntry}.
static std::map<std::string, std::map<std::string, std::string>>
ComputeCaseRenames(const NodeIndex &Idx, std::vector<std::string> &Log, const std::string &Scope = std::string())
{
    std::map<std::string, std::map<std::string, std::string>> Renames;   // zipPath -> (old -> new)
    //A file some delta in the library names is a base: its bytes must stay as they are, or the delta rebuilds garbage
    //(and its SOURCE would lie). Every delta, not only this scope's: a base is named from anywhere.
    std::set<std::string> Named;
    for (const auto &[Id, N] : Idx.Nodes)
        for (const auto &L : N.LoweredLayers())
            for (const std::string &C : ManifestModel::LayerBaseSources(L)) Named.insert(C);
    TreeNames Tn;
    const Fold::Library Lib = LibraryOf(Idx);
    for (const auto &[LId, LN] : Idx.Nodes)
    {
        if (!LN.IsVariant()) continue;
        //Scoped to one package: only its own variants can rename its zips (another package's variant reaching them
        //renames toward that package's layers, which the scope leaves alone anyway). Walking every variant of the
        //library read every zip it holds - minutes, gigabytes - to fix one package.
        if (!Scope.empty())
        {
            std::error_code Ec;
            const std::string Dir = std::filesystem::weakly_canonical(LN.BundleDir, Ec).string();
            const std::string D = Ec ? LN.BundleDir.string() : Dir;
            if (D != Scope && D.rfind(Scope + "/", 0) != 0) continue;
        }
        //The tree as the mount makes it spells each folder and name its lowest layer's way; a later layer spelling one
        //otherwise is renamed toward it, through the mount that put it there. A delta cannot be renamed, only rebuilt;
        //a folder of loose files is left alone.
        const LaunchTree L = LaunchTree::Of(Fold::Resolve(Lib, LId));
        const EntriesFn Archives = [&](size_t I) -> const std::vector<std::string> * {
            return L.S.layers[I].type == ::LayerType::Dir ? nullptr : Tn.Names.Of(L, I, true);
        };
        for (const auto &[I, M] : CaseRenames(L.S, L.T, Archives))
        {
            const LayerSpec &X = L.S.layers[I];
            if (X.type == ::LayerType::Delta)
            {
                for (const auto &[Entry, To] : M)
                    Log.push_back("case-fix: '" + Entry + "' inside the delta " + X.source
                                  + " is spelled unlike the layers beneath it — a delta cannot be renamed; rebuild it (skipped)");
                continue;
            }
            if (!X.cid.empty() && Named.count(X.cid))
            {
                Log.push_back("case-fix: " + X.source + " is a delta's base — rewriting it would break the delta (skipped)");
                continue;
            }
            auto &Z = Renames[X.source];
            for (const auto &[Entry, To] : M)
            {
                const auto Ex = Z.find(Entry);
                if (Ex != Z.end() && Ex->second != To)
                    Log.push_back("case-fix: conflicting canonicalization of '" + Entry + "' in " + X.source + " (skipped)");
                else
                    Z[Entry] = To;
            }
        }
    }
    return Renames;
}

// Rewrite one zip applying entry renames — libzip copies each entry's data (STORE, no re-compress) into a temp
// archive under the new name, preserving structure/attrs/mtime, then atomically replaces the original.
static bool ApplyZipRenames(const std::string &ZipPath, const std::map<std::string, std::string> &Renames,
                            std::vector<std::string> &Log)
{
    int Err = 0;
    zip_t *Src = zip_open(ZipPath.c_str(), ZIP_RDONLY, &Err);
    if (!Src) { Log.push_back("case-fix: cannot open " + ZipPath); return false; }
    const std::string Tmp = ZipPath + ".casefix.tmp";
    zip_t *Dst = zip_open(Tmp.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &Err);
    if (!Dst) { zip_close(Src); Log.push_back("case-fix: cannot create " + Tmp); return false; }

    const zip_int64_t N = zip_get_num_entries(Src, 0);
    bool Ok = true;
    for (zip_int64_t i = 0; i < N && Ok; ++i)
    {
        const char *Name = zip_get_name(Src, (zip_uint64_t)i, 0);
        if (!Name) { Ok = false; break; }
        std::string Nm = Name;
        auto It = Renames.find(Nm);
        std::string NewNm = (It != Renames.end()) ? It->second : Nm;

        if (!Nm.empty() && Nm.back() == '/')   // explicit directory entry
        {
            std::string D = NewNm; if (!D.empty() && D.back() == '/') D.pop_back();
            // Canonicalization can map several case-variant dir entries onto ONE spelling — the collapse is the
            // point (the husk merges into the real dir), so "already exists" is success, not failure.
            if (zip_dir_add(Dst, D.c_str(), ZIP_FL_ENC_UTF_8) < 0
                && zip_error_code_zip(zip_get_error(Dst)) != ZIP_ER_EXISTS) Ok = false;
            continue;
        }
        zip_source_t *S = zip_source_zip_file(Dst, Src, (zip_uint64_t)i, 0, 0, -1, nullptr);
        if (!S) { Ok = false; break; }
        zip_int64_t Ix = zip_file_add(Dst, NewNm.c_str(), S, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE);
        if (Ix < 0) { zip_source_free(S); Ok = false; break; }
        zip_set_file_compression(Dst, (zip_uint64_t)Ix, ZIP_CM_STORE, 0);   // keep STORE (FUSE needs it)
        zip_uint8_t Ops = 0; zip_uint32_t Attr = 0;
        if (zip_file_get_external_attributes(Src, (zip_uint64_t)i, 0, &Ops, &Attr) == 0)
            zip_file_set_external_attributes(Dst, (zip_uint64_t)Ix, 0, Ops, Attr);
        zip_stat_t St; zip_stat_init(&St);
        if (zip_stat_index(Src, (zip_uint64_t)i, 0, &St) == 0 && (St.valid & ZIP_STAT_MTIME))
            zip_file_set_mtime(Dst, (zip_uint64_t)Ix, St.mtime, 0);
    }

    std::error_code Ec;
    if (!Ok) { zip_discard(Dst); zip_close(Src); std::filesystem::remove(Tmp, Ec); Log.push_back("case-fix: rewrite failed " + ZipPath); return false; }
    if (zip_close(Dst) < 0) { zip_close(Src); std::filesystem::remove(Tmp, Ec); Log.push_back("case-fix: finalize failed " + ZipPath); return false; }
    zip_close(Src);
    std::filesystem::rename(Tmp, ZipPath, Ec);   // atomic replace
    if (Ec) { Log.push_back("case-fix: replace failed " + ZipPath); return false; }
    return true;
}

int FixCaseConflicts(const NodeIndex &Idx, std::vector<std::string> &Log, const std::filesystem::path *ScopeDir)
{
    std::string Scope;
    if (ScopeDir) { std::error_code Ec; Scope = std::filesystem::weakly_canonical(*ScopeDir, Ec).string(); if (Ec) Scope = ScopeDir->string(); }
    auto Renames = ComputeCaseRenames(Idx, Log, Scope);
    int Fixed = 0;
    for (const auto &[ZipPath, M] : Renames)
    {
        if (M.empty()) continue;
        if (!Scope.empty() && ZipPath.rfind(Scope, 0) != 0) continue;   // outside the requested bundle → leave it
        Log.push_back("case-fix: " + std::filesystem::path(ZipPath).filename().string()
                      + " — " + std::to_string(M.size()) + (M.size() == 1 ? " entry" : " entries"));
        for (const auto &[O, Nw] : M) Log.push_back("    " + O + "  ->  " + Nw);
        if (ApplyZipRenames(ZipPath, M, Log)) ++Fixed;
    }
    return Fixed;
}


} // namespace Validate
