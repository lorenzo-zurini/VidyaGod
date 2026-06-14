#ifndef COVERCACHE_H
#define COVERCACHE_H

#include <QObject>
#include <QString>
#include <QSet>

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

signals:
    void coverReady(QString cid);

private:
    explicit CoverCache(QObject * parent = nullptr);
    void request(const QString & Cid);          // start an async fetch (no-op if already in flight)
    QSet<QString> InFlight;                      // CIDs currently being fetched (dedup)
};

#endif // COVERCACHE_H
