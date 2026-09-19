#include "networktab.h"
#include "appmodel.h"
#include "ipfswrapper.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QGroupBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QMenu>
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QMessageBox>
#include <QTimer>
#include <QSignalBlocker>

#include <functional>
#include <vector>

// NetworkTab — see networktab.h. The matrix is rebuilt wholesale on every refresh(); refresh is deferred (never
// synchronous from a cell toggle, and never while a ⋮ menu's nested exec() is on the stack) to avoid deleting a
// cell widget out from under a running handler.

NetworkTab::NetworkTab(AppModel & model, QWidget * parent) : QWidget(parent), Model(model)
{
    buildUi();

    FriendsManager * FM = FriendsManager::instance();
    connect(FM, &FriendsManager::friendRequest,  this, [this]{ scheduleRefresh(); });
    connect(FM, &FriendsManager::friendAccepted, this, [this]{ scheduleRefresh(); });
    connect(FM, &FriendsManager::friendDeclined, this, [this]{ scheduleRefresh(); });
    connect(FM, &FriendsManager::friendPresence, this, [this]{ scheduleRefresh(); });
    connect(FM, &FriendsManager::friendProfile,  this, [this]{ scheduleRefresh(); });
    connect(FM, &FriendsManager::friendRemoved,  this, [this]{ scheduleRefresh(); });
    connect(&Model, &AppModel::friendCatalogChanged, this, [this]{ scheduleRefresh(); });
    connect(&Model, &AppModel::networkingChanged,    this, [this]{ scheduleRefresh(); });

    connect(&Model, &AppModel::libraryPublished, this, [this](const QString & addr, const QString & top, const QString &){
        if (PublishButton) PublishButton->setEnabled(true);
        if (PublishStatus) PublishStatus->setText("Published " + addr + " — " + top);
        scheduleRefresh();   // a publish can add libraries (e.g. VidyaGodRunners) → rebuild the matrix so the new Share column shows
    });
    connect(&Model, &AppModel::libraryPublishFailed, this, [this](const QString & m){
        if (PublishButton) PublishButton->setEnabled(true);
        if (PublishStatus) PublishStatus->setText("Publish failed: " + m);
    });

    refresh();
}

