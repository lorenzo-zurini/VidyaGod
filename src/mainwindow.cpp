#include "mainwindow.h"
#include "commonutils.h"
#include "packageeditor.h"
#include "jsonoperations.h"
#include "filesystemoperations.h"
#include "prelaunchwindow.h"
#include "ipfswrapper.h"
#include "runnerwrapper.h"
#include "covercache.h"
#include "downloadmanager.h"
#include "ipfstab.h"
#include "settingstab.h"
#include "catalogtab.h"

#include <thread>
#include <atomic>
#include <memory>

#include <QPainter>
#include <QApplication>
#include <QButtonGroup>
#include <algorithm>
#include <QGuiApplication>
#include <QFrame>
#include <QPaintEvent>
#include <QHeaderView>
#include <QProgressBar>
#include <QClipboard>
#include <QScrollBar>
#include <QTimer>
#include <QTableWidgetItem>
#include <QTreeWidgetItem>
#include <QStyledItemDelegate>
#include <QStyleOptionProgressBar>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QSpinBox>




// ═════════════════════════════════════════════════════════════════════════════
// MainWindow
// ═════════════════════════════════════════════════════════════════════════════

MainWindow::MainWindow(nlohmann::ordered_json * gc, QDir * appData, QWidget * parent)
    : QMainWindow(parent), AppDataDir(appData), GlobalConfigJSON(gc)   // base first, then members in declaration order
{
    setWindowTitle("Vidya God");
    setMinimumSize(640, 480);
    {
        QRect screen = QGuiApplication::primaryScreen()->geometry();
        int w = screen.width()  / 2;
        int h = screen.height() / 2;
        int x = (screen.width()  - w) / 2;
        int y = (screen.height() - h) / 2;
        // Restore last saved geometry if present
        if ((*GlobalConfigJSON)["Settings"].contains("CardPixelWidth"))
            CardPixelWidth = int((*GlobalConfigJSON)["Settings"]["CardPixelWidth"]);
        if ((*GlobalConfigJSON)["Settings"].contains("MaxConcurrentDownloads"))
            IpfsWrapper::SetMaxConcurrentDownloads(int((*GlobalConfigJSON)["Settings"]["MaxConcurrentDownloads"]));
        if ((*GlobalConfigJSON)["Settings"].contains("SortMode"))
            CurrentSort = static_cast<SortMode>(int((*GlobalConfigJSON)["Settings"]["SortMode"]));
        if ((*GlobalConfigJSON)["Settings"].contains("WindowW"))
            w = int((*GlobalConfigJSON)["Settings"]["WindowW"]);
        if ((*GlobalConfigJSON)["Settings"].contains("WindowH"))
            h = int((*GlobalConfigJSON)["Settings"]["WindowH"]);
        if ((*GlobalConfigJSON)["Settings"].contains("WindowX"))
            x = int((*GlobalConfigJSON)["Settings"]["WindowX"]);
        if ((*GlobalConfigJSON)["Settings"].contains("WindowY"))
            y = int((*GlobalConfigJSON)["Settings"]["WindowY"]);
        setGeometry(x, y, w, h);
    }
    QWidget * cw = new QWidget(this);
    QVBoxLayout * cl = new QVBoxLayout(cw);
    cl->setContentsMargins(0,0,0,0); cl->setSpacing(0);
    cw->setLayout(cl);
    setCentralWidget(cw);
    CatalogIndex = PackageCatalog::BuildCatalogIndex(*GlobalConfigJSON);   // node-native catalog source
    DownloadMgr = new DownloadManager(*this);                             // owns the Catalog download lifecycle
    BuildStaticUI();
    BuildLibraryGameCards();
    BuildLibraryDynamicUI();
    BuildPackagesDynamicUI();

    // Test/dev hook: open directly on a named tab (e.g. VIDYAGOD_START_TAB=IPFS) so a headless screenshot harness
    // can land on it without coordinate-clicking. Selecting it fires currentChanged (e.g. starts the IPFS refresh).
    const QString StartTab = qEnvironmentVariable("VIDYAGOD_START_TAB");
    if (!StartTab.isEmpty() && MainWindowTabWidget)
        for (int i = 0; i < MainWindowTabWidget->count(); ++i)
            if (MainWindowTabWidget->tabText(i).compare(StartTab, Qt::CaseInsensitive) == 0)
            { MainWindowTabWidget->setCurrentIndex(i); break; }
}

