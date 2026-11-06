#include "traycontroller.h"
#include "appmodel.h"
#include "apppaths.h"

#include <QSystemTrayIcon>
#include <QMenu>
#include <QIcon>
#include <QWidget>

#include <nlohmann/json.hpp>

TrayController::TrayController(AppModel & model, QWidget * window, QObject * parent)
    : QObject(parent), Model(model)
{
    if (!AppPaths::DaemonSupported()) return;                // In-package mode: run + exit, never a resident daemon.
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;   // no tray → available() stays false, normal behavior

    Tray = new QSystemTrayIcon(QIcon(":/vidyagod.png"), this);
    Tray->setToolTip("VidyaGod");

    QMenu * Menu = new QMenu(window);
    QAction * ShowAct = Menu->addAction("Show VidyaGod");
    Menu->addSeparator();
    QAction * QuitAct = Menu->addAction("Quit");
    Tray->setContextMenu(Menu);

    connect(ShowAct, &QAction::triggered, this, [this]{ emit restoreRequested(); });
    connect(QuitAct, &QAction::triggered, this, [this]{ emit quitRequested(); });
    // Left-click / double-click the tray icon restores the window (right-click shows the menu).
    connect(Tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r){
        if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick) emit restoreRequested();
    });

    Tray->show();
}

bool TrayController::available() const { return Tray != nullptr; }

bool TrayController::setting(const char * Key, bool Default) const
{
    const auto & Cfg = *Model.config();
    if (Cfg.contains("Settings") && Cfg["Settings"].is_object())
    {
        const auto & S = Cfg["Settings"];
        if (S.contains("Tray") && S["Tray"].is_object() && S["Tray"].contains(Key) && S["Tray"][Key].is_boolean())
            return bool(S["Tray"][Key]);
    }
    return Default;
}

void TrayController::rememberHidden(bool Hidden)
{
    auto & Cfg = *Model.config();
    Cfg["Settings"]["Tray"]["LaunchHidden"] = Hidden;
    Model.save();
}

void TrayController::notifyHidden()
{
    if (!Tray || Hinted) return;
    Hinted = true;
    Tray->showMessage("VidyaGod is still running",
                      "It keeps seeding your library in the background. Click the tray icon to reopen, or Quit from "
                      "its menu.", QSystemTrayIcon::Information, 4000);
}
