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
// LibraryGameCard
// ═════════════════════════════════════════════════════════════════════════════

LibraryGameCard::LibraryGameCard(nlohmann::ordered_json * gc, int game, std::string subgameId)
    : GlobalConfigJSON(gc), Game(game), SubgameID(subgameId)
{
    MANIFESTJSON = new nlohmann::ordered_json;
}

LibraryGameCard::~LibraryGameCard() { delete MANIFESTJSON; }

void LibraryGameCard::InitializeClassVariables()
{
    PackagePath = std::filesystem::path(std::string((*GlobalConfigJSON)["LIBRARY"][Game]["PATH"]));
    std::vector<std::string> AsmWarn;
    if (!JSONOps::AssembleManifest(QString::fromStdString(PackagePath.string()), *MANIFESTJSON, AsmWarn)) return;

    int idx = ContainerWrapper::FindGameIndex(*MANIFESTJSON, SubgameID);
    if (idx == -1) return;

    GameTitle = QString::fromStdString((*MANIFESTJSON)["GAMES"][idx]["TITLE"]);

    // Sort keys
    SortTitle = GameTitle.toLower();
    if (SortTitle.startsWith("the ")) SortTitle = SortTitle.mid(4);

    auto & Meta = (*MANIFESTJSON)["GAMES"][idx]["METADATA"];

    SortDate = (Meta.is_object() && Meta.contains("RELEASEDATE") && Meta["RELEASEDATE"].is_string())
               ? QString::fromStdString(std::string(Meta["RELEASEDATE"])) : "9999";

    auto strOrEmpty = [&](const char * key) -> QString {
        return (Meta.is_object() && Meta.contains(key) && Meta[key].is_string())
               ? QString::fromStdString(std::string(Meta[key])) : "";
    };
    auto numPad = [&](const char * key) -> QString {
        if (!Meta.is_object() || !Meta.contains(key)) return "999";
        auto & v = Meta[key];
        int n = v.is_number() ? int(v) : (v.is_string() ? QString::fromStdString(std::string(v)).toInt() : 999);
        return QString("%1").arg(n, 5, 10, QChar('0'));
    };
    SeriesName    = strOrEmpty("SERIES");
    SortSeriesKey = SeriesName + "|" + numPad("SERIESSORTNUMBER") + "|"
                  + strOrEmpty("SUBSERIES") + "|" + numPad("SUBSERIESSORTNUMBER")
                  + "|" + SortTitle;

    //Cover — local-first via CoverCache, which handles the dual-form COVER (a filename, or an object
    //{PATH,SOURCE:{ipfs,CID}}): local file if present, else the cached IPFS fetch, else a background fetch that
    //fires CoverCache::coverReady(cid) so the card reloads. A null CoverOriginal renders the placeholder.
    CoverOriginal = QPixmap();
    CoverCid.clear();
    const nlohmann::ordered_json * CoverNode = nullptr;
    if (Meta.is_object() && Meta.contains("COVER"))                  CoverNode = &Meta["COVER"];
    else if ((*MANIFESTJSON)["GAMES"][idx].contains("COVER"))        CoverNode = &(*MANIFESTJSON)["GAMES"][idx]["COVER"];
    if (CoverNode)
    {
        const QString Path = CoverCache::instance()->resolve(*CoverNode, QString::fromStdString(PackagePath.string()));
        if (!Path.isEmpty()) CoverOriginal.load(Path);
        else { QString f; CoverCache::Locate(*CoverNode, f, CoverCid); }   // pending fetch — refresh on coverReady
    }
}

