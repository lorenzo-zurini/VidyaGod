#include "ipfstab.h"
#include "ipfsmodel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QHeaderView>
#include <QFileDialog>
#include <QInputDialog>
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
#include <QTimer>

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

#include "guiformat.h"   // HumanBytesQ — the one shared GUI byte formatter

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

// Tree depth: 0 = category, 1 = package group, 2 = CID leaf.
static int RowDepth(const QTreeWidgetItem * it)
{
    int d = 0;
    while ((it = it->parent()) != nullptr) ++d;
    return d;
}

static const QString CatAssetsName = QStringLiteral("Assets");

// ===== IpfsTab =====

IpfsTab::IpfsTab(IpfsModel & model, QWidget * parent)
    : QWidget(parent), Model(model)
{
    buildUi();

    connect(&Model, &IpfsModel::cidChanged,        this, [this](const QString & cid){ renderLeaf(cid); });
    connect(&Model, &IpfsModel::cidRemoved,        this, [this](const QString & cid){ removeLeaf(cid); });
    connect(&Model, &IpfsModel::modelReset,        this, [this]{ reconcile(); });
    connect(&Model, &IpfsModel::nodeStatusChanged, this, [this]{ paintStatus(); });
    connect(&Model, &IpfsModel::packagePublished,  this, [this](const QString & pkg, const QString & cid, const QString & err){
        if (cid.isEmpty())
        {
            QMessageBox::warning(this, "Publish package", QString("Publishing \"%1\" failed:\n%2").arg(pkg, err));
            return;
        }
        for (auto it = IpfsPinGroups.constBegin(); it != IpfsPinGroups.constEnd(); ++it)
            if (it.key().section(QChar(0x1f), 1) == pkg) paintGroupCid(it.value(), pkg);
        QApplication::clipboard()->setText(cid);
        QMessageBox::information(this, "Publish package",
            QString("Package CID for \"%1\" (copied to clipboard):\n\n%2\n\nAnyone can add it as a package source "
                    "to receive this package.").arg(pkg, cid));
    });

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

    // Toolbar: search + global actions (per-row actions live in the tree's context menu).
    QHBoxLayout * toolbar = new QHBoxLayout();
    SearchBox = new QLineEdit(this);
    SearchBox->setPlaceholderText("Search name / package / CID…");
    SearchBox->setClearButtonEnabled(true);
    connect(SearchBox, &QLineEdit::textChanged, this, [this]{ applyFilters(); updateFilterCounts(); });
    toolbar->addWidget(SearchBox, 1);

    QPushButton * addSrcBtn = new QPushButton("Add source…", this);
    addSrcBtn->setToolTip("Add a package or library CID someone shared with you — its packages appear in the Catalog.");
    connect(addSrcBtn, &QPushButton::clicked, this, [this]{
        bool ok = false;
        const QString Cid = QInputDialog::getText(this, "Add source", "Package / library CID:",
                                                  QLineEdit::Normal, QString(), &ok).trimmed();
        if (!ok || Cid.isEmpty()) return;
        const QString Name = QInputDialog::getText(this, "Add source", "Name for this source:",
                                                   QLineEdit::Normal, Cid.left(12), &ok).trimmed();
        if (!ok) return;
        Model.addSource(Cid, Name.isEmpty() ? Cid.left(12) : Name);
    });
    toolbar->addWidget(addSrcBtn);

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
    toolbar->addWidget(SeedBtn);

    QPushButton * refreshBtn = new QPushButton("Refresh", this);
    connect(refreshBtn, &QPushButton::clicked, &Model, &IpfsModel::refreshNow);   // force a fresh size/health re-stat
    toolbar->addWidget(refreshBtn);
    v->addLayout(toolbar);

    // Sidebar filters + content tree, side by side.
    QHBoxLayout * body = new QHBoxLayout();
    body->addWidget(buildSidebar());

    IpfsPins = new QTreeWidget(this);
    IpfsPins->setColumnCount(7);
    IpfsPins->setHeaderLabels({"Name", "Size", "Progress", "Speed", "Status", "Health", "CID"});
    IpfsPins->header()->setSectionResizeMode(QHeaderView::Interactive);
    IpfsPins->header()->setStretchLastSection(false);
    IpfsPins->setItemDelegateForColumn(2, new ProgressBarDelegate(IpfsPins));
    IpfsPins->setEditTriggers(QAbstractItemView::NoEditTriggers);
    IpfsPins->setSelectionMode(QAbstractItemView::ExtendedSelection);
    IpfsPins->setSortingEnabled(true);
    IpfsPins->sortByColumn(0, Qt::AscendingOrder);
    IpfsPins->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(IpfsPins, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint & p){ showContextMenu(p); });
    body->addWidget(IpfsPins, 1);
    v->addLayout(body, 1);

    v->addWidget(IpfsStatusTable);
    v->addWidget(IpfsHintLabel);

    CountsDebounce = new QTimer(this);
    CountsDebounce->setSingleShot(true);
    CountsDebounce->setInterval(300);
    connect(CountsDebounce, &QTimer::timeout, this, [this]{ updateFilterCounts(); });
}

