#ifndef DOWNLOADMANAGER_H
#define DOWNLOADMANAGER_H

#include <QObject>
#include <QSet>
#include <QHash>
#include <QString>
#include <QStringList>

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

signals:
    void transferQueued(const QString & cid);      // pre-show a CID as "Queued" in the IPFS transfers table
    void transferUnqueued(const QString & cid);    // drop a still-queued CID row (download finished/cancelled)
    void transfersChanged();                       // the transfer set changed — refresh the IPFS tab
    void downloadStarted(const QString & groupKey);            // mark a package's Catalog card(s) downloading
    void downloadProgress(const QString & groupKey, double avg);  // paint averaged % onto the card(s)
    void downloadFinished(const QString & groupKey);           // clear the package's downloading state

private:
    AppModel & Model;
    QWidget *  DialogParent;

    QSet<QString>               DownloadingUids;   // PACKAGEUIDs with an import in flight (survives rebuilds)
    QSet<QString>               CancellingUids;    // PACKAGEUIDs the user cancelled (suppresses the failure dialog)
    QHash<QString, QString>     DownloadCidToUid;  // in-flight content CID → its owning PACKAGEUID
    QHash<QString, QStringList> DownloadUidCids;   // PACKAGEUID → its content CIDs (for averaging progress)
    QHash<QString, double>      DownloadCidPct;    // in-flight CID → latest percent (0..100)
};

#endif // DOWNLOADMANAGER_H
