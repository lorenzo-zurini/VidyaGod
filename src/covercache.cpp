#include "covercache.h"
#include "ipfswrapper.h"

#include <QFileInfo>
#include <QDir>
#include <thread>

CoverCache * CoverCache::instance()
{
    static CoverCache * Inst = new CoverCache();
    return Inst;
}

CoverCache::CoverCache(QObject * parent) : QObject(parent), OnlineProbe([]{ return IpfsWrapper::DaemonRunning(); }) {}

void CoverCache::setOnlineProbe(std::function<bool()> Probe) { OnlineProbe = std::move(Probe); }

void CoverCache::Locate(const nlohmann::ordered_json &Cover, QString &File, QString &Cid)
{
    File.clear(); Cid.clear();
    if (Cover.is_string())
        File = QString::fromStdString(Cover.get<std::string>());
    else if (Cover.is_object())
    {
        File = QString::fromStdString(Cover.value("PATH", std::string()));
        if (Cover.contains("SOURCE") && Cover["SOURCE"].is_object()
            && Cover["SOURCE"].value("TYPE", std::string()) == "ipfs")
            Cid = QString::fromStdString(Cover["SOURCE"].value("CID", std::string()));
    }
}

QString CoverCache::resolve(const nlohmann::ordered_json &Cover, const QString &PackageDir)
{
    QString File, Cid;
    Locate(Cover, File, Cid);
    if (File.isEmpty()) return QString();

    // Local-first: the cover lives next to the manifest at PackageDir/<PATH> (authoring, or once hydrated).
    const QString Local = QDir::cleanPath(PackageDir + "/" + File);
    if (QFileInfo::exists(Local)) return Local;

    // Not here yet — fetch it there in the background; coverReady(cid) fires when it lands.
    if (!Cid.isEmpty()) request(Cid, Local);
    return QString();
}

bool CoverCache::ShouldNegativeCache(bool FetchOk, bool NodeOnline)
{
    // Remember ONLY a give-up that happened while ONLINE. A failure while offline/pre-online says nothing about
    // whether the content is obtainable — it will be retried once the network is up (onNetworkOnline re-nudges).
    return !FetchOk && NodeOnline;
}

void CoverCache::recordFetchResult(const QString & DestPath, bool FetchOk, bool NodeOnline)
{
    if (ShouldNegativeCache(FetchOk, NodeOnline)) Failed.insert(DestPath);
}

void CoverCache::onNetworkOnline()
{
    // Networking just came up. Any give-up recorded while it was down (or any real give-up worth a fresh try now
    // that providers are reachable) should not keep the tile blank — clear the negative cache and ask every cover
    // surface to re-resolve (the coverReady handlers ignore the cid and kick a debounced repaint).
    if (Failed.isEmpty()) { emit coverReady(QString()); return; }
    Failed.clear();
    emit coverReady(QString());
}

void CoverCache::request(const QString & Cid, const QString & DestPath)
{
    if (Cid.isEmpty() || DestPath.isEmpty() || InFlight.contains(DestPath) || Failed.contains(DestPath)) return;
    // A cover can only be fetched over the network. While the node is offline/pre-online the fetch would fail
    // instantly (and, before this guard, poison the negative cache); skip it — onNetworkOnline re-nudges a
    // re-resolve once networking is up, which starts the fetch then.
    if (!OnlineProbe()) return;
    InFlight.insert(DestPath);
    const std::string C = Cid.toStdString(), D = DestPath.toStdString();
    std::thread([this, C, D, Cid, DestPath]{
        std::string Err;
        // BOUNDED WAIT (30s): a cover is optional cosmetic content on a detached thread. Waiting forever would
        // leak one thread per unfetchable cover (stale/unpublished art). Give up after 30s.
        const bool Ok = !IpfsWrapper::FetchToPath(C, D, &Err, /*TimeoutMs=*/30000).empty();
        QMetaObject::invokeMethod(this, [this, Cid, DestPath, Ok]{
            InFlight.remove(DestPath);
            // NEGATIVE-CACHE a give-up so a repaint does not immediately re-request it — that would spawn a fresh
            // detached thread every 30s forever for a cover nobody seeds — but ONLY when the node was ONLINE at the
            // give-up. A failure while the network was down (or dropped mid-fetch) is not a verdict on the content,
            // so it is not cached and is retried once online. On success the tile paints; on an online give-up it
            // shows no art but stops churning. Retried on the online transition (onNetworkOnline) or a catalog reload.
            recordFetchResult(DestPath, Ok, OnlineProbe());
            emit coverReady(Cid);                       // GUI thread: callers re-resolve + repaint
        }, Qt::QueuedConnection);
    }).detach();
}
