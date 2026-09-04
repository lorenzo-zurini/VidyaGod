#ifndef VGIPFSAPI_H
#define VGIPFSAPI_H

// C ABI for the embedded Boxo IPFS node (VidyaGodIPFS → libvgipfs.so), linked in-process and called via
// this hand-mirrored declaration of its cgo //export surface (see VidyaGodIPFS/api.go). It replaces the
// external Kubo `ipfs` CLI. Convention: fallible calls return 0 on success / -1 on failure; results and error
// reasons come back through char** out-params allocated by the library and freed with VgFree.

extern "C" {

// Transfer lifecycle callback: kind 0=Started 1=Progress 2=Finished. err is non-NULL only on a failed Finished.
// Mirrors IpfsWrapper::TransferEvent. Invoked on the calling (worker) thread; the C++ side marshals to the GUI.
typedef void (*VgTransferCb)(const char *cid, int kind, double percent, int ok, const char *err);

void VgSetLogVerbose(int on);   // flip the node's verbose diagnostic log (vglog.go) on/off at runtime
int  VgHealth(char **outJson);  // live service health: [{name,status,detail}], status ok|warn|down|off (cheap; pollable)
int  VgStart(const char *repoPath, char **errOut);   // open/create the private repo; idempotent
void VgStop(void);
int  VgStarted(void);   // 1 if the node repo is open
int  VgOnline(void);    // 1 if the node's network stack is up (joined the swarm/DHT)

int  VgAddNoCopy(const char *path, char **outCid, char **errOut);   // seed a file by reference (filestore --nocopy)
int  VgAddNoCopyMeta(const char *path, char **outCid, char **errOut); // seed a TEXT-ONLY Meta-CID in place (*.json only, no staging)
int  VgFetchToPath(const char *cid, const char *dest, char **errOut); // materialize + seed in place (no duplication)
int  VgFetchDirToPath(const char *cid, const char *dest, char **errOut); // recursively materialize a UnixFS DIRECTORY CID (online)

int  VgPinLs(char **outJson, char **errOut);   // JSON array of recursively-pinned CIDs
int  VgPinRm(const char *cid, char **errOut);
long long VgCidSize(const char *cid);          // CumulativeSize, -1 if unknown (may fetch: bitswap 15s + gateway 20s bounds)
long long VgCidSizeLocal(const char *cid);     // CumulativeSize from the LOCAL store only — fast -1 when unreadable
int  VgCidMissing(const char *cid);            // 1 if a pinned CID's backing file is gone (orphaned ref), 0 ok, -1 n/a
int  VgComputeCid(const char *path, char **outCid, char **errOut); // a file's CID with NO side effects (nothing seeded/pinned)
long long VgCidFileSizeLocal(const char *cid);   // UnixFS FILE size (payload bytes) from the local store, -1 unknown
char *VgCidServeStatus(const char *cid); // "" if deliverable (cheap: stat only), else the reason (caller frees)
int  VgServeFailures(char **outJson); // JSON [{cid,err,when}] of blocks a PEER asked for that we could not deliver (drains)
char *VgVerifyCid(const char *cid);            // "" if the whole DAG READS back cleanly, else first read error (caller frees).
                                               // Catches what VgCidMissing cannot: backing file present but bytes changed.
int  VgHasLocal(const char *cid);              // 1 if the node holds the block locally (ref or block), 0 no, -1 n/a
int  VgPeerID(char **outId);                   // this node's libp2p peer ID
int  VgListenAddrs(char **outJson);            // JSON array of dialable /p2p/ multiaddrs (for peering/diagnostics)
int  VgConnect(const char *multiaddr, char **errOut); // dial + hold a connection to a peer at a full /p2p/ multiaddr
int  VgDropRef(const char *cid, char **errOut);// delete a CID's closure (filestore refs + blocks) + unpin, so it can be re-referenced
int  VgDropCached(const char *cid, char **errOut);// purge a CID's locally-CACHED (bitswap) blocks — a cancelled download's partial — + compact

int  VgPeerCount(void);                         // M3
int  VgRepoStat(char **outJson, char **errOut); // M3 — {"RepoSize":n,"StorageMax":n}
int  VgProviderCount(const char *cid, int timeoutMs); // M3
int  VgSetSeedLevels(const char *collectionsJson, const char *packagesJson); // record meta-CID levels + start the 3-pass seed announce
int  VgSeedAnnounced(const char *cid);                // 1 if the pinned CID's DHT announce has completed (seeding), 0 if still queued
void VgBandwidthRates(double *inBps, double *outBps); // global down/up throughput (bytes/sec); 0 when offline
int  VgActiveUploads(int windowMs, char **outJson);   // JSON array of pinned CIDs served to a peer within windowMs
int  VgOrphanedRefPaths(char **outJson);              // JSON array of distinct filestore backing paths that are gone (orphans to heal)
int  VgUnservableRefs(char **outJson);                // JSON [{cid,path,status,err}] for every filestore entry whose bytes FAIL to verify
                                                      // (status 12 = contents changed, 11 = file gone) — refs we advertise but cannot serve

void VgRequestCancel(const char *cid);
void VgClearCancel(const char *cid);
void VgSetTransferCb(VgTransferCb cb);

// ---- friends / multiplayer social layer (see VidyaGodIPFS/social.go + friend.go) ----
// Inbound friend event: kind mirrors friend.go's evFriend* (0=request 1=accept 2=decline 3=presence 4=profile
// 5=removed, 6=suggest); json is the affected contact ({peer,nick,pic,state,online,seen,play,...}), or for kind 6
// a suggestion ({peer,nick,via,game}). Invoked on a node goroutine — the C++ side marshals to the GUI thread.
typedef void (*VgFriendCb)(int kind, const char *json);

int  VgFriendCode(char **outId);                       // this node's shareable friend code (= its peer ID)
int  VgSetProfile(const char *nick, const char *picCid, char **errOut);
int  VgGetProfile(char **outJson);                     // {"nick":..,"pic":..}
int  VgFriendList(char **outJson);                     // JSON array of contacts (incl. play/plabel/pident/psince/popen)

int  VgFriendAdd(const char *peerID, const char *note, char **errOut);   // send a friend request
int  VgFriendAccept(const char *peerID, char **errOut);
int  VgFriendDecline(const char *peerID, char **errOut);
int  VgFriendBlock(const char *peerID, char **errOut);
int  VgFriendRemove(const char *peerID);
int  VgFriendPing(const char *peerID);                 // 1 reachable, 0 not, -1 n/a
void VgSetFriendCb(VgFriendCb cb);

// ---- virtual LAN of friends (see VidyaGodIPFS/friendlan.go) ----
// There is NO session/host. Friends share one implicit virtual LAN (10.66.0.0/16); each peer's vIP is a pure function
// of its peer ID, so membership + routing come from the accepted-friends set with zero coordination.
// The launch vars {VIDYAGOD_SANDBOX, VIDYAGOD_SANDBOX_NET=isolated, SELF_VIP, SELF_NAME, SUBNET, PEER_VIPS,
// PEER_NAMES} for a sandboxed game launch that joins the LAN. PEER_VIPS/PEER_NAMES are positionally parallel and
// ordered by peer id (deterministic across calls). -1 if the LAN is unavailable.
int  VgLanLaunchVars(char **outJson);
// Per-friend LAN link state for the UI: [{peer,nick,vip,online,link:"direct"|"relayed"|"connecting"|"down",rttMs}].
int  VgLanPeers(char **outJson);
// Full network/firewall diagnostic sweep (~15s, concurrent, read-only): [{"name","status","detail"}...].
// -1 when the node is offline.
int  VgNetworkTest(char **outJson);
// Replace the GLOBAL LAN roster's excluded set (comma-separated peer ids). Applies immediately, mid-game included.
void VgLanSetExcluded(const char *csv);

// ---- overlay tunnel (see VidyaGodIPFS/overlay.go) ----
// Bring up a TUN configured from the friend LAN (lanConfig) and forward IP packets between friends over libp2p —
// broadcast/multicast fanned out so LAN games discover each other. Needs CAP_NET_ADMIN in the current netns (holds
// inside a bubblewrap userns netns). The production path is VgOverlayServe: the TUN lives in the game's OWN sandbox
// netns (the host stack is never touched); VgOverlayStart is a host-netns debug/CLI path.
int  VgOverlayStart(char **outName, char **errOut);
// bridge≠0 → in-node NAT gateway (internet + real-LAN unicast); hostRelay≠0 → real-LAN broadcast reflector.
int  VgOverlayServe(const char *sockPath, int bridge, int hostRelay, char **errOut); // nested: listen for the sandbox's TUN fd
void VgOverlayStop(void);
int  VgOverlayActive(void);   // 1 if forwarding on an attached TUN, else 0

void VgFree(char *p);   // free a char* returned through an out-param

} // extern "C"

#endif // VGIPFSAPI_H
