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
    qDeleteAll(*AvailableGameCards);
    delete AvailableGameCards;
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

    // ── Available tab (repo games to download over IPFS) ───────────────────────
    BuildAvailableTab();

    // ── Settings tab ───────────────────────────────────────────────────────────
    BuildSettingsTab();

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
        if (AvailableGameCards)
        {
            bool Any = false;
            for (LibraryGameCard * Card : *AvailableGameCards)
                if (Card && Card->CoverOriginal.isNull()) { Card->InitializeClassVariables(); Any = true; }
            if (Any && AvailableView) AvailableView->refreshVisuals();
        }
    });
    connect(CoverCache::instance(), &CoverCache::coverReady, this, [this](QString){
        if (CoverRefreshTimer && !CoverRefreshTimer->isActive()) CoverRefreshTimer->start();
    });
}

// ═════════════════════════════════════════════════════════════════════════════
// Settings tab — global configurator (category sidebar + stacked forms).
// Only GLOBAL settings live here; per-game settings stay in the PreLaunchWindow.
// ═════════════════════════════════════════════════════════════════════════════

void MainWindow::BuildSettingsTab()
{
    SettingsTabWidget = new QWidget(MainWindowTabWidget);
    QHBoxLayout * outer = new QHBoxLayout(SettingsTabWidget);
    outer->setContentsMargins(0,0,0,0); outer->setSpacing(0);
    SettingsTabWidget->setLayout(outer);
    MainWindowTabWidget->addTab(SettingsTabWidget, "Settings");

    // Left: category sidebar.
    SettingsCategoryList = new QListWidget(SettingsTabWidget);
    SettingsCategoryList->setFixedWidth(170);
    SettingsCategoryList->setFrameShape(QFrame::NoFrame);
    SettingsCategoryList->addItem("Installed Packages");
    SettingsCategoryList->addItem("Runners");
    SettingsCategoryList->addItem("Repositories");
    SettingsCategoryList->addItem("Downloads");
    SettingsCategoryList->addItem("Storage & Paths");
    outer->addWidget(SettingsCategoryList);

    // Right: stacked category forms.
    SettingsStack = new QStackedWidget(SettingsTabWidget);
    outer->addWidget(SettingsStack, 1);

    // Page 0 — Installed Packages (built in BuildStaticUI; the +Add Local Package / Package Editor / list).
    SettingsStack->addWidget(PackagesTabWidget);

    // Page 1 — Runners (a scroll area we rebuild in place on structural edits).
    {
        QWidget * page = new QWidget(SettingsStack);
        QVBoxLayout * pl = new QVBoxLayout(page); page->setLayout(pl);
        pl->setContentsMargins(8,8,8,8);
        SettingsRunnersScroll = new QScrollArea(page);
        SettingsRunnersScroll->setWidgetResizable(true);
        SettingsRunnersScroll->setFrameShape(QFrame::NoFrame);
        pl->addWidget(SettingsRunnersScroll);
        SettingsStack->addWidget(page);
        RebuildSettingsRunnersPage();
    }

    // Page 2 — Repositories (a scroll area we rebuild in place on add/remove).
    {
        QWidget * page = new QWidget(SettingsStack);
        QVBoxLayout * pl = new QVBoxLayout(page); page->setLayout(pl);
        pl->setContentsMargins(8,8,8,8);
        SettingsReposScroll = new QScrollArea(page);
        SettingsReposScroll->setWidgetResizable(true);
        SettingsReposScroll->setFrameShape(QFrame::NoFrame);
        pl->addWidget(SettingsReposScroll);
        SettingsStack->addWidget(page);
        RebuildSettingsReposPage();
    }

    // Page 3 — Downloads.
    SettingsStack->addWidget(BuildDownloadsSettingsPage());

    // Page 4 — Storage & Paths.
    SettingsStack->addWidget(BuildPathsSettingsPage());

    QObject::connect(SettingsCategoryList, &QListWidget::currentRowChanged,
                     SettingsStack, &QStackedWidget::setCurrentIndex);
    SettingsCategoryList->setCurrentRow(0);
}

