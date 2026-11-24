#include "generalpage.h"
#include "theme.h"   // the named colours
#include "appmodel.h"
#include "apppaths.h"
#include "autostart.h"
#include "screensaver.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QMessageBox>
#include <QSystemTrayIcon>

#include <nlohmann/json.hpp>


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

    {   // the screensaver: controller mode's screen after a while with no input (an OLED's burn-in)
        QLabel * h = new QLabel("Screen", this);
        h->setStyleSheet("font-weight:bold;margin-top:8px;");
        auto * row = new QHBoxLayout();
        row->addWidget(new QLabel("Screensaver after", this));
        auto * combo = new QComboBox(this);
        combo->setObjectName("screenSaver");
        const std::pair<const char *, int> Choices[] = { { "Off", 0 }, { "30 seconds", 30 }, { "1 minute", 60 }, { "2 minutes", 120 },
                                                         { "5 minutes", 300 }, { "10 minutes", 600 }, { "30 minutes", 1800 } };
        const int Now = ScreenSaver::SecondsIn(*Model.config());
        for (const auto & [Label, Seconds] : Choices) combo->addItem(Label, Seconds);
        if (combo->findData(Now) < 0) combo->addItem(QString::number(Now) + " seconds", Now);
        combo->setCurrentIndex(combo->findData(Now));
        connect(combo, &QComboBox::currentIndexChanged, this, [this, combo](int) {
            (*Model.config())["Settings"]["ScreenSaverSeconds"] = combo->currentData().toInt();
            Model.save();
        });
        row->addWidget(combo);
        row->addStretch(1);
        QLabel * hint = new QLabel("Controller mode's screen gives way to the digital rain after this long with no input, so nothing stays lit long enough to burn into an OLED screen. Never while a game runs; any input ends it.", this);
        hint->setWordWrap(true);
        hint->setStyleSheet("color:" + Theme::Dim + ";font-size:9pt;");
        pl->addWidget(h);
        pl->addLayout(row);
        pl->addWidget(hint);
    }

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

    pl->addStretch(1);
}
