#include "networktab.h"
#include "deckshell.h"
#include "theme.h"   // the named colours
#include "appmodel.h"
#include "peersharing.h"
#include "ipfswrapper.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QDialog>
#include <QSet>
#include <QListWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <vector>

// NetworkTab — see networktab.h. The lists and the table are rebuilt from the model, deferred (never under a running
// handler, never under an open menu or list) and only when what they show changed; the control a pad was on gets the
// focus back by its name after a rebuild.

namespace
{
std::function<std::vector<IpfsWrapper::NearbyPeer>()> &NearbySource()
{
    static std::function<std::vector<IpfsWrapper::NearbyPeer>()> S = [] { return IpfsWrapper::NearbyPeers(); };
    return S;
}

QString CodeTail(const QString & Peer) { return QObject::tr("code …%1").arg(Peer.right(10)); }

QString DisplayName(const std::string & Nick, const QString & Peer)
{
    return Nick.empty() ? Peer.left(12) + "…" : QString::fromStdString(Nick);
}

void Clear(QVBoxLayout * L)
{
    while (QLayoutItem * It = L->takeAt(0))
    {
        if (It->widget()) { It->widget()->hide(); It->widget()->deleteLater(); }
        delete It;
    }
}

QLabel * Dim(const QString & Text, QWidget * Parent)
{
    auto * L = new QLabel(Text, Parent);
    L->setWordWrap(true);
    L->setTextFormat(Qt::PlainText);
    L->setStyleSheet("color:" + Theme::Dim + ";");
    return L;
}
}

std::vector<IpfsWrapper::NearbyPeer> NetworkTab::Nearby() { return NearbySource()(); }

void NetworkTab::SetNearbySource(std::function<std::vector<IpfsWrapper::NearbyPeer>()> Source)
{
    NearbySource() = Source ? std::move(Source) : [] { return IpfsWrapper::NearbyPeers(); };
}

NetworkTab::NetworkTab(AppModel & model, QWidget * parent) : QWidget(parent), Model(model)
{
    buildUi();

    PeersManager * FM = PeersManager::instance();
    for (auto Sig : { &PeersManager::peerRequestSent, &PeersManager::peerRemoved })
        connect(FM, Sig, this, [this] { scheduleRefresh(); });
    connect(FM, &PeersManager::peerRequest,  this, [this] { scheduleRefresh(); });
    connect(FM, &PeersManager::peerAccepted, this, [this] { scheduleRefresh(); });
    connect(FM, &PeersManager::peerDeclined, this, [this] { scheduleRefresh(); });
    connect(FM, &PeersManager::peerPresence, this, [this] { scheduleRefresh(); });
    connect(FM, &PeersManager::peerProfile,  this, [this] { scheduleRefresh(); });
    connect(&Model.peers(), &PeerSharing::peerCatalogChanged, this, [this] { scheduleRefresh(); });
    connect(&Model.peers(), &PeerSharing::shareStateChanged, this, [this] { scheduleRefresh(); });
    connect(&Model, &AppModel::networkingChanged, this, [this] { scheduleRefresh(); });
    connect(&Model, &AppModel::nodeReady,         this, [this] { scheduleRefresh(); });
    connect(&Model, &AppModel::catalogChanged,    this, [this] { scheduleRefresh(); });   // a library, a peer's games
    connect(&Model, &AppModel::libraryPublished, this, [this](const QString &, const QString & top, const QString & note) {
        PublishStatus->setText(tr("Published %1.").arg(top) + (note.isEmpty() ? QString() : "\n" + tr("Not published whole: ") + note));
        scheduleRefresh();
    });
    connect(&Model, &AppModel::libraryPublishFailed, this, [this](const QString & m) {
        PublishStatus->setText(tr("Publishing failed: ") + m);
        scheduleRefresh();
    });

    refresh();
}

