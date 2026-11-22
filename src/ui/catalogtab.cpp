#include "catalogtab.h"
#include "cardpool.h"
#include "theme.h"   // the named colours
#include "cardtoolbar.h"
#include "offlinebanner.h"
#include "installed.h"
#include "shelf.h"
#include "appmodel.h"          // AppModel — catalog/config + card size
#include "covercache.h"        // re-take covers when the cards grow
#include "packagecatalog.h"
#include "manifestmodel.h"
#include "commonutils.h"

#include <QRegularExpression>
#include <QVBoxLayout>
#include <chrono>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QButtonGroup>
#include <QDialog>
#include <QScrollArea>
#include <QFrame>
#include <QLabel>
#include <QMessageBox>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

CatalogTab::CatalogTab(AppModel &model, QWidget *parent)
    : QWidget(parent), Model(model)
{
    buildUi();   // calls rebuild() once
    connect(&Model, &AppModel::catalogChanged,  this, &CatalogTab::rebuild);
    connect(&Model, &AppModel::coversReady,     this, &CatalogTab::refreshCovers);
    connect(&Model, &AppModel::cardSizeChanged, this, &CatalogTab::onCardSizeChanged);
    connect(CoverCache::instance(), &CoverCache::coversTooSmall, this, [this] {   // wider cards: decode the covers again
        for (LibraryGameCard * Card : *AvailableGameCards) if (Card && !Card->CoverOriginal.isNull()) Card->CoverStale = true;
        refreshCovers();
    });
}

// Download-state hooks (driven by DownloadManager signals) — paint download state onto this tab's cards and track
// the in-flight set so a rebuild() can restore the overlay without querying anyone.
CatalogTab::~CatalogTab()
{
    qDeleteAll(*AvailableGameCards);
    delete AvailableGameCards;
}

void CatalogTab::markDownloading(const QString &groupKey)
{
    ActiveDownloads[groupKey] = 0.0;
    for (LibraryGameCard * c : *AvailableGameCards)
        if (c && c->GameKey == groupKey) { c->Downloading = true; c->DownloadPercent = 0.0; }
    if (AvailableView) AvailableView->repaintCards();   // overlay only — must NOT re-scale covers (fires per progress tick)
}
void CatalogTab::setDownloadProgress(const QString &groupKey, double avg)
{
    if (ActiveDownloads.contains(groupKey)) ActiveDownloads[groupKey] = avg;
    bool any = false;
    for (LibraryGameCard * c : *AvailableGameCards)
        if (c && c->Downloading && c->GameKey == groupKey) { c->DownloadPercent = avg; any = true; }
    if (any && AvailableView) AvailableView->repaintCards();   // overlay only — re-scaling every cover here pegged the GUI
}
void CatalogTab::clearDownloading(const QString &groupKey)
{
    ActiveDownloads.remove(groupKey);
    for (LibraryGameCard * c : *AvailableGameCards)
        if (c && c->GameKey == groupKey) { c->Downloading = false; c->DownloadPercent = -1.0; }
}
void CatalogTab::refreshCovers()
{
    bool Any = false;
    for (LibraryGameCard * Card : *AvailableGameCards)
        if (Card && (Card->CoverOriginal.isNull() || Card->CoverStale)) { Card->InitializeClassVariables(); Any = true; }
    if (Any && AvailableView) AvailableView->refreshVisuals();
}

