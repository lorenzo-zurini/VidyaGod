#include "mainwindow.h"
#include "asyncwork.h"
#include "appmodel.h"
#include "downloadmanager.h"
#include "traycontroller.h"
#include "librarytab.h"
#include "catalogtab.h"
#include "settingstab.h"
#include "ipfstab.h"
#include "friendstab.h"
#include "ipfsmodel.h"
#include "covercache.h"
#include "ipfswrapper.h"       // IpfsManager (transfer progress signals) + StartNode/StopNode
#include "apppaths.h"            // AppPaths::DataRoot (the IPFS repo lives at <DataRoot>/ipfs)
#include "prelaunchwindow.h"

#include <thread>

#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QTabWidget>
#include <QTimer>
#include <QPointer>
#include <QVBoxLayout>
#include <QWidget>
#include <QCloseEvent>
#include <QEvent>
#include <QIcon>
#include <QDir>

#include <nlohmann/json.hpp>

// ═════════════════════════════════════════════════════════════════════════════
// MainWindow — composition root (creates the model + tabs, wires the cross-controller signals).
// ═════════════════════════════════════════════════════════════════════════════

MainWindow::MainWindow(nlohmann::ordered_json * gc, QDir * appData, QWidget * parent)
    : QMainWindow(parent)
{
    setWindowTitle("Vidya God");
    setWindowIcon(QIcon(":/vidyagod.png"));
    setMinimumSize(640, 480);

    Model = new AppModel(gc, appData, this);   // owns config + catalog + persisted UI state (card size, etc.)

    {
        QRect screen = QGuiApplication::primaryScreen()->geometry();
        int w = screen.width()  / 2;
        int h = screen.height() / 2;
        int x = (screen.width()  - w) / 2;
        int y = (screen.height() - h) / 2;
        auto & S = (*gc)["Settings"];
        if (S.contains("WindowW")) w = int(S["WindowW"]);
        if (S.contains("WindowH")) h = int(S["WindowH"]);
        if (S.contains("WindowX")) x = int(S["WindowX"]);
        if (S.contains("WindowY")) y = int(S["WindowY"]);
        setGeometry(x, y, w, h);
    }

    QWidget * cw = new QWidget(this);
    QVBoxLayout * cl = new QVBoxLayout(cw);
    cl->setContentsMargins(0,0,0,0); cl->setSpacing(0);
    cw->setLayout(cl);
    setCentralWidget(cw);

    IpfsModelPtr = new IpfsModel(*Model, this);              // owns transfer state; single IpfsManager consumer
    connect(Model, &AppModel::ipfsHealthChanged, IpfsModelPtr, &IpfsModel::refreshNow);   // orphan heal ran → re-poll health/red→green
    DownloadMgr  = new DownloadManager(*Model, *IpfsModelPtr, this, this);   // dialog parent = this; lifetime = this

    // Daemon/foreground-hybrid tray. When a tray is available the app no longer quits on the last window closing —
    // closing/minimizing hide to the tray (keeping the node seeding); the tray "Quit" is the real exit path.
    Tray = new TrayController(*Model, this, this);
    if (Tray->available())
        QApplication::setQuitOnLastWindowClosed(false);
    connect(Tray, &TrayController::restoreRequested, this, [this]{ restoreFromTray(); });
    connect(Tray, &TrayController::quitRequested,    this, [this]{ ReallyQuitting = true; close(); });

    BuildStaticUI();

    // Test/dev hook: open directly on a named tab (e.g. VIDYAGOD_START_TAB=IPFS) so a headless screenshot harness
    // can land on it without coordinate-clicking. Selecting it fires currentChanged (e.g. starts the IPFS refresh).
    const QString StartTab = qEnvironmentVariable("VIDYAGOD_START_TAB");
    if (!StartTab.isEmpty() && MainWindowTabWidget)
        for (int i = 0; i < MainWindowTabWidget->count(); ++i)
            if (MainWindowTabWidget->tabText(i).compare(StartTab, Qt::CaseInsensitive) == 0)
            { MainWindowTabWidget->setCurrentIndex(i); break; }
}