void MainWindow::RebuildSettingsRunnersPage()
{
    if (!SettingsRunnersScroll) return;
    if (SettingsRunnersScroll->widget()) SettingsRunnersScroll->widget()->deleteLater();

    QWidget * contents = new QWidget(SettingsRunnersScroll);
    QVBoxLayout * v = new QVBoxLayout(contents); contents->setLayout(v);
    SettingsRunnersScroll->setWidget(contents);

    // Runners are packages from the configured repositories. One that ships its own build (e.g. Proton over
    // IPFS) must be imported (build fetched + DEFPREFIX generated) before games can use it.
    QLabel * intro = new QLabel(
        "Runners are packages from your repositories. A runner that ships its own build (e.g. Proton over "
        "IPFS) must be imported before games can use it.", contents);
    intro->setWordWrap(true);
    intro->setStyleSheet("color:#8f98a0;font-size:9pt;");
    v->addWidget(intro);

    //Runners are the ROLE:"runner" nodes of the global catalog graph — one card per runner node. An EMBEDDED runner
    //(bundled inside a game package) is hidden until installed — it's offered in that game's download dialog instead;
    //global runners always show (installed or installable via the Import button below).
    std::vector<const Node*> Runners;
    for (const auto & [Id, N] : CatalogIndex.Nodes)
    {
        (void)Id;
        if (!N.IsRunner()) continue;
        if (PackageCatalog::IsEmbeddedRunner(CatalogIndex, N.NodeId)
            && !PackageCatalog::RunnerInstalled(CatalogIndex, N.NodeId)) continue;
        Runners.push_back(&N);
    }
    std::sort(Runners.begin(), Runners.end(), [](const Node* A, const Node* B){ return A->NodeId < B->NodeId; });
    if (Runners.empty())
    {
        QLabel * none = new QLabel("No runners found in your repositories.", contents);
        none->setStyleSheet("color:#8f98a0;");
        v->addWidget(none);
    }
    for (const Node * R : Runners)
    {
        const std::string rid = R->NodeId;
        QGroupBox * card = new QGroupBox(QString::fromStdString(rid), contents);
        QVBoxLayout * cv = new QVBoxLayout(card); card->setLayout(cv);

        QString guest;
        for (const auto & P : R->GuestPlatform) guest += (guest.isEmpty() ? "" : ", ") + QString::fromStdString(P);
        const QString Desc = QString::fromStdString(R->HostPlatform) + " → [" + guest + "]";

        const bool Avail    = RunnerWrapper::ExecutableAvailable(R->Exec);
        const bool Ships    = !PackageCatalog::NodeContentCids(CatalogIndex, rid).empty();
        const bool Imported = ContainerWrapper::RunnerNodeImported(CatalogIndex, rid);
        QHBoxLayout * row = new QHBoxLayout();
        row->addWidget(new QLabel(Desc, card), 1);
        QLabel * st = new QLabel(card);
        if (!Ships && Avail) st->setText("<span style='color:#8f98a0;'>built-in</span>");
        else if (!Ships)    st->setText("<span style='color:#c0726a;'>not installed on system</span>");
        else if (Imported)  st->setText("<span style='color:#5fb55f;'>Imported</span>");
        else                 st->setText("<span style='color:#c0726a;'>Not imported</span>");
        row->addWidget(st);
        if (Ships && !Imported && IpfsWrapper::Available())
        {
            QPushButton * btn = new QPushButton("Import", card);
            connect(btn, &QPushButton::clicked, this, [this, rid, btn, st]{
                if (!IpfsFetchReady(this)) return;              // need the embedded node online to fetch
                btn->setEnabled(false); btn->setText("Importing…");
                st->setText("<span style='color:#c6a15f;'>Importing… (see IPFS tab)</span>");
                // The worker reads config + index; hand it private copies so it never races the GUI thread (which
                // owns the live GlobalConfigJSON/CatalogIndex and may mutate them concurrently).
                auto Cfg = std::make_shared<nlohmann::ordered_json>(*GlobalConfigJSON);
                NodeIndex Idx = CatalogIndex;
                std::thread([this, rid, Cfg, Idx = std::move(Idx)]{
                    std::string Err;
                    bool Ok = ContainerWrapper::ImportRunnerNode(*Cfg, Idx, rid, &Err);
                    QMetaObject::invokeMethod(this, [this, Ok, Err]{
                        if (!Ok) LogErr("MainWindow", "Runner import failed: " + Err);   // no dialog — see the IPFS tab
                        RebuildSettingsRunnersPage(); IpfsTabPtr->refresh();
                    }, Qt::QueuedConnection);
                }).detach();
            });
            row->addWidget(btn);
        }
        else if (Ships && !Imported)
        {
            QLabel * need = new QLabel("IPFS unavailable", card);
            need->setStyleSheet("color:#8f98a0;font-style:italic;");
            row->addWidget(need);
        }
        cv->addLayout(row);
        v->addWidget(card);
    }
    v->addStretch(1);
}

