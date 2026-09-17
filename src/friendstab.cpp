#include "friendstab.h"
#include <QCheckBox>
#include "appmodel.h"
#include "ipfswrapper.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QHeaderView>
#include <QGroupBox>
#include <QApplication>
#include <QClipboard>
#include <QMessageBox>
#include <QMenu>
#include <QAction>
#include <QTimer>

// See friendstab.h. The tab talks to the backend only through IpfsWrapper's Friends free functions (mutations) and
// the FriendsManager signals (live updates) — every inbound friend event just triggers a full refresh(), which is
// cheap (the address book is small).
//
// Scope: identity + mutual-consent friendship + online/offline. There is deliberately NO "what are you playing",
// no Join button and no co-play suggestions — friends are simply on one always-on virtual LAN (see the launch
// window's Virtual LAN panel), not a lobby/host/join system.

FriendsTab::FriendsTab(AppModel & model, QWidget * parent)
    : QWidget(parent), Model(model)
{
    buildUi();

    // Live updates: any friend event → repaint. Presence/state/profile all fold into one refresh.
    FriendsManager * FM = FriendsManager::instance();
    connect(FM, &FriendsManager::friendRequest,  this, [this]{ refresh(); });
    connect(FM, &FriendsManager::friendAccepted, this, [this]{ refresh(); });
    connect(FM, &FriendsManager::friendDeclined, this, [this]{ refresh(); });
    connect(FM, &FriendsManager::friendPresence, this, [this]{ refresh(); });
    connect(FM, &FriendsManager::friendProfile,  this, [this]{ refresh(); });
    connect(FM, &FriendsManager::friendRemoved,  this, [this]{ refresh(); });
    connect(&Model, &AppModel::friendCatalogChanged, this, [this]{ refresh(); });   // a friend shared/withdrew games

    // Networking coming up/down changes whether we have a friend code + can act.
    connect(&Model, &AppModel::networkingChanged, this, [this]{ refresh(); });

    refresh();
}

void FriendsTab::buildUi()
{
    auto * Root = new QVBoxLayout(this);

    // --- Your identity ---
    auto * IdBox = new QGroupBox("Your identity", this);
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
    NickEdit->setPlaceholderText("How friends see you");
    connect(NickEdit, &QLineEdit::textEdited, this, [this]{ NickEdited = true; });
    connect(NickEdit, &QLineEdit::returnPressed, this, &FriendsTab::setNickClicked);
    NickRow->addWidget(NickEdit, 1);
    auto * SaveNick = new QPushButton("Save", IdBox);
    connect(SaveNick, &QPushButton::clicked, this, &FriendsTab::setNickClicked);
    NickRow->addWidget(SaveNick);
    IdLayout->addLayout(NickRow);

    Root->addWidget(IdBox);

    // --- Add a friend ---
    auto * AddRow = new QHBoxLayout();
    AddRow->addWidget(new QLabel("Add a friend:", this));
    AddEdit = new QLineEdit(this);
    AddEdit->setPlaceholderText("Paste a friend's code");
    connect(AddEdit, &QLineEdit::returnPressed, this, &FriendsTab::addFriendClicked);
    AddRow->addWidget(AddEdit, 1);
    auto * AddBtn = new QPushButton("Send request", this);
    connect(AddBtn, &QPushButton::clicked, this, &FriendsTab::addFriendClicked);
    AddRow->addWidget(AddBtn);
    Root->addLayout(AddRow);

    // --- Offline hint ---
    StatusHint = new QLabel(this);
    StatusHint->setStyleSheet("color: palette(mid);");
    StatusHint->setWordWrap(true);
    Root->addWidget(StatusHint);

    // --- Contacts ---
    Table = new QTableWidget(this);
    Table->setColumnCount(5);
    Table->setHorizontalHeaderLabels({"Nickname", "Code", "Status", "Presence", ""});
    Table->horizontalHeader()->setStretchLastSection(false);
    Table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int Col = 1; Col <= 3; ++Col)
        Table->horizontalHeader()->setSectionResizeMode(Col, QHeaderView::ResizeToContents);
    Table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    Table->verticalHeader()->setVisible(false);
    Table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    Table->setSelectionMode(QAbstractItemView::NoSelection);
    Root->addWidget(Table, 1);
}

