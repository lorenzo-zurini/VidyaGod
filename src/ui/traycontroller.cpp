#include "traycontroller.h"
#include "appmodel.h"
#include "apppaths.h"
#include "containerwrapper.h"   // LiveCount / StopAll: the running game

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
    const auto Tip = [this] { Tray->setToolTip(Model.networkingEnabled() ? "VidyaGod — networking on: seeding and downloading"
                                                                          : "VidyaGod — networking off"); };
    Tip();
    connect(&Model, &AppModel::networkingChanged, this, Tip);
    Tray->setContextMenu(TrayMenu(Model, window, [this]{ emit restoreRequested(); }, [this]{ emit quitRequested(); }));
    // Left-click / double-click the tray icon restores the window (right-click shows the menu).
    connect(Tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r){
        if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick) emit restoreRequested();
    });

    Tray->show();
}

QMenu *TrayMenu(AppModel &Model, QWidget *Parent, std::function<void()> Show, std::function<void()> Quit)
{
    //What a program that lives in the tray for its seeding is asked from there: show it, the network on or off, the
    //game it runs stopped (only while one runs), quit.
    auto *Menu = new QMenu(Parent);
    QAction *ShowAct = Menu->addAction("Show VidyaGod");
    Menu->addSeparator();
    QAction *Net = Menu->addAction(QString());
    Net->setObjectName("trayNetworking");
    QAction *Stop = Menu->addAction("Stop the running game");
    Stop->setObjectName("trayStopGame");
    Menu->addSeparator();
    QAction *QuitAct = Menu->addAction("Quit");
    const auto Refresh = [&Model, Net, Stop] {
        Net->setText(Model.networkingEnabled() ? "Turn networking off" : "Turn networking on");
        Stop->setVisible(ContainerWrapper::LiveCount() > 0);
    };
    Refresh();
    QObject::connect(Menu, &QMenu::aboutToShow, Menu, Refresh);
    QObject::connect(ShowAct, &QAction::triggered, Menu, [Show] { Show(); });
    QObject::connect(QuitAct, &QAction::triggered, Menu, [Quit] { Quit(); });
    QObject::connect(Net, &QAction::triggered, Menu, [&Model, Refresh] { Model.setNetworkingEnabled(!Model.networkingEnabled()); Refresh(); });
    //Told to stop as the launch window's Stop does: its saves are stored, its runtime comes down.
    QObject::connect(Stop, &QAction::triggered, Menu, [] { ContainerWrapper::StopAll(); });
    return Menu;
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