// Repositories settings page — add/remove the git repos that share dehydrated packages.
// Each Settings.Repositories[] entry is {NAME, PATH:<git url>}, cloned to ~/.VidyaGod/LIBRARY/<name>
// and indexed. Adding one clones it (off-thread); removing drops the reference (the disposable clone is
// left on disk). After any change the Store + Runners pages are rebuilt so the new catalog shows.
void MainWindow::RebuildSettingsReposPage()
{
    if (!SettingsReposScroll) return;
    if (SettingsReposScroll->widget()) SettingsReposScroll->widget()->deleteLater();

    QWidget * contents = new QWidget(SettingsReposScroll);
    QVBoxLayout * v = new QVBoxLayout(contents); contents->setLayout(v);
    SettingsReposScroll->setWidget(contents);

    QLabel * intro = new QLabel(
        "Repositories are git repos that share dehydrated packages (manifests + IPFS CIDs, no bundled content). "
        "Cloned into your LIBRARY, where packages hydrate their content in place; private repos work if your git is "
        "set up to authenticate non-interactively (SSH key or a stored token).", contents);
    intro->setWordWrap(true);
    intro->setStyleSheet("color:#8f98a0;font-size:9pt;");
    v->addWidget(intro);

    // ── Sync now ── pull all repos again to pick up packages added/updated upstream (no restart needed).
    {
        QHBoxLayout * syncRow = new QHBoxLayout();
        QPushButton * syncBtn = new QPushButton("Sync now", contents);
        syncRow->addWidget(syncBtn); syncRow->addStretch(1);
        v->addLayout(syncRow);
        connect(syncBtn, &QPushButton::clicked, this, [this, syncBtn]{
            syncBtn->setEnabled(false); syncBtn->setText("Syncing…");
            // Sync on a private config copy (it git-pulls + rebuilds LIBRARY), then apply just LIBRARY back on the
            // GUI thread — so the worker never mutates the live GlobalConfigJSON the GUI may be reading/writing.
            auto Cfg = std::make_shared<nlohmann::ordered_json>(*GlobalConfigJSON);
            std::thread([this, Cfg]{
                PackageCatalog::SyncRepositories(*Cfg);   // git pull each repo + reindex LIBRARY (into the copy)
                QMetaObject::invokeMethod(this, [this, Cfg]{
                    (*GlobalConfigJSON)["LIBRARY"] = (*Cfg)["LIBRARY"];   // SyncRepositories only writes LIBRARY
                    SaveGlobalConfigJSON();
                    RebuildDynamicUI(); RebuildSettingsReposPage(); RebuildSettingsRunnersPage(); RebuildAvailableTab();
                }, Qt::QueuedConnection);
            }).detach();
        });
    }

    auto & S = (*GlobalConfigJSON)["Settings"];
    if (!S.contains("Repositories") || !S["Repositories"].is_array())
        S["Repositories"] = nlohmann::ordered_json::array();

    if (S["Repositories"].empty())
    {
        QLabel * none = new QLabel("No repositories configured.", contents);
        none->setStyleSheet("color:#8f98a0;");
        v->addWidget(none);
    }
    for (int i = 0; i < int(S["Repositories"].size()); ++i)
    {
        const auto & R = S["Repositories"][i];
        const std::string Url  = R.is_object() ? R.value("PATH", std::string()) : std::string();
        std::string Name = R.is_object() ? R.value("NAME", std::string()) : std::string();
        if (Name.empty()) { QString b = QString::fromStdString(Url); b = b.section('/', -1); if (b.endsWith(".git")) b.chop(4); Name = b.toStdString(); }

        QGroupBox * card = new QGroupBox(QString::fromStdString(Name), contents);
        QHBoxLayout * row = new QHBoxLayout(card); card->setLayout(row);
        QLabel * urlLbl = new QLabel("<span style='color:#8f98a0;'>" + QString::fromStdString(Url).toHtmlEscaped() + "</span>", card);
        urlLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
        row->addWidget(urlLbl, 1);
        QPushButton * rm = new QPushButton("Remove", card);
        connect(rm, &QPushButton::clicked, this, [this, i]{
            auto & SS = (*GlobalConfigJSON)["Settings"];
            if (SS.contains("Repositories") && SS["Repositories"].is_array() && i < int(SS["Repositories"].size()))
                SS["Repositories"].erase(SS["Repositories"].begin() + i);
            SaveGlobalConfigJSON();
            RebuildSettingsReposPage(); RebuildSettingsRunnersPage(); RebuildAvailableTab();
        });
        row->addWidget(rm);
        v->addWidget(card);
    }

    // ── Add-repository row ──
    QGroupBox * add = new QGroupBox("Add repository", contents);
    QFormLayout * af = new QFormLayout(add); add->setLayout(af);
    QLineEdit * nameEdit = new QLineEdit(add);
    nameEdit->setPlaceholderText("(optional — defaults to the URL's basename)");
    QLineEdit * urlEdit = new QLineEdit(add);
    urlEdit->setPlaceholderText("https://github.com/you/Repo.git  or  git@github.com:you/Repo.git");
    af->addRow("Name:", nameEdit);
    af->addRow("Git URL:", urlEdit);
    QPushButton * addBtn = new QPushButton("Add + sync", add);
    af->addRow("", addBtn);
    connect(addBtn, &QPushButton::clicked, this, [this, nameEdit, urlEdit, addBtn]{
        const QString Url = urlEdit->text().trimmed();
        if (Url.isEmpty()) { QMessageBox::warning(this, "Add repository", "Enter a git URL."); return; }
        nlohmann::ordered_json Entry = nlohmann::ordered_json::object();
        const QString Nm = nameEdit->text().trimmed();
        if (!Nm.isEmpty()) Entry["NAME"] = Nm.toStdString();
        Entry["PATH"] = Url.toStdString();
        auto & SS = (*GlobalConfigJSON)["Settings"];
        if (!SS.contains("Repositories") || !SS["Repositories"].is_array()) SS["Repositories"] = nlohmann::ordered_json::array();
        SS["Repositories"].push_back(Entry);
        SaveGlobalConfigJSON();
        addBtn->setEnabled(false); addBtn->setText("Cloning…");
        // Clone/pull + index off-thread on a private config copy (includes the entry just added above), then apply
        // LIBRARY back on the GUI thread — no worker mutation of the live GlobalConfigJSON.
        auto Cfg = std::make_shared<nlohmann::ordered_json>(*GlobalConfigJSON);
        std::thread([this, Cfg]{
            PackageCatalog::SyncRepositories(*Cfg);   // git clone/pull + reindex LIBRARY (into the copy)
            QMetaObject::invokeMethod(this, [this, Cfg]{
                (*GlobalConfigJSON)["LIBRARY"] = (*Cfg)["LIBRARY"];
                SaveGlobalConfigJSON();
                RebuildDynamicUI(); RebuildSettingsReposPage(); RebuildSettingsRunnersPage(); RebuildAvailableTab();
            }, Qt::QueuedConnection);
        }).detach();
    });
    v->addWidget(add);

    v->addStretch(1);
}

