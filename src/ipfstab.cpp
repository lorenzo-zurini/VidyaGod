#include "ipfstab.h"
#include "ipfsmodel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QHeaderView>
#include <QFileDialog>
#include <QMessageBox>
#include <QApplication>
#include <QClipboard>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QStyleOptionProgressBar>
#include <QPainter>
#include <QStyle>
#include <QColor>
#include <QFont>
#include <QDir>

#include <cmath>
#include <utility>

// ===== IPFS-tab-local rendering helpers =====

// Item role carrying a transfer's lifecycle status, so the delegate colour-codes the progress bar.
static constexpr int StatusRole = Qt::UserRole + 1;
enum TransferStatus : int { StDownloading = 0, StQueued = 1, StPinning = 2, StStalled = 3, StErrored = 4, StSeeded = 5 };

// Paints a column's integer value (0..100) as a progress bar (value lives in the item, so the table stays sortable).
// A negative value renders an indeterminate "busy" bar. Every leaf carries a bar, colour-coded by StatusRole.
class ProgressBarDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override
    {
        const QVariant dv = idx.data(Qt::DisplayRole);
        if (!dv.isValid()) { QStyledItemDelegate::paint(p, opt, idx); return; }   // group/category rows: no bar
        const int v = dv.toInt();
        QStyleOptionProgressBar bar;
        bar.rect = opt.rect.adjusted(2, 3, -2, -3);
        bar.minimum = 0; bar.maximum = (v < 0 ? 0 : 100);   // max 0 = indeterminate
        bar.progress = (v < 0 ? 0 : v);
        bar.textVisible = (v >= 0);
        bar.text = (v >= 0) ? QString::number(v) + "%" : QString();
        bar.textAlignment = Qt::AlignCenter;
        bar.palette = opt.palette;
        QColor c;
        switch (idx.data(StatusRole).toInt())
        {
        case StQueued:  c = QColor(0x3F, 0x51, 0xB5); break;   // indigo (queued / pending "not fetched")
        case StPinning: c = QColor(0x1B, 0x5E, 0x20); break;   // dark green
        case StStalled: c = QColor(0xF9, 0xA8, 0x25); break;   // amber
        case StErrored: c = QColor(0xC0, 0x39, 0x2B); break;   // red
        case StSeeded:  break;                                 // seeded → default blue, full (100%)
        default: break;                                        // downloading → default highlight (blue)
        }
        if (c.isValid()) bar.palette.setColor(QPalette::Highlight, c);
        QApplication::style()->drawControl(QStyle::CE_ProgressBar, &bar, p);
    }
};

// Compact human-readable byte size ("—" for unknown/negative).
static QString HumanBytes(long long N)
{
    if (N < 0) return QStringLiteral("—");
    double B = double(N); const char * U[] = { "B", "KB", "MB", "GB", "TB" }; int I = 0;
    while (B >= 1024.0 && I < 4) { B /= 1024.0; ++I; }
    return QString::number(B, 'f', I == 0 ? 0 : 1) + " " + U[I];
}

// Renders a CID's network availability ("health") as coloured text.
static std::pair<QString, QColor> IpfsHealthText(int Providers, int Missing)
{
    if (Missing == 1)    return { QStringLiteral("Errored: missing files"), QColor("#c0726a") };
    if (Providers <= -2) return { QStringLiteral("…"),               QColor("#8f98a0") };
    if (Providers == -1) return { QStringLiteral("—"),               QColor("#8f98a0") };
    if (Providers == 0)  return { QStringLiteral("✗ 0"),             QColor("#c0726a") };
    if (Providers == 1)  return { QStringLiteral("● 1"),             QColor("#d6a23e") };
    return { QString("● %1").arg(Providers),                         QColor("#5fb55f") };
}

// ===== IpfsTab =====

IpfsTab::IpfsTab(IpfsModel & model, QWidget * parent)
    : QWidget(parent), Model(model)
{
    buildUi();

    connect(&Model, &IpfsModel::cidChanged,       this, [this](const QString & cid){ renderLeaf(cid); });
    connect(&Model, &IpfsModel::cidRemoved,       this, [this](const QString & cid){ removeLeaf(cid); });
    connect(&Model, &IpfsModel::modelReset,       this, [this]{ reconcile(); });
    connect(&Model, &IpfsModel::nodeStatusChanged, this, [this]{ paintStatus(); });

    paintStatus();
    reconcile();
}

