#include "catalogtab.h"
#include "downloadmanager.h"   // Mw.DownloadMgr->{startDownload,requestCancel,isDownloading,busyPercent,applyProgress}
#include "packagecatalog.h"
#include "manifestmodel.h"
#include "ipfswrapper.h"       // IpfsManager
#include "commonutils.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QButtonGroup>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

CatalogTab::CatalogTab(MainWindow &Owner, QWidget *parent)
    : QWidget(parent), Mw(Owner) { buildUi(); }

// DownloadManager hooks — paint download state onto this tab's cards (the cards live here now).
void CatalogTab::markDownloading(const QString &groupKey)
{
    for (LibraryGameCard * c : *AvailableGameCards)
        if (c && c->GroupKey == groupKey) { c->Downloading = true; c->DownloadPercent = 0.0; }
    if (AvailableView) AvailableView->refreshVisuals();
}
void CatalogTab::setDownloadProgress(const QString &groupKey, double avg)
{
    bool any = false;
    for (LibraryGameCard * c : *AvailableGameCards)
        if (c && c->Downloading && c->GroupKey == groupKey) { c->DownloadPercent = avg; any = true; }
    if (any && AvailableView) AvailableView->refreshVisuals();
}
void CatalogTab::refreshCovers()
{
    bool Any = false;
    for (LibraryGameCard * Card : *AvailableGameCards)
        if (Card && Card->CoverOriginal.isNull()) { Card->InitializeClassVariables(); Any = true; }
    if (Any && AvailableView) AvailableView->refreshVisuals();
}

void CatalogTab::buildUi()
{
    // Load persisted collapse state for repo sections.
    AvailableCollapsedRepos.clear();
    if ((*Mw.GlobalConfigJSON)["Settings"].contains("AvailableCollapsedRepos")
        && (*Mw.GlobalConfigJSON)["Settings"]["AvailableCollapsedRepos"].is_array())
        for (const auto & R : (*Mw.GlobalConfigJSON)["Settings"]["AvailableCollapsedRepos"])
            if (R.is_string()) AvailableCollapsedRepos.insert(QString::fromStdString(std::string(R)));

    QVBoxLayout * v = new QVBoxLayout(this);
    v->setContentsMargins(0,0,0,0); v->setSpacing(0);

    // Toolbar — mirrors the Library tab: Name/Date/Series sort (within each repo) + size picker.
    QWidget * toolbar = new QWidget(this);
    QHBoxLayout * tl = new QHBoxLayout(toolbar);
    tl->setContentsMargins(8,4,8,4);

    const QString sortBtnStyle =
        "QPushButton{background:transparent;border:none;font-size:9pt;padding:2px 8px;}"
        "QPushButton:checked{border-bottom:2px solid palette(highlight);font-weight:bold;}"
        "QPushButton:hover{color:palette(highlighted-text);}";
    QButtonGroup * sortGroup = new QButtonGroup(toolbar);
    sortGroup->setExclusive(true);
    auto makeSortBtn = [&](const QString & lbl, MainWindow::SortMode mode) {
        QPushButton * b = new QPushButton(lbl, toolbar);
        b->setCheckable(true); b->setChecked(AvailableSort == mode);
        b->setStyleSheet(sortBtnStyle);
        sortGroup->addButton(b);
        connect(b, &QPushButton::toggled, this, [this, mode](bool checked){
            if (!checked) return;
            AvailableSort = mode; applyFilter();   // cheap: re-sort the existing pool (no restat)
        });
        tl->addWidget(b);
    };
    makeSortBtn("Name", MainWindow::SortMode::Name); makeSortBtn("Date", MainWindow::SortMode::Date); makeSortBtn("Series", MainWindow::SortMode::Series);
    tl->addStretch();
    QLineEdit * availSearch = new QLineEdit(toolbar);
    availSearch->setPlaceholderText("Search…");
    availSearch->setClearButtonEnabled(true);
    availSearch->setFixedWidth(180);
    connect(availSearch, &QLineEdit::textChanged, this, [this](const QString & t){
        AvailableSearch = t.trimmed();
        applyFilter();                             // cheap: re-filter the existing pool (no restat)
    });
    tl->addWidget(availSearch);
    auto makeSizeBtn = [&](const QString & lbl, int w) {
        QPushButton * b = new QPushButton(lbl, toolbar);
        b->setCheckable(true); b->setChecked(Mw.CardPixelWidth == w);
        b->setStyleSheet(
            "QPushButton{color:#8f98a0;background:transparent;border:none;font-size:9pt;padding:2px 8px;}"
            "QPushButton:checked{color:#c6d4df;border-bottom:2px solid #4a90d9;}"
            "QPushButton:hover{color:#c6d4df;}");
        connect(b, &QPushButton::clicked, this, [this,w,lbl,toolbar](){
            Mw.CardPixelWidth = w;
            for (auto * x : toolbar->findChildren<QPushButton*>()) if (x->isCheckable() && (x->text()=="Large"||x->text()=="Medium"||x->text()=="Small")) x->setChecked(x->text()==lbl);
            if (AvailableView) {
                AvailableView->setCards(AvailableGameCards);   // point at the full pool so every cover is re-scaled
                AvailableView->prescaleCovers(Mw.CardPixelWidth);
                applyFilter();                        // repoint to the filtered subset + relayout
            }
        });
        tl->addWidget(b);
    };
    makeSizeBtn("Large",250); makeSizeBtn("Medium",185); makeSizeBtn("Small",120);
    v->addWidget(toolbar);

    AvailableView = new LibraryView(this);
    AvailableView->setHoverAction("⬇  Download", true);
    AvailableView->setEmptyMessage("Nothing to download.\n\nAdd a repository in Settings → Repositories (or hit “Sync now”) to see shared games here.");
    v->addWidget(AvailableView);

    connect(AvailableView, &LibraryView::downloadRequested, this, [this](LibraryGameCard * card){ Mw.DownloadMgr->startDownload(card); });
    connect(AvailableView, &LibraryView::cancelRequested,   this, [this](LibraryGameCard * card){ Mw.DownloadMgr->requestCancel(card); });
    connect(AvailableView, &LibraryView::groupToggled, this, [this](const QString & name, bool collapsed){
        if (collapsed) AvailableCollapsedRepos.insert(name); else AvailableCollapsedRepos.remove(name);
        auto & arr = (*Mw.GlobalConfigJSON)["Settings"]["AvailableCollapsedRepos"] = nlohmann::ordered_json::array();
        for (const QString & R : AvailableCollapsedRepos) arr.push_back(R.toStdString());
        Mw.SaveGlobalConfigJSON();
    });

    // Live download progress: average a package's content-CID transfer percents onto its Available card(s).
    connect(IpfsManager::instance(), &IpfsManager::transferProgress, this,
            [this](QString cid, double pct){ Mw.DownloadMgr->applyProgress(cid, pct); });
    connect(IpfsManager::instance(), &IpfsManager::transferFinished, this,
            [this](QString cid, bool, QString){ Mw.DownloadMgr->applyProgress(cid, 100.0); });

    rebuild();
}