void NetworkTab::buildUi()
{
    auto * Outer = new QVBoxLayout(this);
    Outer->setContentsMargins(0, 0, 0, 0);
    auto * Scroll = new QScrollArea(this);
    Scroll->setWidgetResizable(true);
    Scroll->setFrameShape(QFrame::NoFrame);
    Outer->addWidget(Scroll);
    auto * Page = new QWidget(Scroll);
    Scroll->setWidget(Page);
    auto * Root = new QVBoxLayout(Page);
    Root->setSpacing(12);

    // --- What you share, and the one press it may need ---
    {
        auto * Row = new QHBoxLayout();
        ShareState = new QLabel(Page);
        ShareState->setObjectName("shareState");
        ShareState->setWordWrap(true);
        ShareState->setTextFormat(Qt::PlainText);
        Row->addWidget(ShareState, 1);
        PrepareButton = new QPushButton(tr("Prepare it now"), Page);
        PrepareButton->setObjectName("prepareSharing");
        connect(PrepareButton, &QPushButton::clicked, this, [this] {
            PrepareButton->setEnabled(false);
            Model.peers().prepareSharing();
        });
        Row->addWidget(PrepareButton);
        TurnOnButton = new QPushButton(tr("Turn on networking"), Page);
        connect(TurnOnButton, &QPushButton::clicked, this, [this] { Model.setNetworkingEnabled(true); });
        Row->addWidget(TurnOnButton);
        Root->addLayout(Row);
    }
    Status = new QLabel(Page);
    Status->setObjectName("peersStatus");
    Status->setTextFormat(Qt::PlainText);   // it names peers by the names they chose
    Status->setWordWrap(true);
    Status->hide();
    Root->addWidget(Status);

    // --- Waiting on you ---
    WaitingBox = new QGroupBox(tr("Waiting on you"), Page);
    WaitingBox->setAccessibleName("Waiting on you");
    WaitingRows = new QVBoxLayout(WaitingBox);
    Root->addWidget(WaitingBox);

    // --- The peers: a list, and the chosen one's details beside it ---
    {
        auto * Head = new QHBoxLayout();
        auto * Title = new QLabel("<b>" + tr("Peers") + "</b>", Page);
        Head->addWidget(Title);
        Head->addStretch(1);
        auto * Defaults = new QPushButton(tr("Defaults…"), Page);
        Defaults->setObjectName("peerDefaults");
        Defaults->setToolTip(tr("What a new peer is given. Pairing starts from it."));
        connect(Defaults, &QPushButton::clicked, this, &NetworkTab::openDefaults);
        Head->addWidget(Defaults);
        Root->addLayout(Head);
        auto * Body = new QHBoxLayout();
        List = new QListWidget(Page);
        List->setObjectName("peers");
        List->setAccessibleName("Peers");
        List->setMinimumWidth(240);
        List->setMaximumWidth(380);
        List->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
        connect(List, &QListWidget::currentRowChanged, this, [this] { rebuildDetails(); });
        Body->addWidget(List, 0, Qt::AlignTop);
        Details = new QWidget(Page);
        Details->setObjectName("peerDetails");
        DetailsLayout = new QVBoxLayout(Details);
        DetailsLayout->setContentsMargins(12, 0, 0, 0);
        Body->addWidget(Details, 1, Qt::AlignTop);
        Root->addLayout(Body);
    }
    EmptyHint = Dim(tr("Nobody yet. Pair with a device on your network below, or send someone your code."), Page);
    EmptyHint->setObjectName("emptyHint");
    Root->addWidget(EmptyHint);

    // --- Add someone ---
    AddBox = new QGroupBox(tr("Add someone"), Page);
    AddBox->setAccessibleName("Add someone");
    auto * AddLayout = new QVBoxLayout(AddBox);
    AddLayout->addWidget(Dim(tr("On your network:"), AddBox));
    NearbyRows = new QVBoxLayout();
    AddLayout->addLayout(NearbyRows);
    NearbyHint = Dim(tr("No other VidyaGod on this network. Start VidyaGod on another device here, and it shows up to pair "
                        "with in one press."), AddBox);
    AddLayout->addWidget(NearbyHint);
    {
        AddLayout->addWidget(new QLabel(tr("Your code, for someone not on your network:"), AddBox));
        auto * CodeRow = new QHBoxLayout();
        CodeValue = new QLabel(AddBox);
        CodeValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
        CodeValue->setStyleSheet("font-family: monospace; font-weight: 600;");
        CodeValue->setAccessibleName("Peer code");
        CodeValue->setWordWrap(true);
        CodeValue->setMinimumWidth(0);
        CodeRow->addWidget(CodeValue, 1);
        CopyButton = new QPushButton(tr("Copy"), AddBox);
        connect(CopyButton, &QPushButton::clicked, this, [] {
            const std::string Code = IpfsWrapper::PeerCode();
            if (!Code.empty()) QApplication::clipboard()->setText(QString::fromStdString(Code));
        });
        CodeRow->addWidget(CopyButton);
        AddLayout->addLayout(CodeRow);
        auto * AddRow = new QHBoxLayout();
        AddRow->addWidget(new QLabel(tr("Their code:"), AddBox));
        AddEdit = new QLineEdit(AddBox);
        AddEdit->setObjectName("peerCode");
        AddEdit->setPlaceholderText(tr("Paste their code"));
        connect(AddEdit, &QLineEdit::returnPressed, this, &NetworkTab::addPeerClicked);
        AddRow->addWidget(AddEdit, 1);
        AddLayout->addLayout(AddRow);
        AddLayout->addWidget(permissionTicks(AddBox, QStringLiteral("code")));
        auto * Send = new QPushButton(tr("Send request"), AddBox);
        Send->setObjectName("sendRequest");
        SendButton = Send;
        connect(Send, &QPushButton::clicked, this, &NetworkTab::addPeerClicked);
        auto * SendRow = new QHBoxLayout();
        SendRow->addWidget(Send);
        SendRow->addStretch();
        AddLayout->addLayout(SendRow);
    }
    Root->addWidget(AddBox);

    // --- Advanced, folded ---
    AdvancedToggle = new QToolButton(Page);
    AdvancedToggle->setObjectName("advancedToggle");
    AdvancedToggle->setText(tr("Advanced"));
    AdvancedToggle->setCheckable(true);
    AdvancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    AdvancedToggle->setArrowType(Qt::RightArrow);
    Root->addWidget(AdvancedToggle);
    Advanced = new QWidget(Page);
    Advanced->setObjectName("advanced");
    Advanced->hide();
    connect(AdvancedToggle, &QToolButton::toggled, this, [this](bool On) {
        Advanced->setVisible(On);
        AdvancedToggle->setArrowType(On ? Qt::DownArrow : Qt::RightArrow);
    });
    auto * Adv = new QVBoxLayout(Advanced);
    {
        auto * NickRow = new QHBoxLayout();
        NickRow->addWidget(new QLabel(tr("Your name, as peers see it:"), Advanced));
        NickEdit = new QLineEdit(Advanced);
        NickEdit->setPlaceholderText(tr("Defaults to this machine's name"));
        connect(NickEdit, &QLineEdit::textEdited, this, [this] { NickEdited = true; });
        connect(NickEdit, &QLineEdit::returnPressed, this, &NetworkTab::setNickClicked);
        NickRow->addWidget(NickEdit, 1);
        auto * Save = new QPushButton(tr("Save"), Advanced);
        connect(Save, &QPushButton::clicked, this, &NetworkTab::setNickClicked);
        NickRow->addWidget(Save);
        Adv->addLayout(NickRow);
    }
    AutoAccept = new QCheckBox(tr("Accept every request by itself, with the \"New peers\" row (server mode)"), Advanced);
    connect(AutoAccept, &QCheckBox::toggled, this, [this](bool On) { Model.peers().setAutoAcceptEnabled(On); });
    Adv->addWidget(AutoAccept);
    {
        auto * PubRow = new QHBoxLayout();
        PublishButton = new QPushButton(tr("Publish everything again"), Advanced);
        PublishButton->setObjectName("publishAll");
        PublishButton->setToolTip(tr("Freeze every package into its blocks, name its node files, and record what peers "
                                     "are given. What you share is kept up to date by itself; this redoes all of it."));
        connect(PublishButton, &QPushButton::clicked, this, &NetworkTab::publishClicked);
        PubRow->addWidget(PublishButton);
        auto * Backup = new QPushButton(tr("Back up my identity…"), Advanced);
        connect(Backup, &QPushButton::clicked, this, &NetworkTab::backupIdentityClicked);
        PubRow->addWidget(Backup);
        auto * Restore = new QPushButton(tr("Restore an identity…"), Advanced);
        connect(Restore, &QPushButton::clicked, this, &NetworkTab::restoreIdentityClicked);
        PubRow->addWidget(Restore);
        PubRow->addStretch();
        Adv->addLayout(PubRow);
    }
    PublishStatus = new QLabel(Advanced);
    PublishStatus->setWordWrap(true);
    PublishStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    PublishStatus->setStyleSheet("color:" + Theme::Dim + ";");
    Adv->addWidget(PublishStatus);
    Root->addWidget(Advanced);
    Root->addStretch(1);

    NearbyTimer = new QTimer(this);
    NearbyTimer->setInterval(2000);
    connect(NearbyTimer, &QTimer::timeout, this, &NetworkTab::rebuildRows);
}