QWidget * IpfsTab::buildSidebar()
{
    QWidget * side = new QWidget(this);
    side->setFixedWidth(200);
    QVBoxLayout * sv = new QVBoxLayout(side);
    sv->setContentsMargins(0,0,0,0);
    sv->setSpacing(4);

    const char * ListStyle =
        "QListWidget{background:#20252b;border:1px solid #2c333b;border-radius:4px;font-size:9pt;}"
        "QListWidget::item{padding:3px 6px;}"
        "QListWidget::item:selected{background:#2f3944;color:#e6eef5;}";
    auto sectionLabel = [side](const QString & text){
        QLabel * l = new QLabel(text, side);
        l->setStyleSheet("color:#8f98a0;font-size:8pt;font-weight:bold;padding-left:2px;");
        return l;
    };

    sv->addWidget(sectionLabel("STATUS"));
    StatusFilters = new QListWidget(side);
    StatusFilters->setStyleSheet(ListStyle);
    const std::pair<int, const char *> Statuses[] = {
        { SAll,         "All"         }, { SDownloading, "Downloading" }, { SSeeding, "Seeding" },
        { SQueued,      "Queued"      }, { SStalled,     "Stalled"     }, { SErrored, "Errored" },
        { SPending,     "Not fetched" },
    };
    for (const auto & [Kind, Name] : Statuses)
    {
        QListWidgetItem * it = new QListWidgetItem(QString::fromLatin1(Name), StatusFilters);
        it->setData(Qt::UserRole, Kind);
    }
    StatusFilters->setCurrentRow(0);
    connect(StatusFilters, &QListWidget::currentItemChanged, this, [this](QListWidgetItem * cur, QListWidgetItem *){
        CurrentStatus = cur ? cur->data(Qt::UserRole).toInt() : SAll;
        applyFilters();
    });
    sv->addWidget(StatusFilters, 3);

    sv->addWidget(sectionLabel("CATEGORIES"));
    CategoryFilters = new QListWidget(side);
    CategoryFilters->setStyleSheet(ListStyle);
    for (const char * Name : { "All", "Content", "Assets", "Meta" })
    {
        QListWidgetItem * it = new QListWidgetItem(QString::fromLatin1(Name), CategoryFilters);
        it->setData(Qt::UserRole, QString::fromLatin1(Name) == "All" ? QString() : QString::fromLatin1(Name));
    }
    CategoryFilters->setCurrentRow(0);
    connect(CategoryFilters, &QListWidget::currentItemChanged, this, [this](QListWidgetItem * cur, QListWidgetItem *){
        CurrentCategory = cur ? cur->data(Qt::UserRole).toString() : QString();
        applyFilters();
    });
    sv->addWidget(CategoryFilters, 2);
    sv->addStretch(1);
    return side;
}

// Find or create a CID's leaf under its category (Content/Assets/Meta) → package group, using the model's label/
// package/category for this CID. Default expansion: categories open (Assets closed), package groups closed.
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
        cat->setExpanded(CatName != CatAssetsName);
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
        paintGroupCid(grp, PkgName);
        IpfsPinGroups.insert(GroupKey, grp);
    }

    IpfsPins->setSortingEnabled(false);
    QString LeafName = st.label.isEmpty() ? QStringLiteral("(unknown)") : st.label;
    if (LeafName.startsWith(PkgName + " — ")) LeafName = LeafName.mid(PkgName.size() + 3);
    else if (LeafName == PkgName)             LeafName = QStringLiteral("content");
    QTreeWidgetItem * child = new QTreeWidgetItem(grp);
    child->setText(0, LeafName);
    child->setToolTip(0, QString("%1\n%2").arg(st.label, cid));
    child->setText(6, cid);
    IpfsPinChildren.insert(cid, child);
    IpfsPins->setSortingEnabled(true);
    return child;
}

