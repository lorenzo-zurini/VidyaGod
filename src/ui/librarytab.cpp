#include "librarytab.h"
#include "cardpool.h"
#include "cardtoolbar.h"
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

void AddLocalPackage(AppModel &Model, QWidget *Parent)
{
    const QString sel = QFileDialog::getExistingDirectory(Parent, "Select package or directory…");
    if (sel.isEmpty()) return;
    const auto [added, skipped] = Model.importPackagesFromDir(sel);
    if (added == 0 && skipped == 0) { QMessageBox::warning(Parent, "No packages found", "No valid packages found."); return; }
    QMessageBox::information(Parent, "Done", QString("Added %1 package(s). %2 skipped.").arg(added).arg(skipped));
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

    // Sort + status + search + size: the toolbar both card tabs share; this tab's authoring buttons follow.
    const CardToolbar Bar = MakeCardToolbar(this, Model,
        { {"Name", SortMode::Name}, {"Date", SortMode::Date}, {"Series", SortMode::Series}, {"Recent", SortMode::Recent} },
        CurrentSort,
        [this](SortMode Mode) {
            CurrentSort = Mode;
            (*Model.config())["Settings"]["SortMode"] = static_cast<int>(CurrentSort);
            Model.save();
            sortAndFilter(); View->layoutCards(Model.cardPixelWidth());   // cheap: re-sort + re-filter (covers already scaled)
        },
        [this](const QString &Text) {
            LibrarySearch = Text;
            sortAndFilter(); View->layoutCards(Model.cardPixelWidth());   // cheap: re-filter (covers already scaled)
        },
        "Library status");
    QWidget * toolbar = Bar.Bar;
    QHBoxLayout * tl = Bar.Row;
    StatusLabel = Bar.Status;

    // Authoring entry points — moved here from Settings → Installed Packages so they're discoverable on the main screen.
    QPushButton * edBtn  = new QPushButton("Package Editor", toolbar);
    QPushButton * addBtn = new QPushButton("Add Local Package", toolbar);
    tl->addWidget(edBtn); tl->addWidget(addBtn);   // "Add CID" now lives on the Catalog tab
    connect(edBtn, &QPushButton::clicked, this, [this]{
        PackageEditor::OpenFor(Model.config(), this, "");
    });
    connect(addBtn, &QPushButton::clicked, this, [this]{ AddLocalPackage(Model, this); });

    layout->addWidget(toolbar);

    View = new LibraryView(this);
    View->setAccessibleName("Library games");   // what ui card / ui show call it
    View->setEmptyMessage("No games yet.\n\nGet games from the Catalog tab (add a source in Settings or a peer in Network first),\nor make one: Package Editor → New package.");
    layout->addWidget(View);

    // Persisted collapse state for the Series-view sections.
    LibraryCollapsedSeries = LoadCollapsed(Model, "LibraryCollapsedSeries");
    connect(View, &LibraryView::groupToggled, this, [this](const QString & name, bool collapsed){
        if (collapsed) LibraryCollapsedSeries.insert(name); else LibraryCollapsedSeries.remove(name);
        SaveCollapsed(Model, "LibraryCollapsedSeries", LibraryCollapsedSeries);
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
    qDeleteAll(*LibraryGameCards);
    *LibraryGameCards = CardPool::Installed(Model);
    LogOut("LibraryTab", "Library shows " + std::to_string(LibraryGameCards->size()) + " hydrated game(s).");
}

void LibraryTab::sortAndFilter()
{
    auto & cards = *LibraryGameCards;
    CardPool::Sort(cards, CurrentSort);

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
            for (auto & [Uid, At] : *Played) At = QString::fromStdString(InstanceStore::LastRun(*Cfg, Uid));
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
    SyncSizeButtons(this, cardW);
    relayout();
}
