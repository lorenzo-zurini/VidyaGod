#include "mainwindow.h"
#include "commonutils.h"
#include "packageeditor.h"
#include "jsonoperations.h"
#include "filesystemoperations.h"
#include "prelaunchwindow.h"
#include "ipfswrapper.h"
#include "runnerwrapper.h"

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
#include <QHBoxLayout>
#include <QVBoxLayout>


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

    std::string cov =
        (Meta.is_object() && Meta.contains("COVER") && Meta["COVER"].is_string())
        ? std::string(Meta["COVER"])
        : ((*MANIFESTJSON)["GAMES"][idx].contains("COVER")
           ? std::string((*MANIFESTJSON)["GAMES"][idx]["COVER"]) : "");
    if (!cov.empty())
        CoverOriginal.load(QDir::cleanPath(
            QString::fromStdString(PackagePath.string()) + "/" +
            QString::fromStdString(cov)));
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

    //Gate: the runner this game uses must be installed (IPFS builds are fetched ahead of time, never at
    //launch). If a required build/DEFPREFIX is missing, offer to install the runner instead of launching.
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

            const QString Rid = QString::fromStdString(CP.RunnerID);
            if (RunnerPkg.is_null() || !RunnerWrapper::ShipsBuild(RunnerPkg))
            { QMessageBox::warning(nullptr, "Runner unavailable",
                  "This game needs runner '" + Rid + "', which isn't installed and can't be fetched."); return; }
            if (!IpfsWrapper::Available())
            { QMessageBox::warning(nullptr, "Kubo required",
                  "Runner '" + Rid + "' must be downloaded over IPFS, but Kubo (ipfs) isn't installed."); return; }
            if (QMessageBox::question(nullptr, "Install runner?",
                    "This game needs runner '" + Rid + "', which isn't installed yet.\n\n"
                    "Download and install it now? (progress shows in the IPFS tab)") != QMessageBox::Yes)
                return;
            std::thread([GC = GlobalConfigJSON, RunnerPkg, Rid]{
                std::string Err;
                bool Ok = ContainerWrapper::InstallRunner(*GC, RunnerPkg, &Err);
                QMetaObject::invokeMethod(qApp, [Ok, Err, Rid]{
                    if (Ok) QMessageBox::information(nullptr, "Runner installed",
                                "Runner '" + Rid + "' installed — press Play to launch.");
                    else    QMessageBox::warning(nullptr, "Runner install failed", QString::fromStdString(Err));
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

    // ── Settings tab ───────────────────────────────────────────────────────────
    BuildSettingsTab();

    // ── IPFS tab ─────────────────────────────────────────────────────────────────
    BuildIpfsTab();
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

    // Page 1 — Storage & Paths.
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
    // IPFS) must be installed (build fetched + DEFPREFIX generated) before games can use it.
    QLabel * intro = new QLabel(
        "Runners are packages from your repositories. A runner that ships its own build (e.g. Proton over "
        "IPFS) must be installed before games can use it.", contents);
    intro->setWordWrap(true);
    intro->setStyleSheet("color:#8f98a0;font-size:9pt;");
    v->addWidget(intro);

    std::vector<nlohmann::ordered_json> Pkgs;
    for (const auto & Pkg : ContainerWrapper::CatalogPackages(*GlobalConfigJSON))
        if (JSONOps::HasRunners(Pkg)) Pkgs.push_back(Pkg);
    if (Pkgs.empty())
    {
        QLabel * none = new QLabel("No runners found in your repositories.", contents);
        none->setStyleSheet("color:#8f98a0;");
        v->addWidget(none);
    }
    for (const auto & Pkg : Pkgs)
    {
        const nlohmann::ordered_json & R = Pkg["RUNNERS"][0];
        std::string rid = R.value("RUNNER_ID", std::string("(unnamed)"));
        QGroupBox * card = new QGroupBox(QString::fromStdString(R.value("NAME", rid)), contents);
        QVBoxLayout * cv = new QVBoxLayout(card); card->setLayout(cv);
        QFormLayout * f = new QFormLayout(); cv->addLayout(f);

        QString guest;
        if (R.contains("GUEST_PLATFORM") && R["GUEST_PLATFORM"].is_array())
            for (const auto & P : R["GUEST_PLATFORM"])
                if (P.is_string()) guest += (guest.isEmpty() ? "" : ", ") + QString::fromStdString(std::string(P));
        f->addRow("RUNNER_ID:",      new QLabel(QString::fromStdString(rid), card));
        f->addRow("TYPE:",           new QLabel(QString::fromStdString(R.value("TYPE", std::string())), card));
        f->addRow("GUEST_PLATFORM:", new QLabel(guest, card));

        // Install state + action.
        const bool Ships     = RunnerWrapper::ShipsBuild(Pkg);
        const bool Installed = RunnerWrapper::IsInstalled(Pkg);
        QHBoxLayout * row = new QHBoxLayout();
        QLabel * st = new QLabel(card);
        if (!Ships)          st->setText("<span style='color:#8f98a0;'>built-in (no install needed)</span>");
        else if (Installed)  st->setText("<span style='color:#5fb55f;'>Installed</span>");
        else                 st->setText("<span style='color:#c0726a;'>Not installed</span>");
        row->addWidget(st, 1);
        if (Ships && !Installed && IpfsWrapper::Available())
        {
            QPushButton * btn = new QPushButton("Install", card);
            connect(btn, &QPushButton::clicked, this, [this, Pkg, btn, st]{
                btn->setEnabled(false); btn->setText("Installing…");
                st->setText("<span style='color:#c6a15f;'>Installing… (see IPFS tab)</span>");
                std::thread([this, Pkg]{
                    std::string Err;
                    bool Ok = ContainerWrapper::InstallRunner(*GlobalConfigJSON, Pkg, &Err);
                    QMetaObject::invokeMethod(this, [this, Ok, Err]{
                        if (!Ok) QMessageBox::warning(this, "Runner install failed", QString::fromStdString(Err));
                        RebuildSettingsRunnersPage();
                        RefreshIpfsTab();
                    }, Qt::QueuedConnection);
                }).detach();
            });
            row->addWidget(btn);
        }
        else if (Ships && !Installed)
        {
            QLabel * need = new QLabel("install Kubo to fetch", card);
            need->setStyleSheet("color:#8f98a0;font-style:italic;");
            row->addWidget(need);
        }
        cv->addLayout(row);
        v->addWidget(card);
    }
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

    // Transfers (live fetches from the launch worker).
    QGroupBox * txBox = new QGroupBox("Transfers", IpfsTabWidget);
    QVBoxLayout * txl = new QVBoxLayout(txBox);
    IpfsTransfers = new QTableWidget(0, 3, txBox);
    IpfsTransfers->setHorizontalHeaderLabels({"CID", "Progress", "Status"});
    IpfsTransfers->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    IpfsTransfers->setEditTriggers(QAbstractItemView::NoEditTriggers);
    IpfsTransfers->setSelectionMode(QAbstractItemView::NoSelection);
    IpfsTransfers->verticalHeader()->setVisible(false);
    txl->addWidget(IpfsTransfers);
    v->addWidget(txBox, 1);

    // Seeded content (pinned CIDs).
    QGroupBox * pinBox = new QGroupBox("Seeded content (pinned)", IpfsTabWidget);
    QVBoxLayout * pl = new QVBoxLayout(pinBox);
    IpfsPins = new QTableWidget(0, 2, pinBox);
    IpfsPins->setHorizontalHeaderLabels({"CID", ""});
    IpfsPins->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    IpfsPins->setEditTriggers(QAbstractItemView::NoEditTriggers);
    IpfsPins->setSelectionMode(QAbstractItemView::NoSelection);
    IpfsPins->verticalHeader()->setVisible(false);
    pl->addWidget(IpfsPins);
    v->addWidget(pinBox, 1);

    // Live transfer notifications (marshalled onto the GUI thread by IpfsManager).
    IpfsManager * mgr = IpfsManager::instance();
    connect(mgr, &IpfsManager::transferStarted, this, [this](QString cid) {
        if (!IpfsTransfers) return;
        int row = IpfsTransferRows.value(cid, -1);
        if (row < 0) {
            row = IpfsTransfers->rowCount(); IpfsTransfers->insertRow(row);
            IpfsTransferRows.insert(cid, row);
            IpfsTransfers->setItem(row, 0, new QTableWidgetItem(cid));
            QProgressBar * bar = new QProgressBar(); bar->setRange(0, 0); // indeterminate until first %
            IpfsTransfers->setCellWidget(row, 1, bar);
            IpfsTransfers->setItem(row, 2, new QTableWidgetItem("Fetching…"));
        } else if (IpfsTransfers->item(row, 2)) {
            IpfsTransfers->item(row, 2)->setText("Fetching…");
        }
    });
    connect(mgr, &IpfsManager::transferProgress, this, [this](QString cid, double pct) {
        int row = IpfsTransferRows.value(cid, -1);
        if (row < 0 || !IpfsTransfers) return;
        if (auto * bar = qobject_cast<QProgressBar*>(IpfsTransfers->cellWidget(row, 1))) {
            bar->setRange(0, 100); bar->setValue(int(pct + 0.5));
        }
    });
    connect(mgr, &IpfsManager::transferFinished, this, [this](QString cid, bool ok) {
        int row = IpfsTransferRows.value(cid, -1);
        if (row >= 0 && IpfsTransfers) {
            if (auto * bar = qobject_cast<QProgressBar*>(IpfsTransfers->cellWidget(row, 1))) {
                bar->setRange(0, 100); if (ok) bar->setValue(100);
            }
            if (IpfsTransfers->item(row, 2)) IpfsTransfers->item(row, 2)->setText(ok ? "Done" : "Failed");
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
    const std::vector<IpfsWrapper::PinEntry> Pins = IpfsWrapper::Pins();
    IpfsPins->setRowCount(int(Pins.size()));
    for (int i = 0; i < int(Pins.size()); ++i)
    {
        const QString Cid = QString::fromStdString(Pins[i].Cid);
        IpfsPins->setItem(i, 0, new QTableWidgetItem(Cid));
        QWidget * cell = new QWidget();
        QHBoxLayout * cl = new QHBoxLayout(cell); cl->setContentsMargins(2,2,2,2); cl->setSpacing(4);
        QPushButton * copyBtn  = new QPushButton("Copy CID", cell);
        QPushButton * unpinBtn = new QPushButton("Unpin", cell);
        connect(copyBtn,  &QPushButton::clicked, this, [Cid]{ QApplication::clipboard()->setText(Cid); });
        connect(unpinBtn, &QPushButton::clicked, this, [this, Cid]{ IpfsWrapper::Unpin(Cid.toStdString()); RefreshIpfsTab(); });
        cl->addWidget(copyBtn); cl->addWidget(unpinBtn);
        IpfsPins->setCellWidget(i, 1, cell);
    }
    IpfsPins->resizeColumnToContents(1);
}

void MainWindow::BuildLibraryGameCards()
{
    qDeleteAll(*LibraryGameCards); LibraryGameCards->clear();
    for (int i = 0; i < (int)(*GlobalConfigJSON)["LIBRARY"].size(); i++) {
        nlohmann::ordered_json pm;
        std::vector<std::string> AsmWarn;
        if (!JSONOps::AssembleManifest(QString::fromStdString(
                std::string((*GlobalConfigJSON)["LIBRARY"][i]["PATH"])), pm, AsmWarn)) continue;
        //Only packages with games show in the library (the archetype). A pure-dependency or
        //runner-only package contributes no cards.
        if (!JSONOps::HasGames(pm)) continue;
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
