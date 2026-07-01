#include "ipfstab.h"
#include "appmodel.h"           // AppModel — Mw.GlobalConfigJSON → Model.config()
#include "packagecatalog.h"
#include "manifestmodel.h"
#include "commonutils.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTreeWidget>
#include <QHeaderView>
#include <QTimer>
#include <QFileDialog>
#include <QMessageBox>
#include <QApplication>
#include <QClipboard>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QStyleOptionProgressBar>
#include <QPainter>
#include <QStyle>
#include <QDateTime>
#include <QColor>
#include <QFont>
#include <QDir>
#include <QMap>
#include <QTableWidgetItem>
#include <QTreeWidgetItem>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

// ===== IPFS-tab-local helpers (all formerly file-statics in mainwindow.cpp) =====

// Item role carrying a transfer's lifecycle status, so the delegate colour-codes the progress bar.
static constexpr int StatusRole = Qt::UserRole + 1;
enum TransferStatus { StDownloading = 0, StQueued = 1, StPinning = 2, StStalled = 3, StErrored = 4 };

// Paints a column's integer value (0..100) as a progress bar (value lives in the item, so the table stays sortable).
// A negative value renders an indeterminate "busy" bar. The bar is colour-coded by StatusRole:
// queued = purple, downloading = default (blue), pinning = dark green, stalled = amber, errored = red.
class ProgressBarDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override
    {
        // Only cells that carry an explicit progress value render a bar — package-group rows and idle seeded leaves
        // (no value in the Progress column) fall through to the default painter (blank), so no bogus 0% bars.
        const QVariant dv = idx.data(Qt::DisplayRole);
        if (!dv.isValid()) { QStyledItemDelegate::paint(p, opt, idx); return; }
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
        case StQueued:  c = QColor(0x7E, 0x57, 0xC2); break;   // purple
        case StPinning: c = QColor(0x1B, 0x5E, 0x20); break;   // dark green
        case StStalled: c = QColor(0xF9, 0xA8, 0x25); break;   // amber
        case StErrored: c = QColor(0xC0, 0x39, 0x2B); break;   // red
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

// Maps every ipfs SOURCE CID in the catalog to a human label ("<package> — <component>") and (optionally) its owning
// package name, so the IPFS tab can show what each CID is and group the seeded list by package.
// Build the CID → label/package maps from the ALREADY-BUILT in-memory catalog index. Must NOT re-scan the catalog
// from disk (BuildCatalogIndex): this runs on the GUI thread (ensureTransferRow + applySnapshot, the latter on every
// refresh), so a disk rescan here froze the UI for many seconds during downloads.
static QHash<QString, QString> BuildCidLabels(const NodeIndex & Idx, QHash<QString, QString> * OutPackages = nullptr)
{
    QHash<QString, QString> Labels;
    for (const auto & [NodeId, N] : Idx.Nodes)
    {
        std::string PkgName;
        {
            std::string F = N.BundleDir.filename().string();
            size_t i = 0;
            while (i < F.size()) { if (F[i] == '[') { size_t c = F.find(']', i); if (c == std::string::npos) break; i = c + 1; }
                                   else if (F[i] == ' ') ++i; else break; }
            PkgName = (i < F.size()) ? F.substr(i) : F;
        }
        if (PkgName.empty()) PkgName = N.Meta.is_object() ? N.Meta.value("TITLE", N.GameKey()) : N.GameKey();

        if (N.Layers.is_array())
            for (const auto & L : N.Layers)
            {
                if (!ManifestModel::IsVfsLayer(L.value("TYPE", std::string()))) continue;
                if (!L.contains("SOURCE") || !L["SOURCE"].is_object() || L["SOURCE"].value("TYPE", std::string()) != "ipfs") continue;
                const std::string Cid = L["SOURCE"].value("CID", std::string());
                if (Cid.empty()) continue;
                Labels.insert(QString::fromStdString(Cid), QString::fromStdString(NodeId));
                if (OutPackages)
                    OutPackages->insert(QString::fromStdString(Cid), QString::fromStdString(PkgName.empty() ? std::string("(unnamed)") : PkgName));
            }

        if (N.Meta.is_object() && N.Meta.contains("COVER") && N.Meta["COVER"].is_object())
        {
            const auto & Cv = N.Meta["COVER"];
            if (Cv.contains("SOURCE") && Cv["SOURCE"].is_object() && Cv["SOURCE"].value("TYPE", std::string()) == "ipfs")
            {
                const std::string Cid = Cv["SOURCE"].value("CID", std::string());
                if (!Cid.empty())
                {
                    Labels.insert(QString::fromStdString(Cid), QString::fromStdString(PkgName + " — cover"));
                    if (OutPackages) OutPackages->insert(QString::fromStdString(Cid), QStringLiteral("Assets"));
                }
            }
        }
    }
    return Labels;
}

// ===== IpfsTab =====

IpfsTab::IpfsTab(AppModel &model, QWidget *parent)
    : QWidget(parent), Model(model)
{
    buildUi();
    connect(&Model, &AppModel::catalogChanged, this, &IpfsTab::refresh);   // pins change on import/sync/download
}

void IpfsTab::setActive(bool On)
{
    if (!IpfsRefreshTimer) return;   // ipfs unavailable → no timer
    if (On) { if (!IpfsRefreshTimer->isActive()) IpfsRefreshTimer->start(5000); refresh(); }
    else    IpfsRefreshTimer->stop();
}

void IpfsTab::queueTransfer(const QString &cid)
{
    if (QTreeWidgetItem * leaf = ensureLeaf(cid))
    {
        leaf->setData(2, Qt::DisplayRole, -1);         // indeterminate bar until first %
        leaf->setData(2, StatusRole, StQueued);        // → delegate paints the bar purple
        leaf->setText(4, QStringLiteral("Queued"));
        if (leaf->parent()) leaf->parent()->setExpanded(true);
    }
    IpfsTransferQueued.insert(cid);
}

void IpfsTab::clearQueuedTransfer(const QString &cid)
{
    if (!IpfsTransferQueued.remove(cid)) return;
    IpfsTransferProgress.remove(cid);
    IpfsTransferSpeed.remove(cid);
    // Drop the leaf only if it isn't seeded (a still-queued row that never started + isn't pinned is just noise).
    if (IpfsPinChildren.contains(cid) && !IpfsWrapper::HasLocal(cid.toStdString()))
        delete IpfsPinChildren.take(cid);
    refresh();
}

// Find or create a CID's leaf in the unified tree, under its package group (creating the group if needed). Shared by
// the transfer signal handlers and the seeded-pin render in applySnapshot, so a downloading CID and the same CID once
// seeded are the SAME row. Idle leaves carry no Progress value (blank bar); the caller sets transfer/seeded state.
QTreeWidgetItem * IpfsTab::ensureLeaf(const QString & cid)
{
    if (!IpfsPins) return nullptr;
    if (QTreeWidgetItem * leaf = IpfsPinChildren.value(cid, nullptr)) return leaf;   // already have a row

    if (!IpfsCidLabels.contains(cid)) IpfsCidLabels = BuildCidLabels(Model.catalogIndex(), &IpfsCidPackages);
    const QString PkgName = IpfsCidPackages.value(cid, QStringLiteral("Unknown / not in your library"));
    QTreeWidgetItem * grp = IpfsPinGroups.value(PkgName, nullptr);
    if (!grp)
    {
        grp = new QTreeWidgetItem(IpfsPins);
        grp->setText(0, PkgName);
        QFont f = grp->font(0); f.setBold(true); grp->setFont(0, f); grp->setFont(1, f);
        grp->setFlags(grp->flags() & ~Qt::ItemIsSelectable);
        grp->setExpanded(false);
        IpfsPinGroups.insert(PkgName, grp);
    }

    IpfsPins->setSortingEnabled(false);
    const QString Label = IpfsCidLabels.value(cid, QStringLiteral("(unknown)"));
    QString LeafName = Label;
    if (Label.startsWith(PkgName + " — ")) LeafName = Label.mid(PkgName.size() + 3);
    else if (Label == PkgName)             LeafName = QStringLiteral("content");
    QTreeWidgetItem * child = new QTreeWidgetItem(grp);
    child->setText(0, LeafName);
    child->setText(5, cid);
    const long long Sz = IpfsCidStat.value(cid).SizeBytes;
    child->setText(1, HumanBytes(Sz));
    child->setData(1, Qt::UserRole, (qlonglong)Sz);

    QWidget * cell = new QWidget();
    QHBoxLayout * cl = new QHBoxLayout(cell); cl->setContentsMargins(2,1,2,1); cl->setSpacing(4);
    QPushButton * copyBtn  = new QPushButton("Copy CID", cell);
    QPushButton * unpinBtn = new QPushButton("Unpin", cell);
    connect(copyBtn,  &QPushButton::clicked, this, [cid]{ QApplication::clipboard()->setText(cid); });
    connect(unpinBtn, &QPushButton::clicked, this, [this, cid]{ IpfsWrapper::Unpin(cid.toStdString()); refresh(); });
    cl->addWidget(copyBtn); cl->addWidget(unpinBtn); cl->addStretch();
    IpfsPins->setItemWidget(child, 6, cell);
    IpfsPinChildren.insert(cid, child);
    IpfsPins->setSortingEnabled(true);

    // Async-fill the total size if we don't have it cached (one cheap stat, off the GUI thread).
    if (Sz < 0)
        std::thread([this, cid]{
            const long long S = IpfsWrapper::CidSize(cid.toStdString());
            QMetaObject::invokeMethod(this, [this, cid, S]{
                if (S >= 0) IpfsCidStat[cid].SizeBytes = S;
                if (QTreeWidgetItem * l = IpfsPinChildren.value(cid, nullptr))
                { l->setText(1, HumanBytes(S)); l->setData(1, Qt::UserRole, (qlonglong)S); }
            }, Qt::QueuedConnection);
        }).detach();
    return child;
}

void IpfsTab::buildUi()
{
    QVBoxLayout * v = new QVBoxLayout(this);
    v->setContentsMargins(12,12,12,12);

    // The full UI is ALWAYS built (networking is off by default, so the node may not be running yet — it starts when
    // the user enables networking). When the node isn't up, refresh() shows an "off" status; once it comes up,
    // MainWindow::onNodeReady() / the periodic tick populate the live status + tables. (Previously this built a
    // permanent "unavailable" stub when the node wasn't running at construction, leaving the tab dead after enabling.)

    // Status strip: a single-row table (Network | Peers | Seeded | ↓ Down | ↑ Up | Repo | Disk free), plus a hint
    // label shown only when the node is off.
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
    // (added to the layout at the BOTTOM of buildUi, beneath the transfers + seeded tables)

    IpfsHintLabel = new QLabel("IPFS node is off — enable networking in Settings → IPFS to download and seed.", this);
    IpfsHintLabel->setStyleSheet("color:#8f98a0;font-size:9pt;");
    IpfsHintLabel->hide();

    // Button row (seed + refresh).
    QHBoxLayout * statusRow = new QHBoxLayout();
    QPushButton * refreshBtn = new QPushButton("Refresh", this);
    connect(refreshBtn, &QPushButton::clicked, this, [this]{ IpfsCidStat.clear(); refresh(); });  // force re-stat (fresh size/health)

    QPushButton * seedBtn = new QPushButton("Seed folder…", this);
    seedBtn->setToolTip("Add a folder's published content to the IPFS node so it seeds (e.g. your master library).");
    connect(seedBtn, &QPushButton::clicked, this, [this, seedBtn]{
        const QString TheVidya = QDir::homePath() + "/The Vidya";
        const QString Def = QDir(TheVidya).exists() ? TheVidya : QDir::homePath();
        const QString Dir = QFileDialog::getExistingDirectory(this, "Seed folder — pick the folder containing your packages", Def);
        if (Dir.isEmpty()) return;
        seedBtn->setEnabled(false); seedBtn->setText("Seeding…");
        std::thread([this, seedBtn, Dir]{
            int Mismatched = 0;
            const int Seeded = PackageCatalog::SeedDirectory(Dir.toStdString(),
                [seedBtn](int done, int total, const std::string &){
                    QMetaObject::invokeMethod(seedBtn, [seedBtn, done, total]{
                        seedBtn->setText(QString("Seeding %1/%2…").arg(done).arg(total)); }, Qt::QueuedConnection);
                }, &Mismatched);
            QMetaObject::invokeMethod(this, [this, seedBtn, Seeded, Mismatched]{
                seedBtn->setText("Seed folder…"); seedBtn->setEnabled(true);
                refresh();
                QString Msg = QString("Seeded %1 referenced file%2.").arg(Seeded).arg(Seeded == 1 ? "" : "s");
                if (Mismatched > 0) Msg += QString("\n\n%1 file%2 changed since publish (or couldn't be added), so the "
                                                   "recorded CID couldn't be re-seeded.").arg(Mismatched).arg(Mismatched == 1 ? "" : "s");
                QMessageBox::information(this, "Seed folder", Msg);
            }, Qt::QueuedConnection);
        }).detach();
    });

    statusRow->addStretch(1);
    statusRow->addWidget(seedBtn);
    statusRow->addWidget(refreshBtn);
    v->addLayout(statusRow);

    IpfsCidLabels = BuildCidLabels(Model.catalogIndex(), &IpfsCidPackages);

    // Unified content view (transfers + seeded, grouped by package). A CID being downloaded and the same CID once
    // seeded are the SAME row — Progress+Speed while fetching, then Status shows seeded health / uploading.
    QGroupBox * pinBox = new QGroupBox("Content", this);
    QVBoxLayout * pl = new QVBoxLayout(pinBox);
    IpfsPins = new QTreeWidget(pinBox);
    IpfsPins->setColumnCount(7);
    IpfsPins->setHeaderLabels({"Name", "Size", "Progress", "Speed", "Status", "CID", ""});
    IpfsPins->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    IpfsPins->setItemDelegateForColumn(2, new ProgressBarDelegate(IpfsPins));
    IpfsPins->setEditTriggers(QAbstractItemView::NoEditTriggers);
    IpfsPins->setSelectionMode(QAbstractItemView::NoSelection);
    IpfsPins->setSortingEnabled(true);
    IpfsPins->sortByColumn(0, Qt::AscendingOrder);
    pl->addWidget(IpfsPins);
    v->addWidget(pinBox, 1);

    // Status strip at the BOTTOM, beneath the transfers + seeded tables.
    v->addWidget(IpfsStatusTable);
    v->addWidget(IpfsHintLabel);

    // Live transfer notifications (marshalled onto the GUI thread by IpfsManager).
    IpfsManager * mgr = IpfsManager::instance();
    connect(mgr, &IpfsManager::transferStarted, this, [this](const QString &cid) {
        QTreeWidgetItem * leaf = ensureLeaf(cid);
        if (!leaf) return;
        IpfsTransferProgress.insert(cid, leaf);
        leaf->setData(2, Qt::DisplayRole, -1);          // indeterminate bar until first %
        leaf->setData(2, StatusRole, StDownloading);    // fresh download → default (blue) bar
        leaf->setText(3, QString());                    // clear any stale speed
        leaf->setText(4, QStringLiteral("Fetching…")); leaf->setForeground(4, QColor("#c6d4df"));
        if (leaf->parent()) leaf->parent()->setExpanded(true);
        IpfsTransferQueued.remove(cid);
        const qlonglong Now = QDateTime::currentMSecsSinceEpoch();
        IpfsTransferSpeed.insert(cid, qMakePair((qlonglong)0, Now));  // start the rate clock
        IpfsTransferLastProgress.insert(cid, Now);                    // start the stall clock
        IpfsTransferStalled.remove(cid);
    });
    connect(mgr, &IpfsManager::transferProgress, this, [this](const QString &cid, double pct) {
        QTreeWidgetItem * leaf = IpfsTransferProgress.value(cid, nullptr);
        if (!leaf) return;
        leaf->setData(2, Qt::DisplayRole, int(pct + 0.5));

        const qlonglong NowMs = QDateTime::currentMSecsSinceEpoch();
        IpfsTransferLastProgress.insert(cid, NowMs);   // bytes are flowing → reset the stall clock
        if (IpfsTransferStalled.remove(cid)) {         // was flagged stalled — peer(s) came back, flip status active
            leaf->setData(2, StatusRole, StDownloading);
            leaf->setText(4, QStringLiteral("Fetching…"));
        }

        const long long Size = IpfsCidStat.value(cid).SizeBytes;
        if (Size < 0) return;
        const qlonglong Bytes = (qlonglong)(pct / 100.0 * double(Size));
        const qlonglong Now   = QDateTime::currentMSecsSinceEpoch();
        const QPair<qlonglong,qlonglong> Sample = IpfsTransferSpeed.value(cid, qMakePair((qlonglong)0, Now));
        const qlonglong Dt = Now - Sample.second;
        if (Dt >= 500) {
            const qlonglong Rate = Dt > 0 ? (Bytes - Sample.first) * 1000 / Dt : 0;
            leaf->setText(3, Rate > 0 ? (HumanBytes(Rate) + "/s") : QString());
            IpfsTransferSpeed.insert(cid, qMakePair(Bytes, Now));
        }
    });
    connect(mgr, &IpfsManager::transferFinalizing, this, [this](const QString &cid, double percent) {
        IpfsTransferSpeed.remove(cid);
        IpfsTransferLastProgress.remove(cid);   // finalizing isn't a stall — stop watching it
        IpfsTransferStalled.remove(cid);
        if (QTreeWidgetItem * leaf = IpfsTransferProgress.value(cid, nullptr)) {
            leaf->setData(2, StatusRole, StPinning);                                   // dark-green bar
            leaf->setData(2, Qt::DisplayRole, percent >= 0 ? int(percent + 0.5) : -1); // -1 = indeterminate "busy"
            leaf->setText(3, QString());                                               // clear Speed
            leaf->setText(4, QStringLiteral("Pinning…"));
        }
    });
    connect(mgr, &IpfsManager::transferFinished, this, [this](const QString &cid, bool ok, const QString &error) {
        IpfsTransferQueued.remove(cid);
        IpfsTransferSpeed.remove(cid);
        IpfsTransferLastProgress.remove(cid);
        IpfsTransferStalled.remove(cid);
        if (QTreeWidgetItem * leaf = IpfsTransferProgress.value(cid, nullptr)) {
            leaf->setText(3, QString());   // clear Speed
            if (ok) {
                // Done → the SAME row becomes a seeded leaf: drop the progress bar, let refresh() render seeded/health.
                leaf->setData(2, Qt::DisplayRole, QVariant());
                leaf->setData(2, StatusRole, QVariant());
                leaf->setText(4, QStringLiteral("Done"));
                IpfsTransferProgress.remove(cid);
            } else {
                const QString Tail = error.section('\n', -1).trimmed();
                if (Tail == "cancelled") {                          // user cancelled → drop the leaf if not seeded
                    IpfsTransferProgress.remove(cid);
                    if (!IpfsWrapper::HasLocal(cid.toStdString())) delete IpfsPinChildren.take(cid);
                } else {
                    leaf->setData(2, StatusRole, StErrored);   // red bar; keep the leaf so the error stays visible
                    const QString Reason = (Tail == "missing files") ? QStringLiteral("Errored: missing files")
                                         : error.isEmpty()           ? QStringLiteral("Failed")
                                                                     : ("Failed: " + Tail);
                    leaf->setText(4, Reason);
                    leaf->setToolTip(4, error.isEmpty() ? QStringLiteral("Download failed") : error);
                    leaf->setForeground(4, QColor("#c0726a"));
                }
            }
        }
        refresh();                                    // a finished fetch likely added a pin
    });

    // Periodic status/pin refresh — started/stopped by setActive() when the tab is shown.
    IpfsRefreshTimer = new QTimer(this);
    connect(IpfsRefreshTimer, &QTimer::timeout, this, [this]{ refresh(); });

    // Stall watchdog (the "still 2 MB/s" fix): no forward progress for a while → clear phantom speed + flag stalled.
    IpfsStallTimer = new QTimer(this);
    IpfsStallTimer->setInterval(2000);
    connect(IpfsStallTimer, &QTimer::timeout, this, [this]{
        if (IpfsTransferLastProgress.isEmpty()) return;
        const qlonglong Now = QDateTime::currentMSecsSinceEpoch();
        constexpr qlonglong StallMs = 6000;
        for (auto it = IpfsTransferLastProgress.constBegin(); it != IpfsTransferLastProgress.constEnd(); ++it) {
            const QString & Cid = it.key();
            if (Now - it.value() < StallMs || IpfsTransferStalled.contains(Cid)) continue;
            QTreeWidgetItem * leaf = IpfsTransferProgress.value(Cid, nullptr);
            if (!leaf) continue;
            IpfsTransferStalled.insert(Cid);
            leaf->setData(2, StatusRole, StStalled);   // → delegate paints the bar amber
            leaf->setText(3, QString());
            leaf->setText(4, QStringLiteral("Stalled — waiting for peers…"));
        }
    });
    IpfsStallTimer->start();

    refresh();   // set the initial status (the "node off" message, or live data if it's already up)
}

void IpfsTab::refresh()
{
    if (!IpfsStatusTable) return;
    if (!IpfsWrapper::Available())
    {
        // Node not running (networking off, or not started yet). Grey the strip + show the hint.
        IpfsHintLabel->show();
        IpfsStatusTable->item(0, 0)->setText(QStringLiteral("off"));
        IpfsStatusTable->item(0, 0)->setForeground(QColor("#c0726a"));
        for (int c = 1; c < IpfsStatusTable->columnCount(); ++c) IpfsStatusTable->item(0, c)->setText(QStringLiteral("—"));
        return;
    }
    IpfsHintLabel->hide();
    if (IpfsRefreshInFlight) return;                                       // a gather is already running
    IpfsRefreshInFlight = true;

    QSet<QString> HaveSize;
    for (auto it = IpfsCidStat.constBegin(); it != IpfsCidStat.constEnd(); ++it)
        if (it.value().SizeBytes >= 0) HaveSize.insert(it.key());
    std::thread([this, HaveSize]{
        const bool   Daemon = IpfsWrapper::DaemonRunning();
        const int    Peers  = Daemon ? IpfsWrapper::PeerCount() : 0;
        const QString Repo  = QString::fromStdString(IpfsWrapper::RepoSizeHuman());
        const IpfsWrapper::BandwidthRates Bw = IpfsWrapper::Bandwidth();
        const std::vector<IpfsWrapper::PinEntry> Pins = IpfsWrapper::Pins();
        QSet<QString> Uploading;                                             // seeded items a peer is pulling right now
        for (const auto & C : IpfsWrapper::ActiveUploads(90000)) Uploading.insert(QString::fromStdString(C));
        QHash<QString, long long> Sizes;
        for (const auto & P : Pins)
        {
            const QString C = QString::fromStdString(P.Cid);
            if (!HaveSize.contains(C)) { const long long S = IpfsWrapper::CidSize(P.Cid); if (S >= 0) Sizes[C] = S; }
        }
        QMetaObject::invokeMethod(this, [this, Daemon, Peers, Repo, Bw, Pins, Sizes, Uploading]{
            IpfsRefreshInFlight = false;
            applySnapshot(Daemon, Peers, Repo, Bw.DownBps, Bw.UpBps, Pins, Sizes, Uploading);
        }, Qt::QueuedConnection);
    }).detach();
}

void IpfsTab::applySnapshot(bool Daemon, int Peers, const QString & Repo, double DownBps, double UpBps,
                            const std::vector<IpfsWrapper::PinEntry> & Pins,
                            const QHash<QString, long long> & Sizes,
                            const QSet<QString> & Uploading)
{
    if (!IpfsStatusTable) return;
    for (auto it = Sizes.constBegin(); it != Sizes.constEnd(); ++it) IpfsCidStat[it.key()].SizeBytes = it.value();  // merge sizes

    long long Total = 0;
    for (const auto & P : Pins) { const long long s = IpfsCidStat.value(QString::fromStdString(P.Cid)).SizeBytes; if (s >= 0) Total += s; }

    auto Rate = [](double Bps){ return Bps >= 1.0 ? (HumanBytes((long long)Bps) + "/s") : QStringLiteral("—"); };
    auto Set = [this](int col, const QString & text, const QColor & fg = QColor()){
        QTableWidgetItem * it = IpfsStatusTable->item(0, col);
        it->setText(text);
        it->setForeground(fg.isValid() ? fg : QColor("#c6d4df"));
    };
    Set(0, Daemon ? QStringLiteral("● connected") : QStringLiteral("● connecting…"),
           Daemon ? QColor("#5fb55f") : QColor("#d6a23e"));
    Set(1, Daemon ? QString::number(Peers) : QStringLiteral("—"));
    Set(2, QString("%1 · %2").arg(Pins.size()).arg(HumanBytes(Total)));
    Set(3, Rate(DownBps), DownBps >= 1.0 ? QColor("#5fb55f") : QColor("#8f98a0"));
    Set(4, Rate(UpBps),   UpBps   >= 1.0 ? QColor("#4a90d9") : QColor("#8f98a0"));
    Set(5, Repo.isEmpty() ? QStringLiteral("—") : Repo);
    {
        std::error_code Ec;
        const auto Sp = std::filesystem::space(PackageCatalog::LibraryRootDir(*Model.config()), Ec);
        Set(6, Ec ? QStringLiteral("—") : QString("%1 free").arg(HumanBytes((long long)Sp.available)));
    }

    if (!IpfsPins) return;
    IpfsCidLabels = BuildCidLabels(Model.catalogIndex(), &IpfsCidPackages);   // keep names current with the catalog

    QMap<QString, QStringList> ByPackage;
    QSet<QString> DesiredCids;
    const QString Unknown = QStringLiteral("Unknown / not in your library");
    for (const auto & P : Pins)
    {
        const QString Cid = QString::fromStdString(P.Cid);
        ByPackage[IpfsCidPackages.value(Cid, Unknown)].append(Cid);
        DesiredCids.insert(Cid);
    }
    for (const QString & Cid : IpfsTransferProgress.keys()) DesiredCids.insert(Cid);   // protect in-flight/failed leaves

    QScrollBar * VBar = IpfsPins->verticalScrollBar();
    const int Scroll = VBar ? VBar->value() : 0;
    IpfsPins->setSortingEnabled(false);

    // Drop leaves that are neither pinned nor mid-transfer.
    for (const QString & Cid : IpfsPinChildren.keys())
        if (!DesiredCids.contains(Cid)) delete IpfsPinChildren.take(Cid);

    // Render each pinned CID's leaf (created via ensureLeaf, so a downloading CID and the seeded CID are the SAME row).
    for (auto it = ByPackage.constBegin(); it != ByPackage.constEnd(); ++it)
        for (const QString & Cid : it.value())
        {
            QTreeWidgetItem * child = ensureLeaf(Cid);
            if (!child) continue;
            const IpfsWrapper::StatInfo St = IpfsCidStat.value(Cid);
            child->setText(1, HumanBytes(St.SizeBytes));
            child->setData(1, Qt::UserRole, (qlonglong)St.SizeBytes);
            if (IpfsTransferProgress.contains(Cid)) continue;    // mid-transfer/failed → leave its Progress + Status alone
            child->setData(2, Qt::DisplayRole, QVariant());      // idle seeded → no progress bar
            child->setData(2, StatusRole, QVariant());
            child->setText(3, QString());                        // no speed
            if (Uploading.contains(Cid))
            {
                child->setText(4, QStringLiteral("⬆ uploading")); child->setForeground(4, QColor("#4a90d9"));
            }
            else
            {
                const auto [Txt, Col] = IpfsHealthText(St.Providers, St.Missing);   // ● N providers seeding it
                child->setText(4, Txt); child->setForeground(4, Col);
            }
        }

    // Group totals + prune empty groups (a cancelled download can leave a childless group behind).
    for (const QString & Pkg : IpfsPinGroups.keys())
    {
        QTreeWidgetItem * g = IpfsPinGroups.value(Pkg, nullptr);
        if (!g) continue;
        const int n = g->childCount();
        if (n == 0) { IpfsPinGroups.remove(Pkg); delete g; continue; }
        long long GrpTotal = 0;
        for (int i = 0; i < n; ++i) GrpTotal += g->child(i)->data(1, Qt::UserRole).toLongLong();
        g->setText(1, QString("%1 item%2 · %3").arg(n).arg(n == 1 ? "" : "s").arg(HumanBytes(GrpTotal)));
    }

    IpfsPins->setSortingEnabled(true);
    if (VBar) VBar->setValue(Scroll);
    IpfsPins->resizeColumnToContents(3);
    IpfsPins->resizeColumnToContents(4);
    IpfsPins->resizeColumnToContents(5);
    if (qEnvironmentVariableIsSet("VIDYAGOD_IPFS_EXPAND")) IpfsPins->expandAll();

    gatherHealth();
}

void IpfsTab::gatherHealth()
{
    if (IpfsHealthInFlight) return;
    auto Todo = std::make_shared<QStringList>();
    for (const QString & Cid : IpfsPinChildren.keys())
        if (IpfsCidStat.value(Cid).Providers == -2) *Todo << Cid;
    if (Todo->isEmpty()) return;
    IpfsHealthInFlight = true;

    const int Workers = std::min<int>(5, Todo->size());
    auto Next      = std::make_shared<std::atomic<int>>(0);
    auto Remaining = std::make_shared<std::atomic<int>>(Workers);
    for (int w = 0; w < Workers; ++w)
        std::thread([this, Todo, Next, Remaining]{
            for (;;)
            {
                const int i = Next->fetch_add(1);
                if (i >= Todo->size()) break;
                const QString Cid = Todo->at(i);
                const std::string C = Cid.toStdString();
                const int M = IpfsWrapper::CidMissing(C) ? 1 : 0;
                const int N = (M == 1) ? -1 : IpfsWrapper::ProviderCount(C);
                QMetaObject::invokeMethod(this, [this, Cid, N, M]{
                    IpfsCidStat[Cid].Providers = N;
                    IpfsCidStat[Cid].Missing   = M;
                    if (QTreeWidgetItem * child = IpfsPinChildren.value(Cid, nullptr))
                    { const auto [Txt, Col] = IpfsHealthText(N, M); child->setText(2, Txt); child->setForeground(2, Col); }
                }, Qt::QueuedConnection);
            }
            if (Remaining->fetch_sub(1) == 1)
                QMetaObject::invokeMethod(this, [this]{ IpfsHealthInFlight = false; }, Qt::QueuedConnection);
        }).detach();
}
