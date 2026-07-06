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

int  VgStart(const char *repoPath, char **errOut);   // open/create the private repo; idempotent
void VgStop(void);
int  VgStarted(void);   // 1 if the node repo is open
int  VgOnline(void);    // 1 if the node's network stack is up (joined the swarm/DHT)

int  VgAddNoCopy(const char *path, char **outCid, char **errOut);   // seed a file by reference (filestore --nocopy)
int  VgFetchToPath(const char *cid, const char *dest, char **errOut); // materialize + seed in place (no duplication)
int  VgFetchDirToPath(const char *cid, const char *dest, char **errOut); // recursively materialize a UnixFS DIRECTORY CID (online)

int  VgPinLs(char **outJson, char **errOut);   // JSON array of recursively-pinned CIDs
int  VgPinRm(const char *cid, char **errOut);
long long VgCidSize(const char *cid);          // CumulativeSize, -1 if unknown
int  VgCidMissing(const char *cid);            // 1 if a pinned CID's backing file is gone (orphaned ref), 0 ok, -1 n/a
int  VgHasLocal(const char *cid);              // 1 if the node holds the block locally (ref or block), 0 no, -1 n/a
int  VgPeerID(char **outId);                   // this node's libp2p peer ID
int  VgListenAddrs(char **outJson);            // JSON array of dialable /p2p/ multiaddrs (for peering/diagnostics)
int  VgConnect(const char *multiaddr, char **errOut); // dial + hold a connection to a peer at a full /p2p/ multiaddr
int  VgDropRef(const char *cid, char **errOut);// delete a CID's closure (filestore refs + blocks) + unpin, so it can be re-referenced
int  VgDropCached(const char *cid, char **errOut);// purge a CID's locally-CACHED (bitswap) blocks — a cancelled download's partial — + compact

int  VgPeerCount(void);                         // M3
int  VgRepoStat(char **outJson, char **errOut); // M3 — {"RepoSize":n,"StorageMax":n}
int  VgProviderCount(const char *cid, int timeoutMs); // M3
void VgBandwidthRates(double *inBps, double *outBps); // global down/up throughput (bytes/sec); 0 when offline
int  VgActiveUploads(int windowMs, char **outJson);   // JSON array of pinned CIDs served to a peer within windowMs

void VgRequestCancel(const char *cid);
void VgClearCancel(const char *cid);
void VgSetTransferCb(VgTransferCb cb);

// ---- friends / multiplayer social layer (see VidyaGodIPFS/social.go + friend.go) ----
// Inbound friend event: kind mirrors friend.go's evFriend* (0=request 1=accept 2=decline 3=presence 4=profile
// 5=removed); json is the affected contact ({peer,nick,pic,state,online,seen}). Invoked on a node goroutine — the
// C++ side marshals to the GUI thread.
typedef void (*VgFriendCb)(int kind, const char *json);

int  VgFriendCode(char **outId);                       // this node's shareable friend code (= its peer ID)
int  VgSetProfile(const char *nick, const char *picCid, char **errOut);
int  VgGetProfile(char **outJson);                     // {"nick":..,"pic":..}
int  VgFriendList(char **outJson);                     // JSON array of contacts
int  VgFriendAdd(const char *peerID, const char *note, char **errOut);   // send a friend request
int  VgFriendAccept(const char *peerID, char **errOut);
int  VgFriendDecline(const char *peerID, char **errOut);
int  VgFriendBlock(const char *peerID, char **errOut);
int  VgFriendRemove(const char *peerID);
int  VgFriendPing(const char *peerID);                 // 1 reachable, 0 not, -1 n/a
void VgSetFriendCb(VgFriendCb cb);

// ---- multiplayer session / lobby layer (see VidyaGodIPFS/session.go) ----
// Inbound session event: kind mirrors session.go's evSession* (0=invite 1=roster 2=ended); json is the payload
// (invite {id,game,host}; roster {id,host,game,subnet,members:[{peer,nick,vip,ready}]}; ended {id}).
typedef void (*VgSessionCb)(int kind, const char *json);

int  VgSessionCreate(const char *gameCid, char **outJson, char **errOut);  // host a session; returns its JSON
int  VgSessionInvite(const char *sid, const char *peerID, char **errOut);  // invite a friend
int  VgSessionJoin(const char *sid, const char *hostPeer, char **errOut);  // join a hosted session
int  VgSessionLeave(const char *sid, char **errOut);
int  VgSessionReady(const char *sid, int ready, char **errOut);
int  VgSessionList(char **outJson);                    // JSON array of all sessions
int  VgSessionRoster(const char *sid, char **outJson); // JSON of one session; -1 if unknown
void VgSetSessionCb(VgSessionCb cb);

// ---- overlay tunnel (see VidyaGodIPFS/overlay.go) ----
// Bring up a TUN configured from a session's roster and forward IP packets between members over libp2p; returns the
// interface name through outName. Needs CAP_NET_ADMIN in the current netns (holds inside a bubblewrap userns netns).
int  VgOverlayStart(const char *sid, char **outName, char **errOut);
void VgOverlayStop(void);
int  VgOverlayActive(void);   // 1 if forwarding on an attached TUN, else 0

void VgFree(char *p);   // free a char* returned through an out-param

} // extern "C"

#endif // VGIPFSAPI_H
