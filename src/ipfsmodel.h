#ifndef IPFSMODEL_H
#define IPFSMODEL_H

#include <QObject>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QPair>
#include <QSet>

#include <cstdint>

#include "ipfswrapper.h"   // IpfsWrapper::StatInfo / PinEntry

class AppModel;
class QTimer;

// ---------------------------------------------------------------------------
// IpfsModel — the download/IPFS runtime state, extracted out of IpfsTab so the view is pure. It is the SINGLE consumer
// of the node's transfer events (IpfsManager) and owns:
//   • per-CID transfer/seed state (phase, %, speed, size, health) + human label/package/category,
//   • the node status strip data (peers, repo, bandwidth, disk),
//   • the polling (status/pins/health) + stall watchdog timers,
// exposing it all through Qt signals. IpfsTab renders from it; DownloadManager reads per-CID progress from it (so the
// Catalog card bars and the IPFS tree share ONE source of truth). Peer to AppModel (reads catalog/config through it);
// AppModel does not depend on this. Lives on the GUI thread; off-thread gathers marshal back via invokeMethod.
// ---------------------------------------------------------------------------
class IpfsModel : public QObject
{
    Q_OBJECT
public:
    explicit IpfsModel(AppModel & model, QObject * parent = nullptr);

    // Presentation-free per-CID state (the view maps phase → colours/status text).
    struct CidState {
        QString label, package, category;
        enum Phase : std::uint8_t { Pending, Queued, Downloading, Pinning, Stalled, Errored, Seeded } phase = Seeded;
        double    pct       = -1.0;   // 0..100 during a transfer; -1 = indeterminate
        qlonglong size      = -1;     // CumulativeSize bytes; -1 unknown
        double    speedBps  = -1.0;   // -1 = none/unknown
        int       providers = -2;     // -2 not queried, -1 failed, >=0 count
        int       missing   = -1;     // -1 unknown, 0 present, 1 backing file gone
        bool      uploading = false;  // a peer is pulling it right now
        QString   error;              // failure reason (Errored)
    };

    // Node status strip.
    struct NodeStatus {
        bool      available = false;  // repo open (node started)
        bool      daemon    = false;  // network up
        int       peers     = 0;
        QString   repo;               // "5.0 GB / 10 GB"
        double    downBps = 0.0, upBps = 0.0;
        qlonglong diskFree  = -1;
        int       pinCount  = 0;
        qlonglong totalSize = 0;
    };

    // ── Read API ──
    const QHash<QString, CidState> & cids() const { return Cids; }
    bool                             has(const QString & cid) const { return Cids.contains(cid); }
    CidState                         state(const QString & cid) const { return Cids.value(cid); }
    const NodeStatus &               nodeStatus() const { return Status; }
    double    pct(const QString & cid)  const { return Cids.value(cid).pct; }    // for DownloadManager progress
    qlonglong size(const QString & cid) const { return Cids.value(cid).size; }

    // ── Package-level CIDs (first-class): the meta-CID of ONE package folder, between a file's content CID and a
    // whole source's library meta-CID. Others add it in Settings → Sources to receive exactly that package. ──
    QString packageDir(const QString & pkg) const { return PkgDirs.value(pkg); }
    QString packageCid(const QString & pkg) const;   // recorded CID from Settings.PackageCids, empty if never published

public slots:
    void setActive(bool on);                 // tab shown/hidden → start/stop the periodic refresh
    void refreshNow();                       // force a status/pin/health gather (clears cached health first if force)
    void cancel(const QString & cid);        // abort/drop a download (→ IpfsWrapper::CancelDownload)
    void prioritize(const QString & cid);    // jump a queued CID (→ IpfsWrapper::PrioritizeDownload)
    void markQueued(const QString & cid);    // a download enqueued a CID (pre-show it as Queued)
    void clearQueued(const QString & cid);   // a queued CID was dropped/finished before starting
    void seedFolder(const QString & dir);    // add a folder's published content to the node (off-thread)
    void publishPackage(const QString & pkg);       // mint/refresh the package-level meta-CID (off-thread), persist it
    void recheckHealth(const QStringList & cids);   // drop cached health for these CIDs and re-gather
    void unpinMany(const QStringList & cids);       // stop seeding a batch, then one refresh
    void addSource(const QString & cid, const QString & name);   // append a package source CID + kick a sync

signals:
    void cidChanged(const QString & cid);    // one CID's state changed → upsert its row
    void cidRemoved(const QString & cid);    // a CID left the set → drop its row
    void modelReset();                       // the whole CID set was reconciled (pins snapshot) → rebuild the tree
    void nodeStatusChanged();                // the status strip data changed
    void seedProgress(int done, int total);  // "Seeding N/M…" for the tab's Seed button
    void seedFinished(int seeded, int mismatched);
    void packagePublished(const QString & pkg, const QString & cid, const QString & error);

private:
    void refresh();                          // internal: kick the off-thread gather (no-op if node down / already running)
    void applySnapshot(const NodeStatus & status,
                       const std::vector<IpfsWrapper::PinEntry> & pins,
                       const QHash<QString, long long> & sizes,
                       const QSet<QString> & uploading);   // merge pins/status/sources → Cids + Status, emit signals
    void gatherHealth();                     // off-thread provider-count / missing pass for leaves lacking it
    void ensureSize(const QString & cid);    // async-fill one CID's byte size if unknown
    void ensureLabels(const QString & cid);  // fill one CID's label/package/category from the catalog if missing
    void rebuildLabels();                    // (re)derive label/package/category for all known CIDs from the catalog
    void tick();                             // stall watchdog: flag transfers with no recent forward progress

    AppModel & Model;

    QHash<QString, CidState>                   Cids;
    QHash<QString, QString>                    PkgDirs;           // package display name → its bundle dir (for publish)
    NodeStatus                                 Status;
    QSet<QString>                              PendingSources;    // configured source CIDs not yet fetched
    QHash<QString, QPair<qlonglong,qlonglong>> Speed;             // CID → {sampleBytes, sampleMs} for the rate calc
    QHash<QString, qlonglong>                  LastProgress;      // CID → ms of last forward progress (stall detection)

    QTimer * RefreshTimer = nullptr;
    QTimer * StallTimer   = nullptr;
    bool     Active         = false;
    bool     RefreshInFlight = false;
    bool     HealthInFlight  = false;
};

#endif // IPFSMODEL_H
