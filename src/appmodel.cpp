#include "appmodel.h"
#include "asyncwork.h"   // guarded detached workers (P7: replaces raw std::thread(...).detach() with `this` captures)
#include "packagecatalog.h"
#include "manifestmodel.h"
#include "containerwrapper.h"
#include "ipfswrapper.h"
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

        // Seed the package-side recommended runner (DeclareExec.RUNNER) into PREFERRED_RUNNER. This is a fresh entry
        // (the dup check above skips existing ones), so it never overwrites a user's choice; PickRunnerNode reads it
        // as usual and the user overrides in the picker. Prefer the representative launchable, else any launchable
        // that declares one (a RECOMMENDED one wins).
        std::string RecRunner = Rep->RecommendedRunner;
        if (RecRunner.empty())
            for (const auto & [Id, N] : BIdx.Nodes)
                if (N.IsLaunchable() && !N.RecommendedRunner.empty()) { RecRunner = N.RecommendedRunner; if (N.Recommended) break; }
        if (!RecRunner.empty()) Slim["USERSETTINGS"]["PREFERRED_RUNNER"] = RecRunner;

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
                PackageCatalog::HealSourceContent(*Cfg);
                // The cheap per-file self-check (size gate) runs inside SeedDirectory during the heal; collect
                // anything it could NOT fix so the UI can surface it rather than leaving it in a log line.
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