void NetworkTab::buildUi()
{
    auto * Root = new QVBoxLayout(this);

    // --- Identity + controls ---
    auto * IdBox = new QGroupBox("You", this);
    auto * IdLayout = new QVBoxLayout(IdBox);

    auto * CodeRow = new QHBoxLayout();
    CodeRow->addWidget(new QLabel("Friend code:", IdBox));
    CodeValue = new QLabel(IdBox);
    CodeValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    CodeValue->setStyleSheet("font-family: monospace;");
    CodeRow->addWidget(CodeValue, 1);
    CopyButton = new QPushButton("Copy", IdBox);
    connect(CopyButton, &QPushButton::clicked, this, []{
        const std::string Code = IpfsWrapper::FriendCode();
        if (!Code.empty()) QApplication::clipboard()->setText(QString::fromStdString(Code));
    });
    CodeRow->addWidget(CopyButton);
    IdLayout->addLayout(CodeRow);

    auto * NickRow = new QHBoxLayout();
    NickRow->addWidget(new QLabel("Nickname:", IdBox));
    NickEdit = new QLineEdit(IdBox);
    NickEdit->setPlaceholderText("How friends see you (defaults to this machine's name)");
    connect(NickEdit, &QLineEdit::textEdited,    this, [this]{ NickEdited = true; });
    connect(NickEdit, &QLineEdit::returnPressed, this, &NetworkTab::setNickClicked);
    NickRow->addWidget(NickEdit, 1);
    auto * SaveNick = new QPushButton("Save", IdBox);
    connect(SaveNick, &QPushButton::clicked, this, &NetworkTab::setNickClicked);
    NickRow->addWidget(SaveNick);
    IdLayout->addLayout(NickRow);

    auto * AddRow = new QHBoxLayout();
    AddRow->addWidget(new QLabel("Add a peer:", IdBox));
    AddEdit = new QLineEdit(IdBox);
    AddEdit->setPlaceholderText("Paste a friend's code");
    connect(AddEdit, &QLineEdit::returnPressed, this, &NetworkTab::addPeerClicked);
    AddRow->addWidget(AddEdit, 1);
    auto * AddBtn = new QPushButton("Send request", IdBox);
    connect(AddBtn, &QPushButton::clicked, this, &NetworkTab::addPeerClicked);
    AddRow->addWidget(AddBtn);
    IdLayout->addLayout(AddRow);

    auto * PubRow = new QHBoxLayout();
    PublishButton = new QPushButton("Verify && Publish", IdBox);
    connect(PublishButton, &QPushButton::clicked, this, &NetworkTab::publishClicked);
    PubRow->addWidget(PublishButton);
    AutoAccept = new QCheckBox("Auto-accept peer requests (server mode)", IdBox);
    connect(AutoAccept, &QCheckBox::toggled, this, [this](bool on){ Model.setAutoAcceptEnabled(on); });
    PubRow->addWidget(AutoAccept);
    auto * Backup = new QPushButton("Back up identity…", IdBox);
    connect(Backup, &QPushButton::clicked, this, &NetworkTab::backupIdentityClicked);
    PubRow->addWidget(Backup);
    auto * Restore = new QPushButton("Restore…", IdBox);
    connect(Restore, &QPushButton::clicked, this, &NetworkTab::restoreIdentityClicked);
    PubRow->addWidget(Restore);
    PubRow->addStretch();
    IdLayout->addLayout(PubRow);

    PublishStatus = new QLabel(IdBox);
    PublishStatus->setWordWrap(true);
    PublishStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    PublishStatus->setStyleSheet("font-family: monospace; color: palette(mid);");
    IdLayout->addWidget(PublishStatus);

    Root->addWidget(IdBox);

    StatusHint = new QLabel(this);
    StatusHint->setStyleSheet("color: palette(mid);");
    StatusHint->setWordWrap(true);
    Root->addWidget(StatusHint);

    Table = new QTableWidget(this);
    Table->verticalHeader()->setVisible(false);
    Table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    Table->setSelectionMode(QAbstractItemView::NoSelection);
    Root->addWidget(Table, 1);
}

void NetworkTab::setActive(bool on) { if (on) refresh(); }

void NetworkTab::scheduleRefresh()
{
    if (RefreshQueued) return;
    RefreshQueued = true;
    QTimer::singleShot(0, this, [this]{ RefreshQueued = false; refresh(); });
}

void NetworkTab::addPeerClicked()
{
    const QString Code = AddEdit->text().trimmed();
    if (Code.isEmpty()) return;
    std::string Err;
    if (IpfsWrapper::FriendAdd(Code.toStdString(), std::string(), &Err)) AddEdit->clear();
    else QMessageBox::warning(this, "Add peer", QString::fromStdString(Err.empty() ? "Could not send the request." : Err));
    scheduleRefresh();
}

void NetworkTab::setNickClicked()
{
    std::string Err;
    if (!IpfsWrapper::SetProfile(NickEdit->text().trimmed().toStdString(), std::string(), &Err))
        QMessageBox::warning(this, "Nickname", QString::fromStdString(Err.empty() ? "Could not set the nickname (is networking on?)." : Err));
    NickEdited = false;
    scheduleRefresh();
}

void NetworkTab::publishClicked()
{
    PublishButton->setEnabled(false);
    PublishStatus->setText("Publishing… (freezing packages into content-addressed blocks and seeding them)");
    Model.publishLibraries();
}

void NetworkTab::backupIdentityClicked()
{
    const QString Path = QFileDialog::getSaveFileName(this, "Back up identity key", "vidyagod-identity.key");
    if (Path.isEmpty()) return;
    std::string Err;
    if (IpfsWrapper::ExportIdentity(Path.toStdString(), &Err))
        QMessageBox::information(this, "Identity backed up", "Your identity key was saved.\nKeep it private — anyone with this file becomes you.");
    else
        QMessageBox::warning(this, "Backup failed", QString::fromStdString(Err));
}