QWidget * MainWindow::BuildDownloadsSettingsPage()
{
    QWidget * page = new QWidget(SettingsStack);
    QVBoxLayout * pl = new QVBoxLayout(page); page->setLayout(pl);
    pl->setContentsMargins(12,12,12,12);
    QFormLayout * form = new QFormLayout();
    pl->addLayout(form);

    // Max simultaneous downloads — Settings.MaxConcurrentDownloads. Caps how many files fetch at once across ALL
    // packages, so one slow/stalled file no longer holds up the rest of the queue.
    QSpinBox * maxDl = new QSpinBox(page);
    maxDl->setRange(1, 32);
    maxDl->setValue(IpfsWrapper::MaxConcurrentDownloads());
    {
        auto & S = (*GlobalConfigJSON)["Settings"];
        if (S.contains("MaxConcurrentDownloads") && S["MaxConcurrentDownloads"].is_number_integer())
            maxDl->setValue(int(S["MaxConcurrentDownloads"]));
    }
    QObject::connect(maxDl, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v){
        IpfsWrapper::SetMaxConcurrentDownloads(v);
        (*GlobalConfigJSON)["Settings"]["MaxConcurrentDownloads"] = v;
        SaveGlobalConfigJSON();
    });
    form->addRow("Max simultaneous downloads:", maxDl);

    QLabel * note = new QLabel("Files download in parallel up to this limit. A stalled download keeps retrying in the "
                               "background without blocking the others; raise this to fetch more at once.", page);
    note->setWordWrap(true);
    note->setStyleSheet("color:#8f98a0;font-size:9pt;");
    pl->addWidget(note);
    pl->addStretch(1);
    return page;
}

