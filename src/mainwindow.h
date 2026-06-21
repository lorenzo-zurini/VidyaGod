#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

#include "nlohmann/json.hpp"

class QDir;
class QTabWidget;
class QTimer;
class AppModel;
class DownloadManager;
class LibraryTab;
class CatalogTab;
class SettingsTab;
class IpfsTab;

// ─────────────────────────────────────────────────────────────────────────────
// MainWindow — the composition root: it creates the AppModel (the single state/signal hub), the DownloadManager,
// and the five tab widgets, then wires the cross-controller signals (download flow ↔ Catalog/IPFS, IpfsManager
// transfer progress, lazy cover loading, tab-visibility). It owns no view logic — every tab reads state through the
// AppModel and reacts to its signals. The only app-wide hook left here is RefreshPackage (driven by the editor).
// ─────────────────────────────────────────────────────────────────────────────
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    MainWindow(nlohmann::ordered_json * GlobalConfigJSON, QDir * AppDataDir, QWidget * parent = nullptr);

    //Reacts to a package being saved in the editor: rebuilds the catalog (every tab refreshes via the model) and
    //reloads any open prelaunch dialog for that package. Static + self-locating so any editor (wherever opened) can
    //drive it.
    static void RefreshPackage(const QString & PackagePath);

protected:
    void closeEvent(QCloseEvent * e) override;

private:
    void BuildStaticUI();

    AppModel        * Model       = nullptr;   // owns config + catalog + persisted UI state; the signal hub
    DownloadManager * DownloadMgr = nullptr;   // owns the Catalog download lifecycle

    QTabWidget * MainWindowTabWidget = nullptr;
    LibraryTab  * LibraryTabPtr  = nullptr;
    CatalogTab  * CatalogTabPtr   = nullptr;
    SettingsTab * SettingsTabPtr  = nullptr;
    IpfsTab     * IpfsTabPtr       = nullptr;
    QTimer      * CoverRefreshTimer = nullptr;  // debounces lazy cover-ready bursts into one model notification
};

#endif // MAINWINDOW_H
