#ifndef IPFSWRAPPER_H
#define IPFSWRAPPER_H

#include <string>
#include <vector>
#include <functional>

#include <QObject>
#include <QString>

// ---------------------------------------------------------------------------
// IpfsWrapper — a thin wrapper around the embedded in-process IPFS node
// (VidyaGodIPFS → libvgipfs.so), called via its C ABI (vgipfsapi.h).
//
// There is NO external Kubo dependency any more: VidyaGod runs its own Boxo-based
// node against a private repo, started once at process startup (VgStart in main).
// FETCH/SEED + status only. The node fetches write-through to a filestore, so the
// destination file is the only on-disk copy (no blockstore duplication).
//
// These are free functions usable off the GUI thread and in headless mode (the
// launch worker calls FetchToPath). The IPFS tab uses the status/pin helpers and
// the IpfsManager signal hub (see ipfsmanager parts) for live transfer display.
// ---------------------------------------------------------------------------
namespace IpfsWrapper {

// Open the embedded node's repo + bring its network up. In the GUI this is DEFERRED to after the window shows (so
// startup isn't blocked) and GATED on the user's "Enable networking" toggle (OFF by default — launching already-
// hydrated games reads files by path through the VFS, no node needed; only download/seed needs the network).
// Headless CLI modes that need the node (--fetch/--seed/--node/…) start it up front. Idempotent. False (+*Error) on
// a repo-open failure.
bool StartNode(const std::string & RepoPath, std::string * Error = nullptr);
// Close the node (network down, repo closed). Called when the user disables networking, and at exit.
void StopNode();

// True if the embedded node started (its repo opened). False before StartNode (e.g. networking disabled).
bool Available();

// True if the embedded node's network stack is up (joined the swarm/DHT). Seeding/fetching peers needs this.
bool DaemonRunning();

// Fetches a CID's content write-through to DestPath (creating parent dirs) and seeds it from there as a filestore
// reference — one copy at the destination, no separate blockstore/cache copy. This is THE fetch primitive:
// everything (game content, runner builds, covers) materializes in place at its local PATH inside the package's
// LIBRARY dir. No-op (returns DestPath) if DestPath already exists. Returns DestPath on success, "" on failure
// (with *Error set when provided). Reports lifecycle/progress through the TransferCallback.
std::string FetchToPath(const std::string &Cid, const std::string &DestPath, std::string *Error = nullptr);

// Recursively materializes a UnixFS DIRECTORY CID (a folder of dehydrated packages) into DestDir — fetches the whole
// small manifest tree (node JSON + covers, no content bytes) over the network and writes it to disk. Requires the IPFS
// node's networking to be up. Returns DestDir on success, "" on failure (with *Error set). Used to add a package set by
// CID; each package's content hydrates later, on demand, via FetchToPath.
std::string FetchDirToPath(const std::string &Cid, const std::string &DestDir, std::string *Error = nullptr);

// Seeds a local file by adding it to the node's filestore by reference (blocks REFERENCE the file in place — no
// second copy) and returns its content-addressed CID, "" on failure (with *Error set when provided). This is how
// publishing dehydrates a package: each layer's local content is added → its CID is recorded in the manifest.
// Works offline (content enters the local repo); peers receive it once the node is online.
std::string AddNoCopy(const std::string &Path, std::string *Error = nullptr);

// ----- concurrency throttle: cap how many FetchToPath calls run at once (configurable) -----
// A single global limit shared across all downloads (every package's hydrate worker draws from it), so the user can
// trade bandwidth/peer-connections against parallelism. Changing it takes effect immediately for fetches not yet
// started. Default 3. SetMaxConcurrentDownloads clamps to [1, 32].
void SetMaxConcurrentDownloads(int N);
int  MaxConcurrentDownloads();

// RAII permit: construct to acquire a download slot (blocks until one is free), destruct to release it. Wrap each
// FetchToPath in one of these so at most MaxConcurrentDownloads() fetches run concurrently. Movable so a slot can be
// acquired by a dispatcher then handed to the worker thread that performs the fetch.
class DownloadSlot {
public:
    DownloadSlot();
    ~DownloadSlot();
    DownloadSlot(DownloadSlot &&Other) noexcept : Owned(Other.Owned) { Other.Owned = false; }
    DownloadSlot &operator=(const DownloadSlot &) = delete;
    DownloadSlot(const DownloadSlot &) = delete;
private:
    bool Owned = true;   // false after being moved-from, so only the live instance releases on destruction
};

// One file to fetch: its CID, the destination path, and whether a failure is tolerable (covers are optional).
struct FetchTarget { std::string Cid; std::string LocalPath; bool Optional = false; };

// Fetch every target CONCURRENTLY, each bounded by a DownloadSlot, so at most MaxConcurrentDownloads() run at once
// across the whole batch (the dispatcher acquires a slot before spawning each worker; a freed slot starts the next).
// Already-present destinations are skipped by FetchToPath. The FIRST non-optional failure stops dispatching further
// work, makes the call return false, and sets *Error; optional failures are logged and skipped. This is the single
// download pump — one batch can mix a game's content layers AND its runners' build layers so they download together.
bool FetchTargetsConcurrent(const std::vector<FetchTarget> &Targets, std::string *Error = nullptr);

// ----- cancellation: abort an in-flight FetchToPath for a CID at its next checkpoint -----
// RequestCancel marks a CID so a running FetchToPath aborts; ClearCancel un-marks it (call once the import has
// returned, so a later re-download isn't pre-cancelled). Best-effort, thread-safe.
void RequestCancel(const std::string &Cid);
void ClearCancel(const std::string &Cid);

// ----- best-effort status helpers for the IPFS tab (empty/0 when offline) -----

// Number of connected swarm peers, 0 if offline.
int PeerCount();

// Global node throughput in bytes/sec (down = received, up = sent); {0,0} when offline. Rolling rate from the
// libp2p bandwidth counter — the aggregate across all peers/streams (downloads + seeding).
struct BandwidthRates { double DownBps = 0.0; double UpBps = 0.0; };
BandwidthRates Bandwidth();

// Pinned CIDs served to a peer within the last WindowMs — the seeded items being uploaded right now. Empty when offline.
std::vector<std::string> ActiveUploads(int WindowMs);

// Human-readable local repo usage — "RepoSize / StorageMax" (e.g. "5.0 GB / 10 GB"), or just the size, "" if
// unknown.
std::string RepoSizeHuman();

// A locally-pinned (seeded) CID.
struct PinEntry { std::string Cid; };

// Recursively-pinned CIDs (`ipfs pin ls --type=recursive`) — the seeded content.
std::vector<PinEntry> Pins();

// Per-CID stats for the IPFS tab's Size + Health columns. SizeBytes is cheap; Providers is a slow DHT query
// (gather it off-thread and cache it).
struct StatInfo {
    long long SizeBytes = -1;   // CumulativeSize bytes (-1 = unknown)
    int       Providers = -2;   // distinct peers announcing the CID: -2 not queried, -1 query failed/no daemon, >=0 count
    int       Missing    = -1;  // backing file gone (orphaned ref): -1 not checked, 0 present, 1 missing
};

// CumulativeSize of a CID via `ipfs files stat` (cheap metadata; -1 if unknown).
long long CidSize(const std::string &Cid);

// True if a pinned CID's content was seeded by reference but its backing file has since been deleted (orphaned ref).
// Local + cheap (no network); surfaced in the IPFS tab as "Errored: missing files".
bool CidMissing(const std::string &Cid);

// True if the node holds the CID's block locally (a filestore reference or a plain block), without a network fetch.
// An orphaned reference (backing file gone) still counts as held — pair with CidMissing to tell them apart.
bool HasLocal(const std::string &Cid);

// Deletes a CID's closure (filestore references + plain blocks) and unpins it, so a subsequent AddNoCopy re-creates
// fresh references against a new backing file (the node's filestore otherwise skips re-adding a block it already has).
bool DropRef(const std::string &Cid);

// Purges a CID's locally-CACHED (bitswap) blocks — the partial data left by a cancelled/aborted download — and
// compacts to reclaim the disk. Offline (never fetches); only touches plain cached blocks, not filestore references.
bool DropCached(const std::string &Cid);

// ----- peer identity + explicit connectivity (diagnostics + direct peering) -----
// This node's libp2p peer ID ("" if offline).
std::string PeerID();
// Dialable /p2p/ multiaddrs for this node (so another node can connect straight to it).
std::vector<std::string> ListenAddrs();
// Dial + hold a direct connection to a peer at a full /p2p/ multiaddr. True on success.
bool Connect(const std::string &Multiaddr);

// Network availability ("file health"): how many peers announce the CID, via `ipfs routing findprovs`. SLOW (a DHT
// walk) — call off the GUI thread and cache. -1 if no daemon / the query failed; otherwise the provider count.
int ProviderCount(const std::string &Cid);

// Unpins a CID (`ipfs pin rm`). Returns true on success.
bool Unpin(const std::string &Cid);

// ----- live transfer reporting (so the IPFS tab can show in-flight fetches) -----
// FetchSync (which may run on a worker thread) reports its lifecycle through an optional callback —
// mirrors the Log() callback hook. IpfsManager installs one to marshal events onto the GUI thread.
struct TransferEvent {
    enum Kind { Started, Progress, Finished, Finalizing } Kind;   // Finalizing: bytes down, re-referencing ("pinning")
    std::string Cid;
    double      Percent = -1.0;   // 0..100 for Progress; -1 if unknown
    bool        Ok = false;       // Finished only
    std::string Error;            // Finished+!Ok: the failure reason (e.g. the ipfs error: "no space left on device")
};
using TransferCallback = std::function<void(const TransferEvent &)>;
void SetTransferCallback(TransferCallback Callback);

} // namespace IpfsWrapper

// ---------------------------------------------------------------------------
// IpfsManager — a GUI-thread QObject that relays IpfsWrapper transfer events as Qt signals. A single
// shared instance (instance()) bridges launch-worker fetches to the IPFS tab. Status/pin queries stay
// on IpfsWrapper's free functions; this only carries live transfer notifications.
// ---------------------------------------------------------------------------
class IpfsManager : public QObject
{
    Q_OBJECT
public:
    static IpfsManager * instance();

signals:
    void transferStarted(QString cid);
    void transferProgress(QString cid, double percent);
    void transferFinalizing(QString cid, double percent);         // all bytes down; "pinning" (re-reference) step; percent 0..100, -1 if indeterminate
    void transferFinished(QString cid, bool ok, QString error);   // error is the reason when !ok (else empty)
    void queueEnqueued(QString cid);                              // the download queue queued a CID (pre-show it)
    void queueRemoved(QString cid);                              // a still-queued CID was cancelled before starting

private:
    explicit IpfsManager(QObject * parent = nullptr);
};

#endif // IPFSWRAPPER_H
