#include "sharingtab.h"
#include "appmodel.h"
#include "ipfswrapper.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QGroupBox>
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QMessageBox>

// See sharingtab.h. Publish side only: (Verify &) Publish your libraries under your friend code, and back up / restore
// the identity key. Long operations (the DHT publish) run in AppModel off the GUI thread; this tab just drives them
// and shows the result.

SharingTab::SharingTab(AppModel & model, QWidget * parent)
    : QWidget(parent), Model(model)
{
    buildUi();
    connect(&Model, &AppModel::networkingChanged, this, [this]{ refresh(); });
    connect(&Model, &AppModel::libraryPublished, this, [this](const QString & addr, const QString & top, const QString & note){
        PublishButton->setEnabled(true);
        QString msg = "Published " + addr + "\n→ " + top;
        if (!note.isEmpty()) msg += "\n⚠ " + note;
        PublishStatus->setText(msg);
    });
    connect(&Model, &AppModel::libraryPublishFailed, this, [this](const QString & m){
        PublishButton->setEnabled(true);
        PublishStatus->setText("Publish failed: " + m);
    });
    connect(&Model, &AppModel::gameAdded, this, [this](const QString & dir){
        if (AddButton) AddButton->setEnabled(true);
        if (AddCidEdit) AddCidEdit->clear();
        if (AddStatus) AddStatus->setText("Added → " + dir);
    });
    connect(&Model, &AppModel::gameAddFailed, this, [this](const QString & m){
        if (AddButton) AddButton->setEnabled(true);
        if (AddStatus) AddStatus->setText("Add failed: " + m);
    });
    refresh();
}

void SharingTab::buildUi()
{
    auto * Root = new QVBoxLayout(this);

    // --- Your library address ---
    auto * AddrBox = new QGroupBox("Your friend code", this);
    auto * AddrLayout = new QVBoxLayout(AddrBox);
    auto * Hint = new QLabel("Your friend code identifies you to friends — share it so they can connect. It never "
                             "changes; it is derived from your identity key.", AddrBox);
    Hint->setWordWrap(true);
    Hint->setStyleSheet("color: palette(mid);");
    AddrLayout->addWidget(Hint);
    auto * Row = new QHBoxLayout();
    Row->addWidget(new QLabel("Friend code:", AddrBox));
    AddressValue = new QLabel(AddrBox);
    AddressValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    AddressValue->setStyleSheet("font-family: monospace;");
    Row->addWidget(AddressValue, 1);
    CopyButton = new QPushButton("Copy", AddrBox);
    connect(CopyButton, &QPushButton::clicked, this, []{
        const std::string Code = IpfsWrapper::FriendCode();
        if (!Code.empty()) QApplication::clipboard()->setText(QString::fromStdString(Code));
    });
    Row->addWidget(CopyButton);
    AddrLayout->addLayout(Row);
    Root->addWidget(AddrBox);

    // --- Publish ---
    auto * PubBox = new QGroupBox("Publish your libraries", this);
    auto * PubLayout = new QVBoxLayout(PubBox);
    LibrariesLabel = new QLabel("Publish freezes every game in your library into content-addressed blocks and seeds "
                                "them, so friends can fetch any of them by CID. Each game's CID changes only when that "
                                "game changes — everything else stays put and de-duplicates automatically.", PubBox);
    LibrariesLabel->setWordWrap(true);
    PubLayout->addWidget(LibrariesLabel);
    PublishButton = new QPushButton("Verify && Publish", PubBox);
    connect(PublishButton, &QPushButton::clicked, this, &SharingTab::publishClicked);
    PubLayout->addWidget(PublishButton);
    PublishStatus = new QLabel(PubBox);
    PublishStatus->setWordWrap(true);
    PublishStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    PublishStatus->setStyleSheet("font-family: monospace; color: palette(mid);");
    PubLayout->addWidget(PublishStatus);
    Root->addWidget(PubBox);

    // --- Add a friend's game (by launchable CID) ---
    auto * AddBox = new QGroupBox("Add a friend's game", this);
    auto * AddLayout = new QVBoxLayout(AddBox);
    auto * AddHint = new QLabel("Paste a game's CID (a friend shares one from their published library). It downloads "
                                "into your library and appears in the Library tab — de-duplicating anything you already "
                                "have.", AddBox);
    AddHint->setWordWrap(true);
    AddHint->setStyleSheet("color: palette(mid);");
    AddLayout->addWidget(AddHint);
    auto * AddRow = new QHBoxLayout();
    AddCidEdit = new QLineEdit(AddBox);
    AddCidEdit->setPlaceholderText("bafy… (game CID)");
    AddRow->addWidget(AddCidEdit, 1);
    AddButton = new QPushButton("Add game", AddBox);
    connect(AddButton, &QPushButton::clicked, this, &SharingTab::addGameClicked);
    connect(AddCidEdit, &QLineEdit::returnPressed, this, &SharingTab::addGameClicked);
    AddRow->addWidget(AddButton);
    AddLayout->addLayout(AddRow);
    AddStatus = new QLabel(AddBox);
    AddStatus->setWordWrap(true);
    AddStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    AddStatus->setStyleSheet("font-family: monospace; color: palette(mid);");
    AddLayout->addWidget(AddStatus);
    Root->addWidget(AddBox);

    // --- Identity backup ---
    auto * IdBox = new QGroupBox("Identity backup", this);
    auto * IdLayout = new QVBoxLayout(IdBox);
    auto * IdHint = new QLabel("Your identity key is your friend code. If you lose it there is no recovery — back it up "
                               "somewhere safe and never share it.", IdBox);
    IdHint->setWordWrap(true);
    IdHint->setStyleSheet("color: palette(mid);");
    IdLayout->addWidget(IdHint);
    auto * IdRow = new QHBoxLayout();
    auto * Backup = new QPushButton("Back up identity…", IdBox);
    connect(Backup, &QPushButton::clicked, this, &SharingTab::backupIdentityClicked);
    IdRow->addWidget(Backup);
    auto * Restore = new QPushButton("Restore identity…", IdBox);
    connect(Restore, &QPushButton::clicked, this, &SharingTab::restoreIdentityClicked);
    IdRow->addWidget(Restore);
    IdRow->addStretch();
    IdLayout->addLayout(IdRow);
    Root->addWidget(IdBox);

    Root->addStretch();
}

