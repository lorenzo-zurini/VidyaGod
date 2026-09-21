#include "covercache.h"
#include "downloadqueue.h"
#include "ipfswrapper.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
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
        const QString Bundle = QDir::cleanPath(PackageDir + "/" + File);
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

void CoverCache::sweepNow()
{
    SweepSoon = false;
    if (MissDest.isEmpty() || !OnlineProbe()) return;   // offline: an enqueue would fail terminally, not wait

    std::vector<IpfsWrapper::FetchTarget> Batch;
    for (auto It = MissDest.begin(); It != MissDest.end(); )
    {
        if (QFileInfo::exists(It.key())) { It = MissDest.erase(It); continue; }   // landed since recorded
        if (!QFileInfo(It.key()).dir().exists()) { It = MissDest.erase(It); continue; } // package deleted → ghost miss
        Batch.push_back({ It.value().toStdString(), It.key().toStdString(), /*Optional=*/true });
        ++It;
    }
    if (Batch.empty()) return;
    // Covers are ORDINARY queue items — no special-casing. Enqueue them (optional, deduped against in-flight jobs) and
    // bump each to the front (small files the user is looking at). The rolling scheduler handles a dead cover exactly
    // like any item: it stalls, is demoted to the back with a backoff, and a healthy download runs — and a requeue
    // resets a rotated job's priority, so dead art can never keep outranking games.
    (void)IpfsWrapper::EnqueueBatch(Batch);
    for (const auto & T : Batch) IpfsWrapper::PrioritizeDownload(T.Cid);
}

void CoverCache::onTransferFinished(const QString & Cid, bool Ok, const QString & /*Error*/)
{
    // Ours? (The feed carries every download's completions.) On success the file is on disk — surfaces re-resolve
    // and the miss entries clear there; on failure the miss stays recorded and rides the next sweep.
    bool Ours = false;
    for (const QString & C : MissDest) if (C == Cid) { Ours = true; break; }
    if (Ours && Ok) emit coverReady(Cid);   // on success the file is on disk (resolve() clears the miss); a failure
                                            // just leaves the miss recorded to ride the next sweep — retry, batched.
}

void CoverCache::onNetworkOnline()
{
    sweepNow();                    // the misses that piled up while offline go into the queue now
    emit coverReady(QString());    // nudge every surface to re-resolve (handlers ignore the cid, repaint debounced)
}
