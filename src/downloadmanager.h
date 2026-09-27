#ifndef DOWNLOADMANAGER_H
#define DOWNLOADMANAGER_H

#include <QObject>
#include <QSet>
#include <QHash>
#include <QString>
#include <QStringList>

#include <atomic>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class AppModel;
class IpfsModel;
class LibraryGameCard;
class QWidget;

// ---------------------------------------------------------------------------
// DownloadManager — owns the Catalog download lifecycle (the per-package download dialog, the hydrate/runner-import
// worker, and the in-flight bookkeeping). It reads catalog/config through the AppModel and, on completion, asks the
// model to rebuild the catalog. It NEVER references the other views: it reports transfer/progress state purely via
// signals (the IPFS tab + Catalog cards connect to them). A QWidget is held solely to parent the modal dialog.
// ---------------------------------------------------------------------------
class DownloadManager : public QObject
{
    Q_OBJECT
public:
    DownloadManager(AppModel & model, IpfsModel & ipfs, QWidget * dialogParent, QObject * parent = nullptr);
    //Cancels the downloads in flight and JOINS their workers: a worker must never outlive the manager it posts back to
    //(nor the application — a thread still inside Qt while the app tears down crashes its cleanup).
    ~DownloadManager() override;

public slots:
    // The per-package download dialog + worker.
    void startDownload(LibraryGameCard * card);
    // Confirm + abort the in-flight download for a card's package.
    void requestCancel(LibraryGameCard * card);
    // Re-launch any downloads that a previous run left interrupted (crash/close) — call once at startup when online.
    void resumeAll();

public:
    // The ONE non-interactive download core (shared by the dialog path, resumeAll, and the runner Import button which
    // feeds it a runner-only group: launchIds empty, runnerIds = {rid}). Persists the selection for crash-resume. A
    // runner install is not special — it is just this pump with runner-build targets in the batch.
    void beginDownload(const QString & key, const std::vector<std::string> & launchIds,
                       const std::vector<std::string> & runnerIds, const std::map<std::string, bool> & toggles);

signals:
    void transfersChanged();                       // the transfer set changed — refresh the IPFS tab
    void downloadStarted(const QString & groupKey);            // mark a package's Catalog card(s) downloading
    void downloadProgress(const QString & groupKey, double avg);  // paint averaged % onto the card(s)
    void downloadFinished(const QString & groupKey);           // clear the package's downloading state

private:
    //beginDownload's workers: a finished one is joined when the next download starts (a thread's stack is kept until
    //join — the tray daemon grew one per download), the rest on destruction.
    struct Worker { std::thread T; std::shared_ptr<std::atomic<bool>> Done; };
    std::list<Worker> Workers;
    void recomputeKeyProgress(const QString &Key);   // one card's size-weighted average
    void reapWorkers();
public:
    int workerCount() const { return (int)Workers.size(); }   // test observability: worker threads not yet joined
    QStringList cidsOf(const QString &Key) const { return DownloadUidCids.value(Key); }   // a download's content CIDs
    //The CIDs cancelling Key's download may abort: its own, minus those another in-flight download still needs.
    QStringList cancellableCids(const QString &Key) const;
private:
    void persistActive(const QString & key, const std::vector<std::string> & launchIds,
                       const std::vector<std::string> & runnerIds, const std::map<std::string, bool> & toggles);
    void unpersistActive(const QString & key);
    // Recompute a package's size-weighted download % from IpfsModel (per-CID pct/size) and emit downloadProgress.
    void recomputeProgress(const QString & cid);

    AppModel & Model;
    IpfsModel & Ipfs;                            // source of truth for per-CID transfer progress + size
    QWidget *  DialogParent;

    QSet<QString>               DownloadingUids;   // PACKAGEUIDs with an import in flight (survives rebuilds)
    QSet<QString>               CancellingUids;    // PACKAGEUIDs the user cancelled (suppresses the failure dialog)
    //in-flight content CID → every download (card key) that needs it: one variant sits under several tiles (RoC and
    //TFT), so two cards can download the same CIDs — each tracks and averages all of its own
    QHash<QString, QSet<QString>> DownloadCidToUid;
    QHash<QString, QStringList> DownloadUidCids;   // PACKAGEUID → its content CIDs (for averaging progress)
};

#endif // DOWNLOADMANAGER_H
