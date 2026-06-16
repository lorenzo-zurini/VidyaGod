#include "packagecatalog.h"
#include "manifestmodel.h"
#include "commonutils.h"
#include "jsonoperations.h"
#include "processenv.h"
#include "ipfswrapper.h"
#include "runnerwrapper.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStringList>
#include <set>
#include <filesystem>
#include <algorithm>

using namespace ManifestModel;   // LayerLocator / IsVfsLayer / ForEachVfsLayer / PackageHydrated / PackageIpfsCids

namespace PackageCatalog {

// ----- per-package user settings -----

nlohmann::ordered_json GetPackageUserSettings(const nlohmann::ordered_json &GlobalConfigJSON, const std::string &PackageUID)
{
    if (!GlobalConfigJSON.contains("LIBRARY")) return nlohmann::ordered_json::object();
    for (auto &Entry : GlobalConfigJSON["LIBRARY"])
        if (Entry.contains("PACKAGEUID") && std::string(Entry["PACKAGEUID"]) == PackageUID)
            return Entry.contains("USERSETTINGS") ? Entry["USERSETTINGS"] : nlohmann::ordered_json::object();
    return nlohmann::ordered_json::object();
}

void SetPackageUserSetting(nlohmann::ordered_json &GlobalConfigJSON, const std::string &PackageUID, const std::string &Key, const nlohmann::ordered_json &Value)
{
    if (!GlobalConfigJSON.contains("LIBRARY")) return;
    for (auto &Entry : GlobalConfigJSON["LIBRARY"])
        if (Entry.contains("PACKAGEUID") && std::string(Entry["PACKAGEUID"]) == PackageUID)
        {
            if (!Entry.contains("USERSETTINGS") || !Entry["USERSETTINGS"].is_object())
                Entry["USERSETTINGS"] = nlohmann::ordered_json::object();
            Entry["USERSETTINGS"][Key] = Value;
            return;
        }
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
    return QDir::cleanPath(QDir::homePath() + "/.VidyaGod/LIBRARY").toStdString();
}

std::string LibraryRootDir(const nlohmann::ordered_json &GlobalConfigJSON) { return LibraryDir(GlobalConfigJSON); }

//A repository's clone URL. Each Settings.Repositories[] entry is an object with a "PATH" (the URL), or a bare URL.
static std::string RepositoryURL(const nlohmann::ordered_json &R)
{
    if (R.is_object() && R.contains("PATH") && R["PATH"].is_string()) return std::string(R["PATH"]);
    if (R.is_string()) return std::string(R);
    return std::string();
}

//The clone directory name for a git repository: its NAME, else the URL basename minus a ".git" suffix.
static std::string GitRepoName(const nlohmann::ordered_json &R)
{
    std::string Name = R.is_object() ? R.value("NAME", std::string()) : std::string();
    if (Name.empty())
    {
        std::string U = RepositoryURL(R);
        while (!U.empty() && U.back() == '/') U.pop_back();
        const auto Slash = U.find_last_of('/');
        Name = (Slash == std::string::npos) ? U : U.substr(Slash + 1);
        const std::string Suffix = ".git";
        if (Name.size() > Suffix.size() && Name.compare(Name.size() - Suffix.size(), Suffix.size(), Suffix) == 0)
            Name = Name.substr(0, Name.size() - Suffix.size());
    }
    return Name.empty() ? std::string("repo") : Name;
}

//A repository's git working tree: LIBRARY/<name>. The clone IS the library — its dehydrated package dirs hydrate in place.
static std::string RepositoryLocalDir(const nlohmann::ordered_json &GlobalConfigJSON, const nlohmann::ordered_json &R)
{
    return QDir::cleanPath(QString::fromStdString(LibraryDir(GlobalConfigJSON) + "/" + GitRepoName(R))).toStdString();
}

std::vector<std::string> RepositoryDirs(const nlohmann::ordered_json &GlobalConfigJSON)
{
    std::vector<std::string> Dirs;
    const nlohmann::ordered_json *Settings = (GlobalConfigJSON.contains("Settings") && GlobalConfigJSON["Settings"].is_object())
                                             ? &GlobalConfigJSON["Settings"] : nullptr;
    if (Settings && Settings->contains("Repositories") && (*Settings)["Repositories"].is_array())
        for (const auto &R : (*Settings)["Repositories"])
            if (!RepositoryURL(R).empty()) Dirs.push_back(RepositoryLocalDir(GlobalConfigJSON, R));
    return Dirs;
}

// ----- catalog -----

std::vector<std::pair<nlohmann::ordered_json, std::string>> CatalogPackagesWithDir(const nlohmann::ordered_json &GlobalConfigJSON)
{
    std::vector<std::pair<nlohmann::ordered_json, std::string>> Packages;
    std::set<std::string> SeenUID; // first occurrence wins → earlier repository shadows later ones
    for (const auto &RepoDir : RepositoryDirs(GlobalConfigJSON))
    {
        QDir Dir(QString::fromStdString(RepoDir));
        if (!Dir.exists()) continue;
        for (const QString &Sub : Dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        {
            const QString PkgDir = Dir.filePath(Sub);
            nlohmann::ordered_json Pkg; std::vector<std::string> Warn;
            if (!JSONOps::AssembleManifest(PkgDir, Pkg, Warn)) continue;
            std::string Uid = Pkg.value("PACKAGEUID", std::string());
            if (!Uid.empty() && !SeenUID.insert(Uid).second) continue; // already provided by a higher-priority repo
            Packages.emplace_back(std::move(Pkg), PkgDir.toStdString());
        }
    }
    return Packages;
}

std::vector<nlohmann::ordered_json> CatalogPackages(const nlohmann::ordered_json &GlobalConfigJSON)
{
    std::vector<nlohmann::ordered_json> Packages;
    for (auto &[Pkg, Dir] : CatalogPackagesWithDir(GlobalConfigJSON)) { (void)Dir; Packages.push_back(std::move(Pkg)); }
    return Packages;
}

std::vector<nlohmann::ordered_json> RegistryRunners(const nlohmann::ordered_json &GlobalConfigJSON)
{
    std::vector<nlohmann::ordered_json> Runners;
    for (const auto &Pkg : CatalogPackages(GlobalConfigJSON))
        if (JSONOps::HasRunners(Pkg))
            for (const auto &R : Pkg["RUNNERS"]) Runners.push_back(R);
    return Runners;
}

// ----- git plumbing -----

//Runs git with the given arguments, blocking up to a couple of minutes. Returns true on a clean exit.
static bool RunGit(const QStringList &Args, const QString &WorkDir = QString())
{
    QProcess P;
    P.setProcessEnvironment(SystemToolEnv());                                   // system git must not inherit the AppImage LD_LIBRARY_PATH
    if (!WorkDir.isEmpty()) P.setWorkingDirectory(WorkDir);
    P.start("git", Args);
    if (!P.waitForStarted(10000)) return false;
    if (!P.waitForFinished(120000)) { P.kill(); P.waitForFinished(2000); return false; }
    return P.exitStatus() == QProcess::NormalExit && P.exitCode() == 0;
}

//Clones (first run) or updates a git repository into CloneDir. Self-heals from a rewritten remote history: a plain
//fast-forward pull fails there, so fall back to fetch + hard-reset, and re-clone only if even that fails.
static bool SyncGitRepository(const std::string &Url, const std::string &CloneDir)
{
    if (Url.empty()) return false;
    const QString Dir = QString::fromStdString(CloneDir);
    if (QDir(Dir + "/.git").exists())
    {
        if (RunGit({"pull", "--ff-only"}, Dir)) return true;                      // normal fast-forward
        if (RunGit({"fetch", "origin"}, Dir) && RunGit({"reset", "--hard", "@{u}"}, Dir))
            return true;                                                          // diverged/rewritten → realign
        QDir(Dir).removeRecursively();                                            // unrecoverable → re-clone below
    }
    else if (QDir(Dir).exists())
        QDir(Dir).removeRecursively();                                           // a non-git dir (old copied mirror) → replace with a clone
    QDir().mkpath(QString::fromStdString(std::filesystem::path(CloneDir).parent_path().string())); // LIBRARY root must exist
    return RunGit({"clone", "--depth", "1", QString::fromStdString(Url), Dir});
}

//Upserts a slim index entry {PACKAGEUID,PACKAGENAME,PATH,REPO} into Arr keyed by PACKAGEUID. A local (no-REPO)
//entry with the same UID wins and is left untouched (the user's own package shadows a repo copy).
static void UpsertIndexEntry(nlohmann::ordered_json &Arr, const std::string &Uid, const std::string &Repo,
                             const std::string &MirrorDir, const std::string &Name)
{
    for (auto &E : Arr)
    {
        if (E.value("PACKAGEUID", std::string()) != Uid) continue;
        if (!E.contains("REPO")) return;                                          // local entry owns this UID
        E["PACKAGENAME"] = Name;
        E["PATH"]        = MirrorDir;
        E["REPO"]        = Repo;
        E.erase("PACKAGEVERSION");                                                // drop the legacy field if present
        return;
    }
    nlohmann::ordered_json Slim;
    Slim["PACKAGEUID"]  = Uid;
    Slim["PACKAGENAME"] = Name;
    Slim["PATH"]        = MirrorDir;
    Slim["REPO"]        = Repo;
    Arr.push_back(std::move(Slim));
}

//True if any variant of a runner package has been imported (build cached + DEFPREFIX) — its "hydrated" state.
static bool RunnerPkgImported(const nlohmann::ordered_json &Pkg, const std::string &PackageDir)
{
    for (const std::string &Vid : RunnerWrapper::VariantIds(Pkg))
        if (RunnerWrapper::IsImported(Pkg, PackageDir, Vid)) return true;
    return false;
}

//Drops REPO-sourced entries whose PACKAGEUID no longer comes from any synced repo AND that hold nothing the user
//downloaded, deleting their dir (only under LibRoot). Local (no-REPO) entries and downloaded orphans are kept.
static void ReconcileIndex(nlohmann::ordered_json &Arr, const std::set<std::string> &SeenUids, const std::string &LibRoot)
{
    for (int i = (int)Arr.size() - 1; i >= 0; --i)
    {
        nlohmann::ordered_json &E = Arr[i];
        if (!E.contains("REPO")) continue;                                        // local — keep
        if (SeenUids.count(E.value("PACKAGEUID", std::string()))) continue;       // still in a repo — keep
        const std::string Path = E.value("PATH", std::string());
        //A downloaded orphan (kept): any launchable node in the bundle is hydrated locally.
        bool Downloaded = false;
        NodeIndex Idx; ManifestModel::ScanBundleNodes(Path, Idx);
        for (const auto &[NodeId, N] : Idx.Nodes)
            if (N.IsLaunchable() && NodeHydrated(Idx, NodeId)) { Downloaded = true; break; }
        if (Downloaded) continue;                                                 // downloaded orphan — keep
        if (!Path.empty() && Path.rfind(LibRoot, 0) == 0) { std::error_code Ec; std::filesystem::remove_all(Path, Ec); }
        Arr.erase(Arr.begin() + i);
    }
}

//A bundle's index identity, derived from its node files (everything-is-a-node): the representative launchable
//node's UID + title, and whether the bundle defines any launchable / runner node. Replaces the AssembleManifest
//PACKAGEUID/PACKAGENAME/HasGames/HasRunners reads.
struct BundleIdentity { std::string Uid, Name; bool HasLaunchable = false, HasRunner = false; bool Valid = false; };
static BundleIdentity ScanBundleIdentity(const std::string &BundleDir)
{
    BundleIdentity Id;
    NodeIndex Idx;
    ManifestModel::ScanBundleNodes(BundleDir, Idx);
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

void SyncRepositories(nlohmann::ordered_json &GlobalConfigJSON)
{
    if (!GlobalConfigJSON.contains("Settings") || !GlobalConfigJSON["Settings"].is_object()) return;
    if (!GlobalConfigJSON["Settings"].contains("Repositories") || !GlobalConfigJSON["Settings"]["Repositories"].is_array()) return;

    //Ensure LIBRARY exists BEFORE iterating — adding a top-level key reallocates the object's storage, so creating
    //it later would dangle a cached reference into the Repositories array.
    if (!GlobalConfigJSON.contains("LIBRARY") || !GlobalConfigJSON["LIBRARY"].is_array())
        GlobalConfigJSON["LIBRARY"] = nlohmann::ordered_json::array();

    const std::string LibRoot = LibraryDir(GlobalConfigJSON);
    std::set<std::string> SeenUid;

    for (auto &R : GlobalConfigJSON["Settings"]["Repositories"])
    {
        const std::string Url = RepositoryURL(R);
        if (Url.empty()) continue;
        const std::string RepoName = GitRepoName(R);
        const std::string Local = RepositoryLocalDir(GlobalConfigJSON, R);
        LogOut("PackageCatalog::SyncRepositories", "Syncing git repository " + Url + " -> " + Local);
        if (!SyncGitRepository(Url, Local))
            LogWarn("PackageCatalog::SyncRepositories", "git clone/pull failed for " + Url + " (using last-synced clone, if any).");

        QDir D(QString::fromStdString(Local));
        if (!D.exists()) { LogWarn("PackageCatalog::SyncRepositories", "Repository missing (skipped): " + Local); continue; }

        int Games = 0, Runners = 0;
        for (const QString &Sub : D.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        {
            const QString ClonePkgDir = D.filePath(Sub);
            const BundleIdentity Id = ScanBundleIdentity(ClonePkgDir.toStdString());
            if (!Id.Valid) continue;

            //The cloned bundle dir IS the library package — content hydrates here in place. Index it directly.
            SeenUid.insert(Id.Uid);
            UpsertIndexEntry(GlobalConfigJSON["LIBRARY"], Id.Uid, RepoName, ClonePkgDir.toStdString(), Id.Name);
            if (Id.HasLaunchable) ++Games;
            if (Id.HasRunner)     ++Runners;
        }
        LogOut("PackageCatalog::SyncRepositories", "Indexed " + Local + " ("
               + std::to_string(Games) + " game(s), " + std::to_string(Runners) + " runner(s)).");
    }

    ReconcileIndex(GlobalConfigJSON["LIBRARY"], SeenUid, LibRoot);
}

// ----- import / publish -----

bool ImportPackage(nlohmann::ordered_json &GlobalConfigJSON, const nlohmann::ordered_json &Manifest,
                   const std::string &PackageDir, std::string *Error)
{
    auto Fail = [&](const std::string &M) -> bool { if (Error) *Error = M; LogErr("PackageCatalog::ImportPackage", M); return false; };

    const std::string Uid = Manifest.value("PACKAGEUID", std::string());
    if (Uid.empty()) return Fail("package has no PACKAGEUID");
    if (!GlobalConfigJSON.contains("LIBRARY") || !GlobalConfigJSON["LIBRARY"].is_array())
        GlobalConfigJSON["LIBRARY"] = nlohmann::ordered_json::array();

    const std::filesystem::path Dest(PackageDir);                                 // the cloned package dir = the library package
    std::error_code Ec;
    int Fetched = 0;
    bool FetchFailed = false;
    std::string FetchErr;

    //Fetch each VFS layer's content in place at Dest/PATH (CID is the permanent identity; PATH the local home).
    if (Manifest.contains("COMPONENTS"))
        ForEachVfsLayer(Manifest["COMPONENTS"], [&](const nlohmann::ordered_json &S){
            if (FetchFailed) return;
            std::filesystem::path Local; std::string Cid;
            LayerLocator(S, Dest, Local, Cid);
            if (Cid.empty() || Local == Dest) return;                            // no backend, or no PATH
            if (std::filesystem::exists(Local, Ec)) return;                      // already present
            std::string Err;
            if (IpfsWrapper::FetchToPath(Cid, Local.string(), &Err).empty())
            { FetchFailed = true; FetchErr = "could not fetch layer CID " + Cid + " (" + Err + ")"; return; }
            ++Fetched;
        });
    if (FetchFailed) return Fail(FetchErr);

    //Fetch covers in place too (COVER objects are {PATH, SOURCE:{ipfs,CID}} — LayerLocator reads them like a layer).
    auto FetchCover = [&](const nlohmann::ordered_json &Holder)
    {
        if (!Holder.contains("COVER") || !Holder["COVER"].is_object()) return;
        std::filesystem::path Local; std::string Cid;
        LayerLocator(Holder["COVER"], Dest, Local, Cid);
        if (Cid.empty() || Local == Dest || std::filesystem::exists(Local, Ec)) return;
        std::string Err;
        if (IpfsWrapper::FetchToPath(Cid, Local.string(), &Err).empty())
            LogWarn("PackageCatalog::ImportPackage", "could not fetch cover CID " + Cid + " (" + Err + ")");
        else ++Fetched;
    };
    if (Manifest.contains("GAMES") && Manifest["GAMES"].is_array())
        for (const auto &G : Manifest["GAMES"])
        {
            if (!G.is_object()) continue;
            if (G.contains("METADATA") && G["METADATA"].is_object()) FetchCover(G["METADATA"]);
            FetchCover(G);
        }
    LogSucc("PackageCatalog::ImportPackage", "Hydrated package " + Uid + " in place at " + Dest.string()
            + " (" + std::to_string(Fetched) + " file(s) fetched)");

    //Ensure a LIBRARY entry points at the hydrated dir (sync usually created it already; add if missing).
    bool Found = false;
    for (auto &E : GlobalConfigJSON["LIBRARY"])
        if (E.value("PACKAGEUID", std::string()) == Uid) { E["PATH"] = Dest.string(); Found = true; break; }
    if (!Found)
    {
        nlohmann::ordered_json Slim;
        Slim["PACKAGEUID"]  = Uid;
        Slim["PACKAGENAME"] = Manifest.value("PACKAGENAME", std::string());
        Slim["PATH"]        = Dest.string();
        GlobalConfigJSON["LIBRARY"].push_back(std::move(Slim));
    }
    return true;
}

bool IsPackageImported(const nlohmann::ordered_json &GlobalConfigJSON, const nlohmann::ordered_json &Manifest)
{
    const std::string Uid = Manifest.value("PACKAGEUID", std::string());
    if (Uid.empty()) return false;
    if (!GlobalConfigJSON.contains("LIBRARY") || !GlobalConfigJSON["LIBRARY"].is_array()) return false;
    for (const auto &E : GlobalConfigJSON["LIBRARY"])
        if (E.value("PACKAGEUID", std::string()) == Uid)
            return PackageHydrated(Manifest, E.value("PATH", std::string()));     // in LIBRARY + content present locally
    return false;
}

bool PublishPackage(const std::string &PackageDir, const std::string &DehydratedDestDir, std::string *Error)
{
    auto Fail = [&](const std::string &M) -> bool { if (Error) *Error = M; LogErr("PackageCatalog::PublishPackage", M); return false; };

    std::error_code Ec;
    const std::filesystem::path Pkg(PackageDir);
    if (!std::filesystem::is_directory(Pkg, Ec)) return Fail("not a package directory: " + PackageDir);

    if (!IpfsWrapper::DaemonRunning())
        LogWarn("PackageCatalog::PublishPackage",
                "no IPFS daemon running — CIDs will be computed but content seeds to peers only once `ipfs daemon` is up.");

    int Seeded = 0, Walked = 0, Covers = 0;

    //Walk every *.json fragment directly (no assemble/decompose round-trip — preserves each subcomponent's exact
    //file placement). Content-address VFS layers AND cover assets in place; re-save only mutated fragments.
    for (const auto &Entry : std::filesystem::directory_iterator(Pkg, Ec))
    {
        if (!Entry.is_regular_file() || Entry.path().extension() != ".json") continue;
        QFile FragFile(QString::fromStdString(Entry.path().string()));
        nlohmann::ordered_json Frag;
        if (JSONOps::LoadJSON(&FragFile, &Frag)) continue;                       // LoadJSON returns true on FAILURE

        bool Mutated = false;

        //Content layers: keep PATH, add SOURCE:{ipfs,CID} (idempotent — skip those already carrying a CID).
        if (Frag.contains("COMPONENTS") && Frag["COMPONENTS"].is_array())
        for (auto &C : Frag["COMPONENTS"])
        {
            if (!C.is_object() || !C.contains("SUBCOMPONENTS") || !C["SUBCOMPONENTS"].is_array()) continue;
            for (auto &S : C["SUBCOMPONENTS"])
            {
                if (!IsVfsLayer(LayerType(S))) continue;
                ++Walked;
                std::filesystem::path Local; std::string Cid;
                LayerLocator(S, Pkg, Local, Cid);

                std::error_code Rc;
                if (!Cid.empty()) continue;                                      // already has an ipfs CID — idempotent
                if (!std::filesystem::exists(Local, Rc)) continue;               // no local content to seed
                std::string Err;
                const std::string NewCid = IpfsWrapper::AddNoCopy(Local.string(), &Err);
                if (NewCid.empty()) return Fail("could not seed layer " + Local.string() + " (" + Err + ")");

                nlohmann::ordered_json Src = (S.contains("SOURCE") && S["SOURCE"].is_object())
                                                 ? S["SOURCE"] : nlohmann::ordered_json::object();
                Src["TYPE"] = "ipfs";                                            // keep any existing SOURCE.PATH override
                Src["CID"]  = NewCid;
                S["SOURCE"] = std::move(Src);
                Mutated = true;
                ++Seeded;
            }
        }

        //Cover art: content-address the curated GAMES[].METADATA.COVER like a layer — keep the filename in PATH, add
        //SOURCE:{ipfs,CID}. Upgrades the legacy bare-string form; idempotent once a CID is present.
        auto SeedCover = [&](nlohmann::ordered_json &Holder)
        {
            if (!Holder.contains("COVER")) return;
            nlohmann::ordered_json &Cover = Holder["COVER"];
            std::string File;
            if (Cover.is_string()) File = Cover.get<std::string>();
            else if (Cover.is_object())
            {
                if (Cover.contains("SOURCE") && Cover["SOURCE"].is_object()
                    && !Cover["SOURCE"].value("CID", std::string()).empty()) return;   // already addressed
                File = Cover.value("PATH", std::string());
            }
            else return;
            if (File.empty()) return;
            std::error_code Rc;
            const std::filesystem::path Local = Pkg / File;
            if (!std::filesystem::exists(Local, Rc)) return;                      // not a local file (CID-only ref)
            std::string Err;
            const std::string NewCid = IpfsWrapper::AddNoCopy(Local.string(), &Err);
            if (NewCid.empty()) { LogWarn("PackageCatalog::PublishPackage", "could not seed cover " + Local.string() + " (" + Err + ")"); return; }
            Cover = nlohmann::ordered_json{ {"PATH", File}, {"SOURCE", {{"TYPE", "ipfs"}, {"CID", NewCid}}} };
            Mutated = true;
            ++Covers;
        };
        if (Frag.contains("GAMES") && Frag["GAMES"].is_array())
        for (auto &G : Frag["GAMES"])
        {
            if (!G.is_object()) continue;
            if (G.contains("METADATA") && G["METADATA"].is_object()) SeedCover(G["METADATA"]);
            SeedCover(G);                                                         // legacy game-level COVER
        }

        //Node files (everything-is-a-node): seed VFS layers in LAYERS + the cover in META.COVER, same as above.
        if (Frag.contains("NODE_ID") && Frag["NODE_ID"].is_string())
        {
            if (Frag.contains("LAYERS") && Frag["LAYERS"].is_array())
            for (auto &S : Frag["LAYERS"])
            {
                if (!IsVfsLayer(LayerType(S))) continue;
                ++Walked;
                std::filesystem::path Local; std::string Cid;
                LayerLocator(S, Pkg, Local, Cid);
                std::error_code Rc;
                if (!Cid.empty()) continue;                                      // already addressed — idempotent
                if (!std::filesystem::exists(Local, Rc)) continue;               // no local content to seed
                std::string Err;
                const std::string NewCid = IpfsWrapper::AddNoCopy(Local.string(), &Err);
                if (NewCid.empty()) return Fail("could not seed layer " + Local.string() + " (" + Err + ")");
                nlohmann::ordered_json Src = (S.contains("SOURCE") && S["SOURCE"].is_object()) ? S["SOURCE"] : nlohmann::ordered_json::object();
                Src["TYPE"] = "ipfs"; Src["CID"] = NewCid; S["SOURCE"] = std::move(Src);
                Mutated = true; ++Seeded;
            }
            if (Frag.contains("META") && Frag["META"].is_object()) SeedCover(Frag["META"]);
        }

        if (Mutated && !JSONOps::SaveJSON(&Frag, &FragFile))
            return Fail("could not write annotated manifest fragment: " + Entry.path().string());
    }
    LogSucc("PackageCatalog::PublishPackage", "Dehydrated " + PackageDir + " (" + std::to_string(Seeded)
            + " of " + std::to_string(Walked) + " layer(s) + " + std::to_string(Covers) + " cover(s) newly seeded)");

    //Export the dehydrated manifest, if requested — a clean manifest-only copy (no image bytes; covers travel as CIDs).
    if (!DehydratedDestDir.empty())
    {
        std::filesystem::remove_all(DehydratedDestDir, Ec);
        const int Copied = MirrorDehydrated(PackageDir, DehydratedDestDir);
        LogSucc("PackageCatalog::PublishPackage", "Exported dehydrated manifest to " + DehydratedDestDir
                + " (" + std::to_string(Copied) + " JSON fragment(s))");
    }
    return true;
}

int MirrorDehydrated(const std::string &SrcDir, const std::string &DestDir)
{
    std::error_code Ec;
    std::filesystem::create_directories(DestDir, Ec);
    int Copied = 0;
    for (const auto &Entry : std::filesystem::directory_iterator(SrcDir, Ec))
    {
        if (!Entry.is_regular_file() || Entry.path().extension() != ".json") continue;   // manifests only
        std::error_code Ce;
        std::filesystem::copy_file(Entry.path(), std::filesystem::path(DestDir) / Entry.path().filename(),
                                   std::filesystem::copy_options::overwrite_existing, Ce);
        if (Ce) LogWarn("PackageCatalog::MirrorDehydrated", "skip " + Entry.path().string() + " (" + Ce.message() + ")");
        else ++Copied;
    }
    return Copied;
}

// ----- node-graph catalog (everything-is-a-node) -----

NodeIndex BuildCatalogIndex(const nlohmann::ordered_json &GlobalConfigJSON)
{
    std::vector<std::filesystem::path> Roots;
    for (const auto &D : RepositoryDirs(GlobalConfigJSON)) Roots.emplace_back(D);
    return ManifestModel::BuildNodeIndex(Roots);
}

std::vector<std::vector<const Node*>> PresentableGroups(const NodeIndex &Idx)
{
    std::map<std::string, std::vector<const Node*>> Groups;
    for (const auto &[Id, N] : Idx.Nodes)
        if (N.IsLaunchable() && N.Presentable())
            Groups[N.GroupKey()].push_back(&N);

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

//Invoke Fn for every VFS layer in a launchable's content closure (runner build excluded), with its resolved local
//path + ipfs CID (resolved against the OWNING node's bundle dir — cross-bundle-correct).
static void ForEachContentLayer(const NodeIndex &Idx, const std::string &LaunchNodeId,
    const std::function<void(const nlohmann::ordered_json&, const std::filesystem::path&, const std::string&)> &Fn)
{
    for (const std::string &Id : ManifestModel::ResolveNodeOrder(Idx, LaunchNodeId, {}))
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

bool NodeHydrated(const NodeIndex &Idx, const std::string &LaunchNodeId)
{
    if (!Idx.Find(LaunchNodeId)) return false;
    bool AllPresent = true;
    ForEachContentLayer(Idx, LaunchNodeId, [&](const nlohmann::ordered_json&, const std::filesystem::path &Local, const std::string&){
        std::error_code Ec;
        if (!std::filesystem::exists(Local, Ec)) AllPresent = false;
    });
    return AllPresent;
}

std::vector<std::string> NodeContentCids(const NodeIndex &Idx, const std::string &LaunchNodeId)
{
    std::vector<std::string> Cids;
    std::set<std::string> Seen;
    ForEachContentLayer(Idx, LaunchNodeId, [&](const nlohmann::ordered_json&, const std::filesystem::path&, const std::string &Cid){
        if (!Cid.empty() && Seen.insert(Cid).second) Cids.push_back(Cid);
    });
    return Cids;
}

bool HydrateNode(const NodeIndex &Idx, const std::string &LaunchNodeId, std::string *Error)
{
    auto Fail = [&](const std::string &M) -> bool { if (Error) *Error = M; LogErr("PackageCatalog::HydrateNode", M); return false; };
    const Node *Launch = Idx.Find(LaunchNodeId);
    if (!Launch) return Fail("launch node not found: " + LaunchNodeId);

    bool Failed = false; std::string Err; int Fetched = 0;
    ForEachContentLayer(Idx, LaunchNodeId, [&](const nlohmann::ordered_json&, const std::filesystem::path &Local, const std::string &Cid){
        if (Failed) return;
        std::error_code Ec;
        if (std::filesystem::exists(Local, Ec)) return;                          // already present
        if (Cid.empty()) { Failed = true; Err = "missing local content with no IPFS source: " + Local.string(); return; }
        std::string E;
        if (IpfsWrapper::FetchToPath(Cid, Local.string(), &E).empty()) { Failed = true; Err = "could not fetch layer CID " + Cid + " (" + E + ")"; return; }
        ++Fetched;
    });
    if (Failed) return Fail(Err);

    if (Launch->Meta.is_object() && Launch->Meta.contains("COVER") && Launch->Meta["COVER"].is_object())
    {
        std::filesystem::path Local; std::string Cid;
        LayerLocator(Launch->Meta["COVER"], Launch->BundleDir, Local, Cid);
        std::error_code Ec;
        if (!Cid.empty() && Local != Launch->BundleDir && !std::filesystem::exists(Local, Ec))
        {
            std::string E;
            if (IpfsWrapper::FetchToPath(Cid, Local.string(), &E).empty())
                LogWarn("PackageCatalog::HydrateNode", "could not fetch cover CID " + Cid + " (" + E + ")");
            else ++Fetched;
        }
    }
    LogSucc("PackageCatalog::HydrateNode", "Hydrated node '" + LaunchNodeId + "' (" + std::to_string(Fetched) + " file(s) fetched).");
    return true;
}

} // namespace PackageCatalog
