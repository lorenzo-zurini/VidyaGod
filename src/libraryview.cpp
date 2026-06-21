#include "libraryview.h"
#include "manifestmodel.h"
#include "packagecatalog.h"
#include "covercache.h"
#include "prelaunchwindow.h"
#include "ipfswrapper.h"
#include "commonutils.h"

#include <QPainter>
#include <QApplication>
#include <QGuiApplication>
#include <QScrollBar>
#include <QMessageBox>
#include <QMetaObject>

// True if the embedded IPFS node can fetch content right NOW (started AND its network stack is up). Otherwise
// shows the reason and returns false. The node runs in-process — there is no external daemon to install or start.
bool IpfsFetchReady(QWidget * parent)
{
    if (!IpfsWrapper::Available())
    {
        QMessageBox::warning(parent, "IPFS unavailable",
            "VidyaGod's IPFS node didn't start, so content can't be downloaded.\n\n"
            "Check the logs and restart VidyaGod.");
        return false;
    }
    if (!IpfsWrapper::DaemonRunning())
    {
        QMessageBox::warning(parent, "No network connection",
            "VidyaGod's IPFS node isn't connected to the network yet, so content can't be downloaded.\n\n"
            "Check your internet connection and try again in a moment.");
        return false;
    }
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// LibraryGameCard
// ═════════════════════════════════════════════════════════════════════════════

LibraryGameCard::LibraryGameCard(nlohmann::ordered_json * gc, const NodeIndex * index,
                                 std::vector<std::string> groupNodeIds)
    : Index(index), GroupNodeIds(std::move(groupNodeIds)), GlobalConfigJSON(gc)
{
    if (!GroupNodeIds.empty()) RepNodeId = GroupNodeIds.front();
}

void LibraryGameCard::InitializeClassVariables()
{
    const Node * Rep = (Index && !RepNodeId.empty()) ? Index->Find(RepNodeId) : nullptr;
    if (!Rep) return;

    PackagePath = Rep->BundleDir;
    GroupKey    = QString::fromStdString(Rep->GroupKey());
    RepUid      = Rep->Uid;

    const nlohmann::ordered_json & Meta = Rep->Meta;
    GameTitle = QString::fromStdString(Meta.is_object() ? Meta.value("TITLE", RepNodeId) : RepNodeId);

    // Sort keys
    SortTitle = GameTitle.toLower();
    if (SortTitle.startsWith("the ")) SortTitle = SortTitle.mid(4);

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
    if (Meta.is_object() && Meta.contains("COVER"))
    {
        const nlohmann::ordered_json & CoverNode = Meta["COVER"];
        const QString Path = CoverCache::instance()->resolve(CoverNode, QString::fromStdString(PackagePath.string()));
        if (!Path.isEmpty()) CoverOriginal.load(Path);
        else { QString f; CoverCache::Locate(CoverNode, f, CoverCid); }   // pending fetch — refresh on coverReady
    }
}

void LibraryGameCard::play()
{
    if (!Index || GroupNodeIds.empty()) return;

    //Validate the node graph before launching; hard errors block launch. Scope it to THIS package's own closure
    //(its editions + their PARENTS) so an unrelated package's issues — e.g. another game's legitimate cross-layer
    //case collisions — never surface here or block this launch.
    {
        std::set<std::string> Scope;
        for (const std::string &Ed : GroupNodeIds)
        {
            Scope.insert(Ed);
            for (const std::string &Id : ManifestModel::ResolveNodeOrder(*Index, Ed, {})) Scope.insert(Id);
        }
        std::vector<std::string> ValErr, ValWarn;
        ManifestModel::ValidateNodeGraph(*Index, ValErr, ValWarn, &Scope);
        for (const auto &W : ValWarn) LogWarn("LibraryGameCard", "Node graph: " + W);
        if (!ValErr.empty())
        {
            QString Msg = "This package's node graph has errors and cannot be launched:\n\n";
            for (const auto &E : ValErr) Msg += "• " + QString::fromStdString(E) + "\n";
            QMessageBox::critical(nullptr, "Manifest Error", Msg);
            return;
        }
    }

    //Gate: a USABLE runner must exist for the (representative) edition — a PATH runner present on the system or a
    //hydrated build runner. A compatible-but-not-installed runner does NOT count (you can't launch without it).
    if (const Node * Rep = Index->Find(RepNodeId))
        if (PackageCatalog::UsableRunners(*Index, *Rep).empty())
        {
            QMessageBox::warning(nullptr, "Runner unavailable",
                "No installed runner can launch this game on this machine. "
                "Download its runner from the Catalog (it's offered in the download dialog), then try again.");
            return;
        }

    bool skip = false;
    if (!RepUid.empty()) {
        auto US = PackageCatalog::GetPackageUserSettings(*GlobalConfigJSON, RepUid);
        if (US.contains("SKIP_LAUNCH_DIALOG") && US["SKIP_LAUNCH_DIALOG"].is_boolean())
            skip = bool(US["SKIP_LAUNCH_DIALOG"]);
    }
    bool shift = (QGuiApplication::keyboardModifiers() & Qt::ShiftModifier) != 0;
    auto * dlg = new PreLaunchWindow(GlobalConfigJSON, Index, GroupNodeIds, nullptr);
    dlg->show();
    if (skip && !shift)
        QMetaObject::invokeMethod(dlg, "onLaunchClicked", Qt::QueuedConnection);
}

void LibraryGameCard::edit()
{
    if (!Index || GroupNodeIds.empty()) return;
    (new PreLaunchWindow(GlobalConfigJSON, Index, GroupNodeIds, nullptr))->show();
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
    const int sW = cardW * 30 / 100;   // overlay thumbnail (matches secW() at this width)
    const int sH = sW * 3 / 2;
    auto scaleMain = [&](LibraryGameCard * c) {
        c->CoverScaled = c->CoverOriginal.isNull() ? QPixmap()
            : c->CoverOriginal.scaled(cardW, cardH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        c->ElidedTitle = TitleFM.elidedText(c->GameTitle, Qt::ElideRight, textW);
    };
    for (auto * c : *Cards) {
        scaleMain(c);
        for (auto * sc : c->Secondaries) {                       // a package's secondary games (Catalog) — scale small too
            scaleMain(sc);
            sc->CoverSmall = sc->CoverOriginal.isNull() ? QPixmap()
                : sc->CoverOriginal.scaled(sW, sH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
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
    SecRects.resize(count);
    int totalH;

    if (Groups.isEmpty())
    {
        // Flat grid (Library tab) — unchanged (no per-package secondaries here).
        for (int i = 0; i < count; i++) {
            Rects[i] = QRect(hGap + (i % cols) * (cardW + hGap),
                             VPad + (i / cols) * (cardH + MinGap), cardW, cardH);
            SecRects[i].clear();
        }
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
                // Uniform-height cells. A package's secondary covers (Catalog) are OVERLAID along the bottom edge of
                // its main tile — purely aesthetic — so they don't change the row height.
                const int rows = (n + cols - 1) / cols;
                for (int k = 0; k < n; k++)
                {
                    const int i   = g.first + k;
                    const int col = k % cols, rrow = k / cols;
                    const int x = hGap + col * (cardW + hGap);
                    const int cy = y + rrow * (cardH + MinGap);
                    Rects[i] = QRect(x, cy, cardW, cardH);

                    // Overlay strip: small thumbnails anchored to the bottom of the main tile, left→right, capped.
                    SecRects[i].clear();
                    const int nsec = qMin((int)Cards->at(i)->Secondaries.size(), SecMax);
                    if (nsec > 0)
                    {
                        const int sW = secW(), sH = secH();
                        const int sy = cy + cardH - SecGap - sH;
                        for (int s = 0; s < nsec; s++)
                            SecRects[i].append(QRect(x + SecGap + s * (sW + SecGap), sy, sW, sH));
                    }
                }
                y += rows * cardH + (rows - 1) * MinGap + MinGap;
            }
            else
            {
                for (int k = 0; k < n; k++) { Rects[g.first + k] = QRect(); SecRects[g.first + k].clear(); }   // collapsed → hidden
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

        // Secondary covers (Catalog per-package): small thumbnails overlaid along the bottom edge of the main cover,
        // purely aesthetic (the games are pickable from the package's download dialog). Drawn UNDER the Downloading/
        // hover overlays below, so those cover them.
        if (i < SecRects.size() && !SecRects[i].isEmpty()) {
            const auto & secs = card->Secondaries;
            for (int s = 0; s < SecRects[i].size() && s < (int)secs.size(); s++) {
                const QRect & sr = SecRects[i][s];
                LibraryGameCard * sc = secs[s];
                p.fillRect(sr.adjusted(-1,-1,1,1), QColor(0,0,0,120));     // subtle shadow/separation
                if (!sc->CoverSmall.isNull()) p.drawPixmap(sr, sc->CoverSmall);
                else { p.fillRect(sr, QApplication::palette().color(QPalette::Mid));
                       p.setPen(QColor(0x8f,0x98,0xa0)); p.drawText(sr.adjusted(2,2,-2,-2), Qt::AlignCenter|Qt::TextWordWrap, sc->GameTitle); }
                p.setPen(QColor(255,255,255,90)); p.setBrush(Qt::NoBrush); p.drawRect(sr.adjusted(0,0,-1,-1));  // thin border
            }
            // "+N" badge when there are more secondaries than overlay slots.
            const int extra = (int)secs.size() - SecRects[i].size();
            if (extra > 0) {
                const QRect & last = SecRects[i].last();
                p.fillRect(last, QColor(0,0,0,150));
                p.setPen(Qt::white); p.setFont(PlayFont);
                p.drawText(last, Qt::AlignCenter, QString("+%1").arg(extra));
                p.setFont(TitleFont);
            }
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
            p.setPen(QColor(0xff,0xff,0xff,150));
            p.drawText(QRect(r.left(), bar.bottom()+4, r.width(), 18),
                       Qt::AlignHCenter | Qt::AlignTop, "click to cancel");
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
    p.setFont(TitleFont);
}

void LibraryView::onMouseMove(QMouseEvent * e)
{
    if (!Cards) return;
    const QPoint pos = e->pos() + QPoint(0, verticalScrollBar()->value());
    int newHover = -1;
    const int count = Cards->count();
    for (int i = 0; i < count; i++)
        if (Rects[i].contains(pos)) { newHover = i; break; }   // overlay thumbnails sit inside the main rect
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
        // A click anywhere on the card (including over the aesthetic secondary thumbnails) is the package action.
        if (!Rects[i].contains(pos)) continue;          // collapsed cards have empty rects → skipped
        SelectedIdx = i;                                // keep keyboard focus in sync with the click
        if (Cards->at(i)->Downloading) { emit cancelRequested(Cards->at(i)); return; }   // in flight → offer to cancel
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
    if (Cards->at(i)->Downloading) { emit cancelRequested(Cards->at(i)); return; }
    if (EmitDownload) emit downloadRequested(Cards->at(i));
    else              Cards->at(i)->play();
}