void SharingTab::setActive(bool on) { if (on) refresh(); }

void SharingTab::refresh()
{
    const std::string Code = IpfsWrapper::FriendCode();
    const bool Online = IpfsWrapper::Available() && !Code.empty();
    AddressValue->setText(Online ? QString::fromStdString(Code) : QStringLiteral("— (networking off)"));
    CopyButton->setEnabled(Online);
    PublishButton->setEnabled(Online);
    if (!Online)
        PublishStatus->setText("Enable networking (IPFS tab) to publish and seed your library.");
}

void SharingTab::publishClicked()
{
    PublishButton->setEnabled(false);
    PublishStatus->setText("Publishing… (freezing packages into content-addressed blocks and seeding them)");
    Model.publishLibraries();
}

void SharingTab::addGameClicked()
{
    const QString Cid = AddCidEdit->text().trimmed();
    if (Cid.isEmpty()) { AddStatus->setText("Paste a game CID first."); return; }
    AddButton->setEnabled(false);
    AddStatus->setText("Fetching " + Cid.left(20) + "… (downloading the game and its content)");
    Model.addGameByCid(Cid);
}

void SharingTab::backupIdentityClicked()
{
    const QString Path = QFileDialog::getSaveFileName(this, "Back up identity key", "vidyagod-identity.key");
    if (Path.isEmpty()) return;
    std::string Err;
    if (IpfsWrapper::ExportIdentity(Path.toStdString(), &Err))
        QMessageBox::information(this, "Identity backed up",
                                 "Your identity key was saved.\nKeep it private — anyone with this file becomes you.");
    else
        QMessageBox::warning(this, "Backup failed", QString::fromStdString(Err));
}

void SharingTab::restoreIdentityClicked()
{
    const QString Path = QFileDialog::getOpenFileName(this, "Restore identity key");
    if (Path.isEmpty()) return;
    if (QMessageBox::warning(this, "Restore identity",
            "This REPLACES your current identity — your friend code and library address will change to the restored "
            "one. You must restart VidyaGod for it to take effect.\n\nContinue?",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    std::string Err;
    if (IpfsWrapper::ImportIdentity(Path.toStdString(), &Err))
        QMessageBox::information(this, "Identity restored",
                                 "The identity was installed. RESTART VidyaGod now for your restored friend code and "
                                 "library address to take effect.");
    else
        QMessageBox::warning(this, "Restore failed", QString::fromStdString(Err));
}
