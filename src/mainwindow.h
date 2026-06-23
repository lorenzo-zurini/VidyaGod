#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

#include "nlohmann/json.hpp"

class QDir;
class QTabWidget;
class QTimer;
class AppModel;
class DownloadManager;
class TrayController;
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

    //Show the window, or come up hidden in the tray if Start-in-tray / the remembered last state says so. Called by
    //main() instead of show(), so a tray start never flashes the window.
    void startup();

protected:
    void closeEvent(QCloseEvent * e) override;     // close → hide to tray (if enabled), else quit
    void changeEvent(QEvent * e) override;         // minimize → hide to tray (if enabled)

private:
    void BuildStaticUI();
    void restoreFromTray();                        // un-hide + raise + focus
    void hideToTray();                             // hide window, remember the hidden state, hint once

    AppModel        * Model       = nullptr;   // owns config + catalog + persisted UI state; the signal hub
    DownloadManager * DownloadMgr = nullptr;   // owns the Catalog download lifecycle
    TrayController  * Tray        = nullptr;    // tray icon + close/minimize/start-in-tray behavior
    bool              ReallyQuitting = false;   // set by the tray "Quit" path so closeEvent quits instead of hiding

    QTabWidget * MainWindowTabWidget = nullptr;
    LibraryTab  * LibraryTabPtr  = nullptr;
    CatalogTab  * CatalogTabPtr   = nullptr;
    SettingsTab * SettingsTabPtr  = nullptr;
    IpfsTab     * IpfsTabPtr       = nullptr;
    QTimer      * CoverRefreshTimer = nullptr;  // debounces lazy cover-ready bursts into one model notification
};

#endif // MAINWINDOW_H
