#include "librarytab.h"
#include "installed.h"
#include "shelf.h"
#include <QDate>
#include <QMenu>
#include "appmodel.h"          // AppModel — catalog/config + card size
#include "covercache.h"        // re-take covers when the cards grow
#include "packagecatalog.h"   // GoldenSince / SetGolden (the golden marker) and the rest
#include "manifestmodel.h"
#include "packageeditor.h"     // the authoring entry points moved here from Settings
#include "mainwindow.h"        // MainWindow::RefreshPackage (app-wide "package edited" hook)
#include "asyncwork.h"
#include "instancestore.h"   // LASTRUN — the Recent sort
#include "commonutils.h"       // LogOut — surface the shown-card count (the "empty library" diagnostic)

#include <QRegularExpression>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QButtonGroup>
#include <QFrame>
#include <QFileDialog>
#include <QMessageBox>
#include <QDialog>
#include <QLabel>
#include <QScrollArea>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

LibraryTab::LibraryTab(AppModel &model, QWidget *parent)
    : QWidget(parent), Model(model)
{
    auto & S = (*Model.config())["Settings"];
    if (S.contains("SortMode") && S["SortMode"].is_number_integer())
        CurrentSort = static_cast<SortMode>(int(S["SortMode"]));
    buildUi();
    connect(&Model, &AppModel::catalogChanged, this, &LibraryTab::rebuild);
    connect(&Model, &AppModel::coversReady,    this, &LibraryTab::refreshCovers);
    connect(&Model, &AppModel::cardSizeChanged, this, &LibraryTab::onCardSizeChanged);
    connect(CoverCache::instance(), &CoverCache::coversTooSmall, this, [this] {   // wider cards: decode the covers again
        for (LibraryGameCard * Card : *LibraryGameCards) if (Card && !Card->CoverOriginal.isNull()) Card->CoverStale = true;
        refreshCovers();
    });
    rebuild();
}

LibraryTab::~LibraryTab()
{
    qDeleteAll(*LibraryGameCards);
    delete LibraryGameCards;
}

