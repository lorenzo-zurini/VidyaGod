#ifndef COVERCACHE_H
#define COVERCACHE_H

#include <QObject>
#include <QString>
#include <QHash>
#include <QSet>
#include <functional>

#include "nlohmann/json.hpp"

// ---------------------------------------------------------------------------
// CoverCache — resolves a package's cover art to a loadable image path. PAINT NEVER FETCHES.
//
// METADATA.COVER is dual-form, exactly like a content layer: a bare filename (authoring) or an object
// { "PATH": "<filename>", "SOURCE": { "TYPE": "ipfs", "CID": "<cid>" } } (published).
//
// resolve() is a pure disk check: PackageDir/PATH if it exists, else "" (blank tile) — its only side effect is
// RECORDING the miss (cid → dest), never fetching. A periodic SWEEP (~1/min, plus a short debounce after the
// first new miss and one on the online transition) batches every recorded miss into the ONE DownloadQueue all
// content uses — same path, same retry behavior, deduped by CID against in-flight jobs, prioritized (covers are
// small and the user is looking at the blank tile right now). When a cover's transfer finishes, coverReady(cid)
// fires so surfaces re-resolve and repaint; a failed cover simply stays in the miss set and rides the next sweep.
//
// This replaced a detached thread per cover with a 30 s deadline and a negative cache — all three were artifacts
// of not using the queue, and all three are gone.
// ---------------------------------------------------------------------------
class IpfsManager;

class CoverCache : public QObject
{
    Q_OBJECT
public:
    static CoverCache * instance();

    // Pure lookup: a loadable image path for Cover (string or {PATH,SOURCE}), or "" (paint it blank). A miss with
    // a known CID is recorded for the sweep; NOTHING is fetched here.
    QString resolve(const nlohmann::ordered_json &Cover, const QString &PackageDir);

    // Extracts the (filename, cid) locators from a COVER value. Either may be empty.
    static void Locate(const nlohmann::ordered_json &Cover, QString &File, QString &Cid);

    // Enqueue every still-missing recorded cover as ONE batch (no-op while offline / nothing missing). Runs on the
    // periodic timer; public so the online transition and tests can drive it directly.
    void sweepNow();

    // Called on the node's offline→online transition: sweep the misses that piled up while there was no network,
    // and nudge every surface to re-resolve.
    void onNetworkOnline();

    // Transfer completion from IpfsManager (any CID; non-cover CIDs are ignored). Public slot so tests can drive
    // it without a live node. A success emits coverReady(cid) and drops the miss; a failure keeps the miss for the
    // next sweep — retry-forever, batched, no negative cache.
    void onTransferFinished(const QString & Cid, bool Ok, const QString & Error);

    // Test observability: how many (cid,dest) misses are currently recorded.
    int missCount() const { return MissDest.size(); }

    // Test seam: override the "is the node online?" probe (defaults to IpfsWrapper::DaemonRunning) — the sweep
    // skips while offline (a pre-online enqueue would fail terminally instead of waiting).
    void setOnlineProbe(std::function<bool()> Probe);

signals:
    void coverReady(QString cid);

private:
    explicit CoverCache(QObject * parent = nullptr);
    std::function<bool()> OnlineProbe;      // "is the node online?" (injectable for tests)
    QHash<QString, QString> MissDest;       // dest path → cid, every cover seen missing and not yet landed
    bool SweepSoon = false;                 // a debounced near-term sweep is already scheduled
};

#endif // COVERCACHE_H
