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

void VgRequestCancel(const char *cid);
void VgClearCancel(const char *cid);
void VgSetTransferCb(VgTransferCb cb);

void VgFree(char *p);   // free a char* returned through an out-param

} // extern "C"

#endif // VGIPFSAPI_H