MainWindow::~MainWindow()
{
    qDeleteAll(*LibraryGameCards);
    delete LibraryGameCards;
    // (the Available card pool is owned + freed by CatalogTab)
}

void MainWindow::BuildStaticUI()
{
    MainWindowTabWidget = new QTabWidget(centralWidget());
    centralWidget()->layout()->addWidget(MainWindowTabWidget);

    // ── Library tab ──────────────────────────────────────────────────────────
    LibraryTabWidget = new QWidget(MainWindowTabWidget);
    LibraryTabWidgetLayout = new QVBoxLayout(LibraryTabWidget);
    LibraryTabWidgetLayout->setContentsMargins(0,0,0,0);
    LibraryTabWidgetLayout->setSpacing(0);
    LibraryTabWidget->setLayout(LibraryTabWidgetLayout);
    MainWindowTabWidget->addTab(LibraryTabWidget, "Library");

    // Size picker toolbar
    QWidget * toolbar = new QWidget(LibraryTabWidget);
    QHBoxLayout * tl = new QHBoxLayout(toolbar);
    tl->setContentsMargins(8,4,8,4);

    // Sort buttons — left side
    const QString sortBtnStyle =
        "QPushButton{background:transparent;border:none;font-size:9pt;padding:2px 8px;}"
        "QPushButton:checked{border-bottom:2px solid palette(highlight);font-weight:bold;}"
        "QPushButton:hover{color:palette(highlighted-text);}";

    QButtonGroup * sortGroup = new QButtonGroup(toolbar);
    sortGroup->setExclusive(true);
    auto makeSortBtn = [&](const QString & lbl, SortMode mode) {
        QPushButton * b = new QPushButton(lbl, toolbar);
        b->setCheckable(true);
        b->setChecked(CurrentSort == mode);
        b->setStyleSheet(sortBtnStyle);
        sortGroup->addButton(b);
        QObject::connect(b, &QPushButton::toggled, this, [this, mode](bool checked){
            if (!checked) return;
            CurrentSort = mode;
            sortCards(); View->layoutCards(CardPixelWidth);   // cheap: re-sort + re-filter (covers already scaled)
        });
        tl->addWidget(b);
    };
    makeSortBtn("Name",   SortMode::Name);
    makeSortBtn("Date",   SortMode::Date);
    makeSortBtn("Series", SortMode::Series);

    tl->addStretch();

    // Search filter (by title).
    QLineEdit * searchEdit = new QLineEdit(toolbar);
    searchEdit->setPlaceholderText("Search…");
    searchEdit->setClearButtonEnabled(true);
    searchEdit->setFixedWidth(180);
    QObject::connect(searchEdit, &QLineEdit::textChanged, this, [this](const QString & t){
        LibrarySearch = t.trimmed();
        sortCards(); View->layoutCards(CardPixelWidth);   // cheap: re-filter (covers already scaled)
    });
    tl->addWidget(searchEdit);

    // Size buttons — right side
    auto makeBtn = [&](const QString & lbl, int w) {
        QPushButton * b = new QPushButton(lbl, toolbar);
        b->setCheckable(true); b->setChecked(CardPixelWidth == w);
        b->setStyleSheet(
            "QPushButton{color:#8f98a0;background:transparent;border:none;font-size:9pt;padding:2px 8px;}"
            "QPushButton:checked{color:#c6d4df;border-bottom:2px solid #4a90d9;}"
            "QPushButton:hover{color:#c6d4df;}");
        QObject::connect(b, &QPushButton::clicked, this, [this,w,lbl,toolbar](){
            CardPixelWidth = w;
            for (auto * x : toolbar->findChildren<QPushButton*>()) x->setChecked(x->text()==lbl);
            View->prescaleCovers(CardPixelWidth);
            View->layoutCards(CardPixelWidth);
        });
        tl->addWidget(b);
    };
    makeBtn("Large",250); makeBtn("Medium",185); makeBtn("Small",120);
    LibraryTabWidgetLayout->addWidget(toolbar);

    View = new LibraryView(LibraryTabWidget);
    View->setEmptyMessage("No games yet.\n\nAdd a repository in Settings → Repositories, then download games from the Available tab.");
    LibraryTabWidgetLayout->addWidget(View);

    // Persisted collapse state for the Series-view sections.
    if ((*GlobalConfigJSON)["Settings"].contains("LibraryCollapsedSeries")
        && (*GlobalConfigJSON)["Settings"]["LibraryCollapsedSeries"].is_array())
        for (const auto & N : (*GlobalConfigJSON)["Settings"]["LibraryCollapsedSeries"])
            if (N.is_string()) LibraryCollapsedSeries.insert(QString::fromStdString(std::string(N)));
    connect(View, &LibraryView::groupToggled, this, [this](const QString & name, bool collapsed){
        if (collapsed) LibraryCollapsedSeries.insert(name); else LibraryCollapsedSeries.remove(name);
        auto & arr = (*GlobalConfigJSON)["Settings"]["LibraryCollapsedSeries"] = nlohmann::ordered_json::array();
        for (const QString & N : LibraryCollapsedSeries) arr.push_back(N.toStdString());
        SaveGlobalConfigJSON();
    });

    // ── Packages (built here, hosted as a Settings category — see BuildSettingsTab) ───────────
    PackagesTabWidget = new QWidget(MainWindowTabWidget);
    PackagesTabWidgetLayout = new QVBoxLayout(PackagesTabWidget);
    PackagesTabWidget->setLayout(PackagesTabWidgetLayout);

    QGroupBox * ptb = new QGroupBox(PackagesTabWidget);
    QHBoxLayout * ptbl = new QHBoxLayout(ptb); ptb->setLayout(ptbl);
    PackagesTabWidgetLayout->addWidget(ptb);

    QPushButton * addBtn = new QPushButton("Add Local Package", ptb);
    addBtn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    ptbl->addWidget(addBtn);
    QObject::connect(addBtn, &QPushButton::clicked, this, &MainWindow::on_AddGameButton_clicked);

    QPushButton * edBtn = new QPushButton("Package Editor", ptb);
    edBtn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    ptbl->addWidget(edBtn);
    QObject::connect(edBtn, &QPushButton::clicked, this,
        [this]{ auto * Ed = new PackageEditor(GlobalConfigJSON, this);
                connect(Ed, &PackageEditor::packageSaved, &MainWindow::RefreshPackage); Ed->show(); });

    PackagesScrollArea = new QScrollArea(PackagesTabWidget);
    PackagesScrollArea->setWidgetResizable(true);
    PackagesTabWidgetLayout->addWidget(PackagesScrollArea);

    // ── Catalog (Available) tab ───────────────────────────────────────────────
    CatalogTabPtr = new CatalogTab(*this);
    MainWindowTabWidget->addTab(CatalogTabPtr, "Catalog");

    // ── Settings tab ───────────────────────────────────────────────────────────
    SettingsTabPtr = new SettingsTab(*this);   // owns the sidebar + Runners/Repos/Downloads/Paths pages (page 0 = PackagesTabWidget)
    MainWindowTabWidget->addTab(SettingsTabPtr, "Settings");

    // ── IPFS tab ─────────────────────────────────────────────────────────────────
    IpfsTabPtr = new IpfsTab(*this, MainWindowTabWidget);
    MainWindowTabWidget->addTab(IpfsTabPtr, "IPFS");

    // Only poll IPFS (subprocess-spawning) while its tab is the visible one.
    connect(MainWindowTabWidget, &QTabWidget::currentChanged, this, [this](int){
        IpfsTabPtr->setActive(MainWindowTabWidget->currentWidget() == IpfsTabPtr);
    });

    // Lazy cover loading: when a background cover fetch lands, reload the affected cards. Debounced so a burst of
    // arrivals (e.g. first paint of the Library/Store) coalesces into a single refresh.
    CoverRefreshTimer = new QTimer(this);
    CoverRefreshTimer->setSingleShot(true);
    CoverRefreshTimer->setInterval(200);
    connect(CoverRefreshTimer, &QTimer::timeout, this, [this]{
        if (LibraryGameCards)
        {
            bool Any = false;
            for (LibraryGameCard * Card : *LibraryGameCards)
                if (Card && Card->CoverOriginal.isNull()) { Card->InitializeClassVariables(); Any = true; }
            if (Any && View) View->refreshVisuals();
        }
        if (CatalogTabPtr) CatalogTabPtr->refreshCovers();
    });
    connect(CoverCache::instance(), &CoverCache::coverReady, this, [this](QString){
        if (CoverRefreshTimer && !CoverRefreshTimer->isActive()) CoverRefreshTimer->start();
    });
}

