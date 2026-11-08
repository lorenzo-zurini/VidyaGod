#include "appmodel.h"
#include "friendsharing.h"
#include "nodestore.h"
#include "covercache.h"
#include "platform/platform.h"   // ReleaseFreedMemory

#include <algorithm>
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
#include <QFileInfo>

#include <cctype>
#include <set>
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

// The size covers are kept at: the largest card (Large, or a wider width set by hand), 2:3 as every card is drawn.
static QSize CoverDisplaySize(int CardWidth)
{
    const int W = std::max(250, CardWidth);
    return QSize(W, W * 3 / 2);
}

AppModel::AppModel(nlohmann::ordered_json * config, QDir * appDataDir, QObject * parent)
    : QObject(parent), Config(config), AppDataDir(appDataDir)
{
    // Apply persisted startup settings the model owns.
    auto & S = (*Config)["Settings"];
    if (S.contains("CardPixelWidth") && S["CardPixelWidth"].is_number_integer())
        CardPixelWidth = int(S["CardPixelWidth"]);
    if (S.contains("MaxConcurrentDownloads") && S["MaxConcurrentDownloads"].is_number_integer())
        IpfsWrapper::SetMaxConcurrentDownloads(int(S["MaxConcurrentDownloads"]));

    // Content-addressed cover store: covers with a SOURCE.CID resolve + fetch to ASSETS/<cid>, shared across the
    // CATALOG stub, the LIBRARY install, and any package referencing the same cover — no per-bundle duplication.
    CoverCache::instance()->setAssetsRoot(QString::fromStdString(PackageCatalog::AssetsRootDir(*Config)));
    CoverCache::instance()->setDisplaySize(CoverDisplaySize(CardPixelWidth));

    // Drop LIBRARY records for locally-added bundles the user has since moved/deleted (their PATH is gone), so the
    // library doesn't list dead tiles. Repo packages are untouched (their content may just be un-hydrated).
    if (PackageCatalog::PruneMovedLocalPackages(*Config) > 0) save();

    {
        std::string Inputs;
        std::vector<Diagnostics::Entry> Said;
        NodeIndex Idx = PackageCatalog::BuildCatalogIndex(*Config, &Inputs, &Said);   // node-native catalog source
        setCatalog(std::move(Idx), std::move(Inputs), std::move(Said));
    }

    Friends = new FriendSharing(*this);   // the friend protocol: shares, received libraries, per-peer toggles

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
    return JSONOps::SaveConfig(*Config, AppDataDir->absolutePath().toStdString());
}