void NetworkTab::restoreIdentityClicked()
{
    const QString Path = QFileDialog::getOpenFileName(this, "Restore identity key");
    if (Path.isEmpty()) return;
    if (QMessageBox::warning(this, "Restore identity",
            "This REPLACES your current identity — your friend code will change to the restored one. You must restart "
            "VidyaGod for it to take effect.\n\nContinue?",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    std::string Err;
    if (IpfsWrapper::ImportIdentity(Path.toStdString(), &Err))
        QMessageBox::information(this, "Identity restored", "The identity was installed. RESTART VidyaGod now for it to take effect.");
    else
        QMessageBox::warning(this, "Restore failed", QString::fromStdString(Err));
}

void NetworkTab::refresh()
{
    // Never rebuild while a ⋮ menu (nested exec) is open, nor synchronously — defer, coalesced.
    if (QApplication::activePopupWidget())
    {
        if (!RefreshQueued) { RefreshQueued = true; QTimer::singleShot(150, this, [this]{ RefreshQueued = false; refresh(); }); }
        return;
    }

    const std::string Code = IpfsWrapper::FriendCode();
    const bool Online = IpfsWrapper::Available() && !Code.empty();
    CodeValue->setText(Online ? QString::fromStdString(Code) : QStringLiteral("— (networking off)"));
    CopyButton->setEnabled(Online);
    PublishButton->setEnabled(Online);
    StatusHint->setVisible(!Online);
    if (!Online)
        StatusHint->setText("Networking is off. Enable it in the IPFS tab to get your friend code, publish your libraries, and reach friends.");
    if (!NickEdited)
        NickEdit->setText(QString::fromStdString(IpfsWrapper::GetProfile().Nick));
    { QSignalBlocker Block(AutoAccept); AutoAccept->setChecked(Model.autoAcceptEnabled()); }

    const QStringList Libs = Model.myLibraryNames();
    const std::vector<IpfsWrapper::Contact> Contacts = IpfsWrapper::FriendList();

    const int NLibs   = Libs.size();
    const int RecvCol = 2 + NLibs;
    const int VlanCol = RecvCol + 1;
    const int PresCol = RecvCol + 2;
    const int ActCol  = RecvCol + 3;

    QStringList Headers; Headers << "Peer" << "Accepted";
    for (const QString & L : Libs) Headers << ("Share: " + L);
    Headers << "Receive" << "vLAN" << "Presence" << "";

    Table->clear();
    Table->setColumnCount(Headers.size());
    Table->setHorizontalHeaderLabels(Headers);
    Table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int C = 1; C < Headers.size(); ++C)
        Table->horizontalHeader()->setSectionResizeMode(C, QHeaderView::ResizeToContents);
    Table->setRowCount(1 + (int)Contacts.size());

    // A centered checkbox cell. The initial state is set BEFORE connecting, so programmatic rebuilds never fire onToggle.
    auto CheckCell = [this](int Row, int Col, bool Checked, bool Enabled, std::function<void(bool)> OnToggle) {
        auto * W = new QWidget(Table);
        auto * L = new QHBoxLayout(W);
        L->setContentsMargins(0, 0, 0, 0);
        L->setAlignment(Qt::AlignCenter);
        auto * CB = new QCheckBox(W);
        CB->setChecked(Checked);
        CB->setEnabled(Enabled);
        connect(CB, &QCheckBox::toggled, this, std::move(OnToggle));
        L->addWidget(CB);
        Table->setCellWidget(Row, Col, W);
    };

    // Row 0 — "New peers" defaults (applied the instant a request is accepted).
    Table->setItem(0, 0, new QTableWidgetItem("New peers (defaults)"));
    for (int i = 0; i < NLibs; ++i)
    {
        const QString & Lib = Libs[i];
        CheckCell(0, 2 + i, Model.newPeerShareDefaults().contains(Lib), true,
                  [this, Lib](bool on){ Model.setNewPeerShareDefault(Lib, on); });
    }
    CheckCell(0, RecvCol, Model.newPeerDefault("receive"),  true, [this](bool on){ Model.setNewPeerDefault("receive",  on); });
    CheckCell(0, VlanCol, Model.newPeerDefault("vlan"),     true, [this](bool on){ Model.setNewPeerDefault("vlan",     on); });
    CheckCell(0, PresCol, Model.newPeerDefault("presence"), true, [this](bool on){ Model.setNewPeerDefault("presence", on); });

    // Contact rows.
    for (int i = 0; i < (int)Contacts.size(); ++i)
    {
        const int Row = 1 + i;
        const IpfsWrapper::Contact & C = Contacts[i];
        const QString Peer  = QString::fromStdString(C.PeerID);
        const bool Accepted = (C.State == "accepted");
        const bool Incoming = (C.State == "incoming");
        const bool Pending  = (C.State == "pending");

        QString Name = QString::fromStdString(C.Nick.empty()
            ? (C.PeerID.size() > 12 ? C.PeerID.substr(0, 12) + "…" : C.PeerID) : C.Nick);
        if (Incoming) Name += "  (wants to add you)";
        else if (Pending) Name += "  (request sent)";
        else if (C.State == "blocked") Name += "  (blocked)";
        if (Accepted) Name += C.Online ? "  ●" : "  ○";
        Table->setItem(Row, 0, new QTableWidgetItem(Name));

        // Accepted: checked=accepted/pending; a pending row is checked-but-disabled (awaiting them). Checking an
        // incoming request accepts + applies the New-peer defaults; unchecking an accepted friend removes them.
        CheckCell(Row, 1, Accepted || Pending, Accepted || Incoming, [this, Peer, Accepted](bool on){
            if (on) Model.acceptPeer(Peer);
            else if (Accepted)   // un-checking an accepted friend REMOVES them (destructive) → confirm the misclick
            {
                if (QMessageBox::question(this, "Remove friend",
                        "Remove this friend? You'll stop sharing with each other and their shared games leave your "
                        "catalog. Games you've already installed from them are kept.",
                        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
                { IpfsWrapper::FriendRemove(Peer.toStdString()); Model.forgetFriend(Peer); }
            }
            scheduleRefresh();   // (No → the rebuild restores the checkbox to its accepted state)
        });

        for (int j = 0; j < NLibs; ++j)
        {
            const QString & Lib = Libs[j];
            CheckCell(Row, 2 + j, Model.isSharingWithFriend(Peer, Lib), Accepted, [this, Peer, Lib](bool on){
                if (on) Model.shareLibraryWithFriend(Peer, Lib); else Model.stopSharingWithFriend(Peer, Lib);
            });
        }
        CheckCell(Row, RecvCol, Model.isReceivingFrom(Peer),        Accepted, [this, Peer](bool on){ Model.setReceivingFrom(Peer, on); });
        CheckCell(Row, VlanCol, Model.isInVlan(Peer),               Accepted, [this, Peer](bool on){ Model.setInVlan(Peer, on); });
        CheckCell(Row, PresCol, Model.isPresenceSharedWith(Peer),   Accepted, [this, Peer](bool on){ Model.setPresenceSharedWith(Peer, on); });

        // Per-row ⋮ menu: Decline (incoming) / Block / Remove.
        auto * Menu = new QPushButton("⋮", Table);
        auto * M = new QMenu(Menu);
        if (Incoming)
            connect(M->addAction("Decline"), &QAction::triggered, this, [this, Peer]{
                IpfsWrapper::FriendDecline(Peer.toStdString()); Model.forgetFriend(Peer); scheduleRefresh();
            });
        connect(M->addAction("Block"), &QAction::triggered, this, [this, Peer]{
            IpfsWrapper::FriendBlock(Peer.toStdString()); Model.forgetFriend(Peer); scheduleRefresh();
        });
        connect(M->addAction("Remove"), &QAction::triggered, this, [this, Peer]{
            IpfsWrapper::FriendRemove(Peer.toStdString()); Model.forgetFriend(Peer); scheduleRefresh();
        });
        Menu->setMenu(M);
        Table->setCellWidget(Row, ActCol, Menu);
    }
}