QWidget * MainWindow::BuildPathsSettingsPage()
{
    QWidget * page = new QWidget(SettingsStack);
    QVBoxLayout * pl = new QVBoxLayout(page); page->setLayout(pl);
    pl->setContentsMargins(12,12,12,12);
    QFormLayout * form = new QFormLayout();
    pl->addLayout(form);

    const QString defaultTempRoot = QDir::cleanPath(QDir::homePath() + "/.VidyaGod/TEMP");

    // Temporary / runtime root — Settings.Paths.TempRoot (empty = default).
    QWidget * rootRow = new QWidget(page);
    QHBoxLayout * rl = new QHBoxLayout(rootRow); rl->setContentsMargins(0,0,0,0);
    rootRow->setLayout(rl);
    QLineEdit * rootEdit = new QLineEdit(rootRow);
    rootEdit->setPlaceholderText(defaultTempRoot + "  (default)");
    {
        auto & S = (*GlobalConfigJSON)["Settings"];
        if (S.contains("Paths") && S["Paths"].is_object()
            && S["Paths"].contains("TempRoot") && S["Paths"]["TempRoot"].is_string())
            rootEdit->setText(QString::fromStdString(std::string(S["Paths"]["TempRoot"])));
    }
    auto writeTempRoot = [this,rootEdit]{
        auto & S = (*GlobalConfigJSON)["Settings"];
        QString t = rootEdit->text().trimmed();
        if (t.isEmpty()) {
            if (S.contains("Paths") && S["Paths"].is_object()) S["Paths"].erase("TempRoot");
        } else {
            if (!S.contains("Paths") || !S["Paths"].is_object()) S["Paths"] = nlohmann::ordered_json::object();
            S["Paths"]["TempRoot"] = t.toStdString();
        }
        SaveGlobalConfigJSON();
    };
    QObject::connect(rootEdit, &QLineEdit::editingFinished, this, writeTempRoot);
    rl->addWidget(rootEdit, 1);
    QPushButton * browse = new QPushButton("Browse…", rootRow);
    QObject::connect(browse, &QPushButton::clicked, this, [this,rootEdit,writeTempRoot]{
        QString d = QFileDialog::getExistingDirectory(this, "Select temporary / runtime root");
        if (!d.isEmpty()) { rootEdit->setText(d); writeTempRoot(); }
    });
    rl->addWidget(browse);
    form->addRow("Temporary / runtime root:", rootRow);

    // Library folder — Settings.Paths.LibraryRoot (empty = default). Imported packages are hydrated here,
    // one subfolder per repo.
    const QString defaultLibRoot = QDir::cleanPath(QDir::homePath() + "/.VidyaGod/library");
    QWidget * libRow = new QWidget(page);
    QHBoxLayout * ll = new QHBoxLayout(libRow); ll->setContentsMargins(0,0,0,0);
    libRow->setLayout(ll);
    QLineEdit * libEdit = new QLineEdit(libRow);
    libEdit->setPlaceholderText(defaultLibRoot + "  (default)");
    {
        auto & S = (*GlobalConfigJSON)["Settings"];
        if (S.contains("Paths") && S["Paths"].is_object()
            && S["Paths"].contains("LibraryRoot") && S["Paths"]["LibraryRoot"].is_string())
            libEdit->setText(QString::fromStdString(std::string(S["Paths"]["LibraryRoot"])));
    }
    auto writeLibRoot = [this,libEdit]{
        auto & S = (*GlobalConfigJSON)["Settings"];
        QString t = libEdit->text().trimmed();
        if (t.isEmpty()) {
            if (S.contains("Paths") && S["Paths"].is_object()) S["Paths"].erase("LibraryRoot");
        } else {
            if (!S.contains("Paths") || !S["Paths"].is_object()) S["Paths"] = nlohmann::ordered_json::object();
            S["Paths"]["LibraryRoot"] = t.toStdString();
        }
        SaveGlobalConfigJSON();
    };
    QObject::connect(libEdit, &QLineEdit::editingFinished, this, writeLibRoot);
    ll->addWidget(libEdit, 1);
    QPushButton * libBrowse = new QPushButton("Browse…", libRow);
    QObject::connect(libBrowse, &QPushButton::clicked, this, [this,libEdit,writeLibRoot]{
        QString d = QFileDialog::getExistingDirectory(this, "Select library folder");
        if (!d.isEmpty()) { libEdit->setText(d); writeLibRoot(); }
    });
    ll->addWidget(libBrowse);
    form->addRow("Library folder:", libRow);

    // AppData dir (read-only, informational).
    QLineEdit * appDataLbl = new QLineEdit(AppDataDir->path(), page);
    appDataLbl->setReadOnly(true);
    form->addRow("App data directory:", appDataLbl);

    QLabel * note = new QLabel("Path changes apply to future launches.", page);
    note->setStyleSheet("color:#8f98a0;font-size:9pt;");
    pl->addWidget(note);
    pl->addStretch(1);
    return page;
}