void FriendsTab::setActive(bool on)
{
    if (on) refresh();
}

void FriendsTab::addFriendClicked()
{
    const QString Code = AddEdit->text().trimmed();
    if (Code.isEmpty()) return;
    std::string Err;
    if (IpfsWrapper::FriendAdd(Code.toStdString(), std::string(), &Err))
        AddEdit->clear();
    else
        QMessageBox::warning(this, "Add friend", QString::fromStdString(Err.empty() ? "Could not send the request." : Err));
    refresh();
}

void FriendsTab::setNickClicked()
{
    std::string Err;
    if (!IpfsWrapper::SetProfile(NickEdit->text().trimmed().toStdString(), std::string(), &Err))
        QMessageBox::warning(this, "Nickname", QString::fromStdString(Err.empty() ? "Could not set the nickname (is networking on?)." : Err));
    NickEdited = false;
}

void FriendsTab::refresh()
{
    // A friend event can fire while a Share ▾ / Install ▾ menu is open — those run a nested event loop (QMenu::exec).
    // Rebuilding the table now calls setCellWidget, which deletes the cell widget that OWNS the open menu, freeing it
    // while its exec() frame is still on the stack (use-after-free on unwind). Defer until the popup closes; coalesce
    // repeated events into one pending retry. (activePopupWidget() is the open menu; NickEdit etc. are not popups.)
    if (QApplication::activePopupWidget())
    {
        if (!RefreshQueued)
        {
            RefreshQueued = true;
            QTimer::singleShot(200, this, [this]{ RefreshQueued = false; refresh(); });
        }
        return;
    }

    const std::string Code = IpfsWrapper::FriendCode();
    const bool Online = IpfsWrapper::Available() && !Code.empty();

    CodeValue->setText(Online ? QString::fromStdString(Code) : QStringLiteral("—"));
    CopyButton->setEnabled(Online);
    StatusHint->setVisible(!Online);
    if (!Online)
        StatusHint->setText("Networking is off. Enable it in the IPFS tab to get your friend code and reach friends. "
                            "Your contacts and nickname are saved and will be here when you come back online.");

    // Show the saved nickname unless the user is mid-edit.
    if (!NickEdited)
        NickEdit->setText(QString::fromStdString(IpfsWrapper::GetProfile().Nick));

    const std::vector<IpfsWrapper::Contact> Contacts = IpfsWrapper::FriendList();
    Table->setRowCount(static_cast<int>(Contacts.size()));
    int Row = 0;
    for (const auto & C : Contacts)
    {
        const QString Peer = QString::fromStdString(C.PeerID);
        const QString Short = Peer.size() > 14 ? (Peer.left(8) + "…" + Peer.right(4)) : Peer;

        Table->setItem(Row, 0, new QTableWidgetItem(QString::fromStdString(C.Nick.empty() ? "(unknown)" : C.Nick)));
        auto * CodeItem = new QTableWidgetItem(Short);
        CodeItem->setToolTip(Peer);
        Table->setItem(Row, 1, CodeItem);

        QString State = QString::fromStdString(C.State);
        if (State == "incoming") State = "wants to add you";
        else if (State == "pending") State = "request sent";
        Table->setItem(Row, 2, new QTableWidgetItem(State));
        Table->setItem(Row, 3, new QTableWidgetItem(C.Online ? "● online" : "offline"));

        // Per-row actions.
        auto * Actions = new QWidget(Table);
        auto * AL = new QHBoxLayout(Actions);
        AL->setContentsMargins(2, 2, 2, 2);
        AL->setSpacing(4);
        if (C.State == "incoming")
        {
            auto * Accept = new QPushButton("Accept", Actions);
            connect(Accept, &QPushButton::clicked, this, [this, Peer]{ IpfsWrapper::FriendAccept(Peer.toStdString()); refresh(); });
            AL->addWidget(Accept);
            auto * Decline = new QPushButton("Decline", Actions);
            connect(Decline, &QPushButton::clicked, this, [this, Peer]{ IpfsWrapper::FriendDecline(Peer.toStdString()); refresh(); });
            AL->addWidget(Decline);
        }
        else
        {
            // Library sharing is offered ONLY for an ACCEPTED friend — never a pending/blocked contact. (The Go layer
            // also refuses to push to / serve a non-accepted peer; this keeps the UI from implying otherwise, and from
            // toggling a share onto a blocked peer.) A non-accepted row gets Remove only.
            if (C.State == "accepted")
            {
                // Share ▾ — pick which of MY libraries to share with this friend. Bilateral: they must also receive.
                auto * Share = new QPushButton("Share ▾", Actions);
                auto * SM = new QMenu(Share);
                const QStringList Libs = Model.myLibraryNames();
                if (Libs.isEmpty()) SM->addAction("Publish a library first")->setEnabled(false);
                for (const QString & L : Libs)
                {
                    QAction * A = SM->addAction(L);
                    A->setCheckable(true);
                    A->setChecked(Model.isSharingWithFriend(Peer, L));
                    connect(A, &QAction::toggled, this, [this, Peer, L](bool on){
                        if (on) Model.shareLibraryWithFriend(Peer, L); else Model.stopSharingWithFriend(Peer, L);
                    });
                }
                Share->setMenu(SM);
                AL->addWidget(Share);

                // Get games — ask this friend for the libraries they share with us (→ friendCatalogChanged → refresh).
                auto * Get = new QPushButton("Get games", Actions);
                connect(Get, &QPushButton::clicked, this, [this, Peer]{ Model.requestFriendLibraries(Peer); });
                AL->addWidget(Get);

                // Install ▾ — games this friend has shared with us (browse-then-install; each pulls the graph on demand).
                // Read via contains() throughout: a bare operator[] on a missing key would INSERT a null into the live
                // config during a refresh (a read mutating state).
                const auto & Cfg = *Model.config();
                const std::string P = Peer.toStdString();
                if (Cfg.contains("FriendLibraries") && Cfg["FriendLibraries"].is_object()
                    && Cfg["FriendLibraries"].contains(P) && Cfg["FriendLibraries"][P].is_object()
                    && !Cfg["FriendLibraries"][P].empty())
                {
                    auto * Inst = new QPushButton("Install ▾", Actions);
                    auto * IM = new QMenu(Inst);
                    for (const auto & [Lib, Cids] : Cfg["FriendLibraries"][P].items())
                    {
                        if (!Cids.is_array()) continue;
                        IM->addSection(QString::fromStdString(Lib));
                        for (const auto & C : Cids)
                            if (C.is_string())
                            {
                                const QString Cid = QString::fromStdString(C.get<std::string>());
                                connect(IM->addAction("game " + Cid.left(12) + "…"), &QAction::triggered,
                                        this, [this, Cid]{ Model.installFriendGame(Cid); });
                            }
                    }
                    Inst->setMenu(IM);
                    AL->addWidget(Inst);
                }
            }

            auto * Remove = new QPushButton("Remove", Actions);
            connect(Remove, &QPushButton::clicked, this, [this, Peer]{
                IpfsWrapper::FriendRemove(Peer.toStdString());   // Go: drops the contact + purgeShares (stops serving)
                Model.forgetFriend(Peer);                        // C++: forget our share record + what they shared with us
                refresh();
            });
            AL->addWidget(Remove);
        }
        AL->addStretch();
        Table->setCellWidget(Row, 4, Actions);
        ++Row;
    }
}