void NetworkTab::setActive(bool on)
{
    Showing = on;
    if (on) { Stale = false; refresh(); NearbyTimer->start(); }
    else NearbyTimer->stop();
}

//Hidden, a change waits for the page to show: every catalog change, peer's presence and share state rebuilt the rows
//and the table — a planned fetch and a stat for each of a receiving peer's files — on a page nobody was looking at.
void NetworkTab::scheduleRefresh()
{
    if (!Showing) { Stale = true; return; }
    if (RefreshQueued) return;
    RefreshQueued = true;
    QTimer::singleShot(0, this, [this] { RefreshQueued = false; refresh(); });
}

void NetworkTab::refresh()
{
    if (QApplication::activePopupWidget())   // a menu is open: never rebuild under it
    {
        if (!RefreshQueued) { RefreshQueued = true; QTimer::singleShot(150, this, [this] { RefreshQueued = false; refresh(); }); }
        return;
    }
    ++Refreshes;
    const std::string Code = IpfsWrapper::PeerCode();
    const bool Online = IpfsWrapper::Available() && !Code.empty();
    QString Breakable = QString::fromStdString(Code);   // breakable every 13 characters (it has no spaces); Copy copies it whole
    for (int I = Breakable.size() - 13; I > 0; I -= 13) Breakable.insert(I, QChar(0x200B));
    CodeValue->setText(Online ? Breakable : tr("— (networking is off)"));
    CopyButton->setEnabled(Online);
    PublishButton->setEnabled(Online && !Model.publishing());
    if (!NickEdited) NickEdit->setText(QString::fromStdString(IpfsWrapper::GetProfile().Nick));
    { QSignalBlocker A(AutoAccept); AutoAccept->setChecked(Model.peers().autoAcceptEnabled()); }

    using S = PeerSharing::ShareState;
    const S St = Model.peers().shareState();
    QString Line = Model.peers().shareStateText();
    if (!Model.networkingEnabled()) Line = tr("Networking is off: nobody can reach you, and you reach nobody.");
    else if (!Online) Line = tr("Networking is starting…");
    else if (Line.isEmpty()) Line = tr("You share nothing yet.");
    ShareState->setText(Line);
    PrepareButton->setVisible(Online && St == S::Unpublished);
    PrepareButton->setEnabled(!Model.publishing());
    TurnOnButton->setVisible(!Model.networkingEnabled() && DeckShell::On());   // the desktop window's banner has its own
    //Nobody can be asked while networking is off: the button says why instead of failing when pressed.
    SendButton->setEnabled(Online);
    SendButton->setToolTip(Online ? QString() : tr("Networking is off — turn it on to send a request."));
    rebuildRows();
}