// The package-level meta-CID lives on the package group row (CID column) once published.
void IpfsTab::paintGroupCid(QTreeWidgetItem * grp, const QString & pkg)
{
    const QString Cid = Model.packageCid(pkg);
    grp->setText(6, Cid);
    grp->setForeground(6, QColor("#8f98a0"));
    grp->setToolTip(6, Cid.isEmpty() ? QString()
        : QString("Package CID — share it; others add it as a package source to receive \"%1\".").arg(pkg));
}

// Paint one CID's columns from the model's CidState (creating its leaf if needed). A seeded pin whose backing files
// are gone is presented as ERRORED (qBittorrent's "missing files" semantics), not as a healthy seed.
void IpfsTab::renderLeaf(const QString & cid)
{
    if (!Model.has(cid)) { removeLeaf(cid); return; }
    QTreeWidgetItem * leaf = ensureLeaf(cid);
    if (!leaf) return;
    const IpfsModel::CidState st = Model.state(cid);
    using P = IpfsModel::CidState;

    // Size.
    leaf->setText(1, st.phase == P::Pending ? QString() : HumanBytesQ(st.size));
    leaf->setData(1, Qt::UserRole, (qlonglong)(st.size < 0 ? 0 : st.size));

    // Progress bar value + colour code.
    const bool MissingFiles = (st.phase == P::Seeded && st.missing == 1);
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
        if (MissingFiles)
        {
            Role = StErrored; Status = QStringLiteral("Errored: missing files"); Fg = QColor("#c0726a");
            leaf->setToolTip(4, QStringLiteral("The seeded content's backing file is gone from disk — "
                                               "it can no longer be served to peers."));
        }
        else
        {
            Role = StSeeded;
            Status = st.uploading ? QStringLiteral("⬆ Uploading") : QStringLiteral("Seeding");
            Fg = st.uploading ? QColor("#4a90d9") : QColor("#5fb55f");
        }
        break;
    }
    leaf->setData(2, Qt::DisplayRole, Bar);
    leaf->setData(2, StatusRole, Role);
    leaf->setText(3, st.speedBps > 0 ? (HumanBytesQ((long long)st.speedBps) + "/s") : QString());
    leaf->setText(4, Status);
    leaf->setForeground(4, Fg);

    // Health (blank for a not-yet-fetched source).
    if (st.phase == P::Pending) leaf->setText(5, QString());
    else { const auto [Txt, Col] = IpfsHealthText(st.providers, st.missing); leaf->setText(5, Txt); leaf->setForeground(5, Col); }

    leaf->setHidden(!leafMatches(cid));
    if (CountsDebounce) CountsDebounce->start();
}

