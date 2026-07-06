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
// Multiplayer session / lobby layer (VidyaGodIPFS/session.go).
// A Session is a group of friends who agree to play one game together over a private overlay network. The host is
// authoritative: it assigns each member an overlay virtual IP (vIP) and broadcasts the roster. This is the SIGNALLING
// only — the roster + vIP map is what the packet tunnel + LAN emulator (Goldberg et al.) are later configured from.
// ---------------------------------------------------------------------------

// One participant: peer ID, nickname, assigned overlay vIP (e.g. 10.66.42.2), and ready state.
struct Member {
    std::string PeerID;
    std::string Nick;
    std::string VIP;
    bool        Ready = false;
};
// A session/lobby snapshot.
struct Session {
    std::string         Id;
    std::string         Host;     // host peer ID
    std::string         Game;     // game package CID both members must hold
    std::string         Subnet;   // overlay /24, e.g. "10.66.42.0/24"
    bool                AmHost = false;
    std::vector<Member> Members;
};

// Host a new session for a game CID; returns the created session (Id populated), or a session with empty Id on error.
Session SessionCreate(const std::string &GameCID, std::string *Error = nullptr);
// Invite a friend (peer ID) to a session we host.
bool SessionInvite(const std::string &Sid, const std::string &PeerID, std::string *Error = nullptr);
// Join a hosted session by its id + the host's peer ID.
bool SessionJoin(const std::string &Sid, const std::string &HostPeer, std::string *Error = nullptr);
// Leave a session (host: ends it for everyone).
bool SessionLeave(const std::string &Sid, std::string *Error = nullptr);
// Set our ready state in a session.
bool SessionReady(const std::string &Sid, bool Ready, std::string *Error = nullptr);
// All sessions we're currently in.
std::vector<Session> SessionList();
// One session's current roster (Id empty if unknown).
Session SessionRoster(const std::string &Sid);
// The custom variables to launch a game into this session (VIDYAGOD_SANDBOX + overlay vIPs), which a Goldberg-style
// LAN-emulator content node writes into its config so the players find each other over the overlay. Empty if the
// session / our vIP isn't known yet.
std::map<std::string, std::string> SessionLaunchVars(const std::string &Sid);

// Inbound session event (mirrors session.go evSession*). Delivered on a node thread; SessionManager marshals it to
// the GUI thread as Qt signals.
struct SessionEvent {
    enum Kind { Invite, Roster, Ended } Kind;
    std::string Id;         // session id (all kinds)
    std::string Game;       // Invite/Roster
    std::string Host;       // Invite (inviter) / Roster (host)
    Session     Snapshot;   // Roster: the full session; else only Id/Game/Host populated
};
using SessionCallback = std::function<void(const SessionEvent &)>;
void SetSessionCallback(SessionCallback Callback);

// ----- overlay tunnel (VidyaGodIPFS/overlay.go) -----
// Bring up the private virtual-LAN for a session: creates a TUN configured from the roster's vIPs and forwards IP
// packets between members over libp2p, so the game (and its LAN emulator) see each other as if on one LAN. Returns
// the TUN interface name, "" on failure (with *Error). Needs CAP_NET_ADMIN — in production it runs inside the game's
// bubblewrap netns. OverlayStop tears it down; OverlayActive reports whether it is forwarding.
std::string OverlayStart(const std::string &Sid, std::string *Error = nullptr);
// Nested-sandbox variant: configure the session's routes and listen on SockPath for the TUN fd that the in-sandbox
// sandbox-init will hand back (it creates the TUN inside the game's own netns). Non-blocking. True on listen success.
bool OverlayServe(const std::string &Sid, const std::string &SockPath, std::string *Error = nullptr);
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

// ---------------------------------------------------------------------------
// SessionManager — GUI-thread QObject relaying IpfsWrapper session events (which fire on a node thread) as Qt
// signals. A single shared instance installs the session callback once; the play-together UI + the (future) tunnel
// layer connect to these signals.
// ---------------------------------------------------------------------------
class SessionManager : public QObject
{
    Q_OBJECT
public:
    static SessionManager * instance();

signals:
    void sessionInvite(QString id, QString game, QString host);   // a friend invited us
    void sessionRoster(QString id);                               // the roster/vIPs/ready changed (query SessionRoster)
    void sessionEnded(QString id);                                // the session ended / we left

private:
    explicit SessionManager(QObject * parent = nullptr);
};

#endif // IPFSWRAPPER_H