void IpfsTab::setActive(bool on)
{
    if (on) IpfsFitColumnsOnShow = true;
    Model.setActive(on);
}

void IpfsTab::buildUi()
{
    QVBoxLayout * v = new QVBoxLayout(this);
    v->setContentsMargins(12,12,12,12);

    // Status strip: Network | Peers | Seeded | ↓ | ↑ | Repo | Disk free.
    IpfsStatusTable = new QTableWidget(1, 7, this);
    IpfsStatusTable->setHorizontalHeaderLabels({"Network", "Peers", "Seeded", "↓ Down", "↑ Up", "Repo", "Disk free"});
    IpfsStatusTable->verticalHeader()->setVisible(false);
    IpfsStatusTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    IpfsStatusTable->horizontalHeader()->setHighlightSections(false);
    IpfsStatusTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    IpfsStatusTable->setSelectionMode(QAbstractItemView::NoSelection);
    IpfsStatusTable->setFocusPolicy(Qt::NoFocus);
    IpfsStatusTable->setShowGrid(false);
    IpfsStatusTable->setStyleSheet("QTableWidget{background:#20252b;border:1px solid #2c333b;border-radius:4px;}"
                                   "QHeaderView::section{background:transparent;color:#8f98a0;border:none;"
                                   "padding:3px 4px;font-size:8pt;}");
    for (int c = 0; c < 7; ++c) {
        QTableWidgetItem * it = new QTableWidgetItem(QStringLiteral("—"));
        it->setTextAlignment(Qt::AlignCenter);
        IpfsStatusTable->setItem(0, c, it);
    }
    IpfsStatusTable->setFixedHeight(IpfsStatusTable->horizontalHeader()->sizeHint().height()
                                    + IpfsStatusTable->verticalHeader()->defaultSectionSize() + 4);

    IpfsHintLabel = new QLabel("IPFS node is off — enable networking in Settings → IPFS to download and seed.", this);
    IpfsHintLabel->setStyleSheet("color:#8f98a0;font-size:9pt;");
    IpfsHintLabel->hide();

    // Button row (seed + refresh).
    QHBoxLayout * statusRow = new QHBoxLayout();
    QPushButton * refreshBtn = new QPushButton("Refresh", this);
    connect(refreshBtn, &QPushButton::clicked, &Model, &IpfsModel::refreshNow);   // force a fresh size/health re-stat

    SeedBtn = new QPushButton("Seed folder…", this);
    SeedBtn->setToolTip("Add a folder's published content to the IPFS node so it seeds (e.g. your master library).");
    connect(SeedBtn, &QPushButton::clicked, this, [this]{
        const QString TheVidya = QDir::homePath() + "/The Vidya";
        const QString Def = QDir(TheVidya).exists() ? TheVidya : QDir::homePath();
        const QString Dir = QFileDialog::getExistingDirectory(this, "Seed folder — pick the folder containing your packages", Def);
        if (Dir.isEmpty()) return;
        SeedBtn->setEnabled(false); SeedBtn->setText("Seeding…");
        Model.seedFolder(Dir);
    });
    connect(&Model, &IpfsModel::seedProgress, this, [this](int done, int total){
        SeedBtn->setText(QString("Seeding %1/%2…").arg(done).arg(total));
    });
    connect(&Model, &IpfsModel::seedFinished, this, [this](int seeded, int mismatched){
        SeedBtn->setText("Seed folder…"); SeedBtn->setEnabled(true);
        QString Msg = QString("Seeded %1 referenced file%2.").arg(seeded).arg(seeded == 1 ? "" : "s");
        if (mismatched > 0) Msg += QString("\n\n%1 file%2 changed since publish (or couldn't be added), so the "
                                           "recorded CID couldn't be re-seeded.").arg(mismatched).arg(mismatched == 1 ? "" : "s");
        QMessageBox::information(this, "Seed folder", Msg);
    });
    statusRow->addStretch(1);
    statusRow->addWidget(SeedBtn);
    statusRow->addWidget(refreshBtn);
    v->addLayout(statusRow);

    // Unified content view (transfers + seeded), grouped category → package → CID.
    QGroupBox * pinBox = new QGroupBox("Content", this);
    QVBoxLayout * pl = new QVBoxLayout(pinBox);
    IpfsPins = new QTreeWidget(pinBox);
    IpfsPins->setColumnCount(8);
    IpfsPins->setHeaderLabels({"Name", "Size", "Progress", "Speed", "Status", "Health", "CID", ""});
    IpfsPins->header()->setSectionResizeMode(QHeaderView::Interactive);
    IpfsPins->header()->setStretchLastSection(false);
    IpfsPins->setItemDelegateForColumn(2, new ProgressBarDelegate(IpfsPins));
    IpfsPins->setEditTriggers(QAbstractItemView::NoEditTriggers);
    IpfsPins->setSelectionMode(QAbstractItemView::NoSelection);
    IpfsPins->setSortingEnabled(true);
    IpfsPins->sortByColumn(0, Qt::AscendingOrder);
    pl->addWidget(IpfsPins);
    v->addWidget(pinBox, 1);

    v->addWidget(IpfsStatusTable);
    v->addWidget(IpfsHintLabel);
}