// ═════════════════════════════════════════════════════════════════════════════
// IPFS tab — embedded-node transfers + seeded (pinned) content. The node runs in-process (libvgipfs); if it
// failed to start the tab shows a greyed message instead. There is no external daemon — the node joins the
// network itself and the tab reports its connection + seeded content.
// ═════════════════════════════════════════════════════════════════════════════


//The "Available" tab: the un-hydrated repo games, shown in the SAME card grid as the Library (LibraryView),
//grouped by repository into collapsible sections. Hovering a card shows a Download button; clicking it fetches
//the package's content over IPFS and moves it to the Library.
void MainWindow::BuildAvailableTab()
{
    // Load persisted collapse state for repo sections.
    AvailableCollapsedRepos.clear();
    if ((*GlobalConfigJSON)["Settings"].contains("AvailableCollapsedRepos")
        && (*GlobalConfigJSON)["Settings"]["AvailableCollapsedRepos"].is_array())
        for (const auto & R : (*GlobalConfigJSON)["Settings"]["AvailableCollapsedRepos"])
            if (R.is_string()) AvailableCollapsedRepos.insert(QString::fromStdString(std::string(R)));

    AvailableTabWidget = new QWidget(MainWindowTabWidget);
    QVBoxLayout * v = new QVBoxLayout(AvailableTabWidget);
    v->setContentsMargins(0,0,0,0); v->setSpacing(0);
    AvailableTabWidget->setLayout(v);
    MainWindowTabWidget->addTab(AvailableTabWidget, "Catalog");

    // Toolbar — mirrors the Library tab: Name/Date/Series sort (within each repo) + size picker.
    QWidget * toolbar = new QWidget(AvailableTabWidget);
    QHBoxLayout * tl = new QHBoxLayout(toolbar);
    tl->setContentsMargins(8,4,8,4);

    const QString sortBtnStyle =
        "QPushButton{background:transparent;border:none;font-size:9pt;padding:2px 8px;}"
        "QPushButton:checked{border-bottom:2px solid palette(highlight);font-weight:bold;}"
        "QPushButton:hover{color:palette(highlighted-text);}";
    QButtonGroup * sortGroup = new QButtonGroup(toolbar);
    sortGroup->setExclusive(true);
    auto makeSortBtn = [&](const QString & lbl, SortMode mode) {
        QPushButton * b = new QPushButton(lbl, toolbar);
        b->setCheckable(true); b->setChecked(AvailableSort == mode);
        b->setStyleSheet(sortBtnStyle);
        sortGroup->addButton(b);
        connect(b, &QPushButton::toggled, this, [this, mode](bool checked){
            if (!checked) return;
            AvailableSort = mode; ApplyAvailableFilter();   // cheap: re-sort the existing pool (no restat)
        });
        tl->addWidget(b);
    };
    makeSortBtn("Name", SortMode::Name); makeSortBtn("Date", SortMode::Date); makeSortBtn("Series", SortMode::Series);
    tl->addStretch();
    QLineEdit * availSearch = new QLineEdit(toolbar);
    availSearch->setPlaceholderText("Search…");
    availSearch->setClearButtonEnabled(true);
    availSearch->setFixedWidth(180);
    connect(availSearch, &QLineEdit::textChanged, this, [this](const QString & t){
        AvailableSearch = t.trimmed();
        ApplyAvailableFilter();                             // cheap: re-filter the existing pool (no restat)
    });
    tl->addWidget(availSearch);
    auto makeSizeBtn = [&](const QString & lbl, int w) {
        QPushButton * b = new QPushButton(lbl, toolbar);
        b->setCheckable(true); b->setChecked(CardPixelWidth == w);
        b->setStyleSheet(
            "QPushButton{color:#8f98a0;background:transparent;border:none;font-size:9pt;padding:2px 8px;}"
            "QPushButton:checked{color:#c6d4df;border-bottom:2px solid #4a90d9;}"
            "QPushButton:hover{color:#c6d4df;}");
        connect(b, &QPushButton::clicked, this, [this,w,lbl,toolbar](){
            CardPixelWidth = w;
            for (auto * x : toolbar->findChildren<QPushButton*>()) if (x->isCheckable() && (x->text()=="Large"||x->text()=="Medium"||x->text()=="Small")) x->setChecked(x->text()==lbl);
            if (AvailableView) {
                AvailableView->setCards(AvailableGameCards);   // point at the full pool so every cover is re-scaled
                AvailableView->prescaleCovers(CardPixelWidth);
                ApplyAvailableFilter();                        // repoint to the filtered subset + relayout
            }
        });
        tl->addWidget(b);
    };
    makeSizeBtn("Large",250); makeSizeBtn("Medium",185); makeSizeBtn("Small",120);
    v->addWidget(toolbar);

    AvailableView = new LibraryView(AvailableTabWidget);
    AvailableView->setHoverAction("⬇  Download", true);
    AvailableView->setEmptyMessage("Nothing to download.\n\nAdd a repository in Settings → Repositories (or hit “Sync now”) to see shared games here.");
    v->addWidget(AvailableView);

    connect(AvailableView, &LibraryView::downloadRequested, this, [this](LibraryGameCard * card){ DownloadMgr->startDownload(card); });
    connect(AvailableView, &LibraryView::cancelRequested,   this, [this](LibraryGameCard * card){ DownloadMgr->requestCancel(card); });
    connect(AvailableView, &LibraryView::groupToggled, this, [this](const QString & name, bool collapsed){
        if (collapsed) AvailableCollapsedRepos.insert(name); else AvailableCollapsedRepos.remove(name);
        auto & arr = (*GlobalConfigJSON)["Settings"]["AvailableCollapsedRepos"] = nlohmann::ordered_json::array();
        for (const QString & R : AvailableCollapsedRepos) arr.push_back(R.toStdString());
        SaveGlobalConfigJSON();
    });

    // Live download progress: average a package's content-CID transfer percents onto its Available card(s).
    connect(IpfsManager::instance(), &IpfsManager::transferProgress, this,
            [this](QString cid, double pct){ DownloadMgr->applyProgress(cid, pct); });
    connect(IpfsManager::instance(), &IpfsManager::transferFinished, this,
            [this](QString cid, bool, QString){ DownloadMgr->applyProgress(cid, 100.0); });

    RebuildAvailableTab();
}