// ═════════════════════════════════════════════════════════════════════════════
// Settings tab — global configurator (category sidebar + stacked forms).
// Only GLOBAL settings live here; per-game settings stay in the PreLaunchWindow.
// ═════════════════════════════════════════════════════════════════════════════


// ═════════════════════════════════════════════════════════════════════════════
// IPFS tab — embedded-node transfers + seeded (pinned) content. The node runs in-process (libvgipfs); if it
// failed to start the tab shows a greyed message instead. There is no external daemon — the node joins the
// network itself and the tab reports its connection + seeded content.
// ═════════════════════════════════════════════════════════════════════════════


//The "Available" tab: the un-hydrated repo games, shown in the SAME card grid as the Library (LibraryView),
//grouped by repository into collapsible sections. Hovering a card shows a Download button; clicking it fetches
//the package's content over IPFS and moves it to the Library.

void MainWindow::BuildLibraryGameCards()
{
    qDeleteAll(*LibraryGameCards); LibraryGameCards->clear();
    //One tile per presentable GROUP of launchable nodes. Only HYDRATED groups (every edition's content present
    //locally) appear in the Library; un-hydrated ones live in the Available tab.
    for (const std::vector<const Node*> & Group : PackageCatalog::PresentableGroups(CatalogIndex))
    {
        std::vector<std::string> Ids;
        bool AllHydrated = true;
        for (const Node * N : Group)
        {
            Ids.push_back(N->NodeId);
            if (!PackageCatalog::NodeHydrated(CatalogIndex, N->NodeId)) AllHydrated = false;
        }
        if (!AllHydrated || Ids.empty()) continue;
        auto * c = new LibraryGameCard(GlobalConfigJSON, &CatalogIndex, std::move(Ids));
        c->InitializeClassVariables();
        LibraryGameCards->append(c);
    }
}