void IpfsTab::removeLeaf(const QString & cid)
{
    if (QTreeWidgetItem * leaf = IpfsPinChildren.take(cid)) delete leaf;
    if (CountsDebounce) CountsDebounce->start();
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
    applyFilters();
    updateFilterCounts();

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

// ===== filtering =====

int IpfsTab::effectiveStatus(const QString & cid) const
{
    const IpfsModel::CidState st = Model.state(cid);
    using P = IpfsModel::CidState;
    switch (st.phase)
    {
    case P::Pending:     return SPending;
    case P::Queued:      return SQueued;
    case P::Downloading: return SDownloading;
    case P::Pinning:     return SDownloading;
    case P::Stalled:     return SStalled;
    case P::Errored:     return SErrored;
    case P::Seeded:      return st.missing == 1 ? SErrored : SSeeding;   // missing files ⇒ errored, like qBittorrent
    }
    return SSeeding;
}

bool IpfsTab::leafMatches(const QString & cid) const
{
    if (CurrentStatus != SAll && effectiveStatus(cid) != CurrentStatus) return false;
    const IpfsModel::CidState st = Model.state(cid);
    if (!CurrentCategory.isEmpty())
    {
        const QString Cat = st.category.isEmpty() ? QStringLiteral("Content") : st.category;
        if (Cat != CurrentCategory) return false;
    }
    const QString Needle = SearchBox ? SearchBox->text().trimmed() : QString();
    if (!Needle.isEmpty()
        && !st.label.contains(Needle, Qt::CaseInsensitive)
        && !st.package.contains(Needle, Qt::CaseInsensitive)
        && !cid.contains(Needle, Qt::CaseInsensitive)) return false;
    return true;
}

// Hide non-matching leaves and any group/category left empty; auto-expand matches while a filter is active, and
// restore the default expansion (categories open except Assets, packages closed) when the last filter is cleared.
void IpfsTab::applyFilters()
{
    if (!IpfsPins) return;
    const bool Filtered = CurrentStatus != SAll || !CurrentCategory.isEmpty()
                          || (SearchBox && !SearchBox->text().trimmed().isEmpty());

    for (auto it = IpfsPinChildren.constBegin(); it != IpfsPinChildren.constEnd(); ++it)
        it.value()->setHidden(!leafMatches(it.key()));

    for (QTreeWidgetItem * grp : IpfsPinGroups)
    {
        bool AnyVisible = false;
        for (int i = 0; i < grp->childCount() && !AnyVisible; ++i) AnyVisible = !grp->child(i)->isHidden();
        grp->setHidden(!AnyVisible);
        if (Filtered && AnyVisible) grp->setExpanded(true);
    }
    for (auto it = IpfsPinCategories.constBegin(); it != IpfsPinCategories.constEnd(); ++it)
    {
        QTreeWidgetItem * cat = it.value();
        bool AnyVisible = false;
        for (int i = 0; i < cat->childCount() && !AnyVisible; ++i) AnyVisible = !cat->child(i)->isHidden();
        cat->setHidden(!AnyVisible);
        if (Filtered && AnyVisible) cat->setExpanded(true);
    }

    if (!Filtered && WasFiltered)
    {
        for (auto it = IpfsPinCategories.constBegin(); it != IpfsPinCategories.constEnd(); ++it)
            it.value()->setExpanded(it.key() != CatAssetsName);
        for (QTreeWidgetItem * grp : IpfsPinGroups) grp->setExpanded(false);
    }
    WasFiltered = Filtered;
}

void IpfsTab::updateFilterCounts()
{
    if (!StatusFilters || !CategoryFilters) return;
    int ByStatus[SCount] = {};
    QHash<QString, int> ByCat;
    int Total = 0;
    const QHash<QString, IpfsModel::CidState> & Cids = Model.cids();
    for (auto it = Cids.constBegin(); it != Cids.constEnd(); ++it)
    {
        ++Total;
        const int S = effectiveStatus(it.key());
        if (S >= 0 && S < SCount) ++ByStatus[S];
        ++ByCat[it->category.isEmpty() ? QStringLiteral("Content") : it->category];
    }
    static const char * StatusNames[SCount] = { "All", "Downloading", "Seeding", "Queued", "Stalled", "Errored", "Not fetched" };
    for (int i = 0; i < StatusFilters->count(); ++i)
    {
        QListWidgetItem * it = StatusFilters->item(i);
        const int Kind = it->data(Qt::UserRole).toInt();
        const int N = (Kind == SAll) ? Total : ByStatus[Kind];
        it->setText(QString("%1 (%2)").arg(QString::fromLatin1(StatusNames[Kind])).arg(N));
    }
    for (int i = 0; i < CategoryFilters->count(); ++i)
    {
        QListWidgetItem * it = CategoryFilters->item(i);
        const QString Cat = it->data(Qt::UserRole).toString();
        const int N = Cat.isEmpty() ? Total : ByCat.value(Cat, 0);
        it->setText(QString("%1 (%2)").arg(Cat.isEmpty() ? QStringLiteral("All") : Cat).arg(N));
    }
}

// ===== context menu =====

void IpfsTab::showContextMenu(const QPoint & pos)
{
    if (!IpfsPins) return;
    QTreeWidgetItem * Clicked = IpfsPins->itemAt(pos);

    // The CID set the leaf actions apply to: the multi-selection, or just the clicked row if it isn't part of one.
    QStringList Sel;
    for (QTreeWidgetItem * s : IpfsPins->selectedItems())
        if (RowDepth(s) == 2) Sel << s->text(6);
    if (Clicked && RowDepth(Clicked) == 2 && !Sel.contains(Clicked->text(6)))
    {
        Sel = QStringList{ Clicked->text(6) };
        IpfsPins->setCurrentItem(Clicked);
    }

    QMenu menu(this);
    using P = IpfsModel::CidState;

    if (!Sel.isEmpty())
    {
        bool AnyQueued = false, AnyActive = false, AnyPinned = false;
        for (const QString & c : Sel)
        {
            const P st = Model.state(c);
            AnyQueued |= (st.phase == P::Queued);
            AnyActive |= (st.phase == P::Queued || st.phase == P::Downloading
                          || st.phase == P::Pinning || st.phase == P::Stalled);
            AnyPinned |= (st.phase == P::Seeded || st.phase == P::Errored);
        }
        const int N = Sel.size();
        menu.addAction(N == 1 ? QStringLiteral("Copy CID") : QString("Copy %1 CIDs").arg(N), this, [Sel]{
            QApplication::clipboard()->setText(Sel.join('\n'));
        });
        if (AnyQueued)
            menu.addAction("Prioritize", this, [this, Sel]{ for (const QString & c : Sel) Model.prioritize(c); });
        if (AnyActive)
            menu.addAction("Cancel download", this, [this, Sel]{ for (const QString & c : Sel) Model.cancel(c); });
        menu.addAction("Re-check health", this, [this, Sel]{ Model.recheckHealth(Sel); });
        if (AnyPinned)
        {
            menu.addSeparator();
            menu.addAction(N == 1 ? QStringLiteral("Unpin (stop seeding)…") : QString("Unpin %1 items…").arg(N),
                           this, [this, Sel, N]{
                if (QMessageBox::question(this, "Unpin",
                        QString("Stop seeding %1 item%2? The content stays on disk; only the node's pin is removed.")
                            .arg(N).arg(N == 1 ? "" : "s")) != QMessageBox::Yes) return;
                Model.unpinMany(Sel);
            });
        }
    }
    else if (Clicked && RowDepth(Clicked) == 1)
    {
        const QString Pkg = Clicked->text(0);
        const QString PkgCid = Model.packageCid(Pkg);
        const bool CanPublish = !Model.packageDir(Pkg).isEmpty();
        if (!PkgCid.isEmpty())
            menu.addAction("Copy package CID", this, [PkgCid]{ QApplication::clipboard()->setText(PkgCid); });
        if (CanPublish)
            menu.addAction(PkgCid.isEmpty() ? QStringLiteral("Publish package CID…")
                                            : QStringLiteral("Re-publish package CID…"),
                           this, [this, Pkg]{ Model.publishPackage(Pkg); });
        if (!menu.isEmpty()) menu.addSeparator();
        menu.addAction(Clicked->isExpanded() ? "Collapse" : "Expand",
                       this, [Clicked]{ Clicked->setExpanded(!Clicked->isExpanded()); });
    }

    if (!menu.isEmpty()) menu.addSeparator();
    menu.addAction("Expand all",   this, [this]{ IpfsPins->expandAll(); });
    menu.addAction("Collapse all", this, [this]{ IpfsPins->collapseAll(); });
    menu.exec(IpfsPins->viewport()->mapToGlobal(pos));
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
        g->setText(1, QString("%1 item%2 · %3").arg(n).arg(n == 1 ? "" : "s").arg(HumanBytesQ(GrpTotal)));
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
        c->setText(1, QString("%1 item%2 · %3").arg(CatItems).arg(CatItems == 1 ? "" : "s").arg(HumanBytesQ(CatTotal)));
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
    auto Rate = [](double Bps){ return Bps >= 1.0 ? (HumanBytesQ((long long)Bps) + "/s") : QStringLiteral("—"); };
    auto Set = [this](int col, const QString & text, const QColor & fg = QColor()){
        QTableWidgetItem * it = IpfsStatusTable->item(0, col);
        it->setText(text);
        it->setForeground(fg.isValid() ? fg : QColor("#c6d4df"));
    };
    Set(0, S.daemon ? QStringLiteral("● connected") : QStringLiteral("● connecting…"),
           S.daemon ? QColor("#5fb55f") : QColor("#d6a23e"));
    Set(1, S.daemon ? QString::number(S.peers) : QStringLiteral("—"));
    Set(2, QString("%1 · %2").arg(S.pinCount).arg(HumanBytesQ(S.totalSize)));
    Set(3, Rate(S.downBps), S.downBps >= 1.0 ? QColor("#5fb55f") : QColor("#8f98a0"));
    Set(4, Rate(S.upBps),   S.upBps   >= 1.0 ? QColor("#4a90d9") : QColor("#8f98a0"));
    Set(5, S.repo.isEmpty() ? QStringLiteral("—") : S.repo);
    Set(6, S.diskFree < 0 ? QStringLiteral("—") : QString("%1 free").arg(HumanBytesQ(S.diskFree)));
}
