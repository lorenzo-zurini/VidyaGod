#include "catalogtab.h"
#include "appmodel.h"          // AppModel — catalog/config + card size
#include "packagecatalog.h"
#include "manifestmodel.h"
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

CatalogTab::CatalogTab(AppModel &model, QWidget *parent)
    : QWidget(parent), Model(model)
{
    buildUi();   // calls rebuild() once
    connect(&Model, &AppModel::catalogChanged,  this, &CatalogTab::rebuild);
    connect(&Model, &AppModel::coversReady,     this, &CatalogTab::refreshCovers);
    connect(&Model, &AppModel::cardSizeChanged, this, &CatalogTab::onCardSizeChanged);
}

// Download-state hooks (driven by DownloadManager signals) — paint download state onto this tab's cards and track
// the in-flight set so a rebuild() can restore the overlay without querying anyone.
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
        if (Card && Card->CoverOriginal.isNull()) { Card->InitializeClassVariables(); Any = true; }
    if (Any && AvailableView) AvailableView->refreshVisuals();
}

void CatalogTab::buildUi()
{
    // Load persisted collapse state for repo sections.
    AvailableCollapsedRepos.clear();
    if ((*Model.config())["Settings"].contains("AvailableCollapsedRepos")
        && (*Model.config())["Settings"]["AvailableCollapsedRepos"].is_array())
        for (const auto & R : (*Model.config())["Settings"]["AvailableCollapsedRepos"])
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
    auto makeSortBtn = [&](const QString & lbl, SortMode mode) {
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
    makeSortBtn("Name", SortMode::Name); makeSortBtn("Date", SortMode::Date); makeSortBtn("Series", SortMode::Series);
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
        b->setObjectName("sizeBtn");
        b->setCheckable(true); b->setChecked(Model.cardPixelWidth() == w);
        b->setStyleSheet(
            "QPushButton{color:#8f98a0;background:transparent;border:none;font-size:9pt;padding:2px 8px;}"
            "QPushButton:checked{color:#c6d4df;border-bottom:2px solid #4a90d9;}"
            "QPushButton:hover{color:#c6d4df;}");
        connect(b, &QPushButton::clicked, this, [this,w](){ Model.setCardPixelWidth(w); });   // → cardSizeChanged → onCardSizeChanged
        tl->addWidget(b);
    };
    makeSizeBtn("Large",250); makeSizeBtn("Medium",185); makeSizeBtn("Small",120);
    v->addWidget(toolbar);

    AvailableView = new LibraryView(this);
    AvailableView->setHoverAction("⬇  Download", true);
    AvailableView->setEmptyMessage("Nothing to download.\n\nAdd a repository in Settings → Repositories (or hit “Sync now”) to see shared games here.");
    v->addWidget(AvailableView);

    // Card clicks are re-emitted as requests; MainWindow wires them to the DownloadManager (no direct coupling).
    connect(AvailableView, &LibraryView::downloadRequested, this, &CatalogTab::downloadRequested);
    connect(AvailableView, &LibraryView::cancelRequested,   this, &CatalogTab::cancelRequested);
    connect(AvailableView, &LibraryView::groupToggled, this, [this](const QString & name, bool collapsed){
        if (collapsed) AvailableCollapsedRepos.insert(name); else AvailableCollapsedRepos.remove(name);
        auto & arr = (*Model.config())["Settings"]["AvailableCollapsedRepos"] = nlohmann::ordered_json::array();
        for (const QString & R : AvailableCollapsedRepos) arr.push_back(R.toStdString());
        Model.save();
    });

    rebuild();
}

// Model card-size changed (from either tab's size buttons) — re-check the matching button and re-scale + relayout.
void CatalogTab::onCardSizeChanged(int cardW)
{
    const QString want = cardW >= 250 ? "Large" : cardW <= 120 ? "Small" : "Medium";
    for (auto * b : findChildren<QPushButton*>("sizeBtn")) b->setChecked(b->text() == want);
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
    AvailableVisible.clear();
    qDeleteAll(*AvailableGameCards); AvailableGameCards->clear();

    //One card per un-hydrated presentable group: at least one edition's content is still missing AND fetchable
    //over IPFS. (A fully-hydrated group lives in the Library tab.)
    for (const std::vector<const Node*> & Group : PackageCatalog::PresentableGroups(Model.catalogIndex()))
    {
        std::vector<std::string> Ids;
        bool AnyMissing = false, AnyFetchable = false;
        for (const Node * N : Group)
        {
            Ids.push_back(N->NodeId);
            if (!PackageCatalog::NodeHydrated(Model.catalogIndex(), N->NodeId)) AnyMissing = true;
            if (!PackageCatalog::NodeContentCids(Model.catalogIndex(), N->NodeId).empty()) AnyFetchable = true;
        }
        if (!AnyMissing || !AnyFetchable || Ids.empty()) continue;
        auto * c = new LibraryGameCard(Model.config(), &Model.catalogIndex(), std::move(Ids));
        c->InitializeClassVariables();

        const bool Busy = ActiveDownloads.contains(c->GameKey);
        c->Downloading = Busy;
        c->DownloadPercent = Busy ? ActiveDownloads.value(c->GameKey) : -1.0;   // carry current progress across rebuilds
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
        for (const std::string & Nid : ManifestModel::ResolveNodeOrder(Model.catalogIndex(), Lid, {})) {
            const Node * N = Model.catalogIndex().Find(Nid);
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
    AvailableView->prescaleCovers(Model.cardPixelWidth());
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
    AvailableView->layoutCards(Model.cardPixelWidth());
}

// Ensure a transfers-table row exists for a CID (creating it with the given status if absent), and return its
// progress item. Columns: Name | Size | Progress | Speed | Status | CID. Used both to pre-show "Queued" CIDs and to
// open an active "Fetching…" row. Kicks an off-thread CidSize fill when the size isn't cached yet.


QString CatalogTab::repoNameForBundle(const std::filesystem::path & BundleDir) const
{
    std::error_code Ec;
    const std::string B = std::filesystem::weakly_canonical(BundleDir, Ec).string();
    for (const std::string & RepoDir : PackageCatalog::RepositoryDirs(*Model.config()))
    {
        const std::string R = std::filesystem::weakly_canonical(std::filesystem::path(RepoDir), Ec).string();
        if (!R.empty() && (B == R || B.rfind(R + "/", 0) == 0))
            return QString::fromStdString(std::filesystem::path(RepoDir).filename().string());
    }
    return QStringLiteral("Local");
}