void MainWindow::sortCards()
{
    auto & cards = *LibraryGameCards;
    switch (CurrentSort) {
    case SortMode::Name:
        std::sort(cards.begin(), cards.end(),
            [](auto * a, auto * b){ return a->SortTitle < b->SortTitle; });
        break;
    case SortMode::Date:
        std::sort(cards.begin(), cards.end(),
            [](auto * a, auto * b){ return a->SortDate < b->SortDate; });
        break;
    case SortMode::Series:
        // Games without a series sort last, alphabetically by title within series.
        std::sort(cards.begin(), cards.end(), [](auto * a, auto * b) {
            bool aNoSeries = a->SeriesName.isEmpty();
            bool bNoSeries = b->SeriesName.isEmpty();
            if (aNoSeries != bNoSeries) return bNoSeries; // series before no-series
            if (aNoSeries && bNoSeries) return a->SortTitle < b->SortTitle;
            return a->SortSeriesKey < b->SortSeriesKey;
        });
        break;
    }

    // Apply the search filter to produce the visible subset the view actually shows.
    LibraryVisible.clear();
    for (auto * c : cards)
        if (LibrarySearch.isEmpty() || c->GameTitle.contains(LibrarySearch, Qt::CaseInsensitive))
            LibraryVisible.append(c);
    View->setCards(&LibraryVisible);

    // Series mode → collapsible sections (one per series, ungrouped games under "Other"), computed over the
    // VISIBLE cards. Other modes → flat (no groups, no bands).
    View->setSeriesGroups({});
    if (CurrentSort == SortMode::Series) {
        QVector<LibraryView::Group> groups;
        const int n = LibraryVisible.count();
        int start = 0;
        while (start < n) {
            const QString & sName = LibraryVisible[start]->SeriesName;
            int end = start;
            while (end + 1 < n && LibraryVisible[end + 1]->SeriesName == sName) ++end;
            const QString Name = sName.isEmpty() ? QStringLiteral("Other") : sName;
            groups.append({ Name, start, end, LibraryCollapsedSeries.contains(Name) });
            start = end + 1;
        }
        View->setGroups(groups);
    } else {
        View->setGroups({});
    }
}