void CatalogTab::buildUi()
{
    // Load persisted collapse state for repo sections.
    AvailableCollapsedRepos = LoadCollapsed(Model, "AvailableCollapsedRepos");

    QVBoxLayout * v = new QVBoxLayout(this);
    v->setContentsMargins(0,0,0,0); v->setSpacing(0);
    v->addWidget(new OfflineBanner(Model, "Networking is off — what is listed here can be browsed; downloads wait until it is on.", this));

    // Sort (within each repo) + search + size: the toolbar both card tabs share; then this tab's own button.
    const CardToolbar Bar = MakeCardToolbar(this, Model,
        { {"Name", SortMode::Name}, {"Date", SortMode::Date}, {"Series", SortMode::Series} }, AvailableSort,
        [this](SortMode Mode) { AvailableSort = Mode; applyFilter(); },        // cheap: re-sort the existing pool (no restat)
        [this](const QString &Text) { AvailableSearch = Text; applyFilter(); });   // cheap: re-filter the pool (no restat)
    QWidget * toolbar = Bar.Bar;
    QHBoxLayout * tl = Bar.Row;
    // Add a package source by its IPFS folder CID — its games drop straight into this catalog.
    QPushButton * cidBtn = new QPushButton("Add CID", toolbar);
    connect(cidBtn, &QPushButton::clicked, this, [this]{ openPackageSourcesDialog(); });
    tl->addWidget(cidBtn);
    v->addWidget(toolbar);

    AvailableView = new LibraryView(this);
    AvailableView->setAccessibleName("Catalog games");
    AvailableView->setHoverAction("⬇  Download", true);
    AvailableView->setEmptyMessage("Nothing to download.\n\nHit “Add CID” above (or add a source in Settings → Sources) to see shared games here.");
    v->addWidget(AvailableView);

    // Card clicks are re-emitted as requests; MainWindow wires them to the DownloadManager (no direct coupling).
    connect(AvailableView, &LibraryView::downloadRequested, this, &CatalogTab::downloadRequested);
    connect(AvailableView, &LibraryView::cancelRequested,   this, &CatalogTab::cancelRequested);
    connect(AvailableView, &LibraryView::groupToggled, this, [this](const QString & name, bool collapsed){
        if (collapsed) AvailableCollapsedRepos.insert(name); else AvailableCollapsedRepos.remove(name);
        SaveCollapsed(Model, "AvailableCollapsedRepos", AvailableCollapsedRepos);
    });

    rebuild();
}

// Model card-size changed (from either tab's size buttons) — re-check the matching button and re-scale + relayout.
void CatalogTab::onCardSizeChanged(int cardW)
{
    SyncSizeButtons(this, cardW);
    if (AvailableView) {
        AvailableView->setCards(AvailableGameCards);   // point at the full pool so every cover is re-scaled
        AvailableView->prescaleCovers(Model.cardPixelWidth());
        applyFilter();                                 // repoint to the filtered subset + relayout
    }
}

//(Re)builds the Available grid: one card per un-hydrated repo game, grouped by repository (collapsible).
//EXPENSIVE — rebuilds the full Available card POOL: enumerate presentable groups + check hydration (filesystem
//stats + graph walks) and create one card per un-hydrated, fetchable tile. Call only when the catalog actually
//changes (sync / import / download complete), NOT on every sort/search keystroke — those call applyFilter.
void CatalogTab::rebuild()
{
    if (!AvailableView) return;
    const auto Started = std::chrono::steady_clock::now();
    AvailableVisible.clear();
    qDeleteAll(*AvailableGameCards); AvailableGameCards->clear();

    *AvailableGameCards = CardPool::Available(Model);
    for (LibraryGameCard * c : *AvailableGameCards)
    {
        const bool Busy = ActiveDownloads.contains(c->GameKey);
        c->Downloading = Busy;
        c->DownloadPercent = Busy ? ActiveDownloads.value(c->GameKey) : -1.0;   // carry current progress across rebuilds
    }

    // Point the view at the full pool, then scale every cover ONCE — BEFORE applyFilter shows them.
    // (Scaling must happen while the view's card list IS the pool; otherwise CoverScaled stays null and the
    // covers paint black until the next refreshVisuals — the "flash black on download complete" bug.)
    AvailableView->setCards(AvailableGameCards);
    AvailableView->prescaleCovers(Model.cardPixelWidth());
    applyFilter();
    LogOut("CatalogTab", "Catalog shows " + std::to_string(AvailableGameCards->size()) + " downloadable game(s) (built in "
           + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - Started).count())
           + " ms).");
}

//CHEAP — re-filters the existing pool by search, re-sorts + re-groups, and shows it. No card recreation, no
//filesystem stats, no cover re-scaling. This is the sort/search path.
void CatalogTab::applyFilter()
{
    if (!AvailableView) return;

    auto RepoOf = [this](LibraryGameCard * c) -> QString { return repoNameForBundle(c->PackagePath); };

    AvailableVisible.clear();
    for (LibraryGameCard * c : *AvailableGameCards)
        if (!c->IsSecondary && CardPool::Matches(*c, AvailableSearch))   // secondaries travel with their main tile
            AvailableVisible.append(c);

    // Sort by (repo, within-group key) so each repo is a contiguous run; then build collapsible groups.
    CardPool::SortBySection(AvailableVisible, AvailableSort, *Model.config());

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
    AvailableView->layoutCards(Model.cardPixelWidth());
}