void AppModel::rebuildCatalog()
{
    {
        std::string Inputs;
        std::vector<Diagnostics::Entry> Said;
        NodeIndex Idx = PackageCatalog::BuildCatalogIndex(*Config, &Inputs, &Said);   // re-scan the node graph from disk
        setCatalog(std::move(Idx), std::move(Inputs), std::move(Said));
    }

    // Covers: every tile's cover not on disk here is fetched by the cover cache's own sweep (one pipeline).
    CoverCache::instance()->prefetch(*Catalog);
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
        ManifestModel::DeriveFacts(BIdx);   // the representative launchable carries (or inherits) its UID/TITLE
        const Node * Rep = nullptr;
        for (const auto & [Id, N] : BIdx.Nodes)
            if (N.IsVariant() && !N.IsGraft && (!Rep || (N.Presentable() && !Rep->Presentable()))) Rep = &N;
        if (!Rep) { LogWarn("AppModel::importPackagesFromDir", "Skipping " + Path.toStdString() + ": no variant node."); ++Skipped; continue; }

        const std::string Uid  = Rep->Uid.empty() ? Rep->NodeId : Rep->Uid;
        const std::string Name = Rep->Meta.is_object() ? Rep->Meta.value("TITLE", Rep->NodeId) : Rep->NodeId;
        bool Dup = false;
        for (auto & E : (*Config)["LIBRARY"])
            if (E.value("PACKAGEUID", std::string()) == Uid) { Dup = true; ++Skipped; break; }
        if (Dup) continue;

        nlohmann::ordered_json Slim;
        Slim["PACKAGEUID"] = Uid; Slim["PACKAGENAME"] = Name; Slim["PATH"] = Path.toStdString();

        // Per-package config (incl. RUNNER_CHAIN) lives in the INSTANCE file, not the LIBRARY entry — this entry
        // stays package-free. No runner seed is needed: the chain resolver prefers a runner RECOMMENDED for the tile
        // when nothing is pinned, and the user overrides in the prelaunch picker (which persists to the instance).

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
    CoverCache::instance()->setDisplaySize(CoverDisplaySize(w));
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

QString AppModel::removePackage(const QString & which, bool * Async)
{
    if (Async) *Async = false;
    //A package folder, or the UID of exactly one (Installed Packages passes its folder). The packages LIBRARY holds are
    //found through the index: the config's LIBRARY list names only those added from outside it.
    std::error_code Ec;
    std::string Path = which.toStdString();
    auto & Lib = (*Config)["LIBRARY"];
    if (!std::filesystem::is_directory(Path, Ec))
    {
        auto Dirs = PackageCatalog::PackageDirsWithUid(*Catalog, Path);
        //A package added from outside LIBRARY is also known by the UID its entry records (its folder may hold no node
        //the index read — moved, or never valid).
        if (Dirs.empty())
            for (const auto & E : Lib.is_array() ? Lib : nlohmann::ordered_json::array())
                if (E.is_object() && E.value("PACKAGEUID", std::string()) == Path && !E.value("PATH", std::string()).empty())
                    Dirs.emplace_back(E.value("PATH", std::string()));
        if (Dirs.size() != 1)
        {
            const QString Why = Dirs.empty() ? "no package is '" + which + "'"
                                             : QString("'%1' names %2 packages; name its folder").arg(which).arg(Dirs.size());
            LogWarn("AppModel::removePackage", Why.toStdString());
            return Why;
        }
        Path = Dirs.front().string();
    }
    int idx = -1;
    const std::string Want = std::filesystem::weakly_canonical(Path, Ec).string();
    for (int k = 0; Lib.is_array() && k < int(Lib.size()); ++k)
        if (Lib[k].is_object() && std::filesystem::weakly_canonical(Lib[k].value("PATH", std::string()), Ec).string() == Want) { idx = k; break; }

    const std::string LibRoot = PackageCatalog::LibraryRootDir(*Config);
    bool Managed = false;
    if (!Path.empty() && !LibRoot.empty())
    {
        const std::string P = std::filesystem::weakly_canonical(std::filesystem::path(Path), Ec).string();
        const std::string R = std::filesystem::weakly_canonical(std::filesystem::path(LibRoot), Ec).string();
        Managed = (P.rfind(R + "/", 0) == 0);   // strictly under the library root → a managed (repo-cloned) package
    }

    //A package of LIBRARY that its config entry does not name (every package since the node cutover: the entries are
    //for sources and for packages added from outside) is not removed here. Its content's dehydrate walks the whole
    //closure — another package's shared files (a DirectPlay several games use) and content that has no source to
    //download again would go with it. Only what this has always removed is removed.
    if (Managed && idx < 0)
    {
        const QString Why = "a package of the library is not removed from here: its content can be shared with other "
                            "packages, and what has no source cannot be downloaded again";
        LogWarn("AppModel::removePackage", Why.toStdString() + " (" + Path + ")");
        return Why;
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
        for (const auto & [NodeId, N] : Catalog->Nodes)
            if (N.IsVariant() &&
                std::filesystem::weakly_canonical(N.BundleDir, Ec).string() == CanonPath)
                ToDehydrate.push_back(NodeId);
        if (ToDehydrate.empty()) return "nothing of it is installed here";
        std::shared_ptr<const NodeIndex> Idx = Catalog;            // the worker reads the index as it is now (immutable)
        AsyncWork::Run(this,
            [Idx, ToDehydrate = std::move(ToDehydrate)]{ for (const std::string & NodeId : ToDehydrate) PackageCatalog::DehydrateNode(*Idx, NodeId); },
            [this]{ rebuildCatalog(); });  // emits catalogChanged
        if (Async) *Async = true;
        return {};   // config unchanged (manifest kept); the worker rebuilds the catalog when the content is gone
    }

    // Local/portable package added from outside the library → only drop the reference; never touch the user's files.
    // This is cheap (no IPFS work), so it stays synchronous.
    if (idx < 0) return "no package is '" + which + "'";
    Lib.erase(idx);
    save();
    rebuildCatalog();
    return {};
}

// ── Sources: re-index every configured CID package source, fetching any not-yet-present one (needs the node online;
//    a no-op once fetched). Runs off-thread on a PRIVATE config copy and applies just LIBRARY back on the GUI thread —
//    the worker never mutates the live GlobalConfigJSON the GUI may be reading/writing. The model outlives every view,
//    so capturing `this` is safe. Called on node-ready (bootstrap of the default runners source) and "Sync now". ──

// Give the node what to announce FIRST, tracked, ahead of the batched content queue: the published package folders
// (the share record — what a friend fetches) and pin folders (what a pinning service looks up), then every node block
// of our own tree. Everything else pinned is content, announced after in the batched queue. Called on node-ready +
// after every publish; idempotent on the node side.
void AppModel::pushSeedLevels()
{
    std::vector<std::string> Roots, Nodes;
    if (Config->contains("Libraries") && (*Config)["Libraries"].is_object())
        for (const auto & [Lib, Rows] : (*Config)["Libraries"].items())
            if (Rows.is_array())
                for (const auto & R : Rows)
                    if (R.is_object())
                        for (const char * K : { "cid", "pin" })   // the share folder, then the pin folder
                            if (R.contains(K) && R[K].is_string() && !R[K].get<std::string>().empty()) Roots.push_back(R[K].get<std::string>());
    for (const auto & [Cid, N] : Catalog->Nodes)
        if (!N.Received && !N.Cid.empty()) Nodes.push_back(Cid);
    IpfsWrapper::SetSeedLevels(Roots, Nodes);
}

void AppModel::syncSources()
{
    auto Cfg = std::make_shared<nlohmann::ordered_json>(*Config);
    auto Before = std::make_shared<const nlohmann::ordered_json>(Config->value("LIBRARY", nlohmann::ordered_json::array()));
    // Snapshot LIBRARY before the sync mutates it, so we can tell whether anything actually changed. On the common
    // launch (sources already present + unchanged) SyncPackageSources is a no-op, so we must NOT rebuild the catalog:
    // the ctor already built it, and a redundant BuildCatalogIndex here — posted to the MAIN thread while the IPFS
    // node is simultaneously starting up — is what froze the UI for several seconds on launch. When it DID change
    // (first run / a source CID bump), the worker builds the new index and the completion only swaps it in.
    auto Changed  = std::make_shared<bool>(false);
    auto NewIndex = std::make_shared<NodeIndex>();
    auto NewInputs = std::make_shared<std::string>();
    auto NewSaid = std::make_shared<std::vector<Diagnostics::Entry>>();
    AsyncWork::Run(this,
        [Cfg, Changed, NewIndex, NewInputs, NewSaid]{
            const nlohmann::ordered_json OrigLib = Cfg->value("LIBRARY", nlohmann::ordered_json::array());
            PackageCatalog::SyncPackageSources(*Cfg);   // fetch-if-missing + index CID package sources (no-op offline once fetched)
            *Changed = Cfg->value("LIBRARY", nlohmann::ordered_json::array()) != OrigLib;
            if (*Changed) *NewIndex = PackageCatalog::BuildCatalogIndex(*Cfg, NewInputs.get(), NewSaid.get());   // heavy scan OFF the main thread
        },
        [this, Cfg, Before, Changed, NewIndex, NewInputs, NewSaid]{
            if (*Changed)
            {
                PackageCatalog::MergeListChange((*Config)["LIBRARY"], *Before, (*Cfg)["LIBRARY"], PackageCatalog::LibraryEntryKey);
                save();
                setCatalog(std::move(*NewIndex), std::move(*NewInputs), std::move(*NewSaid));   // cheap swap on the main thread (build already done)
                emit catalogChanged();
                emit packageSourcesChanged();
            }
            healOrphansIfAny();            // re-point any orphaned no-copy refs so the node can actually SERVE its content
            Friends->completeReceivedClosures();    // node-ready → finish landing any received root's node closure (resumes after a restart)
            Friends->reRegisterShares();            // node-ready → replay config["Sharing"] into the node's in-memory share table: a
                                           // restarted seeder shared NOTHING until its next publish (the durable record
                                           // never reached Go), so friends saw empty snapshots
            pushSeedLevels();              // node-ready → announce our seeded content to the DHT, meta-CIDs first
            Friends->pushLanRoster();               // node-ready → apply the persisted Virtual-LAN roster (excluded members)
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

const std::unordered_map<std::string, Installed::NodeHydration> & AppModel::hydration() const
{
    if (!Hydration) Hydration = Installed::HydrationMap(*Catalog);
    return *Hydration;
}

void AppModel::setCatalog(NodeIndex Idx, std::string Inputs, std::vector<Diagnostics::Entry> Said)
{
    Catalog = std::make_shared<const NodeIndex>(std::move(Idx));
    Hydration.reset();
    PackageCatalog::PublishAppIndex(Catalog, std::move(Inputs), std::move(Said));
    std::thread([] { Platform::ReleaseFreedMemory(); }).detach();   // the replaced index back to the system, off this thread
}

void AppModel::healOrphansIfAny()
{
    bool Expected = false;
    if (!HealInFlight->compare_exchange_strong(Expected, true)) return;  // single-flight: a heal is already running
    auto Cfg = std::make_shared<nlohmann::ordered_json>(*Config);
    // NOTE: the worker must not touch this model at all — it can outlive it (a model torn down mid-pass: a closed
    // app, every test). It computes into shared locals; the completion, which runs only while the model lives,
    // applies them and reports. InFlight is the one thing the worker clears, and it holds its own reference.
    auto Unhealable = std::make_shared<std::set<std::string>>(KnownUnhealable);
    auto Healed     = std::make_shared<bool>(false);
    auto Unservable = std::make_shared<std::vector<std::pair<std::string, std::string>>>();   // (recorded CID, why)
    auto InFlight   = HealInFlight;
    //Once per run, after the orphans: hold every content file the seed roots name that is not held whole
    //(SeedUnheld) — inside this single flight, so the two passes never race over the same files.
    //Armed only with a started node (the 90 s timer heals with networking off too), and re-armed if the node went away
    //mid-pass: the flag used up while offline meant the pass never ran that session.
    const bool SeedUnheldNow = !SeededUnheld && IpfsWrapper::Available();
    if (SeedUnheldNow) SeededUnheld = true;
    auto SeedDone = std::make_shared<bool>(false);
    AsyncWork::Run(this,
        [Cfg, Unhealable, Healed, Unservable, SeedUnheldNow, SeedDone, InFlight]{
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
                for (const std::string &Dir : PackageCatalog::SeedRoots(*Cfg))
                {
                    std::vector<PackageCatalog::SeedFailure> Bad;
                    PackageCatalog::SeedDirectory(Dir, {}, nullptr, false, false, false, &Bad);
                    for (const auto &F : Bad)
                        Unservable->emplace_back(F.RecordedCid, "content no longer matches the published CID (" + F.Path + ")");
                }
                // Whatever is STILL orphaned after the heal is content truly gone (no on-disk copy to re-point to):
                // remember it so subsequent sweeps skip the heavy re-seed until something changes.
                Unhealable->clear();
                for (const std::string & P : IpfsWrapper::OrphanedRefPaths()) Unhealable->insert(P);
                *Healed = true;
            }
            if (SeedUnheldNow)
                for (const std::string &Dir : PackageCatalog::SeedRoots(*Cfg))
                {
                    std::vector<PackageCatalog::SeedFailure> Bad;
                    if (PackageCatalog::SeedUnheld(Dir, &Bad) > 0) *Healed = true;
                    for (const auto &F : Bad)
                        Unservable->emplace_back(F.RecordedCid, F.ActualCid.empty()
                            ? "content could not be read or added (" + F.Path + ")"
                            : "content does not match its recorded CID (" + F.Path + ")");
                }
            *SeedDone = IpfsWrapper::Available();                   // stopped early: the next heal runs it again
            InFlight->store(false);
        },
        [this, Unhealable, Healed, Unservable, SeedUnheldNow, SeedDone]{
            if (SeedUnheldNow && !*SeedDone) SeededUnheld = false;
            KnownUnhealable = std::move(*Unhealable);
            for (const auto &[Cid, Why] : *Unservable)
                emit contentUnservable(QString::fromStdString(Cid), QString::fromStdString(Why));
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
    auto Before = std::make_shared<const nlohmann::ordered_json>(Config->value("LIBRARY", nlohmann::ordered_json::array()));
    auto Err = std::make_shared<std::string>();
    AsyncWork::Run(this,
        [Cfg, Err]{ PackageCatalog::SyncPackageSources(*Cfg, Err.get()); },
        [this, Cfg, Before, Err]{
            PackageCatalog::MergeListChange((*Config)["LIBRARY"], *Before, (*Cfg)["LIBRARY"], PackageCatalog::LibraryEntryKey);
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
        [Dest, Cid, Dir, Err]{ *Dir = NodeStore::HydratePackage(Dest, Cid, Err.get()); },
        [this, Dir, Err]{
            if (Dir->empty()) { emit gameAddFailed(QString::fromStdString(*Err)); return; }
            rebuildCatalog();     // the new checkout is on-disk → it appears in the Library
            pushSeedLevels();     // seed its content so we become a provider too
            emit gameAdded(QString::fromStdString(*Dir));
        });
}

void AppModel::publishLibraries()
{
    // Gigagraph publish: freeze the on-disk library into dag-json blocks and STORE them (DagPut → pinned/announced/
    // seedable), then record the shareable list of launchable root CIDs. No IPNS — the list is off-IPFS VidyaGod data.
    // One at a time: two would write Libraries over each other. Asked again meanwhile, it runs once more
    // after, with publishing() true throughout (nothing offers a publish in between).
    if (Publishing) { PublishAgain = true; return; }
    Publishing = true;
    runPublish();
}

void AppModel::runPublish()
{
    auto Cfg  = std::make_shared<nlohmann::ordered_json>(*Config);
    auto Cids = std::make_shared<std::vector<std::string>>();
    auto Err  = std::make_shared<std::string>();
    auto Gaps = std::make_shared<std::string>();
    AsyncWork::Run(this,
        [Cfg, Cids, Err, Gaps]{ *Cids = PackageCatalog::PublishLibrary(*Cfg, Err.get(), Gaps.get()); },
        [this, Cfg, Cids, Err, Gaps]{
            Publishing = PublishAgain;
            if (PublishAgain) { PublishAgain = false; QMetaObject::invokeMethod(this, &AppModel::runPublish, Qt::QueuedConnection); }
            if (Cids->empty()) { emit libraryPublishFailed(QString::fromStdString(*Err)); return; }
            (*Config)["Libraries"] = (*Cfg)["Libraries"];   // the per-library grouping (drives Share ▾)
            Config->erase("PublishedList");
            Config->erase("PublishedManifest");
            save();
            pushSeedLevels();
            Friends->reRegisterShares();   // re-push every active share with the fresh CIDs (a re-publish can move them)
            emit libraryPublished(QString::fromStdString(IpfsWrapper::FriendCode()),
                                  QString::number(static_cast<int>(Cids->size())) + " package(s)",
                                  QString::fromStdString(*Gaps));
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
    auto Before = std::make_shared<const nlohmann::ordered_json>(*Config);
    auto Plan  = PendingUpgrade;
    auto Err   = std::make_shared<std::string>();
    auto Ok    = std::make_shared<bool>(false);
    auto Index = std::make_shared<NodeIndex>();
    auto Inputs = std::make_shared<std::string>();
    auto Said = std::make_shared<std::vector<Diagnostics::Entry>>();
    AsyncWork::Run(this,
        [Cfg, Plan, Err, Ok, Index, Inputs, Said, force]{
            *Ok = PackageCatalog::ApplySourceUpgrade(*Cfg, *Plan, force, Err.get());
            if (!*Ok) return;
            PackageCatalog::SyncPackageSources(*Cfg);
            *Index = PackageCatalog::BuildCatalogIndex(*Cfg, Inputs.get(), Said.get());   // heavy scan stays off the main thread
        },
        [this, Cfg, Before, Err, Ok, Index, Inputs, Said]{
            PendingUpgrade.reset();
            if (!*Ok)
            {
                emit packageSourceFailed(QString::fromStdString(
                    Err->empty() ? std::string("the upgrade could not be applied") : *Err));
                return;
            }
            const nlohmann::ordered_json None = nlohmann::ordered_json::array();
            auto List = [&None](const nlohmann::ordered_json &C, const char *K, const char *Sub) -> const nlohmann::ordered_json & {
                const nlohmann::ordered_json *P = &C;
                for (const char *Part : { K, Sub }) { if (!Part) break; if (!P->is_object() || !P->contains(Part)) return None; P = &(*P)[Part]; }
                return *P;
            };
            PackageCatalog::MergeListChange((*Config)["LIBRARY"], List(*Before, "LIBRARY", nullptr), List(*Cfg, "LIBRARY", nullptr),
                                            PackageCatalog::LibraryEntryKey);
            PackageCatalog::MergeListChange((*Config)["Settings"]["PackageSources"], List(*Before, "Settings", "PackageSources"),
                                            List(*Cfg, "Settings", "PackageSources"), PackageCatalog::SourceKey);
            save();
            setCatalog(std::move(*Index), std::move(*Inputs), std::move(*Said));
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
