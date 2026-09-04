#ifndef IPFSWRAPPER_H
#define IPFSWRAPPER_H

#include <string>
#include <vector>
#include <map>
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
// Seed a TEXT-ONLY Meta-CID directly from a package dir/collection: references the *.json manifests in place (no
// staging mirror), skipping content + DEFPREFIX/USERDATA. Same CID as adding a JSON-only mirror of the tree.
std::string AddNoCopyMeta(const std::string &Path, std::string *Error = nullptr);

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

// Distinct filestore backing paths whose file is gone — orphaned no-copy references the node can't serve. Empty = none.
// Cheap probe (one filestore scan) the app polls to trigger the on-demand orphan heal. Empty when offline.
std::vector<std::string> OrphanedRefPaths();

// One record per filestore entry whose bytes no longer verify against its backing range. Unlike OrphanedRefPaths
// (which only stats paths) this READS the data, so it also catches an entry whose file still exists but changed —
// and unlike VerifyCid it enumerates the WHOLE index, so it finds stale entries no manifest references any more.
// Those are the references that make a requesting peer hang: we advertise the block, then fail to deliver it.
struct UnservableRef { std::string Cid, Path, Err; int Status = 0; };   // Status 12 = contents changed, 11 = file gone
std::vector<UnservableRef> UnservableRefs();

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
long long CidSize(const std::string &Cid);       // may fetch over the network (bounded ~35s) — download paths only
long long CidSizeLocal(const std::string &Cid);  // local store only, fast -1 when unreadable — status/refresh paths

// True if a pinned CID's content was seeded by reference but its backing file has since been deleted (orphaned ref).
// Local + cheap (no network); surfaced in the IPFS tab as "Errored: missing files".
bool CidMissing(const std::string &Cid);

// True if the node holds the CID's block locally (a filestore reference or a plain block), without a network fetch.
// An orphaned reference (backing file gone) still counts as held — pair with CidMissing to tell them apart.
bool HasLocal(const std::string &Cid);

// Deletes a CID's closure (filestore references + plain blocks) and unpins it, so a subsequent AddNoCopy re-creates
// fresh references against a new backing file (the node's filestore otherwise skips re-adding a block it already has).
bool DropRef(const std::string &Cid);

// Reads every block of a CID out of the local blockstore — the same path bitswap serves from — and returns "" when
// the whole DAG is servable, else the first read error. CidMissing only stats the backing PATH, so it passes a
// reference whose file exists but whose BYTES changed; that ref then makes every requesting peer HANG (we advertise
// a block we cannot deliver) instead of failing over. This is the only check that catches it. I/O-bound: it reads
// the referenced bytes, so use it in an explicit deep pass, never on a background timer.
// "" when this CID looks DELIVERABLE — every backing file present and still the size its references cover —
// else the reason. Cheap (stat only, no content read), so it can run on every status refresh. This is what
// "Seeding" should be claiming: a pin alone proves nothing about whether we can hand a peer the bytes.
// A same-size edit still slips through; only VerifyCid's byte read catches that.
std::string CidServeStatus(const std::string &Cid);

// Blocks a PEER requested that we could not deliver, since the last call (the list drains). This is the
// uploader-side corruption signal BitTorrent does not have: there only the downloader hashes a piece, so a
// seeder with rotted data serves garbage forever and merely gets banned. Every block here is content-addressed
// and verified on read, so a failed serve is proof — at the exact moment it matters — that we advertised
// something we cannot produce.
struct ServeFailure { std::string Cid, Err; long long When = 0; };
std::vector<ServeFailure> ServeFailures();

std::string VerifyCid(const std::string &Cid);

// The UnixFS FILE size (payload bytes) a CID represents, from the local store; -1 when unknown. Reads only the
// root block, so it is cheap enough to run before every seed. This is the FREE half of "is the file on disk still
// the file we published?": a rebuilt zip or re-authored delta almost never lands on the identical byte count.
// It is a filter, not a proof — a same-size edit still needs VerifyCid's byte read.
long long CidFileSizeLocal(const std::string &Cid);