void LibraryGameCard::play()
{
    //Validate the assembled manifest before launching; hard errors block launch.
    {
        std::vector<std::string> ValErr, ValWarn;
        JSONOps::ValidateManifest(*MANIFESTJSON, ValErr, ValWarn);
        for (const auto &W : ValWarn) LogWarn("LibraryGameCard", "Manifest: " + W);
        if (!ValErr.empty())
        {
            QString Msg = "This package's manifest has errors and cannot be launched:\n\n";
            for (const auto &E : ValErr) Msg += "• " + QString::fromStdString(E) + "\n";
            QMessageBox::critical(nullptr, "Manifest Error", Msg);
            return;
        }
    }

    //Gate: the runner this game uses must be imported (IPFS builds are fetched ahead of time, never at
    //launch). If a required build/DEFPREFIX is missing, offer to import the runner instead of launching.
    {
        nlohmann::ordered_json ManifestCopy = *MANIFESTJSON;                  // copy — the probe folds runner comps
        struct ContainerParams CP(PackagePath, SubgameID, std::string());
        ContainerWrapper Probe(*GlobalConfigJSON, ManifestCopy, CP);          // resolution only (no mount)
        if (!ContainerWrapper::EnsureSources(CP))
        {
            nlohmann::ordered_json RunnerPkg;
            for (const auto & Pkg : ContainerWrapper::CatalogPackages(*GlobalConfigJSON))
                if (JSONOps::HasRunners(Pkg))
                    for (const auto & R : Pkg["RUNNERS"])
                        if (R.value("RUNNER_ID", std::string()) == CP.RunnerID) { RunnerPkg = Pkg; break; }

            const std::string Vid = CP.RunnerVariantID;
            const QString Rid = QString::fromStdString(CP.RunnerID + (Vid.empty() ? "" : (":" + Vid)));
            if (RunnerPkg.is_null() || !RunnerWrapper::ShipsBuild(RunnerPkg, Vid))
            { QMessageBox::warning(nullptr, "Runner unavailable",
                  "This game needs runner '" + Rid + "', which isn't imported and can't be fetched."); return; }
            if (!IpfsWrapper::Available())
            { QMessageBox::warning(nullptr, "Kubo required",
                  "Runner '" + Rid + "' must be downloaded over IPFS, but Kubo (ipfs) isn't installed."); return; }
            if (QMessageBox::question(nullptr, "Import runner?",
                    "This game needs runner '" + Rid + "', which isn't imported yet.\n\n"
                    "Download and import it now? (progress shows in the IPFS tab)") != QMessageBox::Yes)
                return;
            std::thread([GC = GlobalConfigJSON, RunnerPkg, Vid, Rid]{
                std::string Err;
                bool Ok = ContainerWrapper::ImportRunner(*GC, RunnerPkg, Vid, &Err);
                QMetaObject::invokeMethod(qApp, [Ok, Err, Rid]{
                    if (Ok) QMessageBox::information(nullptr, "Runner imported",
                                "Runner '" + Rid + "' imported — press Play to launch.");
                    else    QMessageBox::warning(nullptr, "Runner import failed", QString::fromStdString(Err));
                }, Qt::QueuedConnection);
            }).detach();
            return;
        }
    }

    std::string uid;
    if (MANIFESTJSON->contains("PACKAGEUID") && !(*MANIFESTJSON)["PACKAGEUID"].is_null())
        uid = std::string((*MANIFESTJSON)["PACKAGEUID"]);
    bool skip = false;
    if (!uid.empty()) {
        auto US = ContainerWrapper::GetPackageUserSettings(*GlobalConfigJSON, uid);
        if (US.contains("SKIP_LAUNCH_DIALOG") && US["SKIP_LAUNCH_DIALOG"].is_boolean())
            skip = bool(US["SKIP_LAUNCH_DIALOG"]);
    }
    bool shift = (QGuiApplication::keyboardModifiers() & Qt::ShiftModifier) != 0;
    auto * dlg = new PreLaunchWindow(GlobalConfigJSON, MANIFESTJSON,
                                     PackagePath.string(), SubgameID, nullptr);
    dlg->show();
    if (skip && !shift)
        QMetaObject::invokeMethod(dlg, "onLaunchClicked", Qt::QueuedConnection);
}

void LibraryGameCard::edit()
{
    (new PreLaunchWindow(GlobalConfigJSON, MANIFESTJSON,
                         PackagePath.string(), SubgameID, nullptr))->show();
}


// ═════════════════════════════════════════════════════════════════════════════
// LibraryView
// ═════════════════════════════════════════════════════════════════════════════

LibraryView::LibraryView(QWidget * parent)
    : QAbstractScrollArea(parent), TitleFM(QApplication::font())
{

    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameShape(QFrame::NoFrame);
    setStyleSheet(
        "QScrollBar:vertical{width:12px;border:none;}"
        "QScrollBar::handle:vertical{border-radius:6px;min-height:32px;}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}");

    viewport()->setMouseTracking(true);
    // WA_OpaquePaintEvent: skip the background clear before our paintEvent.
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);

    TitleFont = QApplication::font();
    TitleFont.setPointSize(9);
    TitleFM   = QFontMetrics(TitleFont);

    PlayFont = TitleFont;
    PlayFont.setBold(true);
    PlayFont.setPointSize(10);
}

void LibraryView::setCards(QList<LibraryGameCard *> * cards)
{
    Cards        = cards;
    HoveredIdx   = -1;
    LastCols     = 0;
    CardW        = 0;
    SeriesGroups.clear();
    Rects.clear();
    if (Cards) Rects.resize(Cards->count());
}

void LibraryView::setSeriesGroups(const QVector<SeriesGroup> & groups)
{
    SeriesGroups = groups;
    viewport()->update();
}

void LibraryView::refreshVisuals()
{
    if (CardW > 0) prescaleCovers(CardW); // recompute CoverScaled + ElidedTitle from the (changed) data
    viewport()->update();                 // repaint in place — Rects/SeriesGroups/CardW are untouched
}

