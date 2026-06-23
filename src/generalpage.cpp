#include "generalpage.h"
#include "appmodel.h"

#include <QVBoxLayout>
#include <QCheckBox>
#include <QLabel>
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

    pl->addStretch(1);
}
