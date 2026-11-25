#include "nodeservice.h"
#include "apppaths.h"            // AppPaths::DataRoot (the IPFS repo lives at <DataRoot>/IPFS)
#include "appmodel.h"
#include "asyncwork.h"
#include "covercache.h"
#include "downloadmanager.h"
#include "peersharing.h"
#include "ipfsmodel.h"
#include "ipfswrapper.h"

#include <QTimer>

#include <algorithm>
#include <memory>

NodeService::NodeService(AppModel &M, IpfsModel &I, DownloadManager &D, QObject *Parent)
    : QObject(Parent), Model(M), Ipfs(I), Downloads(D)
{
    // Resume any downloads a previous run left interrupted (crash/close) — but only once the embedded node's network
    // is up, so the fetch can actually proceed. Polls briefly then stops; a no-op when there's nothing to resume.
    // Started by ready() (only after the node is brought up — networking is opt-in), not at construction.
    ResumeTimer = new QTimer(this);
    ResumeTimer->setInterval(1500);
    connect(ResumeTimer, &QTimer::timeout, this, [this]{
        if (IpfsWrapper::DaemonRunning()) {
            ResumeTimer->stop();
            Downloads.resumeAll();
            // The network is up now: retry any cover whose fetch gave up while it was down (else those tiles stay
            // blank until the app restarts). Clears the negative cache and re-resolves every cover surface.
            CoverCache::instance()->onNetworkOnline();
        }
    });

    // Networking is opt-in (Settings → IPFS): the toggle starts or stops the node.
    connect(&Model, &AppModel::networkingChanged, this, [this](bool on){ apply(on); });
}

void NodeService::start()
{
    // Deferred one turn of the event loop (the window is up first), and only when the user opted into networking.
    NetworkingOn = Model.networkingEnabled();   // arm the retry loop for this initial start too
    if (NetworkingOn) QTimer::singleShot(0, this, [this]{ startAsync(); });
}

void NodeService::apply(bool enabled)
{
    NetworkingOn = enabled;
    if (enabled)
    {
        NodeRetryDelayMs = 2000;
        startAsync();
    }
    else stopAsync();
}

void NodeService::stopAsync()
{
    //Brought fully down — no network activity until re-enabled — off the GUI thread: a stop waits for the node's
    //closes (the DHT, the host, a datastore compaction in flight), and the window froze for all of it.
    ResumeTimer->stop();
    if (NodeStopPending) return;
    NodeStopPending = true;
    AsyncWork::Run(this,
        []{ IpfsWrapper::StopNode(); },
        [this]{
            NodeStopPending = false;
            Ipfs.refreshNow();   // reflect the "node off" state
            if (NetworkingOn) startAsync();   // turned on again while it stopped
        });
}

void NodeService::startAsync()
{
    if (NodeStopPending) return;                               // a stop in flight starts it again when done
    if (IpfsWrapper::Available()) { ready(); return; }   // already running (e.g. re-enabled)
    if (NodeStartPending) return;                              // an attempt or scheduled retry is already in flight
    NodeStartPending = true;
    const std::string Repo = (AppPaths::DataRoot() / "IPFS").string();
    auto Ok = std::make_shared<bool>(false);
    AsyncWork::Run(this,
        [Repo, Ok]{ std::string Err; *Ok = IpfsWrapper::StartNode(Repo, &Err); },   // off-thread: repo open + swarm join can take a moment
        [this, Ok]{
            NodeStartPending = false;
            if (*Ok && !NetworkingOn) { stopAsync(); return; }   // turned off while it started: down again
            if (*Ok) { NodeRetryDelayMs = 2000; ready(); return; }
            // A start can fail transiently — most commonly the previous instance still holds the repo's
            // datastore lock while it finishes shutting down ("resource temporarily unavailable" on a quick
            // app restart). Retry with backoff instead of leaving the node silently off until the next
            // launch; a permanent holder just keeps this ticking quietly in the background.
            if (!NetworkingOn) return;
            QTimer::singleShot(NodeRetryDelayMs, this,
                               [this]{ if (NetworkingOn && !IpfsWrapper::Available()) startAsync(); });
            NodeRetryDelayMs = std::min(NodeRetryDelayMs * 2, 30000);
        });
}

void NodeService::ready()
{
    // The node's repo is open now (Available()==true); refresh the IPFS tab immediately so it's functional the moment
    // networking is enabled, instead of waiting for its periodic tick. Its network stack comes up shortly after — the
    // ResumeTimer polls for that, then resumes any interrupted downloads.
    Ipfs.refreshNow();
    // The Network tab reads live node state (peer code + Verify&Publish enable) but only refreshes on
    // networkingChanged — which fired at TOGGLE time, before this async start finished, so it is still showing
    // "networking off" with Publish disabled. Tell it the node is up now.
    Model.markNodeReady();
    if (!ResumeTimer->isActive()) ResumeTimer->start();

    // Fetch/index any not-yet-present CID package sources now the node is up — this is what bootstraps the default
    // runners source on a first run (git repos, which needed no node, previously did this at config-init time).
    Model.syncSources();

    // Re-arm peer-library shares: Go's share table is in-memory (empty on every start), config["Sharing"] is the
    // durable record. Replay it now the peer service is live, else the seeder half is silently dead until the user
    // toggles a share again. Also refresh what peers share WITH us so the peer catalog is current after a restart.
    {
        Model.peers().pushPresenceDeny();   // apply the per-peer presence-hide roster now the peer service is live
        Model.peers().reRegisterShares();
        Model.peers().acceptWaitingRequests();   // server mode: requests that came while we were away
        // Materialise every receiving-from peer's browsable stubs from our PERSISTED snapshot (so the catalog is
        // populated at startup without waiting for a fresh snapshot) AND re-request in case their shares changed. This
        // is what makes a restart show a peer's library again — applyPeerLibrarySnapshot only fires on a NEW snapshot.
        Model.peers().reconcileReceivedLibraries();
    }
}

