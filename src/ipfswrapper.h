#ifndef IPFSWRAPPER_H
#define IPFSWRAPPER_H

#include <string>
#include <vector>
#include <functional>

#include <QObject>
#include <QString>

// ---------------------------------------------------------------------------
// IpfsWrapper — a thin wrapper around the embedded in-process IPFS node
// (external/VidyaGodIPFS → libvgipfs.so), called via its C ABI (vgipfsapi.h).
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

// True if the embedded node started (its repo opened). The node is compiled in, so this is normally true.
bool Available();

// True if the embedded node's network stack is up (joined the swarm/DHT). Seeding/fetching peers needs this.
bool DaemonRunning();

// Fetches a CID's content write-through to DestPath (creating parent dirs) and seeds it from there as a filestore
// reference — one copy at the destination, no separate blockstore/cache copy. This is THE fetch primitive:
// everything (game content, runner builds, covers) materializes in place at its local PATH inside the package's
// LIBRARY dir. No-op (returns DestPath) if DestPath already exists. Returns DestPath on success, "" on failure
// (with *Error set when provided). Reports lifecycle/progress through the TransferCallback.
std::string FetchToPath(const std::string &Cid, const std::string &DestPath, std::string *Error = nullptr);

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

// ----- cancellation: abort an in-flight FetchToPath for a CID at its next checkpoint -----
// RequestCancel marks a CID so a running FetchToPath aborts; ClearCancel un-marks it (call once the import has
// returned, so a later re-download isn't pre-cancelled). Best-effort, thread-safe.
void RequestCancel(const std::string &Cid);
void ClearCancel(const std::string &Cid);

// ----- best-effort status helpers for the IPFS tab (empty/0 when offline) -----

// Number of connected swarm peers, 0 if offline.
int PeerCount();

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

private:
    explicit IpfsManager(QObject * parent = nullptr);
};

#endif // IPFSWRAPPER_H
