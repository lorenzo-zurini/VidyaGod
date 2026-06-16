#include "mainwindow.h"
#include "commonutils.h"
#include "packageeditor.h"
#include "jsonoperations.h"
#include "filesystemoperations.h"
#include "prelaunchwindow.h"
#include "ipfswrapper.h"
#include "runnerwrapper.h"
#include "covercache.h"

#include <thread>

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
#include <QTableWidgetItem>
#include <QTreeWidgetItem>
#include <QStyledItemDelegate>
#include <QStyleOptionProgressBar>
#include <QHBoxLayout>
#include <QVBoxLayout>


// Paints a column's integer value (0..100) as a progress bar, so a transfers table stays sortable (the value
// lives in the item, not a fragile cell widget). A negative value renders an indeterminate "busy" bar.
class ProgressBarDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override
    {
        const int v = idx.data(Qt::DisplayRole).toInt();
        QStyleOptionProgressBar bar;
        bar.rect = opt.rect.adjusted(2, 3, -2, -3);
        bar.minimum = 0; bar.maximum = (v < 0 ? 0 : 100);   // max 0 = indeterminate
        bar.progress = (v < 0 ? 0 : v);
        bar.textVisible = (v >= 0);
        bar.text = (v >= 0) ? QString::number(v) + "%" : QString();
        bar.textAlignment = Qt::AlignCenter;
        QApplication::style()->drawControl(QStyle::CE_ProgressBar, &bar, p);
    }
};


// ═════════════════════════════════════════════════════════════════════════════
// MainWindow
// ═════════════════════════════════════════════════════════════════════════════

