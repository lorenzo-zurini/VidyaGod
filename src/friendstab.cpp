#include "friendstab.h"
#include "appmodel.h"
#include "ipfswrapper.h"
#include "packagecatalog.h"   // PlayIdent/GroupNodeIds — the SHARED identity recipe
#include "prelaunchwindow.h"

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
#include <QCheckBox>
#include <QSignalBlocker>

// See friendstab.h. The tab talks to the backend only through IpfsWrapper's Friends free functions (mutations) and
// the FriendsManager signals (live updates) — every inbound friend event just triggers a full refresh(), which is
// cheap (the address book is small).

FriendsTab::FriendsTab(AppModel & model, QWidget * parent)
    : QWidget(parent), Model(model)
{
    buildUi();

    // Live updates: any friend event → repaint. Presence/state/profile all fold into one refresh.
    // Live updates: any friend event → repaint (Qt lets the slot take fewer args than the signal).
    FriendsManager * FM = FriendsManager::instance();
    connect(FM, &FriendsManager::friendRequest,  this, [this]{ refresh(); });
    connect(FM, &FriendsManager::friendAccepted, this, [this]{ refresh(); });
    connect(FM, &FriendsManager::friendDeclined, this, [this]{ refresh(); });
    connect(FM, &FriendsManager::friendPresence, this, [this]{ refresh(); });
    connect(FM, &FriendsManager::friendProfile,  this, [this]{ refresh(); });
    connect(FM, &FriendsManager::friendRemoved,  this, [this]{ refresh(); });
    connect(FM, &FriendsManager::friendPlaying,  this, [this]{ refresh(); });
    connect(FM, &FriendsManager::friendSuggestion, this, [this]{ refresh(); });

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

    // Visibility. "Open to join" is deliberately MANUAL and advisory — it is shown to friends as a badge and never
    // gates their Join button, because a flag the user has to remember to set would otherwise hide a live game.
    InvisibleBox = new QCheckBox("Appear invisible (hide what I'm playing)", IdBox);
    connect(InvisibleBox, &QCheckBox::toggled, this, [](bool On){ IpfsWrapper::SetInvisible(On); });
    IdLayout->addWidget(InvisibleBox);
    OpenBox = new QCheckBox("Open to join (tells friends they're welcome)", IdBox);
    connect(OpenBox, &QCheckBox::toggled, this, [](bool On){ IpfsWrapper::SetOpenToJoin(On); });
    IdLayout->addWidget(OpenBox);

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
    Table->setColumnCount(6);
    Table->setHorizontalHeaderLabels({"Nickname", "Code", "Status", "Presence", "Playing", ""});
    Table->horizontalHeader()->setStretchLastSection(false);
    Table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int Col = 1; Col <= 3; ++Col)
        Table->horizontalHeader()->setSectionResizeMode(Col, QHeaderView::ResizeToContents);
    Table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    Table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    Table->verticalHeader()->setVisible(false);
    Table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    Table->setSelectionMode(QAbstractItemView::NoSelection);
    Root->addWidget(Table, 1);

    // --- Players you met ---
    // Strangers a mutual friend told us about because we were in the SAME game. They are not contacts and have no
    // route to us; "Add friend" sends an ordinary mutual-consent request, exactly as pasting their code would.
    MetBox = new QGroupBox("Players you met", this);
    MetLayout = new QVBoxLayout(MetBox);
    MetBox->setVisible(false);
    Root->addWidget(MetBox);
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

    // Read the toggles back without re-firing their handlers (a live presence event refreshes us at any moment).
    {
        QSignalBlocker B1(InvisibleBox), B2(OpenBox);
        InvisibleBox->setChecked(IpfsWrapper::Invisible());
        OpenBox->setChecked(IpfsWrapper::GetPlaying().Open);
    }

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

        // What they launched. The open flag is decoration only, and is shown ONLY while they are actually playing
        // so a toggle left on after quitting never advertises an empty game.
        QString Playing = C.PlayNode.empty() ? QString()
                        : QString::fromStdString(C.PlayLabel.empty() ? C.PlayNode : C.PlayLabel);
        if (!C.PlayNode.empty() && C.PlayOpen) Playing += "  · open to join";
        auto * PlayItem = new QTableWidgetItem(Playing);
        if (!C.PlayNode.empty())
            PlayItem->setToolTip(QString::fromStdString(C.PlayNode) +
                                 (C.PlayIdent.empty() ? QString() : "\n" + QString::fromStdString(C.PlayIdent)));
        Table->setItem(Row, 4, PlayItem);

        // Per-row actions.
        auto * Actions = new QWidget(Table);
        auto * AL = new QHBoxLayout(Actions);
        AL->setContentsMargins(2, 2, 2, 2);
        AL->setSpacing(4);
        // Join is offered whenever they are in a game — deliberately NOT gated on their "open to join" flag, which
        // is manual and usually forgotten; gating on it would hide most joinable games.
        if (C.State == "accepted" && !C.PlayNode.empty())
        {
            auto * Join = new QPushButton("Join", Actions);
            connect(Join, &QPushButton::clicked, this, [this, Peer]{ joinClicked(Peer); });
            AL->addWidget(Join);
        }
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
            auto * Remove = new QPushButton("Remove", Actions);
            connect(Remove, &QPushButton::clicked, this, [this, Peer]{ IpfsWrapper::FriendRemove(Peer.toStdString()); refresh(); });
            AL->addWidget(Remove);
        }
        AL->addStretch();
        Table->setCellWidget(Row, 5, Actions);
        ++Row;
    }

    refreshSuggestions();
}

