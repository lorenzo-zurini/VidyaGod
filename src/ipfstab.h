#ifndef IPFSTAB_H
#define IPFSTAB_H

#include <QWidget>
#include <QHash>
#include <QSet>
#include <QString>
#include <QPair>

#include <vector>

#include "ipfswrapper.h"   // IpfsWrapper::StatInfo / PinEntry

class AppModel;
class QLabel;
class QTableWidget;
class QTreeWidget;
class QTimer;
class QTableWidgetItem;
class QTreeWidgetItem;

// ---------------------------------------------------------------------------
// IpfsTab — the "IPFS" tab: live transfers table, seeded (pinned) content tree, node status, the IpfsManager signal
// handling, the stall watchdog, and the periodic refresh. It reads the catalog/config through the AppModel and
// exposes a small slot API the download flow (DownloadManager) drives by signal to seed/clear transfer rows.
// ---------------------------------------------------------------------------
class IpfsTab : public QWidget
{
    Q_OBJECT
public:
    IpfsTab(AppModel &model, QWidget *parent = nullptr);

public slots:
    void refresh();                                                  // off-thread status + pin gather (no-op if ipfs absent)
    void setActive(bool On);                                         // start/stop the periodic refresh when the tab is shown

    // Driven by the Catalog download flow (DownloadManager signals):
    void queueTransfer(const QString &cid);                          // pre-show a download CID as "Queued"
    void clearQueuedTransfer(const QString &cid);                    // drop a still-queued row (download finished/cancelled)

public:
    QTreeWidgetItem *ensureLeaf(const QString &cid);   // find/create a CID's leaf in the unified tree (its package group)

private:
    void buildUi();
    void applySnapshot(bool Daemon, int Peers, const QString &Repo, double DownBps, double UpBps,
                       const std::vector<IpfsWrapper::PinEntry> &Pins,
                       const QHash<QString, long long> &Sizes,
                       const QSet<QString> &Uploading);
    void gatherHealth();

    AppModel &Model;

    QTableWidget *IpfsStatusTable = nullptr;   // one-row status strip: Network|Peers|Seeded|↓|↑|Repo|Disk
    QLabel       *IpfsHintLabel    = nullptr;   // shown only when the node is off ("enable networking…")
    QTreeWidget  *IpfsPins        = nullptr;   // UNIFIED per-package tree: Name|Size|Progress|Speed|Status|CID|actions —
                                               // live transfers AND seeded content in one view (was two widgets)
    QTimer       *IpfsRefreshTimer = nullptr;
    bool          IpfsRefreshInFlight = false; // a worker is already gathering ipfs status — skip re-entrancy

    QHash<QString, QTreeWidgetItem*>           IpfsTransferProgress;  // CID → its leaf while a transfer is active/failed
    QSet<QString>                              IpfsTransferQueued;    // CIDs shown as "Queued" (in a download but not yet started)
    QHash<QString, QPair<qlonglong,qlonglong>> IpfsTransferSpeed;    // CID → {sampleBytes, sampleMs} for the rate calc
    QHash<QString, qlonglong>                  IpfsTransferLastProgress; // CID → ms of last forward progress (stall detection)
    QSet<QString>                              IpfsTransferStalled;   // CIDs currently shown as stalled
    QSet<QString>                              IpfsPendingSources;    // configured source CIDs not yet fetched (shown as "Not fetched")
    QTimer       *IpfsStallTimer = nullptr;    // ages out phantom speed + flags transfers with no recent progress

    QHash<QString, QString> IpfsCidLabels;     // CID → human label ("<package> — <component>")
    QHash<QString, QString> IpfsCidPackages;   // CID → owning package name (for grouping)
    QHash<QString, IpfsWrapper::StatInfo> IpfsCidStat;  // CID → size + provider count (cached; Refresh clears it)
    QHash<QString, QTreeWidgetItem*> IpfsPinGroups;     // package name → group row (incremental tree)
    QHash<QString, QTreeWidgetItem*> IpfsPinChildren;   // CID → leaf row
    bool          IpfsHealthInFlight = false;  // a background provider-count pass is running
};

#endif // IPFSTAB_H