//(Re)builds the Available grid: one card per un-hydrated repo game, grouped by repository (collapsible).
//EXPENSIVE — rebuilds the full Available card POOL: enumerate presentable groups + check hydration (filesystem
//stats + graph walks) and create one card per un-hydrated, fetchable tile. Call only when the catalog actually
//changes (sync / import / download complete), NOT on every sort/search keystroke — those call applyFilter.
void CatalogTab::rebuild()
{
    if (!AvailableView) return;
    AvailableVisible.clear();
    qDeleteAll(*AvailableGameCards); AvailableGameCards->clear();

    //One card per un-hydrated presentable group: at least one edition's content is still missing AND fetchable
    //over IPFS. (A fully-hydrated group lives in the Library tab.)
    for (const std::vector<const Node*> & Group : PackageCatalog::PresentableGroups(Mw.CatalogIndex))
    {
        std::vector<std::string> Ids;
        bool AnyMissing = false, AnyFetchable = false;
        for (const Node * N : Group)
        {
            Ids.push_back(N->NodeId);
            if (!PackageCatalog::NodeHydrated(Mw.CatalogIndex, N->NodeId)) AnyMissing = true;
            if (!PackageCatalog::NodeContentCids(Mw.CatalogIndex, N->NodeId).empty()) AnyFetchable = true;
        }
        if (!AnyMissing || !AnyFetchable || Ids.empty()) continue;
        auto * c = new LibraryGameCard(Mw.GlobalConfigJSON, &Mw.CatalogIndex, std::move(Ids));
        c->InitializeClassVariables();

        const bool Busy = Mw.DownloadMgr->isDownloading(c->GroupKey);
        c->Downloading = Busy;
        c->DownloadPercent = Busy ? Mw.DownloadMgr->busyPercent(c->GroupKey) : -1.0;   // carry current progress across rebuilds
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
        for (const std::string & Nid : ManifestModel::ResolveNodeOrder(Mw.CatalogIndex, Lid, {})) {
            const Node * N = Mw.CatalogIndex.Find(Nid);
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

    // Point the view at the full pool, then scale every cover ONCE — BEFORE applyFilter shows them.
    // (Scaling must happen while the view's card list IS the pool; otherwise CoverScaled stays null and the
    // covers paint black until the next refreshVisuals — the "flash black on download complete" bug.)
    AvailableView->setCards(AvailableGameCards);
    AvailableView->prescaleCovers(Mw.CardPixelWidth);
    applyFilter();
}

//CHEAP — re-filters the existing pool by search, re-sorts + re-groups, and shows it. No card recreation, no
//filesystem stats, no cover re-scaling. This is the sort/search path.
void CatalogTab::applyFilter()
{
    if (!AvailableView) return;

    // Repo name for a card (groups un-repo'd local entries under "Local").
    auto RepoOf = [this](LibraryGameCard * c) -> QString { return repoNameForBundle(c->PackagePath); };

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
        switch (AvailableSort) { case MainWindow::SortMode::Date:   return a->SortDate + "|" + a->SortTitle;
                                 case MainWindow::SortMode::Series: return a->SortSeriesKey;
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
    AvailableView->layoutCards(Mw.CardPixelWidth);
}

// Ensure a transfers-table row exists for a CID (creating it with the given status if absent), and return its
// progress item. Columns: Name | Size | Progress | Speed | Status | CID. Used both to pre-show "Queued" CIDs and to
// open an active "Fetching…" row. Kicks an off-thread CidSize fill when the size isn't cached yet.


QString CatalogTab::repoNameForBundle(const std::filesystem::path & BundleDir) const
{
    std::error_code Ec;
    const std::string B = std::filesystem::weakly_canonical(BundleDir, Ec).string();
    for (const std::string & RepoDir : PackageCatalog::RepositoryDirs(*Mw.GlobalConfigJSON))
    {
        const std::string R = std::filesystem::weakly_canonical(std::filesystem::path(RepoDir), Ec).string();
        if (!R.empty() && (B == R || B.rfind(R + "/", 0) == 0))
            return QString::fromStdString(std::filesystem::path(RepoDir).filename().string());
    }
    return QStringLiteral("Local");
}