void MainWindow::BuildStaticUI()
{
    MainWindowTabWidget = new QTabWidget(centralWidget());
    centralWidget()->layout()->addWidget(MainWindowTabWidget);

    LibraryTabPtr  = new LibraryTab(*Model, MainWindowTabWidget);
    CatalogTabPtr  = new CatalogTab(*Model, MainWindowTabWidget);
    SettingsTabPtr = new SettingsTab(*Model, MainWindowTabWidget);
    IpfsTabPtr     = new IpfsTab(*IpfsModelPtr, MainWindowTabWidget);
    FriendsTabPtr  = new FriendsTab(*Model, MainWindowTabWidget);

    MainWindowTabWidget->addTab(LibraryTabPtr,  "Library");
    MainWindowTabWidget->addTab(CatalogTabPtr,  "Catalog");
    MainWindowTabWidget->addTab(SettingsTabPtr, "Settings");
    MainWindowTabWidget->addTab(IpfsTabPtr,     "IPFS");
    MainWindowTabWidget->addTab(FriendsTabPtr,  "Friends");

    // ── Cross-controller wiring (the only place a signal crosses between two components) ──

    // Only poll IPFS (off-thread gathers) while its tab is the visible one.
    connect(MainWindowTabWidget, &QTabWidget::currentChanged, this, [this](int){
        IpfsTabPtr->setActive(MainWindowTabWidget->currentWidget() == IpfsTabPtr);
        FriendsTabPtr->setActive(MainWindowTabWidget->currentWidget() == FriendsTabPtr);
    });

    // Catalog card clicks → the download controller.
    connect(CatalogTabPtr, &CatalogTab::downloadRequested, DownloadMgr, &DownloadManager::startDownload);
    connect(CatalogTabPtr, &CatalogTab::cancelRequested,   DownloadMgr, &DownloadManager::requestCancel);

    // Settings → Runners "Import" → the SAME download pump, fed a runner-only group (no launchables, one runner build).
    // A runner install is not a special flow — just this batch with runner-build targets, so it inherits persistence/
    // resume, IPFS-tab rows and cancel identically to a game download.
    connect(Model, &AppModel::runnerImportRequested, DownloadMgr, [this](const QString & Rid){
        DownloadMgr->beginDownload("runner:" + Rid, {}, { Rid.toStdString() }, {});
    });

    // Download controller → Catalog cards (downloading overlay + progress).
    connect(DownloadMgr, &DownloadManager::downloadStarted,  CatalogTabPtr, &CatalogTab::markDownloading);
    connect(DownloadMgr, &DownloadManager::downloadProgress, CatalogTabPtr, &CatalogTab::setDownloadProgress);
    connect(DownloadMgr, &DownloadManager::downloadFinished, CatalogTabPtr, &CatalogTab::clearDownloading);

    // A completed/changed download → refresh the IPFS model (pick up the new pin). Queued-row state + per-CID progress
    // flow directly into IpfsModel (from the download queue's callback + the node's transfer events, both via
    // IpfsManager); DownloadManager reads that progress back to paint the Catalog card (wired in its ctor).
    connect(DownloadMgr, &DownloadManager::transfersChanged, IpfsModelPtr, &IpfsModel::refreshNow);

    // Lazy cover loading: coalesce a burst of cover-ready arrivals into one model notification; the Library/Catalog
    // tabs repaint their visible cards on coversReady.
    CoverRefreshTimer = new QTimer(this);
    CoverRefreshTimer->setSingleShot(true);
    CoverRefreshTimer->setInterval(200);
    connect(CoverRefreshTimer, &QTimer::timeout, this, [this]{ Model->notifyCoversReady(); });
    connect(CoverCache::instance(), &CoverCache::coverReady, this, [this](QString){
        if (CoverRefreshTimer && !CoverRefreshTimer->isActive()) CoverRefreshTimer->start();
    });

    // Resume any downloads a previous run left interrupted (crash/close) — but only once the embedded node's network
    // is up, so the fetch can actually proceed. Polls briefly then stops; a no-op when there's nothing to resume.
    // Started by onNodeReady() (only after the node is brought up — networking is opt-in), not at construction.
    ResumeTimer = new QTimer(this);
    ResumeTimer->setInterval(1500);
    connect(ResumeTimer, &QTimer::timeout, this, [this]{
        if (IpfsWrapper::DaemonRunning()) { ResumeTimer->stop(); DownloadMgr->resumeAll(); }
    });

    // Networking is opt-in (Settings → IPFS). Grey the network-dependent tabs until it's on, and react to the toggle
    // (start/stop the node, re-enable/disable the tabs).
    setNetworkTabsEnabled(Model->networkingEnabled());
    connect(Model, &AppModel::networkingChanged, this, [this](bool on){ applyNetworkingState(on); });
}

