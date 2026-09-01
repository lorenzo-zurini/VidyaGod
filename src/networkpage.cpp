#include "networkpage.h"
#include "appmodel.h"
#include "asyncwork.h"
#include "ipfswrapper.h"

#include <QVBoxLayout>
#include <QPushButton>
#include <QTreeWidget>
#include <QHeaderView>
#include <QLabel>

#include <memory>

NetworkPage::NetworkPage(AppModel &model, QWidget *parent)
    : QWidget(parent), Model(model)
{
    QVBoxLayout *Layout = new QVBoxLayout(this);

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
                auto *It = new QTreeWidgetItem(ResultTree);
                const bool Fail = C.Status == "fail", Warn = C.Status == "warn";
                Fails += Fail; Warns += Warn;
                It->setText(0, Fail ? "✗" : Warn ? "⚠" : "✓");
                It->setForeground(0, Fail ? QBrush(QColor("#cc4444"))
                                          : Warn ? QBrush(QColor("#ccaa00")) : QBrush(QColor("#44aa44")));
                It->setText(1, QString::fromStdString(C.Name));
                It->setText(2, QString::fromStdString(C.Detail));
                It->setToolTip(2, QString::fromStdString(C.Detail));
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