// What a file's CID WOULD be, WITHOUT seeding it — nothing enters the blockstore, filestore or pinset. Same
// importer settings as AddNoCopy, so it answers "do these bytes still match the CID we published?" (content drift)
// without the side effects of adding. Works with no node started. "" on failure, with *Error set.
std::string ComputeCid(const std::string &Path, std::string *Error = nullptr);

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

// Seed announce (3-level schema): hand the node the level-3 collection + level-2 package meta-CIDs; it announces every
// pinned CID to the DHT in 3 ordered passes (collections → packages → content) on start + periodically. Idempotent.
void SetSeedLevels(const std::vector<std::string> &Collections, const std::vector<std::string> &Packages);
// True once a pinned CID's DHT announce has completed (→ "seeding"); false while still "queued for seeding".
bool SeedAnnounced(const std::string &Cid);

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

// ---------------------------------------------------------------------------
// Friends / multiplayer social layer (VidyaGodIPFS/social.go + friend.go).
// A friend is found by their peer ID (for the node's Ed25519 key the peer ID embeds the public key, so it IS the
// shareable "friend code" and libp2p authenticates the friend for free). Contacts persist in the node's repo
// (<repo>/social.json) independently of the network; the live protocol needs networking to be up.
// ---------------------------------------------------------------------------

// One address-book entry. State is the friendship lifecycle: pending (we asked) / incoming (they asked) /
// accepted (mutual) / blocked. Online + LastSeen come from presence pings.
struct Contact {
    std::string PeerID;
    std::string Nick;
    std::string PicCID;
    std::string State;          // "pending" | "incoming" | "accepted" | "blocked"
    bool        Online   = false;
    long long   LastSeen = 0;   // unix-ms, 0 = never
};
struct Profile { std::string Nick; std::string PicCID; };

// This node's shareable friend code (its libp2p peer ID), "" if offline.
std::string FriendCode();
// This node's own profile (nickname + profile-picture content CID).
Profile GetProfile();
bool SetProfile(const std::string &Nick, const std::string &PicCID, std::string *Error = nullptr);
// Every contact (any state), unordered.
std::vector<Contact> FriendList();

// Send a friend request to a peer ID (records it as pending + notifies them with our profile).
bool FriendAdd(const std::string &PeerID, const std::string &Note = std::string(), std::string *Error = nullptr);
// Accept an incoming request (marks accepted + notifies the peer with our profile).
bool FriendAccept(const std::string &PeerID, std::string *Error = nullptr);
// Decline/unfriend (removes locally + best-effort notifies the peer).
bool FriendDecline(const std::string &PeerID, std::string *Error = nullptr);
// Block a peer (drops all their traffic; no notification).
bool FriendBlock(const std::string &PeerID, std::string *Error = nullptr);
// Remove a contact entirely (local only).
bool FriendRemove(const std::string &PeerID);
// Actively probe reachability now: 1 online, 0 offline, -1 n/a (offline node).
int FriendPing(const std::string &PeerID);

// Inbound friend event (mirrors friend.go evFriend*). Delivered on a node thread; the FriendsManager marshals it
// to the GUI thread as Qt signals.
struct FriendEvent {
    enum Kind { Request, Accept, Decline, Presence, Profile, Removed } Kind;
    Contact C;   // the affected contact; for Removed only PeerID is populated
};
using FriendCallback = std::function<void(const FriendEvent &)>;
void SetFriendCallback(FriendCallback Callback);

// ---------------------------------------------------------------------------
// Virtual LAN of friends (VidyaGodIPFS/friendlan.go).
// There is NO session/host. Friends share one implicit virtual LAN (10.66.0.0/16); each peer's vIP is a pure function
// of its peer ID, so membership + routing come from the accepted-friends set with zero coordination, and the LAN lives
// ONLY inside a game's sandbox netns (the host stack is never touched).
// ---------------------------------------------------------------------------