void MainWindow::RefreshPackage(const QString & PackagePath)
{
    const QString Want = QDir::cleanPath(PackagePath);

    // Capture the affected windows in ONE clean pass over topLevelWidgets(), as QPointers, BEFORE touching anything.
    // Then defer the mutating work (catalog rebuild + dialog reloads) to the next event-loop turn. This is called
    // synchronously from the editor's save signal (appendLayer → SaveNodes → savedToDisk → packageSaved), and both
    // rebuildCatalog() and ReloadAndRebuild() can delete top-level widgets — doing that while iterating a snapshot of
    // topLevelWidgets() left a freed widget in the snapshot, which the next qobject_cast dereferenced (SIGSEGV). It
    // could also delete the very editor whose signal we're unwinding. Snapshotting as QPointers + deferring fixes both:
    // the casts happen once on a stable set, and anything deleted meanwhile null-checks out.
    QPointer<MainWindow> Mw;
    QList<QPointer<PreLaunchWindow>> Dialogs;
    for (QWidget * W : QApplication::topLevelWidgets())
    {
        if (auto * MW = qobject_cast<MainWindow *>(W)) { if (!Mw) Mw = MW; }
        else if (auto * PL = qobject_cast<PreLaunchWindow *>(W))
            if (QDir::cleanPath(QString::fromStdString(PL->packagePath())) == Want) Dialogs.append(PL);
    }

    QTimer::singleShot(0, [Mw, Dialogs]() {
        // Rebuild the catalog through the live MainWindow's model (every tab refreshes via catalogChanged; the
        // CatalogIndex address is stable, so dialogs borrowing it stay valid).
        if (Mw && Mw->Model) Mw->Model->rebuildCatalog();
        // Reload any still-open prelaunch dialog(s) for this bundle.
        for (const QPointer<PreLaunchWindow> & PL : Dialogs)
            if (PL) PL->ReloadAndRebuild();
    });
}

void MainWindow::closeEvent(QCloseEvent * e)
{
    // Persist geometry on every close (hide or quit) so the window reopens where it was.
    auto * cfg = Model->config();
    if (!isMinimized() && isVisible())   // a minimized/hidden window reports bogus geometry — keep the last good one
    {
        (*cfg)["Settings"]["WindowW"] = width();
        (*cfg)["Settings"]["WindowH"] = height();
        (*cfg)["Settings"]["WindowX"] = x();
        (*cfg)["Settings"]["WindowY"] = y();
    }
    Model->save();   // card size + sort mode are persisted by their owners on change

    // Close-to-tray: hide instead of quitting (the node keeps seeding). The tray "Quit" sets ReallyQuitting first.
    if (!ReallyQuitting && Tray && Tray->available() && Tray->closeToTray())
    {
        e->ignore();
        hideToTray();
        return;
    }
    if (Tray) Tray->rememberHidden(false);   // exiting from a visible window → relaunch visible
    QMainWindow::closeEvent(e);
    QApplication::quit();                     // quitOnLastWindowClosed is off when a tray is active
}

