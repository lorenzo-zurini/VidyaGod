#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <vector>
#include <QPointer>
#include <QRect>

#include "nlohmann/json.hpp"

class QDir;
class QTabWidget;
class QTimer;
class AppModel;
class DownloadManager;
class IpfsModel;
class LibraryGameCard;
class TrayController;
class LibraryTab;
class CatalogTab;
class SettingsTab;
class IpfsTab;
class NetworkTab;

// ─────────────────────────────────────────────────────────────────────────────
// MainWindow — the composition root: it creates the AppModel (the single state/signal hub), the DownloadManager,
// and the five tab widgets, then wires the cross-controller signals (download flow ↔ Catalog/IPFS, IpfsManager
// transfer progress, lazy cover loading, tab-visibility). It owns no view logic — every tab reads state through the
// AppModel and reacts to its signals. The only app-wide hook left here is RefreshPackage (driven by the editor).
// ─────────────────────────────────────────────────────────────────────────────
class DeckShell;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    // FromLastIndex: the app's own start — its model starts from the last index kept (AppModel).
    // Controller: come up in controller mode (the desktop tabs are built the first time desktop mode is shown).
    MainWindow(nlohmann::ordered_json * GlobalConfigJSON, QDir * AppDataDir, QWidget * parent = nullptr, bool FromLastIndex = false,
               bool Controller = false);
    ~MainWindow() override;   // lets go of the editor hook it installed (it holds this window and its model)

    //Reacts to a package being saved in the editor: rebuilds the catalog (every tab refreshes via the model) and
    //reloads any open prelaunch dialog for that package. Static + self-locating so any editor (wherever opened) can
    //drive it.
    static void RefreshPackage(const QString & PackagePath);

    //Show the window, or come up hidden in the tray if forceTray (--tray) / Start-in-tray / the remembered last
    //state says so. Called by main() instead of show(), so a tray start never flashes the window.
    void startup(bool forceTray = false);

    //The app's model: what the command socket and the CLI drive (Command::Context).
    AppModel * model() const { return Model; }
    IpfsModel * ipfsModel() const { return IpfsModelPtr; }
    DownloadManager * downloads() const { return DownloadMgr; }
    //A Catalog card by its key or (a unique part of) its title — null when none or several match.
    LibraryGameCard * catalogCard(const QString & KeyOrTitle) const;
    LibraryGameCard * libraryCard(const QString & KeyOrTitle) const;   // the same on the Library tab
    QString cardMiss(bool Catalog, const QString & KeyOrTitle) const;   // why no single card is that (LibraryView::cardMiss)
    TrayController * tray() const { return Tray; }
    void restoreFromTray();                        // un-hide + raise + focus (a second start of the app does it)
    //Controller mode: the window full screen showing the DeckShell, controllers read; off, the desktop tabs as they
    //were. Saves nothing (Settings.Controller.Start decides how the app starts).
    void setControllerMode(bool On);
    bool controllerMode() const { return Controller; }
    DeckShell * deckShell() const { return Deck; }
    //Read Settings.Controller again: whether a controller drives the desktop window, and rumbles.
    void applyControllerSettings();
    void hideToTray();                             // hide window, remember the hidden state, hint once

protected:
    void closeEvent(QCloseEvent * e) override;     // close → hide to tray (if enabled), else quit
    void changeEvent(QEvent * e) override;         // minimize → hide to tray (if enabled)

private:
    void BuildStaticUI();       // what both modes share: the cover refresh, the runner import
    void BuildDesktop();        // the desktop tabs and their wiring (the first time desktop mode is shown)

    AppModel        * Model       = nullptr;   // owns config + catalog + persisted UI state; the signal hub
    IpfsModel       * IpfsModelPtr = nullptr;   // download/IPFS runtime state (single transfer-event consumer)
    DownloadManager * DownloadMgr = nullptr;   // owns the Catalog download lifecycle
    class NodeService *Node = nullptr;   // the IPFS node's start, retry, stop and the work its readiness unblocks
    TrayController  * Tray        = nullptr;    // tray icon + close/minimize/start-in-tray behavior
    bool              ReallyQuitting = false;   // set by the tray "Quit" path so closeEvent quits instead of hiding
    bool QuittingAfterGames = false;   // quitting, the running game being ended first (closeEvent)

    QTabWidget * MainWindowTabWidget = nullptr;
    LibraryTab  * LibraryTabPtr  = nullptr;
    CatalogTab  * CatalogTabPtr   = nullptr;
    SettingsTab * SettingsTabPtr  = nullptr;
    IpfsTab     * IpfsTabPtr       = nullptr;
    NetworkTab  * NetworkTabPtr    = nullptr;
    QTimer      * CoverRefreshTimer = nullptr;  // debounces lazy cover-ready bursts into one model notification
    class QStackedWidget * Modes = nullptr;     // the desktop page and the controller shell
    QWidget     * DesktopPage = nullptr;
    DeckShell   * Deck = nullptr;               // built the first time controller mode is
    bool          Controller = false;
    bool          Constructed = false;          // the constructor's EventLog::AttachTree has run (later trees attach themselves)
    QRect         DesktopGeometry;              // where the window was before it went full screen
};

#endif // MAINWINDOW_H