MainWindow::MainWindow(nlohmann::ordered_json * gc, QDir * appData, QWidget * parent)
    : GlobalConfigJSON(gc), AppDataDir(appData), QMainWindow(parent)
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
    BuildStaticUI();
    BuildLibraryGameCards();
    BuildLibraryDynamicUI();
    BuildPackagesDynamicUI();
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
    BuildIpfsTab();

    // Only poll IPFS (subprocess-spawning) while its tab is the visible one.
    connect(MainWindowTabWidget, &QTabWidget::currentChanged, this, [this](int){
        const bool OnIpfs = (MainWindowTabWidget->currentWidget() == IpfsTabWidget);
        if (!IpfsRefreshTimer) return;
        if (OnIpfs) { if (!IpfsRefreshTimer->isActive()) IpfsRefreshTimer->start(5000); RefreshIpfsTab(); }
        else        IpfsRefreshTimer->stop();
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

    // Page 3 — Storage & Paths.
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

    //Runners are the ROLE:"runner" nodes of the global catalog graph — one card per runner node.
    std::vector<const Node*> Runners;
    for (const auto & [Id, N] : CatalogIndex.Nodes) { (void)Id; if (N.IsRunner()) Runners.push_back(&N); }
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
                if (!IpfsFetchReady(this)) return;              // need a running daemon to fetch
                btn->setEnabled(false); btn->setText("Importing…");
                st->setText("<span style='color:#c6a15f;'>Importing… (see IPFS tab)</span>");
                std::thread([this, rid]{
                    std::string Err;
                    bool Ok = ContainerWrapper::ImportRunnerNode(*GlobalConfigJSON, CatalogIndex, rid, &Err);
                    QMetaObject::invokeMethod(this, [this, Ok, Err]{
                        if (!Ok) QMessageBox::warning(this, "Runner import failed", QString::fromStdString(Err));
                        RebuildSettingsRunnersPage(); RefreshIpfsTab();
                    }, Qt::QueuedConnection);
                }).detach();
            });
            row->addWidget(btn);
        }
        else if (Ships && !Imported)
        {
            QLabel * need = new QLabel("install Kubo", card);
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
            std::thread([this]{
                PackageCatalog::SyncRepositories(*GlobalConfigJSON);   // git pull each repo + reindex LIBRARY
                QMetaObject::invokeMethod(this, [this]{
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
        // Clone/pull + mirror + index off-thread (network); persist and refresh the pages when done.
        std::thread([this]{
            PackageCatalog::SyncRepositories(*GlobalConfigJSON);   // mutates LIBRARY/RUNNERS
            QMetaObject::invokeMethod(this, [this]{
                SaveGlobalConfigJSON();
                RebuildDynamicUI(); RebuildSettingsReposPage(); RebuildSettingsRunnersPage(); RebuildAvailableTab();
            }, Qt::QueuedConnection);
        }).detach();
    });
    v->addWidget(add);

    v->addStretch(1);
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
// IPFS tab — Kubo transfers + seeded (pinned) content. Kubo is OPTIONAL: when the
// `ipfs` binary is absent the tab shows a greyed install message instead. VidyaGod
// never starts the daemon — it only reports whether the user is running one.
// ═════════════════════════════════════════════════════════════════════════════

// Maps every ipfs SOURCE CID in the catalog to a human label ("<package> — <component>"), so the IPFS tab can
// show what each CID actually is instead of a raw hash. When OutPackages is given, also maps CID → its owning
// package name (used to group the seeded-content list by package).
static QHash<QString, QString> BuildCidLabels(const nlohmann::ordered_json & gc, QHash<QString, QString> * OutPackages = nullptr)
{
    QHash<QString, QString> Labels;
    NodeIndex Idx = PackageCatalog::BuildCatalogIndex(gc);
    for (const auto & [NodeId, N] : Idx.Nodes)
    {
        //A friendly bundle name for grouping: the node's title, else its GROUP/id.
        std::string PkgName = N.Meta.is_object() ? N.Meta.value("TITLE", std::string()) : std::string();
        if (PkgName.empty()) PkgName = N.GroupKey();

        //Content layer CIDs → labelled by their node (the component-equivalent), grouped by bundle.
        if (N.Layers.is_array())
            for (const auto & L : N.Layers)
            {
                if (!ManifestModel::IsVfsLayer(L.value("TYPE", std::string()))) continue;
                if (!L.contains("SOURCE") || !L["SOURCE"].is_object() || L["SOURCE"].value("TYPE", std::string()) != "ipfs") continue;
                const std::string Cid = L["SOURCE"].value("CID", std::string());
                if (Cid.empty()) continue;
                Labels.insert(QString::fromStdString(Cid), QString::fromStdString(NodeId));
                if (OutPackages)
                    OutPackages->insert(QString::fromStdString(Cid), QString::fromStdString(PkgName.empty() ? std::string("(unnamed)") : PkgName));
            }

        //Cover-art CIDs → a single "Assets" group, labelled by bundle.
        if (N.Meta.is_object() && N.Meta.contains("COVER") && N.Meta["COVER"].is_object())
        {
            const auto & Cv = N.Meta["COVER"];
            if (Cv.contains("SOURCE") && Cv["SOURCE"].is_object() && Cv["SOURCE"].value("TYPE", std::string()) == "ipfs")
            {
                const std::string Cid = Cv["SOURCE"].value("CID", std::string());
                if (!Cid.empty())
                {
                    Labels.insert(QString::fromStdString(Cid), QString::fromStdString((PkgName.empty() ? NodeId : PkgName) + " — cover"));
                    if (OutPackages) OutPackages->insert(QString::fromStdString(Cid), QStringLiteral("Assets"));
                }
            }
        }
    }
    return Labels;
}

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

    connect(AvailableView, &LibraryView::downloadRequested, this, &MainWindow::DownloadAvailable);
    connect(AvailableView, &LibraryView::cancelRequested, this, [this](LibraryGameCard * card){
        if (!card) return;
        const QString Key = card->GroupKey;
        if (Key.isEmpty() || !DownloadingUids.contains(Key)) return;
        if (QMessageBox::question(this, "Cancel download?",
                "Stop downloading “" + card->GameTitle + "”?") != QMessageBox::Yes) return;
        CancellingUids.insert(Key);                                   // suppress the failure dialog on abort
        for (const QString & c : DownloadUidCids.value(Key)) IpfsWrapper::RequestCancel(c.toStdString());
    });
    connect(AvailableView, &LibraryView::groupToggled, this, [this](const QString & name, bool collapsed){
        if (collapsed) AvailableCollapsedRepos.insert(name); else AvailableCollapsedRepos.remove(name);
        auto & arr = (*GlobalConfigJSON)["Settings"]["AvailableCollapsedRepos"] = nlohmann::ordered_json::array();
        for (const QString & R : AvailableCollapsedRepos) arr.push_back(R.toStdString());
        SaveGlobalConfigJSON();
    });

    // Live download progress: average a package's content-CID transfer percents onto its Available card(s).
    auto applyProgress = [this](QString cid, double pct){
        auto it = DownloadCidToUid.constFind(cid);
        if (it == DownloadCidToUid.constEnd()) return;
        const QString Key = it.value();
        if (pct >= 0) DownloadCidPct[cid] = pct;
        const QStringList & Cids = DownloadUidCids[Key];
        double Sum = 0; for (const QString & c : Cids) Sum += DownloadCidPct.value(c, 0.0);
        const double Avg = Cids.isEmpty() ? -1.0 : Sum / Cids.size();
        bool Any = false;
        for (LibraryGameCard * card : *AvailableGameCards)
            if (card && card->Downloading && card->GroupKey == Key)
            { card->DownloadPercent = Avg; Any = true; }
        if (Any && AvailableView) AvailableView->refreshVisuals();
    };
    connect(IpfsManager::instance(), &IpfsManager::transferProgress, this,
            [applyProgress](QString cid, double pct){ applyProgress(cid, pct); });
    connect(IpfsManager::instance(), &IpfsManager::transferFinished, this,
            [applyProgress](QString cid, bool){ applyProgress(cid, 100.0); });

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

        const bool Busy = DownloadingUids.contains(c->GroupKey);
        double BusyPct = -1.0;                                                    // carry current progress across rebuilds
        if (Busy) { const QStringList & cs = DownloadUidCids.value(c->GroupKey);
                    if (!cs.isEmpty()) { double s = 0; for (const QString & cc : cs) s += DownloadCidPct.value(cc, 0.0); BusyPct = s / cs.size(); } }
        c->Downloading = Busy;
        c->DownloadPercent = BusyPct;
        AvailableGameCards->append(c);
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
    for (LibraryGameCard * c : *AvailableGameCards)
        if (AvailableSearch.isEmpty() || c->GameTitle.contains(AvailableSearch, Qt::CaseInsensitive))
            AvailableVisible.append(c);

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

//Hydrates the package behind an Available card over IPFS (in place, on a worker), then moves it to the Library.
void MainWindow::DownloadAvailable(LibraryGameCard * card)
{
    if (!card) return;
    if (!IpfsFetchReady(this)) return;
    const QString Key = card->GroupKey;
    if (Key.isEmpty() || DownloadingUids.contains(Key)) return;       // already in flight

    // Hydrate every edition in the tile's group; gather the union of their content CIDs for progress tracking.
    const std::vector<std::string> LaunchIds = card->GroupNodeIds;

    DownloadingUids.insert(Key);
    QStringList Cids;
    for (const std::string & Lid : LaunchIds)
        for (const auto & C : PackageCatalog::NodeContentCids(CatalogIndex, Lid))
        {
            const QString Qc = QString::fromStdString(C);
            if (DownloadCidToUid.contains(Qc)) continue;
            Cids << Qc; DownloadCidToUid[Qc] = Key; DownloadCidPct[Qc] = 0.0;
        }
    DownloadUidCids[Key] = Cids;

    // Mark this tile's card(s) "Downloading…" in place (cheap — no pool rebuild / no filesystem restat).
    for (LibraryGameCard * c : *AvailableGameCards)
        if (c && c->GroupKey == Key) { c->Downloading = true; c->DownloadPercent = 0.0; }
    AvailableView->refreshVisuals();
    RefreshIpfsTab();

    // Snapshot the node index for THIS worker: concurrent downloads must not read the shared CatalogIndex while
    // a completing download reassigns it (RebuildDynamicUI) — that was the multi-download crash.
    NodeIndex Snapshot = CatalogIndex;
    std::thread([this, LaunchIds, Key, Snapshot = std::move(Snapshot)]{
        std::string Err; bool Ok = true;
        for (const std::string & Lid : LaunchIds)
            if (!PackageCatalog::HydrateNode(Snapshot, Lid, &Err)) { Ok = false; break; }
        QMetaObject::invokeMethod(this, [this, Ok, Err, Key]{
            DownloadingUids.remove(Key);
            const bool Cancelled = CancellingUids.remove(Key);
            for (const QString & c : DownloadUidCids.value(Key))
            { IpfsWrapper::ClearCancel(c.toStdString()); DownloadCidToUid.remove(c); DownloadCidPct.remove(c); }
            DownloadUidCids.remove(Key);
            if (Ok) RebuildDynamicUI();
            else if (!Cancelled) QMessageBox::warning(this, "Download failed", QString::fromStdString(Err));
            RebuildAvailableTab(); RefreshIpfsTab();
        }, Qt::QueuedConnection);
    }).detach();
}

void MainWindow::BuildIpfsTab()
{
    IpfsTabWidget = new QWidget(MainWindowTabWidget);
    QVBoxLayout * v = new QVBoxLayout(IpfsTabWidget);
    v->setContentsMargins(12,12,12,12);
    MainWindowTabWidget->addTab(IpfsTabWidget, "IPFS");

    if (!IpfsWrapper::Available())
    {
        QLabel * msg = new QLabel(
            "<div style='color:#8f98a0;'><b>IPFS is unavailable</b><br><br>"
            "Please install <b>Kubo</b> (the IPFS implementation) and make sure <code>ipfs</code> is on your "
            "PATH to enable IPFS functionality.<br><br>"
            "Run <code>ipfs daemon</code> afterwards to fetch and seed shared packages.</div>", IpfsTabWidget);
        msg->setAlignment(Qt::AlignCenter); msg->setWordWrap(true);
        v->addStretch(1); v->addWidget(msg); v->addStretch(1);
        return;                                              // greyed/empty — no controls
    }

    // Status row.
    QHBoxLayout * statusRow = new QHBoxLayout();
    IpfsStatusLabel = new QLabel(QStringLiteral("…"), IpfsTabWidget);
    QPushButton * refreshBtn = new QPushButton("Refresh", IpfsTabWidget);
    connect(refreshBtn, &QPushButton::clicked, this, &MainWindow::RefreshIpfsTab);
    statusRow->addWidget(IpfsStatusLabel, 1);
    statusRow->addWidget(refreshBtn);
    v->addLayout(statusRow);

    // Build the CID → label + CID → package indexes up front (label for the table, package for grouping).
    IpfsCidLabels = BuildCidLabels(*GlobalConfigJSON, &IpfsCidPackages);

    // Transfers (live fetches). Name | CID | Progress | Status — sortable; progress lives in the item and is
    // delegate-drawn (no cell widget), so re-sorting can't desync it.
    QGroupBox * txBox = new QGroupBox("Transfers", IpfsTabWidget);
    QVBoxLayout * txl = new QVBoxLayout(txBox);
    IpfsTransfers = new QTableWidget(0, 4, txBox);
    IpfsTransfers->setHorizontalHeaderLabels({"Name", "CID", "Progress", "Status"});
    IpfsTransfers->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    IpfsTransfers->setItemDelegateForColumn(2, new ProgressBarDelegate(IpfsTransfers));
    IpfsTransfers->setEditTriggers(QAbstractItemView::NoEditTriggers);
    IpfsTransfers->setSelectionMode(QAbstractItemView::NoSelection);
    IpfsTransfers->verticalHeader()->setVisible(false);
    IpfsTransfers->setSortingEnabled(true);
    IpfsTransfers->sortByColumn(0, Qt::AscendingOrder);
    txl->addWidget(IpfsTransfers);
    v->addWidget(txBox, 1);

    // Seeded content (pinned CIDs), grouped by package and sortable (click a header to sort within groups).
    QGroupBox * pinBox = new QGroupBox("Seeded content (pinned)", IpfsTabWidget);
    QVBoxLayout * pl = new QVBoxLayout(pinBox);
    IpfsPins = new QTreeWidget(pinBox);
    IpfsPins->setColumnCount(3);
    IpfsPins->setHeaderLabels({"Name", "CID", ""});
    IpfsPins->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    IpfsPins->setEditTriggers(QAbstractItemView::NoEditTriggers);
    IpfsPins->setSelectionMode(QAbstractItemView::NoSelection);
    IpfsPins->setSortingEnabled(true);
    IpfsPins->sortByColumn(0, Qt::AscendingOrder);
    pl->addWidget(IpfsPins);
    v->addWidget(pinBox, 1);

    // Live transfer notifications (marshalled onto the GUI thread by IpfsManager). Each CID's progress item
    // pointer is tracked (it survives re-sorting); the live row is derived from it on demand.
    IpfsManager * mgr = IpfsManager::instance();
    connect(mgr, &IpfsManager::transferStarted, this, [this](QString cid) {
        if (!IpfsTransfers) return;
        QTableWidgetItem * prog = IpfsTransferProgress.value(cid, nullptr);
        if (!prog) {
            if (!IpfsCidLabels.contains(cid)) IpfsCidLabels = BuildCidLabels(*GlobalConfigJSON, &IpfsCidPackages);
            IpfsTransfers->setSortingEnabled(false);          // keep the row intact while we fill it
            const int row = IpfsTransfers->rowCount(); IpfsTransfers->insertRow(row);
            IpfsTransfers->setItem(row, 0, new QTableWidgetItem(IpfsCidLabels.value(cid, QStringLiteral("(unknown)"))));
            IpfsTransfers->setItem(row, 1, new QTableWidgetItem(cid));
            prog = new QTableWidgetItem(); prog->setData(Qt::DisplayRole, -1);   // indeterminate until first %
            IpfsTransfers->setItem(row, 2, prog);
            IpfsTransfers->setItem(row, 3, new QTableWidgetItem("Fetching…"));
            IpfsTransfers->setSortingEnabled(true);
            IpfsTransferProgress.insert(cid, prog);
        } else {
            const int row = IpfsTransfers->row(prog);
            if (row >= 0 && IpfsTransfers->item(row, 3)) IpfsTransfers->item(row, 3)->setText("Fetching…");
        }
    });
    connect(mgr, &IpfsManager::transferProgress, this, [this](QString cid, double pct) {
        if (QTableWidgetItem * prog = IpfsTransferProgress.value(cid, nullptr))
            prog->setData(Qt::DisplayRole, int(pct + 0.5));
    });
    connect(mgr, &IpfsManager::transferFinished, this, [this](QString cid, bool ok) {
        if (QTableWidgetItem * prog = IpfsTransferProgress.value(cid, nullptr)) {
            prog->setData(Qt::DisplayRole, ok ? 100 : prog->data(Qt::DisplayRole).toInt());
            const int row = IpfsTransfers->row(prog);
            if (row >= 0 && IpfsTransfers->item(row, 3)) IpfsTransfers->item(row, 3)->setText(ok ? "Done" : "Failed");
        }
        RefreshIpfsTab();                                    // a finished fetch likely added a pin
    });

    // Periodic status/pin refresh — only while the IPFS tab is visible (each tick spawns ipfs subprocesses;
    // no point paying that when the user isn't looking). Started/stopped by the tab-change handler in BuildStaticUI.
    IpfsRefreshTimer = new QTimer(this);
    connect(IpfsRefreshTimer, &QTimer::timeout, this, &MainWindow::RefreshIpfsTab);
    if (MainWindowTabWidget && MainWindowTabWidget->currentWidget() == IpfsTabWidget)
    { IpfsRefreshTimer->start(5000); RefreshIpfsTab(); }
}

void MainWindow::RefreshIpfsTab()
{
    if (!IpfsWrapper::Available() || !IpfsStatusLabel) return;
    if (IpfsRefreshInFlight) return;                                       // a gather is already running
    IpfsRefreshInFlight = true;

    // Gather the (subprocess-spawning) ipfs status + pins OFF the GUI thread, then apply on the GUI thread.
    std::thread([this]{
        const bool   Daemon = IpfsWrapper::DaemonRunning();
        const int    Peers  = Daemon ? IpfsWrapper::PeerCount() : 0;
        const QString Repo  = QString::fromStdString(IpfsWrapper::RepoSizeHuman());
        const std::vector<IpfsWrapper::PinEntry> Pins = IpfsWrapper::Pins();
        QMetaObject::invokeMethod(this, [this, Daemon, Peers, Repo, Pins]{
            IpfsRefreshInFlight = false;
            ApplyIpfsSnapshot(Daemon, Peers, Repo, Pins);
        }, Qt::QueuedConnection);
    }).detach();
}

// Paints the gathered IPFS status + grouped pins onto the IPFS tab (GUI thread only).
void MainWindow::ApplyIpfsSnapshot(bool Daemon, int Peers, const QString & Repo,
                                   const std::vector<IpfsWrapper::PinEntry> & Pins)
{
    if (!IpfsStatusLabel) return;
    QString S = QString("Daemon: %1").arg(Daemon
        ? "<span style='color:#5fb55f;'>running</span>"
        : "<span style='color:#c0726a;'>stopped — run <code>ipfs daemon</code> to seed</span>");
    if (Daemon) S += QString("   •   Peers: %1").arg(Peers);
    if (!Repo.isEmpty()) S += QString("   •   Repo: %1").arg(Repo);
    IpfsStatusLabel->setText(S);

    if (!IpfsPins) return;
    IpfsCidLabels = BuildCidLabels(*GlobalConfigJSON, &IpfsCidPackages);   // keep names current with the catalog

    // Group the pinned CIDs by owning package (QMap → keys already alphabetical; unknowns bucketed last).
    QMap<QString, QStringList> ByPackage;
    const QString Unknown = QStringLiteral("Unknown / not in your library");
    for (const auto & P : Pins)
    {
        const QString Cid = QString::fromStdString(P.Cid);
        ByPackage[IpfsCidPackages.value(Cid, Unknown)].append(Cid);
    }

    IpfsPins->setSortingEnabled(false);                                    // batch-build, then let the header sort
    IpfsPins->clear();
    for (auto it = ByPackage.constBegin(); it != ByPackage.constEnd(); ++it)
    {
        const QString & PkgName = it.key();
        QTreeWidgetItem * grp = new QTreeWidgetItem(IpfsPins);
        grp->setText(0, QString("%1   (%2)").arg(PkgName).arg(it.value().size()));
        QFont f = grp->font(0); f.setBold(true); grp->setFont(0, f);
        grp->setFlags(grp->flags() & ~Qt::ItemIsSelectable);
        for (const QString & Cid : it.value())
        {
            const QString Label = IpfsCidLabels.value(Cid, QStringLiteral("(unknown)"));
            QString Leaf = Label;                                          // child shows the layer, not the package
            if (Label.startsWith(PkgName + " — ")) Leaf = Label.mid(PkgName.size() + 3);
            else if (Label == PkgName)             Leaf = QStringLiteral("content");
            QTreeWidgetItem * child = new QTreeWidgetItem(grp);
            child->setText(0, Leaf);
            child->setText(1, Cid);
            QWidget * cell = new QWidget();
            QHBoxLayout * cl = new QHBoxLayout(cell); cl->setContentsMargins(2,1,2,1); cl->setSpacing(4);
            QPushButton * copyBtn  = new QPushButton("Copy CID", cell);
            QPushButton * unpinBtn = new QPushButton("Unpin", cell);
            connect(copyBtn,  &QPushButton::clicked, this, [Cid]{ QApplication::clipboard()->setText(Cid); });
            connect(unpinBtn, &QPushButton::clicked, this, [this, Cid]{ IpfsWrapper::Unpin(Cid.toStdString()); RefreshIpfsTab(); });
            cl->addWidget(copyBtn); cl->addWidget(unpinBtn); cl->addStretch();
            IpfsPins->setItemWidget(child, 2, cell);
        }
    }
    IpfsPins->setSortingEnabled(true);
    IpfsPins->expandAll();
    IpfsPins->resizeColumnToContents(1);
    IpfsPins->resizeColumnToContents(2);
}

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