PeerSharing::Permissions & NetworkTab::ticks(const QString & key)
{
    auto It = Ticks.find(key);
    if (It == Ticks.end()) It = Ticks.emplace(key, Model.peers().defaultPermissions()).first;
    return It->second;
}

QWidget * NetworkTab::permissionTicks(QWidget * row, const QString & key)
{
    auto * W = new QWidget(row);
    auto * L = new QHBoxLayout(W);
    L->setContentsMargins(0, 0, 0, 0);
    const PeerSharing::Permissions T = ticks(key);
    auto Tick = [&](const QString & Name, const QString & Text, bool On, std::function<void(bool)> Set) {
        auto * B = new QCheckBox(Text, W);
        B->setObjectName(Name + ":" + key);
        B->setChecked(On);
        connect(B, &QCheckBox::toggled, this, std::move(Set));
        L->addWidget(B);
    };
    for (const QString & Lib : Model.peers().myLibraryNames())
        Tick("share:" + Lib, tr("Share: %1").arg(Lib), T.Libraries.contains(Lib), [this, key, Lib](bool On) {
            QStringList & Libs = ticks(key).Libraries;
            Libs.removeAll(Lib);
            if (On) Libs << Lib;
        });
    Tick("receive", tr("Receive"), T.Receive, [this, key](bool On) { ticks(key).Receive = On; });
    Tick("vlan", tr("vLAN"), T.Vlan, [this, key](bool On) { ticks(key).Vlan = On; });
    Tick("presence", tr("Presence"), T.Presence, [this, key](bool On) { ticks(key).Presence = On; });
    L->addStretch();
    return W;
}