// Find or create a CID's leaf under its category (Content/Assets/Meta) → package group, using the model's label/
// package/category for this CID.
QTreeWidgetItem * IpfsTab::ensureLeaf(const QString & cid)
{
    if (!IpfsPins) return nullptr;
    if (QTreeWidgetItem * leaf = IpfsPinChildren.value(cid, nullptr)) return leaf;

    const IpfsModel::CidState st = Model.state(cid);
    const QString PkgName = st.package.isEmpty() ? QStringLiteral("Unknown / not in your library") : st.package;
    const QString CatName = st.category.isEmpty() ? QStringLiteral("Content") : st.category;

    QTreeWidgetItem * cat = IpfsPinCategories.value(CatName, nullptr);
    if (!cat)
    {
        cat = new QTreeWidgetItem(IpfsPins);
        cat->setText(0, CatName);
        QFont cf = cat->font(0); cf.setBold(true); cf.setPointSizeF(cf.pointSizeF() + 0.5); cat->setFont(0, cf);
        cat->setFlags(cat->flags() & ~Qt::ItemIsSelectable);
        cat->setExpanded(true);
        IpfsPinCategories.insert(CatName, cat);
    }
    const QString GroupKey = CatName + QChar(0x1f) + PkgName;
    QTreeWidgetItem * grp = IpfsPinGroups.value(GroupKey, nullptr);
    if (!grp)
    {
        grp = new QTreeWidgetItem(cat);
        grp->setText(0, PkgName);
        QFont f = grp->font(0); f.setBold(true); grp->setFont(0, f); grp->setFont(1, f);
        grp->setFlags(grp->flags() & ~Qt::ItemIsSelectable);
        grp->setExpanded(false);
        IpfsPinGroups.insert(GroupKey, grp);
    }

    IpfsPins->setSortingEnabled(false);
    QString LeafName = st.label.isEmpty() ? QStringLiteral("(unknown)") : st.label;
    if (LeafName.startsWith(PkgName + " — ")) LeafName = LeafName.mid(PkgName.size() + 3);
    else if (LeafName == PkgName)             LeafName = QStringLiteral("content");
    QTreeWidgetItem * child = new QTreeWidgetItem(grp);
    child->setText(0, LeafName);
    child->setText(6, cid);

    QWidget * cell = new QWidget();
    QHBoxLayout * cl = new QHBoxLayout(cell); cl->setContentsMargins(2,1,2,1); cl->setSpacing(4);
    QPushButton * prioBtn   = new QPushButton("Prioritize", cell);
    QPushButton * cancelBtn = new QPushButton("Cancel", cell);
    prioBtn->setVisible(false); cancelBtn->setVisible(false);
    connect(prioBtn,   &QPushButton::clicked, this, [this, cid]{ Model.prioritize(cid); });
    connect(cancelBtn, &QPushButton::clicked, this, [this, cid]{ Model.cancel(cid); });
    QPushButton * copyBtn  = new QPushButton("Copy CID", cell);
    QPushButton * unpinBtn = new QPushButton("Unpin", cell);
    connect(copyBtn,  &QPushButton::clicked, this, [cid]{ QApplication::clipboard()->setText(cid); });
    connect(unpinBtn, &QPushButton::clicked, this, [this, cid]{ IpfsWrapper::Unpin(cid.toStdString()); Model.refreshNow(); });
    cl->addWidget(prioBtn); cl->addWidget(cancelBtn); cl->addWidget(copyBtn); cl->addWidget(unpinBtn); cl->addStretch();
    IpfsPins->setItemWidget(child, 7, cell);
    IpfsPinChildren.insert(cid, child);
    IpfsCancelBtns.insert(cid, cancelBtn);
    IpfsPrioBtns.insert(cid, prioBtn);
    IpfsPins->setSortingEnabled(true);
    return child;
}

