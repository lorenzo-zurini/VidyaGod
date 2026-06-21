#ifndef IPFSTAB_H
#define IPFSTAB_H

#include <QWidget>
#include <QHash>
#include <QSet>
#include <QString>
#include <QPair>

#include <vector>

#include "ipfswrapper.h"   // IpfsWrapper::StatInfo / PinEntry

class MainWindow;
class QLabel;
class QTableWidget;
class QTreeWidget;
class QTimer;
class QTableWidgetItem;
class QTreeWidgetItem;

// ---------------------------------------------------------------------------
// IpfsTab — the "IPFS" tab extracted out of MainWindow: live transfers table, seeded (pinned) content tree, node
// status, the IpfsManager signal handling, the stall watchdog, and the periodic refresh. It's the tab QWidget
// itself (MainWindow adds it). It reads the catalog/config from MainWindow (friend), and exposes a small API the
// download flow uses to seed/clear transfer rows.
// ---------------------------------------------------------------------------
class IpfsTab : public QWidget
{
    Q_OBJECT
public:
    IpfsTab(MainWindow &Owner, QWidget *parent = nullptr);

    void refresh();                                                  // off-thread status + pin gather (no-op if ipfs absent)
    void setActive(bool On);                                         // start/stop the periodic refresh when the tab is shown

    // Used by the Catalog download flow (DownloadManager):
    QTableWidgetItem *ensureTransferRow(const QString &cid, const QString &status);
    void queueTransfer(const QString &cid);                          // pre-show a download CID as "Queued"
    void clearQueuedTransfer(const QString &cid);                    // drop a still-queued row (download finished/cancelled)

private:
    void buildUi();
    void applySnapshot(bool Daemon, int Peers, const QString &Repo,
                       const std::vector<IpfsWrapper::PinEntry> &Pins,
                       const QHash<QString, long long> &Sizes);
    void gatherHealth();

    MainWindow &Mw;

    QLabel       *IpfsStatusLabel = nullptr;
    QTableWidget *IpfsTransfers   = nullptr;   // Name|Size|Progress|Speed|Status|CID — live + queued fetches (sortable)
    QTreeWidget  *IpfsPins        = nullptr;   // pinned (seeded) CIDs, grouped by package (Name|Size|Health|CID)
    QTimer       *IpfsRefreshTimer = nullptr;
    bool          IpfsRefreshInFlight = false; // a worker is already gathering ipfs status — skip re-entrancy

    QHash<QString, QTableWidgetItem*>          IpfsTransferProgress;  // CID → progress cell item (survives re-sorting)
    QSet<QString>                              IpfsTransferQueued;    // CIDs shown as "Queued" (in a download but not yet started)
    QHash<QString, QPair<qlonglong,qlonglong>> IpfsTransferSpeed;    // CID → {sampleBytes, sampleMs} for the rate calc
    QHash<QString, qlonglong>                  IpfsTransferLastProgress; // CID → ms of last forward progress (stall detection)
    QSet<QString>                              IpfsTransferStalled;   // CIDs currently shown as stalled
    QTimer       *IpfsStallTimer = nullptr;    // ages out phantom speed + flags transfers with no recent progress

    QHash<QString, QString> IpfsCidLabels;     // CID → human label ("<package> — <component>")
    QHash<QString, QString> IpfsCidPackages;   // CID → owning package name (for grouping)
    QHash<QString, IpfsWrapper::StatInfo> IpfsCidStat;  // CID → size + provider count (cached; Refresh clears it)
    QHash<QString, QTreeWidgetItem*> IpfsPinGroups;     // package name → group row (incremental tree)
    QHash<QString, QTreeWidgetItem*> IpfsPinChildren;   // CID → leaf row
    bool          IpfsHealthInFlight = false;  // a background provider-count pass is running
};

#endif // IPFSTAB_H