//(Re)builds the Available grid: one card per un-hydrated repo game, grouped by repository (collapsible).
//EXPENSIVE — rebuilds the full Available card POOL: enumerate presentable groups + check hydration (filesystem
//stats + graph walks) and create one card per un-hydrated, fetchable tile. Call only when the catalog actually
//changes (sync / import / download complete), NOT on every sort/search keystroke — those call ApplyAvailableFilter.
void MainWindow::RebuildAvailableTab()
{
    if (!AvailableView) return;
    AvailableVisible.clear();
    qDeleteAll(*AvailableGameCards); AvailableGameCards->clear();

    //One card per un-hydrated presentable group: at least one edition's content is still missing AND fetchable
    //over IPFS. (A fully-hydrated group lives in the Library tab.)
    for (const std::vector<const Node*> & Group : PackageCatalog::PresentableGroups(CatalogIndex))
    {
        std::vector<std::string> Ids;
        bool AnyMissing = false, AnyFetchable = false;
        for (const Node * N : Group)
        {
            Ids.push_back(N->NodeId);
            if (!PackageCatalog::NodeHydrated(CatalogIndex, N->NodeId)) AnyMissing = true;
            if (!PackageCatalog::NodeContentCids(CatalogIndex, N->NodeId).empty()) AnyFetchable = true;
        }
        if (!AnyMissing || !AnyFetchable || Ids.empty()) continue;
        auto * c = new LibraryGameCard(GlobalConfigJSON, &CatalogIndex, std::move(Ids));
        c->InitializeClassVariables();

        const bool Busy = DownloadMgr->isDownloading(c->GroupKey);
        c->Downloading = Busy;
        c->DownloadPercent = Busy ? DownloadMgr->busyPercent(c->GroupKey) : -1.0;   // carry current progress across rebuilds
        AvailableGameCards->append(c);
    }

    // ── Per-package grouping (Catalog diverges from Library): a bundle with several un-hydrated games shows its
    // base game as the main tile and the rest as small secondaries beneath it. Base = the game all others build on,
    // detected from its content-closure node set: the leanest closure is the base (validated as universally correct
    // across the multi-game bundles; tie-break by representative node id). ──
    auto ClosureSize = [this](LibraryGameCard * c) -> int {
        const std::string & Lid = c->RepNodeId;
        if (Lid.empty()) return 0;
        int n = 0;
        for (const std::string & Nid : ManifestModel::ResolveNodeOrder(CatalogIndex, Lid, {})) {
            const Node * N = CatalogIndex.Find(Nid);
            if (N && !N->IsRunner()) n++;
        }
        return n;
    };
    std::map<std::string, std::vector<LibraryGameCard*>> ByBundle;     // bundle dir → its un-hydrated game cards
    std::vector<std::string> BundleOrder;
    for (LibraryGameCard * c : *AvailableGameCards) {
        const std::string Key = c->PackagePath.string();
        if (ByBundle.find(Key) == ByBundle.end()) BundleOrder.push_back(Key);
        ByBundle[Key].push_back(c);
    }
    for (const std::string & Key : BundleOrder) {
        std::vector<LibraryGameCard*> & games = ByBundle[Key];
        if (games.size() < 2) continue;                                // single-game package → normal full tile
        std::sort(games.begin(), games.end(), [&](LibraryGameCard * a, LibraryGameCard * b){
            const int sa = ClosureSize(a), sb = ClosureSize(b);
            if (sa != sb) return sa < sb;                              // leanest closure = the base game
            return a->RepNodeId < b->RepNodeId;
        });
        LibraryGameCard * Main = games.front();                       // most-base un-hydrated game anchors the package
        for (size_t i = 1; i < games.size(); i++) {
            games[i]->IsSecondary = true;
            Main->Secondaries.push_back(games[i]);
        }
    }

    // Point the view at the full pool, then scale every cover ONCE — BEFORE ApplyAvailableFilter shows them.
    // (Scaling must happen while the view's card list IS the pool; otherwise CoverScaled stays null and the
    // covers paint black until the next refreshVisuals — the "flash black on download complete" bug.)
    AvailableView->setCards(AvailableGameCards);
    AvailableView->prescaleCovers(CardPixelWidth);
    ApplyAvailableFilter();
}

