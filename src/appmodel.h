#ifndef APPMODEL_H
#define APPMODEL_H

#include <QObject>
#include <QString>

#include "nlohmann/json.hpp"
#include "manifestmodel.h"   // NodeIndex

class QDir;

// ---------------------------------------------------------------------------
// AppModel — the single source of truth for the GUI: it OWNS the live GlobalConfigJSON pointer, the cross-bundle
// catalog NodeIndex, the persisted card-pixel-width, and the AppDataDir. Every view (Library/Catalog/Settings
// subpages/IPFS/Packages) holds a pointer to it, reads state through it, calls its mutators, and reacts to its
// signals. Views NEVER call each other or reach into MainWindow — all cross-view communication flows through the
// model's signals. The model outlives every view (created first, destroyed last), so its worker threads can safely
// capture `this`; a delivered refresh that targets a since-destroyed view is auto-disconnected by Qt.
// ---------------------------------------------------------------------------
class AppModel : public QObject
{
    Q_OBJECT
public:
    AppModel(nlohmann::ordered_json * config, QDir * appDataDir, QObject * parent = nullptr);

    // ── Shared state (views read directly; the model owns the lifetime) ──
    nlohmann::ordered_json * config()       const { return Config; }
    NodeIndex &              catalogIndex()        { return CatalogIndex; }
    const NodeIndex &        catalogIndex()  const { return CatalogIndex; }
    QDir *                   appDataDir()    const { return AppDataDir; }
    int                      cardPixelWidth() const { return CardPixelWidth; }

    // ── Mutators (perform the change, persist, then emit so every view reacts) ──
    bool save();                                       // write GlobalConfigJSON to disk
    void rebuildCatalog();                             // re-scan CatalogIndex from disk → emit catalogChanged()
    void setCardPixelWidth(int w);                     // persist + emit cardSizeChanged(w) (no-op if unchanged)
    void removePackage(const QString & uid);           // drop a LIBRARY entry (+ its managed files) → rebuild
    void notifyCoversReady();                          // a batch of covers finished loading → emit coversReady()

    // ── Async repo / runner ops (do the git/IPFS work off-thread, then rebuild + emit on the GUI thread) ──
    void syncRepositories();                           // git-pull every repo + reindex LIBRARY
    bool addRepository(const QString & name, const QString & url);   // append + clone/pull + reindex; false if empty/duplicate
    void removeRepository(int index);                  // drop the reference (disposable clone left on disk)
    void importRunner(const QString & runnerNodeId);   // fetch a runner's build + generate its DEFPREFIX

signals:
    void catalogChanged();          // CatalogIndex rebuilt — Library/Catalog/Packages/IPFS refresh
    void cardSizeChanged(int w);    // card pixel width changed — Library/Catalog relayout
    void coversReady();             // lazy cover load(s) landed — repaint visible cards
    void repositoriesChanged();     // the repo list was edited — the Repositories page rebuilds

private:
    nlohmann::ordered_json * Config;
    QDir *                   AppDataDir;
    NodeIndex                CatalogIndex;
    int                      CardPixelWidth = 185;
};

#endif // APPMODEL_H
