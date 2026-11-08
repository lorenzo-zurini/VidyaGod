#ifndef NODESERVICE_H
#define NODESERVICE_H

#include <QObject>

class AppModel;
class DownloadManager;
class IpfsModel;
class QTimer;

// The embedded IPFS node's life in the app: brought up when networking is on (off the GUI thread, retried with a
// backoff while a previous instance still holds the repo), brought down when it is turned off, and the work its
// readiness unblocks — the IPFS and Network tabs refreshed, sources synced, friend shares re-armed and received
// libraries reconciled; once its network is up, interrupted downloads resumed and failed covers retried.
class NodeService : public QObject
{
    Q_OBJECT
public:
    NodeService(AppModel &Model, IpfsModel &Ipfs, DownloadManager &Downloads, QObject *Parent = nullptr);
    void start();   // at startup, once the window is up: the node comes up when networking is on

private:
    void apply(bool Enabled);   // networking toggled: start or stop the node
    void startAsync();          // open the repo off-thread, retrying with a backoff
    void ready();               // the repo is open: what waited for it

    AppModel &Model;
    IpfsModel &Ipfs;
    DownloadManager &Downloads;
    QTimer *ResumeTimer = nullptr;   // polls for the node's network coming up, then resumes downloads once
    bool NetworkingOn = false;       // mirrors the runtime networking toggle (gates node-start retries)
    bool NodeStartPending = false;   // a StartNode attempt (or scheduled retry) is in flight
    int NodeRetryDelayMs = 2000;     // backoff for failed node starts (doubles up to 30s)
};

#endif // NODESERVICE_H