void LibraryTab::buildUi()
{
    QVBoxLayout * layout = new QVBoxLayout(this);
    layout->setContentsMargins(0,0,0,0);
    layout->setSpacing(0);

    // Size + sort + search toolbar.
    QWidget * toolbar = new QWidget(this);
    QHBoxLayout * tl = new QHBoxLayout(toolbar);
    tl->setContentsMargins(8,4,8,4);

    // Sort buttons — left side.
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
        connect(b, &QPushButton::toggled, this, [this, mode](bool checked){
            if (!checked) return;
            CurrentSort = mode;
            (*Model.config())["Settings"]["SortMode"] = static_cast<int>(CurrentSort);
            Model.save();
            sortAndFilter(); View->layoutCards(Model.cardPixelWidth());   // cheap: re-sort + re-filter (covers already scaled)
        });
        tl->addWidget(b);
    };
    makeSortBtn("Name",   SortMode::Name);
    makeSortBtn("Date",   SortMode::Date);
    makeSortBtn("Series", SortMode::Series);
    makeSortBtn("Recent", SortMode::Recent);

    StatusLabel = new QLabel(toolbar);
    StatusLabel->setAccessibleName("Library status");
    StatusLabel->setStyleSheet("color:#8f98a0; font-size:9pt; padding-left:12px;");
    tl->addWidget(StatusLabel);

    tl->addStretch();

    // Search filter (by title).
    QLineEdit * searchEdit = new QLineEdit(toolbar);
    searchEdit->setPlaceholderText("Search…");
    searchEdit->setClearButtonEnabled(true);
    searchEdit->setFixedWidth(180);
    connect(searchEdit, &QLineEdit::textChanged, this, [this](const QString & t){
        LibrarySearch = t.trimmed();
        sortAndFilter(); View->layoutCards(Model.cardPixelWidth());   // cheap: re-filter (covers already scaled)
    });
    tl->addWidget(searchEdit);

    // Size buttons — right side.
    auto makeSizeBtn = [&](const QString & lbl, int w) {
        QPushButton * b = new QPushButton(lbl, toolbar);
        b->setObjectName("size" + lbl);   // sizeLarge, sizeMedium, sizeSmall: one name each
        b->setCheckable(true); b->setChecked(Model.cardPixelWidth() == w);
        b->setStyleSheet(
            "QPushButton{color:#8f98a0;background:transparent;border:none;font-size:9pt;padding:2px 8px;}"
            "QPushButton:checked{color:#c6d4df;border-bottom:2px solid #4a90d9;}"
            "QPushButton:hover{color:#c6d4df;}");
        connect(b, &QPushButton::clicked, this, [this,w](){ Model.setCardPixelWidth(w); });   // → cardSizeChanged → onCardSizeChanged
        tl->addWidget(b);
    };
    makeSizeBtn("Large",250); makeSizeBtn("Medium",185); makeSizeBtn("Small",120);

    // Authoring entry points — moved here from Settings → Installed Packages so they're discoverable on the main screen.
    auto * sep = new QFrame(toolbar); sep->setFrameShape(QFrame::VLine); sep->setStyleSheet("color:#3a4048;");
    tl->addWidget(sep);
    QPushButton * edBtn  = new QPushButton("Package Editor", toolbar);
    QPushButton * addBtn = new QPushButton("Add Local Package", toolbar);
    tl->addWidget(edBtn); tl->addWidget(addBtn);   // "Add CID" now lives on the Catalog tab
    connect(edBtn, &QPushButton::clicked, this, [this]{
        PackageEditor::OpenFor(Model.config(), this, "");
    });
    connect(addBtn, &QPushButton::clicked, this, [this]{
        const QString sel = QFileDialog::getExistingDirectory(this, "Select package or directory…");
        if (sel.isEmpty()) return;
        const auto [added, skipped] = Model.importPackagesFromDir(sel);
        if (added == 0 && skipped == 0) { QMessageBox::warning(this, "No packages found", "No valid packages found."); return; }
        QMessageBox::information(this, "Done", QString("Added %1 package(s). %2 skipped.").arg(added).arg(skipped));
    });

    layout->addWidget(toolbar);

    View = new LibraryView(this);
    View->setAccessibleName("Library games");   // what ui card / ui show call it
    View->setEmptyMessage("No games yet.\n\nGet games from the Catalog tab (add a source in Settings or a friend in Network first),\nor make one: Package Editor → New package.");
    layout->addWidget(View);

    // Persisted collapse state for the Series-view sections.
    if ((*Model.config())["Settings"].contains("LibraryCollapsedSeries")
        && (*Model.config())["Settings"]["LibraryCollapsedSeries"].is_array())
        for (const auto & N : (*Model.config())["Settings"]["LibraryCollapsedSeries"])
            if (N.is_string()) LibraryCollapsedSeries.insert(QString::fromStdString(std::string(N)));
    connect(View, &LibraryView::groupToggled, this, [this](const QString & name, bool collapsed){
        if (collapsed) LibraryCollapsedSeries.insert(name); else LibraryCollapsedSeries.remove(name);
        auto & arr = (*Model.config())["Settings"]["LibraryCollapsedSeries"] = nlohmann::ordered_json::array();
        for (const QString & N : LibraryCollapsedSeries) arr.push_back(N.toStdString());
        Model.save();
    });
    //A card's menu: the golden marker — this machine's note that the package's golden pass is done. Cosmetic and local
    //(Settings.GoldenPackages), keyed by the family, so every card of the family shows it.
    connect(View, &LibraryView::cardMenuRequested, this, [this](LibraryGameCard * card, const QPoint & at){
        if (!card || card->RepUid.empty()) return;
        const std::string Since = PackageCatalog::GoldenSince(*Model.config(), card->RepUid);
        QMenu Menu(this);
        QAction * Golden = Menu.addAction(Since.empty() ? QStringLiteral("Mark as golden")
                                                        : QStringLiteral("Golden since %1 — unmark").arg(QString::fromStdString(Since)));
        Golden->setObjectName("goldenToggle");
        if (Menu.exec(at) != Golden) return;
        PackageCatalog::SetGolden(*Model.config(), card->RepUid, Since.empty(), QDate::currentDate().toString(Qt::ISODate).toStdString());
        Model.save();
        for (LibraryGameCard * C : *LibraryGameCards)
            if (C->RepUid == card->RepUid) C->Golden = Since.empty();
        View->repaintCards();
    });
}

void LibraryTab::buildCards()
{
    qDeleteAll(*LibraryGameCards); LibraryGameCards->clear();
    //One tile per presentable GROUP of launchable nodes. Only HYDRATED groups (every edition's content present
    //locally) appear in the Library; un-hydrated ones live in the Available tab.
    const auto Hyd = Installed::HydrationMap(Model.catalogIndex());   // O(N+E) once — never per-launchable (deep chains → O(N²))
    //The cards and the launch dialogs they open read one snapshot: a later re-index replaces it, and they are rebuilt.
    const std::shared_ptr<const NodeIndex> Snapshot = Model.catalogSnapshot();
    for (const Shelf::Tile & Tile : Shelf::Tiles(*Snapshot))
    {
        std::vector<std::string> Ids;
        bool AllHydrated = true, HasContent = false;
        for (const Node * N : Tile.Rows)
        {
            Ids.push_back(N->Key());
            auto H = Hyd.find(N->Key());
            if (H == Hyd.end() || !H->second.Hydrated) AllHydrated = false;
            if (H != Hyd.end() && H->second.OnDisk) HasContent = true;   // a file of it is HERE, not only declared
        }
        if (!Tile.Graft.empty())   // a graft's card: the graft is the game, so it must be installed too
        {
            auto H = Hyd.find(Tile.Graft);
            if (H == Hyd.end() || !H->second.Hydrated) AllHydrated = false;
        }
        if (!AllHydrated || Ids.empty() || !HasContent)            // skip content-less/malformed (vacuously hydrated)
        {
            if (AllHydrated && !Ids.empty())                      // every row installed, yet nothing of its own on disk
                LogOut("LibraryTab", "not shown: '" + Tile.Tile.value("TITLE", Tile.Uid) + "' (tile " + Tile.Uid
                                     + ") — none of its rows has content of its own on disk");
            continue;
        }
        auto * c = new LibraryGameCard(Model.config(), Snapshot, std::move(Ids), Tile.Uid);
        c->InitializeClassVariables();
        LibraryGameCards->append(c);
    }
    LogOut("LibraryTab", "Library shows " + std::to_string(LibraryGameCards->size()) + " hydrated game(s).");
}

