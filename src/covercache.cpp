#include "covercache.h"
#include "downloadqueue.h"
#include "ipfswrapper.h"

#include <QDir>
#include <QFileInfo>
#include <QTimer>

namespace {
constexpr int SweepPeriodMs   = 60'000;   // the steady cadence: a minuscule stat-sweep of recorded misses
constexpr int SweepDebounceMs = 2'000;    // first miss after quiet → sweep soon, so a fresh catalog isn't blank for a minute
constexpr int CoverAttemptMs  = 30'000;   // ONE bounded attempt per sweep. Unbounded would park a cover nobody seeds
                                          // on a DownloadSlot forever (3 dead covers = every game download wedged);
                                          // the sweep itself is the retry-forever loop, one attempt per lap.
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

QString CoverCache::resolve(const nlohmann::ordered_json &Cover, const QString &PackageDir)
{
    QString File, Cid;
    Locate(Cover, File, Cid);
    if (File.isEmpty()) return QString();

    QString Local = QDir::cleanPath(PackageDir + "/" + File);
    if (QFileInfo::exists(Local))
    {
        MissDest.remove(Local);            // landed (by us or anyone) — stop tracking
        return Local;
    }
    // Not on disk → the tile paints BLANK. The only side effect here is remembering the miss for the sweep.
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
        if (QFileInfo::exists(It.key())) { FailedOnce.remove(It.value()); It = MissDest.erase(It); continue; }   // landed since recorded
        if (!QFileInfo(It.key()).dir().exists())                                  // package deleted → the miss is a
        { FailedOnce.remove(It.value()); It = MissDest.erase(It); continue; }     // ghost; fetching would resurrect the dir
        Batch.push_back({It.value().toStdString(), It.key().toStdString(), /*Optional=*/true, CoverAttemptMs});
        ++It;
    }
    if (Batch.empty()) return;
    // ONE batch through the one queue all content uses: deduped by CID against in-flight jobs (a re-sweep of a
    // still-downloading cover just joins it), a previously-failed CID is requeued — that IS the retry policy.
    (void)IpfsWrapper::EnqueueBatch(Batch);
    // Front of the queue for FIRST-time misses only (small files the user is staring at). A cover that already
    // failed once keeps normal priority on its re-sweeps, so dead art can never keep jumping ahead of games.
    for (const auto & T : Batch)
    {
        const QString Cid = QString::fromStdString(T.Cid);
        if (!FailedOnce.contains(Cid)) IpfsWrapper::PrioritizeDownload(T.Cid);
    }
}

void CoverCache::onTransferFinished(const QString & Cid, bool Ok, const QString & /*Error*/)
{
    // Ours? (The feed carries every download's completions.) On success the file is on disk — surfaces re-resolve
    // and the miss entries clear there; on failure the miss stays recorded and rides the next sweep.
    bool Ours = false;
    for (const QString & C : MissDest) if (C == Cid) { Ours = true; break; }
    if (!Ours) return;
    if (Ok) { FailedOnce.remove(Cid); emit coverReady(Cid); }
    else FailedOnce.insert(Cid);       // demote its re-sweeps to normal priority (see sweepNow) — NOT a negative cache

}

void CoverCache::onNetworkOnline()
{
    sweepNow();                    // the misses that piled up while offline go into the queue now
    emit coverReady(QString());    // nudge every surface to re-resolve (handlers ignore the cid, repaint debounced)
}