void NetworkTab::rebuildRows()
{
    if (QApplication::activePopupWidget()) return;   // a menu is open: never delete it under itself (the next poll comes)
    struct Req { QString Peer, Name; bool Nearby; };
    std::vector<Req> Waiting;
    std::set<QString> Known;
    const std::vector<IpfsWrapper::NearbyPeer> Near = NearbySource()();
    std::set<QString> NearSet;
    for (const auto & N : Near) NearSet.insert(QString::fromStdString(N.Peer));
    for (const auto & C : IpfsWrapper::PeerList())
    {
        const QString P = QString::fromStdString(C.PeerID);
        Known.insert(P);
        if (C.State == "incoming") Waiting.push_back({ P, DisplayName(C.Nick, P), NearSet.count(P) > 0 });
    }
    struct NearRow { QString Peer, Name; };
    std::vector<NearRow> Nearby;
    for (const auto & N : Near)
    {
        const QString P = QString::fromStdString(N.Peer);
        if (N.State.empty() && !Known.count(P)) Nearby.push_back({ P, DisplayName(N.Nick, P) });
    }

    //What the rows show (the ticks follow the libraries): anything else is a rebuild that would take a pad's place.
    QString Now = Model.peers().myLibraryNames().join(',') + '\n';
    for (const auto & R : Waiting) Now += "w|" + R.Peer + '|' + R.Name + '|' + (R.Nearby ? "n" : "f") + '\n';
    for (const auto & N : Nearby) Now += "n|" + N.Peer + '|' + N.Name + '\n';
    if (Now != Shown)
    {
        Shown = Now;
        const QWidget * Focus = QApplication::focusWidget();
        const QString FocusName = Focus && isAncestorOf(Focus) ? Focus->objectName() : QString();

        Clear(WaitingRows);
        for (const auto & R : Waiting)
        {
            auto * W = new QWidget(WaitingBox);
            W->setObjectName("waiting:" + R.Peer);
            auto * V = new QVBoxLayout(W);
            V->setContentsMargins(0, 4, 0, 4);
            auto * Name = new QLabel("<b>" + R.Name.toHtmlEscaped() + "</b>  <span style='color:" + Theme::Dim + "'>"
                                     + (CodeTail(R.Peer) + " · " + tr("wants to pair with you")).toHtmlEscaped() + "</span>", W);
            Name->setWordWrap(true);
            V->addWidget(Name);
            if (!R.Nearby)   // its name is anyone's to choose: say so where the choice is made
                V->addWidget(Dim(tr("Not on your network: check the code ends the same on their screen before you give it anything."), W));
            V->addWidget(permissionTicks(W, R.Peer));
            auto * Buttons = new QHBoxLayout();
            auto * Accept = new QPushButton(tr("Accept"), W);
            Accept->setObjectName("accept:" + R.Peer);
            connect(Accept, &QPushButton::clicked, this, [this, P = R.Peer, N = R.Name] { acceptClicked(P, N); });
            auto * Decline = new QPushButton(tr("Decline"), W);
            Decline->setObjectName("decline:" + R.Peer);
            connect(Decline, &QPushButton::clicked, this, [this, P = R.Peer] { Model.peers().declinePeer(P); Ticks.erase(P); scheduleRefresh(); });
            Buttons->addWidget(Accept);
            Buttons->addWidget(Decline);
            Buttons->addStretch();
            V->addLayout(Buttons);
            WaitingRows->addWidget(W);
        }
        WaitingBox->setVisible(!Waiting.empty());

        Clear(NearbyRows);
        for (const auto & N : Nearby)
        {
            auto * W = new QWidget(AddBox);
            W->setObjectName("nearby:" + N.Peer);
            auto * V = new QVBoxLayout(W);
            V->setContentsMargins(0, 2, 0, 2);
            V->addWidget(new QLabel("<b>" + N.Name.toHtmlEscaped() + "</b>  <span style='color:" + Theme::Dim + "'>"
                                    + CodeTail(N.Peer).toHtmlEscaped() + "</span>", W));
            V->addWidget(permissionTicks(W, N.Peer));
            auto * Pair = new QPushButton(tr("Pair"), W);
            Pair->setObjectName("pair:" + N.Peer);
            connect(Pair, &QPushButton::clicked, this, [this, P = N.Peer, Nm = N.Name] { pairClicked(P, Nm); });
            auto * Buttons = new QHBoxLayout();
            Buttons->addWidget(Pair);
            Buttons->addStretch();
            V->addLayout(Buttons);
            NearbyRows->addWidget(W);
        }
        NearbyHint->setVisible(Nearby.empty());

        if (!FocusName.isEmpty())
            for (QWidget * C : findChildren<QWidget *>(FocusName))
                if (C->isVisibleTo(this) && C != Focus) { C->setFocus(); break; }
    }
    rebuildPeers();
}

namespace
{
//One permission: its switch, named for the pad and the tests, and what it does in one sentence under it.
QCheckBox * Permission(QVBoxLayout * L, QWidget * Parent, const QString & Name, const QString & Label, const QString & Says,
                       bool On, bool Enabled)
{
    auto * B = new QCheckBox(Label, Parent);
    B->setObjectName(Name);
    B->setChecked(On);
    B->setEnabled(Enabled);
    B->setStyleSheet("QCheckBox { font-weight: 600; }");
    L->addWidget(B);
    auto * T = Dim(Says, Parent);
    T->setContentsMargins(24, 0, 0, 6);
    L->addWidget(T);
    return B;
}

QString ShareSays(const QString & Lib) { return QObject::tr("They see your library “%1” in their Catalog.").arg(Lib); }
const char * ReceiveSays = "Their shared games appear in your Catalog.";
const char * VlanSays = "Games you launch share a private LAN with them: LAN multiplayer across the internet.";
const char * PresenceSays = "They see whether you are online and what you play.";
}

QString NetworkTab::chosenPeer() const
{
    const QListWidgetItem * It = List->currentItem();
    return It ? It->data(Qt::UserRole).toString() : QString();
}

