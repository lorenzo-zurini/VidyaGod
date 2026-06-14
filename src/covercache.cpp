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

    // 1. Local-first: the curated image sitting next to the manifest (authoring / a hydrated copy).
    if (!File.isEmpty())
    {
        const QString Local = QDir::cleanPath(PackageDir + "/" + File);
        if (QFileInfo::exists(Local)) return Local;
    }
    // 2. Cached IPFS fetch.
    if (!Cid.isEmpty())
    {
        if (IpfsWrapper::IsCached(Cid.toStdString()))
            return QString::fromStdString(IpfsWrapper::CachePath(Cid.toStdString()));
        request(Cid);                                   // 3. not here yet — fetch in the background
    }
    return QString();
}

void CoverCache::request(const QString & Cid)
{
    if (Cid.isEmpty() || InFlight.contains(Cid)) return;
    InFlight.insert(Cid);
    const std::string C = Cid.toStdString();
    std::thread([this, C, Cid]{
        std::string Err;
        IpfsWrapper::FetchSync(C, &Err);                // fetches into the cache + seeds (best-effort)
        QMetaObject::invokeMethod(this, [this, Cid]{
            InFlight.remove(Cid);
            emit coverReady(Cid);                       // GUI thread: callers re-resolve + repaint
        }, Qt::QueuedConnection);
    }).detach();
}
