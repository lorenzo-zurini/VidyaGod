#include "generalpage.h"
#include "appmodel.h"
#include "apppaths.h"
#include "autostart.h"

#include <QVBoxLayout>
#include <QCheckBox>
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
            h->setStyleSheet("color:#8f98a0;font-size:9pt;margin-left:22px;");
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

    // ── Start on login (XDG autostart) — reflects the on-disk entry, not the config. Only offered in Normal run
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
    loginLbl->setStyleSheet("color:#8f98a0;font-size:9pt;margin-left:22px;");
    pl->addWidget(loginLbl);

    pl->addStretch(1);
}