void MainWindow::changeEvent(QEvent * e)
{
    // Minimize-to-tray: when the window is minimized, hide it to the tray instead of leaving a taskbar entry.
    if (e->type() == QEvent::WindowStateChange && isMinimized()
        && Tray && Tray->available() && Tray->minimizeToTray())
    {
        // Defer the hide so the minimize animation/state settles first; clear the minimized bit so the next show
        // comes up normal rather than minimized.
        QTimer::singleShot(0, this, [this]{ setWindowState(windowState() & ~Qt::WindowMinimized); hideToTray(); });
        e->accept();
        return;
    }
    QMainWindow::changeEvent(e);
}

void MainWindow::startup(bool forceTray)
{
    if (Tray && Tray->available() && (forceTray || Tray->shouldStartHidden()))
    {
        Tray->notifyHidden();   // make the tray icon's presence obvious on a hidden start
    }
    else
    {
        show();
    }

    // Defer the IPFS node start until AFTER the window is up (opening the repo + joining the swarm delays startup),
    // and only when the user has opted into networking. singleShot(0) runs it once the event loop is processing.
    if (Model->networkingEnabled())
        QTimer::singleShot(0, this, [this]{ startNodeAsync(); });
}

void MainWindow::setNetworkTabsEnabled(bool enabled)
{
    if (!MainWindowTabWidget) return;
    for (int i = 0; i < MainWindowTabWidget->count(); ++i)
    {
        QWidget * W = MainWindowTabWidget->widget(i);
        if (W == CatalogTabPtr || W == IpfsTabPtr) MainWindowTabWidget->setTabEnabled(i, enabled);
    }
}

void MainWindow::applyNetworkingState(bool enabled)
{
    setNetworkTabsEnabled(enabled);
    if (enabled)
        startNodeAsync();
    else
    {
        if (ResumeTimer) ResumeTimer->stop();
        IpfsWrapper::StopNode();   // bring the node fully down — no network activity until re-enabled
        if (IpfsModelPtr) IpfsModelPtr->refreshNow();   // reflect the "node off" state
    }
}

void MainWindow::startNodeAsync()
{
    if (IpfsWrapper::Available()) { onNodeReady(); return; }   // already running (e.g. re-enabled)
    const std::string Repo = (AppPaths::DataRoot() / "IPFS").string();
    auto Ok = std::make_shared<bool>(false);
    AsyncWork::Run(this,
        [Repo, Ok]{ std::string Err; *Ok = IpfsWrapper::StartNode(Repo, &Err); },   // off-thread: repo open + swarm join can take a moment
        [this, Ok]{ if (*Ok) onNodeReady(); });
}

void MainWindow::onNodeReady()
{
    // The node's repo is open now (Available()==true); refresh the IPFS tab immediately so it's functional the moment
    // networking is enabled, instead of waiting for its periodic tick. Its network stack comes up shortly after — the
    // ResumeTimer polls for that, then resumes any interrupted downloads.
    if (IpfsModelPtr) IpfsModelPtr->refreshNow();
    if (ResumeTimer && !ResumeTimer->isActive()) ResumeTimer->start();

    // Fetch/index any not-yet-present CID package sources now the node is up — this is what bootstraps the default
    // runners source on a first run (git repos, which needed no node, previously did this at config-init time).
    if (Model) Model->syncSources();
}

void MainWindow::restoreFromTray()
{
    showNormal();
    raise();
    activateWindow();
    if (Tray) Tray->rememberHidden(false);
}

void MainWindow::hideToTray()
{
    hide();
    if (Tray) { Tray->rememberHidden(true); Tray->notifyHidden(); }
}
