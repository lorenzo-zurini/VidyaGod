#include "appmodel.h"
#include "asyncwork.h"   // guarded detached workers (P7: replaces raw std::thread(...).detach() with `this` captures)
#include "packagecatalog.h"
#include "nodegraph.h"   // HydratePackage — add a game by launchable CID (gigagraph sharing consumer)
#include "manifestmodel.h"
#include "containerwrapper.h"
#include "ipfswrapper.h"
#include "downloadqueue.h"   // IpfsWrapper::EnqueueBatch — the ONE rolling queue that browse fetches ride
#include "jsonoperations.h"
#include "filesystemoperations.h"   // FSOps::CheckPackageValid (local-package import)
#include "commonutils.h"

#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTimer>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

AppModel::AppModel(nlohmann::ordered_json * config, QDir * appDataDir, QObject * parent)
    : QObject(parent), Config(config), AppDataDir(appDataDir)
{
    // Apply persisted startup settings the model owns.
    auto & S = (*Config)["Settings"];
    if (S.contains("CardPixelWidth") && S["CardPixelWidth"].is_number_integer())
        CardPixelWidth = int(S["CardPixelWidth"]);
    if (S.contains("MaxConcurrentDownloads") && S["MaxConcurrentDownloads"].is_number_integer())
        IpfsWrapper::SetMaxConcurrentDownloads(int(S["MaxConcurrentDownloads"]));

    // Drop LIBRARY records for locally-added bundles the user has since moved/deleted (their PATH is gone), so the
    // library doesn't list dead tiles. Repo packages are untouched (their content may just be un-hydrated).
    if (PackageCatalog::PruneMovedLocalPackages(*Config) > 0) save();

    CatalogIndex = PackageCatalog::BuildCatalogIndex(*Config);   // node-native catalog source

    // A friend sent us their COMPLETE shared set (a snapshot, per the bilateral protocol — pushed on any share change
    // and on our explicit requestFriendLibraries). We REPLACE our record for that peer wholesale: this is the only way
    // a withdraw can't be lost (a missed single "unshare" would otherwise leave a phantom entry). "{}" = they share
    // nothing with us → drop the peer. Persist + refresh the friend-catalog view ONLY when something actually changed,
    // so a redundant snapshot (e.g. a periodic re-push) doesn't churn the disk or the UI.
    connect(FriendsManager::instance(), &FriendsManager::friendLibrary, this,
        [this](const QString & peer, const QString & libsJson, quint64 seq) { applyFriendLibrarySnapshot(peer, libsJson, seq); });

    // Self-heal on reachability: when a friend we RECEIVE from comes online, re-materialise + re-request so the catalog
    // fills regardless of the order shares/accepts/toggles happened in (a node-ready request can fire before the friend
    // connection is up and fail silently; this retries the moment they're reachable). Fires only on the online transition.
    connect(FriendsManager::instance(), &FriendsManager::friendPresence, this,
        [this](const QString & peer, bool online) { if (online) reconcileReceivedFriend(peer); });

    // A friend browse block landed in the rolling queue → (debounced) reconcile so the next level (tiles after their
    // roots) enqueues and the catalog rebuilds to show what arrived. Same queue as any transfer; we react only to CIDs
    // we ourselves enqueued for browse (FriendBrowseCids).
    connect(IpfsManager::instance(), &IpfsManager::transferFinished, this,
        [this](const QString & cid, bool ok, const QString &) { onFriendBlockLanded(cid, ok); });

    // Auto-accept ("server mode"): if enabled, accept every incoming friend request automatically and apply the
    // New-Peer defaults. Runs whether or not the Network tab is open.
    connect(FriendsManager::instance(), &FriendsManager::friendRequest, this,
        [this](const QString & peer, const QString &, const QString &) { if (autoAcceptEnabled()) acceptPeer(peer); });

    // A peer can become accepted WITHOUT our local acceptPeer() — a mutual-add "crossing" that auto-converges to
    // accepted (TestFriendMutualCrossingConverges). The New-Peer defaults (Receive/Presence/vLAN/Share) still must
    // apply, else e.g. Receive-on-by-default never kicks in. applyNewPeerDefaults is idempotent (toggles no-op if set).
    connect(FriendsManager::instance(), &FriendsManager::friendAccepted, this,
        [this](const QString & peer, const QString &, const QString &) { applyNewPeerDefaults(peer); });

    // Background serve-reliability sweep: periodically re-point any orphaned no-copy reference so content that was
    // moved/re-created MID-SESSION is repaired before (or shortly after) a peer requests it — not only on next launch.
    // Cheap when nothing is wrong (a filestore path scan, no re-seed); no-op while the node is offline.
    OrphanHealTimer = new QTimer(this);
    OrphanHealTimer->setInterval(90'000);
    connect(OrphanHealTimer, &QTimer::timeout, this, [this]{ healOrphansIfAny(); });
    OrphanHealTimer->start();
}

bool AppModel::save()
{
    return JSONOps::SaveJSON(Config, new QFile(AppDataDir->filePath("GlobalConfig.JSON")));
}

void AppModel::rebuildCatalog()
{
    CatalogIndex = PackageCatalog::BuildCatalogIndex(*Config);   // re-scan the node graph from disk
    emit catalogChanged();
}

std::pair<int, int> AppModel::importPackagesFromDir(const QString & Sel)
{
    // Collect valid package bundles at/under Sel (a bundle short-circuits the recursion).
    QStringList Paths;
    std::function<void(const QString &)> Scan = [&](const QString & D) {
        QDir Dir(D);
        if (FSOps::CheckPackageValid(&Dir)) { Paths.append(D); return; }
        for (const QString & S : Dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            Scan(QDir::cleanPath(D + QDir::separator() + S));
    };
    Scan(Sel);

    int Added = 0, Skipped = 0;
    for (const QString & Path : Paths)
    {
        // Node-native identity: a library bundle must define a launchable node (runner-only bundles aren't games).
        NodeIndex BIdx; ManifestModel::ScanBundleNodes(Path.toStdString(), BIdx);
        ManifestModel::LinkGames(BIdx);   // link variants to their tile so the representative inherits UID/TITLE
        const Node * Rep = nullptr;
        for (const auto & [Id, N] : BIdx.Nodes)
            if (N.IsLaunchable() && (!Rep || (N.Presentable() && !Rep->Presentable()))) Rep = &N;
        if (!Rep) { LogWarn("AppModel::importPackagesFromDir", "Skipping " + Path.toStdString() + ": no launchable node."); ++Skipped; continue; }

        const std::string Uid  = Rep->Uid.empty() ? Rep->NodeId : Rep->Uid;
        const std::string Name = Rep->Meta.is_object() ? Rep->Meta.value("TITLE", Rep->NodeId) : Rep->NodeId;
        bool Dup = false;
        for (auto & E : (*Config)["LIBRARY"])
            if (E.value("PACKAGEUID", std::string()) == Uid) { Dup = true; ++Skipped; break; }
        if (Dup) continue;

        nlohmann::ordered_json Slim;
        Slim["PACKAGEUID"] = Uid; Slim["PACKAGENAME"] = Name; Slim["PATH"] = Path.toStdString();

        // Per-package config (incl. PREFERRED_RUNNER) now lives in the INSTANCE file, not the LIBRARY entry — this
        // entry stays package-free. No PREFERRED_RUNNER seed is needed: PickRunnerNode already defaults to the
        // package-side RECOMMENDED runner (DeclareExec.RUNNER) when nothing is pinned, so a fresh game picks it
        // automatically, and the user overrides in the prelaunch picker (which persists to the instance).

        (*Config)["LIBRARY"].push_back(Slim);
        ++Added;
    }
    if (Added > 0) { save(); rebuildCatalog(); }
    return { Added, Skipped };
}

void AppModel::setCardPixelWidth(int w)
{
    if (w == CardPixelWidth) return;
    CardPixelWidth = w;
    (*Config)["Settings"]["CardPixelWidth"] = w;
    save();
    emit cardSizeChanged(w);
}

void AppModel::notifyCoversReady()
{
    emit coversReady();
}

bool AppModel::networkingEnabled() const
{
    const auto & S = (*Config)["Settings"];
    return S.contains("IPFS") && S["IPFS"].is_object() && S["IPFS"].value("Enabled", false);
}

void AppModel::setNetworkingEnabled(bool on)
{
    if (on == networkingEnabled()) return;
    (*Config)["Settings"]["IPFS"]["Enabled"] = on;
    save();
    emit networkingChanged(on);
}

void AppModel::removePackage(const QString & uid)
{
    const std::string Uid = uid.toStdString();
    auto & Lib = (*Config)["LIBRARY"];
    int idx = -1;
    for (int k = 0; k < int(Lib.size()); ++k)
        if (Lib[k].value("PACKAGEUID", std::string()) == Uid) { idx = k; break; }
    if (idx < 0) return;

    const std::string Path    = Lib[idx].value("PATH", std::string());
    const std::string LibRoot = PackageCatalog::LibraryRootDir(*Config);
    std::error_code Ec;
    bool Managed = false;
    if (!Path.empty() && !LibRoot.empty())
    {
        const std::string P = std::filesystem::weakly_canonical(std::filesystem::path(Path), Ec).string();
        const std::string R = std::filesystem::weakly_canonical(std::filesystem::path(LibRoot), Ec).string();
        Managed = (P.rfind(R + "/", 0) == 0);   // strictly under the library root → a managed (repo-cloned) package
    }

    if (Managed)
    {
        // De-HYDRATE the managed package: delete its content + unpin/drop-ref its CIDs, but KEEP the manifests + cover
        // so it drops back into the Catalog as re-downloadable (not deleted outright — that was the bug: it vanished
        // from the catalog). Leave the LIBRARY index entry too (it's the repo package's record; Installed Packages
        // filters by hydration, so the package disappears from there once its content is gone).
        //
        // The dehydrate itself is HEAVY (a real game's closure is thousands-to-hundreds-of-thousands of blocks: a full
        // offline DAG walk + per-block filestore deletes + a leveldb compaction), so it MUST run off the GUI thread or
        // the window freezes for its whole duration. Snapshot the node set + a private NodeIndex copy (the worker reads
        // it; the GUI owns the live one), do the work on a thread, then rebuild on the GUI thread when it lands.
        const std::string CanonPath = std::filesystem::weakly_canonical(std::filesystem::path(Path), Ec).string();
        std::vector<std::string> ToDehydrate;
        for (const auto & [NodeId, N] : CatalogIndex.Nodes)
            if (N.IsLaunchable() &&
                std::filesystem::weakly_canonical(N.BundleDir, Ec).string() == CanonPath)
                ToDehydrate.push_back(NodeId);
        if (ToDehydrate.empty()) return;   // nothing hydrated under this bundle
        auto Idx = std::make_shared<NodeIndex>(CatalogIndex);      // private copy for the worker (mirrors importRunner)
        AsyncWork::Run(this,
            [Idx, ToDehydrate = std::move(ToDehydrate)]{ for (const std::string & NodeId : ToDehydrate) PackageCatalog::DehydrateNode(*Idx, NodeId); },
            [this]{ rebuildCatalog(); });  // emits catalogChanged
        return;   // config unchanged (manifest kept); the worker rebuilds the catalog when the content is gone
    }

    // Local/portable package added from outside the library → only drop the reference; never touch the user's files.
    // This is cheap (no IPFS work), so it stays synchronous.
    Lib.erase(idx);
    save();
    rebuildCatalog();
}

// ── Sources: re-index every configured CID package source, fetching any not-yet-present one (needs the node online;
//    a no-op once fetched). Runs off-thread on a PRIVATE config copy and applies just LIBRARY back on the GUI thread —
//    the worker never mutates the live GlobalConfigJSON the GUI may be reading/writing. The model outlives every view,
//    so capturing `this` is safe. Called on node-ready (bootstrap of the default runners source) and "Sync now". ──

// Give the node the 3-level meta-CIDs (collections = Settings.PackageSources[].CID, packages = Settings.PackageCids
// values) so its seed announce goes out ordered — the shareable units first — with content being every other pinned
// root. Called on node-ready + whenever sources/package CIDs change; idempotent on the node side.
void AppModel::pushSeedLevels()
{
    std::vector<std::string> Collections, Packages;
    const auto & S = (*Config)["Settings"];
    if (S.contains("PackageSources") && S["PackageSources"].is_array())
        for (const auto & Src : S["PackageSources"])
        {
            const std::string Cid = Src.is_object() ? Src.value("CID", std::string())
                                  : (Src.is_string() ? Src.get<std::string>() : std::string());
            if (!Cid.empty()) Collections.push_back(Cid);
        }
    if (S.contains("PackageCids") && S["PackageCids"].is_object())
        for (const auto & [Key, Val] : S["PackageCids"].items())
            if (Val.is_string() && !Val.get<std::string>().empty()) Packages.push_back(Val.get<std::string>());
    IpfsWrapper::SetSeedLevels(Collections, Packages);
}

void AppModel::syncSources()
{
    auto Cfg = std::make_shared<nlohmann::ordered_json>(*Config);
    // Snapshot LIBRARY before the sync mutates it, so we can tell whether anything actually changed. On the common
    // launch (sources already present + unchanged) SyncPackageSources is a no-op, so we must NOT rebuild the catalog:
    // the ctor already built it, and a redundant BuildCatalogIndex here — posted to the MAIN thread while the IPFS
    // node is simultaneously starting up — is what froze the UI for several seconds on launch. When it DID change
    // (first run / a source CID bump), the worker builds the new index and the completion only swaps it in.
    auto Changed  = std::make_shared<bool>(false);
    auto NewIndex = std::make_shared<NodeIndex>();
    AsyncWork::Run(this,
        [Cfg, Changed, NewIndex]{
            const nlohmann::ordered_json OrigLib = Cfg->value("LIBRARY", nlohmann::ordered_json::array());
            PackageCatalog::SyncPackageSources(*Cfg);   // fetch-if-missing + index CID package sources (no-op offline once fetched)
            *Changed = Cfg->value("LIBRARY", nlohmann::ordered_json::array()) != OrigLib;
            if (*Changed) *NewIndex = PackageCatalog::BuildCatalogIndex(*Cfg);   // heavy scan OFF the main thread
        },
        [this, Cfg, Changed, NewIndex]{
            if (*Changed)
            {
                (*Config)["LIBRARY"] = (*Cfg)["LIBRARY"];
                save();
                CatalogIndex = std::move(*NewIndex);   // cheap swap on the main thread (build already done)
                emit catalogChanged();
                emit packageSourcesChanged();
            }
            healOrphansIfAny();            // re-point any orphaned no-copy refs so the node can actually SERVE its content
            pushSeedLevels();              // node-ready → announce our seeded content to the DHT, meta-CIDs first
            pushLanRoster();               // node-ready → apply the persisted Virtual-LAN roster (excluded members)
            // A source that couldn't be fetched (hostile network, provider unreachable — the node-side retries are
            // deliberately bounded so this worker can't wedge) must not be abandoned until the next app start:
            // re-run the sync in a minute. Sources already materialized are skipped, so the retry is cheap.
            if (!SyncRetryPending && PackageCatalog::HasMissingSources(*Config) && IpfsWrapper::DaemonRunning())
            {
                SyncRetryPending = true;
                QTimer::singleShot(60000, this, [this]{ SyncRetryPending = false; if (IpfsWrapper::DaemonRunning()) syncSources(); });
            }
        });
}

// Push Settings.LanExcludedPeers (the GLOBAL Virtual-LAN roster's un-ticked members) into the node. Called at
// node-ready; the launch window's ticks call IpfsWrapper::SetLanExcluded directly on change.
void AppModel::pushLanRoster()
{
    std::vector<std::string> Ex;
    const auto & S = (*Config)["Settings"];
    if (S.contains("LanExcludedPeers") && S["LanExcludedPeers"].is_array())
        for (const auto & P : S["LanExcludedPeers"])
            if (P.is_string()) Ex.push_back(P.get<std::string>());
    IpfsWrapper::SetLanExcluded(Ex);
}

void AppModel::healOrphansIfAny()
{
    bool Expected = false;
    if (!HealInFlight.compare_exchange_strong(Expected, true)) return;   // single-flight: a heal is already running
    auto Cfg = std::make_shared<nlohmann::ordered_json>(*Config);
    // NOTE: the worker must not touch members (KnownUnhealable is main-thread state) — it computes into
    // shared locals; the completion applies them. HealInFlight is atomic and may clear from the worker.
    auto Unhealable = std::make_shared<std::set<std::string>>(KnownUnhealable);
    auto Healed     = std::make_shared<bool>(false);
    AsyncWork::Run(this,
        [Cfg, Unhealable, Healed, this]{
            const std::vector<std::string> Orphans = IpfsWrapper::OrphanedRefPaths();
            // Drop any previously-unhealable path that's no longer orphaned (content was restored), then treat the
            // rest as "known gone" so we don't re-run the (heavier) heal for content that genuinely can't be found.
            std::set<std::string> OrphanSet(Orphans.begin(), Orphans.end());
            for (auto It = Unhealable->begin(); It != Unhealable->end(); )
                It = OrphanSet.count(*It) ? std::next(It) : Unhealable->erase(It);
            std::vector<std::string> Fresh;
            for (const std::string & P : Orphans) if (!Unhealable->count(P)) Fresh.push_back(P);
            if (!Fresh.empty())
            {
                LogOut("AppModel::healOrphansIfAny", std::to_string(Fresh.size()) + " orphaned reference(s) detected — re-pointing");
                // The loop below IS the heal: an additive SeedDirectory per source dir re-points orphaned refs and
                // collects what it could not fix. This used to ALSO call HealSourceContent first, whose cheap pass
                // is the very same per-dir SeedDirectory — so every heal swept all 1150 referenced files TWICE
                // (each sweep a cidMissing walk through the Go node's leveldb + sha256), and the report the extra
                // call produced was discarded. Startup profiling found the node burning steady CPU on exactly this.
                for (const std::string &Dir : PackageCatalog::PackageSourceDirs(*Cfg))
                {
                    std::vector<PackageCatalog::SeedFailure> Bad;
                    PackageCatalog::SeedDirectory(Dir, {}, nullptr, false, false, false, &Bad);
                    for (const auto &F : Bad)
                        QMetaObject::invokeMethod(this, [this, F]{
                            emit contentUnservable(QString::fromStdString(F.RecordedCid),
                                QString::fromStdString("content no longer matches the published CID (" + F.Path + ")"));
                        }, Qt::QueuedConnection);
                }
                // Whatever is STILL orphaned after the heal is content truly gone (no on-disk copy to re-point to):
                // remember it so subsequent sweeps skip the heavy re-seed until something changes.
                Unhealable->clear();
                for (const std::string & P : IpfsWrapper::OrphanedRefPaths()) Unhealable->insert(P);
                *Healed = true;
            }
            HealInFlight.store(false);
        },
        [this, Unhealable, Healed]{
            KnownUnhealable = std::move(*Unhealable);
            if (*Healed) emit ipfsHealthChanged();
        });
}

// ── Package sources (IPFS folder CIDs): add fetches the dehydrated tree off-thread (requires the node online — a fetch
//    failure is surfaced via packageSourceFailed, the source stays configured so a later sync picks it up); remove is
//    cheap (drop config entry + fetched dir + LIBRARY entries). ──
bool AppModel::addPackageSource(const QString & cid, const QString & name)
{
    if (!PackageCatalog::AddPackageSource(*Config, cid.trimmed().toStdString(), name.trimmed().toStdString()))
        return false;   // empty or duplicate CID — caller warns
    save();
    emit packageSourcesChanged();   // INSTANT feedback: the source appears in the list/IPFS tab now (as pending); the
                                    // off-thread fetch below re-emits when its manifests + catalog have landed.
    auto Cfg = std::make_shared<nlohmann::ordered_json>(*Config);
    auto Err = std::make_shared<std::string>();
    AsyncWork::Run(this,
        [Cfg, Err]{ PackageCatalog::SyncPackageSources(*Cfg, Err.get()); },
        [this, Cfg, Err]{
            (*Config)["LIBRARY"] = (*Cfg)["LIBRARY"];
            save();
            rebuildCatalog();
            emit packageSourcesChanged();
            pushSeedLevels();   // a new source's collection CID should announce promptly (meta-first)
            if (!Err->empty()) emit packageSourceFailed(QString::fromStdString(*Err));
        });
    return true;
}

void AppModel::removePackageSource(int index)
{
    // Abort any in-flight fetch/retry of this source's CID — a removed source must not keep a sync worker busy.
    {
        const auto & S = (*Config)["Settings"];
        if (S.contains("PackageSources") && S["PackageSources"].is_array()
            && index >= 0 && index < (int)S["PackageSources"].size())
        {
            const auto & Src = S["PackageSources"][index];
            const std::string Cid = Src.is_object() ? Src.value("CID", std::string())
                                  : Src.is_string() ? Src.get<std::string>() : std::string();
            if (!Cid.empty()) IpfsWrapper::RequestCancel(Cid);
        }
    }
    PackageCatalog::RemovePackageSource(*Config, index);
    save();
    rebuildCatalog();
    emit packageSourcesChanged();
}

void AppModel::addGameByCid(const QString & launchableCid)
{
    const std::string Cid = launchableCid.trimmed().toStdString();
    if (Cid.empty()) { emit gameAddFailed("empty CID"); return; }
    const std::filesystem::path Dest = PackageCatalog::LibraryRootDir(*Config);
    auto Dir = std::make_shared<std::string>();
    auto Err = std::make_shared<std::string>();
    AsyncWork::Run(this,
        [Dest, Cid, Dir, Err]{ *Dir = NodeGraph::HydratePackage(Dest, Cid, Err.get()); },
        [this, Dir, Err]{
            if (Dir->empty()) { emit gameAddFailed(QString::fromStdString(*Err)); return; }
            rebuildCatalog();     // the new checkout is on-disk → it appears in the Library
            pushSeedLevels();     // seed its content so we become a provider too
            emit gameAdded(QString::fromStdString(*Dir));
        });
}

QStringList AppModel::myLibraryNames() const
{
    QStringList Out;
    if (Config->contains("Libraries") && (*Config)["Libraries"].is_object())
        for (const auto & [Name, _] : (*Config)["Libraries"].items()) Out << QString::fromStdString(Name);
    return Out;
}

void AppModel::shareLibraryWithFriend(const QString & peer, const QString & lib)
{
    const std::string P = peer.toStdString(), L = lib.toStdString();
    std::vector<std::string> Cids;   // the library's launchable CIDs (from the last publish)
    if (Config->contains("Libraries") && (*Config)["Libraries"].is_object()
        && (*Config)["Libraries"].contains(L) && (*Config)["Libraries"][L].is_array())
        for (const auto & C : (*Config)["Libraries"][L]) if (C.is_string()) Cids.push_back(C.get<std::string>());
    std::string Err;
    if (!IpfsWrapper::ShareLibrary(P, L, Cids, &Err))
    { LogWarn("AppModel::shareLibraryWithFriend", "share '" + L + "' with " + P + " failed: " + Err); return; }
    auto & Sh = (*Config)["Sharing"][P];
    if (!Sh.is_array()) Sh = nlohmann::ordered_json::array();
    bool Found = false;
    for (const auto & X : Sh) if (X == L) { Found = true; break; }
    if (!Found) Sh.push_back(L);
    save();
}

void AppModel::stopSharingWithFriend(const QString & peer, const QString & lib)
{
    const std::string P = peer.toStdString(), L = lib.toStdString();
    std::string Err;
    IpfsWrapper::UnshareLibrary(P, L, &Err);
    if ((*Config).contains("Sharing") && (*Config)["Sharing"].contains(P) && (*Config)["Sharing"][P].is_array())
    {
        auto & Sh = (*Config)["Sharing"][P];
        for (auto It = Sh.begin(); It != Sh.end(); ++It) if (*It == L) { Sh.erase(It); break; }
        if (Sh.empty()) (*Config)["Sharing"].erase(P);
    }
    save();
}

bool AppModel::isSharingWithFriend(const QString & peer, const QString & lib) const
{
    const std::string P = peer.toStdString(), L = lib.toStdString();
    if (!Config->contains("Sharing") || !(*Config)["Sharing"].is_object()
        || !(*Config)["Sharing"].contains(P) || !(*Config)["Sharing"][P].is_array())
        return false;
    for (const auto & X : (*Config)["Sharing"][P]) if (X == L) return true;
    return false;
}

void AppModel::requestFriendLibraries(const QString & peer)
{
    std::string Err;
    if (!IpfsWrapper::RequestFriendLibraries(peer.toStdString(), &Err))
        LogWarn("AppModel::requestFriendLibraries", "request to " + peer.toStdString() + " failed: " + Err);
}

void AppModel::applyFriendLibrarySnapshot(const QString & peer, const QString & libsJson, quint64 seq)
{
    const std::string P = peer.toStdString();
    // SEQ AUTHORITY (last-writer-wins): snapshots ride independent, concurrently-handled streams, so a stale one can be
    // delivered/emitted after a fresher one. Keep only the highest stamp per peer; drop anything older. seq==0
    // (unstamped) always applies. This is where ordering is enforced — a Go-side receive gate can't (its emit is
    // off-lock). Update the high-water mark only after the payload validates, so a malformed stale message can't bump it.
    if (seq != 0)
    {
        const auto It = FriendLibSeq.find(P);
        if (It != FriendLibSeq.end() && seq <= It->second) return;   // reordered straggler / duplicate → ignore
    }
    nlohmann::ordered_json Libs;                        // {name:[cid,…]} — parse + sanitize the snapshot
    try { Libs = nlohmann::ordered_json::parse(libsJson.toStdString()); }
    catch (const std::exception & Ex)
    { LogWarn("AppModel::friendLibrary", "bad snapshot from " + P + ": " + Ex.what()); return; }
    if (!Libs.is_object()) return;                      // malformed → ignore, keep what we have
    if (seq != 0) FriendLibSeq[P] = seq;                // payload is well-formed → advance the high-water mark
    nlohmann::ordered_json Clean = nlohmann::ordered_json::object();
    for (const auto & [Name, Arr] : Libs.items())
    {
        if (!Arr.is_array()) continue;
        nlohmann::ordered_json C = nlohmann::ordered_json::array();
        for (const auto & X : Arr) if (X.is_string()) C.push_back(X.get<std::string>());
        if (!C.empty()) Clean[Name] = std::move(C);
    }
    auto & FL = (*Config)["FriendLibraries"];
    if (!FL.is_object()) FL = nlohmann::ordered_json::object();
    const bool Had = FL.contains(P);
    if (Clean.empty())
    {
        if (!Had) return;                               // nothing to drop, nothing changed
        FL.erase(P);
    }
    else
    {
        if (Had && FL[P] == Clean) return;              // identical snapshot → no-op (no disk/UI churn)
        FL[P] = std::move(Clean);
    }
    save();
    emit friendCatalogChanged();
    if (isReceivingFrom(peer)) writeFriendStubsAsync(peer);   // materialise/refresh their browsable catalog stubs
}

void AppModel::reRegisterShares()
{
    // Go's per-friend share table is in-memory only; config["Sharing"] = {peer:[lib,…]} is the durable record. Replay
    // it into Go with the current config["Libraries"] CIDs so a restart or a re-publish (which can move launchable CIDs)
    // keeps the seeder half alive. A share whose library no longer exists pushes an EMPTY set (an authoritative
    // "nothing here" snapshot for that lib name — harmless; the friend just sees it empty until we re-populate).
    if (!Config->contains("Sharing") || !(*Config)["Sharing"].is_object()) return;
    const auto & Libs = (*Config)["Libraries"];
    for (const auto & [P, Names] : (*Config)["Sharing"].items())
    {
        if (!Names.is_array()) continue;
        for (const auto & N : Names)
        {
            if (!N.is_string()) continue;
            const std::string L = N.get<std::string>();
            std::vector<std::string> Cids;
            if (Libs.is_object() && Libs.contains(L) && Libs[L].is_array())
                for (const auto & C : Libs[L]) if (C.is_string()) Cids.push_back(C.get<std::string>());
            std::string Err;
            if (!IpfsWrapper::ShareLibrary(P, L, Cids, &Err))
                LogWarn("AppModel::reRegisterShares", "re-share '" + L + "' with " + P + " failed: " + Err);
        }
    }
}

bool AppModel::hasFriendLibraries(const QString & peer) const
{
    const std::string P = peer.toStdString();
    return Config->contains("FriendLibraries") && (*Config)["FriendLibraries"].is_object()
        && (*Config)["FriendLibraries"].contains(P) && (*Config)["FriendLibraries"][P].is_object()
        && !(*Config)["FriendLibraries"][P].empty();
}

void AppModel::reconcileReceivedFriend(const QString & peer)
{
    // Make the receiver's view independent of WHEN the snapshot arrived. Two idempotent, self-healing steps:
    if (!isReceivingFrom(peer)) return;
    // 1. Re-materialise from what we already have — shows cached tiles at once, and RETRIES a browse-fetch that failed
    //    while the peer was offline (writeFriendStubsAsync is per-peer serialised, so repeated calls coalesce). A fresh
    //    library_req reply would NO-OP when the snapshot is unchanged (seq high-water), so this is what retries the fetch.
    if (hasFriendLibraries(peer)) writeFriendStubsAsync(peer);
    // 2. Re-request in case their shares changed while we were away (a CHANGED snapshot re-triggers materialisation).
    requestFriendLibraries(peer);
}

void AppModel::reconcileReceivedLibraries()
{
    if (!Config->contains("ReceiveFrom") || !(*Config)["ReceiveFrom"].is_array()) return;
    for (const auto & X : (*Config)["ReceiveFrom"])
        if (X.is_string()) reconcileReceivedFriend(QString::fromStdString(X.get<std::string>()));
}

void AppModel::forgetFriend(const QString & peer)
{
    const std::string P = peer.toStdString();
    bool Changed = false;
    if (Config->contains("Sharing") && (*Config)["Sharing"].is_object() && (*Config)["Sharing"].contains(P))
    { (*Config)["Sharing"].erase(P); Changed = true; }   // Go already dropped its side via VgFriendRemove→purgeShares
    if (Config->contains("FriendLibraries") && (*Config)["FriendLibraries"].is_object()
        && (*Config)["FriendLibraries"].contains(P))
    { (*Config)["FriendLibraries"].erase(P); Changed = true; }   // drops the peer's folded browse tiles on rebuild
    // Legacy cleanup: drop any old "friend:<peer>:*" PackageSources (retired stub model) — KEEP anything installed.
    // Done inline (not via stopReceivingFromFriend) so forgetFriend refreshes the UI exactly ONCE, after every erase.
    if (Config->contains("Settings") && (*Config)["Settings"].is_object()
        && (*Config)["Settings"].contains("PackageSources") && (*Config)["Settings"]["PackageSources"].is_array())
    {
        const std::string Prefix = "friend:" + P + ":";
        auto & Arr = (*Config)["Settings"]["PackageSources"];
        for (int i = (int)Arr.size() - 1; i >= 0; --i)
        {
            const std::string C = (Arr[i].is_object() && Arr[i].contains("CID") && Arr[i]["CID"].is_string())
                                  ? Arr[i]["CID"].get<std::string>() : std::string();
            if (C.rfind(Prefix, 0) != 0) continue;
            PackageCatalog::RemovePackageSource(*Config, i, /*PreserveInstalled=*/true);
            Changed = true;
        }
    }
    auto EraseFrom = [&](const char * Key, nlohmann::ordered_json & Root) {
        if (Root.contains(Key) && Root[Key].is_array())
            for (auto It = Root[Key].begin(); It != Root[Key].end(); ++It)
                if (*It == P) { Root[Key].erase(It); Changed = true; break; }
    };
    EraseFrom("ReceiveFrom", *Config);
    bool DenyChanged = false;
    if (Config->contains("PresenceDeny") && (*Config)["PresenceDeny"].is_array())
        for (auto It = (*Config)["PresenceDeny"].begin(); It != (*Config)["PresenceDeny"].end(); ++It)
            if (*It == P) { (*Config)["PresenceDeny"].erase(It); Changed = true; DenyChanged = true; break; }
    if (Config->contains("Settings") && (*Config)["Settings"].is_object())
        EraseFrom("LanExcludedPeers", (*Config)["Settings"]);
    FriendLibSeq.erase(P);   // forget the seq high-water mark too, so a re-add starts fresh (no stale-drop of the first snapshot)
    if (DenyChanged) pushPresenceDeny();
    if (Changed) { save(); rebuildCatalog(); emit friendCatalogChanged(); }
}

// ── Network tab per-peer toggles ─────────────────────────────────────────────

static bool ArrHas(const nlohmann::ordered_json & Arr, const std::string & V)
{
    if (!Arr.is_array()) return false;
    for (const auto & X : Arr) if (X == V) return true;
    return false;
}

bool AppModel::isReceivingFrom(const QString & peer) const
{
    return Config->contains("ReceiveFrom") && ArrHas((*Config)["ReceiveFrom"], peer.toStdString());
}

void AppModel::setReceivingFrom(const QString & peer, bool on)
{
    const std::string P = peer.toStdString();
    auto & RF = (*Config)["ReceiveFrom"];
    if (!RF.is_array()) RF = nlohmann::ordered_json::array();
    const bool Present = ArrHas(RF, P);
    if (on == Present) return;                       // no change
    if (on) RF.push_back(P);
    else    for (auto It = RF.begin(); It != RF.end(); ++It) if (*It == P) { RF.erase(It); break; }
    save();
    if (on) requestFriendLibraries(peer);            // pull their current snapshot → stubs written on arrival
    else    stopReceivingFromFriend(peer);           // drop what we've already materialised
}

void AppModel::stopReceivingFromFriend(const QString & peer)
{
    const std::string P = peer.toStdString();
    bool Changed = false;
    // Dropping the peer's snapshot removes its folded browse tiles from the catalog on the next rebuild (the catalog
    // only folds in FriendLibraries for peers we still Receive from).
    if (Config->contains("FriendLibraries") && (*Config)["FriendLibraries"].is_object()
        && (*Config)["FriendLibraries"].contains(P))
    { (*Config)["FriendLibraries"].erase(P); Changed = true; }
    FriendLibSeq.erase(P);
    // Legacy cleanup: drop any old "friend:<peer>:*" PackageSources from the retired stub model — KEEP anything the
    // user already installed (converted to a local package, files intact).
    if (Config->contains("Settings") && (*Config)["Settings"].is_object()
        && (*Config)["Settings"].contains("PackageSources") && (*Config)["Settings"]["PackageSources"].is_array())
    {
        const std::string Prefix = "friend:" + P + ":";
        auto & Arr = (*Config)["Settings"]["PackageSources"];
        for (int i = (int)Arr.size() - 1; i >= 0; --i)
        {
            const std::string C = (Arr[i].is_object() && Arr[i].contains("CID") && Arr[i]["CID"].is_string())
                                  ? Arr[i]["CID"].get<std::string>() : std::string();
            if (C.rfind(Prefix, 0) != 0) continue;
            PackageCatalog::RemovePackageSource(*Config, i, /*PreserveInstalled=*/true);
            Changed = true;
        }
    }
    if (Changed) { save(); rebuildCatalog(); emit friendCatalogChanged(); }
}

bool AppModel::isPresenceSharedWith(const QString & peer) const
{
    return !(Config->contains("PresenceDeny") && ArrHas((*Config)["PresenceDeny"], peer.toStdString()));
}

void AppModel::setPresenceSharedWith(const QString & peer, bool on)
{
    const std::string P = peer.toStdString();
    auto & D = (*Config)["PresenceDeny"];
    if (!D.is_array()) D = nlohmann::ordered_json::array();
    const bool Denied = ArrHas(D, P);
    if (on && Denied)  { for (auto It = D.begin(); It != D.end(); ++It) if (*It == P) { D.erase(It); break; } }
    else if (!on && !Denied) D.push_back(P);
    else return;                                     // no change
    save();
    pushPresenceDeny();
}

void AppModel::pushPresenceDeny()
{
    std::vector<std::string> D;
    if (Config->contains("PresenceDeny") && (*Config)["PresenceDeny"].is_array())
        for (const auto & X : (*Config)["PresenceDeny"]) if (X.is_string()) D.push_back(X.get<std::string>());
    IpfsWrapper::SetPresenceDeny(D);
}

bool AppModel::isInVlan(const QString & peer) const
{
    if (!Config->contains("Settings") || !(*Config)["Settings"].is_object()) return true;
    return !ArrHas((*Config)["Settings"].value("LanExcludedPeers", nlohmann::ordered_json::array()), peer.toStdString());
}

void AppModel::setInVlan(const QString & peer, bool on)
{
    const std::string P = peer.toStdString();
    auto & E = (*Config)["Settings"]["LanExcludedPeers"];
    if (!E.is_array()) E = nlohmann::ordered_json::array();
    const bool Excluded = ArrHas(E, P);
    if (on && Excluded)  { for (auto It = E.begin(); It != E.end(); ++It) if (*It == P) { E.erase(It); break; } }
    else if (!on && !Excluded) E.push_back(P);
    else return;                                     // no change
    save();
    pushLanRoster();
}

void AppModel::acceptPeer(const QString & peer)
{
    std::string Err;
    if (!IpfsWrapper::FriendAccept(peer.toStdString(), &Err))
    { LogWarn("AppModel::acceptPeer", "accept " + peer.toStdString() + " failed: " + Err); return; }
    applyNewPeerDefaults(peer);
}

void AppModel::applyNewPeerDefaults(const QString & peer)
{
    setPresenceSharedWith(peer, newPeerDefault("presence"));
    setInVlan(peer, newPeerDefault("vlan"));
    for (const QString & Lib : newPeerShareDefaults()) shareLibraryWithFriend(peer, Lib);
    if (newPeerDefault("receive")) setReceivingFrom(peer, true);
}

bool AppModel::autoAcceptEnabled() const { return Config->value("AutoAcceptPeers", false); }

void AppModel::setAutoAcceptEnabled(bool on)
{
    if (on == autoAcceptEnabled()) return;
    (*Config)["AutoAcceptPeers"] = on;
    save();
}

bool AppModel::newPeerDefault(const QString & key) const
{
    const std::string K = key.toStdString();
    const bool Fallback = (K == "presence");         // presence ON by default; receive / vlan OFF
    if (!Config->contains("NewPeerDefaults") || !(*Config)["NewPeerDefaults"].is_object()) return Fallback;
    return (*Config)["NewPeerDefaults"].value(K, Fallback);
}

void AppModel::setNewPeerDefault(const QString & key, bool on)
{
    auto & D = (*Config)["NewPeerDefaults"];
    if (!D.is_object()) D = nlohmann::ordered_json::object();
    D[key.toStdString()] = on;
    save();
}

QStringList AppModel::newPeerShareDefaults() const
{
    QStringList Out;
    if (Config->contains("NewPeerDefaults") && (*Config)["NewPeerDefaults"].is_object()
        && (*Config)["NewPeerDefaults"].contains("share") && (*Config)["NewPeerDefaults"]["share"].is_array())
        for (const auto & X : (*Config)["NewPeerDefaults"]["share"])
            if (X.is_string()) Out << QString::fromStdString(X.get<std::string>());
    return Out;
}

void AppModel::setNewPeerShareDefault(const QString & lib, bool on)
{
    auto & D = (*Config)["NewPeerDefaults"];
    if (!D.is_object()) D = nlohmann::ordered_json::object();
    auto & Sh = D["share"];
    if (!Sh.is_array()) Sh = nlohmann::ordered_json::array();
    const std::string L = lib.toStdString();
    const bool In = ArrHas(Sh, L);
    if (on && !In) Sh.push_back(L);
    else if (!on && In) { for (auto It = Sh.begin(); It != Sh.end(); ++It) if (*It == L) { Sh.erase(It); break; } }
    else return;
    save();
}

void AppModel::writeFriendStubsAsync(const QString & peer)
{
    // Enqueue the peer's shared node CIDs into the ONE rolling queue (IpfsWrapper::EnqueueBatch) as BLOCK fetches: they
    // appear in the IPFS transfer list, are retried by the dispatcher, and reach the friend via warmFriends — exactly
    // like a file transfer, no parallel path. The catalog reads landed blocks from the blockstore (BuildCatalogIndex,
    // LocalOnly). Enqueue only what's NOT already local: missing roots, plus the LIBRARYITEM tiles of roots we already
    // have (a card needs its tile). As roots land, transferFinished re-runs this (debounced) so their tiles follow.
    if (!isReceivingFrom(peer)) return;
    const std::string P = peer.toStdString();
    std::vector<std::string> Roots;
    if (Config->contains("FriendLibraries") && (*Config)["FriendLibraries"].is_object()
        && (*Config)["FriendLibraries"].contains(P) && (*Config)["FriendLibraries"][P].is_object())
        for (const auto & [Lib, Arr] : (*Config)["FriendLibraries"][P].items())
            if (Arr.is_array())
                for (const auto & X : Arr) if (X.is_string()) Roots.push_back(X.get<std::string>());
    if (Roots.empty()) return;

    const std::filesystem::path Browse =
        std::filesystem::path(PackageCatalog::LibraryRootDir(*Config)).parent_path() / ".vgbrowse";
    auto Target = [&](const std::string & Cid) {
        FriendBrowseCids.insert(Cid);
        return IpfsWrapper::FetchTarget{ Cid, (Browse / Cid).string(), /*Optional=*/true, /*Dir=*/false, /*Block=*/true };
    };

    // LOCAL reads only (blockstore, no network — NOT a fetch): which roots/tiles are already present.
    const NodeIndex LocalRoots = NodeGraph::BuildFrozenIndex(Roots, nullptr, /*Shallow=*/true, /*LocalOnly=*/true);
    std::vector<IpfsWrapper::FetchTarget> Batch;
    std::vector<std::string> TileCids;
    for (const std::string & R : Roots)
    {
        const Node * N = LocalRoots.Find(R);
        if (N) { if (!N->LibraryItem.empty()) TileCids.push_back(N->LibraryItem); }
        else     Batch.push_back(Target(R));   // missing root -> enqueue into the rolling queue
    }
    if (!TileCids.empty())
    {
        const NodeIndex LocalTiles = NodeGraph::BuildFrozenIndex(TileCids, nullptr, /*Shallow=*/true, /*LocalOnly=*/true);
        for (const std::string & T : TileCids)
            if (!LocalTiles.Find(T)) Batch.push_back(Target(T));   // missing tile -> enqueue
    }
    if (!Batch.empty()) IpfsWrapper::EnqueueBatch(Batch);
    rebuildCatalog();               // show whatever is already local now
    emit friendCatalogChanged();
}

void AppModel::onFriendBlockLanded(const QString & cid, bool /*ok*/)
{
    if (!FriendBrowseCids.count(cid.toStdString())) return;   // not one of our browse blocks -> ignore
    if (FriendReconcilePending) return;                       // coalesce a burst of completions into one reconcile
    FriendReconcilePending = true;
    QTimer::singleShot(400, this, [this] {
        FriendReconcilePending = false;
        reconcileReceivedLibraries();   // re-enqueue the next level (tiles after roots) + rebuild; convergent
    });
}

void AppModel::publishLibraries()
{
    // Gigagraph publish: freeze the on-disk library into dag-json blocks and STORE them (DagPut → pinned/announced/
    // seedable), then record the shareable list of launchable root CIDs. No IPNS — the list is off-IPFS VidyaGod data.
    auto Cfg  = std::make_shared<nlohmann::ordered_json>(*Config);
    auto Cids = std::make_shared<std::vector<std::string>>();
    auto Err  = std::make_shared<std::string>();
    AsyncWork::Run(this,
        [Cfg, Cids, Err]{ *Cids = PackageCatalog::PublishLibrary(*Cfg, Err.get()); },
        [this, Cfg, Cids, Err]{
            if (Cids->empty()) { emit libraryPublishFailed(QString::fromStdString(*Err)); return; }
            (*Config)["PublishedList"] = (*Cfg)["PublishedList"];   // adopt the freshly-written flat list…
            (*Config)["Libraries"]     = (*Cfg)["Libraries"];       // …AND the per-library grouping (drives Share ▾)
            save();
            pushSeedLevels();
            reRegisterShares();   // re-push every active share with the fresh CIDs (a re-publish can move them)
            emit libraryPublished(QString::fromStdString(IpfsWrapper::FriendCode()),
                                  QString::number(static_cast<int>(Cids->size())) + " game(s)",
                                  QString());
        });
}

void AppModel::planSourceUpgrade(const QString & name, const QString & cid)
{
    // Planning FETCHES the new manifest tree (to a staging dir) and diffs it — network + disk, so off the GUI thread.
    // It mutates nothing, so a plain copy of the config is enough and a failure leaves the live source untouched.
    auto Cfg  = std::make_shared<nlohmann::ordered_json>(*Config);
    auto Plan = std::make_shared<PackageCatalog::SourceUpgradePlan>();
    auto Err  = std::make_shared<std::string>();
    const std::string Name = name.toStdString(), Cid = cid.toStdString();
    AsyncWork::Run(this,
        [Cfg, Plan, Err, Name, Cid]{ *Plan = PackageCatalog::PlanSourceUpgrade(*Cfg, Name, Cid, Err.get()); },
        [this, Plan, Err]{
            if (!Plan->Valid)
            {
                emit packageSourceFailed(QString::fromStdString(
                    Err->empty() ? std::string("could not plan the upgrade") : *Err));
                return;
            }
            PendingUpgrade = Plan;
            auto Gb = [](long long B) { return QString::number(double(B) / 1e9, 'f', 2) + " GB"; };
            const bool Unrelated = Plan->SharedPackages == 0 && Plan->OldPackages > 0;
            QString S = QString("%1\n    %2\n→ %3\n\n"
                                "Packages: %4 → %5 (%6 shared)\n"
                                "Manifests: +%7 changed %8 removed %9\n\n"
                                "Kept (not re-downloaded): %10 file(s), %11\n"
                                "Moved (renamed, not re-downloaded): %12 file(s)\n"
                                "To download on demand: %13 item(s)\n"
                                "Deprecated (kept + still seeded): %14 file(s), %15")
                .arg(QString::fromStdString(Plan->Name),
                     QString::fromStdString(Plan->OldCid.empty() ? "(none)" : Plan->OldCid),
                     QString::fromStdString(Plan->NewCid))
                .arg(Plan->OldPackages).arg(Plan->NewPackages).arg(Plan->SharedPackages)
                .arg(Plan->JsonAdded.size()).arg(Plan->JsonChanged.size()).arg(Plan->JsonRemoved.size())
                .arg(Plan->ContentKeep.size()).arg(Gb(Plan->KeptBytes))
                .arg(Plan->ContentMove.size())
                .arg(Plan->ContentNew.size())
                .arg(Plan->ContentDeprecate.size()).arg(Gb(Plan->DeprecatedBytes));
            emit sourceUpgradePlanned(S, Unrelated);
        });
}

void AppModel::applySourceUpgrade(bool force)
{
    if (!PendingUpgrade || !PendingUpgrade->Valid) { emit packageSourceFailed("No planned upgrade to apply."); return; }
    // Applying moves files and re-seeds — off-thread. Work on a copy, then swap the two keys the upgrade owns.
    auto Cfg   = std::make_shared<nlohmann::ordered_json>(*Config);
    auto Plan  = PendingUpgrade;
    auto Err   = std::make_shared<std::string>();
    auto Ok    = std::make_shared<bool>(false);
    auto Index = std::make_shared<NodeIndex>();
    AsyncWork::Run(this,
        [Cfg, Plan, Err, Ok, Index, force]{
            *Ok = PackageCatalog::ApplySourceUpgrade(*Cfg, *Plan, force, Err.get());
            if (!*Ok) return;
            PackageCatalog::SyncPackageSources(*Cfg);
            *Index = PackageCatalog::BuildCatalogIndex(*Cfg);   // heavy scan stays off the main thread
        },
        [this, Cfg, Err, Ok, Index]{
            PendingUpgrade.reset();
            if (!*Ok)
            {
                emit packageSourceFailed(QString::fromStdString(
                    Err->empty() ? std::string("the upgrade could not be applied") : *Err));
                return;
            }
            (*Config)["LIBRARY"] = (*Cfg)["LIBRARY"];
            (*Config)["Settings"]["PackageSources"] = (*Cfg)["Settings"]["PackageSources"];
            save();
            CatalogIndex = std::move(*Index);
            emit catalogChanged();
            emit packageSourcesChanged();
        });
}

void AppModel::importRunner(const QString & runnerNodeId)
{
    // A runner install is NOT special — it is a download whose targets are the runner's build layers. Hand it to the
    // ONE download pump (DownloadManager::beginDownload, wired by MainWindow) so it gets persistence/resume, IPFS-tab
    // rows, cancel and the atomic-resumable fetch identically to a game download. The DEFPREFIX is that download's
    // post-fetch step.
    emit runnerImportRequested(runnerNodeId);
}

