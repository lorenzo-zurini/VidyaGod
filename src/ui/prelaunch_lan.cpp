#include "guiformat.h"   // PlainLabel
#include "installed.h"
#include "theme.h"   // the named colours
#include "shelf.h"
#include "prelaunchwindow.h"
#include "fold.h"   // a row's options and entries are its resolution's
#include "apppaths.h"
#include "packageeditor.h"
#include "mainwindow.h"
#include "covercache.h"
#include "packagecatalog.h"
#include "containerwrapper.h"   // StringVariableSubstitution / ContainerParams (CustomVar preview substitution)
#include "launchsession.h"     // ResolveTakingBack — what the game changed, read back before the values show
#include "launchresolver.h"     // ResolveChainIds / ResolveChainTail / kNativeTerminalId (runner daisy-chain UI)
#include "jsonoperations.h"
#include "ipfswrapper.h"     // LanPeers/SetLanExcluded/LanLaunchVars — the Virtual LAN panel
#include "variantpicker.h"     // NaturalLess — deterministic version-aware variant ordering
#include "instancestore.h"     // the instance picker
#include "asyncwork.h"        // the size under the cover, counted off the GUI thread

#include <set>
#include <algorithm>

#include <QBrush>
#include <QCompleter>
#include <QDir>
#include <QFile>
#include <QPixmap>
#include <QFont>
#include <QLineEdit>
#include <QRandomGenerator>
#include <QDoubleSpinBox>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QGridLayout>
#include <QTreeWidgetItemIterator>
#include <QHeaderView>
#include <QGuiApplication>
#include <QApplication>
#include <QScrollBar>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QResizeEvent>
#include <QDesktopServices>
#include <QInputDialog>
#include <QMenu>
#include <QPainter>
#include "keycapture.h"
#include <QPainterPath>
#include <QUrl>
#include <QDateTime>
#include <QLocale>
#include <QPointer>

#include "prelaunchwindow_p.h"

void PreLaunchWindow::BuildLanPanel(QVBoxLayout * LeftCol)
{
    LanGroup = new QGroupBox("Virtual LAN", this);
    QVBoxLayout * GL = new QVBoxLayout(LanGroup);
    GL->setContentsMargins(10, 6, 10, 8);
    GL->setSpacing(4);
    LanStatus = new QLabel(LanGroup);
    LanStatus->setStyleSheet("color:" + Theme::Dim + ";font-size:9pt;");
    GL->addWidget(LanStatus);
    LanRows = new QVBoxLayout();
    LanRows->setSpacing(2);
    GL->addLayout(LanRows);

    // The virtual LAN is host-less: every accepted friend is simply present on the shared 10.66/16, discovered by
    // vIP with no join-a-specific-friend targeting. The panel below just shows each friend's live link status.
    LanGroup->setVisible(false);                 // shown once the first poll finds friends / a LAN
    LeftCol->addWidget(LanGroup);

    LanTimer = new QTimer(this);
    LanTimer->setInterval(2000);
    connect(LanTimer, &QTimer::timeout, this, &PreLaunchWindow::RefreshLanPanel);
    LanTimer->start();
    RefreshLanPanel();
}

void PreLaunchWindow::RefreshLanPanel()
{
    const std::vector<IpfsWrapper::LanPeer> Peers = IpfsWrapper::LanPeers();

    // Own status line: our vIP comes from the LAN launch vars (cheap; empty when the node/LAN is down).
    const auto Vars = IpfsWrapper::LanLaunchVars();
    const auto VipIt = Vars.find("VIDYAGOD_SELF_VIP");
    if (VipIt != Vars.end())
        LanStatus->setText("Your vIP: " + QString::fromStdString(VipIt->second) + " — LAN ready");
    else
        LanStatus->setText("LAN offline (node down)");

    // The excluded set (GLOBAL roster) from config — ticks reflect it, toggles rewrite it.
    std::set<std::string> Excluded;
    {
        const auto & S = (*GlobalConfigJSON)["Settings"];
        if (S.contains("LanExcludedPeers") && S["LanExcludedPeers"].is_array())
            for (const auto & P : S["LanExcludedPeers"])
                if (P.is_string()) Excluded.insert(P.get<std::string>());
    }

    // Reconcile rows: upsert every reported friend, drop rows for friends that vanished.
    std::set<std::string> Seen;
    for (const IpfsWrapper::LanPeer & P : Peers)
    {
        Seen.insert(P.Peer);
        auto It = LanRowByPeer.find(P.Peer);
        if (It == LanRowByPeer.end())
        {
            QWidget * Row = new QWidget(LanGroup);
            Row->setObjectName(QString::fromStdString("lanrow_" + P.Peer));
            QHBoxLayout * RL = new QHBoxLayout(Row);
            RL->setContentsMargins(0, 0, 0, 0);
            RL->setSpacing(6);
            QCheckBox * Cb = new QCheckBox(PlainLabel(QString::fromStdString(P.Nick.empty() ? P.Peer.substr(0, 8) : P.Nick)), Row);
            Cb->setChecked(!Excluded.count(P.Peer));
            const std::string Peer = P.Peer;
            connect(Cb, &QCheckBox::toggled, this, [this, Peer](bool On){
                // Rewrite Settings.LanExcludedPeers (the GLOBAL roster) + push it into the node — live, mid-game too.
                auto & S = (*GlobalConfigJSON)["Settings"];
                std::vector<std::string> Ex;
                if (S.contains("LanExcludedPeers") && S["LanExcludedPeers"].is_array())
                    for (const auto & E : S["LanExcludedPeers"])
                        if (E.is_string() && E.get<std::string>() != Peer) Ex.push_back(E.get<std::string>());
                if (!On) Ex.push_back(Peer);
                nlohmann::ordered_json Arr = nlohmann::ordered_json::array();
                for (const std::string & E : Ex) Arr.push_back(E);
                S["LanExcludedPeers"] = Arr;
                persistGlobalConfig();
                IpfsWrapper::SetLanExcluded(Ex);
            });
            RL->addWidget(Cb);
            RL->addStretch();
            QLabel * Badge = new QLabel(Row);
            Badge->setStyleSheet("font-size:9pt;");
            RL->addWidget(Badge);
            LanRows->addWidget(Row);
            It = LanRowByPeer.emplace(P.Peer, std::make_pair(Cb, Badge)).first;
        }
        // Badge: link quality at a glance — green direct (+RTT), amber relayed, grey otherwise.
        QLabel * Badge = It->second.second;
        if (P.Link == "direct")
        {
            QString T = "● direct";
            if (P.RttMs >= 0) T += QString(" · %1 ms").arg(P.RttMs);
            Badge->setText(T);
            Badge->setStyleSheet("font-size:9pt;color:#6dbf6d;");
        }
        else if (P.Link == "relayed") { Badge->setText("● relayed");    Badge->setStyleSheet("font-size:9pt;color:#c9a227;"); }
        else if (P.Link == "connecting") { Badge->setText("○ connecting…"); Badge->setStyleSheet("font-size:9pt;color:" + Theme::Dim + ";"); }
        else                          { Badge->setText("○ offline");    Badge->setStyleSheet("font-size:9pt;color:" + Theme::Dim + ";"); }
    }
    for (auto It = LanRowByPeer.begin(); It != LanRowByPeer.end();)
    {
        if (!Seen.count(It->first))
        {
            if (QWidget * Row = It->second.first->parentWidget()) Row->deleteLater();
            It = LanRowByPeer.erase(It);
        }
        else ++It;
    }
    LanGroup->setVisible(!Peers.empty());
}
