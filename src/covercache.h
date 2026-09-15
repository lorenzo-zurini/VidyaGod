#ifndef COVERCACHE_H
#define COVERCACHE_H

#include <QObject>
#include <QString>
#include <QSet>
#include <functional>

#include "nlohmann/json.hpp"

// ---------------------------------------------------------------------------
// CoverCache — resolves a package's cover art to a loadable image path, LOCAL-FIRST with lazy IPFS fallback.
//
// METADATA.COVER is dual-form, exactly like a content layer: a bare filename (authoring) or an object
// { "PATH": "<filename>", "SOURCE": { "TYPE": "ipfs", "CID": "<cid>" } } (published). resolve():
//   1. returns PackageDir/PATH if that file exists (offline, zero-fetch);
//   2. else, if a CID is present and already cached, returns its cache path;
//   3. else returns "" and kicks an async fetch (deduped) — emitting coverReady(cid) when it lands so the
//      caller can re-resolve and repaint.
//
// A single shared instance (instance()) is used by every cover surface (Library/Store cards, prelaunch, editor).
// Reuses IpfsWrapper's fetch/cache/seed machinery; adds no new IPFS plumbing.
// ---------------------------------------------------------------------------
class CoverCache : public QObject
{
    Q_OBJECT
public:
    static CoverCache * instance();

    // Returns a loadable image path for Cover (a JSON COVER value: string or {PATH,SOURCE}), or "" if not yet
    // available — in which case an async fetch is started (when a CID is known) and coverReady(cid) will fire.
    QString resolve(const nlohmann::ordered_json &Cover, const QString &PackageDir);

    // Extracts the (filename, cid) locators from a COVER value. Either may be empty.
    static void Locate(const nlohmann::ordered_json &Cover, QString &File, QString &Cid);

    // A cover fetch give-up is remembered ONLY when it happened while the node was ONLINE. An offline / pre-online
    // failure is not evidence the content is unavailable — there was simply no network to fetch over — and caching
    // it would blank the tile until the app restarts even after networking comes up (the pass-4 poisoning bug).
    static bool ShouldNegativeCache(bool FetchOk, bool NodeOnline);

    // Record a fetch outcome for DestPath (negative-caches per ShouldNegativeCache). Public so the decision +
    // conservation are unit-testable without a live node.
    void recordFetchResult(const QString & DestPath, bool FetchOk, bool NodeOnline);

    // Called on the node's offline→online transition: a cover that could not be fetched while the network was down
    // deserves a fresh attempt now that it is up. Clears the negative cache and nudges every surface to re-resolve.
    void onNetworkOnline();

    // Test observability: how many dest paths are currently negative-cached.
    int negativeCacheCount() const { return Failed.size(); }

    // Test seam: override the "is the node online?" probe (defaults to IpfsWrapper::DaemonRunning). Lets tests
    // drive request()/recordFetchResult through the offline→online sequence without a live node.
    void setOnlineProbe(std::function<bool()> Probe);

signals:
    void coverReady(QString cid);

private:
    explicit CoverCache(QObject * parent = nullptr);
    void request(const QString & Cid, const QString & DestPath);   // async fetch the cover to DestPath (deduped)
    std::function<bool()> OnlineProbe;                              // "is the node online?" (injectable for tests)
    QSet<QString> InFlight;                                          // dest paths currently being fetched (dedup)
    QSet<QString> Failed;                                            // dest paths whose bounded fetch gave up — do
                                                                     // NOT re-request (else one detached thread per
                                                                     // repaint every 30s for stale/unpublished art)
};

#endif // COVERCACHE_H
