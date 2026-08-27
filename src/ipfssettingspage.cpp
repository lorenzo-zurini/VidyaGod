#include "ipfssettingspage.h"
#include "appmodel.h"
#include "ipfswrapper.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QCheckBox>
#include <QLabel>
#include <QSpinBox>

#include <nlohmann/json.hpp>

IpfsSettingsPage::IpfsSettingsPage(AppModel & model, QWidget * parent)
    : QWidget(parent), Model(model)
{
    QVBoxLayout * pl = new QVBoxLayout(this);
    pl->setContentsMargins(12, 12, 12, 12);
    pl->setSpacing(8);

    QLabel * heading = new QLabel("Networking (IPFS)", this);
    heading->setStyleSheet("font-weight:bold;");
    pl->addWidget(heading);

    // Master switch — OFF by default. Off = the embedded node is fully down (no swarm, no DHT, no seeding/downloading).
    // Launching already-downloaded games still works (the VFS reads the files by path). AppModel starts/stops the
    // node and greys the Catalog/IPFS tabs in response.
    QCheckBox * netCb = new QCheckBox("Enable networking (downloads & seeding)", this);
    netCb->setChecked(Model.networkingEnabled());
    connect(netCb, &QCheckBox::toggled, this, [this](bool on){ Model.setNetworkingEnabled(on); });
    pl->addWidget(netCb);

    QLabel * netHint = new QLabel(
        "Off by default. While off, VidyaGod makes no network connections — it won't download or seed, and the "
        "Catalog is disabled. Games you've already downloaded still launch (their files are read locally). Turn "
        "this on to fetch new games and seed your library to other players.", this);
    netHint->setWordWrap(true);
    netHint->setStyleSheet("color:#8f98a0;font-size:9pt;margin-left:22px;");
    pl->addWidget(netHint);

    // Max simultaneous downloads — Settings.MaxConcurrentDownloads. Caps how many files fetch at once across ALL
    // packages (a game's content + its runner's build all draw from this one pool).
    QFormLayout * form = new QFormLayout();
    form->setContentsMargins(0, 8, 0, 0);
    pl->addLayout(form);

    QSpinBox * maxDl = new QSpinBox(this);
    maxDl->setRange(1, 32);
    maxDl->setValue(IpfsWrapper::MaxConcurrentDownloads());
    {
        auto & S = (*Model.config())["Settings"];
        if (S.contains("MaxConcurrentDownloads") && S["MaxConcurrentDownloads"].is_number_integer())
            maxDl->setValue(int(S["MaxConcurrentDownloads"]));
    }
    connect(maxDl, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v){
        IpfsWrapper::SetMaxConcurrentDownloads(v);
        (*Model.config())["Settings"]["MaxConcurrentDownloads"] = v;
        Model.save();
    });
    form->addRow("Max simultaneous downloads:", maxDl);

    QLabel * dlNote = new QLabel("Files download in parallel up to this limit; a stalled one keeps retrying in the "
                                 "background without blocking the others.", this);
    dlNote->setWordWrap(true);
    dlNote->setStyleSheet("color:#8f98a0;font-size:9pt;");
    pl->addWidget(dlNote);

    // Friend LAN — Settings.LanEnabled (default OFF). When on, every game launch joins the virtual LAN of accepted
    // friends: the launch runs in an ISOLATED network namespace with the overlay TUN as its only interface, so
    // LAN-multiplayer games discover friends' games as if on one physical LAN. The host network stack is untouched.
    QLabel * lanHeading = new QLabel("Friend LAN (multiplayer)", this);
    lanHeading->setStyleSheet("font-weight:bold;margin-top:12px;");
    pl->addWidget(lanHeading);

    QCheckBox * lanCb = new QCheckBox("Join the virtual LAN of friends when launching games", this);
    lanCb->setChecked((*Model.config())["Settings"].value("LanEnabled", false));
    connect(lanCb, &QCheckBox::toggled, this, [this](bool on){
        (*Model.config())["Settings"]["LanEnabled"] = on;
        Model.save();
    });
    pl->addWidget(lanCb);

    QLabel * lanHint = new QLabel(
        "Games launch inside a private virtual LAN shared with your accepted friends (Friends tab) — LAN "
        "multiplayer works across the internet as if you were in one room. While enabled, launched games can "
        "reach ONLY that virtual LAN (no direct internet); turn it off for online-service games.", this);
    lanHint->setWordWrap(true);
    lanHint->setStyleSheet("color:#8f98a0;font-size:9pt;margin-left:22px;");
    pl->addWidget(lanHint);
    pl->addStretch(1);
}
