#ifndef DOWNLOADMANAGER_H
#define DOWNLOADMANAGER_H

#include <QObject>
#include <QSet>
#include <QHash>
#include <QString>
#include <QStringList>

class MainWindow;
class LibraryGameCard;

// ---------------------------------------------------------------------------
// DownloadManager — owns the Catalog download lifecycle (the per-package download dialog, the hydrate/runner-import
// worker, and the in-flight bookkeeping) extracted out of MainWindow. It still drives MainWindow-owned UI (the IPFS
// transfer table, the Available cards) so it holds a reference to MainWindow and is declared a friend of it; the
// transfer table + IPFS tab themselves stay in MainWindow. A single instance is created by MainWindow.
// ---------------------------------------------------------------------------
class DownloadManager : public QObject
{
    Q_OBJECT
public:
    explicit DownloadManager(MainWindow &Owner);

    // The per-package download dialog + worker (was MainWindow::DownloadAvailable).
    void startDownload(LibraryGameCard *card);
    // Confirm + abort the in-flight download for a card's package (was the cancelRequested handler).
    void requestCancel(LibraryGameCard *card);
    // Average a content CID's transfer percent onto its Available card(s) (was the applyProgress lambda).
    void applyProgress(const QString &cid, double pct);

    // For RebuildAvailableTab: is this package downloading, and its current averaged percent (-1 if unknown).
    bool   isDownloading(const QString &groupKey) const { return DownloadingUids.contains(groupKey); }
    double busyPercent(const QString &groupKey) const;

private:
    MainWindow &Mw;

    QSet<QString>               DownloadingUids;   // PACKAGEUIDs with an import in flight (survives rebuilds)
    QSet<QString>               CancellingUids;    // PACKAGEUIDs the user cancelled (suppresses the failure dialog)
    QHash<QString, QString>     DownloadCidToUid;  // in-flight content CID → its owning PACKAGEUID
    QHash<QString, QStringList> DownloadUidCids;   // PACKAGEUID → its content CIDs (for averaging progress)
    QHash<QString, double>      DownloadCidPct;    // in-flight CID → latest percent (0..100)
};

#endif // DOWNLOADMANAGER_H
