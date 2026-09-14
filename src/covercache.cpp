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

CoverCache::CoverCache(QObject * parent) : QObject(parent) {}

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

void CoverCache::request(const QString & Cid, const QString & DestPath)
{
    if (Cid.isEmpty() || DestPath.isEmpty() || InFlight.contains(DestPath) || Failed.contains(DestPath)) return;
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
            // detached thread every 30s forever for a cover nobody seeds. It is retried when CoverCache is rebuilt
            // (a catalog reload / re-sync, e.g. after a re-mint). On success the tile paints; on failure it shows
            // no art but stops churning.
            if (!Ok) Failed.insert(DestPath);
            emit coverReady(Cid);                       // GUI thread: callers re-resolve + repaint
        }, Qt::QueuedConnection);
    }).detach();
}