void MainWindow::BuildLibraryDynamicUI()
{
    sortCards();   // sorts + filters into LibraryVisible and calls View->setCards()
    View->prescaleCovers(CardPixelWidth);
    View->layoutCards(CardPixelWidth);
}

void MainWindow::BuildPackagesDynamicUI()
{
    if (PackagesScrollArea->widget()) PackagesScrollArea->widget()->deleteLater();
    QWidget * w = new QWidget(PackagesScrollArea);
    PackagesScrollArea->setWidget(w);
    QGridLayout * g = new QGridLayout(w); w->setLayout(g);
    int row = 0;
    for (int i = 0; i < (int)(*GlobalConfigJSON)["LIBRARY"].size(); i++) {
        //Only HYDRATED packages (content present locally) are listed — synced-but-not-downloaded repo entries
        //live in the Available tab, not here. Content-less packages (a malformed game with no layers, or a
        //PATH-only runner) are vacuously hydrated, so require real content to exclude them.
        const std::string LibPath = (*GlobalConfigJSON)["LIBRARY"][i].value("PATH", std::string());
        //Only HYDRATED bundles with a launchable node appear here (a runner-only bundle has none).
        NodeIndex BIdx; ManifestModel::ScanBundleNodes(LibPath, BIdx);
        bool AnyHydratedLaunchable = false;
        for (const auto & [NodeId, N] : BIdx.Nodes)
            if (N.IsLaunchable() && PackageCatalog::NodeHydrated(CatalogIndex, NodeId)) { AnyHydratedLaunchable = true; break; }
        if (!AnyHydratedLaunchable) continue;
        g->addWidget(new QLabel(QString::fromStdString((*GlobalConfigJSON)["LIBRARY"][i].value("PACKAGENAME", std::string())),w),row,0);
        g->addWidget(new QLabel(QString::fromStdString((*GlobalConfigJSON)["LIBRARY"][i].value("PACKAGEUID", std::string())),w),row,1);
        QPushButton * rb = new QPushButton("Remove", w);
        // Key removal by PACKAGEUID, resolved at click time — robust against the array shifting under us.
        const std::string Uid = (*GlobalConfigJSON)["LIBRARY"][i].value("PACKAGEUID", std::string());
        QObject::connect(rb, &QPushButton::clicked, this, [this, Uid]{
            auto & Lib = (*GlobalConfigJSON)["LIBRARY"];
            int idx = -1;
            for (int k = 0; k < (int)Lib.size(); k++)
                if (Lib[k].value("PACKAGEUID", std::string()) == Uid) { idx = k; break; }
            if (idx < 0) return;
            // A managed import (under the library folder) → delete its hydrated copy. A local/portable package
            // added from elsewhere → only drop the reference (never touch the user's own files).
            const std::string Path = Lib[idx].value("PATH", std::string());
            const std::string LibRoot = PackageCatalog::LibraryRootDir(*GlobalConfigJSON);
            std::error_code Ec;
            if (!Path.empty() && !LibRoot.empty())
            {
                const std::string P = std::filesystem::weakly_canonical(std::filesystem::path(Path), Ec).string();
                const std::string R = std::filesystem::weakly_canonical(std::filesystem::path(LibRoot), Ec).string();
                if (P.rfind(R + "/", 0) == 0) std::filesystem::remove_all(P, Ec); // P strictly under the library root
            }
            Lib.erase(idx); RebuildDynamicUI(); SaveGlobalConfigJSON();
        });
        g->addWidget(rb, row, 2);
        row++;
    }
    if (row == 0)
    {
        QLabel * none = new QLabel("No installed packages yet - download packages from the Catalog tab or add local packages.", w);
        none->setStyleSheet("color:#8f98a0;");
        g->addWidget(none, 0, 0, 1, 3);
    }
    g->setRowStretch(g->rowCount(), 1);
}

void MainWindow::RebuildDynamicUI()
{
    CatalogIndex = PackageCatalog::BuildCatalogIndex(*GlobalConfigJSON);   // re-scan the node graph from disk
    BuildLibraryGameCards(); BuildLibraryDynamicUI(); BuildPackagesDynamicUI();
}