void FriendsTab::refreshSuggestions()
{
    while (QLayoutItem * Item = MetLayout->takeAt(0))
    {
        if (Item->widget()) Item->widget()->deleteLater();
        delete Item;
    }
    const std::vector<IpfsWrapper::FriendSuggestion> Met = IpfsWrapper::FriendSuggestions();
    MetBox->setVisible(!Met.empty());
    for (const auto & S : Met)
    {
        const QString Peer = QString::fromStdString(S.Peer);
        auto * RowW = new QWidget(MetBox);
        auto * RL = new QHBoxLayout(RowW);
        RL->setContentsMargins(2, 2, 2, 2);
        const QString Who = S.Nick.empty() ? (Peer.left(8) + "…" + Peer.right(4)) : QString::fromStdString(S.Nick);
        auto * Text = new QLabel(Who + "  —  met in " + QString::fromStdString(S.Game), RowW);
        Text->setToolTip("Suggested by " + QString::fromStdString(S.Via) + "\n" + Peer);
        RL->addWidget(Text, 1);
        auto * Add = new QPushButton("Add friend", RowW);
        connect(Add, &QPushButton::clicked, this, [this, Peer]{
            IpfsWrapper::FriendAdd(Peer.toStdString());     // an ordinary mutual-consent request
            IpfsWrapper::DismissSuggestion(Peer.toStdString());
            refresh();
        });
        RL->addWidget(Add);
        auto * Dismiss = new QPushButton("Dismiss", RowW);
        connect(Dismiss, &QPushButton::clicked, this, [this, Peer]{
            IpfsWrapper::DismissSuggestion(Peer.toStdString());
            refreshSuggestions();
        });
        RL->addWidget(Dismiss);
        MetLayout->addWidget(RowW);
    }
}

void FriendsTab::joinClicked(const QString & Peer)
{
    IpfsWrapper::Contact Target;
    for (const auto & C : IpfsWrapper::FriendList())
        if (C.PeerID == Peer.toStdString()) Target = C;
    if (Target.PlayNode.empty()) return;

    const QString Label = QString::fromStdString(Target.PlayLabel.empty() ? Target.PlayNode : Target.PlayLabel);

    // We may simply not have their game. There is nothing to open, so warn and stop — never block, never fetch.
    const Node * N = Model.catalogIndex().Find(Target.PlayNode);
    if (!N)
    {
        QMessageBox::information(this, "Can't join yet",
            QString::fromStdString(Target.Nick.empty() ? "Your friend" : Target.Nick) +
            " is playing “" + Label + "”, which isn't in your library.\n\n"
            "Add it (node id: " + QString::fromStdString(Target.PlayNode) + ") and the Join button will work.");
        return;
    }

    // Their vIP is what the game must be pointed at. It comes from the LAN link table rather than being recomputed.
    QString Vip;
    for (const auto & P : IpfsWrapper::LanPeers())
        if (P.Peer == Target.PeerID) Vip = QString::fromStdString(P.Vip);
    if (Vip.isEmpty())
    {
        QMessageBox::information(this, "Can't join yet",
            "That friend isn't on your virtual LAN right now, so there is no address to join.\n\n"
            "Check they are online and not un-ticked in the launch window's Virtual LAN panel.");
        return;
    }

    // Version mismatch is a WARNING, never a block — different builds are often interoperable, and an ident whose
    // recipe prefix we don't recognise means UNKNOWN rather than wrong.
    QString Warning;
    const std::string Mine = PackageCatalog::PlayIdent(Model.catalogIndex(), Target.PlayNode);
    if (!Target.PlayIdent.empty() && !Mine.empty() &&
        Target.PlayIdent.rfind("v1:", 0) == 0 && Mine.rfind("v1:", 0) == 0 && Target.PlayIdent != Mine)
        Warning = "Your copy of this game differs from theirs — it may or may not connect.";

    auto * Dlg = new PreLaunchWindow(Model.config(), &Model.catalogIndex(),
                                     PackageCatalog::GroupNodeIds(Model.catalogIndex(), Target.PlayNode), nullptr);
    Dlg->setAttribute(Qt::WA_DeleteOnClose);
    Dlg->PresetJoin(Vip, Peer, Target.PlayNode, Warning);
    Dlg->show();
}