void LibraryTab::sortAndFilter()
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
    case SortMode::Recent:
        // Most recently played first; games never played after them, by title.
        // The times are read off the GUI thread (countSizes); until they land, the order is by title, then re-sorted.
        std::stable_sort(cards.begin(), cards.end(), [](auto * a, auto * b) {
            const QString & A = a->LastPlayedCache, & B = b->LastPlayedCache;
            if (A.isEmpty() != B.isEmpty()) return B.isEmpty();
            if (A != B) return A > B;
            return a->SortTitle < b->SortTitle;
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

void LibraryTab::relayout()
{
    View->prescaleCovers(Model.cardPixelWidth());
    View->layoutCards(Model.cardPixelWidth());
}

void LibraryTab::rebuild()
{
    buildCards();
    sortAndFilter();   // sorts + filters into LibraryVisible and calls View->setCards()
    relayout();
    countSizes();
}

void LibraryTab::countSizes()
{
    const int Games = LibraryGameCards->count();
    StatusLabel->setText(QString("%1 game%2").arg(Games).arg(Games == 1 ? "" : "s"));
    if (!Model.config()) return;
    //What the installed packages hold on disk: a walk of every package folder, so off the GUI thread; the cards get
    //their own size for the hover card. One count at a time: rebuilds while one runs (a download landing) ask for one
    //more when it ends, never a pile of walks over the whole library.
    if (Counting) { CountAgain = true; return; }
    Counting = true;
    auto Cfg = std::make_shared<const nlohmann::ordered_json>(*Model.config());
    std::shared_ptr<const NodeIndex> Idx = Model.catalogSnapshot();
    auto Found = std::make_shared<std::vector<PackageCatalog::InstalledPackage>>();
    auto Played = std::make_shared<std::map<std::string, QString>>();   // family UID -> its newest LASTRUN
    for (LibraryGameCard * C : *LibraryGameCards) if (C && !C->RepUid.empty()) (*Played)[C->RepUid];
    AsyncWork::Run(this, [Cfg, Idx, Found, Played]{
            *Found = PackageCatalog::InstalledPackages(*Cfg, *Idx);
            for (auto & [Uid, At] : *Played)
            {
                const auto Names = InstanceStore::List(*Cfg, Uid);   // newest LASTRUN first
                if (Names.empty()) continue;
                const auto C = InstanceStore::ReadConfig(*Cfg, Uid, Names.front());
                if (C.is_object() && C.contains("LASTRUN") && C["LASTRUN"].is_string()) At = QString::fromStdString(C["LASTRUN"].get<std::string>());
            }
        },
        [this, Found, Played, Games]{
            Counting = false;
            if (CountAgain) { CountAgain = false; countSizes(); return; }   // the cards changed meanwhile: count them
            for (LibraryGameCard * C : *LibraryGameCards)
                if (const auto It = Played->find(C->RepUid); C && It != Played->end()) { C->LastPlayedCache = It->second; C->LastPlayedRead = true; }
            if (CurrentSort == SortMode::Recent) { sortAndFilter(); View->layoutCards(Model.cardPixelWidth()); }
            std::map<std::string, std::uintmax_t> ByDir;
            std::uintmax_t Total = 0;
            for (const auto & P : *Found) { ByDir[P.Dir.lexically_normal().string()] = P.Bytes; Total += P.Bytes; }
            for (LibraryGameCard * C : *LibraryGameCards)
                if (const auto It = ByDir.find(C->PackagePath.lexically_normal().string()); It != ByDir.end()) C->Bytes = static_cast<qint64>(It->second);
            StatusLabel->setText(QString("%1 game%2 · %3 installed").arg(Games).arg(Games == 1 ? "" : "s")
                                     .arg(QString::fromStdString(HumanBytes(static_cast<long long>(Total)))));
        });
}

void LibraryTab::refreshCovers()
{
    bool Any = false;
    for (LibraryGameCard * Card : *LibraryGameCards)
        if (Card && (Card->CoverOriginal.isNull() || Card->CoverStale)) { Card->InitializeClassVariables(); Any = true; }
    if (Any && View) View->refreshVisuals();
}

void LibraryTab::onCardSizeChanged(int cardW)
{
    const QString want = cardW >= 250 ? "Large" : cardW <= 120 ? "Small" : "Medium";
    for (auto * b : findChildren<QPushButton*>(QRegularExpression("^size(Large|Medium|Small)$"))) b->setChecked(b->text() == want);
    relayout();
}