void MainWindow::RefreshPackage(const QString & PackagePath)
{
    const QString Want = QDir::cleanPath(PackagePath);

    // Locate the live MainWindow (the editor that emitted may have been opened from anywhere).
    MainWindow * Self = nullptr;
    for (QWidget * W : QApplication::topLevelWidgets())
        if (auto * MW = qobject_cast<MainWindow *>(W)) { Self = MW; break; }

    // Re-scan the node graph and rebuild the tiles. The CatalogIndex address is stable (a member), so open
    // prelaunch dialogs that borrow it stay valid — they just re-read below. Cards no longer share state with
    // dialogs, so a full rebuild is safe.
    if (Self)
    {
        Self->RebuildDynamicUI();
        if (Self->CatalogTabPtr) Self->CatalogTabPtr->rebuild();
    }

    // Reload any open prelaunch dialog(s) whose current edition lives in this bundle.
    for (QWidget * W : QApplication::topLevelWidgets())
        if (auto * PL = qobject_cast<PreLaunchWindow *>(W))
            if (QDir::cleanPath(QString::fromStdString(PL->packagePath())) == Want)
                PL->ReloadAndRebuild();
}

void MainWindow::on_AddGameButton_clicked()
{
    QString sel = QFileDialog::getExistingDirectory(this, "Select package or directory...");
    if (sel.isEmpty()) return;
    QStringList paths;
    std::function<void(const QString &)> scan = [&](const QString & d) {
        QDir dir(d);
        if (FSOps::CheckPackageValid(&dir)) { paths.append(d); return; }
        for (const QString & s : dir.entryList(QDir::Dirs|QDir::NoDotAndDotDot))
            scan(QDir::cleanPath(d + QDir::separator() + s));
    };
    scan(sel);
    if (paths.isEmpty()) { QMessageBox::warning(this,"No packages found","No valid packages found."); return; }
    int added=0, skipped=0;
    for (const QString & path : paths) {
        //Node-native identity: a library bundle must define a launchable node (runner-only bundles aren't games).
        NodeIndex BIdx; ManifestModel::ScanBundleNodes(path.toStdString(), BIdx);
        const Node * Rep = nullptr;
        for (const auto & [id, N] : BIdx.Nodes)
            if (N.IsLaunchable() && (!Rep || (N.Presentable() && !Rep->Presentable()))) Rep = &N;
        if (!Rep) {
            LogWarn("MainWindow", "Skipping " + path.toStdString() + ": no launchable node (not a library bundle).");
            skipped++; continue;
        }
        const std::string Uid  = Rep->Uid.empty() ? Rep->NodeId : Rep->Uid;
        const std::string Name = Rep->Meta.is_object() ? Rep->Meta.value("TITLE", Rep->NodeId) : Rep->NodeId;
        bool dup=false;
        for (auto & e : (*GlobalConfigJSON)["LIBRARY"])
            if (e.value("PACKAGEUID", std::string()) == Uid) { dup=true; skipped++; break; }
        if (dup) continue;
        nlohmann::ordered_json slim;
        slim["PACKAGEUID"]=Uid;
        slim["PACKAGENAME"]=Name;
        slim["PATH"]=path.toStdString();
        (*GlobalConfigJSON)["LIBRARY"].push_back(slim);
        added++;
    }
    if (added>0) { SaveGlobalConfigJSON(); RebuildDynamicUI(); }
    QMessageBox::information(this,"Done",
        QString("Added %1 package(s). %2 skipped.").arg(added).arg(skipped));
}

bool MainWindow::SaveGlobalConfigJSON()
{
    return JSONOps::SaveJSON(GlobalConfigJSON, new QFile(AppDataDir->filePath("GlobalConfig.JSON")));
}

void MainWindow::closeEvent(QCloseEvent * e)
{
    (*GlobalConfigJSON)["Settings"]["WindowW"]        = width();
    (*GlobalConfigJSON)["Settings"]["WindowH"]        = height();
    (*GlobalConfigJSON)["Settings"]["WindowX"]        = x();
    (*GlobalConfigJSON)["Settings"]["WindowY"]        = y();
    (*GlobalConfigJSON)["Settings"]["CardPixelWidth"] = CardPixelWidth;
    (*GlobalConfigJSON)["Settings"]["SortMode"]        = static_cast<int>(CurrentSort);
    SaveGlobalConfigJSON();
    QMainWindow::closeEvent(e);
}

void MainWindow::MainWindowGridSizeChanged() {}
