#ifndef IPFSWRAPPER_H
#define IPFSWRAPPER_H

#include <string>
#include <vector>
#include <functional>

#include <QObject>
#include <QString>

// ---------------------------------------------------------------------------
// IpfsWrapper — a thin wrapper around the Kubo `ipfs` CLI.
//
// Kubo is an OPTIONAL external dependency: every function degrades gracefully
// when `ipfs` is absent (Available() is false; the rest return empty/false).
// This is FETCH/SEED ONLY — no publishing. VidyaGod NEVER starts the daemon; it
// only detects whether the user is running one (seeding requires their daemon).
//
// These are free functions usable off the GUI thread and in headless mode (the
// launch worker calls FetchSync). The IPFS tab uses the status/pin helpers and
// the IpfsManager signal hub (see ipfsmanager parts) for live transfer display.
// ---------------------------------------------------------------------------
namespace IpfsWrapper {

// True if the `ipfs` binary is on PATH.
bool Available();

// True if a local Kubo daemon is reachable (online mode) — pinned content only
// actually seeds to peers while a daemon is running. Detect only; never started.
bool DaemonRunning();

// The deterministic on-disk cache location for a CID: ~/.VidyaGod/DOWNLOADS/ipfs/<CID>.
// Pure (no I/O) — ResolveLayerSource uses it to resolve an ipfs SOURCE to a path.
std::string CachePath(const std::string &Cid);

// True if CachePath(Cid) holds a completed fetch (its sibling ".complete" marker exists).
bool IsCached(const std::string &Cid);

// Fetches a CID into its cache (`ipfs get`) and pins it (`ipfs pin add`) so it seeds,
// blocking until done. No-op if already cached. Returns the cache path on success, or
// "" on failure (with *Error set to a human-readable reason when provided).
std::string FetchSync(const std::string &Cid, std::string *Error = nullptr);

// ----- best-effort status helpers for the IPFS tab (empty/0 when unavailable) -----

// Number of connected swarm peers (`ipfs swarm peers`), 0 if no daemon.
int PeerCount();

// Human-readable local repo size from `ipfs repo stat --human` (e.g. "1.2 GB"), "" if unknown.
std::string RepoSizeHuman();

// A locally-pinned (seeded) CID.
struct PinEntry { std::string Cid; };

// Recursively-pinned CIDs (`ipfs pin ls --type=recursive`) — the seeded content.
std::vector<PinEntry> Pins();

// Unpins a CID (`ipfs pin rm`). Returns true on success.
bool Unpin(const std::string &Cid);

// ----- live transfer reporting (so the IPFS tab can show in-flight fetches) -----
// FetchSync (which may run on a worker thread) reports its lifecycle through an optional callback —
// mirrors the Log() callback hook. IpfsManager installs one to marshal events onto the GUI thread.
struct TransferEvent {
    enum Kind { Started, Progress, Finished } Kind;
    std::string Cid;
    double      Percent = -1.0;   // 0..100 for Progress; -1 if unknown
    bool        Ok = false;       // Finished only
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
    void transferFinished(QString cid, bool ok);

private:
    explicit IpfsManager(QObject * parent = nullptr);
};

#endif // IPFSWRAPPER_H
