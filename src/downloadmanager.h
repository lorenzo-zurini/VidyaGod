#ifndef DOWNLOADMANAGER_H
#define DOWNLOADMANAGER_H

#include <QObject>
#include <QSet>
#include <QHash>
#include <QString>
#include <QStringList>

#include <map>
#include <string>
#include <vector>

class AppModel;
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
    DownloadManager(AppModel & model, QWidget * dialogParent, QObject * parent = nullptr);

public slots:
    // The per-package download dialog + worker.
    void startDownload(LibraryGameCard * card);
    // Confirm + abort the in-flight download for a card's package.
    void requestCancel(LibraryGameCard * card);
    // Average a content CID's transfer percent onto its package and emit downloadProgress (wired from IpfsManager).
    void applyProgress(const QString & cid, double pct);
    // Re-launch any downloads that a previous run left interrupted (crash/close) — call once at startup when online.
    void resumeAll();

signals:
    void transferQueued(const QString & cid);      // pre-show a CID as "Queued" in the IPFS transfers table
    void transferUnqueued(const QString & cid);    // drop a still-queued CID row (download finished/cancelled)
    void transfersChanged();                       // the transfer set changed — refresh the IPFS tab
    void downloadStarted(const QString & groupKey);            // mark a package's Catalog card(s) downloading
    void downloadProgress(const QString & groupKey, double avg);  // paint averaged % onto the card(s)
    void downloadFinished(const QString & groupKey);           // clear the package's downloading state

private:
    // Non-interactive download core (shared by the dialog path + resumeAll); persists the selection for crash-resume.
    void beginDownload(const QString & key, const std::vector<std::string> & launchIds,
                       const std::vector<std::string> & runnerIds, const std::map<std::string, bool> & toggles);
    void persistActive(const QString & key, const std::vector<std::string> & launchIds,
                       const std::vector<std::string> & runnerIds, const std::map<std::string, bool> & toggles);
    void unpersistActive(const QString & key);

    AppModel & Model;
    QWidget *  DialogParent;

    QSet<QString>               DownloadingUids;   // PACKAGEUIDs with an import in flight (survives rebuilds)
    QSet<QString>               CancellingUids;    // PACKAGEUIDs the user cancelled (suppresses the failure dialog)
    QHash<QString, QString>     DownloadCidToUid;  // in-flight content CID → its owning PACKAGEUID
    QHash<QString, QStringList> DownloadUidCids;   // PACKAGEUID → its content CIDs (for averaging progress)
    QHash<QString, double>      DownloadCidPct;    // in-flight CID → latest percent (0..100)
    QHash<QString, qlonglong>   DownloadCidSize;   // CID → byte size (for size-weighted progress; filled off-thread)
};

#endif // DOWNLOADMANAGER_H