// The custom variables to launch a game onto the friend LAN (VIDYAGOD_SANDBOX, VIDYAGOD_SANDBOX_NET=isolated, SELF_VIP,
// SUBNET, PEER_VIPS, PEER_NAMES), which a Goldberg-style LAN-emulator content node writes into its config so the
// players find each other over the overlay. Empty if the LAN is unavailable.
std::map<std::string, std::string> LanLaunchVars();

// One friend's virtual-LAN link, as maintained by the node's always-on heartbeat loop (overlaylink.go).
struct LanPeer {
    std::string Peer;    // peer id
    std::string Nick;
    std::string Vip;     // 10.66.x.y (pure function of the peer id)
    bool        Online = false;
    std::string Link;    // "direct" | "relayed" | "connecting" | "down"
    long long   RttMs = -1;
};
// Live per-friend LAN link state (empty when the node/maintainer is down). Poll-friendly (~2s).
std::vector<LanPeer> LanPeers();

// One row of the Settings→Network firewall diagnostic. Status is "ok"/"warn"/"fail"; Detail is the sentence a
// user acts on ("outbound UDP looks BLOCKED ...").
struct NetCheck { std::string Name, Status, Detail; };
// Runs the node's full network sweep (BLOCKS up to ~15s — call from a worker). Empty + *NodeOffline=true when
// the node is not online (there is nothing meaningful to probe without a host).
std::vector<NetCheck> NetworkTest(bool *NodeOffline = nullptr);
// Replace the GLOBAL LAN roster's excluded set (the launch window's un-ticked members). Immediate, mid-game too.
void SetLanExcluded(const std::vector<std::string> &PeerIds);

// ----- overlay tunnel (VidyaGodIPFS/overlay.go) -----
// Bring up the friend-LAN: a TUN configured from lanConfig() (each friend's vIP = f(peerID)) forwarding IP packets
// between friends over libp2p, broadcast/multicast fanned out so LAN games discover each other. Returns the TUN name,
// "" on failure. Needs CAP_NET_ADMIN. OverlayStart is a host-netns debug/CLI path; the production launch uses
// OverlayServe. OverlayStop tears it down; OverlayActive reports whether it is forwarding.
std::string OverlayStart(std::string *Error = nullptr);
// Nested-sandbox variant (production): configure the friend-LAN routes and listen on SockPath for the TUN fd that the
// in-sandbox sandbox-init hands back (it creates the TUN inside the game's OWN netns). Non-blocking. True on listen ok.
// Bridge = in-node NAT gateway (internet + real-LAN unicast through the game's TUN); HostRelay = real-LAN
// broadcast reflection both ways. Together with the overlay itself: the TRI-PLANE LAN, all rootless, no helpers.
bool OverlayServe(const std::string &SockPath, bool Bridge, bool HostRelay, std::string *Error = nullptr);
void OverlayStop();
bool OverlayActive();

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

// ---------------------------------------------------------------------------
// FriendsManager — a GUI-thread QObject that relays IpfsWrapper friend events (which fire on a node thread) as Qt
// signals. A single shared instance (instance()) installs the friend callback once; the Friends UI + session layer
// connect to these signals. Mutations go through IpfsWrapper's free functions.
// ---------------------------------------------------------------------------
class FriendsManager : public QObject
{
    Q_OBJECT
public:
    static FriendsManager * instance();

signals:
    void friendRequest(QString peer, QString nick, QString pic);   // someone wants to be our friend
    void friendAccepted(QString peer, QString nick, QString pic);  // a request we sent was accepted (or crossed)
    void friendDeclined(QString peer);                             // declined / unfriended by the peer
    void friendPresence(QString peer, bool online);                // a friend's reachability changed
    void friendProfile(QString peer, QString nick, QString pic);   // a friend updated their nickname/picture
    void friendRemoved(QString peer);                              // local removal (echoed for UI symmetry)

private:
    explicit FriendsManager(QObject * parent = nullptr);
};

#endif // IPFSWRAPPER_H