void NetworkTab::rebuildPeers()
{
    if (QApplication::activePopupWidget()) return;
    PeerSharing & F = Model.peers();
    std::vector<IpfsWrapper::Contact> Contacts;
    for (const auto & C : IpfsWrapper::PeerList()) if (C.State != "incoming") Contacts.push_back(C);   // requests: above

    //What the list shows: rebuilt only when that changes (a pad keeps its row).
    QString Now;
    std::vector<std::pair<QString, QString>> Rows;   // peer, its line
    for (const auto & C : Contacts)
    {
        const QString P = QString::fromStdString(C.PeerID);
        QString Line = DisplayName(C.Nick, P);
        if (C.State == "accepted") Line += C.Online ? "  ●" : "  ○";
        if (C.State == "pending") Line += "  · " + tr("asked");
        else if (C.State == "blocked") Line += "  · " + tr("blocked");
        else if (F.isReceivingFrom(P))
        {
            const PeerSharing::Received R = F.receivedFrom(P);
            Line += "  · " + (R.Packages == 1 ? tr("1 game") : tr("%1 games").arg(R.Packages));
        }
        Rows.push_back({ P, Line });
        Now += P + '|' + Line + '\n';
    }
    EmptyHint->setVisible(Contacts.empty());
    List->setVisible(!Contacts.empty());
    Details->setVisible(!Contacts.empty());
    if (Now != TableShown)
    {
        TableShown = Now;
        const QString Was = chosenPeer();
        const bool Focused = List->hasFocus();
        QSignalBlocker B(List);
        List->clear();
        int Pick = 0;
        for (const auto & [P, Line] : Rows)
        {
            auto * It = new QListWidgetItem(Line, List);
            It->setData(Qt::UserRole, P);
            It->setToolTip(CodeTail(P));
            if (P == Was) Pick = List->count() - 1;
        }
        if (List->count() > 0) List->setCurrentRow(Pick);
        List->setFixedHeight(std::max(60, List->sizeHintForRow(0) * std::min(List->count(), 10) + 2 * List->frameWidth() + 4));
        if (Focused) List->setFocus();
    }
    rebuildDetails();
}

