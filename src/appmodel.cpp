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

    // A managed import (under the library folder) → delete its hydrated copy. A local/portable package added from
    // elsewhere → only drop the reference (never touch the user's own files).
    const std::string Path    = Lib[idx].value("PATH", std::string());
    const std::string LibRoot = PackageCatalog::LibraryRootDir(*Config);
    std::error_code Ec;
    if (!Path.empty() && !LibRoot.empty())
    {
        const std::string P = std::filesystem::weakly_canonical(std::filesystem::path(Path), Ec).string();
        const std::string R = std::filesystem::weakly_canonical(std::filesystem::path(LibRoot), Ec).string();
        if (P.rfind(R + "/", 0) == 0) std::filesystem::remove_all(P, Ec);   // P strictly under the library root
    }
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

void AppModel::addRepository(const QString & name, const QString & url)
{
    nlohmann::ordered_json Entry = nlohmann::ordered_json::object();
    if (!name.trimmed().isEmpty()) Entry["NAME"] = name.trimmed().toStdString();
    Entry["PATH"] = url.trimmed().toStdString();
    auto & SS = (*Config)["Settings"];
    if (!SS.contains("Repositories") || !SS["Repositories"].is_array()) SS["Repositories"] = nlohmann::ordered_json::array();
    SS["Repositories"].push_back(Entry);
    save();
    syncRepositories();   // clone/pull + reindex the freshly-added repo, then emit
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
        bool Ok = ContainerWrapper::ImportRunnerNode(*Cfg, Idx, Rid, &Err);
        QMetaObject::invokeMethod(this, [this, Ok, Err]{
            if (!Ok) LogErr("AppModel::importRunner", "Runner import failed: " + Err);   // no dialog — see the IPFS tab
            rebuildCatalog();   // emits catalogChanged (Runners page + IPFS tab refresh)
        }, Qt::QueuedConnection);
    }).detach();
}
