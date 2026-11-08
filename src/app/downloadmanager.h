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

#include "shelf.h"   // Shelf::GraftChoice


//The optional content's ticks as a content walk's graft choice: nothing decided yet = the row's pre-ticked grafts;
//otherwise exactly the ticked ones.
inline Shelf::GraftChoice TickedGrafts(const std::map<std::string, bool> &Ticks)
{
    if (Ticks.empty()) return std::nullopt;
    std::vector<std::string> Out;
    for (const auto &[K, On] : Ticks) if (On) Out.push_back(K);
    return Out;
}

class AppModel;
class IpfsModel;

// ---------------------------------------------------------------------------
// DownloadManager — the download service: the hydrate/runner-import worker, the in-flight bookkeeping (what each
// download needs, its size-weighted progress, cancels), and resume after a restart. It reads catalog/config through
// the AppModel and, on completion, asks the model to rebuild the catalog. No widget: it reports purely via signals
// (the IPFS tab + Catalog cards connect to them); a card's Download and Cancel — the dialog, the question — are the
// UI's (downloaddialog.h), and end here with a decided selection or key.
// ---------------------------------------------------------------------------
class DownloadManager : public QObject
{
    Q_OBJECT
public:
    DownloadManager(AppModel & model, IpfsModel & ipfs, QObject * parent = nullptr);
    //Cancels the downloads in flight and JOINS their workers: a worker must never outlive the manager it posts back to
    //(nor the application — a thread still inside Qt while the app tears down crashes its cleanup).
    ~DownloadManager() override;

public slots:
    // Re-launch any downloads that a previous run left interrupted (crash/close) — call once at startup when online.
    void resumeAll();

public:
    // The ONE non-interactive download core (shared by the dialog path, resumeAll, and the runner Import button which
    // feeds it a runner-only group: launchIds empty, runnerIds = {rid}). Persists the selection for crash-resume. A
    // runner install is not special — it is just this pump with runner-build targets in the batch.
    void beginDownload(const QString & key, const std::vector<std::string> & launchIds,
                       const std::vector<std::string> & runnerIds, const std::map<std::string, bool> & toggles);
    bool downloading(const QString & key) const { return DownloadingUids.contains(key); }   // in flight

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
    QStringList activeKeys() const { return QStringList(DownloadingUids.begin(), DownloadingUids.end()); }   // downloads in flight
    QStringList cidsOf(const QString &Key) const { return DownloadUidCids.value(Key); }   // a download's content CIDs
    //The CIDs cancelling Key's download may abort: its own, minus those another in-flight download still needs.
    QStringList cancellableCids(const QString &Key) const;
    //Cancel Key's download (a card's Cancel, once the user confirms): abort what is its own alone, and remember it.
    void cancelKey(const QString &Key);
    //What a cancelled download's end may purge from the cache: the CIDs its cancel aborted whose fetch did not finish
    //and that no other in-flight download needs — never a finished file, never another card's content.
    QStringList purgeOnCancel(const QString &Key) const;
private:
    void persistActive(const QString & key, const std::vector<std::string> & launchIds,
                       const std::vector<std::string> & runnerIds, const std::map<std::string, bool> & toggles);
    void unpersistActive(const QString & key);
    // Recompute a package's size-weighted download % from IpfsModel (per-CID pct/size) and emit downloadProgress.
    void recomputeProgress(const QString & cid);

    AppModel & Model;
    IpfsModel & Ipfs;                            // source of truth for per-CID transfer progress + size

    QSet<QString>               DownloadingUids;   // PACKAGEUIDs with an import in flight (survives rebuilds)
    QSet<QString>               CancellingUids;    // PACKAGEUIDs the user cancelled (suppresses the failure dialog)
    //in-flight content CID → every download (card key) that needs it: one variant sits under several tiles (RoC and
    //TFT), so two cards can download the same CIDs — each tracks and averages all of its own
    QHash<QString, QSet<QString>> DownloadCidToUid;
    QHash<QString, QStringList> DownloadUidCids;
    QHash<QString, QStringList> CancelledCids;     // key → the CIDs its cancel aborted   // PACKAGEUID → its content CIDs (for averaging progress)
};

#endif // DOWNLOADMANAGER_H