void LibraryView::prescaleCovers(int cardW)
{
    const int cardH = cardW * 3 / 2;
    const int textW = cardW - EditW - 12;
    if (!Cards) return;
    for (auto * c : *Cards) {
        c->CoverScaled = c->CoverOriginal.isNull() ? QPixmap()
            : c->CoverOriginal.scaled(cardW, cardH,
                Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        c->ElidedTitle = TitleFM.elidedText(c->GameTitle, Qt::ElideRight, textW);
    }
}

// Adaptive spacing: gap = (vW - cols*cardW) / (cols+1) grows as window widens.
// Because gap is integer division, it changes only every (cols+1) pixels of drag
// (~5-6px for a typical column count). Full repaint fires only when cols or the
// integer gap actually changes — not on every pixel.
void LibraryView::layoutCards(int cardW)
{
    const int vW    = viewport()->width();
    const int vH    = viewport()->height();
    const int count = Cards ? Cards->count() : 0;
    const int cardH = cardW * 3 / 2;
    const int cols  = qMax(1, (vW + MinGap) / (cardW + MinGap));
    const int hGap  = qMax(MinGap, (vW - cols * cardW) / (cols + 1));
    const int rows  = (count + cols - 1) / cols;
    const int totalH = VPad + rows * cardH + (rows - 1) * MinGap + VPad;

    verticalScrollBar()->setRange(0, qMax(0, totalH - vH));
    verticalScrollBar()->setPageStep(vH);

    // Skip repaint when layout is identical — covers most resize events.
    if (cols == LastCols && hGap == LastHGap && cardW == CardW) return;

    CardW = cardW; CardH = cardH;
    LastCols = cols; LastHGap = hGap;

    Rects.resize(count);
    for (int i = 0; i < count; i++) {
        Rects[i] = QRect(
            hGap + (i % cols) * (cardW + hGap),
            VPad + (i / cols) * (cardH  + MinGap),
            cardW, cardH);
    }

    viewport()->update();
}

void LibraryView::scrollContentsBy(int, int)
{
    viewport()->update();
}

void LibraryView::wheelEvent(QWheelEvent * e)
{
    const int delta = e->angleDelta().y();
    verticalScrollBar()->setValue(verticalScrollBar()->value() - delta * 3 / 4);
    e->accept();
}

// resizeEvent fires after QAbstractScrollArea has already resized the viewport.
// viewport()->width() is the new value here.
void LibraryView::resizeEvent(QResizeEvent * e)
{
    QAbstractScrollArea::resizeEvent(e);
    layoutCards(CardW);
    // For horizontal expand within the same column band, layoutCards() returns
    // without calling update(). Qt marks only the newly exposed right strip as
    // dirty — onPaint fills it with background + any cards that intersect it.
}

bool LibraryView::viewportEvent(QEvent * e)
{
    switch (e->type()) {
    case QEvent::Paint:            onPaint(static_cast<QPaintEvent *>(e));   return true;
    case QEvent::MouseMove:        onMouseMove(static_cast<QMouseEvent *>(e)); return true;
    case QEvent::MouseButtonPress: onMousePress(static_cast<QMouseEvent *>(e)); return true;
    case QEvent::Leave:            onLeave(); return true;
    default: return QAbstractScrollArea::viewportEvent(e);
    }
}

void LibraryView::onPaint(QPaintEvent * e)
{
    if (!Cards) return;

    const int   scrollY = verticalScrollBar()->value();
    const QRect vRect   = e->rect();
    const QRect cRect   = vRect.translated(0, scrollY);

    QPainter p(viewport());
    p.setClipRect(vRect);
    p.fillRect(vRect, QApplication::palette().color(QPalette::Window));
    p.translate(0, -scrollY);

    // Series group backgrounds — drawn in content coordinates before cards
    for (auto & g : SeriesGroups) {
        const int topY = Rects[g.first].top()    - MinGap;
        const int botY = Rects[g.last].bottom()  + MinGap;
        const QRect bgR(0, topY, viewport()->width(), botY - topY);
        if (bgR.intersects(cRect)) p.fillRect(bgR, g.color);
    }

    p.setFont(TitleFont);
    const int count = Cards->count();
    for (int i = 0; i < count; i++) {
        const QRect & r = Rects[i];
        if (!r.intersects(cRect)) continue;

        auto * card = Cards->at(i);

        // Cover — GPU texture path if viewport is QOpenGLWidget
        if (!card->CoverScaled.isNull())
            p.drawPixmap(r, card->CoverScaled);
        else {
            p.fillRect(r, QApplication::palette().color(QPalette::Mid));
            p.setPen(QColor(0x8f, 0x98, 0xa0));
            p.drawText(r.adjusted(6,6,-6,-6),
                       Qt::AlignCenter | Qt::TextWordWrap, card->GameTitle);
        }

        if (i != HoveredIdx) continue;

        // Hover: darken + play button + title strip
        p.fillRect(r, QColor(0,0,0,110));

        const QRect btn(r.left()+(r.width()-90)/2, r.top()+(r.height()-32)/2-16, 90, 32);
        p.setBrush(QColor(0x4a,0x90,0xd9,230));
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(btn, 4, 4);
        p.setFont(PlayFont);
        p.setPen(Qt::white);
        p.drawText(btn, Qt::AlignCenter, "▶  Play");

        p.setFont(TitleFont);
        const QRect line(r.left(), r.bottom()-LineH, r.width(), LineH);
        p.fillRect(line, QColor(0,0,0,160));
        p.setPen(QColor(0xff,0xff,0xff,220));
        p.drawText(line.adjusted(8,0,-(EditW+4),0),
                   Qt::AlignVCenter|Qt::AlignLeft|Qt::TextSingleLine,
                   card->ElidedTitle);
        p.setPen(QColor(0xc6,0xd4,0xdf,200));
        p.drawText(QRect(r.right()-EditW, r.bottom()-LineH, EditW, LineH),
                   Qt::AlignCenter, "···");
    }
}

void LibraryView::onMouseMove(QMouseEvent * e)
{
    if (!Cards) return;
    const QPoint pos = e->pos() + QPoint(0, verticalScrollBar()->value());
    int newHover = -1;
    const int count = Cards->count();
    for (int i = 0; i < count; i++)
        if (Rects[i].contains(pos)) { newHover = i; break; }
    if (newHover == HoveredIdx) return;

    const int sy = verticalScrollBar()->value();
    if (HoveredIdx >= 0) viewport()->update(Rects[HoveredIdx].translated(0,-sy));
    HoveredIdx = newHover;
    if (HoveredIdx >= 0) viewport()->update(Rects[HoveredIdx].translated(0,-sy));
}

void LibraryView::onMousePress(QMouseEvent * e)
{
    if (!Cards || e->button() != Qt::LeftButton) return;
    const QPoint pos = e->pos() + QPoint(0, verticalScrollBar()->value());
    const int count = Cards->count();
    for (int i = 0; i < count; i++) {
        if (!Rects[i].contains(pos)) continue;
        QRect editR(Rects[i].right()-EditW, Rects[i].bottom()-LineH, EditW, LineH);
        if (editR.contains(pos)) Cards->at(i)->edit();
        else                     Cards->at(i)->play();
        return;
    }
}

void LibraryView::onLeave()
{
    if (HoveredIdx < 0) return;
    const int old = HoveredIdx; HoveredIdx = -1;
    viewport()->update(Rects[old].translated(0, -verticalScrollBar()->value()));
}


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
    BuildStaticUI();
    BuildLibraryGameCards();
    BuildLibraryDynamicUI();
    BuildPackagesDynamicUI();
}

