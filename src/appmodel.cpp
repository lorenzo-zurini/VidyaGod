#include "appmodel.h"
#include "packagecatalog.h"
#include "manifestmodel.h"
#include "containerwrapper.h"
#include "ipfswrapper.h"
#include "jsonoperations.h"
#include "commonutils.h"

#include <QDir>
#include <QFile>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
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

// ── Repositories: sync/add/remove. Each writes any config change synchronously (so it persists), then does the
//    git clone/pull + LIBRARY reindex off-thread on a PRIVATE config copy, and applies just LIBRARY back on the GUI
//    thread — the worker never mutates the live GlobalConfigJSON the GUI may be reading/writing. The model outlives
//    every view, so capturing `this` is safe. ──

void AppModel::syncRepositories()
{
    auto Cfg = std::make_shared<nlohmann::ordered_json>(*Config);
    std::thread([this, Cfg]{
        PackageCatalog::SyncRepositories(*Cfg);   // git pull each repo + reindex LIBRARY (into the copy)
        QMetaObject::invokeMethod(this, [this, Cfg]{
            (*Config)["LIBRARY"] = (*Cfg)["LIBRARY"];   // SyncRepositories only writes LIBRARY
            save();
            rebuildCatalog();              // emits catalogChanged
            emit repositoriesChanged();
        }, Qt::QueuedConnection);
    }).detach();
}

// Normalize a git URL for duplicate detection: lowercased, trailing "/" and ".git" stripped (so
// https://h/Repo.git and https://h/Repo/ are the same repo).
static std::string NormalizeRepoUrl(const QString & Url)
{
    QString U = Url.trimmed().toLower();
    while (U.endsWith('/')) U.chop(1);
    if (U.endsWith(".git")) U.chop(4);
    return U.toStdString();
}

bool AppModel::addRepository(const QString & name, const QString & url)
{
    if (url.trimmed().isEmpty()) return false;
    auto & SS = (*Config)["Settings"];
    if (!SS.contains("Repositories") || !SS["Repositories"].is_array()) SS["Repositories"] = nlohmann::ordered_json::array();

    const std::string Key = NormalizeRepoUrl(url);
    for (const auto & R : SS["Repositories"])
        if (R.is_object() && NormalizeRepoUrl(QString::fromStdString(R.value("PATH", std::string()))) == Key)
            return false;   // already configured — caller warns

    nlohmann::ordered_json Entry = nlohmann::ordered_json::object();
    if (!name.trimmed().isEmpty()) Entry["NAME"] = name.trimmed().toStdString();
    Entry["PATH"] = url.trimmed().toStdString();
    SS["Repositories"].push_back(Entry);
    save();
    syncRepositories();   // clone/pull + reindex the freshly-added repo, then emit
    return true;
}

void AppModel::removeRepository(int index)
{
    auto & SS = (*Config)["Settings"];
    if (SS.contains("Repositories") && SS["Repositories"].is_array() && index >= 0 && index < int(SS["Repositories"].size()))
        SS["Repositories"].erase(SS["Repositories"].begin() + index);
    save();
    rebuildCatalog();
    emit repositoriesChanged();
}

void AppModel::importRunner(const QString & runnerNodeId)
{
    const std::string Rid = runnerNodeId.toStdString();
    // The worker reads config + index; hand it private copies so it never races the GUI thread (which owns the live
    // GlobalConfigJSON / CatalogIndex and may mutate them concurrently).
    auto Cfg = std::make_shared<nlohmann::ordered_json>(*Config);
    NodeIndex Idx = CatalogIndex;
    std::thread([this, Rid, Cfg, Idx = std::move(Idx)]{
        std::string Err;
        bool Ok = RunnerInstall::ImportRunnerNode(*Cfg, Idx, Rid, &Err);
        QMetaObject::invokeMethod(this, [this, Ok, Err]{
            if (!Ok) LogErr("AppModel::importRunner", "Runner import failed: " + Err);   // no dialog — see the IPFS tab
            rebuildCatalog();   // emits catalogChanged (Runners page + IPFS tab refresh)
        }, Qt::QueuedConnection);
    }).detach();
}
