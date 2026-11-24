#include "covercache.h"
#include "manifestmodel.h"   // NodeIndex — prefetch walks its tiles
#include "contentfile.h"
#include "downloadqueue.h"
#include "ipfswrapper.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThreadPool>
#include <QTimer>

#include <filesystem>
#include <system_error>

namespace {
constexpr int SweepPeriodMs   = 60'000;   // the steady cadence: a minuscule stat-sweep of recorded misses
constexpr int SweepDebounceMs = 2'000;    // first miss after quiet → sweep soon, so a fresh catalog isn't blank for a minute
}

CoverCache * CoverCache::instance()
{
    static CoverCache * Inst = new CoverCache();
    return Inst;
}

CoverCache::CoverCache(QObject * parent) : QObject(parent), OnlineProbe([]{ return IpfsWrapper::DaemonRunning(); })
{
    // Completion feed: the SAME transfer events every download emits (queued through IpfsManager onto this thread).
    connect(IpfsManager::instance(), &IpfsManager::transferFinished, this, &CoverCache::onTransferFinished);
    auto * T = new QTimer(this);
    connect(T, &QTimer::timeout, this, &CoverCache::sweepNow);
    T->start(SweepPeriodMs);
}

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

void CoverCache::setAssetsRoot(const QString & Dir) { AssetsRoot = Dir; }

// Where a cover's bytes live: ASSETS/<cid> when it has a SOURCE.CID and the assets store is configured (shared,
// content-addressed, fetched once for every tile that references the CID); else in-bundle PackageDir/PATH (a local
// authored cover). Cover CIDs are content hashes (safe filenames), but sanitize defensively.
QString CoverCache::coverPath(const QString & File, const QString & Cid, const QString & PackageDir) const
{
    if (!Cid.isEmpty() && !AssetsRoot.isEmpty())
    {
        QString Safe; Safe.reserve(Cid.size());
        for (QChar ch : Cid) Safe.append((ch.isLetterOrNumber() || ch == '-' || ch == '_' || ch == '.') ? ch : QChar('_'));
        return QDir::cleanPath(AssetsRoot + "/" + Safe);
    }
    return QDir::cleanPath(PackageDir + "/" + File);
}

QString CoverCache::resolve(const nlohmann::ordered_json &Cover, const QString &PackageDir)
{
    QString File, Cid;
    Locate(Cover, File, Cid);
    if (File.isEmpty() && Cid.isEmpty()) return QString();

    QString Local = coverPath(File, Cid, PackageDir);
    if (QFileInfo::exists(Local))
    {
        MissDest.remove(Local);            // landed (by us or anyone) — stop tracking
        return Local;
    }
    // The addressed path (ASSETS/<cid>) is empty — but the bytes may be LOCAL: an authored cover whose SOURCE.CID was
    // stamped at publish yet whose PNG still lives in its bundle (your own library, or before ASSETS was ever
    // populated). PROMOTE it into the shared, content-addressed assets store (copy bundle → ASSETS/<cid>) so every tile
    // that references the CID shares one file and it survives a bundle move — and show it immediately, with no fetch.
    // Only received covers (no local bytes) fall through to the fetch sweep.
    if (!File.isEmpty() && !PackageDir.isEmpty())
    {
        const QString Bundle = QDir::cleanPath(QString::fromStdString(   // a bumped name when versions share it
            ContentFile::Path(PackageDir.toStdString(), File.toStdString(), Cid.toStdString()).string()));
        if (Bundle != Local && QFileInfo::exists(Bundle))
        {
            if (!Cid.isEmpty() && !AssetsRoot.isEmpty())     // Local == ASSETS/<cid>: copy the bundle PNG there
            {
                std::error_code Ec;
                std::filesystem::create_directories(std::filesystem::path(Local.toStdString()).parent_path(), Ec);
                const QString Tmp = Local + ".cvtmp";        // temp+rename so a reader never sees a half-written cover
                QFile::remove(Tmp);
                if (QFile::copy(Bundle, Tmp) && QFile::rename(Tmp, Local))
                { MissDest.remove(Local); return Local; }
                QFile::remove(Tmp);
            }
            MissDest.remove(Local);
            return Bundle;                                   // promotion not possible → serve the in-bundle bytes
        }
    }
    // Not on disk anywhere local → the tile paints BLANK. Remember the miss for the fetch sweep (received covers).
    if (!Cid.isEmpty() && !MissDest.contains(Local))
    {
        MissDest.insert(Local, Cid);
        if (!SweepSoon)
        {
            SweepSoon = true;
            QTimer::singleShot(SweepDebounceMs, this, &CoverCache::sweepNow);
        }
    }
    return QString();
}