void NetworkTab::rebuildDetails()
{
    if (QApplication::activePopupWidget()) return;
    PeerSharing & F = Model.peers();
    const QString Peer = chosenPeer();
    IpfsWrapper::Contact C;
    bool Found = false;
    for (const auto & X : IpfsWrapper::PeerList())
        if (QString::fromStdString(X.PeerID) == Peer) { C = X; Found = true; break; }
    const QStringList Libs = F.myLibraryNames();
    QString Now = Peer + '|' + Libs.join(',');
    if (Found)
    {
        Now += '|' + QString::fromStdString(C.Nick) + '|' + QString::fromStdString(C.State) + (C.Online ? "|on|" : "|off|");
        for (const QString & L : Libs) Now += F.isSharingWithPeer(Peer, L) ? '1' : '0';
        const PeerSharing::Received R = F.receivedFrom(Peer);
        Now += QString("|%1%2%3|%4/%5").arg(F.isReceivingFrom(Peer) ? "r" : "-", F.isInVlan(Peer) ? "v" : "-",
                                            F.isPresenceSharedWith(Peer) ? "p" : "-").arg(R.Here).arg(R.Packages);
    }
    if (Now == DetailsShown) return;
    DetailsShown = Now;
    const QWidget * Focus = QApplication::focusWidget();
    const QString FocusName = Focus && Details->isAncestorOf(Focus) ? Focus->objectName() : QString();
    Clear(DetailsLayout);
    if (!Found) return;

    const bool Accepted = C.State == "accepted", Pending = C.State == "pending";
    auto * Head = new QLabel("<b style='font-size:15px'>" + DisplayName(C.Nick, Peer).toHtmlEscaped() + "</b>  <span style='color:"
                             + Theme::Dim + "'>" + CodeTail(Peer).toHtmlEscaped() + "</span>", Details);
    DetailsLayout->addWidget(Head);
    const QString Where = Accepted ? (C.Online ? tr("Paired · online now") : tr("Paired · offline"))
                        : Pending  ? tr("Asked: waiting for them to accept on their side")
                        : C.State == "blocked" ? tr("Blocked: they cannot reach you") : QString::fromStdString(C.State);
    auto * State = Dim(Where, Details);
    State->setObjectName("peerState");
    DetailsLayout->addWidget(State);
    DetailsLayout->addSpacing(6);
    const bool Can = Accepted || Pending;
    for (const QString & Lib : Libs)
        connect(Permission(DetailsLayout, Details, "share:" + Lib + ":" + Peer, tr("Share “%1”").arg(Lib), ShareSays(Lib),
                           F.isSharingWithPeer(Peer, Lib), Can),
                &QCheckBox::toggled, this, [this, Peer, Lib](bool On) {
                    if (On) Model.peers().shareLibraryWithPeer(Peer, Lib); else Model.peers().stopSharingWithPeer(Peer, Lib);
                    scheduleRefresh();
                });
    const PeerSharing::Received R = F.receivedFrom(Peer);
    QString Recv = tr(ReceiveSays);
    if (F.isReceivingFrom(Peer)) Recv += " " + (R.Packages == 0 ? tr("They share none yet.") : tr("%1 of the %2 they share have landed here.").arg(R.Here).arg(R.Packages));
    connect(Permission(DetailsLayout, Details, "receive:" + Peer, tr("Receive their games"), Recv, F.isReceivingFrom(Peer), Can),
            &QCheckBox::toggled, this, [this, Peer](bool On) { Model.peers().setReceivingFrom(Peer, On); scheduleRefresh(); });
    connect(Permission(DetailsLayout, Details, "vlan:" + Peer, tr("Virtual LAN"), tr(VlanSays), F.isInVlan(Peer), Can),
            &QCheckBox::toggled, this, [this, Peer](bool On) { Model.peers().setInVlan(Peer, On); scheduleRefresh(); });
    connect(Permission(DetailsLayout, Details, "presence:" + Peer, tr("Presence"), tr(PresenceSays), F.isPresenceSharedWith(Peer), Can),
            &QCheckBox::toggled, this, [this, Peer](bool On) { Model.peers().setPresenceSharedWith(Peer, On); scheduleRefresh(); });

    // More ▾: Block… / Remove… — each asked first: each ends what you share with them, in one click.
    auto * More = new QPushButton(tr("More"), Details);
    More->setObjectName("menu:" + Peer);
    auto * M = new QMenu(More);
    connect(M->addAction(tr("Block…")), &QAction::triggered, this, [this, Peer] {
        if (QMessageBox::question(this, tr("Block peer"), tr("Block this peer? They can no longer reach you or ask again, "
                "you stop sharing with each other, and their games leave your Catalog."),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
        Model.peers().blockPeer(Peer); scheduleRefresh(); });
    connect(M->addAction(tr("Remove…")), &QAction::triggered, this, [this, Peer] {
        if (QMessageBox::question(this, tr("Remove peer"), tr("Remove this peer? You stop sharing with each other and their "
                "games leave your Catalog. Games you installed from them stay."),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
        Model.peers().removePeer(Peer); scheduleRefresh(); });
    More->setMenu(M);
    auto * Row = new QHBoxLayout();
    Row->addWidget(More);
    Row->addStretch(1);
    DetailsLayout->addLayout(Row);
    if (!FocusName.isEmpty())
        if (QWidget * W = Details->findChild<QWidget *>(FocusName)) W->setFocus();
}

void NetworkTab::openDefaults()
{
    auto * D = new QDialog(this);
    D->setObjectName("peerDefaultsDialog");
    D->setWindowTitle(tr("Defaults for new peers"));
    D->setAttribute(Qt::WA_DeleteOnClose);
    auto * L = new QVBoxLayout(D);
    L->addWidget(Dim(tr("What a new peer is given when you pair. Each pairing starts from these, and you can change them there."), D));
    L->addSpacing(6);
    PeerSharing & F = Model.peers();
    for (const QString & Lib : F.myLibraryNames())
        connect(Permission(L, D, "default:share:" + Lib, tr("Share “%1”").arg(Lib), ShareSays(Lib), F.newPeerShareDefaults().contains(Lib), true),
                &QCheckBox::toggled, this, [this, Lib](bool On) { Model.peers().setNewPeerShareDefault(Lib, On); });
    connect(Permission(L, D, "default:receive", tr("Receive their games"), tr(ReceiveSays), F.newPeerDefault("receive"), true),
            &QCheckBox::toggled, this, [this](bool On) { Model.peers().setNewPeerDefault("receive", On); });
    connect(Permission(L, D, "default:vlan", tr("Virtual LAN"), tr(VlanSays), F.newPeerDefault("vlan"), true),
            &QCheckBox::toggled, this, [this](bool On) { Model.peers().setNewPeerDefault("vlan", On); });
    connect(Permission(L, D, "default:presence", tr("Presence"), tr(PresenceSays), F.newPeerDefault("presence"), true),
            &QCheckBox::toggled, this, [this](bool On) { Model.peers().setNewPeerDefault("presence", On); });
    auto * Done = new QPushButton(tr("Done"), D);
    connect(Done, &QPushButton::clicked, D, &QDialog::accept);
    L->addWidget(Done, 0, Qt::AlignRight);
    //A pairing's ticks not touched yet follow the new defaults; ones the user changed stay as they are.
    const PeerSharing::Permissions Before = F.defaultPermissions();
    connect(D, &QDialog::finished, this, [this, Before] {
        for (auto It = Ticks.begin(); It != Ticks.end();)
        {
            const PeerSharing::Permissions & T = It->second;
            const bool Untouched = T.Receive == Before.Receive && T.Vlan == Before.Vlan && T.Presence == Before.Presence
                                   && QSet<QString>(T.Libraries.begin(), T.Libraries.end()) == QSet<QString>(Before.Libraries.begin(), Before.Libraries.end());
            It = Untouched ? Ticks.erase(It) : std::next(It);
        }
        //The code's ticks are built once (the rows are rebuilt): set them as they are now.
        if (!Ticks.count(QStringLiteral("code")))
        {
            const PeerSharing::Permissions Now = ticks(QStringLiteral("code"));
            for (QCheckBox * B : AddBox->findChildren<QCheckBox *>())
            {
                const QString N = B->objectName();
                if (!N.endsWith(":code")) continue;
                const QString K = N.chopped(5);
                B->setChecked(K == "receive" ? Now.Receive : K == "vlan" ? Now.Vlan : K == "presence" ? Now.Presence
                              : Now.Libraries.contains(K.mid(6)));
            }
        }
        Shown.clear();
        scheduleRefresh();
    });
    D->open();
}

void NetworkTab::pairClicked(const QString & peer, const QString & name)
{
    std::string Err;
    const bool Ok = Model.peers().pairWith(peer, ticks(peer), &Err);
    Status->setText(Ok ? tr("Asked %1 to pair: accept it on %1.").arg(name)
                       : tr("Could not ask %1 to pair: %2").arg(name, QString::fromStdString(Err)));
    Status->show();
    if (Ok) Ticks.erase(peer);
    scheduleRefresh();
}

void NetworkTab::acceptClicked(const QString & peer, const QString & name)
{
    const bool Ok = Model.peers().acceptPairing(peer, ticks(peer));
    Status->setText(Ok ? tr("Paired with %1.").arg(name) : tr("Could not accept %1: networking is off or still starting.").arg(name));
    Status->show();
    if (Ok) Ticks.erase(peer);
    scheduleRefresh();
}

void NetworkTab::addPeerClicked()
{
    const QString Code = AddEdit->text().trimmed();
    if (Code.isEmpty()) return;
    std::string Err;
    const bool Ok = Model.peers().pairWith(Code, ticks(QStringLiteral("code")), &Err);
    if (Ok) { AddEdit->clear(); Ticks.erase(QStringLiteral("code")); }
    Status->setText(Ok ? tr("Request sent: they accept it on their side.")
                       : tr("Could not send the request: ") + QString::fromStdString(Err.empty() ? "is networking on?" : Err));
    Status->show();
    scheduleRefresh();
}

void NetworkTab::setNickClicked()
{
    std::string Err;
    if (!Model.peers().setNickname(NickEdit->text(), &Err))
    {
        Status->setText(tr("Could not save your name: ") + QString::fromStdString(Err.empty() ? "is networking on?" : Err));
        Status->show();
    }
    NickEdited = false;
    scheduleRefresh();
}

void NetworkTab::publishClicked()
{
    PublishButton->setEnabled(false);
    PublishStatus->setText(tr("Publishing… (freezing packages into their blocks and seeding them)"));
    Model.publishLibraries();
}

void NetworkTab::backupIdentityClicked()
{
    const QString Path = QFileDialog::getSaveFileName(this, tr("Back up identity key"), "vidyagod-identity.key");
    if (Path.isEmpty()) return;
    std::string Err;
    if (IpfsWrapper::ExportIdentity(Path.toStdString(), &Err))
        QMessageBox::information(this, tr("Identity backed up"), tr("Your identity key was saved.\nKeep it private — anyone with this file becomes you."));
    else
        QMessageBox::warning(this, tr("Backup failed"), QString::fromStdString(Err));
}

void NetworkTab::restoreIdentityClicked()
{
    const QString Path = QFileDialog::getOpenFileName(this, tr("Restore identity key"));
    if (Path.isEmpty()) return;
    if (QMessageBox::warning(this, tr("Restore identity"),
            tr("This REPLACES your current identity — your code will change to the restored one. You must restart "
               "VidyaGod for it to take effect.\n\nContinue?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    std::string Err;
    if (IpfsWrapper::ImportIdentity(Path.toStdString(), &Err))
        QMessageBox::information(this, tr("Identity restored"), tr("The identity was installed. RESTART VidyaGod now for it to take effect."));
    else
        QMessageBox::warning(this, tr("Restore failed"), QString::fromStdString(Err));
}
