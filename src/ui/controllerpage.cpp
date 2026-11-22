#include "controllerpage.h"
#include "theme.h"        // the named colours
#include "appmodel.h"
#include "deckshell.h"    // ControllerMode — the settings; DeckShell::On — whether this page is inside it
#include "mainwindow.h"   // controller mode on, the settings applied live
#include "padinput.h"     // the controllers connected

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

ControllerPage::ControllerPage(AppModel & model, QWidget * parent)
    : QWidget(parent), Model(model)
{
    QVBoxLayout * pl = new QVBoxLayout(this);
    pl->setContentsMargins(12, 12, 12, 12);
    pl->setSpacing(8);

    QLabel * padHeading = new QLabel("Controller", this);
    padHeading->setStyleSheet("font-weight:bold;");
    pl->addWidget(padHeading);
    auto * startRow = new QHBoxLayout();
    QLabel * startLbl = new QLabel("Start in", this);
    auto * startCombo = new QComboBox(this);
    startCombo->setAccessibleName("Controller Start in");
    startCombo->addItem("Automatic — controller mode inside Steam's Game Mode", "auto");
    startCombo->addItem("Controller mode", "controller");
    startCombo->addItem("The desktop window", "desktop");
    startCombo->setCurrentIndex(std::max(0, startCombo->findData(QString::fromStdString(
        ControllerMode::StartName(ControllerMode::StartSetting(*Model.config()))))));
    startRow->addWidget(startLbl);
    startRow->addWidget(startCombo, 1);
    pl->addLayout(startRow);
    connect(startCombo, &QComboBox::currentIndexChanged, this, [this, startCombo](int){
        if (const auto S = ControllerMode::StartFrom(startCombo->currentData().toString().toStdString()))
            ControllerMode::SetStart(*Model.config(), *S);
        Model.save();
    });
    QCheckBox * deskCb = new QCheckBox("A controller drives this window too (Start with Back held: controller mode)", this);
    deskCb->setChecked(ControllerMode::DesktopNavigation(*Model.config()));
    pl->addWidget(deskCb);
    QCheckBox * hapCb = new QCheckBox("Rumble as the focus moves", this);
    hapCb->setChecked(ControllerMode::Haptics(*Model.config()));
    pl->addWidget(hapCb);
    const auto Apply = [this] {
        Model.save();
        if (auto * W = qobject_cast<MainWindow *>(window())) W->applyControllerSettings();
    };
    connect(deskCb, &QCheckBox::toggled, this, [this, Apply](bool on){ ControllerMode::SetDesktopNavigation(*Model.config(), on); Apply(); });
    connect(hapCb, &QCheckBox::toggled, this, [this, Apply](bool on){ ControllerMode::SetHaptics(*Model.config(), on); Apply(); });
    QLabel * pads = new QLabel(this);
    pads->setObjectName("controllersConnected");
    pads->setWordWrap(true);
    pads->setStyleSheet("color:" + Theme::Dim + ";font-size:9pt;margin-left:2px;");
    const auto ShowPads = [pads] {
        QStringList Names;
        for (const PadInput::PadInfo & P : PadInput::instance().pads()) Names << P.Name;
        pads->setText(Names.isEmpty() ? QStringLiteral("No controller connected.")
                                      : "Connected: " + Names.join(", "));
    };
    ShowPads();
    connect(&PadInput::instance(), &PadInput::padsChanged, pads, ShowPads);
    pl->addWidget(pads);
    //Not in controller mode itself (where this page is shown inside it): the page may be built before it is placed,
    //so ask the mode, not the window.
    if (!DeckShell::On())
    {
        QPushButton * enter = new QPushButton("Enter controller mode", this);
        enter->setObjectName("enterControllerMode");
        connect(enter, &QPushButton::clicked, this, [this]{ if (auto * W = qobject_cast<MainWindow *>(window())) W->setControllerMode(true); });
        auto * row = new QHBoxLayout();
        row->addWidget(enter);
        row->addStretch(1);
        pl->addLayout(row);
    }

    pl->addStretch(1);
}