// Ensure a transfers-table row exists for a CID (creating it with the given status if absent), and return its
// progress item. Columns: Name | Size | Progress | Speed | Status | CID. Used both to pre-show "Queued" CIDs and to
// open an active "Fetching…" row. Kicks an off-thread CidSize fill when the size isn't cached yet.


QString CatalogTab::repoNameForBundle(const std::filesystem::path & BundleDir) const
{
    return CardPool::SectionOf(*Model.config(), BundleDir);
}

// "Add CID" — manage IPFS folder-CID package sources (add/remove). The list rebuilds on packageSourcesChanged, which
// AppModel::addPackageSource now emits IMMEDIATELY (before the off-thread fetch), so an added CID appears instantly.
void CatalogTab::openPackageSourcesDialog()
{
    auto * dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle("Package sources (IPFS CID)");
    dlg->resize(560, 420);
    auto * root = new QVBoxLayout(dlg);

    auto * info = new QLabel(QStringLiteral(
        "A package source is an IPFS folder CID of dehydrated packages (node manifests only — covers and content "
        "stream in on demand). Adding one fetches the manifests and lists its games in this catalog, downloadable on "
        "demand. Requires IPFS networking to be on."), dlg);
    info->setWordWrap(true); info->setStyleSheet("color:" + Theme::Dim + ";font-size:9pt;");
    root->addWidget(info);

    auto * listHost = new QWidget(dlg);
    auto * listLay  = new QVBoxLayout(listHost); listLay->setContentsMargins(0, 0, 0, 0);
    auto * scroll   = new QScrollArea(dlg); scroll->setWidgetResizable(true); scroll->setWidget(listHost);
    root->addWidget(scroll, 1);

    auto * addRow  = new QHBoxLayout();
    auto * cidEdit = new QLineEdit(dlg); cidEdit->setPlaceholderText("Folder CID (Qm… / bafy…)");
    auto * nameEdit= new QLineEdit(dlg); nameEdit->setPlaceholderText("Name (optional)"); nameEdit->setMaximumWidth(160);
    auto * add2    = new QPushButton("Add", dlg);
    addRow->addWidget(cidEdit, 1); addRow->addWidget(nameEdit); addRow->addWidget(add2);
    root->addLayout(addRow);

    auto rebuild = [this, listHost, listLay]() {
        QLayoutItem * it;
        while ((it = listLay->takeAt(0)) != nullptr) { if (it->widget()) it->widget()->deleteLater(); delete it; }
        const auto * cfg = Model.config();
        const bool has = cfg->contains("Settings") && (*cfg)["Settings"].is_object()
                         && (*cfg)["Settings"].contains("PackageSources") && (*cfg)["Settings"]["PackageSources"].is_array()
                         && !(*cfg)["Settings"]["PackageSources"].empty();
        if (!has) { listLay->addWidget(new QLabel("No package sources.", listHost)); listLay->addStretch(); return; }
        int i = 0;
        for (const auto & Src : (*cfg)["Settings"]["PackageSources"])
        {
            const QString cid  = QString::fromStdString(Src.is_object() ? Src.value("CID", std::string())
                                                        : (Src.is_string() ? std::string(Src) : std::string()));
            const QString name = QString::fromStdString(Src.is_object() ? Src.value("NAME", std::string()) : std::string());
            auto * card = new QFrame(listHost); card->setFrameShape(QFrame::StyledPanel);
            auto * cl   = new QHBoxLayout(card);
            cl->addWidget(new QLabel(name.isEmpty() ? cid : (name + "  —  " + cid), card), 1);
            auto * rm = new QPushButton("Remove", card);
            connect(rm, &QPushButton::clicked, this, [this, i]{ Model.removePackageSource(i); });
            cl->addWidget(rm);
            listLay->addWidget(card);
            ++i;
        }
        listLay->addStretch();
    };
    rebuild();

    connect(add2, &QPushButton::clicked, dlg, [this, dlg, cidEdit, nameEdit]{
        if (!Model.addPackageSource(cidEdit->text(), nameEdit->text()))
        { QMessageBox::warning(dlg, "Add CID", "Enter a folder CID that isn't already added."); return; }
        cidEdit->clear(); nameEdit->clear();   // appears in the list instantly; the fetch fills it in off-thread
    });
    connect(&Model, &AppModel::packageSourcesChanged, dlg, [rebuild]{ rebuild(); });
    connect(&Model, &AppModel::packageSourceFailed, dlg, [dlg](const QString & m){ QMessageBox::warning(dlg, "Package source", m); });

    dlg->show();
}
