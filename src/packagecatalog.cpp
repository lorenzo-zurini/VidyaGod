#include "packagecatalog.h"
#include "packagecatalog_p.h"
#include "apppaths.h"
#include "instancestore.h"
#include "manifestmodel.h"
#include "nodegraph.h"        // gigagraph catalog: GatherWorkingTree / LiftLibraryItemEdge / FreezeToIndex
#include "commonutils.h"
#include "jsonoperations.h"
#include "ipfswrapper.h"
#include "downloadqueue.h"      // EnqueueBatch/WaitBatch — closure completion rides the ONE rolling queue
#include "runnerwrapper.h"
#include "containerwrapper.h"   // RunnerNodeImported (runner install state) — .cpp-only include avoids a header cycle
#include "varsubst.h"           // %KEY% substitution — resolve CustomVar-templated content-layer PATHs for hydration
#include "launchresolver.h"     // ResolveChainIds — pool a game's resolved runner chain into its hydrate (full-closure)
#include "runnerinstall.h"      // CollectRunnerNodeTargets — a runner node's build download targets
#include "launchparams.h"       // ContainerParams (minimal, for the chain resolve)

#include <QDir>
#include <QFile>
#include <QStringList>
#include <cstdint>
#include <map>
#include <set>
#include <deque>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <vector>

using namespace ManifestModel;   // LayerLocator / IsVfsLayer / ForEachVfsLayer / PackageHydrated / PackageIpfsCids

