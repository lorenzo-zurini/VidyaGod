#include "sharingtab.h"
#include "appmodel.h"
#include "ipfswrapper.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
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
    refresh();
}

void SharingTab::buildUi()
{
    auto * Root = new QVBoxLayout(this);

    // --- Your library address ---
    auto * AddrBox = new QGroupBox("Your library address", this);
    auto * AddrLayout = new QVBoxLayout(AddrBox);
    auto * Hint = new QLabel("Your friend code IS your library address. Friends who receive your library resolve this "
                             "name to your current catalog. It never changes — it is derived from your identity key.", AddrBox);
    Hint->setWordWrap(true);
    Hint->setStyleSheet("color: palette(mid);");
    AddrLayout->addWidget(Hint);
    auto * Row = new QHBoxLayout();
    Row->addWidget(new QLabel("Address:", AddrBox));
    AddressValue = new QLabel(AddrBox);
    AddressValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    AddressValue->setStyleSheet("font-family: monospace;");
    Row->addWidget(AddressValue, 1);
    CopyButton = new QPushButton("Copy", AddrBox);
    connect(CopyButton, &QPushButton::clicked, this, []{
        const std::string Code = IpfsWrapper::FriendCode();
        if (!Code.empty()) QApplication::clipboard()->setText(QString::fromStdString("/ipns/" + Code));
    });
    Row->addWidget(CopyButton);
    AddrLayout->addLayout(Row);
    Root->addWidget(AddrBox);

    // --- Publish ---
    auto * PubBox = new QGroupBox("Publish your libraries", this);
    auto * PubLayout = new QVBoxLayout(PubBox);
    LibrariesLabel = new QLabel("Verify & Publish re-mints every package in your libraries (the collections under your "
                                "LIBRARY folder), rebuilds your signed index, and points your address at it. Touching one "
                                "game moves only that entry — subscribers see just that update.", PubBox);
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

    // --- Identity backup ---
    auto * IdBox = new QGroupBox("Identity backup", this);
    auto * IdLayout = new QVBoxLayout(IdBox);
    auto * IdHint = new QLabel("Your identity key is your friend code AND your library address. If you lose it there is "
                               "no recovery — back it up somewhere safe and never share it.", IdBox);
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
    AddressValue->setText(Online ? QString::fromStdString("/ipns/" + Code) : QStringLiteral("— (networking off)"));
    CopyButton->setEnabled(Online);
    PublishButton->setEnabled(Online);
    if (!Online)
        PublishStatus->setText("Enable networking (IPFS tab) to publish — a publish needs the DHT.");
}

void SharingTab::publishClicked()
{
    PublishButton->setEnabled(false);
    PublishStatus->setText("Verifying & publishing… (re-minting packages, this can take a while)");
    Model.publishLibraries();
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
