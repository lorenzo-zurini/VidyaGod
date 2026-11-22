#include "generalpage.h"
#include "theme.h"   // the named colours
#include "appmodel.h"
#include "apppaths.h"
#include "autostart.h"
#include "deckshell.h"    // ControllerMode — the controller settings
#include "mainwindow.h"   // controller mode on, the settings applied live
#include "padinput.h"     // the controllers connected

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QPushButton>
#include <QCheckBox>
#include <QLabel>
#include <QMessageBox>
#include <QSystemTrayIcon>

#include <nlohmann/json.hpp>

#include <algorithm>

GeneralPage::GeneralPage(AppModel & model, QWidget * parent)
    : QWidget(parent), Model(model)
{
    QVBoxLayout * pl = new QVBoxLayout(this);
    pl->setContentsMargins(12, 12, 12, 12);
    pl->setSpacing(8);

    QLabel * heading = new QLabel("Tray & background", this);
    heading->setStyleSheet("font-weight:bold;");
    pl->addWidget(heading);

    // Reads Settings.Tray.<Key> (default Default); writes + persists on toggle. MainWindow reads these live.
    auto addToggle = [&](const QString & Label, const char * Key, bool Default, const QString & Hint) {
        QCheckBox * cb = new QCheckBox(Label, this);
        bool val = Default;
        {
            auto & S = (*Model.config())["Settings"];
            if (S.contains("Tray") && S["Tray"].is_object() && S["Tray"].contains(Key) && S["Tray"][Key].is_boolean())
                val = bool(S["Tray"][Key]);
        }
        cb->setChecked(val);
        connect(cb, &QCheckBox::toggled, this, [this, Key](bool on){
            (*Model.config())["Settings"]["Tray"][Key] = on;
            Model.save();
        });
        pl->addWidget(cb);
        if (!Hint.isEmpty())
        {
            QLabel * h = new QLabel(Hint, this);
            h->setWordWrap(true);
            h->setStyleSheet("color:" + Theme::Dim + ";font-size:9pt;margin-left:22px;");
            pl->addWidget(h);
        }
        return cb;
    };

    addToggle("Close to tray", "CloseToTray", true,
              "Closing the window keeps VidyaGod running in the tray (so it keeps seeding) instead of quitting.");
    addToggle("Minimize to tray", "MinimizeToTray", true,
              "Minimizing the window hides it to the tray instead of the taskbar.");
    addToggle("Start in tray", "StartInTray", false,
              "Launch hidden in the tray. Otherwise VidyaGod reopens the way you left it last time.");

    if (!QSystemTrayIcon::isSystemTrayAvailable())
    {
        QLabel * warn = new QLabel("⚠ No system tray was detected on this desktop — these options have no effect "
                                   "until one is available (closing will quit normally).", this);
        warn->setWordWrap(true);
        warn->setStyleSheet("color:#d4a017;font-size:9pt;");
        pl->addWidget(warn);
    }

    // ── Start on login (XDG autostart / the Windows Run key) — reflects the real entry, not the config. Only offered in Normal run
    // mode (a portable / in-package / custom-path instance shouldn't register a fixed login entry).
    QCheckBox * loginCb = new QCheckBox("Launch at login", this);
    loginCb->setChecked(AutoStart::IsEnabled());
    pl->addWidget(loginCb);

    QString loginHint = "Start VidyaGod automatically when you log in (hidden in the tray) so it keeps seeding.";
    if (!AppPaths::AutostartSupported())
    {
        loginCb->setEnabled(false);
        loginCb->setChecked(false);
        loginHint = AppPaths::GetMode() == AppPaths::Mode::Portable
            ? "Not available in portable mode — a login entry can't point at a portable, movable install."
            : "Not available when VidyaGod is launched with custom paths.";
    }
    else
    {
        connect(loginCb, &QCheckBox::toggled, this, [this, loginCb](bool on){
            QString err;
            if (!AutoStart::SetEnabled(on, &err))
            {
                QMessageBox::warning(this, "Launch at login", "Couldn't update the login entry:\n" + err);
                loginCb->setChecked(AutoStart::IsEnabled());   // revert to the real state
            }
        });
    }
    QLabel * loginLbl = new QLabel(loginHint, this);
    loginLbl->setWordWrap(true);
    loginLbl->setStyleSheet("color:" + Theme::Dim + ";font-size:9pt;margin-left:22px;");
    pl->addWidget(loginLbl);

    // ── Controller: how the app starts (controller mode or the desktop window), whether a controller drives this
    // window, rumble — and the controllers connected now.
    pl->addSpacing(14);
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
