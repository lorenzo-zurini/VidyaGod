#include "networkpage.h"
#include "theme.h"   // the named colours
#include "appmodel.h"
#include "asyncwork.h"
#include "ipfswrapper.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QCheckBox>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QHeaderView>
#include <QLabel>
#include <QTimer>

#include <memory>

#include <nlohmann/json.hpp>

//One ✓/⚠/✗/– row per service. Shared by the live-health and sweep sections so the two halves read alike.
static void FillStatusRow(QTreeWidgetItem *It, const std::string &Status, const std::string &Name, const std::string &Detail)
{
    const bool Down = Status == "down" || Status == "fail";
    const bool Warn = Status == "warn";
    const bool Off  = Status == "off";
    It->setText(0, Down ? "✗" : Warn ? "⚠" : Off ? "–" : "✓");
    It->setForeground(0, Down ? QBrush(QColor("#cc4444"))
                     : Warn ? QBrush(QColor("#ccaa00"))
                     : Off  ? QBrush(QColor(Theme::Dim)) : QBrush(QColor("#44aa44")));
    It->setText(1, QString::fromStdString(Name));
    It->setText(2, QString::fromStdString(Detail));
    It->setToolTip(2, QString::fromStdString(Detail));
}

NetworkPage::NetworkPage(AppModel &model, QWidget *parent)
    : QWidget(parent), Model(model)
{
    QVBoxLayout *Layout = new QVBoxLayout(this);

    // --- The switch: OFF by default. Off = the embedded node is fully down (no swarm, no DHT, no seeding or
    // downloading); installed games still launch (the mount reads their files by path). AppModel starts/stops the
    // node, and the window's one offline banner follows it.
    QCheckBox *NetCb = new QCheckBox("Enable networking (downloads, seeding and peers)", this);
    NetCb->setChecked(Model.networkingEnabled());
    connect(NetCb, &QCheckBox::toggled, this, [this](bool On){ Model.setNetworkingEnabled(On); });
    //Turned on or off elsewhere (the banner, a command): the box follows, without toggling it back.
    connect(&Model, &AppModel::networkingChanged, NetCb, [NetCb](bool On){ const QSignalBlocker B(NetCb); NetCb->setChecked(On); });
    Layout->addWidget(NetCb);
    QLabel *NetHint = new QLabel("Off by default. While off, VidyaGod makes no network connections: nothing downloads "
                                 "or seeds, and peers cannot reach you. Installed games still play.", this);
    NetHint->setWordWrap(true);
    NetHint->setStyleSheet("color:" + Theme::Dim + ";font-size:9pt;margin-left:22px;");
    Layout->addWidget(NetHint);

    // Max simultaneous downloads — Settings.MaxConcurrentDownloads: how many files fetch at once across every
    // package (a game's content and its runner's build draw from this one pool).
    QFormLayout *Form = new QFormLayout();
    Form->setContentsMargins(0, 4, 0, 8);
    Form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);   // a number field as wide as a number
    Layout->addLayout(Form);
    QSpinBox *MaxDl = new QSpinBox(this);
    MaxDl->setAccessibleName("Max simultaneous downloads");
    MaxDl->setRange(1, 32);
    MaxDl->setValue(IpfsWrapper::MaxConcurrentDownloads());
    if (const auto &S = (*Model.config())["Settings"]; S.contains("MaxConcurrentDownloads") && S["MaxConcurrentDownloads"].is_number_integer())
        MaxDl->setValue(int(S["MaxConcurrentDownloads"]));
    MaxDl->setToolTip("Files download in parallel up to this limit; a stalled one keeps retrying without blocking the others.");
    connect(MaxDl, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int V){
        IpfsWrapper::SetMaxConcurrentDownloads(V);
        (*Model.config())["Settings"]["MaxConcurrentDownloads"] = V;
        Model.save();
    });
    Form->addRow("Max simultaneous downloads:", MaxDl);

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
        "discovery, content lookups, and the live links to your peers.\n"
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
    //Off by choice is not a fault: one neutral row, no red "DOWN".
    if (!Model.networkingEnabled())
    {
        while (HealthTree->topLevelItemCount() > 1) delete HealthTree->takeTopLevelItem(HealthTree->topLevelItemCount() - 1);
        QTreeWidgetItem *It = HealthTree->topLevelItemCount() ? HealthTree->topLevelItem(0) : new QTreeWidgetItem(HealthTree);
        FillStatusRow(It, "off", "Node", "Off — networking is turned off");
        HealthStatus->setText("Service health — off");
        return;
    }
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
    auto Why     = std::make_shared<std::vector<std::string>>();   // what the test logged as errors, if it found nothing
    AsyncWork::Run(this,
        [Checks, Offline, Why]{
            LogRecorder Said({});
            *Checks = IpfsWrapper::NetworkTest(Offline.get());
            for (const auto &E : Said.Take()) if (E.Level == LogLevel::ERR && Why->size() < 6) Why->push_back(E.Message);
        },
        [this, Checks, Offline, Why]{
            RunButton->setEnabled(true);
            if (*Offline)
            {
                StatusLabel->setText(Model.networkingEnabled() ? "The node is offline (still starting?) — nothing to probe yet. Run it again in a moment."
                                                               : "The node is offline: networking is turned off. Turn it on above, then run the test.");
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
            {
                QString W;
                for (const auto &L : *Why) W += "\n" + QString::fromStdString(L);
                StatusLabel->setText("The test returned nothing" + (W.isEmpty() ? QString(".") : ":" + W));
            }
            else if (Fails)
                StatusLabel->setText(QString("%1 problem(s), %2 warning(s) — the failing rows say what to unblock.").arg(Fails).arg(Warns));
            else if (Warns)
                StatusLabel->setText(QString("No hard failures; %1 warning(s) worth a look.").arg(Warns));
            else
                StatusLabel->setText("Everything passes — the network is not your problem.");
        });
}