MainWindow::~MainWindow()
{
    qDeleteAll(*LibraryGameCards);
    delete LibraryGameCards;
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
            sortCards();
            View->layoutCards(CardPixelWidth);
        });
        tl->addWidget(b);
    };
    makeSortBtn("Name",   SortMode::Name);
    makeSortBtn("Date",   SortMode::Date);
    makeSortBtn("Series", SortMode::Series);

    tl->addStretch();

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
    LibraryTabWidgetLayout->addWidget(View);

    // ── Packages tab ──────────────────────────────────────────────────────────
    PackagesTabWidget = new QWidget(MainWindowTabWidget);
    PackagesTabWidgetLayout = new QVBoxLayout(PackagesTabWidget);
    PackagesTabWidget->setLayout(PackagesTabWidgetLayout);
    MainWindowTabWidget->addTab(PackagesTabWidget, "Packages");

    QGroupBox * ptb = new QGroupBox(PackagesTabWidget);
    QHBoxLayout * ptbl = new QHBoxLayout(ptb); ptb->setLayout(ptbl);
    PackagesTabWidgetLayout->addWidget(ptb);

    QPushButton * addBtn = new QPushButton("+", ptb);
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

    // ── Store tab (catalog games to import over IPFS) ──────────────────────────
    BuildStoreTab();

    // ── Settings tab ───────────────────────────────────────────────────────────
    BuildSettingsTab();

    // ── IPFS tab ─────────────────────────────────────────────────────────────────
    BuildIpfsTab();

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
        RebuildStoreTab();
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
    SettingsCategoryList->addItem("Runners");
    SettingsCategoryList->addItem("Repositories");
    SettingsCategoryList->addItem("Storage & Paths");
    outer->addWidget(SettingsCategoryList);

    // Right: stacked category forms.
    SettingsStack = new QStackedWidget(SettingsTabWidget);
    outer->addWidget(SettingsStack, 1);

    // Page 0 — Runners (a scroll area we rebuild in place on structural edits).
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

    // Page 1 — Repositories (a scroll area we rebuild in place on add/remove).
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

    // Page 2 — Storage & Paths.
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

    //Runners are the HasRunners packages of the unified LIBRARY (kind is emergent — the same index holds games,
    //runners, and dual game+runner packages). Assemble each entry's manifest from its mirrored dir.
    std::vector<nlohmann::ordered_json> Pkgs;
    if (GlobalConfigJSON->contains("LIBRARY") && (*GlobalConfigJSON)["LIBRARY"].is_array())
        for (const auto & Ent : (*GlobalConfigJSON)["LIBRARY"])
        {
            nlohmann::ordered_json Pkg; std::vector<std::string> Warn;
            if (JSONOps::AssembleManifest(QString::fromStdString(Ent.value("PATH", std::string())), Pkg, Warn)
                && JSONOps::HasRunners(Pkg)) Pkgs.push_back(Pkg);
        }
    if (Pkgs.empty())
    {
        QLabel * none = new QLabel("No runners found in your repositories.", contents);
        none->setStyleSheet("color:#8f98a0;");
        v->addWidget(none);
    }
    for (const auto & Pkg : Pkgs)
    {
        const nlohmann::ordered_json & E = Pkg["RUNNERS"][0];
        std::string rid = E.value("RUNNER_ID", std::string("(unnamed)"));
        QGroupBox * card = new QGroupBox(QString::fromStdString(E.value("NAME", rid)), contents);
        QVBoxLayout * cv = new QVBoxLayout(card); card->setLayout(cv);
        cv->addWidget(new QLabel("<span style='color:#8f98a0;'>RUNNER_ID: " + QString::fromStdString(rid) + "</span>", card));

        // One row per runner VARIANT: TYPE/platforms + import state + Import button.
        for (const std::string & Vid : RunnerWrapper::VariantIds(Pkg))
        {
            const nlohmann::ordered_json V = RunnerWrapper::Variant(Pkg, Vid);
            QString guest;
            if (V.contains("GUEST_PLATFORM") && V["GUEST_PLATFORM"].is_array())
                for (const auto & P : V["GUEST_PLATFORM"])
                    if (P.is_string()) guest += (guest.isEmpty() ? "" : ", ") + QString::fromStdString(std::string(P));
            const QString Desc = QString::fromStdString(Vid) + "  ·  " + QString::fromStdString(V.value("TYPE", std::string()))
                               + "  ·  " + QString::fromStdString(V.value("HOST_PLATFORM", std::string())) + " → [" + guest + "]";

            const bool Ships     = RunnerWrapper::ShipsBuild(Pkg, Vid);
            const bool Imported = RunnerWrapper::IsImported(Pkg, Vid);
            QHBoxLayout * row = new QHBoxLayout();
            row->addWidget(new QLabel(Desc, card), 1);
            QLabel * st = new QLabel(card);
            if (!Ships)          st->setText("<span style='color:#8f98a0;'>built-in</span>");
            else if (Imported)  st->setText("<span style='color:#5fb55f;'>Imported</span>");
            else                 st->setText("<span style='color:#c0726a;'>Not imported</span>");
            row->addWidget(st);
            if (Ships && !Imported && IpfsWrapper::Available())
            {
                QPushButton * btn = new QPushButton("Import", card);
                connect(btn, &QPushButton::clicked, this, [this, Pkg, Vid, btn, st]{
                    btn->setEnabled(false); btn->setText("Importing…");
                    st->setText("<span style='color:#c6a15f;'>Importing… (see IPFS tab)</span>");
                    std::thread([this, Pkg, Vid]{
                        std::string Err;
                        bool Ok = ContainerWrapper::ImportRunner(*GlobalConfigJSON, Pkg, Vid, &Err);
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
        }
        v->addWidget(card);
    }
    v->addStretch(1);
}

// Repositories settings page — add/remove the git repos that share dehydrated packages.
// Each Settings.Repositories[] entry is {NAME, PATH:<git url>}, cloned to ~/.VidyaGod/DOWNLOADS/<name>
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
        "Cloned into ~/.VidyaGod/DOWNLOADS and indexed into the catalog; private repos work if your git is set "
        "up to authenticate non-interactively (SSH key or a stored token).", contents);
    intro->setWordWrap(true);
    intro->setStyleSheet("color:#8f98a0;font-size:9pt;");
    v->addWidget(intro);

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
            RebuildSettingsReposPage(); RebuildSettingsRunnersPage(); RebuildStoreTab();
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
            ContainerWrapper::SyncRepositories(*GlobalConfigJSON);   // mutates LIBRARY/RUNNERS
            QMetaObject::invokeMethod(this, [this]{
                SaveGlobalConfigJSON();
                RebuildSettingsReposPage(); RebuildSettingsRunnersPage(); RebuildStoreTab(); RebuildDynamicUI();
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
    for (const auto & Pkg : ContainerWrapper::CatalogPackages(gc))
    {
        const std::string PkgName = Pkg.value("PACKAGENAME", Pkg.value("PACKAGEUID", std::string()));
        //Content layers → grouped under their package.
        if (Pkg.contains("COMPONENTS") && Pkg["COMPONENTS"].is_array())
        for (const auto & C : Pkg["COMPONENTS"])
        {
            const std::string CompName = C.value("NAME", std::string());
            if (!C.contains("SUBCOMPONENTS") || !C["SUBCOMPONENTS"].is_array()) continue;
            for (const auto & S : C["SUBCOMPONENTS"])
            {
                const std::string T = S.value("TYPE", std::string());
                if (T != "VFSZipLayer" && T != "VFSDirLayer" && T != "VFSFileLayer") continue;
                if (!S.contains("SOURCE") || !S["SOURCE"].is_object() || S["SOURCE"].value("TYPE", std::string()) != "ipfs") continue;
                const std::string Cid = S["SOURCE"].value("CID", std::string());
                if (Cid.empty()) continue;
                std::string Label = PkgName;                                   // default: package name
                if (!CompName.empty() && CompName.find(PkgName) == std::string::npos)   // component adds info
                    Label = PkgName.empty() ? CompName : (PkgName + " — " + CompName);
                else if (PkgName.empty() && !CompName.empty())
                    Label = CompName;
                Labels.insert(QString::fromStdString(Cid), QString::fromStdString(Label));
                if (OutPackages)
                    OutPackages->insert(QString::fromStdString(Cid),
                                        QString::fromStdString(PkgName.empty() ? std::string("(unnamed package)") : PkgName));
            }
        }

        //Cover-art CIDs → a single "Assets" group (separate from game content), labeled by package/game.
        if (Pkg.contains("GAMES") && Pkg["GAMES"].is_array())
        for (const auto & G : Pkg["GAMES"])
        {
            if (!G.is_object()) continue;
            const std::string GameName = G.value("TITLE", G.value("GAMEID", std::string()));
            auto consider = [&](const nlohmann::ordered_json & Holder)
            {
                if (!Holder.contains("COVER") || !Holder["COVER"].is_object()) return;
                const auto & Cv = Holder["COVER"];
                if (!Cv.contains("SOURCE") || !Cv["SOURCE"].is_object() || Cv["SOURCE"].value("TYPE", std::string()) != "ipfs") return;
                const std::string Cid = Cv["SOURCE"].value("CID", std::string());
                if (Cid.empty()) return;
                const std::string Base = PkgName.empty() ? GameName : PkgName;
                const std::string Label = (GameName.empty() || GameName == Base) ? (Base + " — cover")
                                                                                 : (Base + " — " + GameName + " cover");
                Labels.insert(QString::fromStdString(Cid), QString::fromStdString(Label));
                if (OutPackages) OutPackages->insert(QString::fromStdString(Cid), QStringLiteral("Assets"));
            };
            if (G.contains("METADATA") && G["METADATA"].is_object()) consider(G["METADATA"]);
            consider(G);
        }
    }
    return Labels;
}

void MainWindow::BuildStoreTab()
{
    StoreTabWidget = new QWidget(MainWindowTabWidget);
    QVBoxLayout * sl = new QVBoxLayout(StoreTabWidget); StoreTabWidget->setLayout(sl);
    MainWindowTabWidget->addTab(StoreTabWidget, "Store");

    StoreScroll = new QScrollArea(StoreTabWidget);
    StoreScroll->setWidgetResizable(true);
    sl->addWidget(StoreScroll, 1);
    RebuildStoreTab();
}

//Lists every catalog game that isn't imported yet, with an Install button that fetches its content over IPFS
//and adds it to the library. Mirrors RebuildSettingsRunnersPage (runner imports).
void MainWindow::RebuildStoreTab()
{
    if (!StoreScroll) return;
    if (StoreScroll->widget()) StoreScroll->widget()->deleteLater();

    QWidget * contents = new QWidget(StoreScroll);
    QVBoxLayout * v = new QVBoxLayout(contents); contents->setLayout(v);
    StoreScroll->setWidget(contents);

    QLabel * intro = new QLabel(
        "Games shared in your repositories. Import one to download its content over IPFS and add it to your "
        "library. (Its runner is fetched on first launch.)", contents);
    intro->setWordWrap(true);
    intro->setStyleSheet("color:#8f98a0;font-size:9pt;");
    v->addWidget(intro);

    //The Store lists the UN-HYDRATED games of the library — entries synced/mirrored from a repo whose content
    //hasn't been downloaded yet. Importing fetches their content in place (into the mirror dir = Dir).
    int shown = 0;
    const auto & Lib = (*GlobalConfigJSON)["LIBRARY"];
    for (int li = 0; li < (Lib.is_array() ? (int)Lib.size() : 0); li++)
    {
        const std::string Dir = Lib[li].value("PATH", std::string());            // the package's managed mirror dir
        nlohmann::ordered_json Pkg; std::vector<std::string> Warn;
        if (!JSONOps::AssembleManifest(QString::fromStdString(Dir), Pkg, Warn)) continue;
        if (!JSONOps::HasGames(Pkg)) continue;
        if (ContainerWrapper::PackageHydrated(Pkg, Dir)) continue;                // already downloaded → Library tab
        if (ContainerWrapper::PackageIpfsCids(Pkg).empty()) continue;            // nothing fetchable over IPFS
        shown++;

        const std::string Name = Pkg.value("PACKAGENAME", Pkg.value("PACKAGEUID", std::string("(unnamed)")));
        const std::string Ver  = Pkg.value("PACKAGEVERSION", std::string());
        QGroupBox * card = new QGroupBox(QString::fromStdString(Name + (Ver.empty() ? "" : ("  ·  v" + Ver))), contents);
        QHBoxLayout * cardRow = new QHBoxLayout(card); card->setLayout(cardRow);

        //Lazy cover thumbnail (local-first, else IPFS-cached; a background fetch refreshes the Store on arrival).
        QLabel * cover = new QLabel(card);
        cover->setFixedSize(56, 84);
        cover->setScaledContents(true);
        cover->setStyleSheet("background:palette(mid);border:1px solid palette(dark);");
        const nlohmann::ordered_json * CoverNode = nullptr;
        if (Pkg.contains("GAMES") && Pkg["GAMES"].is_array() && !Pkg["GAMES"].empty())
        {
            const auto & G0 = Pkg["GAMES"][0];
            if (G0.contains("METADATA") && G0["METADATA"].is_object() && G0["METADATA"].contains("COVER")) CoverNode = &G0["METADATA"]["COVER"];
            else if (G0.contains("COVER")) CoverNode = &G0["COVER"];
        }
        if (CoverNode)
        {
            const QString CovPath = CoverCache::instance()->resolve(*CoverNode, QString::fromStdString(Dir));
            if (!CovPath.isEmpty()) { QPixmap pm(CovPath); if (!pm.isNull()) cover->setPixmap(pm); }
        }
        cardRow->addWidget(cover);
        QVBoxLayout * cv = new QVBoxLayout(); cardRow->addLayout(cv, 1);

        QString titles;
        if (Pkg.contains("GAMES") && Pkg["GAMES"].is_array())
            for (const auto & G : Pkg["GAMES"])
                titles += (titles.isEmpty() ? "" : ", ")
                        + QString::fromStdString(G.value("TITLE", G.value("GAMEID", std::string())));
        cv->addWidget(new QLabel("<span style='color:#8f98a0;'>" + titles + "</span>", card));

        const int CidCount = (int)ContainerWrapper::PackageIpfsCids(Pkg).size();
        QHBoxLayout * row = new QHBoxLayout();
        row->addWidget(new QLabel(QString("%1 content layer(s) over IPFS").arg(CidCount), card), 1);
        if (IpfsWrapper::Available())
        {
            QPushButton * btn = new QPushButton("Import", card);
            const nlohmann::ordered_json PkgCopy = Pkg;   // capture by value for the worker
            const std::string DirCopy = Dir;
            connect(btn, &QPushButton::clicked, this, [this, PkgCopy, DirCopy, btn]{
                btn->setEnabled(false); btn->setText("Importing…");
                // Fetch content on a worker thread (the slow part); register in LIBRARY on the GUI thread.
                std::thread([this, PkgCopy, DirCopy]{
                    std::string Err; bool Ok = true;
                    for (const std::string & Cid : ContainerWrapper::PackageIpfsCids(PkgCopy))
                        if (IpfsWrapper::FetchSync(Cid, &Err).empty()) { Ok = false; break; }
                    QMetaObject::invokeMethod(this, [this, Ok, Err, PkgCopy, DirCopy]{
                        if (Ok)
                        {
                            std::string E2;   // content is now cached → ImportPackage's fetches are instant cache hits
                            if (ContainerWrapper::ImportPackage(*GlobalConfigJSON, PkgCopy, DirCopy, &E2))
                            { SaveGlobalConfigJSON(); RebuildDynamicUI(); }
                            else QMessageBox::warning(this, "Import failed", QString::fromStdString(E2));
                        }
                        else QMessageBox::warning(this, "Import failed", QString::fromStdString(Err));
                        RebuildStoreTab(); RefreshIpfsTab();
                    }, Qt::QueuedConnection);
                }).detach();
            });
            row->addWidget(btn);
        }
        else
        {
            QLabel * need = new QLabel("install Kubo to download", card);
            need->setStyleSheet("color:#8f98a0;font-style:italic;");
            row->addWidget(need);
        }
        cv->addLayout(row);
        v->addWidget(card);
    }
    if (shown == 0)
    {
        QLabel * none = new QLabel("No new games available to import.", contents);
        none->setStyleSheet("color:#8f98a0;");
        v->addWidget(none);
    }
    v->addStretch(1);
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

    // Periodic status/pin refresh.
    IpfsRefreshTimer = new QTimer(this);
    connect(IpfsRefreshTimer, &QTimer::timeout, this, &MainWindow::RefreshIpfsTab);
    IpfsRefreshTimer->start(5000);
    RefreshIpfsTab();
}

void MainWindow::RefreshIpfsTab()
{
    if (!IpfsWrapper::Available() || !IpfsStatusLabel) return;

    const bool Daemon = IpfsWrapper::DaemonRunning();
    QString S = QString("Daemon: %1").arg(Daemon
        ? "<span style='color:#5fb55f;'>running</span>"
        : "<span style='color:#c0726a;'>stopped — run <code>ipfs daemon</code> to seed</span>");
    if (Daemon) S += QString("   •   Peers: %1").arg(IpfsWrapper::PeerCount());
    const QString Repo = QString::fromStdString(IpfsWrapper::RepoSizeHuman());
    if (!Repo.isEmpty()) S += QString("   •   Repo: %1").arg(Repo);
    IpfsStatusLabel->setText(S);

    if (!IpfsPins) return;
    IpfsCidLabels = BuildCidLabels(*GlobalConfigJSON, &IpfsCidPackages);   // keep names current with the catalog
    const std::vector<IpfsWrapper::PinEntry> Pins = IpfsWrapper::Pins();

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

void MainWindow::BuildLibraryGameCards()
{
    qDeleteAll(*LibraryGameCards); LibraryGameCards->clear();
    for (int i = 0; i < (int)(*GlobalConfigJSON)["LIBRARY"].size(); i++) {
        nlohmann::ordered_json pm;
        std::vector<std::string> AsmWarn;
        const std::string LibPath = (*GlobalConfigJSON)["LIBRARY"][i].value("PATH", std::string());
        if (!JSONOps::AssembleManifest(QString::fromStdString(LibPath), pm, AsmWarn)) continue;
        //Only packages with games show in the library (the archetype). A pure-dependency or
        //runner-only package contributes no cards.
        if (!JSONOps::HasGames(pm)) continue;
        //Only HYDRATED games appear in the Library; un-hydrated (synced-but-not-downloaded) ones live in the Store.
        if (!ContainerWrapper::PackageHydrated(pm, LibPath)) continue;
        for (int j = 0; j < (int)pm["GAMES"].size(); j++) {
            std::string sid = pm["GAMES"][j].contains("GAMEID") &&
                !pm["GAMES"][j]["GAMEID"].is_null()
                ? std::string(pm["GAMES"][j]["GAMEID"]) : "";
            auto * c = new LibraryGameCard(GlobalConfigJSON, i, sid);
            c->InitializeClassVariables();
            LibraryGameCards->append(c);
        }
    }
}

void MainWindow::sortCards()
{
    auto & cards = *LibraryGameCards;
    switch (CurrentSort) {
    case SortMode::Name:
        std::sort(cards.begin(), cards.end(),
            [](auto * a, auto * b){ return a->SortTitle < b->SortTitle; });
        View->setSeriesGroups({});
        break;

    case SortMode::Date:
        std::sort(cards.begin(), cards.end(),
            [](auto * a, auto * b){ return a->SortDate < b->SortDate; });
        View->setSeriesGroups({});
        break;

    case SortMode::Series: {
        // Games without a series sort last, alphabetically by title within series
        std::sort(cards.begin(), cards.end(), [](auto * a, auto * b) {
            bool aNoSeries = a->SeriesName.isEmpty();
            bool bNoSeries = b->SeriesName.isEmpty();
            if (aNoSeries != bNoSeries) return bNoSeries; // series before no-series
            if (aNoSeries && bNoSeries) return a->SortTitle < b->SortTitle;
            return a->SortSeriesKey < b->SortSeriesKey;
        });

        // Build series groups with deterministic colors from the series name hash
        QVector<LibraryView::SeriesGroup> groups;
        const int n = cards.count();
        int start = 0;
        while (start < n) {
            const QString & sName = cards[start]->SeriesName;
            int end = start;
            while (end + 1 < n && cards[end + 1]->SeriesName == sName) ++end;
            if (!sName.isEmpty()) {
                uint h = qHash(sName);
                LibraryView::SeriesGroup g;
                g.first = start; g.last = end; g.name = sName;
                g.color = QColor::fromHsv(static_cast<int>(h % 360), 55, 90, 55);
                groups.append(g);
            }
            start = end + 1;
        }
        View->setSeriesGroups(groups);
        break;
    }
    }
}

void MainWindow::BuildLibraryDynamicUI()
{
    sortCards();
    View->setCards(LibraryGameCards);
    View->prescaleCovers(CardPixelWidth);
    View->layoutCards(CardPixelWidth);
}

void MainWindow::BuildPackagesDynamicUI()
{
    if (PackagesScrollArea->widget()) PackagesScrollArea->widget()->deleteLater();
    QWidget * w = new QWidget(PackagesScrollArea);
    PackagesScrollArea->setWidget(w);
    QGridLayout * g = new QGridLayout(w); w->setLayout(g);
    for (int i = 0; i < (int)(*GlobalConfigJSON)["LIBRARY"].size(); i++) {
        g->addWidget(new QLabel(QString::fromStdString((*GlobalConfigJSON)["LIBRARY"][i]["PACKAGENAME"]),w),i,0);
        g->addWidget(new QLabel(QString::fromStdString((*GlobalConfigJSON)["LIBRARY"][i]["PACKAGEUID"]),w),i,1);
        g->addWidget(new QLabel(QString::fromStdString((*GlobalConfigJSON)["LIBRARY"][i]["PACKAGEVERSION"]),w),i,2);
        QPushButton * rb = new QPushButton("Remove", w);
        QObject::connect(rb, &QPushButton::clicked, this, [this,i]{
            // A managed import (under the library folder) → delete its hydrated copy. A local/portable package
            // added from elsewhere → only drop the reference (never touch the user's own files).
            const std::string Path = (*GlobalConfigJSON)["LIBRARY"][i].value("PATH", std::string());
            const std::string LibRoot = ContainerWrapper::LibraryRootDir(*GlobalConfigJSON);
            std::error_code Ec;
            if (!Path.empty() && !LibRoot.empty())
            {
                const std::string P = std::filesystem::weakly_canonical(std::filesystem::path(Path), Ec).string();
                const std::string R = std::filesystem::weakly_canonical(std::filesystem::path(LibRoot), Ec).string();
                if (P.rfind(R + "/", 0) == 0) std::filesystem::remove_all(P, Ec); // P strictly under the library root
            }
            (*GlobalConfigJSON)["LIBRARY"].erase(i); RebuildDynamicUI(); SaveGlobalConfigJSON();
        });
        g->addWidget(rb, i, 3);
    }
    g->setRowStretch(g->rowCount(), 1);
}

void MainWindow::RebuildDynamicUI()
{
    BuildLibraryGameCards(); BuildLibraryDynamicUI(); BuildPackagesDynamicUI();
}

void MainWindow::RefreshPackage(const QString & PackagePath)
{
    const QString Want = QDir::cleanPath(PackagePath);

    // Locate the live MainWindow (the editor that emitted may have been opened from anywhere).
    MainWindow * Self = nullptr;
    for (QWidget * W : QApplication::topLevelWidgets())
        if (auto * MW = qobject_cast<MainWindow *>(W)) { Self = MW; break; }

    // Re-render every game card for this package IN PLACE (re-assemble manifest + re-derive title/cover).
    // Never delete cards here — an open prelaunch borrows a card's MANIFESTJSON pointer.
    if (Self && Self->LibraryGameCards)
    {
        bool Any = false;
        for (LibraryGameCard * Card : *Self->LibraryGameCards)
            if (Card && QDir::cleanPath(QString::fromStdString(Card->PackagePath.string())) == Want)
            { Card->InitializeClassVariables(); Any = true; }
        if (Any && Self->View) Self->View->refreshVisuals(); // re-scale + repaint in place (keeps layout)
    }

    // Reload any open prelaunch dialog(s) for this package.
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
        nlohmann::ordered_json m;
        std::vector<std::string> AsmWarn;
        if (!JSONOps::AssembleManifest(path, m, AsmWarn)) { skipped++; continue; }
        if (m.contains("__VG_ERRORS__")) { // conflicting identities — not a single valid package
            LogWarn("MainWindow", "Skipping " + path.toStdString() + ": conflicting package identities.");
            skipped++; continue;
        }
        if (!JSONOps::HasGames(m)) { // runner-only / pure-dependency package — not a library entry
            LogWarn("MainWindow", "Skipping " + path.toStdString() + ": no games (not a library package).");
            skipped++; continue;
        }
        bool dup=false;
        for (auto & e : (*GlobalConfigJSON)["LIBRARY"])
            if (e["PACKAGEUID"]==m["PACKAGEUID"]) { dup=true; skipped++; break; }
        if (dup) continue;
        nlohmann::ordered_json slim;
        slim["PACKAGEUID"]=m["PACKAGEUID"]; slim["PACKAGENAME"]=m["PACKAGENAME"];
        slim["PACKAGEVERSION"]=m["PACKAGEVERSION"]; slim["PATH"]=path.toStdString();
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
