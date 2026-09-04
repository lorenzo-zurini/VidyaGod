#include "networkpage.h"
#include "appmodel.h"
#include "asyncwork.h"
#include "ipfswrapper.h"

#include <QVBoxLayout>
#include <QPushButton>
#include <QTreeWidget>
#include <QHeaderView>
#include <QLabel>
#include <QTimer>

#include <memory>

//One ✓/⚠/✗/– row per service. Shared by the live-health and sweep sections so the two halves read alike.
static void FillStatusRow(QTreeWidgetItem *It, const std::string &Status, const std::string &Name, const std::string &Detail)
{
    const bool Down = Status == "down" || Status == "fail";
    const bool Warn = Status == "warn";
    const bool Off  = Status == "off";
    It->setText(0, Down ? "✗" : Warn ? "⚠" : Off ? "–" : "✓");
    It->setForeground(0, Down ? QBrush(QColor("#cc4444"))
                     : Warn ? QBrush(QColor("#ccaa00"))
                     : Off  ? QBrush(QColor("#8f98a0")) : QBrush(QColor("#44aa44")));
    It->setText(1, QString::fromStdString(Name));
    It->setText(2, QString::fromStdString(Detail));
    It->setToolTip(2, QString::fromStdString(Detail));
}

NetworkPage::NetworkPage(AppModel &model, QWidget *parent)
    : QWidget(parent), Model(model)
{
    QVBoxLayout *Layout = new QVBoxLayout(this);

    // --- Live service health (proactive: polls while the page is visible) ---
    HealthStatus = new QLabel("Service health", this);
    HealthStatus->setStyleSheet("font-weight: bold;");
    Layout->addWidget(HealthStatus);

    HealthTree = new QTreeWidget(this);
    HealthTree->setObjectName("ServiceHealthResults");
    HealthTree->setColumnCount(3);
    HealthTree->setHeaderLabels({"", "Service", "State"});
    HealthTree->setRootIsDecorated(false);
    HealthTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    HealthTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    HealthTree->header()->setStretchLastSection(true);
    HealthTree->setSelectionMode(QAbstractItemView::NoSelection);
    HealthTree->setFocusPolicy(Qt::NoFocus);
    Layout->addWidget(HealthTree, 1);

    HealthTimer = new QTimer(this);
    HealthTimer->setInterval(3000);
    connect(HealthTimer, &QTimer::timeout, this, &NetworkPage::refreshHealth);

    QLabel *Blurb = new QLabel(
        "Checks every network feature against this machine and network: internet reachability, DNS, "
        "peer-to-peer connectivity over TCP and UDP, whether other players can reach you, same-network (LAN) "
        "discovery, content lookups, and the live links to your friends.\n"
        "A firewall that silently drops UDP or multicast makes things merely feel slow — this makes it say so.",
        this);
    Blurb->setWordWrap(true);
    Layout->addWidget(Blurb);

    RunButton = new QPushButton("Run network test", this);
    RunButton->setObjectName("RunNetworkTestButton");
    Layout->addWidget(RunButton, 0, Qt::AlignLeft);

    StatusLabel = new QLabel(this);
    Layout->addWidget(StatusLabel);

    ResultTree = new QTreeWidget(this);
    ResultTree->setObjectName("NetworkTestResults");
    ResultTree->setColumnCount(3);
    ResultTree->setHeaderLabels({"", "Check", "Result"});
    ResultTree->setRootIsDecorated(false);
    ResultTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    ResultTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    ResultTree->header()->setStretchLastSection(true);
    ResultTree->setSelectionMode(QAbstractItemView::NoSelection);
    ResultTree->setFocusPolicy(Qt::NoFocus);
    Layout->addWidget(ResultTree, 1);

    connect(RunButton, &QPushButton::clicked, this, &NetworkPage::runTest);
}

void NetworkPage::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    refreshHealth();          // instant first paint, then the poll keeps it live
    HealthTimer->start();
}

void NetworkPage::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    HealthTimer->stop();
}

void NetworkPage::refreshHealth()
{
    if (HealthBusy) return;   // never stack polls (a stalled node must not queue up work)
    HealthBusy = true;
    auto Rows = std::make_shared<std::vector<IpfsWrapper::NetCheck>>();
    AsyncWork::Run(this,
        [Rows]{ *Rows = IpfsWrapper::ServiceHealth(); },   // cheap, but off-thread so a wedged node can't freeze the GUI
        [this, Rows]{
            HealthBusy = false;
            //Update in place (no clear+rebuild): rebuilding every 3s would fight tooltips and flicker.
            while (HealthTree->topLevelItemCount() > static_cast<int>(Rows->size()))
                delete HealthTree->takeTopLevelItem(HealthTree->topLevelItemCount() - 1);
            int Downs = 0, Warns = 0;
            for (int i = 0; i < static_cast<int>(Rows->size()); ++i)
            {
                const auto &R = (*Rows)[i];
                QTreeWidgetItem *It = i < HealthTree->topLevelItemCount()
                                    ? HealthTree->topLevelItem(i) : new QTreeWidgetItem(HealthTree);
                FillStatusRow(It, R.Status, R.Name, R.Detail);
                Downs += R.Status == "down";
                Warns += R.Status == "warn";
            }
            if (Rows->empty())      HealthStatus->setText("Service health — node not responding");
            else if (Downs)         HealthStatus->setText(QString("Service health — %1 service(s) DOWN").arg(Downs));
            else if (Warns)         HealthStatus->setText(QString("Service health — %1 warning(s)").arg(Warns));
            else                    HealthStatus->setText("Service health — all services healthy");
        });
}

void NetworkPage::runTest()
{
    RunButton->setEnabled(false);
    StatusLabel->setText("Testing… (takes up to ~30 seconds)");
    ResultTree->clear();

    auto Checks  = std::make_shared<std::vector<IpfsWrapper::NetCheck>>();
    auto Offline = std::make_shared<bool>(false);
    AsyncWork::Run(this,
        [Checks, Offline]{ *Checks = IpfsWrapper::NetworkTest(Offline.get()); },
        [this, Checks, Offline]{
            RunButton->setEnabled(true);
            if (*Offline)
            {
                StatusLabel->setText("The node is offline — nothing to probe. Connect first, then run again.");
                return;
            }
            int Fails = 0, Warns = 0;
            for (const auto &C : *Checks)
            {
                FillStatusRow(new QTreeWidgetItem(ResultTree), C.Status, C.Name, C.Detail);
                Fails += C.Status == "fail";
                Warns += C.Status == "warn";
            }
            if (Checks->empty())
                StatusLabel->setText("The test returned nothing — see the log.");
            else if (Fails)
                StatusLabel->setText(QString("%1 problem(s), %2 warning(s) — the failing rows say what to unblock.").arg(Fails).arg(Warns));
            else if (Warns)
                StatusLabel->setText(QString("No hard failures; %1 warning(s) worth a look.").arg(Warns));
            else
                StatusLabel->setText("Everything passes — the network is not your problem.");
        });
}