// Paint one CID's columns from the model's CidState (creating its leaf if needed).
void IpfsTab::renderLeaf(const QString & cid)
{
    if (!Model.has(cid)) { removeLeaf(cid); return; }
    QTreeWidgetItem * leaf = ensureLeaf(cid);
    if (!leaf) return;
    const IpfsModel::CidState st = Model.state(cid);
    using P = IpfsModel::CidState;

    // Size.
    leaf->setText(1, st.phase == P::Pending ? QString() : HumanBytes(st.size));
    leaf->setData(1, Qt::UserRole, (qlonglong)(st.size < 0 ? 0 : st.size));

    // Progress bar value + colour code.
    const int Bar = (st.phase == P::Seeded) ? 100 : (st.pct >= 0 ? (int)std::lround(st.pct) : -1);
    int Role = StSeeded; QString Status; QColor Fg("#c6d4df");
    switch (st.phase)
    {
    case P::Pending:     Role = StQueued;      Status = QStringLiteral("Not fetched — will sync when online"); Fg = QColor("#8f98a0"); break;
    case P::Queued:      Role = StQueued;      Status = QStringLiteral("Queued"); break;
    case P::Downloading: Role = StDownloading; Status = QStringLiteral("Fetching…"); break;
    case P::Pinning:     Role = StPinning;     Status = QStringLiteral("Pinning…"); break;
    case P::Stalled:     Role = StStalled;     Status = QStringLiteral("Stalled — waiting for peers…"); break;
    case P::Errored: {
        Role = StErrored; Fg = QColor("#c0726a");
        const QString Tail = st.error.section('\n', -1).trimmed();
        Status = (Tail == "missing files") ? QStringLiteral("Errored: missing files")
               : st.error.isEmpty()        ? QStringLiteral("Failed")
                                           : ("Failed: " + Tail);
        leaf->setToolTip(4, st.error.isEmpty() ? QStringLiteral("Download failed") : st.error);
        break; }
    case P::Seeded:
        Role = StSeeded;
        Status = st.uploading ? QStringLiteral("⬆ Uploading") : QStringLiteral("Seeding");
        Fg = st.uploading ? QColor("#4a90d9") : QColor("#5fb55f");
        break;
    }
    leaf->setData(2, Qt::DisplayRole, Bar);
    leaf->setData(2, StatusRole, Role);
    leaf->setText(3, st.speedBps > 0 ? (HumanBytes((long long)st.speedBps) + "/s") : QString());
    leaf->setText(4, Status);
    leaf->setForeground(4, Fg);

    // Health (blank for a not-yet-fetched source).
    if (st.phase == P::Pending) leaf->setText(5, QString());
    else { const auto [Txt, Col] = IpfsHealthText(st.providers, st.missing); leaf->setText(5, Txt); leaf->setForeground(5, Col); }

    // Queue controls: Cancel while queued/active; Prioritize only while still queued.
    const bool Fetching = (st.phase == P::Downloading || st.phase == P::Pinning || st.phase == P::Stalled);
    if (QPushButton * b = IpfsCancelBtns.value(cid, nullptr)) b->setVisible(st.phase == P::Queued || Fetching);
    if (QPushButton * b = IpfsPrioBtns.value(cid, nullptr))   b->setVisible(st.phase == P::Queued);

    if (leaf->parent()) leaf->parent()->setExpanded(true);
}

void IpfsTab::removeLeaf(const QString & cid)
{
    IpfsCancelBtns.remove(cid); IpfsPrioBtns.remove(cid);   // buttons die with the item widget
    if (QTreeWidgetItem * leaf = IpfsPinChildren.take(cid)) delete leaf;
}

