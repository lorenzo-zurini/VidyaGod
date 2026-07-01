#include "librarytab.h"
#include "appmodel.h"          // AppModel — catalog/config + card size
#include "packagecatalog.h"
#include "manifestmodel.h"
#include "packageeditor.h"     // the authoring entry points moved here from Settings
#include "mainwindow.h"        // MainWindow::RefreshPackage (app-wide "package edited" hook)

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

    // Authoring entry points — moved here from Settings → Installed Packages so they're discoverable on the main screen.
    auto * sep = new QFrame(toolbar); sep->setFrameShape(QFrame::VLine); sep->setStyleSheet("color:#3a4048;");
    tl->addWidget(sep);
    QPushButton * edBtn  = new QPushButton("Package Editor", toolbar);
    QPushButton * addBtn = new QPushButton("Add Local Package", toolbar);
    QPushButton * cidBtn = new QPushButton("Add by CID…", toolbar);
    tl->addWidget(edBtn); tl->addWidget(addBtn); tl->addWidget(cidBtn);
    connect(cidBtn, &QPushButton::clicked, this, [this]{ openPackageSourcesDialog(); });
    connect(edBtn, &QPushButton::clicked, this, [this]{
        auto * Ed = new PackageEditor(Model.config(), this);
        connect(Ed, &PackageEditor::packageSaved, &MainWindow::RefreshPackage);
        Ed->show();
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
    View->setEmptyMessage("No games yet.\n\nAdd a repository in Settings → Repositories, then download games from the Available tab.");
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
}

void LibraryTab::openPackageSourcesDialog()
{
    auto * dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle("Package sources (IPFS CID)");
    dlg->resize(560, 420);
    auto * root = new QVBoxLayout(dlg);

    auto * info = new QLabel(QStringLiteral(
        "A package source is an IPFS folder CID of dehydrated packages (manifests + covers, no content). Adding one "
        "fetches the manifests only and lists its games in the catalog under “CID Packages”, downloadable on "
        "demand. Requires IPFS networking to be on."), dlg);
    info->setWordWrap(true); info->setStyleSheet("color:#8f98a0;font-size:9pt;");
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
        { QMessageBox::warning(dlg, "Add by CID", "Enter a folder CID that isn't already added."); return; }
        cidEdit->clear(); nameEdit->clear();   // the fetch runs off-thread; the list refreshes on packageSourcesChanged
    });
    connect(&Model, &AppModel::packageSourcesChanged, dlg, [rebuild]{ rebuild(); });
    connect(&Model, &AppModel::packageSourceFailed, dlg, [dlg](const QString & m){ QMessageBox::warning(dlg, "Package source", m); });

    dlg->show();
}

void LibraryTab::buildCards()
{
    qDeleteAll(*LibraryGameCards); LibraryGameCards->clear();
    //One tile per presentable GROUP of launchable nodes. Only HYDRATED groups (every edition's content present
    //locally) appear in the Library; un-hydrated ones live in the Available tab.
    for (const std::vector<const Node*> & Group : PackageCatalog::PresentableGroups(Model.catalogIndex()))
    {
        std::vector<std::string> Ids;
        bool AllHydrated = true, HasContent = false;
        for (const Node * N : Group)
        {
            Ids.push_back(N->NodeId);
            if (!PackageCatalog::NodeHydrated(Model.catalogIndex(), N->NodeId)) AllHydrated = false;
            if (PackageCatalog::NodeHasContent(Model.catalogIndex(), N->NodeId)) HasContent = true;
        }
        if (!AllHydrated || Ids.empty() || !HasContent) continue;   // skip content-less/malformed (vacuously hydrated)
        auto * c = new LibraryGameCard(Model.config(), &Model.catalogIndex(), std::move(Ids));
        c->InitializeClassVariables();
        LibraryGameCards->append(c);
    }
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
}

void LibraryTab::refreshCovers()
{
    bool Any = false;
    for (LibraryGameCard * Card : *LibraryGameCards)
        if (Card && Card->CoverOriginal.isNull()) { Card->InitializeClassVariables(); Any = true; }
    if (Any && View) View->refreshVisuals();
}

void LibraryTab::onCardSizeChanged(int cardW)
{
    const QString want = cardW >= 250 ? "Large" : cardW <= 120 ? "Small" : "Medium";
    for (auto * b : findChildren<QPushButton*>("sizeBtn")) b->setChecked(b->text() == want);
    relayout();
}