void CoverCache::prefetch(const NodeIndex &Idx)
{
    if (AssetsRoot.isEmpty()) return;
    bool Added = false;
    for (const auto &[Key, N] : Idx.Nodes)
    {
        (void)Key;
        if (!N.Meta.is_object() || !N.Meta.contains("COVER")) continue;
        QString File, Cid;
        Locate(N.Meta["COVER"], File, Cid);
        if (Cid.isEmpty()) continue;
        const QString PackageDir = QString::fromStdString(N.BundleDir.string());
        const QString Local = coverPath(File, Cid, PackageDir);
        if (MissDest.contains(Local) || QFileInfo::exists(Local)) continue;
        if (!File.isEmpty() && !PackageDir.isEmpty()
            && QFileInfo::exists(QString::fromStdString(ContentFile::Path(PackageDir.toStdString(), File.toStdString(), Cid.toStdString()).string())))
            continue;                                   // its bytes are in its bundle: resolve() promotes them when painted
        MissDest.insert(Local, Cid);
        Added = true;
    }
    if (Added && !SweepSoon)
    {
        SweepSoon = true;
        QTimer::singleShot(SweepDebounceMs, this, &CoverCache::sweepNow);
    }
}

CoverCache::Stamp CoverCache::StampOf(const QString &Path)
{
    const QFileInfo Fi(Path);
    return Fi.exists() ? Stamp{ Fi.lastModified(), Fi.size() } : Stamp{};
}

QPixmap CoverCache::take(const QString &Path)
{
    const Stamp Now = StampOf(Path);
    if (auto It = Pixmaps.find(Path); It != Pixmaps.end())
    {
        if (It->At == Now) return It->Pix;
        Pixmaps.erase(It);                                 // the file changed since: decoded again
    }
    if (auto It = Decoded.find(Path); It != Decoded.end())
    {
        DecodedImage D = std::move(*It);
        Decoded.erase(It);
        if (D.At == Now)
        {
            QPixmap P = QPixmap::fromImage(std::move(D.Img));   // a QPixmap is made on the GUI thread only
            Pixmaps.insert(Path, Kept{ P, Now });
            return P;
        }
    }
    if (!Decoding.contains(Path) && !Undecodable.contains(Path))
    {
        Decoding.insert(Path);
        ++Decodes;
        QThreadPool::globalInstance()->start([this, Path, Max = DisplayMax, Gen = DisplayGen] {
            //The bytes first, then the decode: Qt probes the image format under one lock for the whole process, and a
            //probe that read the file itself held it for as long as a busy disk took — while the GUI thread, making
            //any icon, waited on it (most of a minute at startup).
            QImage Img;
            const Stamp At = StampOf(Path);                  // the file as read: a change after it is decoded again
            if (QFile F(Path); F.open(QIODevice::ReadOnly)) Img = QImage::fromData(F.readAll());
            if (!Img.isNull() && Max.isValid() && (Img.width() > Max.width() || Img.height() > Max.height()))
                Img = Img.scaled(Max, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);   // as a card draws it
            QMetaObject::invokeMethod(this, [this, Path, Gen, At, Img = std::move(Img)]() mutable {   // the cache lives for the app
                Decoding.remove(Path);
                if (Gen != DisplayGen) { emit coverDecoded(); return; }   // decoded for smaller cards: surfaces take it again
                if (Img.isNull()) { Undecodable.insert(Path); return; }
                Decoded.insert(Path, DecodedImage{ std::move(Img), At });
                emit coverDecoded();
            }, Qt::QueuedConnection);
        });
    }
    return QPixmap();
}