// Full rebuild from the model's CID set: upsert every known CID, drop leaves no longer in the model, refresh totals.
void IpfsTab::reconcile()
{
    if (!IpfsPins) return;
    QScrollBar * VBar = IpfsPins->verticalScrollBar();
    const int Scroll = VBar ? VBar->value() : 0;
    IpfsPins->setSortingEnabled(false);

    const QHash<QString, IpfsModel::CidState> & Cids = Model.cids();
    for (const QString & cid : IpfsPinChildren.keys())
        if (!Cids.contains(cid)) removeLeaf(cid);
    for (auto it = Cids.constBegin(); it != Cids.constEnd(); ++it) renderLeaf(it.key());

    updateGroupTotals();

    IpfsPins->setSortingEnabled(true);
    if (VBar) VBar->setValue(Scroll);
    if (IpfsFitColumnsOnShow)
    {
        for (int c = 0; c < IpfsPins->columnCount(); ++c) IpfsPins->resizeColumnToContents(c);
        IpfsFitColumnsOnShow = false;
    }
    else
    {
        IpfsPins->resizeColumnToContents(3);   // Speed
        IpfsPins->resizeColumnToContents(4);   // Status
        IpfsPins->resizeColumnToContents(5);   // Health
    }
    if (qEnvironmentVariableIsSet("VIDYAGOD_IPFS_EXPAND")) IpfsPins->expandAll();
}

void IpfsTab::updateGroupTotals()
{
    for (const QString & Key : IpfsPinGroups.keys())
    {
        QTreeWidgetItem * g = IpfsPinGroups.value(Key, nullptr);
        if (!g) continue;
        const int n = g->childCount();
        if (n == 0) { IpfsPinGroups.remove(Key); delete g; continue; }
        long long GrpTotal = 0;
        for (int i = 0; i < n; ++i) GrpTotal += g->child(i)->data(1, Qt::UserRole).toLongLong();
        g->setText(1, QString("%1 item%2 · %3").arg(n).arg(n == 1 ? "" : "s").arg(HumanBytes(GrpTotal)));
    }
    for (const QString & Cat : IpfsPinCategories.keys())
    {
        QTreeWidgetItem * c = IpfsPinCategories.value(Cat, nullptr);
        if (!c) continue;
        if (c->childCount() == 0) { IpfsPinCategories.remove(Cat); delete c; continue; }
        long long CatTotal = 0; int CatItems = 0;
        for (int i = 0; i < c->childCount(); ++i)
        {
            QTreeWidgetItem * g = c->child(i);
            CatItems += g->childCount();
            for (int j = 0; j < g->childCount(); ++j) CatTotal += g->child(j)->data(1, Qt::UserRole).toLongLong();
        }
        c->setText(1, QString("%1 item%2 · %3").arg(CatItems).arg(CatItems == 1 ? "" : "s").arg(HumanBytes(CatTotal)));
    }
}

void IpfsTab::paintStatus()
{
    if (!IpfsStatusTable) return;
    const IpfsModel::NodeStatus & S = Model.nodeStatus();
    if (!S.available)
    {
        IpfsHintLabel->show();
        IpfsStatusTable->item(0, 0)->setText(QStringLiteral("off"));
        IpfsStatusTable->item(0, 0)->setForeground(QColor("#c0726a"));
        for (int c = 1; c < IpfsStatusTable->columnCount(); ++c) IpfsStatusTable->item(0, c)->setText(QStringLiteral("—"));
        return;
    }
    IpfsHintLabel->hide();
    auto Rate = [](double Bps){ return Bps >= 1.0 ? (HumanBytes((long long)Bps) + "/s") : QStringLiteral("—"); };
    auto Set = [this](int col, const QString & text, const QColor & fg = QColor()){
        QTableWidgetItem * it = IpfsStatusTable->item(0, col);
        it->setText(text);
        it->setForeground(fg.isValid() ? fg : QColor("#c6d4df"));
    };
    Set(0, S.daemon ? QStringLiteral("● connected") : QStringLiteral("● connecting…"),
           S.daemon ? QColor("#5fb55f") : QColor("#d6a23e"));
    Set(1, S.daemon ? QString::number(S.peers) : QStringLiteral("—"));
    Set(2, QString("%1 · %2").arg(S.pinCount).arg(HumanBytes(S.totalSize)));
    Set(3, Rate(S.downBps), S.downBps >= 1.0 ? QColor("#5fb55f") : QColor("#8f98a0"));
    Set(4, Rate(S.upBps),   S.upBps   >= 1.0 ? QColor("#4a90d9") : QColor("#8f98a0"));
    Set(5, S.repo.isEmpty() ? QStringLiteral("—") : S.repo);
    Set(6, S.diskFree < 0 ? QStringLiteral("—") : QString("%1 free").arg(HumanBytes(S.diskFree)));
}
