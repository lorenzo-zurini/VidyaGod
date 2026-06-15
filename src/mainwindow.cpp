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

// True if IPFS can fetch content right NOW (binary present AND a daemon is running). Otherwise shows the reason
// and returns false. VidyaGod never starts the daemon — it only detects and tells the user to.
static bool IpfsFetchReady(QWidget * parent)
{
    if (!IpfsWrapper::Available())
    {
        QMessageBox::warning(parent, "Kubo required",
            "Downloading content over IPFS needs Kubo (the `ipfs` CLI), which isn't installed.");
        return false;
    }
    if (!IpfsWrapper::DaemonRunning())
    {
        QMessageBox::warning(parent, "IPFS daemon not running",
            "Your IPFS daemon isn't running, so content can't be downloaded.\n\n"
            "Start it with `ipfs daemon`, or enable the ipfs systemd user service, then try again.");
        return false;
    }
    return true;
}

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
            nlohmann::ordered_json RunnerPkg; std::string RunnerDir;
            for (auto & PD : ContainerWrapper::CatalogPackagesWithDir(*GlobalConfigJSON))
                if (JSONOps::HasRunners(PD.first))
                    for (const auto & R : PD.first["RUNNERS"])
                        if (R.value("RUNNER_ID", std::string()) == CP.RunnerID) { RunnerPkg = PD.first; RunnerDir = PD.second; break; }

            const std::string Vid = CP.RunnerVariantID;
            const QString Rid = QString::fromStdString(CP.RunnerID + (Vid.empty() ? "" : (":" + Vid)));
            if (RunnerPkg.is_null() || !RunnerWrapper::ShipsBuild(RunnerPkg, Vid))
            { QMessageBox::warning(nullptr, "Runner unavailable",
                  "This game needs runner '" + Rid + "', which isn't imported and can't be fetched."); return; }
            if (!IpfsFetchReady(nullptr)) return;
            if (QMessageBox::question(nullptr, "Import runner?",
                    "This game needs runner '" + Rid + "', which isn't imported yet.\n\n"
                    "Download and import it now? (progress shows in the IPFS tab)") != QMessageBox::Yes)
                return;
            std::thread([GC = GlobalConfigJSON, RunnerPkg, RunnerDir, Vid, Rid]{
                std::string Err;
                bool Ok = ContainerWrapper::ImportRunner(*GC, RunnerPkg, RunnerDir, Vid, &Err);
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
    setFocusPolicy(Qt::StrongFocus);   // accept keyboard focus for arrow-key navigation
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
    SelectedIdx  = -1;
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

void LibraryView::setGroups(const QVector<Group> & groups)
{
    Groups = groups;
    HeaderRects.resize(groups.size());
    LastCols = -1;   // force a relayout (group structure changed)
    layoutCards(CardW > 0 ? CardW : 185);
    viewport()->update();
}

void LibraryView::setHoverAction(const QString & label, bool emitDownload)
{
    HoverLabel = label;
    EmitDownload = emitDownload;
    ShowEditCorner = !emitDownload;   // Available cards have no "···" edit corner
}

void LibraryView::setEmptyMessage(const QString & message)
{
    EmptyMessage = message;
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

    // Always refresh the scrollbar from the cached content height — a vertical-only resize changes vH but not
    // the column layout, so it would otherwise be skipped below.
    verticalScrollBar()->setRange(0, qMax(0, ContentH - vH));
    verticalScrollBar()->setPageStep(vH);

    // Skip recompute when layout is identical — covers most resize events. (setGroups forces LastCols=-1.)
    if (cols == LastCols && hGap == LastHGap && cardW == CardW) return;

    CardW = cardW; CardH = cardH;
    LastCols = cols; LastHGap = hGap;

    Rects.resize(count);
    int totalH;

    if (Groups.isEmpty())
    {
        // Flat grid (Library tab) — unchanged.
        for (int i = 0; i < count; i++)
            Rects[i] = QRect(hGap + (i % cols) * (cardW + hGap),
                             VPad + (i / cols) * (cardH + MinGap), cardW, cardH);
        const int rows = (count + cols - 1) / cols;
        totalH = VPad + rows * cardH + (rows - 1) * MinGap + VPad;
    }
    else
    {
        // Collapsible sections: a full-width header band before each group; a collapsed group lays out no
        // cards (its members get empty rects, skipped in paint + hit-testing).
        HeaderRects.resize(Groups.size());
        int y = VPad;
        for (int gi = 0; gi < Groups.size(); gi++)
        {
            const Group & g = Groups[gi];
            HeaderRects[gi] = QRect(0, y, vW, HeaderH);
            y += HeaderH;
            const int n = (g.last >= g.first) ? (g.last - g.first + 1) : 0;
            if (!g.collapsed && n > 0)
            {
                for (int k = 0; k < n; k++)
                {
                    const int i = g.first + k;
                    Rects[i] = QRect(hGap + (k % cols) * (cardW + hGap),
                                     y + (k / cols) * (cardH + MinGap), cardW, cardH);
                }
                const int rows = (n + cols - 1) / cols;
                y += rows * cardH + (rows - 1) * MinGap + MinGap;
            }
            else
            {
                for (int k = 0; k < n; k++) Rects[g.first + k] = QRect();   // collapsed → hidden
                y += MinGap;
            }
        }
        totalH = y + VPad;
    }

    ContentH = totalH;
    verticalScrollBar()->setRange(0, qMax(0, totalH - vH));
    verticalScrollBar()->setPageStep(vH);
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

    // Empty state: centered first-run hint, no cards to draw.
    if (Cards->isEmpty() && !EmptyMessage.isEmpty())
    {
        p.setPen(QColor(0x8f, 0x98, 0xa0));
        p.drawText(viewport()->rect().adjusted(40, 0, -40, 0),
                   Qt::AlignCenter | Qt::TextWordWrap, EmptyMessage);
        return;
    }

    p.translate(0, -scrollY);

    // Series group backgrounds — drawn in content coordinates before cards (flat/Library mode only)
    for (auto & g : SeriesGroups) {
        const int topY = Rects[g.first].top()    - MinGap;
        const int botY = Rects[g.last].bottom()  + MinGap;
        const QRect bgR(0, topY, viewport()->width(), botY - topY);
        if (bgR.intersects(cRect)) p.fillRect(bgR, g.color);
    }

    // Collapsible section headers (Available mode) — a band with a chevron + repo name + count.
    for (int gi = 0; gi < Groups.size() && gi < HeaderRects.size(); gi++) {
        const QRect & h = HeaderRects[gi];
        if (!h.intersects(cRect)) continue;
        const Group & g = Groups[gi];
        p.fillRect(h, QColor(0xff,0xff,0xff,16));
        p.fillRect(QRect(h.left(), h.bottom()-1, h.width(), 1), QColor(0,0,0,60));
        p.setPen(QColor(0xc6,0xd4,0xdf,230));
        p.setFont(PlayFont);
        const int n = (g.last >= g.first) ? (g.last - g.first + 1) : 0;
        p.drawText(h.adjusted(12,0,-12,0), Qt::AlignVCenter|Qt::AlignLeft,
                   (g.collapsed ? "▸  " : "▾  ") + g.name + QString("   (%1)").arg(n));
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

        // In-flight import (Available): darken + a centered "Downloading…" label and a progress bar; no hover.
        if (card->Downloading) {
            p.fillRect(r, QColor(0,0,0,150));
            p.setFont(PlayFont); p.setPen(Qt::white);
            const int pct = card->DownloadPercent >= 0 ? (int)(card->DownloadPercent + 0.5) : -1;
            p.drawText(QRect(r.left(), r.top(), r.width(), r.height()/2),
                       Qt::AlignHCenter | Qt::AlignBottom,
                       pct >= 0 ? QString("Downloading…  %1%").arg(pct) : QStringLiteral("Downloading…"));
            // Thin progress bar (indeterminate-full when percent unknown).
            const int bw = qMin(r.width() - 24, 160), bh = 6;
            const QRect bar(r.left() + (r.width()-bw)/2, r.top() + r.height()/2 + 6, bw, bh);
            p.setPen(Qt::NoPen); p.setBrush(QColor(255,255,255,60)); p.drawRoundedRect(bar, 3, 3);
            p.setBrush(QColor(0x4a,0x90,0xd9,235));
            const int fillW = pct >= 0 ? bar.width() * pct / 100 : bar.width();
            p.drawRoundedRect(QRect(bar.left(), bar.top(), fillW, bar.height()), 3, 3);
            p.setFont(TitleFont);
            continue;
        }

        // Keyboard focus ring (drawn whether or not hovered).
        if (i == SelectedIdx) {
            p.setBrush(Qt::NoBrush);
            QPen ring(QColor(0x4a,0x90,0xd9), 3); p.setPen(ring);
            p.drawRoundedRect(r.adjusted(1,1,-2,-2), 4, 4);
        }

        if (i != HoveredIdx) continue;

        // Hover: darken + action button (Play or Download) + title strip
        p.fillRect(r, QColor(0,0,0,110));

        const int btnW = qMax(90, TitleFM.horizontalAdvance(HoverLabel) + 28);
        const QRect btn(r.left()+(r.width()-btnW)/2, r.top()+(r.height()-32)/2-16, btnW, 32);
        p.setBrush(QColor(0x4a,0x90,0xd9,230));
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(btn, 4, 4);
        p.setFont(PlayFont);
        p.setPen(Qt::white);
        p.drawText(btn, Qt::AlignCenter, HoverLabel);

        p.setFont(TitleFont);
        const int textRight = ShowEditCorner ? (EditW+4) : 8;
        const QRect line(r.left(), r.bottom()-LineH, r.width(), LineH);
        p.fillRect(line, QColor(0,0,0,160));
        p.setPen(QColor(0xff,0xff,0xff,220));
        p.drawText(line.adjusted(8,0,-textRight,0),
                   Qt::AlignVCenter|Qt::AlignLeft|Qt::TextSingleLine,
                   card->ElidedTitle);
        if (ShowEditCorner) {
            p.setPen(QColor(0xc6,0xd4,0xdf,200));
            p.drawText(QRect(r.right()-EditW, r.bottom()-LineH, EditW, LineH),
                       Qt::AlignCenter, "···");
        }
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

    // Section header → toggle collapse.
    for (int gi = 0; gi < Groups.size() && gi < HeaderRects.size(); gi++) {
        if (!HeaderRects[gi].contains(pos)) continue;
        Groups[gi].collapsed = !Groups[gi].collapsed;
        emit groupToggled(Groups[gi].name, Groups[gi].collapsed);
        LastCols = -1;                 // force relayout (section heights changed)
        layoutCards(CardW);
        viewport()->update();
        return;
    }

    const int count = Cards->count();
    for (int i = 0; i < count; i++) {
        if (!Rects[i].contains(pos)) continue;          // collapsed cards have empty rects → skipped
        SelectedIdx = i;                                // keep keyboard focus in sync with the click
        if (Cards->at(i)->Downloading) return;          // import in flight — ignore
        if (EmitDownload) { emit downloadRequested(Cards->at(i)); return; }
        QRect editR(Rects[i].right()-EditW, Rects[i].bottom()-LineH, EditW, LineH);
        if (ShowEditCorner && editR.contains(pos)) Cards->at(i)->edit();
        else                                       Cards->at(i)->play();
        return;
    }
}

void LibraryView::onLeave()
{
    if (HoveredIdx < 0) return;
    const int old = HoveredIdx; HoveredIdx = -1;
    viewport()->update(Rects[old].translated(0, -verticalScrollBar()->value()));
}

void LibraryView::keyPressEvent(QKeyEvent * e)
{
    switch (e->key()) {
    case Qt::Key_Left:  moveSelection(-1, 0); return;
    case Qt::Key_Right: moveSelection( 1, 0); return;
    case Qt::Key_Up:    moveSelection( 0,-1); return;
    case Qt::Key_Down:  moveSelection( 0, 1); return;
    case Qt::Key_Return: case Qt::Key_Enter: case Qt::Key_Space:
        activateCard(SelectedIdx); return;
    default: QAbstractScrollArea::keyPressEvent(e);
    }
}

// Geometry-based move: from the selected card's centre, pick the nearest visible card in the requested
// direction (collapsed/empty rects are skipped). Works across the grouped layout's uneven rows.
void LibraryView::moveSelection(int dx, int dy)
{
    if (!Cards || Cards->isEmpty()) return;
    if (SelectedIdx < 0 || SelectedIdx >= Cards->count() || Rects[SelectedIdx].isNull()) {
        for (int i = 0; i < Cards->count(); i++) if (!Rects[i].isNull()) { setSelected(i); return; }
        return;
    }
    const QPoint c = Rects[SelectedIdx].center();
    int best = -1; double bestScore = 1e18;
    for (int i = 0; i < Cards->count(); i++) {
        if (i == SelectedIdx || Rects[i].isNull()) continue;
        const QPoint p = Rects[i].center();
        const int ex = p.x() - c.x(), ey = p.y() - c.y();
        if (dx > 0 && ex <= 0) continue;  if (dx < 0 && ex >= 0) continue;
        if (dy > 0 && ey <= 0) continue;  if (dy < 0 && ey >= 0) continue;
        // distance along the travel axis + a heavy penalty for drifting off it
        const double score = dx != 0 ? (qAbs(ex) + 3.0 * qAbs(ey)) : (qAbs(ey) + 3.0 * qAbs(ex));
        if (score < bestScore) { bestScore = score; best = i; }
    }
    if (best >= 0) setSelected(best);
}

void LibraryView::setSelected(int i)
{
    SelectedIdx = i;
    ensureCardVisible(i);
    viewport()->update();
}

void LibraryView::ensureCardVisible(int i)
{
    if (i < 0 || i >= Rects.size() || Rects[i].isNull()) return;
    const int sy = verticalScrollBar()->value(), vh = viewport()->height();
    if (Rects[i].top() < sy)               verticalScrollBar()->setValue(qMax(0, Rects[i].top() - VPad));
    else if (Rects[i].bottom() > sy + vh)  verticalScrollBar()->setValue(Rects[i].bottom() - vh + VPad);
}

void LibraryView::activateCard(int i)
{
    if (!Cards || i < 0 || i >= Cards->count() || Rects[i].isNull()) return;
    if (Cards->at(i)->Downloading) return;
    if (EmitDownload) emit downloadRequested(Cards->at(i));
    else              Cards->at(i)->play();
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
            BuildLibraryDynamicUI();   // re-sort + re-filter + relayout
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
        BuildLibraryDynamicUI();
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
    std::vector<std::pair<nlohmann::ordered_json, std::string>> Pkgs;          // (manifest, package dir)
    if (GlobalConfigJSON->contains("LIBRARY") && (*GlobalConfigJSON)["LIBRARY"].is_array())
        for (const auto & Ent : (*GlobalConfigJSON)["LIBRARY"])
        {
            const std::string Dir = Ent.value("PATH", std::string());
            nlohmann::ordered_json Pkg; std::vector<std::string> Warn;
            if (JSONOps::AssembleManifest(QString::fromStdString(Dir), Pkg, Warn) && JSONOps::HasRunners(Pkg))
                Pkgs.push_back({Pkg, Dir});
        }
    if (Pkgs.empty())
    {
        QLabel * none = new QLabel("No runners found in your repositories.", contents);
        none->setStyleSheet("color:#8f98a0;");
        v->addWidget(none);
    }
    for (const auto & PD : Pkgs)
    {
        const nlohmann::ordered_json & Pkg = PD.first;
        const std::string & PkgDir = PD.second;
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
            const bool Imported = RunnerWrapper::IsImported(Pkg, PkgDir, Vid);
            const bool Avail    = RunnerWrapper::ExecutableAvailable(V);
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
                connect(btn, &QPushButton::clicked, this, [this, Pkg, PkgDir, Vid, btn, st]{
                    if (!IpfsFetchReady(this)) return;          // need a running daemon to fetch
                    btn->setEnabled(false); btn->setText("Importing…");
                    st->setText("<span style='color:#c6a15f;'>Importing… (see IPFS tab)</span>");
                    std::thread([this, Pkg, PkgDir, Vid]{
                        std::string Err;
                        bool Ok = ContainerWrapper::ImportRunner(*GlobalConfigJSON, Pkg, PkgDir, Vid, &Err);
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
                ContainerWrapper::SyncRepositories(*GlobalConfigJSON);   // git pull each repo + reindex LIBRARY
                QMetaObject::invokeMethod(this, [this]{
                    SaveGlobalConfigJSON();
                    RebuildSettingsReposPage(); RebuildSettingsRunnersPage(); RebuildAvailableTab(); RebuildDynamicUI();
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
            ContainerWrapper::SyncRepositories(*GlobalConfigJSON);   // mutates LIBRARY/RUNNERS
            QMetaObject::invokeMethod(this, [this]{
                SaveGlobalConfigJSON();
                RebuildSettingsReposPage(); RebuildSettingsRunnersPage(); RebuildAvailableTab(); RebuildDynamicUI();
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
    MainWindowTabWidget->addTab(AvailableTabWidget, "Available");

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
            AvailableSort = mode; RebuildAvailableTab();
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
        RebuildAvailableTab();
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
            if (AvailableView) { AvailableView->prescaleCovers(CardPixelWidth); AvailableView->layoutCards(CardPixelWidth); }
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
        const QString Uid = it.value();
        if (pct >= 0) DownloadCidPct[cid] = pct;
        const QStringList & Cids = DownloadUidCids[Uid];
        double Sum = 0; for (const QString & c : Cids) Sum += DownloadCidPct.value(c, 0.0);
        const double Avg = Cids.isEmpty() ? -1.0 : Sum / Cids.size();
        bool Any = false;
        for (LibraryGameCard * card : *AvailableGameCards)
            if (card && card->Downloading
                && QString::fromStdString((*GlobalConfigJSON)["LIBRARY"][card->Game].value("PACKAGEUID", std::string())) == Uid)
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
void MainWindow::RebuildAvailableTab()
{
    if (!AvailableView) return;
    qDeleteAll(*AvailableGameCards); AvailableGameCards->clear();

    // Repo name for a LIBRARY entry (groups un-repo'd local entries under "Local").
    auto RepoOf = [&](int i) -> QString {
        const auto & E = (*GlobalConfigJSON)["LIBRARY"][i];
        const std::string R = E.value("REPO", std::string());
        return R.empty() ? QStringLiteral("Local") : QString::fromStdString(R);
    };

    const auto & Lib = (*GlobalConfigJSON)["LIBRARY"];
    for (int i = 0; i < (Lib.is_array() ? (int)Lib.size() : 0); i++)
    {
        const std::string Dir = Lib[i].value("PATH", std::string());
        nlohmann::ordered_json Pkg; std::vector<std::string> Warn;
        if (!JSONOps::AssembleManifest(QString::fromStdString(Dir), Pkg, Warn)) continue;
        if (!JSONOps::HasGames(Pkg)) continue;
        if (ContainerWrapper::PackageHydrated(Pkg, Dir)) continue;                // downloaded → Library tab
        if (ContainerWrapper::PackageIpfsCids(Pkg).empty()) continue;            // nothing fetchable over IPFS
        const QString PkgUid = QString::fromStdString(Pkg.value("PACKAGEUID", std::string()));
        const bool Busy = DownloadingUids.contains(PkgUid);
        double BusyPct = -1.0;                                                    // carry current progress across rebuilds
        if (Busy) { const QStringList & cs = DownloadUidCids.value(PkgUid);
                    if (!cs.isEmpty()) { double s = 0; for (const QString & c : cs) s += DownloadCidPct.value(c, 0.0); BusyPct = s / cs.size(); } }
        for (int j = 0; j < (int)Pkg["GAMES"].size(); j++)
        {
            std::string sid = (Pkg["GAMES"][j].contains("GAMEID") && !Pkg["GAMES"][j]["GAMEID"].is_null())
                              ? std::string(Pkg["GAMES"][j]["GAMEID"]) : "";
            auto * c = new LibraryGameCard(GlobalConfigJSON, i, sid);
            c->InitializeClassVariables();
            if (!AvailableSearch.isEmpty() && !c->GameTitle.contains(AvailableSearch, Qt::CaseInsensitive))
            { delete c; continue; }                                              // filtered out by search
            c->Downloading = Busy;
            c->DownloadPercent = BusyPct;
            AvailableGameCards->append(c);
        }
    }

    // Sort by (repo, within-group key) so each repo is a contiguous run; then build collapsible groups.
    auto keyOf = [this](LibraryGameCard * a) -> QString {
        switch (AvailableSort) { case SortMode::Date:   return a->SortDate + "|" + a->SortTitle;
                                 case SortMode::Series: return a->SortSeriesKey;
                                 default:               return a->SortTitle; }
    };
    std::sort(AvailableGameCards->begin(), AvailableGameCards->end(), [&](LibraryGameCard * a, LibraryGameCard * b){
        const QString ra = RepoOf(a->Game), rb = RepoOf(b->Game);
        if (ra != rb) return ra < rb;
        return keyOf(a) < keyOf(b);
    });

    QVector<LibraryView::Group> groups;
    const int n = AvailableGameCards->count();
    for (int s = 0; s < n; ) {
        const QString r = RepoOf(AvailableGameCards->at(s)->Game);
        int e = s; while (e + 1 < n && RepoOf(AvailableGameCards->at(e + 1)->Game) == r) ++e;
        groups.append({ r, s, e, AvailableCollapsedRepos.contains(r) });
        s = e + 1;
    }

    AvailableView->setCards(AvailableGameCards);
    AvailableView->setGroups(groups);
    AvailableView->prescaleCovers(CardPixelWidth);
    AvailableView->layoutCards(CardPixelWidth);
}

//Hydrates the package behind an Available card over IPFS (in place, on a worker), then moves it to the Library.
void MainWindow::DownloadAvailable(LibraryGameCard * card)
{
    if (!card) return;
    if (!IpfsFetchReady(this)) return;
    const int i = card->Game;
    if (i < 0 || i >= (int)(*GlobalConfigJSON)["LIBRARY"].size()) return;
    const std::string Dir = (*GlobalConfigJSON)["LIBRARY"][i].value("PATH", std::string());
    nlohmann::ordered_json Pkg; std::vector<std::string> Warn;
    if (!JSONOps::AssembleManifest(QString::fromStdString(Dir), Pkg, Warn)) return;
    const QString Uid = QString::fromStdString(Pkg.value("PACKAGEUID", std::string()));
    if (Uid.isEmpty() || DownloadingUids.contains(Uid)) return;       // already in flight

    DownloadingUids.insert(Uid);
    // Track this package's content CIDs so transfer-progress signals can aggregate onto its card.
    QStringList Cids;
    for (const auto & C : ContainerWrapper::PackageIpfsCids(Pkg))
    { const QString Qc = QString::fromStdString(C); Cids << Qc; DownloadCidToUid[Qc] = Uid; DownloadCidPct[Qc] = 0.0; }
    DownloadUidCids[Uid] = Cids;

    RebuildAvailableTab();    // repaint all cards of this package as "Downloading…"
    RefreshIpfsTab();

    const nlohmann::ordered_json PkgCopy = Pkg;
    const std::string DirCopy = Dir;
    std::thread([this, PkgCopy, DirCopy, Uid]{
        std::string Err;
        bool Ok = ContainerWrapper::ImportPackage(*GlobalConfigJSON, PkgCopy, DirCopy, &Err);
        QMetaObject::invokeMethod(this, [this, Ok, Err, Uid]{
            DownloadingUids.remove(Uid);
            for (const QString & c : DownloadUidCids.value(Uid)) { DownloadCidToUid.remove(c); DownloadCidPct.remove(c); }
            DownloadUidCids.remove(Uid);
            if (Ok) { SaveGlobalConfigJSON(); RebuildDynamicUI(); }
            else    QMessageBox::warning(this, "Download failed", QString::fromStdString(Err));
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
        //Skip content-less packages (a malformed game with no VFS layers is vacuously "hydrated").
        if (!ContainerWrapper::PackageHasContent(pm)) continue;
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
        nlohmann::ordered_json pm; std::vector<std::string> AsmWarn;
        if (!JSONOps::AssembleManifest(QString::fromStdString(LibPath), pm, AsmWarn)) continue;
        if (!ContainerWrapper::PackageHasContent(pm)) continue;
        if (!ContainerWrapper::PackageHydrated(pm, LibPath)) continue;
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
            const std::string LibRoot = ContainerWrapper::LibraryRootDir(*GlobalConfigJSON);
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
        QLabel * none = new QLabel("No installed packages yet — download games from the Available tab.", w);
        none->setStyleSheet("color:#8f98a0;");
        g->addWidget(none, 0, 0, 1, 3);
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
        slim["PACKAGEUID"]=m.value("PACKAGEUID", std::string());
        slim["PACKAGENAME"]=m.value("PACKAGENAME", std::string());
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