//CHEAP — re-filters the existing pool by search, re-sorts + re-groups, and shows it. No card recreation, no
//filesystem stats, no cover re-scaling. This is the sort/search path.
void MainWindow::ApplyAvailableFilter()
{
    if (!AvailableView) return;

    // Repo name for a card (groups un-repo'd local entries under "Local").
    auto RepoOf = [this](LibraryGameCard * c) -> QString { return RepoNameForBundle(c->PackagePath); };

    AvailableVisible.clear();
    for (LibraryGameCard * c : *AvailableGameCards) {
        if (c->IsSecondary) continue;                                 // secondaries travel with their package's main tile
        // A package matches if its main OR any secondary game matches the search (so a search keeps the whole package).
        bool match = AvailableSearch.isEmpty() || c->GameTitle.contains(AvailableSearch, Qt::CaseInsensitive);
        for (LibraryGameCard * sc : c->Secondaries)
            if (!match && sc->GameTitle.contains(AvailableSearch, Qt::CaseInsensitive)) match = true;
        if (match) AvailableVisible.append(c);
    }

    // Sort by (repo, within-group key) so each repo is a contiguous run; then build collapsible groups.
    auto keyOf = [this](LibraryGameCard * a) -> QString {
        switch (AvailableSort) { case SortMode::Date:   return a->SortDate + "|" + a->SortTitle;
                                 case SortMode::Series: return a->SortSeriesKey;
                                 default:               return a->SortTitle; }
    };
    std::sort(AvailableVisible.begin(), AvailableVisible.end(), [&](LibraryGameCard * a, LibraryGameCard * b){
        const QString ra = RepoOf(a), rb = RepoOf(b);
        if (ra != rb) return ra < rb;
        return keyOf(a) < keyOf(b);
    });

    QVector<LibraryView::Group> groups;
    const int n = AvailableVisible.count();
    for (int s = 0; s < n; ) {
        const QString r = RepoOf(AvailableVisible.at(s));
        int e = s; while (e + 1 < n && RepoOf(AvailableVisible.at(e + 1)) == r) ++e;
        groups.append({ r, s, e, AvailableCollapsedRepos.contains(r) });
        s = e + 1;
    }

    AvailableView->setCards(&AvailableVisible);
    AvailableView->setGroups(groups);
    AvailableView->layoutCards(CardPixelWidth);
}

// Ensure a transfers-table row exists for a CID (creating it with the given status if absent), and return its
// progress item. Columns: Name | Size | Progress | Speed | Status | CID. Used both to pre-show "Queued" CIDs and to
// open an active "Fetching…" row. Kicks an off-thread CidSize fill when the size isn't cached yet.


QString MainWindow::RepoNameForBundle(const std::filesystem::path & BundleDir) const
{
    std::error_code Ec;
    const std::string B = std::filesystem::weakly_canonical(BundleDir, Ec).string();
    for (const std::string & RepoDir : PackageCatalog::RepositoryDirs(*GlobalConfigJSON))
    {
        const std::string R = std::filesystem::weakly_canonical(std::filesystem::path(RepoDir), Ec).string();
        if (!R.empty() && (B == R || B.rfind(R + "/", 0) == 0))
            return QString::fromStdString(std::filesystem::path(RepoDir).filename().string());
    }
    return QStringLiteral("Local");
}

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
        if (Self->AvailableView) Self->RebuildAvailableTab();
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
