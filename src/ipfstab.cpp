#include "ipfstab.h"
#include "mainwindow.h"        // friend access: Mw.GlobalConfigJSON
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

// Item role marking a transfer's progress cell as being in the "pinning" (post-download re-reference) phase, so the
// delegate paints the same bar dark green instead of the default download colour.
static constexpr int PinningRole = Qt::UserRole + 1;

// Paints a column's integer value (0..100) as a progress bar (value lives in the item, so the table stays sortable).
// A negative value renders an indeterminate "busy" bar. PinningRole → the chunk is painted dark green.
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
        bar.palette = opt.palette;
        if (idx.data(PinningRole).toBool())
            bar.palette.setColor(QPalette::Highlight, QColor(0x1B, 0x5E, 0x20));   // dark green during pinning
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

// A sortable table item whose display text is human bytes but whose sort key is the raw byte count.
class ByteSizeItem : public QTableWidgetItem
{
public:
    explicit ByteSizeItem(long long Bytes) : QTableWidgetItem(HumanBytes(Bytes)) { setData(Qt::UserRole, (qlonglong)Bytes); }
    bool operator<(const QTableWidgetItem & O) const override
    { return data(Qt::UserRole).toLongLong() < O.data(Qt::UserRole).toLongLong(); }
};

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
static QHash<QString, QString> BuildCidLabels(const nlohmann::ordered_json & gc, QHash<QString, QString> * OutPackages = nullptr)
{
    QHash<QString, QString> Labels;
    NodeIndex Idx = PackageCatalog::BuildCatalogIndex(gc);
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
        if (PkgName.empty()) PkgName = N.Meta.is_object() ? N.Meta.value("TITLE", N.GroupKey()) : N.GroupKey();

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

IpfsTab::IpfsTab(MainWindow &Owner, QWidget *parent)
    : QWidget(parent), Mw(Owner)
{
    buildUi();
}

void IpfsTab::setActive(bool On)
{
    if (!IpfsRefreshTimer) return;   // ipfs unavailable → no timer
    if (On) { if (!IpfsRefreshTimer->isActive()) IpfsRefreshTimer->start(5000); refresh(); }
    else    IpfsRefreshTimer->stop();
}

void IpfsTab::queueTransfer(const QString &cid)
{
    if (!IpfsTransfers) return;
    ensureTransferRow(cid, QStringLiteral("Queued"));
    IpfsTransferQueued.insert(cid);
}

void IpfsTab::clearQueuedTransfer(const QString &cid)
{
    if (!IpfsTransferQueued.remove(cid)) return;
    if (QTableWidgetItem * p = IpfsTransferProgress.value(cid, nullptr))
    {
        const int r = IpfsTransfers ? IpfsTransfers->row(p) : -1;
        if (r >= 0) IpfsTransfers->removeRow(r);
        IpfsTransferProgress.remove(cid);
        IpfsTransferSpeed.remove(cid);
    }
}

QTableWidgetItem * IpfsTab::ensureTransferRow(const QString & cid, const QString & status)
{
    if (!IpfsTransfers) return nullptr;
    if (QTableWidgetItem * prog = IpfsTransferProgress.value(cid, nullptr)) return prog;   // already have a row

    if (!IpfsCidLabels.contains(cid)) IpfsCidLabels = BuildCidLabels(*Mw.GlobalConfigJSON, &IpfsCidPackages);
    IpfsTransfers->setSortingEnabled(false);                       // keep the row intact while we fill it
    const int row = IpfsTransfers->rowCount(); IpfsTransfers->insertRow(row);
    IpfsTransfers->setItem(row, 0, new QTableWidgetItem(IpfsCidLabels.value(cid, QStringLiteral("(unknown)"))));
    IpfsTransfers->setItem(row, 1, new ByteSizeItem(IpfsCidStat.value(cid).SizeBytes));
    QTableWidgetItem * prog = new QTableWidgetItem(); prog->setData(Qt::DisplayRole, -1);   // indeterminate until first %
    IpfsTransfers->setItem(row, 2, prog);
    IpfsTransfers->setItem(row, 3, new QTableWidgetItem());        // Speed — blank until progress arrives
    IpfsTransfers->setItem(row, 4, new QTableWidgetItem(status));
    IpfsTransfers->setItem(row, 5, new QTableWidgetItem(cid));
    IpfsTransfers->setSortingEnabled(true);
    IpfsTransferProgress.insert(cid, prog);

    // Async-fill the total size if we don't have it cached (one cheap `files stat`, off the GUI thread).
    if (IpfsCidStat.value(cid).SizeBytes < 0)
        std::thread([this, cid]{
            const long long Sz = IpfsWrapper::CidSize(cid.toStdString());
            QMetaObject::invokeMethod(this, [this, cid, Sz]{
                if (Sz >= 0) IpfsCidStat[cid].SizeBytes = Sz;
                QTableWidgetItem * p = IpfsTransferProgress.value(cid, nullptr);
                const int r = p ? IpfsTransfers->row(p) : -1;
                if (r >= 0 && IpfsTransfers->item(r, 1))
                { IpfsTransfers->item(r, 1)->setData(Qt::UserRole, (qlonglong)Sz);
                  IpfsTransfers->item(r, 1)->setText(HumanBytes(Sz)); }
            }, Qt::QueuedConnection);
        }).detach();
    return prog;
}

void IpfsTab::buildUi()
{
    QVBoxLayout * v = new QVBoxLayout(this);
    v->setContentsMargins(12,12,12,12);

    if (!IpfsWrapper::Available())
    {
        QLabel * msg = new QLabel(
            "<div style='color:#8f98a0;'><b>IPFS is unavailable</b><br><br>"
            "VidyaGod's built-in IPFS node didn't start, so shared packages can't be fetched or seeded.<br><br>"
            "Check the logs and restart VidyaGod.</div>", this);
        msg->setAlignment(Qt::AlignCenter); msg->setWordWrap(true);
        v->addStretch(1); v->addWidget(msg); v->addStretch(1);
        return;                                              // greyed/empty — no controls
    }

    // Status row.
    QHBoxLayout * statusRow = new QHBoxLayout();
    IpfsStatusLabel = new QLabel(QStringLiteral("…"), this);
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

    statusRow->addWidget(IpfsStatusLabel, 1);
    statusRow->addWidget(seedBtn);
    statusRow->addWidget(refreshBtn);
    v->addLayout(statusRow);

    IpfsCidLabels = BuildCidLabels(*Mw.GlobalConfigJSON, &IpfsCidPackages);

    // Transfers (live fetches).
    QGroupBox * txBox = new QGroupBox("Transfers", this);
    QVBoxLayout * txl = new QVBoxLayout(txBox);
    IpfsTransfers = new QTableWidget(0, 6, txBox);
    IpfsTransfers->setHorizontalHeaderLabels({"Name", "Size", "Progress", "Speed", "Status", "CID"});
    IpfsTransfers->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    IpfsTransfers->setItemDelegateForColumn(2, new ProgressBarDelegate(IpfsTransfers));
    IpfsTransfers->setEditTriggers(QAbstractItemView::NoEditTriggers);
    IpfsTransfers->setSelectionMode(QAbstractItemView::NoSelection);
    IpfsTransfers->verticalHeader()->setVisible(false);
    IpfsTransfers->setSortingEnabled(true);
    IpfsTransfers->sortByColumn(0, Qt::AscendingOrder);
    txl->addWidget(IpfsTransfers);
    v->addWidget(txBox, 1);

    // Seeded content (pinned CIDs), grouped by package.
    QGroupBox * pinBox = new QGroupBox("Seeded content (pinned)", this);
    QVBoxLayout * pl = new QVBoxLayout(pinBox);
    IpfsPins = new QTreeWidget(pinBox);
    IpfsPins->setColumnCount(5);
    IpfsPins->setHeaderLabels({"Name", "Size", "Health", "CID", ""});
    IpfsPins->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    IpfsPins->setEditTriggers(QAbstractItemView::NoEditTriggers);
    IpfsPins->setSelectionMode(QAbstractItemView::NoSelection);
    IpfsPins->setSortingEnabled(true);
    IpfsPins->sortByColumn(0, Qt::AscendingOrder);
    pl->addWidget(IpfsPins);
    v->addWidget(pinBox, 1);

    // Live transfer notifications (marshalled onto the GUI thread by IpfsManager).
    IpfsManager * mgr = IpfsManager::instance();
    connect(mgr, &IpfsManager::transferStarted, this, [this](QString cid) {
        if (!IpfsTransfers) return;
        const bool Existed = IpfsTransferProgress.contains(cid);
        ensureTransferRow(cid, QStringLiteral("Fetching…"));
        if (QTableWidgetItem * prog = IpfsTransferProgress.value(cid, nullptr))
            prog->setData(PinningRole, false);   // fresh download → default bar colour (clears any prior pinning flag)
        if (Existed)   // was a "Queued" (or re-transferred) row — flip its status to active
            if (QTableWidgetItem * prog = IpfsTransferProgress.value(cid, nullptr))
            { const int row = IpfsTransfers->row(prog);
              if (row >= 0 && IpfsTransfers->item(row, 4)) IpfsTransfers->item(row, 4)->setText("Fetching…"); }
        IpfsTransferQueued.remove(cid);
        const qlonglong Now = QDateTime::currentMSecsSinceEpoch();
        IpfsTransferSpeed.insert(cid, qMakePair((qlonglong)0, Now));  // start the rate clock
        IpfsTransferLastProgress.insert(cid, Now);                    // start the stall clock
        IpfsTransferStalled.remove(cid);
    });
    connect(mgr, &IpfsManager::transferProgress, this, [this](QString cid, double pct) {
        QTableWidgetItem * prog = IpfsTransferProgress.value(cid, nullptr);
        if (!prog) return;
        prog->setData(Qt::DisplayRole, int(pct + 0.5));

        const qlonglong NowMs = QDateTime::currentMSecsSinceEpoch();
        IpfsTransferLastProgress.insert(cid, NowMs);   // bytes are flowing → reset the stall clock
        if (IpfsTransferStalled.remove(cid)) {         // was flagged stalled — peer(s) came back, flip status active
            const int r = IpfsTransfers->row(prog);
            if (r >= 0 && IpfsTransfers->item(r, 4)) IpfsTransfers->item(r, 4)->setText("Fetching…");
        }

        const long long Size = IpfsCidStat.value(cid).SizeBytes;
        if (Size < 0) return;
        const qlonglong Bytes = (qlonglong)(pct / 100.0 * double(Size));
        const qlonglong Now   = QDateTime::currentMSecsSinceEpoch();
        const QPair<qlonglong,qlonglong> Sample = IpfsTransferSpeed.value(cid, qMakePair((qlonglong)0, Now));
        const qlonglong Dt = Now - Sample.second;
        if (Dt >= 500) {
            const qlonglong Rate = Dt > 0 ? (Bytes - Sample.first) * 1000 / Dt : 0;
            const int row = IpfsTransfers->row(prog);
            if (row >= 0 && IpfsTransfers->item(row, 3))
                IpfsTransfers->item(row, 3)->setText(Rate > 0 ? (HumanBytes(Rate) + "/s") : QString());
            IpfsTransferSpeed.insert(cid, qMakePair(Bytes, Now));
        }
    });
    connect(mgr, &IpfsManager::transferFinalizing, this, [this](QString cid, double percent) {
        IpfsTransferSpeed.remove(cid);
        IpfsTransferLastProgress.remove(cid);   // finalizing isn't a stall — stop watching it
        IpfsTransferStalled.remove(cid);
        if (QTableWidgetItem * prog = IpfsTransferProgress.value(cid, nullptr)) {
            prog->setData(PinningRole, true);                                       // → delegate paints the bar dark green
            prog->setData(Qt::DisplayRole, percent >= 0 ? int(percent + 0.5) : -1); // same bar; -1 = indeterminate "busy"
            const int row = IpfsTransfers->row(prog);
            if (row >= 0 && IpfsTransfers->item(row, 3)) IpfsTransfers->item(row, 3)->setText(QString());   // clear Speed
            if (row >= 0 && IpfsTransfers->item(row, 4)) IpfsTransfers->item(row, 4)->setText("Pinning…");
        }
    });
    connect(mgr, &IpfsManager::transferFinished, this, [this](QString cid, bool ok, QString error) {
        IpfsTransferQueued.remove(cid);
        IpfsTransferSpeed.remove(cid);
        IpfsTransferLastProgress.remove(cid);
        IpfsTransferStalled.remove(cid);
        if (QTableWidgetItem * prog = IpfsTransferProgress.value(cid, nullptr)) {
            const int row = IpfsTransfers->row(prog);
            if (row >= 0 && IpfsTransfers->item(row, 3)) IpfsTransfers->item(row, 3)->setText(QString());  // clear Speed
            if (ok) {
                prog->setData(Qt::DisplayRole, 100);
                if (row >= 0 && IpfsTransfers->item(row, 4)) IpfsTransfers->item(row, 4)->setText("Done");
                QTimer::singleShot(1500, this, [this, cid]{
                    if (QTableWidgetItem * p = IpfsTransferProgress.value(cid, nullptr)) {
                        const int r = IpfsTransfers->row(p);
                        if (r >= 0) IpfsTransfers->removeRow(r);
                        IpfsTransferProgress.remove(cid);
                    }
                });
            } else if (row >= 0 && IpfsTransfers->item(row, 4)) {
                const QString Tail = error.section('\n', -1).trimmed();
                const QString Reason = (Tail == "missing files") ? QStringLiteral("Errored: missing files")
                                     : error.isEmpty()           ? QStringLiteral("Failed")
                                                                 : ("Failed: " + Tail);
                QTableWidgetItem * st = IpfsTransfers->item(row, 4);
                st->setText(Reason);
                st->setToolTip(error.isEmpty() ? QStringLiteral("Download failed") : error);
                st->setForeground(QColor("#c0726a"));
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
        if (!IpfsTransfers || IpfsTransferLastProgress.isEmpty()) return;
        const qlonglong Now = QDateTime::currentMSecsSinceEpoch();
        constexpr qlonglong StallMs = 6000;
        for (auto it = IpfsTransferLastProgress.constBegin(); it != IpfsTransferLastProgress.constEnd(); ++it) {
            const QString & Cid = it.key();
            if (Now - it.value() < StallMs || IpfsTransferStalled.contains(Cid)) continue;
            QTableWidgetItem * prog = IpfsTransferProgress.value(Cid, nullptr);
            if (!prog) continue;
            const int row = IpfsTransfers->row(prog);
            if (row < 0) continue;
            IpfsTransferStalled.insert(Cid);
            if (IpfsTransfers->item(row, 3)) IpfsTransfers->item(row, 3)->setText(QString());
            if (IpfsTransfers->item(row, 4)) IpfsTransfers->item(row, 4)->setText("Stalled — waiting for peers…");
        }
    });
    IpfsStallTimer->start();
}

void IpfsTab::refresh()
{
    if (!IpfsWrapper::Available() || !IpfsStatusLabel) return;
    if (IpfsRefreshInFlight) return;                                       // a gather is already running
    IpfsRefreshInFlight = true;

    QSet<QString> HaveSize;
    for (auto it = IpfsCidStat.constBegin(); it != IpfsCidStat.constEnd(); ++it)
        if (it.value().SizeBytes >= 0) HaveSize.insert(it.key());
    std::thread([this, HaveSize]{
        const bool   Daemon = IpfsWrapper::DaemonRunning();
        const int    Peers  = Daemon ? IpfsWrapper::PeerCount() : 0;
        const QString Repo  = QString::fromStdString(IpfsWrapper::RepoSizeHuman());
        const std::vector<IpfsWrapper::PinEntry> Pins = IpfsWrapper::Pins();
        QHash<QString, long long> Sizes;
        for (const auto & P : Pins)
        {
            const QString C = QString::fromStdString(P.Cid);
            if (!HaveSize.contains(C)) { const long long S = IpfsWrapper::CidSize(P.Cid); if (S >= 0) Sizes[C] = S; }
        }
        QMetaObject::invokeMethod(this, [this, Daemon, Peers, Repo, Pins, Sizes]{
            IpfsRefreshInFlight = false;
            applySnapshot(Daemon, Peers, Repo, Pins, Sizes);
        }, Qt::QueuedConnection);
    }).detach();
}

void IpfsTab::applySnapshot(bool Daemon, int Peers, const QString & Repo,
                            const std::vector<IpfsWrapper::PinEntry> & Pins,
                            const QHash<QString, long long> & Sizes)
{
    if (!IpfsStatusLabel) return;
    for (auto it = Sizes.constBegin(); it != Sizes.constEnd(); ++it) IpfsCidStat[it.key()].SizeBytes = it.value();  // merge sizes

    long long Total = 0;
    for (const auto & P : Pins) { const long long s = IpfsCidStat.value(QString::fromStdString(P.Cid)).SizeBytes; if (s >= 0) Total += s; }

    QString S = QString("Network: %1").arg(Daemon
        ? "<span style='color:#5fb55f;'>connected</span>"
        : "<span style='color:#c0726a;'>connecting…</span>");
    if (Daemon) S += QString("   •   Peers: %1").arg(Peers);
    if (!Repo.isEmpty()) S += QString("   •   Repo: %1").arg(Repo);
    S += QString("   •   Seeded: %1 item%2 · %3").arg(Pins.size()).arg(Pins.size() == 1 ? "" : "s").arg(HumanBytes(Total));
    {
        std::error_code Ec;
        const auto Sp = std::filesystem::space(PackageCatalog::LibraryRootDir(*Mw.GlobalConfigJSON), Ec);
        if (!Ec) S += QString("   •   Disk: %1 free of %2").arg(HumanBytes((long long)Sp.available)).arg(HumanBytes((long long)Sp.capacity));
    }
    IpfsStatusLabel->setText(S);

    if (!IpfsPins) return;
    IpfsCidLabels = BuildCidLabels(*Mw.GlobalConfigJSON, &IpfsCidPackages);   // keep names current with the catalog

    QMap<QString, QStringList> ByPackage;
    QSet<QString> DesiredCids;
    const QString Unknown = QStringLiteral("Unknown / not in your library");
    for (const auto & P : Pins)
    {
        const QString Cid = QString::fromStdString(P.Cid);
        ByPackage[IpfsCidPackages.value(Cid, Unknown)].append(Cid);
        DesiredCids.insert(Cid);
    }

    QScrollBar * VBar = IpfsPins->verticalScrollBar();
    const int Scroll = VBar ? VBar->value() : 0;
    IpfsPins->setSortingEnabled(false);

    for (const QString & Cid : IpfsPinChildren.keys())
        if (!DesiredCids.contains(Cid)) delete IpfsPinChildren.take(Cid);
    for (const QString & Pkg : IpfsPinGroups.keys())
        if (!ByPackage.contains(Pkg))
        {
            QTreeWidgetItem * g = IpfsPinGroups.take(Pkg);
            for (const QString & Cid : IpfsPinChildren.keys())
                if (IpfsPinChildren.value(Cid) && IpfsPinChildren.value(Cid)->parent() == g) IpfsPinChildren.remove(Cid);
            delete g;
        }

    for (auto it = ByPackage.constBegin(); it != ByPackage.constEnd(); ++it)
    {
        const QString & PkgName = it.key();
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
        long long GrpTotal = 0;
        for (const QString & Cid : it.value()) { const long long s = IpfsCidStat.value(Cid).SizeBytes; if (s >= 0) GrpTotal += s; }
        grp->setText(1, QString("%1 item%2 · %3").arg(it.value().size()).arg(it.value().size() == 1 ? "" : "s").arg(HumanBytes(GrpTotal)));

        for (const QString & Cid : it.value())
        {
            QTreeWidgetItem * child = IpfsPinChildren.value(Cid, nullptr);
            if (!child)
            {
                const QString Label = IpfsCidLabels.value(Cid, QStringLiteral("(unknown)"));
                QString Leaf = Label;
                if (Label.startsWith(PkgName + " — ")) Leaf = Label.mid(PkgName.size() + 3);
                else if (Label == PkgName)             Leaf = QStringLiteral("content");
                child = new QTreeWidgetItem(grp);
                child->setText(0, Leaf);
                child->setText(3, Cid);
                QWidget * cell = new QWidget();
                QHBoxLayout * cl = new QHBoxLayout(cell); cl->setContentsMargins(2,1,2,1); cl->setSpacing(4);
                QPushButton * copyBtn  = new QPushButton("Copy CID", cell);
                QPushButton * unpinBtn = new QPushButton("Unpin", cell);
                connect(copyBtn,  &QPushButton::clicked, this, [Cid]{ QApplication::clipboard()->setText(Cid); });
                connect(unpinBtn, &QPushButton::clicked, this, [this, Cid]{ IpfsWrapper::Unpin(Cid.toStdString()); refresh(); });
                cl->addWidget(copyBtn); cl->addWidget(unpinBtn); cl->addStretch();
                IpfsPins->setItemWidget(child, 4, cell);
                IpfsPinChildren.insert(Cid, child);
            }
            const IpfsWrapper::StatInfo St = IpfsCidStat.value(Cid);
            child->setText(1, HumanBytes(St.SizeBytes));
            child->setData(1, Qt::UserRole, (qlonglong)St.SizeBytes);
            const auto [Txt, Col] = IpfsHealthText(St.Providers, St.Missing);
            child->setText(2, Txt); child->setForeground(2, Col);
        }
    }

    IpfsPins->setSortingEnabled(true);
    if (VBar) VBar->setValue(Scroll);
    IpfsPins->resizeColumnToContents(2);
    IpfsPins->resizeColumnToContents(3);
    IpfsPins->resizeColumnToContents(4);
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