QPixmap CoverCache::scaled(const QPixmap &From, int W, int H)
{
    if (From.isNull() || W <= 0 || H <= 0) return QPixmap();
    const QPair<qint64, qint64> Key{ From.cacheKey(), (qint64(W) << 32) | qint64(H) };
    if (auto It = Scaled.constFind(Key); It != Scaled.constEnd()) return *It;
    //Sizes come and go (a card-size slider dragged): what no card draws any more is let go now and then.
    if (Scaled.size() > 4 * Pixmaps.size() + 256) Scaled.clear();
    ++Scales;
    QPixmap P = From.scaled(W, H, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    Scaled.insert(Key, P);
    return P;
}

void CoverCache::setDisplaySize(QSize Max)
{
    const bool Grew = DisplayMax.isValid() && (Max.width() > DisplayMax.width() || Max.height() > DisplayMax.height());
    DisplayMax = Max;
    if (!Grew) return;
    ++DisplayGen;
    Decoded.clear();   // decoded for smaller cards
    Pixmaps.clear();
    Scaled.clear();
    emit coversTooSmall();
}

void CoverCache::sweepNow()
{
    SweepSoon = false;
    if (MissDest.isEmpty() || !OnlineProbe()) return;   // offline: an enqueue would fail terminally, not wait

    std::vector<IpfsWrapper::FetchTarget> Batch;
    for (auto It = MissDest.begin(); It != MissDest.end(); )
    {
        if (QFileInfo::exists(It.key())) { It = MissDest.erase(It); continue; }   // landed since recorded
        //A bundle path whose folder is gone: the package was deleted → a ghost miss. ASSETS is the store itself, made by
        //its first fetch.
        if (!It.key().startsWith(QDir::cleanPath(AssetsRoot) + "/") && !QFileInfo(It.key()).dir().exists()) { It = MissDest.erase(It); continue; }
        Batch.push_back({ It.value().toStdString(), It.key().toStdString(), /*Optional=*/true, /*Dir=*/false, /*Verify=*/false, /*Front=*/true });
        ++It;
    }
    if (Batch.empty()) return;
    // Covers are ORDINARY queue items — no special-casing. Enqueued (optional, deduped against in-flight jobs) at the
    // front (small files the user is looking at), in the enqueue itself: bumped after it, a cover could already have had
    // its turn and fail to be bumped. The rolling scheduler handles a dead cover exactly like any item: it stalls, is
    // demoted to the back with a backoff, and a healthy download runs — and a requeue resets a rotated job's priority,
    // so dead art can never keep outranking games.
    (void)IpfsWrapper::EnqueueBatch(Batch);
}

void CoverCache::onTransferFinished(const QString & Cid, bool Ok, const QString & /*Error*/)
{
    // Ours? (The feed carries every download's completions.) On success the file is on disk — surfaces re-resolve
    // and the miss entries clear there; on failure the miss stays recorded and rides the next sweep.
    bool Ours = false;
    for (const QString & C : MissDest) if (C == Cid) { Ours = true; break; }
    // On success the file is on disk (resolve() clears the miss) and a cover that did not decode gets another try; a
    // failure just leaves the miss recorded to ride the next sweep — retry, batched.
    if (Ours && Ok) { Undecodable.clear(); emit coverReady(Cid); }
}

void CoverCache::onNetworkOnline()
{
    sweepNow();                    // the misses that piled up while offline go into the queue now
    emit coverReady(QString());    // nudge every surface to re-resolve (handlers ignore the cid, repaint debounced)
}