namespace PackageCatalog {

// ----- per-package user settings -----

// Per-package config now lives in the INSTANCE file (<root>/USERDATA/<uid>/<instance>/instance.json), NOT in
// GlobalConfig — see InstanceStore. The GlobalConfigJSON arg is kept ONLY to locate the USERDATA root
// (Settings.Paths.UserDataRoot); it is no longer read from / written to for these keys. Instance="" ⇒ the active
// instance (a WRITE creates DefaultInstance if the game has none; a READ never creates one).
nlohmann::ordered_json GetPackageUserSettings(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &PackageUID,
                                              const std::string &Instance)
{
    return InstanceStore::ReadConfig(GlobalConfigJSON, PackageUID, Instance);
}

nlohmann::ordered_json GetPackageVariables(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &PackageUID,
                                           const std::string &Instance)
{
    const nlohmann::ordered_json US = InstanceStore::ReadConfig(GlobalConfigJSON, PackageUID, Instance);
    if (US.contains("VARIABLES") && US["VARIABLES"].is_object()) return US["VARIABLES"];
    return nlohmann::ordered_json::object();
}

void MergePackageVariables(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &PackageUID,
                           const std::map<std::string, std::string> &NewVars, const std::string &Instance)
{
    if (NewVars.empty()) return;
    nlohmann::ordered_json Vars = GetPackageVariables(GlobalConfigJSON, PackageUID, Instance);
    for (const auto &[K, V] : NewVars) Vars[K] = V;
    SetPackageUserSetting(GlobalConfigJSON, PackageUID, "VARIABLES", Vars, Instance);
}

void SetPackageUserSetting(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &PackageUID,
                           const std::string &Key, const nlohmann::ordered_json &Value, const std::string &Instance)
{
    nlohmann::ordered_json C = InstanceStore::ReadConfig(GlobalConfigJSON, PackageUID, Instance);
    C[Key] = Value;
    std::string Err;
    if (!InstanceStore::WriteConfig(GlobalConfigJSON, PackageUID, Instance, C, &Err))
        LogErr("PackageCatalog::SetPackageUserSetting", "could not persist " + Key + " for " + PackageUID + " (" + Err + ")");
}

// ----- on-disk locations -----

//The managed library root — repos clone here (one subfolder per repo); every package hydrates in place beside its
//manifest. Overridable via Settings.Paths.LibraryRoot; default ~/.VidyaGod/LIBRARY.
static std::string LibraryDir(const nlohmann::ordered_json &GlobalConfigJSON)
{
    if (GlobalConfigJSON.contains("Settings") && GlobalConfigJSON["Settings"].is_object())
    {
        const auto &S = GlobalConfigJSON["Settings"];
        if (S.contains("Paths") && S["Paths"].is_object() && S["Paths"].contains("LibraryRoot")
            && S["Paths"]["LibraryRoot"].is_string() && !std::string(S["Paths"]["LibraryRoot"]).empty())
            return QDir::cleanPath(QString::fromStdString(std::string(S["Paths"]["LibraryRoot"]))).toStdString();
    }
    return QDir::cleanPath(QString::fromStdString((AppPaths::DataRoot() / "LIBRARY").string())).toStdString();
}

std::string LibraryRootDir(const nlohmann::ordered_json &GlobalConfigJSON) { return LibraryDir(GlobalConfigJSON); }

// CATALOG and ASSETS are top-level SIBLINGS of LIBRARY (beside it, whether LibraryRoot is default or overridden):
//   LIBRARY/ = what you've installed (hydrated, published, seeded)
//   CATALOG/ = received browse stubs (pinned node blocks, NOT published) — <Nick> - <Lib>/<pkg>/<node>.json
//   ASSETS/  = content-addressed shared files (covers) — <cid>, one per CID, shared by every tile that references it
// The catalog scan spans LIBRARY + CATALOG (merged index resolves a LIBRARY game's dep even when the dep is a
// CATALOG stub); publish scans LIBRARY only.
std::string CatalogRootDir(const nlohmann::ordered_json &GlobalConfigJSON)
{
    return QDir::cleanPath(QString::fromStdString(
        (std::filesystem::path(LibraryDir(GlobalConfigJSON)).parent_path() / "CATALOG").string())).toStdString();
}
std::string AssetsRootDir(const nlohmann::ordered_json &GlobalConfigJSON)
{
    return QDir::cleanPath(QString::fromStdString(
        (std::filesystem::path(LibraryDir(GlobalConfigJSON)).parent_path() / "ASSETS").string())).toStdString();
}

// Pure path-prefix test: is Path inside Base? (weakly-canonical so it works whether or not either exists yet.)
static bool PathUnder(const std::filesystem::path &Base, const std::filesystem::path &Path)
{
    std::error_code Ec;
    const std::filesystem::path B = std::filesystem::weakly_canonical(Base, Ec);
    const std::filesystem::path P = std::filesystem::weakly_canonical(Path, Ec);
    auto It = std::mismatch(B.begin(), B.end(), P.begin(), P.end());
    return It.first == B.end();                                     // B is a prefix of P
}

// ----- package sources (IPFS folder CIDs) -----
//A source's CID (object {CID,NAME} or a bare CID string) and its on-disk name (NAME, else a filesystem-safe CID prefix).
std::string PackageSourceCID(const nlohmann::ordered_json &S)
{
    if (S.is_object() && S.contains("CID") && S["CID"].is_string()) return std::string(S["CID"]);
    if (S.is_string()) return std::string(S);
    return std::string();
}
// An IPNS-name source (a friend, or a manually-added /ipns/ address) resolves to a mutable top-level index rather than
// being a static folder CID: an explicit IPNS/FRIEND flag, or a CID field carrying the /ipns/ prefix.
bool IsIpnsSource(const nlohmann::ordered_json &S)
{
    if (!S.is_object()) return false;
    if (S.value("IPNS", false) || S.value("FRIEND", false)) return true;
    return PackageSourceCID(S).rfind("/ipns/", 0) == 0;
}
static std::string PackageSourceName(const nlohmann::ordered_json &S)
{
    std::string N = S.is_object() ? S.value("NAME", std::string()) : std::string();
    if (N.empty()) { const std::string C = PackageSourceCID(S); N = C.size() > 12 ? C.substr(0, 12) : C; }
    for (char &c : N) if (c == '/' || c == '\\' || c == ':') c = '_';
    return N.empty() ? std::string("cidsource") : N;
}
//CID package sources ARE the library now (git is gone): each fetches into LIBRARY/<name>. "Under a package source" =
//"under the LIBRARY root". Local (externally-added) packages live OUTSIDE LIBRARY, so they stay distinguishable.
static std::string PackageSourcesRoot(const nlohmann::ordered_json &GlobalConfigJSON)
{
    return LibraryDir(GlobalConfigJSON);
}
// The on-disk DIR SEGMENT for a source — distinct from the display NAME. For an IPNS/FRIEND source the display name is
// the friend's NICK, which is ATTACKER-CHOSEN: keying the dir by it lets a friend name themselves "Games" (or an
// official collection) and hijack/delete your own collection dir on subscribe/unsubscribe/mirror. So an IPNS source's
// dir is derived from its CID (the /ipns/<peerID> — unique per peer, not attacker-meaningful) under a reserved
// "_friend_" prefix that a real collection name can never produce. Static folder-CID sources keep their NAME.
static std::string PackageSourceDirSegment(const nlohmann::ordered_json &S)
{
    // A friend-share source (scheme "friend:<peerID>:<lib>"): derive the dir from the peer ID + library — NEVER the
    // attacker-chosen nick — under the reserved "_friend_" prefix a real collection name can't produce. The library
    // name is attacker-chosen, so HEX-encode it: two lib names that would sanitize to the same string must not collide
    // into one dir (which would make each library's prune rm the other's packages).
    {
        const std::string Fc = PackageSourceCID(S);
        if (Fc.rfind("friend:", 0) == 0)
        {
            const std::string Rest = Fc.substr(7);
            const std::string::size_type Colon = Rest.find(':');
            const std::string Peer = (Colon == std::string::npos) ? Rest : Rest.substr(0, Colon);
            const std::string Lib  = (Colon == std::string::npos) ? std::string() : Rest.substr(Colon + 1);
            std::string PeerSan;
            for (char c : Peer) PeerSan.push_back((std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.') ? c : '_');
            if (PeerSan.empty()) PeerSan = "peer";
            // Hash the (attacker-chosen, up-to-256-byte) lib name to a fixed 16-hex tail: unambiguous (no sanitize
            // collision) AND bounded (a raw hex-encode of a long lib name could overflow NAME_MAX).
            std::uint64_t H = 1469598103934665603ULL;                 // FNV-1a/64
            for (unsigned char c : Lib) { H ^= (std::uint64_t)c; H *= 1099511628211ULL; }
            static const char *Hx = "0123456789abcdef";
            std::string Tail(16, '0');
            for (int k = 15; k >= 0; --k) { Tail[k] = Hx[H & 0xFULL]; H >>= 4; }
            return "_friend_" + PeerSan + "_" + Tail;
        }
    }
    if (IsIpnsSource(S))
    {
        std::string Peer = PackageSourceCID(S);
        if (Peer.rfind("/ipns/", 0) == 0) Peer = Peer.substr(6);
        std::string San;
        for (char c : Peer) San.push_back((std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.') ? c : '_');
        if (San.empty()) San = "peer";
        return "_friend_" + San;
    }
    return PackageSourceName(S);
}
std::string PackageSourceDir(const nlohmann::ordered_json &GlobalConfigJSON, const nlohmann::ordered_json &S)
{
    return QDir::cleanPath(QString::fromStdString(PackageSourcesRoot(GlobalConfigJSON) + "/" + PackageSourceDirSegment(S))).toStdString();
}

bool IsPackageSourcePath(const nlohmann::ordered_json &GlobalConfigJSON, const std::filesystem::path &BundleDir)
{
    return !BundleDir.empty() && PathUnder(PackageSourcesRoot(GlobalConfigJSON), BundleDir);
}

//A managed package: sourced from a CID package source (i.e. NOT a standalone locally-added bundle). (Git repos were
//removed — every shared package now comes from a Settings.PackageSources[] CID, so "managed" == "under a source dir".)
static bool PathUnderManagedSource(const nlohmann::ordered_json &GlobalConfigJSON, const std::filesystem::path &Path)
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

bool IsLocalPackagePath(const nlohmann::ordered_json &GlobalConfigJSON, const std::filesystem::path &BundleDir)
{
    return !BundleDir.empty() && !PathUnderManagedSource(GlobalConfigJSON, BundleDir);
}

std::vector<std::filesystem::path> LocalPackageDirs(const nlohmann::ordered_json &GlobalConfigJSON)
{
    std::vector<std::filesystem::path> Out;
    if (!GlobalConfigJSON.contains("LIBRARY") || !GlobalConfigJSON["LIBRARY"].is_array()) return Out;
    std::error_code Ec;
    for (const auto &E : GlobalConfigJSON["LIBRARY"])
    {
        const std::string Path = E.is_object() ? E.value("PATH", std::string()) : std::string();
        if (Path.empty()) continue;
        if (PathUnderManagedSource(GlobalConfigJSON, Path)) continue;   // repo / CID-source package — indexed via its root
        if (std::filesystem::is_directory(Path, Ec)) Out.emplace_back(Path);
    }
    return Out;
}

int PruneMovedLocalPackages(nlohmann::ordered_json &GlobalConfigJSON)
{
    if (!GlobalConfigJSON.contains("LIBRARY") || !GlobalConfigJSON["LIBRARY"].is_array()) return 0;
    auto &Lib = GlobalConfigJSON["LIBRARY"];
    std::error_code Ec;
    int Removed = 0;
    for (auto It = Lib.begin(); It != Lib.end(); )
    {
        const std::string Path = It->is_object() ? It->value("PATH", std::string()) : std::string();
        const bool Local = !Path.empty() && !PathUnderManagedSource(GlobalConfigJSON, Path);
        if (Local && !std::filesystem::is_directory(Path, Ec))
        {
            LogWarn("PackageCatalog::PruneMovedLocalPackages",
                    "Dropping moved/deleted local package '" + It->value("PACKAGENAME", Path) + "' (" + Path + ").");
            It = Lib.erase(It); ++Removed;
        }
        else ++It;
    }
    return Removed;
}

//A bundle's index identity, derived from its node files (everything-is-a-node): the representative launchable
//node's UID + title, and whether the bundle defines any launchable / runner node. Replaces the AssembleManifest
//PACKAGEUID/PACKAGENAME/HasGames/HasRunners reads.
BundleIdentity ScanBundleIdentity(const std::string &BundleDir)   // decl + BundleIdentity: packagecatalog_p.h
{
    BundleIdentity Id;
    NodeIndex Idx;
    ManifestModel::ScanBundleNodes(BundleDir, Idx);
    ManifestModel::LinkGames(Idx);   // a launchable variant inherits its tile's UID/TITLE (DeclareExec + DeclareLibraryItem
                                     // may live on separate nodes); without this, such a bundle is mislabelled by its exec node-id
    const Node *Rep = nullptr;                                                    // prefer a presentable launchable
    for (const auto &[NodeId, N] : Idx.Nodes)
    {
        if (N.IsLaunchable()) { Id.HasLaunchable = true; if (!Rep || (N.Presentable() && !Rep->Presentable())) Rep = &N; }
        if (N.IsRunner())     Id.HasRunner = true;
    }
    if (!Rep)                                                                     // runner-only / content-only bundle
        for (const auto &[NodeId, N] : Idx.Nodes) { if (!N.Uid.empty()) { Rep = &N; break; } }
    if (!Rep && !Idx.Nodes.empty()) Rep = &Idx.Nodes.begin()->second;             // no UID anywhere (runners) → use NODE_ID
    if (Rep)
    {
        Id.Uid  = Rep->Uid.empty() ? Rep->NodeId : Rep->Uid;
        Id.Name = Rep->Meta.is_object() ? Rep->Meta.value("TITLE", Rep->NodeId) : Rep->NodeId;
        Id.Valid = !Id.Uid.empty();
    }
    return Id;
}

//Upsert a LIBRARY entry for a static CID-source package: PATH + name + CIDSOURCE (its source's CID). Removal is
//explicit via RemovePackageSource (an immutable source's contents never change, so there is no reconcile/auto-prune).
//SKIPS entries tagged with a "SOURCE" (an IPNS/friend mirror owns those, keyed by PATH) so a friend package whose
//UID collides with a static one can never repoint — or be repointed by — the static entry.
static void UpsertCidEntry(nlohmann::ordered_json &Arr, const std::string &Uid, const std::string &SourceCid,
                           const std::string &Dir, const std::string &Name)
{
    for (auto &E : Arr)
    {
        if (E.value("PACKAGEUID", std::string()) != Uid) continue;
        if (E.contains("SOURCE")) continue;   // an IPNS-mirror entry — not this (static) upsert's to touch
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
        if (PackageSourceCID(Src).rfind("friend:", 0) == 0) continue;   // friend-share sources are disk-only, never fetched
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

// The CIDSOURCE of the mirror entry AT this exact PkgDir ("" if none). The mirror is keyed BY PATH (its own dir),
// never by bare UID, so a friend's package can neither repoint nor be repointed by an entry from another source even
// on a UID collision (the static UpsertCidEntry skips SOURCE-tagged entries; this only ever touches its own).
static std::string MirrorCidForPath(const nlohmann::ordered_json &Library, const std::string &PkgDir)
{
    if (Library.is_array())
        for (const auto &E : Library)
            if (E.is_object() && E.value("PATH", std::string()) == PkgDir) return E.value("CIDSOURCE", std::string());
    return {};
}
// Upsert a mirror entry keyed by PATH, tagged with SOURCE (so the static upsert leaves it alone).
static void UpsertMirrorEntry(nlohmann::ordered_json &Arr, const std::string &PkgDir, const std::string &Uid,
                              const std::string &Cid, const std::string &Name, const std::string &SourceKey)
{
    for (auto &E : Arr)
        if (E.is_object() && E.value("PATH", std::string()) == PkgDir)
        { E["PACKAGEUID"] = Uid; E["PACKAGENAME"] = Name; E["CIDSOURCE"] = Cid; E["SOURCE"] = SourceKey; return; }
    Arr.push_back(nlohmann::ordered_json{{"PACKAGEUID", Uid}, {"PACKAGENAME", Name}, {"PATH", PkgDir},
                                         {"CIDSOURCE", Cid}, {"SOURCE", SourceKey}});
}

// A hostile /ipns/ source is untrusted input: bound the index doc and its entry count so it cannot OOM the parser or
// spawn an unbounded fetch/dir loop on every sync.
static constexpr long long kMaxIpnsIndexBytes = 8LL * 1024 * 1024;   // a text index of thousands of pkgs is ≪ this
static constexpr int       kMaxIpnsPackages   = 20000;               // across all of one source's libraries

// Subscribe path for an IPNS-name source (a friend / a manual /ipns/ address): resolve the name → the ONE inline
// index doc → mirror each package's dehydrated bundle (its OWN per-package meta-CID) into the source dir, PATH-scoped
// and SOURCE-tagged. A package whose CID is unchanged is left in place (the index diff); a changed CID is fetched to
// STAGING and swapped only on success (never destroy the current copy before the new bytes are in hand). Packages that
// vanished from the index are pruned (entry + dir). Covers stay lazy (their content CIDs travel in the JSON). Returns
// the number of LAUNCHABLE packages indexed.
static int MirrorIpnsSource(nlohmann::ordered_json &GlobalConfigJSON, const nlohmann::ordered_json &Src, std::string *Error)
{
    const std::string Name = PackageSourceCID(Src);
    if (Name.empty()) return 0;
    std::string RErr;
    const std::string TopCid = IpfsWrapper::IpnsResolve(Name, &RErr);
    if (TopCid.empty())
    {
        LogErr("PackageCatalog::SyncPackageSources", "IPNS resolve of " + Name + " failed: " + RErr);
        if (Error && Error->empty()) *Error = RErr;
        return 0;   // leave whatever is already mirrored in place — a resolve failure never destroys prior state
    }

    const std::string BaseDir   = PackageSourceDir(GlobalConfigJSON, Src);
    const std::string SourceKey = std::filesystem::path(BaseDir).filename().string();   // the SOURCE tag (per-source)
    std::error_code Ec;
    std::filesystem::create_directories(BaseDir, Ec);

    // A single safe path segment: alnum/._-/space only, no separators, no "."/".." — so a hostile packageUID or
    // library name can never traverse out of the source dir.
    auto SanSeg = [](const std::string &S) {
        std::string O;
        for (unsigned char C : S)
            O.push_back((std::isalnum(C) || C == '.' || C == '_' || C == '-' || C == ' ') ? static_cast<char>(C) : '_');
        while (!O.empty() && (O.back() == '.' || O.back() == ' ')) O.pop_back();
        return (O.empty() || O == "." || O == "..") ? std::string("_") : O;
    };

    // Refuse an oversized index BEFORE fetching it (CidSize resolves over the network) so a hostile source can't fill
    // the disk during the fetch; then fetch, size-check the file too, and parse.
    if (const long long Sz = IpfsWrapper::CidSize(TopCid); Sz > kMaxIpnsIndexBytes)
    { LogErr("PackageCatalog::SyncPackageSources", "IPNS index " + TopCid + " is " + std::to_string(Sz)
             + " bytes — refusing (max " + std::to_string(kMaxIpnsIndexBytes) + ")"); return 0; }

    nlohmann::ordered_json Top;
    {
        const std::string Tmp = BaseDir + "/.ipnsfetch.json";
        std::string FErr;
        if (IpfsWrapper::FetchToPath(TopCid, Tmp, &FErr).empty())
        { LogErr("PackageCatalog::SyncPackageSources", "fetch IPNS index " + TopCid + ": " + FErr); return 0; }
        const auto FileSz = std::filesystem::file_size(Tmp, Ec);
        if (!Ec && (long long)FileSz > kMaxIpnsIndexBytes)
        { std::error_code Re; std::filesystem::remove(Tmp, Re);
          LogErr("PackageCatalog::SyncPackageSources", "IPNS index doc exceeds the size cap — refusing"); return 0; }
        bool Ok = false;
        { std::ifstream In(Tmp); try { In >> Top; Ok = true; } catch (const std::exception &E)
            { LogErr("PackageCatalog::SyncPackageSources", "bad IPNS index JSON: " + std::string(E.what())); } }
        std::error_code Re; std::filesystem::remove(Tmp, Re);
        if (!Ok || !Top.is_object() || !Top["libraries"].is_array())
        { LogErr("PackageCatalog::SyncPackageSources", "IPNS source " + Name + ": malformed index"); return 0; }
    }

    int Indexed = 0, Seen = 0;
    std::set<std::string> KeepDirs;   // PkgDirs present in this sync — everything else of ours is an orphan to prune
    bool Overflow = false;
    for (const auto &Lib : Top["libraries"])
    {
        if (Overflow) break;
        if (!Lib.is_object() || !Lib["packages"].is_array()) continue;
        const std::string LibName = SanSeg(Lib.value("name", std::string()));
        for (const auto &P : Lib["packages"])
        {
            if (++Seen > kMaxIpnsPackages)
            { LogWarn("PackageCatalog::SyncPackageSources", "IPNS source " + Name + " lists more than "
                      + std::to_string(kMaxIpnsPackages) + " packages — truncating"); Overflow = true; break; }
            if (!P.is_object()) continue;
            const std::string Uid = P.value("packageUID", std::string());
            const std::string Cid = P.value("cid", std::string());
            if (Uid.empty() || Cid.empty()) continue;
            const std::string PkgDir = QDir::cleanPath(QString::fromStdString(
                BaseDir + "/" + LibName + "/" + SanSeg(Uid))).toStdString();
            KeepDirs.insert(PkgDir);

            const bool Synced = SourceDirSynced(PkgDir, Ec);
            const std::string Was = MirrorCidForPath(GlobalConfigJSON["LIBRARY"], PkgDir);
            if (!Synced || Was != Cid)
            {
                // Fetch to STAGING; swap only on success. A transient failure never leaves the current copy deleted.
                const std::string Staging = PkgDir + ".new";
                std::error_code Re; std::filesystem::remove_all(Staging, Re);
                std::string FErr;
                if (IpfsWrapper::FetchDirToPath(Cid, Staging, &FErr).empty())
                { LogErr("PackageCatalog::SyncPackageSources", "mirror package " + Uid + " (" + Cid + "): " + FErr);
                  std::filesystem::remove_all(Staging, Re);
                  if (Error && Error->empty()) *Error = FErr; continue; }   // keep the existing copy + entry
                // The staging fetch is the DEHYDRATED meta tree (*.json only). CARRY OVER already-HYDRATED content
                // (the non-.json layer bytes CollectContentTargets wrote into the bundle) from the old dir so a
                // metadata-only upstream change doesn't force a multi-GB re-download; a layer whose CID actually
                // changed is caught + re-fetched by the launch-time CID verify. Then swap atomically.
                if (Synced)
                {
                    for (const auto &F : std::filesystem::recursive_directory_iterator(PkgDir, Re))
                    {
                        if (Re || !F.is_regular_file() || F.path().extension() == ".json") continue;
                        std::error_code Ce;
                        const std::filesystem::path Rel = std::filesystem::relative(F.path(), PkgDir, Ce);
                        if (Ce || Rel.empty()) continue;
                        const std::filesystem::path Dst = std::filesystem::path(Staging) / Rel;
                        if (std::filesystem::exists(Dst, Ce)) continue;   // the new tree already has it
                        std::filesystem::create_directories(Dst.parent_path(), Ce);
                        std::filesystem::rename(F.path(), Dst, Ce);       // move (same fs: cheap; no copy of GBs)
                    }
                }
                std::filesystem::remove_all(PkgDir, Re);
                std::filesystem::rename(Staging, PkgDir, Re);
                if (Re) { LogErr("PackageCatalog::SyncPackageSources", "swap-in of " + Uid + " failed: " + Re.message());
                          std::filesystem::remove_all(Staging, Re); continue; }
            }
            const BundleIdentity Id = ScanBundleIdentity(PkgDir);
            if (!Id.Valid) { LogWarn("PackageCatalog::SyncPackageSources", "mirrored " + Uid + " is not a valid bundle — skipping"); continue; }
            UpsertMirrorEntry(GlobalConfigJSON["LIBRARY"], PkgDir, Id.Uid, Cid, Id.Name, SourceKey);
            if (Id.HasLaunchable) ++Indexed;
        }
    }

    // Prune orphans: a package removed from the friend's index (and not merely truncated by the cap) should not linger
    // in LIBRARY or on disk forever. Drop THIS source's entries whose dir is gone from the index + remove their dirs.
    if (!Overflow && GlobalConfigJSON["LIBRARY"].is_array())
    {
        auto &Lib = GlobalConfigJSON["LIBRARY"];
        for (int i = (int)Lib.size() - 1; i >= 0; --i)
        {
            const auto &E = Lib[i];
            if (!E.is_object() || E.value("SOURCE", std::string()) != SourceKey) continue;
            const std::string P = E.value("PATH", std::string());
            if (KeepDirs.count(P)) continue;
            std::error_code Re; std::filesystem::remove_all(P, Re);
            LogOut("PackageCatalog::SyncPackageSources", "pruned orphaned mirror entry " + E.value("PACKAGEUID", std::string()));
            Lib.erase(Lib.begin() + i);
        }
    }

    LogOut("PackageCatalog::SyncPackageSources", "Mirrored IPNS source '" + PackageSourceName(Src) + "' ("
           + std::to_string(Indexed) + " launchable) <- " + Name);
    return Indexed;
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
        //Legacy friend-share sources (scheme "friend:<peer>:<lib>", the retired stub receiver) are DISK-ONLY —
        //never fetched / IPNS-resolved here. Received shares now land straight in the library tree (no source).
        if (Cid.rfind("friend:", 0) == 0) continue;
        //An IPNS-name source (a friend / a manual /ipns/ address) resolves to a MUTABLE top-level index and mirrors
        //per-package meta-CIDs — a different flow from a static folder CID (handled just below).
        if (IsIpnsSource(Src)) { Indexed += MirrorIpnsSource(GlobalConfigJSON, Src, Error); continue; }
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

        const BundleIdentity Self = ScanBundleIdentity(Dir);
        if (Self.Valid)
            Upsert(Dir, Self);                                                        // single-package CID
        else
        {
            QDir D(QString::fromStdString(Dir));                                      // collection CID → scan subdirs
            for (const QString &Sub : D.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
            {
                const QString PkgDir = D.filePath(Sub);
                const BundleIdentity Id = ScanBundleIdentity(PkgDir.toStdString());
                if (!Id.Valid) continue;
                Upsert(PkgDir.toStdString(), Id);
            }
        }
        LogOut("PackageCatalog::SyncPackageSources", "Indexed CID source '" + PackageSourceName(Src) + "' ("
               + std::to_string(Packages) + " package(s), " + std::to_string(Games) + " launchable).");
    }
    return Indexed;
}

bool AddPackageSource(nlohmann::ordered_json &GlobalConfigJSON, const std::string &Cid, const std::string &Name, bool Friend)
{
    if (Cid.empty()) return false;
    if (!GlobalConfigJSON.contains("Settings") || !GlobalConfigJSON["Settings"].is_object())
        GlobalConfigJSON["Settings"] = nlohmann::ordered_json::object();
    auto &S = GlobalConfigJSON["Settings"];
    if (!S.contains("PackageSources") || !S["PackageSources"].is_array())
        S["PackageSources"] = nlohmann::ordered_json::array();
    for (const auto &E : S["PackageSources"]) if (PackageSourceCID(E) == Cid) return false;   // already added
    nlohmann::ordered_json Src;
    Src["CID"] = Cid;
    if (!Name.empty()) Src["NAME"] = Name;
    // A friend source is an IPNS-name subscription (the friend's /ipns/<peerID> library address). FRIEND marks it in
    // the UI as "a friend's library, added by accepting them" vs a manually-pasted source; IPNS makes the resolve
    // path fire even without the /ipns/ prefix (belt-and-braces with IsIpnsSource).
    if (Friend) { Src["FRIEND"] = true; Src["IPNS"] = true; }
    S["PackageSources"].push_back(std::move(Src));
    return true;
}

int PackageSourceIndexForCID(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &Cid)
{
    if (!GlobalConfigJSON.contains("Settings") || !GlobalConfigJSON["Settings"].is_object()) return -1;
    const auto &S = GlobalConfigJSON["Settings"];
    if (!S.contains("PackageSources") || !S["PackageSources"].is_array()) return -1;
    for (int i = 0; i < (int)S["PackageSources"].size(); ++i)
        if (PackageSourceCID(S["PackageSources"][i]) == Cid) return i;
    return -1;
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
            // The reference still points at the OLD path, which no longer exists. Re-pointing needs the closure
            // dropped first — a plain re-add dedup-skips and would leave us advertising an unreadable block.
            IpfsWrapper::DropRef(Cid);
            std::string AddErr;
            if (IpfsWrapper::AddNoCopy(Dest.string(), &AddErr).empty())
                LogWarn("PackageCatalog::ApplySourceUpgrade", "deprecated content not re-seeded: " + Dest.string() + " (" + AddErr + ")");
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
        IpfsWrapper::DropRef(Cid);
        std::string AddErr;
        IpfsWrapper::AddNoCopy(FromTo.second, &AddErr);
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
    fs::remove_all(P.StagingDir, Ec);

    // 5. Point config at the new CID, and drop LIBRARY rows for packages that no longer exist so the catalog matches.
    auto &Sources = GlobalConfigJSON["Settings"]["PackageSources"];
    for (auto &S : Sources)
        if (PackageSourceName(S) == P.Name) { if (S.is_object()) S["CID"] = P.NewCid; else S = nlohmann::ordered_json{{"NAME", P.Name}, {"CID", P.NewCid}}; }
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

    // Package meta-CIDs live only in config (they never appear inside a node JSON), keyed by package dir/UID.
    if (GlobalConfigJSON.contains("Settings") && GlobalConfigJSON["Settings"].is_object())
    {
        const auto &S = GlobalConfigJSON["Settings"];
        if (S.contains("PackageCids") && S["PackageCids"].is_object())
            for (const auto &[Key, Val] : S["PackageCids"].items())
                if (Val.is_string() && !Val.get<std::string>().empty() && !Dir.empty() && PathUnder(Dir, Key))
                    Cids.insert(Val.get<std::string>());
    }
    return Cids;
}

static bool DirHasContent(const std::string &Dir);   // defined below, near PlanReceivedFetches

void RemovePackageSource(nlohmann::ordered_json &GlobalConfigJSON, int Index, bool PreserveInstalled)
{
    if (!GlobalConfigJSON.contains("Settings") || !GlobalConfigJSON["Settings"].is_object()) return;
    auto &S = GlobalConfigJSON["Settings"];
    if (!S.contains("PackageSources") || !S["PackageSources"].is_array()) return;
    auto &Arr = S["PackageSources"];
    if (Index < 0 || Index >= (int)Arr.size()) return;

    const std::string Dir = PackageSourceDir(GlobalConfigJSON, Arr[Index]);

    if (PreserveInstalled)
    {
        // Friend-share source removal must NEVER rm a package the user has INSTALLED. Convert dirs with hydrated
        // content to LOCAL (clear the SOURCE tag, keep files); delete only stub-only packages. No un-seed here: stubs
        // seed nothing, and a kept install must stay seeded. The source dir itself is removed only if it ends up empty.
        if (GlobalConfigJSON.contains("LIBRARY") && GlobalConfigJSON["LIBRARY"].is_array())
        {
            auto &Lib = GlobalConfigJSON["LIBRARY"];
            for (int i = (int)Lib.size() - 1; i >= 0; --i)
            {
                if (!Lib[i].is_object()) continue;
                const std::string P = Lib[i].value("PATH", std::string());
                if (P.empty() || !PathUnder(Dir, P)) continue;
                if (DirHasContent(P)) Lib[i].erase("SOURCE");            // installed → keep as a local package
                else { std::error_code EcP; std::filesystem::remove_all(P, EcP); Lib.erase(Lib.begin() + i); }
            }
        }
        std::error_code EcD; std::filesystem::remove(Dir, EcD);          // removes the source dir only if now empty
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
        int Dropped = 0;
        for (const std::string &C : Mine) if (IpfsWrapper::DropRef(C)) ++Dropped;
        if (Dropped)
            LogSucc("PackageCatalog::RemovePackageSource",
                    "un-seeded " + std::to_string(Dropped) + "/" + std::to_string(Mine.size())
                    + " CID(s) of the removed source");
    }

    //Drop LIBRARY entries under this source's dir, then delete the fetched dir.
    if (GlobalConfigJSON.contains("LIBRARY") && GlobalConfigJSON["LIBRARY"].is_array())
    {
        auto &Lib = GlobalConfigJSON["LIBRARY"];
        for (auto It = Lib.begin(); It != Lib.end(); )
        {
            const std::string P = It->is_object() ? It->value("PATH", std::string()) : std::string();
            if (!P.empty() && PathUnder(Dir, P)) It = Lib.erase(It); else ++It;
        }
    }
    std::error_code Ec; std::filesystem::remove_all(Dir, Ec);
    Arr.erase(Arr.begin() + Index);
}

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

std::vector<std::string> PublishLibrary(nlohmann::ordered_json &Config, std::string *Error)
{
    const std::filesystem::path Root = LibraryRootDir(Config);
    std::map<std::string, nlohmann::ordered_json> Tree;
    std::map<std::string, std::filesystem::path>  Dirs;
    NodeGraph::GatherWorkingTree(Root, Tree, Dirs, /*SkipReserved=*/true);   // never mint a received friend stub
    if (Tree.empty()) { if (Error) *Error = "no packages to publish under " + Root.string(); return {}; }
    NodeGraph::LiftLibraryItemEdge(Tree);
    NodeGraph::MintResult MR;
    if (!NodeGraph::Mint(Tree, MR, Error)) return {};   // DagPut every node block → pinned + announced + seedable

    // Group PUBLISH'd nodes by LIBRARY (the collection dir the package lives in), so sharing is per-library. The share
    // axis is the author's PUBLISH flag, decoupled from type — so runners (GUEST exec) and libraries (no exec) are
    // shareable too, not just games. `Libraries` = {libName: [{cid,node,uid,title,tilecid,tilenode}]} — each entry
    // carries, besides the node-block CID, the metadata a RECEIVER needs to route the block to its FINAL library path
    // (LIBRARY/<nick> - <lib>/[uid] <title>/<node>.json) BEFORE fetching it, so a received share rides the ordinary
    // rolling queue straight into the library tree with no intermediary dir and no post-fetch materialize step.
    // A flat `PublishedList` of bare CIDs stays for convenience. Off-IPFS VidyaGod app data (friend channel).
    std::map<std::string, std::string> CidToHandle;   // resolve a LIBRARYITEM that is already a CID (re-published
    for (const auto &[H, C] : MR.HandleToCid) CidToHandle[C] = H;   // received package) back to its tree handle
    nlohmann::ordered_json Libs = nlohmann::ordered_json::object();
    nlohmann::ordered_json Flat = nlohmann::ordered_json::array();
    for (const auto &[Handle, Doc] : Tree)
    {
        if (!Doc.value("PUBLISH", false)) continue;    // SHARE axis: only nodes the author flagged PUBLISH=true.
                                                       // Replaces the old "DeclareExec && !GUEST" filter (which
                                                       // silently excluded runners and no-exec library heads).
        const auto CidIt = MR.HandleToCid.find(Handle);
        if (CidIt == MR.HandleToCid.end()) continue;   // node was skipped (dangling/bad) — not shareable

        // The node's LIBRARYITEM tile (lifted above): the receiver fetches it alongside and names the package dir
        // after its UID/TITLE — variants sharing one tile land in ONE package dir, exactly like the local tree.
        std::string TileHandle;
        if (const std::string Li = Doc.value("LIBRARYITEM", std::string()); !Li.empty())
        {
            if (Tree.count(Li)) TileHandle = Li;
            else if (const auto Rit = CidToHandle.find(Li); Rit != CidToHandle.end()) TileHandle = Rit->second;
            if (TileHandle.empty())
            {
                // The node NAMES a tile we cannot resolve — typically a received share whose tile block hasn't
                // landed yet. Emitting it now would ship uid/title = the bare handle, forking third-party trees
                // into "[handle] handle/" dirs that never reconcile with the correct "[uid] Title/" of the next
                // publish. Skip it THIS round; it re-enters the share list once the tile is in the tree.
                LogWarn("PackageCatalog::PublishLibrary", "'" + Handle + "' has an unresolved LIBRARYITEM ("
                        + Li.substr(0, 16) + "…) — held out of the share list until its tile lands");
                continue;
            }
        }
        std::string TileCid;
        if (!TileHandle.empty())
            if (const auto Tit = MR.HandleToCid.find(TileHandle); Tit != MR.HandleToCid.end()) TileCid = Tit->second;

        std::string Uid = Handle, Title = Handle;      // no tile → the node stands alone under its own handle
        if (!TileHandle.empty())
        {
            const nlohmann::ordered_json &T = Tree.at(TileHandle);
            Uid = T.value("UID", std::string());
            if (Uid.empty()) Uid = TileHandle;
            Title = T.value("TITLE", TileHandle);
        }
        // Bound the emitted title (user-authored, unbounded) at a UTF-8 boundary: the receiver's inbound gate
        // rejects a WHOLE snapshot over one >512-byte field — silently, at the far end — and its dir segment must
        // stay under NAME_MAX. 120 bytes is ample for a display title.
        if (Title.size() > 120)
        {
            Title.resize(120);
            while (!Title.empty() && (static_cast<unsigned char>(Title.back()) & 0xC0) == 0x80) Title.pop_back();
        }

        std::string LibName = LibraryOf(Dirs[Handle], Root);
        if (LibName.empty()) LibName = "Library";
        // pkg = the node's ACTUAL package dir basename: the receiver reproduces the seeder's tree exactly, so a
        // multi-game package (AoE2: AoK + Conquerors + …) lands as ONE dir and the catalog's by-bundle sibling
        // grouping ("Other games in this package") survives the wire. uid/title stay for display + legacy fallback.
        nlohmann::ordered_json E{{"cid", CidIt->second}, {"node", Handle},
                                 {"pkg", Dirs[Handle].filename().string()},
                                 {"uid", Uid}, {"title", Title}};
        if (!TileCid.empty()) { E["tilecid"] = TileCid; E["tilenode"] = TileHandle; }
        Libs[LibName].push_back(std::move(E));
        Flat.push_back(CidIt->second);
    }
    Config["Libraries"]     = std::move(Libs);
    Config["PublishedList"] = std::move(Flat);
    LogSucc("PackageCatalog::PublishLibrary", "published " + std::to_string(MR.HandleToCid.size())
            + " node block(s), " + std::to_string(Config["Libraries"].size()) + " library(ies), "
            + std::to_string(Config["PublishedList"].size()) + " launchable(s)");
    return MR.Published;   // the share list (PUBLISH'd roots), not the launch axis
}

// Bounds for the received-share planner (a friend's snapshot is UNTRUSTED input).
static constexpr size_t kMaxFriendRootsPerLib = 20000;    // cap a hostile/huge per-library item set (Go caps at 100k)
static constexpr size_t kMaxFriendRootsTotal  = 100000;   // snapshot-wide cap across all of one peer's libraries

// True if a dir holds any HYDRATED content (a file that isn't a node *.json) — i.e. the user has INSTALLED this
// package. Source removal must never delete such a dir: a withdrawal (or a transient failure) must never rm a
// user's installed game.
static bool DirHasContent(const std::string &Dir)
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

std::vector<ReceivedFetch> PlanReceivedFetches(const nlohmann::ordered_json &GlobalConfigJSON,
                                               const std::string &NickLabel, const nlohmann::ordered_json &Libs)
{
    namespace fs = std::filesystem;
    // UNTRUSTED input (a friend's snapshot): every path segment is sanitized to a plain filename — no separators, no
    // traversal — and the "[uid] title" / "<nick> - <lib>" shapes keep any segment from ever being "." or "..".
    auto San = [](const std::string &In) {
        std::string O;
        for (char c : In) O.push_back((std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.'
                                       || c == ' ' || c == '[' || c == ']') ? c : '_');
        if (O.size() > 120) O.resize(120);   // NAME_MAX is 255 BYTES; wire caps (512-byte title) exceed it — a
                                             // within-bounds snapshot must still yield satisfiable dests, or the
                                             // queue retries an ENAMETOOLONG mkdir forever. San output is ASCII.
        return O.empty() ? std::string("x") : O;
    };
    std::vector<ReceivedFetch> Out;
    if (!Libs.is_object()) return Out;
    const fs::path Root = fs::path(CatalogRootDir(GlobalConfigJSON));   // received stubs live in CATALOG, not LIBRARY
    std::set<std::string> UsedDests;   // 900 variants share ONE tile → one target; a NODE_ID collision keeps first-seen
    auto Add = [&](const std::string &Cid, const fs::path &Dest) {
        if (Cid.empty() || Cid.size() > 128) return;
        if (!UsedDests.insert(Dest.string()).second) return;
        Out.push_back(ReceivedFetch{Cid, Dest.string()});
    };
    size_t Total = 0;   // snapshot-wide bound (a hostile friend could send many libs x many items)
    for (const auto &[LibName, Items] : Libs.items())
    {
        if (!Items.is_array()) continue;
        const fs::path LibDir = Root / San(NickLabel + " - " + LibName);
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
            std::string NodeId = It.value("node", std::string());
            if (NodeId.empty()) NodeId = Cid.substr(0, 12);
            std::string Uid = It.value("uid", std::string());
            if (Uid.empty()) Uid = NodeId;
            std::string Title = It.value("title", std::string());
            if (Title.empty()) Title = NodeId;
            // The seeder's real package dir name keeps multi-game packages ONE dir (sibling grouping is by bundle
            // dir); the "[uid] title" shape is only the fallback for a snapshot without it.
            std::string PkgSeg = It.value("pkg", std::string());
            if (PkgSeg.empty()) PkgSeg = "[" + Uid + "] " + Title;
            const fs::path PkgDir = LibDir / San(PkgSeg);
            Add(Cid, PkgDir / (San(NodeId) + ".json"));
            if (const std::string TileCid = It.value("tilecid", std::string()); !TileCid.empty())
            {
                std::string TileNode = It.value("tilenode", std::string());
                if (TileNode.empty()) TileNode = TileCid.substr(0, 12);
                Add(TileCid, PkgDir / (San(TileNode) + ".json"));
            }
        }
    }
    return Out;
}

// CompleteClosure: fetch a launchable's MISSING node blocks (PARENTS reachable from it that the index cannot
// resolve — the state every received share starts in: only the exec + tile were shared) into its OWN package dir,
// through the ONE rolling queue — each missing CID is a plain FetchTarget {cid, <bundle>/<cid>.json, Verify}, the
// same path as every other CID in the app. Waves: fetch the frontier, read the landed blocks (blockstore, local),
// discover the next frontier from their PARENTS/LIBRARYITEM, repeat until the closure closes. Landed files are
// renamed to their NODE_ID (cosmetic — identity is the NODE_ID inside; the scan never cares about filenames).
// Synchronous (WaitBatch) — call OFF the GUI thread. False + *Error when the closure cannot converge (a dangling
// ref, an unreachable seeder past the wait bound — the queue keeps retrying in the background either way).
bool CompleteClosure(const NodeIndex &Idx, const std::string &LaunchId, std::string *Error)
{
    namespace fs = std::filesystem;
    auto San = [](const std::string &In) {
        std::string O;
        for (char c : In) O.push_back((std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.'
                                       || c == ' ' || c == '[' || c == ']') ? c : '_');
        if (O.size() > 120) O.resize(120);
        return O.empty() ? std::string("x") : O;
    };
    const Node *Root = Idx.Find(LaunchId);
    if (!Root) { if (Error) *Error = "unknown node " + LaunchId; return false; }
    if (Root->BundleDir.empty()) { if (Error) *Error = LaunchId + " has no package dir to complete into"; return false; }
    const fs::path Bundle = Root->BundleDir;

    // Refs of one node: PARENTS + LIBRARYITEM. Resolvable via the index (disk tree) or via a block landed this call.
    std::map<std::string, nlohmann::ordered_json> LandedDoc;       // cid → parsed block (this call)
    auto RefsOf = [](const std::vector<std::string> &Ps, const std::string &Li, std::vector<std::string> &Out) {
        for (const std::string &P : Ps) if (!P.empty()) Out.push_back(P);
        if (!Li.empty()) Out.push_back(Li);
    };

    // Seed the frontier: BFS the ALREADY-PRESENT part of the closure; every unresolved ref is missing.
    std::set<std::string> Visited, Missing;
    std::deque<const Node *> Q{ Root };
    Visited.insert(Root->Key());
    while (!Q.empty())
    {
        const Node *N = Q.front(); Q.pop_front();
        std::vector<std::string> Refs;
        RefsOf(N->Parents, N->LibraryItem, Refs);
        for (const std::string &R : Refs)
        {
            if (!Visited.insert(R).second) continue;
            if (const Node *C = Idx.Find(R)) Q.push_back(C);
            else Missing.insert(R);
        }
    }

    size_t Total = 0;
    while (!Missing.empty())
    {
        if ((Total += Missing.size()) > 200000)                          // hostile/looping closure — bounded like the freezer
        { if (Error) *Error = "closure exceeds 200000 nodes"; return false; }

        std::vector<IpfsWrapper::FetchTarget> Batch;
        std::vector<std::string> Wave(Missing.begin(), Missing.end());
        for (const std::string &C : Wave)
            Batch.push_back(IpfsWrapper::FetchTarget{ C, (Bundle / (San(C) + ".json")).string(),
                                                      /*Optional=*/false, /*Dir=*/false, /*Verify=*/true });
        const auto H = IpfsWrapper::EnqueueBatch(Batch);
        std::string WErr;
        if (!IpfsWrapper::WaitBatch(H, 10 * 60 * 1000, &WErr))           // bounded; the queue keeps rolling regardless
        { if (Error) *Error = "closure fetch did not converge: " + WErr; return false; }

        // Read the landed blocks (local), discover the NEXT frontier, and give each file its NODE_ID name.
        const auto Blocks = IpfsWrapper::DagGetManyLocal(Wave);
        std::set<std::string> Next;
        for (const std::string &C : Wave)
        {
            const auto It = Blocks.find(C);
            if (It == Blocks.end()) { if (Error) *Error = "closure block " + C + " did not land"; return false; }
            nlohmann::ordered_json J = nlohmann::ordered_json::parse(It->second, nullptr, false);
            if (J.is_discarded() || !J.is_object()) { if (Error) *Error = "closure block " + C + " is not a node"; return false; }
            NodeGraph::NormalizeLinks(J);
            std::vector<std::string> Refs;
            std::vector<std::string> Ps;
            if (J.contains("PARENTS") && J["PARENTS"].is_array())
                for (const auto &P : J["PARENTS"]) if (P.is_string()) Ps.push_back(P.get<std::string>());
            RefsOf(Ps, J.value("LIBRARYITEM", std::string()), Refs);
            for (const std::string &R : Refs)
            {
                if (!Visited.insert(R).second) continue;
                if (!Idx.Find(R) && !LandedDoc.count(R)) Next.insert(R);
            }
            LandedDoc[C] = J;

            const std::string NodeId = J.value("LABEL", std::string());
            if (!NodeId.empty())
            {
                std::error_code Ec;
                const fs::path From = Bundle / (San(C) + ".json"), To = Bundle / (San(NodeId) + ".json");
                if (fs::exists(From, Ec) && !fs::exists(To, Ec)) fs::rename(From, To, Ec);   // cosmetic; best-effort
            }
        }
        Missing = std::move(Next);
    }
    return true;
}

bool NodeClosureIncomplete(const NodeIndex &Idx, const std::string &Id)
{
    const Node *N = Idx.Find(Id);
    if (!N) return false;
    for (const auto &P : N->Parents)                                    // adapt iteration to the actual Parents type
        if (!Idx.Find(P)) return true;
    return false;
}

// The gigagraph catalog: the on-disk library is the pretty, handle-based working tree (LIBRARY/[uid] Title/…); we
// derive the CID-addressed node graph from it in memory (NodeGraph::FreezeToIndex) — identity = each node's dag-json
// CID, NODE_ID demotes to a label, cross-package edges resolve to CIDs, and every node carries its on-disk BundleDir
// so launch mounts local content. No on-disk rewrite (git model: working tree is truth, CID index is derived on load;
// DagPut only happens when publishing). The tile edge is lifted (PARENTS → LIBRARYITEM) before freezing.
NodeIndex BuildCatalogIndex(const nlohmann::ordered_json &GlobalConfigJSON)
{
    std::map<std::string, nlohmann::ordered_json> Tree;
    std::map<std::string, std::filesystem::path>  Dirs;
    NodeGraph::GatherWorkingTree(LibraryRootDir(GlobalConfigJSON), Tree, Dirs);
    NodeGraph::GatherWorkingTree(CatalogRootDir(GlobalConfigJSON), Tree, Dirs);   // received browse stubs (merged in
                                                                                 // → a LIBRARY game's dep resolves
                                                                                 // even when the dep is a CATALOG stub)
    for (const auto &D : LocalPackageDirs(GlobalConfigJSON))   // externally-added bundles that live OUTSIDE LIBRARY
        NodeGraph::GatherWorkingTree(D, Tree, Dirs);
    NodeGraph::LiftLibraryItemEdge(Tree);
    std::string Err;
    NodeIndex Idx = NodeGraph::FreezeToIndex(Tree, Dirs, &Err);
    if (Idx.Nodes.empty() && !Tree.empty())
        LogErr("PackageCatalog::BuildCatalogIndex", "freeze failed (" + std::to_string(Tree.size()) + " node(s)): " + Err);
    else
        LogOut("PackageCatalog::BuildCatalogIndex", "Indexed " + std::to_string(Idx.Nodes.size())
               + " node(s) by CID from " + LibraryRootDir(GlobalConfigJSON));

    return Idx;
}

std::vector<std::vector<const Node*>> PresentableGroups(const NodeIndex &Idx)
{
    std::map<std::string, std::vector<const Node*>> Groups;
    for (const auto &[Id, N] : Idx.Nodes)
        if (N.IsLaunchable() && N.Presentable())
            Groups[N.GameKey()].push_back(&N);

    std::vector<std::vector<const Node*>> Out;
    for (auto &[K, V] : Groups)
    {
        std::sort(V.begin(), V.end(), [](const Node *A, const Node *B){
            if (A->Recommended != B->Recommended) return A->Recommended;     // recommended edition first
            return A->NodeId < B->NodeId;
        });
        Out.push_back(std::move(V));
    }
    auto Title = [](const Node *N){ return N->Meta.is_object() ? N->Meta.value("TITLE", N->NodeId) : N->NodeId; };
    std::sort(Out.begin(), Out.end(), [&](const std::vector<const Node*> &A, const std::vector<const Node*> &B){
        return Title(A.front()) < Title(B.front());
    });
    return Out;
}

std::vector<const Node*> RunnerCandidates(const NodeIndex &Idx, const Node &Launch)
{
    std::vector<const Node*> Out;
    for (const auto &[Id, N] : Idx.Nodes)
    {
        if (!N.IsRunner() || N.HostPlatform != MachinePlatform()) continue;
        bool Guest = false;
        for (const auto &G : N.GuestPlatform) if (G == Launch.HostPlatform) { Guest = true; break; }
        if (Guest && RunnerWrapper::ExecutableAvailable(N.Exec)) Out.push_back(&N);
    }
    return Out;   // std::map iteration = sorted by node id
}

std::vector<const Node*> CompatibleRunners(const NodeIndex &Idx, const Node &Launch)
{
    std::vector<const Node*> Out;
    for (const auto &[Id, N] : Idx.Nodes)
    {
        (void)Id;
        if (!N.IsRunner() || N.HostPlatform != MachinePlatform()) continue;
        for (const auto &G : N.GuestPlatform) if (G == Launch.HostPlatform) { Out.push_back(&N); break; }
    }
    return Out;   // no executable/install gate — these are runners that COULD run it once installed
}

bool RunnerInstalled(const NodeIndex &Idx, const std::string &RunnerNodeId)
{
    const Node *R = Idx.Find(RunnerNodeId);
    if (!R || !R->IsRunner()) return false;
    // Ships its own build (any VFS layer in its content closure — local PATH or a remote CID) → must be imported
    // (build hydrated + DEFPREFIX). Otherwise it's a PATH runner → usable iff its executable resolves on this system.
    bool ShipsBuild = false;
    for (const std::string &Id : ManifestModel::ResolveNodeOrder(Idx, RunnerNodeId, {}))
    {
        if (Id == RunnerNodeId) continue;
        const Node *N = Idx.Find(Id);
        if (!N || N->IsRunner() || !N->Layers.is_array()) continue;
        for (const auto &L : N->Layers) if (ManifestModel::IsRunnerBuildLayer(L)) { ShipsBuild = true; break; }
        if (ShipsBuild) break;
    }
    if (ShipsBuild)
        return RunnerInstall::RunnerNodeImported(Idx, RunnerNodeId);
    return RunnerWrapper::ExecutableAvailable(R->Exec);
}

bool IsEmbeddedRunner(const NodeIndex &Idx, const std::string &RunnerNodeId)
{
    const Node *R = Idx.Find(RunnerNodeId);
    if (!R || !R->IsRunner()) return false;
    for (const auto &[Id, N] : Idx.Nodes)
    {
        (void)Id;
        if (N.IsLaunchable() && N.BundleDir == R->BundleDir) return true;   // a game shares its bundle → embedded
    }
    return false;
}

std::vector<const Node*> UsableRunners(const NodeIndex &Idx, const Node &Launch)
{
    std::vector<const Node*> Out;
    for (const Node *R : CompatibleRunners(Idx, Launch))
        if (RunnerInstalled(Idx, R->NodeId)) Out.push_back(R);
    return Out;
}

std::vector<const Node*> CandidateRunners(const NodeIndex &Idx, const std::string &InputPlatform)
{
    //Installed runners that can consume InputPlatform (GUEST ∋ InputPlatform), regardless of HOST — a chain step's
    //choices. Unlike UsableRunners, HOST may be an intermediate platform (e.g. snes9x: snes→win32). Sorted by node id.
    std::vector<const Node*> Out;
    for (const auto &[Id, N] : Idx.Nodes)
    {
        (void)Id;
        if (!N.IsRunner()) continue;
        bool Serves = false;
        for (const auto &G : N.GuestPlatform) if (G == InputPlatform) { Serves = true; break; }
        if (Serves && RunnerInstalled(Idx, N.NodeId)) Out.push_back(&N);
    }
    return Out;   // std::map iteration = sorted by node id
}

//Invoke Fn for every VFS layer in a launchable's content closure (runner build excluded), with its resolved local
//path + ipfs CID (resolved against the OWNING node's bundle dir — cross-bundle-correct).
static void ForEachContentLayer(const NodeIndex &Idx, const std::string &LaunchNodeId,
    const std::map<std::string, bool> &Toggles,
    const std::function<void(const nlohmann::ordered_json&, const std::filesystem::path&, const std::string&)> &Fn)
{
    for (const std::string &Id : ManifestModel::ResolveNodeOrder(Idx, LaunchNodeId, Toggles))
    {
        const Node *N = Idx.Find(Id);
        if (!N || N->IsRunner() || !N->Layers.is_array()) continue;
        for (const auto &L : N->Layers)
        {
            if (!IsVfsLayer(LayerType(L))) continue;
            std::filesystem::path Local; std::string Cid;
            LayerLocator(L, N->BundleDir, Local, Cid);
            if (Local == N->BundleDir) continue;                                 // no PATH
            Fn(L, Local, Cid);
        }
    }
}

// The global CustomVar default namespace (KEY -> DEFAULT) across the whole node graph. CustomVars resolve in ONE
// global namespace (see LaunchResolver::ResolveCustomVariables), so a single map suffices to substitute the %KEY%
// tokens that appear in content-layer PATHs — e.g. a shared library node whose PATH is "%dgv_zip%". Later (more
// specific) declarations win, matching the launch resolver's hierarchy; for hydration only the file identity matters.
static std::map<std::string, std::string> CustomVarDefaults(const NodeIndex &Idx)
{
    std::map<std::string, std::string> Vars;
    for (const auto &[Id, N] : Idx.Nodes)
    {
        (void)Id;
        if (!N.Layers.is_array()) continue;
        for (const auto &L : N.Layers)
            if (L.is_object() && L.value("TYPE", std::string()) == "CustomVar")
            {
                const std::string Key = L.value("KEY", std::string());
                if (!Key.empty()) Vars[Key] = L.value("DEFAULT", std::string());
            }
    }
    return Vars;
}

// How a content layer's PATH resolves against the local filesystem — the path-only half of its fetch state (the CID
// half is applied at the call site, since it decides fetchable-vs-broken). A PATH may be CustomVar-templated
// ("%dgv_zip%" -> a real zip); substitute the global var namespace first. A path that STILL carries a %token% after
// substitution is RUNTIME-sourced (a runner's "%RunnerMount%/..." prefix mount) — it resolves to a live mount at
// launch, not to on-disk content (mirrors LaunchSources' IsRuntimeSourcedLayer skip).
enum class PathState : std::uint8_t { Runtime, Present, Missing };

// Runtime-sourced is a property of the LAYER, and it is decided by the ONE predicate — never re-derived from
// the resolved absolute path. Deriving it from the path was wrong twice over:
//   * a library root containing a '%' segment (a folder literally named "My %Games%") made EVERY layer under
//     it classify as Runtime, so the whole library silently reported hydrated; and
//   * a URL-escaped filename ("100%25%20done.zip"), which the predicate deliberately treats as real content,
//     fell through to substitute-and-see, where the undefined %25% survived and again produced Runtime — so a
//     fetchable-missing layer was invisible to the download button and the game launched against a hole.
// A path whose tokens are ALL defined is CustomVar-templated ("%dgv_zip%" -> a real zip) and does resolve;
// one with any undefined token is a live runtime mount ("%RunnerMount%/...") and has no on-disk file at all.
// Asked this way rather than by substituting, because substitution LOGS a warning per undefined token and
// here an undefined token is the expected answer, not a problem.
static PathState ResolvePathState(const nlohmann::ordered_json &Layer, const std::filesystem::path &Local,
                                  const std::map<std::string, std::string> &Vars)
{
    const std::vector<std::string> Tokens = ManifestModel::PathVariableTokens(ManifestModel::LayerPathString(Layer));
    for (const std::string &T : Tokens) if (!Vars.count(T)) return PathState::Runtime;

    std::string S = Local.string();
    if (!Tokens.empty()) VarSubst::StringVariableSubstitution(S, Vars);   // every token IS defined
    std::error_code Ec;
    return std::filesystem::exists(S, Ec) ? PathState::Present : PathState::Missing;
}

// HYDRATION = "everything fetchable has been fetched" (NOT "every backing file exists"). A content layer is un-hydrated
// ONLY when it is FETCHABLE-MISSING: its backing file is absent AND it carries a remote source (CID) to fetch it from.
// A missing local-only layer (no CID) is not un-hydrated — there is nothing to fetch; it is a broken/incomplete package,
// which --validate-nodes owns, not the download button. A runtime-sourced %path% has no on-disk file at all.
static bool LayerFetchableMissing(PathState PS, const std::string &Cid)
{
    return PS == PathState::Missing && !Cid.empty();
}

bool NodeHydrated(const NodeIndex &Idx, const std::string &LaunchNodeId)
{
    if (!Idx.Find(LaunchNodeId)) return false;
    const std::map<std::string, std::string> Vars = CustomVarDefaults(Idx);
    bool AllFetched = true;
    ForEachContentLayer(Idx, LaunchNodeId, {}, [&](const nlohmann::ordered_json&L, const std::filesystem::path &Local, const std::string &Cid){
        if (LayerFetchableMissing(ResolvePathState(L, Local, Vars), Cid)) AllFetched = false;
    });
    return AllFetched;
}

bool NodeHasContent(const NodeIndex &Idx, const std::string &LaunchNodeId)
{
    // True iff the node's closure defines at least one VFS content layer. Distinguishes a real (downloadable) game
    // from a content-less/malformed node, which is VACUOUSLY "hydrated" (no layers → nothing missing) and would
    // otherwise show up in the Library + Installed Packages with nothing to launch.
    bool Has = false;
    ForEachContentLayer(Idx, LaunchNodeId, {}, [&](const nlohmann::ordered_json&, const std::filesystem::path&, const std::string&){ Has = true; });
    return Has;
}

std::unordered_map<std::string, NodeHydration> HydrationMap(const NodeIndex &Idx)
{
    std::unordered_map<std::string, NodeHydration> Memo;
    std::unordered_map<std::string, PathState> StatCache;            // raw local path -> resolved path state (stat'd once)
    const std::map<std::string, std::string> Vars = CustomVarDefaults(Idx);   // resolve %dgv_zip% etc. before stat'ing
    std::function<NodeHydration(const std::string &)> Compute = [&](const std::string &Id) -> NodeHydration {
        auto It = Memo.find(Id);
        if (It != Memo.end()) return It->second;
        Memo[Id] = {true, false};                                    // cycle guard (graph is a DAG; validation checks this)
        NodeHydration R;                                             // {Hydrated=true, HasContent=false}
        const Node *N = Idx.Find(Id);
        // Own content layers (runner nodes contribute nothing — their layers are never mounted as content).
        if (N && !N->IsRunner() && N->Layers.is_array())
            for (const auto &L : N->Layers)
            {
                if (!L.is_object() || !IsVfsLayer(LayerType(L))) continue;
                R.HasContent = true;
                std::filesystem::path Local; std::string Cid;
                LayerLocator(L, N->BundleDir, Local, Cid);
                const std::string Key = Local.string();
                auto Sc = StatCache.find(Key);
                PathState PS;
                if (Sc != StatCache.end()) PS = Sc->second;
                else { PS = ResolvePathState(L, Local, Vars); StatCache[Key] = PS; }
                if (LayerFetchableMissing(PS, Cid)) R.Hydrated = false;
            }
        // Fold in the parents' (already-memoized) closure result. A parent MISSING from the index means the closure
        // itself isn't fetched (a received share before install, or a broken ref) — that can never count as hydrated:
        // "hydrated" previously held VACUOUSLY for such nodes (no reachable content layers → nothing missing), which
        // filed un-installed received games under the Library tab as playable and left the Catalog empty.
        if (N)
            for (const std::string &P : N->Parents)
            {
                if (!Idx.Find(P)) { R.Hydrated = false; continue; }
                const NodeHydration PC = Compute(P);
                R.Hydrated    = R.Hydrated && PC.Hydrated;
                R.HasContent  = R.HasContent || PC.HasContent;
            }
        Memo[Id] = R;
        return R;
    };
    for (const auto &[Id, N] : Idx.Nodes) { (void)N; Compute(Id); }
    return Memo;
}

int DehydrateNode(const NodeIndex &Idx, const std::string &LaunchNodeId)
{
    // Inverse of HydrateNode: delete the node closure's local content-layer files (keeping the manifests + cover, so
    // the package returns to the Catalog as re-downloadable) and unpin + drop the references for their CIDs (so the
    // node stops "seeding" content that's no longer on disk). Cover art (META.COVER) is left in place — it's tiny,
    // git-tracked, and needed to render the catalog tile.
    int Removed = 0;
    std::set<std::string> Cids;
    ForEachContentLayer(Idx, LaunchNodeId, {}, [&](const nlohmann::ordered_json&, const std::filesystem::path &Local, const std::string &Cid){
        if (!Cid.empty()) Cids.insert(Cid);
        std::error_code Ec;
        if (std::filesystem::exists(Local, Ec) && std::filesystem::is_regular_file(Local, Ec))
        { std::filesystem::remove(Local, Ec); if (!Ec) ++Removed; }
    });
    for (const std::string &C : Cids) { IpfsWrapper::Unpin(C); IpfsWrapper::DropRef(C); }
    return Removed;
}

std::vector<std::string> GroupNodeIds(const NodeIndex &Idx, const std::string &LaunchNodeId)
{
    std::vector<std::string> Ids{LaunchNodeId};     // first ⇒ preselected in the variant picker
    const Node *N = Idx.Find(LaunchNodeId);
    if (!N) return Ids;
    const std::string Key = N->GameKey();
    for (const std::vector<const Node*> &Group : PresentableGroups(Idx))
        for (const Node *G : Group)
            if (G->GameKey() == Key && G->NodeId != LaunchNodeId)
                Ids.push_back(G->NodeId);
    return Ids;
}

std::vector<std::string> NodeContentCids(const NodeIndex &Idx, const std::string &LaunchNodeId, const std::map<std::string, bool> &Toggles)
{
    std::vector<std::string> Cids;
    std::set<std::string> Seen;
    ForEachContentLayer(Idx, LaunchNodeId, Toggles, [&](const nlohmann::ordered_json&, const std::filesystem::path&, const std::string &Cid){
        if (!Cid.empty() && Seen.insert(Cid).second) Cids.push_back(Cid);
    });
    return Cids;
}

std::map<std::string, long long> NodeContentSizes(const NodeIndex &Idx, const std::string &LaunchNodeId,
                                                  const std::map<std::string, bool> &Toggles)
{
    // The stamped SOURCE.SIZE per content CID — the sizes-in-JSON keystone: a download size must derive INSTANTLY
    // and OFFLINE from the graph, never from a ~35s network probe per CID (which left the pre-download dialog
    // "estimating…" for minutes). Only stamped (>0) entries are returned; the caller probes the rare unstamped rest.
    std::map<std::string, long long> Out;
    ForEachContentLayer(Idx, LaunchNodeId, Toggles, [&](const nlohmann::ordered_json &L, const std::filesystem::path&, const std::string &Cid){
        if (Cid.empty() || !L.contains("SOURCE") || !L["SOURCE"].is_object()) return;
        const long long Sz = L["SOURCE"].value("SIZE", (long long)0);
        if (Sz > 0) Out[Cid] = Sz;
    });
    return Out;
}

bool CollectContentTargets(const NodeIndex &Idx, const std::string &LaunchNodeId, const std::map<std::string, bool> &Toggles,
                           std::vector<IpfsWrapper::FetchTarget> &Out, std::string *Error)
{
    auto Fail = [&](const std::string &M) -> bool { if (Error) *Error = M; LogErr("PackageCatalog::CollectContentTargets", M); return false; };
    const Node *Launch = Idx.Find(LaunchNodeId);
    if (!Launch) return Fail("launch node not found: " + LaunchNodeId);

    // Required content layers: fetch any that aren't already on disk; a missing layer with no IPFS source is fatal.
    bool MissingSource = false; std::string MissingErr;
    ForEachContentLayer(Idx, LaunchNodeId, Toggles, [&](const nlohmann::ordered_json &L, const std::filesystem::path &Local, const std::string &Cid){
        if (MissingSource) return;
        std::error_code Ec;
        if (std::filesystem::exists(Local, Ec)) return;                          // already present
        if (Cid.empty()) { MissingSource = true; MissingErr = "missing local content with no IPFS source: " + Local.string(); return; }
        // Push the stamped SOURCE.SIZE to the node so a gateway-fallback fetch of this layer reports a real % (and a
        // pre-fetch total). Done HERE — at fetch-build, on the shared path for the GUI download, --fetch AND
        // --download-all — so it works headless with no GUI model, and before getRoot's gateway phase runs.
        if (L.contains("SOURCE") && L["SOURCE"].is_object())
        { const long long Sz = L["SOURCE"].value("SIZE", (long long)0); if (Sz > 0) IpfsWrapper::SetExpectedSize(Cid, Sz); }
        Out.push_back({Cid, Local.string(), false});
    });
    if (MissingSource) return Fail(MissingErr);

    // Cover: fetch if remote; if already on disk but un-seeded/orphaned, seed it by reference so a downloader serves
    // it too. Best-effort (a cover never fails a hydrate).
    if (Launch->Meta.is_object() && Launch->Meta.contains("COVER") && Launch->Meta["COVER"].is_object())
    {
        std::filesystem::path Local; std::string Cid;
        LayerLocator(Launch->Meta["COVER"], Launch->BundleDir, Local, Cid);
        std::error_code Ec;
        if (!Cid.empty() && Local != Launch->BundleDir)
        {
            if (!std::filesystem::exists(Local, Ec))
            {
                const auto &Cv = Launch->Meta["COVER"];                      // register the cover's stamped SIZE too
                if (Cv.contains("SOURCE") && Cv["SOURCE"].is_object())
                { const long long Sz = Cv["SOURCE"].value("SIZE", (long long)0); if (Sz > 0) IpfsWrapper::SetExpectedSize(Cid, Sz); }
                Out.push_back({Cid, Local.string(), true});                 // not local → fetch over IPFS (FetchToPath seeds it)
            }
            else if (!IpfsWrapper::HasLocal(Cid) || IpfsWrapper::CidMissing(Cid))
            {
                if (IpfsWrapper::CidMissing(Cid)) IpfsWrapper::DropRef(Cid);   // re-point a stale reference
                std::string E;
                if (IpfsWrapper::AddNoCopy(Local.string(), &E).empty())
                    LogWarn("PackageCatalog::CollectContentTargets", "could not seed local cover " + Local.string() + " (" + E + ")");
            }
        }
    }
    return true;
}

std::vector<std::string> RunnerChainIds(const NodeIndex &Idx, const std::string &LaunchNodeId,
                                        const nlohmann::ordered_json &GlobalConfigJSON)
{
    const Node *Launch = Idx.Find(LaunchNodeId);
    if (!Launch) return {};
    ContainerParams Cp(Launch->BundleDir, LaunchNodeId, std::string());
    Cp.NodeIdx = &Idx; Cp.LaunchNodeId = LaunchNodeId; Cp.PackageUID = Launch->Uid;
    std::vector<std::string> Ids;
    for (const std::string &RunnerId : LaunchResolver::ResolveChainIds(Idx, *Launch, Cp, GlobalConfigJSON))
        if (RunnerId != LaunchResolver::kNativeTerminalId) Ids.push_back(RunnerId);
    return Ids;
}

bool CollectRunnerChainTargets(const NodeIndex &Idx, const std::string &LaunchNodeId,
                               const nlohmann::ordered_json &GlobalConfigJSON,
                               std::vector<IpfsWrapper::FetchTarget> &Out, std::string *Error)
{
    // A downloaded game is only PLAYABLE once its runtime is present too — so a full-closure hydrate pools the resolved
    // runner CHAIN's build content (e.g. a Minecraft version's java_<major> JRE, or a wine game's Proton build) into the
    // same fetch batch. The game's own PARENTS closure (dgVoodoo/jlib_*/DirectPlay content nodes) is already covered by
    // CollectContentTargets — only the runner, which the closure walk skips (IsRunner), is added here. A native /
    // build-less runner (kNativeTerminalId, a PATH runner) contributes nothing. Best-effort: an unresolvable chain is
    // not fatal to the game content fetch (the launch itself will report a missing runner).
    const Node *Launch = Idx.Find(LaunchNodeId);
    if (!Launch) return true;
    ContainerParams Cp(Launch->BundleDir, LaunchNodeId, std::string());
    Cp.NodeIdx = &Idx; Cp.LaunchNodeId = LaunchNodeId; Cp.PackageUID = Launch->Uid;   // for the per-package runner pin lookup
    for (const std::string &RunnerId : LaunchResolver::ResolveChainIds(Idx, *Launch, Cp, GlobalConfigJSON))
    {
        if (RunnerId == LaunchResolver::kNativeTerminalId) continue;
        std::string E;
        if (!RunnerInstall::CollectRunnerNodeTargets(Idx, RunnerId, Out, &E))
            LogWarn("PackageCatalog::CollectRunnerChainTargets", "runner '" + RunnerId + "': " + E);
    }
    (void)Error;
    return true;
}

bool HydrateNode(const NodeIndex &Idx, const std::string &LaunchNodeId, const std::map<std::string, bool> &Toggles,
                 std::string *Error, const nlohmann::ordered_json *GlobalConfigJSON)
{
    std::vector<IpfsWrapper::FetchTarget> Targets;
    if (!CollectContentTargets(Idx, LaunchNodeId, Toggles, Targets, Error)) return false;
    // Full-closure: also pool the resolved runner chain's build so the game is immediately launchable (identity = the
    // node's Declare* layers; a game hydrate pulls the whole runtime, no separate "install the runner" step).
    //Discarding this told the caller the hydrate SUCCEEDED with the runner build silently absent from the batch:
    //the package downloads, reports done, and then cannot launch — and the download UI has nothing left to say.
    if (GlobalConfigJSON && !CollectRunnerChainTargets(Idx, LaunchNodeId, *GlobalConfigJSON, Targets, Error))
        LogErr("PackageCatalog::HydrateNode", "could not resolve the runner chain for '" + LaunchNodeId
                   + "' — its runtime will NOT be fetched, so the package will download but not launch"
                   + (Error && !Error->empty() ? " (" + *Error + ")" : ""));
    if (!IpfsWrapper::FetchTargetsConcurrent(Targets, Error))
    { LogErr("PackageCatalog::HydrateNode", "hydrate failed for '" + LaunchNodeId + "'"); return false; }
    LogSucc("PackageCatalog::HydrateNode", "Hydrated node '" + LaunchNodeId + "' (" + std::to_string(Targets.size()) + " file(s)).");
    return true;
}

} // namespace PackageCatalog
