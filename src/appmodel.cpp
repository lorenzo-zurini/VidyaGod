#include "appmodel.h"
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
        NodeIndex Idx = CatalogIndex;      // private copy for the worker (mirrors importRunner)
        std::thread([this, Idx = std::move(Idx), ToDehydrate = std::move(ToDehydrate)]{
            for (const std::string & NodeId : ToDehydrate) PackageCatalog::DehydrateNode(Idx, NodeId);
            QMetaObject::invokeMethod(this, [this]{ rebuildCatalog(); }, Qt::QueuedConnection);  // emits catalogChanged
        }).detach();
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

void AppModel::syncSources()
{
    auto Cfg = std::make_shared<nlohmann::ordered_json>(*Config);
    std::thread([this, Cfg]{
        PackageCatalog::SyncPackageSources(*Cfg);   // fetch-if-missing + index CID package sources (no-op offline once fetched)
        QMetaObject::invokeMethod(this, [this, Cfg]{
            (*Config)["LIBRARY"] = (*Cfg)["LIBRARY"];
            save();
            rebuildCatalog();              // emits catalogChanged
            emit packageSourcesChanged();
        }, Qt::QueuedConnection);
    }).detach();
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
    std::thread([this, Cfg]{
        std::string Err;
        PackageCatalog::SyncPackageSources(*Cfg, &Err);
        QMetaObject::invokeMethod(this, [this, Cfg, Err]{
            (*Config)["LIBRARY"] = (*Cfg)["LIBRARY"];
            save();
            rebuildCatalog();
            emit packageSourcesChanged();
            if (!Err.empty()) emit packageSourceFailed(QString::fromStdString(Err));
        }, Qt::QueuedConnection);
    }).detach();
    return true;
}

void AppModel::removePackageSource(int index)
{
    PackageCatalog::RemovePackageSource(*Config, index);
    save();
    rebuildCatalog();
    emit packageSourcesChanged();
}

void AppModel::importRunner(const QString & runnerNodeId)
{
    // A runner install is NOT special — it is a download whose targets are the runner's build layers. Hand it to the
    // ONE download pump (DownloadManager::beginDownload, wired by MainWindow) so it gets persistence/resume, IPFS-tab
    // rows, cancel and the atomic-resumable fetch identically to a game download. The DEFPREFIX is that download's
    // post-fetch step.
    emit runnerImportRequested(runnerNodeId);
}

void AppModel::generateRunnerDefPrefix(const QString & runnerNodeId, bool force)
{
    // Rebuild ONLY the DEFPREFIX (no download — the build is already local). Off-thread; reads a private index copy so
    // it never races the GUI thread. This is the deliberate, re-runnable prefix step (Settings → Runners button / CLI).
    const std::string Rid = runnerNodeId.toStdString();
    NodeIndex Idx = CatalogIndex;
    std::thread([this, Rid, force, Idx = std::move(Idx)]{
        std::string Err;
        const bool Ok = RunnerInstall::GenerateRunnerDefPrefix(Idx, Rid, force, &Err);
        QMetaObject::invokeMethod(this, [this, Ok, Err]{
            if (!Ok) LogErr("AppModel::generateRunnerDefPrefix", "DEFPREFIX generation failed: " + Err);
            rebuildCatalog();   // emits catalogChanged (Runners page refresh)
        }, Qt::QueuedConnection);
    }).detach();
}
